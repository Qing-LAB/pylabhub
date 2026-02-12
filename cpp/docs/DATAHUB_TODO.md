# Data Exchange Hub - Implementation TODO

**Last Updated:** 2026-02-12
**Priority Legend:** 🔴 Critical | 🟡 High | 🟢 Medium | 🔵 Low

**Doc policy:** This file is the **single source of truth** for execution. When tracking or executing the plan, update and follow this document. Other docs (test plan, HEP, critical review) provide rationale and detail; they do **not** override priorities or the checklist here. See `docs/DOC_STRUCTURE.md` for the full documentation layout.

**Rationale documents (do not duplicate here):**
- **Test plan & MessageHub review:** `docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` — phased test plan (Phase A–D), test infrastructure needs, and MessageHub code review (C++20, abstraction, logic, DataHub integration). All rationale and descriptions for the testing and MessageHub items below live there. **Cross-platform is integrated:** tests and review consider all supported platforms.
- **Critical review & design:** `docs/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md`, `docs/DATAHUB_DESIGN_DISCUSSION.md` — design decisions, integrity policy, flexible zone semantics. **Cross-platform is part of review/design:** behavior and APIs are defined and verified for Windows, Linux, macOS, FreeBSD; see “Cross-Platform and Behavioral Consistency” below.

---

## Recent completions (2026-02-12)

- **ConsumerSyncPolicy** – Added `ConsumerSyncPolicy` enum (Latest_only, Single_reader, Sync_reader) to config and header. Implemented: Latest_only = read commit_index only; Single_reader = one shared read_index, consumer reads in order, writer blocks when (write_index - read_index) >= capacity; Sync_reader = per-consumer positions in reserved_header, read_index = min(positions), writer backpressure. Tests: policy_latest_only, policy_single_reader, policy_sync_reader.
- **Slot-finding DRY** – Extracted `get_next_slot_to_read(header, last_seen, heartbeat_slot)` in data_block.cpp; both `DataBlockConsumer::acquire_consume_slot(int)` and `DataBlockSlotIterator::try_next(int)` use it. Single place for policy-based next-slot logic.
- **Writer timeout diagnostics** – In `acquire_write`: on timeout waiting for write_lock, log "timeout while waiting for write_lock" with pid/owner; on timeout waiting for reader_count to drain, log "timeout while waiting for readers to drain (possible zombie reader)" with pid/reader_count. Library provides probes; recovery remains explicit (data_block_recovery, release_zombie_readers).
- **DataBlockMutex try_lock_for** – Added `DataBlockMutex::try_lock_for(int timeout_ms)` (POSIX: pthread_mutex_timedlock + EOWNERDEAD handling; Windows: WaitForSingleObject with timeout). Zombie recoverer worker uses try_lock_for(5000) so test never hangs; on timeout logs and exits 1.
- **ZombieOwnerRecovery under TSan** – Test skips when `__SANITIZE_THREAD__` (robust mutex EOWNERDEAD recovery triggers TSan false positive "unlock of unlocked mutex").
- **MessageHub "Not connected"** – Downgraded to LOGGER_WARN so in-process tests without broker do not fail ExpectWorkerOk (no "ERROR" in stderr).
- **High-load integrity test** – high_load_single_reader worker: Single_reader policy, ring capacity 4, scaled iterations (50k/5k); producer writes monotonic uint64_t, consumer asserts slot_id and payload in order. Test: HighLoadSingleReaderIntegrity.
- **MessageHub Phase C groundwork (no-broker path)** – `test_message_hub.cpp` + `messagehub_workers.cpp` now cover lifecycle-only and **no-broker** behavior: connect/disconnect idempotence, send/receive when not connected, register_producer/discover_producer when broker is unavailable, and JSON-parse failure paths. These tests use the existing worker framework and Logger/Lifecycle modules; they exercise the C++20 pImpl design without introducing test-only hooks.

---

## Recent completions (2026-02-11)

- **Test coverage summary** – Added “Current test coverage (Layer 3 DataHub)” to `docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`: table mapping plan areas to tests/workers; gaps (Phase C, D, recovery scenario) noted.
- **MessageHub JSON safety** – message_hub.cpp: register_producer and discover_producer use try/catch around json::parse; .contains("status")/"message" before access; discover_producer validates presence and types of shm_name, schema_hash, schema_version before .get<>(); missing/invalid fields return false/nullopt with log instead of throwing.
- **DataBlock/slot error-handling tests**: test_error_handling.cpp + error_handling_workers: acquire_consume_slot timeout → nullptr, find_consumer wrong secret → nullptr, release_write_slot/release_consume_slot invalid handle → false, write/commit/read bounds → false, double release idempotent, slot_iterator try_next timeout → !ok. Ensures recoverable errors are handled without UB; contract violations (e.g. handle after destroy) remain documented.
- **Phase B – Slot protocol tests**: test_slot_protocol (WriteReadSucceedsInProcess, ChecksumUpdateVerifySucceeds, LayoutWithChecksumAndFlexibleZoneSucceeds, DiagnosticHandleOpensAndAccessesHeader); all passing.
- **Recovery/diagnostics tests**: test_recovery_api.cpp + recovery_workers for heartbeat_manager, integrity_validator, recovery_api (datablock_is_process_alive), slot_diagnostics, slot_recovery. Workers run with Logger+CryptoUtils+MessageHub lifecycle; create DataBlock, exercise APIs; ExpectWorkerOk with expected_stderr_substrings (MessageHub logs "Not connected" at ERROR when no broker). SlotDiagnostics::is_valid() implementation added.
- **FileLock test cleanup**: GetTempLockPath pre-cleanup fixed to use `get_expected_lock_fullname_for` instead of incorrect `.lock.` prefix.
- **base_file_sink.cpp**: Fixed (void)m_use_flock → (void)use_flock for correct unused-parameter suppression on Windows.
- **plh_heartbeat_manager.hpp** – Moved to utils/heartbeat_manager.hpp (per plh_* convention).
- **Recovery error codes** – Documented in recovery_api.hpp: diagnose return values (0, -1..-5), RecoveryResult enum.
- **release_write_slot** – Documented in data_block.hpp: when it returns false (invalid handle, checksum update failure); idempotent behavior.
- **Slot handle lifetime contract** – Documented in data_block.hpp, data_block.cpp, DATAHUB_DATABLOCK_CRITICAL_REVIEW.md, IMPLEMENTATION_GUIDANCE.md: release or destroy all SlotWriteHandle/SlotConsumeHandle before destroying producer/consumer; otherwise use-after-free.
- **DataBlockLayout** – Centralized offset/layout calculation in data_block.cpp: DataBlockLayout struct with from_config/from_header, slot_checksum_base(), validate(); creator/attacher/checksum/diagnostic handle use it. New test LayoutWithChecksumAndFlexibleZoneSucceeds.
- **Slot checksum layout** – Fixed overlap: layout is now Header | SlotRWStates | SlotChecksums | FlexibleZone | StructuredData when enable_checksum; checksum impl uses correct offset; ChecksumUpdateVerifySucceeds test re-enabled.

