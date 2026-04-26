# Test Compliance Audit — L1/L2/L3/L4

**Date**: 2026-04-18 (audit snapshot); progress refreshed 2026-04-21.
**Status**: **Active reference document.** §1-§5 are the authoritative
contract + violation taxonomy + file-level verdicts — still accurate
for the Pattern 3 conversion sweep. §7 progress tracker is updated
through 2026-04-21 (L2 complete; L3 tracked in harness tasks
`21.L4` + `21.L5`).
**Purpose**: Ground-truth inventory of every test `.cpp` file under
`tests/test_layer[1-4]_*/` against the framework contract. No fixes;
findings only (remediation is tracked separately).
**Scope**: 96 test files (excluding `workers/` subdirs, which are
Pattern-3 implementation files not subject to fixture rules).

---

## 1. The contract (absolute, no exceptions)

> **Any test that touches lifecycle state runs in a new subprocess. No in-process `LifecycleGuard`. Ever.**

Stated by the user, 2026-04-18. This is the binary rule: any test whose body (or any member/helper it calls) constructs, invokes, or transitively depends on a lifecycle-module-backed class is required to spawn a fresh subprocess whose own `main()` does the lifecycle work inside a worker dispatcher wrapped by `run_gtest_worker` or `run_worker_bare` (which `_exit()` to skip static destructors — see `tests/test_framework/shared_test_helpers.h:303-322`).

### Sanctioned patterns
| Pattern | Fixture base | Lifecycle? | Where it lives |
|---|---|---|---|
| **1** | `PureApiTest` (in `tests/test_framework/test_patterns.h:89-94`) or plain `::testing::Test` / plain `TEST()` | **No** | Main gtest runner (in-process) |
| **2** | plain `::testing::Test` with `ThreadRacer` | No (threading only) | Main gtest runner (in-process) |
| **3** | `IsolatedProcessTest` (`test_patterns.h:111-206`) | Yes — in forked subprocess only | Worker dispatcher → `run_gtest_worker`/`run_worker_bare` |

### Forbidden in-process idioms (all constitute violations)
- `SetUpTestSuite` + `static std::unique_ptr<LifecycleGuard>` — "V1 suite-scope compromise"
- `SetUp` / `TearDown` + per-test `LifecycleGuard` member — "V1 per-test compromise"
- `LifecycleGuard guard(...)` inside a test-body scope that runs in the gtest process — "V1 inline"
- "Logger-only" or empty-module-list `LifecycleGuard` in fixture scope — still V1
- Fabricating a lifecycle-requiring class (`RoleAPIBase`, `Logger::instance().set_logfile(...)`, `JsonConfig`, `FileLock`, ZMQ ops, engine `.initialize()`, etc.) without ANY guard — "V2 no-guard fabrication"

---

## 2. Lifecycle module inventory

These classes define their own lifecycle module via `Get*LifecycleModule()` or `GetZMQContextModule()` / `GetDataBlockModule()`:

| Module | Header | Registration API |
|---|---|---|
| `Logger` | `src/include/utils/logger.hpp:97` | `Logger::GetLifecycleModule()` |
| `FileLock` | `src/include/utils/file_lock.hpp:143` | `FileLock::GetLifecycleModule()` |
| `JsonConfig` | `src/include/utils/json_config.hpp:283` | `JsonConfig::GetLifecycleModule()` |
| `HubConfig` | `src/include/utils/hub_config.hpp:110` | `HubConfig::GetLifecycleModule()` |
| `SchemaStore` | `src/include/utils/schema_registry.hpp:52` | `SchemaStore::GetLifecycleModule()` |
| `CryptoUtils` | `src/include/utils/crypto_utils.hpp:190` | `crypto::GetLifecycleModule()` |
| `ZMQContext` | `src/include/utils/zmq_context.hpp:45` | `hub::GetZMQContextModule()` |
| `DataBlock` | `src/include/utils/data_block.hpp:1217` | `hub::GetDataBlockModule()` |

