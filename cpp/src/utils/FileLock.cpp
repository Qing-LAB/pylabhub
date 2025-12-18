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
 * **POSIX**: Uses `flock()` on a dedicated `.lock` file. This is a widely
 *       supported and robust advisory locking mechanism for *local* filesystems.
 *     - **Windows**: Uses `LockFileEx()` on a handle to a dedicated `.lock` file.
 * 3.  **Lock File Strategy**: A separate lock file (e.g., `config.json.lock` for
 *     `config.json`) is used instead of locking the target file directly. This
 *     avoids issues where file content operations might interfere with the lock
 *     itself and simplifies the implementation.
 *     **IMPORTANT**: This is an *advisory* lock. It means only processes that
 *     explicitly attempt to acquire this lock will be affected. Other processes
 *     (or processes not using this `FileLock` mechanism) can still access and
 *     modify the original target file without being blocked.
 *     **WARNING**: The reliability of `flock()` over network filesystems (like NFS)
 *     is not guaranteed and can be implementation-dependent. This locking
 *     mechanism is safest when used on local filesystems.
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

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(PLATFORM_WIN64)
#include <sstream>
#include <windows.h>

// Helper to convert UTF-16 wstring to UTF-8 string using the Windows API.
// This replaces the deprecated std::wstring_convert and <codecvt> functionality.
static std::string wstring_to_utf8(const std::wstring &wstr)
{
    if (wstr.empty())
    {
        return std::string();
    }
    int size_needed =
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(
        CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), &strTo[0], size_needed, NULL, NULL);
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
        std::wstring longw =
            pylabhub::utils::win32_to_long_path(abs); // ensure this handles relative -> absolute
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

// Polling interval for timed and blocking lock attempts.
// This value is a heuristic balance between CPU usage (shorter sleep means
// more polling) and responsiveness (longer sleep means slower reaction to a
// released lock). 20ms is a reasonable default.
static constexpr std::chrono::milliseconds LOCK_POLLING_INTERVAL = std::chrono::milliseconds(20);

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
    std::filesystem::path path; // The original path for which a lock is requested.
    bool valid = false;         // True if the lock is currently held.
    std::error_code ec;         // Stores the last error if `valid` is false.
    std::string lock_key;       // The canonical key for the lock path in g_proc_locks.
    std::shared_ptr<ProcLockState>
        proc_state; // The process-local state for this lock.

#if defined(PLATFORM_WIN64)
    void *handle = nullptr; // The Windows file handle (HANDLE) for the .lock file.
#else
    int fd = -1; // The POSIX file descriptor for the .lock file.
#endif
};

// Custom deleter for the unique_ptr. This is the idiomatic way to handle
// cleanup for a Pimpl object that holds resources. It ensures that whenever the
// unique_ptr destroys the FileLockImpl object (on destruction, reset, or
// move-assignment), this code is run.
void FileLock::FileLockImplDeleter::operator()(FileLockImpl *p)
{
    if (!p)
    {
        return;
    }

    if (p->valid)
    {
        LOGGER_DEBUG("FileLock: Releasing lock for '{}'", p->path.string());
    }

    // --- 1. Release OS-level lock ---
    if (p->valid)
    {
#if defined(PLATFORM_WIN64)
        OVERLAPPED ov = {};
        UnlockFileEx((HANDLE)p->handle, 0, 1, 0, &ov);
        CloseHandle((HANDLE)p->handle);
        p->handle = nullptr;
#else
        flock(p->fd, LOCK_UN);
        ::close(p->fd);
        p->fd = -1;
#endif
    }

    // --- 2. Release process-local lock ---
    if (p->proc_state)
    {
        std::lock_guard<std::mutex> lg(g_registry_mtx);
        if (--p->proc_state->owners == 0)
        {
            LOGGER_TRACE(
                "FileLock: Last process-local owner for '{}' released. Notifying waiters.",
                p->lock_key);
            g_proc_locks.erase(p->lock_key);
            p->proc_state->cv.notify_all();
        }
        else
        {
            LOGGER_TRACE("FileLock: Process-local lock for '{}' released. {} owners remain.",
                         p->lock_key,
                         p->proc_state->owners);
        }
    }

    // Finally, delete the memory for the Pimpl object itself.
    delete p;
}

// Forward declaration for the private locking function.
static void open_and_lock(FileLockImpl *pImpl,
                          ResourceType type,
                          LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout);

