// JsonConfig.hpp
#pragma once

/*******************************************************************************
 * @file JsonConfig.hpp
 * @brief In-memory JSON configuration manager with explicit file locking.
 *
 * **Design Philosophy**
 *
 * `JsonConfig` is designed as an in-memory data manager. The caller is responsible
 * for managing all file I/O transactions through an explicit, stateful locking model.
 *
 * 1.  **Stateful, Explicit Locking**:
 *     - To perform a read-modify-write operation, the caller MUST first acquire an
 *       exclusive file lock using `lock()` or `lock_for()`.
 *     - A successful `lock()` call automatically reloads the configuration from disk,
 *       ensuring the in-memory state is fresh and preventing "lost update" race
 *       conditions in multi-process environments.
 *     - Write operations (`save`, `replace`, `set`, etc.) are guarded and will
 *       fail if the object is not in a `locked` state.
 *     - The lock must be explicitly released with `unlock()` to allow other
 *       processes to access the file.
 *
 * 2.  **Concurrency Model**:
 *     - **Inter-Process Safety**: A `FileLock` object, managed internally, provides
 *       exclusive, process-safe write access to the underlying file.
 *     - **Intra-Process (Thread) Safety**: A `std::shared_mutex` (`rwMutex`)
 *       protects the internal `nlohmann::json` data object, allowing concurrent
 *       reads from multiple threads. Write operations require a unique lock.
 *
 * 3.  **Atomic On-Disk Writes**: The `save()` operation uses a robust
 *     `atomic_write_json` helper. This function writes the new content to a
 *     temporary file in the same directory and then atomically renames it over the
 *     original file. This guarantees that a configuration file is never left in a
 *     corrupted, partially-written state, even if the application crashes mid-write.
 *
 * 4.  **API Design**:
 *     - **Locking**: `lock()`, `lock_for()`, `unlock()`, and `is_locked()` provide
 *       full control over the file transaction lifecycle.
 *     - **Read-only Access**: `get()`, `get_or()`, `has()`, and `as_json()`
 *       operate on the current in-memory data and do not require a lock.
 *     - **Write Access**: `save()`, `replace()`, `set()`, `erase()`, `update()`, and
 *       `with_json_write()` all require the object to be locked first.
 *     - **Exception Safety**: All public methods are `noexcept`.
 *
 * 5.  **Shared Library Friendliness (ABI Stability)**: The implementation is
 *     hidden behind a `std::unique_ptr` (the Pimpl idiom), ensuring a stable
 *     Application Binary Interface (ABI).
 *
 * **Usage (Multi-Process Read-Modify-Write)**
 * ```cpp
 * #include "utils/JsonConfig.hpp"
 * #include <chrono>
 *
 * JsonConfig config("/path/to/config.json");
 *
 * // Lock the config file with a timeout. This also reloads it from disk.
 * if (config.lock_for(std::chrono::seconds(5))) {
 *     // Read, modify, and save the data
 *     int current_value = config.get_or<int>("counter", 0);
 *     config.set("counter", current_value + 1);
 *
 *     if (config.save()) {
 *         // Save was successful
 *     }
 *
 *     config.unlock(); // Release the lock for other processes
 * } else {
 *     // Failed to acquire lock
 * }
 * ```
 ******************************************************************************/

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "PathUtil.hpp"
#include "nlohmann/json.hpp"
#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"
#include "utils/RecursionGuard.hpp"
#include "utils/ScopeGuard.hpp"

