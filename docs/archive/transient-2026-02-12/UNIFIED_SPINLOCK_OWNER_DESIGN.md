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

## 2. Identity from Factory; Spinlock Only Manipulates State

**Owner** = the memory (the 32-byte state). It is the “section” that records who holds the lock.

**Holder identity** = (PID, TID, token). This is **not** generated inside the spinlock. It is supplied by the **factory** (e.g. when creating a guard or a “holder” object):

- **Token mode:** Factory produces (0, 0, unique_token). The guard (or holder) is created with that identity; the spinlock is only given (state, identity) and updates the state.
- **Pid/tid mode:** Factory produces (my_pid, my_tid, generation_or_token) from the current process/context. Again the spinlock only receives (state, identity) and manipulates the state.

So: **PID, TID, and token are provided at the factory.** The spinlock **only manipulates the state** (CAS, store) using the identity it is given. It never generates PID/TID/token.

When we **pass the guard around** (like AtomicGuard handoff), we are passing the **holder identity** (and a reference to the lock/state). The owner (memory) can stay in one place. We only need (PID, TID, token) to be **unique per logical holder** so that (1) the state can record “this identity holds it” and (2) only that holder can release (CAS identity → 0).

---

## 3. Single Spinlock Abstraction

**One** spinlock type that:

- Takes a pointer to the **unified owner** (32-byte state) and optional name.
- Implements **one** spin loop (backoff, timeout) that operates on that state using a **caller-supplied identity** (from the factory).
- Uses a **mode** or **policy** (chosen at construction or by convention) to decide:
  - **Token mode:** Acquire = CAS(token field, 0 → identity.token); release = CAS(token field, identity.token → 0). pid/tid in identity are 0; handoff allowed.
  - **Pid/tid mode:** Acquire = CAS(pid, 0 → identity.pid), set tid from identity, handle recursion and zombie reclaim; release = clear pid/tid, bump token (generation). Identity comes from current process; no handoff.

Implementation can be:

- **Enum + single code path:** Spinlock stores `OwnerMode { Token, PidTid }` and a pointer to the 32-byte state. `try_lock_for` / `unlock` do a switch on mode and run the appropriate CAS/checks. One spin loop, one unlock path; a few branches on mode.
- **Template policy (internal only):** Internal implementation is a template on policy (TokenPolicy vs PidTidPolicy); the public API still exposes a single non-template `Spinlock` or keeps the name `SharedSpinLock` and uses the shared-memory policy. No change to public ABI.

So we **do not** maintain two different spinlock implementations (one for token, one for shared). We have one implementation that interprets the same 32-byte layout in two ways.

---

## 4. Single Guard Abstraction

**One** RAII guard type (or one family with the same interface):

- Holds a reference (or pointer) to the spinlock and the information needed to release (token in token mode, or implicit pid/tid in pid/tid mode).
- **Token mode:** Guard stores the token; it is **movable** so the guard can be handed off to another thread.
- **Pid/tid mode:** Guard does not store a transferable token; it is **non-movable** (or move = delete); only the locking thread may unlock.

So a single guard type can support both behaviors: when the spinlock is in token mode, the guard is movable and holds the token; when in pid/tid mode, the guard is non-movable. Fewer types, fewer branches in the rest of the codebase.

---

## 5. Simplified Memory Model

- **Every** lock uses the same 32-byte owner state. No “tiny” one-word owner for in-process only.
- **Where it lives:** In DataBlock it lives in `SharedMemoryHeader::spinlock_states[]`. For in-process-only use (e.g. SpinGuard), we allocate one 32-byte state on the stack or heap and pass a pointer to the spinlock. No second layout, no second owner type.
- **Initialization:** Both use the same factory code: `init_spinlock_state(SharedSpinLockState*)` in `shared_memory_spinlock.hpp` zeros all four fields. DataBlock calls it when initializing the header and when releasing/claiming a spinlock slot; `InProcessSpinState` ctor calls it for its inline state.
- **Unified access:** All code that acquires/releases goes through the same spinlock API and the same owner layout. Less branching (“if token lock do X else do Y” at the call site) and fewer places for mistakes.

---

## 6. User API: How to Get the Lock and the Holder

**Lock:** `InProcessSpinlock`. Create one with the factory: `make_in_process_spinlock()` (no move; the lock is constructed in place by RVO).

**Holder:** The holder is the **guard** object. The public class to use is **`SpinGuard`** (type alias for `InProcessSpinlockGuard`). You do not have a separate “holder” type—the guard *is* the holder: it carries the token and releases on destruction.

- **Get a holder (blocking):** Construct the guard with the lock: `SpinGuard g(lock);` — this acquires the lock and `g` is the holder until it is destroyed or you call `g.release()` or move it away.
- **Get a holder (try / timeout):** Default-construct a guard, then `g.try_lock(lock, timeout_ms)` — if it returns `true`, `g` is the holder.

**Example:**

```cpp
#include "utils/in_process_spinlock.hpp"

auto state = pylabhub::hub::make_in_process_spin_state();

// Blocking: construct guard → guard performs lock; you are the holder until guard goes out of scope
{
    pylabhub::hub::SpinGuard g(state);
    // ... critical section ...
}   // g destroyed → lock released

// Try with timeout: get holder only if acquired
pylabhub::hub::SpinGuard g;
if (g.try_lock(state, 100)) {
    // ... critical section ...
    g.release();   // or let destructor release
}

// Handoff to another thread
std::promise<pylabhub::hub::SpinGuard> p;
std::thread t1([&] {
    pylabhub::hub::SpinGuard g(state);
    p.set_value(std::move(g));
});
std::thread t2([&] {
    pylabhub::hub::SpinGuard g = p.get_future().get();  // now t2 is the holder
    g.release();
});
t1.join(); t2.join();
```

