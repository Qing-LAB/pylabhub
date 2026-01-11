/*******************************************************************************
 * @file FileLock.hpp
 * @brief Cross-platform, RAII-style, advisory file lock for inter-process
 *        and inter-thread synchronization.
 *
 * @see src/utils/FileLock.cpp
 * @see tests/test_filelock.cpp
 *
 * **Design Philosophy**
 *
 * `FileLock` provides a robust, cross-platform mechanism for managing exclusive
 * access to a shared resource, identified by a filesystem path. It is a critical
 * component for ensuring data integrity in multi-process applications, such as
 * when reading or writing a shared `JsonConfig` file.
 *
 * 1.  **RAII (Resource Acquisition Is Initialization)**: The lock is acquired in
 *     the constructor and automatically released in the destructor. This modern
 *     C++ pattern guarantees that locks are always released, even in the
 *     presence of exceptions, preventing deadlocks caused by leaked lock files.
 *
 * 2.  **Two-Layer Locking Model**: `FileLock` provides consistent semantics for
 *     both inter-process and intra-process (multi-threaded) synchronization.
 *     - **Inter-Process Lock**: An OS-level file lock (`flock` on POSIX,
 *       `LockFileEx` on Windows) is used on a dedicated `.lock` file. This
 *       ensures that only one process can hold the lock at a time.
 *     - **Intra-Process Lock**: A process-local registry (a map guarded by a
 *       `std::mutex` and `std::condition_variable`) is used to manage lock
 *       contention between different threads *within the same process*. This is
 *       necessary because OS-level file locks can behave differently across
 *       platforms for threads in the same process. This layer ensures that
 *       `Blocking` and `NonBlocking` modes work identically everywhere.
 *
 * 3.  **Advisory Lock**: This is an *advisory* lock, not a mandatory one. It
 *     relies on all cooperating processes and threads to use the `FileLock`
 *     mechanism to access the shared resource. It does NOT prevent a
 *     non-cooperating process from ignoring the lock and accessing the target
 *     file directly.
 *
 * 4.  **Separate Lock File**: Instead of locking the target resource directly, a
 *     separate lock file is used (e.g., `/path/to/file.txt.lock`). This avoids
 *     potential interference with file content operations and simplifies the
 *     implementation.
 *
 * 5.  **Path Canonicalization**: To ensure that different path representations
 *     (e.g., `/path/./file` vs `/path/file`, or symlinks) all contend for the
 *     same lock, `FileLock` automatically resolves the resource path to a
 *     canonical form before creating the lock file path. It uses
 *     `std::filesystem::canonical` if the path exists, and falls back to
 *     `std::filesystem::absolute` if it doesn't (to allow locking a resource
 *     before it is created).
 *
 * 6.  **ABI Stability (Pimpl Idiom)**: The class uses the Pimpl idiom to hide
 *     all implementation details, including platform-specific handles and internal
 *     locking primitives, ensuring a stable ABI for the shared library.
 *
 * **Usage**
 *
 * `FileLock` is a lifecycle-managed utility. Its module must be initialized
 * before a `FileLock` object can be constructed.
 *
 * ```cpp
 * #include "utils/Lifecycle.hpp"
 * #include "utils/FileLock.hpp"
 * #include "utils/Logger.hpp" // FileLock depends on Logger
 *
 * void perform_exclusive_work(const std::filesystem::path& resource) {
 *     // In main() or test setup, ensure the lifecycle is started:
 *     // pylabhub::utils::LifecycleGuard guard(
 *     //     pylabhub::utils::FileLock::GetLifecycleModule(),
 *     //     pylabhub::utils::Logger::GetLifecycleModule()
 *     // );
 *
 *     // Attempt to acquire a lock with a 5-second timeout.
 *     pylabhub::utils::FileLock lock(resource,
 *                                 pylabhub::utils::ResourceType::File,
 *                                 std::chrono::seconds(5));
 *
 *     if (lock.valid()) {
 *         // ... perform work ...
 *     } else {
 *         // Handle failure. Using a logger here is safe because the FileLock
 *         // module depends on the Logger module.
 *         LOGGER_ERROR("Failed to acquire lock for {}. Error: {}",
 *                      resource.string(), lock.error_code().message());
 *     }
 *     // Lock is automatically released here when `lock` goes out of scope.
 * }
 * ```
 ******************************************************************************/
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

