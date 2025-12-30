/*******************************************************************************
 * @file FileLock.cpp
 * @brief Implementation of the cross-platform, RAII-style advisory file lock.
 *
 * @see include/utils/FileLock.hpp
 *
 * **Implementation Details**
 *
 * This file contains the private implementation of the `FileLock` class, which
 * provides a robust, two-layer locking mechanism for both inter-process and
 * intra-process synchronization.
 *
 * 1.  **Pimpl Idiom and RAII**:
 *     - The `FileLockImpl` struct holds all private data, including the OS-specific
 *       file handle (`HANDLE` on Windows, `int` fd on POSIX) and state flags.
 *     - The public `FileLock` class holds a `std::unique_ptr` to `FileLockImpl`
 *       with a custom deleter (`FileLockImplDeleter`).
 *     - The custom deleter is the heart of the RAII pattern. Its `operator()`
 *       is called automatically when the `unique_ptr` is destroyed. This is where
 *       the OS lock and the process-local lock are released, guaranteeing
 *       cleanup even if exceptions occur.
 *
 * 2.  **Path Canonicalization (`get_expected_lock_fullname_for`)**:
 *     - This static method is the single source of truth for converting a target
 *       resource path into a canonical lock file path.
 *     - It first attempts `std::filesystem::canonical` to resolve symlinks and
 *       normalize the path. This is crucial for ensuring that different path
 *       strings pointing to the same file contend for the same lock.
 *     - If `canonical` fails (e.g., because the file doesn't exist yet), it
 *       falls back to `std::filesystem::absolute`, which still normalizes the
 *       path, allowing locks to be taken out for files that will be created later.
 *     - The `make_lock_key` helper further normalizes this path into a string
 *       key (e.g., by lowercasing on Windows) for use in internal registries.
 *
 * 3.  **Two-Layer Locking (`open_and_lock`)**:
 *     - **Layer 1: Process-Local Lock (`acquire_process_local_lock`)**:
 *       - Before attempting any OS-level locking, a thread must acquire a lock
 *         in the process-local registry (`g_proc_locks`).
 *       - This registry is a map from a canonical path key to a state struct
 *         containing an owner count and a `std::condition_variable`.
 *       - This layer unifies behavior across platforms. On Windows, `LockFileEx`
 *         is re-entrant for threads in the same process, but this layer makes it
 *         behave like a non-re-entrant POSIX `flock`, ensuring that one thread
 *         will block another in the same process, as users would expect.
 *     - **Layer 2: OS-Level Lock (`run_os_lock_loop`)**:
 *       - Only after acquiring the process-local lock does the code attempt to
 *         acquire the system-wide inter-process lock using `LockFileEx` (Windows)
 *         or `flock` (POSIX).
 *       - If this OS lock fails, the process-local lock is rolled back to allow
 *         other threads in the same process to proceed.
 *
 * 4.  **Automatic Cleanup (`FileLockFinalizer`)**:
 *     - A static `FileLockFinalizer` object is created when the library is loaded.
 *     - Its constructor registers a cleanup function (`do_filelock_cleanup`) with
 *       the `LifecycleManager` to be run at application shutdown.
 *     - The `FileLock::cleanup` function safely iterates over all lock files
 *       created by the process, attempts to acquire a non-blocking lock on each,
 *       and if successful, deletes the file. This prevents leaving stale `.lock`
 *       files on the filesystem while avoiding the deletion of locks still held
 *       by other running processes.
 ******************************************************************************/
#include "utils/FileLock.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(PLATFORM_WIN64)
#include <sstream>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Internal utilities from other modules
#include "format_tools.hpp"
#include "scope_guard.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

// Helper to convert UTF-16 wstring to UTF-8 string using the Windows API.
// This replaces the deprecated std::wstring_convert and <codecvt> functionality.
#if defined(PLATFORM_WIN64)
static std::string wstring_to_utf8(const std::wstring &wstr)
{
    if (wstr.empty())
    {
        return std::string();
    }
    int size_needed =
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), &strTo[0], size_needed, NULL,
                        NULL);
    return strTo;
}
#endif

