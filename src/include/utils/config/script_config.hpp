#pragma once
/**
 * @file script_config.hpp
 * @brief ScriptConfig — categorical config for script engine selection and paths.
 *
 * Parsed from the "script" JSON section + top-level "python_venv" and
 * "stop_on_script_error". Single source of truth for script.type validation.
 */

#include "utils/json_fwd.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct ScriptConfig
{
    std::string type{"python"};          ///< "python", "lua", or "native" (future)
    std::string path{"."};               ///< Parent dir of the script/<type>/ package
    std::string python_venv;             ///< venv name (Python only), empty = base env
    bool        type_explicit{false};    ///< true when "type" was present in JSON
    bool        stop_on_script_error{false}; ///< Fatal on script exception in callback.
};

/// Parse the "script" section from a JSON config object.
/// Validates type ∈ {"python", "lua"}.
/// Also reads top-level "python_venv".
/// @param j      Root JSON object.
/// @param base   Role directory base path (for resolving relative script.path).
///               Pass empty path to skip resolution (from_json_file mode).
/// @param tag    Context tag for error messages (e.g., "Producer config").
inline ScriptConfig parse_script_config(const nlohmann::json &j,
                                         const std::filesystem::path &base,
                                         const char *tag)
{
    namespace fs = std::filesystem;
    ScriptConfig sc;

    if (j.contains("script") && j["script"].is_object())
    {
        const auto &s = j["script"];
        sc.type_explicit = s.contains("type");
        sc.type = s.value("type", std::string{"python"});
        if (sc.type != "python" && sc.type != "lua")
            throw std::invalid_argument(
                std::string(tag) + ": script.type must be \"python\" or \"lua\", got: \""
                + sc.type + "\"");
        sc.path = s.value("path", std::string{"."});
    }

    sc.python_venv = j.value("python_venv", std::string{});
    sc.stop_on_script_error = j.value("stop_on_script_error", false);

    // Resolve relative script.path against the role directory base.
    if (!base.empty() && !sc.path.empty() && !fs::path(sc.path).is_absolute())
        sc.path = fs::weakly_canonical(base / sc.path).string();

    return sc;
}

} // namespace pylabhub::config
