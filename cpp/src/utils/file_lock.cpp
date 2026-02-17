// file_lock.cpp
#include "plh_base.hpp"
#include "utils/lifecycle.hpp"
#include "utils/file_lock.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#if defined(PYLABHUB_IS_POSIX)
#include <fcntl.h>
#include <sys/file.h> // added for flock
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

using namespace pylabhub::platform;

// Module-level flag to indicate if the FileLock has been initialized.
static std::atomic<bool> g_filelock_initialized{false};

namespace
{
constexpr int kLockFileMode = 0644;
constexpr std::chrono::milliseconds kFileLockShutdownTimeoutMs{2000};
}

static std::string make_lock_key(const std::filesystem::path &lockpath)
{
    try
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        std::wstring longw = pylabhub::format_tools::win32_to_long_path(lockpath);

        if (longw.empty())
        {
            return lockpath.generic_string();
        }

        for (auto &ch : longw)
            ch = towlower(ch);

        return pylabhub::format_tools::ws2s(longw);
#else
        // Use the lexically-normal absolute representation for a deterministic key
        try
        {
            auto abs = std::filesystem::absolute(lockpath).lexically_normal();
            return abs.generic_string();
        }
        catch (...)
        {
            return lockpath.generic_string();
        }
#endif
    }
    catch (...)
    {
        return lockpath.string();
    }
}

static std::filesystem::path canonical_lock_path_for_os(const std::filesystem::path &lockpath)
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    auto wpath = pylabhub::format_tools::win32_to_long_path(lockpath);
    if (wpath.empty())
    {
        return lockpath;
    }

    return std::filesystem::path(wpath);
#else
    return lockpath;
#endif
}

namespace pylabhub::utils
{

static constexpr std::chrono::milliseconds LOCK_POLLING_INTERVAL = std::chrono::milliseconds(20);

static std::mutex g_proc_registry_mtx;
struct ProcLockState
{
    int owners = 0;
    int waiters = 0;
    std::condition_variable cv;
};
static std::unordered_map<std::string, std::shared_ptr<ProcLockState>> g_proc_locks;

// Pimpl Implementation
struct FileLockImpl
{
    std::filesystem::path path;
    std::filesystem::path canonical_lock_file_path;
    bool valid = false;
    std::error_code ec;
    std::string lock_key;
    std::shared_ptr<ProcLockState> proc_state;

#if defined(PYLABHUB_PLATFORM_WIN64)
    void *handle = nullptr;
#else
    int fd = -1;
#endif
};

void FileLock::FileLockImplDeleter::operator()(FileLockImpl *ptr)
{
    if (ptr == nullptr)
    {
        return;
    }

    if (ptr->valid)
    {
        // This is noisy, so keep it at TRACE level
        // LOGGER_DEBUG("FileLock: Releasing lock for '{}'", p->path.string());
    }

    if (ptr->valid)
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        if (ptr->handle != nullptr)
        {
            OVERLAPPED ov = {};
            UnlockFileEx((HANDLE)ptr->handle, 0, 1, 0, &ov);
            CloseHandle((HANDLE)ptr->handle);
            ptr->handle = nullptr;
        }
#else
        if (ptr->fd != -1)
        {
            // Release using flock (kernel advisory whole-file unlock)
            // Best-effort: ignore errors on unlock/close path.
            flock(ptr->fd, LOCK_UN);
            ::close(ptr->fd);
            ptr->fd = -1;
        }
#endif
    }

    if (ptr->proc_state != nullptr)
    {
        std::lock_guard<std::mutex> lock_guard(g_proc_registry_mtx);
        if (--ptr->proc_state->owners == 0)
        {
            ptr->proc_state->cv.notify_all();
            if (ptr->proc_state->waiters == 0)
            {
                g_proc_locks.erase(ptr->lock_key);
            }
        }
    }

    delete ptr;
}

// Forward declarations
static void open_and_lock(FileLockImpl *pImpl, ResourceType type, LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout);
static bool prepare_lock_path(FileLockImpl *pImpl, ResourceType type);
static bool acquire_process_local_lock(FileLockImpl *pImpl, LockMode mode,
                                       std::optional<std::chrono::milliseconds> timeout);
static void rollback_process_local_lock(FileLockImpl *pImpl);
static std::error_code ensure_lock_directory(FileLockImpl *pImpl);
static bool run_os_lock_loop(FileLockImpl *pImpl, LockMode mode,
                             std::optional<std::chrono::milliseconds> timeout);

