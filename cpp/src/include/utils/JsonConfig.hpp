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
 *     - **API**: The `transaction().read()` and `transaction().write()` methods, which
 *       provide safe, scoped access via lambdas. They internally use the
 *       `lock_for_read()` and `lock_for_write()` factories.
 *       - `transaction().read()` acquires a *shared* lock, allowing multiple
 *         concurrent readers.
 *       - `transaction().write()` acquires a *unique* lock, ensuring exclusive
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

class JsonConfig;

class PYLABHUB_UTILS_EXPORT JsonConfigTransaction
{
  public:
    JsonConfigTransaction(JsonConfigTransaction &&) noexcept = default;
    JsonConfigTransaction &operator=(JsonConfigTransaction &&) noexcept = default;
    JsonConfigTransaction(const JsonConfigTransaction &) = delete;
    JsonConfigTransaction &operator=(const JsonConfigTransaction &) = delete;
    ~JsonConfigTransaction();

    template <typename F>
    void read(F &&fn, std::error_code *ec = nullptr) &&;

    template <typename F>
    void write(F &&fn, std::error_code *ec = nullptr) &&;

  private:
    friend class JsonConfig;
    explicit JsonConfigTransaction(JsonConfig *owner,
                                   typename JsonConfig::AccessFlags flags) noexcept;

    JsonConfig *d_owner;
    typename JsonConfig::AccessFlags d_flags;
};

class PYLABHUB_UTILS_EXPORT JsonConfig
{
    friend class JsonConfigTransaction;

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
     * destructor releases it. Prefer using the lambda-based `transaction().read()`
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
     * destructor releases it. Prefer using the lambda-based `transaction().write()`
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
     * @brief Flags to control the behavior of a transaction.
     */
    enum class AccessFlags
    {
        Default = 0,
        /**< Default behavior: read from memory, no automatic commit. */
        UnSynced = Default,
        /**< Alias for Default. Operation is on in-memory data only. */
        ReloadFirst = 1 << 0,
        /**< Reload data from disk before the operation. */
        CommitAfter = 1 << 1,
        /**< Commit data to disk after the operation (write access only). */
        FullSync = ReloadFirst | CommitAfter,
        /**< Reload before and commit after the operation. */
    };

    friend AccessFlags operator|(AccessFlags a, AccessFlags b)
    {
        return static_cast<AccessFlags>(static_cast<int>(a) | static_cast<int>(b));
    }

    /**
     * @brief Creates a transaction token for performing a read or write operation.
     * @param flags Flags to control the transaction's behavior (e.g., reload, commit).
     * @return A single-use transaction object.
     *
     * @example
     * @code
     *   cfg.transaction(JsonConfig::AccessFlags::FullSync).write([&](auto& j) {
     *       j["key"] = "value";
     *   });
     * @endcode
     */
    [[nodiscard]] JsonConfigTransaction transaction(AccessFlags flags = AccessFlags::Default) noexcept;

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

#include "utils/JsonConfigTransaction.inl"