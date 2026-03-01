# Testing TODO

**Purpose:** Track testing tasks, test phases (A-D), coverage gaps, and testing infrastructure improvements.

**Master TODO:** `docs/TODO_MASTER.md`  
**Test Strategy:** `docs/README/README_testing.md`

---

## Current Focus

### Phase C: Integration Tests
**Status**: ✅ Complete (424/424 passing as of 2026-02-19)

- [x] **MessageHub and broker tests** – Phase C broker integration + consumer registration complete
- [x] **Multi-process IPC tests** – Producer/consumer across process boundaries (E2E test)
- [x] **hub::Producer + hub::Consumer active API** – 15 tests; HELLO/BYE tracking, SHM callbacks, ctrl messaging, idempotency, destructor-BYE regression
- [ ] **Cross-platform consistency** – Run same tests on Linux (done), Windows, macOS, FreeBSD

### Phase D: High-Load and Edge Cases
**Status**: 🔵 Partial — RAII stress tests added; extended/platform tests deferred

- [x] **RAII multi-process ring-buffer stress** — `DatahubStressRaiiTest` (tests 423–424):
  - `MultiProcessFullCapacityStress`: 500 × 4KB slots, ring=32 (15 wraparounds), 2 racing consumers,
    enforced BLAKE2b + app-level XOR-fold, random 0–5ms write / 0–10ms read delays
  - `SingleReaderBackpressure`: 100 slots, ring=8, consumer 0–20ms delays force producer to block
- [ ] **High-load extended stress** – Hours-long soak tests; multiple producers simultaneously
- [ ] **Edge case scenarios** – Wraparound at 2^64, slot_id rollover, capacity exhaustion
- [ ] **Broker-coordinated recovery** – Cross-process zombie detection (blocked on broker protocol extension)
- [ ] **Slot-checksum in-place repair** – Blocked: existing repair reinitialises header; needs WriteAttach approach

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

### Layer 4: pylabhub-actor Tests (new — 2026-02-21; updated 2026-02-23)
**Status**: 🔵 Partial — config + metrics unit tests done; integration tests pending

Unit tests live in `tests/test_layer4_actor/` (86 tests as of 2026-02-28, all passing).

#### ✅ Done (unit layer — no Python init, no live actor)
- [x] **ActorConfig parsing** — `loop_timing` (fixed_pace / compensating / default / invalid),
  `broker_pubkey` (present / absent), `broker` endpoint, `interval_ms` / `timeout_ms`,
  all four `validation` policy fields + defaults + invalid values, uid auto-gen,
  non-ACTOR- uid warning (no throw), multi-role, empty roles map, all error cases
  (missing script, missing channel, invalid kind, bad file, malformed JSON).
- [x] **ActorRoleAPI metrics** — initial zeroes; `increment_script_errors` / `loop_overrun_count`
  / `set_last_cycle_work_us`; `reset_all_role_run_metrics` clears all three; does not affect
  identity fields; accumulation after reset; reset on fresh API is no-op; `slot_valid` flag;
  identity getters round-trip; two instances are independent.

#### Pending integration tests for the `pylabhub-actor` executable:

- [ ] **Actor + Hub round-trip** — spawn broker + producer actor + consumer actor; verify consumer on_read receives correct typed slot data (log-file / ZMQ report-back validation)
- [ ] **UID auto-generation** — run actor with no uid in config; verify ACTOR-{NAME}-{8HEX} format in stderr
- [ ] **UID non-conforming warning** — run actor with old-style uid; verify warning log
- [ ] **Schema declaration hash mismatch** — producer and consumer with different field types in slot_schema; verify consumer connect() fails with "schema mismatch" in log before any data is read (Layer 2 check — `compute_schema_hash()` in actor_host.cpp)
- [ ] **Schema declaration hash match** — same schema on both sides; verify successful connection
- [ ] **--keygen** — run with --keygen and keyfile path; verify JSON file created with correct fields; run again; verify file overwritten; verify missing-dir case creates parent dir
- [ ] **Validation policy: slot_checksum enforce** — consumer verifies checksum; corrupt a slot; verify on_read NOT called (skip) or called with api.slot_valid()=false (pass)
- [ ] **Validation policy: on_python_error stop** — on_read raises exception; verify actor exits cleanly
- [ ] **SharedSpinLockPy** — producer and consumer both use api.spinlock(0); verify mutual exclusion via counter increment test (no lost increments)
- [ ] **Multi-role actor** — one actor JSON with 1 producer + 1 consumer role; verify both threads start; producer writes; consumer reads
- [ ] **interval_ms=0 producer** — verify write loop runs without delay; measure throughput
- [ ] **timeout_ms consumer** — verify on_read(slot=None, timed_out=True) fires after timeout with no producer
- [ ] **--validate mode** — run with --validate; verify ctypes layout printed to stdout; exit(0)
- [ ] **Legacy flat format** — run with old-format JSON; verify deprecation warning; actor runs correctly

