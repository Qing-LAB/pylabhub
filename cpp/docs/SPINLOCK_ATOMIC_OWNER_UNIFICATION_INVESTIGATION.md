# Spinlock / AtomicOwner Unification: Runtime Cost and DataBlock Integration

**See also:** [UNIFIED_SPINLOCK_OWNER_DESIGN.md](UNIFIED_SPINLOCK_OWNER_DESIGN.md) — single owner layout (32 bytes), single spinlock and guard abstraction; no second kind of owner.

## Goal

Unify the "atomic owner" (provided memory + protocol) and spinlock (operates on it with backoff/timeout) so **one** abstraction and **one** owner layout serve both token-based (in-process) and shared-memory (DataBlock) cases, without increasing runtime cost or breaking the DataBlock memory model.

## 1. DataBlock Memory Model Constraints

- **SharedMemoryHeader** is a fixed 4KB layout in **shared memory**, mapped by multiple processes.
- It contains `SharedSpinLockState spinlock_states[8]` — plain structs (atomics + padding only). No C++ objects with vtables.
- **ABI**: Layout is versioned; `static_assert(sizeof(SharedSpinLockState) == 32)` and header size 4096 are critical.
- **Implication**: We **cannot** put a vtable pointer or any process-specific pointer inside the header. Different processes have different vtable addresses; shared memory must be layout-compatible and pointer-free for polymorphic types. So the "owner" for the shared case **must** remain:
  - **Memory**: caller-provided (pointer to `SharedSpinLockState` in the header).
  - **Protocol**: implemented in **code** (same binary in all processes), not in an object that lives in shm.

So any abstraction must work with **"pointer to state + protocol implemented in code"**. No C++ object with virtual functions can live inside the shared header.

## 2. Runtime Cost of Abstraction Options

### Option A: Virtual functions (owner as interface)

- **Idea**: Base class `AtomicOwner` with `virtual bool try_acquire(...)`, `virtual void release(...)`. Token owner and shared-memory owner as derived classes.
- **DataBlock**: The state lives in shm, so we cannot put the owner object in the header. We would need a **process-local wrapper** (e.g. `SharedMemoryOwner : AtomicOwner`) that holds `SharedSpinLockState*` and implements the interface. Spinlock would hold `AtomicOwner&` or a pointer and call `try_acquire` / `release` through the vtable.
- **Runtime cost**:
  - **Indirect call** in the hot path (every iteration of the spin loop: try_acquire, and on unlock: release). Compiler generally cannot inline across virtual calls.
  - One **branch** per call that is hard to predict (vtable slot).
  - **Extra memory**: wrapper object per lock use (or per lock): at least one pointer + vtable pointer (e.g. 16 bytes on 64-bit), plus any name/context.
- **Verdict**: **Increases runtime code load** (indirect calls in hot path) and adds **memory/complication** (wrapper required; cannot put owner in shm). **Not recommended** for the spin loop.

### Option B: Template policy (owner as type)

- **Idea**: Spinlock is a template on the **owner policy**: `Spinlock<OwnerPolicy>`. The policy is a **type** (e.g. `TokenOwnerPolicy`, `SharedMemoryOwnerPolicy`) that defines how to try_acquire/release on a given state pointer. No virtual; policy is a compile-time type.
- **DataBlock**: No change. We still pass `SharedSpinLockState*` and a name. We use `Spinlock<SharedMemoryOwnerPolicy>(state, name)`. The state stays in the header; no wrapper in shm. Policy is purely in code.
- **Runtime cost**:
  - **Zero**: All calls to the policy are inlined (policy methods can be static or inlined). No vtable, no indirect call, no extra branch for "which kind of owner."
  - **Code size**: Two instantiations of the spin loop (one for token, one for shared). Slightly larger binary, no extra cost at runtime.
- **Memory**: No extra per-lock state. Spinlock still holds only `State*` + name (or state* only for token). No process-local wrapper required.
- **Verdict**: **No runtime overhead**, integrates with DataBlock (same pointer, same layout). **Recommended.**

### Option C: Enum + switch (single type, branch on kind)

- **Idea**: One `Spinlock` class that stores `enum OwnerKind { Token, Shared }` and `void* state`. In `try_lock_for` / `unlock`, `switch (kind)` and run token vs shared logic.
- **DataBlock**: Pass `state = &header->spinlock_states[i]`, kind = Shared. State stays in header.
- **Runtime cost**:
  - **One branch** per lock/unlock (predictable after first call; compiler can optimize).
  - No vtable, no indirect call. Both code paths in one translation unit; compiler may still inline.
