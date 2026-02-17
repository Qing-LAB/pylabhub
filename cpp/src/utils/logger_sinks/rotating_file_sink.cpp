// File: rotating_file_sink.cpp
#include "plh_base.hpp"

#include "utils/logger_sinks/rotating_file_sink.hpp"

#include <chrono>
#include <filesystem>

namespace pylabhub::utils
{

RotatingFileSink::RotatingFileSink(const std::filesystem::path &base_filepath,
                                   size_t max_file_size_bytes, size_t max_backup_files,  // NOLINT(bugprone-easily-swappable-parameters) order: size then count
                                   bool use_flock)
    : m_max_file_size_bytes(max_file_size_bytes > 0 ? max_file_size_bytes : 1),
      m_max_backup_files(max_backup_files), m_current_size_bytes(0)
{
    try
    {
        open(base_filepath, use_flock);
        m_current_size_bytes = size();
    }
    catch (const std::system_error &e)
    {
        throw std::runtime_error(fmt::format("Failed to open rotating log file '{}': {}",
                                             base_filepath.string(), e.what()));
    }
}

void RotatingFileSink::write(const LogMessage &msg, Sink::WRITE_MODE mode)
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

    auto formatted_message = pylabhub::utils::Sink::format_logmsg(msg, mode);
    BaseFileSink::fwrite(formatted_message);
    m_current_size_bytes += formatted_message.length();
}

void RotatingFileSink::flush()
{
    BaseFileSink::fflush();
}

std::string RotatingFileSink::description() const
{
    return fmt::format("RotatingFile: path='{}', max_size={}, max_files={}", path().string(),
                       m_max_file_size_bytes, m_max_backup_files);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- rotation steps and recovery branches
void RotatingFileSink::rotate()
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
    for (size_t i = m_max_backup_files - 1; i > 0; --i)
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
            Sink::ASYNC_WRITE);
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
