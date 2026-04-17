// File: rotating_file_sink.cpp
#include "utils/logger_sinks/rotating_file_sink.hpp"

#include "utils/format_tools.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

namespace pylabhub::utils
{

RotatingFileSink::RotatingFileSink(const std::filesystem::path &base_filepath,
                                   size_t max_file_size_bytes, size_t max_backup_files,  // NOLINT(bugprone-easily-swappable-parameters) order: size then count
                                   bool use_flock)
    : RotatingFileSink(base_filepath, max_file_size_bytes, max_backup_files,
                       use_flock, Mode::Numeric)
{
}

RotatingFileSink::RotatingFileSink(const std::filesystem::path &base_filepath,
                                   size_t max_file_size_bytes, size_t max_backup_files,  // NOLINT(bugprone-easily-swappable-parameters)
                                   bool use_flock, Mode mode)
    : m_base_filepath(base_filepath),
      m_max_file_size_bytes(max_file_size_bytes > 0 ? max_file_size_bytes : 1),
      m_max_backup_files(max_backup_files), m_current_size_bytes(0),
      m_mode(mode)
{
    const std::filesystem::path initial_path =
        (m_mode == Mode::Timestamped) ? compose_timestamped_path_() : base_filepath;

    try
    {
        open(initial_path, use_flock);
        m_current_size_bytes = size();

        // In Timestamped mode, also prune pre-existing log files in the
        // directory so the backup count is enforced at startup too.
        if (m_mode == Mode::Timestamped)
            prune_timestamped_backups_();
    }
    catch (const std::system_error &e)
    {
        throw std::runtime_error(fmt::format("Failed to open rotating log file '{}': {}",
                                             initial_path.string(), e.what()));
    }
}

std::filesystem::path RotatingFileSink::compose_timestamped_path_() const
{
    // Filename format: <stem>-YYYY-MM-DD-HH-MM-SS.uuuuuu.log
    //
    // Always include microseconds so filenames sort lexicographically in
    // chronological order — no mtime dependency (which other file ops can
    // contaminate), no collision-fallback branching. The fractional part
    // uses '.' as the separator which preserves lex ordering against the
    // trailing ".log" extension (digits < 'l' by ASCII).
    std::string stem = m_base_filepath.stem().string();
    if (stem.empty())
        stem = m_base_filepath.filename().string();

    const std::string ts = pylabhub::format_tools::formatted_time(
        std::chrono::system_clock::now(), /*use_dash_spacer=*/true);
    // ts shape: "YYYY-MM-DD-HH-MM-SS.uuuuuu" (26 chars).

    return m_base_filepath.parent_path() / (stem + "-" + ts + ".log");
}

void RotatingFileSink::prune_timestamped_backups_()
{
    namespace fs = std::filesystem;

    if (m_max_backup_files == 0)
        return;  // no limit → keep all files

    const fs::path dir = m_base_filepath.parent_path();
    std::string stem = m_base_filepath.stem().string();
    if (stem.empty())
        stem = m_base_filepath.filename().string();
    const std::string prefix = stem + "-";

    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;

    // Collect all <stem>-*.log files in the directory.
    std::vector<fs::path> candidates;
    for (const auto &entry : fs::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 &&                     // starts with <stem>-
            name.size() > prefix.size() + 4 &&                 // has room for timestamp + ".log"
            name.substr(name.size() - 4) == ".log")            // ends with .log
        {
            candidates.push_back(entry.path());
        }
    }

    // Sort by filename — names always include microseconds, so lex order
    // equals chronological order. Filename is the authoritative creation
    // record; mtime could be contaminated by unrelated file operations.
    std::sort(candidates.begin(), candidates.end());

    // Keep newest max_backup_files + 1 (the +1 accounts for the currently
    // active file, which is in `candidates` too). Delete the rest.
    const size_t keep = m_max_backup_files + 1;
    if (candidates.size() <= keep)
        return;

