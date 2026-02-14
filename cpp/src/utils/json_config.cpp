/**
 * @file json_config.cpp
 * @brief Implements the thread-safe and process-safe JSON configuration manager.
 */
#include "plh_service.hpp"

#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>

#if defined(PYLABHUB_IS_POSIX)
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "utils/json_config.hpp"

namespace pylabhub::utils
{
namespace fs = std::filesystem;

// Module-level flag to indicate if the JsonConfig has been initialized.
static std::atomic<bool> g_jsonconfig_initialized{false};

/**
 * @struct JsonConfig::Impl
 * @brief The private implementation (PImpl) of the JsonConfig class.
 *
 * This struct holds all the internal state for a `JsonConfig` object,
 * including the file path, the in-memory JSON data, and synchronization
 * primitives. This keeps the public header file clean and ABI-stable.
 */
struct JsonConfig::Impl
{
    /// @brief The full path to the configuration file.
    std::filesystem::path configPath;
    /// @brief The in-memory cache of the JSON data.
    nlohmann::json data = nlohmann::json::object();
    /// @brief A mutex for thread-safe access to the in-memory `data`.
    mutable std::shared_mutex dataMutex;
    /// @brief A mutex to protect the initialization process (`configPath`).
    std::mutex initMutex;
    /**
     * @brief A flag indicating if the in-memory `data` has been modified
     * since the last successful load from or commit to disk.
     */
    std::atomic<bool> dirty{false};
    Impl() = default;
    ~Impl() = default;
};

// ReadLock/WriteLock impls (unchanged)
struct JsonConfig::ReadLock::ImplInner
{
    JsonConfig *owner = nullptr;
    std::shared_lock<std::shared_mutex> lock;
    std::optional<basics::RecursionGuard> guard;
};

struct JsonConfig::WriteLock::ImplInner
{
    JsonConfig *owner = nullptr;
    std::unique_lock<std::shared_mutex> lock;
    std::optional<basics::RecursionGuard> guard;
};

// ----------------- JsonConfig public methods -----------------

JsonConfig::JsonConfig() noexcept : pImpl(std::make_unique<Impl>())
{
    if (!lifecycle_initialized())
    {
        PLH_PANIC(
            "JsonConfig created before its module was initialized via LifecycleManager. Aborting.");
    }
}

JsonConfig::JsonConfig(const std::filesystem::path &configFile, bool createIfMissing,
                       std::error_code *err_code) noexcept
    : pImpl(std::make_unique<Impl>())
{
    if (!lifecycle_initialized())
    {
        PLH_PANIC(
            "JsonConfig created before its module was initialized via LifecycleManager. Aborting.");
    }
    if (!init(configFile, createIfMissing, err_code))
    {
        // init populates err_code; ctor remains noexcept
    }
}

JsonConfig::~JsonConfig()
{
    // IMPORTANT: we must not leak internal tx records.
    // This does NOT make holding a proxy across JsonConfig lifetime safe;
    // it only cleans bookkeeping.
    std::lock_guard<std::mutex> tx_lock(d_tx_mutex);
    d_tx_index.clear();
    d_tx_list.clear();
}

JsonConfig::JsonConfig(JsonConfig &&other) noexcept : pImpl(std::move(other.pImpl))
{
    // Design note:
    // If you keep move enabled, moving a JsonConfig with outstanding transactions is dangerous.
    // Minimal safe policy: require "no outstanding tx" on move, otherwise PANIC in debug.
    {
        std::lock_guard<std::mutex> tx_lock(other.d_tx_mutex);
        if (!other.d_tx_list.empty())
        {
            LOGGER_ERROR(
                "JsonConfig move-ctor with outstanding transactions. This is unsafe by contract.");
            // You can PLH_PANIC here if you want it strict.
        }
        // We intentionally do NOT transfer tx records; moved-from object becomes empty.
        other.d_tx_index.clear();
        other.d_tx_list.clear();
    }
}

JsonConfig &JsonConfig::operator=(JsonConfig &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    {
        std::lock_guard<std::mutex> tx_lock(d_tx_mutex);
        d_tx_index.clear();
        d_tx_list.clear();
    }

    pImpl = std::move(other.pImpl);

    {
        std::lock_guard<std::mutex> tx_lock(other.d_tx_mutex);
        if (!other.d_tx_list.empty())
        {
            LOGGER_ERROR("JsonConfig move-assign with outstanding transactions. This is unsafe by "
                         "contract.");
            // Optionally PLH_PANIC.
        }
        other.d_tx_index.clear();
        other.d_tx_list.clear();
    }

    return *this;
}

std::filesystem::path JsonConfig::config_path() const noexcept
{
    if (!pImpl)
    {
        return {};
    }
    std::lock_guard<std::mutex> init_guard(pImpl->initMutex);
    return std::filesystem::path{pImpl->configPath};
}

bool JsonConfig::is_initialized() const noexcept
{
    if (!pImpl)
    {
        return false;
    }
    std::lock_guard<std::mutex> init_guard(pImpl->initMutex);
    return !pImpl->configPath.empty();
}

bool JsonConfig::has_path() const noexcept
{
    return is_initialized();
}

bool JsonConfig::is_dirty() const noexcept
{
    if (!is_initialized())
    {
        return false;
    }
    return pImpl->dirty.load(std::memory_order_acquire);
}

// New private helper requested by header-only txn logic to avoid touching Impl in headers.
void JsonConfig::private_set_dirty_unsafe_(bool value) noexcept
{
    if (!pImpl)
    {
        return;
    }
    pImpl->dirty.store(value, std::memory_order_release);
}

bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing,
                      std::error_code *err_code) noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> init_guard(pImpl->initMutex);

            // Security: refuse to operate on a path that is a symbolic link.
            // This prevents attacks where the config file is replaced with a symlink
            // to overwrite a sensitive system file.
            if (fs::is_symlink(configFile))
            {
                if (err_code != nullptr)
                {
                    *err_code = std::make_error_code(std::errc::operation_not_permitted);
                }
                LOGGER_ERROR(
                    "JsonConfig::init: target '{}' is a symbolic link, refusing to initialize.",
                    configFile.string());
                return false;
            }

            pImpl->configPath = configFile;

            if (createIfMissing)
            {
                std::error_code lfs;
                if (!fs::exists(configFile, lfs))
                {
                    // If the file doesn't exist, create it with an empty JSON object.
                    // This is done atomically.
                    nlohmann::json empty = nlohmann::json::object();
                    atomic_write_json(configFile, empty, err_code);
                    if (err_code != nullptr && static_cast<bool>(*err_code))
                    {
                        return false;
                    }
                }
            }
        }
        // After setting the path, reload the contents from disk.
        return reload(err_code);
    }
    catch (const std::exception &ex)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        LOGGER_ERROR("JsonConfig::init: exception during init: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
}

