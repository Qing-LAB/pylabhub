// JsonConfig.cpp
// Implementation of non-template JsonConfig methods and atomic_write_json.
//
// Design / critical notes:
//  - The header `JsonConfig.hpp` contains template helpers and the Impl struct.
//  - This file implements init(), save(), reload(), replace(), as_json(), and atomic_write_json()
//    to avoid including OS-specific headers in every translation unit that includes the header.
//  - Write policy:
//      * with_json_write() obtains an in-process exclusive guard (AtomicGuard.try_acquire_if_zero).
//        Only one writer may hold that guard at a time.
//      * save() will refuse to run while the exclusive guard is active (fail-fast) to avoid races.
//      * All write operations to disk acquire a non-blocking FileLock to avoid blocking other
//      processes.
//  - atomic_write_json will write to a temp file in same directory and then atomically replace
//    the target (POSIX: rename + fsync(dir), Windows: ReplaceFileW).
//
// Important failure semantics:
//  - Functions return bool indicating success; failure details are logged via JC_LOG_* macros.
//  - FileLock errors are logged with their std::error_code messages.

#include "fileutil/JsonConfig.hpp"
#include "fileutil/PathUtil.hpp"

#include <fstream>
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

JsonConfig::JsonConfig() noexcept = default;
JsonConfig::JsonConfig(const std::filesystem::path &configFile)
{
    init(configFile, false);
}
JsonConfig::~JsonConfig() = default;
JsonConfig::JsonConfig(JsonConfig &&) noexcept = default;
JsonConfig &JsonConfig::operator=(JsonConfig &&) noexcept = default;

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
            JC_LOG_ERROR("JsonConfig::init: cannot acquire lock for "
                         << configFile << " code=" << e.value() << " msg=\"" << e.message()
                         << "\"");
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
                JC_LOG_ERROR("JsonConfig::init: failed to create file: " << ex.what());
                return false;
            }
            catch (...)
            {
                JC_LOG_ERROR("JsonConfig::init: unknown error creating file");
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
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            return false;

        if (_impl->configPath.empty())
        {
            JC_LOG_ERROR("JsonConfig::save: configPath not initialized (call init() first)");
            return false;
        }

        // If an exclusive with_json_write is active, refuse to save (policy: fail-fast).
        if (_impl->exclusive_guard.load(std::memory_order_acquire) != 0)
        {
            JC_LOG_ERROR("JsonConfig::save: refusing to save while with_json_write exclusive guard "
                         "is active");
            return false;
        }

        // Acquire cross-process lock in non-blocking mode
        FileLock flock(_impl->configPath, LockMode::NonBlocking);
        if (!flock.valid())
        {
            auto ec = flock.error_code();
            JC_LOG_ERROR("JsonConfig::save: failed to acquire lock for "
                         << _impl->configPath << " code=" << ec.value() << " msg=\"" << ec.message()
                         << "\"");
            return false;
        }

        // Copy data under shared lock, then write outside memory lock
        json toWrite;
        {
            std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
            toWrite = _impl->data;
        }

        atomic_write_json(_impl->configPath, toWrite);
        return true;
    }
    catch (const std::exception &e)
    {
        JC_LOG_ERROR("JsonConfig::save: exception: " << e.what());
        return false;
    }
    catch (...)
    {
        JC_LOG_ERROR("JsonConfig::save: unknown exception");
        return false;
    }
}

bool JsonConfig::reload() noexcept
{
    try
    {
        std::lock_guard<std::mutex> g(_initMutex);
        if (!_impl)
            return false;

        if (_impl->configPath.empty())
        {
            JC_LOG_ERROR("JsonConfig::reload: configPath not initialized (call init() first)");
            return false;
        }

        // Acquire non-blocking cross-process lock so we don't block waiting for other processes
        FileLock flock(_impl->configPath, LockMode::NonBlocking);
        if (!flock.valid())
        {
            auto ec = flock.error_code();
            JC_LOG_ERROR("JsonConfig::reload: failed to acquire lock for "
                         << _impl->configPath << " code=" << ec.value() << " msg=\"" << ec.message()
                         << "\"");
            return false;
        }

        // Read file into json
        std::ifstream in(_impl->configPath);
        if (!in.is_open())
        {
            // If file doesn't exist that's a recoverable state only if createIfMissing was used
            // earlier
            JC_LOG_ERROR("JsonConfig::reload: cannot open file: " << _impl->configPath);
            return false;
        }

        json newdata;
        in >> newdata;
        if (!in && !in.eof())
        {
            JC_LOG_ERROR("JsonConfig::reload: parse/read error for " << _impl->configPath);
            return false;
        }

        {
            std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
            _impl->data = std::move(newdata);
        }

        return true;
    }
    catch (const std::exception &e)
    {
        JC_LOG_ERROR("JsonConfig::reload: exception: " << e.what());
        return false;
    }
    catch (...)
    {
        JC_LOG_ERROR("JsonConfig::reload: unknown exception");
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
            JC_LOG_ERROR("JsonConfig::replace: configPath not initialized (call init() first)");
            return false;
        }

        // Acquire non-blocking cross-process lock; fail fast if busy
        FileLock flock(_impl->configPath, LockMode::NonBlocking);
        if (!flock.valid())
        {
            auto ec = flock.error_code();
            JC_LOG_ERROR("JsonConfig::replace: failed to acquire lock for "
                         << _impl->configPath << " code=" << ec.value() << " msg=\"" << ec.message()
                         << "\"");
            return false;
        }

        // Persist newData to disk atomically (may throw)
        atomic_write_json(_impl->configPath, newData);

        // Update in-memory data under write lock
        {
            std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
            _impl->data = newData;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        JC_LOG_ERROR("JsonConfig::replace: exception: " << e.what());
        return false;
    }
    catch (...)
    {
        JC_LOG_ERROR("JsonConfig::replace: unknown exception");
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