### Classes with in-ctor `lifecycle_initialized()` check (PLH_PANIC if missing)
- `JsonConfig` (`src/utils/config/json_config.cpp:83-87, 94-98`) — aborts without its module
- `FileLock` — per header `src/include/utils/file_lock.hpp:149`
- `SchemaStore` — per `src/utils/schema/schema_registry.cpp:64`
- `HubConfig` — per `src/utils/config/hub_config.cpp:534`

### Classes transitively requiring lifecycle (register dynamic modules or use lifecycle-backed singletons)
- **`ThreadManager`** (`src/utils/service/thread_manager.cpp:175-185`) — registers dynamic module at ctor; declares `depends_on "pylabhub::utils::Logger"`. Silently degrades if LifecycleManager not initialized, but degrades unsafely (hang demonstrated in `MetricsApiTest.ProcessorAPI_ReportAndSnapshot`).
- **`RoleAPIBase`** (`src/utils/service/role_api_base.cpp:47-48`) — eagerly constructs `ThreadManager` in pImpl ctor.
- **`RoleHostBase`** (`src/utils/service/role_host_base.cpp:74-77`) — lazily constructs `RoleAPIBase` in `startup_()`.
- **`RoleConfig::load` / `RoleConfig::load_from_directory`** (`src/utils/config/role_config.cpp`) — uses `JsonConfig`.
- **`BrokerService`**, **`BrokerRequestComm`**, **`Messenger`**, **`InboxQueue`**, **`InboxClient`** — use ZMQ context + Logger.
- **`hub::Producer`**, **`hub::Consumer`**, `ShmQueue`, `ZmqQueue` — use `DataBlock` / `ZMQContext`.
- **`Logger::instance().set_logfile(...)` / `set_rotating_logfile(...)`** — depends on Logger module being loaded.

---

## 3. File inventory & verdicts

**Legend**:
- **P1** = Pattern 1 (pure / in-process, no lifecycle) — compliant
- **P3** = Pattern 3 (`IsolatedProcessTest` / SpawnWorker) — compliant
- **V1** = In-process `LifecycleGuard` in fixture or test body — violation
- **V2** = No guard + fabricates lifecycle-requiring class — violation
- **V3** = Mixed/stray (needs spot-verification) — may be violation

File locations: all under `tests/` (path prefix omitted).

### 3.1 Layer 1 (`test_layer1_base/`) — 9 files

| File | Verdict | Notes (line refs into the file) |
|---|---|---|
| `test_backoff_strategy.cpp` | **P1** | Plain `TEST()` macros. No lifecycle, no threads. |
| `test_debug_info.cpp` | **P1** | Plain `TEST()`. Uses `EXPECT_DEATH` for panic tests. |
| `test_formattable.cpp` | **P1** | `FormatToolsTest : ::testing::Test`. No lifecycle. |
| `test_module_def.cpp` | **P1** | Plain `TEST()` — pure builder API. |
| `test_portable_atomic_shared_ptr.cpp` | **P1** | Plain `TEST()`. Threads for atomic race coverage; no lifecycle. |
| `test_recursionguard.cpp` | **P1** | Plain `TEST()`. |
| `test_result.cpp` | **P1** | Plain `TEST()`. Pure `Result<T,E>`. |
| `test_scopeguard.cpp` | **P1** | Plain `TEST()`. Pure RAII. |
| `test_spinlock.cpp` | **P1** | Plain `TEST()`. In-process spinlock state. |

**L1 totals**: 9 files, **0 violations**.

### 3.2 Layer 2 (`test_layer2_service/`) — 37 files

