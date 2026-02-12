# Spinlock Header and Source Layout

This document describes how the spinlock-related headers and source files are organized, their dependencies, and the rationale so the logic stays clear and maintainable. For reuse of backoff, state init, and guards across utils, see **docs/UTILS_FUNDAMENTAL_FACILITIES.md**.

---

## 1. Overview: Three Components

| Component | Path | Type | Role |
|-----------|------|------|------|
| **Shared memory spinlock** | `utils/shared_memory_spinlock.hpp` + `.cpp` | Public API + implementation | Cross-process lock: **layout** (state struct + init) and **pid/tid-mode** lock/guard. |
| **Detail owner ops** | `utils/detail/spinlock_owner_ops.hpp` | Header-only, **not** public API | **Token-mode** operations on the same 32-byte state (CAS on `generation` only). |
| **In-process spin state** | `utils/in_process_spin_state.hpp` | Header-only | **State owner** (InProcessSpinState) + **guard** (SpinGuard) that performs locking; token mode, same state layout. |

**Unifying idea:** One 32-byte state layout (`SharedSpinLockState`) and one init function (`init_spinlock_state`) for both “in-process token” and “cross-process pid/tid” usage. The **state** and **init** live in the shared-memory header; the **protocol** (how we acquire/release) differs: token ops in `detail`, pid/tid in `shared_memory_spinlock.cpp`.

---

## 2. Dependency Graph

```
                    ┌─────────────────────────────────────┐
                    │  shared_memory_spinlock.hpp         │
                    │  ─────────────────────────────────│
                    │  • SharedSpinLockState (32 bytes)  │
                    │  • init_spinlock_state()           │
                    │  • SharedSpinLock (declaration)    │
                    │  • SharedSpinLockGuard* (decl)     │
                    └──────────────┬─────────────────────┘
                                   │
           ┌───────────────────────┼───────────────────────┐
           │                       │                       │
           ▼                       ▼                       │
┌──────────────────────┐  ┌─────────────────────────────┐  │
│ detail/              │  │ shared_memory_spinlock.cpp  │  │
│ spinlock_owner_ops   │  │ ───────────────────────────  │  │
│ .hpp                 │  │ SharedSpinLock (pid/tid      │  │
│ ───────────────────  │  │ implementation): lock(),     │  │
│ Token-mode ops only: │  │ try_lock_for(), unlock(),   │  │
│ try_acquire_token(), │  │ SharedSpinLockGuard*,       │  │
│ release_token(),     │  │ SharedSpinLockGuardOwning   │  │
│ token_lock_held(),   │  │ (uses platform, logger,     │  │
│ next_token()         │  │  backoff from plh_service)  │  │
│ includes:            │  └─────────────────────────────┘  │
│  shared_memory_      │                                    │
│  spinlock.hpp        │                                    │
└──────────┬───────────┘                                    │
           │                                                 │
           ▼                                                 │
┌──────────────────────┐                                     │
│ in_process_spin_      │◄────────────────────────────────────┘
│ state.hpp             │  (also includes shared_memory_spinlock.hpp
│ ───────────────────  │   for SharedSpinLockState + init)
│ InProcessSpinState,  │
│ InProcessSpinState   │
│ Guard, SpinGuard,    │
│ make_in_process_     │
│ spin_state()         │
│ includes:            │
│  detail/spinlock_    │
│  owner_ops.hpp       │
│  shared_memory_      │
│  spinlock.hpp        │
│  backoff_strategy.hpp│
└──────────────────────┘
```

- **shared_memory_spinlock.hpp** is the **foundation**: it defines the state layout and init. No spinlock logic in the header beyond declarations.
- **detail/spinlock_owner_ops.hpp** depends **only** on `shared_memory_spinlock.hpp`; it implements token-mode acquire/release/query and token generation. It is **not** included by the rest of the project except by `in_process_spin_state.hpp`.
- **in_process_spin_state.hpp** is the **in-process public API** for token mode: state owner (InProcessSpinState) + guard (SpinGuard) that performs locking; uses detail token ops and shared state/init.
- **shared_memory_spinlock.cpp** implements the **cross-process** (pid/tid) lock; it does **not** use the detail token ops or in_process_spin_state.

---

## 3. File-by-File Responsibilities

### 3.1 `utils/shared_memory_spinlock.hpp`

- **Defines (canonical):**
  - `SharedSpinLockState` — the single 32-byte owner layout (owner_pid, owner_tid, generation, recursion_count, padding). Used in DataBlock header and by both in-process and cross-process locks.
  - `init_spinlock_state(SharedSpinLockState*)` — single place that sets “free” state (all four fields zero). Used by DataBlock and by `InProcessSpinState` ctor.
- **Declares (implementation in .cpp):**
  - `SharedSpinLock` — cross-process spinlock (takes pointer to state + name).
  - `SharedSpinLockGuard`, `SharedSpinLockGuardOwning` — RAII for SharedSpinLock.
- **Dependencies:** `pylabhub_utils_export.h`, `plh_platform.hpp`, standard headers. No other utils spinlock headers.
- **Exported:** Yes (utils library). Header is public; state + init are shared by in-process code.

