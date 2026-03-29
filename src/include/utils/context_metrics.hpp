#pragma once
/**
 * @file context_metrics.hpp
 * @brief Per-session timing metrics for queue operations.
 *
 * ContextMetrics is a transport-agnostic metrics container for queue
 * acquire/release timing. It is owned at the queue level:
 *   - SHM path: DataBlock pImpl owns the instance; ShmQueue reads via DataBlock::metrics()
 *   - ZMQ path: ZmqQueue owns the instance directly
 *
 * All timing fields (wait, iteration, exec, elapsed) are written by the
 * queue implementation's acquire/release internals. The `configured_period_us`
 * field is written by the queue's set_timing_params() — not by DataBlock.
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
 * Exposed through QueueReader/QueueWriter::metrics() → QueueMetrics bridge.
 * All mutators are inline (zero cost). Fields are private; use accessors.
 */
struct PYLABHUB_UTILS_EXPORT ContextMetrics
{
    using Clock = std::chrono::steady_clock;

    // ── Readers (const) ─────────────���────────────────────────────────────────

    [[nodiscard]] Clock::time_point context_start_time_val() const noexcept { return context_start_time_; }
    [[nodiscard]] uint64_t context_elapsed_us_val() const noexcept { return context_elapsed_us_; }
    [[nodiscard]] uint64_t last_slot_wait_us_val()  const noexcept { return last_slot_wait_us_; }
    [[nodiscard]] uint64_t last_iteration_us_val()  const noexcept { return last_iteration_us_; }
    [[nodiscard]] uint64_t max_iteration_us_val()   const noexcept { return max_iteration_us_; }
    [[nodiscard]] uint64_t last_slot_exec_us_val()  const noexcept { return last_slot_exec_us_; }
    [[nodiscard]] uint64_t checksum_error_count_val() const noexcept
    {
        return checksum_error_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t configured_period_us_val() const noexcept { return configured_period_us_; }

    // ── Writers (mutators) ─────────────���─────────────────────────────────────

    // Session boundaries
    void set_context_start(Clock::time_point t) noexcept { context_start_time_ = t; }
    void set_context_elapsed(uint64_t us)       noexcept { context_elapsed_us_ = us; }

    // Acquire/release timing
    void set_last_slot_wait(uint64_t us)  noexcept { last_slot_wait_us_ = us; }
    void set_last_iteration(uint64_t us)  noexcept { last_iteration_us_ = us; }
    void set_max_iteration(uint64_t us)   noexcept { max_iteration_us_ = us; }
    void update_max_iteration(uint64_t us) noexcept
    {
        if (us > max_iteration_us_)
        {
            max_iteration_us_ = us;
        }
    }
    void set_last_slot_exec(uint64_t us) noexcept { last_slot_exec_us_ = us; }

    // Checksum
    void inc_checksum_error() noexcept
    {
        checksum_error_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Config
    void set_configured_period(uint64_t us) noexcept { configured_period_us_ = us; }

    // ── Reset ──────────��─────────────────────────────��───────────────────────

    /// Clear all counters. If preserve_config is true, configured_period_us is
    /// preserved (it's configuration, not a measurement).
    void clear(bool preserve_config = true) noexcept
    {
        const auto saved = preserve_config ? configured_period_us_ : uint64_t{0};
        context_start_time_ = {};
        context_elapsed_us_ = 0;
        last_slot_wait_us_  = 0;
        last_iteration_us_  = 0;
        max_iteration_us_   = 0;
        last_slot_exec_us_  = 0;
        checksum_error_count_.store(0, std::memory_order_relaxed);
        configured_period_us_ = saved;
    }

  private:
    // ── Session boundaries ────────────────────────────────────────────────────
    Clock::time_point context_start_time_{};  ///< Set on first acquire; zero until then.
    uint64_t          context_elapsed_us_{0}; ///< Elapsed since context_start_time (µs).

    // ── Acquire/release timing ───────────────────��───────────────────────────
    uint64_t last_slot_wait_us_{0};  ///< Time blocking inside acquire (µs).
    uint64_t last_iteration_us_{0};  ///< Start-to-start between acquires (µs).
    uint64_t max_iteration_us_{0};   ///< Peak iteration time since reset (µs). Init 0 = any first measure wins.
    uint64_t last_slot_exec_us_{0};  ///< Acquire to release (µs).

    // ── Checksum ��───────────────────────────────��────────────────────────────
    std::atomic<uint64_t> checksum_error_count_{0}; ///< Atomic: written from data thread, read from metrics snapshot.

    // ── Config ──────────────────────────────────────────���────────────────────
    uint64_t configured_period_us_{0}; ///< Target period (µs). 0 = MaxRate. Set by queue, not DataBlock.
};

} // namespace pylabhub::hub

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
