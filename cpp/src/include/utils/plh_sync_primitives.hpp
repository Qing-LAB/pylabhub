#pragma once

#include "pylabhub_utils_export.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <chrono>
#include <thread>

#include "plh_platform.hpp" // For platform-specific definitions and headers

#if defined(PYLABHUB_IS_POSIX)
#include <fcntl.h>    // For shm_open, O_CREAT, O_RDWR
#include <pthread.h>  // For pthread_mutex_t (if used in future in SharedSpinLock)
#include <sys/mman.h> // For mmap, munmap
#include <sys/stat.h> // For mode constants (S_IRUSR, S_IWUSR)
#include <unistd.h>   // For ftruncate, close, getpid
#include <errno.h>    // For ESRCH
#include <signal.h>   // For kill
#endif

namespace pylabhub::hub
{

/**
 * @struct SharedSpinLockState
 * @brief Represents the atomic state of a shared spin-lock residing in shared memory.
 */
struct SharedSpinLockState
{
    std::atomic<uint64_t> owner_pid{0};       // 0 means unlocked
    std::atomic<uint64_t> generation{0};      // Incremented on release, to mitigate PID reuse
    std::atomic<uint32_t> recursion_count{0}; // For recursive locking by same thread
    uint64_t owner_thread_id{0}; // Thread ID of lock holder (only valid if owner_pid != 0)
};

/**
 * @class SharedSpinLock
 * @brief Implements a robust, cross-process spin-lock using atomic variables
 *        entirely within a shared memory segment.
 *
 * This lock uses a PID and a generation counter to handle ownership and
 * mitigate issues with process termination and PID reuse. It also supports
 * recursive locking by the same thread.
 *
 * The `SharedSpinLock` operates on a `SharedSpinLockState`
 * struct residing in shared memory.
 */
class PYLABHUB_UTILS_EXPORT SharedSpinLock
{
  public:
    /**
     * @brief Constructs a SharedSpinLock.
     * @param state A pointer to the SharedSpinLockState struct in shared memory.
     * @param name A name for logging/error reporting (typically the DataBlock name + lock index).
     */
    SharedSpinLock(SharedSpinLockState *state, const std::string &name);

    /**
     * @brief Acquires the spin-lock, blocking if necessary.
     * @param timeout_ms The maximum time to wait for the lock. 0 means no timeout (spin
     * indefinitely).
     * @return True if the lock was acquired, false if timeout occurred.
     */
    bool try_lock_for(int timeout_ms = 0);

    /**
     * @brief Acquires the spin-lock, blocking indefinitely until acquired.
     */
    void lock();

    /**
     * @brief Releases the spin-lock.
     * @throws std::runtime_error if the lock is released by a non-owner.
     */
    void unlock();

    /**
     * @brief Checks if the current process is the owner of this spin-lock.
     */
    bool is_locked_by_current_process() const;

    /**
     * @brief Checks if the current thread is the owner of this spin-lock.
     */
    bool is_locked_by_current_thread() const;

  private:
    // Helper to get current PID (cross-platform)
    static uint64_t get_current_pid();

    // Helper to get current thread ID (cross-platform)
    static uint64_t get_current_thread_id();

    // Helper to check if a process is alive (cross-platform)
    bool is_process_alive(uint64_t pid) const;

    SharedSpinLockState *m_state;
    std::string m_name; // For logging/error reporting
};

/**
 * @class SharedSpinLockGuard
 * @brief RAII guard for SharedSpinLock.
 *
 * Automatically locks the mutex on construction and unlocks it on destruction.
 * Does not support recursive locking from a different thread than the owner.
 */
class PYLABHUB_UTILS_EXPORT SharedSpinLockGuard
{
  public:
    explicit SharedSpinLockGuard(SharedSpinLock &lock);
    ~SharedSpinLockGuard();

    SharedSpinLockGuard(const SharedSpinLockGuard &) = delete;
    SharedSpinLockGuard &operator=(const SharedSpinLockGuard &) = delete;
    SharedSpinLockGuard(SharedSpinLockGuard &&) noexcept = delete;
    SharedSpinLockGuard &operator=(SharedSpinLockGuard &&) noexcept = delete;

  private:
    SharedSpinLock &m_lock;
};

} // namespace pylabhub::hub