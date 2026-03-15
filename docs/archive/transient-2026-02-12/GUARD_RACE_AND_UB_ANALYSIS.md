# Guard Classes: Race Conditions and Corner Cases (UB Analysis)

This document summarizes the analysis of all guard types in the codebase for race conditions, constructor/destructor ordering, move semantics, and potential undefined behavior.

---

## 1. InProcessSpinStateGuard (SpinGuard)

**Header:** `utils/in_process_spin_state.hpp`

### Thread model
- **State (InProcessSpinState):** Thread-safe; multiple threads may contend; mutual exclusion is enforced.
- **Guard instance:** Not thread-safe; one guard must not be used concurrently by multiple threads. Handoff (move to another thread) is safe: move in one thread, then use the result only in the other thread.

### Constructor
- **Default:** `state_ = nullptr`, `token_ = 0`. No lock, no UB.
- **Blocking:** `state_(&s)`, `token_(next_token())`, then `s.lock_with_token(token_)`. All `noexcept`. If the process is killed during the spin, the object is fully constructed (state_ and token_ set) but the lock might not be held. On destruction, `release_token(state, token)` does CAS(expected=token, 0). If we never acquired, generation is still 0, so CAS fails and we do not release. No double-release.

### Destructor
- `if (state_ && token_ != 0) { state_->unlock(token_); token_ = 0; }`. All `noexcept`; `unlock`/`release_token` do not throw.
- **Lifetime:** If `state_` points to an `InProcessSpinState` that has already been destroyed, this is use-after-free (caller’s responsibility to keep state alive).

### Move constructor
- Copies `state_` and `token_` from `other`, then sets `other.state_ = nullptr`, `other.token_ = 0`. Moved-from guard’s dtor does nothing. No concurrent access on the same instance (per doc).

### Move assignment
- `if (this != &other)`: release current lock, take `other`’s state/token, null out `other`. Self-move is explicitly handled (no-op). No double-release.

### try_lock
- If currently holding, unlock then set `state_ = &s`. If `try_lock_for_with_token` fails, set `state_ = nullptr` and return; `token_` is left unchanged (reused on next try_lock). No acquire on failure, so no UB.

### release() / detach()
- **release():** Unlock and set `token_ = 0`; `state_` unchanged. Second call returns false, no double-unlock. Dtor sees `token_ == 0` and does nothing.
- **detach():** Set `state_ = nullptr`, `token_ = 0` without unlocking (intentional “lock leak”). Dtor does nothing.

### Avoiding the “state destroyed before guard” problem
The guard holds a raw pointer to `InProcessSpinState`. If the state is destroyed while a guard still points to it, the guard’s destructor would call `state_->unlock()` and cause use-after-free. To avoid that:

1. **Scope discipline:** Keep the state at broader scope than any guard (e.g. state as a class or static member, guards only in function/local scope so they are always destroyed before the state).
2. **Shared ownership:** If the state’s lifetime is dynamic, keep it in a `std::shared_ptr<InProcessSpinState>`. The guard cannot hold that shared_ptr without an API change, but the *owner* of the state can; ensure no guard outlives that owner (e.g. guards are not stored in long-lived containers that outlive the state).
3. **Document and review:** When storing guards (e.g. in a member or container), ensure the state’s lifetime is at least as long.

There is no in-process “validity” check for a raw pointer in C++; the only fully safe approach without changing the API is lifetime discipline above. A debug-only “guard count” in the state (assert zero in state’s dtor) could catch misuse in development but would require an ABI/layout change.

### Summary
- No UB from races or constructor/destructor ordering when used as documented (single-thread use per guard instance, handoff by move). Lifetime of `InProcessSpinState` must exceed any guard that references it.

---

## 2. RecursionGuard

**Header:** `utils/recursion_guard.hpp` (namespace `pylabhub::basics`)

### Thread model
- Thread-local, fixed-capacity stack; one guard instance is not shared across threads. No races on the guard object itself if used in a single thread.

### Constructor
- Pushes `key` onto thread-local `get_recursion_stack()` (fixed-capacity, no allocation). If current depth would exceed `kMaxRecursionDepth` (64), panics (abort) and does not return. Otherwise does not throw. No heap allocation, so no `std::bad_alloc` from the guard.

### Destructor
- `noexcept`. If `key_ == nullptr` (moved-from), no-op. Otherwise removes key (LIFO pop or search-and-shift for non-LIFO). No throw.

### Move constructor
- `key_(other.key_)`, then `other.key_ = nullptr`. Only one guard “owns” the key on the stack; the new guard will pop it. Moved-from guard’s dtor is a no-op. Safe.

### Move assignment
- Deleted; no analysis needed.

### Summary
- Exception-safe (strong guarantee of vector on push_back for this element type). No UB from move or destruction order when used per-thread.