    const size_t to_delete = candidates.size() - keep;
    for (size_t i = 0; i < to_delete; ++i)
    {
        fs::remove(candidates[i], ec);
        if (ec)
        {
            PLH_DEBUG("Logger Error: Failed to delete old timestamped log '{}': {}",
                      candidates[i].string(), ec.message());
        }
    }
}

void RotatingFileSink::write(const LogMessage &msg, bool sync_flag)
{
    if (!is_open())
    {
        return;
    }

    // Check if rotation is needed *before* writing the new message.
    if (m_current_size_bytes >= m_max_file_size_bytes)
    {
        rotate();
        // After rotation, we might not have a valid file handle if opening the new file failed.
        if (!is_open())
        {
            return;
        }
    }

    auto formatted_message = pylabhub::utils::Sink::format_logmsg(msg, sync_flag);
    BaseFileSink::fwrite(formatted_message);
    m_current_size_bytes += formatted_message.length();
}

void RotatingFileSink::flush()
{
    BaseFileSink::fflush();
}

std::string RotatingFileSink::description() const
{
    const char *mode_str = (m_mode == Mode::Timestamped) ? "timestamped" : "numeric";
    return fmt::format("RotatingFile: base='{}' active='{}' max_size={} max_files={} mode={}",
                       m_base_filepath.string(), path().string(),
                       m_max_file_size_bytes, m_max_backup_files, mode_str);
}

void RotatingFileSink::rotate()
{
    if (m_mode == Mode::Timestamped)
        rotate_timestamped_();
    else
        rotate_numeric_();
}