- **Memory**: One byte (or word) for enum + one pointer. Slightly more than current SharedSpinLock (which doesn’t need the enum if we only have one kind).
- **Verdict**: **Low overhead**, works with DataBlock. Good if we want a single `Spinlock` type (e.g. for APIs that take "any spinlock") without templates. Slightly more overhead than Option B (one branch per call).

### Option D: Function pointers (no virtual, but indirect call)

- **Idea**: Spinlock holds `bool (*try_acquire)(void* state, ...)`, `void (*release)(void* state, ...)`, and `void* state`.
- **Runtime cost**: Same as virtual for the hot path — **indirect call** on every try_acquire/release. No vtable but still an indirect call. **Not recommended** for the same reason as virtual.

## 3. Conclusion and Recommendation

| Approach        | Runtime cost      | DataBlock compatible? | Memory / complexity      |
|----------------|-------------------|------------------------|---------------------------|
| Virtual        | Extra (indirect)   | Only with wrapper      | Wrapper; no owner in shm  |
| **Template**   | **None**           | **Yes, no change**     | **None**                  |
| Enum + switch  | One branch/call    | Yes                    | One enum per lock         |
| Function ptrs  | Extra (indirect)   | Yes                    | Two pointers per lock     |

- **Virtual functions** would add runtime cost (indirect calls in the spin loop) and memory complications (cannot put owner in shm; need a process-local wrapper). They are **not** a good fit for this hot path.
- **Template policy** gives a single abstraction (owner = provided memory + protocol-as-type) with **zero** added runtime cost and **no** change to the DataBlock memory model: the header still only holds `SharedSpinLockState`; we just call the shared-memory protocol via a template parameter. The spinlock "only operates on" the owner; the owner defines how tokens/state are updated and what they mean; backoff/timeout live in the spin loop and are shared by all owner types.
- **Enum + switch** is a reasonable alternative if we need a single non-template type (e.g. for ABI or for passing "any spinlock" through an API); it keeps one predictable branch per lock/unlock and remains compatible with the current DataBlock layout.

So: **unification is possible** without increasing runtime code load and without virtuals, by using a **template on the owner policy**. The "atomic owner" is the memory provided (token state or `SharedSpinLockState`); the spinlock operates on it; and we get one backoff/timeout implementation for all purposes.

---

## 4. pImpl and ABI Compatibility (Do Not Break)

The DataBlock and utils APIs are designed for **ABI stability**: public headers define types that must remain layout- and signature-stable so that callers compiled against one version of the library can link and run with another.

### 4.1 What is in the ABI

- **data_block.hpp** includes **shared_memory_spinlock.hpp**, so the following are **public API** and part of the ABI:
  - **SharedSpinLockState** — struct layout (size 32, member order, alignment). Used inside `SharedMemoryHeader::spinlock_states[]`. Any change breaks shared-memory layout and versioning.
  - **SharedSpinLock** — class size, member layout (`m_state`, `m_name`), ctor/dtor, `try_lock_for`, `lock`, `unlock`, `is_locked_by_current_process`. Returned **by value** from `DataBlockProducer::get_spinlock()` and `DataBlockConsumer::get_spinlock()`, so size and layout must stay fixed.
  - **SharedSpinLockGuard** — size, layout (`m_lock`), ctor/dtor. Used inside `SharedSpinLockGuardOwning`.
  - **SharedSpinLockGuardOwning** — size, layout (`m_lock`, `m_guard`), ctor/dtor. Returned by `DataBlockProducer::acquire_spinlock()` as `std::unique_ptr<SharedSpinLockGuardOwning>`; destructor is called across the DLL boundary.
- **pImpl**: `DataBlockProducer`, `DataBlockConsumer`, `SlotWriteHandle`, `SlotConsumeHandle`, etc. expose only `std::unique_ptr<Impl> pImpl`. The **Impl** types are not in the public header; we can change their implementation freely. The spinlock types, however, are **not** behind pImpl — they are returned or held by value/reference, so they are part of the stable ABI.

### 4.2 What we must not do

- **Do not** change the **names** of `SharedSpinLock`, `SharedSpinLockState`, `SharedSpinLockGuard`, `SharedSpinLockGuardOwning`. Renaming or replacing them with a template alias (e.g. `using SharedSpinLock = Spinlock<SharedMemoryOwnerPolicy>`) would change mangled names and break existing linkees.
- **Do not** change the **binary layout** (size, alignment, member order) of these types. Adding or removing members, or changing types of members, breaks ABI.
- **Do not** change **signatures** of public methods (parameter types, return types, cv-qualification). Adding new overloads or optional parameters can be done with care; changing existing ones breaks ABI.
- **Do not** put virtual functions or vtable pointers into these types (already ruled out for shared memory and runtime cost).

