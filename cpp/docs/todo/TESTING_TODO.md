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
- [x] MessageHub broker integration ✅ complete (2026-02-18)
- [x] Consumer registration to broker ✅ complete (2026-02-18)
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

### Layer 4: pylabhub-producer Tests ✅ Complete (2026-03-02)
**Status**: ✅ 14 tests passing — `tests/test_layer4_producer/`

#### Config unit tests (no lifecycle, no Python) — 8 tests
- [x] **ProducerConfig FromJsonFile_Basic** — all fields parsed; uid, name, channel, interval_ms, shm, script, validation
- [x] **ProducerConfig FromJsonFile_UidAutoGen** — PROD- prefix auto-generated when uid absent
- [x] **ProducerConfig FromJsonFile_SchemaFields** — slot_schema + flexzone_schema field names, types, count
- [x] **ProducerConfig FromJsonFile_MissingChannel** — throws std::runtime_error
- [x] **ProducerConfig FromJsonFile_MalformedJson** — throws std::runtime_error
- [x] **ProducerConfig FromJsonFile_FileNotFound** — throws std::runtime_error
- [x] **ProducerConfig FromDirectory_Basic** — resolves script_path to absolute path
- [x] **ProducerConfig StopOnScriptError_DefaultFalse** — default false; update_checksum default true

#### CLI integration tests (binary invoked via WorkerProcess) — 6 tests
- [x] **`--init` creates structure** — `producer.json`, `script/python/__init__.py`, `vault/`, `logs/`, `run/`
- [x] **`--init` default values** — uid has PROD- prefix, script.path=".", stop_on_script_error=false
- [x] **`--keygen` writes keypair** — creates vault file; stdout "Producer vault written to" + public_key
- [x] **`--validate` exits 0** — loads Python script, prints "Validation passed.", exits 0
- [x] **Config malformed JSON** — "Config error" in stderr, non-zero exit
- [x] **Config file not found** — stderr non-empty, non-zero exit

Note: `--keygen` parent-dir-creation and overwrite-entropy tests deferred to backlog.

### Layer 4: pylabhub-consumer Tests ✅ Complete (2026-03-02)
**Status**: ✅ 12 tests passing — `tests/test_layer4_consumer/`

#### Config unit tests — 6 tests
- [x] **ConsumerConfig FromJsonFile_Basic** — all fields parsed; uid, name, channel, timeout_ms, shm, script, validation
- [x] **ConsumerConfig FromJsonFile_UidAutoGen** — CONS- prefix auto-generated
- [x] **ConsumerConfig FromJsonFile_SchemaFields** — slot_schema + flexzone_schema field names, types
- [x] **ConsumerConfig FromJsonFile_MissingChannel** — throws std::runtime_error
- [x] **ConsumerConfig FromJsonFile_MalformedJson** — throws std::runtime_error
- [x] **ConsumerConfig FromDirectory_Basic** — resolves script_path to absolute path

#### CLI integration tests — 6 tests
- [x] **`--init` creates structure** — `consumer.json`, `script/python/__init__.py`, `vault/`, `logs/`, `run/`
- [x] **`--init` default values** — uid has CONS- prefix, script.path=".", stop_on_script_error=false
- [x] **`--keygen` writes keypair** — creates vault file; stdout "Consumer vault written to" + public_key
- [x] **`--validate` exits 0** — loads Python script, prints "Validation passed.", exits 0
- [x] **Config malformed JSON** — "Config error" in stderr, non-zero exit
- [x] **Config file not found** — stderr non-empty, non-zero exit

### Layer 4: pylabhub-actor Tests — ARCHIVED (actor eliminated 2026-03-01)

`pylabhub-actor` and its test suite (`tests/test_layer4_actor/`) were removed from the build
and deleted from disk on 2026-03-02 (HEP-CORE-0018 decision). The completed unit tests
(config parsing, role metrics, CLI keygen/register-with, 98 tests) are preserved in git history.
Replaced by standalone `pylabhub-producer` + `pylabhub-consumer` + `pylabhub-processor` binaries.

