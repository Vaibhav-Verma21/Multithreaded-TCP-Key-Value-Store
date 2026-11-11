#pragma once

#include "lru_cache.hpp"
#include "thread_pool.hpp"
#include <string>
#include <atomic>
#include <cstdint>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <signal.h>
  using socket_t = int;
  constexpr socket_t INVALID_SOCKET_FD = -1;
#endif

/**
 * ScopedSocket
 *
 * RAII wrapper ensuring socket descriptors are closed when leaving scope,
 * preventing socket leaks upon client disconnection, errors, or exceptions.
 */
class ScopedSocket {
public:
    explicit ScopedSocket(socket_t fd = INVALID_SOCKET_FD);
    ~ScopedSocket();

    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;

    ScopedSocket(ScopedSocket&& other) noexcept;
    ScopedSocket& operator=(ScopedSocket&& other) noexcept;

    socket_t get() const { return fd_; }
    void close();

private:
    socket_t fd_{INVALID_SOCKET_FD};
};

/**
 * Server
 *
 * Multithreaded TCP Key-Value Server implementing a text-based protocol.
 * The main thread binds, listens, and accepts connections, delegating
 * each connected client's I/O session to worker threads in ThreadPool.
 */
class Server {
public:
    Server(uint16_t port, size_t cache_capacity, size_t num_worker_threads);
    ~Server();

    // Non-copyable, non-movable
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Starts listening and runs the accept loop (blocks until stop() is called)
    void start();

    // Initiates clean shutdown: closes listening socket and drains/joins thread pool
    void stop();

    // Expose cache reference for testing
    LRUCache& get_cache() { return cache_; }

    // Parses a single protocol command line and produces the response
    std::string process_command(const std::string& line);

private:
    // Handles a client connection until disconnect or error
    void handle_client(socket_t client_fd);

    // Reliably sends all bytes, handling partial writes and OS signals
    bool send_all(socket_t fd, const std::string& data);

    const uint16_t port_;
    LRUCache cache_;
    ThreadPool thread_pool_;

    std::atomic<bool> running_{false};
    std::atomic<socket_t> server_fd_{INVALID_SOCKET_FD};
};

