/**
 * @file JsonConfig.cpp
 * @brief Implementation of JsonConfig public API and platform-aware atomic write.
 *
 * Detailed behavior:
 *  - `with_json_write_impl` acquires `initMutex` then exclusive `rwMutex` to allow
 *    a safe modify-and-save pattern. The function captures errors via std::error_code.
 *  - `save` snapshots under shared lock, then calls atomic_write_json which performs
 *    platform-specific durable replace semantics (POSIX and Windows implemented below).
 *
 * All public methods avoid throwing. Any thrown exceptions are caught and converted
 * into std::error_code when an `ec` pointer is provided; otherwise the method returns false.
 */
#include "platform.hpp"
#include "utils/JsonConfig.hpp"

#include <fstream>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <fstream>

#if defined(PLATFORM_WIN64)
#include <windows.h>
#include <winbase.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#endif

using namespace pylabhub::basics;

namespace pylabhub::utils
{

namespace fs = std::filesystem;

// Private implementation
struct JsonConfig::Impl
{
    std::filesystem::path configPath;
    nlohmann::json data = nlohmann::json::object();

    // Structural lock: guards lifecycle and operations that change file/structure
    std::mutex initMutex;

    // Data lock: permits shared reads / exclusive writes
    mutable std::shared_mutex rwMutex;

    std::atomic<bool> dirty{false};

    Impl() = default;
    ~Impl() = default;
};

// ---------------- Constructors / destructor ----------------

JsonConfig::JsonConfig() noexcept : pImpl(std::make_unique<Impl>()) {}

JsonConfig::JsonConfig(const std::filesystem::path &configFile, bool createIfMissing,
                       std::error_code *ec) : pImpl(std::make_unique<Impl>())
{
    if (!init(configFile, createIfMissing, ec))
    {
        // init already sets ec on failure
    }
}

JsonConfig::~JsonConfig() = default;
JsonConfig::JsonConfig(JsonConfig &&) noexcept = default;
JsonConfig &JsonConfig::operator=(JsonConfig &&) noexcept = default;

// ---------------- Simple accessors ----------------

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

// ---------------- init / reload / save ----------------

bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing,
                      std::error_code *ec)
{
    try
    {
        if (!pImpl) pImpl = std::make_unique<Impl>();
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        pImpl->configPath = configFile;

        if (createIfMissing)
        {
            // Use FileLock to avoid TOCTOU when creating
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
                // write empty object atomically
                nlohmann::json empty = nlohmann::json::object();
                atomic_write_json(configFile, empty, ec);
                if (ec && *ec)
                    return false;
            }
        }

        // load file into memory
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

        // Acquire advisory cross-process lock for consistent read
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
            // treat missing file as empty
            std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
            pImpl->data = nlohmann::json::object();
            pImpl->dirty.store(false, std::memory_order_release);
            if (ec) *ec = std::error_code{};
            return true;
        }

        nlohmann::json newdata;
        in >> newdata;
        {
            std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
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

        // snapshot under shared lock
        nlohmann::json snapshot;
        {
            std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
            snapshot = pImpl->data;
        }

        atomic_write_json(pImpl->configPath, snapshot, ec);
        if (ec && *ec)
        {
            return false;
        }

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

// ---------------- scoped accessors ----------------

bool JsonConfig::with_json_write_impl(std::function<void(nlohmann::json &)> fn,
                                     std::error_code *ec) noexcept
{
    if (!pImpl)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    const void *key = static_cast<const void *>(this);
    if (RecursionGuard::is_recursing(key))
    {
        if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        LOGGER_WARN("JsonConfig::with_json_write_impl - recursive call detected; refusing to re-enter.");
        return false;
    }
    RecursionGuard guard(key);

    try
    {
        // Acquire structural lock
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (pImpl->configPath.empty())
        {
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }

        // Exclusive access to JSON
        {
            std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
            fn(pImpl->data); // user callback; must be exception-safe
            pImpl->dirty.store(true, std::memory_order_release);
        }

        // Persist changes
        if (!save(ec))
        {
            // save() already set ec
            return false;
        }

        if (ec) *ec = std::error_code{};
        return true;
    }
    catch (const std::exception &ex)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::with_json_write_impl: callback threw: {}", ex.what());
        return false;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        LOGGER_ERROR("JsonConfig::with_json_write_impl: callback threw unknown");
        return false;
    }
}

