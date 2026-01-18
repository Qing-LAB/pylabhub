#pragma once

/**
 * @file JsonConfig.hpp
 * @brief Thread-safe and process-safe JSON configuration manager.
 *
 * This variant implements JsonConfig-owned Transaction objects, but exposes
 * a rvalue-only TransactionProxy to enforce immediate-use at compile time:
 *
 *   cfg.transaction(flags).read(...);
 *   cfg.transaction(flags).write(...);
 *
 * The proxy's read/write are &&-qualified, so:
 *   auto t = cfg.transaction(); t.read(...);   // will NOT compile
 *
 * ABI note:
 *  - pImpl is an incomplete type here.
 *  - This header must NOT access Impl fields directly.
 *  - All operations that require Impl internals must call JsonConfig methods.
 */

#include <chrono>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <source_location>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "debug_info.hpp"
#include "nlohmann/json.hpp"
#include "recursion_guard.hpp"
#include "utils/FileLock.hpp"
#include "utils/Lifecycle.hpp"
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
  private:
    class Transaction; // internal bookkeeping record (not user-facing)

  public:
    class ReadLock;
    class WriteLock;

    // Transaction flags
    enum class AccessFlags
    {
        Default = 0,
        UnSynced = Default,
        ReloadFirst = 1 << 0,
        CommitAfter = 1 << 1,
        FullSync = ReloadFirst | CommitAfter,
    };

    friend AccessFlags operator|(AccessFlags a, AccessFlags b)
    {
        return static_cast<AccessFlags>(static_cast<int>(a) | static_cast<int>(b));
    }

    // ----------------- Rvalue-only transaction proxy -----------------
    class TransactionProxy
    {
      public:
        TransactionProxy() = delete;
        TransactionProxy(const TransactionProxy &) = delete;
        TransactionProxy &operator=(const TransactionProxy &) = delete;

        TransactionProxy(TransactionProxy &&other) noexcept
            : owner_(other.owner_), id_(other.id_), flags_(other.flags_), consumed_(other.consumed_)
        {
            other.owner_ = nullptr;
            other.id_ = 0;
            other.consumed_ = true;
        }

        TransactionProxy &operator=(TransactionProxy &&) = delete;

        ~TransactionProxy()
        {
#ifndef NDEBUG
            // If someone calls cfg.transaction(...) and forgets to consume it, warn loudly.
            if (owner_ && !consumed_)
            {
                LOGGER_ERROR("JsonConfig::transaction() proxy was not consumed (missing .read()/.write()).");
            }
#endif
        }

        template <typename F>
        void read(F &&fn, std::error_code *ec = nullptr,
                  std::source_location loc = std::source_location::current()) && noexcept
        {
            consumed_ = true;
            if (!owner_)
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::not_connected);
                return;
            }
            owner_->consume_read_(id_, flags_, std::forward<F>(fn), ec, loc);
        }

        template <typename F>
        void write(F &&fn, std::error_code *ec = nullptr,
                   std::source_location loc = std::source_location::current()) && noexcept
        {
            consumed_ = true;
            if (!owner_)
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::not_connected);
                return;
            }
            owner_->consume_write_(id_, flags_, std::forward<F>(fn), ec, loc);
        }

      private:
        friend class JsonConfig;
        TransactionProxy(JsonConfig *owner, uint64_t id, AccessFlags flags) noexcept
            : owner_(owner), id_(id), flags_(flags)
        {
        }

        JsonConfig *owner_ = nullptr;
        uint64_t id_ = 0;
        AccessFlags flags_ = AccessFlags::Default;
        bool consumed_ = false;
    };

  public:
    JsonConfig() noexcept;
    explicit JsonConfig(const std::filesystem::path &configFile, bool createIfMissing = false,
                        std::error_code *ec = nullptr) noexcept;
    ~JsonConfig();

    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;
    JsonConfig(JsonConfig &&) noexcept;
    JsonConfig &operator=(JsonConfig &&) noexcept;

    static ModuleDef GetLifecycleModule();
    static bool lifecycle_initialized() noexcept;

    bool is_initialized() const noexcept;
    bool init(const std::filesystem::path &configFile, bool createIfMissing = false,
              std::error_code *ec = nullptr) noexcept;

    bool reload(std::error_code *ec = nullptr) noexcept;
    bool overwrite(std::error_code *ec = nullptr) noexcept;

    std::filesystem::path config_path() const noexcept;

    using TxId = uint64_t;

    // NEW: return a proxy by value (enforces immediate-use via &&-qualified methods)
    [[nodiscard]] TransactionProxy transaction(AccessFlags flags = AccessFlags::Default,
                                               std::error_code *ec = nullptr) noexcept;

    // Optional manual release (still supported, but usually unnecessary now)
    bool release_transaction(TxId id, std::error_code *ec = nullptr) noexcept;

    // Manual locking API (unchanged)
    std::optional<ReadLock> lock_for_read(std::error_code *ec) const noexcept;
    std::optional<WriteLock> lock_for_write(std::error_code *ec) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    // Internal I/O helpers (implemented in .cpp; safe for ABI)
    bool private_load_from_disk_unsafe(std::error_code *ec) noexcept;
    bool private_commit_to_disk_unsafe(const nlohmann::json &snapshot,
                                       std::error_code *ec) noexcept;
    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j,
                                  std::error_code *ec) noexcept;

    // NEW: private helper to clear/set dirty without exposing Impl.
    // Must be implemented in JsonConfig.cpp (it will touch pImpl->dirty).
    void private_set_dirty_unsafe_(bool v) noexcept;

    // Transaction bookkeeping record (internal only)
    class Transaction
    {
      public:
        Transaction(const Transaction &) = delete;
        Transaction &operator=(const Transaction &) = delete;
        Transaction(Transaction &&) = delete;
        Transaction &operator=(Transaction &&) = delete;

        Transaction(JsonConfig *owner, TxId id, AccessFlags flags) noexcept
            : d_owner(owner), d_id(id), d_flags(flags), d_used(false)
        {
        }

        ~Transaction() = default;

      private:
        friend class JsonConfig;
        JsonConfig *d_owner;
        TxId d_id;
        AccessFlags d_flags;
        bool d_used;
    };

    // storage for outstanding transactions
    mutable std::mutex d_tx_mutex;
    std::list<std::unique_ptr<Transaction>> d_tx_list;
    std::unordered_map<TxId, std::list<std::unique_ptr<Transaction>>::iterator> d_tx_index;
    TxId d_next_txid = 1;

    // helpers
    Transaction *create_transaction_internal(AccessFlags flags, std::error_code *ec) noexcept;
    void destroy_transaction_internal(TxId id) noexcept;
    Transaction *find_transaction_locked(TxId id) noexcept;

    // NEW: consume helpers used by TransactionProxy (template, but must not touch Impl fields directly!)
    template <typename F>
    void consume_read_(TxId id, AccessFlags flags, F &&fn, std::error_code *ec,
                       std::source_location loc) noexcept
    {
        // 0) Basic connectivity check
        if (!is_initialized())
        {
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::not_connected);
            return;
        }

        // 1) Find tx and mark used under mutex
        Transaction *tx = nullptr;
        {
            std::lock_guard<std::mutex> lg(d_tx_mutex);
            tx = find_transaction_locked(id);
            if (!tx)
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::invalid_argument);
                return;
            }
            if (tx->d_used)
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::operation_not_permitted);
                return;
            }
            tx->d_used = true;
        }

        // 2) recursion guard
        if (basics::RecursionGuard::is_recursing(pImpl.get()))
        {
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
            return;
        }

        // 3) optional reload
        if ((static_cast<int>(flags) & static_cast<int>(AccessFlags::ReloadFirst)) != 0)
        {
            std::error_code reload_ec;
            if (!reload(&reload_ec))
            {
                destroy_transaction_internal(id);
                if (ec)
                    *ec = reload_ec;
                return;
            }
        }

        // 4) acquire read lock and call user
        std::error_code lock_ec;
        auto rlock_opt = lock_for_read(&lock_ec);
        if (!rlock_opt)
        {
            destroy_transaction_internal(id);
            if (ec)
                *ec = lock_ec;
            return;
        }

        try
        {
            fn(rlock_opt->json());
            if (ec)
                *ec = std::error_code{};
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("JsonConfig tx read lambda threw: {}, called at {}", ex.what(),
                         SRCLOC_TO_STR(loc));
            if (ec)
                *ec = std::make_error_code(std::errc::io_error);
        }
        catch (...)
        {
            LOGGER_ERROR("JsonConfig tx read lambda threw unknown exception, called at {}",
                         SRCLOC_TO_STR(loc));
            if (ec)
                *ec = std::make_error_code(std::errc::io_error);
        }

        // 5) always erase tx record
        destroy_transaction_internal(id);
    }

    template <typename F>
    void consume_write_(TxId id, AccessFlags flags, F &&fn, std::error_code *ec,
                        std::source_location loc) noexcept
    {
        if (!is_initialized())
        {
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::not_connected);
            return;
        }

        // 1) Find tx and mark used under mutex
        Transaction *tx = nullptr;
        {
            std::lock_guard<std::mutex> lg(d_tx_mutex);
            tx = find_transaction_locked(id);
            if (!tx)
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::invalid_argument);
                return;
            }
            if (tx->d_used)
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::operation_not_permitted);
                return;
            }
            tx->d_used = true;
        }

        // 2) recursion guard
        if (basics::RecursionGuard::is_recursing(pImpl.get()))
        {
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
            return;
        }

        // 3) acquire unique in-memory write lock
        std::error_code lock_ec;
        auto wlock_opt = lock_for_write(&lock_ec);
        if (!wlock_opt)
        {
            destroy_transaction_internal(id);
            if (ec)
                *ec = lock_ec;
            return;
        }
        auto wlock = std::move(*wlock_opt);

        // 4) If ReloadFirst: we need process lock (FileLock) + load-from-disk.
        // ABI-safe: do NOT access Impl fields here; use config_path() to get a copy.
        if ((static_cast<int>(flags) & static_cast<int>(AccessFlags::ReloadFirst)) != 0)
        {
            const auto path = config_path();
            if (path.empty())
            {
                destroy_transaction_internal(id);
                if (ec)
                    *ec = std::make_error_code(std::errc::not_connected);
                return;
            }

            FileLock fLock(path, ResourceType::File, LockMode::Blocking);
            if (!fLock.valid())
            {
                destroy_transaction_internal(id);
                if (ec)
                    *ec = fLock.error_code();
                return;
            }

            // We hold the unique data mutex via wlock, so it's safe to load into memory.
            if (!private_load_from_disk_unsafe(ec))
            {
                destroy_transaction_internal(id);
                return;
            }
        }

        // 5) Snapshot before change (ABI-safe: use wlock.json())
        nlohmann::json before;
        try
        {
            before = wlock.json();
        }
        catch (...)
        {
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::io_error);
            return;
        }

        // 6) Run user lambda
        try
        {
            fn(wlock.json());
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("JsonConfig tx write lambda threw: {}, called at {}", ex.what(),
                         SRCLOC_TO_STR(loc));
            // rollback (ABI-safe)
            try
            {
                wlock.json() = std::move(before);
                private_set_dirty_unsafe_(false);
            }
            catch (...)
            {
            }
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::io_error);
            return;
        }
        catch (...)
        {
            LOGGER_ERROR("JsonConfig tx write lambda threw unknown exception, called at {}",
                         SRCLOC_TO_STR(loc));
            try
            {
                wlock.json() = std::move(before);
                private_set_dirty_unsafe_(false);
            }
            catch (...)
            {
            }
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::io_error);
            return;
        }

        // 7) Validate JSON by dump (parse step is unnecessary; dump already traverses the structure).
        // If you *want* strict validation, keep parse; it can throw on invalid UTF-8 etc.
        try
        {
            (void)wlock.json().dump();
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("JsonConfig tx produced invalid JSON: {}, called at {}", ex.what(),
                         SRCLOC_TO_STR(loc));
            try
            {
                wlock.json() = std::move(before);
                private_set_dirty_unsafe_(false);
            }
            catch (...)
            {
            }
            destroy_transaction_internal(id);
            if (ec)
                *ec = std::make_error_code(std::errc::invalid_argument);
            return;
        }

        // 8) CommitAfter: snapshot and commit to disk (ABI-safe: snapshot via wlock.json())
        if ((static_cast<int>(flags) & static_cast<int>(AccessFlags::CommitAfter)) != 0)
        {
            nlohmann::json snapshot;
            try
            {
                snapshot = wlock.json();
            }
            catch (...)
            {
                try
                {
                    wlock.json() = std::move(before);
                    private_set_dirty_unsafe_(false);
                }
                catch (...)
                {
                }
                destroy_transaction_internal(id);
                if (ec)
                    *ec = std::make_error_code(std::errc::io_error);
                return;
            }

            // Release in-memory lock before slow disk I/O
            wlock = JsonConfig::WriteLock();

            if (!private_commit_to_disk_unsafe(snapshot, ec))
            {
                // keep in-memory change, report error
                destroy_transaction_internal(id);
                return;
            }
        }

        destroy_transaction_internal(id);
        if (ec)
            *ec = std::error_code{};
    }

  public:
    // Forward declare ReadLock/WriteLock to keep public API similar
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
        struct ImplInner;
        std::unique_ptr<ImplInner> d_;
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
        struct ImplInner;
        std::unique_ptr<ImplInner> d_;
        friend class JsonConfig;
    };
}; // JsonConfig class

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace pylabhub::utils
