// JsonConfig.hpp
#pragma once

/*******************************************************************************
 * @file JsonConfig.hpp
 * @brief Thread-safe and process-safe JSON configuration manager.
 *
 * **Design Philosophy**
 * `JsonConfig` provides a robust interface for managing configuration files,
 * handling both in-process (multi-threaded) and cross-process concurrency.
 *
 * 1.  **Concurrency Model**:
 *     - **Inter-Process Safety**: Uses an *advisory* `FileLock` mechanism. This means that other processes *using `JsonConfig`* will cooperate and only one process will attempt to write to the configuration file at a time. All file operations (`save`, `reload`, `replace`) acquire a non-blocking advisory file lock, following a "fail-fast" policy to avoid deadlocks. This mechanism does *not* provide mandatory locking against processes that do not respect this advisory lock.
 *     - **Intra-Process (Thread) Safety**: Employs a two-level locking scheme.
 *       - A coarse-grained `std::mutex` (`initMutex`) serializes all **structurally significant** operations like `init`, `save`, `reload`, `replace`, and move construction/assignment. This protects the object's structural integrity.
 *       - A fine-grained `std::shared_mutex` (`rwMutex`) protects the internal `nlohmann::json` data object for simple key-value accessors (`get`, `set`, etc.), allowing high-performance concurrent reads.
 *       - **IMPORTANT**: To achieve this performance, the caller must ensure that move operations on a `JsonConfig` object are externally synchronized with any concurrent access to that same object. Accessing an object while it is being moved from by another thread will result in undefined behavior.
 *
 * 2.  **Atomic On-Disk Writes**: The `save()` operation uses a robust
 *     `atomic_write_json` helper. This function writes the new content to a
 *     temporary file in the same directory, `fsync`s it, and then atomically
 *     renames it over the original file. This guarantees that a configuration
 *     file is never left in a corrupted, partially-written state, even if the
 *     application crashes mid-write.
 *
 * 3.  **API Design**:
 *     - **Template Accessors**: `get`, `set`, `get_or`, etc., provide a simple,
 *       type-safe way to access top-level keys.
 *     - **Callback-Based Access**: `with_json_read` and `with_json_write`
 *       provide efficient, block-based access to the underlying `json` object,
 *       avoiding unnecessary copies for complex operations.
 *     - **Exception Safety**: All public methods are `noexcept` and are designed
 *       to catch internal exceptions (e.g., from file I/O or JSON parsing),
 *       log an error, and return `false`. This ensures that a configuration
 *       error cannot crash the application.
 *
 * 4.  **Shared Library Friendliness (ABI Stability)**: The implementation is
 *     hidden behind a `std::unique_ptr` (the Pimpl idiom). This ensures that
 *     changes to private members do not alter the class's size or layout,
 *     maintaining a stable Application Binary Interface (ABI), which is critical
 *     for shared libraries.
 *
 * **Usage**
 * ```cpp
 * #include "utils/JsonConfig.hpp"
 *
 * // Initialize with a path
 * JsonConfig config;
 * if (!config.init("/path/to/config.json", true)) { // create if missing
 *     // Handle initialization failure
 * }
 *
 * // Set a value
 * config.set("port", 8080);
 * config.save();
 *
 * // Get a value with a default
 * std::string host = config.get_or<std::string>("host", "localhost");
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

    // Copying is disallowed. Moving is enabled but requires CAREFUL synchronization.
    // See cpp/docs/README_utils.md for a detailed explanation of the risks
    // associated with moving a JsonConfig object while it is in use by another thread.
    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) noexcept;
    JsonConfig &operator=(JsonConfig &&) noexcept;

    // Non-template operations (implemented in JsonConfig.cpp)
    bool init(const std::filesystem::path &configFile, bool createIfMissing);
    bool save() noexcept;
    bool reload() noexcept;
    bool replace(const json &newData) noexcept;
    json as_json() const noexcept;

    // with_json_write: exclusive write callback. It holds _initMutex during the callback,
    // provides the user callback with exclusive access to the json data, and then
    // atomically saves the result to disk. Returns true if the entire operation
    // succeeds. It is noexcept and will catch exceptions from the user callback.
    template <typename F> bool with_json_write(F &&fn) noexcept;

    // with_json_read: read-only callback that receives const json& under a shared lock.
    // Returns true on success; false if the instance is not initialized or if the callback throws.
    // noexcept and const so callers can safely call it without exceptions escaping.
    template <typename F> bool with_json_read(F &&cb) const noexcept;

    // ---------------- High-Performance Template Accessors ----------------
    // The following methods are optimized for high-contention, read-heavy workloads.
    // They use only the fine-grained `rwMutex` for data protection, bypassing the
    // coarser structural lock (`initMutex`) to allow for maximum concurrency.
    //
    // See `cpp/docs/README_utils.md` for a detailed discussion of the concurrency
    // model and the requirement for external synchronization of move operations.
    // ----------------------------------------------------------------------
    // Top-level key helpers. Implemented in-header (templates).
    template <typename T> bool set(const std::string &key, T const &value) noexcept;

    // Provides a safe, non-throwing way to retrieve a value.
    // Returns true on success, false if key is not found or type conversion fails.
    template <typename T>
    bool get(const std::string &key, T &out_value) const noexcept;

    // Returns the value for a given key, or a default value if the key does not
    // exist or a type conversion error occurs.
    template <typename T> T get_or(const std::string &key, T const &default_value) const noexcept;

    bool has(const std::string &key) const noexcept;
    bool erase(const std::string &key) noexcept;

    template <typename Func> bool update(const std::string &key, Func &&updater) noexcept;

  private:
    // private impl holds the JSON + locks + dirty flag
    // complete type here so header-only/template functions can safely access _impl members
    // -----------------------------------------------------------------------------
    struct Impl
    {
        std::filesystem::path configPath;
        nlohmann::json data;
        mutable std::shared_mutex rwMutex; // Protects `data` for fine-grained reads/writes.
        mutable std::mutex initMutex;      // Protects all structural state and serializes lifecycle/structural operations.
        std::atomic<bool> dirty{false};    // true if memory may be newer than disk

        Impl() : data(json::object()) {}
        ~Impl() = default;
    };

    // The Pimpl idiom provides a stable ABI, which is critical for shared libraries.
    std::unique_ptr<Impl> pImpl;

    // save_locked: performs the actual atomic on-disk write. Caller must hold _initMutex.
    bool save_locked(std::error_code &ec);

    // reload_locked: performs the actual file read. Caller must hold _initMutex.
    bool reload_locked() noexcept;

  private:
    // atomic, cross-platform write helper (definition in JsonConfig.cpp)
    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j);
};

// ---------------- with_json_write ----------------
template <typename F> bool JsonConfig::with_json_write(F &&fn) noexcept
{
    const void *key = static_cast<const void *>(this);

    // Detect and refuse nested calls on the same instance for this thread to prevent deadlocks.
    if (RecursionGuard::is_recursing(key))
    {
        LOGGER_WARN("JsonConfig::with_json_write - nested call detected on same instance; refusing to re-enter.");
        return false;
    }
    RecursionGuard guard(key);

    // Refuse to operate if the object has not been initialized with a file path.
    if (!pImpl || pImpl->configPath.empty())
    {
        LOGGER_ERROR("JsonConfig::with_json_write: cannot modify uninitialized config object.");
        return false;
    }
    std::lock_guard<std::mutex> lg(pImpl->initMutex);

    try
    {
        // Acquire a unique lock on the data to provide exclusive access to the callback.
        std::unique_lock<std::shared_mutex> w_lock(pImpl->rwMutex);

        // Pass the json object to the user's callback for modification.
        std::forward<F>(fn)(pImpl->data);

        // If the callback completes without throwing, mark the data as dirty.
        pImpl->dirty.store(true, std::memory_order_release);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: callback threw an exception: {}", e.what());
        return false; // Callback failed, do not save.
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: callback threw an unknown exception");
        return false; // Callback failed, do not save.
    }

    // After the callback successfully modifies the data, save it to disk.
    std::error_code ec;
    bool saved = save_locked(ec);
    if (!saved)
    {
        LOGGER_ERROR("JsonConfig::with_json_write: save_locked failed: {}", ec.message());
    }
    return saved;
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
            LOGGER_WARN("JsonConfig::with_json_read - nested call detected on same instance; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(key);

        // Ensure structural state is present before attempting to read
        std::lock_guard<std::mutex> lg(pImpl->initMutex);
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
    try
    {
        const void *key_ptr = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(key_ptr))
        {
            LOGGER_WARN(
                "JsonConfig::set - nested call detected on same instance; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(key_ptr);

        // Refuse to set data if the object has not been initialized with a file path.
        // This prevents creating a "pathless" config that can't be saved.
        if (!pImpl || pImpl->configPath.empty())
        {
            LOGGER_ERROR("JsonConfig::set: cannot set value on uninitialized config object.");
            return false;
        }
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

template <typename T>
bool JsonConfig::get(const std::string &key, T &out_value) const noexcept
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
    try
    {
        const void *key_ptr = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(key_ptr))
        {
            LOGGER_WARN(
                "JsonConfig::erase - nested call detected on same instance; refusing to re-enter.");
            return false;
        }
        RecursionGuard guard(key_ptr);

        if (!pImpl)
            return false;
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
    try
    {
        const void *key_ptr = static_cast<const void *>(this);
        if (RecursionGuard::is_recursing(key_ptr))
        {
            LOGGER_WARN("JsonConfig::update - nested call detected on same instance; refusing to "
                        "re-enter.");
            return false;
        }
        RecursionGuard guard(key_ptr);

        // Refuse to update data if the object has not been initialized with a file path.
        if (!pImpl || pImpl->configPath.empty())
        {
            LOGGER_ERROR("JsonConfig::update: cannot update value on uninitialized config object.");
            return false;
        }
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

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// end JsonConfig.hpp
