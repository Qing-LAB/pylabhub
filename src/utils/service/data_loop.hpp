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
#include "utils/script_engine_factory.hpp"  // EngineGlobalLockRelease

#include <chrono>
#include <functional>
#include <optional>
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

    /// Mirror of `ScriptEngine::release_global_lock_during_wait()` for
    /// the engine bound to this loop.  Read by the role host BEFORE
    /// calling `run_data_loop` and stored here as a cached bool —
    /// `run_data_loop` itself never reaches into the engine to query
    /// it.  Default false → wraps below are constant-folded out.
    /// See ScriptEngine + HEP-CORE-0011 §"Engine Thread Affinity"
    /// "Optional global-lock release during idle waits".
    bool              release_global_lock_during_wait{false};
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
    const std::string &tag = api.short_tag();

    // -- Timing setup --------------------------------------------------------
    const auto policy      = cfg.loop_timing;
    const double period_us = cfg.period_us;
    const bool is_max_rate = (policy == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, cfg.queue_io_wait_timeout_ratio);
    const auto short_timeout    = std::chrono::duration_cast<std::chrono::milliseconds>(
        short_timeout_us + std::chrono::microseconds{999});

    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::time_point::max();

    // Cache the GIL-release-during-wait flag ONCE.  Read from cfg here,
    // never per-iteration via the engine — keeps the loop free of any
    // ScriptEngine virtual call.  When false (default) the inner
    // `std::optional<EngineGlobalLockRelease>` stays empty: ctor/dtor
    // are no-ops, no engine touch, identical observable behaviour to
    // the pre-flag loop.  When true the optional is emplaced ONCE per
    // cycle around the idle-wait region (Steps A + B); the GIL is
    // released across the wait and reacquired before any subsequent
    // engine-touching step (drain_inbox_sync, invoke_and_commit).
    const bool release_lock_idle = cfg.release_global_lock_during_wait;

    // -- Outer loop ----------------------------------------------------------
    //
    // HEP-CORE-0036 §8.2 outer guard — the loop runs only while at
    // least one Presence is `Authorized` (Layer 3 data plane armed,
    // §4.3.2).  When the last Authorized presence transitions out
    // (hub-dead per §4.3.3 → `Unregistered`, voluntary DEREG →
    // `Deregistered`), `any_presence_authorized()` flips false and
    // the loop exits cleanly on its next iteration.  Multi-hub roles
    // (dual-hub processor) keep running as long as any presence on
    // any live hub stays Authorized.
    while (core.is_running() &&
           !core.is_shutdown_requested() &&
           !core.is_critical_error() &&
           api.any_presence_authorized())
    {
        if (core.is_process_exit_requested())
            break;

        const auto cycle_start = Clock::now();

        // -- Steps A + B + B': idle-wait region with INSIDE-the-wrap
        //    shutdown check.  Wrapped in EngineGlobalLockRelease iff
        //    `release_lock_idle` is true.  Steps A (queue I/O wait),
        //    B (deadline sleep), AND the post-wait shutdown check
        //    (Step B') are all pure C++ — they do NOT touch the
        //    engine — so they all run with the GIL released.
        //
        //    **Why Step B' moved INSIDE the wrap (HEP-CORE-0011
        //    §"Engine Thread Affinity" sub-section "Optional global-
        //    lock release during idle waits"):**  the optional dtor
        //    at the closing brace reacquires the GIL via
        //    `PyEval_RestoreThread`, which BLOCKS until the GIL is
        //    available.  If a script-spawned non-yielding C
        //    extension (Flask handler with a long native call,
        //    NumPy op without `nogil`, buggy module) holds the GIL,
        //    that reacquire blocks indefinitely.  By checking the
        //    shutdown flag (a plain C++ atomic read — no GIL
        //    needed) BEFORE the dtor's reacquire, we guarantee that
        //    a shutdown signal is OBSERVED and that
        //    `cleanup_on_shutdown` (audited GIL-free for all three
        //    roles — SHM queue ops only) RUNS even on the worst
        //    case where the reacquire would block.  The reacquire
        //    is still attempted on scope exit; if it blocks, the
        //    bounded join in `EngineHost::shutdown_()` (HEP-CORE-0011
        //    §"ThreadManager" → "Bounded Shutdown Join") detaches
        //    the worker thread after the timeout so the parent
        //    unblocks.
        AcquireContext ctx{short_timeout, short_timeout_us, deadline, is_max_rate};
        bool has_data;
        bool shutdown_observed = false;
        {
            std::optional<scripting::EngineGlobalLockRelease> idle_release;
            if (release_lock_idle)
                idle_release.emplace();

            // -- Step A: Role-specific acquire -----------------------------------
            has_data = ops.acquire(ctx);

            // -- Step B: Deadline wait -------------------------------------------
            if (!is_max_rate && has_data &&
                deadline != Clock::time_point::max() && Clock::now() < deadline)
            {
                std::this_thread::sleep_until(deadline);
            }

            // -- Step B': Shutdown observation + cleanup INSIDE the wrap.
            //    Atomic-flag reads do not need the GIL; cleanup_on_shutdown
            //    is audited GIL-free.  This must happen BEFORE the dtor
            //    reacquires the GIL — see comment block above.
            if (!core.is_running() ||
                core.is_shutdown_requested() ||
                core.is_critical_error() ||
                core.is_process_exit_requested())
            {
                ops.cleanup_on_shutdown();
                shutdown_observed = true;
            }
        }

        if (shutdown_observed)
            break;

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

    // HEP-CORE-0036 §8.2 — if the loop exited solely because the
    // §4.3.2 outer guard flipped (no presence Authorized) while the
    // other flags are still in their running positions, log it
    // explicitly.  In production this means a hub died and the role
    // has nothing left to serve; in tests it usually means the test
    // set up the data loop without walking presences through to
    // `Authorized` (test misconfiguration).  Either way, the WARN
    // surfaces the cause instead of leaving operators to guess from
    // a silent exit.
    const bool guard_exit =
        !api.any_presence_authorized() &&
        core.is_running() &&
        !core.is_shutdown_requested() &&
        !core.is_critical_error() &&
        !core.is_process_exit_requested();
    if (guard_exit)
    {
        LOGGER_WARN("[{}] run_data_loop exiting via §8.2 outer guard: "
                    "no Presence is Authorized.  In production this "
                    "means the last live hub for this role died (see "
                    "HEP-CORE-0036 §4.3.3); in tests it may mean a "
                    "setup path did not transition any presence through "
                    "to Authorized.", tag);
    }

    LOGGER_INFO("[{}] run_data_loop exiting: running={} shutdown={} critical={} "
                "any_presence_authorized={}",
                tag, core.is_running(), core.is_shutdown_requested(),
                core.is_critical_error(), api.any_presence_authorized());
}

} // namespace pylabhub::scripting