/**
 * @brief Internal helper to load data from disk.
 * @note This function is not thread-safe or process-safe by itself. The caller
 *       *must* hold the appropriate locks (`dataMutex` and `FileLock`) before calling.
 */
bool JsonConfig::private_load_from_disk_unsafe(std::error_code *err_code) noexcept
{
    try
    {
        std::ifstream input_stream(pImpl->configPath);
        if (!input_stream.is_open())
        {
            // If the file doesn't exist or can't be opened, treat it as an empty
            // JSON object. This is a design choice to allow for optional config files.
            pImpl->data = nlohmann::json::object();
            pImpl->dirty.store(false, std::memory_order_release);
            if (err_code != nullptr)
            {
                *err_code = std::error_code{};
            }
            return true;
        }

        nlohmann::json newdata;
        input_stream >> newdata;

        pImpl->data = std::move(newdata);
        pImpl->dirty.store(false, std::memory_order_release);

        if (err_code != nullptr)
        {
            *err_code = std::error_code{};
        }
        return true;
    }
    catch (const std::exception &ex)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        LOGGER_ERROR(
            "JsonConfig::private_load_from_disk_unsafe: JSON parse error or I/O failure: {}",
            ex.what());
        return false;
    }
    catch (...)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
}

bool JsonConfig::reload(std::error_code *err_code) noexcept
{
    try
    {
        if (!pImpl)
        {
            if (err_code != nullptr)
            {
                *err_code = std::make_error_code(std::errc::not_connected);
            }
            return false;
        }

        // 1) Acquire an exclusive lock on the in-memory data, as we are about to replace it.
        std::unique_lock<std::shared_mutex> data_lock(pImpl->dataMutex);

        // 2) Acquire a process-level lock for safe disk access.
        FileLock fLock(pImpl->configPath, ResourceType::File, LockMode::Blocking);
        if (!fLock.valid())
        {
            if (err_code != nullptr)
            {
                *err_code = fLock.error_code();
            }
            return false;
        }

        // 3) With both locks held, it's safe to load from disk.
        return private_load_from_disk_unsafe(err_code);
    }
    catch (...)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
}