### 4.3 How to unify without breaking ABI

- **Keep the public types as-is.** `SharedSpinLock`, `SharedSpinLockState`, `SharedSpinLockGuard`, and `SharedSpinLockGuardOwning` remain the **stable** types in the public header with the same layout and signatures.
- **Unify inside the implementation.** In the .cpp (or in a header-only detail namespace that is not part of the exported API), introduce a **template** or internal helper (e.g. `detail::SpinlockImpl<OwnerPolicy>`) that implements the spin loop, backoff, and timeout. The **current** `SharedSpinLock` implementation (in shared_memory_spinlock.cpp) then uses this internal template with the shared-memory policy; the public `SharedSpinLock` class remains a concrete, non-template class that holds `SharedSpinLockState*` and `std::string` and delegates to the internal implementation. A **new** token-based lock (e.g. for in-process use, or a future `TokenSpinlock` in a different header) can reuse the same internal template with a token policy, without touching the public DataBlock API.
- **No template in the public API.** The public API continues to use and return `SharedSpinLock` (concrete class), not `Spinlock<Policy>`. Template policy is used only for **code reuse inside the implementation**, so that backoff/timeout and the “operate on owner” logic are written once and shared.

This way we get:
- **Unification** of logic (one spin loop, one backoff strategy) for both shared-memory and token-based owners.
- **No ABI break**: public types and pImpl boundaries remain unchanged.
- **No runtime cost**: no virtuals; internal template inlines away.

---

## 5. Unified Owner Layout: PID, TID, TOKEN (Minimal Requirement)

To unify what the "owner" looks like across token-only (in-process) and PID/TID (cross-process) use cases, we can define a **single minimal layout** that all owner state conforms to: minimal memory, one shape, tuned per situation.

### 5.1 Minimal requirement: three fields

- **PID** — process identifier (or 0 when not used).
- **TID** — thread identifier (or 0 when not used).
- **TOKEN** — one additional field for extra information during handling (holder token, generation, recursion, or packed flags).

All owner state (whether in shared memory or process-local) has at least these three. The spinlock operates only on this layout; the **meaning** of each field is tuned per situation.

### 5.2 Interpretation by situation

| Situation        | PID / TID use              | TOKEN use                                      |
|-----------------|----------------------------|------------------------------------------------|
| **Cross-process** (DataBlock) | Ownership: only (pid,tid) may unlock; zombie reclaim by PID. | Generation (bump on release), or pack generation + recursion. |
| **In-process token**         | 0 / 0 (ignored).           | Holder token; whoever has the token can release (handoff). |
| **Hybrid** (future)         | Optional identity.         | Token + generation or other metadata.          |

So we keep **one shape** (PID, TID, TOKEN) and vary only how we interpret the fields and what protocol we run (e.g. CAS on PID vs CAS on TOKEN).

### 5.3 Mapping to current SharedSpinLockState (32 bytes, ABI)

Current layout (must preserve for ABI):

- `owner_pid` (8), `owner_tid` (8), `generation` (8), `recursion_count` (4), `padding` (4) = 32 bytes.

**Conceptual mapping without changing layout:**

- **PID** = `owner_pid`
- **TID** = `owner_tid`
- **TOKEN** = `generation` (additional info: generation counter; can be repurposed or packed with recursion if we ever consolidate)
- Keep `recursion_count` as the extra word needed for recursive locking in the cross-process case (or pack into TOKEN low bits if we want exactly three words).

So the **minimal requirement** is satisfied: PID, TID, and one TOKEN-sized field. We already have that; the third word is `generation`. For a **strict** "exactly three fields" layout we could later define a new type (e.g. `AtomicOwnerState`: pid, tid, token only = 24 bytes) for token-only or new use cases, while keeping `SharedSpinLockState` as the 32-byte extended form (PID, TID, TOKEN, recursion_count) for backward compatibility.

### 5.4 One layout everywhere (unified design)

We **do not** introduce a second, smaller owner type. Per [UNIFIED_SPINLOCK_OWNER_DESIGN.md](UNIFIED_SPINLOCK_OWNER_DESIGN.md):

- **One owner layout:** the 32-byte struct (PID, TID, TOKEN, recursion_count) is used everywhere — in DataBlock shared memory and for in-process token use (pid=0, tid=0, token=holder).
- **One spinlock, one guard:** a single abstraction operates on this layout; mode (token vs pid/tid) selects semantics. Extra memory (32 bytes instead of one word) is acceptable; fewer code branches and one memory model reduce errors.
- No separate "minimal" 24-byte or one-word owner: one layout, one protocol layer, tuned per situation by mode/convention only.
