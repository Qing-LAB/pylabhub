#pragma once
// FileLock.hpp - cross-platform file lock RAII wrapper
// Place at: include/fileutil/FileLock.hpp
//
// Usage:
//   FileLock lock(path, LockMode::NonBlocking);
//   if (!lock.valid()) { handle error: lock.error_code() }
//
// Behavior:
//  - On POSIX uses flock(fd, LOCK_EX[|LOCK_NB]) on an on-disk lock file
//  - On Windows uses CreateFileW + LockFileEx with long-path support
//  - The lock file is <parent-of-path>/<basename>.lock
//  - Moveable but non-copyable

#include <filesystem>
#include <system_error>

namespace pylabhub::fileutil
{

enum class LockMode
{
    Blocking,
    NonBlocking
};

class FileLock
{
  public:
    /// Construct and attempt to acquire lock immediately.
    /// If mode == NonBlocking it will fail fast if lock is busy.
    explicit FileLock(const std::filesystem::path &path, LockMode mode = LockMode::Blocking);

    // Movable
    FileLock(FileLock &&other) noexcept;
    FileLock &operator=(FileLock &&other) noexcept;

    // Non-copyable
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    ~FileLock();

    /// Return whether lock was successfully acquired and is held.
    bool valid() const noexcept;

    /// If valid() == false, error_code() holds the last error (may be default constructed if not
    /// relevant).
    std::error_code error_code() const noexcept;

  private:
    void open_and_lock(LockMode mode);

    std::filesystem::path _path;
    bool _valid = false;
    std::error_code _ec;

#if defined(_WIN32)
    void *_handle = nullptr; // stored Windows handle (HANDLE)
#else
    int _fd = -1;
#endif
};

} // namespace pylabhub::fileutil
