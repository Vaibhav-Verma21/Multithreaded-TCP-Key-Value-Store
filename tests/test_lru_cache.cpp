#include "../src/lru_cache.hpp"
#include <cassert>
#include <iostream>
#include <string>

void test_basic_insert_and_get() {
    std::cout << "[RUNNING] test_basic_insert_and_get..." << std::endl;
    LRUCache cache(2);

    cache.put("k1", "v1");
    cache.put("k2", "v2");

    auto val1 = cache.get("k1");
    assert(val1.has_value() && *val1 == "v1");

    auto val2 = cache.get("k2");
    assert(val2.has_value() && *val2 == "v2");

    auto val3 = cache.get("k3");
    assert(!val3.has_value());

    auto stats = cache.get_stats();
    assert(stats.hits == 2);
    assert(stats.misses == 1);
    assert(stats.evictions == 0);
    assert(stats.size == 2);
    assert(stats.capacity == 2);

    std::cout << "[PASSED] test_basic_insert_and_get" << std::endl;
}

void test_value_update() {
    std::cout << "[RUNNING] test_value_update..." << std::endl;
    LRUCache cache(2);

    cache.put("k1", "old_val");
    assert(cache.get("k1") == "old_val");

    cache.put("k1", "new_val");
    assert(cache.get("k1") == "new_val");

    auto stats = cache.get_stats();
    assert(stats.size == 1);
    assert(stats.evictions == 0);

    std::cout << "[PASSED] test_value_update" << std::endl;
}

void test_evict_order() {
    std::cout << "[RUNNING] test_evict_order..." << std::endl;
    LRUCache cache(2);

    cache.put("k1", "v1");
    cache.put("k2", "v2");
    // Cache order: [k2 (MRU), k1 (LRU)]

    // Inserting k3 should evict k1 (LRU)
    cache.put("k3", "v3");

    assert(!cache.get("k1").has_value());
    assert(cache.get("k2") == "v2");
    assert(cache.get("k3") == "v3");

    auto stats = cache.get_stats();
    assert(stats.evictions == 1);
    assert(stats.size == 2);

    std::cout << "[PASSED] test_evict_order" << std::endl;
}

void test_get_refreshes_recency() {
    std::cout << "[RUNNING] test_get_refreshes_recency..." << std::endl;
    LRUCache cache(2);

    cache.put("k1", "v1");
    cache.put("k2", "v2");
    // Cache order: [k2 (MRU), k1 (LRU)]

    // Access k1 -> refreshes k1 to MRU! Order now: [k1 (MRU), k2 (LRU)]
    auto val1 = cache.get("k1");
    assert(val1.has_value() && *val1 == "v1");

    // Inserting k3 must now evict k2, NOT k1!
    cache.put("k3", "v3");

    assert(cache.get("k1") == "v1");
    assert(!cache.get("k2").has_value()); // k2 was evicted
    assert(cache.get("k3") == "v3");

    auto stats = cache.get_stats();
    assert(stats.evictions == 1);

    std::cout << "[PASSED] test_get_refreshes_recency" << std::endl;
}

void test_delete() {
    std::cout << "[RUNNING] test_delete..." << std::endl;
    LRUCache cache(3);

    cache.put("k1", "v1");
    cache.put("k2", "v2");

    // Deleting nonexistent key
    assert(!cache.del("k3"));

    // Deleting existent key
    assert(cache.del("k1"));
    assert(!cache.get("k1").has_value());
    assert(cache.get_stats().size == 1);

    // Can delete remaining key
    assert(cache.del("k2"));
    assert(!cache.get("k2").has_value());
    assert(cache.get_stats().size == 0);

    // Further deletes return false
    assert(!cache.del("k2"));

    std::cout << "[PASSED] test_delete" << std::endl;
}

void test_capacity_one_edge_case() {
    std::cout << "[RUNNING] test_capacity_one_edge_case..." << std::endl;
    LRUCache cache(1);

    cache.put("k1", "v1");
    assert(cache.get("k1") == "v1");
    assert(cache.get_stats().size == 1);

    // Inserting k2 must evict k1
    cache.put("k2", "v2");
    assert(!cache.get("k1").has_value());
    assert(cache.get("k2") == "v2");
    assert(cache.get_stats().size == 1);
    assert(cache.get_stats().evictions == 1);

    // Updating k2 in capacity 1
    cache.put("k2", "v2_updated");
    assert(cache.get("k2") == "v2_updated");
    assert(cache.get_stats().size == 1);

    // Deleting from capacity 1
    assert(cache.del("k2"));
    assert(!cache.get("k2").has_value());
    assert(cache.get_stats().size == 0);

    std::cout << "[PASSED] test_capacity_one_edge_case" << std::endl;
}

int main() {
    std::cout << "=== Running LRUCache Unit Tests ===" << std::endl;
    test_basic_insert_and_get();
    test_value_update();
    test_evict_order();
    test_get_refreshes_recency();
    test_delete();
    test_capacity_one_edge_case();
    std::cout << "=== All LRUCache Unit Tests PASSED! ===" << std::endl;
    return 0;
}

