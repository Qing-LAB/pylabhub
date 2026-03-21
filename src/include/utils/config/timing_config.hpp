#pragma once
/**
 * @file timing_config.hpp
 * @brief TimingConfig — categorical config for loop timing and queue I/O.
 *
 * Parsed from top-level timing fields. Uses existing shared helpers:
 * resolve_period_us(), parse_loop_timing_policy(), default_loop_timing_policy().
 */

#include "utils/json_fwd.hpp"
#include "utils/logger.hpp"
#include "utils/loop_timing_policy.hpp"

#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct TimingConfig
{
    double              target_period_ms{0.0};
    double              target_rate_hz{0.0};
    int64_t             period_us{0};           ///< Resolved from above.
    LoopTimingPolicy    loop_timing{LoopTimingPolicy::MaxRate};
    double              queue_io_wait_timeout_ratio{::pylabhub::kDefaultQueueIoWaitRatio};
    int                 slot_acquire_timeout_ms{-1};  ///< @deprecated Use ratio instead.
    int                 heartbeat_interval_ms{0};
};

/// Parse timing fields from a JSON config object.
/// @param j              Root JSON object.
/// @param tag            Context tag for error messages.
/// @param default_period Default for target_period_ms (100 for producer, 0 for consumer/processor).
inline TimingConfig parse_timing_config(const nlohmann::json &j,
                                         const char *tag,
                                         double default_period = 0.0)
{
    TimingConfig tc;
    tc.target_period_ms = j.value("target_period_ms", default_period);
    tc.target_rate_hz   = j.value("target_rate_hz",   0.0);
    tc.queue_io_wait_timeout_ratio = j.value("queue_io_wait_timeout_ratio",
                                              ::pylabhub::kDefaultQueueIoWaitRatio);
    tc.slot_acquire_timeout_ms = j.value("slot_acquire_timeout_ms", -1);
    tc.heartbeat_interval_ms   = j.value("heartbeat_interval_ms",   0);

    if (tc.target_period_ms < 0.0)
        throw std::runtime_error(
            std::string(tag) + ": 'target_period_ms' must be >= 0");

    if (tc.target_rate_hz < 0.0)
        throw std::runtime_error(
            std::string(tag) + ": 'target_rate_hz' must be >= 0");

    if (tc.queue_io_wait_timeout_ratio < ::pylabhub::kMinQueueIoWaitRatio ||
        tc.queue_io_wait_timeout_ratio > ::pylabhub::kMaxQueueIoWaitRatio)
        throw std::runtime_error(
            std::string(tag) + ": 'queue_io_wait_timeout_ratio' must be between " +
            std::to_string(::pylabhub::kMinQueueIoWaitRatio) + " and " +
            std::to_string(::pylabhub::kMaxQueueIoWaitRatio));

    tc.period_us = static_cast<int64_t>(
        ::pylabhub::resolve_period_us(tc.target_rate_hz, tc.target_period_ms, tag));

    if (j.contains("loop_timing"))
    {
        tc.loop_timing = ::pylabhub::parse_loop_timing_policy(
            j["loop_timing"].get<std::string>(),
            static_cast<double>(tc.period_us), tag);
    }
    else
    {
        tc.loop_timing = ::pylabhub::default_loop_timing_policy(
            static_cast<double>(tc.period_us));
    }

    if (j.contains("slot_acquire_timeout_ms"))
    {
        LOGGER_WARN("[{}] 'slot_acquire_timeout_ms' is deprecated; use "
                    "'queue_io_wait_timeout_ratio' instead (current: {})",
                    tag, tc.queue_io_wait_timeout_ratio);
    }

    return tc;
}

} // namespace pylabhub::config