#include "pylabhub_utils_export.h"
// Disable warning C4251 for Pimpl members.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::utils
{

using json = nlohmann::json;

class PYLABHUB_UTILS_EXPORT JsonConfig
{
  public:
    JsonConfig() noexcept;
    explicit JsonConfig(const std::filesystem::path &configFile);
    ~JsonConfig();

    // Copying and moving is disallowed to prevent race conditions and ensure
    // that a single object instance manages a given configuration file.
    // For ownership transfer, use std::unique_ptr<JsonConfig>.
    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) = delete;
    JsonConfig &operator=(JsonConfig &&) = delete;

    // ----- Initialization -----
    bool init(const std::filesystem::path &configFile, bool createIfMissing);

    // ----- Explicit Locking API -----
    // Acquires an exclusive file lock, blocking until the lock is obtained.
    // On success, reloads the config from disk to prevent lost updates.
    bool lock(LockMode mode = LockMode::Blocking);

    // Acquires an exclusive file lock, waiting for the specified duration.
    // On success, reloads the config from disk.
    template <typename Rep, typename Period>
    bool lock_for(const std::chrono::duration<Rep, Period> &timeout);

    // Releases the exclusive file lock.
    void unlock();

    // Checks if the config object currently holds the file lock.
    bool is_locked() const;

    // ----- I/O Methods (require lock) -----
    // Saves the in-memory JSON state to disk. Requires lock to be held.
    bool save() noexcept;
    // Replaces the entire in-memory JSON object and saves to disk. Requires lock.
    bool replace(const json &newData) noexcept;

    // ----- In-Memory Read API (no lock required) -----
    json as_json() const noexcept;
    // Callback-based read access to the underlying json object.
    template <typename F> bool with_json_read(F &&cb) const noexcept;

    // ----- In-Memory Write API (require lock) -----
    // Callback-based write access. The user callback gets exclusive access.
    // The final state is NOT automatically saved; call save() explicitly.
    template <typename F> bool with_json_write(F &&fn) noexcept;

    // Performs a full, atomic read-modify-write transaction, handling the locking.
    template <typename Rep, typename Period, typename F>
    bool lock_and_transaction(const std::chrono::duration<Rep, Period> &timeout, F &&fn) noexcept;

    // Sets a top-level key/value. Requires lock.
    template <typename T> bool set(const std::string &key, T const &value) noexcept;
    // Erases a top-level key. Requires lock.
    bool erase(const std::string &key) noexcept;
    // Updates a key's value via a callback. Creates key if it doesn't exist. Requires lock.
    template <typename Func> bool update(const std::string &key, Func &&updater) noexcept;

    // ----- High-Performance Template Accessors (no lock required) -----
    template <typename T> bool get(const std::string &key, T &out_value) const noexcept;
    template <typename T> T get_or(const std::string &key, T const &default_value) const noexcept;
    bool has(const std::string &key) const noexcept;

  private:
    // private impl holds the JSON + locks + dirty flag
    // complete type here so header-only/template functions can safely access _impl members
    // -----------------------------------------------------------------------------
    struct Impl
    {
        std::filesystem::path configPath;
        nlohmann::json data;
        mutable std::shared_mutex rwMutex; // Protects `data` for fine-grained reads/writes.
        mutable std::mutex
            initMutex; // Protects all structural state and serializes lifecycle/structural ops.
        std::atomic<bool> dirty{false}; // true if memory may be newer than disk
        std::unique_ptr<FileLock> fileLock; // Manages the process-wide file lock.

        Impl() : data(json::object()) {}
        ~Impl() = default;
    };

    // The Pimpl idiom provides a stable ABI, which is critical for shared libraries.
    std::unique_ptr<Impl> pImpl;

    // Raw I/O helpers. Caller must hold file lock and initMutex.
    bool save_under_lock_io(std::error_code &ec);
    bool reload_under_lock_io() noexcept;

  private:
    // atomic, cross-platform write helper (definition in JsonConfig.cpp)
    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j);
};

// ---------------- Explicit Locking: lock_for ----------------
template <typename Rep, typename Period>
bool JsonConfig::lock_for(const std::chrono::duration<Rep, Period> &timeout)
{
    if (!pImpl || pImpl->configPath.empty())
    {
        LOGGER_ERROR("JsonConfig::lock_for: cannot lock an uninitialized config object.");
        return false;
    }

    std::lock_guard<std::mutex> g(pImpl->initMutex);

    // Use the timeout constructor of FileLock
    auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    pImpl->fileLock =
        std::make_unique<FileLock>(pImpl->configPath, ResourceType::File, timeout_ms);

    if (pImpl->fileLock->valid())
    {
        // Lock acquired, now reload to get the freshest data.
        if (reload_under_lock_io())
        {
            return true;
        }
        else
        {
            // If reload fails, we can't guarantee a safe state, so release the lock.
            pImpl->fileLock.reset(); // This calls destructor and releases lock
            LOGGER_ERROR("JsonConfig::lock_for: failed to reload config after acquiring lock.");
            return false;
        }
    }

    // Failed to acquire lock
    pImpl->fileLock.reset();
    return false;
}

// ---------------- with_json_write ----------------
template <typename F> bool JsonConfig::with_json_write(F &&fn) noexcept
{
    if (!is_locked())
    {
        LOGGER_ERROR("JsonConfig::with_json_write: write operations require the config to be "
                     "locked first.");
        return false;
    }

    try
    {
        // Acquire a unique lock on the data to provide exclusive access to the callback.
        std::unique_lock<std::shared_mutex> w_lock(pImpl->rwMutex);

        // Pass the json object to the user's callback for modification.
        std::forward<F>(fn)(pImpl->data);

        // If the callback completes without throwing, mark the data as dirty.
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
        LOGGER_ERROR("JsonConfig::with_json_write: callback threw an unknown exception");
        return false;
    }
}