| File | LG count | SW count | Verdict | Evidence / notes |
|---|---|---|---|---|
| `test_backoff_strategy.cpp` | 0 | 0 | **P1** | Duplicate of L1 file (timing verification). Pure. |
| `test_configure_logger.cpp` | 4 | 0 | **V1** | `guard_` member, per-test `SetUp` at line 56; `TearDown` releases. Per-test LifecycleGuard. Fabricates `RoleConfig::load_from_directory` + `Logger::set_rotating_logfile`. (my 21.2) |
| `test_crypto_utils.cpp` | 0 | 21 | **P3** | `IsolatedProcessTest`; all 21 tests spawn workers. |
| `test_engine_factory.cpp` | 0 | 0 | **P1** | Plain `TEST()`. Constructs `NativeEngine`/`LuaEngine`/`PythonEngine` objects but does NOT call `.initialize()` or use Logger. Pure factory dispatch. (my 21.1) |
| `test_filelock.cpp` | 3 | 10 | **V1** | `static s_lifecycle` at line 37; `SetUpTestSuite` at line 39 creates in-process `LifecycleGuard` with FileLock+Logger modules. All test bodies SpawnWorker — but the parent-side guard is still a contract violation. |
| `test_filelock_singleprocess.cpp` | 3 | 0 | **V1** | `static s_lifecycle` + `SetUpTestSuite` at line 46. In-process FileLock tests using the guard state. |
| `test_framework_selftest.cpp` | 0 | 4 | **P3** | All tests spawn workers to validate the framework. |
| `test_hub_vault.cpp` | 0 | 0 | **P1** | `HubVaultTest`; no lifecycle-requiring fabrication. |
| `test_interactive_signal_handler.cpp` | 2 | 0 | **V1** | Inline `LifecycleGuard guard(MakeModDefList(Logger::GetLifecycleModule()))` at line 152 inside a test body. |
| `test_jsonconfig.cpp` | 4 | 0 | **V1** | `static s_lifecycle` + `SetUpTestSuite` at line 57. All tests use JsonConfig in-process under that guard. |
| `test_lifecycle.cpp` | 3 | 19 | **P3** | `LG` matches are string literals in comments and `HasSubstr(...)` assertions (e.g., line 28); no actual guard in this file. All tests SpawnWorker. |
| `test_lifecycle_dynamic.cpp` | 0 | 13 | **P3** | Pure Pattern 3. |
| `test_logger.cpp` | 1 | 17 | **P3** | `LG` match at line 1 is a comment. All tests SpawnWorker. |
| `test_loop_timing_policy.cpp` | 0 | 0 | **P1** | Pure policy-struct parsing/validation. |
| `test_lua_engine.cpp` | 0 | 0 | **V2** | `std::make_unique<RoleAPIBase>` at line 86 — fabricates ThreadManager without any lifecycle. No `LifecycleGuard` anywhere. Also calls `LuaEngine::initialize()` which uses Logger. |
| `test_metrics_api.cpp` | 0 | 0 | **V2** | `std::make_unique<RoleAPIBase>` at line 37 — no guard. Demonstrated flake source (`MetricsApiTest.ProcessorAPI_ReportAndSnapshot` 60s timeout). |
| `test_net_address.cpp` | 0 | 0 | **P1** | Pure address parsing. |
| `test_python_engine.cpp` | 0 | 0 | **V2** | `std::make_unique<RoleAPIBase>` at line 108 — no guard. Also uses `py::scoped_interpreter` via fixture. |
| `test_role_cli.cpp` | 0 | 0 | **P1** | Pure `parse_role_args` argv tests. No lifecycle. (my 21.1) |
| `test_role_config.cpp` | 3 | 0 | **V1** | `static s_lifecycle` + `SetUpTestSuite` at line 35 with JsonConfig+FileLock+Logger modules. Uses `RoleConfig::load` throughout. |
| `test_role_data_loop.cpp` | 0 | 0 | **V2** | Two fixture classes: `RunDataLoopTest` (line 69) and `ThreadManagerTest` (line 213). Both fabricate `RoleAPIBase` (lines 79, 261) without any guard. |
| `test_role_directory.cpp` | 0 | 0 | **P1** | Pure `RoleDirectory` path/validation API. No lifecycle-requiring fabrication. |
| `test_role_host_base.cpp` | 2 | 0 | **V1** | `lifecycle_` member + `SetUp`/`TearDown` per-test at line 122. Fabricates `RoleHostBase` via test subclass + `startup_`. (my 21.3) |
| `test_role_host_core.cpp` | 0 | 0 | **P1** | Pure `RoleHostCore` state/metrics. |
| `test_role_init_directory.cpp` | 0 | 0 | **P1** | Only uses `RoleDirectory::init_directory` (writes files + nlohmann::json parse); no `RoleConfig::load`, no lifecycle. (my 21.2) |
| `test_role_logging_roundtrip.cpp` | 4 | 0 | **V1** | `guard_` member + `SetUp`/`TearDown` per-test at line 67. Fabricates `RoleConfig::load_from_directory`. (my 21.2) |
| `test_role_registry.cpp` | 0 | 0 | **P1** | Pure registry + map. No lifecycle. (my 21.1) |
| `test_role_vault.cpp` | 0 | 0 | **P1** | Pure RoleVault file I/O + crypto; no lifecycle. |
| `test_schema_validation.cpp` | 0 | 0 | **P1** | Pure schema API. |
| `test_scriptengine_native_dylib.cpp` | 0 | 0 | **V2** | `std::make_unique<RoleAPIBase>` at lines 94, 459, 497, 587, 628 — no guard. Also calls `NativeEngine::initialize()`. |
| `test_shared_memory_spinlock.cpp` | 0 | 2 | **P3** | Uses SpawnWorker for multi-process contention tests. |
| `test_slot_rw_coordinator.cpp` | 0 | 0 | **P1** | Pure coordinator state. |
| `test_slot_view_helpers.cpp` | 0 | 0 | **P1** | Pure ctypes/slot-view. |
| `test_uid_utils.cpp` | 0 | 0 | **P1** | Pure UID generation. |
| `test_zmq_context.cpp` | 5 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 28 with `GetZMQContextModule()`. All tests construct `zmq::context_t` under that guard. |

