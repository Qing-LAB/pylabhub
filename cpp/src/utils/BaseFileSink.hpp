#pragma once

#include "platform.hpp"
#include <filesystem>
#include <string>

namespace pylabhub::utils
{

/**
 * @class BaseFileSink
 * @brief An internal helper class that encapsulates common, cross-platform
 *        file I/O operations for logger sinks.
 *
 * This class is not a Sink itself. It handles the opening, closing, writing,
 * flushing, and size querying of a single file, providing a common implementation
 * for both FileSink and RotatingFileSink.
 */
class BaseFileSink
{
  public:
    BaseFileSink();
    virtual ~BaseFileSink();

    // This class manages a raw file handle, so it is non-copyable and non-movable.
    BaseFileSink(const BaseFileSink &) = delete;
    BaseFileSink &operator=(const BaseFileSink &) = delete;
    BaseFileSink(BaseFileSink &&) = delete;
    BaseFileSink &operator=(BaseFileSink &&) = delete;

  protected:
    // Throws std::system_error on failure.
    void open(const std::filesystem::path &path, bool use_flock);
    void close();
    void write(const std::string &content);
    void flush();
    size_t size() const;
    bool is_open() const;
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
