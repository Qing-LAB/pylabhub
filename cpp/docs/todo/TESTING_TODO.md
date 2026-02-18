# Testing TODO

**Purpose:** Track testing tasks, test phases (A-D), coverage gaps, and testing infrastructure improvements.

**Master TODO:** `docs/TODO_MASTER.md`  
**Test Strategy:** `docs/README/README_testing.md`

---

## Current Focus

### Phase C: Integration Tests
**Status**: 🟡 In Progress

- [ ] **MessageHub and broker tests** – Complete Phase C tests for MessageHub/broker integration
- [ ] **Multi-process IPC tests** – Producer/consumer across process boundaries
- [ ] **Cross-platform consistency** – Run same tests on Linux, Windows, macOS, FreeBSD

### Phase D: High-Load and Edge Cases
**Status**: 🟢 Ready to Start

- [ ] **High-load stress tests** – Extended duration, multiple producers/consumers
- [ ] **Edge case scenarios** – Wraparound, capacity boundaries, race conditions
- [ ] **Recovery scenarios** – Zombie processes, corrupted state, integrity repair

---

## Test Phase Checklist

### Phase A: Protocol/API Correctness ✅
- [x] Flexible zone access (empty when no zones, populated when configured)
- [x] Checksum validation (false when no zones, true when valid)
- [x] Consumer config matching (expected_config validation)
- [x] Schema validation tests

### Phase B: Slot Protocol (Single Process) ✅
- [x] Write/read basic flow
- [x] Checksum enforced mode
- [x] Layout smoke test (checksum + flexible zone)
- [x] Diagnostic handle access
- [x] Error handling (timeouts, bounds, double-release)

### Phase C: Integration (Multi-Process)
- [x] Basic producer/consumer IPC
- [x] ConsumerSyncPolicy variants (Latest_only, Single_reader, Sync_reader)
- [x] High-load single reader integrity test
- [ ] MessageHub broker integration
- [ ] Consumer registration to broker
- [ ] Cross-process recovery scenarios (broker-coordinated; facility-layer tests ✅ done separately)

### Phase D: High-Load and Edge Cases
- [ ] Extended duration stress tests (hours)
- [ ] Multiple producers, multiple consumers
- [ ] Slot wraparound at 2^64
- [ ] Capacity boundary conditions
- [ ] Race condition scenarios
- [ ] Platform-specific behavior verification

---

## Test Infrastructure

### Multi-Process Test Framework ✅
- [x] Worker process pattern established
- [x] ExpectWorkerOk with stderr validation
- [x] Lifecycle management in workers
- [x] Test framework shared utilities

### Platform Coverage
- [ ] **Linux** – Primary development platform (complete)
- [ ] **Windows** – Build and test (basic coverage)
- [ ] **macOS** – Build and test (basic coverage)
- [ ] **FreeBSD** – Build and test (pending)

### Sanitizer Coverage
- [x] **ThreadSanitizer** – Enabled, passing (except known EOWNERDEAD false positive)
- [ ] **AddressSanitizer** – Enable and verify
- [ ] **UndefinedBehaviorSanitizer** – Enable and verify

---

## Coverage Gaps

### High Priority
- [ ] Consumer registration to broker (protocol not yet defined)
- [ ] Broker schema registry tests
- [ ] MessageHub error paths with broker
- [ ] Recovery: cross-process zombie detection (broker-coordinated) — requires broker protocol
- [ ] Recovery: slot-checksum in-place repair (current repair path reinitialises header; needs WriteAttach mode instead of create)

### Medium Priority
- [ ] Flexible zone by-name access edge cases
- [ ] Transaction API exception safety (comprehensive)
- [ ] Diagnostic API comprehensive coverage

### Low Priority
- [ ] stuck_duration_ms in diagnostics (requires timestamp on acquire)
- [ ] Schema versioning and compatibility rules
- [ ] Migration path testing

---

## Recent Completions

### 2026-02-17 (Integrity validation tests)
- ✅ **Integrity repair test suite** (`test_datahub_integrity_repair.cpp` + workers) — 3 tests:
  fresh ChecksumPolicy::Enforced block validates successfully (slot checksum path exercised);
  layout checksum corruption detected (FAILED on both repair=false and repair=true — not repairable);
  magic number corruption detected (FAILED). Secrets 78001–78003.
  Slot-checksum in-place repair deferred: existing repair path uses `create_datablock_producer_impl`
  which reinitialises the header — incompatible with in-place repair testing.
  **Total: 384/384 passing.**