/**
 * @brief Internal helper to commit a snapshot of JSON data to disk.
 * @note This function is not thread-safe by itself. The caller *must* ensure
 *       that no other threads are modifying the in-memory data while the snapshot
 *       is being taken.
 */
bool JsonConfig::private_commit_to_disk_unsafe(const nlohmann::json &snapshot,
                                               std::error_code *err_code) noexcept
{
    try
    {
        // The file lock ensures that no other process can write to the file
        // at the same time.
        FileLock file_lock(pImpl->configPath, ResourceType::File, LockMode::Blocking);
        if (!file_lock.valid())
        {
            if (err_code != nullptr)
            {
                *err_code = file_lock.error_code();
            }
            return false;
        }

        atomic_write_json(pImpl->configPath, snapshot, err_code);
        if (err_code != nullptr && static_cast<bool>(*err_code))
        {
            return false;
        }

        // Only mark as clean after a successful write.
        pImpl->dirty.store(false, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
}

bool JsonConfig::overwrite(std::error_code *err_code) noexcept
{
    if (!pImpl)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::not_connected);
        }
        return false;
    }

    try
    {
        nlohmann::json snapshot;
        {
            // Acquire a read lock to safely create a snapshot of the current
            // in-memory state.
            if (auto read_guard = lock_for_read(err_code))
            {
                snapshot = read_guard->json();
            }
            else
            {
                return false;
            }
        }

        // Commit the snapshot to disk.
        return private_commit_to_disk_unsafe(snapshot, err_code);
    }
    catch (const std::exception &ex)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        LOGGER_ERROR("JsonConfig::overwrite: exception: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
}

// ---------------- Transaction creation / storage ----------------

JsonConfig::TransactionProxy JsonConfig::transaction(AccessFlags flags,
                                                     std::error_code *err_code) noexcept
{
    Transaction *transaction = create_transaction_internal(err_code);
    if (transaction == nullptr)
    {
        // create_transaction_internal sets err_code
        return {nullptr, 0, flags};
    }

    // Return a proxy; user can only consume it immediately due to &&-qualified read/write methods.
    return {this, transaction->d_id, flags};
}

JsonConfig::Transaction *JsonConfig::create_transaction_internal(std::error_code *err_code) noexcept
{
    if (!pImpl)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::not_connected);
        }
        return nullptr;
    }

    std::lock_guard<std::mutex> tx_lock(d_tx_mutex);

    TxId tx_id = d_next_txid++;

    auto node = std::make_unique<Transaction>(tx_id);

    d_tx_list.push_back(std::move(node));
    auto list_it = std::prev(d_tx_list.end());

    d_tx_index.emplace(tx_id, list_it);

    if (err_code != nullptr)
    {
        *err_code = std::error_code{};
    }
    return list_it->get();
}

void JsonConfig::destroy_transaction_internal(TxId tx_id) noexcept
{
    std::lock_guard<std::mutex> tx_lock(d_tx_mutex);

    auto it_idx = d_tx_index.find(tx_id);
    if (it_idx == d_tx_index.end())
    {
        return;
    }

    auto it_list = it_idx->second;
    d_tx_list.erase(it_list);
    d_tx_index.erase(it_idx);
}

bool JsonConfig::release_transaction(TxId tx_id, std::error_code *err_code) noexcept
{
    std::lock_guard<std::mutex> tx_lock(d_tx_mutex);

    auto it_idx = d_tx_index.find(tx_id);
    if (it_idx == d_tx_index.end())
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::invalid_argument);
        }
        return false;
    }

    d_tx_list.erase(it_idx->second);
    d_tx_index.erase(it_idx);

    if (err_code != nullptr)
    {
        *err_code = std::error_code{};
    }
    return true;
}

JsonConfig::Transaction *JsonConfig::find_transaction_locked(TxId tx_id) noexcept
{
    auto it_idx = d_tx_index.find(tx_id);
    if (it_idx == d_tx_index.end())
    {
        return nullptr;
    }
    return it_idx->second->get();
}

// ----------------- ReadLock / WriteLock factory and implementations ----------------

std::optional<JsonConfig::ReadLock> JsonConfig::lock_for_read(std::error_code *err_code) const noexcept
{
    if (!pImpl)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::not_connected);
        }
        return std::nullopt;
    }

    if (basics::RecursionGuard::is_recursing(pImpl.get()))
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::resource_deadlock_would_occur);
        }
        return std::nullopt;
    }

    ReadLock read_lock;
    read_lock.d_ = std::make_unique<ReadLock::ImplInner>();
    read_lock.d_->owner = const_cast<JsonConfig *>(this);
    read_lock.d_->guard.emplace(this->pImpl.get());
    read_lock.d_->lock = std::shared_lock(pImpl->dataMutex);

    if (err_code != nullptr)
    {
        *err_code = std::error_code{};
    }
    return read_lock;
}

