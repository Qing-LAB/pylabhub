// AtomicGuard.hpp
#pragma once

/*******************************************************************************
 * @file AtomicGuard.hpp
 * @brief Token-based ownership guard using a single atomic owner word.
 *
 * **Design Philosophy: Hybrid Concurrency & ABI Stability**
 * `AtomicGuard` manages exclusive ownership using a hybrid approach to balance
 * performance, safety, and long-term library compatibility:
 * 1. **Lock-Free Fast Path**: `acquire()` and `release()` are extremely cheap,
 *    performing a single atomic CAS operation. This optimizes for the common
 *    case of acquiring and releasing a lock within a single scope.
 * 2. **Blocking Slow Path**: `transfer_to()` is a complex operation that must
 *    be atomic with respect to other transfers and destruction. It uses
 *    `std::scoped_lock` to lock both guards, ensuring correctness and
 *    simplicity at the cost of being a blocking call.
 * 3. **ABI Stability (Pimpl Idiom)**: The implementation is hidden behind a
 *    `std::unique_ptr<AtomicGuardImpl>`. This ensures that changes to private
 *    members (e.g., adding new flags) do not alter the class's size or layout,
 *    maintaining a stable Application Binary Interface (ABI). This is critical
 *    for shared libraries, as it allows them to be updated without requiring
 *    consumers of the library to recompile.
 *
 * **Ownership (`transfer_to`) vs. C++ Object Lifecycle (`std::move`)**
 * It is crucial to distinguish between transferring lock ownership and moving the guard object:
 * - **`transfer_to(dest)`**: This is a *semantic* operation that moves the *lock ownership*
 *   from one guard (`this`) to another (`dest`). Both guard objects continue to exist
 *   as separate entities. The source guard becomes inactive, and the destination guard
 *   becomes active.
 * - **`std::move(source)`**: This is a *C++ language* feature that transfers the
 *   *internal resources* of the `source` guard object to a new guard object.
 *   The `source` is left in a valid but empty (moved-from) state; its
 *   destructor becomes a no-op, and calling any other methods on it is unsafe.
 *   If the source guard was active, the new guard will be active with the same
 *   token and ownership state. This is the standard mechanism for returning a
 *   guard from a factory function or moving it into a container.
 *
 * **Core Invariants**:
 * - `AtomicOwner::state_` is the single source of truth: `0` means free, while a
 *   non-zero value is the unique token of the owning `AtomicGuard`.
 * - Each `AtomicGuard` has a persistent, non-zero token that never changes.
 * - An internal `is_active_` flag tracks whether the guard *believes* it holds
 *   the lock. This flag is the source of truth for the destructor's behavior,
 *   ensuring it only attempts to release a lock it is responsible for.
 * - Copying is disabled. Moving is enabled to support modern C++ patterns.
 * - C++17 is required (`inline static`, `std::scoped_lock`, `[[nodiscard]]`).
 *
 * **Usage and Best Practices**:
 * 1.  **RAII-Style Guard**: The most common use is as a stack-based RAII object. The destructor automatically handles releasing the lock.
 *
 *     ```cpp
 *     AtomicOwner owner;
 *     {
 *         // Attempt to acquire on construction
 *         AtomicGuard guard(&owner, true);
 *         if (guard.active()) {
 *             // ... work with the guarded resource ...
 *         }
 *     } // guard's destructor is called, releasing the lock.
 *     ```
 *
 * 2.  **Explicit Ownership Transfer**: To pass lock ownership between two existing guards, use `transfer_to()`.
 *
 *     ```cpp
 *     AtomicGuard source_guard(&owner, true);
 *     AtomicGuard dest_guard(&owner);
 *     if (source_guard.transfer_to(dest_guard)) {
 *         // dest_guard is now active, source_guard is not.
 *     }
 *     ```
 *
 * 3.  **Moving a Guard**: To transfer the guard object itself (e.g., from a factory function), use `std::move`.
 *
 *     ```cpp
 *     AtomicGuard create_and_acquire_guard(AtomicOwner* owner) {
 *         AtomicGuard g(owner, true);
 *         return g; // Implicitly moved
 *     }
 *
 *     AtomicOwner owner;
 *     {
 *         AtomicGuard my_guard = create_and_acquire_guard(&owner);
 *         if (my_guard.active()) {
 *             // ... work with the guarded resource ...
 *         }
 *     } // my_guard's destructor releases the lock.
 *     ```
 *
 * 4.  **Check Operation Success**: Methods like `acquire()`, `release()`, and `transfer_to()` are marked with `[[nodiscard]]`. It is critical to check their return values. The robust destructor will call `PANIC()` (which defaults to `std::abort()`) if it detects an invariant violation, helping to catch logic errors early.
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

    // Destructor: performs a robust release. If the guard believes it is the
    // active owner but fails to release the lock, it will call PANIC() to
    // signal a critical invariant violation. It is a no-op for moved-from
    // guards.
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

    // Checks if this guard's token matches the value in the AtomicOwner.
    // This provides a point-in-time snapshot of the shared state and is thread-safe.
    // Note that this may differ from the guard's internal `is_active_` belief,
    // for instance, immediately after another guard has acquired the lock.
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
    // This method atomically transfers lock ownership from `this` guard to the `dest` guard.
    //
    // **Semantics**:
    //  - It acquires locks on both guards using `std::scoped_lock` to ensure the
    //    operation is atomic and deadlock-free with respect to other transfers.
    //  - It checks if `this` guard is the current owner of the resource.
    //  - If so, it performs a CAS on the `AtomicOwner` to switch the owner token
    //    from `this->token()` to `dest.token()`.
    //  - On success, it updates the internal `is_active_` flags of both guards.
    //
    // **Caller Responsibility & Thread Safety**:
    // The caller is responsible for ensuring that both `this` and `dest` guard
    // objects remain valid for the entire duration of this call. Calling this
    // method with a reference to a guard that is being concurrently destroyed
    // in another thread can lead to undefined behavior. The internal checks
    // against a `being_destructed_` flag are a best-effort mitigation but
    // cannot prevent all lifetime-related race conditions.
    //
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

} // namespace pylabhub::utils