LoopPolicy C++ metrics tests (HEP-CORE-0008) are fully covered in
`tests/test_layer3_datahub/test_datahub_loop_policy.cpp` (tests 6–16, secrets 80006–80016).

### ScriptHost / PythonScriptHost threading model (tests done — 2026-02-28)
**Status**: ✅ Complete — 10 tests in `tests/test_layer2_service/test_script_host.cpp`

- [x] **ScriptHost base threading** — threaded startup/shutdown, idempotent shutdown, early-stop
- [x] **ScriptHost exception in do_initialize** — exception propagated via future to caller
- [x] **ScriptHost returns false without signal** — set exception on promise; base_startup_ throws
- [x] **ScriptHost direct mode (Lua path)** — do_initialize on calling thread; signal_ready by base

### PythonInterpreter / Admin Shell / Consumer ctypes (coverage gaps — 2026-03-01)
**Status**: 🔵 No automated tests — identified by code review `CODE_REVIEW_2026-03-01_hub-python-actor-headers.md`

- [ ] **HP-C1 — `pylabhub.reset()` deadlock regression** — spawn admin shell; from a running script
  call `pylabhub.reset()`; verify hub does NOT freeze and subsequent commands execute normally.
  (Fix: `reset_namespace_unlocked()` called by binding; `exec_mu` no longer re-entered.)
- [ ] **HP-C2 — stdout/stderr leak on exec() exception** — trigger a non-Python exception inside
  `py::exec()` path (e.g., `buf.getvalue()` encoding error); verify subsequent admin shell commands
  still produce output (not silently discarded). (Fix: `make_scope_guard` restores stdout/stderr.)
- [ ] **BN-H1 — Consumer binary ctypes.from_buffer_copy round-trip** — run a `pylabhub-consumer`
  with a typed slot schema; verify `on_consume(in_slot, ...)` receives a valid ctypes struct
  (not TypeError). (Fix: `from_buffer()` → `from_buffer_copy()` for read-only memoryviews.)

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
- [x] Consumer registration to broker — ✅ done (test_datahub_broker_consumer.cpp)
- [x] Broker schema registry tests — ✅ done (test_datahub_broker_schema.cpp, 7 tests, HEP-CORE-0016 Phase 3)
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
- [ ] **FlexZone atomic-ref usage example** — test demonstrating `std::atomic_ref<T>` pattern
  for out-of-transaction lock-free FlexZone access (from RAII_LAYER_TODO backlog)
- [ ] **Layout checksum stability** — attach with mismatched layout (should fail); structured
  buffer alignment test (8-byte, 16-byte types); large flex zone (multi-page); zero flex zone
  (slots only) — (from MEMORY_LAYOUT_TODO backlog; low priority, layout is stable)
- [ ] **RAII move semantics audit** — confirm all RAII handles support efficient move (no copy)
- [ ] **Zero-cost abstraction verification** — profile `with_transaction` vs primitive API
  with optimizations; confirm no overhead on hot path

---

## Recent Completions

### 2026-03-03 (HEP Document Review + Code Review + Source Polish)
- ✅ **All 17 HEP documents updated** — mermaid diagrams, source file references, status fields
- ✅ **5-pass code review** (L0-1, L2, L3, L4, cross-cutting) — all passes EXCELLENT/PASS
  - Fixed: `debug_info.hpp` duplicate @file header, `scope_guard.hpp` missing @file,
    `recursion_guard.hpp` wrong filename in @file, `hub_queue.hpp`/`hub_processor.hpp` stale
    HEP-0012 cross-references → HEP-0015, `data_block.hpp` include path inconsistency
- ✅ **Umbrella header reorganization** — 15 orphan headers added to appropriate umbrella:
  `result.hpp` → L1; `uid_utils/uuid_utils/interactive_signal_handler/zmq_context` → L2;
  `heartbeat_manager/recovery_api/slot_diagnostics/slot_recovery/integrity_validator` → L3a;
  `channel_access_policy/channel_pattern/data_block_mutex` → L3b
- ✅ **Example build fix** — `0xBAD5ECRET` invalid literal → `0xBAD5EC` in both datahub examples
  **Total: 750/750 passing (no changes to test code).**

