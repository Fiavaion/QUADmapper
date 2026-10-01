#pragma once

// One background thread running posted tasks in order (TD-35): opening media files and
// freeing what owned decode threads, FFmpeg contexts or D3D11 objects, so neither waits
// on the render thread. The thread starts with the first task. The destructor runs every
// task still queued, then joins: whatever a task touches must outlive the worker.

#include "Logger.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace quadmapper {

class BackgroundWorker {
public:
    explicit BackgroundWorker(std::string name) : name_(std::move(name)) {}

    ~BackgroundWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    BackgroundWorker(const BackgroundWorker&) = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;

    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
            if (!thread_.joinable()) thread_ = std::thread(&BackgroundWorker::run, this);
        }
        wake_.notify_all();
    }

    // Blocks until every task posted before the call has finished.
    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }

    // Tasks queued or running.
    size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size() + (busy_ ? 1 : 0);
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;  // stopping, and nothing left to run
            std::function<void()> task = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            try {
                task();
            } catch (const std::exception& e) {
                Logger::error("{}: a background task failed: {}", name_, e.what());
            } catch (...) {
                Logger::error("{}: a background task failed", name_);
            }
            task = nullptr;  // whatever it captured is freed here, on this thread
            lock.lock();
            busy_ = false;
            if (queue_.empty()) idle_.notify_all();
        }
    }

    const std::string name_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> queue_;
    bool busy_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace quadmapper
