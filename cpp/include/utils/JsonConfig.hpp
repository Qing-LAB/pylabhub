/*******************************************************************************
 * @file JsonConfig.hpp
 * @brief Thread-safe and process-safe JSON configuration manager with
 *        explicit file locking and atomic writes.
 *
 * @see src/utils/JsonConfig.cpp
 * @see tests/test_jsonconfig.cpp
 *
 * **Design Philosophy**
 *
 * `JsonConfig` is a high-level manager for a JSON configuration file, designed
 * to be robust in both multi-threaded and multi-process environments. It serves
 * as an in-memory cache of the on-disk data, with an explicit API for
 * synchronizing changes.
 *
 * 1.  **Stateful, Explicit Locking for Process Safety**:
 *     - To prevent race conditions between different processes, all write
 *       operations are protected by an internal `FileLock`.
 *     - A typical read-modify-write operation must be performed within a
 *       transaction. The recommended way to do this is with the high-level
 *       `lock_and_transaction` method, which handles acquiring the file lock,
 *       reloading data from disk, executing the user's modifications, saving
 *       back to disk, and releasing the lock.
 *     - This model prevents the "lost update" problem, where two processes might
 *       read the same data, modify it, and have one overwrite the other's changes.
 *
 * 2.  **Two-Layer Concurrency Model**:
 *     - **Inter-Process Safety**: A `pylabhub::utils::FileLock` is used to
 *       ensure that only one process can have write access to the configuration
 *       file at any given time.
 *     - **Intra-Process (Thread) Safety**: A `std::shared_mutex` protects the
 *       in-memory `nlohmann::json` object. This allows multiple threads within
 *       the same process to perform concurrent reads (`get`, `get_or`, `has`),
 *       while write operations (`set`, `update`, etc.) require an exclusive lock,
 *       ensuring the integrity of the in-memory data.
 *
 * 3.  **Atomic On-Disk Writes**:
 *     - The `save()` operation (called internally by `lock_and_transaction`)
 *       uses a robust `atomic_write_json` helper. This function first writes the
 *       new content to a temporary file (`.tmp`) in the same directory and then
 *       atomically renames it over the original file (`rename` on POSIX,
 *       `ReplaceFileW` on Windows).
 *     - This guarantees that the configuration file is never left in a corrupted,
 *       partially-written state, even if the application crashes mid-write.
 *
 * 4.  **ABI Stability (Pimpl Idiom)**:
 *     - The implementation is hidden behind a `std::unique_ptr` to a private
 *       `Impl` struct. This isolates all third-party headers (`nlohmann::json`)
 *       and STL containers from the public API, ensuring a stable Application
 *       Binary Interface (ABI) for the shared library.
 *
 * **Recommended Usage: Atomic Transactions**
 *
 * For any read-modify-write operation, the `lock_and_transaction` method is the
 * safest and most convenient approach.
 *
 * ```cpp
 * #include "utils/JsonConfig.hpp"
 * #include <chrono>
 *
 * // Create an instance tied to a specific file path.
 * pylabhub::utils::JsonConfig config("/path/to/settings.json");
 *
 * // This lambda will be executed once the file lock is acquired.
 * auto increment_counter = [](nlohmann::json& data) {
 *     int current_value = data.value("launches", 0);
 *     data["launches"] = current_value + 1;
 * };
 *
 * // Perform a safe, atomic read-modify-write transaction with a 5s timeout.
 * bool success = config.lock_and_transaction(std::chrono::seconds(5), increment_counter);
 *
 * if (success) {
 *     // The "launches" counter in settings.json was safely incremented.
 * } else {
 *     // Failed to acquire lock or an error occurred during the transaction.
 *     // The logger will contain details.
 * }
 * ```
 ******************************************************************************/
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include "nlohmann/json.hpp"

#include "pylabhub_utils_export.h"
#include "format_tools.hpp"
#include "recursion_guard.hpp"
#include "scope_guard.hpp"
#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"