void RotatingFileSink::rotate_timestamped_()
{
    close();

    // Open a fresh file with a new timestamp. The old file keeps its own
    // timestamped name (no rename needed).
    const std::filesystem::path new_path = compose_timestamped_path_();

    try
    {
        open(new_path, m_use_flock);
        m_current_size_bytes = 0;

        constexpr int kSystemLevel = 5; // L_SYSTEM
        auto formatted_message = pylabhub::utils::Sink::format_logmsg(
            LogMessage{
                .timestamp  = std::chrono::system_clock::now(),
                .process_id = pylabhub::platform::get_pid(),
                .thread_id  = pylabhub::platform::get_native_thread_id(),
                .level      = kSystemLevel,
                .body       = pylabhub::format_tools::make_buffer(
                    "--- Log rotated (timestamped) ---"),
            },
            /*sync_flag=*/false);
        BaseFileSink::fwrite(formatted_message);
        BaseFileSink::fflush();
        m_current_size_bytes += formatted_message.length();

        // Prune old timestamped files to enforce max_backup_files.
        prune_timestamped_backups_();
    }
    catch (const std::system_error &e)
    {
        PLH_DEBUG("Logger Error: Failed to open new timestamped log '{}': {}",
                  new_path.string(), e.what());
        m_current_size_bytes = 0;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- rotation steps and recovery branches
void RotatingFileSink::rotate_numeric_()
{
    auto base_path = path();
    close();

    std::error_code err_code;

    // 1. Remove the oldest backup file, if it exists.
    std::filesystem::path oldest_backup(base_path.string() + "." +
                                        std::to_string(m_max_backup_files));
    if (m_max_backup_files > 0 && std::filesystem::exists(oldest_backup))
    {
        std::filesystem::remove(oldest_backup, err_code);
        if (err_code)
        {
            PLH_DEBUG("Logger Error: Failed to remove oldest backup '{}': {}",
                      oldest_backup.string(), err_code.message());
            // Recovery attempt: Try to re-open the original file to continue logging
            // without losing further messages, even though rotation failed.
            try
            {
                open(base_path, m_use_flock);
                // Reset size so we don't immediately retry rotation on the next write,
                // which would create a tight failure loop.
                m_current_size_bytes = 0;
            }
            catch (const std::system_error &e)
            {
                PLH_DEBUG("Logger Error: Failed to recover by re-opening log file '{}': {}",
                          base_path.string(), e.what());
            }
            return;
        }
    }

    // 2. Shift existing backup files: log.2 -> log.3, log.1 -> log.2
    // Guard against unsigned underflow when m_max_backup_files == 0 (wraps to SIZE_MAX).
    for (size_t i = (m_max_backup_files > 1 ? m_max_backup_files - 1 : 0); i > 0; --i)
    {
        std::filesystem::path current_backup(base_path.string() + "." + std::to_string(i));
        if (std::filesystem::exists(current_backup))
        {
            std::filesystem::path next_backup(base_path.string() + "." + std::to_string(i + 1));
            std::filesystem::rename(current_backup, next_backup, err_code);
            if (err_code)
            {
                PLH_DEBUG("Logger Error: Failed to rename backup '{}' to '{}': {}",
                          current_backup.string(), next_backup.string(), err_code.message());
                // Recovery attempt: Try to re-open the original file.
                try
                {
                    open(base_path, m_use_flock);
                    m_current_size_bytes = 0; // Prevent tight retry loop on next write.
                }
                catch (const std::system_error &e)
                {
                    PLH_DEBUG("Logger Error: Failed to recover by re-opening log file '{}': {}",
                              base_path.string(), e.what());
                }
                return;
            }
        }
    }

    // 3. Rename the current log file to the first backup: log.txt -> log.txt.1
    if (m_max_backup_files > 0 && std::filesystem::exists(base_path))
    {
        std::filesystem::path first_backup(base_path.string() + ".1");
        std::filesystem::rename(base_path, first_backup, err_code);
        if (err_code)
        {
            PLH_DEBUG("Logger Error: Failed to rename base log file to '{}': {}",
                      first_backup.string(), err_code.message());
            // Recovery attempt: Try to re-open the original file.
            try
            {
                open(base_path, m_use_flock);
                m_current_size_bytes = 0; // Prevent tight retry loop on next write.
            }
            catch (const std::system_error &e)
            {
                PLH_DEBUG("Logger Error: Failed to recover by re-opening log file '{}': {}",
                          base_path.string(), e.what());
            }
            return;
        }
    }
    else if (std::filesystem::exists(base_path))
    {
        std::filesystem::remove(base_path, err_code);
        if (err_code)
        {
            PLH_DEBUG("Logger Error: Failed to remove base log file '{}': {}", base_path.string(),
                      err_code.message());
            // Recovery attempt: Try to re-open the original file.
            try
            {
                open(base_path, m_use_flock);
                m_current_size_bytes = 0; // Prevent tight retry loop on next write.
            }
            catch (const std::system_error &e)
            {
                PLH_DEBUG("Logger Error: Failed to recover by re-opening log file '{}': {}",
                          base_path.string(), e.what());
            }
            return;
        }
    }

    // 4. Open a new primary log file.
    try
    {
        open(base_path, m_use_flock);
        m_current_size_bytes = 0; // Reset size for the new file.
        constexpr int kSystemLevel = 5; // L_SYSTEM
        auto formatted_message = pylabhub::utils::Sink::format_logmsg(
            LogMessage{
                .timestamp = std::chrono::system_clock::now(),
                .process_id = pylabhub::platform::get_pid(),
                .thread_id = pylabhub::platform::get_native_thread_id(),
                .level = kSystemLevel,
                .body = pylabhub::format_tools::make_buffer("--- Log rotated successfully ---"),
            },
            /*sync_flag=*/false);
        BaseFileSink::fwrite(formatted_message);
        BaseFileSink::fflush();
        m_current_size_bytes += formatted_message.length();
    }
    catch (const std::system_error &e)
    {
        PLH_DEBUG("Logger Error: Failed to open or write to a new log file '{}' after rotation: {}",
                  base_path.string(), e.what());
        m_current_size_bytes = 0;
    }
}

} // namespace pylabhub::utils