// This function is now a public static member of FileLock.
std::filesystem::path FileLock::get_expected_lock_fullname_for(const std::filesystem::path &target,
                                                  ResourceType type) noexcept
{
    try
    {
        // First, resolve the path to an absolute form to handle ., .., and
        // relative paths safely and consistently. This does not resolve symlinks
        // and does not require the path to exist.
        const auto abs_target = std::filesystem::absolute(target).lexically_normal();

        if (type == ResourceType::Directory)
        {
            // For a directory, the lock file is named after the directory and
            // lives in its parent directory.
            auto fname_str = abs_target.filename().string();
            auto parent = abs_target.parent_path();

            // If the filename is empty (e.g. for root paths like "/" or "C:\"),
            // use a fixed, safe name for the lock file.
            if (fname_str.empty() || fname_str == "." || fname_str == "..")
            {
                fname_str = "pylabhub_root";
            }
            return parent / (fname_str + ".dir.lock");
        }
        else
        {
            // For a file, the lock is alongside the file.
            return abs_target.string() + ".lock";
        }
    }
    catch (const std::exception &e)
    {
        // Filesystem operations can throw. In a noexcept function, the best we
        // can do is log and return an empty path to signal failure.
        LOGGER_ERROR("FileLock::get_expected_lock_fullname_for threw an exception for target '{}': {}",
                     target.string(),
                     e.what());
        return {};
    }
    catch (...)
    {
        LOGGER_ERROR("FileLock::get_expected_lock_fullname_for threw an unknown exception for target '{}'",
                     target.string());
        return {};
    }
}

// ---------------- FileLock implementation ----------------

// Constructor: attempts to open and lock the file based on the specified mode.
// Note: We use `new FileLockImpl` because std::make_unique cannot be used with
// a custom deleter type that is only forward-declared in the header.
FileLock::FileLock(const std::filesystem::path &path, ResourceType type, LockMode mode) noexcept
    : pImpl(new FileLockImpl)
{
    // Delegate all logic to the private implementation.
    pImpl->path = path;
    open_and_lock(pImpl.get(), type, mode, std::nullopt);
}

// Timed constructor: attempts to open and lock for a given duration.
FileLock::FileLock(const std::filesystem::path &path,
                   ResourceType type,
                   std::chrono::milliseconds timeout) noexcept
    : pImpl(new FileLockImpl)
{
    pImpl->path = path;
    // A timed lock is a variation of a blocking lock.
    open_and_lock(pImpl.get(), type, LockMode::Blocking, timeout);
}