---

## Recent completions (2026-02-10)

- **Platform shm_***: `ShmHandle`, `shm_create()`, `shm_attach()`, `shm_close()`, `shm_unlink()` in `plh_platform.hpp`/`platform.cpp`. `data_block.cpp` and `open_datablock_for_diagnostic` refactored to use them.
- **Platform**: `pylabhub::platform::is_process_alive(uint64_t)` and `monotonic_time_ns()` in place; `SharedSpinLock::is_process_alive()` removed; callers use platform. `monotonic_time_ns()` uses `std::chrono::steady_clock`.
- **Slot RW C API**: Full C API in `data_block.cpp` (`slot_rw_acquire_write`, `slot_rw_commit`, `slot_rw_release_write`, `slot_rw_acquire_read`, `slot_rw_validate_read`, `slot_rw_release_read`, metrics/reset) with optional `SharedMemoryHeader*` for metrics.
- **Recovery/diagnostics**: CMake re-enabled `data_block_recovery`, `slot_diagnostics`, `slot_recovery`, `heartbeat_manager`, `integrity_validator`. Public `DataBlockDiagnosticHandle` and `open_datablock_for_diagnostic()`; recovery uses them instead of internal `DataBlock`. `datablock_validate_integrity` and related calls fixed (MessageHub reference, `shared_secret` uint64_t, RecoveryResult logging).
- **MessageHub**: `zmq::recv_multipart` return value checked to fix `-Wunused-result`; header `send_message` signature aligned with implementation (message_type, json_payload).
- **Recovery/diagnostics headers**: Moved from top-level `plh_*` to `utils/` (recovery_api.hpp, slot_diagnostics.hpp, slot_recovery.hpp, integrity_validator.hpp). `plh_*` prefix reserved for umbrella headers only (see CONTRIBUTING.md).
- **Duplication/redundancy**: Removed get_current_pid wrapper in data_block; spinlock and recovery use platform monotonic time; recovery uses open_for_recovery() and set_recovery_timestamp(); spinlock header comment and includes fixed.
- **Cross-platform**: Documented in DATAHUB_DATABLOCK_CRITICAL_REVIEW.md §2.3.

---

## Remaining plan (summary)

Use this list to track what’s left; details live in the sections below and in the rationale documents above. **Testing and MessageHub:** rationale and phased plan are in `docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` (refer to it; do not duplicate there).

### API and documentation
- [x] **release_write_slot** – Documented in data_block.hpp: returns false if handle invalid or checksum update failed; idempotent (already-released returns true).
- [x] **Slot handle lifetime contract** – Documented in data_block.hpp and data_block.cpp: release or destroy all SlotWriteHandle/SlotConsumeHandle before destroying producer/consumer; otherwise use-after-free.
- [x] **Recovery error codes** – Added to `utils/recovery_api.hpp`: diagnose 0, -1..-5; RecoveryResult enum documented inline.
- [ ] **DataBlockMutex** – When reintegrating: decide factory vs direct ctor and document exception vs optional/expected.

### Functionality and design (review + agree)
- [ ] **DataBlockMutex not used by DataBlock** – Reintegrate for control zone (spinlock alloc, etc.) when DataHub integration is done; see DataBlockMutex follow-ups below.
- [ ] **Consumer flexible_zone_info (see docs/FLEXIBLE_ZONE_INITIALIZATION.md)** – Document that it’s only populated when using factory with expected_config; enforce or clarify for flexible-zone-by-name access.
- [ ] **Integrity repair path** – Optional: low-level repair using only DataBlockDiagnosticHandle (no full producer/consumer) to avoid broker lifecycle side effects.

### DataBlockMutex follow-ups (on integration)
- [ ] **pthread_mutex_destroy** – Intentionally omitted (documented in destructor); no change unless design changes.
- [ ] **Constructor exception behavior** – Revisit when integrating; consider factory returning std::optional or std::expected.

### Phase 1 (schema)
- [ ] **1.4 Broker schema registry** – Broker stores schema_hash, schema_version, schema_name; DISC_RESP includes them; optional GET_SCHEMA API.
- [ ] **1.5 Schema versioning policy** – Compatibility rules, optional is_schema_compatible(), migration docs.

### Phase 2 (verify / complete)
- [ ] **2.1 Slot RW C API** – Verify existing implementation matches TODO and HEP; update checklist if already done.
- [ ] **2.2 Template wrappers** – with_typed_write/with_typed_read; type safety checks.
- [ ] **2.3 Transaction guards** – WriteTransactionGuard, ReadTransactionGuard; exception safety.

### Phase 3 and later
- [ ] **3.x Factory/lifecycle** – Create DataBlockMutex for control zone when reintegrating; align with Phase 3 priorities in DATAHUB_TODO.
- [ ] **Testing** – Run same tests on all supported platforms; avoid platform-only skip.
- [ ] **Deployment** – Per DATAHUB_TODO deployment phase.

### Foundational APIs used by DataBlock (rationale & coverage: `docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` Part 0)
- [x] **Platform shm_* tests** – Added in test_layer0_platform/test_platform_shm.cpp: create, attach (same process), read/write, close, unlink; portable names; SHM_CREATE_UNLINK_FIRST. (Rationale: doc Part 0.)
- [x] **SharedSpinLock tests** – Added in test_layer2_service/test_shared_memory_spinlock.cpp: try_lock_for, lock/unlock, timeout, recursion, SharedSpinLockGuard(Owning), two-thread mutual exclusion with state in shm. (Rationale: doc Part 0. Zombie reclaim: covered indirectly via platform is_process_alive; optional multi-process spinlock test later.)
- **Already covered:** Platform (get_pid, monotonic_time_ns, elapsed_time_ns, is_process_alive) in test_platform_core; Backoff in test_backoff_strategy; Crypto (BLAKE2b, verify) in test_crypto_utils; Lifecycle in test_lifecycle; Schema BLDS in test_schema_blds.

