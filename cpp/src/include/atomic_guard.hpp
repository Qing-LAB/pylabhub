// AtomicGuard.hpp
#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>

namespace pylabhub::basics
{

/*******************************************************************************
 * Minimal header-only AtomicOwner + AtomicGuard
 *
 * Design & semantics highlights:
 * - No internal mutex.
 * - Move-only guard semantics: move ctor/assign transfer ownership state.
 * - After a move, the source is left valid, detached, inactive, AND receives
 *   a fresh token so tokens remain unique.
 * - `active()` is authoritative: it checks the AtomicOwner's current token.
 *
 * Thread-safety contract:
 * - It is UB to concurrently access the *same* AtomicGuard object from multiple
 *   threads without external synchronization. Moving a guard must not race with
 *   other operations on that same guard object.
 ******************************************************************************/

class AtomicOwner
{
  public:
    AtomicOwner() noexcept : state_(0) {}
    explicit AtomicOwner(uint64_t initial) noexcept : state_(initial) {}

    AtomicOwner(const AtomicOwner &) = delete;
    AtomicOwner &operator=(const AtomicOwner &) = delete;
    AtomicOwner(AtomicOwner &&) noexcept = delete;            // atomic state cannot be moved
    AtomicOwner &operator=(AtomicOwner &&) noexcept = delete; // atomic state cannot be moved

    uint64_t load() const noexcept { return state_.load(std::memory_order_acquire); }
    void store(uint64_t v) noexcept { state_.store(v, std::memory_order_release); }

    bool compare_exchange_strong(uint64_t &expected, uint64_t desired) noexcept
    {
        return state_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    bool is_free() const noexcept { return load() == 0; }

    std::atomic<uint64_t> &atomic_ref() noexcept { return state_; }
    const std::atomic<uint64_t> &atomic_ref() const noexcept { return state_; }

  private:
    std::atomic<uint64_t> state_;
};

class AtomicGuard
{
  public:
    // Default: detached guard with fresh token.
    AtomicGuard() noexcept : owner_(nullptr), token_(generate_token()), is_active_(false) {}

    // Attach to owner and optionally try to acquire.
    explicit AtomicGuard(AtomicOwner *owner, bool tryAcquire = false) noexcept
        : owner_(owner), token_(generate_token()), is_active_(false)
    {
        if (tryAcquire && owner_)
            (void)acquire();
    }

    AtomicGuard(const AtomicGuard &) = delete;
    AtomicGuard &operator=(const AtomicGuard &) = delete;

    // Move constructor: take source token/owner/is_active; source left detached + new token.
    AtomicGuard(AtomicGuard &&o) noexcept
        : owner_(o.owner_), token_(o.token_), is_active_(o.is_active_)
    {
        // detach source and give it a fresh token
        o.owner_ = nullptr;
        o.is_active_ = false;
        o.token_ = generate_token();
    }

    // Move assignment: release ours if active, then take from source. Source gets fresh token.
    AtomicGuard &operator=(AtomicGuard &&o) noexcept
    {
        if (this == &o)
            return *this;

        // Release our own active lock (best-effort) if held.
        if (is_active_ && owner_ != nullptr)
        {
            uint64_t expected = token_;
            owner_->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                         std::memory_order_acquire);
            is_active_ = false;
        }

        // Take ownership from source
        owner_ = o.owner_;
        token_ = o.token_;
        is_active_ = o.is_active_;

        // Leave source detached/inactive and give it a fresh token
        o.owner_ = nullptr;
        o.is_active_ = false;
        o.token_ = generate_token();

        return *this;
    }

    ~AtomicGuard() noexcept
    {
        // Best-effort release ONLY IF we believe we hold the lock.
        if (!is_active_ || !owner_)
        {
            return;
        }

        // Compare owner state to token; if it matches, attempt CAS -> 0.
        uint64_t expected = token_;
        if (owner_->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
        {
            is_active_ = false;
            return;
        }

#ifndef NDEBUG
        // In debug builds assert, to catch logic errors early.
        // If we get here, it means is_active_ was true, but the owner's token
        // did not match ours. This is a true invariant violation.
        assert(false && "AtomicGuard destructor: invariant violation (is_active_ is true, but "
                        "owner has different token).");
#endif
        // In release builds, be robust: do nothing (the lock is effectively leaked).
    }

    // Attach / detach
    void attach(AtomicOwner *owner) noexcept { owner_ = owner; }

    void detach_no_release() noexcept
    {
        owner_ = nullptr;
        is_active_ = false;
    }

    // Acquire / release
    [[nodiscard]] bool acquire() noexcept
    {
        if (!owner_)
            return false;
        uint64_t expected = 0;
        if (owner_->atomic_ref().compare_exchange_strong(
                expected, token_, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            is_active_ = true;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool release() noexcept
    {
        if (!owner_)
            return false;
        uint64_t expected = token_;
        if (owner_->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
        {
            is_active_ = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool attach_and_acquire(AtomicOwner *owner) noexcept
    {
        owner_ = owner;
        return acquire();
    }

    // Authoritative active() — checks owner state rather than internal belief.
    bool active() const noexcept
    {
        if (!owner_)
            return false;
        return owner_->load() == token_;
    }

    uint64_t token() const noexcept { return token_; }

  private:
    static uint64_t generate_token() noexcept
    {
        static std::atomic<uint64_t> next{1};
        uint64_t t;
        while ((t = next.fetch_add(1, std::memory_order_relaxed)) == 0)
        {
            // wrap-around guard (extremely unlikely)
        }
        return t;
    }

    // Members
    AtomicOwner *owner_;
    uint64_t token_;
    // `is_active_` is a plain `bool` because the AtomicGuard object itself is not designed
    // for concurrent access by multiple threads. The class contract states: "It is UB to
    // concurrently access the *same* AtomicGuard object from multiple threads without
    // external synchronization." Therefore, accesses to `is_active_` do not need to be
    // atomic, as they occur within an execution path that has exclusive access to
    // this specific AtomicGuard instance.
    bool is_active_;
};

} // namespace pylabhub::basics