**Thread safety (lock used with SpinGuard):**

- **State (InProcessSpinState):** Thread-safe and cross-thread-safe. Multiple threads may contend on the same state; mutual exclusion is enforced. The guard performs the actual lock/unlock.
- **Guard (SpinGuard):** Not safe for concurrent use on the same guard instance. One guard represents one logical holder; do not call methods on the same guard from more than one thread without external synchronization. **Handoff is safe:** moving the guard to another thread (e.g. `std::promise<SpinGuard>`) transfers ownership so that only the destination thread uses that guard.

---

## 7. C++ API: Move/Copy, Exceptions, and ABI

### State owner (`InProcessSpinState`)

- **Copy:** Deleted. The state owner is a unique resource.
- **Move:** Deleted. It holds `SharedSpinLockState` (atomics are not moveable). Use the factory so the state is constructed in place (e.g. `auto state = make_in_process_spin_state();` with RVO).
- **ABI:** Header-only. For stability, do not change the order or types of members. Adding new methods is safe.

### Guard (`SpinGuard` / `InProcessSpinStateGuard`) — performs locking

- **Copy:** Deleted. The guard represents unique ownership of the lock; copying would allow two “owners” to release.
- **Move:** Allowed. Supports handoff between threads (e.g. `std::promise<InProcessSpinStateGuard>`). After move, the source guard is inactive and must not be used to release.
- **RAII:** Destructor releases (guard performs unlock) if the guard still holds. All operations are `noexcept`.

### Exception policy

- **InProcessSpinState / InProcessSpinStateGuard:** All public APIs are `noexcept`. No exceptions are thrown. This matches **AtomicGuard**, which is also fully `noexcept`. Use `try_lock(lock, timeout_ms)` for non-blocking or time-bounded acquisition; the blocking constructor is “lock or spin forever.”
- **SharedSpinLock** (cross-process): `unlock()` may throw `std::runtime_error` if a non-owner tries to release. That is intentional for cross-process safety. In-process token lock does not need that check in the same way (token is proof of ownership).

### C-API

- No C API is exposed for the in-process spinlock. C++20 public API only; C API only when necessary (e.g. shared-memory lock if needed from C).

---

## 8. Comparison with Former AtomicGuard (Removed)

AtomicGuard and AtomicOwner were removed; InProcessSpinState + SpinGuard provide equivalent behavior. **Test coverage parity:** `tests/test_layer1_base/test_spinlock.cpp` (suite `InProcessSpinStateTest`) exercises the same scenarios the old AtomicGuard tests did: basic acquire/release, RAII, move/handoff, detach-then-reuse, self-move, concurrent stress, and transfer between threads.

| Aspect | AtomicGuard (removed) | SpinGuard (InProcessSpinStateGuard) |
|--------|-------------|------------------------|
| Copy | Deleted | Deleted |
| Move | Allowed (handoff) | Allowed (handoff) |
| Exceptions | All `noexcept` | All `noexcept` |
| Blocking acquire | `attach_and_acquire(owner)` or ctor(owner, true) | Ctor `InProcessSpinlockGuard(lock)` |
| Try / timeout | `attach(owner); acquire()` (no timeout) | `try_lock(lock, timeout_ms)` |
| Release | `release()` (keeps owner/token; can re-acquire) | `release()` (clears token; next acquire gets new token) |
| Detach | `detach()` (owner=nullptr; leak if holding) | `detach()` (lock_=nullptr, token_=0) |
| “Do I hold?” | `active()` (queries owner state) | `holds_lock()` (local state; token_ != 0) |
| Token | Generated once per guard, kept after release | Generated once per guard, cleared on release |

Behavioral difference: After `release()`, AtomicGuard keeps its token so a subsequent `acquire()` re-uses the same token. InProcessSpinlockGuard clears the token on `release()` so the next `try_lock()` acquires with a new token. Both are consistent; the unified guard prefers “one logical hold per token” and avoids reusing a token across release/acquire cycles.

---

## 9. Migration Path (No ABI Break)

1. **Keep current public types and layout:** `SharedSpinLockState`, `SharedSpinLock`, `SharedSpinLockGuard`, `SharedSpinLockGuardOwning` remain as-is (same names, same 32-byte layout, same public API). Document that `SharedSpinLockState` is the **unified owner** (pid, tid, token, recursion_count).
2. **Refactor spinlock implementation internally:** One spin loop and one unlock path; add an internal “mode” (token vs pid/tid) or use an internal template so both behaviors share code. Public `SharedSpinLock` continues to use pid/tid mode only; no change to existing callers.
3. **Introduce token-mode use of the same layout:** For in-process token semantics, create a spinlock (or reuse the same class with a mode parameter) that operates on a 32-byte state (stack or heap) in token mode. No new owner type: same 32-byte struct, pid=0, tid=0, token=holder.
4. **AtomicOwner/AtomicGuard removed:** Replaced by InProcessSpinState + SpinGuard (unified 32-byte state, token mode, same guard handoff semantics). See **test_spinlock.cpp** for equivalent test coverage.

Result: one owner layout, one spinlock abstraction, one guard abstraction; fewer code branches and a simpler mental model, without breaking the existing DataBlock/ABI surface.
