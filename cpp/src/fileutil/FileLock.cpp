// FileLock.cpp
// Implementation of FileLock (cross-platform)
// (modified to use new logger macros from logger.hpp)
//
// Design / critical notes:
//  - Purpose: provide a small RAII wrapper for a cross-process file lock used by JsonConfig.
//  - Semantics: Default behavior supports both Blocking and NonBlocking modes set at construction.
//    NonBlocking mode is used throughout JsonConfig by policy: fail fast if a lock is held by
//    another process.
//  - POSIX: uses open() to create/open a lock file and flock(fd, LOCK_EX[|LOCK_NB]) to lock it.
//  - Windows: uses CreateFileW on a lock-file path then LockFileEx/UnlockFileEx on the handle.
//    We convert file paths to long-paths via PathUtil::win32_to_long_path to handle long paths.
//  - The lock file path is derived from the target path: parent/<basename>.lock
//    This keeps lock files local to same directory and avoids races across directories.
//  - The class is movable (transfer ownership) and non-copyable.
//  - All system errors are stored in _ec for callers to inspect when valid()==false.
//
// Thread-safety: FileLock instances are single-thread objects; you can create/destroy them on any
// thread, but you should not attempt to use the same instance concurrently from multiple threads.

#include "fileutil/FileLock.hpp"
#include "fileutil/PathUtil.hpp"
#include "util/Logger.hpp" // <--- added

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <sstream>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pylabhub::fileutil
{

// Helper: build lockfile path for a given target path.
// Lock name policy: <parent>/<filename>.lock
static std::filesystem::path make_lock_path(const std::filesystem::path &target)
{
    std::filesystem::path parent = target.parent_path();
    if (parent.empty())
        parent = ".";
    std::string fname = target.filename().string();
    if (fname.empty())
    {
        // fallback if target is a directory or path ends with slash
        fname = parent.filename().string();
        if (fname.empty())
            fname = "file";
    }
    return parent / (fname + ".lock");
}

// ---------------- FileLock implementation ----------------

FileLock::FileLock(const std::filesystem::path &path, LockMode mode)
    : _path(path), _valid(false), _ec()
{
    open_and_lock(mode);
}

FileLock::FileLock(FileLock &&other) noexcept
    : _path(std::move(other._path)), _valid(other._valid), _ec(other._ec)
{
#if defined(_WIN32)
    _handle = other._handle;
    other._handle = nullptr;
#else
    _fd = other._fd;
    other._fd = -1;
#endif
    other._valid = false;
    other._ec.clear();
}

FileLock &FileLock::operator=(FileLock &&other) noexcept
{
    if (this == &other)
        return *this;

    // Release any existing lock in this
#if defined(_WIN32)
    if (_handle)
    {
        // attempt to unlock and close; ignore failures
        UnlockFileEx((HANDLE)_handle, 0, MAXDWORD, MAXDWORD, nullptr);
        CloseHandle((HANDLE)_handle);
        _handle = nullptr;
    }
#else
    if (_fd != -1)
    {
        flock(_fd, LOCK_UN);
        ::close(_fd);
        _fd = -1;
    }
#endif

    _path = std::move(other._path);
    _valid = other._valid;
    _ec = other._ec;

#if defined(_WIN32)
    _handle = other._handle;
    other._handle = nullptr;
#else
    _fd = other._fd;
    other._fd = -1;
#endif

    other._valid = false;
    other._ec.clear();

    return *this;
}

FileLock::~FileLock()
{
    // Release lock and close file/handle.
#if defined(_WIN32)
    if (_handle)
    {
        // Attempt to unlock entire file then close
        OVERLAPPED ov = {};
        // Ignore errors during cleanup
        UnlockFileEx((HANDLE)_handle, 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle((HANDLE)_handle);
        _handle = nullptr;
    }
#else
    if (_fd != -1)
    {
        // best-effort unlock and close
        flock(_fd, LOCK_UN);
        ::close(_fd);
        _fd = -1;
    }
#endif
    _valid = false;
    _ec.clear();
}

bool FileLock::valid() const noexcept
{
    return _valid;
}

std::error_code FileLock::error_code() const noexcept
{
    return _ec;
}

void FileLock::open_and_lock(LockMode mode)
{
    // Reset state
    _valid = false;
    _ec.clear();

    auto lockpath = make_lock_path(_path);

#if defined(_WIN32)
    // Create/Open lock file with exclusive access to ensure we can lock it
    // Convert to long path for Windows API calls
    std::wstring lockpath_w = win32_to_long_path(lockpath);

    HANDLE h =
        CreateFileW(lockpath_w.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, // allow other processes to open the file,
                                                        // we use LockFileEx to enforce lock
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        _ec = std::error_code(GetLastError(), std::system_category());
        // log using new logger API
        LOGGER_WARN("FileLock: CreateFileW failed for {} err={}", lockpath.string().c_str(),
                    _ec.value());
        _handle = nullptr;
        _valid = false;
        return;
    }

    // Prepare flags: exclusive lock; optionally fail immediately
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    if (mode == LockMode::NonBlocking)
        flags |= LOCKFILE_FAIL_IMMEDIATELY;

    OVERLAPPED ov = {};
    BOOL ok = LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov);
    if (!ok)
    {
        DWORD err = GetLastError();
        _ec = std::error_code(static_cast<int>(err), std::system_category());
        LOGGER_WARN("FileLock: LockFileEx failed for {} err={}", lockpath.string().c_str(), err);
        CloseHandle(h);
        _handle = nullptr;
        _valid = false;
        return;
    }

    // Success
    _handle = reinterpret_cast<void *>(h);
    _valid = true;
    _ec.clear();
    return;

#else
    // POSIX implementation
    int flags = O_CREAT | O_RDWR;
    // Mode 0666 so creation honors umask
    int fd = ::open(lockpath.c_str(), flags, 0666);
    if (fd == -1)
    {
        _ec = std::error_code(errno, std::generic_category());
        LOGGER_WARN("FileLock: open failed for {} err={}", lockpath.string().c_str(),
                    _ec.message().c_str());
        _fd = -1;
        _valid = false;
        return;
    }

    int op = LOCK_EX;
    if (mode == LockMode::NonBlocking)
        op |= LOCK_NB;

    if (flock(fd, op) != 0)
    {
        _ec = std::error_code(errno, std::generic_category());
        LOGGER_WARN("FileLock: flock failed for {} err={}", lockpath.string().c_str(),
                    _ec.message().c_str());
        ::close(fd);
        _fd = -1;
        _valid = false;
        return;
    }

    // Success
    _fd = fd;
    _valid = true;
    _ec.clear();
    return;
#endif
}

} // namespace pylabhub::fileutil
