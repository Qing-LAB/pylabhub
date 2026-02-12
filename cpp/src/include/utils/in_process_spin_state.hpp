#pragma once
/**
 * @file in_process_spin_state.hpp
 * @brief In-process spin state (token mode): state owner + guard that performs locking.
 *
 * This header provides the **state owner** (InProcessSpinState), which holds the
 * unified 32-byte SharedSpinLockState. The **locking/guarding** is done by the
 * guard (SpinGuard / InProcessSpinStateGuard), which acquires and releases via
 * token. Same layout as SharedSpinLock; pid/tid are 0, generation holds the token.
 *
 * User API:
 *   - State owner: InProcessSpinState (or get one via make_in_process_spin_state()).
 *   - Holder / lock operator: SpinGuard. You get a holder by constructing it with
 *     the state (blocking acquire) or by try_lock(state, timeout).
 *
 * Example:
 *   @code
 *   auto state = make_in_process_spin_state();
 *
 *   // Blocking: construct guard → guard performs lock; you hold until guard scope ends
 *   {
 *       SpinGuard g(state);
 *       // ... critical section ...
 *   }   // g destroyed → guard releases
 *
 *   SpinGuard g;
 *   if (g.try_lock(state, 100)) {
 *       // ... critical section ...
 *       g.release();
 *   }
 *
 *   // Handoff to another thread
 *   std::promise<SpinGuard> p;
 *   std::thread t1([&] { SpinGuard g(state); p.set_value(std::move(g)); });
 *   std::thread t2([&] { SpinGuard g = p.get_future().get(); g.release(); });
 *   @endcode
 *
 * Thread safety:
 *   - State (InProcessSpinState): Thread-safe. Multiple threads may contend on the
 *     same state; mutual exclusion is enforced. The guard performs the actual lock.
 *   - Guard (SpinGuard): Not safe for concurrent use on the same instance. Handoff
 *     (move to another thread) is safe.
 *
 * ABI: Header-only. Do not change member layout.
 * Exceptions: All public APIs are noexcept.
 */
#include "utils/detail/spinlock_owner_ops.hpp"
#include "utils/shared_memory_spinlock.hpp"
#include "utils/backoff_strategy.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace pylabhub::hub
{

/**
 * In-process spin state (token semantics). Owns the 32-byte state; does not
 * perform locking by itself—the guard (SpinGuard) does the acquire/release.
 *
 * Thread-safe: multiple threads may contend on the same InProcessSpinState;
 * the guard (or with_token APIs) performs the actual lock/unlock.
 *
 * Copy: deleted. Move: deleted (state contains atomics; use factory, e.g. RVO).
 */
class InProcessSpinState
{
  public:
    InProcessSpinState() noexcept { init_spinlock_state(&state_); }

    /**
     * Acquire with a token supplied by the caller (e.g. guard). No token generation here.
     */
    bool try_acquire_with_token(uint64_t token) noexcept
    {
        return detail::try_acquire_token(&state_, token);
    }

    /** Spin until acquired using the given token (caller-owned). */
    void lock_with_token(uint64_t token) noexcept
    {
        pylabhub::utils::ExponentialBackoff backoff;
        int i = 0;
        while (!detail::try_acquire_token(&state_, token))
            backoff(i++);
    }

    /** Try to acquire with timeout using the given token. Uses std::chrono (header-only; cross-process code uses platform::monotonic_time_ns). */
    bool try_lock_for_with_token(int timeout_ms, uint64_t token) noexcept
    {
        if (detail::try_acquire_token(&state_, token))
            return true;
        if (timeout_ms <= 0)
            return false;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        pylabhub::utils::ExponentialBackoff backoff;
        int iteration = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (detail::try_acquire_token(&state_, token))
                return true;
            backoff(iteration++);
        }
        return false;
    }

    /** @return true if acquired; on success, out_token is the holder token. */
    bool try_lock_for(int timeout_ms, uint64_t &out_token) noexcept
    {
        uint64_t token = detail::next_token();
        if (try_lock_for_with_token(timeout_ms, token))
        {
            out_token = token;
            return true;
        }
        return false;
    }

    void lock(uint64_t &out_token) noexcept
    {
        uint64_t token = detail::next_token();
        lock_with_token(token);
        out_token = token;
    }

    void unlock(uint64_t token) noexcept
    {
        detail::release_token(&state_, token);
    }

    bool is_locked() const noexcept
    {
        return detail::token_lock_held(&state_);
    }

    SharedSpinLockState *state() noexcept { return &state_; }
    const SharedSpinLockState *state() const noexcept { return &state_; }

    InProcessSpinState(const InProcessSpinState &) = delete;
    InProcessSpinState &operator=(const InProcessSpinState &) = delete;
    InProcessSpinState(InProcessSpinState &&) noexcept = delete;
    InProcessSpinState &operator=(InProcessSpinState &&) noexcept = delete;

  private:
    SharedSpinLockState state_;
};

