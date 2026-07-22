#pragma once
/**
 * @file startup_config.hpp
 * @brief StartupConfig — categorical config for HEP-0023 startup coordination.
 *
 * Parsed from "startup.wait_for_roles" JSON array.
 * Single source of truth for wait_for_roles validation.
 */

#include "utils/startup_wait.hpp" // WaitForRole, kDefaultStartupWaitTimeoutMs, kMaxStartupWaitTimeoutMs

#include "utils/json_fwd.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace pylabhub::config
{

struct StartupConfig
{
    std::vector<pylabhub::WaitForRole> wait_for_roles;

    /// HEP-CORE-0032 §8.6 strict-mode ABI reject on the role side.
    /// When true, on a broker REG_ACK / CONSUMER_REG_ACK whose ABI
    /// envelope reports a MAJOR-axis mismatch, the role refuses the
    /// Registered transition (`register_producer` / `apply_consumer_reg_ack`
    /// return false) instead of just logging.  Default false — same
    /// behaviour as pre-#327 log-only mode.  Task #327.
    bool strict_abi_mismatch{false};

    /// HEP-CORE-0041 §D4.5 mutual-auth requirement on the consumer's
    /// SHM attach handshake.  When true, `initiate_consumer_handshake`
    /// runs the 3-frame variant and requires the producer to prove
    /// possession of the seckey matching the `producer_pubkey_z85`
    /// broker delivered in `CONSUMER_REG_ACK.producers[i].zmq_pubkey`.
    /// Producer-side impersonation (a squatter that opens the same
    /// AF_UNIX endpoint after the real producer crashes) fails Frame 3
    /// and the consumer refuses the attach with a
    /// PRODUCER_NOT_AUTHENTICATED marker.  **Default true (#262 close-out,
    /// HEP-CORE-0044 §8.4): mutual auth is on by default; consumers
    /// challenge the producer's identity end-to-end.**  Set false only to
    /// interoperate with pre-#262 role builds that speak the 2-frame
    /// handshake.  Task #262.
    bool shm_require_mutual_auth{true};
};

/// Parse "startup.wait_for_roles" from a JSON config object.
/// @param j   Root JSON object.
/// @param tag Context tag for error messages.
inline StartupConfig parse_startup_config(const nlohmann::json &j, const char *tag)
{
    (void)tag; // used in error messages below

    StartupConfig sc;

    if (!j.contains("startup") || !j.at("startup").is_object())
        return sc;

    const auto &s = j.at("startup");
    // Reject unknown nested keys under "startup".
    for (auto it = s.begin(); it != s.end(); ++it)
    {
        if (it.key() != "wait_for_roles" && it.key() != "strict_abi_mismatch" &&
            it.key() != "shm_require_mutual_auth")
            throw std::runtime_error("startup: unknown config key 'startup." + it.key() + "'");
    }

    // HEP-CORE-0032 §8.6 strict-mode ABI reject (task #327, 2026-07-03).
    // Optional bool; default false (log-only ABI-check behaviour).
    if (s.contains("strict_abi_mismatch"))
    {
        if (!s.at("strict_abi_mismatch").is_boolean())
            throw std::runtime_error("startup.strict_abi_mismatch: must be a boolean");
        sc.strict_abi_mismatch = s.at("strict_abi_mismatch").get<bool>();
    }

    // HEP-CORE-0041 §D4.5 mutual-auth opt-in (task #262, 2026-07-03).
    if (s.contains("shm_require_mutual_auth"))
    {
        if (!s.at("shm_require_mutual_auth").is_boolean())
            throw std::runtime_error("startup.shm_require_mutual_auth: must be a boolean");
        sc.shm_require_mutual_auth = s.at("shm_require_mutual_auth").get<bool>();
    }

    if (!s.contains("wait_for_roles") || !s.at("wait_for_roles").is_array())
        return sc;

    for (const auto &w : s.at("wait_for_roles"))
    {
        if (!w.is_object())
            throw std::runtime_error("startup.wait_for_roles: each entry must be a JSON object");
        // Reject unknown keys per array entry.
        for (auto it = w.begin(); it != w.end(); ++it)
        {
            const auto &k = it.key();
            if (k != "uid" && k != "timeout_ms")
                throw std::runtime_error("startup.wait_for_roles[].: unknown config key '" + k +
                                         "'");
        }
        if (!w.contains("uid") || !w.at("uid").is_string())
            throw std::runtime_error("startup.wait_for_roles: each entry must have a string 'uid'");

        pylabhub::WaitForRole wr;
        wr.uid = w.at("uid").get<std::string>();

        if (wr.uid.empty())
            throw std::runtime_error("startup.wait_for_roles: 'uid' must not be empty");

        wr.timeout_ms = w.value("timeout_ms", pylabhub::kDefaultStartupWaitTimeoutMs);

        if (wr.timeout_ms <= 0)
            throw std::runtime_error("startup.wait_for_roles: timeout_ms must be > 0 for uid='" +
                                     wr.uid + "'");

        if (wr.timeout_ms > pylabhub::kMaxStartupWaitTimeoutMs)
            throw std::runtime_error(
                "startup.wait_for_roles: timeout_ms exceeds maximum (3600000 ms) for uid='" +
                wr.uid + "'");

        sc.wait_for_roles.push_back(std::move(wr));
    }

    return sc;
}

} // namespace pylabhub::config
