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
    std::string type{"python"};          ///< "python", "lua", or "native"
    std::string path{"."};               ///< Parent dir of the script/<type>/ package
    std::string python_venv;             ///< venv name (Python only), empty = base env
    std::string checksum;                ///< BLAKE2b-256 hex of native .so (native only)
    bool        type_explicit{false};    ///< true when "type" was present in JSON
    bool        stop_on_script_error{false}; ///< Fatal on script exception in callback.
};

/// Platform-specific shared library extension.
inline const char *native_lib_extension() noexcept
{
#if defined(_WIN32) || defined(_WIN64)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

/// Resolve a native plugin library path from the config-specified path.
///
/// Follows the same convention as Python/Lua: script/<type>/<entry>.
/// For native, this is: <script.path>/script/native/<filename>
///
/// Search order:
/// 1. Exact path (if file exists)
/// 2. path + platform extension (if no extension given)
/// 3. script_dir/script/native/<filename> (structured, parallel to python/lua)
/// 4. script_dir/script/native/<filename> + platform extension
///
/// @param configured_path  The "script.path" value from config (resolved).
/// @param filename         Library filename (e.g., "libmy_producer" or "libmy_producer.so").
/// @return Resolved absolute path, or empty if not found.
inline std::filesystem::path resolve_native_library(
    const std::string &configured_path,
    const std::string &filename)
{
    namespace fs = std::filesystem;
    const auto ext = native_lib_extension();

    // 1. Exact path (filename is a full path).
    fs::path p(filename);
    if (p.is_absolute() && fs::exists(p) && fs::is_regular_file(p))
        return fs::weakly_canonical(p);

    // 2. Structured: <script.path>/script/native/<filename>
    fs::path script_dir(configured_path);
    fs::path structured = script_dir / "script" / "native" / filename;
    if (fs::exists(structured) && fs::is_regular_file(structured))
        return fs::weakly_canonical(structured);

    // 3. Append platform extension.
    if (structured.extension().empty())
    {
        fs::path with_ext = structured;
        with_ext += ext;
        if (fs::exists(with_ext) && fs::is_regular_file(with_ext))
            return fs::weakly_canonical(with_ext);
    }

    // 4. Direct relative to script.path.
    fs::path direct = script_dir / filename;
    if (fs::exists(direct) && fs::is_regular_file(direct))
        return fs::weakly_canonical(direct);

    if (direct.extension().empty())
    {
        fs::path with_ext = direct;
        with_ext += ext;
        if (fs::exists(with_ext) && fs::is_regular_file(with_ext))
            return fs::weakly_canonical(with_ext);
    }

    return {}; // not found
}

/// Parse the "script" section from a JSON config object.
/// Validates type ∈ {"python", "lua", "native"}.
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
        if (sc.type != "python" && sc.type != "lua" && sc.type != "native")
            throw std::invalid_argument(
                std::string(tag) + ": script.type must be \"python\", \"lua\", or \"native\", got: \""
                + sc.type + "\"");
        sc.path = s.value("path", std::string{"."});
        sc.checksum = s.value("checksum", std::string{});
    }

    sc.python_venv = j.value("python_venv", std::string{});
    sc.stop_on_script_error = j.value("stop_on_script_error", false);

    // Resolve relative script.path against the role directory base.
    if (!base.empty() && !sc.path.empty() && !fs::path(sc.path).is_absolute())
        sc.path = fs::weakly_canonical(base / sc.path).string();

    return sc;
}

} // namespace pylabhub::config
