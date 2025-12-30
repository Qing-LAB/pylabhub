// JsonConfig.cpp
#include "platform.hpp"

#include <fstream>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <optional>
#include <utility>

#if defined(PLATFORM_WIN64)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/file.h>
#endif

#include "utils/JsonConfig.hpp"

namespace pylabhub::utils
{
namespace fs = std::filesystem;

// Use shared_timed_mutex to support timed try-lock_for semantics
struct JsonConfig::Impl
{
    std::filesystem::path configPath;
    nlohmann::json data = nlohmann::json::object();

    // structural lock for init/reload/save and to protect configPath
    std::mutex initMutex;

    // data lock (timed capable)
    std::shared_timed_mutex rwMutex;

    std::atomic<bool> dirty{false};

    Impl() = default;
    ~Impl() = default;
};

// ----------------- ReadLock::Impl -----------------
struct JsonConfig::ReadLock::Impl
{
    // pointer to underlying Impl (must outlive this guard)
    JsonConfig::Impl *owner = nullptr;
    // holds shared ownership of the data lock while this guard is alive
    std::shared_lock<std::shared_timed_mutex> rlock;

    ReadLock::Impl *self_tag_ptr = nullptr; // unused, convenience for debugging
};

// ----------------- WriteLock::Impl -----------------
struct JsonConfig::WriteLock::Impl
{
    // pointer to underlying Impl (must outlive this guard)
    JsonConfig::Impl *owner = nullptr;

    // We keep both the "init" mutex (unique_lock) and the write lock for the lifetime
    std::unique_lock<std::mutex> initLock;                        // holds initMutex
    std::unique_lock<std::shared_timed_mutex> writeLock;          // holds rwMutex

    bool committed = false;

    WriteLock::Impl *self_tag_ptr = nullptr; // unused, convenience for debugging
};

// ----------------- JsonConfig public methods -----------------
JsonConfig::JsonConfig() noexcept : pImpl(std::make_unique<Impl>()) {}
JsonConfig::JsonConfig(const std::filesystem::path &configFile, bool createIfMissing,
                       std::error_code *ec) noexcept : pImpl(std::make_unique<Impl>())
{
    if (!init(configFile, createIfMissing, ec))
    {
        // init populates ec; ctor remains noexcept
    }
}
JsonConfig::~JsonConfig() = default;
JsonConfig::JsonConfig(JsonConfig &&) noexcept = default;
JsonConfig &JsonConfig::operator=(JsonConfig &&) noexcept = default;

bool JsonConfig::is_initialized() const noexcept
{
    if (!pImpl) return false;
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    return !pImpl->configPath.empty();
}

std::filesystem::path JsonConfig::config_path() const noexcept
{
    if (!pImpl) return {};
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    return pImpl->configPath;
}

// init / reload / save largely preserved from your prior implementation
bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing,
                      std::error_code *ec) noexcept
{
    try
    {
        if (!pImpl) pImpl = std::make_unique<Impl>();
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        pImpl->configPath = configFile;

        if (createIfMissing)
        {
            FileLock fl(configFile, ResourceType::File, LockMode::NonBlocking);
            if (!fl.valid())
            {
                if (ec) *ec = fl.error_code();
                LOGGER_ERROR("JsonConfig::init: cannot acquire lock for {} code={} msg=\"{}\"",
                             configFile.string(), fl.error_code().value(), fl.error_code().message());
                return false;
            }

            std::error_code lfs;
            if (!fs::exists(configFile, lfs))
            {
                nlohmann::json empty = nlohmann::json::object();
                atomic_write_json(configFile, empty, ec);
                if (ec && *ec) return false;
            }
        }

        return reload(ec);
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::init: exception: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::init: unknown exception");
        return false;
    }
}

