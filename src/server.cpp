#include "server.hpp"
#include <iostream>
#include <sstream>
#include <vector>
#include <cctype>
#include <cstring>

// -----------------------------------------------------------------------------
// ScopedSocket Implementation
// -----------------------------------------------------------------------------

ScopedSocket::ScopedSocket(socket_t fd) : fd_(fd) {}

ScopedSocket::~ScopedSocket() {
    close();
}

ScopedSocket::ScopedSocket(ScopedSocket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = INVALID_SOCKET_FD;
}

ScopedSocket& ScopedSocket::operator=(ScopedSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = INVALID_SOCKET_FD;
    }
    return *this;
}

void ScopedSocket::close() {
    if (fd_ != INVALID_SOCKET_FD) {
#ifdef _WIN32
        ::closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = INVALID_SOCKET_FD;
    }
}

// -----------------------------------------------------------------------------
// Server Implementation
// -----------------------------------------------------------------------------

Server::Server(uint16_t port, size_t cache_capacity, size_t num_worker_threads)
    : port_(port),
      cache_(cache_capacity),
      thread_pool_(num_worker_threads) {
#ifdef _WIN32
    // On Windows/MinGW, initialize Winsock DLL
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
    }
#endif
}

Server::~Server() {
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

std::string Server::process_command(const std::string& line) {
    if (line.empty()) {
        return "ERROR empty command\n";
    }

    std::istringstream iss(line);
    std::string verb;
    iss >> verb;

    // Convert command verb to uppercase for uniform parsing
    for (char& c : verb) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    if (verb == "GET") {
        std::string key;
        if (!(iss >> key) || (iss >> std::ws && !iss.eof())) {
            return "ERROR usage: GET <key>\n";
        }
        auto val = cache_.get(key);
        if (val.has_value()) {
            return *val + "\n";
        }
        return "NOT_FOUND\n";
    }

    if (verb == "SET") {
        std::string key;
        if (!(iss >> key)) {
            return "ERROR usage: SET <key> <value>\n";
        }
        // Skip leading whitespace before reading the remainder as value
        iss >> std::ws;
        std::string value;
        std::getline(iss, value);
        if (value.empty()) {
            return "ERROR usage: SET <key> <value>\n";
        }
        cache_.put(key, value);
        return "OK\n";
    }

    if (verb == "DEL") {
        std::string key;
        if (!(iss >> key) || (iss >> std::ws && !iss.eof())) {
            return "ERROR usage: DEL <key>\n";
        }
        if (cache_.del(key)) {
            return "DELETED\n";
        }
        return "NOT_FOUND\n";
    }

    if (verb == "STATS") {
        if (iss >> std::ws && !iss.eof()) {
            return "ERROR usage: STATS\n";
        }
        auto stats = cache_.get_stats();
        std::ostringstream oss;
        oss << "hits:" << stats.hits
            << " misses:" << stats.misses
            << " evictions:" << stats.evictions
            << " size:" << stats.size
            << " capacity:" << stats.capacity << "\n";
        return oss.str();
    }

    return "ERROR unknown command: " + verb + "\n";
}

bool Server::send_all(socket_t fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
#ifdef _WIN32
        int flags = 0;
#else
        // MSG_NOSIGNAL prevents the OS from sending SIGPIPE if the client closed the socket
        #ifdef MSG_NOSIGNAL
        int flags = MSG_NOSIGNAL;
        #else
        int flags = 0;
        #endif
#endif
        auto sent = ::send(fd, data.data() + total_sent, static_cast<int>(data.size() - total_sent), flags);
        if (sent <= 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
#else
            if (sent < 0 && errno == EINTR) continue; // Interrupted by signal, retry
#endif
            return false; // Connection closed or socket error
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

void Server::handle_client(socket_t client_fd) {
    // ScopedSocket ensures the file descriptor is closed when this function exits,
    // protecting against socket descriptor leaks regardless of how the loop terminates.
    ScopedSocket client_sock(client_fd);

    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    std::string accumulated;

    while (running_) {
        auto bytes_read = ::recv(client_sock.get(), buffer, sizeof(buffer), 0);

        if (bytes_read == 0) {
            // TCP FIN received: client gracefully closed connection
            break;
        }

        if (bytes_read < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
#else
            if (errno == EINTR) continue; // Signal interruption, retry
#endif
            // Unrecoverable socket error or reset (ECONNRESET)
            break;
        }

        accumulated.append(buffer, static_cast<size_t>(bytes_read));

        // Text protocol framing: extract newline-delimited command strings.
        // TCP is a stream protocol without message boundaries; a single recv() might
        // contain half a command, multiple commands, or commands split across frames.
        size_t newline_pos;
        while ((newline_pos = accumulated.find('\n')) != std::string::npos) {
            std::string line = accumulated.substr(0, newline_pos);
            accumulated.erase(0, newline_pos + 1);

            // Strip optional '\r' for compatibility with telnet / CRLF line endings
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) {
                continue;
            }

            std::string response = process_command(line);
            if (!send_all(client_sock.get(), response)) {
                // If send failed, client has disconnected
                return;
            }
        }
    }
}

void Server::start() {
    running_ = true;

    // 1. Create TCP IPv4 socket
    socket_t s_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s_fd == INVALID_SOCKET_FD) {
        std::cerr << "Failed to create socket." << std::endl;
        return;
    }
    server_fd_ = s_fd;

    // 2. Set SO_REUSEADDR
    // WHY: If the server restarts quickly, existing connections in TIME_WAIT state
    // would otherwise cause bind() to fail with EADDRINUSE ("Address already in use").
    int opt = 1;
#ifdef _WIN32
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    // 3. Bind socket to designated port and INADDR_ANY
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) != 0) {
        std::cerr << "Failed to bind to port " << port_ << std::endl;
        stop();
        return;
    }

    // 4. Mark socket as passive listener
    // WHY SOMAXCONN: Specifies the maximum length to which the queue of pending
    // connections for server_fd_ may grow. Prevents dropping TCP handshakes during bursts.
    if (::listen(server_fd_, SOMAXCONN) != 0) {
        std::cerr << "Failed to listen on socket." << std::endl;
        stop();
        return;
    }

    std::cout << "[Server] Listening on port " << port_ << "..." << std::endl;

    // 5. Accept loop
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        /**
         * WHY accept() unblocks on shutdown:
         * When stop() is invoked (e.g. via SIGINT), shutdown() and close() are called
         * on server_fd_. In POSIX, this immediately unblocks accept() with -1 (EBADF / EINVAL),
         * allowing the loop to break cleanly without waiting for another connection.
         */
        socket_t client_fd = ::accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

        if (client_fd == INVALID_SOCKET_FD) {
            if (!running_) {
                // Server shutdown initiated
                break;
            }
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue; // Interrupted by signal, retry accept
#endif
            std::cerr << "[Server] accept() error, exiting accept loop." << std::endl;
            break;
        }

        // Delegate client session to ThreadPool.
        // Handing off client_fd frees the main thread to immediately return to accept(),
        // preventing head-of-line blocking on incoming connections.
        thread_pool_.enqueue([this, client_fd]() {
            handle_client(client_fd);
        });
    }

    std::cout << "[Server] Accept loop stopped." << std::endl;
}

void Server::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }

    socket_t s_fd = server_fd_.exchange(INVALID_SOCKET_FD);
    if (s_fd != INVALID_SOCKET_FD) {
#ifdef _WIN32
        ::shutdown(s_fd, SD_BOTH);
        ::closesocket(s_fd);
#else
        ::shutdown(s_fd, SHUT_RDWR);
        ::close(s_fd);
#endif
    }

    // Drain queued tasks and join worker threads
    thread_pool_.shutdown();
    std::cout << "[Server] Server stopped cleanly." << std::endl;
}

