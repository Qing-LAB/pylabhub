#pragma once
/**
 * @file timeout_constants.hpp
 * @brief Canonical timeout and poll-interval constants for pylabhub.
 *
 * ## Timeout tiers
 *
 * | Constant         | Default | Purpose                                    |
 * |------------------|---------|--------------------------------------------|
 * | kShortTimeoutMs  | 1 000 ms | Lifecycle shutdown for lightweight modules |
 * | kMidTimeoutMs    | 5 000 ms | Lifecycle shutdown for ZMQ/Python services |
 * | kLongTimeoutMs   | 60 000 ms| Reserved for long-running operations       |
 *
 * ## Poll/retry intervals (fixed — not cmake-overridable)
 *
 * | Constant            | Value  | Purpose                                    |
 * |---------------------|--------|--------------------------------------------|
 * | kZmqPollIntervalMs  | 50 ms  | Peer/ctrl/data thread poll slice           |
 * | kAdminPollIntervalMs| 100 ms | AdminShell REP worker poll slice           |
 * | kRetrySliceMs       | 500 ms | discover_producer retry sleep slice        |
 *
 * ## CMake overrides (build-time only)
 *
 * The three tier values can be overridden at configure time:
 * @code{.sh}
 * cmake -S . -B build -DPYLABHUB_SHORT_TIMEOUT_MS=2000
 * cmake -S . -B build -DPYLABHUB_MID_TIMEOUT_MS=10000
 * @endcode
 */

#ifndef PYLABHUB_SHORT_TIMEOUT_MS
#  define PYLABHUB_SHORT_TIMEOUT_MS 1000   ///< 1 s
#endif
#ifndef PYLABHUB_MID_TIMEOUT_MS
#  define PYLABHUB_MID_TIMEOUT_MS   5000   ///< 5 s
#endif
#ifndef PYLABHUB_LONG_TIMEOUT_MS
#  define PYLABHUB_LONG_TIMEOUT_MS  60000  ///< 60 s (reserved)
#endif

namespace pylabhub {

/// Lifecycle shutdown timeout for lightweight modules (JsonConfig, crypto, HubConfig).
inline constexpr int kShortTimeoutMs = PYLABHUB_SHORT_TIMEOUT_MS;

/// Lifecycle shutdown timeout for heavyweight services (ZMQ, Python interpreter, AdminShell).
inline constexpr int kMidTimeoutMs = PYLABHUB_MID_TIMEOUT_MS;

/// Reserved for long-running operations (not yet used in production paths).
inline constexpr int kLongTimeoutMs = PYLABHUB_LONG_TIMEOUT_MS;

/// ZMQ poll slice for peer/ctrl/data background threads.
/// Fixed at 50 ms — reducing this increases CPU consumption with no protocol benefit.
inline constexpr int kZmqPollIntervalMs = 50;

/// Poll slice for the AdminShell REP worker and shutdown sleep loops.
/// Fixed at 100 ms — coarser than ZMQ poll; latency here is not user-visible.
inline constexpr int kAdminPollIntervalMs = 100;

/// discover_producer() retry sleep slice.
/// Fixed at 500 ms — aligns with broker channel_timeout_s granularity.
inline constexpr int kRetrySliceMs = 500;

} // namespace pylabhub