bool JsonConfig::reload(std::error_code *ec) noexcept
{
    try
    {
        if (!pImpl)
        {
            if (ec) *ec = std::make_error_code(std::errc::not_connected);
            return false;
        }

        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (pImpl->configPath.empty())
        {
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }

        FileLock fl(pImpl->configPath, ResourceType::File, LockMode::NonBlocking);
        if (!fl.valid())
        {
            if (ec) *ec = fl.error_code();
            LOGGER_ERROR("JsonConfig::reload: failed to acquire lock for {} code={} msg=\"{}\"",
                         pImpl->configPath.string(), fl.error_code().value(), fl.error_code().message());
            return false;
        }

        std::ifstream in(pImpl->configPath);
        if (!in.is_open())
        {
            std::unique_lock<std::shared_timed_mutex> w(pImpl->rwMutex);
            pImpl->data = nlohmann::json::object();
            pImpl->dirty.store(false, std::memory_order_release);
            if (ec) *ec = std::error_code{};
            return true;
        }

        nlohmann::json newdata;
        in >> newdata;
        {
            std::unique_lock<std::shared_timed_mutex> w(pImpl->rwMutex);
            pImpl->data = std::move(newdata);
            pImpl->dirty.store(false, std::memory_order_release);
        }

        if (ec) *ec = std::error_code{};
        return true;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::reload: exception: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::reload: unknown exception");
        return false;
    }
}

bool JsonConfig::save(std::error_code *ec) noexcept
{
    try
    {
        if (!pImpl)
        {
            if (ec) *ec = std::make_error_code(std::errc::not_connected);
            return false;
        }

        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (pImpl->configPath.empty())
        {
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }

        nlohmann::json snapshot;
        {
            std::shared_lock<std::shared_timed_mutex> r(pImpl->rwMutex);
            snapshot = pImpl->data;
        }

        atomic_write_json(pImpl->configPath, snapshot, ec);
        if (ec && *ec) return false;

        pImpl->dirty.store(false, std::memory_order_release);
        return true;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::save: exception: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::save: unknown exception");
        return false;
    }
}

// ----------------- Guard constructors / destructors / accessors -----------------

// ReadLock
JsonConfig::ReadLock::ReadLock() noexcept = default;
JsonConfig::ReadLock::ReadLock(ReadLock &&other) noexcept = default;
JsonConfig::ReadLock &JsonConfig::ReadLock::operator=(ReadLock &&other) noexcept = default;
JsonConfig::ReadLock::~ReadLock() = default;

const nlohmann::json &JsonConfig::ReadLock::json() const noexcept
{
    if (!d_ || !d_->owner) return nlohmann::json::value_t::null; // unreachable ideally
    return d_->owner->data;
}

// WriteLock
JsonConfig::WriteLock::WriteLock() noexcept = default;
JsonConfig::WriteLock::WriteLock(WriteLock &&) noexcept = default;
JsonConfig::WriteLock &JsonConfig::WriteLock::operator=(WriteLock &&) noexcept = default;
JsonConfig::WriteLock::~WriteLock() = default;

nlohmann::json &JsonConfig::WriteLock::json() noexcept
{
    if (!d_ || !d_->owner)
    {
        static nlohmann::json dummy = nlohmann::json::object();
        return dummy;
    }
    return d_->owner->data;
}

bool JsonConfig::WriteLock::commit(std::error_code *ec) noexcept
{
    if (!d_ || !d_->owner)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    // commit while still holding both initLock and writeLock (guaranteed by construction).
    try
    {
        // make snapshot
        nlohmann::json snapshot = d_->owner->data;
        // call atomic_write_json under protection of initLock being held (we already hold it)
        atomic_write_json(d_->owner->configPath, snapshot, ec);
        if (ec && *ec) return false;
        d_->owner->dirty.store(false, std::memory_order_release);
        d_->committed = true;
        if (ec) *ec = std::error_code{};
        return true;
    }
    catch (const std::exception &ex)
    {
        LOGGER_ERROR("JsonConfig::WriteLock::commit: exception: {}", ex.what());
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::WriteLock::commit: unknown exception");
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

// ----------------- Factory functions (lock_for_read / lock_for_write) -----------------
std::optional<JsonConfig::ReadLock> JsonConfig::lock_for_read(std::error_code *ec) const noexcept
{
    if (!pImpl)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return std::nullopt;
    }

    // Acquire initMutex briefly to validate state (mirrors previous behavior)
    {
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (pImpl->configPath.empty())
        {
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return std::nullopt;
        }
    }

    // Acquire shared lock on data
    JsonConfig::ReadLock r;
    r.d_ = std::make_unique<JsonConfig::ReadLock::Impl>();
    r.d_->owner = pImpl.get();
    try
    {
        r.d_->rlock = std::shared_lock<std::shared_timed_mutex>(pImpl->rwMutex);
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::resource_unavailable_try_again);
        LOGGER_ERROR("JsonConfig::lock_for_read: exception acquiring shared lock: {}", ex.what());
        return std::nullopt;
    }
    if (ec) *ec = std::error_code{};
    return r;
}

