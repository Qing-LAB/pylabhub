// AtomicGuard.hpp
#pragma once

/*******************************************************************************
 * @file AtomicGuard.hpp
 * @brief Token-based ownership guard using a single atomic owner word.
 *
 * **Design Philosophy: Hybrid Concurrency Model**
 * `AtomicGuard` manages exclusive ownership using a hybrid approach to balance
 * performance and safety:
 * 1. **Lock-Free Fast Path**: `acquire()` and `release()` are extremely cheap,
 *    performing a single atomic CAS operation. This optimizes for the common
 *    case of acquiring and releasing a lock.
 * 2. **Blocking Slow Path**: `transfer_to()` is a complex operation that must
 *    be atomic. It uses `std::scoped_lock` to lock both guards, ensuring
 *    correctness and simplicity at the cost of being a blocking call.
 *
 * **Core Invariants**:
 * - `AtomicOwner::state_` is the single source of truth: `0` means free, while a
 *   non-zero value is the unique token of the owning `AtomicGuard`.
 * - Each `AtomicGuard` has a persistent, non-zero token that never changes.
 * - Copying and moving are disabled to make ownership semantics explicit.
 * - C++17 is required (`inline static`, `std::scoped_lock`, `[[nodiscard]]`).
 *
 * **Usage and Best Practices**:
 * 1.  **RAII-Style Guard**: The most common use is as a stack-based RAII object.
 *     The destructor automatically handles releasing the lock.
 *
 *     ```cpp
 *     AtomicOwner owner;
 *     {
 *         // Attempt to acquire on construction.
 *         AtomicGuard guard(&owner, true);
 *         if (guard.active()) {
 *             // ... work with the guarded resource ...
 *         }
 *     } // guard's destructor is called, releasing the lock.
 *     ```
 *
 * 2.  **Explicit Ownership Transfer**: To pass ownership, use `transfer_to()`.
 *
 *     ```cpp
 *     if (!source_guard.transfer_to(dest_guard)) {
 *         // Handle transfer failure (e.g., source didn't own the lock).
 *     }
 *     ```
 *
 * 3.  **Check Operation Success**: Methods like `acquire()`, `release()`, and
 *     `transfer_to()` are marked with `[[nodiscard]]`. It is critical to check
 *     their return values. The robust destructor will call `std::abort()` if it
 *     detects an invariant violation (e.g., a guard that should have released
 *     the lock but didn't), helping to catch logic errors early.
 ******************************************************************************/

#include <atomic>
#include <cstdint>
#include <memory> // For std::unique_ptr
#include <mutex>  // For std::mutex& return type in guard_mutex()
#include <fmt/core.h> // for fmt::format_to_n

#include "platform.hpp" // project platform macro header (authoritative)

namespace pylabhub::utils
{

// -----------------------------------------------------------------------------
// AtomicOwner: the shared owner token for a controlled resource
// -----------------------------------------------------------------------------
// When building a shared library, classes and functions intended for public use

// Forward-declare the implementation struct for the Pimpl idiom.
struct AtomicOwnerImpl;

class PYLABHUB_API AtomicOwner
{
  public:
    AtomicOwner() noexcept;
    explicit AtomicOwner(uint64_t initial) noexcept;

    AtomicOwner(const AtomicOwner &) = delete;
    AtomicOwner& operator=(const AtomicOwner &) = delete;
    // Pimpl allows for safe move semantics.
    AtomicOwner(AtomicOwner &&) noexcept;
    AtomicOwner& operator=(AtomicOwner &&) noexcept;
    ~AtomicOwner();

    // Public interface remains the same.
#ifndef NDEBUG
    // In debug builds, use sequentially-consistent ordering for easier debugging
    // and more predictable behavior across threads. This is conservative but safe.
    uint64_t load() const noexcept;
    void store(uint64_t v) noexcept;
#else
    // In release builds, use more relaxed (but correct) orderings for performance.
    uint64_t load() const noexcept;
    void store(uint64_t v) noexcept;
#endif

    // CAS wrapper: on success use acq_rel so the successful swap performs both
    // release (publish previous writes) and acquire semantics; on failure use
    // acquire so the caller sees a coherent view of the current owner.
    bool compare_exchange_strong(uint64_t &expected, uint64_t desired) noexcept;

    // Convenience query whether owner is free (state == 0)
    bool is_free() const noexcept;

    std::atomic<uint64_t> &atomic_ref() noexcept;
    const std::atomic<uint64_t> &atomic_ref() const noexcept;

  private:
    // The only member is a pointer to the implementation.
    std::unique_ptr<AtomicOwnerImpl> pImpl;
};

// -----------------------------------------------------------------------------
// AtomicGuard: per-guard token + lightweight acquire/release + explicit transfer
// -----------------------------------------------------------------------------

// Forward-declare the implementation struct for the Pimpl idiom.
struct AtomicGuardImpl;

class PYLABHUB_API AtomicGuard
{
  public:
    // Default constructor: generate a persistent token and leave detached.
    AtomicGuard() noexcept;

    // Construct attached to an owner (optionally attempt a single acquire).
    explicit AtomicGuard(AtomicOwner *owner, bool tryAcquire = false) noexcept;
    
    // No copy. Move is now enabled and safe due to Pimpl.
    AtomicGuard(const AtomicGuard &) = delete;
    AtomicGuard &operator=(const AtomicGuard &) = delete;
    AtomicGuard(AtomicGuard &&) noexcept;
    AtomicGuard &operator=(AtomicGuard &&) noexcept;

    // Destructor: performs a robust, best-effort release.
    ~AtomicGuard() noexcept;

    // Attach to owner without acquiring (thread-safe wrt transfer_to).
    void attach(AtomicOwner *owner) noexcept;

    // Detach without releasing; caller is responsible to avoid leaks.
    void detach_no_release() noexcept;

    // Lightweight attempt to acquire: CAS 0 -> my_token_. Returns true on success.
    [[nodiscard]] bool acquire() noexcept;

    // Lightweight attempt to release: CAS my_token_ -> 0. Returns true on success.
    [[nodiscard]] bool release() noexcept;

    // Attach + try to acquire (thread-safe wrt transfer_to).
    [[nodiscard]] bool attach_and_acquire(AtomicOwner *owner) noexcept;

    // Check if this guard currently holds ownership (best-effort).
    bool active() const noexcept;

    // Return this guard's persistent token (non-zero).
    uint64_t token() const noexcept;

    // Access to the guard mutex for callers that need to perform multi-field
    // observations atomically (advanced usage). Use with caution to avoid
    // deadlocks — if locking multiple guards externally, follow a global order
    // or use std::scoped_lock on the set of mutexes.
    std::mutex &guard_mutex() const noexcept;

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
    [[nodiscard]] bool transfer_to(AtomicGuard &dest) noexcept;

  private:
    // The only data member is a pointer to the implementation.
    // This provides a stable ABI.
    std::unique_ptr<AtomicGuardImpl> pImpl;

    // Token generator: persistent per-process counter.
    static std::atomic<uint64_t> next_token_;

    // Thread-safe token generator.
    static uint64_t generate_token() noexcept;
};

} // namespace pylabhub::util
