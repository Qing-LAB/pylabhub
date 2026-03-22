#pragma once
/**
 * @file validation_config.hpp
 * @brief ValidationConfig — categorical config for checksum and error handling.
 *
 * Two levels:
 * - DirectionalValidationConfig: per-direction (in/out) checksum settings.
 *   New JSON format: <direction>_update_checksum, <direction>_verify_checksum.
 * - ValidationConfig: non-directional script error handling + legacy checksum
 *   fields (for backward compat until old config structs are removed).
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

/// Non-directional validation config.
struct ValidationConfig
{
    bool update_checksum{true};        ///< Legacy — used by old config structs.
    bool verify_checksum{false};       ///< Legacy — used by old config structs.
    bool stop_on_script_error{false};  ///< Fatal on script exception in callback.
};

/// Parse validation config. Reads both legacy format (validation sub-object)
/// and flat top-level fields. Legacy fields will be removed in Phase 5.
inline ValidationConfig parse_validation_config(const nlohmann::json &j)
{
    ValidationConfig vc;

    // Legacy: "validation" sub-object (old config format).
    if (j.contains("validation") && j["validation"].is_object())
    {
        const auto &val = j["validation"];
        vc.update_checksum      = val.value("update_checksum",      true);
        vc.verify_checksum      = val.value("verify_checksum",      false);
        vc.stop_on_script_error = val.value("stop_on_script_error", false);
    }

    // Flat top-level (new format — takes precedence).
    if (j.contains("stop_on_script_error"))
        vc.stop_on_script_error = j.value("stop_on_script_error", false);
    if (j.contains("update_checksum"))
        vc.update_checksum = j.value("update_checksum", true);
    if (j.contains("verify_checksum"))
        vc.verify_checksum = j.value("verify_checksum", false);

    return vc;
}

} // namespace pylabhub::config