### DataHub protocol testing (rationale & description: `docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` Part 1)
- [x] **Phase A – Protocol/API correctness** – test_phase_a_protocol: flexible_zone_span empty when no zones / non-empty when zones; checksum false when no zones / true when valid; consumer without expected_config gets empty zones, with expected_config gets zones; [x] schema (test_schema_validation). See doc Part 1.1 and 1.4.
- [x] **Phase B – Slot protocol in one process** – test_slot_protocol: write_read, checksum (Enforced), layout_smoke (checksum + flexible zone), diagnostic_handle. DataBlockLayout + slot checksum region fix. See doc Part 1.2 and 1.4.
- [x] **DataBlock/slot error handling** – test_error_handling: recoverable failures return false/nullptr/empty (acquire timeout, wrong secret, invalid handle release, write/commit/read bounds, double-release idempotent, iterator try_next timeout). Ensures no segfault on expected error paths; unsafe/unrecoverable cases (e.g. handle used after producer destroyed) remain contract violations.
- [x] **Writer timeout metrics split** – Added `writer_lock_timeout_count` and `writer_reader_timeout_count`; exposed via DataBlockMetrics and slot_rw_get_metrics/reset; new worker + test: writer_timeout_metrics_split.
- [ ] **Phase C – MessageHub and broker** – **Complete tests for MessageHub and broker.**
  - **C.0 No-broker behavior (DONE)** – `test_message_hub.cpp` + `messagehub_workers.cpp` cover: lifecycle init, connect/disconnect idempotence, send/receive when not connected, register_producer / discover_producer when broker is unavailable, and JSON parse/shape errors. These tests reuse the existing worker framework and Logger/Lifecycle modules and validate the C++20 pImpl design without any test-only hooks.
  - **C.1 With-broker happy path** – Add workers + tests that start a minimal in-process broker (or dedicated test binary) implementing REG_REQ/DISC_REQ with the current JSON schema (including `shm_name`, `schema_hash`, `schema_version`). Verify: `register_producer` succeeds and persists state; `discover_producer` returns `ConsumerInfo` matching what was registered; one producer/consumer pair performs a single write/read through DataBlock using the discovered `shm_name` and schema fields.
  - **C.2 Error and timeout paths with broker** – Tests for broker-side failures and timeouts: broker returns structured error JSON; broker not reachable; malformed responses. MessageHub must surface these via `std::optional` and logging, not process aborts. Reuse the test-broker fixture so no new production-only codepaths are added.
  - **C.3 Schema metadata contract** – Tests that registration and discovery carry `schema_hash` and `schema_version` consistently end-to-end, and that DataBlock factories use the discovered schema fields as documented in the HEP and schema validation design.
- [ ] **Phase D – Concurrency and multi-process** – Concurrent readers; writer timeout; TOCTTOU and wrap-around; zombie reclaim; DataBlockMutex. See doc Part 1.3 and 1.4.
- [ ] **Recovery scenario tests** – **Deferred.** See “Recovery (deferred)” below. Beyond smoke we may want: zombie lock reclaim, datablock_validate_integrity on corrupted block, datablock_diagnose_slot stuck detection — after recovery policy is defined.
- [x] **Test infrastructure** – enable test_schema_validation in CMake; converted to IsolatedProcessTest + schema_validation_workers (schema match / mismatch). DataBlockTestFixture, test broker: future. See doc Part 1.5.

### MessageHub follow-ups (rationale & description: `docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` Part 2)
- [x] **MessageHub JSON safety** – register_producer/discover_producer: parse in try/catch (return false/nullopt on parse exception); .contains("status")/"message" before access; discover_producer requires .contains() and type checks (is_string/is_number_unsigned) for shm_name, schema_hash, schema_version before .get<>().
- [ ] **MessageHub receive helper** – Extract shared recv path (poll, recv_multipart, size check, last frame) into private helper to remove duplication between send_message and receive_message.
- [ ] **MessageHub [[nodiscard]] and docs** – Add [[nodiscard]] to send_message, receive_message, discover_producer, connect, register_producer; document broker contract (REG_REQ/DISC_REQ, JSON shape) in one place.
- [ ] **register_consumer** – Implement when broker protocol for consumer registration is defined; add tests.

### Optional code split (when useful)
- [x] **DataBlockLayout** – Centralized layout calculation (DataBlockLayout struct, from_config/from_header, stored in DataBlock, used by creator/attacher/checksum/diagnostic handle). Debug validate() asserts invariants.
- [ ] **Header schema** – Move get_shared_memory_header_schema_info / validate_header_layout_hash to dedicated TU (e.g. data_block_header_schema.cpp) for clear schema/layout boundary.
- [ ] **Slot RW TU** – Move acquire_write/commit_write/… and C wrappers to data_block_slot_rw.cpp to shrink data_block.cpp.
- [ ] **Factory TU** – Move create_datablock_producer_impl, find_datablock_consumer_impl to data_block_factory.cpp.
- [ ] **Checksum TU** – Optional; move checksum helpers to data_block_checksum.cpp with header/buffer-only interface for repair without full producer.

### Other
- [x] **plh_heartbeat_manager.hpp** – Moved to utils/heartbeat_manager.hpp (per plh_* convention).

### Next step plan (immediate)

```mermaid
flowchart TD
    A[Phase A/B: Protocol & slot tests ✅] --> B[Writer timeout metrics split ✅]
    B --> C[Phase C: MessageHub + broker tests]
    C --> D[Phase D: Concurrency & multi-process]
    B --> S[Priority 1.4: Broker schema registry]
    D --> R[Recovery scenarios (deferred)]
```

1. **Phase C – MessageHub + broker tests**
   - Implement Phase C from `docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`: MessageHub unit behavior (connect/disconnect, send/receive when disconnected, parse errors), and with broker: register_producer, discover_producer, one write/read.
   - Reevaluate existing MessageHub workers (`test_layer3_datahub/workers/messagehub_workers.cpp` or equivalents) and update/replace to match current `message_hub.cpp` API and protocol.
2. **Priority 1.4 – Broker schema registry**
   - Define and implement broker-side registry behavior for `schema_hash`, `schema_version`, and `schema_name`; ensure DISC responses and any GET_SCHEMA-style API match the client expectations in `message_hub.cpp` and DataBlock create/find.
