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
#include <atomic>

#if defined(PLATFORM_WIN64)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/file.h>
#endif

#include "utils/JsonConfig.hpp"
#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"


namespace pylabhub::utils
{
namespace fs = std::filesystem;

// Module-level flag to indicate if the JsonConfig has been initialized.
static std::atomic<bool> g_jsonconfig_initialized{false};

struct JsonConfig::Impl
{
    std::filesystem::path configPath;
    nlohmann::json data = nlohmann::json::object();
    std::mutex initMutex;
    std::atomic<bool> dirty{false};
    Impl() = default;
    ~Impl() = default;
};

struct JsonConfig::ReadLock::Impl
{
    JsonConfig *owner = nullptr;
    ReadLock::Impl *self_tag_ptr = nullptr;
};

struct JsonConfig::WriteLock::Impl
{
    JsonConfig *owner = nullptr;
    bool committed = false;
    WriteLock::Impl *self_tag_ptr = nullptr;
};

// ----------------- JsonConfig public methods -----------------
JsonConfig::JsonConfig() noexcept : pImpl(std::make_unique<Impl>()) {
    if (!lifecycle_initialized()) {
        fmt::print(stderr, "FATAL: JsonConfig created before its module was initialized via LifecycleManager. Aborting.\n");
        std::abort();
    }
}
JsonConfig::JsonConfig(const std::filesystem::path &configFile, bool createIfMissing,
                       std::error_code *ec) noexcept : pImpl(std::make_unique<Impl>()) 
{
    if (!lifecycle_initialized()) {
        fmt::print(stderr, "FATAL: JsonConfig created before its module was initialized via LifecycleManager. Aborting.\n");
        std::abort();
    }
    if (!init(configFile, createIfMissing, ec))
    {
        // init populates ec; ctor remains noexcept
    }
}
JsonConfig::~JsonConfig() {}
JsonConfig::JsonConfig(JsonConfig &&other) noexcept : pImpl(std::move(other.pImpl)) {}
JsonConfig &JsonConfig::operator=(JsonConfig &&other) noexcept
{
    pImpl = std::move(other.pImpl);
    return *this;
}



std::filesystem::path JsonConfig::config_path() const noexcept
{
    if (!pImpl) return {};
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    return std::filesystem::path(pImpl->configPath);
}

bool JsonConfig::is_initialized() const noexcept
{
    if (!pImpl) return false;
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    return !pImpl->configPath.empty();
}

bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing,
                      std::error_code *ec) noexcept
{
    try
    {
        if (!pImpl) pImpl = std::make_unique<Impl>();
        {
            std::lock_guard<std::mutex> g(pImpl->initMutex);

            #if PYLABHUB_IS_POSIX
            struct stat lstat_buf;
            if (lstat(configFile.c_str(), &lstat_buf) == 0)
            {
                if (S_ISLNK(lstat_buf.st_mode))
                {
                    if (ec) *ec = std::make_error_code(std::errc::operation_not_permitted);
                    LOGGER_ERROR("JsonConfig::init: target '{}' is a symbolic link, refusing to initialize.", configFile.string());
                    return false;
                }
            }
            #endif

            pImpl->configPath = configFile;

            if (createIfMissing)
            {
                std::error_code lfs;
                if (!fs::exists(configFile, lfs))
                {
                    nlohmann::json empty = nlohmann::json::object();
                    atomic_write_json(configFile, empty, ec);
                    if (ec && *ec) return false;
                }
            }
        }
        return reload(ec);
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::init: exception during init: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

bool JsonConfig::private_load_from_disk_unsafe(std::error_code* ec) noexcept
{
    try
    {
        std::ifstream in(pImpl->configPath);
        if (!in.is_open())
        {
            pImpl->data = nlohmann::json::object();
            pImpl->dirty.store(false, std::memory_order_release);
            if (ec) *ec = std::error_code{};
            return true;
        }

        nlohmann::json newdata;
        in >> newdata;

        pImpl->data = std::move(newdata);
        pImpl->dirty.store(false, std::memory_order_release);

        if (ec) *ec = std::error_code{};
        return true;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::private_load_from_disk_unsafe: exception during load: {}",
                     ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
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

        FileLock fl(pImpl->configPath, ResourceType::File, LockMode::Blocking);
        if (!fl.valid())
        {
            if (ec)
                *ec = fl.error_code();
            return false;
        }

        if (!private_load_from_disk_unsafe(ec))
        {
            LOGGER_ERROR("JsonConfig::reload: failed to load {} from disk", pImpl->configPath.string());
            return false;
        }
        return true;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::reload: exception during reload: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

bool JsonConfig::private_commit_to_disk_unsafe(std::error_code* ec) noexcept
{
    try
    {
        nlohmann::json snapshot = pImpl->data;
        atomic_write_json(pImpl->configPath, snapshot, ec);
        if (ec && *ec) return false;
        pImpl->dirty.store(false, std::memory_order_release);
        return true;
    }
    catch(...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
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

        FileLock fl(pImpl->configPath, ResourceType::File, LockMode::Blocking);
        if (!fl.valid())
        {
            if (ec) *ec = fl.error_code();
            return false;
        }

        if (!private_commit_to_disk_unsafe(ec))
        {
            LOGGER_ERROR("JsonConfig::save: failed to save {} to disk", pImpl->configPath.string());
            return false;
        }
        return true;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::save: exception during save: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

// Guard constructors / destructors / accessors
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
    if (!d_ || !d_->owner) {
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
    return d_->owner->pImpl->data;
}

bool JsonConfig::WriteLock::commit(std::error_code *ec) noexcept
{
    if (!d_ || !d_->owner)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    bool ok = d_->owner->private_commit_to_disk_unsafe(ec);
    if (ok) d_->committed = true;
    return ok;
}


// Factory functions
std::optional<JsonConfig::ReadLock> JsonConfig::lock_for_read(std::error_code *ec) const noexcept
{
    if (!pImpl)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (pImpl->configPath.empty())
        {
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return std::nullopt;
        }
    }

    JsonConfig::ReadLock r;
    r.d_ = std::make_unique<JsonConfig::ReadLock::Impl>();
    r.d_->owner = const_cast<JsonConfig*>(this);
    if (ec) *ec = std::error_code{};
    return r;
}

std::optional<JsonConfig::WriteLock> JsonConfig::lock_for_write(std::error_code *ec) noexcept
{
    if (!pImpl)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (pImpl->configPath.empty())
        {
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return std::nullopt;
        }
    }

    JsonConfig::WriteLock w;
    w.d_ = std::make_unique<JsonConfig::WriteLock::Impl>();
    w.d_->owner = this;

    if (ec) *ec = std::error_code{};
    return w;
}

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
            LOGGER_ERROR("atomic_write_json: CreateFileW(temp) failed for '{}'. Error:{}", tmp_full.string(), err);
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
            LOGGER_ERROR("atomic_write_json: WriteFile failed for '{}'. Error:{}", tmp_full.string(), err);
            return;
        }

        if (!FlushFileBuffers(h))
        {
            DWORD err = GetLastError();
            CloseHandle(h);
            DeleteFileW(tmp_full_w.c_str());
            if (ec) *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: FlushFileBuffers failed for '{}'. Error:{}", tmp_full.string(), err);
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
            // If sharing violation, wait and retry
            if (last_error != ERROR_SHARING_VIOLATION) break;
            Sleep(REPLACE_DELAY_MS);
        }

        if (!replaced)
        {
            if(last_error == ERROR_FILE_NOT_FOUND) {
                // If the target file does not exist, try MoveFileEx as a fallback
                if (!MoveFileExW(tmp_full_w.c_str(), target_w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    last_error = GetLastError();
                    LOGGER_ERROR("atomic_write_json: MoveFileW fallback failed for '{}' -> '{}'. Error:{}", tmp_full.string(), target.string(), last_error);
                    DeleteFileW(tmp_full_w.c_str());
                    if (ec) *ec = std::make_error_code(std::errc::io_error);
                    return;
                } 
            }
            else {
                LOGGER_ERROR("atomic_write_json: ReplaceFileW failed for '{}' after retries. Error:{}", target.string(), last_error);
                DeleteFileW(tmp_full_w.c_str());
                if (ec) *ec = std::make_error_code(std::errc::io_error);
                return;
            }
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

            if (std::rename(tmp_path.c_str(), target.c_str()) != 0)
            {
                int errnum = errno;
                ::unlink(tmp_path.c_str());
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: rename failed for '{}'. Error: {}", target.string(), std::strerror(errnum));
                return;
            }

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

// Lifecycle Integration
bool JsonConfig::lifecycle_initialized() noexcept {
    return g_jsonconfig_initialized.load(std::memory_order_acquire);
}

namespace {
void do_jsonconfig_startup(const char* arg) {
    (void)arg;
    g_jsonconfig_initialized.store(true, std::memory_order_release);
}
void do_jsonconfig_shutdown(const char* arg) {
    (void)arg;
    g_jsonconfig_initialized.store(false, std::memory_order_release);
}
} // namespace

ModuleDef JsonConfig::GetLifecycleModule() {
    ModuleDef module("pylabhub::utils::JsonConfig");
    // JsonConfig depends on FileLock and Logger being available.
    module.add_dependency("pylabhub::utils::FileLockCleanup");
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_jsonconfig_startup);
    module.set_shutdown(&do_jsonconfig_shutdown, 1000);
    return module;
}

} // namespace pylabhub::utils
