#pragma once
/**
 * @file loop_timing_policy.hpp
 * @brief Shared loop timing policy — used by ProducerConfig, ConsumerConfig, ProcessorConfig.
 *
 * All three role binaries use the same three-value policy.  The policy controls how
 * the script iteration loop manages timing between successive callback invocations.
 *
 * JSON key: `"loop_timing"` with values:
 *   `"max_rate"` | `"fixed_rate"` | `"fixed_rate_with_compensation"`
 *
 * ## Rate / Period config
 *
 * Users can specify timing as either a rate (Hz) or a period (ms):
 *   - `"target_rate_hz": 1000`        → period = 1.0 ms
 *   - `"target_period_ms": 0.5`       → period = 0.5 ms (2 kHz)
 *   - Cannot specify both (config error).
 *   - Neither specified + MaxRate → period = 0 (free-run).
 *
 * ## Cross-field constraints (enforced at config parse time)
 *
 *   - `"max_rate"` requires period == 0 (or neither rate/period specified).
 *   - `"fixed_rate"` / `"fixed_rate_with_compensation"` require period > 0.
 *   - For FixedRate: minimum period = kMinPeriodUs (100 μs = 10 kHz default).
 *     Configurable at build time via `PYLABHUB_MAX_LOOP_RATE_HZ`.
 *
 * ## Data loop acquire strategy
 *
 * The data loop uses an inner retry-acquire pattern:
 *   - MaxRate: single acquire attempt with `queue_check_timeout_ms`.
 *   - FixedRate: multiple attempts with short timeout (10% of period, floor 1ms),
 *     retrying until deadline approaches.
 *
 * See docs/tech_draft/loop_design_unified.md for the full design.
 *
 * ## Canonical loop helpers — DO NOT REINVENT
 *
 * Any new periodic loop driven by this policy should use the helpers
 * defined further down in this header rather than rolling its own
 * deadline math.  The single source of truth is:
 *
 *   - `compute_next_deadline(policy, prev_deadline, cycle_start, period_us)`
 *     — handles all three policies correctly.  Returns
 *     `time_point::max()` for MaxRate (no deadline); resets to
 *     `cycle_start + period` for FixedRate; advances from
 *     `prev_deadline` for FixedRateWithCompensation (catch-up).
 *   - `compute_short_timeout(period_us, ratio)` — io-wait timeout for
 *     polled-acquire patterns.
 *
 * Reference uses:
 *   - `src/utils/service/data_loop.hpp` — `run_data_loop<Ops>` template
 *     (role-side data loop; canonical pattern).
 *   - `src/utils/service/hub_script_runner.cpp` — hub-side event /
 *     tick loop.
 *
 * Hand-rolling `next_deadline = now + period` only implements
 * FixedRate; it silently downgrades FixedRateWithCompensation to
 * FixedRate (no catch-up after overrun).  Caught and corrected
 * 2026-05-03 in HubScriptRunner during Phase 7 D1.6.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace pylabhub
{

// ============================================================================
// Compile-time limits
// ============================================================================

/// Maximum loop rate in Hz (minimum period). Default 10 kHz (100 μs period).
/// Override at build time: -DPYLABHUB_MAX_LOOP_RATE_HZ=15000
#ifndef PYLABHUB_MAX_LOOP_RATE_HZ
#define PYLABHUB_MAX_LOOP_RATE_HZ 10000
#endif

static constexpr double kUsPerSecond         = 1'000'000.0;
static constexpr double kUsPerMs             = 1'000.0;

/// Minimum period in microseconds, derived from the max rate.
static constexpr double kMinPeriodUs = kUsPerSecond / PYLABHUB_MAX_LOOP_RATE_HZ;

/// Minimum queue I/O wait timeout in microseconds.
/// This is the floor for the unified formula: `max(period * ratio, kMinQueueIoTimeoutUs)`.
/// 10 μs is achievable on modern CPU/OS (Linux high-res timers, Windows QPC).
/// For MaxRate (period=0), the formula yields 0 which clamps to this floor.
static constexpr double kMinQueueIoTimeoutUs = 10.0; // 10 μs

/// Default queue I/O wait timeout ratio (fraction of period per acquire attempt).
static constexpr double kDefaultQueueIoWaitRatio = 0.1; // 10%

/// Valid range for queue_io_wait_timeout_ratio.
static constexpr double kMinQueueIoWaitRatio = 0.1;
static constexpr double kMaxQueueIoWaitRatio = 0.5;

// ============================================================================
// LoopTimingPolicy
// ============================================================================

/**
 * @enum LoopTimingPolicy
 * @brief Timing policy for the role iteration loop.
 *
 * | Value                     | Period       | Sleep behaviour |
 * |---------------------------|-------------|-----------------|
 * | MaxRate                   | must be 0    | No sleep; iterate as fast as possible. Single acquire per cycle. |
 * | FixedRate                 | must be > 0  | Sleep to deadline. On overrun: reset deadline to `now + period` (no catch-up). |
 * | FixedRateWithCompensation | must be > 0  | Sleep to deadline. On overrun: advance deadline from cycle start (catches up). |
 *
 * **MaxRate** is an explicit, first-class policy — not a side-effect of setting period to 0.
 */
