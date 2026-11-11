# Code Walkthrough: kvstore-cpp Internals

This document provides a comprehensive, file-by-file and function-by-function breakdown of **`kvstore-cpp`**. It is written as a deep-dive reference for technical interviews at companies like Qualcomm, focusing on memory layout, concurrency primitives, POSIX networking, and design trade-offs.

---

## 1. System Overview

`kvstore-cpp` is an in-memory key-value cache server modeled after Redis. It uses a **multithreaded producer-consumer architecture**:
- **Main Thread (Producer)**: Binds to a TCP port, listens for incoming connections, accepts client sockets, and enqueues them into a fixed-size thread pool.
- **Worker Threads (Consumers)**: Dequeue client sockets, read incoming bytes over TCP, frame newline-delimited command strings (`\n`), execute cache operations, and write responses back.
- **LRU Cache (Shared Resource)**: A thread-safe Least Recently Used cache combining a doubly linked list and a hash map for strictly $O(1)$ operations, synchronized via `std::mutex`.

```
                    +--------------------------------+
                    |        Client (nc / load)      |
                    +--------------------------------+
                                    |  TCP (Port 8080)
                                    v
+-------------------------------------------------------------------------+
| Server (Main Listener Thread) [src/server.cpp]                          |
| - socket(), bind(), listen(), accept() loop                             |
| - ScopedSocket RAII resource lifetime management                        |
| - Catches SIGINT: closes listener socket and unblocks accept()          |
+-------------------------------------------------------------------------+
                                    | Dispatches client_fd
                                    v
+-------------------------------------------------------------------------+
| ThreadPool [src/thread_pool.cpp]                                        |
| - Fixed worker threads waiting on std::condition_variable               |
| - Synchronized task queue (std::queue<std::function<void()>>)          |
| - Executes handle_client() worker routine                               |
+-------------------------------------------------------------------------+
                                    | Thread-safe calls
                                    v
+-------------------------------------------------------------------------+
| LRUCache [src/lru_cache.cpp]                                            |
| - std::unordered_map<std::string, Node*> (Hash Table: O(1) key lookup)  |
| - Doubly Linked List (MRU at head <-> LRU at tail: O(1) splice/evict)   |
| - std::mutex protecting cache operations and eviction statistics        |
+-------------------------------------------------------------------------+
```

---

## 2. File-by-File Code Breakdown

---

### Component 1: LRU Cache (`src/lru_cache.hpp` & `src/lru_cache.cpp`)

#### Responsibility:
Stores key-value pairs with a fixed capacity limit. When the cache is full and a new key arrives, the least recently used key is evicted. All operations (`get`, `put`, `del`) run in $O(1)$ time.

#### Internal Data Structures:
1. `struct Node`:
   - `std::string key`: Stored so when an eviction occurs at the tail, we can look up and delete the key from `map_` in $O(1)$.
   - `std::string value`: The actual cached payload.
   - `Node* prev`, `Node* next`: Pointers forming the doubly linked list.
2. `Node* head_` and `Node* tail_` (Sentinel / Dummy Nodes):
   - `head_` always sits before the Most Recently Used (MRU) node.
   - `tail_` always sits after the Least Recently Used (LRU) node.
   - **Why Sentinels?** They guarantee that every valid data node always has a non-null `prev` and `next`. This eliminates boundary checks (`if (head == nullptr)`, `if (node->prev == nullptr)`), reducing CPU branch mispredictions and preventing edge-case bugs.
3. `std::unordered_map<std::string, Node*> map_`:
   - Maps the string key directly to its heap-allocated `Node*`. Provides average $O(1)$ lookup time.
4. `mutable std::mutex mutex_`:
   - Protects the entire data structure from concurrent access by worker threads.
   - Marked `mutable` so `const` methods like `get_stats()` can acquire the lock.

#### Method Breakdown:

- **`LRUCache(size_t capacity)`**:
  - Allocates dummy `head_` and `tail_` nodes.
  - Links them together: `head_->next = tail_; tail_->prev = head_;`.
  - Rejects `capacity == 0` with `std::invalid_argument`.

- **`~LRUCache()`**:
  - Traverses the linked list from `head_` to `tail_`, deleting each node. This guarantees zero memory leaks upon cache destruction.

- **`void detach(Node* node)`**:
  - Splices `node` out of the linked list:
    ```cpp
    node->prev->next = node->next;
    node->next->prev = node->prev;
    ```
  - Takes 2 pointer reassignments; strictly $O(1)$.