// Creates a canonical key for a lock file path for use in registries.
// This ensures that different string representations of the same path resolve to
// the same key.
// - On Windows, it converts to a long path and lower-cases it for case-insensitivity.
// - On POSIX, it uses the generic (slash-separated) string representation.
static std::string make_lock_key(const std::filesystem::path &lockpath)
{
    try
    {
#if defined(PLATFORM_WIN64)
        std::wstring longw = pylabhub::format_tools::win32_to_long_path(lockpath);
        // Lowercase the wide string to normalize for Windows' case-insensitive filesystem.
        for (auto &ch : longw)
            ch = towlower(ch);
        return wstring_to_utf8(longw);
#else
        return lockpath.generic_string();
#endif
    }
    catch (...)
    {
        // Fallback for safety, though should be rare with pre-canonicalized paths.
        return lockpath.string();
    }
}

// Returns a filesystem::path that is safe to use with OS-level file APIs.
static std::filesystem::path canonical_lock_path_for_os(const std::filesystem::path &lockpath)
{
#if defined(PLATFORM_WIN64)
    // On Windows, always use the long-path format (`\\?\C:\...`) to avoid MAX_PATH limitations.
    return std::filesystem::path(pylabhub::format_tools::win32_to_long_path(lockpath));
#else
    // On POSIX, the canonical path is already suitable for OS calls.
    return lockpath;
#endif
}

