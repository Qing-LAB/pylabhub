#include "platform.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <utility>
#include <vector>

#if defined(PLATFORM_WIN64)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "utils/FileLock.hpp"
#include "utils/JsonConfig.hpp"
#include "utils/Logger.hpp"
#include "recursion_guard.hpp"

namespace pylabhub::utils
{
namespace fs = std::filesystem;

// Module-level flag to indicate if the JsonConfig has been initialized.
static std::atomic<bool> g_jsonconfig_initialized{false};

struct JsonConfig::Impl
{
    std::filesystem::path configPath;
    nlohmann::json data = nlohmann::json::object();
    mutable std::shared_mutex dataMutex; // Mutex for thread-safe access to the 'data' member
    std::mutex initMutex;
    std::atomic<bool> dirty{false};      // "in-memory diverged since last successful load/commit"
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
        PLH_PANIC("JsonConfig created before its module was initialized via LifecycleManager. Aborting.");
    }
}

JsonConfig::JsonConfig(const std::filesystem::path &configFile, bool createIfMissing,
                       std::error_code *ec) noexcept
    : pImpl(std::make_unique<Impl>())
{
    if (!lifecycle_initialized())
    {
        PLH_PANIC("JsonConfig created before its module was initialized via LifecycleManager. Aborting.");
    }
    if (!init(configFile, createIfMissing, ec))
    {
        // init populates ec; ctor remains noexcept
    }
}

JsonConfig::~JsonConfig()
{
    // IMPORTANT: we must not leak internal tx records.
    // This does NOT make holding a proxy across JsonConfig lifetime safe;
    // it only cleans bookkeeping.
    std::lock_guard<std::mutex> lg(d_tx_mutex);
    d_tx_index.clear();
    d_tx_list.clear();
}

JsonConfig::JsonConfig(JsonConfig &&other) noexcept
    : pImpl(std::move(other.pImpl))
{
    // Design note:
    // If you keep move enabled, moving a JsonConfig with outstanding transactions is dangerous.
    // Minimal safe policy: require "no outstanding tx" on move, otherwise PANIC in debug.
    {
        std::lock_guard<std::mutex> lg(other.d_tx_mutex);
        if (!other.d_tx_list.empty())
        {
            LOGGER_ERROR("JsonConfig move-ctor with outstanding transactions. This is unsafe by contract.");
            // You can PLH_PANIC here if you want it strict.
        }
        // We intentionally do NOT transfer tx records; moved-from object becomes empty.
        other.d_tx_index.clear();
        other.d_tx_list.clear();
    }
}

JsonConfig &JsonConfig::operator=(JsonConfig &&other) noexcept
{
    if (this == &other) return *this;

    {
        std::lock_guard<std::mutex> lg(d_tx_mutex);
        d_tx_index.clear();
        d_tx_list.clear();
    }

    pImpl = std::move(other.pImpl);

    {
        std::lock_guard<std::mutex> lg(other.d_tx_mutex);
        if (!other.d_tx_list.empty())
        {
            LOGGER_ERROR("JsonConfig move-assign with outstanding transactions. This is unsafe by contract.");
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
        return {};
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    return std::filesystem::path(pImpl->configPath);
}

bool JsonConfig::is_initialized() const noexcept
{
    if (!pImpl)
        return false;
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    return !pImpl->configPath.empty();
}

// New private helper requested by header-only txn logic to avoid touching Impl in headers.
void JsonConfig::private_set_dirty_unsafe_(bool v) noexcept
{
    if (!pImpl) return;
    pImpl->dirty.store(v, std::memory_order_release);
}

bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing,
                      std::error_code *ec) noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> g(pImpl->initMutex);

            // Security: refuse to operate on a path that is a symbolic link.
            if (fs::is_symlink(configFile))
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::operation_not_permitted);
                LOGGER_ERROR("JsonConfig::init: target '{}' is a symbolic link, refusing to initialize.",
                             configFile.string());
                return false;
            }

            pImpl->configPath = configFile;

            if (createIfMissing)
            {
                std::error_code lfs;
                if (!fs::exists(configFile, lfs))
                {
                    nlohmann::json empty = nlohmann::json::object();
                    atomic_write_json(configFile, empty, ec);
                    if (ec && *ec)
                        return false;
                }
            }
        }
        return reload(ec);
    }
    catch (const std::exception &ex)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::init: exception during init: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

