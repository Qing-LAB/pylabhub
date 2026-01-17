#pragma once

/**
 * @file JsonConfig.hpp
 * @brief Thread-safe and process-safe JSON configuration manager.
 *
 * @see src/utils/JsonConfig.cpp
 *
 * **Design Philosophy**
 *
 * `JsonConfig` provides safe, managed access to a JSON configuration file. It is
 * designed to prevent data corruption from concurrent access by implementing a
 * two-layer locking strategy:
 *
 * 1.  **Layer 1: Inter-Thread Safety (In-Memory Protection)**
 *     - **Problem**: Prevents data races when multiple threads within a single
 *       process try to read and write the in-memory `nlohmann::json` object
 *       at the same time.
 *     - **Mechanism**: An internal `std::shared_mutex`.
 *     - **API**: The `with_json_read()` and `with_json_write()` methods, which
 *       provide safe, scoped access via lambdas. They internally use the
 *       `lock_for_read()` and `lock_for_write()` factories.
 *       - `with_json_read()` acquires a *shared* lock, allowing multiple
 *         concurrent readers.
 *       - `with_json_write()` acquires a *unique* lock, ensuring exclusive
 *         write access.
 *     - **Note**: These methods operate **only on the in-memory data** for maximum
 *       performance. They do not perform disk I/O.
 *
 * 2.  **Layer 2: Inter-Process Safety (Disk File Protection)**
 *     - **Problem**: Prevents `Process A` from reading the configuration file
 *       while `Process B` is in the middle of writing to it.
 *     - **Mechanism**: `pylabhub::utils::FileLock`.
 *     - **API**: The `reload()` and `overwrite()` methods.
 *       - `reload()`: Updates the in-memory object from the disk. It acquires
 *         a `FileLock` to ensure it reads a complete, non-corrupt file.
 *       - `overwrite()`: Saves the in-memory object to disk. It acquires a
 *         `FileLock` and uses an atomic write-and-rename pattern to prevent
 *         other processes from seeing a partially written file.
 *
 * **Lifecycle Management**: `JsonConfig` is a lifecycle-managed component. Its
 * module must be registered with `LifecycleManager` and initialized before a
 * `JsonConfig` object can be constructed, otherwise the program will abort.
 */

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <type_traits>
#include <utility>

#include "nlohmann/json.hpp"
#include "recursion_guard.hpp"
#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"

#include "pylabhub_utils_export.h"

namespace pylabhub::utils
{

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class PYLABHUB_UTILS_EXPORT JsonConfig
{
  public:
    /**
     * @brief Returns a ModuleDef for JsonConfig to be used with the LifecycleManager.
     */
    static ModuleDef GetLifecycleModule();
    /**
     * @brief Checks if the JsonConfig module has been initialized by the LifecycleManager.
     */
    static bool lifecycle_initialized() noexcept;

    JsonConfig() noexcept;
    explicit JsonConfig(const std::filesystem::path &configFile, bool createIfMissing = false,
                        std::error_code *ec = nullptr) noexcept;
    ~JsonConfig();

    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) noexcept;
    JsonConfig &operator=(JsonConfig &&) noexcept;

    /**
     * @brief Checks if this JsonConfig instance has been bound to a file path.
     * @return true if init() has been called successfully, false otherwise.
     */
    bool is_initialized() const noexcept;
    bool init(const std::filesystem::path &configFile, bool createIfMissing = false,
              std::error_code *ec = nullptr) noexcept;
    /**
     * @brief Thread-safely and process-safely reloads the configuration from disk,
     *        overwriting the current in-memory state.
     * @param ec Optional: An error_code to capture any errors.
     * @return true on success, false on failure.
     */
    bool reload(std::error_code *ec = nullptr) noexcept;
    /**
     * @brief Thread-safely and process-safely saves the current in-memory state to disk.
     *        This operation is atomic: it writes to a temporary file and then renames it.
     * @param ec Optional: An error_code to capture any errors.
     * @return true on success, false on failure.
     */
    bool overwrite(std::error_code *ec = nullptr) noexcept;

    std::filesystem::path config_path() const noexcept;

    // ----------------- Manual Locking API -----------------
    /**
     * @brief RAII guard for thread-safe read access to the in-memory JSON data.
     *
     * The constructor of this object acquires a shared lock on the data; the
     * destructor releases it. Prefer using the lambda-based `with_json_read()`
     * for simpler, safer scoped access.
     */
    class PYLABHUB_UTILS_EXPORT ReadLock
    {
      public:
        ReadLock() noexcept;
        ReadLock(ReadLock &&) noexcept;
        ReadLock &operator=(ReadLock &&) noexcept;
        ~ReadLock();
        const nlohmann::json &json() const noexcept;
        ReadLock(const ReadLock &) = delete;
        ReadLock &operator=(const ReadLock &) = delete;

      private:
        struct Impl;
        std::unique_ptr<Impl> d_;
        friend class JsonConfig;
    };

    /**
     * @brief RAII guard for thread-safe write access to the in-memory JSON data.
     *
     * The constructor of this object acquires an exclusive lock on the data; the
     * destructor releases it. Prefer using the lambda-based `with_json_write()`
     * for simpler, safer scoped access.
     */
    class PYLABHUB_UTILS_EXPORT WriteLock
    {
      public:
        WriteLock() noexcept;
        WriteLock(WriteLock &&) noexcept;
        WriteLock &operator=(WriteLock &&) noexcept;
        ~WriteLock();
        nlohmann::json &json() noexcept;
        bool commit(std::error_code *ec = nullptr) noexcept;
        WriteLock(const WriteLock &) = delete;
        WriteLock &operator=(const WriteLock &) = delete;

