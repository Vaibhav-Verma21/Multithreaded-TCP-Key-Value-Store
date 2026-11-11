#include "thread_pool.hpp"
#include <stdexcept>

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) {
        throw std::invalid_argument("ThreadPool requires at least 1 thread");
    }

    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);

                    /**
                     * Condition Variable Wait Predicate:
                     * ----------------------------------
                     * cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                     *
                     * WHY this predicate is essential:
                     * 1. Protection against Spurious Wakeups:
                     *    Under POSIX / OS thread scheduling, condition variables may wake
                     *    up spuriously (without any thread having called notify). If we used
                     *    a bare wait() or an 'if' check, the worker would proceed to pop
                     *    from an empty queue, causing undefined behavior or crashes.
                     * 2. Prevention of Lost Wakeups:
                     *    If notify_one() was invoked just before the worker entered wait(),
                     *    the predicate checks tasks_.empty() while holding the mutex. Because
                     *    tasks_ is non-empty, wait() immediately returns without sleeping,
                     *    avoiding permanent deadlocks.
                     */
                    cv_.wait(lock, [this]() {
                        return stop_ || !tasks_.empty();
                    });

                    // If shutdown was requested and all remaining queued tasks have been processed
                    if (stop_ && tasks_.empty()) {
                        return;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }

                // CRITICAL: Execute task OUTSIDE the queue_mutex_ lock.
                // If executed while holding the lock, all other worker threads would be serialized,
                // defeating the entire purpose of multi-threading.
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) {
            throw std::runtime_error("enqueue called on stopped ThreadPool");
        }
        tasks_.push(std::move(task));
    }
    // Wake up one sleeping worker to process the newly arrived task
    cv_.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) {
            return; // Already shutting down
        }
        stop_ = true;
    }

    // Wake up all idle workers so they evaluate the predicate, observe stop_ == true, and exit
    cv_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

