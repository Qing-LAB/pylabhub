/*******************************************************************************
 * @file FileLock.cpp
 * @brief Implementation of a cross-platform RAII advisory file lock using a separate lock file.
 *
 * **Design Philosophy**
 * `FileLock` provides a robust, cross-platform, RAII-style mechanism for
 * managing inter-process *advisory* file locks. It is a critical component for
 * ensuring the integrity of shared resources like configuration files *when all
 * participating processes respect the same locking convention*.
 *
 * 1.  **RAII (Resource Acquisition Is Initialization)**: The lock is acquired in
 *     the constructor and automatically released in the destructor. This prevents
 *     leaked locks, even in the presence of exceptions.
 * 2.  **Cross-Platform Abstraction**: The class provides a unified interface over
 *     divergent platform-specific locking primitives:
 *     - **POSIX**: Uses `flock()` on a dedicated `.lock` file. This is a widely
 *       supported and robust advisory locking mechanism.
 *     - **Windows**: Uses `LockFileEx()` on a handle to a dedicated `.lock` file.
 * 3.  **Lock File Strategy**: A separate lock file (e.g., `config.json.lock` for
 *     `config.json`) is used instead of locking the target file directly. This
 *     avoids issues where file content operations might interfere with the lock
 *     itself and simplifies the implementation.
 *     **IMPORTANT**: This is an *advisory* lock. It means only processes that
 *     explicitly attempt to acquire this lock will be affected. Other processes
 *     (or processes not using this `FileLock` mechanism) can still access and
 *     modify the original target file without being blocked.
 * 4.  **Blocking and Non-Blocking Modes**: The lock can be acquired in either
 *     `Blocking` or `NonBlocking` mode. The `NonBlocking` mode allows for a
 *     "fail-fast" policy, which is used by `JsonConfig` to avoid deadlocks or
 *     long waits if another process holds the advisory lock.
 * 5.  **Movability**: The class is movable but not copyable, allowing ownership
 *     of a lock to be efficiently transferred (e.g., returned from a factory
 *     function) while preventing accidental duplication.
 *
 * **Thread Safety**
 * A `FileLock` instance is designed to provide consistent behavior for both
 * inter-process and intra-process (multi-threaded) contention. While a single
 * `FileLock` object is not safe to be accessed concurrently by multiple threads,
 * the mechanism ensures that multiple `FileLock` instances (even in the same
 * process) attempting to lock the same resource will correctly block or fail
 * according to their `LockMode`.
 ******************************************************************************/

#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"
#include "utils/PathUtil.hpp"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#if defined(PLATFORM_WIN64)
#include <sstream>
#include <windows.h>

// Helper to convert UTF-16 wstring to UTF-8 string using the Windows API.
// This replaces the deprecated std::wstring_convert and <codecvt> functionality.
static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return std::string();
    }
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Portable canonicalizer for registry key. Produces a stable key for a lock file path.
// - Uses absolute + lexical normalization (no symlink resolution).
// - On Windows, convert to long path form and to lower-case to ensure case-insensitive match.
static std::string make_lock_key(const std::filesystem::path &lockpath)
{
    try
    {
        std::filesystem::path abs = std::filesystem::absolute(lockpath).lexically_normal();

#if defined(PLATFORM_WIN64)
        // Convert to long path and lowercase for stable key
        std::wstring longw = pylabhub::utils::win32_to_long_path(abs); // ensure this handles relative -> absolute
        // Lowercase the wchar_t string to normalize case (Windows is case-insensitive)
        for (auto &ch : longw)
            ch = towlower(ch);
        // Convert to UTF-8 using the modern, non-deprecated Windows API.
        return wstring_to_utf8(longw);
#else
        return abs.generic_string(); // use generic_string to avoid platform separators
#endif
    }
    catch (...)
    {
        // Fallback: use plain lexically_normal string (shouldn't usually happen)
        try
        {
            return std::filesystem::absolute(lockpath).lexically_normal().generic_string();
        }
        catch (...)
        {
            // Last resort: raw string
            return lockpath.string();
        }
    }
}