### 2026-03-03 (hub::Processor Enhancements + Dual-Broker + ScriptHost Dedup + C++ Templates)
- ✅ **hub::Processor enhanced API tests** (11 tests) — `test_datahub_hub_processor.cpp`:
  `TimeoutHandler_ProducesOutput`, `TimeoutHandler_NullOutputOnDrop`, `IterationCount_AdvancesOnTimeout`,
  `CriticalError_StopsLoop`, `CriticalError_FromTimeoutHandler`, `PreHook_CalledBeforeHandler`,
  `PreHook_CalledBeforeTimeout`, `ZeroFill_OutputZeroed`, `ZmqQueue_Roundtrip`,
  `ZmqQueue_NullFlexzone`, `ZmqQueue_TimeoutHandler`.
- ✅ **Dual-broker config tests** (5 tests) — `test_processor_config.cpp`:
  `DualBroker_BothPresent`, `DualBroker_FallbackToSingle`, `DualBroker_InHubDir`,
  `DualBroker_OutHubDir`, `DualBroker_MixedConfig`.
- ✅ **ScriptHost dedup** — `RoleHostCore` (engine-agnostic infrastructure) + `PythonRoleHostBase`
  (Python common layer with ~15 virtual hooks); three role subclasses reduced from ~790 to ~150 lines each.
- ✅ **C++ processor pipeline template** — `examples/cpp_processor_template.cpp`; demonstrates
  LifecycleGuard → BrokerService → Producer → Consumer → ShmQueue → Processor → typed handler.
  Build with `PYLABHUB_BUILD_EXAMPLES=ON`.
- ✅ **ZMQ wire format documentation** — HEP-CORE-0002 §7.1 added.
  **Total: 750/750 passing (734 + 16 new tests).**

### 2026-03-02 (Test Gap Closure + Script Host Deduplication)
- ✅ **Script host deduplication** — Extracted 14 shared functions + 3 types from
  processor/producer/consumer `*_script_host.cpp` into `script_host_schema.hpp` (types)
  and `script_host_helpers.hpp` (inline functions) in `pylabhub::scripting` namespace.
  Per-component `*_schema.hpp` files reduced to thin `using` aliases.
- ✅ **B1: Messenger hex codec tests** (8 tests) — new `test_datahub_messenger_protocol.cpp`;
  `hex_encode_schema_hash`/`hex_decode_schema_hash` roundtrip, empty, invalid chars,
  too short/long, case-insensitive, known vectors (all-zero, all-0xFF).
- ✅ **B2: Messenger not-connected guard tests** (4 tests) — added to `test_datahub_messagehub.cpp`;
  `query_channel_schema`, `create_channel`, `connect_channel` return nullopt when not connected;
  `heartbeat_noop_not_running` (suppress/enqueue no-op).
- ✅ **B3: InteractiveSignalHandler lifecycle tests** (7 tests) — new
  `test_interactive_signal_handler.cpp`; constructor stores config, `set_status_callback` before
  install, install/uninstall toggles `is_installed()`, install idempotent, uninstall idempotent,
  RAII destructor on installed handler, force_daemon config cycle. All use `force_daemon=true`.
- ✅ **B4: Messenger callback registration tests** (3 tests) — added to `test_datahub_messagehub.cpp`;
  `on_channel_closing` global register, per-channel register/deregister (nullptr), `on_consumer_died` register.
  **Total: 734/734 passing (712 + 22 new tests).**

### 2026-03-02 (L0–L3 Test Gap Closure — Phase 2, Audit-Driven)
- ✅ **L1 BackoffStrategy tests** (10 tests) — new `test_backoff_strategy.cpp`; ThreePhaseBackoff
  (3 phase transitions), ConstantBackoff (default/custom/iteration-independent), NoBackoff (instant),
  AggressiveBackoff (phase1/capped), free function. Wide timing tolerances for cross-platform.
- ✅ **L1 ModuleDef tests** (16 tests) — new `test_module_def.cpp`; Constants, Constructor
  (valid/empty/max/too-long), Move (ctor/assign), AddDependency (valid/empty/too-long), SetStartup
  (no-arg/with-arg/too-long), SetShutdown (no-arg/too-long), SetAsPersistent. Builder API only.
