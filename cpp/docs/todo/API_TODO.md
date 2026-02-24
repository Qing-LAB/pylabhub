# API TODO

**Purpose:** Track public API refinements, documentation improvements, and API surface enhancements for DataHub.

**Master TODO:** `docs/TODO_MASTER.md`  
**API Reference:** `cpp/src/include/utils/data_block.hpp`  
**Examples:** `cpp/examples/`

---

## Current Focus

### API Documentation Gaps

- [x] **Consumer registration to broker** – ✅ Fully implemented 2026-02-18; CONSUMER_REG/DEREG handshake
- [ ] **stuck_duration_ms in diagnostics** – `SlotDiagnostic::stuck_duration_ms` requires timestamp on acquire
- [ ] **DataBlockMutex documentation** – Factory vs direct constructor, exception vs optional/expected
- [ ] **Flexible zone initialization** – Document when flexible_zone_info is populated

### API Consistency
**Status**: 🟢 Ready

- [x] **release_write_slot** – Documented return values and idempotent behavior
- [x] **Slot handle lifetime** – Contract documented in data_block.hpp
- [x] **Recovery error codes** – All codes documented in recovery_api.hpp
- [ ] **Error code consistency** – Review all APIs for consistent error reporting

### Code Review Open Items (2026-02-21)

- [x] **ChannelPattern duplicate string conversion** ✅ FIXED 2026-02-22 — Shared
  `channel_pattern_to_str()` / `channel_pattern_from_str()` moved to `channel_pattern.hpp`.
  Removed duplicate `pattern_to_wire/from_wire` in `messenger.cpp` and
  `pattern_to_str/from_str` in `broker_service.cpp`.
  - Source: code_review_utils_2025-02-21.md item 8

- [ ] **Logger header two comment blocks** — Logger public header has two separate comment blocks covering overlapping topics. Merge into one coherent doc block. Low priority.
  - Source: REVIEW_cpp_src_hep_2026-02-20.md item 3

---

## Public API Surface

### Core DataBlock API
```cpp
// Factory functions
std::unique_ptr<DataBlockProducer> create_datablock_producer(...)
std::unique_ptr<DataBlockConsumer> find_datablock_consumer(...)

// Producer API
class DataBlockProducer {
    std::unique_ptr<SlotWriteHandle> acquire_write_slot(int timeout_ms);
    bool release_write_slot(SlotWriteHandle& handle);
    void update_heartbeat();
    SharedSpinLock get_spinlock(size_t index);  // For flexible zones
    size_t spinlock_count() const noexcept;
    // ... metrics, diagnostics
};

// Consumer API
class DataBlockConsumer {
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(int timeout_ms);
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(uint64_t slot_id, int timeout_ms);
    bool release_consume_slot(SlotConsumeHandle& handle);
    DataBlockSlotIterator slot_iterator();
    // ... metrics, diagnostics
};

// Handles
class SlotWriteHandle;   // RAII, destroyed before producer
class SlotConsumeHandle; // RAII, destroyed before consumer
```

### Recovery and Diagnostics API
```cpp
// Diagnostics
DataBlockDiagnosticHandle open_datablock_for_diagnostic(const std::string& name);
class SlotDiagnostics;
class IntegrityValidator;

// Recovery
class SlotRecovery;
class HeartbeatManager;
bool datablock_is_process_alive(uint64_t pid);
RecoveryResult datablock_validate_integrity(...);
```

---

## Backlog

### Header / Include Layering Refactor

**Goal**: Establish a clean, layered include structure across the entire codebase so that each
header is self-contained at exactly its abstraction level, C-API users are never exposed to
C++ internals, and compilation time is proportional to what a TU actually uses.

**Motivation** (observed 2026-02-20):
- `data_block.hpp` (and related DataBlock headers) mix C-API primitives, C++ template
  abstractions, and implementation details in a single file. A C-API consumer sees the full
  C++ RAII layer; a high-level C++ user sees raw C-API structs. Neither gets a clean view.
- Compilation is dominated by the weight of transitive includes — `plh_datahub.hpp` pulls in
  all of DataBlock, Messenger, BrokerService, JsonConfig, and pybind11 in some TUs that only
  need one or two of these.
- Mix of C-API and C++ in the same file makes it harder to audit which symbols are stable ABI
  vs. which are internal C++ helpers.
- The `actor_schema.hpp` ↔ `nlohmann::json` coupling (fixed 2026-02-20 with `json_fwd.hpp`)
  is a symptom of the same problem: headers carrying dependencies they don't structurally need.

