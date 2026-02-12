#pragma once
/**
 * @file detail/spinlock_owner_ops.hpp
 * @brief Internal owner operations for the unified 32-byte spinlock state (PID/TID/TOKEN).
 *
 * NOT part of the public API. The spinlock **only manipulates the state** (the
 * owner memory); it never generates identity. (PID, TID, token) are always
 * supplied by the caller/factory (e.g. the guard or the current process).
 * Uniqueness of identity per logical holder is the caller's responsibility.
 *
 * Token mode: identity is (0, 0, token); we only CAS the generation field.
 * pid/tid/recursion_count remain 0.
 */
#include "../shared_memory_spinlock.hpp"

#include <atomic>
#include <cstdint>

namespace pylabhub::hub::detail
{

// -----------------------------------------------------------------------------
// Token-mode operations (in-process; same 32-byte layout, generation = token)
// -----------------------------------------------------------------------------

/** @return true if lock was free and is now held by token. */
inline bool try_acquire_token(SharedSpinLockState *state, uint64_t token) noexcept
{
    if (!state)
        return false;
    uint64_t expected = 0;
    return state->generation.compare_exchange_strong(
        expected, token, std::memory_order_acq_rel, std::memory_order_acquire);
}

/** @return true if lock was held by token and is now released. */
inline bool release_token(SharedSpinLockState *state, uint64_t token) noexcept
{
    if (!state)
        return false;
    uint64_t expected = token;
    return state->generation.compare_exchange_strong(
        expected, 0u, std::memory_order_acq_rel, std::memory_order_acquire);
}

/** @return true if state is currently held (generation != 0). */
inline bool token_lock_held(const SharedSpinLockState *state) noexcept
{
    return state && state->generation.load(std::memory_order_acquire) != 0u;
}

/** Factory: produce a unique token for token-mode holder identity (0, 0, token). Thread-safe. */
inline uint64_t next_token() noexcept
{
    static std::atomic<uint64_t> next{1};
    uint64_t t;
    do
    {
        t = next.fetch_add(1, std::memory_order_relaxed);
    } while (t == 0u);
    return t;
}

} // namespace pylabhub::hub::detail