// Disable warning C4251 for the std::unique_ptr Pimpl member.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::utils
{

using json = nlohmann::json;

/**
 * @class JsonConfig
 * @brief Manages a JSON configuration file with thread and process safety.
 */
class PYLABHUB_UTILS_EXPORT JsonConfig
{
  public:
    /** @brief Default constructor. Creates an uninitialized object. */
    JsonConfig() noexcept;

    /**
     * @brief Constructs and initializes the object for a specific file.
     * @param configFile The path to the JSON configuration file.
     */
    explicit JsonConfig(const std::filesystem::path &configFile);

    ~JsonConfig();

    // Copying and moving are disallowed to ensure a single object instance
    // manages a given configuration file, preventing confusion and race conditions.
    // To manage ownership, use a smart pointer like `std::unique_ptr<JsonConfig>`.
    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) = delete;
    JsonConfig &operator=(JsonConfig &&) = delete;

    /**
     * @brief Initializes or re-initializes the object with a file path.
     * @param configFile The path to the JSON configuration file.
     * @param createIfMissing If `true`, an empty JSON file `{}` will be created
     *                        atomically if one does not already exist.
     * @return `true` on success, `false` on failure.
     */
    bool init(const std::filesystem::path &configFile, bool createIfMissing);

    // ----- Manual Locking API -----
    // These methods provide low-level control over the file lock. For most
    // use cases, `lock_and_transaction` is preferred.

    /**
     * @brief Manually acquires an exclusive file lock.
     * On success, automatically reloads the config from disk to ensure the
     * in-memory state is up-to-date.
     * @param mode The locking mode (`Blocking` or `NonBlocking`).
     * @return `true` if the lock was acquired, `false` otherwise.
     */
    bool lock(LockMode mode = LockMode::Blocking);

    /**
     * @brief Manually acquires an exclusive file lock, waiting up to a specified duration.
     * On success, automatically reloads the config from disk.
     * @tparam Rep Duration representation (e.g., `long long`).
     * @tparam Period Duration period (e.g., `std::milli`).
     * @param timeout The maximum duration to wait for the lock.
     * @return `true` if the lock was acquired, `false` on timeout or error.
     */
    template <typename Rep, typename Period>
    bool lock_for(const std::chrono::duration<Rep, Period> &timeout);

    /** @brief Manually releases the exclusive file lock. */
    void unlock();

    /** @brief Checks if this object instance currently holds the file lock. */
    bool is_locked() const;

    // ----- High-Level Transaction API -----

    /**
     * @brief Performs a complete, atomic read-modify-write transaction.
     *
     * This is the recommended method for safely modifying the configuration. It
     * handles the entire cycle: acquiring the file lock, reloading data from disk,
     * executing the user's modification lambda, saving the new data to disk,
     * and finally releasing the lock.
     *
     * @tparam Rep Duration representation.
     * @tparam Period Duration period.
     * @tparam F A callable type (e.g., a lambda) with the signature `void(nlohmann::json&)`.
     * @param timeout The maximum duration to wait to acquire the file lock.
     * @param fn The function to execute on the JSON data after the lock is acquired.
     * @return `true` if the entire transaction succeeded, `false` otherwise.
     */
    template <typename Rep, typename Period, typename F>
    bool lock_and_transaction(const std::chrono::duration<Rep, Period> &timeout, F &&fn) noexcept;

    // ----- In-Memory Read API (Thread-Safe, No Lock Required) -----

    /**
     * @brief Returns a copy of the entire in-memory JSON object.
     * @return A `nlohmann::json` object.
     */
    json as_json() const noexcept;

    /**
     * @brief Provides read-only, callback-based access to the in-memory JSON data.
     * This is more efficient than `as_json()` if you don't need a deep copy.
     * @tparam F A callable with the signature `void(const nlohmann::json&)`.
     * @param cb The callback to execute with a const reference to the data.
     * @return `true` on success, `false` on error.
     */
    template <typename F> bool with_json_read(F &&cb) const noexcept;

    /**
     * @brief Retrieves a value from the JSON object.
     * @tparam T The desired type of the value.
     * @param key The top-level key for the value.
     * @param[out] out_value The variable to store the retrieved value.
     * @return `true` if the key exists and the type is compatible, `false` otherwise.
     */
    template <typename T> bool get(const std::string &key, T &out_value) const noexcept;

    /**
     * @brief Retrieves a value from the JSON object, returning a default if not found.
     * @tparam T The desired type of the value.
     * @param key The top-level key for the value.
     * @param default_value The value to return if the key doesn't exist or the
     *                      type is incompatible.
     * @return The retrieved value or the default value.
     */
    template <typename T> T get_or(const std::string &key, T const &default_value) const noexcept;

    /**
     * @brief Checks if a top-level key exists in the JSON object.
     * @param key The key to check.
     * @return `true` if the key exists, `false` otherwise.
     */
    bool has(const std::string &key) const noexcept;


    // ----- In-Memory Write API (Thread-Safe, File Lock Required) -----

    /**
     * @brief Atomically saves the current in-memory JSON state to the disk file.
     * Requires the file lock to be held via `lock()` or `lock_for()`.
     * @return `true` on success, `false` on failure.
     */
    bool save() noexcept;

    /**
     * @brief Replaces the entire in-memory JSON object and saves it to disk.
     * Requires the file lock to be held.
     * @param newData The new `nlohmann::json` object to save.
     * @return `true` on success, `false` on failure.
     */
    bool replace(const json &newData) noexcept;

    /**
     * @brief Provides exclusive, callback-based write access to the in-memory data.
     *
     * The final state is NOT automatically saved to disk; you must call `save()`
     * explicitly after your modifications are complete.
     * Requires the file lock to be held.
     *
     * @tparam F A callable with the signature `void(nlohmann::json&)`.
     * @param fn The callback to execute for modifying the data.
     * @return `true` on success, `false` on error.
     */
    template <typename F> bool with_json_write(F &&fn) noexcept;

    /**
     * @brief Sets a top-level key-value pair in the in-memory JSON object.
     * Requires the file lock to be held. Does not automatically save.
     * @tparam T The type of the value.
     * @param key The top-level key.
     * @param value The value to set.
     * @return `true` on success, `false` on error.
     */
    template <typename T> bool set(const std::string &key, T const &value) noexcept;

    /**
     * @brief Erases a top-level key from the in-memory JSON object.
     * Requires the file lock to be held. Does not automatically save.
     * @param key The key to erase.
     * @return `true` if the key was found and erased, `false` otherwise.
     */
    bool erase(const std::string &key) noexcept;

    /**
     * @brief Updates a value via a callback. Creates the key if it doesn't exist.
     * Requires the file lock to be held. Does not automatically save.
     * @tparam Func A callable with the signature `void(nlohmann::json&)`.
     * @param key The key of the value to update.
     * @param updater The function to execute on the target JSON value.
     * @return `true` on success, `false` on error.
     */
    template <typename Func> bool update(const std::string &key, Func &&updater) noexcept;

  private:
    // The Pimpl struct holds all internal state, ensuring a stable ABI.
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    // Internal low-level I/O helpers. Caller must hold the file lock.
    bool save_under_lock_io(std::error_code &ec);
    bool reload_under_lock_io() noexcept;

    // Internal helper for atomic file writes.
    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j);
};

