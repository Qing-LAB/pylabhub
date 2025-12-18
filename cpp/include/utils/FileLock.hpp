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
//  - Uses a separate lock file to avoid interfering with the target file. The
//    naming convention is designed to prevent collisions between file and
//    directory locks:
//    - For a file `/path/to/file.txt`, the lock is `/path/to/file.txt.lock`.
//    - For a directory `/path/to/dir/`, the lock is `/path/to/dir.dir.lock`.
//  - On POSIX, uses flock().
//  - On Windows, uses LockFileEx().
//  - Provides unified semantics for both inter-process and intra-process
//    (i.e., multi-threaded) locking on all platforms.
//  - Movable but non-copyable.

#include <filesystem>
#include <memory> // For std::unique_ptr
#include <system_error>

#include "platform.hpp"
#include "pylabhub_utils_export.h"

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

/**
 * @brief Specifies the type of resource being locked to ensure unambiguous
 * lock file naming.
 */
enum class ResourceType
{
    File,      ///< The lock target is a file.
    Directory, ///< The lock target is a directory.
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
 *
 * @warning The underlying POSIX lock mechanism (flock) may be unreliable
 *          over network filesystems like NFS. This class is best suited for
 *          local filesystem synchronization.
 */
class PYLABHUB_UTILS_EXPORT FileLock
{
  public:
    /**
     * @brief Gets the canonical path for the lock file for a given resource.
     *
     * This allows consumers to know the exact path of the lock file that will be used
     * for a given resource without having to instantiate a FileLock.
     *
     * @param path The path to the resource (file or directory).
     * @param type The type of the resource.
     * @return The absolute path to the corresponding lock file.
     */
    static std::filesystem::path get_expected_lock_fullname_for(
        const std::filesystem::path &path,
        ResourceType type) noexcept;

    /**
     * @brief Constructs a FileLock and attempts to acquire the lock.
     * @param path The path to the file or resource to be locked.
     * @param type The type of resource (File or Directory), which determines
     *             the lock file's naming convention.
     * @param mode The locking mode (Blocking or NonBlocking).
     */
    explicit FileLock(const std::filesystem::path &path,
                      ResourceType type,
                      LockMode mode = LockMode::Blocking) noexcept;

    /**
     * @brief Constructs a FileLock and attempts to acquire the lock within a given time.
     * @param path The path to the file or resource to be locked.
     * @param type The type of resource (File or Directory), which determines
     *             the lock file's naming convention.
     * @param timeout The maximum duration to wait for the lock.
     */
    explicit FileLock(const std::filesystem::path &path,
                      ResourceType type,
                      std::chrono::milliseconds timeout) noexcept;

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
     * type to work correctly with the std::unique_ptr Pimpl idiom.
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
    // The custom deleter for the Pimpl class.
    // The struct itself must be defined here to be a complete type for
    // std::unique_ptr, but its operator() is defined in the .cpp file where
    // FileLockImpl is a complete type.
    struct FileLockImplDeleter
    {
        void operator()(FileLockImpl *p);
    };

    // The only data member is a pointer to the implementation.
    // Using a custom deleter ensures that the resource release logic is
    // correctly invoked whenever the unique_ptr destroys the Impl object,
    // including during move-assignment.
    std::unique_ptr<FileLockImpl, FileLockImplDeleter> pImpl;
};

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
