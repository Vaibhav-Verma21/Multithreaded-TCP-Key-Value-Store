# kvstore-cpp: Systems & Architecture Interview Guide

This guide breaks down the core systems programming concepts, network protocols, concurrency primitives, and algorithmic designs implemented in **`kvstore-cpp`**. It is structured for systems engineering interviews (e.g., Qualcomm, infrastructure, embedded, and distributed systems roles).

---

## 1. End-to-End Request Flow

From the moment an incoming TCP SYN packet hits the network interface card (NIC) to the response transmission:

```
[Client]                [Kernel Network Stack]             [kvstore-cpp Process]
   |                              |                                  |
   |--- TCP 3-Way Handshake ----->| (SYN -> SYN-ACK -> ACK)          |
   |                              | Stored in OS Accept Queue        |
   |                              |                                  |
   |                              |<--- accept() in Main Thread -----|
   |                              |                                  |
   |                              | Returns client_fd                |
   |                              |--------------------------------->| Main Thread
   |                              |                                  | Enqueues task to ThreadPool
   |                              |                                  | cv.notify_one()
   |                              |                                  v
   |                              |                          [Worker Thread]
   |                              |<--- recv(client_fd, buf) --------|
   |--- "SET user:1 Alice\n" ---->|                                  |
   |                              | Returns bytes into buf           |
   |                              |--------------------------------->| Appends to client buffer
   |                              |                                  | Extracts full line ending in '\n'
   |                              |                                  |
   |                              |                                  | Dispatches: process_command()
   |                              |                                  | Acquires LRUCache::mutex_
   |                              |                                  | Updates Hash Map & Linked List
   |                              |                                  | Releases LRUCache::mutex_
   |                              |                                  |
   |                              |<--- send_all(client_fd, "OK\n")--| Writes to TCP Send Buffer
   |<-- TCP ACK + "OK\n" ---------|                                  |
   |                              |                                  | Continues recv() until FIN (EOF)
   |--- TCP FIN (Disconnect) ---->|                                  |
   |                              | recv() returns 0                 |
   |                              |--------------------------------->| ScopedSocket closes client_fd
```

### Detailed Lifecycle
1. **TCP Connection Establishment**: The Linux kernel manages the TCP 3-way handshake in the background. Completed connections move from the SYN backlog queue to the Accept backlog queue.
2. **Accepting Connection**: The main listener thread calls `accept()`, dequeuing a socket descriptor (`client_fd`) from the Accept queue.
3. **Thread Pool Dispatch**: The main thread creates a lambda capturing `client_fd` and pushes it into the `ThreadPool` task queue under `queue_mutex_`, then signals `cv_.notify_one()`. The main thread immediately loops back to `accept()` to avoid head-of-line blocking for subsequent connection arrivals.
4. **Task Acquisition**: An idle worker thread wakes from `cv_.wait()`, pops the task, releases `queue_mutex_`, and begins executing `handle_client(client_fd)`.
5. **Partial Read Buffering**: The worker reads bytes into a stack buffer using `recv()`. Because TCP is a byte-stream protocol with no message boundaries, bytes are accumulated in a `std::string` buffer. The worker scans for `\n`, slicing out complete command lines while keeping unconsumed trailing bytes for subsequent reads.
6. **Command Execution & Synchronization**: The parsed command (e.g. `SET`, `GET`) calls the thread-safe `LRUCache`. The worker acquires `LRUCache::mutex_`, updates the hash table and node pointers, and releases the lock.
7. **Reliable Transmission**: The response string is transmitted using `send_all()`, which loops until all bytes are sent, handling potential partial socket writes and ignoring `SIGPIPE`.
8. **Teardown**: When the client closes the connection, `recv()` returns `0`. The stack-allocated `ScopedSocket` leaves scope, automatically calling `close(client_fd)`.

---

## 2. Core Concepts Explained

### A. How the LRU Cache Achieves $O(1)$ Time Complexity
A Least Recently Used (LRU) cache must support three operations in strictly $O(1)$ time:
1. `get(key)`: Lookup value and promote key to Most Recently Used (MRU).
2. `put(key, value)`: Insert/update value, promote key to MRU, and evict the Least Recently Used (LRU) entry if capacity is exceeded.
3. `del(key)`: Remove key and its associated node.

