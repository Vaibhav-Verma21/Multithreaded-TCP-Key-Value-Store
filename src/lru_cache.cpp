#include "lru_cache.hpp"
#include <stdexcept>

LRUCache::LRUCache(size_t capacity)
    : capacity_(capacity),
      head_(new Node()),
      tail_(new Node()) {
    if (capacity_ == 0) {
        delete head_;
        delete tail_;
        throw std::invalid_argument("LRUCache capacity must be greater than 0");
    }

    // Connect dummy sentinels
    head_->next = tail_;
    tail_->prev = head_;
}

LRUCache::~LRUCache() {
    // Traverse the list and free all dynamically allocated nodes (including dummy sentinels)
    Node* curr = head_;
    while (curr != nullptr) {
        Node* next = curr->next;
        delete curr;
        curr = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
}

void LRUCache::detach(Node* node) {
    // Splicing a node out of the doubly linked list in O(1)
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

void LRUCache::attach_head(Node* node) {
    // Inserting node immediately after dummy head (making it Most Recently Used) in O(1)
    node->next = head_->next;
    node->prev = head_;
    head_->next->prev = node;
    head_->next = node;
}

std::optional<std::string> LRUCache::get(const std::string& key) {
    // Exclusive lock required because recency update mutates prev/next pointers.
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(key);
    if (it == map_.end()) {
        misses_++;
        return std::nullopt;
    }

    hits_++;
    Node* node = it->second;
    detach(node);
    attach_head(node);

    return node->value;
}

void LRUCache::put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(key);
    if (it != map_.end()) {
        // Key exists: update value and refresh recency to MRU position
        Node* node = it->second;
        node->value = value;
        detach(node);
        attach_head(node);
        return;
    }

    // Key is new: check if capacity is reached
    if (size_ >= capacity_) {
        // Evict Least Recently Used node (node right before dummy tail)
        Node* lru = tail_->prev;
        detach(lru);
        map_.erase(lru->key);
        delete lru;
        size_--;
        evictions_++;
    }

    // Allocate and insert the new node at MRU position
    Node* new_node = new Node(key, value);
    attach_head(new_node);
    map_[key] = new_node;
    size_++;
}

bool LRUCache::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = map_.find(key);
    if (it == map_.end()) {
        return false;
    }

    Node* node = it->second;
    detach(node);
    map_.erase(it);
    delete node;
    size_--;
    return true;
}

LRUCache::Stats LRUCache::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Stats{hits_, misses_, evictions_, size_, capacity_};
}

