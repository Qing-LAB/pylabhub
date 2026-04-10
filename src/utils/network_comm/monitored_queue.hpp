/**
 * @file monitored_queue.hpp
 * @brief MonitoredQueue<T> — bounded thread-safe command queue with optional backpressure monitoring.
 *
 * push() is called by API/application threads.
 * drain() is called by the owning thread (poll loop) on each iteration.
 *
 * Two modes, selected by Config::fire_and_forget:
 *
 *  fire_and_forget = true  (default):
 *    ZMQ-backed queues where sends always succeed at the socket level (messages are
 *    silently dropped by ZMQ for dead peers). The queue enforces max_depth and tracks
 *    metrics (total_pushed, total_dropped), but the backpressure monitoring callbacks
 *    are never invoked — they would produce false positives since the queue always
 *    drains to empty on each drain() call regardless of peer liveness.
 *    Peer-dead detection is handled externally via time-since-last-recv tracking.
 *
 *  fire_and_forget = false:
 *    Blocking or semi-blocking queues where sends may stall. Monitoring callbacks fire:
 *    1. Queue non-empty and not shrinking between checks → on_warn fired.
 *    2. Backpressure persists > dead_timeout_ms → on_dead fired once.
 *    3. Queue empty after backpressure → on_cleared fired.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <queue>

namespace pylabhub::hub
{

template <typename T>
class MonitoredQueue
{
public:
    struct Config
    {
        size_t max_depth{256};           ///< Drop oldest when exceeded; 0 = unbounded
        int    check_interval_ms{5000};  ///< Monitoring check rate-limit (ms)
        int    dead_timeout_ms{30000};   ///< Backpressure duration before dead callback (ms)
        bool   fire_and_forget{true};    ///< Skip backpressure monitoring (ZMQ queues always drain)
    };

    using WarnCallback    = std::function<void(size_t depth, int elapsed_ms)>;
    using ClearedCallback = std::function<void(int elapsed_ms)>;
    using DeadCallback    = std::function<void()>;

    explicit MonitoredQueue(Config cfg = {}) : cfg_(std::move(cfg)) {}

    // Move constructor: transfers queue contents and config; monitoring state resets.
    MonitoredQueue(MonitoredQueue &&other) noexcept
    {
        std::lock_guard lock(other.mu_);
        cfg_        = std::move(other.cfg_);
        queue_      = std::move(other.queue_);
        on_warn_    = std::move(other.on_warn_);
        on_cleared_ = std::move(other.on_cleared_);
        on_dead_    = std::move(other.on_dead_);
        total_dropped_.store(other.total_dropped_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        total_pushed_.store(other.total_pushed_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        send_errors_.store(other.send_errors_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        // Monitoring state stays default-initialised in *this.
    }

    // Move assignment: same as move constructor.
    MonitoredQueue &operator=(MonitoredQueue &&other) noexcept
    {
        if (this == &other) return *this;
        // Lock both — order by address to avoid ABBA deadlock.
        auto *lo = this < &other ? this : &other;
        auto *hi = this < &other ? &other : this;
        std::lock_guard lock1(lo->mu_);
        std::lock_guard lock2(hi->mu_);
        cfg_        = std::move(other.cfg_);
        queue_      = std::move(other.queue_);
        on_warn_    = std::move(other.on_warn_);
        on_cleared_ = std::move(other.on_cleared_);
        on_dead_    = std::move(other.on_dead_);
        total_dropped_.store(other.total_dropped_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        total_pushed_.store(other.total_pushed_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        send_errors_.store(other.send_errors_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        // Reset monitoring state: new config/queue means stale state would be inconsistent.
        backpressure_        = false;
        dead_fired_          = false;
        depth_at_last_check_ = 0;
        backpressure_start_  = {};
        last_check_          = std::chrono::steady_clock::now();
        return *this;
    }

    MonitoredQueue(const MonitoredQueue &)            = delete;
    MonitoredQueue &operator=(const MonitoredQueue &) = delete;

    /// No effect when Config::fire_and_forget == true (ZMQ mode).
    void set_on_warn(WarnCallback cb)        { on_warn_    = std::move(cb); }
    /// No effect when Config::fire_and_forget == true (ZMQ mode).
    void set_on_cleared(ClearedCallback cb)  { on_cleared_ = std::move(cb); }
    /// No effect when Config::fire_and_forget == true (ZMQ mode).
    void set_on_dead(DeadCallback cb)        { on_dead_    = std::move(cb); }

    /// Set a wake-up callback invoked after each push().
    /// Used by ZmqPollLoop: the callback sends 1 byte on an inproc PAIR
    /// socket to wake the poll loop immediately.
    void set_on_push_signal(std::function<void()> cb) noexcept
    {
        on_push_signal_ = std::move(cb);
    }

    /// Push an item. If at max_depth, drops the OLDEST item first.
    /// If a push signal is set, calls it after enqueue (outside lock).
    void push(T item)
    {
        {
            std::lock_guard lock(mu_);
            if (cfg_.max_depth > 0 && queue_.size() >= cfg_.max_depth)
            {
                queue_.pop();
                ++total_dropped_;
            }
            queue_.push(std::move(item));
            ++total_pushed_;
        }
        // Signal outside lock to avoid holding mutex during ZMQ send.
        if (on_push_signal_)
            on_push_signal_();
    }

    /// Drain all queued items via sender. sender(T&) called for each item.
    /// Monitoring check runs after drain (rate-limited by check_interval_ms).
    /// Exceptions from sender are caught per-item; send_errors_ is incremented.
    template <typename Sender>
    void drain(Sender&& sender)
    {
        size_t depth_before;
        {
            std::lock_guard lock(mu_);
            depth_before = queue_.size();
            while (!queue_.empty())
            {
                try { sender(queue_.front()); }
                catch (...) { ++send_errors_; }
                queue_.pop();
            }
        }
        if (!cfg_.fire_and_forget) run_check_(depth_before);
    }

    // Thread-safe accessors
    size_t   size()          const { std::lock_guard l(mu_); return queue_.size(); }
    uint64_t total_dropped() const { return total_dropped_.load(std::memory_order_relaxed); }
    uint64_t total_pushed()  const { return total_pushed_.load(std::memory_order_relaxed); }
    uint64_t send_errors()   const { return send_errors_.load(std::memory_order_relaxed); }
    bool     in_backpressure() const { return backpressure_; }  // drain()-thread only

private:
    mutable std::mutex mu_;
    std::queue<T>      queue_;
    Config             cfg_;
    std::function<void()> on_push_signal_; ///< Wake-up callback (optional)

    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_pushed_{0};
    std::atomic<uint64_t> send_errors_{0};

    // Monitoring state — single-writer: the drain() caller thread.
    bool     backpressure_{false};
    bool     dead_fired_{false};
    size_t   depth_at_last_check_{0};
    std::chrono::steady_clock::time_point backpressure_start_{};
    std::chrono::steady_clock::time_point last_check_{std::chrono::steady_clock::now()};

    WarnCallback    on_warn_;
    ClearedCallback on_cleared_;
    DeadCallback    on_dead_;

    void run_check_(size_t depth_before)
    {
        auto now = std::chrono::steady_clock::now();
        auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - last_check_).count();
        if (since_ms < cfg_.check_interval_ms)
            return;
        last_check_ = now;

        if (depth_before > 0 && depth_before >= depth_at_last_check_)
        {
            if (!backpressure_)
            {
                backpressure_       = true;
                dead_fired_         = false;
                backpressure_start_ = now;
            }
            auto elapsed = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - backpressure_start_).count());
            if (on_warn_) on_warn_(depth_before, elapsed);
            if (!dead_fired_ && elapsed >= cfg_.dead_timeout_ms)
            {
                dead_fired_ = true;
                if (on_dead_) on_dead_();
            }
        }
        else if (backpressure_ && depth_before == 0)
        {
            auto elapsed = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - backpressure_start_).count());
            backpressure_ = false;
            dead_fired_   = false;
            if (on_cleared_) on_cleared_(elapsed);
        }
        depth_at_last_check_ = depth_before;
    }
};

} // namespace pylabhub::hub