#include "pylabhub_utils_export.h"
#include "utils/Lifecycle.hpp" // For ModuleDef

// Disable warning C4251 on MSVC for the std::unique_ptr Pimpl member.
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
 * @brief Specifies the type of resource being locked.
 *
 * This is used to generate an unambiguous lock file name, preventing collisions
 * between a lock for a file and a lock for a directory with the same name.
 * - File: `/path/to/resource.txt` -> `/path/to/resource.txt.lock`
 * - Directory: `/path/to/resource/` -> `/path/to/resource.dir.lock`
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
 * @brief A cross-platform, RAII-style advisory file lock for process and
 *        thread synchronization.
 *
 * This class acquires a system-wide lock upon construction and automatically
 * releases it upon destruction. It is designed to be safe for both multi-process
 * and multi-threaded scenarios.
 *
 * @warning The underlying POSIX lock mechanism (`flock`) may be unreliable
 *          over network filesystems like NFS. This class is best suited for
 *          local filesystem synchronization.
 */
class PYLABHUB_UTILS_EXPORT FileLock
{
  public:
    /**
     * @brief Returns a ModuleDef for the FileLock to be used with the LifecycleManager.
     *
     * @param cleanup_on_shutdown If true, a shutdown task will be registered to
     *                            clean up stale lock files with best effort. By default, this is
     * set to false to leave .lock files intact after the process/application exits. Under highly
     * contentioned scenarios, cleaning .lock files may cause competing processes to acquire access
     * to the same .lock file by mistake, leading to potential data corruption.
     * @return A configured ModuleDef for the FileLock utility.
     */
    static ModuleDef GetLifecycleModule(bool cleanup_on_shutdown = false);

    /**
     * @brief Checks if the FileLock module has been initialized by the LifecycleManager.
     */
    static bool lifecycle_initialized() noexcept;

    /**
     * @brief Predicts the canonical path of the lock file for a given resource.
     *
     * This static utility function allows you to determine the exact, canonical
     * path of the lock file that *would* be used for a resource, without needing to
     * instantiate a `FileLock` object. It implements the core path canonicalization
     * logic used by the constructor.
     *
     * @param path The path to the resource (file or directory).
     * @param type The type of the resource (`File` or `Directory`).
     * @return The canonical, absolute path to the corresponding lock file.
     *         Returns an empty path on failure.
     */
    static std::filesystem::path get_expected_lock_fullname_for(const std::filesystem::path &path,
                                                                ResourceType type) noexcept;

    /**
     * @brief Constructs a FileLock and attempts to acquire the lock.
     *
     * @param path The path to the file or resource to be locked.
     * @param type The type of resource (`File` or `Directory`), which determines
     *             the lock file's naming convention.
     * @param mode The locking mode (`Blocking` or `NonBlocking`).
     */
    explicit FileLock(const std::filesystem::path &path, ResourceType type,
                      LockMode mode = LockMode::Blocking) noexcept;

    /**
     * @brief Constructs a FileLock and attempts to acquire the lock within a given time.
     *
     * @param path The path to the file or resource to be locked.
     * @param type The type of resource (`File` or `Directory`).
     * @param timeout The maximum duration to wait for the lock. If the lock is not
     *                acquired within this time, the constructor returns and `valid()`
     *                will be false.
     */
    explicit FileLock(const std::filesystem::path &path, ResourceType type,
                      std::chrono::milliseconds timeout) noexcept;