// ============================================================================
// Template Method Implementations
// ============================================================================

template <typename Rep, typename Period>
bool JsonConfig::lock_for(const std::chrono::duration<Rep, Period> &timeout)
{
    if (!pImpl || pImpl->configPath.empty())
    {
        LOGGER_ERROR("JsonConfig::lock_for: cannot lock an uninitialized config object.");
        return false;
    }

    std::lock_guard<std::mutex> g(pImpl->initMutex);

    // Use the timeout-based constructor of FileLock.
    auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    pImpl->fileLock = std::make_unique<FileLock>(pImpl->configPath, ResourceType::File, timeout_ms);

    if (pImpl->fileLock->valid())
    {
        // Lock acquired, now reload data from disk to prevent lost updates.
        if (reload_under_lock_io())
        {
            return true;
        }
        else
        {
            // If reload fails, we can't guarantee a safe state. Release the lock.
            pImpl->fileLock.reset(); // Calls deleter, which releases the lock.
            LOGGER_ERROR("JsonConfig::lock_for: failed to reload config after acquiring lock.");
            return false;
        }
    }

    // Failed to acquire lock (timed out or other error).
    pImpl->fileLock.reset();
    return false;
}

template <typename F> bool JsonConfig::with_json_write(F &&fn) noexcept
{
    if (!is_locked())
    {
        LOGGER_ERROR("JsonConfig::with_json_write: write operations require a file lock.");
        return false;
    }

    try
    {
        // Acquire an exclusive lock on the in-memory data.
        std::unique_lock<std::shared_mutex> w_lock(pImpl->rwMutex);

        // Pass the json object to the user's callback for modification.
        std::forward<F>(fn)(pImpl->data);

        // Mark the data as "dirty" to indicate it has changed since the last save.
        pImpl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: callback threw an exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: callback threw an unknown exception.");
        return false;
    }
}

