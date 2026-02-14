#pragma once

#include "base_file_sink.hpp"
#include "sink.hpp"

#include <filesystem>
#include <string>

namespace pylabhub::utils
{

/**
 * @class RotatingFileSink
 * @brief A logger sink that writes to a file and rotates it when it exceeds a specified size.
 */
class RotatingFileSink : public Sink, private BaseFileSink
{
  public:
    /**
     * @brief Constructs a new RotatingFileSink.
     * @param base_filepath The path to the primary log file (e.g., "app.log").
     * @param max_file_size_bytes The maximum size in bytes the log file can reach before rotation.
     * @param max_backup_files The maximum number of backup files to keep (e.g., "app.1.log",
     * "app.2.log").
     * @param use_flock If true, enables inter-process locking on POSIX systems.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- API order: size then count
    RotatingFileSink(const std::filesystem::path &base_filepath, size_t max_file_size_bytes,
                     size_t max_backup_files, bool use_flock);

    ~RotatingFileSink() override = default;

    // --- Sink Interface ---
    void write(const LogMessage &msg, Sink::WRITE_MODE mode) override;
    void flush() override;
    std::string description() const override;

  private:
    void rotate();

    size_t m_max_file_size_bytes;
    size_t m_max_backup_files;
    size_t m_current_size_bytes;
};

} // namespace pylabhub::utils