3. **Phase D – Concurrency & multi-process**
   - Add tests for concurrent readers, writer timeout behavior, TOCTTOU and wrap-around, zombie reclaim, and DataBlockMutex in multi-process scenarios, per Part 1.3 of the test plan.

### Recovery (deferred)

Recovery semantics need **further investigation** before we define and test them fully:

- **Clear definition needed:** What should be **recovered** (e.g. stuck slot, orphaned lock) vs what should **fail** (e.g. corrupted header, unreachable producer), and what is the **correct protocol/policy** to enforce (when to reset, when to log-only, when to refuse).
- **For now:** Keep recovery **simple**. Current smoke (test_recovery_api: heartbeat, integrity_validator, slot_diagnostics, slot_recovery, datablock_is_process_alive) stays; deeper recovery behavior and “recovery scenario” tests (zombie reclaim, corrupted block, stuck slot) are **left for later** once the above is defined.
- Do not expand recovery logic or add recovery scenario tests until the policy (recover vs fail, protocol) is agreed and documented.

---

## Quick Status

| Phase | Status | Progress | Target |
|-------|--------|----------|--------|
| **P1-P8 Design** | ✅ Complete | 100% | Done |
| **P9 Design** | ✅ Complete | 100% | Done |
| **Core Implementation** | 🟡 In Progress | 30% | Week 2-3 |
| **P9 Implementation** | ⏳ Not Started | 0% | Week 1 |
| **Testing** | 🟡 Planned | 0% | Week 4 (plan: docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md) |
| **Deployment** | ⏳ Not Started | 0% | Week 5 |

---

## Cross-Platform and Behavioral Consistency

**Principle:** Cross-platform is **integrated into review and design**, not only into implementation. Every feature, API, and test plan must consider all supported platforms (Windows, Linux, macOS, FreeBSD). All DataHub behavior must be **consistent across platforms**. Platform-specific code belongs only in the platform layer; higher layers use a single, documented semantics.

### Rules

1. **Single abstraction layer**
   - Use `pylabhub::platform::*` for: PID, thread ID, monotonic time, process liveness, shared memory.
   - Use `plh_platform.hpp` macros: `PYLABHUB_PLATFORM_WIN64`, `PYLABHUB_IS_POSIX`, `PYLABHUB_PLATFORM_APPLE`, etc.
   - No direct `#ifdef _WIN32` or `#ifdef __linux__` in DataBlock, SlotRWCoordinator, or recovery code.

2. **Identical semantics**
   - Timeouts: always milliseconds; same meaning on all platforms.
   - PID 0: always "not alive" everywhere.
   - Error codes (e.g. `SlotAcquireResult`): same numeric values and meaning on all platforms.
   - Memory ordering: use `std::memory_order_acquire` / `release` (and seq_cst where required) so behavior is correct on both x86 and ARM.

3. **Document platform quirks**
   - Where behavior differs by OS (e.g. POSIX `kill(pid,0)` EPERM → "alive"), document in platform implementation and keep semantics stable at the API boundary (e.g. "alive" vs "not alive" is consistent).

4. **Testing**
   - Run the same unit and integration tests on all supported platforms; avoid platform-only code paths that skip tests.

5. **Clocks**
   - Use `std::chrono::steady_clock` (or platform `monotonic_time_ns()`) for timeouts and elapsed time so behavior is monotonic and consistent across platforms.

---

## Phase 0: Code Refactoring and Service Integration

### 🔴 PRIORITY 0.1: Refactor Helper Functions to Service Modules

**Rationale**: Many helper functions in DataHub could be generalized and moved to appropriate service modules for better code reuse, separation of concerns, and independent testing.

#### Platform Service Enhancements (`src/utils/platform.cpp`)

- [x] **Move PID liveness check** from `SharedSpinLock::is_process_alive()` to `pylabhub::platform::is_process_alive(uint64_t pid)` (✅ 2026-02-10)
  - Done: In `platform.cpp`; `SharedSpinLock::is_process_alive()` removed; callers use platform.
  - Signature: `PYLABHUB_UTILS_EXPORT bool pylabhub::platform::is_process_alive(uint64_t pid);`
  - Benefits: Reusable by FileLock, other IPC modules

- [x] **Move monotonic timestamp** from anonymous namespace to platform utilities (✅ 2026-02-10)
  - Done: `pylabhub::platform::monotonic_time_ns()` in `platform.cpp` using `std::chrono::steady_clock`.
  - Signature: `PYLABHUB_UTILS_EXPORT uint64_t pylabhub::platform::monotonic_time_ns();`
  - Benefits: Consistent timestamp source across modules

- [x] **Add platform-specific shared memory utilities** (✅ 2026-02-10)
  - Done: `pylabhub::platform::ShmHandle`, `shm_create()`, `shm_attach()`, `shm_close()`, `shm_unlink()` in `plh_platform.hpp` and `platform.cpp`. `data_block.cpp` and `open_datablock_for_diagnostic` refactored to use them.
  - Optional follow-up: `data_block_mutex.cpp` uses POSIX-specific `O_EXCL` and retry logic; could be adapted to use platform API if needed.

#### Debug Info Enhancements (`src/utils/debug_info.cpp`)

- [x] **Timeout visibility: metrics, not per-event log** (✅ 2026-02-10)
  - Done: Timeouts are not logged per event (individual timeouts add little diagnostic value; log should report serious issues or aggregate/context useful for diagnosis). Writer and reader timeout counts are already updated in `SharedMemoryHeader` (`writer_timeout_count`, `reader_timeout_count`). Operators and health checks can use these metrics; if accumulated timeouts run high, then investigate (e.g. recovery/diagnostics, logs from other layers).

- [x] **Magic number validation in data_block module** (✅ 2026-02-10)
  - Done: `pylabhub::hub::detail::DATABLOCK_MAGIC_NUMBER` and `detail::is_header_magic_valid(magic_ptr, expected)` inline in `data_block.hpp`. Kept in the module that uses it; not in debug_info.

#### Crypto Utilities Module (NEW)

- [x] **Create `src/utils/crypto_utils.cpp`** (✅ already implemented)
  - Done: `compute_blake2b()`, `verify_blake2b()`, `generate_random_bytes()`, `generate_shared_secret()` in `crypto_utils.cpp`; used by `data_block.cpp`.
  - Benefits: Single point for libsodium initialization, reusable checksums

