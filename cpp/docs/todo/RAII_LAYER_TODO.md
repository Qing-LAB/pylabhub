# RAII Layer TODO

**Purpose:** Track the Phase 3 C++ RAII layer: `with_transaction`, `TransactionContext`,
`SlotIterator`, `SlotRef`, `ZoneRef`, `Result`, and related typed-access infrastructure.

**Master TODO:** `docs/TODO_MASTER.md`
**Design Document:** `docs/archive/transient-2026-02-21/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` (archived; implementation complete)
**Protocol Reference:** `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md`
**Code Review Source:** `docs/code_review/REVIEW_utils_2026-02-15.md`

---

## Current Focus

### Code Review Open Items (2026-02-21)

- [x] **Actor write loop control-flow duplication** ✅ FIXED 2026-02-23 — Extracted into two
  private helpers: `ProducerRoleWorker::step_write_deadline_()` (deadline advance + trigger wait
  + overrun detection; used by both `run_loop_shm()` and `run_loop_zmq()`) and
  `ConsumerRoleWorker::check_read_timeout_()` (timeout baseline advance; used by consumer
  `run_loop_shm()`). Both loops now call the helper in a single line instead of 20+ lines each.
  479/479 tests pass.
  - Source: code_review_utils_2025-02-21.md item 13; `src/actor/actor_host.cpp`

---

## Backlog

- [ ] **LoopPolicy + ContextMetrics (Pass 2)** — Implement `LoopPolicy` enum
  (`MaxRate`/`FixedRate`/`MixTriggered`), `ContextMetrics` struct, and per-iteration timing
  in `SlotIterator::operator++()`. Exposes `api.metrics()` dict to Python actor scripts.
  Full design spec: `docs/HEP/HEP-CORE-0008-LoopPolicy-and-IterationMetrics.md`.
  Files affected: `transaction_context.hpp`, `slot_iterator.hpp`, `data_block.hpp`,
  `data_block.cpp`, `actor_config.hpp/.cpp`, `actor_host.cpp`, `actor_module.cpp`,
  `hub_producer.hpp`, `hub_consumer.hpp`.

- [ ] **FlexZone atomic-ref usage example** — Add a live test that demonstrates the
  correct `std::atomic_ref<T>` pattern for out-of-transaction lock-free FlexZone access.
  See `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` §9 for the documented pattern.

- [ ] **Timeout helpers** — Named timeout constants (`TIMEOUT_IMMEDIATE`, `TIMEOUT_DEFAULT`,
  `TIMEOUT_INFINITE`) to reduce magic numbers in call sites.

- [ ] **Scoped diagnostics** — RAII wrapper for `DataBlockDiagnosticHandle` lifecycle.

- [ ] **Move semantics audit** — Confirm all RAII handles support efficient move (no copy).

- [ ] **Zero-cost abstraction verification** — Profile `with_transaction` vs primitive API
  with optimizations enabled; confirm no overhead on the hot path.

---

## Recent Completions

### 2026-02-17 (All code review items resolved)

- ✅ **[Q-1] `ExponentialBackoff` name** — Note strengthened: historical name, retained for
  API compatibility; callsites listed in comment — `src/include/utils/backoff_strategy.hpp`
- ✅ **[Q-2] RAII headers mid-file** — Comment added explaining intentional placement:
  headers depend on class declarations above; moving to top causes incomplete-type errors —
  `src/include/utils/data_block.hpp`
- ✅ **[A-1] noexcept + throw UB** — Verified: `slot_rw_state()` made `noexcept`, returns
  `nullptr` on bad index; callers check for null — `src/utils/data_block.cpp`
- ✅ **[M-1] `publish()` double-release** — Verified: `release_write_handle()` is
  idempotent (`impl.released` guard); no double-release — `src/utils/data_block.cpp`
- ✅ **[M-5] Handle destructor protocol** — Verified: destructors call release functions
  which set `released=true`; exception-safe — `src/utils/data_block.cpp`
- ✅ **[Q-3] Redundant catch in `with_next_slot`** — Moot: `with_next_slot()` removed
  entirely in post-Phase3 cleanup

### 2026-02-17 (Phase 3 completion + code review resolution)

- ✅ **[C-1] `SlotIterator::begin()` copy bug** — Fixed with `std::move(*this)`
- ✅ **[C-2] `TransactionContext::config()` missing accessor** — Resolved by redesign:
  schema validation moved to factory functions; `TransactionContext` no longer calls `config()`
