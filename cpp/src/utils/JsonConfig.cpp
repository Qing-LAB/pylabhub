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
JsonConfig::JsonConfig(JsonConfig &&) noexcept = default;
JsonConfig &JsonConfig::operator=(JsonConfig &&) noexcept = default;

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
        // Return a copy of the data. This is thread-safe but may be expensive.
        // For performance-sensitive reads, use `with_json_read`.
        if (!pImpl)
            return json::object();
        std::lock_guard<std::mutex> g(pImpl->initMutex); // Protects against concurrent destruction.
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
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Target: {}\n", target.string());
#endif

    std::filesystem::path parent = target.parent_path();
    if (parent.empty())
        parent = ".";

    // Ensure the target directory exists.
    std::error_code ec_dir;
    fs::create_directories(target.parent_path(), ec_dir);
    if (ec_dir)
    {
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): create_directories failed for {}: {}\n",
                            target.parent_path().string(), ec_dir.message());
        throw std::runtime_error("atomic_write_json: create_directories failed: " + ec_dir.message());
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Parent directory '{}' ensured.\n", target.parent_path().string());
#endif


    std::wstring filename = target.filename().wstring();
    std::wstring tmpname = filename + L".tmp" + win32_make_unique_suffix();

    std::filesystem::path tmp_full = parent / std::filesystem::path(tmpname);

    // Convert to long paths to avoid MAX_PATH issues, using the PathUtil helper.
    std::wstring tmp_full_w = win32_to_long_path(tmp_full);
    std::wstring target_w = win32_to_long_path(target);

#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Temp file path: {}\n", tmp_full.string());
#endif

    // Security: Check if the target is a reparse point (e.g., a symlink).
    DWORD attributes = GetFileAttributesW(target_w.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): Target path '{}' is a reparse point. Refusing to write.\n", target.string());
        throw std::runtime_error(
            "atomic_write_json: target path is a reparse point (e.g., symbolic link), "
            "refusing to write for security reasons.");
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Reparse point check passed for target '{}'.\n", target.string());
#endif

    // 1. Create a temporary file. No sharing is allowed to ensure we have exclusive access.
    HANDLE h = CreateFileW(tmp_full_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): CreateFileW(temp) failed for '{}'. Error: {}\n", tmp_full.string(), err);
        throw std::runtime_error(
            fmt::format("atomic_write_json: CreateFileW(temp) failed: {}", err));
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Temp file '{}' created successfully (HANDLE: {:p}).\n", tmp_full.string(), (void*)h);
#endif


    // 2. Write the JSON data to the temporary file.
    std::string out = j.dump(4);
    DWORD written = 0;
    BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    if (!ok || static_cast<size_t>(written) != out.size())
    {
        DWORD err = GetLastError();
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): WriteFile failed for '{}'. Bytes written: {}. Expected: {}. Error: {}\n",
                     tmp_full.string(), written, out.size(), err);
        FlushFileBuffers(h);
        CloseHandle(h);
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(fmt::format("atomic_write_json: WriteFile failed: {}", err));
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Wrote {} bytes to temp file '{}'.\n", written, tmp_full.string());
#endif


    // 3. Flush file buffers to ensure the data is physically written to disk.
    if (!FlushFileBuffers(h))
    {
        DWORD err = GetLastError();
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): FlushFileBuffers failed for '{}'. Error: {}\n", tmp_full.string(), err);
        CloseHandle(h);
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(
            fmt::format("atomic_write_json: FlushFileBuffers failed: {}", err));
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Flushed buffers for temp file '{}'.\n", tmp_full.string());
#endif


    // 4. Close the handle to the temporary file.
    CloseHandle(h);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Closed handle for temp file '{}'.\n", tmp_full.string());