#### Why a Hash Map Alone Fails:
A hash map provides $O(1)$ key-to-value lookup, but cannot track chronological access order. Finding the oldest item would require scanning all entries ($O(N)$).

#### Why a Doubly Linked List Alone Fails:
A doubly linked list allows $O(1)$ node insertion and deletion *if you already have a pointer to the node*. However, looking up a key by value requires traversing the list ($O(N)$).

#### The Combined Solution:
- **`std::unordered_map<std::string, Node*>`**: Maps each key to the raw pointer of its corresponding doubly linked list node in $O(1)$.
- **Custom Doubly Linked List with Sentinels (`head_` and `tail_`)**:
  - `head_->next` points to the MRU node.
  - `tail_->prev` points to the LRU node.
  - Splicing a node out requires updating two pointers:
    ```cpp
    node->prev->next = node->next;
    node->next->prev = node->prev;
    ```
  - Inserting a node at `head_` requires updating four pointers.
  - Evicting from `tail_` takes `tail_->prev` in $O(1)$.
- **Dummy Sentinels**: Eliminate edge cases (empty list, inserting at head, evicting single remaining element) and eliminate all `if (node == head)` branch conditions.

---

### B. Why Mutex Locking is Required (Concrete Race Condition Example)
Consider two concurrent worker threads calling `get()` on different keys without synchronization:

```
Thread A: get("keyA")                     Thread B: get("keyB")
--------------------------------------------------------------------------
1. itA = map_.find("keyA")
   Node* nodeA = itA->second;
                                          1. itB = map_.find("keyB")
                                             Node* nodeB = itB->second;
2. detach(nodeA):
   nodeA->prev->next = nodeA->next;
                                          2. detach(nodeB):
                                             nodeB->prev->next = nodeB->next;
3. attach_head(nodeA):
   nodeA->next = head_->next;
                                          3. attach_head(nodeB):
                                             nodeB->next = head_->next;
   nodeA->prev = head_;
   head_->next->prev = nodeA;
                                             nodeB->prev = head_;
                                             head_->next->prev = nodeB;
   head_->next = nodeA;                   // OVERWRITES head_->next!
                                             head_->next = nodeB;
```

#### What Goes Wrong:
- **Corrupted Pointers**: Both threads read `head_->next` simultaneously. Thread B's assignment overwrites Thread A's pointer update.
- **Memory Leak / Lost Nodes**: `nodeA` is detached from the list or its back-pointer is orphaned, creating a cycle or dangling pointer.
- **Hash Map Rehash Crash**: If Thread A calls `put()` (triggering a `std::unordered_map` bucket rehash) while Thread B calls `get()` (reading buckets), Thread B reads corrupted bucket pointers, leading to a segmentation fault.

#### Why `std::shared_mutex` Does NOT Help an LRU Cache:
In traditional read-heavy systems, a reader-writer lock (`std::shared_mutex`) allows multiple concurrent readers via `std::shared_lock`. However, in an LRU cache, **a read operation (`get`) is also a write operation** because it updates node recency by moving the accessed node to the MRU head. Because `get()` mutates internal pointers, concurrent `get()` calls require exclusive access. Using a shared lock on `get()` would cause data races on the linked list pointers.

---

### C. How the Condition Variable Avoids Busy Waiting

#### The Busy-Waiting Anti-Pattern:
```cpp
// BAD: Burns 100% CPU core spinning
while (tasks_.empty()) {
    // spin
}
```

#### The `std::condition_variable` Mechanism:
A condition variable interacts directly with the OS kernel scheduler (via `futex` on Linux):
1. The thread locks `queue_mutex_` and evaluates the predicate:
   ```cpp
   cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
   ```
2. If `stop_ == false` and `tasks_.empty() == true`, the runtime:
   - Atomically releases `queue_mutex_`.
   - Transitions the thread to `TASK_INTERRUPTIBLE` sleep state.
   - Yields the CPU core to other processes (0% CPU utilization while idle).