std::optional<JsonConfig::WriteLock> JsonConfig::lock_for_write(std::error_code *err_code) noexcept
{
    if (!pImpl)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::not_connected);
        }
        return std::nullopt;
    }

    if (basics::RecursionGuard::is_recursing(pImpl.get()))
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::resource_deadlock_would_occur);
        }
        return std::nullopt;
    }

    WriteLock write_lock;
    write_lock.d_ = std::make_unique<WriteLock::ImplInner>();
    write_lock.d_->owner = this;
    write_lock.d_->guard.emplace(this->pImpl.get());
    write_lock.d_->lock = std::unique_lock(pImpl->dataMutex);

    if (err_code != nullptr)
    {
        *err_code = std::error_code{};
    }
    return write_lock;
}

// ReadLock / WriteLock methods
JsonConfig::ReadLock::ReadLock() noexcept = default;
JsonConfig::ReadLock::ReadLock(ReadLock &&other) noexcept : d_(std::move(other.d_)) {}
JsonConfig::ReadLock &JsonConfig::ReadLock::operator=(ReadLock &&other) noexcept
{
    d_ = std::move(other.d_);
    return *this;
}
JsonConfig::ReadLock::~ReadLock() = default;

const nlohmann::json &JsonConfig::ReadLock::json() const noexcept
{
    if (d_ == nullptr || d_->owner == nullptr)
    {
        static const nlohmann::json null_json = nlohmann::json();
        return null_json;
    }
    return d_->owner->pImpl->data;
}

JsonConfig::WriteLock::WriteLock() noexcept = default;
JsonConfig::WriteLock::WriteLock(WriteLock &&other) noexcept : d_(std::move(other.d_)) {}
JsonConfig::WriteLock &JsonConfig::WriteLock::operator=(WriteLock &&other) noexcept
{
    d_ = std::move(other.d_);
    return *this;
}
JsonConfig::WriteLock::~WriteLock() = default;

// NOLINTNEXTLINE(bugprone-exception-escape) -- returning reference to internal JSON; nlohmann::json can throw on invalid access
nlohmann::json &JsonConfig::WriteLock::json() noexcept
{
    if (d_ == nullptr || d_->owner == nullptr)
    {
        static nlohmann::json dummy = nlohmann::json::object();
        return dummy;
    }

    // A non-const access to the JSON data implies a potential modification.
    // We conservatively mark the object as dirty.
    d_->owner->pImpl->dirty.store(true, std::memory_order_release);
    return d_->owner->pImpl->data;
}

bool JsonConfig::WriteLock::commit(std::error_code *err_code) noexcept
{
    if (d_ != nullptr && d_->owner != nullptr)
    {
        try
        {
            auto *owner = d_->owner;
            // Snapshot the data while the lock is still held.
            nlohmann::json snapshot = owner->pImpl->data;
            // Release the in-memory lock *before* performing slow disk I/O to
            // allow other threads to proceed. The process-level lock inside
            // private_commit_to_disk_unsafe will still protect the file.
            d_.reset();
            return owner->private_commit_to_disk_unsafe(snapshot, err_code);
        }
        catch (...)
        {
            if (err_code != nullptr)
            {
                *err_code = std::make_error_code(std::errc::io_error);
            }
            return false;
        }
    }
    if (err_code != nullptr)
    {
        *err_code = std::make_error_code(std::errc::not_connected);
    }
    return false;
}

// --------------- atomic_write_json: cross-platform implementation ---------------
// Platform-specific helpers live in an anonymous namespace. The public API
// dispatches to atomic_write_json_win (Windows) or atomic_write_json_posix (POSIX)
// so each platform's logic is self-contained and no code is shared across platforms.
//
// Contract for all helpers: err_code may be null; when non-null it is set on failure.
// On failure, callers must not assume partial state (e.g. temp file is always cleaned up
// by the helper that created or owns it).
//
// To verify behavior: each helper's comment states purpose, assumptions, return meaning,
// and cleanup on failure; the two platform entry points (atomic_write_json_win / _posix)
// document the exact step order so it can be checked against the implementation.