#endif


    // 5. Atomically replace the original file with the new temporary file.
    // We need to acquire an exclusive lock on the target file itself during the ReplaceFileW
    // operation to prevent other processes from interfering with the target file at this
    // critical moment.

    HANDLE target_h = INVALID_HANDLE_VALUE;
    bool lock_acquired = false;

    // Open target file, including FILE_SHARE_DELETE to cooperate with ReplaceFileW.
    target_h = CreateFileW(target_w.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (target_h == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): CreateFileW(target for lock) failed for '{}'. Error: {}\n", target.string(), err);
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(
            fmt::format("atomic_write_json: CreateFileW(target for lock) failed: {}", err));
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Target file '{}' opened for mandatory lock (HANDLE: {:p}).\n", target.string(), (void*)target_h);
#endif

    // Lock the first byte of the file, consistent with FileLock's strategy.
    OVERLAPPED ov = {};
    if (!LockFileEx(target_h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                    &ov))
    {
        DWORD err = GetLastError();
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): LockFileEx(target) failed for '{}'. Error: {}\n", target.string(), err);
        CloseHandle(target_h);
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(
            fmt::format("atomic_write_json: LockFileEx(target) failed: {}", err));
    }
    lock_acquired = true;
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Exclusive lock acquired on target '{}'.\n", target.string());
#endif

    // Attempt ReplaceFileW with retries for transient sharing violations.
    const int REPLACE_RETRIES = 5;
    const int REPLACE_DELAY_MS = 100;
    BOOL replaced = FALSE;
    DWORD last_replace_error = 0;

    for (int i = 0; i < REPLACE_RETRIES; ++i)
    {
        // `REPLACEFILE_WRITE_THROUGH` ensures the operation is not just cached but
        // is completed on the physical storage, providing strong durability guarantees.
        replaced = ReplaceFileW(target_w.c_str(), tmp_full_w.c_str(), nullptr,
                                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        if (replaced)
        {
            break; // Success
        }
        last_replace_error = GetLastError();
        if (last_replace_error != ERROR_SHARING_VIOLATION)
        {
            break; // Failed for a non-transient reason, no retry
        }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): ReplaceFileW failed with sharing violation (Error: {}). Retrying (attempt {}/{})...\n", last_replace_error, i + 1, REPLACE_RETRIES);
#endif
        Sleep(REPLACE_DELAY_MS); // Wait before retrying
    }

    if (!replaced)
    {
        DWORD err = last_replace_error;
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(WIN): ReplaceFileW failed for target '{}' after {} retries. Error: {}\n", target.string(), REPLACE_RETRIES, err);
        // Attempt cleanup
        if (lock_acquired)
        {
            UnlockFileEx(target_h, 0, 1, 0, &ov);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
            fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Lock released on target '{}' during ReplaceFileW failure.\n", target.string());
#endif
        }
        CloseHandle(target_h);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Target handle closed during ReplaceFileW failure.\n");
#endif
        DeleteFileW(tmp_full_w.c_str());
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Temp file '{}' deleted during ReplaceFileW failure.\n", tmp_full.string());
#endif
        throw std::runtime_error(fmt::format("atomic_write_json: ReplaceFileW failed: {}", err));
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): ReplaceFileW succeeded for target '{}'.\n", target.string());
#endif


    // Release the lock and close the handle.
    if (lock_acquired)
    {
        UnlockFileEx(target_h, 0, 1, 0, &ov);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Lock released on target '{}'.\n", target.string());
#endif
    }
    CloseHandle(target_h);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Target handle closed.\n", target.string());

    // `ReplaceFileW` on success moves the temp file, so the original temp path is now unused.
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(WIN): Operation completed successfully for '{}'.\n", target.string());
#endif

    // `ReplaceFileW` on success moves the temp file, so the original temp path is now unused.

