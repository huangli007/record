#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <chrono>

namespace nr {

// Bounded thread-safe queue used by the producer-consumer pipeline.
// Supports timeout-based push/pop, closing and draining.
template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t capacity = 64) : capacity_(capacity) {}

    // Returns false when the queue is closed or the timeout elapsed.
    bool push(T item, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!notFull_.wait_for(lock, timeout, [&] { return closed_ || deque_.size() < capacity_; })) {
            return false;
        }
        if (closed_) {
            return false;
        }
        deque_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    // Pushes to the front (used by the audio mixer to hold a frame back).
    bool unshift(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || deque_.size() >= capacity_) {
            return false;
        }
        deque_.push_front(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    // Returns std::nullopt when the queue is closed and drained, or on timeout.
    std::optional<T> pop(std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!notEmpty_.wait_for(lock, timeout, [&] { return closed_ || !deque_.empty(); })) {
            return std::nullopt;
        }
        if (deque_.empty()) {
            return std::nullopt;
        }
        T item = std::move(deque_.front());
        deque_.pop_front();
        notFull_.notify_one();
        return item;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deque_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> deque_;
    size_t capacity_;
    bool closed_ = false;
};

} // namespace nr