---

## 3. ScopeGuard

**Header:** `utils/scope_guard.hpp` (namespace `pylabhub::basics`)

### Thread model
- Not thread-safe; documented. Concurrent access to the same instance is UB.

### Constructor
- Stores callable; can throw if move/copy of callable throws. On throw, object is not constructed, dtor does not run. Safe.

### Destructor
- `noexcept`. If `m_active`, invokes callable in try/catch and swallows exceptions. No throw from dtor. Double execution is avoided because `invoke()`/`invoke_and_rethrow()` set `m_active = false` before invoking.

### Move constructor
- Moves callable and `m_active` from `other`, then `other.dismiss()`. Moved-from guard will not run in its dtor. Safe.

### Move assignment
- Deleted.

### Dangling reference
- Documented: if the callable captures references that go out of scope before the guard runs, behavior is undefined. Caller’s responsibility.

### Summary
- No UB from ctor/dtor/move when used single-threaded and with valid captures. Only documented UB is dangling references in the callable.

---

## 4. SharedSpinLockGuard

**Header:** `utils/shared_memory_spinlock.hpp`  
**Implementation:** `utils/shared_memory_spinlock.cpp`

### Constructor
- Initializes `m_lock(lock)` then calls `m_lock.lock()`. `lock()` can throw (`std::runtime_error` if “Indefinite lock failed” from `try_lock_for(0)` returning false). If `lock()` throws, the guard’s constructor does not complete, so the guard object is not considered constructed and **its destructor is not run** (C++ [class.base.init]/15). So we never call `unlock()` when we did not acquire. No double-unlock or non-owner unlock from this path.

### Destructor
- Calls `m_lock.unlock()`. `unlock()` can throw `std::runtime_error` if the caller is not the owner. In normal use we only run the dtor when we fully constructed (and thus acquired), so we are the owner and `unlock()` does not throw. If misuse or corruption led to dtor running when not owner, `unlock()` would throw and could lead to `std::terminate()` during stack unwinding. Design choice: document that dtor assumes “we hold the lock” and that throwing from dtor is avoided by the implementation when owner; optional hardening would be to catch in dtor and log instead of letting throw (at the cost of masking bugs).

### Copy/Move
- Deleted; no races or double-release from copy/move.

### Summary
- No UB from ctor throwing (dtor not run). Dtor can throw only in aberrant “non-owner” cases; normal use is safe.

---

## 5. SharedSpinLockGuardOwning

**Header:** `utils/shared_memory_spinlock.hpp`  
**Implementation:** `utils/shared_memory_spinlock.cpp`

- Members: `SharedSpinLock m_lock; SharedSpinLockGuard m_guard;`. Construction order: `m_lock(state, name)` then `m_guard(m_lock)`. If `m_lock` ctor throws (e.g. null state), `m_guard` is not constructed; no dtor to run. If `m_guard` ctor throws (i.e. `lock()` throws), `m_guard` is not constructed; when the `SharedSpinLockGuardOwning` ctor is exited by exception, only `m_lock` is destroyed (no unlock). So lock state is not left “held” by us. Safe.

---

## 6. Token / state operations (detail)

**Header:** `utils/detail/spinlock_owner_ops.hpp`

- `try_acquire_token`: CAS(0, token) with acq_rel. No throw.
- `release_token`: CAS(token, 0) with acq_rel. No throw. If we never held (e.g. token mismatch), CAS fails and we do nothing; no UB.
- `next_token`: `static atomic` fetch_add; skip 0. Thread-safe, no throw.

---

## Summary Table

| Guard                     | Ctor throw safe | Dtor throw | Move safe | Concurrent same instance | Notes |
|---------------------------|-----------------|------------|-----------|---------------------------|--------|
| InProcessSpinStateGuard   | N/A (noexcept)  | No         | Yes       | No (handoff only)         | State must outlive guard. |
| RecursionGuard            | N/A (no throw)  | No         | Yes       | No (per-thread)           | Fixed capacity; panic if depth > kMaxRecursionDepth. |
| ScopeGuard                | Yes             | No         | Yes       | No                        | Dangling capture = UB (doc’d). |
| SharedSpinLockGuard       | Yes (dtor not run) | Can (non-owner only) | N/A   | N/A                       | Normal use: dtor does not throw. |
| SharedSpinLockGuardOwning | Yes             | No         | N/A       | N/A                       | Subobject dtors in reverse order. |

**Conclusion:** With documented usage (lifetimes, single-thread use per guard instance, handoff by move only for SpinGuard), there are no identified races or UB in constructor/destructor/move semantics. The only subtlety is `SharedSpinLockGuard::~SharedSpinLockGuard()` calling `unlock()`, which can throw only when the current thread is not the owner; that does not occur when the guard is used normally (acquire in ctor, release in dtor).