namespace pylabhub::utils
{

// --- Process-local lock registry ---
// This registry ensures that even on Windows (where native file locks are
// per-process, meaning a second LockFileEx call from another thread in the
// same process on the same file will succeed immediately), intra-process
// locking attempts will behave consistently (blocking or failing as expected)
// across all platforms.
static std::mutex g_registry_mtx;
struct ProcLockState
{
    int owners = 0; // number of FileLock holders in this process for that path
    std::condition_variable cv;
};
static std::unordered_map<std::string, std::shared_ptr<ProcLockState>> g_proc_locks;

// --- Pimpl Definition ---

// The private implementation of the FileLock.
// All state is held here to provide ABI stability for the public FileLock class.
struct FileLockImpl
{
    std::filesystem::path path;       // The original path for which a lock is requested.
    bool valid = false;               // True if the lock is currently held.
    std::error_code ec;               // Stores the last error if `valid` is false.
    std::string lock_key;             // The canonical key for the lock path in g_proc_locks.
    std::shared_ptr<ProcLockState>
        proc_state; // The process-local state for this lock.

#if defined(PLATFORM_WIN64)
    void *handle = nullptr; // The Windows file handle (HANDLE) for the .lock file.
#else
    int fd = -1; // The POSIX file descriptor for the .lock file.
#endif
};

// Forward declaration for the private locking function.
static void open_and_lock(LockMode mode, FileLockImpl *pImpl);

// Helper to convert LockMode to a string for logging.
static const char *lock_mode_to_string(LockMode mode)
{
    return mode == LockMode::Blocking ? "blocking" : "non-blocking";
}

// Helper: build lockfile path for a given target path.
// Lock name policy: <parent>/<filename>.lock
static std::filesystem::path make_lock_path(const std::filesystem::path &target)
{
    std::filesystem::path parent = target.parent_path();
    if (parent.empty())
        parent = ".";
    std::string fname = target.filename().string();
    if (fname.empty() || fname == "." || fname == "..")
    {
        // Fallback if target is a directory, path ends with a separator, or is "." or "..".
        // Use the parent's name as a basis.
        fname = parent.filename().string();
        if (fname.empty() || fname == "." || fname == "..")
        {
            // Ultimate fallback for root or other unusual paths.
            fname = "file";
        }
    }
    return parent / (fname + ".lock");
}

// ---------------- FileLock implementation ----------------

// Constructor: attempts to open and lock the file based on the specified mode.
FileLock::FileLock(const std::filesystem::path &path, LockMode mode) noexcept
    : pImpl(std::make_unique<FileLockImpl>())
{
    // Delegate all logic to the private implementation.
    pImpl->path = path;
    open_and_lock(mode, pImpl.get());
}

// Move constructor: transfers ownership of the lock from another FileLock instance.
// The default implementation generated by the compiler is correct because the
// class's only member, `std::unique_ptr`, has well-defined move semantics.
FileLock::FileLock(FileLock &&) noexcept = default;

// Move assignment operator: releases the current lock and takes ownership of another.
// The default implementation is correct. `std::unique_ptr`'s move assignment
// operator automatically handles releasing the old resource before acquiring the new one.
FileLock &FileLock::operator=(FileLock &&) noexcept = default;

// Destructor: ensures the lock is released when the object goes out of scope.
FileLock::~FileLock()
{
    // A moved-from FileLock will have a null pImpl. Its destructor is a no-op.
    if (!pImpl)
    {
        return;
    }

    if (pImpl->valid)
    {
        LOGGER_DEBUG("FileLock: Releasing lock for '{}'", pImpl->path.string());
    }

    // --- 1. Release OS-level lock ---
    // This must happen first so that other processes waiting on the OS-level
    // lock can proceed as soon as possible.
    if (pImpl->valid)
    {
        // At this point, pImpl->valid is true, so we hold a valid lock and handle/fd.
        // Release lock and close file/handle. Ignore errors during this best-effort cleanup,
        // as there is little a caller can do about a failure during destruction.
#if defined(PLATFORM_WIN64)
        OVERLAPPED ov = {};
        // Unlock only the first byte, matching the acquisition logic.
        UnlockFileEx((HANDLE)pImpl->handle, 0, 1, 0, &ov);
        CloseHandle((HANDLE)pImpl->handle);
        pImpl->handle = nullptr; // Not strictly necessary, but good practice.
#else
        flock(pImpl->fd, LOCK_UN);
        ::close(pImpl->fd);
        pImpl->fd = -1; // Not strictly necessary, but good practice.
#endif
    }

    // --- 2. Release process-local lock ---
    // This must be done regardless of whether the OS lock was acquired,
    // to correctly decrement the owner count in the shared registry.
    if (pImpl->proc_state)
    {
        std::lock_guard<std::mutex> lg(g_registry_mtx);
        if (--pImpl->proc_state->owners == 0)
        {
            LOGGER_TRACE("FileLock: Last process-local owner for '{}' released. Notifying waiters.",
                         pImpl->lock_key);
            // If we were the last owner for this path in this process,
            // remove the entry from the global map to prevent it from growing
            // indefinitely and notify any waiting threads.
            g_proc_locks.erase(pImpl->lock_key);
            pImpl->proc_state->cv.notify_all();
        }
        else
        {
            LOGGER_TRACE(
                "FileLock: Process-local lock for '{}' released. {} owners remain.",
                pImpl->lock_key, pImpl->proc_state->owners);
        }
        pImpl->proc_state.reset(); // Release shared_ptr ownership.
    }
}

bool FileLock::valid() const noexcept
{
    // A default-constructed or moved-from object will have a null pImpl.
    return pImpl && pImpl->valid;
}

std::error_code FileLock::error_code() const noexcept
{
    // Return a default-constructed (empty) error code if the object is not initialized.
    return pImpl ? pImpl->ec : std::error_code();
}

// Private helper that performs the actual locking logic on the Impl struct.
static void open_and_lock(LockMode mode, FileLockImpl *pImpl)
{
    if (!pImpl)
        return;

    // Reset state in Impl for the new lock attempt.
    pImpl->valid = false;
    pImpl->ec.clear();

    LOGGER_DEBUG("FileLock: Attempting to acquire {} lock on '{}'", lock_mode_to_string(mode),
                 pImpl->path.string());

    auto lockpath = make_lock_path(pImpl->path);

    // --- 1. Process-Local Lock Acquisition ---
    // First, acquire the process-local lock to ensure correct intra-process (i.e.,
    // multi-threaded) semantics on all platforms. This is critical on Windows,
    // where OS file locks are per-process and do not block other threads in the
    // same process. This registry enforces consistent behavior.
    try
    {
        // Normalize the path to generate a consistent key for the lock registry.
        pImpl->lock_key = make_lock_key(lockpath);
        LOGGER_TRACE("FileLock: Using lock key '{}'", pImpl->lock_key);
    }
    catch (const std::exception &e)
    {
        // Filesystem operations can throw. If path normalization fails, the lock
        // cannot be safely managed.
        pImpl->ec = std::make_error_code(std::errc::io_error);
        LOGGER_WARN("FileLock: make_lock_key path conversion for '{}' threw: {}.",
                    lockpath.string(), e.what());
        pImpl->valid = false;
        return;
    }

    { // Scoped lock for the global registry
        std::unique_lock<std::mutex> regl(g_registry_mtx);
        auto &state = g_proc_locks[pImpl->lock_key];
        if (!state)
        {
            state = std::make_shared<ProcLockState>();
        }
        pImpl->proc_state = state; // Store the state for the destructor to use.

        if (mode == LockMode::NonBlocking)
        {
            if (state->owners > 0)
            {
                pImpl->ec = std::make_error_code(std::errc::resource_unavailable_try_again);
                pImpl->valid = false;
                LOGGER_DEBUG(
                    "FileLock: Non-blocking lock on '{}' failed: already locked in-process.",
                    pImpl->path.string());
                // Failed to acquire process-local lock. No OS lock was attempted.
                // The destructor will correctly handle releasing the proc_state.
                return;
            }
        }
        else // Blocking
        {
            if (state->owners > 0)
            {
                LOGGER_TRACE(
                    "FileLock: Blocking lock on '{}' waiting for in-process lock to be released.",
                    pImpl->path.string());
            }
            // Wait until the lock is no longer owned by any other thread in this process.
            state->cv.wait(regl, [&] { return state->owners == 0; });
        }
        // If we are here, we have acquired the process-local lock.
        state->owners++;
        LOGGER_TRACE("FileLock: Acquired process-local lock for '{}'. Owners: {}",
                     pImpl->path.string(), state->owners);
    }

    // --- 2. OS-Level Lock Acquisition ---
    // Now that the process-local lock is held, attempt to acquire the OS-level
    // (inter-process) lock. If this fails, the process-local lock will be
    // automatically released by the destructor, maintaining consistency.

    // Helper to undo the process-local lock acquisition on failure
    auto rollback_proc_local_lock = [&]() {
        std::lock_guard<std::mutex> lg(g_registry_mtx);
        if (pImpl->proc_state)
        {
            if (--pImpl->proc_state->owners == 0)
            {
                g_proc_locks.erase(pImpl->lock_key);
                pImpl->proc_state->cv.notify_all();
            }
            pImpl->proc_state.reset();
        }
    };

    // Ensure the directory for the lock file exists before trying to create the file.
    // This is wrapped in a try/catch as filesystem operations can throw exceptions
    // (e.g., on permission errors not reported by the error_code overload).
    try
    {
        auto parent_dir = lockpath.parent_path();
        if (!parent_dir.empty())
        {
            std::error_code create_ec;
            std::filesystem::create_directories(parent_dir, create_ec);
            if (create_ec)
            {
                pImpl->ec = create_ec;
                LOGGER_WARN("FileLock: create_directories failed for {} err={}",
                            parent_dir.string(), create_ec.message());
                pImpl->valid = false;
                rollback_proc_local_lock();
                return;
            }
        }
    }
    catch (const std::exception &e)
    {
        pImpl->ec = std::make_error_code(std::errc::io_error);
        LOGGER_WARN("FileLock: create_directories threw an exception for {}: {}",
                    lockpath.parent_path().string(), e.what());
        pImpl->valid = false;
        rollback_proc_local_lock();
        return;
    }

#if defined(PLATFORM_WIN64)
    // Convert to long path for Windows API calls to handle paths > MAX_PATH.
    std::wstring lockpath_w = win32_to_long_path(lockpath);

    // Open or create the lock file. We allow sharing at the file system level
    // because we will use LockFileEx to enforce an exclusive lock.
    HANDLE h = CreateFileW(
        lockpath_w.c_str(), GENERIC_READ | GENERIC_WRITE,
        // Allow other processes to open the file. The OS-level lock provided
        // by LockFileEx is what guarantees exclusivity, not the file sharing mode.
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        pImpl->ec = std::error_code(GetLastError(), std::system_category());
        // log using new logger API
        LOGGER_WARN("FileLock: CreateFileW failed for {} err={}", lockpath.string(),
                    pImpl->ec.value());
        pImpl->handle = nullptr;
        pImpl->valid = false;
        rollback_proc_local_lock();
        return;
    }

    // Prepare flags: exclusive lock; optionally fail immediately for non-blocking mode.
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    if (mode == LockMode::NonBlocking)
        flags |= LOCKFILE_FAIL_IMMEDIATELY;

    // The OVERLAPPED struct is required for LockFileEx, even for synchronous operations.
    // We lock the first byte of the file, which is a common and portable pattern.
    OVERLAPPED ov = {};
    BOOL ok = LockFileEx(h, flags, 0, 1, 0, &ov);
    if (!ok)
    {
        DWORD err = GetLastError();
        pImpl->ec = std::error_code(static_cast<int>(err), std::system_category());
        LOGGER_WARN("FileLock: LockFileEx failed for {} err={}", lockpath.string(), err);
        CloseHandle(h);
        pImpl->handle = nullptr;
        pImpl->valid = false;
        rollback_proc_local_lock();
        return;
    }

#else
    // POSIX implementation
    int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    // Mode 0666 so that file creation honors the process's umask.
    int fd = ::open(lockpath.c_str(), flags, 0666);
    if (fd == -1)
    {
        pImpl->ec = std::error_code(errno, std::generic_category());
        LOGGER_WARN("FileLock: open failed for {} err={}", lockpath.string(),
                    pImpl->ec.message());
        pImpl->fd = -1;
        pImpl->valid = false;
        rollback_proc_local_lock();
        return;
    }

// If O_CLOEXEC not available, set FD_CLOEXEC manually:
#ifndef O_CLOEXEC
    int oldfl = fcntl(fd, F_GETFD);
    if (oldfl != -1)
        fcntl(fd, F_SETFD, oldfl | FD_CLOEXEC);
#endif

    // Prepare flags for flock(). LOCK_EX requests an exclusive lock.
    // LOCK_NB makes the call non-blocking.
    int op = LOCK_EX;
    if (mode == LockMode::NonBlocking)
        op |= LOCK_NB;

    if (flock(fd, op) != 0)
    {
        pImpl->ec = std::error_code(errno, std::generic_category());
        LOGGER_WARN("FileLock: flock failed for {} err={}", lockpath.string(),
                    pImpl->ec.message());
        ::close(fd);
        pImpl->fd = -1;
        pImpl->valid = false;
        rollback_proc_local_lock();
        return;
    }
    pImpl->fd = fd;
#endif

    // Success
    LOGGER_DEBUG("FileLock: Successfully acquired {} lock on '{}'", lock_mode_to_string(mode),
                 pImpl->path.string());
#if defined(PLATFORM_WIN64)
    pImpl->handle = reinterpret_cast<void *>(h);
#endif
    pImpl->valid = true;
    pImpl->ec.clear();
}

} // namespace pylabhub::utils
