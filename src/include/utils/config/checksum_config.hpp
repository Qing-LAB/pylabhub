#pragma once
/**
 * @file checksum_config.hpp
 * @brief ChecksumConfig — per-role checksum policy and flexzone control.
 *
 * Parsed from role-level JSON fields (not per-direction):
 *   "checksum":          "enforced" | "manual" | "none"
 *   "flexzone_checksum": true | false
 *
 * The policy applies uniformly to all data streams owned by the role.
 * The creator (producer) writes the policy to SharedMemoryHeader;
 * consumers read and obey it. For ZMQ/Inbox, the zero-checksum wire
 * convention signals the producer's intent (see config_single_truth.md).
 */

#include "utils/json_fwd.hpp"
#include "utils/data_block_policy.hpp"

#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct ChecksumConfig
{
    hub::ChecksumPolicy policy{hub::ChecksumPolicy::Enforced}; ///< Default: auto checksum.
    bool flexzone{true};  ///< Checksum flexzone if present. SHM-specific; ZMQ ignores.
};

/// Parse checksum fields from a JSON config object.
/// @param j    Root JSON object.
/// @param tag  Context tag for error messages (e.g. "prod", "cons").
/// @throws std::runtime_error on invalid "checksum" value.
inline ChecksumConfig parse_checksum_config(const nlohmann::json &j, const char *tag)
{
    ChecksumConfig cc;

    if (config_has(j, "checksum"))
    {
        const auto &val = j["checksum"].get<std::string>();
        if (val == "enforced")
        {
            cc.policy = hub::ChecksumPolicy::Enforced;
        }
        else if (val == "manual")
        {
            cc.policy = hub::ChecksumPolicy::Manual;
        }
        else if (val == "none")
        {
            cc.policy = hub::ChecksumPolicy::None;
        }
        else
        {
            throw std::runtime_error(
                std::string(tag) + ": invalid 'checksum': '" + val +
                "' (expected 'enforced', 'manual', or 'none')");
        }
    }
    // Default: Enforced (safety first).

    cc.flexzone = config_value(j, "flexzone_checksum", true);

    return cc;
}

} // namespace pylabhub::config
