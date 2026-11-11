#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstddef>

/**
 * ThreadPool
 *
 * A robust, fixed-size thread pool managing worker threads and a synchronized
 * FIFO task queue.
 *
 * Thread-safety and Synchronization design:
 * - Worker threads block on std::condition_variable when no tasks are available,
 *   consuming 0% CPU (no busy waiting).
 * - Tasks are dequeued under std::mutex, but executed *outside* the critical section
 *   so workers don't serialize their execution.
 * - Clean shutdown ensures all joined threads exit properly without resource leaks.
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Submits a callable task to the work queue
    void enqueue(std::function<void()> task);

    // Initiates clean shutdown, wakes up all workers, and joins them
    void shutdown();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    bool stop_{false};
};