3. When another thread pushes a task, it calls `cv_.notify_one()`. The kernel wakes one waiting thread, re-acquires `queue_mutex_`, and re-evaluates the predicate.

#### Why the Wait Predicate Lambda is Mandatory:
- **Spurious Wakeups**: POSIX and OS kernel scheduling allow threads to wake from `wait()` without an explicit signal (due to kernel signal interrupts or multi-core race resolutions). A simple `if (!tasks_.empty())` would crash if woken spuriously on an empty queue. The predicate loops until the condition actually holds.
- **Lost Wakeups**: If `notify_one()` occurs after worker thread checks the condition but before it calls `wait()`, the notification is lost. The predicate checks `!tasks_.empty()` *while holding the lock*, ensuring it never sleeps if work is already queued.

---

### D. Socket Call Reference Guide

| Socket API | Purpose & Internal Mechanics |
| :--- | :--- |
| `socket(AF_INET, SOCK_STREAM, 0)` | Allocates an IPv4 TCP socket file descriptor in the kernel table. |
| `setsockopt(..., SO_REUSEADDR, ...)` | Allows reusing a local address/port in `TIME_WAIT` state, preventing `EADDRINUSE` errors on restart. |
| `bind(fd, &addr, sizeof(addr))` | Associates the socket file descriptor with an IP address (`INADDR_ANY`) and port. |
| `listen(fd, SOMAXCONN)` | Transitions the socket from active (connecting) to passive (listening). `SOMAXCONN` defines the maximum pending connection backlog. |
| `accept(fd, &client_addr, &len)` | Extracts the first completed connection from the listening socket's accept queue; creates a new socket file descriptor for communication. |
| `recv(fd, buf, len, 0)` | Reads incoming bytes from the socket's TCP receive buffer. Returns bytes read, `0` on peer disconnect (EOF), or `-1` on error. |
| `send(fd, buf, len, flags)` | Copies application data into the kernel's TCP send buffer. Does not guarantee immediate network transmission. |
| `shutdown(fd, SHUT_RDWR)` | Disables further send and receive operations on the socket; unblocks blocked threads. |
| `close(fd)` | Decrements the kernel reference count for the file descriptor. Deallocates the socket when count reaches 0. |

---

### E. Concurrency Architecture: Thread-per-Connection vs. Thread Pool

| Metric / Dimension | Thread-per-Connection | Fixed Worker Thread Pool |
| :--- | :--- | :--- |
| **Model** | Spawns a new `std::thread` for every incoming client connection. | Pre-allocates $K$ worker threads; incoming clients queued as tasks. |
| **Resource Overhead** | High: Each OS thread allocates 2–8 MB default stack memory. 10,000 clients = ~40 GB RAM. | Low: Fixed memory footprint regardless of total client count. |
| **Context Switching** | Severe thrashing when active connection count exceeds CPU core count. | Bounded: CPU cores remain saturated without scheduler thrashing. |
| **OS Limits** | Quickly hits `/proc/sys/kernel/threads-max` or `ulimit -u`. | Operates within fixed, predictable process boundaries. |
| **Degradation Mode** | Out-Of-Memory (OOM) or thread creation failure (`EAGAIN`). | Latency queueing: Extra requests wait in queue without crashing the process. |

---

## 3. Top 15 Systems / Qualcomm Interview Questions

#### Q1: Why did you use dummy head and tail sentinel nodes in the doubly linked list?
**Answer**: Dummy head and tail nodes eliminate null pointer boundary checks when inserting, removing, or moving nodes. Every active node always has valid non-null `prev` and `next` pointers. This simplifies the code, reduces branching (improving branch prediction in the CPU pipeline), and prevents edge-case bugs when the cache is empty or has only one element.

#### Q2: What is the exact time and space complexity of your LRU cache?
**Answer**:
- Time Complexity: Strictly $O(1)$ for `get`, `put`, and `del`. Hash map lookup is average $O(1)$, and splicing/inserting in a doubly linked list involves a constant number (4 to 6) of pointer updates.
- Space Complexity: $O(C)$ where $C$ is cache capacity. Each entry requires a hash table bucket entry, a `Node` structure (pointers and key/value strings), and auxiliary heap memory.