enum class LoopTimingPolicy : uint8_t
{
    MaxRate,                   ///< No sleep between iterations; maximum throughput.
    FixedRate,                 ///< Fixed start-to-start period; reset deadline on overrun.
    FixedRateWithCompensation, ///< Fixed period; advance deadline from cycle start on overrun.
};

/**
 * @struct LoopTimingParams
 * @brief Single source of truth for timing configuration.
 *
 * Created by parse_timing_config() after strict validation. Carried through
 * ProducerOptions/ConsumerOptions to establish_channel(), the queue (for metrics
 * reporting), and the main loop / SlotIterator (for execution).
 *
 * All fields are validated at parse time: MaxRate requires period_us == 0,
 * FixedRate/Compensation requires period_us > 0.
 */
struct LoopTimingParams
{
    LoopTimingPolicy policy{LoopTimingPolicy::MaxRate};
    uint64_t         period_us{0};       ///< Target period (µs). 0 = MaxRate.
    double           io_wait_ratio{kDefaultQueueIoWaitRatio}; ///< Fraction of period per acquire attempt.
};

// ============================================================================
// resolve_period_us — unify rate_hz / period_ms into microseconds
// ============================================================================

/**
 * @brief Resolve target_rate_hz / target_period_ms into a single period in μs.
 *
 * @param rate_hz     Value of "target_rate_hz" from config. 0 or negative = not set.
 * @param period_ms   Value of "target_period_ms" from config. 0 or negative = not set.
 * @param context     Prefix for error messages.
 * @return Period in microseconds. 0.0 = MaxRate (free-run).
 * @throws std::runtime_error if both are specified, or if period < kMinPeriodUs.
 */
inline double resolve_period_us(double rate_hz, double period_ms, const std::string &context)
{
    const bool has_rate   = rate_hz > 0.0;
    const bool has_period = period_ms > 0.0;

    if (has_rate && has_period)
    {
        throw std::runtime_error(
            context + ": cannot specify both 'target_rate_hz' and 'target_period_ms'");
    }

    double period_us = 0.0;
    if (has_rate)
    {
        period_us = kUsPerSecond / rate_hz;
    }
    else if (has_period)
    {
        period_us = period_ms * kUsPerMs;
    }
    // else: both absent/zero → period_us = 0 (MaxRate)

    // Enforce minimum period for FixedRate (checked later against policy).
    if (period_us > 0.0 && period_us < kMinPeriodUs)
    {
        throw std::runtime_error(
            context + ": period " + std::to_string(period_us) + " μs is below minimum " +
            std::to_string(kMinPeriodUs) + " μs (" +
            std::to_string(PYLABHUB_MAX_LOOP_RATE_HZ) + " Hz max rate). "
            "Use 'max_rate' timing policy for higher rates.");
    }

    return period_us;
}

// ============================================================================
// parse_loop_timing_policy — string → enum
// ============================================================================

/**
 * @brief Parse "loop_timing" JSON string to LoopTimingPolicy enum.
 *
 * Pure string parser — no cross-field validation. Cross-field checks
 * (policy vs period presence) are in parse_timing_config().
 *
 * @param timing_str  Value of the "loop_timing" JSON field.
 * @param context     Prefix for error messages.
 * @throws std::runtime_error on unrecognized value.
 */