bool JsonConfig::private_load_from_disk_unsafe(std::error_code *ec) noexcept
{
    try
    {
        std::ifstream in(pImpl->configPath);
        if (!in.is_open())
        {
            // Decide policy: missing file => empty object (current behavior).
            // This is reasonable for "config optional" workflows, but document it.
            pImpl->data = nlohmann::json::object();
            pImpl->dirty.store(false, std::memory_order_release);
            if (ec)
                *ec = std::error_code{};
            return true;
        }

        nlohmann::json newdata;
        in >> newdata;

        pImpl->data = std::move(newdata);
        pImpl->dirty.store(false, std::memory_order_release);

        if (ec)
            *ec = std::error_code{};
        return true;
    }
    catch (const std::exception &ex)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::private_load_from_disk_unsafe: exception during load: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

bool JsonConfig::reload(std::error_code *ec) noexcept
{
    try
    {
        if (!pImpl)
        {
            if (ec)
                *ec = std::make_error_code(std::errc::not_connected);
            return false;
        }

        // 1) Exclusive lock for in-memory data modification
        std::unique_lock<std::shared_mutex> data_lock(pImpl->dataMutex);

        // 2) Process-level lock for disk access
        FileLock fLock(pImpl->configPath, ResourceType::File, LockMode::Blocking);
        if (!fLock.valid())
        {
            if (ec)
                *ec = fLock.error_code();
            return false;
        }

        return private_load_from_disk_unsafe(ec);
    }
    catch (...)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

bool JsonConfig::private_commit_to_disk_unsafe(const nlohmann::json &snapshot,
                                               std::error_code *ec) noexcept
{
    try
    {
        FileLock fl(pImpl->configPath, ResourceType::File, LockMode::Blocking);
        if (!fl.valid())
        {
            if (ec)
                *ec = fl.error_code();
            return false;
        }

        atomic_write_json(pImpl->configPath, snapshot, ec);
        if (ec && *ec)
            return false;

        pImpl->dirty.store(false, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

bool JsonConfig::overwrite(std::error_code *ec) noexcept
{
    if (!pImpl)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    try
    {
        nlohmann::json snapshot;
        {
            if (auto r = lock_for_read(ec))
            {
                snapshot = r->json();
            }
            else
            {
                return false;
            }
        }

        return private_commit_to_disk_unsafe(snapshot, ec);
    }
    catch (const std::exception &ex)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::overwrite: exception: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

// ---------------- Transaction creation / storage ----------------
//
// Integrated change:
//  - transaction() now returns TransactionProxy by value (rvalue-only consumption).
//  - Internal storage remains list + index (no pointer ownership changes).
//  - If you want "no rehash ever", use std::map in the header for d_tx_index.
//    (Your current runtime safety is OK with unordered_map too, but you asked to move away.)

JsonConfig::TransactionProxy JsonConfig::transaction(AccessFlags flags, std::error_code *ec) noexcept
{
    Transaction *t = create_transaction_internal(flags, ec);
    if (!t)
    {
        // create_transaction_internal sets ec
        return TransactionProxy(nullptr, 0, flags);
    }

    // Return a proxy; user can only consume it immediately due to && read/write.
    return TransactionProxy(this, t->d_id, flags);
}

JsonConfig::Transaction *JsonConfig::create_transaction_internal(AccessFlags flags,
                                                                 std::error_code *ec) noexcept
{
    if (!pImpl)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return nullptr;
    }

    std::lock_guard<std::mutex> lg(d_tx_mutex);

    TxId id = d_next_txid++;

    auto node = std::make_unique<Transaction>(this, id, flags);

    d_tx_list.push_back(std::move(node));
    auto it = std::prev(d_tx_list.end());

    // d_tx_index should be std::map<TxId, list::iterator> to avoid any rehash concerns
    d_tx_index.emplace(id, it);

    if (ec)
        *ec = std::error_code{};
    return it->get();
}

void JsonConfig::destroy_transaction_internal(TxId id) noexcept
{
    std::lock_guard<std::mutex> lg(d_tx_mutex);

    auto it_idx = d_tx_index.find(id);
    if (it_idx == d_tx_index.end())
        return;

    auto it_list = it_idx->second;
    d_tx_list.erase(it_list);
    d_tx_index.erase(it_idx);
}

bool JsonConfig::release_transaction(TxId id, std::error_code *ec) noexcept
{
    std::lock_guard<std::mutex> lg(d_tx_mutex);

    auto it_idx = d_tx_index.find(id);
    if (it_idx == d_tx_index.end())
    {
        if (ec)
            *ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }

    d_tx_list.erase(it_idx->second);
    d_tx_index.erase(it_idx);

    if (ec)
        *ec = std::error_code{};
    return true;
}

JsonConfig::Transaction *JsonConfig::find_transaction_locked(TxId id) noexcept
{
    auto it_idx = d_tx_index.find(id);
    if (it_idx == d_tx_index.end())
        return nullptr;
    return it_idx->second->get();
}

// ----------------- ReadLock / WriteLock factory and implementations ----------------

std::optional<JsonConfig::ReadLock> JsonConfig::lock_for_read(std::error_code *ec) const noexcept
{
    if (!pImpl)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return std::nullopt;
    }

    if (basics::RecursionGuard::is_recursing(pImpl.get()))
    {
        if (ec)
            *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        return std::nullopt;
    }

    ReadLock r;
    r.d_ = std::make_unique<ReadLock::ImplInner>();
    r.d_->owner = const_cast<JsonConfig *>(this);
    r.d_->guard.emplace(this->pImpl.get());
    r.d_->lock = std::shared_lock(pImpl->dataMutex);

    if (ec)
        *ec = std::error_code{};
    return r;
}

std::optional<JsonConfig::WriteLock> JsonConfig::lock_for_write(std::error_code *ec) noexcept
{
    if (!pImpl)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::not_connected);
        return std::nullopt;
    }

    if (basics::RecursionGuard::is_recursing(pImpl.get()))
    {
        if (ec)
            *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        return std::nullopt;
    }

    WriteLock w;
    w.d_ = std::make_unique<WriteLock::ImplInner>();
    w.d_->owner = this;
    w.d_->guard.emplace(this->pImpl.get());
    w.d_->lock = std::unique_lock(pImpl->dataMutex);

    if (ec)
        *ec = std::error_code{};
    return w;
}

// ReadLock / WriteLock methods
JsonConfig::ReadLock::ReadLock() noexcept {}
JsonConfig::ReadLock::ReadLock(ReadLock &&other) noexcept : d_(std::move(other.d_)) {}
JsonConfig::ReadLock &JsonConfig::ReadLock::operator=(ReadLock &&other) noexcept
{
    d_ = std::move(other.d_);
    return *this;
}
JsonConfig::ReadLock::~ReadLock() {}

const nlohmann::json &JsonConfig::ReadLock::json() const noexcept
{
    if (!d_ || !d_->owner)
    {
        static const nlohmann::json null_json = nlohmann::json();
        return null_json;
    }
    return d_->owner->pImpl->data;
}

JsonConfig::WriteLock::WriteLock() noexcept {}
JsonConfig::WriteLock::WriteLock(WriteLock &&other) noexcept : d_(std::move(other.d_)) {}
JsonConfig::WriteLock &JsonConfig::WriteLock::operator=(WriteLock &&other) noexcept
{
    d_ = std::move(other.d_);
    return *this;
}
JsonConfig::WriteLock::~WriteLock() {}

nlohmann::json &JsonConfig::WriteLock::json() noexcept
{
    if (!d_ || !d_->owner)
    {
        static nlohmann::json dummy = nlohmann::json::object();
        return dummy;
    }

    // NOTE on dirty semantics:
    // - We cannot reliably detect whether the caller *actually* mutated the json.
    // - Marking dirty on non-const access is a reasonable conservative rule.
    d_->owner->pImpl->dirty.store(true, std::memory_order_release);
    return d_->owner->pImpl->data;
}

bool JsonConfig::WriteLock::commit(std::error_code *ec) noexcept
{
    if (d_ && d_->owner)
    {
        try
        {
            auto *owner = d_->owner;
            nlohmann::json snapshot = owner->pImpl->data;
            d_.reset(); // release mutex before slow I/O
            return owner->private_commit_to_disk_unsafe(snapshot, ec);
        }
        catch (...)
        {
            if (ec)
                *ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    if (ec)
        *ec = std::make_error_code(std::errc::not_connected);
    return false;
}


// ----------------- atomic write implementation (POSIX and Windows) ----------------
void JsonConfig::atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j,
                                   std::error_code *ec) noexcept
{
    if (ec)
        *ec = std::error_code{};
#if defined(PLATFORM_WIN64)
    // Windows implementation
    try
    {
        std::filesystem::path parent = target.parent_path();
        if (parent.empty())
            parent = ".";
        std::error_code create_ec;
        fs::create_directories(parent, create_ec);
        if (create_ec)
        {
            if (ec)
                *ec = create_ec;
            LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(),
                         create_ec.message());
            return;
        }

        std::wstring filename = target.filename().wstring();
        std::wstring tmpname = filename + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                               std::to_wstring(GetTickCount64());
        std::filesystem::path tmp_full = parent / std::filesystem::path(tmpname);
        std::wstring tmp_full_w = tmp_full.wstring();
        std::wstring target_w = target.wstring();

        HANDLE h = CreateFileW(tmp_full_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            if (ec)
                *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: CreateFileW(temp) failed for '{}'. Error:{}",
                         tmp_full.string(), err);
            return;
        }

        std::string out = j.dump(4);
        DWORD written = 0;
        BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
        if (!ok || written != static_cast<DWORD>(out.size()))
        {
            DWORD err = GetLastError();
            FlushFileBuffers(h);
            CloseHandle(h);
            DeleteFileW(tmp_full_w.c_str());
            if (ec)
                *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: WriteFile failed for '{}'. Error:{}",
                         tmp_full.string(), err);
            return;
        }

        if (!FlushFileBuffers(h))
        {
            DWORD err = GetLastError();
            CloseHandle(h);
            DeleteFileW(tmp_full_w.c_str());
            if (ec)
                *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: FlushFileBuffers failed for '{}'. Error:{}",
                         tmp_full.string(), err);
            return;
        }

        CloseHandle(h);

        const int REPLACE_RETRIES = 5;
        const int REPLACE_DELAY_MS = 100;
        BOOL replaced = FALSE;
        DWORD last_error = 0;
        for (int i = 0; i < REPLACE_RETRIES; ++i)
        {
            replaced = ReplaceFileW(target_w.c_str(), tmp_full_w.c_str(), nullptr,
                                    REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
            if (replaced)
                break;
            last_error = GetLastError();
            // If sharing violation, wait and retry
            if (last_error != ERROR_SHARING_VIOLATION)
                break;
            Sleep(REPLACE_DELAY_MS);
        }

        if (!replaced)
        {
            if (last_error == ERROR_FILE_NOT_FOUND)
            {
                // If the target file does not exist, try MoveFileEx as a fallback
                if (!MoveFileExW(tmp_full_w.c_str(), target_w.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    last_error = GetLastError();
                    LOGGER_ERROR(
                        "atomic_write_json: MoveFileW fallback failed for '{}' -> '{}'. Error:{}",
                        tmp_full.string(), target.string(), last_error);
                    DeleteFileW(tmp_full_w.c_str());
                    if (ec)
                        *ec = std::make_error_code(std::errc::io_error);
                    return;
                }
            }
            else
            {
                LOGGER_ERROR(
                    "atomic_write_json: ReplaceFileW failed for '{}' after retries. Error:{}",
                    target.string(), last_error);
                DeleteFileW(tmp_full_w.c_str());
                if (ec)
                    *ec = std::make_error_code(std::errc::io_error);
                return;
            }
        }

        if (ec)
            *ec = std::error_code{};
        return;
    }
    catch (const std::exception &ex)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: exception: {}", ex.what());
        return;
    }
    catch (...)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: unknown error");
        return;
    }
