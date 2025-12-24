/*******************************************************************************
 * @file FileLock.cpp
 * @brief Implementation of a cross-platform RAII advisory file lock using a separate lock file.
 *
 * **Design Philosophy**
 * `FileLock` provides a robust, cross-platform, RAII-style mechanism for
 * managing inter-process *advisory* file locks. It is a critical component for
 * ensuring the integrity of shared resources like configuration files.
 *
 * 1.  **RAII (Resource Acquisition Is Initialization)**: The lock is acquired in
 *     the constructor and automatically released in the destructor. This prevents
 *     leaked locks, even in the presence of exceptions.
 * 2.  **Path Canonicalization**: To ensure that different filesystem paths
 *     pointing to the same underlying resource (e.g., via relative paths,
 *     different symlinks) contend for the same lock, `FileLock` canonicalizes
 *     the resource path. It first attempts to use `std::filesystem::canonical`
 *     to resolve symlinks, falling back to `std::filesystem::absolute` if the
 *     resource does not yet exist. This provides robust locking for existing
 *     files while still supporting the creation of new, locked files.
 * 3.  **Cross-Platform Abstraction**: The class provides a unified interface over
 *     divergent platform-specific locking primitives:
 *     - **POSIX**: Uses `flock()` on a dedicated `.lock` file. This is a widely
 *       supported and robust advisory locking mechanism for *local* filesystems.
 *     - **Windows**: Uses `LockFileEx()` on a handle to a dedicated `.lock` file.
 * 4.  **Lock File Strategy**: A separate lock file (e.g., `config.json.lock` for
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
 * 5.  **Blocking and Non-Blocking Modes**: The lock can be acquired in either
 *     `Blocking` or `NonBlocking` mode, as well as a timed blocking mode. This
 *     flexibility is used by `JsonConfig` to avoid deadlocks.
 * 6.  **Movability**: The class is movable but not copyable, allowing ownership
 *     of a lock to be efficiently transferred (e.g., returned from a factory
 *     function) while preventing accidental duplication.
 *
 * **Thread Safety**
 * A `FileLock` instance is designed to provide consistent behavior for both
 * inter-process and intra-process (multi-threaded) contention. While a single
 * `FileLock` object is not thread-safe to be accessed concurrently, the
 * underlying mechanism ensures that multiple `FileLock` instances (even in the
 * same process) attempting to lock the same resource will correctly block or
 * fail according to their `LockMode`.
 ******************************************************************************/

#include "utils/FileLock.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include "utils/PathUtil.hpp"
#include "utils/ScopeGuard.hpp"

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
#include <set>

#if defined(PLATFORM_WIN64)
#include <sstream>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// Portable canonicalizer for registry key. Produces a stable key for a lock file path.
// Assumes lockpath is already an absolute, normalized path.
// - On Windows, convert to long path form and to lower-case to ensure case-insensitive match.
static std::string make_lock_key(const std::filesystem::path &lockpath)
{
    try
    {
#if defined(PLATFORM_WIN64)
        // Convert to long path and lowercase for stable key
        std::wstring longw =
            pylabhub::utils::win32_to_long_path(lockpath); 
        // Lowercase the wchar_t string to normalize case (Windows is case-insensitive)
        for (auto &ch : longw)
            ch = towlower(ch);
        // Convert to UTF-8 using the modern, non-deprecated Windows API.
        return wstring_to_utf8(longw);
#else
        return lockpath.generic_string(); // use generic_string to avoid platform separators
#endif
    }
    catch (...)
    {
        // Fallback: use plain string (shouldn't usually happen with canonical paths)
        return lockpath.string();
    }
}

// New helpers: canonical registry key and canonical path suitable for OS calls.

// Return canonical, platform-aware string key for registry storage / comparison.
// Delegates to make_lock_key (which already performs Windows long-path + lowercase).
static std::string canonical_lock_registry_key(const std::filesystem::path &lockpath)
{
    return make_lock_key(lockpath);
}

