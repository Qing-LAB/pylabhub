#pragma once

/**
 * @file JsonConfig.hpp
 * @brief Thread-safe and process-safe JSON configuration manager.
 *
 * This variant implements JsonConfig-owned Transaction objects.
 */

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <type_traits>
#include <utility>
#include <map>
#include <list>
#include <mutex>
#include <source_location>
#include <system_error>
#include <string>

#include "debug_info.hpp"
#include "nlohmann/json.hpp"
#include "recursion_guard.hpp"
#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"
#include "utils/Lifecycle.hpp"

#include "pylabhub_utils_export.h"

namespace pylabhub::utils
{

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class JsonConfig
{
  private:
    class Transaction;
  
  public:
    class ReadLock;
    class WriteLock;

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

    // ---------------- Transaction ownership model ----------------
    // Transactions are created and stored inside JsonConfig. The returned reference
    // is valid until the transaction is destroyed (either used or the JsonConfig is destroyed).
    using TxId = uint64_t;

    // Create and register a new transaction object inside this JsonConfig.
    // Returns a reference to the stored Transaction. The reference is valid until
    // the transaction is used (read/write) or explicitly released/destroyed.
    // On failure returns nullptr.
    // Note: the returned reference must be used immediately (pattern: cfg.transaction().read(...))
    Transaction &transaction(AccessFlags flags = AccessFlags::Default,
                             std::error_code *ec = nullptr);

    // Manually release a transaction by TxId (rarely needed; transaction is auto-removed on use).
    bool release_transaction(TxId id, std::error_code *ec = nullptr) noexcept;

    // Manual locking API (unchanged)
    std::optional<ReadLock> lock_for_read(std::error_code *ec) const noexcept;
    std::optional<WriteLock> lock_for_write(std::error_code *ec) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    // Internal helpers moved to private so Transaction can call them using owner pointer
    bool private_load_from_disk_unsafe(std::error_code *ec) noexcept;
    bool private_commit_to_disk_unsafe(const nlohmann::json &snapshot, std::error_code *ec) noexcept;
    static void atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j,
                                  std::error_code *ec) noexcept;

    // Transaction storage
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

        // read and write are template methods and defined inline below.
        template <typename F>
        void read(F &&fn, std::error_code *ec = nullptr, std::source_location loc = std::source_location::current());

        template <typename F>
        void write(F &&fn, std::error_code *ec = nullptr, std::source_location loc = std::source_location::current());

      private:
        friend class JsonConfig;        

        JsonConfig *d_owner;
        TxId d_id;
        AccessFlags d_flags;
        bool d_used;
    };

    // storage for outstanding transactions
    mutable std::mutex d_tx_mutex;
    // stable storage: list keeps iterators/pointers valid across inserts/erases (except erasing that element)
    std::list<std::unique_ptr<Transaction>> d_tx_list;
    // index: TxId -> iterator into d_tx_list
    std::unordered_map<TxId, std::list<std::unique_ptr<Transaction>>::iterator> d_tx_index;

    TxId d_next_txid = 1;

    // helpers
    Transaction *create_transaction_internal(AccessFlags flags, std::error_code *ec) noexcept;
    void destroy_transaction_internal(TxId id) noexcept;
    Transaction *find_transaction_locked(TxId id) noexcept;

    // Factory helpers for ReadLock/WriteLock (same as before)
    // (declare ReadLock/WriteLock here and define them later or keep as before)
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

// Inline template definitions for Transaction::read / write

template <typename F>
void JsonConfig::Transaction::read(F &&fn, std::error_code *ec, std::source_location loc)
{
    // Single-use semantics: check used flag, and mark used at the end (transaction removed)
    if (!d_owner)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return;
    }

    // Quick check to prevent reuse
    {
        std::lock_guard<std::mutex> lg(d_owner->d_tx_mutex);
        if (d_used)
        {
            if (ec) *ec = std::make_error_code(std::errc::operation_not_permitted);
            return;
        }
        // mark used to prevent races if another thread tries to use same Tx concurrently
        d_used = true;
    }

    // Ensure transaction is erased at the end regardless of outcome
    // Capture id and owner pointer for erase
    TxId id = d_id;
    JsonConfig *owner = d_owner;

    // Recursion guard
    if (basics::RecursionGuard::is_recursing(owner->pImpl.get()))
    {
        // cleanup transaction entry
        owner->destroy_transaction_internal(id);
        if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        return;
    }

    // Optionally reload first (calls owner->reload which itself uses file lock)
    if (static_cast<int>(d_flags) & static_cast<int>(AccessFlags::ReloadFirst))
    {
        std::error_code reload_ec;
        if (!owner->reload(&reload_ec))
        {
            owner->destroy_transaction_internal(id);
            if (ec) *ec = reload_ec;
            return;
        }
    }

    // Acquire read lock via factory
    std::error_code lock_ec;
    auto rlock_opt = owner->lock_for_read(&lock_ec);
    if (!rlock_opt)
    {
        owner->destroy_transaction_internal(id);
        if (ec) *ec = lock_ec;
        return;
    }

    // Execute user lambda while holding read lock
    {
        auto rlock = std::move(*rlock_opt);
        try
        {
            fn(rlock.json());
            if (ec) *ec = std::error_code{};
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("JsonConfig transaction read lambda threw: {}, called at {}", ex.what(), SRCLOC_TO_STR(loc));
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            owner->destroy_transaction_internal(id);
            return;
        }
        catch (...)
        {
            LOGGER_ERROR("JsonConfig transaction read lambda threw unknown exception, called at {}", SRCLOC_TO_STR(loc));
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            owner->destroy_transaction_internal(id);
            return;
        }
    }

    // Destroy transaction entry now that it's been used
    owner->destroy_transaction_internal(id);
}