- [x] **Add header `src/include/utils/crypto_utils.hpp`** (✅ already implemented)
  - Done: Exports above; `GetLifecycleModule()` for libsodium init on startup (used by tests and schema validation).

#### Backoff Strategy Module (NEW)

- [x] **Create `src/include/utils/backoff_strategy.hpp`** (✅ already implemented)
  - Done: Header-only with `ExponentialBackoff`, `ConstantBackoff`, `NoBackoff`, `AggressiveBackoff`, and `backoff(iteration)`.
  - Used by: `data_block.cpp` (pylabhub::utils::backoff), `shared_memory_spinlock.cpp` (ExponentialBackoff).
  - Benefits: Reusable for all spin loops (FileLock could adopt later)

**Status (2026-02-10)**: Phase 0 platform refactoring complete. `shm_*` API in place; `data_block.cpp`, `open_datablock_for_diagnostic`, and `data_block_mutex.cpp` all use platform shm_* API.
**Testing**: Unit tests for each new utility function

#### DataBlockMutex Follow-ups ( revisit when integrating into DataHub )

- [ ] **pthread_mutex_destroy**: Intentionally omitted. Mutex lives in shm; when segment is released (shm_close/unlink), memory is reclaimed—no leak. Calling destroy is unsafe: which process is "last" is unpredictable; destroy while another holds the lock is UB. See `data_block_mutex.cpp` destructor comment.
- [ ] **Constructor exception behavior**: Revisit when integrating DataBlockMutex into DataHub. Current design throws on failure (shm_create, pthread_init, etc.). Consider factory returning `std::optional` or `std::expected` if callers prefer non-throwing API or if embedded as member in DataBlock.

---

## Phase 1: P9 Schema Validation (Week 1)

### 🔴 PRIORITY 1.1: BLDS Schema Generation

- [x] **Define BLDS grammar and type ID mapping** (✅ already in `schema_blds.hpp`)
  - Done: Type ID mapping (f32, f64, i8-i64, u8-u64, arrays, nested structs), canonical BLDS string via `BLDSTypeID`, `BLDSBuilder`.

- [x] **Implement schema registration macros** (✅ already in `schema_blds.hpp`)
  - Done: `PYLABHUB_SCHEMA_BEGIN`, `PYLABHUB_SCHEMA_MEMBER`, `PYLABHUB_SCHEMA_END` with compile-time reflection.

- [x] **Implement `SchemaInfo` struct** (✅ already in `schema_blds.hpp`)
  - Done: `SchemaInfo` with name, blds, hash (32 bytes), version, struct_size; `compute_hash()`, `matches()`, `matches_hash()`.

- [x] **Implement hash computation** (✅ already implemented)
  - Done: `SchemaInfo::compute_hash()` uses `pylabhub::crypto::compute_blake2b_array(blds)` (crypto_utils module). Personalization can be added later if needed for domain separation.

**Estimated Effort**: 2 days
**Dependencies**: Phase 0 (crypto_utils module)
**Testing**: Test BLDS generation for various struct layouts

### 🔴 PRIORITY 1.2: Producer Schema Registration

- [x] **Update `SharedMemoryHeader`** (✅ already has `schema_hash[32]`, `schema_version`)
  - Padding/layout used by `get_shared_memory_header_schema_info()` and header layout hash.

- [x] **Update `create_datablock_producer()` overload with schema** (✅ already in `data_block.hpp/cpp`)
  - Done: Template overload and `create_datablock_producer_impl(..., schema_info)`; hash stored in header.

- [x] **Compute and store schema hash in header** (✅ already implemented)
  - Done: BLDS from schema, BLAKE2b-256 via `SchemaInfo::compute_hash()`, stored in `SharedMemoryHeader::schema_hash`.

- [x] **Include schema in broker registration** (✅ already in `message_hub.cpp` / producer info)
  - Done: Producer info includes `schema_hash`; `REG_REQ`/registration carries schema info.

**Estimated Effort**: 1 day
**Dependencies**: Priority 1.1 (BLDS implementation)
**Testing**: Test producer registration with schema

### 🔴 PRIORITY 1.3: Consumer Schema Validation

- [x] **Update `find_datablock_consumer()` overload with schema** (✅ already in `data_block.hpp/cpp`)
  - Done: Template overload and `find_datablock_consumer_impl(..., schema_info)`.

- [x] **Implement schema validation flow** (✅ in `find_datablock_consumer_impl`)
  - Done: Discover producer, compare header `schema_hash` with expected from local Schema; attach and verify hash in header.

- [x] **Throw `SchemaValidationException` on mismatch** (✅ in `schema_blds.hpp`)
  - Done: `SchemaValidationException` with expected_hash, actual_hash; `validate_schema_match` / `validate_schema_hash`.

- [x] **Update metrics**: Increment `SharedMemoryHeader::schema_mismatch_count` on failure (✅ 2026-02-10)

**Estimated Effort**: 1 day
**Dependencies**: Priority 1.2 (producer registration)
**Testing**: Test consumer rejection on schema mismatch

### 🟡 PRIORITY 1.4: Broker Schema Registry

- [ ] **Update broker to store schema metadata**
  - Current: Broker stores `shm_name`, `producer_pid`
  - Add: `schema_hash`, `schema_version`, `schema_name`

- [ ] **Update `DISC_RESP` message** to include schema info
  ```json
  {
    "type": "DISC_RESP",
    "shm_name": "datablock_sensor_12345",
    "schema_hash": "a1b2c3d4...",
    "schema_version": "2.0.0",
    "schema_name": "SensorHub.SensorData"
  }
  ```

- [ ] **Add schema query API** (optional, for tooling)
  - `GET_SCHEMA_REQ` / `GET_SCHEMA_RESP` messages
  - Return: Full BLDS string, hash, version

**Estimated Effort**: 1 day
**Dependencies**: Priority 1.3 (consumer validation)
**Testing**: Test broker schema registry operations

### 🟢 PRIORITY 1.5: Schema Versioning Policy

- [ ] **Define version compatibility rules**
  - Current: Strict hash matching (no tolerance)
  - Future: Semantic versioning (major break, minor backward-compatible)

- [ ] **Implement version checker** (optional)
  ```cpp
  bool is_schema_compatible(const SchemaVersion& producer,
                           const SchemaVersion& consumer);
  ```

- [ ] **Document migration procedures**
  - How to evolve schemas without breaking consumers
  - Add to Section 11 of HEP document

