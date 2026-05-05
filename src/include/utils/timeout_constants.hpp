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

#include <cstdint>

#ifndef PYLABHUB_SHORT_TIMEOUT_MS
#  define PYLABHUB_SHORT_TIMEOUT_MS 1000   ///< 1 s
#endif
#ifndef PYLABHUB_MID_TIMEOUT_MS
#  define PYLABHUB_MID_TIMEOUT_MS   5000   ///< 5 s
#endif
#ifndef PYLABHUB_LONG_TIMEOUT_MS
#  define PYLABHUB_LONG_TIMEOUT_MS  60000  ///< 60 s (reserved)
#endif

// ── Heartbeat / role liveness defaults (HEP-CORE-0023 §2.5) ─────────────────
#ifndef PYLABHUB_DEFAULT_HEARTBEAT_INTERVAL_MS
#  define PYLABHUB_DEFAULT_HEARTBEAT_INTERVAL_MS 500   ///< 2 Hz client cadence
#endif
#ifndef PYLABHUB_DEFAULT_READY_MISS_HEARTBEATS
#  define PYLABHUB_DEFAULT_READY_MISS_HEARTBEATS 10    ///< Ready -> Pending after 10 missed
#endif
#ifndef PYLABHUB_DEFAULT_PENDING_MISS_HEARTBEATS
#  define PYLABHUB_DEFAULT_PENDING_MISS_HEARTBEATS 10  ///< Pending -> deregistered after +10 missed
#endif
#ifndef PYLABHUB_DEFAULT_GRACE_HEARTBEATS
#  define PYLABHUB_DEFAULT_GRACE_HEARTBEATS 4          ///< CLOSING_NOTIFY -> FORCE_SHUTDOWN window
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

/// Default client heartbeat cadence (2 Hz). Single source of truth for the
/// broker's role-liveness math: both ready and pending timeouts are
/// computed as heartbeat_interval * miss_heartbeats (HEP-CORE-0023 §2.5).
inline constexpr int kDefaultHeartbeatIntervalMs = PYLABHUB_DEFAULT_HEARTBEAT_INTERVAL_MS;

/// Default missed-heartbeat count before Ready -> Pending demotion.
inline constexpr uint32_t kDefaultReadyMissHeartbeats =
    PYLABHUB_DEFAULT_READY_MISS_HEARTBEATS;

/// Default missed-heartbeat count before Pending -> deregistered + CHANNEL_CLOSING_NOTIFY.
inline constexpr uint32_t kDefaultPendingMissHeartbeats =
    PYLABHUB_DEFAULT_PENDING_MISS_HEARTBEATS;

/// Default grace window (in heartbeats) between CHANNEL_CLOSING_NOTIFY and FORCE_SHUTDOWN.
inline constexpr uint32_t kDefaultGraceHeartbeats =
    PYLABHUB_DEFAULT_GRACE_HEARTBEATS;

/// discover_producer() / DISC_REQ retry sleep slice.
/// Defaults to kDefaultHeartbeatIntervalMs so the client retries roughly once
/// per broker heartbeat window.
inline constexpr int kRetrySliceMs = kDefaultHeartbeatIntervalMs;

/// HEP-CORE-0033 §12.2.2 default cross-thread wait bound for hub
/// script-side response augmentation (`HubAPI::augment_*`).  Admin
/// or broker thread blocks on the worker for up to
/// (kDefaultAugmentTimeoutHeartbeats * kDefaultHeartbeatIntervalMs)
/// before giving up and shipping the default response with status
/// `InvokeStatus::TimedOut`.  Scripts may override at runtime via
/// `api.set_augment_timeout(ms)` (typically inside `on_start`):
///   -1 → infinite
///    0 → non-blocking (no augmentation effect)
///   >0 → wait N ms
inline constexpr uint32_t kDefaultAugmentTimeoutHeartbeats = 30;

} // namespace pylabhub