/**
 * RAII guard that performs locking on an InProcessSpinState. Move-only; handoff OK.
 * Token is generated once per guard. The guard does the acquire/release; the state
 * owner only holds the 32-byte state.
 *
 * Copy: deleted. Move: allowed (handoff).
 * Not thread-safe on the same instance; handoff (move) to another thread is safe.
 *
 * Lifetime: The guard stores a raw pointer to InProcessSpinState. You must ensure
 * the state outlives the guard (e.g. state at class or module scope, guards in
 * function scope). If the state is destroyed while a guard still references it,
 * the guard's destructor would perform use-after-free when calling state->unlock().
 * To avoid that, (1) keep state at broader scope than guards, or (2) use a
 * shared owner (e.g. std::shared_ptr<InProcessSpinState>) and have the guard
 * hold a shared_ptr so the state cannot be destroyed while the guard exists.
 */
class InProcessSpinStateGuard
{
  public:
    InProcessSpinStateGuard() noexcept : state_(nullptr), token_(0) {}

    /** Construct and acquire (guard performs lock; blocks until acquired). */
    explicit InProcessSpinStateGuard(InProcessSpinState &s)
        : state_(&s), token_(detail::next_token())
    {
        s.lock_with_token(token_);
    }

    ~InProcessSpinStateGuard() noexcept
    {
        if (state_ && token_ != 0)
        {
            state_->unlock(token_);
            token_ = 0;
        }
    }

    InProcessSpinStateGuard(InProcessSpinStateGuard &&other) noexcept
        : state_(other.state_), token_(other.token_)
    {
        other.state_ = nullptr;
        other.token_ = 0;
    }

    InProcessSpinStateGuard &operator=(InProcessSpinStateGuard &&other) noexcept
    {
        if (this != &other)
        {
            if (state_ && token_ != 0)
                state_->unlock(token_);
            state_ = other.state_;
            token_ = other.token_;
            other.state_ = nullptr;
            other.token_ = 0;
        }
        return *this;
    }

    /**
     * Try to acquire the given state (optionally with timeout).
     * If this guard currently holds another state, that one is released first.
     */
    [[nodiscard]] bool try_lock(InProcessSpinState &s, int timeout_ms = 0) noexcept
    {
        if (state_ && token_ != 0)
            state_->unlock(token_);
        state_ = &s;
        if (token_ == 0)
            token_ = detail::next_token();
        if (!s.try_lock_for_with_token(timeout_ms, token_))
        {
            state_ = nullptr;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool release() noexcept
    {
        if (!state_ || token_ == 0)
            return false;
        state_->unlock(token_);
        token_ = 0;
        return true;
    }

    void detach() noexcept
    {
        state_ = nullptr;
        token_ = 0;
    }

    InProcessSpinStateGuard(const InProcessSpinStateGuard &) = delete;
    InProcessSpinStateGuard &operator=(const InProcessSpinStateGuard &) = delete;

    [[nodiscard]] bool holds_lock() const noexcept { return state_ != nullptr && token_ != 0; }
    [[nodiscard]] uint64_t token() const noexcept { return token_; }

  private:
    InProcessSpinState *state_;
    uint64_t token_;
};

/** Factory: returns an in-process spin state (token mode). */
inline InProcessSpinState make_in_process_spin_state()
{
    return InProcessSpinState{};
}

/** User-facing alias: the guard performs locking; use SpinGuard in user code. */
using SpinGuard = InProcessSpinStateGuard;

} // namespace pylabhub::hub
