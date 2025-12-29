/*******************************************************************************
 * @file JsonConfig.cpp
 * @brief Implementation of the non-template methods for the JsonConfig class.
 *
 * @see include/utils/JsonConfig.hpp
 *
 * **Implementation Details**
 *
 * This file contains the core logic for the `JsonConfig` class, including
 * file I/O, locking, and the platform-specific atomic write helper.
 *
 * 1.  **Initialization (`init`)**:
 *     - The `init` method sets the configuration file path.
 *     - If `createIfMissing` is true, it uses a `FileLock` to safely check for
 *       the file's existence and create it if it's not there. This prevents a
 *       Time-of-Check-to-Time-of-Use (TOCTOU) race condition where another
 *       process could create the file between the `exists` check and the write.
 *
 * 2.  **I/O Helpers (`reload_under_lock_io`, `save_under_lock_io`)**:
 *     - These are the internal workhorses for reading and writing. They are
 *       prefixed with `_under_lock_io` to signify that the caller MUST already
 *       hold the inter-process `FileLock`.
 *     - `reload_under_lock_io`: Reads the entire file, parses it into a new
 *       `nlohmann::json` object, and then swaps it into place under a unique
 *       lock on the intra-process `rwMutex`. This minimizes the time that
 *       writer threads are blocked.
 *     - `save_under_lock_io`: First checks the `dirty` flag. If the in-memory
 *       data has not changed, it skips the expensive disk write entirely.
 *       Otherwise, it calls `atomic_write_json`.
 *
 * 3.  **Atomic Write (`atomic_write_json`)**:
 *     - This static helper function is the key to crash-safe file writes. It
 *       ensures that the configuration file is never left in a corrupt,
 *       partially-written state.
 *     - **On POSIX**: It uses `mkstemp` to create a unique temporary file in the
 *       same directory. It writes the new content to this file, `fsync`s it to
 *       ensure it's on disk, and then uses the atomic `rename` system call to
 *       replace the original file. Finally, it `fsync`s the parent directory
 *       to ensure the directory entry update is also persisted.
 *     - **On Windows**: It uses `CreateFileW` to create a unique temporary file.
 *       After writing and flushing, it uses `ReplaceFileW`. This function is
 *       the atomic equivalent of the POSIX `rename`. It handles the case where
 *       the destination file might not exist by falling back to `MoveFileW`.
 *     - On both platforms, it includes a security check to prevent writing to a
 *       target path that is a symbolic link.
 ******************************************************************************/
#include "utils/JsonConfig.hpp"

#include <fmt/format.h>
#include <fstream>
#include <system_error>

#if defined(PLATFORM_WIN64)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Internal utilities
#include "format_tools.hpp"

namespace pylabhub::utils
{

namespace fs = std::filesystem;
using json = nlohmann::json;

// The Pimpl struct definition, containing all private member variables.
struct JsonConfig::Impl
{
    std::filesystem::path configPath;
    nlohmann::json data;
    mutable std::shared_mutex rwMutex; // Guards `data` for thread-safe reads/writes.
    mutable std::mutex initMutex;      // Guards structural state like `configPath` and `fileLock`.
    std::atomic<bool> dirty{false};    // True if `data` may be newer than the on-disk file.
    std::unique_ptr<FileLock> fileLock; // Manages the inter-process file lock.

