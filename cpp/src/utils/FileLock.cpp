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
#include <cerrno>
#include <cstring>

#if defined(PLATFORM_WIN64)
#include <sstream>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/file.h> // added for flock
#endif

#include "format_tools.hpp"
#include "scope_guard.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include "platform.hpp"

using namespace pylabhub::platform;

// Module-level flag to indicate if the FileLock has been initialized.
static std::atomic<bool> g_filelock_initialized{false};

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

static std::string make_lock_key(const std::filesystem::path &lockpath)
{
    try
    {
#if defined(PLATFORM_WIN64)
        std::wstring longw = pylabhub::format_tools::win32_to_long_path(lockpath);
        for (auto &ch : longw)
            ch = towlower(ch);
        return wstring_to_utf8(longw);
#else
        // Use the lexically-normal absolute representation for a deterministic key
        try {
            auto abs = std::filesystem::absolute(lockpath).lexically_normal();
            return abs.generic_string();
        } catch(...) {
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
#if defined(PLATFORM_WIN64)
    return std::filesystem::path(pylabhub::format_tools::win32_to_long_path(lockpath));
#else
    return lockpath;
#endif
}

namespace pylabhub::utils
{

static constexpr std::chrono::milliseconds LOCK_POLLING_INTERVAL = std::chrono::milliseconds(20);

// Global Registries for Lock Management
static std::mutex g_lockfile_registry_mtx;
static std::unordered_map<std::string, std::string> g_lockfile_registry;

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

#if defined(PLATFORM_WIN64)
    void *handle = nullptr;
#else
    int fd = -1;
#endif
};

void FileLock::FileLockImplDeleter::operator()(FileLockImpl *p)
{
    if (!p) return;

    if (p->valid)
    {
        // This is noisy, so keep it at TRACE level
        // LOGGER_DEBUG("FileLock: Releasing lock for '{}'", p->path.string());
    }

    if (p->valid)
    {
#if defined(PLATFORM_WIN64)
        if (p->handle)
        {
            OVERLAPPED ov = {};
            UnlockFileEx((HANDLE)p->handle, 0, 1, 0, &ov);
            CloseHandle((HANDLE)p->handle);
            p->handle = nullptr;
        }
#else
        if (p->fd != -1)
        {
            // Release using flock (kernel advisory whole-file unlock)
            // Best-effort: ignore errors on unlock/close path.
            flock(p->fd, LOCK_UN);
            ::close(p->fd);
            p->fd = -1;
        }
#endif
    }

    if (p->proc_state)
    {
        std::lock_guard<std::mutex> lg(g_proc_registry_mtx);
        if (--p->proc_state->owners == 0)
        {
            p->proc_state->cv.notify_all();
            if (p->proc_state->waiters == 0)
            {
                g_proc_locks.erase(p->lock_key);
            }
        }
    }

    delete p;
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

std::filesystem::path FileLock::get_expected_lock_fullname_for(const std::filesystem::path &target,
                                                               ResourceType type) noexcept
{
    try
    {
        std::filesystem::path canonical_target;
        std::error_code ec;
        canonical_target = std::filesystem::canonical(target, ec);

        if (ec)
        {
            canonical_target = std::filesystem::absolute(target).lexically_normal();
        }

        if (type == ResourceType::Directory)
        {
            auto fname = canonical_target.filename();
            auto parent = canonical_target.parent_path();
            if (fname.empty() || fname == "." || fname == "..")
            {
                fname = "pylabhub_root";
            }
            fname += ".dir.lock";
            return parent / fname;
        }
        else
        {
            auto p = canonical_target;
            p += ".lock";
            return p;
        }
    }
    catch (...)
    {
        // This function must not throw.
        return {};
    }
}

// Public Methods
FileLock::FileLock(const std::filesystem::path &path, ResourceType type, LockMode mode) noexcept
    : pImpl(new FileLockImpl)
{
    if (!lifecycle_initialized()) {
        fmt::print(stderr, "FATAL: FileLock created before its module was initialized via LifecycleManager. Aborting.\n");
        std::abort();
    }
    pImpl->path = path;
    open_and_lock(pImpl.get(), type, mode, std::nullopt);
}

FileLock::FileLock(const std::filesystem::path &path, ResourceType type,
                   std::chrono::milliseconds timeout) noexcept
    : pImpl(new FileLockImpl)
{
    if (!lifecycle_initialized()) {
        fmt::print(stderr, "FATAL: FileLock created before its module was initialized via LifecycleManager. Aborting.\n");
        std::abort();
    }
    pImpl->path = path;
    open_and_lock(pImpl.get(), type, LockMode::Blocking, timeout);
}

FileLock::~FileLock() = default;
FileLock::FileLock(FileLock &&) noexcept = default;
FileLock &FileLock::operator=(FileLock &&) noexcept = default;

bool FileLock::valid() const noexcept { return pImpl && pImpl->valid; }
std::error_code FileLock::error_code() const noexcept { return pImpl ? pImpl->ec : std::error_code(); }

std::optional<std::filesystem::path> FileLock::get_locked_resource_path() const noexcept
{
    if (pImpl && pImpl->valid)
    {
        try {
            return std::filesystem::absolute(pImpl->path).lexically_normal();
        }
        catch (...) {
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

// Private Helpers
static void open_and_lock(FileLockImpl *pImpl, ResourceType type, LockMode mode,
                          std::optional<std::chrono::milliseconds> timeout)
{
    if (!pImpl) return;
    pImpl->valid = false;
    pImpl->ec.clear();
    
    if (!prepare_lock_path(pImpl, type)) return;
    if (!acquire_process_local_lock(pImpl, mode, timeout)) return;

    if (auto ec = ensure_lock_directory(pImpl); ec)
    {
        pImpl->ec = ec;
        rollback_process_local_lock(pImpl);
        return;
    }
    // fmt::print(stderr, "DEBUG: Acquiring OS-level lock for '{}'\n", pImpl->canonical_lock_file_path.string());
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

static bool acquire_process_local_lock(FileLockImpl *pImpl, LockMode mode,
                                       std::optional<std::chrono::milliseconds> timeout)
{
    try { 
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

static std::error_code ensure_lock_directory(FileLockImpl *pImpl)
{
    try {
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
    
#if defined(PLATFORM_WIN64)
    // Windows implementation
    std::chrono::steady_clock::time_point start_time;
    if (timeout)
    {
        start_time = std::chrono::steady_clock::now();
    }
    auto os_path = canonical_lock_path_for_os(lockpath);
    std::wstring wpath;
    try {
        wpath = pylabhub::format_tools::win32_to_long_path(os_path);
    } catch(...) {
        pImpl->ec = std::make_error_code(std::errc::io_error);
        return false;
    }

    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        pImpl->ec = std::error_code(GetLastError(), std::system_category());
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
            g_lockfile_registry.emplace(reg_key, wstring_to_utf8(wpath));

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

    int fd = ::open(os_path.c_str(), open_flags, S_IRUSR | S_IWUSR);
    if (fd == -1)
    {
        // If O_NOFOLLOW caused ELOOP and we want to permit creation via symlink fallback,
        // we could retry without O_NOFOLLOW. For now, treat ELOOP as an error (safer).
        pImpl->ec = std::error_code(errno, std::generic_category());
        return false;
    }
    #ifndef O_CLOEXEC
    // set FD_CLOEXEC manually if O_CLOEXEC not available
    int flags = fcntl(fd, F_GETFD);
    if (flags != -1)
    {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    #endif

    auto guard = pylabhub::basics::make_scope_guard([&]() {
        if (!pImpl->valid && fd != -1) ::close(fd);
    });

    // If purely blocking with no timeout, just perform flock blocking call directly
    if (mode == LockMode::Blocking && !timeout)
    {
        LOGGER_INFO("PID {} - Attempting blocking flock(fd={}, LOCK_EX) for {}", getpid(), fd, os_path.string());
        if (flock(fd, LOCK_EX) == 0)
        {
            LOGGER_INFO("PID {} - Successfully acquired blocking flock(fd={}) for {}", getpid(), fd, os_path.string());
            auto reg_key = make_lock_key(lockpath);
            std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
            g_lockfile_registry.emplace(reg_key, os_path.string());

            pImpl->fd = fd;
            pImpl->valid = true;
            return true;
        }
        else
        {
            int err = errno;
            LOGGER_ERROR("PID {} - Blocking flock(fd={}) failed for {}. Error: {}", getpid(), fd, os_path.string(), std::strerror(err));
            pImpl->ec = std::error_code(err, std::generic_category());
            return false;
        }
    }

    // Timed or Non-blocking acquisition: try flock with LOCK_NB and poll until timeout if necessary.
    std::chrono::steady_clock::time_point start_time;
    if (timeout) start_time = std::chrono::steady_clock::now();

    int flock_op = LOCK_EX | LOCK_NB;

    while (true)
    {
        LOGGER_INFO("PID {} - Attempting non-blocking flock(fd={}, LOCK_EX | LOCK_NB) for {}", getpid(), fd, os_path.string());
        if (flock(fd, flock_op) == 0)
        {
            LOGGER_INFO("PID {} - Successfully acquired non-blocking flock(fd={}) for {}", getpid(), fd, os_path.string());
            auto reg_key = make_lock_key(lockpath);
            std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
            g_lockfile_registry.emplace(reg_key, os_path.string());
            
            pImpl->fd = fd;
            pImpl->valid = true;
            return true;
        }

        int err = errno;
        LOGGER_INFO("PID {} - Non-blocking flock(fd={}) failed for {}. Error: {}", getpid(), fd, os_path.string(), std::strerror(err));
        // Busy/resource unavailable errors: EWOULDBLOCK or EAGAIN indicate lock is held by someone else.
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
        continue;
    }

#endif
}

void FileLock::cleanup()
{
    /* Cleanup stale lock files */
    std::vector<std::pair<std::string, std::string>> candidates;
    {
        std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
        candidates.reserve(g_lockfile_registry.size());
        for (auto &kv : g_lockfile_registry)
            candidates.emplace_back(kv.first, kv.second);
    }

    std::vector<std::string> removed_keys;
    for (const auto &kv : candidates)
    {
        const std::string &reg_key = kv.first;
        const std::string &path_str = kv.second;
        std::filesystem::path p = canonical_lock_path_for_os(path_str);
        
        FileLock maybe_stale_lock(p, ResourceType::File, LockMode::NonBlocking);
        if(maybe_stale_lock.valid())
        {
             std::error_code ec;
             std::filesystem::remove(p, ec);
             if (!ec) {
                 removed_keys.push_back(reg_key);
             }
        }
    }

    if (!removed_keys.empty())
    {
        std::lock_guard<std::mutex> lg(g_lockfile_registry_mtx);
        for (const auto &k : removed_keys) g_lockfile_registry.erase(k);
    }
}

// Lifecycle Integration
bool FileLock::lifecycle_initialized() noexcept {
    return g_filelock_initialized.load(std::memory_order_acquire);
}

namespace
{
void do_filelock_startup() {
    g_filelock_initialized.store(true, std::memory_order_release);
}
void do_filelock_cleanup() {
    if (FileLock::lifecycle_initialized()) {
        FileLock::cleanup();
    }
    g_filelock_initialized.store(false, std::memory_order_release);
}
}

ModuleDef FileLock::GetLifecycleModule()
{
    ModuleDef module("pylabhub::utils::FileLockCleanup");
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_filelock_startup);
    module.set_shutdown(&do_filelock_cleanup, 2000 /*ms timeout*/);
    return module;
}

} // namespace pylabhub::utils
