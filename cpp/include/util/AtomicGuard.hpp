// AtomicGuard.hpp
#pragma once

/*
AtomicGuard.hpp — token-based ownership guard using a single atomic owner word.

Design & invariants (key points)
--------------------------------
- Each AtomicGuard instance has a persistent, non-zero token (my_token_) assigned
  at construction time via generate_token(). Tokens identify guard instances and
  remain stable for the guard's lifetime.
- AtomicOwner::state_ is the authoritative, shared owner word:
    0 == free/unowned
    non-zero == token of the owning AtomicGuard
- acquire()/release() are lightweight and lock-free: they use the guard's
  persistent token and perform a single CAS on AtomicOwner::state_. They do NOT
  acquire per-guard locks and are safe to call concurrently from different
  guards on the same AtomicOwner.
- transfer_to(dest) is the single explicit transfer operation that moves the
  effective ownership on the shared AtomicOwner by swapping tokens (CAS).
  transfer_to acquires both guards' mutexes (std::scoped_lock) to make the
  multi-field update consistent and to avoid races. transfer_to does NOT change
  the guards' persistent tokens; it only changes which token the AtomicOwner
  currently holds.
- Copying and moving AtomicGuard objects is forbidden to prevent implicit
  ownership transfer; use transfer_to explicitly.
- This header assumes C++17 or later (inline static variables, scoped_lock).

Concurrency contract (summary)
-----------------------------
- acquire()/release(): lock-free, use atomics; safe across different guards that
  operate on the same AtomicOwner.
- transfer_to(dest): blocking (uses std::scoped_lock). During transfer_to the
  result of active() may be transient; callers that need consistent multi-field
  observations should lock the guard's mutex via guard_mutex().
- attach()/detach_no_release(): modify per-guard attachment and take the guard
  mutex; the observable state across threads may be transient unless callers
  synchronize (use guard_mutex()).
- Destructor: sets being_destructed_ and acquires the guard mutex to serialize
  with transfer_to (see destructor comments). transfer_to will detect an
  in-progress destructor and fail (return false) rather than racing.
*/

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <utility>

#include "platform.hpp" // project platform macro header (authoritative)

namespace pylabhub::util
{

// -----------------------------------------------------------------------------
// AtomicOwner: the shared owner token for a controlled resource
// -----------------------------------------------------------------------------
class AtomicOwner
{
  public:
    AtomicOwner() noexcept : state_(0) {}
    explicit AtomicOwner(uint64_t initial) noexcept : state_(initial) {}

    AtomicOwner(const AtomicOwner &) = delete;
    AtomicOwner &operator=(const AtomicOwner &) = delete;

    // Load/store: sequentially-consistent ordering is used for debug-friendliness
    // and predictable behavior; this is conservative but safe.
    uint64_t load() const noexcept { return state_.load(std::memory_order_seq_cst); }
    void store(uint64_t v) noexcept { state_.store(v, std::memory_order_seq_cst); }

    // CAS wrapper: on success use acq_rel so the successful swap performs both
    // release (publish previous writes) and acquire semantics; on failure use
    // acquire so the caller sees a coherent view of the current owner.
    bool compare_exchange_strong(uint64_t &expected, uint64_t desired) noexcept
    {
        return state_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    // Convenience query whether owner is free (state == 0)
    bool is_free() const noexcept { return load() == 0; }

    std::atomic<uint64_t> &atomic_ref() noexcept { return state_; }
    const std::atomic<uint64_t> &atomic_ref() const noexcept { return state_; }

  private:
    std::atomic<uint64_t> state_;
};

// -----------------------------------------------------------------------------
// AtomicGuard: per-guard token + lightweight acquire/release + explicit transfer
// -----------------------------------------------------------------------------
/*
Detailed behavior per API:
 - Constructor: generates a persistent, non-zero token for the guard instance and
   optionally attaches to an AtomicOwner. Token generation is thread-safe via a
   global atomic counter.
 - ~AtomicGuard(): marks the guard as being destroyed (sets being_destructed_),
   then acquires the guard mutex and performs a best-effort release(). The
   destructor blocks briefly while holding the guard mutex to serialize with
   potential concurrent transfer_to() calls, making transfer semantics
   deterministic with respect to destruction.
 - attach(owner): attach the guard to an owner pointer (does not acquire the
   owner). The function acquires the guard mutex to mutate attachment state.
 - detach_no_release(): detach the guard from its owner without releasing. The
   caller becomes responsible for avoiding leaks.
 - acquire(): lock-free CAS: attempt to change AtomicOwner::state_ from 0 -> token.
   Returns true on success. Does not take guard mutex.
 - release(): lock-free CAS: attempt to change AtomicOwner::state_ from token -> 0.
   Returns true on success. Does not take guard mutex.
 - attach_and_acquire(): attaches under the guard mutex then performs the
   lightweight acquire() (which does not take the guard mutex).
 - active(): best-effort check whether this guard is owner (may be transient if
   transfer_to or attach is ongoing). For stable readings, lock guard_mutex().
 - transfer_to(dest): the only operation that takes both guards' mutexes (via
   std::scoped_lock). It attempts a CAS to replace this->token with dest->token
   on the shared AtomicOwner. If the CAS succeeds the AtomicOwner now holds
   dest's token and dest.owner_ is set to the same owner. The call returns true
   on success and false on transient failure, destructor involvement, or if the
   guards are attached to different owners (cross-owner transfers are rejected).
*/

class AtomicGuard
{
  public:
    // Default constructor: generate a persistent token and leave detached.
    AtomicGuard() noexcept
    {
        uint64_t t = generate_token();
        my_token_.store(t, std::memory_order_release);
        owner_.store(nullptr, std::memory_order_release);
    }