- ✅ **[C-3] `validate_read()` always-true stub** — Removed; validation now runs
  automatically in `release_consume_slot()` (TOCTTOU + checksum)
- ✅ **[M-2] Redundant `if constexpr` in `flexzone()`** — Simplified to single `return ZoneRefType(m_handle)`
- ✅ **[M-3] Silent `catch(...)` in `has_zone()`/`size()`** — Exception swallowing removed
- ✅ **[Q-6] Unnecessary `const_cast` in `flexzone() const`** — Removed
- ✅ **[Q-5] Vestigial `SlotIterator::ContextType` alias** — Removed
- ✅ **FlexZone trivially-copyable documentation** — `static_assert` messages updated in
  `zone_ref.hpp`, `transaction_context.hpp`; `HEP-CORE-0007` §9 added;
  `TxAPITestFlexZone` corrected from `std::atomic<T>` to plain POD types
- ✅ `publish()` / `publish_flexzone()` / `suppress_flexzone_checksum()` added to `TransactionContext`
- ✅ `with_transaction` auto-flexzone checksum on normal exit
- ✅ `std::invocable` concept constraint + `[[nodiscard]]` on `with_transaction`
- ✅ Consumer heartbeat auto-registration at construction (supersedes ConsumerHeartbeatGuard plan)
- ✅ `HEP-CORE-0007` — Protocol and policy reference added

### 2026-02-15 (Phase 3 initial implementation)

- ✅ `TransactionContext<FlexZoneT, DataBlockT, IsWrite>` template
- ✅ `WriteTransactionContext` / `ReadTransactionContext` aliases
- ✅ `SlotIterator` non-terminating range with `Result<SlotRef, SlotAcquireError>`
- ✅ `ZoneRef` / `WriteZoneRef` / `ReadZoneRef` typed flexzone wrappers
- ✅ `create_datablock_producer<F,D>` / `find_datablock_consumer<F,D>` factory functions
- ✅ Dual-schema compile-time validation via `static_assert(is_trivially_copyable_v<T>)`

---

## Notes

### Current Phase 3 API Surface (as of 2026-02-17)

```cpp
// Factory (schema-validated at creation/attach time)
std::unique_ptr<DataBlockProducer> create_datablock_producer<FlexZoneT, DataBlockT>(hub, name, policy, config);
std::unique_ptr<DataBlockConsumer> find_datablock_consumer<FlexZoneT, DataBlockT>(hub, name, secret, config);

// Producer transaction
producer->with_transaction<FlexZoneT, DataBlockT>(timeout_ms, [](WriteTransactionContext<F,D>& ctx) {
    ctx.flexzone().get().field = value;   // typed FlexZone write
    for (auto& result : ctx.slots(50ms)) {
        if (result.is_ok()) {
            result.content().get().data = produce();
            break; // auto-publish fires here
        }
    }
    // ctx.publish()              — explicit publish (advanced)
    // ctx.publish_flexzone()     — explicit flexzone checksum update
    // ctx.suppress_flexzone_checksum() — opt out of auto-update
    // ctx.update_heartbeat()     — keep liveness during long operations
});

// Consumer transaction
consumer->with_transaction<FlexZoneT, DataBlockT>(timeout_ms, [](ReadTransactionContext<F,D>& ctx) {
    auto zone = ctx.flexzone();
    for (auto& result : ctx.slots(50ms)) {
        if (result.is_ok()) {
            process(result.content().get());
            break;
        }
    }
});
```

### FlexZone Type Rule (CRITICAL)

`FlexZoneT` and `DataBlockT` must be **trivially copyable** — enforced by `static_assert`
in `ZoneRef`, `TransactionContext`, and the factory functions.

**`std::atomic<T>` members are NOT allowed** (breaks trivial copyability on MSVC).
Use plain POD members + `std::atomic_ref<T>` at call sites for lock-free access outside
the `with_transaction` spinlock scope. See `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` §9.

### Design Principles

1. **RAII everywhere** — Exception-safe: slot auto-published on normal exit, auto-aborted on exception
2. **Trivially-copyable types only** — FlexZoneT and DataBlockT must be POD-layout for shared memory
3. **Schema validation at boundary** — Factory functions validate schema hash at creation/attach
4. **Auto-heartbeat** — `SlotIterator::operator++()` fires heartbeat before each slot attempt
5. **Auto-flexzone checksum** — `with_transaction` updates flexzone checksum on normal exit
