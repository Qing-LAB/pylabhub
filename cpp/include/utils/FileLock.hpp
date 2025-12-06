/*******************************************************************************
  * @file include/utils/FileLock.hpp
  * @brief Cross-platform file lock RAII wrapper
  * @author Quan Qing
  * @date 2025-11-15
  * Reviewed and revised by Quan Qing on 2025-11-15
  * First version created by Quan Qing with ChatGPT assistance.
  * Unit test available at tests/test_filelock.cpp
 ******************************************************************************/
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

#include "platform.hpp" // For PYLABHUB_API

namespace pylabhub::utils
{

enum class LockMode
{
    Blocking,
    NonBlocking
};


// Forward-declare the implementation struct for the Pimpl idiom.
struct FileLockImpl;

class PYLABHUB_API FileLock
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

    // The destructor must be defined in the .cpp file where FileLockImpl is a
    // complete type, which is required by std::unique_ptr<FileLockImpl>.
    ~FileLock();

    /// Return whether lock was successfully acquired and is held.
    bool valid() const noexcept;

    /// If valid() == false, error_code() holds the last error (may be default constructed if not
    /// relevant).
    std::error_code error_code() const noexcept;

  private:
    // The only data member is a pointer to the implementation.
    // This provides a stable ABI.
    std::unique_ptr<FileLockImpl> pImpl;
};

} // namespace pylabhub::utils
