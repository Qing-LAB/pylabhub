#pragma once
/**
 * @file hub_config.hpp
 * @brief HubConfig — categorical config for hub/broker connection.
 *
 * Parsed from directional JSON fields: <direction>_hub_dir.
 * Hub broker endpoint and pubkey are resolved from the hub directory.
 */

#include "utils/json_fwd.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace pylabhub::config
{

struct HubConfig
{
    std::string hub_dir;        ///< Resolved absolute path to hub directory (or empty).
    std::string broker;         ///< Broker endpoint, e.g. "tcp://127.0.0.1:5570".
    std::string broker_pubkey;  ///< Z85 CURVE25519 public key (40 chars, or empty).
};

/// Parse hub config for a given direction ("in" or "out").
/// Reads <direction>_hub_dir from JSON, resolves broker endpoint + pubkey
/// from the hub directory if present.
/// @param j         Root JSON object.
/// @param base_dir  Role directory base (for resolving relative hub_dir).
/// @param direction "in" or "out".
inline HubConfig parse_hub_config(const nlohmann::json &j,
                                   const std::filesystem::path &base_dir,
                                   const char *direction)
{
    namespace fs = std::filesystem;
    const std::string prefix = std::string(direction) + "_";

    HubConfig hc;
    const std::string hub_dir_raw = j.value(prefix + "hub_dir", std::string{});

    if (hub_dir_raw.empty())
        return hc;

    // Resolve relative to role base directory.
    const fs::path p(hub_dir_raw);
    const fs::path resolved = p.is_absolute()
                              ? fs::weakly_canonical(p)
                              : fs::weakly_canonical(base_dir / p);
    hc.hub_dir = resolved.string();

    // Read broker endpoint from hub.json.
    const auto hub_json_path = resolved / "hub.json";
    std::ifstream hf(hub_json_path);
    if (hf.is_open())
    {
        try
        {
            const auto hj = nlohmann::json::parse(hf);
            hc.broker = hj.at("hub").at("broker_endpoint").get<std::string>();
        }
        catch (const std::exception &)
        {
            // hub.json exists but malformed — leave broker empty, caller decides.
        }
    }

    // Read broker pubkey from hub.pubkey.
    const auto pubkey_path = resolved / "hub.pubkey";
    if (fs::exists(pubkey_path))
    {
        std::ifstream pk(pubkey_path);
        std::getline(pk, hc.broker_pubkey);
    }

    return hc;
}

} // namespace pylabhub::config