**L2 totals**:
- Compliant: 24 files (**P1** = 19, **P3** = 5)
- **Violations**: 13 files (**V1** = 8, **V2** = 5)

### 3.3 Layer 3 (`test_layer3_datahub/`) — 43 files

| File | LG | SW | Verdict | Evidence / notes |
|---|---|---|---|---|
| `test_datahub_broker_admin.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 161. |
| `test_datahub_broker_consumer.cpp` | 0 | 6 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_broker.cpp` | 0 | 6 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_broker_health.cpp` | 0 | 6 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_broker_protocol.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 192. |
| `test_datahub_broker_request_comm.cpp` | 0 | 4 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_broker_schema.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 155. |
| `test_datahub_broker_shutdown.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 195. |
| `test_datahub_c_api_checksum.cpp` | 0 | 6 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_c_api_recovery.cpp` | 0 | 7 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_c_api_slot_protocol.cpp` | 0 | 17 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_c_api_validation.cpp` | 0 | 5 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_channel_access_policy.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 182. |
| `test_datahub_channel_group.cpp` | 0 | 7 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_config_validation.cpp` | 0 | 6 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_e2e.cpp` | 0 | 1 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_exception_safety.cpp` | 0 | 3 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_handle_semantics.cpp` | 0 | 3 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_header_structure.cpp` | 0 | 3 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_hub_config_script.cpp` | 6 | 0 | **V1** | Two fixture classes each with `static s_lifecycle_` + `SetUpTestSuite` at lines 75 and 179. |
| `test_datahub_hub_federation.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 167. |
| `test_datahub_hub_inbox_queue.cpp` | 4 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 42. Comment at line 35-37 acknowledges "owns the process-wide LifecycleGuard" — the intentional compromise. |
| `test_datahub_hub_monitored_queue.cpp` | 0 | 0 | **P1** | Tests `hub::MonitoredQueue<T>` — a plain container class. File header (line 1-14) documents it as pure data-structure testing. No ZMQ / Logger / lifecycle-managed class. |
| `test_datahub_hub_queue.cpp` | 1 | 27 | **P3** | `LG` match at line 72 is a comment. All tests use SpawnWorker. |
| `test_datahub_hub_zmq_queue.cpp` | 4 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 85. |
| `test_datahub_integrity_repair.cpp` | 0 | 3 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_loop_policy.cpp` | 4 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 82. Comment at line 10-11 explicitly says "via SetUpTestSuite... held in a static LifecycleGuard". |
| `test_datahub_metrics.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 157. |
| `test_datahub_mutex.cpp` | 0 | 8 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_policy_enforcement.cpp` | 0 | 11 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_producer_consumer.cpp` | 0 | 10 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_recovery_scenarios.cpp` | 0 | 6 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_role_flexzone.cpp` | 4 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 41. Comment at line 10 notes the intent. |
| `test_datahub_role_state_machine.cpp` | 0 | 4 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_schema_blds.cpp` | 0 | 10 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_schema_library.cpp` | 0 | 0 | **P1** | Explicit file-header comment (line 5): *"SchemaLibrary is a plain utility class (no SHM, no ZMQ, no lifecycle)"*. |
| `test_datahub_schema_registry.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 60. |
| `test_datahub_schema_validation.cpp` | 0 | 5 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_stress_raii.cpp` | 0 | 2 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_transaction_api.cpp` | 0 | 6 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_write_attach.cpp` | 0 | 4 | **P3** | `IsolatedProcessTest`. |
| `test_datahub_zmq_endpoint_registry.cpp` | 3 | 0 | **V1** | `static s_lifecycle_` + `SetUpTestSuite` at line 151. |
| `test_datahub_zmq_poll_loop.cpp` | 0 | 0 | **P1 (borderline — flagged)** | Two fixtures. `PeriodicTaskTest` is pure logic. `ZmqPollLoopTest` at line 175-189 constructs its OWN `zmq::context_t` (line 180) — bypasses `hub::GetZMQContextModule()`. Transitively may use Logger via `ZmqPollLoop::run()`. Needs decision: (a) treat as legitimate P1 because it doesn't use the lifecycle-managed context, or (b) convert to P3 because any Logger usage under the hood violates the contract. |