#else
    // POSIX implementation
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Target: {}\n", target.string());
#endif
    namespace fs = std::filesystem;

    // Security: Check if the target is a symlink to prevent symlink attacks where
    // an attacker could replace the config file with a link to a sensitive system
    // file (e.g., /etc/passwd), causing us to overwrite it. We use lstat, which
    // does not follow the link.
    struct stat lstat_buf;
    if (lstat(target.c_str(), &lstat_buf) == 0)
    {
        if (S_ISLNK(lstat_buf.st_mode))
        {
            fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): Target path '{}' is a symbolic link. Refusing to write.\n", target.string());
            throw std::runtime_error("atomic_write_json: target path is a symbolic link, refusing "
                                     "to write for security reasons.");
        }
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Symlink check passed for target '{}'.\n", target.string());
#endif

    std::string dir = target.parent_path().string();
    if (dir.empty())
        dir = ".";

    // Ensure the target directory exists.
    std::error_code ec_dir;
    fs::create_directories(target.parent_path(), ec_dir);
    if (ec_dir)
    {
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): create_directories failed for {}: {}\n",
                            target.parent_path().string(), ec_dir.message());
        throw std::runtime_error("atomic_write_json: create_directories failed: " + ec_dir.message());
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Parent directory '{}' ensured.\n", target.parent_path().string());
#endif

    // 1. Create a secure temporary file in the same directory as the target.
    // `mkstemp` creates a file with a unique name and opens it, returning a file descriptor.
    std::string filename = target.filename().string();
    std::string tmpl = dir + "/" + filename + ".tmp.XXXXXX";
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');

    int fd = mkstemp(tmpl_buf.data());
    if (fd == -1)
    {
        int err = errno;
        fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): mkstemp failed for '{}'. Error: {} ({})\n", tmpl_buf.data(), err, std::strerror(err));
        throw std::runtime_error(
            fmt::format("atomic_write_json: mkstemp failed: {}", std::strerror(err)));
    }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
    fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Temp file '{}' created successfully (FD: {}).\n", tmpl_buf.data(), fd);
#endif

    std::string tmp_path = tmpl_buf.data();
    bool tmp_unlinked = false;

    try
    {
        // 2. Write the JSON data to the temporary file.
        std::string out = j.dump(4);
        const char *buf = out.data();
        size_t toWrite = out.size();
        size_t written = 0;
        while (toWrite > 0)
        {
            ssize_t w = ::write(fd, buf + written, toWrite);
            if (w < 0)
            {
                int err = errno;
                fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): write failed for '{}'. Bytes written: {}. Expected: {}. Error: {} ({})\n",
                             tmp_path, written, out.size(), err, std::strerror(err));
                ::close(fd);
                ::unlink(tmp_path.c_str());
                throw std::runtime_error(
                    fmt::format("atomic_write_json: write failed: {}", std::strerror(err)));
            }
            written += static_cast<size_t>(w);
            toWrite -= static_cast<size_t>(w);
        }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Wrote {} bytes to temp file '{}'.\n", written, tmp_path);
#endif

        // 3. Sync the file contents to disk to ensure data durability.
        if (::fsync(fd) != 0)
        {
            int err = errno;
            fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): fsync(file) failed for '{}'. Error: {} ({})\n", tmp_path, err, std::strerror(err));
            ::close(fd);
            ::unlink(tmp_path.c_str());
            throw std::runtime_error(
                fmt::format("atomic_write_json: fsync(file) failed: {}", std::strerror(err)));
        }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Flushed buffers for temp file '{}'.\n", tmp_path);
#endif

        // 4. If the original file exists, copy its permissions to the new file
        // to maintain security context.
        struct stat st;
        if (stat(target.c_str(), &st) == 0)
        {
            if (fchmod(fd, st.st_mode) != 0)
            {
                int err = errno;
                fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): fchmod failed for '{}'. Error: {} ({})\n", tmp_path, err, std::strerror(err));
                ::close(fd);
                ::unlink(tmp_path.c_str());
                throw std::runtime_error(
                    fmt::format("atomic_write_json: fchmod failed: {}", std::strerror(err)));
            }
        }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Copied permissions to temp file '{}'.\n", tmp_path);