    // Construct attached to an owner (optionally attempt a single acquire).
    explicit AtomicGuard(AtomicOwner *owner, bool tryAcquire = false) noexcept
    {
        uint64_t t = generate_token();
        my_token_.store(t, std::memory_order_release);
        owner_.store(owner, std::memory_order_release);
        if (tryAcquire && owner)
            acquire(); // lightweight, lock-free attempt
    }

    // No copy, no move. Explicit transfer_to only.
    AtomicGuard(const AtomicGuard &) = delete;
    AtomicGuard &operator=(const AtomicGuard &) = delete;
    AtomicGuard(AtomicGuard &&) = delete;
    AtomicGuard &operator=(AtomicGuard &&) = delete;

    // Destructor: mark destructing then serialize with any concurrent transfers by
    // taking the guard mutex. This blocks briefly if another thread is performing
    // transfer_to involving this guard; that is intentional to avoid races.
    ~AtomicGuard() noexcept
    {
        // Mark that we're being destructed so transfer_to can detect this quickly.
        being_destructed_.store(true, std::memory_order_release);

        // Acquire the guard mutex to serialize final cleanup with any transfer_to.
        // Holding the mutex ensures transfer_to can't concurrently modify per-guard
        // fields while we do the final release. Blocking briefly here is a conscious
        // trade-off for deterministic semantics.
        std::lock_guard<std::mutex> lk(guard_mtx_);

        // Best-effort release using the lock-free release() implementation.
        release();
    }

    // Attach to owner without acquiring (thread-safe wrt transfer_to).
    // NOTE: attach only sets the owner pointer under the guard mutex. The overall
    // system state may be transient relative to other threads until synchronization
    // is applied (see the concurrency contract at the top). For multi-field
    // observations use guard_mutex() externally.
    void attach(AtomicOwner *owner) noexcept
    {
        std::lock_guard<std::mutex> lk(guard_mtx_);
        owner_.store(owner, std::memory_order_release);
    }

    // Detach without releasing; caller is responsible to avoid leaks.
    void detach_no_release() noexcept
    {
        std::lock_guard<std::mutex> lk(guard_mtx_);
        owner_.store(nullptr, std::memory_order_release);
    }