**Estimated Effort**: 0.5 days
**Dependencies**: Priority 1.4 (schema registry)
**Testing**: Test version compatibility logic

---

## Phase 2: Core SlotRWCoordinator Abstraction (Week 2)

- ### 🔴 PRIORITY 2.1: C API (Layer 0) – stabilize abstraction and tests
-
- [x] **Define C result enum and metrics struct** – `slot_rw_coordinator.h` exposes `SlotAcquireResult` (`SLOT_ACQUIRE_OK`, `SLOT_ACQUIRE_TIMEOUT`, `SLOT_ACQUIRE_NOT_READY`, `SLOT_ACQUIRE_LOCKED`, `SLOT_ACQUIRE_ERROR`, `SLOT_ACQUIRE_INVALID_STATE`) and `DataBlockMetrics`, matching the design intent in HEP §4.2 and the header layout in `SharedMemoryHeader`.
- [x] **Implement core C API over `SlotRWState`** – `data_block.cpp` implements:
  - `slot_rw_acquire_write`, `slot_rw_commit`, `slot_rw_release_write`
  - `slot_rw_acquire_read`, `slot_rw_validate_read`, `slot_rw_release_read`
  - `slot_acquire_result_string`, `slot_rw_get_metrics`, `slot_rw_reset_metrics`  
  These are thin `extern "C"` wrappers over the C++ helpers (`acquire_write`, `commit_write`, `release_write`, `acquire_read`, `validate_read`, `release_read`), using `pylabhub::platform::*` and `utils::backoff` only, so the slot protocol can be tested independently of `DataBlock`.
- [x] **Layer-0 focused tests for SlotRWState** – `test_layer2_service/test_slot_rw_coordinator.cpp`:
  - Allocates a single `SlotRWState` + synthetic `SharedMemoryHeader` in plain memory (no shm, no DataBlock).
  - Exercises writer/reader acquisition, generation capture, wrap-around detection, and metrics `get/reset` purely via the C API.
  - Verifies that core behaviour and metrics wiring are correct without involving `DataBlockProducer` / `DataBlockConsumer`.
- [ ] **TU split and layering guardrails** – Move the C API and its helpers into a dedicated TU (see “Slot RW TU” below) so:
  - Layer 0 (C API) depends only on `SlotRWState`, `SharedMemoryHeader`, `platform`, and `backoff`.
  - Higher layers (`DataBlock`, transaction guards, typed access) depend *on* this API instead of reimplementing slot logic, making tests and future bindings reuse the same core behavior.

**Goal:** treat SlotRWCoordinator as a **first-class lower layer** with its own tests and translation unit, so all higher-level tests (DataBlock, factories, MessageHub integration) benefit from a verified, reusable abstraction instead of ad-hoc per-call logic.

### 🟡 PRIORITY 2.2: C++ Template Wrappers (Layer 1.75) - `slot_rw_access.hpp`

- [ ] **Implement `with_typed_write<T>`**
  ```cpp
  template <typename T, typename Func>
  auto with_typed_write(DataBlockProducer& producer, int timeout_ms, Func&& func);
  ```

- [ ] **Implement `with_typed_read<T>`**
  ```cpp
  template <typename T, typename Func>
  auto with_typed_read(DataBlockConsumer& consumer, uint64_t slot_id, int timeout_ms, Func&& func);
  ```

- [ ] **Add type safety checks**
  - Verify `sizeof(T) <= slot_buffer_size`
  - Verify alignment requirements

**Estimated Effort**: 1 day
**Dependencies**: Priority 2.1 (C API)
**Testing**: Test typed access with various struct types
**Reference**: Section 5.3 of HEP-CORE-0002-DataHub-FINAL.md

### 🟡 PRIORITY 2.3: Transaction Guards (Layer 2) – polish and test

- [x] **Implement `WriteTransactionGuard` / `ReadTransactionGuard`** – Guards are implemented in `data_block.cpp` with pImpl-safe semantics: acquire in ctor, release in dtor, move-only, `slot()` returning `std::optional` by reference, and `noexcept` destructors.
- [ ] **Exception-safety tests and usage guidance** – Add targeted tests (e.g. `test_transaction_api.cpp`) that:
  - Verify guards always release slots when lambdas throw (including nested exceptions).
  - Confirm no exceptions escape guard destructors, and that `slot()` remains `noexcept` and safe to use across the shared-library boundary.
  - Document recommended usage patterns in `DATAHUB_IMPLEMENTATION_SUMMARY.md` and the HEP (e.g. “prefer guards/with_* helpers over manual acquire/release in application code”).

**Goal:** make the guard layer the **default entry-point** for C++ callers, with strong exception-safety guarantees, so tests and examples can be written against a clean RAII abstraction instead of raw handle management.

---

## Phase 3: DataBlock Factory and Lifecycle (Week 2)

### 🔴 PRIORITY 3.1: Producer Factory Implementation

- [ ] **Implement `create_datablock_producer()` (non-schema version)**
  - Create shared memory segment (use platform utilities from Phase 0)
  - Initialize `SharedMemoryHeader` (magic, version, config)
  - Initialize `SlotRWState` array (all FREE)
  - Initialize flexible zones (zero-initialized)
  - Initialize SharedSpinLock states
  - Create `DataBlockMutex` for control zone

- [ ] **Implement schema-aware overload** (depends on P9)
  - Compute schema hash
  - Store in header
  - Include in broker registration

- [ ] **Implement `DataBlockProducer::register_with_broker()`**
  - Send `REG_REQ` to broker via MessageHub
  - Include: `channel_name`, `shm_name`, `schema_hash`, `schema_version`
  - Handle `REG_RESP` (success/failure)

**Estimated Effort**: 2 days
**Dependencies**: Priority 2.1 (SlotRWState C API), Phase 0 (platform utilities)
**Testing**: Test producer creation with various configs

### 🔴 PRIORITY 3.2: Consumer Factory Implementation

- [ ] **Implement `find_datablock_consumer()` (non-schema version)**
  - Send `DISC_REQ` to broker via MessageHub
  - Parse `DISC_RESP` to get `shm_name`
  - Attach to existing shared memory (use platform utilities)
  - Verify magic number, version, config
  - Register heartbeat in `SharedMemoryHeader::consumer_heartbeats`

- [ ] **Implement schema-aware overload** (depends on P9)
  - Retrieve schema hash from broker
  - Compare with expected hash
  - Verify hash in header matches
  - Throw `SchemaValidationException` on mismatch

