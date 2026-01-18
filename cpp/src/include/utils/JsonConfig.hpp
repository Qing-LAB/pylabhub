#pragma once

/**
 * @file JsonConfig.hpp
 * @brief Thread-safe and process-safe JSON configuration manager.
 *
 * Provides a robust class for managing JSON configuration files with guarantees
 * for thread-safety (for in-memory access) and process-safety (for file I/O).
 *
 * Key features include:
 * - **Atomic File Writes**: Uses a temporary file and atomic rename/replace to
 *   prevent file corruption, even if the application crashes during a write.
 * - **Process-Level Locking**: Employs `pylabhub::utils::FileLock` to ensure
 *   that only one process can write to the configuration file at a time.
 * - **Thread-Safe In-Memory Cache**: Uses a `std::shared_mutex` to allow
 *   concurrent reads and exclusive writes to the in-memory JSON object.
 * - **Transactional API**: A modern, fluent API using an rvalue-proxy pattern
 *   (`TransactionProxy`) ensures that read/write operations are clear, concise,
 *   and safe from common lifecycle errors.
 * - **Recursion Protection**: Prevents deadlocks by disallowing nested
 *   transactions within the same `JsonConfig` object.
 *
 * This variant implements JsonConfig-owned Transaction objects, but exposes
 * a rvalue-only TransactionProxy to enforce immediate-use at compile time:
 *
 * @code
 *   cfg.transaction(flags).read(...);
 *   cfg.transaction(flags).write(...);
 * @endcode
 *
 * The proxy's read/write are &&-qualified, so the following will not compile:
 * @code
 *   auto t = cfg.transaction(); // ERROR: proxy is an rvalue
 *   t.read(...);
 * @endcode
 *
 * @note This header uses the PImpl idiom, so implementation details are not
 * exposed. All methods that require access to internal state are defined in
 * the corresponding `.cpp` file.
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

/**
 * @class JsonConfig
 * @brief Manages thread-safe and process-safe access to a JSON configuration file.
 *
 * This class provides a high-level interface for reading and writing structured
 * data to a JSON file on disk. It handles the complexities of concurrent access
 * from multiple threads or processes, ensuring data integrity.
 */
class PYLABHUB_UTILS_EXPORT JsonConfig
{
  private:
    class Transaction; // internal bookkeeping record (not user-facing)

  public:
    class ReadLock;
    class WriteLock;

    /**
     * @enum AccessFlags
     * @brief Flags to control the behavior of a transaction.
     *
     * These flags can be combined using the `|` operator to specify how a
     * transaction should interact with the in-memory cache and the on-disk file.
     */
    enum class AccessFlags
    {
        /**
         * @brief Default behavior. Operates on the current in-memory cache without
         * synchronizing with the disk.
         */
        Default = 0,
        /**
         * @brief Alias for `Default`. The transaction is not synchronized with the disk.
         */
        UnSynced = Default,
        /**
         * @brief Reload the configuration from disk *before* executing the transaction.
         * This ensures the operation uses the most up-to-date file content.
         * A process-level file lock is held during this operation.
         */
        ReloadFirst = 1 << 0,
        /**
         * @brief Commit the changes to disk *after* the write transaction successfully completes.
         * This operation is atomic. A process-level file lock is held.
         * This flag is ignored by read transactions.
         */
        CommitAfter = 1 << 1,
        /**
         * @brief Combination of `ReloadFirst` and `CommitAfter`.
         * Guarantees a full, atomic read-modify-write cycle.
         */
        FullSync = ReloadFirst | CommitAfter,
    };

    friend AccessFlags operator|(AccessFlags a, AccessFlags b)
    {
        return static_cast<AccessFlags>(static_cast<int>(a) | static_cast<int>(b));
    }

    /**
     * @enum CommitDecision
     * @brief Return type for a write lambda to control whether a commit occurs.
     *
     * If a write transaction is initiated with the `CommitAfter` flag, the
     * lambda can return this enum to either proceed with or veto the commit.
     */
    enum class CommitDecision
    {
        /**
         * @brief Proceed with the disk commit if the `CommitAfter` flag is set.
         */
        Commit,
        /**
         * @brief Veto the disk commit, even if the `CommitAfter` flag is set.
         * The changes will remain in-memory and the object will be marked as dirty.
         */
        SkipCommit
    };