    // Lightweight attempt to acquire: CAS 0 -> my_token_. Returns true on success.
    // This is lock-free and cheap; it does not lock guard_mtx_.
    bool acquire() noexcept
    {
        AtomicOwner *own = owner_.load(std::memory_order_acquire);
        if (!own)
            return false;

        uint64_t tok = my_token_.load(std::memory_order_acquire);
        assert(tok != 0 &&
               "my_token_ must be non-zero (token should be generated at construction)");

        uint64_t expected = 0;
        if (own->atomic_ref().compare_exchange_strong(expected, tok, std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
        {
            return true;
        }
        return false;
    }

    // Lightweight attempt to release: CAS my_token_ -> 0. Returns true on success.
    // Lock-free; does not lock guard_mtx_.
    bool release() noexcept
    {
        AtomicOwner *own = owner_.load(std::memory_order_acquire);
        if (!own)
            return false;

        uint64_t tok = my_token_.load(std::memory_order_acquire);
        assert(tok != 0 && "my_token_ must be non-zero");

        uint64_t expected = tok;
        if (own->atomic_ref().compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
        {
            // Ownership cleared in shared owner; token remains with guard (persistent).
            return true;
        }
        return false;
    }

    // Attach + try to acquire (thread-safe wrt transfer_to).
    bool attach_and_acquire(AtomicOwner *owner) noexcept
    {
        std::lock_guard<std::mutex> lk(guard_mtx_);
        owner_.store(owner, std::memory_order_release);
        // lightweight acquire: CAS on owner (lock-free)
        return acquire();
    }

    // Check if this guard currently holds ownership (best-effort).
    // IMPORTANT: active() may be transient or inconsistent if called concurrently
    // with transfer_to() or attach(). To get a stable view, lock guard_mutex()
    // before calling active().
    bool active() const noexcept
    {
        AtomicOwner *own = owner_.load(std::memory_order_acquire);
        if (!own)
            return false;
        uint64_t tok = my_token_.load(std::memory_order_acquire);
        if (tok == 0)
            return false; // defensive; token should never be zero
        uint64_t cur = own->atomic_ref().load(std::memory_order_acquire);
        return cur == tok;
    }

    // Return this guard's persistent token (non-zero).
    uint64_t token() const noexcept { return my_token_.load(std::memory_order_acquire); }

    // Access to the guard mutex for callers that need to perform multi-field
    // observations atomically (advanced usage). Use with caution to avoid
    // deadlocks — if locking multiple guards externally, follow a global order
    // or use std::scoped_lock on the set of mutexes.
    std::mutex &guard_mutex() const noexcept { return const_cast<std::mutex &>(guard_mtx_); }

    // -----------------------------
    // transfer_to: the ONLY operation that moves ownership on the shared owner.
    //
    // Semantics:
    //  - Fast pre-check: if either guard is already being destructed, return false.
    //  - Acquire both guard mutexes with std::scoped_lock (blocking, deadlock-free).
    //  - Re-check desctruction flags under the lock to avoid TOCTOU races.
    //  - If this guard is currently the owner (shared owner contains my_token_), attempt
    //    CAS to replace my_token_ with dest.my_token_. On success, set dest.owner_.
    //  - Returns true on success; false on transient failure, destructor involvement,
    //    or cross-owner mismatch (dest already attached to a different owner).
    //
    // Note: transfer_to will block briefly waiting for the guard mutexes if necessary.
    // This keeps the implementation simple and deterministic. Because the destructor
    // also acquires the guard mutex, transfer_to will not silently race with destructor:
    // either transfer_to runs first, or destructor waits for it (or transfer_to sees
    // the being_destructed_ flag and returns false).
    bool transfer_to(AtomicGuard &dest) noexcept
    {
        // Fast pre-check: if either guard is being destructed, fail early.
        if (being_destructed_.load(std::memory_order_acquire) ||
            dest.being_destructed_.load(std::memory_order_acquire))
        {
            return false;
        }

        // Acquire both guards' mutexes using scoped_lock (blocks until both are acquired).
        std::scoped_lock lk(guard_mtx_, dest.guard_mtx_);

        // Re-check under the locks to avoid TOCTOU races.
        if (being_destructed_.load(std::memory_order_acquire) ||
            dest.being_destructed_.load(std::memory_order_acquire))
        {
            return false;
        }

        // Now we can safely examine per-guard fields and operate on the shared owner.
        AtomicOwner *own = owner_.load(std::memory_order_acquire);
        uint64_t mytok = my_token_.load(std::memory_order_acquire);

        if (!own || mytok == 0)
            return false; // not attached or token invalid

        AtomicOwner *d_own = dest.owner_.load(std::memory_order_acquire);
        if (d_own && d_own != own)
            return false; // reject cross-owner transfer

        // dest token should already be non-zero (generated at construction).
        uint64_t dest_tok = dest.my_token_.load(std::memory_order_acquire);
        assert(dest_tok != 0 && "dest token must be non-zero (generated at creation)");

        // Perform the atomic token swap: replace mytok with dest_tok on the owner.
        uint64_t expected = mytok;
        bool ok = own->atomic_ref().compare_exchange_strong(
            expected, dest_tok, std::memory_order_acq_rel, std::memory_order_acquire);
        if (!ok)
            return false; // CAS failed due to concurrent owner change

        // CAS succeeded: update dest.owner_ (still under locks so visible to readers
        // synchronizing on the guard mutex).
        dest.owner_.store(own, std::memory_order_release);

        return true;
    }

  private:
    // Guard-local fields are atomic so that acquire()/release() remain lock-free.
    std::atomic<AtomicOwner *> owner_{nullptr};
    std::atomic<uint64_t> my_token_{0}; // non-zero after construction

    // Mutex used only for transfer_to / attach / detach to protect multi-field updates.
    mutable std::mutex guard_mtx_;

    // Flag set during destruction to prevent racing transfers. transfer_to checks
    // this flag and will fail if either guard is being destructed.
    std::atomic<bool> being_destructed_{false};

    // Token generator: persistent per-process counter (inline variable requires C++17)
    static inline std::atomic<uint64_t> next_token_{1};

    // Thread-safe token generator. We use relaxed ordering because uniqueness is the
    // only required property; there are no ordering constraints between token
    // generation and other atomic operations.
    static uint64_t generate_token() noexcept
    {
        uint64_t t = next_token_.fetch_add(1, std::memory_order_relaxed);
        // Skip zero if counter wrapped (extremely unlikely).
        if (t == 0)
        {
            do
            {
                t = next_token_.fetch_add(1, std::memory_order_relaxed);
            } while (t == 0);
        }
        return t;
    }
};

} // namespace pylabhub::util

// -----------------------------------------------------------------------------
// Platform notes (short):
// - This header requires C++17 or later (inline variables, std::scoped_lock, etc.).
// - On modern 64-bit POSIX/Windows systems std::atomic<uint64_t> is usually lock-free.
//   On some 32-bit targets 64-bit atomics may be emulated (slower). The code remains
//   correct but performance characteristics differ. If you require lock-free 64-bit
//   atomics, consider adding a compile-time check (optional).
// -----------------------------------------------------------------------------
// example_raii.cpp
// Demonstrates common RAII usage of AtomicGuard.
//
// Build:
//   g++ -std=c++17 -O2 example_raii.cpp -o example_raii -pthread
//
// Run:
//   ./example_raii
/////////////////////////////////////////////////////////////
// Example of RAII use
//
// #include "AtomicGuard.hpp"
// #include <iostream>
//
// using namespace pylabhub::util;
//
// int main()
// {
//     AtomicOwner owner;
//
//     // 1) Simple RAII: construct with owner and tryAcquire=true to attempt immediate acquire.
//     {
//         AtomicGuard guard(&owner, true); // attach and try an acquire
//         if (guard.active()) {
//             std::cout << "Guard acquired owner on construction (token=" << guard.token() <<
//             ")\n";
//         } else {
//             std::cout << "Guard did not acquire owner on construction; can call acquire()
//             manually.\n"; if (guard.acquire()) {
//                 std::cout << "Guard acquired via acquire() (token=" << guard.token() << ")\n";
//             }
//         }
//
//         // Work while owning the resource...
//         // When the guard goes out of scope (end of this block), its destructor will
//         // call release() to attempt to release the owner (RAII).
//     } // destructor runs here (best-effort release)
//
//     // 2) attach_and_acquire: attach then acquire in one call (thread-safe w.r.t transfer_to).
//     {
//         AtomicGuard guard;
//         bool ok = guard.attach_and_acquire(&owner);
//         std::cout << "attach_and_acquire returned " << (ok ? "true" : "false") << "\n";
//         if (ok) {
//             std::cout << "Guard owns owner with token " << guard.token() << "\n";
//         }
//     } // destructor releases if owned
//
//     // 3) If you need to check active() and want a consistent multi-field snapshot,
//     //    lock the guard's mutex before querying. This is advanced usage:
//     {
//         AtomicGuard g1(&owner), g2(&owner);
//         g1.acquire();
//
//         // transfer ownership g1 -> g2
//         if (g1.transfer_to(g2)) {
//             // To observe that g2 is owner in a race-free way, lock g2's mutex:
//             std::lock_guard<std::mutex> lk(g2.guard_mutex());
//             bool active_now = g2.active(); // stable while holding guard_mutex()
//             std::cout << "After transfer, g2.active() (under lock) = " << active_now << "\n";
//         }
//         // g2 will release when out of scope if it holds ownership.
//     }
//
//     return 0;
// }