// ---------------- with_json_read ----------------
template <typename F> bool JsonConfig::with_json_read(F &&cb) const noexcept
{
    try
    {
        const void *key = static_cast<const void *>(this);
        // Detect and refuse nested calls to prevent deadlocks.
        if (RecursionGuard::is_recursing(key))
        {
            LOGGER_WARN("JsonConfig::with_json_read - nested call detected on same instance; "
                        "refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(key);

        // Ensure the implementation object exists before attempting to read.
        if (!pImpl)
            return false;

        // Acquire shared lock for concurrent reads
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        const json &ref = pImpl->data;

        // Invoke callback with a const reference to avoid copying
        std::forward<F>(cb)(ref);
        return true;
    }
    catch (...)
    {
        // Swallow exceptions as per library convention for read helpers
        return false;
    }
}

// ---------------- Template accessors ----------------

template <typename T> bool JsonConfig::set(const std::string &key, T const &value) noexcept
{
    if (!is_locked())
    {
        LOGGER_ERROR(
            "JsonConfig::set: write operations require the config to be locked first.");
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
        const void *key_ptr = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(key_ptr))
        {
            LOGGER_WARN(
                "JsonConfig::get - nested call detected on same instance; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(key_ptr);

        if (!pImpl)
        {
            return false;
        }

        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        auto it = pImpl->data.find(key);
        if (it == pImpl->data.end())
        {
            return false;
        }
        it->get_to(out_value);
        return true;
    }
    catch (...)
    {
        // On any exception (e.g., nlohmann::json::type_error), return false.
        return false;
    }
}

template <typename T>
T JsonConfig::get_or(const std::string &key, T const &default_value) const noexcept
{
    try
    {
        const void *key_ptr = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(key_ptr))
        {
            LOGGER_WARN("JsonConfig::get_or - nested call detected on same instance; refusing to "
                        "re-enter.");
            return default_value;
        }
        RecursionGuard guard(key_ptr);

        if (!pImpl)
            return default_value;
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        auto it = pImpl->data.find(key);
        if (it == pImpl->data.end())
            return default_value;
        try
        {
            return it->template get<T>();
        }
        catch (...)
        {
            return default_value;
        }
    }
    catch (...)
    {
        return default_value;
    }
}

inline bool JsonConfig::has(const std::string &key) const noexcept
{
    try
    {
        const void *key_ptr = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(key_ptr))
        {
            LOGGER_WARN(
                "JsonConfig::has - nested call detected on same instance; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(key_ptr);

        if (!pImpl)
            return false;
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        return pImpl->data.find(key) != pImpl->data.end();
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
        LOGGER_ERROR(
            "JsonConfig::erase: write operations require the config to be locked first.");
        return false;
    }
    try
    {
        std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
        auto it = pImpl->data.find(key);
        if (it == pImpl->data.end())
            return false;
        pImpl->data.erase(it);
        pImpl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

template <typename Func> bool JsonConfig::update(const std::string &key, Func &&updater) noexcept
{
    static_assert(std::is_invocable_v<Func, json &>, "update(Func) must be invocable as f(json&)");
    if (!is_locked())
    {
        LOGGER_ERROR(
            "JsonConfig::update: write operations require the config to be locked first.");
        return false;
    }
    try
    {
        std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
        json &target = pImpl->data[key]; // create if missing
        updater(target);
        pImpl->dirty.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// ---------------- lock_and_transaction ----------------
template <typename Rep, typename Period, typename F>
bool JsonConfig::lock_and_transaction(const std::chrono::duration<Rep, Period> &timeout,
                                      F &&fn) noexcept
{
    // This high-level function encapsulates the entire lock-transact-save-unlock cycle.
    static_assert(std::is_invocable_v<F, json &>, "lock_and_transaction(Func) must be invocable as f(json&)");
    if (!pImpl)
    {
        LOGGER_ERROR("JsonConfig::lock_and_transaction: cannot operate on an uninitialized config object.");
        return false;
    }

    // 1. Acquire the inter-process file lock. This also reloads data from disk.
    if (lock_for(timeout))
    {
        // 2. Ensure the file lock is always released on scope exit.
        auto file_unlock_guard = make_scope_guard([this]() { unlock(); });

        // 3. Perform the user's modification and save the result.
        // We can reuse the existing `with_json_write` and `save` members.
        if (with_json_write(std::forward<F>(fn)) && save())
        {
            // If both modification and save succeed, the transaction is complete.
            return true;
        }

        // If with_json_write or save failed, the error is logged by those functions.
        // The file_unlock_guard will ensure the lock is released.
        return false;
    }

    LOGGER_DEBUG("JsonConfig::lock_and_transaction: failed to acquire file lock within timeout.");
    return false;
}

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// end JsonConfig.hpp