- [ ] **Implement heartbeat registration**
  - Find free slot in `consumer_heartbeats[8]`
  - Store PID/UUID, timestamp
  - Update heartbeat periodically (background thread or user-called)

**Estimated Effort**: 2 days
**Dependencies**: Priority 3.1 (producer factory), Phase 0 (platform utilities)
**Testing**: Test consumer discovery and attachment

### 🟡 PRIORITY 3.3: DataBlock Lifecycle Integration

- [ ] **Register DataHub module with Lifecycle**
  - Already done in `message_hub.cpp` (`GetLifecycleModule()`)
  - Verify: ZeroMQ context initialization in startup
  - Verify: Cleanup in shutdown

- [ ] **Add lifecycle check in factory functions**
  ```cpp
  if (!pylabhub::hub::lifecycle_initialized()) {
      throw std::runtime_error("DataHub module not initialized");
  }
  ```

- [ ] **Document lifecycle requirements** in API docs
  - User must create `LifecycleGuard` with `GetLifecycleModule()`
  - Example: See Section 7 of HEP document

**Estimated Effort**: 0.5 days
**Dependencies**: None (already partially implemented)
**Testing**: Test factory calls before/after lifecycle init

---

## Phase 4: MessageHub and Broker Protocol (Week 2-3)

### 🔴 PRIORITY 4.1: MessageHub C++ Wrapper

- [ ] **Complete `MessageHub::connect()` implementation**
  - Create ZeroMQ REQ socket
  - Apply CurveZMQ security (if `server_key` provided)
  - Connect to broker endpoint

- [ ] **Complete `MessageHub::send_message()` implementation**
  - Send two-part message: `[channel, message]`
  - Wait for response with timeout
  - Handle ZeroMQ errors (EAGAIN, ETERM)

- [ ] **Complete `MessageHub::register_producer()` implementation**
  - Construct `REG_REQ` JSON message
  - Call `send_message()`
  - Parse `REG_RESP`

- [ ] **Complete `MessageHub::discover_producer()` implementation**
  - Construct `DISC_REQ` JSON message
  - Call `send_message()`
  - Parse `DISC_RESP`
  - Return `ConsumerInfo` struct

**Estimated Effort**: 2 days
**Dependencies**: None (ZeroMQ already linked)
**Testing**: Test broker communication (requires test broker)
**Reference**: Section 6 of HEP-CORE-0002-DataHub-FINAL.md

### 🟡 PRIORITY 4.2: Broker Service (Separate Binary)

- [ ] **Create `src/admin/broker_service.cpp`**
  - Implement minimal broker (3 messages: REG_REQ, DISC_REQ, DEREG_REQ)
  - Use ZeroMQ REP socket
  - Store producer registry in-memory (std::map)

- [ ] **Implement broker message handlers**
  - `handle_reg_req()`: Register producer, return `REG_RESP`
  - `handle_disc_req()`: Lookup producer, return `DISC_RESP`
  - `handle_dereg_req()`: Deregister producer, return `DEREG_RESP`

- [ ] **Add CurveZMQ support** (optional, for production)
  - Generate broker keypair
  - Enforce client authentication

- [ ] **Add broker CLI** (`datablock-broker`)
  - Start/stop broker
  - Query registered producers
  - Set log level

**Estimated Effort**: 2 days
**Dependencies**: Priority 4.1 (MessageHub)
**Testing**: Integration tests with MessageHub
**Reference**: Section 6.2 of HEP-CORE-0002-DataHub-FINAL.md

---

## Phase 5: P8 Error Recovery (Week 3)

### 🟢 PRIORITY 5.1: Diagnostics API Implementation

- [ ] **Implement `datablock_diagnose_slot()`**
  - Attach to shared memory (read-only)
  - Read `SlotRWState` fields
  - Check if slot is stuck (heuristic: writer lock held > 5 seconds)
  - Fill `SlotDiagnostic` struct

- [ ] **Implement `datablock_diagnose_all_slots()`**
  - Loop over all slots
  - Call `datablock_diagnose_slot()` for each

- [ ] **Implement `datablock_is_process_alive()`**
  - Delegate to `pylabhub::platform::is_process_alive()` (from Phase 0)

**Estimated Effort**: 1 day
**Dependencies**: Phase 0 (platform utilities), Priority 2.1 (SlotRWState)
**Testing**: Test diagnostics on stuck slots

### 🟢 PRIORITY 5.2: Recovery Operations Implementation

- [ ] **Implement `datablock_force_reset_slot()`**
  - Check if writer PID is alive (unless `force=true`)
  - Reset `SlotRWState` to FREE
  - Clear reader count
  - Update metrics: `recovery_actions_count`

- [ ] **Implement `datablock_release_zombie_readers()`**
  - Check if any readers are zombies (heuristic: heartbeat timeout)
  - Clear `reader_count` if all zombies (or `force=true`)

- [ ] **Implement `datablock_release_zombie_writer()`**
  - Check if writer PID is dead
  - Clear `write_lock`
  - Transition to FREE

- [ ] **Implement `datablock_cleanup_dead_consumers()`**
  - Scan `consumer_heartbeats[8]`
  - Check if PID is alive
  - Clear dead consumer entries

**Estimated Effort**: 1.5 days
**Dependencies**: Priority 5.1 (diagnostics)
**Testing**: Test recovery on simulated crashes

### 🟢 PRIORITY 5.3: Integrity Validation

- [ ] **Implement `datablock_validate_integrity()`**
  - Verify magic number
  - Verify version compatibility
  - Recompute checksums (if enabled)
  - Compare with stored checksums
  - If `repair=true`: Rewrite corrected checksums

**Estimated Effort**: 1 day
**Dependencies**: Phase 0 (crypto_utils), Priority 5.2 (recovery ops)
**Testing**: Test integrity validation on corrupted data

### 🟡 PRIORITY 5.4: CLI Tool (`datablock-admin`)

- [ ] **Create `src/admin/datablock_admin.cpp`**
  - Command: `datablock-admin diagnose <shm_name>`
  - Command: `datablock-admin force-reset <shm_name> [--slot=N] [--force]`
  - Command: `datablock-admin cleanup-zombies <shm_name>`
  - Command: `datablock-admin validate <shm_name> [--repair]`

- [ ] **Add CLI parsing** (use `fmt` for output formatting)
  - Parse arguments
  - Call recovery API functions
  - Print human-readable output

