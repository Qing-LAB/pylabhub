# Utils: Fundamental Facilities — Reuse, Design, and Separation

This document summarizes how the fundamental concurrency and utility facilities in `src/utils` and `src/include/utils` are organized for **reuse**, **consistent design**, and **logical separation** of structure vs function and abstraction layers.

---

## 1. Single Sources (No Duplication)

| Concern | Single source | Used by |
|--------|----------------|---------|
| **Spin state layout** | `SharedSpinLockState` in `shared_memory_spinlock.hpp` | DataBlock header, InProcessSpinState, SharedSpinLock |
| **Spin state init** | `init_spinlock_state(SharedSpinLockState*)` in `shared_memory_spinlock.hpp` | DataBlock (header init, acquire/release slot), InProcessSpinState ctor |
| **Token-mode ops** | `detail/spinlock_owner_ops.hpp` (try_acquire_token, release_token, token_lock_held, next_token) | InProcessSpinState + SpinGuard only |
| **Backoff** | `backoff_strategy.hpp` (ExponentialBackoff, `backoff(iteration)`) | SharedSpinLock (.cpp), InProcessSpinState (header), data_block.cpp |

No duplicate definitions of state layout, init, or backoff. Token ops are internal (detail) and used only by the in-process guard.

---

## 2. Structure vs Function Separation

**Structure (data layout + init):**

- **SharedSpinLockState** — 32-byte canonical layout (owner_pid, owner_tid, generation, recursion_count). One definition.
- **init_spinlock_state()** — single function that zeros all four fields; used for both shared-memory and in-process state.

**Function (operations on state):**

- **Token mode:** `detail::try_acquire_token`, `release_token`, `token_lock_held`, `next_token` — operate on a `SharedSpinLockState*`; no allocation, no backoff. Identity (token) is always supplied by the caller.
- **Pid/tid mode:** Implemented in `shared_memory_spinlock.cpp` (SharedSpinLock::try_lock_for, lock, unlock). Uses same state layout; different protocol (pid/tid, recursion, zombie reclaim).

**Abstraction (state owner vs lock operator):**

- **State owner:** Holds the state. **InProcessSpinState** owns a `SharedSpinLockState` inline and exposes token-mode APIs (try_acquire_with_token, lock_with_token, unlock). Does not “lock” by itself.
- **Lock operator:** Performs acquire/release. **SpinGuard** (InProcessSpinStateGuard) holds a pointer to the state owner and a token; it performs the lock (ctor/try_lock) and unlock (dtor/release). **SharedSpinLock** is both the handle to state (pointer) and the lock (pid/tid protocol in .cpp).

So: **state layout + init** are shared; **protocol** is split (token in detail + in_process_spin_state; pid/tid in shared_memory_spinlock.cpp); **state owner** (InProcessSpinState) vs **lock operator** (SpinGuard / SharedSpinLock) are clearly separated.

---

## 3. Backoff and Timeout Conventions

**Backoff:** All spin loops that need backoff use `pylabhub::utils::ExponentialBackoff` or the convenience function `backoff(iteration)` from `backoff_strategy.hpp`. No ad-hoc sleep/yield elsewhere for spin loops.

**Timeout:**

- **In-process (header-only):** `InProcessSpinState::try_lock_for_with_token` uses `std::chrono::steady_clock` so the header does not depend on `plh_platform`. Acceptable for in-process-only use.
- **Cross-process / shared memory:** `SharedSpinLock::try_lock_for` and `data_block.cpp` use `pylabhub::platform::monotonic_time_ns()` and `elapsed_time_ns()` for consistency with the rest of the DataBlock and OS behavior.

So: one backoff module; two timeout sources by design (header-only vs platform), not by accident.

---

## 4. Guards and Namespaces

| Guard / facility | Namespace | Role | Header |
|------------------|-----------|------|--------|
| **RecursionGuard** | pylabhub::basics | Thread-local re-entry detection | recursion_guard.hpp |
| **ScopeGuard** | pylabhub::basics | Callable on scope exit | scope_guard.hpp |
| **SpinGuard** | pylabhub::hub | Token-mode lock holder (performs lock/unlock on InProcessSpinState) | in_process_spin_state.hpp |
| **SharedSpinLockGuard** | pylabhub::hub | RAII for SharedSpinLock (pid/tid) | shared_memory_spinlock.hpp |

All are RAII; each has a distinct purpose. Basics (recursion, scope) live in `pylabhub::basics`; hub (spin state, shared spinlock) in `pylabhub::hub`. `plh_base.hpp` pulls in both so user code can use any of them without caring about the namespace split.

---

## 5. When to Use Which Facility

| Need | Use |
|------|-----|
| In-process, single address space, handoff between threads | **InProcessSpinState** + **SpinGuard** (token mode). State owner + guard that performs locking. |
| Cross-process, state in shared memory (e.g. DataBlock header) | **SharedSpinLock** + **SharedSpinLockGuard** (pid/tid mode). State pointer + name; implementation in .cpp. |
| Init any spin state (shm or in-process) | **init_spinlock_state()** before first use. |
| Spin loop with backoff | **ExponentialBackoff** or **backoff(iteration)** from `backoff_strategy.hpp`. |
| Re-entry detection (per-thread) | **RecursionGuard** (basics). |
| Cleanup on scope exit | **ScopeGuard** / **make_scope_guard** (basics). |

---

## 6. File Layout Summary

- **shared_memory_spinlock.hpp** — Structure (SharedSpinLockState), init (init_spinlock_state), and declarations for SharedSpinLock and its guards. Implementation in **shared_memory_spinlock.cpp**.
- **detail/spinlock_owner_ops.hpp** — Token-mode operations only; no state ownership, no backoff. Included only by **in_process_spin_state.hpp**.
- **in_process_spin_state.hpp** — State owner (InProcessSpinState) and lock operator (SpinGuard); uses detail token ops and **backoff_strategy.hpp** for spin loops. Header-only.
- **backoff_strategy.hpp** — Single backoff module; used by SharedSpinLock, InProcessSpinState, and data_block.cpp.

This keeps **reuse** (one state, one init, one backoff), **consistent design** (state owner vs lock operator, token vs pid/tid), and **logical separation** (structure in one place, token ops in detail, backoff in utils, guards in basics/hub).