### 3.2 `utils/shared_memory_spinlock.cpp`

- **Implements:** `SharedSpinLock` (ctor, lock, try_lock_for, unlock, is_locked_by_current_process) and guard types. Uses **pid/tid** and recursion on the same `SharedSpinLockState`; does **not** use token mode or `detail/spinlock_owner_ops.hpp`.
- **Dependencies:** `shared_memory_spinlock.hpp`, `plh_service.hpp` (platform, logger, backoff). No in_process_spinlock or detail.

### 3.3 `utils/detail/spinlock_owner_ops.hpp`

- **Defines (all inline):**
  - Token-mode only: `try_acquire_token()`, `release_token()`, `token_lock_held()`, `next_token()`.
  - Operates on `SharedSpinLockState*`; in token mode only the `generation` field is used (as token); pid/tid/recursion_count stay 0.
- **Design:** Spinlock “only manipulates the state”; identity (token) is supplied by the caller/factory (e.g. guard). Not part of public API; not exported from the utils DLL.
- **Dependencies:** Only `../shared_memory_spinlock.hpp` (for `SharedSpinLockState`). No backoff, no in_process_spinlock.

### 3.4 `utils/in_process_spin_state.hpp`

- **Defines (all inline / header-only):**
  - `InProcessSpinState` — state owner; holds `SharedSpinLockState` inline; ctor calls `init_spinlock_state(&state_)`. Does not perform locking; the guard does. Offers `try_acquire_with_token`, `lock_with_token`, `try_lock_for_with_token`, `unlock(token)`, `is_locked()`.
  - `InProcessSpinStateGuard` — RAII guard (move-only, handoff) that **performs** locking; generates token once per guard; uses detail token ops.
  - `SpinGuard` — type alias for the guard (user-facing name).
  - `make_in_process_spin_state()` — factory.
- **Dependencies:** `utils/detail/spinlock_owner_ops.hpp`, `utils/shared_memory_spinlock.hpp`, `utils/backoff_strategy.hpp`. Does **not** depend on shared_memory_spinlock.cpp or plh_service.
- **Exported:** No (header-only). Used by `plh_base.hpp` and tests.

---

## 4. Why This Layout

- **Single state layout and init:** All spinlock state (DataBlock shm, in-process lock) uses `SharedSpinLockState` and `init_spinlock_state()`. No duplicate definitions or init logic.
- **Clear split by protocol:**  
  - **Token mode** (in-process, handoff): logic in `detail/spinlock_owner_ops.hpp` + `in_process_spin_state.hpp`; no .cpp, no ABI surface.  
  - **Pid/tid mode** (cross-process): logic in `shared_memory_spinlock.cpp`; can use platform/logger/backoff and stay ABI-stable.
- **Detail stays internal:** Only `in_process_spin_state.hpp` includes `detail/spinlock_owner_ops.hpp`. Public API is `shared_memory_spinlock.hpp` (state + SharedSpinLock) and `in_process_spin_state.hpp` (InProcessSpinState + SpinGuard).
- **No circular deps:** Flow is one-way: shared_memory_spinlock.hpp → detail/spinlock_owner_ops.hpp → in_process_spin_state.hpp; and shared_memory_spinlock.hpp → shared_memory_spinlock.cpp.

---

## 5. What Depends on What (Summary)

| Consumer | Uses |
|----------|------|
| **DataBlock** | `shared_memory_spinlock.hpp` (SharedSpinLockState, init_spinlock_state); gets state pointer via `get_shared_spinlock_state()` and uses SharedSpinLock(state, name) in code paths that need cross-process locking. |
| **plh_base.hpp** | `in_process_spin_state.hpp` (InProcessSpinState, SpinGuard, make_in_process_spin_state) for in-process state + guard. |
| **Tests (spin state)** | `in_process_spin_state.hpp` and/or `shared_memory_spinlock.hpp` as needed. |
| **Tests (SharedSpinLock)** | `shared_memory_spinlock.hpp`; init state with `init_spinlock_state()`. |

Nothing outside `in_process_spin_state.hpp` should include `detail/spinlock_owner_ops.hpp`.

---

## 6. Possible Future Refactors (Optional)

- **Shared spin loop / backoff:** `shared_memory_spinlock.cpp` could call into a shared (e.g. template or detail) spin/backoff helper so the same backoff and timeout logic is used for both pid/tid and token mode. Today, token mode’s spin loop lives in `in_process_spin_state.hpp` and pid/tid in `.cpp`; unifying would be a logic/organization improvement without changing public API or state layout.
- **Explicit “policy” in one header:** All spinlock-related types could be listed in one place (e.g. a small “spinlock overview” comment in `shared_memory_spinlock.hpp`) with pointers to in-process vs cross-process and to this layout doc.

This layout keeps the relation between in_process_spin_state, shared_memory_spinlock, and spinlock_owner_ops clear and keeps dependencies and init logic consistent. For a broader view of reuse, backoff, and guards, see **docs/UTILS_FUNDAMENTAL_FACILITIES.md**.