- ✅ **L1 DebugInfo tests** (7 tests) — new `test_debug_info.cpp`; PrintStackTrace (with/without
  external tools), PLH_PANIC (aborts/includes source location via EXPECT_DEATH), debug_msg_rt
  (no crash/format error swallowed), SRCLOC_TO_STR format.
- ✅ **L0 PlatformShm edge cases** (5 tests) — extended `test_platform_shm.cpp`; ShmClose_NullHandle,
  ShmClose_AlreadyClosed, ShmAttach_NullName, ShmUnlink_NullName, ShmUnlink_Nonexistent.
- ✅ **L2 ZmqContext concurrency** (1 test) — MultiThread_GetContext_Safe (4 threads, same pointer).
- ✅ **L2 HubVault/ActorVault** (4 tests) — Create_OverExisting + MoveConstructor for both vault types.
- ✅ **L2 SharedSpinLock** (3 tests) — IsLockedAfterUnlock, BlockingLock_WaitsForRelease, ExcessUnlock_Throws.
- ✅ **L2 ScriptHost** (1 test) — FinalizeNotCalledAfterFailedInit.
- ✅ **L3 Processor** (3 tests) — ProcessorClose, ProcessorHandlerHotSwap (handler swap after
  input_timeout cycle), ProcessorHandlerRemoval (nullptr→re-install).
- ✅ **L3 ZmqQueue overflow** (2 tests) — PullFrom_BufferFull_DropsOldest (depth-4 buffer),
  PullFrom_BufferFull_NoDeadlock (rapid push, no hang).
- ✅ **L3 SchemaRegistry search dirs** (2 tests) — SetSearchDirs_LoadsFromCustomPath,
  SetSearchDirs_OverridesDefault. Use temp directories with real schema files.
  **Total: 705/705 passing (651 + 54 new tests).**

### 2026-03-02 (L0–L3 Test Gap Closure — cross-platform aware)
- ✅ **L1 format_tools tests** (8 tests) — `make_buffer` basic/empty, `make_buffer_rt` basic/empty,
  `filename_only` unix/windows/no-separator/empty. Added to `test_formattable.cpp`.
- ✅ **L2 uid_utils tests** (18 tests) — new `test_uid_utils.cpp`; generators (5 roles), generators
  with name (3), validators (5), `sanitize_name_part` (5: normal/special chars/empty/too long/dashes).
- ✅ **L2 ActorVault tests** (12 tests) — new `test_actor_vault.cpp`; mirrors HubVault pattern;
  creation (4: writes file, restricted perms, valid Z85, empty password), opening (4: correct password,
  wrong password throws, corrupted throws, missing throws), encryption (2: secrets not plaintext,
  different UID different ciphertext), identity (2: UID roundtrip, different UIDs different keys).
  Permission test guarded with `#ifndef PYLABHUB_PLATFORM_WIN64`.
- ✅ **L2 ZmqContext tests** (3 tests) — new `test_zmq_context.cpp`; lifecycle fixture with
  `GetZMQContextModule()`; `GetContext_ReturnsValid`, `GetContext_SameInstance`, `CreateSocket_Works`.
- ✅ **L3 ZmqQueue tests** (11 tests) — new `test_datahub_hub_zmq_queue.cpp`; factory (2), lifecycle (3),
  read/write roundtrip (3: single item, multiple items, read timeout returns null), write semantics (2:
  abort not sent, item_size correct), metadata (1: name returns endpoint). All use `tcp://127.0.0.1:<port>`.
- ✅ **L3 SchemaLibrary file-loading tests** (3 tests) — added `DatahubSchemaFileLoadTest` to
  `test_datahub_schema_library.cpp`; `LoadFromDir_SingleFile`, `LoadFromDir_NestedPath`,
  `LoadFromDir_InvalidJson_Skipped`. Uses temp directories with real `.json` schema files.
- ✅ **L3 BrokerService admin API tests** (8 tests) — new `test_datahub_broker_admin.cpp`; in-process
  `LocalBrokerHandle` pattern; `ListChannels_Empty/OneChannel/FieldPresence` (3),
  `Snapshot_Empty/OneChannel/AfterConsumer` (3), `CloseChannel_Existing/NonExistent` (2).
