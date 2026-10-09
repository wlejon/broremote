#pragma once
// Runs `on_timeout` if done() is not called within `ms`: bounds a blocking
// handshake (the callback shuts the stream down, so the blocked read returns).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace broremote {

class Watchdog {
public:
    Watchdog(uint32_t ms, std::function<void()> on_timeout)
        : thread_([this, ms, f = std::move(on_timeout)] {
              std::unique_lock<std::mutex> lk(m_);
              if (!cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return done_; })) {
                  fired_ = true;
                  f();
              }
          }) {}
    ~Watchdog() { done(); }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    void done() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] bool fired() const { return fired_; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    std::atomic<bool> fired_{false};
    std::thread thread_;  // last: starts once the rest exists
};

}  // namespace broremote