**L3 totals**:
- Compliant: 28 (**P3** = 25, **P1** = 3)
- **Violations**: 14 (**V1** = 14)
- Borderline: 1 (`test_datahub_zmq_poll_loop.cpp` — classified P1 pending contract-interpretation call)

### 3.4 Layer 4 — 6 files

All L4 test files spawn binaries as child processes (not via `SpawnWorker`-to-self but via `WorkerProcess(<binary_path>, ...)`). All inherit from `IsolatedProcessTest` (where applicable). This is structurally equivalent to Pattern 3 — lifecycle runs in the child.

| File | Verdict | Notes |
|---|---|---|
| `test_layer4_integration/test_admin_shell.cpp` | **P3** | (Disabled — spawned the now-deleted `pylabhub-hubshell`; revisit when `plh_hub` lands per HEP-CORE-0033 §15 Phase 9.) |
| `test_layer4_integration/test_channel_broadcast.cpp` | **P3** | (Disabled — same reason.) |
| `test_layer4_integration/test_pipeline_roundtrip.cpp` | **P3** | (Disabled — same reason.) |
| `test_layer4_producer/test_producer_cli.cpp` | **P3** | Spawns `pylabhub-producer`. Scheduled for deletion in HEP-0024 Phase 20. |
| `test_layer4_consumer/test_consumer_cli.cpp` | **P3** | Same. Scheduled for deletion. |
| `test_layer4_processor/test_processor_cli.cpp` | **P3** | Same. Scheduled for deletion. |

**L4 totals**: 6 files, **0 violations**.

---

## 4. Violation summary

### Overall
| Layer | Files | P1 | P3 | V1 | V2 | Borderline |
|---|---|---|---|---|---|---|
| L1 | 9 | 9 | 0 | 0 | 0 | 0 |
| L2 | 37 | 19 | 5 | 8 | 5 | 0 |
| L3 | 43 | 3 | 25 | 14 | 0 | 1 |
| L4 | 6 | 0 | 6 | 0 | 0 | 0 |
| **Total** | **95** | **31** | **36** | **22** | **5** | **1** |