**New scenarios from 2026-02-22 code-review fixes:**

- [ ] **PylabhubEnv getters** — in on_write/on_read callback: assert `api.actor_name()`,
  `api.channel()`, `api.kind()`, `api.broker()`, `api.log_level()`, `api.script_dir()` all
  return correct non-empty strings matching the config JSON
- [ ] **Schema type validation error** — producer JSON with `"type": "badtype"` in slot_schema;
  verify actor exits at startup with message containing "unknown type" before any data is written
- [ ] **Schema count=0 error** — producer JSON with `"count": 0` in slot_schema; verify actor
  exits at startup with message containing "count = 0"
- [ ] **AdminShell oversized request** — send >1 MB ZMQ payload to AdminShell REP socket;
  verify response contains `{"error":"request too large"}`; verify subsequent requests work
- [ ] **on_stop registered roles** — register on_stop/on_stop_c in actor script;
  verify `_registered_roles("on_stop")` returns non-empty list (B1 regression guard)
- [ ] **LoopTimingPolicy fixed_pace** — producer with `"loop_timing":"fixed_pace","interval_ms":10`;
  measure actual call rate → ~100 Hz ±10%; `api.loop_overrun_count()` == 0 under normal load
- [ ] **LoopTimingPolicy compensating** — same with `"loop_timing":"compensating"`; inject one
  slow `on_write` cycle; verify overrun increments by 1; average rate recovers
- [ ] **Metrics overrun via script** — producer with `"interval_ms":1` and `on_write` that
  sleeps 5ms; verify `api.loop_overrun_count() > 0` after several iterations
- [ ] **last_cycle_work_us non-zero** — producer with timed `on_write`; read
  `api.last_cycle_work_us()` from next `on_write`; verify > 0

#### LoopPolicy / ContextMetrics tests (HEP-CORE-0008 Pass 2 — no tests exist yet)

These test the DataBlock Pimpl timing introduced in Pass 2. Single-process tests (no lifecycle
init needed; use PureApiTest pattern with a created DataBlockProducer/Consumer pair):

- [ ] **ContextMetrics zero on creation** — fresh producer/consumer: all counters 0,
  `context_start_time == time_point{}`, `period_ms == 0`
  — `tests/test_layer3_datahub/test_datahub_context_metrics.cpp`
- [ ] **iteration_count increments** — acquire/release 5 slots; verify `iteration_count == 5`
- [ ] **last_slot_work_us non-zero** — acquire, sleep 1ms, release; verify `last_slot_work_us >= 1000`
- [ ] **last_iteration_us measured** — two back-to-back acquires with 2ms sleep between;
  verify `last_iteration_us >= 2000`
- [ ] **max_iteration_us tracks peak** — fast acquire, slow acquire (sleep between); verify
  `max_iteration_us >= slow_us && last_iteration_us == fast_us` after fast acquire
- [ ] **context_elapsed_us monotonic** — 3 acquires 1ms apart; verify `context_elapsed_us`
  increases and ≥ 2ms after third acquire
- [ ] **FixedRate overrun_count** — `set_loop_policy(FixedRate, 1ms)`; acquire, sleep 5ms, acquire;
  verify `overrun_count == 1`