template <typename F> bool JsonConfig::with_json_read(F &&cb) const noexcept
{
    try
    {
        // Use a unique key (the `this` pointer) to detect and prevent re-entrant calls
        // from the same thread, which would cause a deadlock on the mutex.
        const void *recursion_key = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(recursion_key))
        {
            LOGGER_WARN("JsonConfig::with_json_read: nested call detected; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(recursion_key);

        if (!pImpl) return false;

        // Acquire a shared lock to allow for concurrent reads from multiple threads.
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        const json &ref = pImpl->data;

        std::forward<F>(cb)(ref);
        return true;
    }
    catch (...)
    {
        return false; // Swallow exceptions as per the noexcept contract.
    }
}

template <typename T> bool JsonConfig::set(const std::string &key, T const &value) noexcept
{
    if (!is_locked())
    {
        LOGGER_ERROR("JsonConfig::set: write operations require a file lock.");
        return false;
    }
    try
    {
        std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
        pImpl->data[key] = value;
        pImpl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

template <typename T> bool JsonConfig::get(const std::string &key, T &out_value) const noexcept
{
    try
    {
        const void *recursion_key = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(recursion_key))
        {
            LOGGER_WARN("JsonConfig::get: nested call detected; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(recursion_key);

        if (!pImpl) return false;

        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        auto it = pImpl->data.find(key);
        if (it == pImpl->data.end())
        {
            return false;
        }
        // Use nlohmann::json's safe `get_to` to avoid exceptions on type mismatch.
        it->get_to(out_value);
        return true;
    }
    catch (...)
    {
        // On any exception (e.g., from get_to), return false.
        return false;
    }
}

template <typename T>
T JsonConfig::get_or(const std::string &key, T const &default_value) const noexcept
{
    try
    {
        const void *recursion_key = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(recursion_key))
        {
            LOGGER_WARN("JsonConfig::get_or: nested call detected; refusing to re-enter.");
            return default_value;
        }
        RecursionGuard guard(recursion_key);

        if (!pImpl) return default_value;

        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        // Use nlohmann::json's built-in `value` method, which is more concise.
        return pImpl->data.value(key, default_value);
    }
    catch (...)
    {
        return default_value;
    }
}

template <typename Func> bool JsonConfig::update(const std::string &key, Func &&updater) noexcept
{
    static_assert(std::is_invocable_v<Func, json &>, "`updater` must be invocable as f(json&)");
    if (!is_locked())
    {
        LOGGER_ERROR("JsonConfig::update: write operations require a file lock.");
        return false;
    }
    try
    {
        std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
        // Using operator[] creates the key with a null value if it doesn't exist.
        json &target = pImpl->data[key];
        updater(target);
        pImpl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

template <typename Rep, typename Period, typename F>
bool JsonConfig::lock_and_transaction(const std::chrono::duration<Rep, Period> &timeout,
                                      F &&fn) noexcept
{
    static_assert(std::is_invocable_v<F, json &>, "`fn` must be invocable as f(json&)");
    if (!pImpl)
    {
        LOGGER_ERROR("JsonConfig::lock_and_transaction: uninitialized config object.");
        return false;
    }

    // 1. Acquire the inter-process file lock and reload data from disk.
    if (lock_for(timeout))
    {
        // 2. Ensure the file lock is always released when this scope exits.
        auto file_unlock_guard = basics::make_scope_guard([this]() { unlock(); });

        // 3. Perform the user's modification and save the result.
        if (with_json_write(std::forward<F>(fn)) && save())
        {
            return true; // Success.
        }
        // If modification or save failed, errors are logged by those functions.
        return false;
    }

    LOGGER_DEBUG("JsonConfig::lock_and_transaction: failed to acquire file lock within timeout.");
    return false;
}


// --- Inline Method Implementations ---

inline bool JsonConfig::has(const std::string &key) const noexcept
{
    try
    {
        const void *recursion_key = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(recursion_key))
        {
            LOGGER_WARN("JsonConfig::has: nested call detected; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(recursion_key);

        if (!pImpl) return false;
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        return pImpl->data.contains(key);
    }
    catch (...)
    {
        return false;
    }
}

inline bool JsonConfig::erase(const std::string &key) noexcept
{
    if (!is_locked())
    {
        LOGGER_ERROR("JsonConfig::erase: write operations require a file lock.");
        return false;
    }
    try
    {
        std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
        if (pImpl->data.erase(key) > 0)
        {
            pImpl->dirty.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
