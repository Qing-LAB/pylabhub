#include "utils/data_header_sync_primitives.hpp"
#include "plh_platform.hpp" // For pylabhub::platform::get_pid() and get_native_thread_id()
#include "utils/logger.hpp" // For logging macros

// For process checking (needed for is_process_alive)
#if defined(PYLABHUB_IS_WINDOWS)
// <windows.h> is included by plh_platform.hpp
#else
#include <errno.h>  // For ESRCH
#include <signal.h> // For kill
#endif

namespace pylabhub::hub
{

// ============================================================================
// SharedSpinLock Implementation
// ============================================================================

SharedSpinLock::SharedSpinLock(SharedSpinLockState *state, const std::string &name)
    : m_state(state), m_name(name)
{
    if (!m_state)
    {
        LOGGER_ERROR("SharedSpinLock '{}': Initialized with a null SharedSpinLockState.", m_name);
        throw std::invalid_argument("SharedSpinLockState cannot be null.");
    }
}

uint64_t SharedSpinLock::get_current_pid()
{
    return pylabhub::platform::get_pid();
}

uint64_t SharedSpinLock::get_current_thread_id()
{
    return pylabhub::platform::get_native_thread_id();
}

bool SharedSpinLock::is_process_alive(uint64_t pid) const
{
    if (pid == 0)
    {
        return false; // PID 0 is typically invalid or refers to the system
    }

#if defined(PYLABHUB_IS_WINDOWS)
    // On Windows, checking if a process is alive is more complex.
    // One approach is to try to open the process.
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == NULL)
    {
        // GetLastError() could be ERROR_INVALID_PARAMETER if PID doesn't exist
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD exitCode;
    BOOL result = GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
    if (!result)
    {
        return false; // Could not get exit code, assume dead or inaccessible
    }
    return exitCode == STILL_ACTIVE;
#else
    // On POSIX systems, `kill(pid, 0)` checks for existence without sending a signal.
    // Returns 0 on success (process exists), -1 on failure.
    // If errno is ESRCH, the process does not exist.
    // If errno is EPERM, the process exists but we don't have permission.
    if (kill(static_cast<pid_t>(pid), 0) == 0)
    {
        return true; // Process exists
    }
    return errno != ESRCH; // Process exists if error is not "No such process"
#endif
}

bool SharedSpinLock::try_lock_for(int timeout_ms)
{
    uint64_t current_pid = get_current_pid();
    uint64_t current_thread_id = get_current_thread_id();

    // Recursive lock check by the same thread
    if (m_state->owner_pid.load(std::memory_order_acquire) == current_pid &&
        m_state->owner_thread_id.load(std::memory_order_acquire) == current_thread_id)
    {
        m_state->recursion_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // Acquire loop
    while (true)
    {
        uint64_t expected_pid = 0;
        uint64_t current_owner_pid = m_state->owner_pid.load(std::memory_order_acquire);

        // Case 1: Lock is currently free
        if (current_owner_pid == 0)
        {
            if (m_state->owner_pid.compare_exchange_strong(expected_pid, current_pid,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire))
            {
                m_state->owner_thread_id.store(current_thread_id, std::memory_order_release);
                m_state->recursion_count.store(1, std::memory_order_release);
                return true; // Acquired
            }
            // Another thread/process just acquired the lock, retry
        }
        // Case 2: Lock is held by another process or a crashed process
        else
        {
            // Check if the owning process is still alive.
            // If not, we can try to "steal" the lock.
            if (!is_process_alive(current_owner_pid))
            {
                LOGGER_WARN(
                    "SharedSpinLock '{}': Detected dead owner PID {}. Attempting to re-acquire.",
                    m_name, current_owner_pid);
                // Attempt to "steal" the lock. We expect the owner_pid to be current_owner_pid.
                // If it's still current_owner_pid, it means no one else stole it yet.
                // If the CAS fails, another process successfully stole it, and we just retry.
                if (m_state->owner_pid.compare_exchange_strong(current_owner_pid, current_pid,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
                {
                    // Increment generation to make sure other processes waiting for old owner
                    // recognize the change.
                    m_state->generation.fetch_add(1, std::memory_order_release);
                    m_state->owner_thread_id.store(current_thread_id, std::memory_order_release);
                    m_state->recursion_count.store(1, std::memory_order_release);
                    return true; // Acquired
                }
            }
        }

        // Spin-wait for a short period before retrying
        std::this_thread::yield(); // Hint to scheduler that this thread can be paused

        if (timeout_ms > 0)
        {
            auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start_time);
            if (elapsed_time.count() >= timeout_ms)
            {
                return false; // Timeout
            }
        }
    }
}

void SharedSpinLock::lock()
{
    if (!try_lock_for(0)) // 0 means indefinite wait
    {
        // This should theoretically not happen for indefinite wait,
        // but included for robustness.
        LOGGER_ERROR("SharedSpinLock '{}': Indefinite lock failed unexpectedly.", m_name);
        throw std::runtime_error("Indefinite lock failed.");
    }
}

void SharedSpinLock::unlock()
{
    uint64_t current_pid = get_current_pid();
    uint64_t current_thread_id = get_current_thread_id();

    if (m_state->owner_pid.load(std::memory_order_acquire) != current_pid ||
        m_state->owner_thread_id.load(std::memory_order_acquire) != current_thread_id)
    {
        LOGGER_ERROR("SharedSpinLock '{}': Attempted to unlock by non-owner. Current owner PID {}, "
                     "Thread ID {}. Caller PID {}, Thread ID {}.",
                     m_name, m_state->owner_pid.load(std::memory_order_acquire),
                     m_state->owner_thread_id.load(std::memory_order_acquire), current_pid,
                     current_thread_id);
        throw std::runtime_error("Attempted to unlock by non-owner.");
    }

    if (m_state->recursion_count.load(std::memory_order_relaxed) > 1)
    {
        m_state->recursion_count.fetch_sub(1, std::memory_order_relaxed);
        return; // Still recursively locked by this thread
    }

    // Release the lock
    m_state->owner_thread_id.store(0, std::memory_order_release);
    m_state->recursion_count.store(0, std::memory_order_release);
    m_state->generation.fetch_add(1, std::memory_order_release); // Increment generation
    m_state->owner_pid.store(0, std::memory_order_release);      // Finally release ownership
}

bool SharedSpinLock::is_locked_by_current_process() const
{
    return m_state->owner_pid.load(std::memory_order_acquire) == get_current_pid();
}

bool SharedSpinLock::is_locked_by_current_thread() const
{
    return m_state->owner_pid.load(std::memory_order_acquire) == get_current_pid() &&
           m_state->owner_thread_id.load(std::memory_order_acquire) == get_current_thread_id();
}

// ============================================================================
// SharedSpinLockGuard Implementation
// ============================================================================

SharedSpinLockGuard::SharedSpinLockGuard(SharedSpinLock &lock) : m_lock(lock)
{
    m_lock.lock();
}

SharedSpinLockGuard::~SharedSpinLockGuard()
{
    m_lock.unlock();
}

// ============================================================================
// SharedSpinLockGuardOwning Implementation
// ============================================================================

SharedSpinLockGuardOwning::SharedSpinLockGuardOwning(SharedSpinLockState *state,
                                                     const std::string &name)
    : m_lock(state, name), m_guard(m_lock)
{
}

SharedSpinLockGuardOwning::~SharedSpinLockGuardOwning() = default;

} // namespace pylabhub::hub