#endif

        // 5. Close the temporary file descriptor.
        if (::close(fd) != 0)
        {
            int err = errno;
            fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): close failed for temp file '{}'. Error: {} ({})\n", tmp_path, err, std::strerror(err));
            ::unlink(tmp_path.c_str());
            throw std::runtime_error(
                fmt::format("atomic_write_json: close failed: {}", std::strerror(err)));
        }
        fd = -1;
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Closed handle for temp file '{}'.\n", tmp_path);
#endif

        // 6. Atomically rename the temporary file to the final target path.
        // We need to acquire an exclusive flock on the target file itself during the rename
        // operation to prevent other processes from interfering with the target file at this
        // critical moment.

        int target_fd = -1;
        bool flock_acquired = false;

        // Open target file for exclusive access to apply a mandatory lock.
        // Use O_CREAT | O_RDWR in case the target file doesn't exist yet (e.g., first write).
        target_fd = ::open(target.c_str(), O_CREAT | O_RDWR, 0666);
        if (target_fd == -1)
        {
            int err = errno;
            fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): open(target for lock) failed for '{}'. Error: {} ({})\n", target.string(), err, std::strerror(err));
            ::unlink(tmp_path.c_str());
            throw std::runtime_error(
                fmt::format("atomic_write_json: open(target for lock) failed: {}", std::strerror(err)));
        }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Target file '{}' opened for mandatory lock (FD: {}).\n", target.string(), target_fd);
#endif

        if (::flock(target_fd, LOCK_EX | LOCK_NB) != 0)
        {
            int err = errno;
            fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): flock(target) failed for '{}'. Error: {} ({})\n", target.string(), err, std::strerror(err));
            ::close(target_fd);
            ::unlink(tmp_path.c_str());
            throw std::runtime_error(
                fmt::format("atomic_write_json: flock(target) failed: {}", std::strerror(err)));
        }
        flock_acquired = true;
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Exclusive flock acquired on target '{}'.\n", target.string());
#endif

        if (std::rename(tmp_path.c_str(), target.c_str()) != 0)
        {
            int err = errno;
            fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): rename failed for target '{}'. Error: {} ({})\n", target.string(), err, std::strerror(err));
            // Attempt cleanup
            if (flock_acquired)
            {
                ::flock(target_fd, LOCK_UN);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
                fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Flock released on target '{}' during rename failure.\n", target.string());
#endif
            }
            ::close(target_fd);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
            fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Target handle closed during rename failure.\n");
#endif
            ::unlink(tmp_path.c_str());
#if defined(DEBUG_ATOMIC_WRITE_JSON)
            fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Temp file '{}' unlinked during rename failure.\n", tmp_path);
#endif
            throw std::runtime_error(
                fmt::format("atomic_write_json: rename failed: {}", std::strerror(err)));
        }
        tmp_unlinked = true;
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Rename succeeded for target '{}'.\n", target.string());
#endif

        // Release the lock and close the handle.
        if (flock_acquired)
        {
            ::flock(target_fd, LOCK_UN);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
            fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Flock released on target '{}'.\n", target.string());
#endif
        }
        ::close(target_fd);
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Target handle closed.\n", target.string());
#endif


        // 7. Sync the parent directory. This is a crucial step to ensure that the
        // directory entry changes from the `rename` operation are durable on disk.
        int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
        if (dfd >= 0)
        {
            if (::fsync(dfd) != 0)
            {
                int err = errno;
                fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): fsync(dir) failed for '{}'. Error: {} ({})\n", dir, err, std::strerror(err));
                ::close(dfd);
                throw std::runtime_error(
                    fmt::format("atomic_write_json: fsync(dir) failed: {}", std::strerror(err)));
            }
            ::close(dfd);
        }
        else
        {
            int err = errno;
            fmt::print(stderr, "ERROR_ATOMIC_WRITE_JSON(POSIX): open(dir) failed for fsync on '{}'. Error: {} ({})\n", dir, err, std::strerror(err));
            throw std::runtime_error(fmt::format(
                "atomic_write_json: open(dir) failed for fsync: {}", std::strerror(err)));
        }
#if defined(DEBUG_ATOMIC_WRITE_JSON)
        fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Operation completed successfully for '{}'.\n", target.string());
#endif
    }
    catch (...)
    {
        // In case of any error, ensure the temporary file is cleaned up.
        if (!tmp_unlinked)
        {
#if defined(DEBUG_ATOMIC_WRITE_JSON)
            fmt::print(stderr, "DEBUG_ATOMIC_WRITE_JSON(POSIX): Cleaning up unlinked temp file '{}'.\n", tmp_path);
#endif
            ::unlink(tmp_path.c_str());
        }
        throw;
    }
#endif
}

} // namespace pylabhub::utils
