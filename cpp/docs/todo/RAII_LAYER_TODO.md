# RAII Layer TODO

**Purpose:** Track C++ RAII layer improvements, transaction API enhancements, and typed access patterns for DataHub.

**Master TODO:** `docs/TODO_MASTER.md`  
**Design Document:** `docs/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` (active)  
**Implementation Guidance:** `docs/IMPLEMENTATION_GUIDANCE.md` § C++ Abstraction Layers

---

## Current Focus

### Transaction API Refinements
**Status**: 🟡 In Progress

- [ ] **Review transaction API patterns** – Evaluate current `with_write_transaction`, `with_read_transaction` usage
- [ ] **Exception safety tests** – Comprehensive tests for exception propagation through transaction lambdas
- [ ] **Guard API improvements** – `WriteTransactionGuard`, `ReadTransactionGuard` usability enhancements
- [x] **Pitfall 10 fixes** – Fixed handle lifetime violations in test code (4 occurrences)

### Typed Access Helpers
**Status**: 🟢 Ready

- [ ] **with_typed_write<T> refinements** – Ensure alignment guarantees for all types
- [ ] **with_typed_read<T> validation** – Add runtime checks for type safety
- [ ] **Typed flexible zone access** – Extend typed access to flexible zones
- [ ] **Documentation** – Update examples with typed access patterns

---

## Design Decisions Under Review

### Context-Centric API (Deferred)
**Original proposal**: Transaction context holds producer/consumer reference, methods like `ctx.write()`, `ctx.commit()`

**Current implementation**: Direct methods on handles - `with_write_transaction(*producer, timeout, lambda)`

**Status**: 🔵 Deferred - Current implementation is working well

**Rationale**:
- Simpler API surface
- Less abstraction overhead
- Handles already provide necessary operations
- No compelling use case for context-centric approach yet

**Revisit if**: User feedback indicates confusion with current API

### Layer Consolidation
**Question**: Should we merge Layer 1 (primitive) and Layer 2 (transaction) more tightly?

**Current**: Clear separation - primitives in `data_block.hpp`, transactions use primitives

**Considerations**:
- Pro: Simpler mental model for users
- Con: Loses flexibility for advanced users who need primitives
- Con: Harder to test layers independently

**Decision**: Keep separation for now

---

## Backlog

### API Enhancements
- [ ] **Iterator improvements** – `DataBlockSlotIterator` enhancements for common patterns
- [ ] **Bulk operations** – Read/write multiple slots in one call (if use case emerges)
- [ ] **Timeout helpers** – Named timeout constants (`TIMEOUT_IMMEDIATE`, `TIMEOUT_DEFAULT`, `TIMEOUT_INFINITE`)
- [ ] **Error context** – Richer error information on acquisition failure

### RAII Patterns
- [ ] **Flexible zone guards** – RAII wrapper for flexible zone access with spinlock
- [ ] **Multi-slot transactions** – Acquire multiple slots atomically (if use case emerges)
- [ ] **Scoped diagnostics** – RAII wrapper for diagnostic handle lifecycle

### Performance
- [ ] **Move semantics** – Ensure all handles support efficient moves
- [ ] **Zero-cost abstractions** – Verify transaction API has no overhead vs primitive API
- [ ] **Inline critical paths** – Profile and inline hot functions

### Documentation and Examples
- [ ] **Update RAII_LAYER_USAGE_EXAMPLE.md** – Incorporate latest API changes
- [ ] **Producer/consumer examples** – Modernize `datahub_producer_example.cpp` and `datahub_consumer_example.cpp`
- [ ] **Error handling guide** – Best practices for handling transaction failures
- [ ] **Migration guide** – From primitive API to transaction API

---

## Related Work

- **Memory Layout** (`docs/todo/MEMORY_LAYOUT_TODO.md`) – Alignment affects typed access
- **Testing** (`docs/todo/TESTING_TODO.md`) – Transaction API tests in Phase B/C
- **API** (`docs/todo/API_TODO.md`) – Public API refinements and documentation

---

## Recent Completions

### 2026-02-14
- ✅ Fixed Pitfall 10 violations in tests (4 occurrences) - handle lifetime issues
- ✅ Documented handle lifetime contract in IMPLEMENTATION_GUIDANCE.md

### 2026-02-13
- ✅ Shared spinlock API (`get_spinlock`, `spinlock_count`)
- ✅ Transaction API exception safety verified

### 2026-02-12
- ✅ Transaction guards (`WriteTransactionGuard`, `ReadTransactionGuard`) implemented
- ✅ Typed access (`with_typed_write<T>`, `with_typed_read<T>`) working

---

## Notes

### Current API Surface

**Layer 1 - Primitive API** (explicit control):
- `producer->acquire_write_slot(timeout)` → `SlotWriteHandle`
- `consumer->acquire_consume_slot(timeout)` → `SlotConsumeHandle`
- Manual `release_write_slot()`, `release_consume_slot()`

**Layer 2 - Transaction API** (recommended):
- `with_write_transaction(*producer, timeout, lambda)`
- `with_read_transaction(*consumer, timeout, lambda)`
- `with_typed_write<T>(*producer, timeout, lambda)`
- `with_typed_read<T>(*consumer, timeout, lambda)`
- `WriteTransactionGuard`, `ReadTransactionGuard`

### Design Principles

1. **RAII everywhere** - Automatic resource cleanup, exception-safe
2. **Type safety** - Typed access validates alignment and size
3. **Zero overhead** - Transaction API should compile to same code as primitive API
4. **Clear ownership** - Handles own slot locks, guards own handles
5. **Fail fast** - Invalid operations caught at API boundary, not in destructor

### Usage Patterns

```cpp
// Pattern 1: Lambda-based (most concise)
with_write_transaction(*producer, 1000, [](WriteTransactionContext& ctx) {
    ctx.slot().write(data, size);
    ctx.slot().commit(size);
});

// Pattern 2: Guard-based (explicit control)
auto guard = WriteTransactionGuard(*producer, 1000);
if (guard.slot()) {
    guard.slot()->write(data, size);
    guard.commit();  // Explicit commit
}

// Pattern 3: Typed access (type-safe)
with_typed_write<MyStruct>(*producer, 1000, [](MyStruct& obj) {
    obj.field1 = value1;
    obj.field2 = value2;
});
```

### Open Questions

- Should we add `with_next_slot` that combines iterator and transaction?
- Do we need timeout policies (e.g., exponential backoff)?
- Should guards be copyable or only movable? (Currently move-only)
