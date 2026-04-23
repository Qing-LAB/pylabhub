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
        if (it.key() != "wait_for_roles")
            throw std::runtime_error(
                "startup: unknown config key 'startup." + it.key() + "'");
    }
    if (!s.contains("wait_for_roles") || !s.at("wait_for_roles").is_array())
        return sc;

    for (const auto &w : s.at("wait_for_roles"))
    {
        if (!w.is_object())
            throw std::runtime_error(
                "startup.wait_for_roles: each entry must be a JSON object");
        // Reject unknown keys per array entry.
        for (auto it = w.begin(); it != w.end(); ++it)
        {
            const auto &k = it.key();
            if (k != "uid" && k != "timeout_ms")
                throw std::runtime_error(
                    "startup.wait_for_roles[].: unknown config key '" + k + "'");
        }
        if (!w.contains("uid") || !w.at("uid").is_string())
            throw std::runtime_error(
                "startup.wait_for_roles: each entry must have a string 'uid'");

        pylabhub::WaitForRole wr;
        wr.uid = w.at("uid").get<std::string>();

        if (wr.uid.empty())
            throw std::runtime_error(
                "startup.wait_for_roles: 'uid' must not be empty");

        wr.timeout_ms = w.value("timeout_ms", pylabhub::kDefaultStartupWaitTimeoutMs);

        if (wr.timeout_ms <= 0)
            throw std::runtime_error(
                "startup.wait_for_roles: timeout_ms must be > 0 for uid='" + wr.uid + "'");

        if (wr.timeout_ms > pylabhub::kMaxStartupWaitTimeoutMs)
            throw std::runtime_error(
                "startup.wait_for_roles: timeout_ms exceeds maximum (3600000 ms) for uid='"
                + wr.uid + "'");

        sc.wait_for_roles.push_back(std::move(wr));
    }

    return sc;
}

} // namespace pylabhub::config