- **`void attach_head(Node* node)`**:
  - Inserts `node` immediately after `head_` (MRU position):
    ```cpp
    node->next = head_->next;
    node->prev = head_;
    head_->next->prev = node;
    head_->next = node;
    ```
  - Takes 4 pointer reassignments; strictly $O(1)$.

- **`std::optional<std::string> get(const std::string& key)`**:
  - Locks `mutex_` via `std::lock_guard`.
  - Searches `map_` for `key`.
  - **If found**: Increments `hits_`, detaches the node from its current position, attaches it at `head_` (refreshing its recency to MRU), and returns `node->value`.
  - **If not found**: Increments `misses_` and returns `std::nullopt`.

- **`void put(const std::string& key, const std::string& value)`**:
  - Locks `mutex_`.
  - **Case 1: Key already exists**: Updates `node->value = value`, detaches it, and attaches it at `head_` (MRU). Size does not change.
  - **Case 2: Key is new**:
    - If `size_ >= capacity_`, evicts the LRU node (`Node* lru = tail_->prev`). Detaches `lru`, removes its key from `map_`, frees memory with `delete lru`, decrements `size_`, and increments `evictions_`.
    - Allocates `new Node(key, value)`.
    - Inserts into `head_` and adds to `map_`.
    - Increments `size_`.

- **`bool del(const std::string& key)`**:
  - Locks `mutex_`.
  - If key exists: detaches the node, removes it from `map_`, calls `delete node`, decrements `size_`, and returns `true`.
  - If not found: returns `false`.

- **`Stats get_stats() const`**:
  - Locks `mutex_` and returns a snapshot struct of `{hits, misses, evictions, size, capacity}`.

#### Concurrency Detail (Why `std::shared_mutex` Doesn't Work):
In read-heavy systems, `std::shared_mutex` allows multiple concurrent readers via `std::shared_lock`. However, in an LRU cache, **a read operation (`get`) modifies pointer links** to update recency. Because `get()` mutates `prev` and `next`, concurrent readers would race on pointer updates, corrupting the list. Thus, exclusive locking (`std::mutex`) is required.

---

### Component 2: Thread Pool (`src/thread_pool.hpp` & `src/thread_pool.cpp`)

#### Responsibility:
Manages a fixed pool of persistent worker threads and a synchronized queue of tasks. Prevents the overhead of creating/destroying threads per connection.

#### Internal Data Structures:
1. `std::vector<std::thread> workers_`: Stores worker thread objects.
2. `std::queue<std::function<void()>> tasks_`: Holds pending work items (client sessions).
3. `std::mutex queue_mutex_`: Synchronizes access to `tasks_` and `stop_`.
4. `std::condition_variable cv_`: Coordinates worker sleeping and waking without busy waiting.
5. `bool stop_`: Flag signaling threads to exit on shutdown.

#### Method Breakdown:

- **`ThreadPool(size_t num_threads)`**:
  - Launches `num_threads` worker threads. Each runs an infinite loop:
    ```cpp
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this]() {
                return stop_ || !tasks_.empty();
            });

            if (stop_ && tasks_.empty()) {
                return; // Graceful thread exit
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task(); // Executed OUTSIDE the lock!
    }
    ```

- **Wait Predicate `[this]() { return stop_ || !tasks_.empty(); }`**:
  - **Spurious Wakeup Protection**: The OS kernel can wake threads even if no signal was sent. The predicate forces the thread to re-check the condition and go back to sleep if no task is available.
  - **Lost Wakeup Protection**: If a task is enqueued before a worker calls `wait()`, the predicate detects `!tasks_.empty()` while holding the lock and proceeds immediately without sleeping.

- **Why Task Execution is Outside the Lock**:
  - If `task()` were executed inside `lock`, only one worker could run at a time, turning the multithreaded pool into a serialized bottleneck. Releasing the lock before calling `task()` allows all workers to run tasks in parallel.

- **`void enqueue(std::function<void()> task)`**:
  - Acquires `queue_mutex_`.
  - Pushes task into `tasks_`.
  - Calls `cv_.notify_one()` to wake up one sleeping worker.

- **`void shutdown()`**:
  - Sets `stop_ = true` under `queue_mutex_`.
  - Calls `cv_.notify_all()` to wake up all sleeping workers.
  - Iterates through `workers_` and calls `join()` on each thread, ensuring all threads terminate cleanly before destruction.