(One L2 file — test_backoff_strategy — is the same content as the L1 file; counted separately here since it's a separate binary target.)

### V1 violations (22) — in-process `LifecycleGuard`
**L2 (8)**:
- `test_configure_logger.cpp` *(my 21.2)*
- `test_filelock.cpp`
- `test_filelock_singleprocess.cpp`
- `test_interactive_signal_handler.cpp`
- `test_jsonconfig.cpp`
- `test_role_config.cpp`
- `test_role_host_base.cpp` *(my 21.3)*
- `test_role_logging_roundtrip.cpp` *(my 21.2)*
- `test_zmq_context.cpp`

(Note: `test_role_config.cpp` is the file I copied the `SetUpTestSuite` compromise from. It was the pre-existing template.)

**L3 (14)**:
- `test_datahub_broker_admin.cpp`
- `test_datahub_broker_protocol.cpp`
- `test_datahub_broker_schema.cpp`
- `test_datahub_broker_shutdown.cpp`
- `test_datahub_channel_access_policy.cpp`
- `test_datahub_hub_config_script.cpp`
- `test_datahub_hub_federation.cpp`
- `test_datahub_hub_inbox_queue.cpp`
- `test_datahub_hub_zmq_queue.cpp`
- `test_datahub_loop_policy.cpp`
- `test_datahub_metrics.cpp`
- `test_datahub_role_flexzone.cpp`
- `test_datahub_schema_registry.cpp`
- `test_datahub_zmq_endpoint_registry.cpp`

### V2 violations (5) — no guard + fabricates lifecycle-requiring class
**All in L2, all fabricate `RoleAPIBase`**:
- `test_lua_engine.cpp`
- `test_metrics_api.cpp` — **demonstrated flake source**
- `test_python_engine.cpp`
- `test_role_data_loop.cpp`
- `test_scriptengine_native_dylib.cpp`

### Borderline (1) — pending interpretation
- `test_datahub_zmq_poll_loop.cpp` — `ZmqPollLoopTest` fixture creates a private `zmq::context_t` without going through `hub::GetZMQContextModule()`. Does "touches lifecycle state" apply when the code bypasses the lifecycle-managed singleton but the class under test (`ZmqPollLoop`) may log via Logger internally? Contract interpretation required.

---

## 5. Notes on correction scope

**V1 and V2 are both resolved by the same mechanism**: convert the test logic into a worker function that uses `run_gtest_worker(lambda, name, mods...)` and have the parent test body call `SpawnWorker(...)` + `ExpectWorkerOk(...)`. The worker `.cpp` goes under `tests/test_layer<N>_*/workers/<module>_workers.cpp` and self-registers via `register_worker_dispatcher(...)`.

Two structural considerations for the correction pass (not designed here — raised for your direction):
1. **Fixture-level guards that never actually own lifecycle-dependent tests**: `test_filelock.cpp` creates a parent-side `LifecycleGuard` but every test only `SpawnWorker`s. The parent guard is pure overhead / violation noise. Removing it is a simpler fix than restructuring as Pattern 3.
2. **Pre-existing violations vs. my violations**: 18 of the 22 V1s are pre-existing; 4 are mine (21.2/21.3). Same mechanism fixes all.

---

## 6. Resolved audit — open questions for your call

1. **Borderline file interpretation**: `test_datahub_zmq_poll_loop.cpp`'s `ZmqPollLoopTest` fixture creates its own `zmq::context_t` at `:180` but the class under test (`ZmqPollLoop::run()`) is likely using Logger internally. Does the contract's "touches lifecycle state" apply here, or does bypassing the managed singleton mean it's OK? Needs a call before the correction pass — not a self-evident answer.

2. **Pre-existing V1 fixes**: 14/22 V1 violations are pre-existing (5 of the 8 L2 + all 14 L3). The established template was `test_role_config.cpp` (which I copied in my 21.2/21.3 work). Correcting all of them is consistent but broad; correcting only my new ones leaves the pre-existing pattern in place as a confusion source. Contract argues for full sweep.

---

## 7. Correction status

Refreshed 2026-04-21. **L2 sweep complete; L3 sweep pending** (tracked
under harness tasks `21.L4` 8-small + `21.L5` 6-large).

### L2 — ✅ all V1/V2 violations converted (2026-04-21)

| File | Violation | Status | Fixed in |
|---|---|---|---|
| `test_role_logging_roundtrip.cpp` | V1 | ✅ Fixed 2026-04-18 | `5d3683c` |
| `test_configure_logger.cpp` | V1 | ✅ Fixed 2026-04-18 | `5d3683c` |
| `test_role_host_base.cpp` | V1 | ✅ Fixed 2026-04-18 | `5d3683c` |
| `test_zmq_context.cpp` | V1 (suite-scope) | ✅ Fixed 2026-04-18 | `4b3fc44` (Phase 1) |
| `test_interactive_signal_handler.cpp` | V1 (1-of-9 inline) | ✅ Fixed 2026-04-18 | `4b3fc44` (Phase 1) |
| `test_filelock_singleprocess.cpp` | V1 (per-test) | ✅ Fixed 2026-04-18 | `4b3fc44` (Phase 1) |
| `test_metrics_api.cpp` | V2 (RoleAPIBase fab) | ✅ Fixed 2026-04-18 | `bc98e23` (Phase 2a) |
| `test_role_data_loop.cpp` | V2 (RoleAPIBase fab) | ✅ Fixed 2026-04-18 | `bc98e23` (Phase 2a) |
| `test_jsonconfig.cpp` | V1 (SetUpTestSuite) | ✅ Fixed 2026-04-19 | `5c1da4f` (Phase 2b) |
| `test_role_config.cpp` | V1 (SetUpTestSuite) | ✅ Fixed 2026-04-19 | `2af8d02` (Phase 2c) |
| `test_scriptengine_native_dylib.cpp` | V2 (RoleAPIBase fab) | ✅ Fixed 2026-04-19 | `124eba1` (Phase 2d) |
| `test_filelock.cpp` | V1 (suite-scope + parent-side FileLock) | ✅ Fixed 2026-04-19 | `3846de2` (Phase 2e; two-worker holder/contender pattern) |
| `test_lua_engine.cpp` | V2 (RoleAPIBase fab) | ✅ Fixed 2026-04-20 — chunk 13 completed sweep; V2 `LuaEngineTest` fixture removed; all 111 `LuaEngineIsolatedTest` tests use Pattern 3 | `cdaafa6` … (chunks 1-13) |
| `test_python_engine.cpp` | V2 (RoleAPIBase fab) | ✅ Fixed 2026-04-21 — 105 `PythonEngineIsolatedTest` Pattern 3; V2 `PythonEngineTest` fixture removed | — |

### Framework strengthening (cross-cutting, not per-file)

| Item | Status | Commit |
|---|---|---|
| Worker completion milestones (`[WORKER_BEGIN]`/`[WORKER_END_OK]`/`[WORKER_FINALIZED]`) | ✅ Active in all Pattern-3 workers | `2212a22` |
| `ExpectLegacyWorkerOk` opt-out for legacy multi-process IPC workers | ✅ Landed; 3 L3 files updated | `2212a22` |
| `all_types_schema()` helper covering bool/int8/int16/uint64 | ✅ Landed | `8dc76c3` |
| `test_patterns.h` + `shared_test_helpers.h` + `test_process_utils.*` documentation | ✅ Updated | `2212a22`, `a63cb79` |
| HEP-CORE-0001 § "Testing implications" | ✅ Authored | `a63cb79` |
| README_testing.md §4 "Choosing a test pattern" + antipatterns + milestones | ✅ Authored | `a63cb79` |

### L3 — still open; 15 files unchanged (verified 2026-04-21)

Grep-verified 2026-04-21: **15 files in `tests/test_layer3_datahub/`
still use the V1 `SetUpTestSuite` + `static std::unique_ptr<LifecycleGuard>`
pattern**. None have been converted to Pattern 3. This tracks the
audit's "14 V1 + 1 borderline" count (the extra is the borderline
file found by the sweep).

