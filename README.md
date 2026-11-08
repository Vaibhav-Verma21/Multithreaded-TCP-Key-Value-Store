# kvstore-cpp

A multithreaded in-memory key-value server written in C++17. Clients connect
over TCP and talk to it with a simple text protocol:

    SET key value
    GET key
    DEL key
    STATS

## Features
- POSIX TCP sockets (Linux) handling multiple concurrent clients
- Worker thread pool with a task queue (std::thread, condition_variable)
- LRU cache built from a custom doubly linked list and a hash map, with O(1) get/put
- Mutex-protected shared state for thread safety

## Build and run
    make
    ./kvstore 8080
    nc localhost 8080

## Testing
- Unit tests for the LRU cache
- Multi-client load test (add your throughput numbers here once you run it)
- Checked with Valgrind and ThreadSanitizer (add this line only after you do it)
