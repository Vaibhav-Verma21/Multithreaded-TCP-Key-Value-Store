#include "server.hpp"
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

// Pointer used solely to bridge the OS C-style signal handler to the C++ Server instance.
// No application state or cache data is stored globally.
static std::atomic<Server*> g_running_server{nullptr};

extern "C" void handle_sigint([[maybe_unused]] int signum) {
    Server* server = g_running_server.load();
    if (server) {
        std::cout << "\n[Signal] Shutdown signal caught. Stopping server gracefully..." << std::endl;
        server->stop();
    }
}

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        handle_sigint(SIGINT);
        return TRUE;
    }
    return FALSE;
}
#endif

int main(int argc, char* argv[]) {
    // Default configuration values
    uint16_t port = 8080;
    size_t cache_capacity = 1024;
    size_t num_threads = std::max(2u, std::thread::hardware_concurrency());

    // Parse command line arguments: [port] [capacity] [threads]
    if (argc >= 2) {
        int parsed_port = std::atoi(argv[1]);
        if (parsed_port <= 0 || parsed_port > 65535) {
            std::cerr << "Usage: " << argv[0] << " [port (1-65535)] [capacity] [threads]\n";
            return 1;
        }
        port = static_cast<uint16_t>(parsed_port);
    }
    if (argc >= 3) {
        int parsed_cap = std::atoi(argv[2]);
        if (parsed_cap <= 0) {
            std::cerr << "Capacity must be positive.\n";
            return 1;
        }
        cache_capacity = static_cast<size_t>(parsed_cap);
    }
    if (argc >= 4) {
        int parsed_threads = std::atoi(argv[3]);
        if (parsed_threads <= 0) {
            std::cerr << "Threads must be positive.\n";
            return 1;
        }
        num_threads = static_cast<size_t>(parsed_threads);
    }

#ifndef _WIN32
    // Ignore SIGPIPE so client disconnects do not terminate the process during send()
    ::signal(SIGPIPE, SIG_IGN);
#endif

    std::cout << "========================================" << std::endl;
    std::cout << "  kvstore-cpp: Multithreaded In-Memory LRU Key-Value Store" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Port:           " << port << std::endl;
    std::cout << "  LRU Capacity:   " << cache_capacity << " entries" << std::endl;
    std::cout << "  Worker Threads: " << num_threads << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    Server server(port, cache_capacity, num_threads);
    g_running_server.store(&server);

    // Register SIGINT / Ctrl+C handlers
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif
    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    // Start accept loop (blocks until server.stop() is called)
    server.start();

    g_running_server.store(nullptr);
    std::cout << "[Main] Server exited cleanly. Goodbye." << std::endl;
    return 0;
}