// Return a filesystem::path that is safe to use with OS APIs on the current platform.
// Assumes lockpath is already an absolute, normalized path.
static std::filesystem::path canonical_lock_path_for_os(const std::filesystem::path &lockpath)
{
#if defined(PLATFORM_WIN64)
    // The OS path needs the long-path prefix for reliability.
    std::wstring w = pylabhub::utils::win32_to_long_path(lockpath);
    return std::filesystem::path(w);
#else
    // On POSIX, the canonical path is already what we need for OS calls.
    return lockpath;
#endif
}

namespace pylabhub::utils
{

// Polling interval for timed and blocking lock attempts.
// This value is a heuristic balance between CPU usage (shorter sleep means
// more polling) and responsiveness (longer sleep means slower reaction to a
// released lock). 20ms is a reasonable default.
static constexpr std::chrono::milliseconds LOCK_POLLING_INTERVAL = std::chrono::milliseconds(20);

// Global registry for tracking created lock files on all platforms
// to allow for cleanup at program exit.
static std::mutex g_lockfile_registry_mtx;
// Map: canonical registry key -> original lockpath string (platform-specific string form).
// Storing the original path string (value) ensures cleanup/open can use the correct path
// representation for OS calls (we will convert value back to fs::path via canonical_lock_path_for_os).
static std::unordered_map<std::string, std::string> g_lockfile_registry;

// --- Process-local lock registry ---
// This registry ensures that even on Windows (where native file locks are
// per-process, meaning a second LockFileEx call from another thread in the
// same process on the same file will succeed immediately), intra-process
// locking attempts will behave consistently (blocking or failing as expected)
// across all platforms.
static std::mutex g_registry_mtx;
struct ProcLockState
{
    int owners = 0;  // number of FileLock holders in this process for that path
    int waiters = 0; // number of threads waiting for this lock in this process
    std::condition_variable cv;
};
static std::unordered_map<std::string, std::shared_ptr<ProcLockState>> g_proc_locks;

// --- Pimpl Definition ---

// The private implementation of the FileLock.
// All state is held here to provide ABI stability for the public FileLock class.
struct FileLockImpl
{
    std::filesystem::path path; // The original path for which a lock is requested.
    std::filesystem::path canonical_lock_file_path; // The canonical, absolute path of the lock file (e.g., .json.lock)
    bool valid = false;         // True if the lock is currently held.
    std::error_code ec;         // Stores the last error if `valid` is false.
    std::string lock_key;       // The canonical key for the lock path in g_proc_locks.
    std::shared_ptr<ProcLockState> proc_state; // The process-local state for this lock.

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
            LOGGER_TRACE("FileLock: Last process-local owner for '{}' released. Notifying waiters.",
                         p->lock_key);
            p->proc_state->cv.notify_all();
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

