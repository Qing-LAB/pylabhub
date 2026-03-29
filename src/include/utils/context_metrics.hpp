#pragma once
/**
 * @file context_metrics.hpp
 * @brief Per-session timing metrics for queue operations.
 *
 * ContextMetrics is a transport-agnostic metrics container for queue
 * acquire/release timing. It is owned at the queue level:
 *   - SHM path: DataBlock pImpl owns the instance; ShmQueue reads via DataBlock::metrics()
 *   - ZMQ path: ZmqQueueImpl owns the instance directly
 *
 * All fields are std::atomic<uint64_t> with relaxed ordering — safe for
 * cross-thread reads (e.g., metrics snapshot from a monitoring thread while
 * the data thread writes). Zero cost on x86-64 (relaxed atomic = plain mov).
 *
 * See docs/HEP/HEP-CORE-0008-LoopPolicy-and-IterationMetrics.md §3.
 */
#include "pylabhub_utils_export.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::hub
{

/**
 * @struct ContextMetrics
 * @brief Per-session timing metrics for queue acquire/release operations.
 *
 * Transport-agnostic. Hosted by:
 *   - SHM: DataBlockProducer/Consumer pImpl (timing written by DataBlock internals)
 *   - ZMQ: ZmqQueueImpl (timing written by recv/send thread)
 *
 * Exposed through QueueReader/QueueWriter::metrics() -> QueueMetrics bridge.
 * All mutators are inline (zero cost). Fields are private atomic.
 */
struct PYLABHUB_UTILS_EXPORT ContextMetrics
{
    using Clock = std::chrono::steady_clock;

    // ── Readers (const, relaxed atomic load) ─────────────────────────────────

    [[nodiscard]] Clock::time_point context_start_time_val() const noexcept
    {
        return Clock::time_point{Clock::duration{context_start_ns_.load(std::memory_order_relaxed)}};
    }
    [[nodiscard]] uint64_t context_elapsed_us_val()   const noexcept { return context_elapsed_us_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t last_slot_wait_us_val()    const noexcept { return last_slot_wait_us_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t last_iteration_us_val()    const noexcept { return last_iteration_us_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t max_iteration_us_val()     const noexcept { return max_iteration_us_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t last_slot_exec_us_val()    const noexcept { return last_slot_exec_us_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t checksum_error_count_val() const noexcept { return checksum_error_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t configured_period_us_val() const noexcept { return configured_period_us_.load(std::memory_order_relaxed); }

    // ── Writers (mutators, relaxed atomic store) ─────────────────────────────

    // Session boundaries
    void set_context_start(Clock::time_point t) noexcept
    {
        context_start_ns_.store(t.time_since_epoch().count(), std::memory_order_relaxed);
    }
    void set_context_elapsed(uint64_t us) noexcept { context_elapsed_us_.store(us, std::memory_order_relaxed); }

    // Acquire/release timing
    void set_last_slot_wait(uint64_t us)  noexcept { last_slot_wait_us_.store(us, std::memory_order_relaxed); }
    void set_last_iteration(uint64_t us)  noexcept { last_iteration_us_.store(us, std::memory_order_relaxed); }
    void set_max_iteration(uint64_t us)   noexcept { max_iteration_us_.store(us, std::memory_order_relaxed); }
    void update_max_iteration(uint64_t us) noexcept
    {
        // Relaxed load+store is safe: single writer (data thread).
        // A concurrent reader may see a stale value — acceptable for diagnostics.
        if (us > max_iteration_us_.load(std::memory_order_relaxed))
        {
            max_iteration_us_.store(us, std::memory_order_relaxed);
        }
    }
    void set_last_slot_exec(uint64_t us) noexcept { last_slot_exec_us_.store(us, std::memory_order_relaxed); }

    // Checksum
    void inc_checksum_error() noexcept
    {
        checksum_error_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Config
    void set_configured_period(uint64_t us) noexcept { configured_period_us_.store(us, std::memory_order_relaxed); }

    // ── Reset ────────────────────────────────────────────────────────────────

    /// Clear all counters. If preserve_config is true, configured_period_us is
    /// preserved (it's configuration, not a measurement).
    void clear(bool preserve_config = true) noexcept
    {
        const auto saved = preserve_config ? configured_period_us_.load(std::memory_order_relaxed) : uint64_t{0};
        context_start_ns_.store(0, std::memory_order_relaxed);
        context_elapsed_us_.store(0, std::memory_order_relaxed);
        last_slot_wait_us_.store(0, std::memory_order_relaxed);
        last_iteration_us_.store(0, std::memory_order_relaxed);
        max_iteration_us_.store(0, std::memory_order_relaxed);
        last_slot_exec_us_.store(0, std::memory_order_relaxed);
        checksum_error_count_.store(0, std::memory_order_relaxed);
        configured_period_us_.store(saved, std::memory_order_relaxed);
    }

  private:
    // All fields are atomic for safe cross-thread reads (monitoring/metrics snapshot).
    // Relaxed ordering: no inter-field consistency guarantee — acceptable for diagnostics.

    // Session boundaries
    std::atomic<int64_t>  context_start_ns_{0};    ///< steady_clock time_since_epoch in ns. 0 = not started.
    std::atomic<uint64_t> context_elapsed_us_{0};   ///< Elapsed since first acquire (us).

    // Acquire/release timing
    std::atomic<uint64_t> last_slot_wait_us_{0};    ///< Time blocking inside acquire (us).
    std::atomic<uint64_t> last_iteration_us_{0};    ///< Start-to-start between acquires (us).
    std::atomic<uint64_t> max_iteration_us_{0};     ///< Peak iteration time since reset. Init 0 = any first measure wins.
    std::atomic<uint64_t> last_slot_exec_us_{0};    ///< Acquire to release (us).

    // Checksum
    std::atomic<uint64_t> checksum_error_count_{0}; ///< Verification failures.

    // Config
    std::atomic<uint64_t> configured_period_us_{0}; ///< Target period (us). 0 = MaxRate. Set by queue.
};

} // namespace pylabhub::hub

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