template <typename F>
void JsonConfig::Transaction::write(F &&fn, std::error_code *ec, std::source_location loc)
{
    if (!d_owner)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return;
    }

    // Prevent reuse
    {
        std::lock_guard<std::mutex> lg(d_owner->d_tx_mutex);
        if (d_used)
        {
            if (ec) *ec = std::make_error_code(std::errc::operation_not_permitted);
            return;
        }
        d_used = true;
    }

    TxId id = d_id;
    JsonConfig *owner = d_owner;

    // Recursion guard
    if (basics::RecursionGuard::is_recursing(owner->pImpl.get()))
    {
        owner->destroy_transaction_internal(id);
        if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        return;
    }

    // Acquire unique write lock
    std::error_code lock_ec;
    auto wlock_opt = owner->lock_for_write(&lock_ec);
    if (!wlock_opt)
    {
        owner->destroy_transaction_internal(id);
        if (ec) *ec = lock_ec;
        return;
    }
    auto wlock = std::move(*wlock_opt);

    // If ReloadFirst, perform reload under unique lock to avoid races
    if (static_cast<int>(d_flags) & static_cast<int>(AccessFlags::ReloadFirst))
    {
        FileLock fLock(owner->pImpl->configPath, ResourceType::File, LockMode::Blocking);
        if (!fLock.valid())
        {
            owner->destroy_transaction_internal(id);
            if (ec) *ec = fLock.error_code();
            return;
        }
        if (!owner->private_load_from_disk_unsafe(ec))
        {
            owner->destroy_transaction_internal(id);
            return;
        }
    }

    // Snapshot before change
    nlohmann::json before;
    try
    {
        before = wlock.json(); // copy while holding unique lock
    }
    catch (...)
    {
        owner->destroy_transaction_internal(id);
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return;
    }

    // Run user-provided lambda safely. WriteLock::json() sets dirty flag.
    bool user_ok = false;
    try
    {
        fn(wlock.json());
        user_ok = true;
    }
    catch (const std::exception &ex)
    {
        LOGGER_ERROR("JsonConfig transaction write lambda threw: {}, called at {}", ex.what(), SRCLOC_TO_STR(loc));
        // rollback
        owner->pImpl->data = std::move(before);
        owner->pImpl->dirty.store(false, std::memory_order_release);
        owner->destroy_transaction_internal(id);
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig transaction write lambda threw unknown exception, called at {}", SRCLOC_TO_STR(loc));
        owner->pImpl->data = std::move(before);
        owner->pImpl->dirty.store(false, std::memory_order_release);
        owner->destroy_transaction_internal(id);
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return;
    }

    // Validate JSON by round-trip dump/parse
    if (user_ok)
    {
        try
        {
            std::string s = wlock.json().dump();
            (void)nlohmann::json::parse(s);
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("JsonConfig transaction produced invalid JSON: {}, called at {}", ex.what(), SRCLOC_TO_STR(loc));
            owner->pImpl->data = std::move(before);
            owner->pImpl->dirty.store(false, std::memory_order_release);
            owner->destroy_transaction_internal(id);
            if (ec) *ec = std::make_error_code(std::errc::invalid_argument);
            return;
        }
    }

    // Handle commit if requested: snapshot, release lock, commit to disk
    if (static_cast<int>(d_flags) & static_cast<int>(AccessFlags::CommitAfter))
    {
        nlohmann::json snapshot;
        try
        {
            snapshot = wlock.json();
        }
        catch (...)
        {
            // rollback
            owner->pImpl->data = std::move(before);
            owner->pImpl->dirty.store(false, std::memory_order_release);
            owner->destroy_transaction_internal(id);
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            return;
        }

        // release the unique lock
        wlock = JsonConfig::WriteLock();

        // commit snapshot to disk (acquires file lock internally)
        if (!owner->private_commit_to_disk_unsafe(snapshot, ec))
        {
            // keep in-memory change (dirty), report error
            owner->destroy_transaction_internal(id);
            return;
        }
    }

    // success
    owner->destroy_transaction_internal(id);
    if (ec) *ec = std::error_code{};
}

} // namespace pylabhub::utils