std::filesystem::path FileLock::get_expected_lock_fullname_for(const std::filesystem::path &path,
                                                               ResourceType type) noexcept
{
    try
    {
        // A path containing a null character or other control characters is invalid.
        constexpr int kFirstControlCharLimit = 32;
        for (const auto &path_char : path.native())
        {
            if (path_char >= 0 && path_char < kFirstControlCharLimit)
            {
                return {};
            }
        }

        std::filesystem::path canonical_target;
        std::error_code err_code;
        canonical_target = std::filesystem::canonical(path, err_code);

        if (err_code)
        {
            canonical_target = std::filesystem::absolute(path).lexically_normal();
        }

        if (type == ResourceType::Directory)
        {
            auto fname = canonical_target.filename();
            auto parent = canonical_target.parent_path();
            if ((canonical_target.has_root_name() && !canonical_target.has_parent_path()) ||
                fname.empty() || fname == "." || fname == "..")
            {
                fname = "pylabhub_root";
            }
            fname += ".dir.lock";
            return parent / fname;
        }
        auto lock_path = canonical_target;
        lock_path += ".lock";
        return lock_path;
    }
    catch (...)
    {
        // This function must not throw.
        return {};
    }
}

// -----------------------------------------------------------------------------
// Exception and panic contract (pImpl-safe, no accidental termination)
// -----------------------------------------------------------------------------
// - All public APIs (constructors, try_lock) are noexcept. No exceptions propagate.
// - Allocation: we use new (std::nothrow). On OOM, direct ctors leave pImpl nullptr
//   (valid() false); try_lock returns std::nullopt. Callers must check valid() or
//   optional and handle failure (e.g. JsonConfig returns false and error_code).
// - Lifecycle: direct constructors call PLH_PANIC (abort) if the FileLock module
//   was not initialized via LifecycleManager. This is intentional programmer-error
//   handling. try_lock returns nullopt instead so it remains non-fatal.
// - Lock acquisition failure (timeout, already held, etc.) is never an exception;
//   it is reported via valid() false or try_lock returning nullopt.
// -----------------------------------------------------------------------------

// Public Methods
FileLock::FileLock(const std::filesystem::path &path, ResourceType type, LockMode mode) noexcept
    : pImpl(new (std::nothrow) FileLockImpl)
{
    if (!lifecycle_initialized())
    {
        PLH_PANIC("FATAL: FileLock created before its module was initialized via LifecycleManager. "
                  "Aborting.");
    }
    if (pImpl != nullptr)
    {
        pImpl->path = path;
        open_and_lock(pImpl.get(), type, mode, std::nullopt);
    }
}

FileLock::FileLock(const std::filesystem::path &path, ResourceType type,
                   std::chrono::milliseconds timeout) noexcept
    : pImpl(new (std::nothrow) FileLockImpl)
{
    if (!lifecycle_initialized())
    {
        PLH_PANIC("FATAL: FileLock created before its module was initialized via LifecycleManager. "
                  "Aborting.");
    }
    if (pImpl != nullptr)
    {
        pImpl->path = path;
        open_and_lock(pImpl.get(), type, LockMode::Blocking, timeout);
    }
}

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

// Private constructor for factory
FileLock::FileLock() noexcept : pImpl(nullptr) {}

std::optional<FileLock> FileLock::try_lock(const std::filesystem::path &path, ResourceType type,
                                           LockMode mode) noexcept
{
    if (!lifecycle_initialized())
    {
        // Cannot PLH_PANIC here, as this function is designed to be non-fatal.
        // Returning nullopt is the correct failure mode.
        return std::nullopt;
    }

    FileLock lock; // Use private constructor
    lock.pImpl.reset(new (std::nothrow) FileLockImpl);
    if (lock.pImpl == nullptr)
    {
        return std::nullopt; // Allocation failure; report as lock unavailable
    }
    lock.pImpl->path = path;

    open_and_lock(lock.pImpl.get(), type, mode, std::nullopt);

    if (lock.valid())
    {
        return {std::move(lock)};
    }
    return std::nullopt;
}

std::optional<FileLock> FileLock::try_lock(const std::filesystem::path &path, ResourceType type,
                                           std::chrono::milliseconds timeout) noexcept
{
    if (!lifecycle_initialized())
    {
        return std::nullopt;
    }

    FileLock lock; // Use private constructor
    lock.pImpl.reset(new (std::nothrow) FileLockImpl);
    if (lock.pImpl == nullptr)
    {
        return std::nullopt; // Allocation failure; report as lock unavailable
    }
    lock.pImpl->path = path;

    open_and_lock(lock.pImpl.get(), type, LockMode::Blocking, timeout);

    if (lock.valid())
    {
        return {std::move(lock)};
    }
    return std::nullopt;
}