- [ ] **MaxRate no overrun** — `set_loop_policy(MaxRate)`; rapid acquires; verify `overrun_count == 0`
- [ ] **clear_metrics resets** — accumulate counters, call `clear_metrics()`, verify all 0
  except `period_ms` preserved
- [ ] **set_loop_policy updates period_ms** — `set_loop_policy(FixedRate, 10ms)`; verify
  `metrics().period_ms == 10`
- [ ] **ctx.metrics() pass-through** — via `with_transaction`, verify `ctx.metrics()` returns
  same ref as `producer.metrics()`
- [ ] **api.metrics() dict keys** — spawn actor with `interval_ms > 0`; verify all keys present
  and `period_ms == interval_ms`; `iteration_count > 0` after 3+ iterations

### ScriptHost / PythonScriptHost threading model (tests done — 2026-02-28)
**Status**: ✅ Complete — 10 tests in `tests/test_layer2_service/test_script_host.cpp`

- [x] **ScriptHost base threading** — threaded startup/shutdown, idempotent shutdown, early-stop
- [x] **ScriptHost exception in do_initialize** — exception propagated via future to caller
- [x] **ScriptHost returns false without signal** — set exception on promise; base_startup_ throws
- [x] **ScriptHost direct mode (Lua path)** — do_initialize on calling thread; signal_ready by base

### HubConfig script-block fields (tests done — 2026-02-28)
**Status**: ✅ Complete — 9 tests in `tests/test_layer3_datahub/test_datahub_hub_config_script.cpp`

- [x] **`hub_script_dir()` from JSON** — resolves to `<hub_dir>/my_script/python`
- [x] **`script_type()` from JSON** — reads `"type"` field correctly
- [x] **`tick_interval_ms()` override** — `"tick_interval_ms":500`; verify 500
- [x] **`health_log_interval_ms()` override** — `"health_log_interval_ms":30000`; verify 30000
- [x] **`hub_dir()` matches config path parent** — lifecycle sets hub_dir correctly
- [x] **`tick_interval_ms()` default** — omit key; verify 1000 ms default
- [x] **`health_log_interval_ms()` default** — verify 60000 ms default
- [x] **`hub_script_dir()` default absent** — no `"script"` block; verify empty path
- [x] **`script_type()` default absent** — no `"script"` block; verify empty string

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

### 2026-02-28 (ScriptHost + HubConfig script-block tests + CMake option)
- ✅ **Layer 2 tests: ScriptHost threading model** (10 tests) — `tests/test_layer2_service/test_script_host.cpp`:
  `test_layer2_script_host` executable; mock subclasses; threaded mode (startup/shutdown/idempotent/
  early-stop/exception propagation/false-without-signal); direct mode (startup-ready/shutdown-finalizes/
  failure-throws). Note: `ThreadedEarlyStop` checks `is_ready()` only AFTER `shutdown()` (join provides
  happens-before; earlier check is racy vs thread_fn_ clearing `ready_`).
- ✅ **Layer 3 tests: HubConfig script-block fields** (9 tests) — `tests/test_layer3_datahub/test_datahub_hub_config_script.cpp`:
  two lifecycle fixtures (Logger + CryptoUtils + FileLock + JsonConfig + HubConfig); configured fixture
  (hub.json with all script/python fields; 5 tests); defaults fixture (absent script/python block; 4 tests).
- ✅ **CMake: `PYLABHUB_PYTHON_REQUIREMENTS_FILE`** — `cmake/ToplevelOptions.cmake` FILEPATH cache var;
  `third_party/cmake/python.cmake` uses variable (+ existence check); `CMakeLists.txt` status message.
  **Total: 573/573 passing**