**Target structure** (sketch):

```
Layer 0 — C ABI (extern "C", no C++ constructs)
  slot_rw_coordinator.h      ← C consumers only need this
  data_block_c_api.h         ← raw acquire/release/checksum (no templates)

Layer 1 — C++ primitives (no pybind11, no nlohmann, no libzmq)
  shared_memory_spinlock.hpp
  data_block_config.hpp      ← DataBlockConfig, enums, constants only
  data_block_handles.hpp     ← SlotWriteHandle, SlotConsumeHandle

Layer 2 — C++ high-level (full DataBlock, Messenger — not pybind11)
  data_block.hpp             ← DataBlockProducer, DataBlockConsumer, templates
  messenger.hpp

Layer 3 — Integration (broker, hub API, JsonConfig)
  broker_service.hpp
  hub_producer.hpp / hub_consumer.hpp
  hub_config.hpp

Umbrella headers (convenience only, include what they promise):
  plh_datahub.hpp            ← all of Layer 3 + 2 + 1
  plh_service.hpp            ← Layer 2 minus DataBlock
```

**Key rules to enforce**:
1. A header at layer N may not include a header from layer > N.
2. C-API headers (`extern "C"`) may not include any C++ header.
3. Internal `.cpp`-only types (Pimpl `Impl`, static helpers) go in `*_internals.hpp` /
   `*_private.hpp` beside the `.cpp` — never in the installed public header.
4. Forward declarations (`*_fwd.hpp`) are preferred over full includes in peer headers.
5. CMake `target_include_directories` should enforce the boundary: C-API targets see only
   Layer 0 includes; `pylabhub-utils` consumers see Layer 1–3.

**Scope**:
- [ ] Audit `data_block.hpp` — extract C-API declarations into `data_block_c_api.h`;
      separate config/enums into `data_block_config.hpp`; keep template layer in `data_block.hpp`
- [ ] Audit `messenger.hpp` — verify no DataBlock or pybind11 leakage
- [ ] Audit `broker_service.hpp`, `hub_producer.hpp`, `hub_consumer.hpp` for unnecessary
      transitive includes
- [ ] Review `actor_schema.hpp` and `actor_config.hpp` — no json.hpp in public actor headers
- [ ] Review `plh_*.hpp` umbrella headers — confirm they only include what they document
- [ ] Add CMake `SYSTEM` / `INTERFACE` include guards to enforce layer boundaries
- [ ] Measure before/after full-rebuild time (`cmake --build build --target all`) as a metric

**Files most affected**: `data_block.hpp`, `plh_datahub.hpp`, `src/utils/CMakeLists.txt`

### API Enhancements
- [ ] **Config builder pattern** – Fluent API for DataBlockConfig construction
- [ ] **Error callbacks** – Register callbacks for specific error conditions
- [ ] **Flexible zone by name** – Access flexible zones by string name instead of index
- [ ] **Batch operations** – Read/write multiple values efficiently
- [ ] **Async API** – Non-blocking variants with futures/promises (if use case emerges)

### Configuration API
- [ ] **Config validation helpers** – Pre-validate config before creation
- [ ] **Config templates** – Named configs for common patterns (e.g., "single_writer", "high_throughput")
- [ ] **Config explicit-fail test** – Test that creation throws with invalid config

### Diagnostics API
- [ ] **Structured diagnostics** – Return diagnostic info as structured data, not just logs
- [ ] **Health check API** – Single call to check if DataBlock is healthy
- [ ] **Performance metrics** – Expose throughput, latency, contention metrics

### Recovery API Improvements
- [ ] **Integrity repair path** – Slot-checksum repair in `validate_integrity` uses
  `create_datablock_producer_impl` which reinitialises the header on open (since shm_create
  uses O_CREAT without O_EXCL).  Should use `attach_datablock_as_writer_impl` (WriteAttach mode)
  instead for safe in-place repair without destroying ring state.
- [ ] **Graceful degradation** – API for dealing with partially corrupted blocks
- [ ] **Recovery policies** – Configurable recovery behavior (aggressive vs conservative)

---

## API Design Principles

### 1. Error Handling Strategy
- **C API**: Return error codes, no exceptions
- **C++ API**: Throw for contract violations, return nullptr/false for expected failures
- **Recovery API**: Return `RecoveryResult` enum

