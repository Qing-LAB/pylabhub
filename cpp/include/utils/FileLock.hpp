/*******************************************************************************
 * @file include/utils/FileLock.hpp
 * @brief Cross-platform advisory file lock RAII wrapper.
 * @author Quan Qing
 * @date 2025-11-15
 *
 * @see tests/test_filelock.cpp
 ******************************************************************************/
#pragma once

// FileLock.hpp - cross-platform advisory file lock RAII wrapper.
// Location: include/utils/FileLock.hpp
//
// Usage:
//   FileLock lock(path, LockMode::NonBlocking);
//   if (!lock.valid()) { handle error: lock.error_code() }
//
// Behavior:
//  - Implements an *advisory* lock. It relies on all cooperating processes and
//    threads to respect the same locking protocol. It does NOT prevent
//    non-cooperating parties from accessing the target file.
//  - Uses a separate '.lock' file to avoid interfering with the target file.
//    (e.g., for target `/a/b/c.txt`, the lock file is `/a/b/c.txt.lock`).
//  - On POSIX, uses flock().
//  - On Windows, uses LockFileEx().
//  - Provides unified semantics for both inter-process and intra-process
//    (i.e., multi-threaded) locking on all platforms.
//  - Movable but non-copyable.

#include <filesystem>
#include <memory> // For std::unique_ptr
#include <system_error>

#include "platform.hpp" // For PYLABHUB_API

// Disable warning C4251 for Pimpl members, which is a common practice when
// exporting classes that use std::unique_ptr with an incomplete type.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::utils
{

/**
 * @brief Specifies the behavior when trying to acquire a lock that is already held.
 */
enum class LockMode
{
    Blocking,    ///< Wait indefinitely until the lock is acquired.
    NonBlocking, ///< Return immediately if the lock cannot be acquired.
};

// Forward-declare the implementation struct for the Pimpl idiom.
struct FileLockImpl;

/**
 * @class FileLock
 * @brief A cross-platform, RAII-style advisory file lock.
 *
 * This class acquires a lock upon construction and releases it upon destruction.
 * It is designed to be safe for both multi-process and multi-threaded scenarios,
 * providing consistent behavior across platforms.
 */
class PYLABHUB_API FileLock
{
  public:
    /**
     * @brief Constructs a FileLock and attempts to acquire the lock.
     * @param path The path to the file or resource to be locked. The lock will be
     *             placed on a corresponding '.lock' file.
     * @param mode The locking mode (Blocking or NonBlocking).
     */
    explicit FileLock(const std::filesystem::path &path, LockMode mode = LockMode::Blocking);

    /// @brief Move constructor. Transfers ownership of an existing lock.
    FileLock(FileLock &&other) noexcept;

    /// @brief Move assignment operator. Transfers ownership of an existing lock.
    FileLock &operator=(FileLock &&other) noexcept;

    // Non-copyable to prevent accidental duplication of lock ownership.
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    /**
     * @brief Destructor. Releases the lock if it is held.
     *
     * This must be defined in the .cpp file where FileLockImpl is a complete
     * type, as required by std::unique_ptr<FileLockImpl>.
     */
    ~FileLock();

    /**
     * @brief Checks if the lock was successfully acquired and is currently held.
     * @return true if the lock is valid, false otherwise.
     */
    bool valid() const noexcept;

    /**
     * @brief Gets the error code from the last failed lock acquisition attempt.
     * @return std::error_code containing the error information. If valid() is
     *         true, or if no error occurred, the code will be empty.
     */
    std::error_code error_code() const noexcept;

  private:
    // The only data member is a pointer to the implementation.
    // This provides a stable ABI and hides implementation details.
    std::unique_ptr<FileLockImpl> pImpl;
};

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