    Impl() : data(json::object()) {}
    ~Impl() = default;
};


// ============================================================================
// Constructor, Destructor, and Initialization
// ============================================================================

JsonConfig::JsonConfig() noexcept : pImpl(std::make_unique<Impl>()) {}
JsonConfig::JsonConfig(const std::filesystem::path &configFile) : pImpl(std::make_unique<Impl>())
{
    init(configFile, false);
}
JsonConfig::~JsonConfig() = default;

bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing)
{
    if (!pImpl) pImpl = std::make_unique<Impl>();

    std::lock_guard<std::mutex> g(pImpl->initMutex);
    pImpl->configPath = configFile;

    if (createIfMissing)
    {
        // Use a temporary, non-blocking FileLock to prevent a TOCTOU race
        // condition where another process could create the file between our `exists`
        // check and our `atomic_write_json` call.
        FileLock creation_lock(pImpl->configPath, ResourceType::File, LockMode::NonBlocking);
        if (creation_lock.valid())
        {
            std::error_code ec;
            if (!fs::exists(pImpl->configPath, ec))
            {
                try
                {
                    // Create the file with an empty JSON object.
                    atomic_write_json(pImpl->configPath, json::object());
                }
                catch (const std::exception &ex)
                {
                    LOGGER_ERROR("JsonConfig::init: failed to create file '{}': {}",
                                 pImpl->configPath.string(), ex.what());
                    return false;
                }
            }
        }
        else
        {
            // This is not a fatal error. It likely means another process is creating
            // the file, which is fine. The user can attempt to lock and load it later.
            LOGGER_WARN("JsonConfig::init: could not acquire lock to check/create file '{}'. "
                        "Another process may be initializing it.",
                        pImpl->configPath.string());
        }
    }

    // The initial in-memory state is an empty object. A `lock()` or `lock_for()`
    // call is required to load the actual contents from disk.
    pImpl->data = json::object();
    pImpl->dirty.store(false, std::memory_order_release);
    return true;
}

// ============================================================================
// Public Non-Template Methods
// ============================================================================

bool JsonConfig::lock(LockMode mode)
{
    if (!pImpl || pImpl->configPath.empty())
    {
        LOGGER_ERROR("JsonConfig::lock: cannot lock an uninitialized config object.");
        return false;
    }

    std::lock_guard<std::mutex> g(pImpl->initMutex);

    pImpl->fileLock = std::make_unique<FileLock>(pImpl->configPath, ResourceType::File, mode);
    if (pImpl->fileLock->valid())
    {
        // Lock acquired. Now reload data from disk to get the freshest state.
        if (reload_under_lock_io())
        {
            return true;
        }
        else
        {
            // If reload fails, we can't guarantee a safe state, so release the lock.
            pImpl->fileLock.reset(); // Destructor releases the OS lock.
            LOGGER_ERROR("JsonConfig::lock: failed to reload config after acquiring lock.");
            return false;
        }
    }

    // Failed to acquire lock.
    pImpl->fileLock.reset();
    return false;
}

void JsonConfig::unlock()
{
    if (pImpl)
    {
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        pImpl->fileLock.reset();
    }
}

bool JsonConfig::is_locked() const
{
    if (!pImpl) return false;

    // This lock is brief, just to safely check the unique_ptr and its state.
    std::lock_guard<std::mutex> g(pImpl->initMutex);
    return pImpl->fileLock && pImpl->fileLock->valid();
}

