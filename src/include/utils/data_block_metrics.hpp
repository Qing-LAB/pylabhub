#pragma once
/**
 * @file data_block_metrics.hpp
 * @brief Layer 1 per-handle timing metrics for DataBlock producers and consumers.
 *
 * ContextMetrics records timing data for a single DataBlockProducer or
 * DataBlockConsumer session. It is stored in the Pimpl of each handle
 * (not in shared memory) and is updated at acquire/release sites.
 *
 * Included automatically by data_block.hpp. Use this header directly only
 * when ContextMetrics is needed without the full DataBlock implementation.
 *
 * See docs/HEP/HEP-CORE-0008-LoopPolicy-and-IterationMetrics.md §3 for the
 * full domain model (5 metric domains, collection sites, pass-through contract).
 */
#include "pylabhub_utils_export.h"

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
 * @brief Per-handle timing metrics for a DataBlock producer or consumer.
 *
 * Owned by DataBlockProducer::Impl / DataBlockConsumer::Impl (Pimpl storage).
 * Updated at acquire and release sites — never stored in shared memory.
 * Survives across with_transaction() calls; call clear_metrics() to reset.
 *
 * Access:
 *   DataBlockProducer::metrics() / DataBlockConsumer::metrics() — const ref to Pimpl.
 *   TransactionContext::metrics() — pass-through reference to the same storage.
 *
 * Metric domains (HEP-CORE-0008 §3):
 *   Domain 2: Acquire/release timing (wait time, start-to-start interval).
 *   Domain 3: Loop scheduling (overrun count relative to configured_period_us).
 */
struct PYLABHUB_UTILS_EXPORT ContextMetrics
{
    using Clock = std::chrono::steady_clock;

    // ── Session boundaries ────────────────────────────────────────────────────
    Clock::time_point context_start_time{};  ///< Set on first acquire; zero until then.
    uint64_t          context_elapsed_us{0}; ///< Elapsed since context_start_time (µs); updated per acquire.
    Clock::time_point context_end_time{};    ///< Zero while running; set on handle destruction.

    // ── Domain 2: Acquire/release timing ─────────────────────────────────────
    uint64_t last_slot_wait_us{0};  ///< Time spent blocking inside acquire_*_slot() (µs).
    uint64_t last_iteration_us{0};  ///< Start-to-start elapsed between the last two acquires (µs).
    uint64_t max_iteration_us{0};   ///< Peak start-to-start elapsed since session start (µs).
    uint64_t iteration_count{0};    ///< Successful slot acquisitions since session start.

    // ── Domain 3: Data flow ─────────────────────────────────────────────────
    uint64_t data_drop_count{0};   ///< Producer: slots overwritten before consumer read (Latest_only). Consumer: always 0.
    uint64_t last_slot_exec_us{0}; ///< Time from acquire to release (user code + overhead) (µs).

    // ── Config reference (informational) ──────────────────────────────────────
    uint64_t configured_period_us{0}; ///< Target period from config (µs). 0 = MaxRate. Informational only.
};

} // namespace pylabhub::hub

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