namespace
{
#if defined(PYLABHUB_PLATFORM_WIN64)
constexpr int kReplaceRetries = 5;
constexpr int kReplaceDelayMs = 100;

/**
 * Ensures the parent directory of target exists (create_directories).
 * Assumption: target is the final file path; parent_path() is used, or "." if empty.
 * Returns true on success. On failure: sets *err_code (if non-null), logs, returns false.
 */
bool ensure_parent_dir_win(const fs::path &target, std::error_code *err_code)
{
    fs::path parent = target.parent_path();
    if (parent.empty())
    {
        parent = ".";
    }
    std::error_code create_ec;
    fs::create_directories(parent, create_ec);
    if (create_ec)
    {
        if (err_code != nullptr)
        {
            *err_code = create_ec;
        }
        LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(),
                     create_ec.message());
        return false;
    }
    return true;
}

/**
 * Creates a unique temp file in parent, writes json_snapshot (pretty-printed, indent 4),
 * flushes with FlushFileBuffers, and closes the handle.
 * Assumptions: parent exists and is writable; temp name uses PID + GetTickCount64 for uniqueness.
 * Returns (tmp_full path, tmp_full_w string) on success for use by atomic_replace_win.
 * On failure: temp file is deleted if it was created, *err_code set (if non-null), logs, returns nullopt.
 */
std::optional<std::pair<fs::path, std::wstring>>
create_and_write_temp_win(const fs::path &parent, const fs::path &target,
                          const nlohmann::json &json_snapshot, std::error_code *err_code)
{
    std::wstring filename = target.filename().wstring();
    std::wstring tmpname = filename + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                           std::to_wstring(GetTickCount64());
    fs::path tmp_full = parent / fs::path(tmpname);
    std::wstring tmp_full_w = tmp_full.wstring();

    HANDLE h = CreateFileW(tmp_full_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(err));
        }
        LOGGER_ERROR("atomic_write_json: CreateFileW(temp) failed for '{}'. Error:{}", tmp_full.string(),
                     err);
        return std::nullopt;
    }

    const std::string out = json_snapshot.dump(4);
    DWORD written = 0;
    const BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    if (!ok || written != static_cast<DWORD>(out.size()))
    {
        const DWORD err = GetLastError();
        FlushFileBuffers(h);
        CloseHandle(h);
        DeleteFileW(tmp_full_w.c_str());
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(err));
        }
        LOGGER_ERROR("atomic_write_json: WriteFile failed for '{}'. Error:{}", tmp_full.string(), err);
        return std::nullopt;
    }

    if (FlushFileBuffers(h) == 0)
    {
        const DWORD err = GetLastError();
        CloseHandle(h);
        DeleteFileW(tmp_full_w.c_str());
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(err));
        }
        LOGGER_ERROR("atomic_write_json: FlushFileBuffers failed for '{}'. Error:{}", tmp_full.string(),
                     err);
        return std::nullopt;
    }

    CloseHandle(h);
    return std::pair{tmp_full, tmp_full_w};
}

/**
 * Atomically makes the temp file become the target file.
 * Behavior: ReplaceFileW with REPLACEFILE_WRITE_THROUGH; retries up to kReplaceRetries
 * on ERROR_SHARING_VIOLATION with Sleep(kReplaceDelayMs). If ReplaceFileW fails with
 * ERROR_FILE_NOT_FOUND (target does not exist), falls back to MoveFileExW with
 * MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
 * Returns true if target now contains the new content. On failure: deletes temp file,
 * sets *err_code to io_error (if non-null), logs, returns false.
 * Assumption: tmp_full_w is a path to a file we own; target_w is the desired final path.
 */