Sample (verified by reading `test_datahub_broker_admin.cpp:158-191`):

```cpp
class BrokerAdminTest : public ::testing::Test {
    static void SetUpTestSuite() {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(...);
    }
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};
```

This is the exact V1 pattern §4 flags. README_testing.md §398 calls it
a non-solution ("Still in-process; still runs pylabhub static-dtor
order at program exit; hides the 60s hang"). It is **not** a
"fixed per real needs" case — these files have simply not yet been
converted.

**Why the suite still passes at 1456/1456**: partial luck + framework
robustness (worker-completion milestones from commit `2212a22` catch
some regressions). But `21.X` ("Investigate teardown hangs +
lifecycle race conditions") is `in_progress` in the harness task list
— episodic SIGTERM-after-60s failures under `-j2` load confirm the
class of bug is still present:
- `DatahubShmQueueTest.ShmQueueWriteFlexzone` (mitigated by
  `ThreePhaseBackoff` cap at 10ms, commit `1d3e584`)
- `DatahubSlotDrainingTest.DrainHoldTrueNeverReturnsNullptr` (same)
- `DatahubHeaderStructureTest.SchemaHashesPopulatedWithTemplateApi`
  (2026-03-18; probable lifecycle teardown hang under load)

**File-by-file breakdown** (`LifecycleGuard` count + recommended task):

| File | Count | Task |
|---|---|---|
| `test_datahub_hub_config_script.cpp` | 6 | 21.L5 large |
| `test_datahub_role_flexzone.cpp` | 4 | 21.L5 large |
| `test_datahub_loop_policy.cpp` | 4 | 21.L5 large |
| `test_datahub_hub_zmq_queue.cpp` | 4 | 21.L5 large |
| `test_datahub_hub_inbox_queue.cpp` | 4 | 21.L5 large |
| `test_datahub_zmq_endpoint_registry.cpp` | 3 | 21.L5 large |
| `test_datahub_metrics.cpp` | 3 | 21.L5 large |
| `test_datahub_hub_federation.cpp` | 3 | 21.L5 large |
| `test_datahub_schema_registry.cpp` | 3 | 21.L4 small |
| `test_datahub_channel_access_policy.cpp` | 3 | 21.L4 small |
| `test_datahub_broker_shutdown.cpp` | 3 | 21.L4 small |
| `test_datahub_broker_schema.cpp` | 3 | 21.L4 small |
| `test_datahub_broker_protocol.cpp` | 3 | 21.L4 small |
| `test_datahub_broker_admin.cpp` | 3 | 21.L4 small |
| `test_datahub_hub_queue.cpp` | 1 | 21.L4 small |

**Conversion tracked by harness tasks**:
- `21.L4` — Pattern 3 conversion: 8 small L3 violators
- `21.L5` — Pattern 3 conversion: 6 large L3 violators (plus 1
  moved from 21.L4 after re-classification; see breakdown above —
  totals may need rebalancing when conversion starts)

Three other L3 parents (`test_datahub_mutex.cpp`,
`test_datahub_broker_request_comm.cpp`,
`test_datahub_channel_group.cpp`) were updated to opt out of the new
milestone check via `ExpectLegacyWorkerOk` since their workers
legitimately bypass `run_gtest_worker` — those remain on the "convert
properly" list too.

### New gap-fill tests added during the sweep (not fixes to violations)

These are tests that did not exist before the sweep, added because the
review-and-augment pass found real coverage holes:

| Test | Commit | Fills |
|---|---|---|
| `LuaEngineIsolatedTest.RegisterSlotType_AllSupportedTypes_Succeeds` | `cdaafa6` | `bool`/`int8`/`int16`/`uint64` type dispatch |
| `LuaEngineIsolatedTest.RegisterSlotType_Packed_vs_Aligned` | `cdaafa6` | Verifies packing arg is honoured, not ignored |
| `LuaEngineIsolatedTest.InvokeProduce_DiscardOnFalse_ButLuaWroteSlot` | `712b770` | Discard does NOT roll back Lua-side writes |
| `LuaEngineIsolatedTest.InvokeConsume_RxSlot_IsReadOnly` | `89b0e9e` | Consumer rx.slot read-only contract (was named but not tested) |

### Next (remaining work as of 2026-04-21)

- **L3 V1 sweep** (15 files) — tracked as harness tasks `21.L4`
  (8 small violators) + `21.L5` (6 large violators), plus the
  1 borderline case (`test_datahub_zmq_poll_loop.cpp`).
- **Before closing the audit**: re-grep every `tests/test_layer*/`
  for stale V1/V2 patterns in case a new file was introduced
  mid-sweep.

End of audit.
