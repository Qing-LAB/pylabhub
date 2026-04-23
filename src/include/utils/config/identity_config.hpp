#pragma once
/**
 * @file identity_config.hpp
 * @brief IdentityConfig — categorical config for role identity.
 *
 * Parsed from the role-specific JSON section ("producer", "consumer", "processor").
 * Handles UID auto-generation and prefix validation.
 */

#include "utils/json_fwd.hpp"
#include "utils/uid_utils.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pylabhub::config
{

struct IdentityConfig
{
    std::string uid;                    ///< Role UID, e.g., "PROD-TEMPSENS-12345678"
    std::string name;                   ///< Human-readable name
    std::string log_level{"info"};      ///< "debug", "info", "warn", "error"
};

/// Parse identity from the role-specific JSON section.
/// @param j         Root JSON object.
/// @param role_tag  Role type: "producer", "consumer", "processor".
///                  Determines the JSON section name and UID prefix.
inline IdentityConfig parse_identity_config(const nlohmann::json &j,
                                             std::string_view role_tag)
{
    if (!j.contains(std::string(role_tag)) || !j[std::string(role_tag)].is_object())
        throw std::runtime_error(
            std::string(role_tag) + " config: missing '" + std::string(role_tag) + "' object");

    const auto &sect = j[std::string(role_tag)];
    // Reject unknown nested keys in the role-tag block.  The block
    // holds identity fields (uid/name/log_level) plus the auth
    // sub-object (validated separately by parse_auth_config).
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "uid" && k != "name" && k != "log_level" && k != "auth")
            throw std::runtime_error(
                std::string(role_tag) + ": unknown config key '"
                + std::string(role_tag) + "." + k + "'");
    }
    IdentityConfig ic;
    ic.uid       = sect.value("uid",       "");
    ic.name      = sect.value("name",      "");
    ic.log_level = sect.value("log_level", "info");

    // Short tag for logging (pre-lifecycle).
    const char *short_tag = (role_tag == "producer") ? "prod"
                          : (role_tag == "consumer") ? "cons"
                          : "proc";

    if (ic.uid.empty())
    {
        if (role_tag == "producer")
            ic.uid = pylabhub::uid::generate_producer_uid(ic.name);
        else if (role_tag == "consumer")
            ic.uid = pylabhub::uid::generate_consumer_uid(ic.name);
        else
            ic.uid = pylabhub::uid::generate_processor_uid(ic.name);

        std::fprintf(stderr,
                     "[%s] No '%.*s.uid' in config — generated: %s\n"
                     "  Add this to %.*s.json to make the UID stable.\n",
                     short_tag,
                     static_cast<int>(role_tag.size()), role_tag.data(),
                     ic.uid.c_str(),
                     static_cast<int>(role_tag.size()), role_tag.data());
    }
    else
    {
        // Validate prefix.
        bool prefix_ok = false;
        if (role_tag == "producer")
            prefix_ok = pylabhub::uid::has_producer_prefix(ic.uid);
        else if (role_tag == "consumer")
            prefix_ok = pylabhub::uid::has_consumer_prefix(ic.uid);
        else
            prefix_ok = pylabhub::uid::has_processor_prefix(ic.uid);

        if (!prefix_ok)
        {
            const char *expected = (role_tag == "producer") ? "PROD-"
                                 : (role_tag == "consumer") ? "CONS-"
                                 : "PROC-";
            std::fprintf(stderr,
                         "[%s] Warning: '%.*s.uid' = '%s' does not start with '%s'.\n",
                         short_tag,
                         static_cast<int>(role_tag.size()), role_tag.data(),
                         ic.uid.c_str(), expected);
        }
    }

    return ic;
}

} // namespace pylabhub::config