#### Q3: Why can't we use a reader-writer lock (`std::shared_mutex`) to allow parallel `get()` reads?
**Answer**: In an LRU cache, a `get()` operation is not read-only. It mutates internal state by detaching the accessed node and moving it to the MRU position (head of the list). Because pointers are modified, concurrent reads would create data races and corrupt the linked list unless protected by an exclusive lock.

#### Q4: How does `std::condition_variable` work under the hood on Linux?
**Answer**: On Linux, `std::condition_variable` is backed by the `futex` (Fast Userspace Mutex) system call. In the absence of contention, state checking occurs in user space with atomic operations. When a thread must wait, it invokes `sys_futex(..., FUTEX_WAIT, ...)` to sleep in the kernel. The waking thread calls `sys_futex(..., FUTEX_WAKE, ...)` to move the sleeping task back to the scheduler's run queue.

#### Q5: What is a spurious wakeup, and why does the operating system allow it?
**Answer**: A spurious wakeup occurs when a waiting thread wakes from a condition variable without any thread explicitly calling `notify_one()` or `notify_all()`. Operating systems permit this because handling corner cases across multi-core architectures (e.g. signal interrupts, kernel context switches, or race conditions inside the kernel futex implementation) without spurious wakeups would require expensive synchronization overhead that slows down the fast path.

#### Q6: What does `SO_REUSEADDR` do, and what problem does it solve?
**Answer**: When a server process terminates, connections that were active enter the TCP `TIME_WAIT` state (lasting $2 \times \text{MSL}$, typically 1–2 minutes) to ensure any lingering delayed packets are drained from the network. Without `SO_REUSEADDR`, restarting the server on the same port immediately fails with `EADDRINUSE` ("Address already in use"). `SO_REUSEADDR` informs the kernel to allow binding to the port even if existing connections are in `TIME_WAIT`.

#### Q7: What is the difference between `close()` and `shutdown()` on a socket?
**Answer**:
- `shutdown()` closes the communication channel in one or both directions (`SHUT_RD`, `SHUT_WR`, or `SHUT_RDWR`) by sending a FIN packet, regardless of how many processes or threads share the file descriptor. It unblocks any thread blocked in `accept()`, `recv()`, or `send()`.
- `close()` decrements the kernel file table reference count. If other descriptors (e.g. created via `fork()`) reference the socket, the underlying connection remains open until all references are closed.

#### Q8: What is `SIGPIPE`, and how does your server handle it?
**Answer**: If a client closes its connection and the server writes to that socket, the first write receives a TCP RST. A second write to the RST-terminated socket triggers the OS signal `SIGPIPE`. By default, `SIGPIPE` terminates the process. We handle this by setting `signal(SIGPIPE, SIG_IGN)` and passing `MSG_NOSIGNAL` to `send()`, so `send()` returns `-1` with `errno == EPIPE` instead of killing the server.

#### Q9: Why is TCP message framing necessary? Can you assume one `recv()` equals one command?
**Answer**: No. TCP is a continuous byte-stream protocol, not a message-oriented protocol like UDP. The kernel can fragment or coalesce application data across IP packets based on MTU, Nagle's algorithm, or TCP window sizes. A single `recv()` call can return half a command, multiple commands, or one and a half commands. Therefore, application protocols must implement framing (e.g., delimiter-based framing like `\n` or length-prefixed framing).

#### Q10: How does the Accept queue backlog work in the Linux kernel?
**Answer**: The kernel maintains two queues for listening sockets:
1. **SYN Queue (Incomplete Handshakes)**: Holds connections that have sent a SYN and are awaiting completion of the 3-way handshake (SYN-ACK / ACK).
2. **Accept Queue (Completed Handshakes)**: Holds connections whose 3-way handshake has completed and are waiting for the application to invoke `accept()`.
`listen(fd, backlog)` specifies the maximum size of the Accept queue. If this queue fills up, incoming connection requests are dropped or ignored, causing clients to retry with exponential backoff.