    /**
     * @brief Attempts to acquire a lock, returning an optional FileLock.
     *
     * This static factory method provides a modern C++ interface for lock
     * acquisition. Instead of constructing an object and checking `valid()`, this
     * method returns an `std::optional<FileLock>`, which contains a value only on
     * success.
     *
     * @param path The path to the resource to be locked.
     * @param type The type of resource (`File` or `Directory`).
     * @param mode The locking mode.
     *             - `LockMode::Blocking` (Default): Waits indefinitely until the
     *               lock is acquired. Returns `std::nullopt` only on a
     *               non-recoverable error (e.g., invalid arguments).
     *             - `LockMode::NonBlocking`: Returns immediately. Returns a
     *               `FileLock` if acquired, otherwise `std::nullopt`.
     * @return An `std::optional<FileLock>` containing a valid lock on success.
     *
     * @code
     * if (auto lock = FileLock::try_lock(path, ResourceType::File, LockMode::NonBlocking)) {
     *     // Safely use the lock, which is valid and scoped to this block.
     *     // lock->get_locked_resource_path()...
     * } else {
     *     // Handle lock failure (e.g., already locked by another process).
     * }
     * @endcode
     */
    [[nodiscard]] static std::optional<FileLock>
    try_lock(const std::filesystem::path &path, ResourceType type,
             LockMode mode = LockMode::Blocking) noexcept;

    /**
     * @brief Attempts to acquire a lock within a given time.
     *
     * @param path The path to the resource to be locked.
     * @param type The type of resource (`File` or `Directory`).
     * @param timeout The maximum duration to wait for the lock.
     * @return An `std::optional<FileLock>` containing a valid lock on success,
     *         or `std::nullopt` if the lock was not acquired within the timeout.
     */
    [[nodiscard]] static std::optional<FileLock>
    try_lock(const std::filesystem::path &path, ResourceType type,
             std::chrono::milliseconds timeout) noexcept;

    /// @brief Move constructor. Transfers ownership of an existing lock.
    FileLock(FileLock &&other) noexcept;

    /// @brief Move assignment operator. Transfers ownership of an existing lock.
    FileLock &operator=(FileLock &&other) noexcept;

    /**
     * @brief [Cleanup] Safely removes leftover lock files created by this process.
     *
     * This function is registered with the `LifecycleManager` and is automatically
     * called at program shutdown. It iterates through all locks created by the
     * current process and attempts to safely remove the associated `.lock` files.
     * To avoid deleting a lock file still in use by another process, it first
     * attempts to acquire a non-blocking lock on the file. If successful, the
     * file is deemed safe to delete.
     */
    static void cleanup();

    // The class is non-copyable to prevent accidental duplication of lock ownership.
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    /**
     * @brief Destructor. Releases the lock if it is held.
     *
     * This is defined in the .cpp file where `FileLockImpl` is a complete
     * type, which is a requirement of the Pimpl idiom with `std::unique_ptr`.
     */
    ~FileLock();

    /**
     * @brief Checks if the lock was successfully acquired and is currently held.
     * @return `true` if the lock is valid, `false` otherwise.
     */
    bool valid() const noexcept;

    /**
     * @brief Gets the error code from the last failed lock acquisition attempt.
     * @return `std::error_code` containing the OS or timeout error. If `valid()`
     *         is true, the error code will be empty/zero.
     */
    std::error_code error_code() const noexcept;

    /**
     * @brief If the lock is valid, returns the absolute path of the resource being protected.
     * @return An optional containing the path if the lock is held, otherwise an empty optional.
     */
    std::optional<std::filesystem::path> get_locked_resource_path() const noexcept;

    /**
     * @brief If the lock is valid, returns the canonical path of the lock file being used.
     *
     * This member function returns the actual, canonicalized, absolute path of the
     * `.lock` file being used by this specific `FileLock` instance. Useful for diagnostics.
     *
     * @return An optional containing the lock file path if the lock is held,
     *         otherwise an empty optional.
     */
    std::optional<std::filesystem::path> get_canonical_lock_file_path() const noexcept;

  private:
    // Private constructor for factory functions
    FileLock() noexcept;

    // The custom deleter for the Pimpl class. This is the key to achieving
    // correct RAII behavior with the Pimpl idiom. Its implementation in the
    // .cpp file contains the lock release logic.
    struct FileLockImplDeleter
    {
        void operator()(FileLockImpl *p);
    };

    // The only data member is a unique_ptr to the implementation, using the
    // custom deleter. This ensures that the resource release logic is
    // correctly invoked whenever the unique_ptr goes out of scope or is reset.
    std::unique_ptr<FileLockImpl, FileLockImplDeleter> pImpl;
};

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
