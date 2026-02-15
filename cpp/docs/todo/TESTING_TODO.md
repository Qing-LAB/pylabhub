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
- [ ] Cross-process recovery scenarios

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
- [ ] Recovery: zombie reader detection and cleanup
- [ ] Recovery: corrupted header/layout repair

### Medium Priority
- [ ] Config explicit-fail test (validation before memory creation)
- [ ] Flexible zone by-name access edge cases
- [ ] Transaction API exception safety (comprehensive)
- [ ] Diagnostic API comprehensive coverage

### Low Priority
- [ ] stuck_duration_ms in diagnostics (requires timestamp on acquire)
- [ ] Schema versioning and compatibility rules
- [ ] Migration path testing

---

## Recent Completions

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
