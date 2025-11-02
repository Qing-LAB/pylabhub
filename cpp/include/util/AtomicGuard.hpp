// AtomicGuard.hpp
#pragma once
/*
AtomicGuard.hpp — token-based ownership guard using a single atomic owner word.

Namespace: pylabhub::util

Design summary:
 - AtomicOwner: holds a std::atomic<uint64_t> state. 0 == free; non-zero == owner token.
 - AtomicGuard: each guard has a unique token (uint64_t). To acquire ownership, CAS 0->token.
   To release, CAS token->0. Only the holder with the matching token may release.
 - Move semantics are safe: moving transfers the token; moved-from guard won't release.
 - Memory ordering: compare_exchange_strong uses acq_rel on success and acquire on failure.
 - No per-guard atomic 'active' flag is required: ownership determined by token equality.

Usage:
  pylabhub::util::AtomicOwner owner;
  pylabhub::util::AtomicGuard g(&owner);
  if (g.acquire()) { ... } // exclusive ownership
*/

#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>

namespace pylabhub::util
{

// AtomicOwner: small wrapper owning the atomic owner-word
struct AtomicOwner
{
    // state_: 0 means free. any non-zero value is an owner token (unique per guard).
    std::atomic<uint64_t> state_{0};

    // Load the current owner token (0 if free).
    uint64_t load() const noexcept { return state_.load(std::memory_order_acquire); }

    // For debugging / introspection: return true if free.
    bool is_free() const noexcept { return load() == 0; }
};

// AtomicGuard: RAII token-based guard
class AtomicGuard
{
  public:
    // Default ctor: not owning, not attached to any AtomicOwner
    AtomicGuard() noexcept = default;

    // Construct with pointer to AtomicOwner. Does NOT acquire by default.
    explicit AtomicGuard(AtomicOwner *owner, bool tryAcquire = false) noexcept : owner_(owner)
    {
        if (tryAcquire && owner_)
        {
            acquire(); // best-effort acquire
        }
    }

    // Non-copyable
    AtomicGuard(const AtomicGuard &) = delete;
    AtomicGuard &operator=(const AtomicGuard &) = delete;

    // Move ctor: steal token and owner pointer. Moved-from object becomes inert.
    AtomicGuard(AtomicGuard &&other) noexcept : owner_(other.owner_), my_token_(other.my_token_)
    {
        other.owner_ = nullptr;
        other.my_token_ = 0;
    }

    // Move assignment: release current ownership if any, then steal other's token.
    AtomicGuard &operator=(AtomicGuard &&other) noexcept
    {
        if (this != &other)
        {
            // release our ownership (if any)
            release();

            // steal
            owner_ = other.owner_;
            my_token_ = other.my_token_;

            other.owner_ = nullptr;
            other.my_token_ = 0;
        }
        return *this;
    }

    // Destructor: if we currently own (token != 0) try to release.
    // noexcept so destruction during stack-unwind is safe.
    ~AtomicGuard() noexcept
    {
        // Attempt release; failure means the shared state no longer holds our token,
        // in which case we silently proceed (avoid throwing in destructor).
        release();
    }

    // Attempt to acquire ownership; returns true if we got it.
    // On success, this guard's token is placed into the shared atomic.
    bool acquire() noexcept
    {
        if (!ensure_token())
            return false; // can't acquire without token (rare)
        if (!owner_)
            return false;

        uint64_t expected = 0;
        // Attempt to become the owner: CAS 0 -> my_token_
        bool ok =
            owner_->state_.compare_exchange_strong(expected, my_token_,
                                                   std::memory_order_acq_rel, // success ordering
                                                   std::memory_order_acquire  // failure ordering
            );
        return ok;
    }

    // Try acquire only if currently free (alias for acquire()).
    bool try_acquire_if_free() noexcept { return acquire(); }

    // Release ownership only if we are currently the owner.
    // Returns true if we performed the release (CAS token -> 0 succeeded).
    // If we are not the owner or have no token, returns false.
    bool release() noexcept
    {
        if (!owner_ || my_token_ == 0)
            return false;

        uint64_t expected = my_token_;
        bool ok = owner_->state_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                         std::memory_order_acquire);
        if (ok)
        {
            my_token_ = 0; // clear token: destructor won't attempt again
        }
        return ok;
    }

    // Return true if this guard currently *holds* ownership (best-effort).
    // This checks that my_token_ != 0 and the shared owner's current token equals ours.
    bool active() const noexcept
    {
        if (!owner_ || my_token_ == 0)
            return false;
        uint64_t cur = owner_->state_.load(std::memory_order_acquire);
        return cur == my_token_;
    }

    // Attach this guard to a different AtomicOwner (does not acquire).
    // Releases our current ownership first (if any), then attaches.
    void attach(AtomicOwner *new_owner) noexcept
    {
        if (owner_ == new_owner)
            return;
        release();
        owner_ = new_owner;
        // my_token_ kept (identity preserved); token only meaningful relative to owner.
    }

    // Detach: release current ownership (if any) and forget owner pointer.
    void detach() noexcept
    {
        release();
        owner_ = nullptr;
    }

    // Get underlying token for diagnostics (0 means none / not generated).
    uint64_t token() const noexcept { return my_token_; }

    // Generate a token without trying to acquire. Returns non-zero token.
    uint64_t ensure_token() noexcept
    {
        if (my_token_ != 0)
            return my_token_;
        // generate token
        uint64_t t = next_token_.fetch_add(1, std::memory_order_relaxed);
        // avoid zero
        if (t == 0)
        {
            t = next_token_.fetch_add(1, std::memory_order_relaxed);
            if (t == 0)
                t = 1;
        }
        my_token_ = t;
        return my_token_;
    }

  private:
    AtomicOwner *owner_{nullptr};
    uint64_t my_token_{0}; // 0 = no token / not currently owning

    // token generator: unique non-zero token values
    static std::atomic<uint64_t> next_token_;
};

// static member definition (inline)
inline std::atomic<uint64_t> AtomicGuard::next_token_{1};

} // namespace pylabhub::util