bool atomic_replace_win(const std::wstring &target_w, const std::wstring &tmp_full_w,
                        const fs::path &target_display, const fs::path &tmp_display,
                        std::error_code *err_code)
{
    BOOL replaced = FALSE;
    DWORD last_error = 0;
    for (int i = 0; i < kReplaceRetries; ++i)
    {
        replaced = ReplaceFileW(target_w.c_str(), tmp_full_w.c_str(), nullptr,
                                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        if (replaced != 0)
        {
            break;
        }
        last_error = GetLastError();
        if (last_error != ERROR_SHARING_VIOLATION)
        {
            break;
        }
        LOGGER_WARN("atomic_write_json: ReplaceFileW encountered sharing violation for '{}', retrying...",
                   target_display.string());
        Sleep(kReplaceDelayMs);
    }

    if (replaced != 0)
    {
        return true;
    }

    if (last_error == ERROR_FILE_NOT_FOUND)
    {
        if (MoveFileExW(tmp_full_w.c_str(), target_w.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        {
            return true;
        }
        last_error = GetLastError();
        LOGGER_ERROR("atomic_write_json: MoveFileW fallback failed for '{}' -> '{}'. Error:{}",
                     tmp_display.string(), target_display.string(), last_error);
    }
    else
    {
        LOGGER_ERROR("atomic_write_json: ReplaceFileW failed for '{}' after retries. Error:{}",
                     target_display.string(), last_error);
    }
    DeleteFileW(tmp_full_w.c_str());
    if (err_code != nullptr)
    {
        *err_code = std::make_error_code(std::errc::io_error);
    }
    return false;
}

/**
 * Windows implementation of atomic JSON write.
 * Order: ensure parent dir -> create temp and write+flush+close -> atomic replace (or move fallback).
 * All exceptions are caught and reported as io_error; *err_code is set and no rethrow.
 */
void atomic_write_json_win(const fs::path &target, const nlohmann::json &json_snapshot,
                           std::error_code *err_code) noexcept
{
    try
    {
        if (!ensure_parent_dir_win(target, err_code))
        {
            return;
        }
        const fs::path parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
        auto opt = create_and_write_temp_win(parent, target, json_snapshot, err_code);
        if (!opt.has_value())
        {
            return;
        }
        const std::wstring target_w = target.wstring();
        const bool ok = atomic_replace_win(target_w, opt->second, target, opt->first, err_code);
        if (ok && err_code != nullptr)
        {
            *err_code = std::error_code{};
        }
    }
    catch (const std::exception &ex)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        LOGGER_ERROR("atomic_write_json: exception: {}", ex.what());
    }
    catch (...)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        LOGGER_ERROR("atomic_write_json: unknown error");
    }
}

#else
// POSIX path: same constants for consistency with Windows retry behavior.
constexpr int kRenameRetries = 5;
constexpr int kRenameDelayMs = 100;

/**
 * Ensures the parent directory of target exists (create_directories).
 * Same contract as ensure_parent_dir_win: parent_path() or "."; on failure sets *err_code, logs, returns false.
 */
bool ensure_parent_dir_posix(const fs::path &target, std::error_code *err_code)
{
    fs::path parent = target.parent_path();
    if (parent.empty())
    {
        parent = ".";
    }
    std::error_code create_ec;
    fs::create_directories(parent, create_ec);
    if (create_ec)
    {
        if (err_code != nullptr)
        {
            *err_code = create_ec;
        }
        LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(),
                     create_ec.message());
        return false;
    }
    return true;
}

/**
 * Refuses to write if target is a symbolic link (security: avoid overwriting an arbitrary file).
 * Uses lstat; if lstat fails or the path is not a symlink, returns true (allow). If target is
 * a symlink: sets *err_code to operation_not_permitted, logs, returns false.
 */
bool reject_if_symlink_posix(const fs::path &target, std::error_code *err_code)
{
    struct stat lstat_buf;
    if (lstat(target.c_str(), &lstat_buf) != 0)
    {
        return true; // not a symlink or path missing; allow
    }
    if (!S_ISLNK(lstat_buf.st_mode))
    {
        return true; // not a symlink
    }
    if (err_code != nullptr)
    {
        *err_code = std::make_error_code(std::errc::operation_not_permitted);
    }
    LOGGER_ERROR("atomic_write_json: target '{}' is a symbolic link, refusing to write", target.string());
    return false;
}

/**
 * Creates a unique temp file via mkstemp in parent with name pattern "<target_filename>.tmp.XXXXXX".
 * Assumptions: parent exists; mkstemp creates file with secure permissions (0600).
 * Returns (tmp_path string, open fd) on success. Caller must pass fd to write_fsync_close_posix
 * (which closes it). On failure: no temp file left, *err_code set (if non-null), logs, returns nullopt.
 */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- parameter order: parent directory then target path (for temp name)
std::optional<std::pair<std::string, int>> create_temp_posix(const fs::path &parent,
                                                              const fs::path &target,
                                                              std::error_code *err_code)
{
    std::string dir = parent.string();
    if (dir.empty())
    {
        dir = ".";
    }
    const std::string filename = target.filename().string();
    const std::string tmpl = dir + "/" + filename + ".tmp.XXXXXX";
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');

    const int file_fd = mkstemp(tmpl_buf.data());
    if (file_fd == -1)
    {
        const int errnum = errno;
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(errnum));
        }
        LOGGER_ERROR("atomic_write_json: mkstemp failed for '{}'. Error: {}", tmpl_buf.data(),
                     std::strerror(errnum));
        return std::nullopt;
    }
    return std::pair{std::string(tmpl_buf.data()), file_fd};
}