// Private Helpers
static void open_and_lock(FileLockImpl *pImpl, ResourceType type, LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout)
{
    if (pImpl == nullptr)
    {
        return;
    }
    pImpl->valid = false;
    pImpl->ec.clear();

    if (!prepare_lock_path(pImpl, type))
    {
        return;
    }
    if (!acquire_process_local_lock(pImpl, mode, timeout))
    {
        return;
    }

    if (auto err_code = ensure_lock_directory(pImpl); err_code)
    {
        pImpl->ec = err_code;
        rollback_process_local_lock(pImpl);
        return;
    }
    // fmt::print(stderr, "DEBUG: Acquiring OS-level lock for '{}'\n",
    // pImpl->canonical_lock_file_path.string());
    if (!run_os_lock_loop(pImpl, mode, timeout))
    {
        rollback_process_local_lock(pImpl);
        return;
    }
    pImpl->valid = true;
}

static bool prepare_lock_path(FileLockImpl *pImpl, ResourceType type)
{
    auto lockpath = FileLock::get_expected_lock_fullname_for(pImpl->path, type);
    if (lockpath.empty())
    {
        pImpl->ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    pImpl->canonical_lock_file_path = lockpath;
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- blocking vs non-blocking and waiter logic
static bool acquire_process_local_lock(FileLockImpl *pImpl, LockMode mode,
                                       std::optional<std::chrono::milliseconds> timeout)
{
    try
    {
        pImpl->lock_key = make_lock_key(pImpl->canonical_lock_file_path);
    }
    catch (...)
    {
        pImpl->ec = std::make_error_code(std::errc::io_error);
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
            pImpl->proc_state.reset();
            return false;
        }
    }
    else
    {
        if (state_ref->owners > 0)
        {
            struct ScopedWaiter
            {
                std::shared_ptr<ProcLockState> s;
                explicit ScopedWaiter(std::shared_ptr<ProcLockState> state) : s(std::move(state))
                {
                    if (s != nullptr)
                    {
                        s->waiters++;
                    }
                }
                ~ScopedWaiter()
                {
                    if (s != nullptr)
                    {
                        s->waiters--;
                    }
                }
            } waiter_guard(state_ref);

            bool acquired = false;
            try
            {
                if (timeout)
                {
                    acquired = state_ref->cv.wait_for(regl, *timeout,
                                                      [&] { return state_ref->owners == 0; });
                }
                else
                {
                    state_ref->cv.wait(regl, [&] { return state_ref->owners == 0; });
                    acquired = true;
                }
            }
            catch (...)
            {
                acquired = false;
            }

            if (!acquired)
            {
                pImpl->ec = std::make_error_code(std::errc::timed_out);
                pImpl->proc_state.reset();
                return false;
            }
        }
    }
    state_ref->owners++;
    return true;
}

static void rollback_process_local_lock(FileLockImpl *pImpl)
{
    std::lock_guard<std::mutex> lock_guard(g_proc_registry_mtx);
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
                return create_ec;
            }
        }
        return {};
    }
    catch (...)
    {
        return std::make_error_code(std::errc::io_error);
    }
}

