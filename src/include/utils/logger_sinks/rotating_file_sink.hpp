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
 *
 * Two rotation modes are supported:
 *   - Numeric (default, backwards-compatible): the active file is
 *     `base_filepath`; on rotation, existing backups are shifted
 *     `base.1 → base.2 → ...` and the active file is renamed to `base.1`.
 *   - Timestamped: `base_filepath` is a prefix; the active file is
 *     `<base_filepath>-<YYYY-MM-DD-HH-MM-SS>.log`. On rotation, the current
 *     file is closed (its name already encodes its creation time) and a new
 *     timestamped file is opened. The directory is scanned for files
 *     matching `<basename>-*.log` and the oldest are deleted if the count
 *     exceeds @c max_backup_files.
 */
class RotatingFileSink : public Sink, private BaseFileSink
{
  public:
    /// Rotation strategy.
    enum class Mode { Numeric, Timestamped };

    /**
     * @brief Constructs a new RotatingFileSink (Numeric mode — legacy).
     * @param base_filepath The path to the primary log file (e.g., "app.log").
     * @param max_file_size_bytes The maximum size in bytes before rotation.
     * @param max_backup_files The max backup files to keep ("app.1.log", ...).
     * @param use_flock If true, enables inter-process locking on POSIX.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- API order: size then count
    RotatingFileSink(const std::filesystem::path &base_filepath, size_t max_file_size_bytes,
                     size_t max_backup_files, bool use_flock);

    /**
     * @brief Constructs a new RotatingFileSink with explicit rotation mode.
     *
     * In Timestamped mode, @p base_filepath is used as a prefix; the active
     * file is `<base_filepath>-<timestamp>.log`.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    RotatingFileSink(const std::filesystem::path &base_filepath, size_t max_file_size_bytes,
                     size_t max_backup_files, bool use_flock, Mode mode);

    ~RotatingFileSink() override = default;

    // --- Sink Interface ---
    void write(const LogMessage &msg, bool sync_flag) override;
    void flush() override;
    std::string description() const override;

  private:
    void rotate();
    void rotate_numeric_();
    void rotate_timestamped_();

    /// Compose `<base_filepath>-<now>.log` (timestamped active filename).
    std::filesystem::path compose_timestamped_path_() const;

    /// Scan directory for `<basename>-*.log`, delete oldest past max_backups.
    void prune_timestamped_backups_();

    std::filesystem::path m_base_filepath;  ///< Template path (constructor arg).
    size_t m_max_file_size_bytes;
    size_t m_max_backup_files;
    size_t m_current_size_bytes;
    Mode   m_mode{Mode::Numeric};
};

} // namespace pylabhub::utils