### 2. Lifetime and Ownership
- **Factories** return `unique_ptr` (exclusive ownership)
- **Handles** are move-only, destroyed before owner
- **Guards** are move-only, RAII, noexcept destructors

### 3. Thread Safety
- **Producer/Consumer**: Thread-safe (internal mutex)
- **Handles**: Not thread-safe, use from one thread
- **C API**: No locking, caller's responsibility

### 4. Noexcept Marking
- Destructors: Always noexcept
- Simple accessors: noexcept if no throw
- Acquisition: Not noexcept (can fail)
- See IMPLEMENTATION_GUIDANCE.md § Explicit noexcept

---

## Documentation Tasks

### API Reference
- [ ] **Doxygen coverage** – Ensure all public APIs have complete documentation
- [ ] **Parameter descriptions** – Document all parameters, return values, exceptions
- [ ] **Usage examples** – At least one example per major API
- [ ] **Thread safety notes** – Document thread safety for each class

### User Guides
- [ ] **Getting started guide** – Simple producer/consumer example
- [ ] **Configuration guide** – All config options explained
- [ ] **Error handling guide** – How to handle failures at each API level
- [ ] **Migration guide** – From C API to C++, from primitive to transaction API

### Examples
- [ ] **Modernize producer example** – Use latest transaction API
- [ ] **Modernize consumer example** – Use iterator and transaction API
- [ ] **Add recovery example** – Show how to detect and recover from errors
- [ ] **Add flexible zone example** – Show typed flexible zone usage

---

## API Stability

### Breaking Changes (Major Version)
Track breaking changes for future major version bump:
- Removing Layer 1.75 (SlotRWAccess) ✅ Done in v1.0
- Config validation (require explicit parameters) ✅ Done in v1.0
- Structured buffer alignment change (compatibility break) - Planned for v2.0

### Deprecation Candidates
None currently. Maintain stable API for v1.x.

### Experimental APIs
Mark clearly as experimental, subject to change:
- Flexible zone by-name access (when added)
- Async API variants (when added)
- Batch operations (when added)

---

## Related Work

- **RAII Layer** (`docs/todo/RAII_LAYER_TODO.md`) – Transaction API is part of public API
- **Testing** (`docs/todo/TESTING_TODO.md`) – API surface needs comprehensive tests
- **Platform** (`docs/todo/PLATFORM_TODO.md`) – Cross-platform API consistency

---

## Recent Completions

### 2026-02-23
- ✅ **LoopPolicy Pass 2: ContextMetrics at DataBlock Pimpl (HEP-CORE-0008)**
  - Added `LoopPolicy` enum + `ContextMetrics` struct to `data_block.hpp`
  - `DataBlockProducerImpl` / `DataBlockConsumerImpl` gain `loop_policy`, `period_ms`,
    `metrics_`, `t_iter_start_`, `t_acquire_done_` fields
  - Timing injected in `acquire_write_slot()` / `acquire_consume_slot()` (Domains 2+3)
  - Work-time measured in `release_write_slot()` / `release_consume_slot()`
  - `set_loop_policy()`, `metrics()`, `clear_metrics()` implemented on both classes
  - `TransactionContext::metrics()` + `now()` const pass-through added
  - `actor_host.cpp`: wires `set_loop_policy()` from `interval_ms`, `clear_metrics()` at role start
  - `actor_api.cpp` + `actor_module.cpp`: `api.metrics()` returns Python dict (Domains 2+3+4)
  — `src/utils/data_block.cpp`, `src/include/utils/data_block.hpp`,
    `src/include/utils/transaction_context.hpp`,
    `src/actor/actor_host.cpp`, `src/actor/actor_api.cpp`, `src/actor/actor_module.cpp`
  **Deferred (Pass 3)**:
  - `SlotIterator::operator++()` sleep logic for RAII FixedRate path
  - `"loop_policy"` + `"period_ms"` in `ProducerOptions` / `ConsumerOptions` (hub_producer/consumer.hpp)
  - Tests for LoopPolicy/ContextMetrics (no tests exist yet — see TESTING_TODO)
  **488/488 passing.**
- ✅ **UAF fix in `timedShutdown()`** — `completed` and `ex_ptr` were stack-allocated in `timedShutdown()`
  but captured by reference in the thread lambda; after `thread.detach()` the function returned and
  destroyed them, leaving the detached thread with dangling references (UAF/UB on write).
  Fix: wrapped both in a `shared_ptr<SharedState>` so the detached thread keeps the allocation alive.
  This was the root cause of the intermittent `LifecycleTest.IsFinalizedFlag` timeout under load.
  — `src/utils/lifecycle.cpp` (`timedShutdown()`)

