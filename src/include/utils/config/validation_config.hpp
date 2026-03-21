#pragma once
/**
 * @file validation_config.hpp
 * @brief ValidationConfig — categorical config for checksum and error handling.
 */

#include "utils/json_fwd.hpp"

namespace pylabhub::config
{

struct ValidationConfig
{
    bool update_checksum{true};        ///< Update BLAKE2b checksum on commit (writer-side).
    bool verify_checksum{false};       ///< Verify checksum on read (reader-side).
    bool stop_on_script_error{false};  ///< Fatal on script exception in callback.
};

/// Parse "validation" section from a JSON config object.
inline ValidationConfig parse_validation_config(const nlohmann::json &j)
{
    ValidationConfig vc;
    if (j.contains("validation") && j["validation"].is_object())
    {
        const auto &val = j["validation"];
        vc.update_checksum      = val.value("update_checksum",      true);
        vc.verify_checksum      = val.value("verify_checksum",      false);
        vc.stop_on_script_error = val.value("stop_on_script_error", false);
    }
    return vc;
}

} // namespace pylabhub::config