---

### Component 3: Server & Protocol (`src/server.hpp` & `src/server.cpp`)

#### Responsibility:
Implements the POSIX TCP socket lifecycle, handles text protocol framing, parses commands, and interacts with the cache.

#### Helper Class: `ScopedSocket`:
- An RAII wrapper around a socket descriptor (`int` on Linux, `SOCKET` on Windows).
- In its destructor: automatically invokes `close(fd_)` (or `closesocket(fd_)`).
- **Why?** Guarantees sockets are never leaked even if client handling returns early due to network errors or exceptions.

#### Method Breakdown:

- **`void start()` (Accept Loop)**:
  1. `socket(AF_INET, SOCK_STREAM, 0)`: Creates an IPv4 TCP stream socket.
  2. `setsockopt(..., SO_REUSEADDR, ...)`: Enables port reuse if the server is restarted quickly while previous connections are in `TIME_WAIT`.
  3. `bind(..., port)`: Binds the socket to `INADDR_ANY` and the specified port.
  4. `listen(..., SOMAXCONN)`: Marks socket as passive listener with maximum kernel connection backlog.
  5. **Accept Loop**:
     ```cpp
     while (running_) {
         socket_t client_fd = accept(server_fd_, ...);
         if (client_fd == INVALID_SOCKET_FD) {
             if (!running_) break; // Unblocked by stop()
             continue;
         }
         thread_pool_.enqueue([this, client_fd]() {
             handle_client(client_fd);
         });
     }
     ```
     - The main thread does **zero data I/O**. It immediately passes `client_fd` to the thread pool so it can return to `accept()` without delay.

- **`void stop()` (Graceful Teardown)**:
  - Sets `running_ = false`.
  - Calls `shutdown(server_fd_, SHUT_RDWR)` and `close(server_fd_)`. This causes the blocking `accept()` call in `start()` to immediately return with an error (`EBADF` or `EINVAL`), cleanly breaking the accept loop.
  - Calls `thread_pool_.shutdown()` to drain and join all worker threads.

- **`void handle_client(socket_t client_fd)` (Worker Routine)**:
  - Wraps `client_fd` in `ScopedSocket`.
  - Maintains a per-connection accumulator string: `std::string accumulated;`.
  - Enters a loop calling `recv(client_fd, buffer, sizeof(buffer), 0)`:
    - **`bytes_read == 0`**: Client cleanly closed connection (TCP FIN). Exit loop.
    - **`bytes_read < 0`**: Socket error or reset (`ECONNRESET`). Exit loop.
    - **`bytes_read > 0`**: Appends chunk to `accumulated`.
  - **Framing Algorithm**:
    - Scans `accumulated` for newline characters (`\n`).
    - Slices out complete lines (`line = accumulated.substr(0, pos)`).
    - Removes trailing `\r` (handles CRLF from Telnet or Windows clients).
    - Passes `line` to `process_command(line)`.
    - Sends response using `send_all()`.
    - Keeps leftover bytes in `accumulated` for the next read.

- **`std::string process_command(const std::string& line)`**:
  - Uses `std::istringstream` to extract the command verb:
    - `SET <key> <value>`: Extracts key and the remaining string as value; calls `cache_.put(key, value)`; returns `"OK\n"`.
    - `GET <key>`: Calls `cache_.get(key)`; returns `"<value>\n"` or `"NOT_FOUND\n"`.
    - `DEL <key>`: Calls `cache_.del(key)`; returns `"DELETED\n"` or `"NOT_FOUND\n"`.
    - `STATS`: Calls `cache_.get_stats()`; returns formatted statistics string.
    - Any unknown/malformed input: Returns `"ERROR <reason>\n"`.

- **`bool send_all(socket_t fd, const std::string& data)`**:
  - TCP sockets do not guarantee sending all bytes in one `send()` call.
  - `send_all()` loops until `total_sent == data.size()`.
  - Uses `MSG_NOSIGNAL` flag to prevent `SIGPIPE` signals if the client suddenly disconnects during transmission.

---

### Component 4: Entry Point (`src/main.cpp`)

#### Responsibility:
Parses command-line arguments, sets up signal handling for graceful shutdown, and instantiates the `Server`.

#### Key Mechanisms:
- **Command-Line Arguments**:
  - `./kvstore [port=8080] [capacity=1024] [threads=hardware_concurrency]`
