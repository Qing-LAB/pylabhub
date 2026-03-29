#pragma once
/**
 * @file timing_config.hpp
 * @brief TimingConfig — categorical config for loop timing and queue I/O.
 *
 * ## Configuration rules (enforced here)
 *
 * - `loop_timing` is **required** — error if absent.
 * - `"max_rate"`: `target_period_ms` and `target_rate_hz` must NOT be present.
 * - `"fixed_rate"` / `"fixed_rate_with_compensation"`: exactly one of
 *   `target_period_ms` or `target_rate_hz` must be present.
 * - `null` values are treated as absent (see json_fwd.hpp null-as-absent convention).
 *
 * After parsing, TimingConfig contains fully validated, non-contradictory values.
 * Downstream code never re-derives policy from period.
 */

#include "utils/json_fwd.hpp"
#include "utils/loop_timing_policy.hpp"

#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct TimingConfig
{
    double              period_us{0.0};         ///< Resolved from target_period_ms or target_rate_hz. 0 = MaxRate.
    LoopTimingPolicy    loop_timing{LoopTimingPolicy::MaxRate};
    double              queue_io_wait_timeout_ratio{::pylabhub::kDefaultQueueIoWaitRatio};
    int                 heartbeat_interval_ms{0};

    /// Build the canonical LoopTimingParams from validated fields.
    [[nodiscard]] ::pylabhub::LoopTimingParams timing_params() const noexcept
    {
        return {loop_timing, static_cast<uint64_t>(period_us), queue_io_wait_timeout_ratio};
    }
};

/// Parse timing fields from a JSON config object.
/// @param j    Root JSON object.
/// @param tag  Context tag for error messages (e.g. "prod", "cons").
/// @throws std::runtime_error on validation failure.
inline TimingConfig parse_timing_config(const nlohmann::json &j, const char *tag)
{
    TimingConfig tc;

    // ── Non-timing fields (optional, with defaults) ─────────────────────
    tc.queue_io_wait_timeout_ratio = config_value(j, "queue_io_wait_timeout_ratio",
                                                   ::pylabhub::kDefaultQueueIoWaitRatio);
    tc.heartbeat_interval_ms = config_value(j, "heartbeat_interval_ms", 0);

    if (tc.queue_io_wait_timeout_ratio < ::pylabhub::kMinQueueIoWaitRatio ||
        tc.queue_io_wait_timeout_ratio > ::pylabhub::kMaxQueueIoWaitRatio)
    {
        throw std::runtime_error(
            std::string(tag) + ": 'queue_io_wait_timeout_ratio' must be between " +
            std::to_string(::pylabhub::kMinQueueIoWaitRatio) + " and " +
            std::to_string(::pylabhub::kMaxQueueIoWaitRatio));
    }

    // ── loop_timing — REQUIRED ──────────────────────────────────────────
    if (!config_has(j, "loop_timing"))
    {
        throw std::runtime_error(
            std::string(tag) + ": 'loop_timing' is required "
            "(\"max_rate\", \"fixed_rate\", or \"fixed_rate_with_compensation\")");
    }

    // Parse the policy string. Cross-field validation is below.
    tc.loop_timing = ::pylabhub::parse_loop_timing_policy(
        j["loop_timing"].get<std::string>(), tag);

    // ── Period/rate presence checks ─────────────────────────────────────
    const bool has_period = config_has(j, "target_period_ms");
    const bool has_rate   = config_has(j, "target_rate_hz");

    if (tc.loop_timing == LoopTimingPolicy::MaxRate)
    {
        // MaxRate: no period or rate allowed.
        if (has_period || has_rate)
        {
            throw std::runtime_error(
                std::string(tag) + ": 'max_rate' conflicts with "
                "'target_period_ms' / 'target_rate_hz' — remove them or set to null");
        }
        tc.period_us = 0.0;
    }
    else
    {
        // FixedRate or FixedRateWithCompensation: exactly one of period/rate.
        if (has_period && has_rate)
        {
            throw std::runtime_error(
                std::string(tag) + ": specify 'target_period_ms' OR 'target_rate_hz', not both");
        }
        if (!has_period && !has_rate)
        {
            throw std::runtime_error(
                std::string(tag) + ": '" + j["loop_timing"].get<std::string>() +
                "' requires 'target_period_ms' or 'target_rate_hz'");
        }

        const double period_ms = has_period ? j["target_period_ms"].get<double>() : 0.0;
        const double rate_hz   = has_rate   ? j["target_rate_hz"].get<double>()   : 0.0;

        if (has_period && period_ms <= 0.0)
        {
            throw std::runtime_error(
                std::string(tag) + ": 'target_period_ms' must be > 0 for fixed_rate policies");
        }
        if (has_rate && rate_hz <= 0.0)
        {
            throw std::runtime_error(
                std::string(tag) + ": 'target_rate_hz' must be > 0 for fixed_rate policies");
        }

        tc.period_us = ::pylabhub::resolve_period_us(rate_hz, period_ms, tag);
    }

    return tc;
}

} // namespace pylabhub::config