/**
 * Writes json_snapshot (pretty-printed, indent 4) to file_fd, fsyncs, optionally fchmods to
 * match target's mode if target exists, then closes file_fd.
 * Assumption: file_fd is the fd from create_temp_posix; tmp_path is that temp file path.
 * On any failure: closes fd (if not already closed), unlinks tmp_path, sets *err_code, logs, returns false.
 * On success: fd is closed; temp file remains for caller to rename.
 */
bool write_fsync_close_posix(int file_fd, const std::string &tmp_path,
                             const nlohmann::json &json_snapshot, const fs::path &target,
                             std::error_code *err_code)
{
    const std::string out = json_snapshot.dump(4);
    const char *buf = out.data();
    size_t to_write = out.size();
    size_t written = 0;
    while (to_write > 0)
    {
        const ssize_t nwritten = ::write(file_fd, buf + written, to_write);
        if (nwritten < 0)
        {
            const int errnum = errno;
            ::close(file_fd);
            ::unlink(tmp_path.c_str());
            if (err_code != nullptr)
            {
                *err_code = std::make_error_code(static_cast<std::errc>(errnum));
            }
            LOGGER_ERROR("atomic_write_json: write failed for '{}'. Error: {}", tmp_path,
                         std::strerror(errnum));
            return false;
        }
        written += static_cast<size_t>(nwritten);
        to_write -= static_cast<size_t>(nwritten);
    }
    if (::fsync(file_fd) != 0)
    {
        const int errnum = errno;
        ::close(file_fd);
        ::unlink(tmp_path.c_str());
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(errnum));
        }
        LOGGER_ERROR("atomic_write_json: fsync(file) failed for '{}'. Error: {}", tmp_path,
                     std::strerror(errnum));
        return false;
    }
    struct stat stat_buf;
    if (stat(target.c_str(), &stat_buf) == 0)
    {
        if (fchmod(file_fd, stat_buf.st_mode) != 0)
        {
            const int errnum = errno;
            ::close(file_fd);
            ::unlink(tmp_path.c_str());
            if (err_code != nullptr)
            {
                *err_code = std::make_error_code(static_cast<std::errc>(errnum));
            }
            LOGGER_ERROR("atomic_write_json: fchmod failed for '{}'. Error: {}", tmp_path,
                         std::strerror(errnum));
            return false;
        }
    }
    if (::close(file_fd) != 0)
    {
        const int errnum = errno;
        ::unlink(tmp_path.c_str());
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(errnum));
        }
        LOGGER_ERROR("atomic_write_json: close failed for '{}'. Error: {}", tmp_path,
                     std::strerror(errnum));
        return false;
    }
    return true;
}

/**
 * Atomically renames tmp_path to target (std::rename). Retries up to kRenameRetries on
 * EBUSY, ETXTBSY, EINTR with sleep kRenameDelayMs; other errors break immediately.
 * Assumption: tmp_path and target are on the same filesystem (rename is atomic on POSIX).
 * On failure: unlinks tmp_path, sets *err_code to last errno (if non-null), logs, returns false.
 */
bool atomic_rename_posix(const std::string &tmp_path, const fs::path &target,
                         std::error_code *err_code)
{
    int last_errnum = 0;
    for (int i = 0; i < kRenameRetries; ++i)
    {
        if (std::rename(tmp_path.c_str(), target.c_str()) == 0)
        {
            return true;
        }
        last_errnum = errno;
        if (last_errnum != EBUSY && last_errnum != ETXTBSY && last_errnum != EINTR)
        {
            break;
        }
        LOGGER_WARN("atomic_write_json: rename encountered transient error {} for '{}', retrying...",
                   std::strerror(last_errnum), target.string());
        std::this_thread::sleep_for(std::chrono::milliseconds(kRenameDelayMs));
    }
    ::unlink(tmp_path.c_str());
    if (err_code != nullptr)
    {
        *err_code = std::make_error_code(static_cast<std::errc>(last_errnum));
    }
    LOGGER_ERROR("atomic_write_json: rename failed for '{}' after retries. Error: {}", target.string(),
                 std::strerror(last_errnum));
    return false;
}

