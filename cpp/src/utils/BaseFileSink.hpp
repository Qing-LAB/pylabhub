#pragma once

namespace pylabhub::utils
{

/**
 * @class BaseFileSink
 * @brief An internal helper class that encapsulates common, cross-platform
 *        file I/O operations for logger sinks.
 *
 * This class is not a Sink itself. It handles the opening, closing, writing,
 * flushing, and size querying of a single file, providing a common implementation
 * for both FileSink and RotatingFileSink. It ensures that file operations are
 * abstracted away from the specific sink logic.
 */
class BaseFileSink
{
  public:
    /** @brief Default constructor. */
    BaseFileSink();
    /** @brief Destructor. Ensures the file handle is closed. */
    virtual ~BaseFileSink();

    // This class manages a raw file handle, so it is non-copyable and non-movable.
    BaseFileSink(const BaseFileSink &) = delete;
    BaseFileSink &operator=(const BaseFileSink &) = delete;
    BaseFileSink(BaseFileSink &&) = delete;
    BaseFileSink &operator=(BaseFileSink &&) = delete;

  protected:
    /**
     * @brief Opens a file at the given path for appending.
     * @param path The path to the file.
     * @param use_flock If true, enables advisory file locking on POSIX systems.
     * @throws std::system_error on failure to open the file.
     */
    void open(const std::filesystem::path &path, bool use_flock);

    /** @brief Closes the currently open file handle. */
    void close();

    /**
     * @brief Writes content to the open file.
     * @param content The string content to write.
     * @throws std::system_error on write failure.
     */
    void fwrite(const std::string &content);

    /** @brief Flushes any buffered data to the disk. */
    void fflush();

    /**
     * @brief Gets the current size of the open file.
     * @return The size of the file in bytes, or 0 if not open or on error.
     */
    size_t size() const;

    /**
     * @brief Checks if a file is currently open.
     * @return `true` if the file handle is valid, `false` otherwise.
     */
    bool is_open() const;

    /**
     * @brief Gets the path of the currently open file.
     * @return A const reference to the filesystem path.
     */
    const std::filesystem::path &path() const { return m_path; }

  protected: // Change private to protected for derived class access
    std::filesystem::path m_path;
    bool m_use_flock = false;

#ifdef PYLABHUB_PLATFORM_WIN64
    void *m_file_handle = nullptr; // Using void* to avoid including <windows.h>
#else
    int m_fd = -1;
#endif
};

} // namespace pylabhub::utils
