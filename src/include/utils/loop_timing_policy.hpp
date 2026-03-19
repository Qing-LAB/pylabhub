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

static constexpr double kUsPerSecond        = 1'000'000.0;
static constexpr double kUsPerMs            = 1'000.0;
static constexpr int    kDefaultQueueCheckMs = 50; ///< Default MaxRate queue check timeout (ms).

/// Minimum period in microseconds, derived from the max rate.
static constexpr double kMinPeriodUs = kUsPerSecond / PYLABHUB_MAX_LOOP_RATE_HZ;

/// Minimum short timeout in microseconds (floor for 10%-of-period).
static constexpr double kMinShortTimeoutUs = kUsPerMs; // 1 ms

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
// parse_loop_timing_policy — cross-field validated parse
// ============================================================================

/**
 * @brief Parse "loop_timing" JSON field and validate against period.
 *
 * @param timing_str  Value of the "loop_timing" JSON field.
 * @param period_us   Already-resolved period in microseconds (0 or positive).
 * @param context     Prefix for error messages.
 * @throws std::runtime_error on invalid value or cross-field violation.
 */
inline LoopTimingPolicy parse_loop_timing_policy(const std::string &timing_str,
                                                  double             period_us,
                                                  const std::string &context)
{
    LoopTimingPolicy policy;
    if (timing_str == "max_rate")
    {
        policy = LoopTimingPolicy::MaxRate;
    }
    else if (timing_str == "fixed_rate")
    {
        policy = LoopTimingPolicy::FixedRate;
    }
    else if (timing_str == "fixed_rate_with_compensation")
    {
        policy = LoopTimingPolicy::FixedRateWithCompensation;
    }
    else
    {
        throw std::runtime_error(context + ": invalid 'loop_timing': '" + timing_str +
                                 "' (expected 'max_rate', 'fixed_rate', or "
                                 "'fixed_rate_with_compensation')");
    }

    if (policy == LoopTimingPolicy::MaxRate && period_us > 0.0)
    {
        throw std::runtime_error(
            context + ": 'loop_timing: max_rate' requires period to be 0 "
                      "(neither 'target_period_ms' nor 'target_rate_hz' may be set)");
    }
    if (policy != LoopTimingPolicy::MaxRate && period_us == 0.0)
    {
        throw std::runtime_error(
            context + ": 'loop_timing: " + timing_str +
            "' requires a positive period ('target_period_ms' or 'target_rate_hz')");
    }
    return policy;
}

// ============================================================================
// default_loop_timing_policy — implicit default when "loop_timing" is absent
// ============================================================================

/**
 * @brief Derive LoopTimingPolicy when "loop_timing" is absent from config.
 *
 * Implicit defaults:
 *   - period_us == 0  → MaxRate
 *   - period_us > 0   → FixedRate
 */
inline LoopTimingPolicy default_loop_timing_policy(double period_us) noexcept
{
    return period_us == 0.0 ? LoopTimingPolicy::MaxRate : LoopTimingPolicy::FixedRate;
}

// ============================================================================
// compute_short_timeout — inner retry acquire timeout
// ============================================================================

/**
 * @brief Compute the short timeout for the inner retry-acquire loop.
 *
 * The data loop retries slot acquisition with a short timeout:
 *   - MaxRate: uses `queue_check_timeout_ms` (user-provided, never zero).
 *   - FixedRate: 10% of period, with a floor of kMinShortTimeoutUs (1 ms).
 *
 * @param policy               Timing policy.
 * @param period_us            Period in microseconds (0 for MaxRate).
 * @param queue_check_timeout_ms  User-provided timeout for MaxRate (ms, > 0).
 * @return Short timeout as std::chrono::microseconds.
 */
inline std::chrono::microseconds compute_short_timeout(
    LoopTimingPolicy policy,
    double           period_us,
    int              queue_check_timeout_ms) noexcept
{
    if (policy == LoopTimingPolicy::MaxRate)
    {
        return std::chrono::milliseconds{queue_check_timeout_ms > 0 ? queue_check_timeout_ms
                                                                          : kDefaultQueueCheckMs};
    }
    // FixedRate / FixedRateWithCompensation: 10% of period, floor 1ms.
    const double tenth = period_us * 0.1;
    const double clamped = std::max(tenth, kMinShortTimeoutUs);
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
            return cycle_start + period;
        return prev_deadline + period;
    }

    // FixedRate: if on time, advance from prev_deadline; if overrun, reset from now.
    if (prev_deadline == std::chrono::steady_clock::time_point::max())
        return cycle_start + period; // first cycle

    const auto ideal = prev_deadline + period;
    const auto now   = std::chrono::steady_clock::now();
    if (now <= ideal)
        return ideal;       // on time — advance cleanly
    return now + period;    // overrun — reset from now (no catch-up)
}

// ============================================================================
// Backward compatibility — old API (deprecated, will be removed)
// ============================================================================

/**
 * @brief Old-style parse that accepts int period_ms.
 * @deprecated Use the double period_us overload instead.
 */
inline LoopTimingPolicy parse_loop_timing_policy(const std::string &timing_str,
                                                  int                period_ms,
                                                  const std::string &context)
{
    return parse_loop_timing_policy(timing_str, static_cast<double>(period_ms) * kUsPerMs, context);
}

/**
 * @brief Old-style default that accepts int period_ms.
 * @deprecated Use the double period_us overload instead.
 */
inline LoopTimingPolicy default_loop_timing_policy(int period_ms) noexcept
{
    return default_loop_timing_policy(static_cast<double>(period_ms) * kUsPerMs);
}

/**
 * @brief Old-style slot acquire timeout computation.
 * @deprecated Replaced by compute_short_timeout() in the new loop design.
 *
 * Kept for backward compatibility during migration. Will be removed when
 * all role hosts switch to the unified data loop.
 */
inline int compute_slot_acquire_timeout(int explicit_ms, int period_ms) noexcept
{
    static constexpr int kMaxRateDefaultMs = 50;

    if (explicit_ms == 0)
    {
        return 0;
    }
    if (explicit_ms > 0)
    {
        return explicit_ms;
    }
    // explicit_ms == -1 (or any negative): derive from period.
    if (period_ms > 0)
    {
        return period_ms / 2 > 0 ? period_ms / 2 : 1;
    }
    return kMaxRateDefaultMs;
}

} // namespace pylabhub
