#pragma once

/**
 * @file JsonConfig.hpp
 * @brief Thread-safe and process-safe JSON configuration manager.
 *
 * @see src/utils/JsonConfig.cpp
 *
 * **Design Philosophy**
 *
 * `JsonConfig` provides safe, managed access to a JSON configuration file on disk.
 * It is designed to prevent data corruption when multiple threads or processes
 * access the same configuration file concurrently.
 *
 * 1.  **Unified Locking**: The class relies exclusively on `pylabhub::utils::FileLock`
 *     for both inter-process and intra-process (thread) synchronization. This
 *     utility provides a robust, two-layer locking model that serves as a single
 *     source of truth for concurrency control.
 *
 * 2.  **RAII-Based Safety**: The `with_json_read` and `with_json_write` methods
 *     acquire a `FileLock` for the duration of the operation, ensuring that the
 *     lock is always released automatically, even in the presence of exceptions.
 *
 * 3.  **Atomic Writes**: When saving changes, it writes to a temporary file first
 *     and then performs an atomic `rename` operation. This guarantees that a
 *     reader will never see a partially written or corrupt JSON file.
 *
 * 4.  **Pimpl Compatibility**: The public API uses `ReadLock` and `WriteLock`
 *     guard objects as Pimpl-compatible handles. This allows templated methods
 *     in the header to access the JSON data object, whose full definition is
 *     hidden in the `.cpp` file, thus preserving ABI stability.
 *
 * 5.  **Explicit Lifecycle Management**: `JsonConfig` depends on both the `Logger`
 *     and `FileLock` utilities. Therefore, it is a lifecycle-managed component.
 *     Its module must be registered and initialized before a `JsonConfig`
 *     object can be constructed, otherwise the program will abort.
 *
 */

#include <chrono>
#include <optional>
#include <filesystem>
#include <system_error>
#include <memory>
#include <utility>
#include <type_traits>

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
    bool reload(std::error_code *ec = nullptr) noexcept;
    bool save(std::error_code *ec = nullptr) noexcept;


    std::filesystem::path config_path() const noexcept;

    // ----------------- Lightweight guard types (Pimpl handles) -----------------
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


    // ----------------- Template convenience wrappers -----------------
    template <typename F>
    bool with_json_read(F &&fn, std::error_code *ec = nullptr) const noexcept
    {
        static_assert(std::is_invocable_v<F, const nlohmann::json &>,
                      "with_json_read(Func) requires callable invocable as f(const nlohmann::json&)");

        if (basics::RecursionGuard::is_recursing(this))
        {
            if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
            LOGGER_WARN("JsonConfig::with_json_read - nested call detected; refusing to re-enter.");
            return false;
        }
        basics::RecursionGuard guard(this);

        FileLock fLock(config_path(), ResourceType::File, LockMode::Blocking);
        if (!fLock.valid())
        {
            if (ec) *ec = fLock.error_code();
            return false;
        }

        if (!const_cast<JsonConfig*>(this)->private_load_from_disk_unsafe(ec))
        {
            return false;
        }

        auto r = lock_for_read(ec);
        if (!r) return false;

        try
        {
            std::forward<F>(fn)(r->json());
            if (ec) *ec = std::error_code{};
            return true;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("JsonConfig::with_json_read: exception in user callback: {}", ex.what());
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        catch (...)
        {
            LOGGER_ERROR("JsonConfig::with_json_read: unknown exception in user callback");
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }

    template <typename F>
    bool with_json_write(F &&fn, std::error_code *ec = nullptr,
                         std::optional<std::chrono::milliseconds> timeout = std::nullopt) noexcept
    {
        static_assert(std::is_invocable_v<F, nlohmann::json &>,
                      "with_json_write(Func) requires callable invocable as f(nlohmann::json&)");
        if (basics::RecursionGuard::is_recursing(this))
        {
            if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
            LOGGER_WARN("JsonConfig::with_json_write - nested call detected; refusing to re-enter.");
            return false;
        }
        basics::RecursionGuard guard(this);

        std::unique_ptr<FileLock> fLock;
        if (timeout.has_value()) {
            // A timeout was provided, use the timed lock constructor.
            fLock = std::make_unique<FileLock>(config_path(), ResourceType::File, *timeout);
        } else {
            // No timeout was provided, use the blocking lock constructor.
            fLock = std::make_unique<FileLock>(config_path(), ResourceType::File, LockMode::Blocking);
        }
        
        if (!fLock || !fLock->valid())
        {
            if (ec) *ec = fLock->error_code();
            return false;
        }

        if (!private_load_from_disk_unsafe(ec))
        {
            return false;
        }

        auto w = lock_for_write(ec);
        if (!w) return false;

        try
        {
            std::forward<F>(fn)(w->json());
            bool ok = w->commit(ec);
            return ok;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("JsonConfig::with_json_write: exception in user callback: {}", ex.what());
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        catch (...)
        {
            LOGGER_ERROR("JsonConfig::with_json_write: unknown exception in user callback");
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    // Non-template factory methods implemented in .cpp — they acquire locks and return guard objects.
    std::optional<ReadLock> lock_for_read(std::error_code *ec = nullptr) const noexcept;
    std::optional<WriteLock> lock_for_write(std::error_code *ec = nullptr) noexcept;


    bool private_load_from_disk_unsafe(std::error_code* ec) noexcept;
    bool private_commit_to_disk_unsafe(std::error_code* ec) noexcept;

    // helper used internally (defined in cpp)
    static void atomic_write_json(const std::filesystem::path &target,
                                  const nlohmann::json &j,
                                  std::error_code *ec = nullptr) noexcept;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace pylabhub::utils