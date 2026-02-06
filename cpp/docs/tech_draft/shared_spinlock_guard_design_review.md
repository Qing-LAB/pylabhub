# SharedSpinLockGuard Design Review

## Problem Description

The `IDataBlockProducer::acquire_user_spinlock` interface is defined to return a `std::unique_ptr<SharedSpinLockGuard>`. This return type implies that the caller receives ownership of a dynamically allocated `SharedSpinLockGuard` object, which is expected to manage the lifetime of the underlying resource (in this case, the acquired lock).

However, the current design of `SharedSpinLockGuard` (defined in `plh_sync_primitives.hpp`) holds a `SharedSpinLock& m_lock;` member. This means `SharedSpinLockGuard` itself does *not own* the `SharedSpinLock` object it refers to; it only holds a reference to it.

## Design Conflict

When `acquire_user_spinlock` attempts to implement this interface:

1.  It calls `m_dataBlock->acquire_shared_spinlock(debug_name)` to get an index to a `SharedSpinLockState*`.
2.  It then needs to construct a `SharedSpinLock` object (e.g., `SharedSpinLock lock_obj(*m_dataBlock->get_shared_spinlock_state(index), debug_name);`).
3.  Finally, it would construct `std::make_unique<SharedSpinLockGuard>(lock_obj);`.

The issue is that `lock_obj` (the `SharedSpinLock` instance) is a local variable on the stack. When `acquire_user_spinlock` returns, `lock_obj` will be destroyed. The `SharedSpinLockGuard` (now managed by a `unique_ptr`) will then hold a dangling reference (`m_lock`) to a destroyed `SharedSpinLock` object. Any subsequent attempt to use this guard would lead to undefined behavior.

## Proposed Solutions

To resolve this fundamental design flaw, `SharedSpinLockGuard` must own the `SharedSpinLock` object it manages.

### Option 1: Modify `SharedSpinLockGuard` to own `SharedSpinLock`

Change the internal member of `SharedSpinLockGuard` from `SharedSpinLock &m_lock;` to `SharedSpinLock m_lock;`.

This would require:
- Modifying `plh_sync_primitives.hpp`.
- Modifying `SharedSpinLockGuard`'s constructor to take `SharedSpinLockState*` and `const std::string&` directly, so it can internally construct its `m_lock` member.
- This is an API-breaking change for `SharedSpinLockGuard`'s constructor.

### Option 2: Change `IDataBlockProducer::acquire_user_spinlock`'s return type

Change the return type of `IDataBlockProducer::acquire_user_spinlock` to something that accurately reflects the `SharedSpinLockGuard`'s reference-based nature (e.g., not returning a `unique_ptr` to it, or returning a `unique_ptr` to the `SharedSpinLock` directly and letting the client manage the guard). This is also an API-breaking change for the `IDataBlockProducer` interface.

## Recommendation

Option 1 is generally preferred if the intent is for the guard to be a self-contained, owning entity. This aligns with the `std::unique_ptr` return type. This design should be implemented, requiring modifications to `plh_sync_primitives.hpp`.

**Status for current task:** For the purpose of enabling compilation, a temporary implementation will be provided for `acquire_user_spinlock` that explicitly notes this design flaw and the potential for runtime issues due to dangling references if the underlying `SharedSpinLock` is temporary. This document serves as a reminder for a future design iteration and code refactoring.