    // Finally, delete the memory for the Pimpl object itself.
    delete p;
}

// Forward declarations for private helper functions
static void open_and_lock(FileLockImpl *pImpl, ResourceType type, LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout);

static bool prepare_lock_path(FileLockImpl *pImpl, ResourceType type);
static bool acquire_process_local_lock(FileLockImpl *pImpl, LockMode mode,
                                       std::optional<std::chrono::milliseconds> timeout);
static void rollback_process_local_lock(FileLockImpl *pImpl);
static std::error_code ensure_lock_directory(FileLockImpl *pImpl);
static bool run_os_lock_loop(FileLockImpl *pImpl, LockMode mode,
                             std::optional<std::chrono::milliseconds> timeout);

// This function is now a public static member of FileLock.
std::filesystem::path FileLock::get_expected_lock_fullname_for(const std::filesystem::path &target,
                                                               ResourceType type) noexcept
{
    try
    {
        // To handle different path representations (relative, symlinks), we
        // resolve the path to a canonical form.
        std::filesystem::path canonical_target;
        std::error_code ec;

        // std::filesystem::canonical resolves symlinks but requires the path to exist.
        canonical_target = std::filesystem::canonical(target, ec);

        // If canonical() fails (e.g., path does not exist), fall back to
        // absolute() which handles relative paths but not symlinks. This allows
        // locking a resource before it is created.
        if (ec)
        {
            canonical_target = std::filesystem::absolute(target).lexically_normal();
        }

        if (type == ResourceType::Directory)
        {
            // For a directory, the lock file is named after the directory and
            // lives in its parent directory.
            auto fname = canonical_target.filename();
            auto parent = canonical_target.parent_path();

            // If the filename is empty (e.g. for root paths like "/" or "C:\"),
            // use a fixed, safe name for the lock file.
            if (fname.empty() || fname == "." || fname == "..")
            {
                fname = "pylabhub_root";
            }

            fname += ".dir.lock";
            return parent / fname;
        }
        else
        {
            // For a file, the lock is alongside the file.
            auto p = canonical_target;
            p += ".lock";
            return p;
        }
    }
    catch (const std::exception &e)
    {
        // Filesystem operations can throw. In a noexcept function, the best we
        // can do is log and return an empty path to signal failure.
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
FileLock::FileLock(const std::filesystem::path &path, ResourceType type,
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

std::optional<std::filesystem::path> FileLock::get_locked_resource_path() const noexcept
{
    if (pImpl && pImpl->valid)
    {
        try
        {
            // The pImpl->path stores the original resource path provided by the user.
            // Using absolute().lexically_normal() here is important because the original
            // path might not have been fully resolved (e.g., relative path, or a path
            // that doesn't exist when canonical() was attempted).
            return std::filesystem::absolute(pImpl->path).lexically_normal();
        }
        catch (...)
        {
            // In case std::filesystem::absolute throws, return empty.
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FileLock::get_canonical_lock_file_path() const noexcept
{
    if (pImpl && pImpl->valid)
    {
        // This path is already canonical and stored.
        return pImpl->canonical_lock_file_path;
    }
    return std::nullopt;
}

// ---------------- Private Helper Implementation ----------------

// Orchestrates the locking process
static void open_and_lock(FileLockImpl *pImpl, ResourceType type, LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout)
{
    if (!pImpl)
        return;

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

    // 1. Prepare and validate lock path, storing it in pImpl.
    if (!prepare_lock_path(pImpl, type))
    {
        // pImpl->ec and logging handled inside prepare_lock_path
        return;
    }

    // 2. Acquire Process-Local Lock
    // This ensures correct blocking behavior between threads in the same process.
    if (!acquire_process_local_lock(pImpl, mode, timeout))
    {
        // pImpl->ec and logging handled inside acquire_process_local_lock
        return;
    }

    // 3. Ensure OS-level Lock Directory
    if (auto ec = ensure_lock_directory(pImpl); ec)
    {
        pImpl->ec = ec;
        pImpl->valid = false;
        rollback_process_local_lock(pImpl);
        return;
    }

    // 4. Run OS-level Lock Loop (LockFileEx / flock)
    if (!run_os_lock_loop(pImpl, mode, timeout))
    {
        // pImpl->ec handled inside run_os_lock_loop
        pImpl->valid = false;
        rollback_process_local_lock(pImpl);
        return;
    }

    // Success
}

static bool prepare_lock_path(FileLockImpl *pImpl, ResourceType type)
{
    auto lockpath = FileLock::get_expected_lock_fullname_for(pImpl->path, type);
    if (lockpath.empty())
    {
        pImpl->ec = std::make_error_code(std::errc::invalid_argument);
        pImpl->valid = false;
        LOGGER_WARN("FileLock: get_expected_lock_fullname_for failed for '{}'",
                    pImpl->path.string());
        return false;
    }
    pImpl->canonical_lock_file_path = lockpath; // Store the canonical lock file path
    return true;
}

static bool acquire_process_local_lock(FileLockImpl *pImpl, LockMode mode,
                                       std::optional<std::chrono::milliseconds> timeout)
{
    try
    {
        // Normalize the path to generate a consistent key for the lock registry.
        pImpl->lock_key = make_lock_key(pImpl->canonical_lock_file_path);
        LOGGER_TRACE("FileLock: Using lock key '{}'", pImpl->lock_key);
    }
    catch (const std::exception &e)
    {
        pImpl->ec = std::make_error_code(std::errc::io_error);
        LOGGER_WARN("FileLock: make_lock_key path conversion for '{}' threw: {}.",
                    pImpl->canonical_lock_file_path.string(), e.what());
        pImpl->valid = false;
        return false;
    }

    std::unique_lock<std::mutex> regl(g_registry_mtx);
    auto &state_ref = g_proc_locks[pImpl->lock_key];
    if (!state_ref)
    {
        state_ref = std::make_shared<ProcLockState>();
    }
    pImpl->proc_state = state_ref;
    auto state = pImpl->proc_state;

    bool acquired = false;

    if (mode == LockMode::NonBlocking)
    {
        if (state->owners > 0)
        {
            pImpl->ec = std::make_error_code(std::errc::resource_unavailable_try_again);
            pImpl->valid = false;
            LOGGER_DEBUG("FileLock: Non-blocking lock on '{}' failed: already locked in-process.",
                         pImpl->path.string());

            if (state->owners == 0 && state->waiters == 0)
            {
                g_proc_locks.erase(pImpl->lock_key);
            }
            pImpl->proc_state.reset();
            return false;
        }
        acquired = true;
    }
    else
    {
        if (state->owners > 0)
        {
            LOGGER_TRACE("FileLock: Lock on '{}' waiting for in-process release.",
                         pImpl->path.string());

            // RAII guard to ensure waiters is decremented even if an exception occurs
            struct ScopedWaiter
            {
                ProcLockState &s;
                ScopedWaiter(ProcLockState &state) : s(state) { s.waiters++; }
                ~ScopedWaiter() { s.waiters--; }
            };
            ScopedWaiter waiter_guard(*state);

            try
            {
                if (timeout)
                {
                    if (state->cv.wait_for(regl, *timeout, [&] { return state->owners == 0; }))
                    {
                        acquired = true;
                    }
                }
                else
                {
                    state->cv.wait(regl, [&] { return state->owners == 0; });
                    acquired = true;
                }
            }
            catch (...)
            {
                // If wait throws, detach and return error to prevent crash (noexcept)
                pImpl->ec = std::make_error_code(std::errc::resource_unavailable_try_again);
                pImpl->valid = false;

                if (state->owners == 0 && state->waiters == 0)
                {
                    g_proc_locks.erase(pImpl->lock_key);
                }
                pImpl->proc_state.reset();
                return false;
            }

            if (!acquired)
            {
                // Timed out waiting for the in-process lock.
                pImpl->ec = std::make_error_code(std::errc::timed_out);
                pImpl->valid = false;
                LOGGER_DEBUG("FileLock: Timed out waiting for in-process lock on '{}'",
                             pImpl->path.string());

                if (state->owners == 0 && state->waiters == 0)
                {
                    g_proc_locks.erase(pImpl->lock_key);
                }
                pImpl->proc_state.reset();
                return false;
            }
        }
        acquired = true;
    }

    if (acquired)
    {
        state->owners++;
        LOGGER_TRACE("FileLock: Acquired process-local lock for '{}'. Owners: {}",
                     pImpl->path.string(), state->owners);
        return true;
    }
    return false;
}

static void rollback_process_local_lock(FileLockImpl *pImpl)
{
    std::lock_guard<std::mutex> lg(g_registry_mtx);
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
                LOGGER_WARN("FileLock: create_directories failed for {} err={}",
                            parent_dir.string(), create_ec.message());
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


// OS-level lock acquisition loop for both Windows and POSIX.
// Handles both blocking and non-blocking modes, including timed blocking.
static bool run_os_lock_loop(FileLockImpl *pImpl, LockMode mode,
                             std::optional<std::chrono::milliseconds> timeout)
{
    const auto &lockpath = pImpl->canonical_lock_file_path;
    const char *mode_str = (mode == LockMode::Blocking) ? "blocking" : "non-blocking";
    if (timeout)
        mode_str = "timed blocking";

    std::chrono::steady_clock::time_point start_time;
    if (timeout)
    {
        start_time = std::chrono::steady_clock::now();
    }

#if defined(PLATFORM_WIN64)
    auto os_path = canonical_lock_path_for_os(lockpath);
    std::wstring lockpath_w = os_path.wstring();

    HANDLE h = CreateFileW(lockpath_w.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        pImpl->ec = std::error_code(GetLastError(), std::system_category());
        LOGGER_WARN("FileLock: CreateFileW failed for {} err={}", lockpath.string(),
                    pImpl->ec.value());
        return false;
    }

    // Ensure handle is closed if lock not acquired.
    // On success, pImpl->handle will take ownership, so we don't close it here.
    bool acquired_os_lock = false;
    auto guard = pylabhub::utils::make_scope_guard([&]() {
        if (!acquired_os_lock)
        {
            CloseHandle(h);
        }
    });

    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    OVERLAPPED ov = {};

    if (mode == LockMode::NonBlocking)
    {
        // For non-blocking, just try once with FAIL_IMMEDIATELY.
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
        if (LockFileEx(h, flags, 0, 1, 0, &ov))
        {
            acquired_os_lock = true;
        }
        else
        {
            pImpl->ec = std::error_code(GetLastError(), std::system_category());
            LOGGER_DEBUG("FileLock: Non-blocking LockFileEx failed for '{}'", pImpl->path.string());
        }
    }
    else if (timeout)
    {
        // For timed blocking, poll with FAIL_IMMEDIATELY and check timeout.
        flags |= LOCKFILE_FAIL_IMMEDIATELY;
        while (true)
        {
            if (LockFileEx(h, flags, 0, 1, 0, &ov))
            {
                acquired_os_lock = true;
                break;
            }
            // LockFileEx failed, check for timeout
            pImpl->ec = std::error_code(GetLastError(), std::system_category());
            if (std::chrono::steady_clock::now() - start_time >= *timeout)
            {
                pImpl->ec = std::make_error_code(std::errc::timed_out);
                LOGGER_DEBUG("FileLock: Timed out acquiring OS lock for '{}'", pImpl->path.string());
                return false; // Return false, guard will run
            }
            std::this_thread::sleep_for(LOCK_POLLING_INTERVAL);
        }
    }
    else // Blocking mode without timeout
    {
        // For blocking, call LockFileEx without FAIL_IMMEDIATELY; it will block until acquired.
        if (LockFileEx(h, flags, 0, 1, 0, &ov))
        {
            acquired_os_lock = true;
        }
        else
        {
            // This should typically not be reached for a successful blocking lock.
            // If it is, it indicates a serious error other than busy.
            pImpl->ec = std::error_code(GetLastError(), std::system_category());
            LOGGER_WARN("FileLock: Blocking LockFileEx failed unexpectedly for {} with error {}",
                        lockpath.string(), pImpl->ec.message());
        }
    }

    if (acquired_os_lock)
    {
        // Add lock file to registry for cleanup at exit.
        auto reg_key = canonical_lock_registry_key(lockpath);
        auto reg_val = os_path.string();
        std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
        g_lockfile_registry.emplace(reg_key, reg_val);

        pImpl->handle = reinterpret_cast<void *>(h); // Transfer ownership of handle to pImpl
        pImpl->ec.clear();
        pImpl->valid = true;
        LOGGER_DEBUG("FileLock: Successfully acquired {} lock on '{}'", mode_str,
                     pImpl->path.string());
        return true;
    }
    else
    {
        // Lock acquisition failed (pImpl->ec is already set by LockFileEx failure or timeout).
        return false; // Return false, guard will run
    }
#else
    // POSIX implementation.
    // To ensure strict exclusivity of flock across processes, each attempt
    // to acquire the lock must be associated with a fresh open file descriptor.
    // If a lock attempt fails, the file descriptor must be closed before retrying.
    // This makes the POSIX behavior consistent with the Windows `try_acquire_os_lock_once_win`
    // which effectively opens and closes the handle on each attempt if it fails to lock.
    int fd = -1; // Initialize fd outside the loop.
    auto os_path = canonical_lock_path_for_os(lockpath);

    while (true)
    {
        // Always open the file inside the loop for each attempt. This ensures a fresh,
        // independent file descriptor for flock exclusivity.
        int open_flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
        open_flags |= O_CLOEXEC;
#endif
        fd = ::open(os_path.c_str(), open_flags, 0666);
        if (fd == -1)
        {
            pImpl->ec = std::error_code(errno, std::generic_category());
            LOGGER_WARN("FileLock: open failed for {} err={}", os_path.string(),
                        pImpl->ec.message());
            return false;
        }

#ifndef O_CLOEXEC
        // Fallback if O_CLOEXEC is not defined, ensure it's set for safety on exec.
        int oldfl = fcntl(fd, F_GETFD);
        if (oldfl != -1)
            fcntl(fd, F_SETFD, oldfl | FD_CLOEXEC);
#endif

        int flock_op = LOCK_EX;
        if (mode == LockMode::NonBlocking || timeout.has_value())
        {
            flock_op |= LOCK_NB; // Use non-blocking flag for polling.
        }

        if (flock(fd, flock_op) == 0)
        {
            // Lock acquired. Store this fd and break the loop.
            pImpl->fd = fd;
            // Add lock file to registry for cleanup at exit.
            {
                auto reg_key = canonical_lock_registry_key(lockpath);
                auto reg_val = os_path.string();
                std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
                g_lockfile_registry.emplace(reg_key, reg_val);
            }
            break; // Lock successfully acquired, exit retry loop.
        }

        // flock failed.
        ::close(fd); // Close the fd for this failed attempt before retrying.

        if (errno != EWOULDBLOCK && errno != EAGAIN) // True error, not just busy (like EBADF)
        {
            pImpl->ec = std::error_code(errno, std::generic_category());
            LOGGER_WARN("FileLock: flock failed for {} err={}", lockpath.string(), pImpl->ec.message());
            return false;
        }

        // Lock is busy (EWOULDBLOCK or EAGAIN).
        if (mode == LockMode::NonBlocking)
        {
            pImpl->ec = std::make_error_code(std::errc::resource_unavailable_try_again);
            LOGGER_DEBUG("FileLock: Non-blocking lock failed for '{}'", pImpl->path.string());
            return false;
        }

        // Busy in Timed Blocking mode, check timeout and retry.
        if (timeout)
        {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed >= *timeout)
            {
                pImpl->ec = std::make_error_code(std::errc::timed_out);
                LOGGER_DEBUG("FileLock: Timed out acquiring OS lock for '{}'",
                             pImpl->path.string());
                return false;
            }
        }
        std::this_thread::sleep_for(LOCK_POLLING_INTERVAL);
    }

    // If we reach here, the lock was acquired.
    pImpl->valid = true;
    pImpl->ec.clear();
    LOGGER_DEBUG("FileLock: Successfully acquired {} lock on '{}'", mode_str, pImpl->path.string());
    return true;
#endif
}


// Static cleanup function to remove stale lock files at program exit.
// We have to be careful here because deletion of a lock file without knowing
// whether it is still held by another process can lead to breaking of the locking
// semantics. Therefore, we attempt to acquire an exclusive lock on each registered
// lock file before deleting it. If we can acquire the lock, we assume no other
// process holds it and it is safe to delete. If we cannot acquire the lock,
// we skip deletion for that file.
// Also we will first make a copy of the registry to avoid holding the mutex
// during blocking I/O operations.
// This function is registered to be called during program finalization.
void FileLock::cleanup()
{
    // Copy the registry entries locally (key->path_str) so we don't hold the mutex during blocking I/O.
    std::vector<std::pair<std::string, std::string>> candidates;
    {
        std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
        candidates.reserve(g_lockfile_registry.size());
        for (auto &kv : g_lockfile_registry)
            candidates.emplace_back(kv.first, kv.second);
    }

    LOGGER_DEBUG("FileLock: Safe-cleaning {} registered lock files.", candidates.size());
    std::vector<std::string> removed_keys;

    for (const auto &kv : candidates)
    {
        const std::string &reg_key = kv.first;
        const std::string &path_str = kv.second;
        std::filesystem::path p(path_str); // construct platform path from stored string

#if defined(PLATFORM_WIN64)
        bool deleted = false;
        std::wstring wpath = pylabhub::utils::win32_to_long_path(p);
        HANDLE h = CreateFileW(wpath.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            LOGGER_TRACE("FileLock::cleanup: cannot open {}, skipping.", path_str);
            continue;
        }

        OVERLAPPED ov = {};
        BOOL ok = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov);
        if (!ok)
        {
            CloseHandle(h);
            LOGGER_TRACE("FileLock::cleanup: lock held on {}, skipping.", path_str);
            continue;
        }

        if (DeleteFileW(wpath.c_str()))
        {
            LOGGER_TRACE("FileLock::cleanup: removed {}", path_str);
            deleted = true;
        }
        else
        {
            DWORD err = GetLastError();
            LOGGER_TRACE("FileLock::cleanup: DeleteFile failed for {} err={} - skipping delete.", path_str, static_cast<int>(err));
        }

        UnlockFileEx(h, 0, 1, 0, &ov);
        CloseHandle(h);

        if (deleted) removed_keys.push_back(reg_key);

#else
        bool deleted = false;
        int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd == -1)
        {
            LOGGER_TRACE("FileLock::cleanup: cannot open {}, skipping.", path_str);
            continue;
        }

        if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(fd);
            LOGGER_TRACE("FileLock::cleanup: lock held on {}, skipping.", path_str);
            continue;
        }

        std::error_code ec;
        std::filesystem::remove(p, ec);
        if (ec) {
            LOGGER_WARN("FileLock::cleanup: remove {} failed: {}", path_str, ec.message());
        } else {
            LOGGER_TRACE("FileLock::cleanup: removed {}", path_str);
            deleted = true;
        }

        flock(fd, LOCK_UN);
        ::close(fd);

        if (deleted) removed_keys.push_back(reg_key);
#endif
    }

    // Erase only those entries that were actually removed.
    if (!removed_keys.empty())
    {
        std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
        for (const auto &k : removed_keys) g_lockfile_registry.erase(k);
    }

    LOGGER_DEBUG("FileLock: cleanup done. removed={} skipped={}", removed_keys.size(),
                 candidates.size() - removed_keys.size());
}


namespace
{
// This static object's constructor will register the cleanup function
// to be called by the global Finalize().
struct FileLockFinalizer
{
    FileLockFinalizer()
    {
        // Register FileLock::cleanup to be called during shutdown.
        // A short timeout is fine, as it's a fast, best-effort cleanup.
        RegisterFinalizer("FileLock::cleanup", &FileLock::cleanup, std::chrono::seconds(1));
    }
};

// Create this object to register the finalizer on all platforms.
static FileLockFinalizer g_finalizer_instance;

} // namespace

} // namespace pylabhub::utils