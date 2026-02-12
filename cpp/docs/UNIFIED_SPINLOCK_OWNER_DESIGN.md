# Unified Spinlock and Owner Design

## Principle: One Owner Layout, One Abstraction

We use **one** owner memory layout and **one** spinlock/guard abstraction for all use cases (in-process token handoff, cross-process DataBlock, future hybrid). A bit of extra memory (one fixed 32-byte state everywhere) is acceptable; simplifying the memory model and unifying access reduces errors and code branches.

---

## 1. Single Owner Layout (32 bytes)

**One struct** for all owner state. No separate “token-only” or “minimal” layout.

| Field | Type | Role |
|-------|------|------|
| **pid** | `std::atomic<uint64_t>` | Process identifier; 0 when not used (e.g. token-only mode). |
| **tid** | `std::atomic<uint64_t>` | Thread identifier; 0 when not used. |
| **token** | `std::atomic<uint64_t>` | Extra info: holder token (token mode), or generation (pid/tid mode), or packed metadata. |
| **recursion_count** | `std::atomic<uint32_t>` | Recursion depth for same-thread re-entry (pid/tid mode); 0 in token mode. |
| *padding* | 4 bytes | Keep total size 32 bytes, cache-friendly. |

- **In-process, token semantics:** pid=0, tid=0; **token** = holder token (whoever has the token can release; handoff). recursion_count=0.
- **Cross-process, pid/tid semantics:** **pid** and **tid** = owner identity; only that (pid,tid) may unlock; **token** = generation (bump on release); **recursion_count** = same-thread re-entry.

Same layout in shared memory (DataBlock header) and in process-local memory (stack/heap). One definition, one access pattern, one spinlock that operates on a pointer to this struct.

**ABI note:** The current `SharedSpinLockState` (owner_pid, owner_tid, generation, recursion_count, padding) already matches this layout. We treat it as the canonical “unified owner”; optionally we introduce a type alias or document the field roles as pid, tid, token, recursion_count without changing member names to preserve ABI.

---

## 2. Single Spinlock Abstraction

**One** spinlock type that:

- Takes a pointer to the **unified owner** (32-byte state) and optional name.
- Implements **one** spin loop (backoff, timeout) that operates on that state.
- Uses a **mode** or **policy** (chosen at construction or by convention) to decide:
  - **Token mode:** Acquire = CAS(token, 0 → my_token); release = CAS(token, my_token → 0). pid/tid ignored for ownership (handoff allowed).
  - **Pid/tid mode:** Acquire = CAS(pid, 0 → my_pid), set tid, handle recursion and zombie reclaim; release = clear pid/tid, bump token (generation). No handoff.

Implementation can be:

- **Enum + single code path:** Spinlock stores `OwnerMode { Token, PidTid }` and a pointer to the 32-byte state. `try_lock_for` / `unlock` do a switch on mode and run the appropriate CAS/checks. One spin loop, one unlock path; a few branches on mode.
- **Template policy (internal only):** Internal implementation is a template on policy (TokenPolicy vs PidTidPolicy); the public API still exposes a single non-template `Spinlock` or keeps the name `SharedSpinLock` and uses the shared-memory policy. No change to public ABI.

So we **do not** maintain two different spinlock implementations (one for token, one for shared). We have one implementation that interprets the same 32-byte layout in two ways.

---

## 3. Single Guard Abstraction

**One** RAII guard type (or one family with the same interface):

- Holds a reference (or pointer) to the spinlock and the information needed to release (token in token mode, or implicit pid/tid in pid/tid mode).
- **Token mode:** Guard stores the token; it is **movable** so the guard can be handed off to another thread.
- **Pid/tid mode:** Guard does not store a transferable token; it is **non-movable** (or move = delete); only the locking thread may unlock.

So a single guard type can support both behaviors: when the spinlock is in token mode, the guard is movable and holds the token; when in pid/tid mode, the guard is non-movable. Fewer types, fewer branches in the rest of the codebase.

---

## 4. Simplified Memory Model

- **Every** lock uses the same 32-byte owner state. No “tiny” one-word owner for in-process only.
- **Where it lives:** In DataBlock it lives in `SharedMemoryHeader::spinlock_states[]`. For in-process-only use (e.g. current AtomicGuard use cases), we allocate one 32-byte state on the stack or heap and pass a pointer to the spinlock. No second layout, no second owner type.
- **Unified access:** All code that acquires/releases goes through the same spinlock API and the same owner layout. Less branching (“if token lock do X else do Y” at the call site) and fewer places for mistakes.

---

## 5. Migration Path (No ABI Break)

1. **Keep current public types and layout:** `SharedSpinLockState`, `SharedSpinLock`, `SharedSpinLockGuard`, `SharedSpinLockGuardOwning` remain as-is (same names, same 32-byte layout, same public API). Document that `SharedSpinLockState` is the **unified owner** (pid, tid, token, recursion_count).
2. **Refactor spinlock implementation internally:** One spin loop and one unlock path; add an internal “mode” (token vs pid/tid) or use an internal template so both behaviors share code. Public `SharedSpinLock` continues to use pid/tid mode only; no change to existing callers.
3. **Introduce token-mode use of the same layout:** For in-process token semantics, create a spinlock (or reuse the same class with a mode parameter) that operates on a 32-byte state (stack or heap) in token mode. No new owner type: same 32-byte struct, pid=0, tid=0, token=holder.
4. **Replace AtomicOwner/AtomicGuard over time:** Where we currently use AtomicOwner (one word) + AtomicGuard, migrate to the unified owner (one 32-byte state) and the unified spinlock in token mode, with a single guard type that supports handoff. Deprecate the old one-word owner and the second abstraction.

Result: one owner layout, one spinlock abstraction, one guard abstraction; fewer code branches and a simpler mental model, without breaking the existing DataBlock/ABI surface.