      private:
        struct Impl;
        std::unique_ptr<Impl> d_;
        friend class JsonConfig;
    };

    // ----------------- Preferred API: Scoped, Thread-Safe Accessors -----------------

    /**
     * @brief Provides thread-safe, read-only access to the JSON data, with an
     *        option to automatically reload from disk first.
     *
     * By default, this method first reloads the configuration from disk to ensure
     * the data is fresh (`reload_before_read = true`). To read from the
     * in-memory cache without disk I/O, set this flag to `false`.
     *
     * It acquires a shared lock, allowing multiple concurrent readers, executes
     * the provided function, and then releases the lock.
     *
     * @param fn A function or lambda with the signature `void(const nlohmann::json&)`
     * @param ec Optional: An error_code to capture any exceptions or I/O errors.
     * @param reload_before_read If true (the default), the configuration will be
     *                           reloaded from disk before the read operation.
     * @return true if the callback executed successfully, false otherwise.
     */
    template <typename F>
    bool with_json_read(F &&fn, std::error_code *ec, bool reload_before_read = true) const noexcept
    {
        static_assert(
            std::is_invocable_v<F, const nlohmann::json &>,
            "with_json_read(Func) requires callable invocable as f(const nlohmann::json&)");

        if (reload_before_read)
        {
            // reload() is thread-safe and process-safe.
            // const_cast is needed because this is a const method, but reload()
            // modifies the internal state (the in-memory data object). From the
            // user's perspective, a read operation should be const, even if it
            // updates a cache internally.
            if (!const_cast<JsonConfig *>(this)->reload(ec))
            {
                return false;
            }
        }

        if (auto r = lock_for_read(ec))
        {
            try
            {
                std::forward<F>(fn)(r->json());
                if (ec)
                    *ec = std::error_code{};
                return true;
            }
            catch (const std::exception &ex)
            {
                LOGGER_ERROR("JsonConfig::with_json_read: exception in user callback: {}",
                             ex.what());
                if (ec)
                    *ec = std::make_error_code(std::errc::io_error);
                return false;
            }
            catch (...)
            {
                LOGGER_ERROR("JsonConfig::with_json_read: unknown exception in user callback");
                if (ec)
                    *ec = std::make_error_code(std::errc::io_error);
                return false;
            }
        }
        // lock_for_read failed, it has set the error code.
        return false;
    }

    /**
     * @brief Provides thread-safe, read-only access to the latest JSON data from disk.
     */
    template <typename F>
    bool with_json_read(F &&fn) const noexcept
    {
        return with_json_read(std::forward<F>(fn), nullptr, true);
    }

    /**
     * @brief Provides thread-safe, exclusive write access to the in-memory JSON data,
     *        with an option to automatically commit to disk.
     *
     * This method acquires a unique lock, executes the provided function, and
     * then, by default, commits the changes to the disk file in a process-safe
     * manner (`commit_after_write = true`). To modify only the in-memory data
     * without persisting, set this flag to `false`.
     *
     * @param fn A function or lambda with the signature `void(nlohmann::json&)`
     * @param ec Optional: An error_code to capture any exceptions or I/O errors.
     * @param commit_after_write If true (the default), changes will be saved to
     *                           disk after the lambda executes.
     * @return true if the callback executed and any requested commit was successful.
     */
    template <typename F>
    bool with_json_write(F &&fn, std::error_code *ec, bool commit_after_write = true) noexcept
    {
        static_assert(std::is_invocable_v<F, nlohmann::json &>,
                      "with_json_write(Func) requires callable invocable as f(nlohmann::json&)");

        if (basics::RecursionGuard::is_recursing(this))
        {
            if (ec)
                *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
            return false;
        }
        basics::RecursionGuard guard(this);

        if (auto w = lock_for_write(ec))
        {
            try
            {
                std::forward<F>(fn)(w->json());

                if (commit_after_write)
                {
                    if (!w->commit(ec))
                    {
                        // commit failed, it has set the error code.
                        return false;
                    }
                }

                if (ec)
                    *ec = std::error_code{};
                return true;
            }
            catch (const std::exception &ex)
            {
                LOGGER_ERROR("JsonConfig::with_json_write: exception in user callback: {}",
                             ex.what());
                if (ec)
                    *ec = std::make_error_code(std::errc::io_error);
                return false;
            }
            catch (...)
            {
                LOGGER_ERROR("JsonConfig::with_json_write: unknown exception in user callback");
                if (ec)
                    *ec = std::make_error_code(std::errc::io_error);
                return false;
            }
        }
        // lock_for_write failed (e.g. recursion), ec is already set.
        return false;
    }

    /**
     * @brief Provides thread-safe, exclusive write access to the in-memory JSON data,
     *        automatically committing to disk.
     */
    template <typename F>
    bool with_json_write(F &&fn) noexcept
    {
        return with_json_write(std::forward<F>(fn), nullptr, true);
    }

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    // Factory methods for manual RAII locking.
    std::optional<ReadLock> lock_for_read(std::error_code *ec) const noexcept;
    std::optional<WriteLock> lock_for_write(std::error_code *ec) noexcept;

    // Internal I/O helpers
    bool private_load_from_disk_unsafe(std::error_code *ec) noexcept;
    bool private_commit_to_disk_unsafe(const nlohmann::json &snapshot,
                                       std::error_code *ec) noexcept;

    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j,
                                  std::error_code *ec) noexcept;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace pylabhub::utils