### 2026-02-27 (LoopPolicy edge-case tests + code review fixes)
- ✅ **Layer 3 tests: LoopPolicy edge cases** (11 tests) — extended `test_datahub_loop_policy.cpp`:
  - 7 edge-case tests (secrets 80006–80012): ZeroOnCreation, MaxRateNoOverrun, LastSlotWorkUsPopulated,
    LastIterationUsPopulated, MaxIterationUsPeak, ContextElapsedUsMonotonic, CtxMetricsPassThrough
  - 4 RAII-specific tests (secrets 80013–80016): RaiiProducerLastSlotWorkUsMultiIter,
    RaiiProducerMetricsViaSlots, RaiiProducerOverrunViaSlots, RaiiConsumerLastSlotWorkUs
  - Fixed RAII release path: `last_slot_work_us` set in `release_write_handle()` and
    `release_consume_handle()` (symmetric RAII destructor + explicit call paths)
  - Fixed RAII multi-iter producer race: `SlotWriteHandle`/`SlotConsumeHandle` store per-handle
    `t_slot_acquired_` (not `owner->t_iter_start_` which gets overwritten between iterations)
  **Total: 550/550 passing**
- ✅ **CODE_REVIEW.md triage complete** (2026-02-27): All 8 critical + 12 high items verified
  as false positives or pre-existing fixes; deferred medium items documented in review.

### 2026-02-26 (Connection policy + security identity)
- ✅ **Layer 3 tests: ConnectionPolicy enforcement** (11 tests) — new `test_datahub_connection_policy.cpp`:
  - Suite 1 (4 tests): `to_str`/`from_str` round-trips, unknown-string fallback to Open
  - Suite 2 (7 tests): Open/Required/Verified broker enforcement, per-channel glob override,
    ephemeral port (`tcp://127.0.0.1:0`) prevents parallel ctest-j collisions
  **Total: 539/539 passing** (528 pre-existing + 11 new Phase 3 connection policy tests)

### 2026-02-23
- ✅ **Layer 2 tests: HubVault** (15 tests) — `tests/test_layer2_service/test_hub_vault.cpp`:
  create/open/publish_public_key basics, file permissions (0600 vault, 0644 pubkey),
  Z85 keypair and 64-char hex token validation, entropy (two creates differ), wrong password throws,
  corrupted vault throws, missing vault throws, `VaultFileDoesNotContainPlaintextSecrets`,
  `EncryptDecryptRoundTrip`, `DifferentHubUidProducesDifferentCiphertext` (cross-uid open fails).
  No lifecycle; uses `gtest_main`; Argon2id ~0.5s/call → 120s timeout.
  **Total: 488/488 passing** (479 pre-existing + 15 new Layer 2 HubVault tests — 1 flaky lifecycle timeout pre-existing, passes in isolation).
- ✅ **Layer 4 tests: ActorConfig parsing** (32 tests) — `tests/test_layer4_actor/test_actor_config.cpp`:
  `loop_timing` (fixed_pace/compensating/default/invalid), `broker_pubkey`, `broker` endpoint,
  `interval_ms`/`timeout_ms`, all four `ValidationPolicy` fields + defaults + invalid values,
  uid auto-gen / non-conforming (warning, no throw), multi-role, empty roles map, all error cases.
  No lifecycle init; `LOGGER_COMPILE_LEVEL=0`.
- ✅ **Layer 4 tests: ActorRoleAPI metrics** (21 tests) — `tests/test_layer4_actor/test_actor_role_metrics.cpp`:
  initial-zero invariant, `increment_script_errors`, `increment_loop_overruns`,
  `set_last_cycle_work_us`, `reset_all_role_run_metrics` (all 3 counters; does not reset identity
  fields; accumulation after reset; no-op on fresh API), `slot_valid` flag, all identity getters,
  instance independence. All inline methods; no Python interpreter init.
  **Total: 479/479 passing** (426 pre-existing + 53 new Layer 4 tests).

### 2026-02-21 (gap-fixing session)
- ✅ **426/426 tests pass** — no regressions after all gap-fix changes (demo scripts, --keygen, schema hash, timeout constants)
- ✅ **Layer 4 test gap identified** — actor integration test plan updated with schema hash mismatch test and --keygen test
- ✅ **426/426 tests pass** — no regressions after pylabhub-actor, UID enforcement, SharedSpinLockPy additions (earlier in same day)

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
  `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` § 11, `docs/IMPLEMENTATION_GUIDANCE.md` Pitfall 11

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
