/*******************************************************************************
 * @file JsonConfig.cpp
 * @brief Implementation of the non-template methods for JsonConfig.
 *
 * This file contains the implementation for the core logic of the JsonConfig
 * class, including file I/O, locking, and the platform-specific atomic write
 * helper function.
 ******************************************************************************/
#include "utils/JsonConfig.hpp"
#include "utils/PathUtil.hpp"
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <system_error>

#if defined(PLATFORM_WIN64)
#include <vector>
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h> // flock
#include <unistd.h>
#endif

namespace pylabhub::utils
{

namespace fs = std::filesystem;

using json = nlohmann::json;

// Constructors / dtor
JsonConfig::JsonConfig() noexcept : pImpl(std::make_unique<Impl>()) {}
JsonConfig::JsonConfig(const std::filesystem::path &configFile)
{
    init(configFile, false);
}
JsonConfig::~JsonConfig() = default;

bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing)
{
    // Ensure the implementation object exists.
    if (!pImpl)
        pImpl = std::make_unique<Impl>();
    // Lock to protect against concurrent init/reload/save operations.
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    pImpl->configPath = configFile;

    if (createIfMissing)
    {
        // Use a FileLock to prevent a Time-of-Check-to-Time-of-Use (TOCTOU) race
        // condition where another process could create the file between our `exists`
        // check and our `atomic_write_json` call.
        FileLock filelock(configFile, ResourceType::File, LockMode::NonBlocking);
        if (!filelock.valid())
        {
            [[maybe_unused]] auto e = filelock.error_code();
            LOGGER_ERROR("JsonConfig::init: cannot acquire lock for {} code={} msg=\"{}\"",
                         configFile.string().c_str(), e.value(), e.message().c_str());
            return false;
        }

        // If the file does not exist, create it with an empty JSON object.
        std::error_code ec;
        if (!std::filesystem::exists(configFile, ec))
        {
            try
            {
                atomic_write_json(configFile, json::object());
            }
            catch (const std::exception &ex)
            {
                LOGGER_ERROR("JsonConfig::init: failed to create file: {}", ex.what());
                return false;
            }
            catch (...)
            {
                LOGGER_ERROR("JsonConfig::init: unknown error creating file");
                return false;
            }
        }
        // `filelock` is released here by its destructor.
    }

    return reload_locked();
}

bool JsonConfig::save() noexcept
{
    try
    {
        // Fast-path optimization: If save() is called from within a with_json_write
        // callback on the same thread and instance, we already hold the `initMutex`.
        // Calling `save_locked` directly avoids a deadlock from trying to re-lock
        // the non-recursive mutex.
        const void *key = static_cast<const void *>(this);
        // The recursion guard checks if we are already inside a guarded method on this thread.
        if (RecursionGuard::is_recursing(key))
        {
            std::error_code ec;
            return save_locked(ec);
        }

        // Normal public save path: acquire the main lock, then call the locked implementation.
        if (!pImpl)
            return false;
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        if (!pImpl)
            return false;

        std::error_code ec;
        return save_locked(ec);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::save: exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::save: unknown exception");
        return false;
    }
}