static bool run_os_lock_loop(FileLockImpl *pImpl, LockMode mode,
                             std::optional<std::chrono::milliseconds> timeout)
{
    const auto &lockpath = pImpl->canonical_lock_file_path;

#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows implementation
    std::chrono::steady_clock::time_point start_time;
    if (timeout)
    {
        start_time = std::chrono::steady_clock::now();
    }
    auto os_path = canonical_lock_path_for_os(lockpath);
    std::wstring wpath;
    try
    {
        wpath = pylabhub::format_tools::win32_to_long_path(os_path);
        if (wpath.empty())
        {
            return false;
        }
    }
    catch (...)
    {
        pImpl->ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    // IMPORTANT: ALWAYS MAKE SURE THE SHARE FLAGS INCLUDE FILE_SHARE_DELETE !
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        pImpl->ec = std::error_code(GetLastError(), std::system_category());
        return false;
    }

    auto guard = pylabhub::basics::make_scope_guard(
        [&]()
        {
            if (!pImpl->valid)
                CloseHandle(h);
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
            pImpl->handle = reinterpret_cast<void *>(h);
            pImpl->valid = true;
            return true;
        }

        DWORD err = GetLastError();
        pImpl->ec = std::error_code(static_cast<int>(err), std::system_category());

        if (mode == LockMode::NonBlocking || err != ERROR_LOCK_VIOLATION)
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
    // POSIX implementation (flock-based advisory locks)
    auto os_path = canonical_lock_path_for_os(lockpath);

    // Open flags - create and read/write. Use O_CLOEXEC when available.
    int open_flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    // Do not follow symlinks for the lock file itself if supported.
    open_flags |= O_NOFOLLOW;
#endif

    int lock_fd = ::open(os_path.c_str(), open_flags, kLockFileMode);
    if (lock_fd == -1)
    {
        // If O_NOFOLLOW caused ELOOP and we want to permit creation via symlink fallback,
        // we could retry without O_NOFOLLOW. For now, treat ELOOP as an error (safer).
        pImpl->ec = std::error_code(errno, std::generic_category());
        return false;
    }
#ifndef O_CLOEXEC
    // set FD_CLOEXEC manually if O_CLOEXEC not available
    int flags = fcntl(lock_fd, F_GETFD);
    if (flags != -1)
    {
        fcntl(lock_fd, F_SETFD, flags | FD_CLOEXEC);
    }
#endif

    auto guard = pylabhub::basics::make_scope_guard(
        [&]()
        {
            if (!pImpl->valid && lock_fd != -1)
            {
                ::close(lock_fd);
            }
        });

    // If purely blocking with no timeout, just perform flock blocking call directly
    if (mode == LockMode::Blocking && !timeout)
    {
        PLH_DEBUG("PID {} - Attempting blocking flock(fd={}, LOCK_EX) for {}", getpid(), lock_fd,
                  os_path.string());
        if (flock(lock_fd, LOCK_EX) == 0)
        {
            PLH_DEBUG("PID {} - Successfully acquired blocking flock(fd={}) for {}", getpid(),
                      lock_fd, os_path.string());
            pImpl->fd = lock_fd;
            pImpl->valid = true;
            return true;
        }
        int err = errno;
        PLH_DEBUG("PID {} - Blocking flock(fd={}) failed for {}. Error: {}", getpid(), lock_fd,
                  os_path.string(), std::strerror(err));
        pImpl->ec = std::error_code(err, std::generic_category());
        return false;
    }

    // Timed or Non-blocking acquisition: try flock with LOCK_NB and poll until timeout if
    // necessary.
    std::chrono::steady_clock::time_point start_time;
    if (timeout)
    {
        start_time = std::chrono::steady_clock::now();
    }

    int flock_op = LOCK_EX | LOCK_NB;

    while (true)
    {
        PLH_DEBUG("PID {} - Attempting non-blocking flock(fd={}, LOCK_EX | LOCK_NB) for {}",
                  getpid(), lock_fd, os_path.string());
        if (flock(lock_fd, flock_op) == 0)
        {
            PLH_DEBUG("PID {} - Successfully acquired non-blocking flock(fd={}) for {}", getpid(),
                      lock_fd, os_path.string());
            pImpl->fd = lock_fd;
            pImpl->valid = true;
            return true;
        }

        int err = errno;
        PLH_DEBUG("PID {} - Non-blocking flock(fd={}) failed for {}. Error: {}", getpid(), lock_fd,
                  os_path.string(), std::strerror(err));
        // Busy/resource unavailable errors: EWOULDBLOCK or EAGAIN indicate lock is held by someone
        // else.
        if (err != EWOULDBLOCK && err != EAGAIN)
        {
            pImpl->ec = std::error_code(err, std::generic_category());
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

// Lifecycle Integration
bool FileLock::lifecycle_initialized() noexcept
{
    return g_filelock_initialized.load(std::memory_order_acquire);
}

namespace
{
void do_filelock_startup(const char *arg)
{
    (void)arg;
    g_filelock_initialized.store(true, std::memory_order_release);
}
void do_filelock_shutdown(const char *arg)
{
    (void)arg;
    g_filelock_initialized.store(false, std::memory_order_release);
}
} // namespace

ModuleDef FileLock::GetLifecycleModule()
{
    ModuleDef module("pylabhub::utils::FileLock");
    module.set_startup(&do_filelock_startup);
    module.set_shutdown(&do_filelock_shutdown, kFileLockShutdownTimeoutMs);
    return module;
}

} // namespace pylabhub::utils
