#pragma once
/**
 * @file startup_wait.hpp
 * @brief WaitForRole: one entry in the startup.wait_for_roles config list (HEP-0023).
 *
 * All three role binaries (producer, consumer, processor) parse this from their
 * JSON config and block in start_role() before calling on_init until each listed
 * role is present in the broker registry (or the per-role timeout expires).
 *
 * JSON format (inside producer.json / consumer.json / processor.json):
 * @code{.json}
 * {
 *   "startup": {
 *     "wait_for_roles": [
 *       { "uid": "prod.sensor.uid3a7f2b1c", "timeout_ms": 10000 },
 *       { "uid": "cons.logger.uid9e1d4c2a", "timeout_ms": 5000 }
 *     ]
 *   }
 * }
 * @endcode
 *
 * Constraints:
 *  - uid must be non-empty.
 *  - timeout_ms must be > 0 and <= 3600000 (1 hour, default: 10000).
 *  - UIDs are matched exactly against the broker's role registry.
 */

#include <string>
#include <vector>

namespace pylabhub
{

/// Default per-role wait timeout when `timeout_ms` is absent from the JSON config.
inline constexpr int kDefaultStartupWaitTimeoutMs = 10000;

/// Maximum allowed per-role wait timeout (1 hour). Prevents runaway startup hangs.
inline constexpr int kMaxStartupWaitTimeoutMs = 3'600'000;

/// One role-presence requirement for startup coordination (HEP-0023).
struct WaitForRole
{
    std::string uid;                                      ///< Exact role UID to wait for (e.g. "prod.sensor.uid3a7f2b1c").
    int         timeout_ms{kDefaultStartupWaitTimeoutMs}; ///< Per-role wait timeout (ms). Must be > 0.
};

} // namespace pylabhub
