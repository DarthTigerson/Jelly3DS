#pragma once

#include <3ds.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace app {

/// Runs a std::function on a libctru thread with an explicit priority / core (std::thread can't set these).
class Thread {
public:
    Thread() = default;
    ~Thread() { join(); }
    bool start(std::function<void()> fn, int priority, int core = -2, size_t stack = 64 * 1024);
    void join();
    bool running() const { return handle_ != nullptr; }

private:
    static void entry(void* arg);
    ::Thread handle_ = nullptr;
    std::function<void()> fn_;
};

/// One background worker for UI network requests, plus a queue of callbacks to run on the main thread.
class Tasks {
public:
    void start();
    void stop();

    void run(std::function<void()> work);
    void post(std::function<void()> fn);
    /// Runs main-thread callbacks; call once per frame.
    void drainMain();
    bool busy() const;

private:
    void loop();

    Thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> work_;
    std::deque<std::function<void()>> main_;
    bool stopping_ = false;
    int active_ = 0;
};

}  // namespace app