- ✅ **XPLAT comments** — 4 cross-platform documentation comments added: `data_block_mutex.cpp` (CLOCK_REALTIME),
  `actor_vault.hpp` (getpass), `backoff_strategy.hpp` (sleep resolution), `shared_memory_spinlock.cpp` (PID reuse).
  **Total: 651/651 passing (588 + 63 new tests).**

### 2026-03-02 (HEP-CORE-0016 Phase 4: SchemaStore lifecycle)
- ✅ **SchemaStore lifecycle singleton tests: `SchemaRegistryTest`** (8 tests) —
  `test_datahub_schema_registry.cpp` in `tests/test_layer3_datahub/`; lifecycle fixture
  (Logger + SchemaStore); covers `GetInstance_SameAddress`, `LifecycleInitialized_True`,
  `RegisterAndGet`, `IdentifyByHash`, `GetUnknown_Nullopt`, `IdentifyUnknown_Nullopt`,
  `Reload_ClearsAndReloads`, `ListSchemas`. **Total: 588/588 passing.**

### 2026-03-02 (Layer 4 processor tests)
- ✅ **Layer 4 config unit tests: `ProcessorConfigTest`** (10 tests) — `test_processor_config.cpp`
  in `tests/test_layer4_processor/`; `PureApiTest` fixture; `processor_config.cpp` compiled directly
  into test binary; covers `from_json_file` (basic fields, uid autogen, in/out schemas, missing
  in_channel, missing out_channel, malformed JSON, file not found), `from_directory` (with script_path
  resolution), `overflow_policy` default (block) and invalid (throws).
- ✅ **Layer 4 CLI integration tests: `ProcessorCliTest`** (6 tests) — `test_processor_cli.cpp`;
  `IsolatedProcessTest` fixture; spawns `pylabhub-processor` binary via `WorkerProcess` (path
  derived as `g_self_exe_path/../bin/pylabhub-processor`); covers `--init` (directory structure +
  default values including both in/out channels and overflow_policy), `--keygen` (vault file created,
  stdout mentions `Processor vault written to` + `public_key`), `--validate` (exits 0, prints
  "Validation passed"), malformed JSON (non-zero exit + "Config error" in stderr), and missing
  config file (non-zero exit). **Total: 580/580 passing.**

### 2026-03-02 (HEP-CORE-0016 Phase 3 — Broker Schema Protocol Tests)
- ✅ **7 `BrokerSchemaTest` tests** — `test_datahub_broker_schema.cpp` in `tests/test_layer3_datahub/`:
  - `SchemaId_StoredOnReg` — producer registers with schema_id; query_channel_schema echoes it back
  - `SchemaBlds_StoredOnReg` — BLDS string stored at REG_REQ time and returned via SCHEMA_REQ
  - `SchemaHash_ReturnedOnQuery` — raw 32-byte hash → hex-encoded round-trip via broker
  - `SchemaReq_UnknownChannel_ReturnsNullopt` — query for unregistered channel returns nullopt
  - `ConsumerSchemaId_IdMatch_Succeeds` — consumer expected_schema_id matches producer's → connect succeeds
  - `ConsumerSchemaId_Mismatch_Fails` — consumer expected_schema_id differs → connect_channel returns nullopt
  - `ConsumerSchemaId_EmptyProducer_Fails` — anonymous producer + consumer expects schema_id → fails
  - Uses in-process `LocalBrokerHandle` pattern (ephemeral port, empty SchemaLibrary)
  - Bug fix: `messenger_protocol.cpp` CONSUMER_REG error now returns nullopt (was fire-and-forget)
  **Total: 564/564 passing.**

