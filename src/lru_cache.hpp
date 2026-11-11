#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <cstddef>

/**
 * LRUCache
 *
 * Thread-safe Least Recently Used (LRU) Cache implementation using a custom
 * doubly linked list and std::unordered_map to achieve strictly O(1) time
 * complexity for get, put, and del operations.
 *
 * Thread Safety Note (std::mutex vs std::shared_mutex):
 * ---------------------------------------------------
 * One might wonder if std::shared_mutex (reader-writer lock) could allow concurrent
 * reads via get(). In a traditional cache, reads don't mutate state. However, in an
 * LRU cache, every get() MUST update recency by detaching the node and moving it to
 * the front of the list. Because get() mutates pointer relationships (prev/next),
 * it is inherently a mutating operation.
 *
 * Using a shared read lock during get() would lead to concurrent pointer modifications,
 * causing race conditions, pointer corruption, or cycles. Therefore, an exclusive
 * std::mutex is required for all access. (Alternatives like 2Q, read-evict queues, or
 * sharded mutexes exist, but for standard LRU, exclusive locking is correct and safe.)
 */
class LRUCache {
public:
    struct Stats {
        size_t hits{0};
        size_t misses{0};
        size_t evictions{0};
        size_t size{0};
        size_t capacity{0};
    };

    explicit LRUCache(size_t capacity);
    ~LRUCache();

    // Non-copyable, non-movable to ensure clean concurrency semantics
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;
    LRUCache(LRUCache&&) = delete;
    LRUCache& operator=(LRUCache&&) = delete;

    // O(1) Get: returns value if found, refreshes recency to MRU (front)
    std::optional<std::string> get(const std::string& key);

    // O(1) Put: inserts or updates key-value pair; evicts LRU if exceeding capacity
    void put(const std::string& key, const std::string& value);

    // O(1) Delete: removes key if present; returns true if deleted, false if not found
    bool del(const std::string& key);

    // Returns a thread-safe snapshot of cache statistics
    Stats get_stats() const;

private:
    struct Node {
        std::string key;
        std::string value;
        Node* prev{nullptr};
        Node* next{nullptr};

        Node() = default;
        Node(std::string k, std::string v) : key(std::move(k)), value(std::move(v)) {}
    };

    // Helper functions for doubly linked list manipulation (caller must hold mutex_)
    void detach(Node* node);
    void attach_head(Node* node);

    const size_t capacity_;
    size_t size_{0};

    // Sentinel dummy nodes to eliminate boundary null checks
    Node* head_{nullptr}; // head_->next is MRU
    Node* tail_{nullptr}; // tail_->prev is LRU

    std::unordered_map<std::string, Node*> map_;

    mutable std::mutex mutex_;
    size_t hits_{0};
    size_t misses_{0};
    size_t evictions_{0};
};

