#pragma once
/**
 * @file data_loop.hpp
 * @brief Internal data loop framework: AcquireContext, retry_acquire, LoopConfig,
 *        and the run_data_loop template.
 *
 * This is an INTERNAL header — not part of the public pylabhub-utils API.
 * It is used by:
 *   - data_loop.cpp (retry_acquire implementation)
 *   - The three role host .cpp files (run_data_loop template instantiation)
 *   - cycle_ops.hpp (AcquireContext + retry_acquire)
 *   - Tests (include directly — tests are internal consumers)
 *
 * The run_data_loop template definition lives in this header so that each
 * call site gets full inlining of the CycleOps methods — the entire point
 * of using templates instead of virtual dispatch.
 */

#include "utils/logger.hpp"
#include "utils/loop_timing_policy.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// AcquireContext — timing state for one cycle, computed by the shared frame
// ============================================================================

/// Passed to CycleOps::acquire() so role code never computes timeouts.
struct AcquireContext
{
    std::chrono::milliseconds              short_timeout;
    std::chrono::microseconds              short_timeout_us;
    std::chrono::steady_clock::time_point  deadline;
    bool                                   is_max_rate;
};

// ============================================================================
// retry_acquire — shared inner retry utility
// ============================================================================

/// Inner retry loop used by CycleOps::acquire() for the primary acquire.
///
/// Calls try_once(short_timeout) repeatedly until:
///   - try_once returns non-null (success)
///   - is_max_rate (single attempt only)
///   - core signals shutdown or process exit
///   - remaining time until deadline < short_timeout_us
///
/// First cycle (deadline == time_point::max()): retries indefinitely
/// until success or shutdown (per loop_design_unified.md S3 Step A).
///
void *retry_acquire(
    const AcquireContext &ctx,
    RoleHostCore &core,
    const std::function<void *(std::chrono::milliseconds)> &try_once);

// ============================================================================
// LoopConfig — timing parameters for the shared data loop
// ============================================================================

struct LoopConfig
{
    double            period_us{0};
    LoopTimingPolicy  loop_timing{LoopTimingPolicy::MaxRate};
    double            queue_io_wait_timeout_ratio{0.1};
};

// ============================================================================
// run_data_loop — shared loop frame (header-defined template)
// ============================================================================

/// Runs the shared data loop: outer condition, inner retry, deadline wait,
/// drain, metrics, next deadline. Role-specific acquire/invoke/commit is
/// delegated to the Ops parameter (duck-typed — no virtual base needed).
///
/// Blocks until shutdown. The Ops type must provide:
///   - bool acquire(const AcquireContext &ctx)
///   - void cleanup_on_shutdown()
///   - bool invoke_and_commit(std::vector<IncomingMessage> &msgs)
///   - void cleanup_on_exit()
template <typename Ops>
void run_data_loop(RoleAPIBase &api, RoleHostCore &core,
                   const LoopConfig &cfg, Ops &ops)
{
    const std::string &tag = api.role_tag();

    // -- Timing setup --------------------------------------------------------
    const auto policy      = cfg.loop_timing;
    const double period_us = cfg.period_us;
    const bool is_max_rate = (policy == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, cfg.queue_io_wait_timeout_ratio);
    const auto short_timeout    = std::chrono::duration_cast<std::chrono::milliseconds>(
        short_timeout_us + std::chrono::microseconds{999});

    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::time_point::max();

    // -- Outer loop ----------------------------------------------------------
    while (core.is_running() &&
           !core.is_shutdown_requested() &&
           !core.is_critical_error())
    {
        if (core.is_process_exit_requested())
            break;

        const auto cycle_start = Clock::now();

        // -- Step A: Role-specific acquire -----------------------------------
        AcquireContext ctx{short_timeout, short_timeout_us, deadline, is_max_rate};
        bool has_data = ops.acquire(ctx);

        // -- Step B: Deadline wait -------------------------------------------
        if (!is_max_rate && has_data &&
            deadline != Clock::time_point::max() && Clock::now() < deadline)
        {
            std::this_thread::sleep_until(deadline);
        }

        // -- Step B': Shutdown check after potential sleep --------------------
        if (!core.is_running() ||
            core.is_shutdown_requested() ||
            core.is_critical_error())
        {
            ops.cleanup_on_shutdown();
            break;
        }
        if (core.is_process_exit_requested())
        {
            ops.cleanup_on_shutdown();
            break;
        }

        // -- Step C: Drain messages + inbox ----------------------------------
        auto msgs = core.drain_messages();
        api.drain_inbox_sync();

        // -- Step D+E: Role-specific invoke + commit -------------------------
        if (!ops.invoke_and_commit(msgs))
            break;

        // -- Step F: Metrics -------------------------------------------------
        const auto now     = Clock::now();
        const auto work_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cycle_start).count());
        core.set_last_cycle_work_us(work_us);
        core.inc_iteration_count();
        if (deadline != Clock::time_point::max() && now > deadline)
            core.inc_loop_overrun();

        // -- Step G: Next deadline -------------------------------------------
        deadline = compute_next_deadline(policy, deadline, cycle_start, period_us);
    }

    // -- Post-loop cleanup ---------------------------------------------------
    ops.cleanup_on_exit();

    LOGGER_INFO("[{}] run_data_loop exiting: running={} shutdown={} critical={}",
                tag, core.is_running(), core.is_shutdown_requested(),
                core.is_critical_error());
}

} // namespace pylabhub::scripting