    /**
     * @class TransactionProxy
     * @brief A short-lived, rvalue-only proxy to execute a read or write transaction.
     *
     * This class is the heart of the fluent transaction API. It is returned by
     * `JsonConfig::transaction()` and is designed to be used immediately. Its
     * methods (`read`/`write`) are `&&-qualified`, meaning they can only be called
     * on a temporary (rvalue) object. This prevents the proxy from being stored
     * in a variable, which could lead to resource leaks or confusing lifetime issues.
     *
     * @warning In debug builds, if a `TransactionProxy` is created and destroyed
     * without `read()` or `write()` being called, a warning is logged to stderr.
     */
    class TransactionProxy
    {
      public:
        TransactionProxy() = delete;
        TransactionProxy(const TransactionProxy &) = delete;
        TransactionProxy &operator=(const TransactionProxy &) = delete;

        /**
         * @brief Move constructor.
         * @param other The proxy to move from. The moved-from proxy becomes invalid.
         */
        TransactionProxy(TransactionProxy &&other) noexcept
            : owner_(other.owner_), id_(other.id_), flags_(other.flags_), consumed_(other.consumed_)
        {
            other.owner_ = nullptr;
            other.id_ = 0;
            other.consumed_ = true;
        }

        TransactionProxy &operator=(TransactionProxy &&) = delete;

        /**
         * @brief Destructor. In debug builds, warns if the proxy was not consumed.
         */
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

        /**
         * @brief Executes a read-only transaction.
         *
         * Acquires a shared (read) lock, executes the provided lambda, and releases
         * the lock. The lambda receives a `const nlohmann::json&`.
         *
         * @tparam F A callable type with the signature `void(const nlohmann::json& j)`.
         * @param fn The lambda or function to execute.
         * @param ec Optional. A `std::error_code` to receive any errors.
         * @param loc Source location for improved debugging. Do not specify manually.
         */
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

        /**
         * @brief Executes a read-write transaction.
         *
         * Acquires an exclusive (write) lock, executes the provided lambda, and
         * releases the lock. The lambda receives a `nlohmann::json&` which can be modified.
         * If the lambda throws an exception, all changes are rolled back.
         *
         * @tparam F A callable type with the signature `void(nlohmann::json& j)` or
         *           `CommitDecision(nlohmann::json& j)`.
         * @param fn The lambda or function to execute.
         * @param ec Optional. A `std::error_code` to receive any errors.
         * @param loc Source location for improved debugging. Do not specify manually.
         */
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
    /**
     * @brief Default constructor. Creates an uninitialized `JsonConfig` object.
     * @note `init()` must be called before the object can be used.
     * @warning Throws a fatal error if the `JsonConfig` lifecycle module has not
     *          been initialized.
     */
    JsonConfig() noexcept;

