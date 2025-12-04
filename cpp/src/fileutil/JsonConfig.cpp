// JsonConfig.cpp
// Implementation of non-template JsonConfig methods and atomic_write_json.
// (modified to use new Logger macros from Logger.hpp)
#include "fileutil/JsonConfig.hpp"
#include "fileutil/PathUtil.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <vector>
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pylabhub::fileutil
{

using json = nlohmann::json;

// thread_local storage definition for header-declared symbol
thread_local std::vector<const void *> JsonConfig::g_with_json_write_stack;

// Constructors / dtor
JsonConfig::JsonConfig() noexcept = default;
JsonConfig::JsonConfig(const std::filesystem::path &configFile)
{
    init(configFile, false);
}
JsonConfig::~JsonConfig() = default;

bool JsonConfig::init(const std::filesystem::path &configFile, bool createIfMissing)
{
    std::lock_guard<std::mutex> g(_initMutex);
    if (!_impl)
        _impl = std::make_unique<Impl>();
    _impl->configPath = configFile;

    if (createIfMissing)
    {
        // Acquire a non-blocking lock to avoid TOCTOU races; fail fast if lock busy
        FileLock flock(configFile, LockMode::NonBlocking);
        if (!flock.valid())
        {
            auto e = flock.error_code();
            LOGGER_ERROR("JsonConfig::init: cannot acquire lock for {} code={} msg=\"{}\"",
                         configFile.string().c_str(), e.value(), e.message().c_str());
            return false;
        }

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
        // flock destructor will release lock
    }

    return reload();
}

bool JsonConfig::save() noexcept
{
    try
    {
        // Fast-path: if we're currently inside with_json_write (same thread & instance),
        // then the thread-local stack contains this pointer and we are already inside the
        // _initMutex-protected region. Call save_locked directly to avoid re-locking.
        const void *key = static_cast<const void *>(this);
        bool in_with_json_write =
            (std::find(g_with_json_write_stack.begin(), g_with_json_write_stack.end(), key) !=
             g_with_json_write_stack.end());

        if (in_with_json_write)
        {
            std::error_code ec;
            return save_locked(ec);
        }

        // Normal public save path: take _initMutex then call save_locked()
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
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
    // Caller MUST hold _initMutex (unless called from with_json_write fast-path).
    ec.clear();
    if (!_impl)
    {
        ec = std::make_error_code(std::errc::not_connected);
        return false;
    }

    if (_impl->configPath.empty())
    {
        LOGGER_ERROR("JsonConfig::save_locked: configPath not initialized (call init() first)");
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }

    // If nothing changed (dirty == false) skip writing to disk.
    if (!_impl->dirty.load(std::memory_order_acquire))
    {
        // nothing to do
        return true;
    }

    // Acquire cross-process lock in non-blocking mode (policy: fail-fast)
    FileLock flock(_impl->configPath, LockMode::NonBlocking);
    if (!flock.valid())
    {
        ec = flock.error_code();
        LOGGER_ERROR("JsonConfig::save_locked: failed to acquire lock for {} code={} msg=\"{}\"",
                     _impl->configPath.string().c_str(), ec.value(), ec.message().c_str());
        return false;
    }

    // Copy data under shared lock, then write outside memory lock (we snapshot)
    json toWrite;
    {
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        toWrite = _impl->data;
    }

    try
    {
        atomic_write_json(_impl->configPath, toWrite);
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

    // Success -> clear dirty flag
    _impl->dirty.store(false, std::memory_order_release);
    return true;
}

bool JsonConfig::reload() noexcept
{
    try
    {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            _impl = std::make_unique<Impl>();

        if (_impl->configPath.empty())
        {
            LOGGER_ERROR("JsonConfig::reload: configPath not initialized (call init() first)");
            return false;
        }

        // Acquire non-blocking cross-process lock so we don't block waiting for other processes
        FileLock flock(_impl->configPath, LockMode::NonBlocking);
        if (!flock.valid())
        {
            auto ec = flock.error_code();
            LOGGER_ERROR("JsonConfig::reload: failed to acquire lock for {} code={} msg=\"{}\"",
                         _impl->configPath.string().c_str(), ec.value(), ec.message().c_str());
            return false;
        }

        // Read file into json
        std::ifstream in(_impl->configPath);
        if (!in.is_open())
        {
            LOGGER_ERROR("JsonConfig::reload: cannot open file: {}",
                         _impl->configPath.string().c_str());
            return false;
        }

        json newdata;
        in >> newdata;
        if (!in && !in.eof())
        {
            LOGGER_ERROR("JsonConfig::reload: parse/read error for {}",
                         _impl->configPath.string().c_str());
            return false;
        }

        {
            std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
            _impl->data = std::move(newdata);
            // memory now matches disk -> clear dirty
            _impl->dirty.store(false, std::memory_order_release);
        }

        return true;
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

bool JsonConfig::replace(const json &newData) noexcept
{
    try
    {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            _impl = std::make_unique<Impl>();

        if (_impl->configPath.empty())
        {
            LOGGER_ERROR("JsonConfig::replace: configPath not initialized (call init() first)");
            return false;
        }

        // Acquire non-blocking cross-process lock; fail fast if busy
        FileLock flock(_impl->configPath, LockMode::NonBlocking);
        if (!flock.valid())
        {
            auto ec = flock.error_code();
            LOGGER_ERROR("JsonConfig::replace: failed to acquire lock for {} code={} msg=\"{}\"",
                         _impl->configPath.string().c_str(), ec.value(), ec.message().c_str());
            return false;
        }

        // Persist newData to disk atomically (may throw)
        atomic_write_json(_impl->configPath, newData);

        // Update in-memory data under write lock; memory == disk so clear dirty.
        {
            std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
            _impl->data = newData;
            _impl->dirty.store(false, std::memory_order_release);
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
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            return json::object();
        std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
        return _impl->data;
    }
    catch (...)
    {
        return json::object();
    }
}

// ---------------- atomic_write_json implementation ----------------
//
// POSIX: write temp file in same directory, fsync file, fchmod to match target if exists,
//        close, rename(tmp, target), fsync directory.
// Windows: create temp file in same directory (CreateFileW), write, FlushFileBuffers,
//          close, call ReplaceFileW to atomically replace target. Use PathUtil to convert
//          to long-path when needed.

void JsonConfig::atomic_write_json(const std::filesystem::path &target, const json &j)
{
#if defined(_WIN32)
    // Windows implementation
    std::filesystem::path parent = target.parent_path();
    if (parent.empty())
        parent = ".";

    std::wstring filename = target.filename().wstring();
    std::wstring tmpname = filename + L".tmp" + win32_make_unique_suffix();

    std::filesystem::path tmp_full = parent / std::filesystem::path(tmpname);

    // Convert to long paths to avoid MAX_PATH issues
    std::wstring tmp_full_w = win32_to_long_path(tmp_full);
    std::wstring target_w = win32_to_long_path(target);

    // Create temp file (no sharing)
    HANDLE h = CreateFileW(tmp_full_w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        std::ostringstream os;
        os << "atomic_write_json: CreateFileW(temp) failed: " << err;
        throw std::runtime_error(os.str());
    }

    // Write JSON data
    std::string out = j.dump(4);
    DWORD written = 0;
    BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    if (!ok || static_cast<size_t>(written) != out.size())
    {
        DWORD err = GetLastError();
        FlushFileBuffers(h);
        CloseHandle(h);
        DeleteFileW(tmp_full_w.c_str());
        std::ostringstream os;
        os << "atomic_write_json: WriteFile failed: " << err;
        throw std::runtime_error(os.str());
    }

    // Flush to disk
    if (!FlushFileBuffers(h))
    {
        DWORD err = GetLastError();
        CloseHandle(h);
        DeleteFileW(tmp_full_w.c_str());
        std::ostringstream os;
        os << "atomic_write_json: FlushFileBuffers failed: " << err;
        throw std::runtime_error(os.str());
    }

    CloseHandle(h);

    // Atomically replace target (strong durability with WRITE_THROUGH)
    BOOL replaced = ReplaceFileW(target_w.c_str(), tmp_full_w.c_str(), nullptr,
                                 REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    if (!replaced)
    {
        DWORD err = GetLastError();
        // Attempt cleanup
        DeleteFileW(tmp_full_w.c_str());
        std::ostringstream os;
        os << "atomic_write_json: ReplaceFileW failed: " << err;
        throw std::runtime_error(os.str());
    }

    // ReplaceFileW moved the file into place; best-effort delete of tmp
    DeleteFileW(tmp_full_w.c_str());

#else
    // POSIX implementation
    namespace fs = std::filesystem;
    std::string dir = target.parent_path().string();
    if (dir.empty())
        dir = ".";

    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec)
    {
        throw std::runtime_error("atomic_write_json: create_directories failed: " + ec.message());
    }

    std::string filename = target.filename().string();
    std::string tmpl = dir + "/" + filename + ".tmp.XXXXXX";
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');

    int fd = mkstemp(tmpl_buf.data());
    if (fd == -1)
    {
        int err = errno;
        std::ostringstream os;
        os << "atomic_write_json: mkstemp failed: " << std::strerror(err);
        throw std::runtime_error(os.str());
    }

    std::string tmp_path = tmpl_buf.data();
    bool tmp_unlinked = false;

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
                int err = errno;
                ::close(fd);
                ::unlink(tmp_path.c_str());
                std::ostringstream os;
                os << "atomic_write_json: write failed: " << std::strerror(err);
                throw std::runtime_error(os.str());
            }
            written += static_cast<size_t>(w);
            toWrite -= static_cast<size_t>(w);
        }

        if (::fsync(fd) != 0)
        {
            int err = errno;
            ::close(fd);
            ::unlink(tmp_path.c_str());
            std::ostringstream os;
            os << "atomic_write_json: fsync(file) failed: " << std::strerror(err);
            throw std::runtime_error(os.str());
        }

        // If target existed, copy file mode
        struct stat st;
        if (stat(target.c_str(), &st) == 0)
        {
            if (fchmod(fd, st.st_mode) != 0)
            {
                int err = errno;
                ::close(fd);
                ::unlink(tmp_path.c_str());
                std::ostringstream os;
                os << "atomic_write_json: fchmod failed: " << std::strerror(err);
                throw std::runtime_error(os.str());
            }
        }

        if (::close(fd) != 0)
        {
            int err = errno;
            ::unlink(tmp_path.c_str());
            std::ostringstream os;
            os << "atomic_write_json: close failed: " << std::strerror(err);
            throw std::runtime_error(os.str());
        }
        fd = -1;

        if (std::rename(tmp_path.c_str(), target.c_str()) != 0)
        {
            int err = errno;
            ::unlink(tmp_path.c_str());
            std::ostringstream os;
            os << "atomic_write_json: rename failed: " << std::strerror(err);
            throw std::runtime_error(os.str());
        }
        tmp_unlinked = true;

        // fsync the directory to ensure the rename is durable
        int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
        if (dfd >= 0)
        {
            if (::fsync(dfd) != 0)
            {
                int err = errno;
                ::close(dfd);
                std::ostringstream os;
                os << "atomic_write_json: fsync(dir) failed: " << std::strerror(err);
                throw std::runtime_error(os.str());
            }
            ::close(dfd);
        }
        else
        {
            int err = errno;
            std::ostringstream os;
            os << "atomic_write_json: open(dir) failed for fsync: " << std::strerror(err);
            throw std::runtime_error(os.str());
        }
    }
    catch (...)
    {
        if (!tmp_unlinked)
        {
            ::unlink(tmp_path.c_str());
        }
        throw;
    }
#endif
}

} // namespace pylabhub::fileutil