### 2026-02-17 (DataBlock three-mode constructor + WriteAttach)

- ✅ **`DataBlockOpenMode` enum added** — `Create` / `WriteAttach` / `ReadAttach` modes in
  `data_block.hpp`; replaces `bool m_is_creator` in internal `DataBlock` class.
  Creator mode still owns and unlinks the segment; WriteAttach and ReadAttach do not.
  — `src/include/utils/data_block.hpp`, `src/utils/data_block.cpp`
- ✅ **`attach_datablock_as_writer_impl()` implemented** — Mirrors consumer validation
  (shared_secret, header layout hash, config checksum, both schemas).  Returns
  `DataBlockProducer` attached in WriteAttach mode (no unlink on destroy).
  — `src/include/utils/data_block.hpp`, `src/utils/data_block.cpp`
- ✅ **Timeout named constants** — `TIMEOUT_IMMEDIATE` (0), `TIMEOUT_DEFAULT` (100 ms),
  `TIMEOUT_INFINITE` (-1) added to `data_block.hpp` near `DataBlockOpenMode` enum.
  — `src/include/utils/data_block.hpp`
- ✅ **`ScopedDiagnosticHandle` alias** — `using ScopedDiagnosticHandle = std::unique_ptr<DataBlockDiagnosticHandle>`
  added near `open_datablock_for_diagnostic`; documented as canonical RAII pattern.
  — `src/include/utils/data_block.hpp`
- ✅ **4 new WriteAttach tests** — `DatahubWriteAttachTest`: basic roundtrip, secret
  validation, schema validation, segment persistence after writer detach.
  — `tests/test_layer3_datahub/test_datahub_write_attach.cpp`

### 2026-02-17 (docs audit — resolved issues verified)
- ✅ **API_ISSUE_NO_CONFIG_OVERLOAD resolved** — The dangerous no-config template overload
  `find_datablock_consumer<FlexZoneT, DataBlockT>(hub, name, secret)` (schema validated but
  NOT config/sizes) has been removed. The only template overload now requires `expected_config`
  (line 1396, `data_block.hpp`). All template consumers enforce full schema+config validation.
- ✅ **Deprecated single-schema template declarations removed** — Phase 3 single-schema
  templates (`create_datablock_producer<T>` / `find_datablock_consumer<T>` without dual schema)
  are gone; comment at line 1418 is a placeholder with empty section.
- ✅ **Obsolete code (`DataBlockSlotIterator`, `with_next_slot`, `LegacyTransactionContext`)
  removed** — None of these symbols exist in `src/`. Confirmed by grep search.

### 2026-02-17 (DRAINING policy reachability documented and tested)

- ✅ **DRAINING unreachability for ordered policies** — Proved (and verified in code) that
  `SlotState::DRAINING` is structurally unreachable for `Single_reader` and `Sync_reader`:
  ring-full check (`write_index - read_index < capacity`) fires **before** `write_index.fetch_add(1)`;
  if reader holds slot K then `read_index ≤ K`, making the ring-full condition impossible to
  pass. DRAINING is a `Latest_only`-only live mechanism.
  — `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` § 11, `docs/IMPLEMENTATION_GUIDANCE.md` Pitfall 11
- ✅ **2 new policy-barrier tests** — `SingleReaderRingFullBlocksNotDraining` and
  `SyncReaderRingFullBlocksNotDraining` verify `writer_reader_timeout_count == 0` and no slot
  in DRAINING state when a ring-full timeout occurs (7 draining tests total, 358 overall).
  — `tests/test_layer3_datahub/`

### 2026-02-17 (DRAINING state machine implemented)

- ✅ **`SlotState::DRAINING` activated** — `acquire_write()` now enters DRAINING when wrapping
  a COMMITTED slot; drain timeout restores COMMITTED; recovery path restores COMMITTED (not FREE).
  New readers automatically rejected (slot_state != COMMITTED → NOT_READY). Eliminates
  reader-race events on wrap-around. — `src/utils/data_block.cpp`,
  `src/utils/data_block_recovery.cpp`, `src/include/utils/data_block.hpp`