namespace pylabhub::utils
{

// Polling interval for timed and blocking lock attempts. This value is a
// heuristic balance between CPU usage and responsiveness.
static constexpr std::chrono::milliseconds LOCK_POLLING_INTERVAL = std::chrono::milliseconds(20);

// --- Global Registries for Lock Management ---

// A registry of all lock files created by this process. This is used by the
// `cleanup()` function at shutdown to remove stale lock files.
static std::mutex g_lockfile_registry_mtx;
// Map: canonical key -> platform-specific path string.
static std::unordered_map<std::string, std::string> g_lockfile_registry;

// The process-local lock registry. This is the core of the intra-process
// (thread-to-thread) locking logic.
static std::mutex g_proc_registry_mtx;
struct ProcLockState
{
    int owners = 0;  // Number of FileLock instances in this process holding the lock.
    int waiters = 0; // Number of threads in this process waiting for the lock.
    std::condition_variable cv;
};
// Map: canonical key -> shared state for that lock.
static std::unordered_map<std::string, std::shared_ptr<ProcLockState>> g_proc_locks;


// ============================================================================
// FileLock Pimpl Implementation
// ============================================================================

// The private implementation of the FileLock.
struct FileLockImpl
{
    std::filesystem::path path; // The original resource path provided by the user.
    std::filesystem::path canonical_lock_file_path; // The canonical, absolute path of the .lock file.
    bool valid = false;                             // True if the lock is currently held.
    std::error_code ec;                             // Stores the last error if `valid` is false.
    std::string lock_key;       // The canonical key for the lock path in `g_proc_locks`.
    std::shared_ptr<ProcLockState> proc_state; // The shared process-local state for this lock.

#if defined(PLATFORM_WIN64)
    void *handle = nullptr; // Windows file handle (HANDLE) to the .lock file.
#else
    int fd = -1; // POSIX file descriptor for the .lock file.
#endif
};

// The custom deleter contains all the lock-release logic. It is called
// automatically when the std::unique_ptr<FileLockImpl> is destroyed.
void FileLock::FileLockImplDeleter::operator()(FileLockImpl *p)
{
    if (!p) return;

    if (p->valid)
    {
        LOGGER_DEBUG("FileLock: Releasing lock for '{}'", p->path.string());
    }

    // --- 1. Release the OS-level (inter-process) lock ---
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

    // --- 2. Release the process-local (intra-process) lock ---
    if (p->proc_state)
    {
        std::lock_guard<std::mutex> lg(g_proc_registry_mtx);
        if (--p->proc_state->owners == 0)
        {
            // If this was the last owner in the process, notify any waiting threads.
            LOGGER_TRACE("FileLock: Last process-local owner for '{}' released. Notifying waiters.",
                         p->lock_key);
            p->proc_state->cv.notify_all();
            // If there are no waiters, we can clean up the state object.
            if (p->proc_state->waiters == 0)
            {
                g_proc_locks.erase(p->lock_key);
            }
        }
        else
        {
            LOGGER_TRACE("FileLock: Process-local lock for '{}' released. {} owners remain.",
                         p->lock_key, p->proc_state->owners);
        }
    }

    delete p;
}

// Forward declarations for private helper functions.
static void open_and_lock(FileLockImpl *pImpl, ResourceType type, LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout);
static bool prepare_lock_path(FileLockImpl *pImpl, ResourceType type);
static bool acquire_process_local_lock(FileLockImpl *pImpl, LockMode mode,
                                       std::optional<std::chrono::milliseconds> timeout);
static void rollback_process_local_lock(FileLockImpl *pImpl);
static std::error_code ensure_lock_directory(FileLockImpl *pImpl);
static bool run_os_lock_loop(FileLockImpl *pImpl, LockMode mode,
                             std::optional<std::chrono::milliseconds> timeout);

std::filesystem::path FileLock::get_expected_lock_fullname_for(const std::filesystem::path &target,
                                                               ResourceType type) noexcept
{
    try
    {
        std::filesystem::path canonical_target;
        std::error_code ec;

        // First, try to get the fully resolved canonical path. This handles symlinks.
        canonical_target = std::filesystem::canonical(target, ec);

        // If canonical() fails (e.g., path doesn't exist), fall back to absolute().
        if (ec)
        {
            canonical_target = std::filesystem::absolute(target).lexically_normal();
        }

        if (type == ResourceType::Directory)
        {
            // For a directory, the lock file is named after the directory and
            // lives in its parent directory to avoid clutter.
            auto fname = canonical_target.filename();
            auto parent = canonical_target.parent_path();

            // Handle root paths like "/" or "C:\" where filename is empty.
            if (fname.empty() || fname == "." || fname == "..")
            {
                fname = "pylabhub_root";
            }

            fname += ".dir.lock";
            return parent / fname;
        }
        else
        {
            // For a file, the lock file is placed alongside the file.
            auto p = canonical_target;
            p += ".lock";
            return p;
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR(
            "FileLock::get_expected_lock_fullname_for threw an exception for target '{}': {}",
            target.string(), e.what());
        return {};
    }
    catch (...)
    {
        LOGGER_ERROR(
            "FileLock::get_expected_lock_fullname_for threw an unknown exception for target '{}'",
            target.string());
        return {};
    }
}

// ---------------- FileLock Public Method Implementations ----------------

// Note: We use `new FileLockImpl` because `std::make_unique` cannot be used with
// a Pimpl type that has a custom deleter defined separately.
FileLock::FileLock(const std::filesystem::path &path, ResourceType type, LockMode mode) noexcept
    : pImpl(new FileLockImpl)
{
    pImpl->path = path;
    open_and_lock(pImpl.get(), type, mode, std::nullopt);
}

FileLock::FileLock(const std::filesystem::path &path, ResourceType type,
                   std::chrono::milliseconds timeout) noexcept
    : pImpl(new FileLockImpl)
{
    pImpl->path = path;
    open_and_lock(pImpl.get(), type, LockMode::Blocking, timeout);
}

// The defaulted special members must be defined in the .cpp file where the
// Impl class and its deleter are complete types.
FileLock::~FileLock() = default;
FileLock::FileLock(FileLock &&) noexcept = default;
FileLock &FileLock::operator=(FileLock &&) noexcept = default;

bool FileLock::valid() const noexcept
{
    return pImpl && pImpl->valid;
}

std::error_code FileLock::error_code() const noexcept
{
    return pImpl ? pImpl->ec : std::error_code();
}

std::optional<std::filesystem::path> FileLock::get_locked_resource_path() const noexcept
{
    if (pImpl && pImpl->valid)
    {
        try
        {
            // The original path might not have been fully resolved, so re-resolve it here.
            return std::filesystem::absolute(pImpl->path).lexically_normal();
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FileLock::get_canonical_lock_file_path() const noexcept
{
    if (pImpl && pImpl->valid)
    {
        return pImpl->canonical_lock_file_path;
    }
    return std::nullopt;
}

// ---------------- Private Helper Function Implementations ----------------

// Main orchestration logic for acquiring a lock.
static void open_and_lock(FileLockImpl *pImpl, ResourceType type, LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout)
{
    if (!pImpl) return;

    pImpl->valid = false;
    pImpl->ec.clear();
    LOGGER_DEBUG("FileLock: Attempting to acquire {} lock on '{}'",
                 timeout ? "timed blocking" : (mode == LockMode::Blocking ? "blocking" : "non-blocking"),
                 pImpl->path.string());

    // 1. Determine the canonical path for the lock file.
    if (!prepare_lock_path(pImpl, type)) return;

    // 2. Acquire the process-local lock. This handles contention between threads
    //    in this process.
    if (!acquire_process_local_lock(pImpl, mode, timeout)) return;

    // 3. Ensure the directory for the lock file exists.
    if (auto ec = ensure_lock_directory(pImpl); ec)
    {
        pImpl->ec = ec;
        rollback_process_local_lock(pImpl);
        return;
    }

    // 4. Acquire the OS-level lock. This handles contention between processes.
    if (!run_os_lock_loop(pImpl, mode, timeout))
    {
        rollback_process_local_lock(pImpl);
        return;
    }

    // If all steps succeed, the lock is valid.
    pImpl->valid = true;
}

// Populates the canonical lock file path in the Impl struct.
static bool prepare_lock_path(FileLockImpl *pImpl, ResourceType type)
{
    auto lockpath = FileLock::get_expected_lock_fullname_for(pImpl->path, type);
    if (lockpath.empty())
    {
        pImpl->ec = std::make_error_code(std::errc::invalid_argument);
        LOGGER_WARN("FileLock: get_expected_lock_fullname_for failed for '{}'", pImpl->path.string());
        return false;
    }
    pImpl->canonical_lock_file_path = lockpath;
    return true;
}

// Manages the process-local (intra-process) lock acquisition.
static bool acquire_process_local_lock(FileLockImpl *pImpl, LockMode mode,
                                       std::optional<std::chrono::milliseconds> timeout)
{
    try
    {
        pImpl->lock_key = make_lock_key(pImpl->canonical_lock_file_path);
        LOGGER_TRACE("FileLock: Using lock key '{}'", pImpl->lock_key);
    }
    catch (const std::exception &e)
    {
        pImpl->ec = std::make_error_code(std::errc::io_error);
        LOGGER_WARN("FileLock: make_lock_key for '{}' threw: {}.", pImpl->canonical_lock_file_path.string(), e.what());
        return false;
    }

    std::unique_lock<std::mutex> regl(g_proc_registry_mtx);
    auto &state_ref = g_proc_locks[pImpl->lock_key];
    if (!state_ref)
    {
        state_ref = std::make_shared<ProcLockState>();
    }
    pImpl->proc_state = state_ref;

    if (mode == LockMode::NonBlocking)
    {
        if (state_ref->owners > 0)
        {
            pImpl->ec = std::make_error_code(std::errc::resource_unavailable_try_again);
            LOGGER_DEBUG("FileLock: Non-blocking lock on '{}' failed: already locked in-process.", pImpl->path.string());
            pImpl->proc_state.reset(); // Don't hold a reference if we failed.
            return false;
        }
    }
    else // Blocking or Timed
    {
        if (state_ref->owners > 0)
        {
            LOGGER_TRACE("FileLock: Lock on '{}' waiting for in-process release.", pImpl->path.string());
            struct ScopedWaiter {
                std::shared_ptr<ProcLockState> s;
                ScopedWaiter(std::shared_ptr<ProcLockState> state) : s(state) { if (s) s->waiters++; }
                ~ScopedWaiter() { if (s) s->waiters--; }
            } waiter_guard(state_ref);

            bool acquired = false;
            try {
                if (timeout)
                {
                    acquired = state_ref->cv.wait_for(regl, *timeout, [&] { return state_ref->owners == 0; });
                }
                else
                {
                    state_ref->cv.wait(regl, [&] { return state_ref->owners == 0; });
                    acquired = true;
                }
            } catch(...) { acquired = false; }


            if (!acquired)
            {
                pImpl->ec = std::make_error_code(std::errc::timed_out);
                LOGGER_DEBUG("FileLock: Timed out waiting for in-process lock on '{}'", pImpl->path.string());
                pImpl->proc_state.reset();
                return false;
            }
        }
    }

    state_ref->owners++;
    LOGGER_TRACE("FileLock: Acquired process-local lock for '{}'. Owners: {}", pImpl->path.string(), state_ref->owners);
    return true;
}

// Releases the process-local lock if the OS-level lock fails.
static void rollback_process_local_lock(FileLockImpl *pImpl)
{
    std::lock_guard<std::mutex> lg(g_proc_registry_mtx);
    if (pImpl->proc_state)
    {
        if (--pImpl->proc_state->owners == 0)
        {
            pImpl->proc_state->cv.notify_all();
            if (pImpl->proc_state->waiters == 0)
            {
                g_proc_locks.erase(pImpl->lock_key);
            }
        }
        pImpl->proc_state.reset();
    }
}

// Ensures the parent directory of the lock file exists.
static std::error_code ensure_lock_directory(FileLockImpl *pImpl)
{
    try
    {
        auto parent_dir = pImpl->canonical_lock_file_path.parent_path();
        if (!parent_dir.empty())
        {
            std::error_code create_ec;
            std::filesystem::create_directories(parent_dir, create_ec);
            if (create_ec)
            {
                LOGGER_WARN("FileLock: create_directories failed for {}: {}", parent_dir.string(), create_ec.message());
                return create_ec;
            }
        }
        return {};
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN("FileLock: create_directories threw an exception for {}: {}",
                    pImpl->canonical_lock_file_path.parent_path().string(), e.what());
        return std::make_error_code(std::errc::io_error);
    }
}

// Manages the OS-level (inter-process) lock acquisition loop.
static bool run_os_lock_loop(FileLockImpl *pImpl, LockMode mode,
                             std::optional<std::chrono::milliseconds> timeout)
{
    const auto &lockpath = pImpl->canonical_lock_file_path;
    std::chrono::steady_clock::time_point start_time;
    if (timeout)
    {
        start_time = std::chrono::steady_clock::now();
    }

#if defined(PLATFORM_WIN64)
    auto os_path = canonical_lock_path_for_os(lockpath);
    HANDLE h = CreateFileW(os_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        pImpl->ec = std::error_code(GetLastError(), std::system_category());
        LOGGER_WARN("FileLock: CreateFileW failed for '{}': {}", os_path.string(), pImpl->ec.message());
        return false;
    }

    auto guard = pylabhub::basics::make_scope_guard([&]() {
        if (!pImpl->valid) CloseHandle(h);
    });

    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    if (mode == LockMode::NonBlocking || timeout)
    {
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
    }

    while (true)
    {
        OVERLAPPED ov = {};
        if (LockFileEx(h, flags, 0, 1, 0, &ov))
        {
            auto reg_key = make_lock_key(lockpath);
            std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
            g_lockfile_registry.emplace(reg_key, os_path.string());

            pImpl->handle = reinterpret_cast<void *>(h);
            pImpl->valid = true;
            return true;
        }

        pImpl->ec = std::error_code(GetLastError(), std::system_category());
        if (mode == LockMode::NonBlocking || pImpl->ec.value() != ERROR_LOCK_VIOLATION)
        {
            return false;
        }
        
        if (timeout && (std::chrono::steady_clock::now() - start_time >= *timeout))
        {
            pImpl->ec = std::make_error_code(std::errc::timed_out);
            return false;
        }
        std::this_thread::sleep_for(LOCK_POLLING_INTERVAL);
    }
#else
    auto os_path = canonical_lock_path_for_os(lockpath);
    while (true)
    {
        int open_flags = O_CREAT | O_RDWR;
        #ifdef O_CLOEXEC
        open_flags |= O_CLOEXEC;
        #endif
        int fd = ::open(os_path.c_str(), open_flags, 0666);
        if (fd == -1)
        {
            pImpl->ec = std::error_code(errno, std::generic_category());
            LOGGER_WARN("FileLock: open failed for '{}': {}", os_path.string(), pImpl->ec.message());
            return false;
        }
        #ifndef O_CLOEXEC
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
        #endif

        int flock_op = LOCK_EX;
        if (mode == LockMode::NonBlocking || timeout)
        {
            flock_op |= LOCK_NB;
        }

        if (flock(fd, flock_op) == 0)
        {
            auto reg_key = make_lock_key(lockpath);
            std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
            g_lockfile_registry.emplace(reg_key, os_path.string());
            
            pImpl->fd = fd;
            pImpl->valid = true;
            return true;
        }

        ::close(fd);

        if (errno != EWOULDBLOCK && errno != EAGAIN)
        {
            pImpl->ec = std::error_code(errno, std::generic_category());
            return false;
        }
        
        if (mode == LockMode::NonBlocking)
        {
            pImpl->ec = std::make_error_code(std::errc::resource_unavailable_try_again);
            return false;
        }
        
        if (timeout && (std::chrono::steady_clock::now() - start_time >= *timeout))
        {
            pImpl->ec = std::make_error_code(std::errc::timed_out);
            return false;
        }
        std::this_thread::sleep_for(LOCK_POLLING_INTERVAL);
    }
#endif
}

// Safely cleans up stale lock files at program exit.
void FileLock::cleanup()
{
    std::vector<std::pair<std::string, std::string>> candidates;
    {
        std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
        candidates.reserve(g_lockfile_registry.size());
        for (auto &kv : g_lockfile_registry)
            candidates.emplace_back(kv.first, kv.second);
    }

    LOGGER_DEBUG("FileLock: Safe-cleaning up to {} registered lock files.", candidates.size());
    std::vector<std::string> removed_keys;

    for (const auto &kv : candidates)
    {
        const std::string &reg_key = kv.first;
        const std::string &path_str = kv.second;
        std::filesystem::path p = canonical_lock_path_for_os(path_str);
        
        // Attempt a non-blocking lock. If it succeeds, no one else holds the lock
        // and it's safe to delete.
        FileLock maybe_stale_lock(p, ResourceType::File, LockMode::NonBlocking);
        if(maybe_stale_lock.valid())
        {
             std::error_code ec;
             std::filesystem::remove(p, ec);
             if (ec) {
                 LOGGER_WARN("FileLock::cleanup: remove '{}' failed: {}", p.string(), ec.message());
             } else {
                 LOGGER_TRACE("FileLock::cleanup: removed stale lock file '{}'", p.string());
                 removed_keys.push_back(reg_key);
             }
        } else {
             LOGGER_TRACE("FileLock::cleanup: lock for '{}' is still held, skipping.", p.string());
        }
    }

    if (!removed_keys.empty())
    {
        std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
        for (const auto &k : removed_keys) g_lockfile_registry.erase(k);
    }
    LOGGER_DEBUG("FileLock: cleanup done. removed={}, skipped={}", removed_keys.size(),
                 candidates.size() - removed_keys.size());
}


namespace
{
// C-style function wrapper for the lifecycle manager.
void do_filelock_cleanup() {
    FileLock::cleanup();
}

// Static registrar object to hook into the application lifecycle.
struct FileLockFinalizer
{
    FileLockFinalizer()
    {
        // Register FileLock::cleanup to be called during application shutdown.
        ModuleDef module("pylabhub::utils::FileLockCleanup");
        module.add_dependency("pylabhub::utils::Logger"); // Depends on Logger.
        module.set_shutdown(&do_filelock_cleanup, 2000 /*ms timeout*/);
        LifecycleManager::instance().register_module(std::move(module));
    }
};

// This global instance's constructor ensures the finalizer is registered on startup.
static FileLockFinalizer g_finalizer_instance;

} // namespace

} // namespace pylabhub::utils