    /**
     * @brief Constructs and initializes a `JsonConfig` object.
     * @param configFile The path to the JSON configuration file.
     * @param createIfMissing If `true`, the file will be created with an empty
     *        JSON object (`{}`) if it does not exist.
     * @param ec Optional. A `std::error_code` to receive any errors during
     *        initialization or the initial file load.
     * @warning Throws a fatal error if the `JsonConfig` lifecycle module has not
     *          been initialized.
     */
    explicit JsonConfig(const std::filesystem::path &configFile, bool createIfMissing = false,
                        std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Destructor.
     */
    ~JsonConfig();

    JsonConfig(const JsonConfig &) = delete;
    JsonConfig &operator=(const JsonConfig &) = delete;

    /**
     * @brief Move constructor.
     * Transfers ownership of the configuration file and its in-memory state.
     * The moved-from object becomes uninitialized.
     * @param other The `JsonConfig` object to move from.
     */
    JsonConfig(JsonConfig &&) noexcept;

    /**
     * @brief Move assignment operator.
     * Transfers ownership of the configuration file and its in-memory state.
     * The moved-from object becomes uninitialized.
     * @param other The `JsonConfig` object to move from.
     * @return A reference to this object.
     */
    JsonConfig &operator=(JsonConfig &&) noexcept;

    /**
     * @brief Gets the lifecycle module definition for `JsonConfig`.
     * @return A `ModuleDef` object that can be registered with the `LifecycleManager`.
     */
    static ModuleDef GetLifecycleModule();

    /**
     * @brief Checks if the `JsonConfig` lifecycle module has been initialized globally.
     * @return `true` if the module is active, `false` otherwise.
     */
    static bool lifecycle_initialized() noexcept;

    /**
     * @brief Checks if the `JsonConfig` object is associated with a file path.
     * @return `true` if `init()` has been successfully called, `false` otherwise.
     */
    bool is_initialized() const noexcept;

    /**
     * @brief Alias for `is_initialized()`.
     * @return `true` if `init()` has been successfully called, `false` otherwise.
     */
    bool has_path() const noexcept;

    /**
     * @brief Checks if the in-memory data has changed since the last load or save.
     * @return `true` if there are uncommitted changes in memory, `false` otherwise.
     */
    bool is_dirty() const noexcept;

    /**
     * @brief Initializes the `JsonConfig` object with a file path.
     *
     * This function must be called on a default-constructed object before it can be used.
     * It is safe to call multiple times, but it is not thread-safe.
     *
     * @param configFile The path to the JSON configuration file.
     * @param createIfMissing If `true`, the file will be created with an empty
     *        JSON object (`{}`) if it does not exist.
     * @param ec Optional. A `std::error_code` to receive any errors.
     * @return `true` on success, `false` on failure.
     */
    bool init(const std::filesystem::path &configFile, bool createIfMissing = false,
              std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Discards in-memory changes and reloads the configuration from disk.
     *
     * This operation is thread-safe and process-safe. It acquires an exclusive
     * lock on the in-memory data and a process lock on the file.
     *
     * @param ec Optional. A `std::error_code` to receive any errors.
     * @return `true` on success, `false` on failure.
     */
    bool reload(std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Forcibly writes the current in-memory state to the disk.
     *
     * This is useful if you want to save the current state regardless of whether
     * it is considered "dirty". The operation is atomic and process-safe.
     * After a successful overwrite, the object is no longer considered dirty.
     *
     * @param ec Optional. A `std::error_code` to receive any errors.
     * @return `true` on success, `false` on failure.
     */
    bool overwrite(std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Gets the path to the configuration file.
     * @return The `std::filesystem::path` if initialized, or an empty path otherwise.
     */
    std::filesystem::path config_path() const noexcept;

    /// @brief Type alias for a transaction ID.
    using TxId = uint64_t;

    /**
     * @brief Begins a transaction, returning a temporary proxy object.
     *
     * This is the main entry point for all read and write operations.
     *
     * @code
     *   // Simple read
     *   cfg.transaction().read([](const json& j){ ... });
     *
     *   // Atomic read-modify-write
     *   cfg.transaction(JsonConfig::AccessFlags::FullSync).write([](json& j){ ... });
     * @endcode
     *
     * @param flags Optional flags to control the transaction's behavior (e.g., reloading, committing).
     * @param ec Optional. A `std::error_code` to receive any errors.
     * @return A `TransactionProxy` object. This is an rvalue and must be consumed immediately.
     */
    [[nodiscard]] TransactionProxy transaction(AccessFlags flags = AccessFlags::Default,
                                               std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Manually releases a transaction record.
     * @deprecated This is a remnant of a previous design and is generally not
     * needed with the new `TransactionProxy` API. It is maintained for
     * binary compatibility but has no practical use.
     * @param id The ID of the transaction to release.
     * @param ec Optional. A `std::error_code` to receive any errors.
     * @return `true` on success, `false` if the ID is not found.
     */
    bool release_transaction(TxId id, std::error_code *ec = nullptr) noexcept;

    /**
     * @brief Acquires a manual shared (read) lock.
     *
     * This is an alternative to the transaction API for cases where the lock
     * needs to be held across a wider scope. The returned `ReadLock` object
     * holds the lock for its lifetime.
     *
     * @param ec Optional. A `std::error_code` to receive any errors.
     * @return An `std::optional<ReadLock>` containing the lock if successful.
     */
    std::optional<ReadLock> lock_for_read(std::error_code *ec) const noexcept;

    /**
     * @brief Acquires a manual exclusive (write) lock.
     *
     * This is an alternative to the transaction API. The returned `WriteLock`
     * object holds the lock for its lifetime. Any access through the lock's
     * `json()` method will mark the `JsonConfig` object as dirty.
     *
     * @param ec Optional. A `std::error_code` to receive any errors.
     * @return An `std::optional<WriteLock>` containing the lock if successful.
     */
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
        static_assert(std::is_invocable_v<F, nlohmann::json &>,
                      "JsonConfig::write() lambda must be callable with signature (nlohmann::json&).");

        using R = std::invoke_result_t<F, nlohmann::json &>;

        static_assert(std::is_void_v<R> || std::is_same_v<R, CommitDecision>,
                      "JsonConfig::write() lambda must return either void or JsonConfig::CommitDecision");

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

        // 3) Hold a single process-level lock for the entire transaction if any disk I/O is needed.
        std::optional<FileLock> fLock;
        const bool needs_disk_io =
            (static_cast<int>(flags) & (static_cast<int>(AccessFlags::ReloadFirst) |
                                        static_cast<int>(AccessFlags::CommitAfter))) != 0;

        if (needs_disk_io)
        {
            const auto path = config_path();
            if (path.empty())
            {
                destroy_transaction_internal(id);
                if (ec)
                    *ec = std::make_error_code(std::errc::not_connected);
                return;
            }
            fLock.emplace(path, ResourceType::File, LockMode::Blocking);
            if (!fLock->valid())
            {
                destroy_transaction_internal(id);
                if (ec)
                    *ec = fLock->error_code();
                return;
            }
        }

        // 4) acquire unique in-memory write lock
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

        // 5) If ReloadFirst: load from disk (we already have the process lock)
        if ((static_cast<int>(flags) & static_cast<int>(AccessFlags::ReloadFirst)) != 0)
        {
            // We hold the unique data mutex via wlock and the process lock via fLock,
            // so it's safe to load into memory.
            if (!private_load_from_disk_unsafe(ec))
            {
                destroy_transaction_internal(id);
                return; // private_load_from_disk_unsafe sets ec
            }
        }

        // 6) Snapshot before change (ABI-safe: use wlock.json())
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

        // 7) Run user lambda
        CommitDecision commit_decision = CommitDecision::Commit;
        try
        {
            if constexpr (std::is_void_v<R>)
            {
                fn(wlock.json());
            }
            else
            {
                commit_decision = fn(wlock.json());
            }
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

        // 8) Validate JSON by dump
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

        // 9) CommitAfter: snapshot and commit to disk (we already have the process lock)
        if ((static_cast<int>(flags) & static_cast<int>(AccessFlags::CommitAfter)) != 0)
        {
            if (commit_decision == CommitDecision::Commit)
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

                // Release in-memory lock before slow disk I/O. The process lock (fLock) is still held.
                wlock = JsonConfig::WriteLock();

                // Directly call atomic_write_json since we hold the lock.
                // This avoids calling private_commit_to_disk_unsafe which tries to take its own lock.
                // We use fLock->path() because it is guaranteed to be valid and holds the actual path.
                atomic_write_json(fLock->get_locked_resource_path().value(), snapshot, ec);
                if (ec && *ec)
                {
                    // keep in-memory change, report error
                    destroy_transaction_internal(id);
                    return;
                }
                private_set_dirty_unsafe_(false); // only set dirty to false on success
            }
        }

        destroy_transaction_internal(id);
        if (ec)
            *ec = std::error_code{};
    }

  public:
    /**
     * @class ReadLock
     * @brief A RAII object that holds a shared (read) lock on the in-memory data.
     *
     * Obtained via `JsonConfig::lock_for_read()`. The lock is released when
     * the `ReadLock` object is destroyed.
     */
    class PYLABHUB_UTILS_EXPORT ReadLock
    {
      public:
        ReadLock() noexcept;
        ReadLock(ReadLock &&) noexcept;
        ReadLock &operator=(ReadLock &&) noexcept;
        ~ReadLock();

        /**
         * @brief Provides const access to the underlying JSON data.
         * @return A const reference to the `nlohmann::json` object.
         */
        const nlohmann::json &json() const noexcept;
        ReadLock(const ReadLock &) = delete;
        ReadLock &operator=(const ReadLock &) = delete;

      private:
        struct ImplInner;
        std::unique_ptr<ImplInner> d_;
        friend class JsonConfig;
    };

    /**
     * @class WriteLock
     * @brief A RAII object that holds an exclusive (write) lock on the in-memory data.
     *
     * Obtained via `JsonConfig::lock_for_write()`. The lock is released when
     * the `WriteLock` object is destroyed.
     */
    class PYLABHUB_UTILS_EXPORT WriteLock
    {
      public:
        WriteLock() noexcept;
        WriteLock(WriteLock &&) noexcept;
        WriteLock &operator=(WriteLock &&) noexcept;
        ~WriteLock();

        /**
         * @brief Provides mutable access to the underlying JSON data.
         * @note Accessing the JSON object via this method will mark the parent
         *       `JsonConfig` object as dirty.
         * @return A reference to the `nlohmann::json` object.
         */
        nlohmann::json &json() noexcept;

        /**
         * @brief Manually commits the current in-memory state to disk.
         *
         * This performs an atomic, process-safe write. The write lock is
         * released *before* the slow I/O operation begins to improve concurrency.
         *
         * @param ec Optional. A `std::error_code` to receive any errors.
         * @return `true` on success, `false` on failure.
         */
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
