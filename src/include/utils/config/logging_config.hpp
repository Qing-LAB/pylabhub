#pragma once
/**
 * @file logging_config.hpp
 * @brief LoggingConfig — per-role logging output and rotation policy.
 *
 * Parsed from the role JSON config's "logging" object:
 *
 *   {
 *     "logging": {
 *       "file_path":    "logs/my.log",   // optional: override default path
 *       "max_size_mb":  10,              // default 10 MiB
 *       "backups":      5,               // default 5 files
 *       "timestamped":  true             // default true (plh_role convention)
 *     }
 *   }
 *
 * All fields are optional; omitted ones take the defaults below. When the
 * "logging" object itself is absent, defaults apply.
 *
 * When file_path is empty, callers (binary main) resolve the default as
 * "<role_dir>/logs/<uid>.log" — the library does not compose paths.
 */

#include "utils/json_fwd.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct LoggingConfig
{
    /// Sentinel meaning "no deletion on rotation — retain all files".
    /// JSON config expresses this as `"backups": -1`.
    static constexpr size_t kKeepAllBackups = std::numeric_limits<size_t>::max();

    /// Log file path (absolute, or relative to <role_dir>).
    /// Empty → caller resolves default as "<role_dir>/logs/<uid>.log".
    std::string file_path;

    /// Max bytes per file before rotation. Default 10 MiB. Must be > 0.
    size_t max_size_bytes = 10ULL * 1024 * 1024;

    /// Number of backup files retained after rotation. Default 5.
    /// JSON: positive integer for that count, `-1` to keep all files
    /// (stored internally as kKeepAllBackups). `0` is invalid.
    size_t max_backup_files = 5;

    /// If true, active file is "<file_path>-<timestamp>.log" and rotation
    /// opens a new timestamped file (no renaming of old files).
    /// Default true — matches plh_role convention.
    bool timestamped = true;
};

/// Parse the "logging" object from a role config JSON.
/// Absent "logging" → returns defaults. Unknown keys inside "logging" throw.
inline LoggingConfig parse_logging_config(const nlohmann::json &j, const char *tag)
{
    LoggingConfig lc;

    if (!config_has(j, "logging"))
        return lc;

    const auto &lj = j["logging"];
    if (!lj.is_object())
    {
        throw std::runtime_error(
            std::string(tag) + ": 'logging' must be a JSON object");
    }

    // Validate keys within the "logging" subobject.
    for (auto it = lj.begin(); it != lj.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "file_path" && k != "max_size_mb" && k != "backups" &&
            k != "timestamped")
        {
            throw std::runtime_error(
                std::string(tag) + ": unknown config key 'logging." + k + "'");
        }
    }

    if (config_has(lj, "file_path"))
        lc.file_path = lj["file_path"].get<std::string>();

    if (config_has(lj, "max_size_mb"))
    {
        const auto mb = lj["max_size_mb"].get<double>();
        if (mb <= 0)
        {
            throw std::runtime_error(
                std::string(tag) + ": 'logging.max_size_mb' must be > 0");
        }
        lc.max_size_bytes = static_cast<size_t>(mb * 1024.0 * 1024.0);
    }

    if (config_has(lj, "backups"))
    {
        const auto n = lj["backups"].get<int64_t>();
        if (n == -1)
        {
            // Sentinel: keep all files, never delete.
            lc.max_backup_files = LoggingConfig::kKeepAllBackups;
        }
        else if (n >= 1)
        {
            lc.max_backup_files = static_cast<size_t>(n);
        }
        else
        {
            throw std::runtime_error(
                std::string(tag) + ": 'logging.backups' must be >= 1 "
                "(or -1 to keep all files); '0' is invalid");
        }
    }

    if (config_has(lj, "timestamped"))
        lc.timestamped = lj["timestamped"].get<bool>();

    return lc;
}

} // namespace pylabhub::config