bool JsonConfig::save() noexcept
{
    if (!is_locked())
    {
        LOGGER_ERROR("JsonConfig::save: write operations require a file lock.");
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> g(pImpl->initMutex);
        std::error_code ec;
        if (!save_under_lock_io(ec))
        {
            LOGGER_ERROR("JsonConfig::save: save_under_lock_io failed: {}", ec.message());
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::save: exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::save: unknown exception.");
        return false;
    }
}

bool JsonConfig::replace(const json &newData) noexcept
{
    if (!is_locked())
    {
        LOGGER_ERROR("JsonConfig::replace: write operations require a file lock.");
        return false;
    }

    try
    {
        {
            std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
            pImpl->data = newData;
            pImpl->dirty.store(true, std::memory_order_release);
        }
        // After replacing in memory, save to disk.
        return save();
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::replace: exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::replace: unknown exception.");
        return false;
    }
}

json JsonConfig::as_json() const noexcept
{
    try
    {
        if (!pImpl) return json::object();
        // Return a deep copy of the data under a shared (read) lock.
        std::shared_lock<std::shared_mutex> r(pImpl->rwMutex);
        return pImpl->data;
    }
    catch (...)
    {
        return json::object();
    }
}

// ============================================================================
// Internal I/O Helpers
// ============================================================================

bool JsonConfig::save_under_lock_io(std::error_code &ec)
{
    // Precondition: Caller MUST hold pImpl->initMutex and the FileLock.
    ec.clear();
    if (!pImpl)
    {
        ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    if (pImpl->configPath.empty())
    {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }

    // Optimization: If the in-memory data hasn't changed, skip the expensive disk write.
    if (!pImpl->dirty.load(std::memory_order_acquire))
    {
        return true;
    }

    // Snapshot the data to be written under a read lock to minimize blocking time.
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
        LOGGER_ERROR("JsonConfig::save_under_lock_io: atomic_write_json failed: {}", ex.what());
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::save_under_lock_io: atomic_write_json threw unknown exception.");
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }

    // On successful write, clear the dirty flag as memory and disk are now in sync.
    pImpl->dirty.store(false, std::memory_order_release);
    return true;
}

bool JsonConfig::reload_under_lock_io() noexcept
{
    // Precondition: Caller MUST hold pImpl->initMutex and the FileLock.
    try
    {
        if (pImpl->configPath.empty()) return false;

        std::error_code ec;
        if (!fs::exists(pImpl->configPath, ec))
        {
            // If file doesn't exist, the valid state is an empty JSON object.
            pImpl->data = json::object();
            pImpl->dirty.store(false, std::memory_order_release);
            return true;
        }

        std::ifstream in(pImpl->configPath);
        if (!in.is_open())
        {
            LOGGER_ERROR("JsonConfig::reload_under_lock_io: cannot open file: {}", pImpl->configPath.string());
            return false;
        }

        json newdata = json::parse(in, nullptr, false);
        if (newdata.is_discarded())
        {
            LOGGER_ERROR("JsonConfig::reload_under_lock_io: parse error for {}", pImpl->configPath.string());
            return false;
        }

        if (newdata.is_null() || !newdata.is_object())
        {
            // Treat empty or non-object JSON as an empty object for consistency.
            newdata = json::object();
        }

        // Atomically update the in-memory data structure.
        {
            std::unique_lock<std::shared_mutex> w(pImpl->rwMutex);
            pImpl->data = std::move(newdata);
            pImpl->dirty.store(false, std::memory_order_release);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("JsonConfig::reload_under_lock_io: exception: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOGGER_ERROR("JsonConfig::reload_under_lock_io: unknown exception.");
        return false;
    }
}

// ============================================================================
// Atomic Write Implementation
// ============================================================================

void JsonConfig::atomic_write_json(const std::filesystem::path &target, const json &j)
{
#if defined(PLATFORM_WIN64)
    // --- Windows Implementation ---
    LOGGER_DEBUG("atomic_write_json(WIN): Target: {}", target.string());

    fs::path parent = target.parent_path();
    if (parent.empty()) parent = ".";

    std::error_code ec_dir;
    fs::create_directories(parent, ec_dir);
    if (ec_dir)
        throw std::runtime_error("atomic_write_json: create_directories failed: " + ec_dir.message());

    // Create a unique temporary file name.
    std::wstring tmpname = target.filename().wstring() + L".tmp" + format_tools::win32_make_unique_suffix();
    fs::path tmp_full = parent / fs::path(tmpname);

    // Convert to long paths to avoid MAX_PATH issues.
    std::wstring tmp_full_w = format_tools::win32_to_long_path(tmp_full);
    std::wstring target_w = format_tools::win32_to_long_path(target);

    // Security: Refuse to write if the target is a symlink or other reparse point.
    DWORD attributes = GetFileAttributesW(target_w.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("atomic_write_json: target path is a reparse point (e.g., symbolic link), refusing to write.");

    // 1. Create and write to the temporary file.
    HANDLE h = CreateFileW(tmp_full_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error(fmt::format("atomic_write_json: CreateFileW(temp) failed: {}", GetLastError()));

    // Use a unique_ptr for RAII-style handle management.
    auto close_handle = [&](HANDLE handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_handle)> handle_guard(h, close_handle);

    std::string out = j.dump(4); // Pretty-print with 4-space indent.
    DWORD written = 0;
    if (!WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) || static_cast<size_t>(written) != out.size())
    {
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(fmt::format("atomic_write_json: WriteFile failed: {}", GetLastError()));
    }

    if (!FlushFileBuffers(h)) // Ensure data is persisted to disk.
    {
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(fmt::format("atomic_write_json: FlushFileBuffers failed: {}", GetLastError()));
    }
    handle_guard.reset(); // Close the handle before rename.

    // 2. Atomically replace the original file.
    if (!ReplaceFileW(target_w.c_str(), tmp_full_w.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
    {
        DWORD err = GetLastError();
        // If ReplaceFileW fails because the destination does not exist, fall back to MoveFileW.
        if (err == ERROR_FILE_NOT_FOUND)
        {
            if (MoveFileW(tmp_full_w.c_str(), target_w.c_str())) return; // Success.
            err = GetLastError();
        }
        DeleteFileW(tmp_full_w.c_str());
        throw std::runtime_error(fmt::format("atomic_write_json: Replace/Move failed: {}", err));
    }
#else
    // --- POSIX Implementation ---
    LOGGER_DEBUG("atomic_write_json(POSIX): Target: {}", target.string());

    struct stat lstat_buf;
    if (lstat(target.c_str(), &lstat_buf) == 0)
    {
        if (S_ISLNK(lstat_buf.st_mode))
            throw std::runtime_error("atomic_write_json: target path is a symbolic link, refusing to write.");
    }

    fs::path parent = target.parent_path();
    if (parent.empty()) parent = ".";

    std::error_code ec_dir;
    fs::create_directories(parent, ec_dir);
    if (ec_dir)
        throw std::runtime_error("atomic_write_json: create_directories failed: " + ec_dir.message());

    // Create a unique temporary file path.
    std::string tmpl_str = (parent / (target.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> tmpl_buf(tmpl_str.begin(), tmpl_str.end());
    tmpl_buf.push_back('\0');

    int fd = mkstemp(tmpl_buf.data());
    if (fd == -1)
        throw std::runtime_error(fmt::format("atomic_write_json: mkstemp failed: {}", std::strerror(errno)));

    std::string tmp_path = tmpl_buf.data();
    auto cleanup_temp_file = [&]() {
        if (fd != -1) ::close(fd);
        ::unlink(tmp_path.c_str());
    };

    try
    {
        std::string out = j.dump(4);
        ssize_t written = ::write(fd, out.data(), out.size());
        if (written < 0 || static_cast<size_t>(written) != out.size())
            throw std::runtime_error(fmt::format("atomic_write_json: write failed: {}", std::strerror(errno)));

        if (::fsync(fd) != 0) // Ensure data is on disk.
            throw std::runtime_error(fmt::format("atomic_write_json: fsync(file) failed: {}", std::strerror(errno)));

        // Try to preserve original file permissions.
        struct stat st;
        if (stat(target.c_str(), &st) == 0)
        {
            if (fchmod(fd, st.st_mode) != 0)
                throw std::runtime_error(fmt::format("atomic_write_json: fchmod failed: {}", std::strerror(errno)));
        }

        if (::close(fd) != 0)
        {
            fd = -1; // Prevent double close in cleanup.
            throw std::runtime_error(fmt::format("atomic_write_json: close failed: {}", std::strerror(errno)));
        }
        fd = -1;

        // Atomically replace original with temp file.
        if (std::rename(tmp_path.c_str(), target.c_str()) != 0)
            throw std::runtime_error(fmt::format("atomic_write_json: rename failed: {}", std::strerror(errno)));

        // fsync the parent directory to ensure the rename operation is persisted.
        int dfd = ::open(parent.c_str(), O_DIRECTORY | O_RDONLY);
        if (dfd >= 0)
        {
            auto close_dfd = [&](int d) { ::close(d); };
            std::unique_ptr<int, decltype(close_dfd)> dfd_guard(&dfd, close_dfd);
            if (::fsync(dfd) != 0)
                throw std::runtime_error(fmt::format("atomic_write_json: fsync(dir) failed: {}", std::strerror(errno)));
        }
    }
    catch (...)
    {
        cleanup_temp_file();
        throw;
    }
#endif
}

// Instantiate the Impl struct in the source file.
// This is required by the Pimpl idiom.
template struct JsonConfig::Impl;

} // namespace pylabhub::utils