std::optional<JsonConfig::WriteLock> JsonConfig::lock_for_write(std::chrono::milliseconds timeout,
                                                               std::error_code *ec) noexcept
{
    if (!pImpl)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return std::nullopt;
    }

    // Acquire initMutex first and keep it for lifetime — mirrors previous ordering (initMutex then rwMutex).
    JsonConfig::WriteLock w;
    w.d_ = std::make_unique<JsonConfig::WriteLock::Impl>();
    w.d_->owner = pImpl.get();

    try
    {
        // lock initMutex (unique_lock movable)
        w.d_->initLock = std::unique_lock<std::mutex>(pImpl->initMutex);

        // prepare writeLock (deferred)
        w.d_->writeLock = std::unique_lock<std::shared_timed_mutex>(pImpl->rwMutex, std::defer_lock);

        if (timeout.count() == 0)
        {
            // block until acquired
            w.d_->writeLock.lock();
        }
        else
        {
            // timed try
            if (!w.d_->writeLock.try_lock_for(timeout))
            {
                // failed to acquire
                if (ec) *ec = std::make_error_code(std::errc::timed_out);
                LOGGER_WARN("JsonConfig::lock_for_write: timed out acquiring write lock");
                return std::nullopt;
            }
        }

        // At this point we hold initLock + writeLock. Validate configPath.
        if (pImpl->configPath.empty())
        {
            // release locks automatically via destructor of unique_lock in w.d_
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return std::nullopt;
        }

        if (ec) *ec = std::error_code{};
        return w;
    }
    catch (const std::system_error &se)
    {
        if (ec) *ec = se.code();
        LOGGER_ERROR("JsonConfig::lock_for_write: system_error: {}", se.what());
        return std::nullopt;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::resource_unavailable_try_again);
        LOGGER_ERROR("JsonConfig::lock_for_write: exception: {}", ex.what());
        return std::nullopt;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::lock_for_write: unknown exception");
        return std::nullopt;
    }
}

