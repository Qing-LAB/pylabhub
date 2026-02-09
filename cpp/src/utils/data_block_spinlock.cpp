#include "utils/data_block_spinlock.hpp"
#include "plh_service.hpp" // For platform, logger, backoff_strategy

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

bool SharedSpinLock::try_lock_for(int timeout_ms)
{
    uint64_t my_pid = get_current_pid();

    // Check if already owned (recursive case)
    if (m_state->owner_pid.load(std::memory_order_relaxed) == my_pid) { // Use relaxed as per spec
        m_state->recursion_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    pylabhub::utils::ExponentialBackoff backoff_strategy;
    int iteration = 0;

    // CAS loop with timeout
    uint64_t expected_pid = 0;
    while (!m_state->owner_pid.compare_exchange_weak(
            expected_pid, my_pid,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {

        // Check if lock holder is dead
        if (expected_pid != 0 && !pylabhub::platform::is_process_alive(expected_pid)) {
            // Force reclaim zombie lock
            LOGGER_WARN(
                "SharedSpinLock '{}': Detected dead owner PID {}. Force reclaiming.",
                m_name, expected_pid);
            m_state->owner_pid.store(my_pid, std::memory_order_acquire);
            m_state->recursion_count.store(1, std::memory_order_relaxed);
            m_state->generation.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Timeout check
        if (timeout_ms > 0)
        {
            auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start_time);
            if (elapsed_time.count() >= timeout_ms)
            {
                return false; // Timeout
            }
        }

        expected_pid = 0; // Reset for next CAS attempt
        backoff_strategy(iteration++); // Exponential backoff
    }

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

    if (m_state->owner_pid.load(std::memory_order_acquire) != current_pid)
    {
        LOGGER_ERROR("SharedSpinLock '{}': Attempted to unlock by non-owner. Current owner PID {}, "
                     "Caller PID {}.",
                     m_name, m_state->owner_pid.load(std::memory_order_acquire), current_pid);
        throw std::runtime_error("Attempted to unlock by non-owner.");
    }

    if (m_state->recursion_count.load(std::memory_order_relaxed) > 1)
    {
        m_state->recursion_count.fetch_sub(1, std::memory_order_relaxed);
        return; // Still recursively locked by this process
    }

    // Release the lock
    m_state->recursion_count.store(0, std::memory_order_release);
    m_state->generation.fetch_add(1, std::memory_order_release); // Increment generation
    m_state->owner_pid.store(0, std::memory_order_release);      // Finally release ownership
}

bool SharedSpinLock::is_locked_by_current_process() const
{
    return m_state->owner_pid.load(std::memory_order_acquire) == get_current_pid();
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