#else
    // POSIX implementation
    try
    {
        std::filesystem::path parent = target.parent_path();
        if (parent.empty())
            parent = ".";
        std::error_code create_ec;
        fs::create_directories(parent, create_ec);
        if (create_ec)
        {
            if (ec)
                *ec = create_ec;
            LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(),
                         create_ec.message());
            return;
        }

        struct stat lstat_buf;
        if (lstat(target.c_str(), &lstat_buf) == 0)
        {
            if (S_ISLNK(lstat_buf.st_mode))
            {
                if (ec)
                    *ec = std::make_error_code(std::errc::operation_not_permitted);
                LOGGER_ERROR("atomic_write_json: target '{}' is a symbolic link, refusing to write",
                             target.string());
                return;
            }
        }

        std::string dir = parent.string();
        if (dir.empty())
            dir = ".";

        std::string filename = target.filename().string();
        std::string tmpl = dir + "/" + filename + ".tmp.XXXXXX";
        std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
        tmpl_buf.push_back('\0');

        int fd = mkstemp(tmpl_buf.data());
        if (fd == -1)
        {
            int errnum = errno;
            if (ec)
                *ec = std::make_error_code(static_cast<std::errc>(errnum));
            LOGGER_ERROR("atomic_write_json: mkstemp failed for '{}'. Error: {}", tmpl_buf.data(),
                         std::strerror(errnum));
            return;
        }

        std::string tmp_path = tmpl_buf.data();

        try
        {
            std::string out = j.dump(4);
            const char *buf = out.data();
            size_t toWrite = out.size();
            size_t written = 0;
            while (toWrite > 0)
            {
                ssize_t w = ::write(fd, buf + written, toWrite);
                if (w < 0)
                {
                    int errnum = errno;
                    ::close(fd);
                    ::unlink(tmp_path.c_str());
                    if (ec)
                        *ec = std::make_error_code(static_cast<std::errc>(errnum));
                    LOGGER_ERROR("atomic_write_json: write failed for '{}'. Error: {}", tmp_path,
                                 std::strerror(errnum));
                    return;
                }
                written += static_cast<size_t>(w);
                toWrite -= static_cast<size_t>(w);
            }

            if (::fsync(fd) != 0)
            {
                int errnum = errno;
                ::close(fd);
                ::unlink(tmp_path.c_str());
                if (ec)
                    *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: fsync(file) failed for '{}'. Error: {}", tmp_path,
                             std::strerror(errnum));
                return;
            }

            struct stat st;
            if (stat(target.c_str(), &st) == 0)
            {
                if (fchmod(fd, st.st_mode) != 0)
                {
                    int errnum = errno;
                    ::close(fd);
                    ::unlink(tmp_path.c_str());
                    if (ec)
                        *ec = std::make_error_code(static_cast<std::errc>(errnum));
                    LOGGER_ERROR("atomic_write_json: fchmod failed for '{}'. Error: {}", tmp_path,
                                 std::strerror(errnum));
                    return;
                }
            }

            if (::close(fd) != 0)
            {
                int errnum = errno;
                ::unlink(tmp_path.c_str());
                if (ec)
                    *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: close failed for '{}'. Error: {}", tmp_path,
                             std::strerror(errnum));
                return;
            }
            fd = -1;

            if (std::rename(tmp_path.c_str(), target.c_str()) != 0)
            {
                int errnum = errno;
                ::unlink(tmp_path.c_str());
                if (ec)
                    *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: rename failed for '{}'. Error: {}",
                             target.string(), std::strerror(errnum));
                return;
            }

            int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
            if (dfd >= 0)
            {
                if (::fsync(dfd) != 0)
                {
                    int errnum = errno;
                    ::close(dfd);
                    if (ec)
                        *ec = std::make_error_code(static_cast<std::errc>(errnum));
                    LOGGER_ERROR("atomic_write_json: fsync(dir) failed for '{}'. Error: {}", dir,
                                 std::strerror(errnum));
                    return;
                }
                ::close(dfd);
            }
            else
            {
                int errnum = errno;
                if (ec)
                    *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: open(dir) failed for fsync: '{}'. Error: {}", dir,
                             std::strerror(errnum));
                return;
            }

            if (ec)
                *ec = std::error_code{};
            return;
        }
        catch (...)
        {
            ::unlink(tmp_path.c_str());
            if (ec)
                *ec = std::make_error_code(std::errc::io_error);
            LOGGER_ERROR("atomic_write_json: unknown exception during POSIX write");
            return;
        }
    }
    catch (const std::exception &ex)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: exception: {}", ex.what());
        return;
    }
    catch (...)
    {
        if (ec)
            *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: unknown error");
        return;
    }
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

ModuleDef JsonConfig::GetLifecycleModule()
{
    ModuleDef module("pylabhub::utils::JsonConfig");
    // JsonConfig depends on FileLock and Logger being available.
    module.add_dependency("pylabhub::utils::FileLock");
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_jsonconfig_startup);
    module.set_shutdown(&do_jsonconfig_shutdown, 1000);
    return module;
}

} // namespace pylabhub::utils