### 2026-03-02 (HEP-CORE-0016 Phase 2 — Schema Integration Tests)
- ✅ **`validate_named_schema<DataT, FlexT>(schema_id, lib)`** — template free function added to
  `schema_library.hpp`; performs: (1) forward lookup via `lib.get(schema_id)`, (2) `sizeof(DataT)`
  vs `slot_info.struct_size` size check, (3) BLDS hash check when `PYLABHUB_SCHEMA_BEGIN/MEMBER/END`
  macros used (detected by `has_schema_registry_v<DataT>`), (4) flexzone size + hash check when
  `FlexT != void`. Throws `SchemaValidationException` on any mismatch or unknown ID; no-op when
  `schema_id` is empty. Companion `validate_named_schema_from_env<>()` builds library from
  `default_search_dirs()` + `load_all()` then delegates.
- ✅ **`has_schema_registry<T>` trait** added to `schema_blds.hpp` — detects `SchemaRegistry<T>`
  specialization via `std::void_t`; exported as `has_schema_registry_v<T>`.
- ✅ **`ProducerOptions::schema_id`** and **`ConsumerOptions::expected_schema_id`** — new `std::string`
  fields (default `""`); when non-empty, `Producer::create<F,D>()` / `Consumer::connect<F,D>()`
  call `validate_named_schema_from_env<DataBlockT, FlexZoneT>(schema_id)` at entry.
- ✅ **7 `DatahubSchemaPhase2Test` tests** — `MatchingStruct_NoThrow`, `EmptySchemaId_NoCheck`,
  `UnknownId_Throws`, `SlotSizeMismatch_Throws`, `SlotHashMismatch_Throws`,
  `FlexzoneSizeMismatch_Throws`, `MatchingFlexzone_NoThrow`. Use in-memory `SchemaLibrary`
  (no file I/O, no env vars). **Total: 557/557 passing.**

### 2026-03-02 (Layer 4 producer + consumer tests)
- ✅ **Layer 4 config unit tests: `ProducerConfigTest`** (8 tests) — `test_producer_config.cpp`
  in `tests/test_layer4_producer/`; `PureApiTest` fixture; `producer_config.cpp` compiled directly
  into test binary; covers `from_json_file` (basic fields, uid autogen, schema fields, missing channel,
  malformed JSON, file not found), `from_directory` (with omitted `hub_dir`), and
  `stop_on_script_error` default false.
- ✅ **Layer 4 CLI integration tests: `ProducerCliTest`** (6 tests) — `test_producer_cli.cpp`;
  `IsolatedProcessTest` fixture; spawns `pylabhub-producer` binary via `WorkerProcess` (path
  derived as `g_self_exe_path/../bin/pylabhub-producer`); covers `--init` (directory structure +
  default values), `--keygen` (vault file created, stdout mentions `public_key`), `--validate`
  (exits 0, prints "Validation passed"), malformed JSON (non-zero exit + "Config error" in
  stderr), and missing config file (non-zero exit). Key fix: `script_path` must be the *parent*
  of `script/<type>/`, not `script/` itself — binary appends `script/<type>/__init__.py`.
- ✅ **Layer 4 config unit tests: `ConsumerConfigTest`** (6 tests) — `test_consumer_config.cpp`
  in `tests/test_layer4_consumer/`; symmetric to producer; `CONS-` prefix; no `shm_slot_count`,
  no `update_checksum`; covers `from_json_file` (5 variants) and `from_directory`.
- ✅ **Layer 4 CLI integration tests: `ConsumerCliTest`** (6 tests) — `test_consumer_cli.cpp`;
  spawns `pylabhub-consumer` binary; same 6 scenarios as producer CLI tests; "Consumer vault
  written to" in stdout for keygen. **Total: 550/550 passing.**

### 2026-02-28 (Actor CLI integration tests)
- ✅ **Layer 4 CLI tests: `pylabhub-actor` CLI integration** (12 tests) — `test_layer4_actor_cli`
  executable using `pylabhub::test_framework` (`WorkerProcess`/`g_self_exe_path`); actor binary
  derived from staged path (`../bin/pylabhub-actor` relative to test binary); covers:
  `--keygen` (write/missing-keyfile/create-parent-dir/overwrite), `--register-with`
  (append/idempotent/missing-actor/missing-hub), config error paths (malformed JSON,
  missing roles, invalid kind, file not found). Fixed: `auth.keyfile` is inside `actor` block,
  not top-level. All Tier 1 (no Python/broker). Total: **585/585 passing**.

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
