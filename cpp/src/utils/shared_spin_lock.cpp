#include "utils/shared_spin_lock.hpp"
#include "plh_service.hpp" // For LOGGER_ERROR, LOGGER_INFO
#include "plh_base.hpp"    // For pylabhub::platform::get_pid(), get_native_thread_id()

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <windows.h>
#else
#include <signal.h>      // For kill()
#include <cerrno>        // For errno
#endif

namespace pylabhub::hub
{

// ============================================================================
// SharedSpinLock Implementation
// ============================================================================

// Helper to get current PID (cross-platform)
// Delegate to the centralized platform utility
uint64_t SharedSpinLock::get_current_pid()
{
    return pylabhub::platform::get_pid();
}

// Helper to get current thread ID (cross-platform)
// Delegate to the centralized platform utility
uint64_t SharedSpinLock::get_current_thread_id()
{
    return pylabhub::platform::get_native_thread_id();
}

// Helper to check if a process is alive (cross-platform)
bool SharedSpinLock::is_process_alive(uint64_t pid) const
{
    if (pid == 0)
        return false; // PID 0 is not a valid running process

#if defined(PYLABHUB_PLATFORM_WIN64)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == NULL)
    {
        // GetLastError() could be ERROR_INVALID_PARAMETER if the process is already dead.
        return false;
    }
    DWORD exit_code;
    if (GetExitCodeProcess(process, &exit_code) == 0)
    {
        CloseHandle(process);
        return false;
    }
    CloseHandle(process);
    // If exit_code is STILL_ACTIVE, the process is alive. Otherwise, it's dead.
    return (exit_code == STILL_ACTIVE);
#else
    // On POSIX, kill(pid, 0) checks if the process exists and you have permission to send it a
    // signal. It returns 0 on success (process exists), or -1 on error. errno == ESRCH means no
    // such process.
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

SharedSpinLock::SharedSpinLock(SharedMemoryHeader::SharedSpinLockState *state,
                               const std::string &name)
    : m_state(state), m_name(name)
{
    if (!m_state)
    {
        throw std::runtime_error("SharedSpinLock: SharedSpinLockState pointer cannot be null.");
    }
}

void SharedSpinLock::lock()
{
    const uint64_t current_pid = get_current_pid();
    const uint64_t current_tid = get_current_thread_id();

    // Loop until lock is acquired
    while (true)
    {
        uint64_t owner_pid = m_state->owner_pid.load(std::memory_order_acquire);
        uint64_t current_generation = m_state->generation.load(std::memory_order_acquire);

        if (owner_pid == 0) // Lock is free
        {
            uint64_t expected_pid = 0;
            // Attempt to acquire. Increment generation on acquire for robustness.
            if (m_state->owner_pid.compare_exchange_strong(expected_pid, current_pid,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire))
            {
                m_state->owner_thread_id = current_tid; // Only current process writes this
                m_state->recursion_count.store(1, std::memory_order_release);
                LOGGER_INFO("SharedSpinLock '{}': Acquired by PID {} (first lock).", m_name,
                            current_pid);
                return;
            }
        }
        else if (owner_pid == current_pid) // Locked by current process
        {
            if (m_state->owner_thread_id == current_tid) // Locked by current thread
            {
                m_state->recursion_count.fetch_add(1, std::memory_order_acq_rel);
                LOGGER_INFO("SharedSpinLock '{}': Acquired recursively by PID {} TID {}.", m_name,
                            current_pid, current_tid);
                return;
            }
            // Locked by current process, but different thread. Spin.
        }
        else // Locked by another process
        {
            // Check if the owning process is still alive. This is the "robust" part.
            if (!is_process_alive(owner_pid))
            {
                LOGGER_WARN(
                    "SharedSpinLock '{}': Detected dead owner PID {}. Attempting to reclaim lock.",
                    m_name, owner_pid);

                uint64_t expected_pid = owner_pid;
                // Attempt to take ownership. Increment generation to invalidate old owner's
                // context. It's important to read generation again here, as it might have changed.
                uint64_t expected_generation = m_state->generation.load(std::memory_order_acquire);
                uint64_t new_generation = expected_generation + 1;

                if (m_state->owner_pid.compare_exchange_strong(expected_pid, current_pid,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
                {
                    m_state->generation.store(new_generation,
                                              std::memory_order_release); // Update generation
                    m_state->owner_thread_id = current_tid;
                    m_state->recursion_count.store(1, std::memory_order_release);
                    LOGGER_INFO("SharedSpinLock '{}': Reclaimed by PID {} (dead owner {}). New "
                                "generation {}.",
                                m_name, current_pid, owner_pid, new_generation);
                    // Critical: The state of the shared memory protected by this mutex is now
                    // potentially inconsistent due to the previous owner dying. A higher-level
                    // mechanism should handle this.
                    return;
                }
            }
        }

        // Spin wait: yield CPU or sleep briefly
        std::this_thread::yield();
    }
}

void SharedSpinLock::unlock()
{
    const uint64_t current_pid = get_current_pid();
    const uint64_t current_tid = get_current_thread_id();

    uint64_t owner_pid = m_state->owner_pid.load(std::memory_order_acquire);
    uint32_t recursion_count = m_state->recursion_count.load(std::memory_order_acquire);

    if (owner_pid != current_pid || m_state->owner_thread_id != current_tid)
    {
        throw std::runtime_error(
            "SharedSpinLock: Attempt to unlock by non-owner or wrong thread for '" + m_name +
            "'. Current Owner PID: " + std::to_string(owner_pid) +
            " My PID: " + std::to_string(current_pid) + " Current Owner TID: " +
            std::to_string(m_state->owner_thread_id) + " My TID: " + std::to_string(current_tid));
    }

    if (recursion_count > 1)
    {
        m_state->recursion_count.fetch_sub(1, std::memory_order_acq_rel);
        LOGGER_INFO("SharedSpinLock '{}': Released recursively by PID {} TID {}. Remaining "
                    "recursion count {}.",
                    m_name, current_pid, current_tid, recursion_count - 1);
    }
    else
    {
        // Last unlock: release the lock
        uint64_t expected_pid = current_pid;
        uint64_t new_generation = m_state->generation.load(std::memory_order_acquire) + 1;
        if (!m_state->owner_pid.compare_exchange_strong(expected_pid, 0, std::memory_order_acq_rel,
                                                        std::memory_order_acquire))
        {
            // This should ideally not happen if checks above are correct, but handle defensively
            throw std::runtime_error(
                "SharedSpinLock: Failed to release lock (compare_exchange_strong failed) for '" +
                m_name + "'.");
        }
        m_state->generation.store(new_generation, std::memory_order_release);
        m_state->recursion_count.store(0, std::memory_order_release); // Ensure recursion count is 0
        m_state->owner_thread_id = 0;                                 // Clear owner TID
        LOGGER_INFO("SharedSpinLock '{}': Released by PID {}. New generation {}.", m_name,
                    current_pid, new_generation);
    }
}

bool SharedSpinLock::try_lock_for(int timeout_ms)
{
    const uint64_t current_pid = get_current_pid();
    const uint64_t current_tid = get_current_thread_id();
    auto start_time = std::chrono::steady_clock::now();

    while (true)
    {
        uint64_t owner_pid = m_state->owner_pid.load(std::memory_order_acquire);

        if (owner_pid == 0) // Lock is free
        {
            uint64_t expected_pid = 0;
            if (m_state->owner_pid.compare_exchange_strong(expected_pid, current_pid,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire))
            {
                m_state->owner_thread_id = current_tid;
                m_state->recursion_count.store(1, std::memory_order_release);
                LOGGER_INFO("SharedSpinLock '{}': Acquired by PID {} (try_lock_for).", m_name,
                            current_pid);
                return true;
            }
        }
        else if (owner_pid == current_pid) // Locked by current process
        {
            if (m_state->owner_thread_id == current_tid) // Locked by current thread
            {
                m_state->recursion_count.fetch_add(1, std::memory_order_acq_rel);
                LOGGER_INFO(
                    "SharedSpinLock '{}': Acquired recursively by PID {} TID {} (try_lock_for).",
                    m_name, current_pid, current_tid);
                return true;
            }
            // Locked by current process, but different thread. Spin.
        }
        else // Locked by another process
        {
            if (!is_process_alive(owner_pid))
            {
                LOGGER_WARN("SharedSpinLock '{}': Detected dead owner PID {} (try_lock_for). "
                            "Attempting to reclaim lock.",
                            m_name, owner_pid);

                uint64_t expected_pid = owner_pid;
                uint64_t expected_generation = m_state->generation.load(std::memory_order_acquire);
                uint64_t new_generation = expected_generation + 1;

                if (m_state->owner_pid.compare_exchange_strong(expected_pid, current_pid,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
                {
                    m_state->generation.store(new_generation, std::memory_order_release);
                    m_state->owner_thread_id = current_tid;
                    m_state->recursion_count.store(1, std::memory_order_release);
                    LOGGER_INFO("SharedSpinLock '{}': Reclaimed by PID {} (dead owner {} - "
                                "try_lock_for). New generation {}.",
                                m_name, current_pid, owner_pid, new_generation);
                    return true;
                }
            }
        }

        // Check for timeout
        if (timeout_ms > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - start_time)
                                      .count() >= timeout_ms)
        {
            LOGGER_INFO("SharedSpinLock '{}': Timed out after {}ms waiting for lock.", m_name,
                        timeout_ms);
            return false;
        }

        // Spin wait: yield CPU
        std::this_thread::yield();
    }
}

bool SharedSpinLock::is_locked_by_current_process() const
{
    return m_state->owner_pid.load(std::memory_order_acquire) == get_current_pid();
}

bool SharedSpinLock::is_locked_by_current_thread() const
{
    return m_state->owner_pid.load(std::memory_order_acquire) == get_current_pid() &&
           m_state->owner_thread_id == get_current_thread_id();
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

} // namespace pylabhub::hub