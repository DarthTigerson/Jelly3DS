#include "tasks.hpp"

namespace app {

void Thread::entry(void* arg) { static_cast<Thread*>(arg)->fn_(); }

bool Thread::start(std::function<void()> fn, int priority, int core, size_t stack) {
    join();
    fn_ = std::move(fn);
    handle_ = threadCreate(entry, this, stack, priority, core, false);
    if (!handle_ && core != -2) handle_ = threadCreate(entry, this, stack, priority, -2, false);  // core unavailable
    return handle_ != nullptr;
}

void Thread::join() {
    if (!handle_) return;
    threadJoin(handle_, U64_MAX);
    threadFree(handle_);
    handle_ = nullptr;
}

void Tasks::start() {
    stopping_ = false;
    // Below the main thread (0x30) so UI rendering always wins.
    thread_.start([this] { loop(); }, 0x31, -2, 128 * 1024);
}

void Tasks::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Tasks::run(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work_.push_back(std::move(work));
    }
    cv_.notify_one();
}

void Tasks::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    main_.push_back(std::move(fn));
}

void Tasks::drainMain() {
    std::deque<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(main_);
    }
    for (auto& fn : pending) fn();
}

bool Tasks::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ > 0 || !work_.empty();
}

void Tasks::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !work_.empty(); });
            if (stopping_) return;
            job = std::move(work_.front());
            work_.pop_front();
            ++active_;
        }
        job();
        std::lock_guard<std::mutex> lock(mutex_);
        --active_;
    }
}

}  // namespace app