- **Signal Handling (`handle_sigint`)**:
  - Bridges the POSIX C-style signal handler to the C++ `Server` object via an atomic pointer `std::atomic<Server*> g_running_server`.
  - When the user presses `Ctrl+C` (`SIGINT`), `handle_sigint()` calls `server->stop()`, initiating the graceful shutdown sequence.
  - On Windows, hooks into `SetConsoleCtrlHandler` for identical behavior.
- **`signal(SIGPIPE, SIG_IGN)`**:
  - Prevents the operating system from terminating the process if a client disconnects while the server is writing to a socket.

---

### Component 5: Test Suites

#### 1. Unit Tests (`tests/test_lru_cache.cpp`):
Standalone executable validating all cache invariant rules with zero third-party dependencies:
- `test_basic_insert_and_get`: Verifies key storage and retrieval.
- `test_value_update`: Verifies updating existing keys does not increase size or cause duplicate entries.
- `test_evict_order`: Verifies the oldest item is evicted when capacity is exceeded.
- `test_get_refreshes_recency`: Verifies that reading an item promotes it to MRU, protecting it from being evicted next.
- `test_delete`: Verifies removing existent and non-existent keys.
- `test_capacity_one_edge_case`: Verifies behavior when capacity is strictly 1 (continuous eviction).

#### 2. Multi-Threaded Load Test (`tests/load_test.cpp`):
- Spawns $N$ concurrent client threads.
- Each thread opens a TCP connection to the server.
- Concurrently sends $M$ interleaved `SET` and `GET` commands.
- Measures total wall-clock time using `std::chrono::high_resolution_clock`.
- Computes throughput (requests per second) and reports success/failure counts.

---

## 3. Step-by-Step Execution Lifecycle

Here is what happens inside the program when you run the server and send a command:

```
Step 1: Program Launch
        ./kvstore 8080 1024 4
        - main() parses arguments.
        - Server instantiates LRUCache(1024) and ThreadPool(4).
        - ThreadPool spawns 4 worker threads; all 4 block on cv_.wait().
        - Server calls start(): creates socket, sets SO_REUSEADDR, binds to 8080, listens.
        - Main thread blocks inside accept().

Step 2: Client Connects
        nc localhost 8080
        - Kernel completes TCP 3-way handshake.
        - accept() unblocks and returns client_fd (e.g., 5).
        - Main thread calls thread_pool_.enqueue(lambda capturing client_fd).
        - Main thread immediately calls accept() again.

Step 3: Worker Dispatch
        - Worker Thread 2 wakes up from cv_.wait().
        - Worker Thread 2 pops task and executes handle_client(5).
        - client_fd is wrapped in ScopedSocket.
        - Worker Thread 2 calls recv(5, buffer).

Step 4: Command Arrival
        Client sends: "SET username Alice\n"
        - recv() returns 19 bytes.
        - handle_client() buffers bytes, finds '\n'.
        - process_command("SET username Alice") is invoked.
        - LRUCache::put("username", "Alice") acquires mutex_.
        - Creates Node("username", "Alice"), inserts into map_ and head_.
        - mutex_ is released.
        - send_all(5, "OK\n") transmits response.
        - Client receives "OK".

Step 5: Client Disconnect
        Client presses Ctrl+C or exits.
        - Kernel sends TCP FIN packet to server.
        - Worker Thread 2's recv() returns 0 (EOF).
        - handle_client() breaks out of loop.
        - ScopedSocket destructor calls close(5).
        - Worker Thread 2 loops back and calls cv_.wait() for new tasks.

Step 6: Server Shutdown
        User presses Ctrl+C on server terminal.
        - SIGINT signal triggers handle_sigint().
        - server->stop() sets running_ = false.
        - shutdown(server_fd_) unblocks accept().
        - thread_pool_.shutdown() wakes all workers (cv_.notify_all()).
        - All worker threads exit and join().
        - main() exits with code 0. Zero memory leaks, zero socket leaks.
```

---

## 4. Quick Verification Commands

```bash
# 1. Build everything
make all
make test
make load_test

# 2. Run unit tests
./test_lru

# 3. Start server
./kvstore 8080 1000 4

# 4. In another terminal, run load test (4 threads, 2500 req/thread = 20,000 commands)
./load_test 4 2500 8080 127.0.0.1
```