// By explicitly defaulting the special member functions in the .cpp file, we
// ensure the compiler generates them at a point where FileLockImpl and its
// deleter are complete types. This is critical for the unique_ptr Pimpl idiom
// to work correctly across module boundaries and with move semantics.
FileLock::~FileLock() = default;
FileLock::FileLock(FileLock &&) noexcept = default;
FileLock &FileLock::operator=(FileLock &&) noexcept = default;

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
static void open_and_lock(FileLockImpl *pImpl,
                          ResourceType type,
                          LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout)
{
    if (!pImpl)
        return;

    const auto start_time = std::chrono::steady_clock::now();

    // Reset state in Impl for the new lock attempt.
    pImpl->valid = false;
    pImpl->ec.clear();

    const char *mode_str = "unknown";
    if (timeout)
    {
        mode_str = "timed blocking";
    }
    else
    {
        mode_str = (mode == LockMode::Blocking) ? "blocking" : "non-blocking";
    }
    LOGGER_DEBUG("FileLock: Attempting to acquire {} lock on '{}'", mode_str, pImpl->path.string());

    auto lockpath = FileLock::get_expected_lock_fullname_for(pImpl->path, type);
    if (lockpath.empty())
    {
        pImpl->ec = std::make_error_code(std::errc::invalid_argument);
        pImpl->valid = false;
        LOGGER_WARN("FileLock: get_expected_lock_fullname_for failed for '{}'", pImpl->path.string());
        return;
    }

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
                    lockpath.string(),
                    e.what());
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
                LOGGER_DEBUG("FileLock: Non-blocking lock on '{}' failed: already locked in-process.",
                             pImpl->path.string());
                // This lock never acquired ownership, so detach from the proc_state
                // to prevent the destructor from decrementing the owner count.
                pImpl->proc_state.reset();
                return;
            }
        }
        else // Blocking or Timed Blocking
        {
            if (state->owners > 0)
            {
                LOGGER_TRACE("FileLock: Lock on '{}' waiting for in-process release.",
                             pImpl->path.string());
            }

            // Wait until the lock is no longer owned by any other thread in this process.
            if (timeout)
            {
                if (!state->cv.wait_for(regl, *timeout, [&] { return state->owners == 0; }))
                {
                    // Timed out waiting for the in-process lock.
                    pImpl->ec = std::make_error_code(std::errc::timed_out);
                    pImpl->valid = false;
                    LOGGER_DEBUG("FileLock: Timed out waiting for in-process lock on '{}'",
                                 pImpl->path.string());
                    return;
                }
            }
            else
            {
                state->cv.wait(regl, [&] { return state->owners == 0; });
            }
        }
        // If we are here, we have acquired the process-local lock.
        state->owners++;
        LOGGER_TRACE("FileLock: Acquired process-local lock for '{}'. Owners: {}",
                     pImpl->path.string(),
                     state->owners);
    }

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

    // --- 2. OS-Level Lock Acquisition ---
    // Now that the process-local lock is held, attempt to acquire the OS-level
    // (inter-process) lock. If this fails, the process-local lock will be
    // automatically released by the destructor, maintaining consistency.

    // Ensure the directory for the lock file exists.
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
                            parent_dir.string(),
                            create_ec.message());
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
                    lockpath.parent_path().string(),
                    e.what());
        pImpl->valid = false;
        rollback_proc_local_lock();
        return;
    }

    // Polling loop for timed and blocking locks. For non-blocking, it runs once.
    while (true)
    {
#if defined(PLATFORM_WIN64)
        std::wstring lockpath_w = win32_to_long_path(lockpath);
        HANDLE h = CreateFileW(lockpath_w.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr,
                               OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);

        if (h == INVALID_HANDLE_VALUE)
        {
            pImpl->ec = std::error_code(GetLastError(), std::system_category());
            LOGGER_WARN("FileLock: CreateFileW failed for {} err={}",
                        lockpath.string(),
                        pImpl->ec.value());
            pImpl->valid = false;
            rollback_proc_local_lock();
            return;
        }

        // For any blocking mode (timed or infinite), we first try a non-blocking lock.
        DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY;
        OVERLAPPED ov = {};
        BOOL ok = LockFileEx(h, flags, 0, 1, 0, &ov);

        if (ok)
        {
            pImpl->handle = reinterpret_cast<void *>(h);
            pImpl->valid = true;
            pImpl->ec.clear();
            LOGGER_DEBUG("FileLock: Successfully acquired {} lock on '{}'", mode_str, pImpl->path.string());
            return; // Success
        }

        // Lock failed. Clean up the handle.
        CloseHandle(h);

        // If it was a non-blocking attempt, we are done.
        if (mode == LockMode::NonBlocking)
        {
            pImpl->ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            LOGGER_DEBUG("FileLock: Non-blocking lock failed for '{}'", pImpl->path.string());
            pImpl->valid = false;
            rollback_proc_local_lock();
            return;
        }
#else
        // POSIX implementation
        int open_flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
        open_flags |= O_CLOEXEC;
#endif
        int fd = ::open(lockpath.c_str(), open_flags, 0666);

        if (fd == -1)
        {
            pImpl->ec = std::error_code(errno, std::generic_category());
            LOGGER_WARN("FileLock: open failed for {} err={}", lockpath.string(), pImpl->ec.message());
            pImpl->valid = false;
            rollback_proc_local_lock();
            return;
        }
#ifndef O_CLOEXEC
        int oldfl = fcntl(fd, F_GETFD);
        if (oldfl != -1)
            fcntl(fd, F_SETFD, oldfl | FD_CLOEXEC);
#endif

        // For any blocking mode (timed or infinite), we first try a non-blocking lock.
        int flock_op = LOCK_EX | LOCK_NB;
        if (flock(fd, flock_op) == 0)
        {
            pImpl->fd = fd;
            pImpl->valid = true;
            pImpl->ec.clear();
            LOGGER_DEBUG("FileLock: Successfully acquired {} lock on '{}'", mode_str, pImpl->path.string());
            return; // Success
        }

        // Lock failed. Clean up the descriptor.
        ::close(fd);

        // If it was a non-blocking attempt, we are done.
        if (mode == LockMode::NonBlocking)
        {
            pImpl->ec = std::error_code(errno, std::generic_category());
            LOGGER_DEBUG("FileLock: Non-blocking lock failed for '{}'", pImpl->path.string());
            pImpl->valid = false;
            rollback_proc_local_lock();
            return;
        }
#endif

        // --- Timed/Blocking Polling Logic ---
        // If we reach here, the non-blocking attempt failed and we are in a blocking mode.
        if (timeout)
        {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed >= *timeout)
            {
                // Timeout expired.
                pImpl->ec = std::make_error_code(std::errc::timed_out);
                LOGGER_DEBUG("FileLock: Timed out acquiring OS lock for '{}'", pImpl->path.string());
                pImpl->valid = false;
                rollback_proc_local_lock();
                return;
            }
        }

        // Wait a short interval before trying again.
        std::this_thread::sleep_for(LOCK_POLLING_INTERVAL);
    }
}

} // namespace pylabhub::utils