// ----------------- atomic_write_json implementation (POSIX + Windows). -----------------
// Copied/adjusted from previous implementation; reports errors via ec (no exceptions escape) 
void JsonConfig::atomic_write_json(const std::filesystem::path &target,
                                   const nlohmann::json &j,
                                   std::error_code *ec) noexcept
{
    if (ec) *ec = std::error_code{};
#if defined(PLATFORM_WIN64)
    // Windows implementation
    try
    {
        std::filesystem::path parent = target.parent_path();
        if (parent.empty()) parent = ".";
        std::error_code create_ec;
        fs::create_directories(parent, create_ec);
        if (create_ec)
        {
            if (ec) *ec = create_ec;
            LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(), create_ec.message());
            return;
        }

        std::wstring filename = target.filename().wstring();
        std::wstring tmpname = filename + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        std::filesystem::path tmp_full = parent / std::filesystem::path(tmpname);
        std::wstring tmp_full_w = tmp_full.wstring();
        std::wstring target_w = target.wstring();

        HANDLE h = CreateFileW(tmp_full_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            if (ec) *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: CreateFileW(temp) failed for '{}'. Error: {}", tmp_full.string(), err);
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
            if (ec) *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: WriteFile failed for '{}'. Error: {}", tmp_full.string(), err);
            return;
        }

        if (!FlushFileBuffers(h))
        {
            DWORD err = GetLastError();
            CloseHandle(h);
            DeleteFileW(tmp_full_w.c_str());
            if (ec) *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: FlushFileBuffers failed for '{}'. Error: {}", tmp_full.string(), err);
            return;
        }

        CloseHandle(h);

        const int REPLACE_RETRIES = 5;
        const int REPLACE_DELAY_MS = 100;
        BOOL replaced = FALSE;
        DWORD last_error = 0;
        for (int i = 0; i < REPLACE_RETRIES; ++i)
        {
            replaced = ReplaceFileW(target_w.c_str(), tmp_full_w.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
            if (replaced) break;
            last_error = GetLastError();
            if (last_error != ERROR_SHARING_VIOLATION) break;
            Sleep(REPLACE_DELAY_MS);
        }

        if (!replaced)
        {
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            LOGGER_ERROR("atomic_write_json: ReplaceFileW failed for '{}' after retries. Error: {}", target.string(), last_error);
            DeleteFileW(tmp_full_w.c_str());
            return;
        }

        if (ec) *ec = std::error_code{};
        return;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: exception: {}", ex.what());
        return;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: unknown error");
        return;
    }
#else
    // POSIX implementation
    try
    {
        std::filesystem::path parent = target.parent_path();
        if (parent.empty()) parent = ".";
        std::error_code create_ec;
        fs::create_directories(parent, create_ec);
        if (create_ec)
        {
            if (ec) *ec = create_ec;
            LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(), create_ec.message());
            return;
        }

        struct stat lstat_buf;
        if (lstat(target.c_str(), &lstat_buf) == 0)
        {
            if (S_ISLNK(lstat_buf.st_mode))
            {
                if (ec) *ec = std::make_error_code(std::errc::operation_not_permitted);
                LOGGER_ERROR("atomic_write_json: target '{}' is a symbolic link, refusing to write", target.string());
                return;
            }
        }

        std::string dir = parent.string();
        if (dir.empty()) dir = ".";

        std::string filename = target.filename().string();
        std::string tmpl = dir + "/" + filename + ".tmp.XXXXXX";
        std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
        tmpl_buf.push_back('\0');

        int fd = mkstemp(tmpl_buf.data());
        if (fd == -1)
        {
            int errnum = errno;
            if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
            LOGGER_ERROR("atomic_write_json: mkstemp failed for '{}'. Error: {}", tmpl_buf.data(), std::strerror(errnum));
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
                    if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                    LOGGER_ERROR("atomic_write_json: write failed for '{}'. Error: {}", tmp_path, std::strerror(errnum));
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
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: fsync(file) failed for '{}'. Error: {}", tmp_path, std::strerror(errnum));
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
                    if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                    LOGGER_ERROR("atomic_write_json: fchmod failed for '{}'. Error: {}", tmp_path, std::strerror(errnum));
                    return;
                }
            }

            if (::close(fd) != 0)
            {
                int errnum = errno;
                ::unlink(tmp_path.c_str());
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: close failed for '{}'. Error: {}", tmp_path, std::strerror(errnum));
                return;
            }
            fd = -1;

            int target_fd = ::open(target.c_str(), O_CREAT | O_RDWR, 0666);
            if (target_fd == -1)
            {
                int errnum = errno;
                ::unlink(tmp_path.c_str());
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: open(target) failed for '{}'. Error: {}", target.string(), std::strerror(errnum));
                return;
            }

            if (::flock(target_fd, LOCK_EX | LOCK_NB) != 0)
            {
                int errnum = errno;
                ::close(target_fd);
                ::unlink(tmp_path.c_str());
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: flock(target) failed for '{}'. Error: {}", target.string(), std::strerror(errnum));
                return;
            }

            if (std::rename(tmp_path.c_str(), target.c_str()) != 0)
            {
                int errnum = errno;
                ::flock(target_fd, LOCK_UN);
                ::close(target_fd);
                ::unlink(tmp_path.c_str());
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: rename failed for '{}'. Error: {}", target.string(), std::strerror(errnum));
                return;
            }

            ::flock(target_fd, LOCK_UN);
            ::close(target_fd);

            int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
            if (dfd >= 0)
            {
                if (::fsync(dfd) != 0)
                {
                    int errnum = errno;
                    ::close(dfd);
                    if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                    LOGGER_ERROR("atomic_write_json: fsync(dir) failed for '{}'. Error: {}", dir, std::strerror(errnum));
                    return;
                }
                ::close(dfd);
            }
            else
            {
                int errnum = errno;
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: open(dir) failed for fsync: '{}'. Error: {}", dir, std::strerror(errnum));
                return;
            }

            if (ec) *ec = std::error_code{};
            return;
        }
        catch (...)
        {
            ::unlink(tmp_path.c_str());
            if (ec) *ec = std::make_error_code(std::errc::io_error);
            LOGGER_ERROR("atomic_write_json: unknown exception during POSIX write");
            return;
        }
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: exception: {}", ex.what());
        return;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("atomic_write_json: unknown error");
        return;
    }
#endif
}

} // namespace pylabhub::utils
