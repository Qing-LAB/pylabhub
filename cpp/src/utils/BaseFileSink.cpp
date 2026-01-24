#include "plh_base.hpp"

#include <stdexcept>
#include <system_error>

#ifdef PYLABHUB_PLATFORM_WIN64
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "BaseFileSink.hpp"

namespace pylabhub::utils
{

BaseFileSink::BaseFileSink() = default;

BaseFileSink::~BaseFileSink()
{
    close();
}

void BaseFileSink::open(const std::filesystem::path &path, bool use_flock)
{
    close(); // Ensure any previous handle is closed.
    m_path = path;
    m_use_flock = use_flock;

#ifdef PYLABHUB_PLATFORM_WIN64
    (void)m_use_flock; // Prevent unused parameter warning on Windows
    std::wstring wpath = pylabhub::format_tools::win32_to_long_path(m_path);
    m_file_handle = CreateFileW(wpath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_file_handle == INVALID_HANDLE_VALUE)
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "CreateFileW failed for log file");
    }
#else
    m_fd = ::open(m_path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (m_fd == -1)
    {
        throw std::system_error(errno, std::generic_category(), "open failed for log file");
    }
#endif
}

void BaseFileSink::close()
{
#ifdef PYLABHUB_PLATFORM_WIN64
    if (m_file_handle != nullptr && m_file_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_file_handle);
        m_file_handle = nullptr;
    }
#else
    if (m_fd != -1)
    {
        ::close(m_fd);
        m_fd = -1;
    }
#endif
    m_path.clear();
}

void BaseFileSink::fwrite(const std::string &content)
{
    if (!is_open())
        return;

#ifdef PYLABHUB_PLATFORM_WIN64
    DWORD bytes_written = 0;
    if (!WriteFile(m_file_handle, content.c_str(), static_cast<DWORD>(content.length()),
                   &bytes_written, nullptr) ||
        bytes_written != content.length())
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Failed to write complete log message to file");
    }
#else
    if (m_use_flock)
    {
        // flock is advisory and may be ignored, but provides good-faith serialization.
        ::flock(m_fd, LOCK_EX);
    }

    ssize_t bytes_written = ::write(m_fd, content.c_str(), content.length());

    if (m_use_flock)
    {
        ::flock(m_fd, LOCK_UN);
    }

    if (bytes_written < 0 || static_cast<size_t>(bytes_written) != content.length())
    {
        throw std::system_error(errno, std::generic_category(),
                                "Failed to write complete log message to file");
    }
#endif
}

void BaseFileSink::fflush()
{
    if (!is_open())
        return;

#ifdef PYLABHUB_PLATFORM_WIN64
    FlushFileBuffers(m_file_handle);
#else
    ::fsync(m_fd);
#endif
}

size_t BaseFileSink::size() const
{
    if (!is_open())
        return 0;

    try
    {
        // This might be slow, but it is accurate.
        return std::filesystem::file_size(m_path);
    }
    catch (const std::filesystem::filesystem_error &)
    {
        return 0;
    }
}

bool BaseFileSink::is_open() const
{
#ifdef PYLABHUB_PLATFORM_WIN64
    return m_file_handle != nullptr && m_file_handle != INVALID_HANDLE_VALUE;
#else
    return m_fd != -1;
#endif
}

} // namespace pylabhub::utils