**Estimated Effort**: 1 day
**Dependencies**: Priority 5.3 (integrity validation)
**Testing**: Manual CLI testing
**Reference**: Section 8.1 of HEP-CORE-0002-DataHub-FINAL.md

---

## Phase 6: Testing (Week 4)

### 🔴 PRIORITY 6.1: Unit Tests

- [ ] **Test SlotRWState coordination** (`test_datablock.cpp`)
  - Single writer, multiple readers
  - Writer timeout
  - Reader TOCTTOU race detection
  - Generation counter wrap-around

- [ ] **Test SharedSpinLock** (`test_datablock.cpp`)
  - Multi-process contention
  - PID reuse detection
  - Recursive locking

- [ ] **Test P9 Schema Validation** (`test_schema_validation.cpp`)
  - Producer registration with schema
  - Consumer validation (matching schema)
  - Consumer rejection (mismatched schema)
  - BLDS generation for various struct types

- [ ] **Test P7 Transaction API** (`test_transaction_api.cpp`)
  - `with_write_transaction()` success path
  - `with_write_transaction()` timeout path
  - `with_read_transaction()` exception safety
  - `with_next_slot()` iterator

- [ ] **Test P8 Recovery API** (`test_recovery_api.cpp`)
  - Diagnose stuck slots
  - Force reset slot
  - Release zombie readers/writers
  - Cleanup dead consumers

**Estimated Effort**: 3 days
**Dependencies**: All previous phases

### 🟡 PRIORITY 6.2: Integration Tests

- [ ] **Test producer-consumer basic flow**
  - Producer writes, consumer reads
  - Verify data integrity
  - Verify metrics updated

- [ ] **Test multi-consumer scenario**
  - 1 producer, 3 consumers
  - Verify heartbeat registration
  - Verify all consumers receive data

- [ ] **Test ring buffer wrap-around**
  - Producer writes > capacity slots
  - Verify `write_index` wraps correctly
  - Verify no data corruption

- [ ] **Test broker discovery**
  - Producer registers
  - Consumer discovers
  - Verify schema validation (P9)

**Estimated Effort**: 2 days
**Dependencies**: Priority 6.1 (unit tests)

### 🟢 PRIORITY 6.3: Stress Tests

- [ ] **High contention test** (`test_benchmarks.cpp`)
  - 1 producer, 10 consumers
  - 10,000 slots/sec
  - Measure latency (p50, p95, p99)

- [ ] **Long-running test**
  - Run for 1 hour
  - Verify no memory leaks
  - Verify no deadlocks
  - Verify metrics consistency

- [ ] **ThreadSanitizer (ARM)**
  - Run all tests with TSan enabled
  - Fix any race conditions detected

**Estimated Effort**: 2 days
**Dependencies**: Priority 6.2 (integration tests)

---

## Phase 7: Deployment and Documentation (Week 5)

### 🟡 PRIORITY 7.1: Python Bindings (Optional)

- [ ] **Create `src/python/pylabhub_datahub.cpp`**
  - Wrap `DataBlockProducer`, `DataBlockConsumer`
  - Wrap recovery API (`diagnose_slot`, `force_reset_slot`)
  - Use pybind11

- [ ] **Add Python examples**
  - Producer example
  - Consumer example
  - Monitoring script (using recovery API)

**Estimated Effort**: 2 days
**Dependencies**: All core implementation complete

### 🟢 PRIORITY 7.2: Prometheus Exporter (Optional)

- [ ] **Create `src/admin/datablock_exporter.cpp`**
  - Periodically query `SharedMemoryHeader::metrics`
  - Expose as Prometheus metrics (HTTP endpoint)
  - Metrics: `datablock_slots_written_total`, `datablock_writer_timeouts_total`, etc.

**Estimated Effort**: 1 day
**Dependencies**: Priority 7.1 (Python bindings, for easier implementation)

### 🟡 PRIORITY 7.3: Documentation

- [ ] **Update API reference**
  - Generate Doxygen docs
  - Add to `docs/api/`

- [ ] **Update emergency procedures**
  - Expand `docs/emergency_procedures.md`
  - Add troubleshooting guide (common errors, how to diagnose)

- [ ] **Update CLAUDE.md** (if needed)
  - Add DataBlock usage examples
  - Update build instructions

**Estimated Effort**: 1 day
**Dependencies**: All implementation complete

---

## Maintenance Tasks

### 🔵 ONGOING: Code Cleanup

- [ ] Remove obsolete TODOs in code
- [ ] Remove unused headers (e.g., `data_header_sync_primitives.hpp` - already deleted)
- [ ] Consolidate duplicate code (e.g., PID checks, timestamp functions)
- [ ] Run clang-tidy and fix warnings
- [ ] Run clang-format on all modified files

### 🔵 ONGOING: Update TODO List

- [ ] Mark completed tasks with ✅
- [ ] Update progress percentages
- [ ] Add new tasks as discovered
- [ ] Remove obsolete tasks

---

## Timeline Summary

| Week | Phase | Key Deliverables |
|------|-------|------------------|
| **Week 1** | Phase 0 + P9 | Helper refactoring, Schema validation complete |
| **Week 2** | Phase 2-3 | SlotRWState API, DataBlock factories |
| **Week 3** | Phase 4-5 | MessageHub, broker, recovery API |
| **Week 4** | Phase 6 | All tests passing, TSan clean |
| **Week 5** | Phase 7 | Python bindings, docs, deployment ready |

---

## Blockers and Risks

### Current Blockers

- None

### Potential Risks

1. **ARM ThreadSanitizer availability**: If ARM CI not available, may miss race conditions
   - Mitigation: Test on x86 TSan + manual ARM testing

2. **Broker service design**: Minimal broker may not scale to 100+ producers
   - Mitigation: Document broker limitations, plan for future refactor if needed

3. **P9 Schema evolution**: Strict hash matching may be too restrictive
   - Mitigation: Document migration procedures, add version compatibility in future

---

## Notes

- **Phase 0 is critical**: Refactoring helper functions will simplify all subsequent phases and improve overall code quality
- **P9 can be implemented in parallel**: Schema validation is mostly independent of core DataBlock implementation
- **Test early and often**: Multi-process IPC bugs are hard to debug; catch them with unit tests
- **Document as you go**: Update HEP document and IMPLEMENTATION_GUIDANCE.md with any design changes

---

**Revision History**:
- **v1.0** (2026-02-09): Initial TODO list created for 5-week implementation timeline