bool JsonConfig::save_locked(std::error_code &ec)
{
    // Precondition: The caller MUST hold `pImpl->initMutex`.

    ec.clear();
    if (!pImpl)
    {
        ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    if (pImpl->configPath.empty())
    {
        LOGGER_ERROR("JsonConfig::save_locked: configPath not initialized (call init() first)");
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }

    // Optimization: If the in-memory data hasn't changed, skip the expensive disk write.
    if (!pImpl->dirty.load(std::memory_order_acquire))
    {
        // nothing to do
        return true;
    }

    // Acquire a non-blocking cross-process lock to prevent corruption from other processes.
    FileLock filelock(pImpl->configPath, ResourceType::File, LockMode::NonBlocking);
    if (!filelock.valid())
    {
        ec = filelock.error_code();
        LOGGER_ERROR("JsonConfig::save_locked: failed to acquire lock for {} code={} msg=\"{}\"",
                     pImpl->configPath.string().c_str(), ec.value(), ec.message().c_str());
        return false;
    }

    // Snapshot the data to be written. This is done under a shared read lock,
    // minimizing the time we block other reader threads.
    json toWrite;
    {
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        toWrite = pImpl->data;
    }

    try
    {
        atomic_write_json(pImpl->configPath, toWrite);
    }
    catch (const std::exception &ex)
    {
        LOGGER_ERROR("JsonConfig::save_locked: atomic_write_json failed: {}", ex.what());
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::save_locked: atomic_write_json unknown failure");
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }

    // On successful write, clear the dirty flag as memory and disk are now in sync.
    pImpl->dirty.store(false, std::memory_order_release);
    return true;
}

bool JsonConfig::reload() noexcept
{
    try
    {
        if (!pImpl)
            return false;
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        return reload_locked();
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::reload: exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::reload: unknown exception");
        return false;
    }
}

bool JsonConfig::reload_locked() noexcept
{
    // Precondition: Caller must hold pImpl->initMutex.
    try
    {
        if (pImpl->configPath.empty())
        {
            LOGGER_ERROR(
                "JsonConfig::reload_locked: configPath not initialized (call init() first)");
            return false;
        }

        // Acquire a non-blocking cross-process lock to ensure we read a consistent file.
        FileLock filelock(pImpl->configPath, ResourceType::File, LockMode::NonBlocking);
        if (!filelock.valid())
        {
            [[maybe_unused]] auto ec = filelock.error_code();
            LOGGER_ERROR(
                "JsonConfig::reload_locked: failed to acquire lock for {} code={} msg=\"{}\"",
                pImpl->configPath.string().c_str(), ec.value(), ec.message().c_str());
            return false;
        }

        // Read and parse the file.
        std::ifstream in(pImpl->configPath);
        if (!in.is_open())
        {
            // This is not an error if the file is legitimately not there (e.g., after init
            // with createIfMissing=false). In that case, we just have an empty config.
            // We only log an error if we expected it to exist.
            std::error_code ec;
            if (fs::exists(pImpl->configPath, ec))
            {
                LOGGER_ERROR("JsonConfig::reload_locked: cannot open file: {}",
                             pImpl->configPath.string().c_str());
            }
            // If the file doesn't exist, we just treat it as an empty JSON object.
            pImpl->data = json::object();
            pImpl->dirty.store(false, std::memory_order_release);
            return true;
        }

        json newdata;
        in >> newdata;
        if (in.good())
        {
            // It is possible that the file is empty, in which case the extraction
            // would fail. We should handle this gracefully.
            if (newdata.is_null())
            {
                newdata = json::object();
            }
        }
        else if (!in.eof())
        {
            LOGGER_ERROR("JsonConfig::reload_locked: parse/read error for {}",
                         pImpl->configPath.string().c_str());
            return false;
        }
        else
        {
            // File is empty, which is valid. Treat as an empty object.
            newdata = json::object();
        }

        // Atomically update the in-memory data.
        {
            std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
            pImpl->data = std::move(newdata);
            // Memory now matches disk, so the dirty flag can be cleared.
            pImpl->dirty.store(false, std::memory_order_release);
        }

        return true;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::reload_locked: exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::reload_locked: unknown exception");
        return false;
    }
}

bool JsonConfig::replace(const json &newData) noexcept
{
    try
    {
        // Ensure the implementation object exists.
        if (!pImpl)
            pImpl = std::make_unique<Impl>();
        // Lock to protect against concurrent init/reload/save operations.
        std::lock_guard<std::mutex> g(pImpl->initMutex);

        if (pImpl->configPath.empty())
        {
            LOGGER_ERROR("JsonConfig::replace: configPath not initialized (call init() first)");
            return false;
        }

        // Acquire a non-blocking cross-process lock before writing to disk.
        FileLock filelock(pImpl->configPath, ResourceType::File, LockMode::NonBlocking);
        if (!filelock.valid())
        {
            [[maybe_unused]] auto ec = filelock.error_code();
            LOGGER_ERROR("JsonConfig::replace: failed to acquire lock for {} code={} msg=\"{}\"",
                         pImpl->configPath.string().c_str(), ec.value(), ec.message().c_str());
            return false;
        }

        // Persist the new data to disk atomically. This may throw on failure.
        atomic_write_json(pImpl->configPath, newData);

        // On successful write, update the in-memory data and clear the dirty flag.
        {
            std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
            pImpl->data = newData;
            pImpl->dirty.store(false, std::memory_order_release);
        }

        return true;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::replace: exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::replace: unknown exception");
        return false;
    }
}

json JsonConfig::as_json() const noexcept
{
    try
    {
        // Return a copy of the data under a shared read lock.
        // This is thread-safe and allows concurrent reads.
        if (!pImpl)
            return json::object();
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        return pImpl->data;
    }
    catch (...)
    {
        return json::object();
    }
}

// ---------------- atomic_write_json implementation ----------------

void JsonConfig::atomic_write_json(const std::filesystem::path &target, const json &j)
{
#if defined(PLATFORM_WIN64)
    // Windows implementation
    LOGGER_DEBUG("atomic_write_json(WIN): Target: {}", target.string());

    std::filesystem::path parent = target.parent_path();
    if (parent.empty())
        parent = ".";

    // Ensure the target directory exists.
    std::error_code ec_dir;
    fs::create_directories(target.parent_path(), ec_dir);
    if (ec_dir)
    {
        throw std::runtime_error("atomic_write_json: create_directories failed: " +
                                 ec_dir.message());
    }
    LOGGER_DEBUG("atomic_write_json(WIN): Parent directory '{}' ensured.",
                 target.parent_path().string());

    std::wstring filename = target.filename().wstring();
    std::wstring tmpname = filename + L".tmp" + win32_make_unique_suffix();

    std::filesystem::path tmp_full = parent / std::filesystem::path(tmpname);

    // Convert to long paths to avoid MAX_PATH issues.
    std::wstring tmp_full_w = win32_to_long_path(tmp_full);
    std::wstring target_w = win32_to_long_path(target);

    LOGGER_DEBUG("atomic_write_json(WIN): Temp file path: {}", tmp_full.string());

    // Security: Check if the target is a reparse point (e.g., a symlink).
    DWORD attributes = GetFileAttributesW(target_w.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        throw std::runtime_error(
            "atomic_write_json: target path is a reparse point (e.g., symbolic link), "
            "refusing to write for security reasons.");
    }
    LOGGER_DEBUG("atomic_write_json(WIN): Reparse point check passed for target '{}'.",
                 target.string());

    // 1. Create and write to a temporary file.
    HANDLE h = CreateFileW(tmp_full_w.c_str(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error(
            fmt::format("atomic_write_json: CreateFileW(temp) failed: {}", GetLastError()));
    }
    LOGGER_DEBUG("atomic_write_json(WIN): Temp file '{}' created.", tmp_full.string());

    // RAII for handle
    auto close_handle = [&](HANDLE handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_handle)> handle_guard(h, close_handle);

    std::string out = j.dump(4);
    DWORD written = 0;
    if (!WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) ||
        static_cast<size_t>(written) != out.size())
    {
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(
            fmt::format("atomic_write_json: WriteFile failed: {}", GetLastError()));
    }
    LOGGER_DEBUG("atomic_write_json(WIN): Wrote {} bytes to temp file.", written);

    if (!FlushFileBuffers(h))
    {
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(
            fmt::format("atomic_write_json: FlushFileBuffers failed: {}", GetLastError()));
    }
    LOGGER_DEBUG("atomic_write_json(WIN): Flushed buffers for temp file.");

    handle_guard.reset(); // Close the handle before rename.

    // 2. Atomically replace the original file.
    if (!ReplaceFileW(target_w.c_str(),
                      tmp_full_w.c_str(),
                      nullptr,
                      REPLACEFILE_WRITE_THROUGH,
                      nullptr,
                      nullptr))
    {
        DWORD err = GetLastError();
        // If ReplaceFileW fails because the destination does not exist, this is a file
        // creation scenario. Fall back to a simple move/rename.
        if (err == ERROR_FILE_NOT_FOUND)
        {
            if (MoveFileW(tmp_full_w.c_str(), target_w.c_str()))
            {
                LOGGER_DEBUG(
                    "atomic_write_json(WIN): MoveFileW succeeded for new file '{}'.",
                    target.string());
                return; // Success
            }
            err = GetLastError(); // Update error to the one from MoveFileW
        }

        // If we are here, either ReplaceFileW failed for a reason other than
        // file-not-found, or the fallback MoveFileW also failed.
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(
            fmt::format("atomic_write_json: Replace/Move failed: {}", err));
    }
    LOGGER_DEBUG("atomic_write_json(WIN): ReplaceFileW succeeded for target '{}'.",
                 target.string());
#else
    // POSIX implementation
    LOGGER_DEBUG("atomic_write_json(POSIX): Target: {}", target.string());
    namespace fs = std::filesystem;

    struct stat lstat_buf;
    if (lstat(target.c_str(), &lstat_buf) == 0)
    {
        if (S_ISLNK(lstat_buf.st_mode))
        {
            throw std::runtime_error(
                "atomic_write_json: target path is a symbolic link, refusing "
                "to write for security reasons.");
        }
    }
    LOGGER_DEBUG("atomic_write_json(POSIX): Symlink check passed for target '{}'.",
                 target.string());

    std::string dir = target.parent_path().string();
    if (dir.empty())
        dir = ".";

    std::error_code ec_dir;
    fs::create_directories(target.parent_path(), ec_dir);
    if (ec_dir)
    {
        throw std::runtime_error("atomic_write_json: create_directories failed: " +
                                 ec_dir.message());
    }
    LOGGER_DEBUG("atomic_write_json(POSIX): Parent directory '{}' ensured.",
                 target.parent_path().string());

    std::string filename = target.filename().string();
    std::string tmpl = dir + "/" + filename + ".tmp.XXXXXX";
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');

    int fd = mkstemp(tmpl_buf.data());
    if (fd == -1)
    {
        throw std::runtime_error(
            fmt::format("atomic_write_json: mkstemp failed: {}", std::strerror(errno)));
    }
    LOGGER_DEBUG("atomic_write_json(POSIX): Temp file '{}' created (FD: {}).",
                 tmpl_buf.data(),
                 fd);

    std::string tmp_path = tmpl_buf.data();
    auto cleanup_temp_file = [&]() {
        if (fd != -1)
            ::close(fd);
        ::unlink(tmp_path.c_str());
    };

    try
    {
        std::string out = j.dump(4);
        ssize_t written = ::write(fd, out.data(), out.size());
        if (written < 0 || static_cast<size_t>(written) != out.size())
        {
            throw std::runtime_error(
                fmt::format("atomic_write_json: write failed: {}", std::strerror(errno)));
        }
        LOGGER_DEBUG("atomic_write_json(POSIX): Wrote {} bytes to temp file.", written);

        if (::fsync(fd) != 0)
        {
            throw std::runtime_error(
                fmt::format("atomic_write_json: fsync(file) failed: {}", std::strerror(errno)));
        }
        LOGGER_DEBUG("atomic_write_json(POSIX): Flushed file buffers.");

        struct stat st;
        if (stat(target.c_str(), &st) == 0)
        {
            if (fchmod(fd, st.st_mode) != 0)
            {
                throw std::runtime_error(
                    fmt::format("atomic_write_json: fchmod failed: {}", std::strerror(errno)));
            }
            LOGGER_DEBUG("atomic_write_json(POSIX): Copied permissions.");
        }

        if (::close(fd) != 0)
        {
            fd = -1; // prevent double close
            throw std::runtime_error(
                fmt::format("atomic_write_json: close failed: {}", std::strerror(errno)));
        }
        fd = -1;
        LOGGER_DEBUG("atomic_write_json(POSIX): Closed temp file handle.");

        if (std::rename(tmp_path.c_str(), target.c_str()) != 0)
        {
            throw std::runtime_error(
                fmt::format("atomic_write_json: rename failed: {}", std::strerror(errno)));
        }
        LOGGER_DEBUG("atomic_write_json(POSIX): Rename succeeded for target '{}'.",
                     target.string());

        int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
        if (dfd >= 0)
        {
            if (::fsync(dfd) != 0)
            {
                ::close(dfd);
                throw std::runtime_error(fmt::format(
                    "atomic_write_json: fsync(dir) failed: {}", std::strerror(errno)));
            }
            ::close(dfd);
            LOGGER_DEBUG("atomic_write_json(POSIX): Synced parent directory.");
        }
        else
        {
            throw std::runtime_error(fmt::format(
                "atomic_write_json: open(dir) for fsync failed: {}", std::strerror(errno)));
        }
    }
    catch (...)
    {
        cleanup_temp_file();
        throw;
    }
#endif
}

} // namespace pylabhub::utils
