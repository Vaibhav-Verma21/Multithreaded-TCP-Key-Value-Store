#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <sstream>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
  inline int close_socket(socket_t s) { return ::closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  constexpr socket_t INVALID_SOCKET_FD = -1;
  inline int close_socket(socket_t s) { return ::close(s); }
#endif

// Simple line reader over TCP
static bool read_line(socket_t fd, std::string& buffer, std::string& out_line) {
    while (true) {
        size_t pos = buffer.find('\n');
        if (pos != std::string::npos) {
            out_line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!out_line.empty() && out_line.back() == '\r') {
                out_line.pop_back();
            }
            return true;
        }

        char tmp[1024];
        auto bytes = ::recv(fd, tmp, sizeof(tmp), 0);
        if (bytes <= 0) {
            return false;
        }
        buffer.append(tmp, static_cast<size_t>(bytes));
    }
}

// Reliable send
static bool send_all(socket_t fd, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        auto sent = ::send(fd, data.data() + total, static_cast<int>(data.size() - total), 0);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    // Default configuration: 4 threads, 1000 requests per thread, port 8080
    size_t num_threads = 4;
    size_t requests_per_thread = 1000;
    uint16_t port = 8080;
    std::string host = "127.0.0.1";

    if (argc >= 2) num_threads = static_cast<size_t>(std::atoi(argv[1]));
    if (argc >= 3) requests_per_thread = static_cast<size_t>(std::atoi(argv[2]));
    if (argc >= 4) port = static_cast<uint16_t>(std::atoi(argv[3]));
    if (argc >= 5) host = argv[4];

    std::cout << "=== kvstore-cpp Load Test ===" << std::endl;
    std::cout << "Target: " << host << ":" << port << std::endl;
    std::cout << "Threads: " << num_threads << std::endl;
    std::cout << "Requests per thread: " << requests_per_thread << " (Total commands: "
              << (num_threads * requests_per_thread * 2) << " [SET + GET])" << std::endl;

    std::atomic<size_t> successful_ops{0};
    std::atomic<size_t> failed_ops{0};

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    for (size_t t = 0; t < num_threads; ++t) {
        workers.emplace_back([t, &successful_ops, &failed_ops, port, host, requests_per_thread]() {
            socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock == INVALID_SOCKET_FD) {
                failed_ops += (requests_per_thread * 2);
                return;
            }

            sockaddr_in saddr{};
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(port);
            inet_pton(AF_INET, host.c_str(), &saddr.sin_addr);

            if (::connect(sock, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) != 0) {
                std::cerr << "[Thread " << t << "] Connection failed!" << std::endl;
                failed_ops += (requests_per_thread * 2);
                close_socket(sock);
                return;
            }

            std::string read_buffer;
            std::string response_line;

            for (size_t i = 0; i < requests_per_thread; ++i) {
                std::string key = "load_key_" + std::to_string(t) + "_" + std::to_string(i);
                std::string val = "load_val_" + std::to_string(i);

                // 1. SET
                std::string set_cmd = "SET " + key + " " + val + "\n";
                if (!send_all(sock, set_cmd) || !read_line(sock, read_buffer, response_line) || response_line != "OK") {
                    failed_ops++;
                    break;
                }
                successful_ops++;

                // 2. GET
                std::string get_cmd = "GET " + key + "\n";
                if (!send_all(sock, get_cmd) || !read_line(sock, read_buffer, response_line) || response_line != val) {
                    failed_ops++;
                    break;
                }
                successful_ops++;
            }

            close_socket(sock);
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    double elapsed_sec = elapsed.count();
    size_t total_ops = successful_ops.load();
    double ops_per_sec = elapsed_sec > 0 ? (static_cast<double>(total_ops) / elapsed_sec) : 0;

    std::cout << "\n--- Load Test Results ---" << std::endl;
    std::cout << "Successful Operations: " << total_ops << std::endl;
    std::cout << "Failed Operations:     " << failed_ops.load() << std::endl;
    std::cout << "Elapsed Time:          " << elapsed_sec << " seconds" << std::endl;
    std::cout << "Throughput:            " << ops_per_sec << " requests/sec" << std::endl;

#ifdef _WIN32
    WSACleanup();
#endif

    return failed_ops.load() > 0 ? 1 : 0;
}