- ✅ **DRAINING tests** — 5 new `DatahubSlotDrainingTest` scenarios verify state machine:
  DRAINING entered, new readers rejected, resolves after release, timeout restores COMMITTED,
  zero reader races on clean wraparound. — `tests/test_layer3_datahub/`
- ✅ **Protocol doc updated** — State machine and producer flow updated with DRAINING transitions.
  — `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md`

### 2026-02-17 (all code review items resolved)

- ✅ **[A-6] `high_resolution_clock` inconsistency** — Replaced with
  `platform::monotonic_time_ns()` in `logger.cpp:87` and `format_tools.cpp:100`
- ✅ **[A-7] `SlotState::DRAINING` undocumented** — Comment updated: active semantics documented
  — `src/include/utils/data_block.hpp`
- ✅ **[CONC-1b] `unlock()` clearing order** — Verified: `owner_pid == 0` is the
  authoritative "lock free" signal; ordering invariant documented in code
  — `src/utils/shared_memory_spinlock.cpp`
- ✅ **[A-4] Shutdown timeout no-op** — Verified: redesigned to real detachable threads,
  not `std::async`; comment in `lifecycle.cpp:37` confirms — `src/utils/lifecycle.cpp`
- ✅ **[A-5] Handle destructors silent errors** — Verified: `LOGGER_WARN` emitted inside
  `release_write_handle()` for checksum failures — `src/utils/data_block.cpp`
- ✅ **[A-9] namespace inside extern "C"** — Verified: namespace placed before `extern "C"`
  block with comment — `src/include/utils/slot_rw_coordinator.h`
- ✅ **[Q-9] Heartbeat helpers not centralized** — Verified: `is_producer_heartbeat_fresh()`
  uses `producer_heartbeat_id_ptr()` / `producer_heartbeat_ns_ptr()` helpers

### 2026-02-17 (code review resolution)

- ✅ **[CONC-1] Zombie lock reclaim CAS** — `SharedSpinLock` zombie reclaim now uses
  `compare_exchange_strong` instead of plain `store` — `src/utils/shared_memory_spinlock.cpp`
- ✅ **[A-2] `flexible_zone_size` size_t → uint32_t** — ABI-fixed in `SharedMemoryHeader`
- ✅ **[A-3] Enum fixed underlying types** — `DataBlockPolicy : uint32_t`,
  `ConsumerSyncPolicy : uint32_t` — `src/include/utils/data_block.hpp`
- ✅ **[pImpl] `SharedSpinLock::m_name` ABI fix** — `std::string` replaced with
  `char m_name[256]` — `src/include/utils/shared_memory_spinlock.hpp`
- ✅ **[C4251/C4324] MSVC export warnings** — Pragmas added in `message_hub.hpp`,
  `data_block.hpp` — Windows compatibility
- ✅ **[Q-10] `update_reader_peak_count` TOCTOU** — Fixed with `compare_exchange_weak` loop

### 2026-02-14
- ✅ Documented all recovery error codes in recovery_api.hpp
- ✅ Unified metrics API with state snapshot fields

### 2026-02-13
- ✅ Documented release_write_slot return values and idempotent behavior
- ✅ Documented slot handle lifetime contract in data_block.hpp
- ✅ Added get_spinlock and spinlock_count to public API

### 2026-02-12
- ✅ Transaction guard API implemented and documented
- ✅ Iterator API refined (try_next, seek_to, seek_latest)

---

## Notes

### API Evolution Strategy

1. **Additive changes only** in minor versions
2. **Deprecation** with at least one minor version notice
3. **Breaking changes** only in major versions
4. **Experimental** APIs clearly marked

### API Review Checklist

Before adding any new public API:
- [ ] Is it necessary? Can existing API cover this?
- [ ] Is the naming consistent with existing APIs?
- [ ] Is the error handling strategy clear?
- [ ] Is thread safety documented?
- [ ] Is lifetime and ownership clear?
- [ ] Are there tests covering the new API?
- [ ] Is it documented with examples?

### Common API Patterns

**Resource acquisition**:
```cpp
// Factory pattern for complex objects
auto obj = create_thing(...);

// Optional for fallible operations
auto opt = try_lock(...);
if (opt.has_value()) { use(*opt); }

// nullptr for expected failures
auto handle = acquire_slot(...);
if (handle) { use(*handle); }
```

**Error reporting**:
```cpp
// Throw for contract violations
if (invalid_config) throw std::invalid_argument("...");

// Return false for expected failures
bool success = operation();

// Return enum for recovery operations
RecoveryResult result = recover();
```
