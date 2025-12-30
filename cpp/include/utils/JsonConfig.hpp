#pragma once

/**
 * @file JsonConfig.hpp
 * @brief Thread-safe configuration holder with atomic on-disk writes and cross-process advisory locking.
 *
 * Option B implementation (RAII guard objects + non-template factory functions).
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

class PYLABHUB_UTILS_EXPORT JsonConfig
{
public:
    JsonConfig() noexcept;
    explicit JsonConfig(const std::filesystem::path &configFile, bool createIfMissing = false,
                        std::error_code *ec = nullptr) noexcept;
    ~JsonConfig();

    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) noexcept;
    JsonConfig &operator=(JsonConfig &&) noexcept;

    bool init(const std::filesystem::path &configFile, bool createIfMissing = false,
              std::error_code *ec = nullptr) noexcept;
    bool reload(std::error_code *ec = nullptr) noexcept;
    bool save(std::error_code *ec = nullptr) noexcept;

    bool is_initialized() const noexcept;
    std::filesystem::path config_path() const noexcept;

    // ----------------- Lightweight guard types -----------------
    // The Impl for these guards is defined in the .cpp; these types are move-only.
    class ReadLock
    {
    public:
        ReadLock() noexcept;
        ReadLock(ReadLock &&) noexcept;
        ReadLock &operator=(ReadLock &&) noexcept;
        ~ReadLock();

        // Access JSON snapshot (const ref). Safe while this object is alive.
        const nlohmann::json &json() const noexcept;

        // non-copyable
        ReadLock(const ReadLock &) = delete;
        ReadLock &operator=(const ReadLock &) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> d_;
        friend class JsonConfig;
    };

    class WriteLock
    {
    public:
        WriteLock() noexcept;
        WriteLock(WriteLock &&) noexcept;
        WriteLock &operator=(WriteLock &&) noexcept;
        ~WriteLock();

        // Access JSON for modification. Safe while this object is alive.
        nlohmann::json &json() noexcept;

        // Commit changes to disk while still holding locks (explicit).
        // Returns true on success; on failure ec is set.
        bool commit(std::error_code *ec = nullptr) noexcept;

        // non-copyable
        WriteLock(const WriteLock &) = delete;
        WriteLock &operator=(const WriteLock &) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> d_;
        friend class JsonConfig;
    };

    // Non-template factory methods implemented in .cpp — they acquire locks and return guard objects.
    // lock_for_read: acquires checks and a shared lock; returns std::nullopt on failure.
    std::optional<ReadLock> lock_for_read(std::error_code *ec = nullptr) const noexcept;

    // lock_for_write: acquires an exclusive lock; timeout==0 => block until acquired.
    // Returns std::nullopt on failure (ec populated if provided).
    std::optional<WriteLock> lock_for_write(std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
                                            std::error_code *ec = nullptr) noexcept;

    // ----------------- Template convenience wrappers (no std::function) -----------------
    template <typename F>
    bool with_json_read(F &&fn, std::error_code *ec = nullptr) const noexcept
    {
        static_assert(std::is_invocable_v<F, const nlohmann::json &>,
                      "with_json_read(Func) requires callable invocable as f(const nlohmann::json&)");
        const void *key = static_cast<const void *>(this);
        if (pylabhub::basics::RecursionGuard::is_recursing(key))
        {
            if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
            LOGGER_WARN("JsonConfig::with_json_read - nested call detected; refusing to re-enter.");
            return false;
        }
        pylabhub::basics::RecursionGuard guard(key);

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
    bool with_json_write(F &&fn, std::chrono::milliseconds timeout = std::chrono::milliseconds{0},
                         std::error_code *ec = nullptr) noexcept
    {
        static_assert(std::is_invocable_v<F, nlohmann::json &>,
                      "with_json_write(Func) requires callable invocable as f(nlohmann::json&)");
        const void *key = static_cast<const void *>(this);
        if (pylabhub::basics::RecursionGuard::is_recursing(key))
        {
            if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
            LOGGER_WARN("JsonConfig::with_json_write - nested call detected; refusing to re-enter.");
            return false;
        }
        pylabhub::basics::RecursionGuard guard(key);

        auto w = lock_for_write(timeout, ec);
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
    struct Impl;                ///< forward-declared Pimpl (defined in .cpp)
    std::unique_ptr<Impl> pImpl;///< owned implementation pointer

    // helper used internally (defined in cpp)
    static void atomic_write_json(const std::filesystem::path &target,
                                  const nlohmann::json &j,
                                  std::error_code *ec = nullptr) noexcept;
};

} // namespace pylabhub::utils