bool JsonConfig::with_json_read_impl(std::function<void(nlohmann::json const &)> fn,
                                    std::error_code *ec) const noexcept
{
    if (!pImpl)
    {
        if (ec) *ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    const void *key = static_cast<const void *>(this);
    if (RecursionGuard::is_recursing(key))
    {
        if (ec) *ec = std::make_error_code(std::errc::resource_deadlock_would_occur);
        LOGGER_WARN("JsonConfig::with_json_read_impl - recursive call detected; refusing to re-enter.");
        return false;
    }
    RecursionGuard guard(key);

    try
    {
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (pImpl->configPath.empty())
        {
            if (ec) *ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }

        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        fn(pImpl->data);
        if (ec) *ec = std::error_code{};
        return true;
    }
    catch (...)
    {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return false;
    }
}

// ---------------- atomic_write_json (platform-aware) ----------------

/**
 * @brief Write json object atomically to `target` path.
 *
 * On POSIX:
 *  - create a temporary file in same directory using mkstemp()
 *  - write data, fsync the file, copy permissions from existing target if present
 *  - close file descriptor, flock target, rename tmp->target, fsync dir
 *
 * On Windows:
 *  - create temp file in same directory via CreateFileW
 *  - write, FlushFileBuffers, CloseHandle
 *  - Lock target region, ReplaceFileW with WRITE_THROUGH, release lock
 *
 * This function catches internal errors and reports them via ec if provided.
 * It does not throw.
 */
void JsonConfig::atomic_write_json(const std::filesystem::path &target, const nlohmann::json &j,
                                   std::error_code *ec)
{
    if (ec) *ec = std::error_code{};
#if defined(PLATFORM_WIN64)
    // Windows implementation using wide paths

    try
    {
        std::filesystem::path parent = target.parent_path();
        if (parent.empty())
            parent = ".";

        // Ensure parent dir exists
        std::error_code create_ec;
        fs::create_directories(parent, create_ec);
        if (create_ec)
        {
            if (ec) *ec = create_ec;
            LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(), create_ec.message());
            return;
        }

        // Build tmp filename
        std::wstring filename = target.filename().wstring();
        // create a unique suffix using process id + timestamp
        std::wstring tmpname = filename + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());

        std::filesystem::path tmp_full = parent / std::filesystem::path(tmpname);
        std::wstring tmp_full_w = tmp_full.wstring();
        std::wstring target_w = target.wstring();

        // Create temp file
        HANDLE h = CreateFileW(tmp_full_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            if (ec) *ec = std::make_error_code(static_cast<std::errc>(err));
            LOGGER_ERROR("atomic_write_json: CreateFileW(temp) failed for '{}'. Error: {}", tmp_full.string(), err);
            return;
        }

        // Write contents
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

        // Try to replace file atomically (ReplaceFileW). We'll retry on sharing violation.
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
            // Attempt cleanup
            DeleteFileW(tmp_full_w.c_str());
            return;
        }

        // Success
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
        if (parent.empty())
            parent = ".";

        std::error_code create_ec;
        fs::create_directories(parent, create_ec);
        if (create_ec)
        {
            if (ec) *ec = create_ec;
            LOGGER_ERROR("atomic_write_json: create_directories failed for {}: {}", parent.string(), create_ec.message());
            return;
        }

        // Symlink check: refuse if target is a symlink
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
            // Write
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

            // fsync file
            if (::fsync(fd) != 0)
            {
                int errnum = errno;
                ::close(fd);
                ::unlink(tmp_path.c_str());
                if (ec) *ec = std::make_error_code(static_cast<std::errc>(errnum));
                LOGGER_ERROR("atomic_write_json: fsync(file) failed for '{}'. Error: {}", tmp_path, std::strerror(errnum));
                return;
            }

            // Copy permissions from existing target if exists
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

            // Acquire exclusive flock on target to prevent races during rename
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

            // rename tmp -> target
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

            // release lock and close
            ::flock(target_fd, LOCK_UN);
            ::close(target_fd);

            // fsync parent dir
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
            // cleanup on unexpected exception
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
