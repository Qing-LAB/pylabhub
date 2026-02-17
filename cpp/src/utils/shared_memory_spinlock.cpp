#include "utils/shared_memory_spinlock.hpp"
#include "plh_service.hpp" // For platform, logger, backoff_strategy

namespace pylabhub::hub
{

// ============================================================================
// SharedSpinLock Implementation
// ============================================================================

namespace
{
constexpr uint64_t kNsPerMs = 1'000'000;
}

SharedSpinLock::SharedSpinLock(SharedSpinLockState *state, std::string name)
    : m_state(state), m_name(std::move(name))
{
    if (m_state == nullptr)
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

bool SharedSpinLock::try_lock_for(int timeout_ms)
{
    uint64_t my_pid = get_current_pid();
    uint64_t my_tid = get_current_thread_id();

    // Check if already owned (recursive case)
    if (m_state->owner_pid.load(std::memory_order_relaxed) == my_pid &&
        m_state->owner_tid.load(std::memory_order_relaxed) == my_tid)
    { // Use relaxed as per spec
        m_state->recursion_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    uint64_t start_ns = pylabhub::platform::monotonic_time_ns();
    pylabhub::utils::ExponentialBackoff backoff_strategy;
    int iteration = 0;

    // CAS loop with timeout (use platform monotonic time for consistency with data_block)
    uint64_t expected_pid = 0;
    while (!m_state->owner_pid.compare_exchange_weak(
        expected_pid, my_pid, std::memory_order_acquire, std::memory_order_relaxed))
    {

        // Check if lock holder is dead
        if (expected_pid != 0 && !pylabhub::platform::is_process_alive(expected_pid))
        {
            // Use CAS (not store) to reclaim zombie lock so only one process wins when
            // multiple processes detect the same dead holder simultaneously.
            uint64_t zombie_pid = expected_pid;
            if (m_state->owner_pid.compare_exchange_strong(
                    zombie_pid, my_pid, std::memory_order_acquire, std::memory_order_relaxed))
            {
                LOGGER_WARN("SharedSpinLock '{}': Reclaimed zombie lock from dead PID {}.",
                            m_name, expected_pid);
                m_state->owner_tid.store(my_tid, std::memory_order_release);
                m_state->recursion_count.store(1, std::memory_order_relaxed);
                m_state->generation.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            // Lost the reclaim race — another process grabbed it first; fall through to spin/timeout.
        }

        // Timeout check
        if (timeout_ms > 0)
        {
            uint64_t elapsed_ms = pylabhub::platform::elapsed_time_ns(start_ns) / kNsPerMs;
            if (elapsed_ms >= static_cast<uint64_t>(timeout_ms))
            {
                return false; // Timeout
            }
        }

        expected_pid = 0;              // Reset for next CAS attempt
        backoff_strategy(iteration++); // Exponential backoff
    }

    m_state->owner_tid.store(my_tid, std::memory_order_relaxed);
    m_state->recursion_count.store(1, std::memory_order_relaxed);
    return true;
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
    uint64_t current_tid = get_current_thread_id();

    if (m_state->owner_pid.load(std::memory_order_acquire) != current_pid ||
        m_state->owner_tid.load(std::memory_order_acquire) != current_tid)
    {
        LOGGER_ERROR(
            "SharedSpinLock '{}': Attempted to unlock by non-owner. Current owner PID:TID {}:{}, "
            "Caller PID:TID {}:{}.",
            m_name, m_state->owner_pid.load(std::memory_order_acquire),
            m_state->owner_tid.load(std::memory_order_acquire), current_pid, current_tid);
        throw std::runtime_error("Attempted to unlock by non-owner.");
    }

    if (m_state->recursion_count.load(std::memory_order_relaxed) > 1)
    {
        m_state->recursion_count.fetch_sub(1, std::memory_order_relaxed);
        return; // Still recursively locked by this process
    }

    // Release the lock.
    // Order: owner_tid BEFORE owner_pid (intentional).
    // owner_pid is the authoritative "lock free" signal used by CAS in try_lock_for().
    // Storing owner_pid last (with release fence) ensures:
    //   1. No contender CAS-succeeds while owner_tid is still being cleared.
    //   2. The intermediate state (owner_pid!=0, owner_tid==0) is harmless — the lock is
    //      still held from a CAS perspective, and zombie reclaim cannot fire because the
    //      process is alive (it is executing this very code).
    m_state->recursion_count.store(0, std::memory_order_release);
    m_state->generation.fetch_add(1, std::memory_order_relaxed);
    m_state->owner_tid.store(0, std::memory_order_relaxed);  // Cleared first; lock still "held" here
    m_state->owner_pid.store(0, std::memory_order_release);  // Authoritative "lock free" signal — last
}

bool SharedSpinLock::is_locked_by_current_process() const
{
    return m_state->owner_pid.load(std::memory_order_acquire) == get_current_pid() &&
           m_state->owner_tid.load(std::memory_order_acquire) == get_current_thread_id();
}

// ============================================================================
// SharedSpinLockGuard Implementation
// ============================================================================

SharedSpinLockGuard::SharedSpinLockGuard(SharedSpinLock &lock) : m_lock(lock)
{
    m_lock.lock();
}

// NOLINTNEXTLINE(bugprone-exception-escape) -- unlock() may throw; required by RAII contract
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