/**
 * Opens the parent directory of target (O_DIRECTORY | O_RDONLY), fsyncs it so the directory
 * entry update is durable, then closes. Used after atomic_rename so the new name is committed to disk.
 * On failure: sets *err_code, logs, returns false. dfd is always closed before return.
 */
bool fsync_parent_posix(const fs::path &target, std::error_code *err_code)
{
    const fs::path parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    const std::string dir = parent.string().empty() ? "." : parent.string();
    const int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
    if (dfd < 0)
    {
        const int errnum = errno;
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(errnum));
        }
        LOGGER_ERROR("atomic_write_json: open(dir) failed for fsync: '{}'. Error: {}", dir,
                     std::strerror(errnum));
        return false;
    }
    bool success = true;
    if (::fsync(dfd) != 0)
    {
        const int errnum = errno;
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(static_cast<std::errc>(errnum));
        }
        LOGGER_ERROR("atomic_write_json: fsync(dir) failed for '{}'. Error: {}", dir,
                     std::strerror(errnum));
        success = false;
    }
    ::close(dfd);
    return success;
}

/**
 * POSIX implementation of atomic JSON write.
 * Order: ensure parent dir -> reject if target is symlink -> create temp -> write+fsync+close ->
 * atomic rename -> fsync parent dir. All exceptions caught and reported as io_error.
 */
void atomic_write_json_posix(const fs::path &target, const nlohmann::json &json_snapshot,
                             std::error_code *err_code) noexcept
{
    try
    {
        if (!ensure_parent_dir_posix(target, err_code))
        {
            return;
        }
        if (!reject_if_symlink_posix(target, err_code))
        {
            return;
        }
        const fs::path parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
        auto opt = create_temp_posix(parent, target, err_code);
        if (!opt.has_value())
        {
            return;
        }
        const std::string tmp_path = opt->first;
        const int file_fd = opt->second;
        if (!write_fsync_close_posix(file_fd, tmp_path, json_snapshot, target, err_code))
        {
            return;
        }
        if (!atomic_rename_posix(tmp_path, target, err_code))
        {
            return;
        }
        if (!fsync_parent_posix(target, err_code))
        {
            return;
        }
        if (err_code != nullptr)
        {
            *err_code = std::error_code{};
        }
    }
    catch (const std::exception &ex)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        LOGGER_ERROR("atomic_write_json: exception: {}", ex.what());
    }
    catch (...)
    {
        if (err_code != nullptr)
        {
            *err_code = std::make_error_code(std::errc::io_error);
        }
        LOGGER_ERROR("atomic_write_json: unknown error");
    }
}
#endif
} // namespace

/**
 * @brief Atomically writes a JSON object to a file.
 *
 * This function ensures that the file is never left in a corrupted state, even
 * if the process is terminated during the write. It does this by first writing
 * to a temporary file and then atomically replacing or renaming it to the target path.
 *
 * @param target The final destination path for the file.
 * @param json_snapshot The `nlohmann::json` object to write.
 * @param err_code A `std::error_code` to receive any errors.
 */
void JsonConfig::atomic_write_json(const std::filesystem::path &target,
                                   const nlohmann::json &json_snapshot,
                                   std::error_code *err_code) noexcept
{
    if (err_code != nullptr)
    {
        *err_code = std::error_code{};
    }
#if defined(PYLABHUB_PLATFORM_WIN64)
    atomic_write_json_win(target, json_snapshot, err_code);
#else
    atomic_write_json_posix(target, json_snapshot, err_code);
#endif
}

// Lifecycle Integration
bool JsonConfig::lifecycle_initialized() noexcept
{
    return g_jsonconfig_initialized.load(std::memory_order_acquire);
}

namespace
{
void do_jsonconfig_startup(const char *arg)
{
    (void)arg;
    g_jsonconfig_initialized.store(true, std::memory_order_release);
}
void do_jsonconfig_shutdown(const char *arg)
{
    (void)arg;
    g_jsonconfig_initialized.store(false, std::memory_order_release);
}
} // namespace

namespace
{
constexpr unsigned int kJsonConfigShutdownTimeoutMs = 1000;
}

ModuleDef JsonConfig::GetLifecycleModule()
{
    ModuleDef module("pylabhub::utils::JsonConfig");
    // JsonConfig depends on FileLock and Logger being available.
    module.add_dependency("pylabhub::utils::FileLock");
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_jsonconfig_startup);
    module.set_shutdown(&do_jsonconfig_shutdown, kJsonConfigShutdownTimeoutMs);
    return module;
}

} // namespace pylabhub::utils
