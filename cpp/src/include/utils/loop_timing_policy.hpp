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
 * Cross-field constraint (enforced at config parse time):
 *   - `"max_rate"`  requires `target_period_ms == 0` (or absent). Error otherwise.
 *   - `"fixed_rate"` / `"fixed_rate_with_compensation"` require `target_period_ms > 0`.
 *
 * When `"loop_timing"` is absent the default is derived implicitly:
 *   - `target_period_ms == 0`  → MaxRate
 *   - `target_period_ms > 0`   → FixedRate
 */

#include <cstdint>
#include <stdexcept>
#include <string>

namespace pylabhub
{

// ============================================================================
// LoopTimingPolicy
// ============================================================================

/**
 * @enum LoopTimingPolicy
 * @brief Timing policy for the role iteration loop.
 *
 * | Value                     | target_period_ms | Sleep behaviour |
 * |---------------------------|-----------------|-----------------|
 * | MaxRate                   | must be 0        | No sleep; iterate as fast as possible. |
 * | FixedRate                 | must be > 0      | Sleep to next start-to-start deadline. On overrun: reset deadline to `now + target_period_ms` (no catch-up; safe default). |
 * | FixedRateWithCompensation | must be > 0      | Same, but on overrun: advance deadline from the original target (catches up lost time; risk of cascading overruns). |
 *
 * **MaxRate** is an explicit, first-class policy — not a side-effect of setting period to 0.
 * Setting `"loop_timing": "max_rate"` in config makes the intent unambiguous and is validated.
 */
enum class LoopTimingPolicy : uint8_t
{
    MaxRate,                   ///< No sleep between iterations; maximum throughput.
    FixedRate,                 ///< Fixed start-to-start period; reset deadline on overrun (safe default for timed loops).
    FixedRateWithCompensation, ///< Fixed period; advance deadline from original target on overrun (maintains average rate).
};

// ============================================================================
// parse_loop_timing_policy — cross-field validated parse
// ============================================================================

/**
 * @brief Parse "loop_timing" JSON field and validate against target_period_ms.
 *
 * @param timing_str  Value of the "loop_timing" JSON field.
 * @param period_ms   Already-parsed target_period_ms (0 or positive).
 * @param context     Prefix for error messages, e.g. "Producer config".
 * @throws std::runtime_error on invalid value or cross-field violation.
 */
inline LoopTimingPolicy parse_loop_timing_policy(const std::string &timing_str,
                                                  int                period_ms,
                                                  const std::string &context)
{
    LoopTimingPolicy policy;
    if (timing_str == "max_rate") {
        policy = LoopTimingPolicy::MaxRate;
    } else if (timing_str == "fixed_rate") {
        policy = LoopTimingPolicy::FixedRate;
    } else if (timing_str == "fixed_rate_with_compensation") {
        policy = LoopTimingPolicy::FixedRateWithCompensation;
    } else {
        throw std::runtime_error(context + ": invalid 'loop_timing': '" + timing_str +
                                 "' (expected 'max_rate', 'fixed_rate', or "
                                 "'fixed_rate_with_compensation')");
    }

    if (policy == LoopTimingPolicy::MaxRate && period_ms != 0) {
        throw std::runtime_error(context + ": 'loop_timing: max_rate' requires "
                                 "'target_period_ms' to be 0 or absent");
    }
    if (policy != LoopTimingPolicy::MaxRate && period_ms == 0) {
        throw std::runtime_error(context + ": 'loop_timing: " + timing_str +
                                 "' requires 'target_period_ms' > 0");
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
 *   - target_period_ms == 0  → MaxRate
 *   - target_period_ms > 0   → FixedRate
 */
inline LoopTimingPolicy default_loop_timing_policy(int period_ms) noexcept
{
    return period_ms == 0 ? LoopTimingPolicy::MaxRate : LoopTimingPolicy::FixedRate;
}

} // namespace pylabhub