### 2026-02-17 (Recovery scenario facility tests)
- ✅ **Recovery scenario test suite** (`test_datahub_recovery_scenarios.cpp` + workers) — 6 tests:
  zombie writer (dead PID in write_lock → release_zombie_writer → FREE);
  zombie readers (reader_count injected → release_zombie_readers → 0);
  force_reset on dead writer (dead write_lock → force_reset succeeds without force flag);
  dead consumer cleanup (fake heartbeat with dead PID → cleanup_dead_consumers removes it);
  is_process_alive sentinel (kDeadPid=INT32_MAX → false; self PID → true);
  force_reset safety guard (alive write_lock → RECOVERY_UNSAFE; recoveryAPI logs ERROR).
  Secrets 77001–77004, 77006. **Scope: facility layer only** — full broker-coordinated
  zombie detection remains deferred (Phase C, requires broker protocol).
  **Total: 381/381 passing.**

### 2026-02-17 (WriteAttach mode tests)
- ✅ **WriteAttach test suite** (`test_datahub_write_attach.cpp` + workers) — 4 tests:
  basic roundtrip (hub creates, source attaches R/W and writes, creator consumer reads);
  secret mismatch → nullptr; schema mismatch → nullptr; segment persists after writer detach.
  Secrets 76001–76004. Verifies broker-owned shared memory model.
  **Total: 375/375 passing.**

### 2026-02-17 (coverage gap tests completed)
- ✅ **Config validation test** (`test_datahub_config_validation.cpp` + workers) — 5 tests:
  all four mandatory-field throw cases + valid config succeeds. Secrets 73001–73005.
- ✅ **Header structure test** (`test_datahub_header_structure.cpp` + workers) — 3 tests:
  template API populates both schema hashes; impl with nullptr zeroes them; different types
  produce different hashes. Secrets 74001–74004. Fix: `flex_zone_size = sizeof(FlexZoneT)` required.
- ✅ **C API validation test** (`test_datahub_c_api_validation.cpp` + workers) — 5 tests:
  `datablock_validate_integrity` succeeds on fresh; fails on nonexistent (allow ERROR logs);
  `datablock_get_metrics` shows 0 commits; `datablock_diagnose_slot` shows FREE;
  `datablock_diagnose_all_slots` returns capacity entries. Secrets 75001–75005.
  **Total: 371/371 passing.**

### 2026-02-17 (docs audit — test refactoring status verified)
- ✅ **Test refactoring complete** — All Phase 1-3 and Phase 4 (T4.1-T4.5) tasks from the
  test refactoring plan are done: shared test types (`test_datahub_types.h`), removed obsolete
  non-template tests, all enabled tests compile; new tests added for schema validation,
  c_api checksum, exception safety, handle semantics. Phase 5 renaming also complete (all
  files follow `test_datahub_*` convention). Verified: 358/358 passing.
  — All transient test planning docs archived to `docs/archive/transient-2026-02-17/`

### 2026-02-17
- ✅ `DatahubSlotDrainingTest` (7 tests): DRAINING state machine tests — entered on wraparound,
  rejects new readers, resolves after reader release, timeout restores COMMITTED, no reader races
  on clean wraparound; plus ring-full barrier proof tests for Single_reader and Sync_reader
  — `tests/test_layer3_datahub/test_datahub_c_api_slot_protocol.cpp`,
    `tests/test_layer3_datahub/workers/datahub_c_api_draining_workers.cpp`
- ✅ Proved DRAINING structurally unreachable for Single_reader / Sync_reader
  (ring-full check before fetch_add creates arithmetic barrier) — documented in
  `docs/DATAHUB_PROTOCOL_AND_POLICY.md` § 11, `docs/IMPLEMENTATION_GUIDANCE.md` Pitfall 11

### 2026-02-14
- ✅ Writer timeout metrics split test (lock vs reader timeout)
- ✅ Unified metrics API tests (total_slots_written, state snapshot)

### 2026-02-13
- ✅ Config validation tests (explicit parameters required)
- ✅ Shared spinlock API tests (get_spinlock, spinlock_count)

### 2026-02-12
- ✅ ConsumerSyncPolicy tests (all three modes)
- ✅ High-load single reader integrity test
- ✅ MessageHub Phase C groundwork (no-broker paths)

---

## Notes

- Test pattern choice: PureApiTest (no lifecycle), LifecycleManagedTest (shared lifecycle), WorkerProcess (multi-process or finalizes lifecycle). See `docs/README/README_testing.md`.
- CTest runs each test in separate process; direct execution runs all in one process. Use WorkerProcess for isolation.
- When test fails: scrutinize before coding. Is it revealing a bug or is the test wrong? See `docs/IMPLEMENTATION_GUIDANCE.md` § Responding to test failures.