#### Q11: What is the difference between blocking I/O and non-blocking I/O?
**Answer**: In blocking I/O, system calls (`accept`, `recv`, `send`) suspend the calling thread until data or connection events arrive. In non-blocking I/O (`O_NONBLOCK`), if no data is available, the call immediately returns `-1` with `errno == EAGAIN` or `EWOULDBLOCK`. Non-blocking I/O is typically combined with I/O multiplexing (`epoll`/`select`/`kqueue`).

#### Q12: Why are tasks executed outside the `ThreadPool` queue lock?
**Answer**: If a worker executed `task()` while holding `queue_mutex_`, all other worker threads would be blocked from acquiring, enqueuing, or dequeuing tasks. Worker threads would be effectively serialized into single-threaded execution. Acquiring the lock only to pop the task and immediately releasing it ensures minimal lock hold time and high concurrency.

#### Q13: What is False Sharing and how can it impact multithreaded performance?
**Answer**: False sharing occurs when threads running on different CPU cores modify independent variables that reside on the same CPU cache line (typically 64 bytes). Whenever one core writes to its variable, the cache coherency protocol (e.g. MESI) invalidates the entire cache line across all cores, forcing expensive cache invalidations and memory bus transfers. It is prevented by padding data structures using `alignas(hardware_destructive_interference_size)`.

#### Q14: How does RAII help prevent file descriptor and memory leaks in network servers?
**Answer**: File descriptors are limited operating system resources (governed by `RLIMIT_NOFILE`). If a client disconnects unexpectedly, a malformed packet triggers an early return, or an exception is thrown, naked `close()` calls can be bypassed. Wrapping descriptors in an RAII class (`ScopedSocket`) guarantees that `close()` is invoked deterministically whenever the object leaves scope on the call stack.

#### Q15: How would you scale this server from 1,000 to 1,000,000 concurrent connections (C10K / C1000K problem)?
**Answer**: A thread-per-connection or thread-per-active-socket model cannot scale to 1,000,000 connections due to thread memory and context-switching overhead. The solution is:
1. Switch to an event-driven Reactor architecture using Linux `epoll` with Edge-Triggered mode (`EPOLLET`).
2. Maintain non-blocking sockets in an epoll event loop.
3. Keep worker threads focused purely on processing cache operations, decoupling connection monitoring from computation.
4. Tune kernel TCP parameters (`net.ipv4.tcp_max_syn_backlog`, `somaxconn`, `sysctl fs.file-max`).

---

## 4. Possible Architectural Extensions

### 1. Event-Driven I/O with Linux `epoll` (Reactor Pattern)
Instead of dedicating worker threads to block on individual client sockets:
- A single I/O thread runs `epoll_wait()`, managing tens of thousands of active socket connections simultaneously.
- When `EPOLLIN` fires, the I/O thread reads all available bytes and passes completed command buffers to the worker pool for processing.
- When `EPOLLOUT` is ready, responses are written back non-blockingly.

### 2. Time-To-Live (TTL) Key Expiration with a Min-Heap
To support commands like `EXPIRE <key> <seconds>`:
- Maintain a min-heap (priority queue) of pairs `(expiration_timestamp, key)`.
- Passive Expiration: Whenever `get(key)` is called, check if its timestamp has elapsed. If expired, delete it and return `NOT_FOUND`.
- Active Expiration: A periodic background cleaner thread inspects the top of the min-heap. If `heap.top().timestamp <= now()`, pop and erase the key.

### 3. Durability & Persistence via Append-Only Log (AOF)
To survive server restarts:
- Every mutating command (`SET`, `DEL`) is serialized and written to a disk log file before or immediately after modifying the in-memory cache.
- Configurable `fsync` policies:
  - `fsync` on every command (maximum durability, lowest throughput).
  - `fsync` once per second (industry standard balance used by Redis).
  - Rely on OS page cache flushing (`fsync` left to kernel).
- On server startup, re-read and replay the AOF log from beginning to end to restore cache state.

