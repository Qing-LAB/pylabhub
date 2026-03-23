#pragma once
/**
 * @file validation_config.hpp
 * @brief Validation config — checksum and error handling.
 *
 * Two levels:
 * - DirectionalValidationConfig: per-direction (in/out) checksum settings.
 *   JSON fields: <direction>_update_checksum, <direction>_verify_checksum.
 * - ValidationConfig: non-directional script error handling (stop_on_script_error).
 */

#include "utils/json_fwd.hpp"

#include <string>

namespace pylabhub::config
{

/// Per-direction checksum settings (new directional format).
/// JSON fields: <direction>_update_checksum, <direction>_verify_checksum.
struct DirectionalValidationConfig
{
    bool update_checksum{true};   ///< Update BLAKE2b checksum on commit (writer-side).
    bool verify_checksum{false};  ///< Verify checksum on read (reader-side).
};

/// Parse directional validation config.
/// @param j         Root JSON object.
/// @param direction "in" or "out".
inline DirectionalValidationConfig parse_directional_validation(
    const nlohmann::json &j, const char *direction)
{
    const std::string pfx = std::string(direction) + "_";
    DirectionalValidationConfig dv;
    dv.update_checksum = j.value(pfx + "update_checksum", true);
    dv.verify_checksum = j.value(pfx + "verify_checksum", false);
    return dv;
}

/// Non-directional validation config (script error handling only).
/// Checksum settings are directional — see DirectionalValidationConfig.
struct ValidationConfig
{
    bool stop_on_script_error{false};  ///< Fatal on script exception in callback.
};

/// Parse non-directional validation config.
inline ValidationConfig parse_validation_config(const nlohmann::json &j)
{
    ValidationConfig vc;
    vc.stop_on_script_error = j.value("stop_on_script_error", false);
    return vc;
}

} // namespace pylabhub::config
