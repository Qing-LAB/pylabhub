#pragma once
/**
 * @file shared_memory_spinlock.hpp
 * @brief Spinlock abstraction over a region of shared memory (cross-process).
 *
 * The lock state (SharedSpinLockState) lives in a shared memory segment; this
 * module does not allocate that memory. Callers place the state in any
 * shared-memory layout (e.g. DataBlock header, or a standalone shm segment).
 *
 * Layout: Same 32-byte state is used by in-process token mode (see
 * utils/in_process_spin_state.hpp). Spin loops use utils::ExponentialBackoff.
 * For organization and fundamental facilities, see docs/SPINLOCK_HEADER_AND_SOURCE_LAYOUT.md
 * and docs/UTILS_FUNDAMENTAL_FACILITIES.md.
 */
#include "pylabhub_utils_export.h"
#include "plh_platform.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace pylabhub::hub
{

/**
 * @struct SharedSpinLockState
 * @brief Represents the atomic state of a shared spin-lock residing in shared memory.
 *
 * Same 32-byte layout for in-process (token mode) and cross-process (pid/tid mode).
 * Initialize with init_spinlock_state() so both DataBlock and in-process lock use the same logic.
 */
struct SharedSpinLockState
{
    std::atomic<uint64_t> owner_pid{0};
    std::atomic<uint64_t> owner_tid{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint32_t> recursion_count{0};
    uint8_t padding[4];
};

/**
 * Initialize one spinlock state to "free" (all four fields zero).
 * Use for: shared memory (DataBlock header, shm segments) and in-process (InProcessSpinState).
 * Call before first use of SharedSpinLock or InProcessSpinState on this state.
 * No-op if state is null. Thread-safe for distinct state pointers.
 */
inline void init_spinlock_state(SharedSpinLockState *state) noexcept
{
    if (!state)
        return;
    state->owner_pid.store(0, std::memory_order_release);
    state->owner_tid.store(0, std::memory_order_release);
    state->generation.store(0, std::memory_order_release);
    state->recursion_count.store(0, std::memory_order_release);
}

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
     * @param name A name for logging/error reporting (e.g. segment name + lock index).
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

  private:
    // Helper to get current PID (cross-platform)
    static uint64_t get_current_pid();

    // Helper to get current thread ID (cross-platform)
    static uint64_t get_current_thread_id();

    SharedSpinLockState *m_state;
    std::string m_name; // For logging/error reporting
};

/**
 * @class SharedSpinLockGuard
 * @brief RAII guard for SharedSpinLock.
 *
 * Automatically locks the mutex on construction and unlocks it on destruction.
 * Does not support recursive locking from a different thread than the owner.
 *
 * Exception safety: If lock() throws in the constructor, the guard is not
 * constructed and its destructor is not run, so unlock() is never called
 * without a prior successful lock. The destructor may throw only if the
 * current thread is not the owner (misuse); see docs/GUARD_RACE_AND_UB_ANALYSIS.md.
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

/**
 * @class SharedSpinLockGuardOwning
 * @brief RAII guard that owns the SharedSpinLock. Use when returning from APIs.
 * Holds lock and guard; lock is constructed first so guard can reference it.
 */
class PYLABHUB_UTILS_EXPORT SharedSpinLockGuardOwning
{
  public:
    SharedSpinLockGuardOwning(SharedSpinLockState *state, const std::string &name);
    ~SharedSpinLockGuardOwning();

    SharedSpinLockGuardOwning(const SharedSpinLockGuardOwning &) = delete;
    SharedSpinLockGuardOwning &operator=(const SharedSpinLockGuardOwning &) = delete;
    SharedSpinLockGuardOwning(SharedSpinLockGuardOwning &&) noexcept = delete;
    SharedSpinLockGuardOwning &operator=(SharedSpinLockGuardOwning &&) noexcept = delete;

  private:
    SharedSpinLock m_lock;
    SharedSpinLockGuard m_guard;
};

} // namespace pylabhub::hub
