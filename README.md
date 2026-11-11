# kvstore-cpp

A high-performance, multithreaded, in-memory Key-Value store (a mini Redis) written in C++17 for Linux/WSL using standard POSIX networking APIs. Designed for systems engineering and technical interviews (Qualcomm, systems software, and infrastructure roles).

---

## Features

- **POSIX TCP Socket Server**: Implemented from scratch using `socket`, `bind`, `listen`, `accept`, `recv`, `send`, and `close`. Configurable listening port (default: `8080`).
- **Text Protocol**: Line-based protocol (`\n` terminated):
  - `SET <key> <value>` &rarr; `OK`
  - `GET <key>` &rarr; `<value>` or `NOT_FOUND`
  - `DEL <key>` &rarr; `DELETED` or `NOT_FOUND`
  - `STATS` &rarr; `hits:<H> misses:<M> evictions:<E> size:<S> capacity:<C>`
  - Unknown/Malformed commands &rarr; `ERROR <reason>`
- **Scratch-Built LRU Cache**:
  - $O(1)$ `get`, `put`, and `del` operations.
  - Combines `std::unordered_map<std::string, Node*>` with a custom doubly linked list utilizing dummy sentinel `head_` and `tail_` nodes.
  - Strictly avoids `std::list` to ensure exact control over memory layout and pointer splicing.
  - Thread-safe via `std::mutex`. (Contains detailed analysis of why `std::shared_mutex` reader-writer locks do not benefit standard LRU caches).
- **Worker Thread Pool**:
  - Fixed-size worker pool (`std::thread`).
  - Thread-safe FIFO task queue synchronized via `std::mutex` and `std::condition_variable`.
  - Non-busy waiting predicate wait loop preventing spurious and lost wakeups.
  - Graceful shutdown joining all threads without resource leaks.
- **Robust Network I/O**:
  - Partial read buffering (accumulates chunks across `recv` calls until `\n`).
  - Clean client disconnect handling (`recv() == 0`).
  - Signal-safe partial write loop (`send_all`).
  - RAII socket management (`ScopedSocket`).
  - Graceful shutdown on `SIGINT` / `SIGTERM`.

---

## Architecture

```
                    +--------------------------------+
                    |        Client (nc / load)      |
                    +--------------------------------+
                                    |  TCP (Port 8080)
                                    v
+-------------------------------------------------------------------------+
| Server (Main Listener Thread)                                           |
| - socket(), bind(), listen(), accept() loop                             |
| - ScopedSocket RAII resource lifetime management                        |
| - Catches SIGINT: closes listener socket and unblocks accept()          |
+-------------------------------------------------------------------------+
                                    | Dispatches client_fd
                                    v
+-------------------------------------------------------------------------+
| ThreadPool                                                              |
| - Fixed worker threads waiting on std::condition_variable               |
| - Synchronized task queue (std::queue<std::function<void()>>)          |
| - Executes handle_client() worker routine                               |
+-------------------------------------------------------------------------+
                                    | Thread-safe calls
                                    v
+-------------------------------------------------------------------------+
| LRUCache                                                                |
| - std::unordered_map<std::string, Node*> (Hash Table: O(1) key lookup)  |
| - Doubly Linked List (MRU at head <-> LRU at tail: O(1) splice/evict)   |
| - std::mutex protecting cache operations and eviction statistics        |
+-------------------------------------------------------------------------+
```

---

## Build Instructions

### Prerequisites
- GCC / G++ (supporting C++17)
- GNU Make
- POSIX-compatible environment (Linux, WSL, or MinGW on Windows)

### Compilation

```bash
# Build the server binary (kvstore)
make all

# Build and run the LRUCache unit tests
make test

# Build the multi-threaded load test client
make load_test

# Clean build artifacts
make clean
```

### Sanitizer Builds

```bash
# ThreadSanitizer (detects data races)
make tsan

# AddressSanitizer & UndefinedBehaviorSanitizer (detects memory errors and UB)
make asan
```

---

## Running the Server

Start the server with default options (port `8080`, capacity `1024`, worker threads = hardware concurrency):

```bash
./kvstore
```

Or specify custom arguments:

```bash
# Usage: ./kvstore [port] [capacity] [worker_threads]
./kvstore 8080 5000 8
```

---

## Usage with Netcat (`nc`)

Connect to the server from another terminal using `nc`:

```bash
$ nc localhost 8080
SET user:101 Alice
OK
GET user:101
Alice
SET user:102 Bob
OK
STATS
hits:1 misses:0 evictions:0 size:2 capacity:1024
DEL user:101
DELETED
GET user:101
NOT_FOUND
FOOBAR
ERROR unknown command: FOOBAR
```

---

## Testing

### 1. LRU Cache Unit Tests (`tests/test_lru_cache.cpp`)
Covers:
- Basic insertion and retrieval.
- Key value updates (ensuring size does not grow).
- Eviction order verification (LRU entry evicted first).
- Recency refresh on `get` (retrieving an older key moves it to MRU, protecting it from upcoming evictions).
- Deletion of existent and non-existent keys.
- Edge case: Cache capacity = 1.

Run with:
```bash
make test
```

### 2. Multi-threaded Load Test Client (`tests/load_test.cpp`)
Simulates $N$ concurrent client threads each sending $M$ consecutive `SET` and `GET` requests over dedicated TCP connections.

```bash
# Build
make load_test

# Usage: ./load_test [threads] [requests_per_thread] [port] [host]
./load_test 8 5000 8080 127.0.0.1
```

### Benchmark Results
> <!-- TODO: Benchmark Results Placeholder -->
> *Run `./load_test` on your target Linux/WSL deployment machine and paste the output metrics here.*
>
> - **Platform**: [TODO: e.g. Ubuntu 22.04 LTS on AMD Ryzen 7 / Intel i7]
> - **Threads**: [TODO]
> - **Total Requests**: [TODO]
> - **Elapsed Time**: [TODO]
> - **Throughput**: [TODO: requests/sec]

### Sanitizer Verification
> <!-- TODO: Sanitizer Run Results Placeholder -->
> *Execute `make tsan` and `make asan` on Linux/WSL (where libtsan and libasan are installed in your distro's toolchain) and record the clean output below.*
>
> - **ThreadSanitizer (TSan)**: [TODO: Verify 0 data races reported on multi-threaded load test]
> - **AddressSanitizer (ASan) & UBSan**: [TODO: Verify 0 memory leaks, 0 heap-use-after-free, and 0 undefined behavior errors]