inline LoopTimingPolicy parse_loop_timing_policy(const std::string &timing_str,
                                                  const std::string &context)
{
    if (timing_str == "max_rate")
    {
        return LoopTimingPolicy::MaxRate;
    }
    if (timing_str == "fixed_rate")
    {
        return LoopTimingPolicy::FixedRate;
    }
    if (timing_str == "fixed_rate_with_compensation")
    {
        return LoopTimingPolicy::FixedRateWithCompensation;
    }

    throw std::runtime_error(context + ": invalid 'loop_timing': '" + timing_str +
                             "' (expected 'max_rate', 'fixed_rate', or "
                             "'fixed_rate_with_compensation')");
}

// ============================================================================
// compute_short_timeout — inner retry acquire timeout
// ============================================================================

/**
 * @brief Compute the short timeout for the inner retry-acquire loop.
 *
 * Unified formula for ALL timing policies (no branching):
 *
 *     short_timeout = max(period_us * ratio, kMinQueueIoTimeoutUs)
 *
 * For MaxRate (period_us=0): yields the floor (10 μs).
 * For FixedRate: yields ratio × period (e.g., 100 μs at 1 kHz with ratio=0.1).
 *
 * @param period_us  Period in microseconds (0 for MaxRate).
 * @param ratio      Queue I/O wait timeout ratio (0.1–0.5, from config).
 * @return Short timeout as std::chrono::microseconds.
 */
inline std::chrono::microseconds compute_short_timeout(
    double period_us,
    double ratio) noexcept
{
    const double computed = period_us * ratio;
    const double clamped  = std::max(computed, kMinQueueIoTimeoutUs);
    return std::chrono::microseconds{static_cast<int64_t>(clamped)};
}

// ============================================================================
// compute_next_deadline — deadline for next cycle
// ============================================================================

/**
 * @brief Compute the next deadline based on timing policy.
 *
 * Called at the END of each cycle (after callback returns) to set the
 * deadline for the NEXT cycle.
 *
 * @param policy          Timing policy.
 * @param prev_deadline   The deadline that was used for THIS cycle.
 *                        For the first cycle, pass time_point::max().
 * @param cycle_start     Start time of the current cycle.
 * @param period_us       Period in microseconds.
 * @return Next deadline. For MaxRate: time_point::max() (no deadline).
 *
 * ## FixedRate
 * If on time (cycle completed before deadline + period): advance deadline
 * by one period.  If overrun (callback took longer than period): reset
 * deadline to now + period (skip the missed beat, no catch-up).
 *
 * ## FixedRateWithCompensation
 * Always advance from previous deadline by period (maintains steady average
 * rate even if individual cycles overrun — risk of cascading overruns).
 */
inline std::chrono::steady_clock::time_point compute_next_deadline(
    LoopTimingPolicy                          policy,
    std::chrono::steady_clock::time_point     prev_deadline,
    std::chrono::steady_clock::time_point     cycle_start,
    double                                    period_us) noexcept
{
    if (policy == LoopTimingPolicy::MaxRate)
    {
        return std::chrono::steady_clock::time_point::max();
    }
    const auto period = std::chrono::microseconds{static_cast<int64_t>(period_us)};

    if (policy == LoopTimingPolicy::FixedRateWithCompensation)
    {
        // Advance from the previous deadline to maintain steady average rate.
        // For the first cycle (prev_deadline == max), use cycle_start + period.
        if (prev_deadline == std::chrono::steady_clock::time_point::max())
        {
            return cycle_start + period;
        }
        return prev_deadline + period;
    }

    // FixedRate: if on time, advance from prev_deadline; if overrun, reset from now.
    if (prev_deadline == std::chrono::steady_clock::time_point::max())
    {
        return cycle_start + period; // first cycle
    }

    const auto ideal = prev_deadline + period;
    const auto now   = std::chrono::steady_clock::now();
    if (now <= ideal)
    {
        return ideal;       // on time — advance cleanly
    }
    return now + period;    // overrun — reset from now (no catch-up)
}


} // namespace pylabhub
