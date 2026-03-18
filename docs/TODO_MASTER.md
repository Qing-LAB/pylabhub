# Data Exchange Hub - Master TODO

**Purpose:** This is the **master execution plan** for the DataHub project. It provides a high-level overview of what needs to be done and references to detailed TODO documents for specific areas.

**Philosophy:** Keep this document concise and high-level. Detailed tasks, completion tracking, and phase-specific work belong in subtopic TODO documents.

---

## Overview

The Data Exchange Hub (DataHub) is a cross-platform IPC framework using shared memory for high-performance data transfer. Current focus is on Layer 4 test coverage for the standalone producer/consumer/processor binaries and Named Schema Registry integration.

**Key Documents:**
- **Design Spec**: `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`
- **Implementation Guidance**: `docs/IMPLEMENTATION_GUIDANCE.md`
- **Pipeline Architecture**: `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md`
- **Producer + Consumer Binaries**: `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md`
- **Named Schema Registry**: `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md` (Phase 1 done)
- **Policy Reference**: `docs/HEP/HEP-CORE-0009-Policy-Reference.md` (active cross-reference)

---

## Current Sprint Focus

### Priority 0 (IN PROGRESS — 2026-03-16): Lua Role Support (feature/lua-role-support)
📍 **Status**: Phase 1+2 complete (all 3 roles); dedup done; API gaps remaining
📋 **Branch**: `feature/lua-role-support`

Completed 2026-03-15/16:
- [x] `LuaRoleHostBase`: worker thread, `run_data_loop_()` hook, cached `ffi.cast`, shared API closures, `drain_inbox_sync_()`, `wait_for_roles_()` ✅
- [x] `LuaProducerHost`: transport-agnostic data loop, FFI slot views, synchronous inbox drain, full API table, `snapshot_metrics_json()` ✅
- [x] `LuaConsumerHost`: read-only slots, demand-driven loop, bare-bytes messages ✅
- [x] `LuaProcessorHost`: dual-channel manual loop, dual messengers, multi-hub support ✅
- [x] All 3 `main.cpp` dispatch: `script.type == "lua"` → Lua host ✅
- [x] Code review + dedup: common closures/inbox/wait-for-roles moved to base class ✅
- [x] 12 unit tests for `LuaRoleHostBase` ✅
- [x] HubConfig lifecycle state machine (assert on out-of-order access) ✅
- [ ] HIGH: `open_inbox()`, `wait_for_role()`, `set_verify_checksum()` API gaps
- [ ] MEDIUM: expanded API (`broadcast_channel`, `list_channels`, `flexzone()`, `metrics()`)
- [ ] Phase 3: ScriptEngine interface refactor (deferred)

### Code Review (CLOSED — 2026-03-17): REVIEW_FullStack_2026-03-17
📋 `docs/code_review/REVIEW_FullStack_2026-03-17.md` — 30 non-Lua findings: 17 FIXED, 8 ACCEPTED, 4 DEFERRED, 1 Lua WIP; **1184/1184 tests**
Key fixes: DataBlockMutex try_lock_for(-1) on both platforms, SharedSpinLock generation guard, StopReason/critical_error unified in RoleHostCore, SHM ownership principle (HEP §2.2), sequential_sync parser, JsonConfig retry-with-timeout

### Code Review (CLOSED — 2026-03-15): REVIEW_Codex_2026-03-15
📋 Archived to `docs/archive/transient-2026-03-15/` — ✅ CLOSED; 4 doc fixes applied, 4 code items routed to API_TODO, 2 routed to PLATFORM/TESTING_TODO, 1 false positive, 1 pre-fixed

### Code Review (CLOSED — 2026-03-14): REVIEW_CodeAndDocs_2026-03-14
📋 Archived to `docs/archive/transient-2026-03-14/` — ✅ CLOSED; 8 fixed, 4 accepted; **1166/1166 tests**

### Priority 0 (CLOSED — 2026-03-14): HEP-0025 System Config & Python Environment
📍 **Status**: ✅ CLOSED — HEP written, config/venv support implemented; **1166/1166 tests**

Completed 2026-03-14:
- [x] `tools/pylabhub-pyenv.py` + bash/PowerShell wrappers; CMake `stage_pyenv_tools` target ✅
- [x] `python_venv` config field in ProducerConfig/ConsumerConfig/ProcessorConfig + 6 tests ✅
- [x] `PythonScriptHost`: 3-tier `resolve_python_home()`, venv activation via `site.addsitedir()` ✅
- [x] `config/pylabhub.json` system config (python_home key) ✅
- [x] HEP-CORE-0025 written (full spec) ✅
- [x] README_Deployment.md §12 (Python environment guide) + `python_venv` in all config tables ✅

### Priority 0 (CLOSED — 2026-03-12): HEP-0024 Role Directory Service
📍 **Status**: ✅ CLOSED — Phases 1–3, 5, 6, 8 implemented; Phases 4, 7 deferred; **1107/1108 tests**
📋 **Details**: `docs/HEP/HEP-CORE-0024-Role-Directory-Service.md`, `docs/todo/API_TODO.md`

Completed 2026-03-12:
- [x] `RoleDirectory` class: `open/create/from_config_file`, path accessors, `resolve_hub_dir`, `hub_broker_endpoint/pubkey`, `warn_if_keyfile_in_role_dir` (0700 vault, security warning) ✅
- [x] `role_cli.hpp` header-only: `RoleArgs`, `parse_role_args`, `resolve_init_name`, `get_role_password`, `get_new_role_password` ✅
- [x] All 3 `Config::from_directory()` migrated to use `RoleDirectory`; hub triplication eliminated ✅
- [x] All 3 `main.cpp` migrated to `role_cli::parse_role_args()` + `RoleDirectory::create()` in `do_init()` ✅
- [x] `role_main_helpers.hpp` password helpers delegated to `role_cli.hpp`; duplicates removed ✅
- [x] 22 L2 tests: `RoleDirectoryTest` (18) + `RoleCliTest` (8) + security warning (4) → 1107/1108 ✅

### Priority -1 (CLOSED — 2026-03-12): Hub-Dead ZMQ Monitor + StopReason Fix
📍 **Status**: ✅ CLOSED — ZMQ socket monitor path implemented; StopReason ordering restored; **1120/1120 tests** (2026-03-12: +3 tests for MR-09/LOW-2/MR-08)

**Completed 2026-03-12:**
- [x] Restored `handle_command(ConnectCmd&, ...)` in `messenger.cpp` (accidentally deleted by prior agent) ✅
- [x] Restored `send_heartbeats()` in `messenger.cpp` (accidentally deleted by prior agent) ✅
- [x] Replaced timer-based hub-dead (`m_last_broker_recv_epoch_ms_`/`hub_last_contact_ms()`) with ZMQ socket monitor: `zmq_socket_monitor()` + PAIR socket polling `ZMQ_EVENT_DISCONNECTED`; `process_monitor_events()` / `close_monitor()` ✅
- [x] ZMTP heartbeat sockopts set BEFORE `connect()` in ConnectCmd handler (`ZMQ_HEARTBEAT_IVL=5s`, `ZMQ_HEARTBEAT_TIMEOUT=30s`) ✅
- [x] `hub_dead_timeout_ms` removed from all 3 role configs (ProducerConfig, ConsumerConfig, ProcessorConfig) ✅
- [x] `StopReason` enum corrected: `Normal=0, PeerDead=1, HubDead=2, CriticalError=3` (prior agent collapsed HubDead) ✅
- [x] `stop_reason()` switch in all 3 API classes restored (cases 1=peer_dead, 2=hub_dead, 3=critical_error) ✅
- [x] All 3 script hosts: `on_hub_dead()` wired (producer: out_messenger_; consumer: in_messenger_; processor: BOTH in+out messengers with shared lambda) ✅
- [x] All 3 script hosts: `on_hub_dead(nullptr)` deregistered in `stop_role()` ✅
- [x] Static code review: frame-size check corrected `< 2` → `< 6`; monitor setup failure LOG_WARN → LOGGER_ERROR with errno ✅

### Priority -1 (CLOSED — 2026-03-11): Peer/Hub-Dead Monitoring + MonitoredQueue
📍 **Status**: ✅ CLOSED — all items implemented and code review fixes applied; **1062/1062 tests**

**Completed 2026-03-11:**
- [x] `MonitoredQueue<T>` header at `src/utils/hub/hub_monitored_queue.hpp` (move ctor/assign, push/drain/run_check, 5 callbacks) ✅
- [x] `hub_producer.cpp` / `hub_consumer.cpp`: replace `ctrl_send_mu`+`ctrl_send_queue` with `MonitoredQueue`; add peer-dead detection (`peer_ever_seen_`, `last_peer_recv_`); `on_peer_dead()` / `ctrl_queue_dropped()` public methods ✅
- [x] `hub_producer.hpp` / `hub_consumer.hpp`: `ctrl_queue_max_depth` / `peer_dead_timeout_ms` in Options; `on_peer_dead()` / `ctrl_queue_dropped()` declared ✅
- [x] `StopReason` enum + `stop_reason_` atomic in `PythonRoleHostBase` ✅
- [x] All 3 script hosts: wire `on_peer_dead` + `on_hub_dead` callbacks; pass `ctrl_queue_max_depth`/`peer_dead_timeout_ms` in opts ✅
- [x] All 3 API classes: `stop_reason()` / `ctrl_queue_dropped()` + pybind11 bindings + `ctrl_queue_dropped` in `snapshot_metrics_json()` ✅
- [x] Config parsing: `ctrl_queue_max_depth`/`peer_dead_timeout_ms` in ProducerConfig/ConsumerConfig/ProcessorConfig ✅
- [x] Code review fixes round 1: `fire_and_forget=true` default; peer-dead detection in embedded mode (handle_peer/ctrl_events_nowait); `on_peer_dead_cb` under `callbacks_mu` (producer + consumer, 3 sites each); ctrl_queue_max_depth=0 unbounded fix; README_EmbeddedAPI.md + HEP-0017 §3.5 ✅
- [x] 9 MonitoredQueue unit tests (+2 new: FireAndForget_True_SkipsCallbacks, MoveAssignment_ResetsMonitoringState) → **1060/1060** ✅
- [x] Code review fixes round 2: `on_peer_dead_cb` not cleared in Producer::close(); Consumer::close() missing 3 callbacks; `PendingConsumerCtrlSend`→`PendingCtrlSend` rename → **1060/1060** ✅
- [x] CR-005: embedded-mode peer-dead tests → **1062/1062** ✅ 2026-03-11

### Priority -1 (CLOSED — 2026-03-10): Full-Stack Code Review Fixes + Abstraction Cleanup
📍 **Status**: ✅ CLOSED — all actionable items resolved; **1045/1045 tests**
📋 **Reviews**:
  - `docs/code_review/REVIEW_FullStack_2026-03-10.md` — ✅ CLOSED 2026-03-10
  - `docs/code_review/REVIEW_FullStack2_2026-03-10.md` — ✅ CLOSED 2026-03-10
  - `docs/code_review/REVIEW_DesignAndCode_2026-03-09.md` — ✅ CLOSED (DC-04/06 deferred)

**Completed this sprint** (2026-03-09/10):
- [x] REVIEW_DesignAndCode_2026-03-09.md: DC-01 METRICS_REQ SHM merge fixed; DC-02/03/05 verified; DC-04/06 deferred ✅
- [x] Consumer inbox_thread_ (ROUTER): ConsumerConfig inbox fields + ConsumerScriptHost::run_inbox_thread_() ✅ 2026-03-10
- [x] 9 L3 ShmQueue test scenarios → **988/988 tests** ✅ 2026-03-10
- [x] Full-stack code review (background agent): 14 findings triaged → REVIEW_FullStack_2026-03-10.md ✅ 2026-03-10
- [x] FS-01/MR-05/MR-10: false positives confirmed by code audit ✅ 2026-03-10
- [x] FS-02: inbox config validation hardened in ProducerConfig+ConsumerConfig; 8 new tests → **996/996** ✅ 2026-03-10
- [x] Code review REVIEW_DataHubInbox_2026-03-09.md: 13 items fixed, CLOSED + archived ✅ 2026-03-09
- [x] REVIEW_FullStack2_2026-03-10.md: A1 (inbox_overflow_policy parsed), A5 (zmq_buffer_depth in ConsumerConfig), A6 (+15 config tests), A11/A18 (InboxQueue per-sender seq gap), A12 (queue_type rename), A20 (HEP doc updated) → **1011/1011** ✅ 2026-03-10
- [x] `LoopDriver`/`loop_driver` → `QueueType`/`queue_type` throughout code+docs; wire key `consumer_queue_type` ✅ 2026-03-10
- [x] ProcessorAPI queue-state accessors: `last_seq`, `in_capacity`, `in_policy`, `out_capacity`, `out_policy`, `set_verify_checksum`; atomic QueueReader*/QueueWriter* members; set/clear in start/stop/cleanup ✅ 2026-03-10
- [x] API naming fix: `ProducerAPI::overrun_count()` → `loop_overrun_count()` (pybind11 + JSON keys) ✅ 2026-03-10
- [x] ConsumerAPI: `set_verify_checksum()` added; `loop_overrun_count: 0` added to `snapshot_metrics_json()` ✅ 2026-03-10
- [x] REVIEW_Processor_2026-03-10.md: all 20 items ✅ CLOSED 2026-03-10 → **1045/1045 tests**
- [x] REVIEW_DeepStack_2026-03-10.md: deep review (9 dimensions, 16 findings); DS-DS-01/02, DS-MET-01, DS-DEAD-01, DS-H18-01/02, DS-CF-01/02/03, DS-H15-01, DS-API-01/02 fixed; DS-MR-09/HR-03/HR-05 deferred (tracked in API_TODO.md) ✅ CLOSED 2026-03-10
- [x] Abstraction leak audit + fix: all 3 script hosts (producer/consumer/processor) now use QueueWriter::write_flexzone/flexzone_size/sync_flexzone_checksum and QueueReader::read_flexzone/flexzone_size; set_checksum_options moved to post-factory abstract call; update_flexzone_checksum fixed in ProducerAPI + ProcessorAPI ✅ 2026-03-10 → **1045/1045**
- [x] REVIEW_FullStack2_2026-03-10: A2/A14 PRE-FIXED confirmed (Processor inbox receive + ProcessorAPI open_inbox/wait_for_role already implemented); A13 FIXED (script_type_explicit + LOGGER_WARN); CLOSED ✅ 2026-03-10
- [x] HEP-0015: JSON example corrected (flexzone_schema, flat inbox keys, startup ⚠ warning); ProcessorConfig struct listing corrected ✅ 2026-03-10
- [x] HEP-0018: JSON examples corrected (target_period_ms, no shm.slot_count, set_critical_error(), loop_overrun_count); ProducerAPI/ConsumerAPI sections expanded ✅ 2026-03-10
- [x] README_Deployment.md: complete rewrite (stale actor content replaced with all 4 binaries, full field refs, Python API, multi-hub pipelines, operational guide) ✅ 2026-03-10

### Priority 1: Layer 4 Producer + Consumer Tests
📍 **Status**: ✅ Complete (2026-03-02) — 26 new tests; **550/550 passing**
📋 **Details**: `docs/todo/TESTING_TODO.md` § "Layer 4: pylabhub-producer/consumer Tests"

Completed:
- [x] `tests/test_layer4_producer/` — config unit tests (8) + CLI integration tests (6)
- [x] `tests/test_layer4_consumer/` — config unit tests (6) + CLI integration tests (6)
- [x] Integration test: full pipeline round-trip via live broker — `test_pipeline_roundtrip.cpp` (hubshell + producer + processor + consumer)

### Priority 2: Schema Registry (HEP-CORE-0016) Phases 2–5
📍 **Status**: ✅ Phase 5 complete (2026-03-02); all 5 phases done
📋 **Details**: `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md`

Completed:
- [x] Phase 1: SchemaLibrary + JSON format (2026-03-01)
- [x] Phase 2: C++ Integration — `has_schema_registry_v<T>`, `validate_named_schema<>()`,
              `ProducerOptions::schema_id`, `ConsumerOptions::expected_schema_id`; 7 tests (2026-03-02)
- [x] Phase 3: Broker protocol — `REG_REQ` schema_id/blds fields, Case A/B annotation,
              `SCHEMA_REQ/ACK`, consumer expected_schema_id validation; 7 tests (2026-03-02)
- [x] Phase 4: `SchemaStore` lifecycle singleton — thread-safe wrapper around SchemaLibrary,
              manual `reload()`, explicit `query_from_broker()`; 8 tests (2026-03-02)

- [x] Phase 5: Script integration — named schema strings in config resolve via
              `SchemaLibrary`; `resolve_schema()` dispatches string/object/null;
              `schema_entry_to_spec()` converts `SchemaFieldDef` → `FieldDef`;
              7 tests (2026-03-02)

### Priority 3: Processor Binary Tests + Phase 2 (HEP-CORE-0015)
📍 **Status**: ✅ Phase 2 complete (2026-03-03) — dual-broker + hub::Processor delegation; **750/750 passing**
📋 **Details**: `docs/HEP/HEP-CORE-0015-Processor-Binary.md`

Completed:
- [x] `tests/test_layer4_processor/` — config unit tests (10+5) + CLI integration tests (6)
- [x] Phase 2: dual-broker config (6 fields + 4 resolvers); ProcessorScriptHost delegates to hub::Processor
- [x] hub::Processor enhancements: timeout handler, pre-hook, zero-fill, critical error, iteration counter
- [x] ScriptHost dedup: RoleHostCore + PythonRoleHostBase extracted (~1600 lines deduped)

### Priority 4: Metrics Plane (HEP-CORE-0019)
📍 **Status**: ✅ Complete (2026-03-05) — 19 new tests; **828/828 passing**
📋 **Details**: `docs/HEP/HEP-CORE-0019-Metrics-Plane.md`, `docs/todo/MESSAGEHUB_TODO.md`

Completed:
- [x] Phases 1–5: `report_metric()` / `snapshot_metrics_json()` API; heartbeat extension;
  `METRICS_REPORT_REQ`; `METRICS_REQ`/`METRICS_ACK`; Python bindings; `pylabhub.metrics()` AdminShell
- [x] 10 MetricsPlaneTest (protocol) + 9 MetricsApiTest (API unit, pybind11-linked)
- [x] pybind11 Default Parameter Rule codified in `IMPLEMENTATION_GUIDANCE.md`
- [x] HEP-0007 §12 updated: METRICS_REPORT_REQ, METRICS_REQ/ACK, HEARTBEAT metrics extension
- [x] HEP-0017 §2: "Four Planes" → "Five Planes" (Metrics plane added)

### Priority 5 (backlog): Platform + Admin Shell Test Gaps
📍 **Status**: ✅ HP-C1/HP-C2/BN-H1 complete; platform backlog only
📋 **Details**: `docs/todo/PLATFORM_TODO.md`, `docs/todo/TESTING_TODO.md`

Completed:
- [x] HP-C1: `pylabhub.reset()` deadlock regression — `test_admin_shell.cpp` (2 tests)
- [x] HP-C2: stdout/stderr leak on exec() exception — `test_admin_shell.cpp` (2 tests)
- [x] BN-H1: Consumer ctypes round-trip — `test_pipeline_roundtrip.cpp` (field-level verification)

Remaining backlog:
- Clang-tidy full pass; Windows MSVC CI gaps

### Priority 5: HEP Document Review + Code Review + Source Polish
📍 **Status**: ✅ Complete (2026-03-03) — all 17 HEPs updated; code review passed; umbrella headers polished
📋 **Details**: Plan file `calm-herding-koala.md`

Completed:
- [x] Session 0: Housekeeping — archived HEP-0005, scrubbed 185 actor references across 12 HEPs
- [x] Batch 1-6: All 17 HEP documents updated with mermaid diagrams, source file references, status fields
- [x] Phase 2: 5-pass code review (L0-1, L2, L3, L4, cross-cutting) — 6 issues found and fixed
- [x] Phase 3: Source polish — doxygen fixes, umbrella header reorganization, orphan header inclusion
- [x] Example build fix — `0xBAD5ECRET` literal error in producer/consumer examples

### Backlog: C++ Templates + README Update
📍 **Status**: 🟡 Templates done (2026-03-03); README deferred
📋 **Details**: `docs/todo/API_TODO.md` § "C++ Pipeline Demo" and § "README Documentation Update"

Completed:
- [x] `examples/cpp_processor_template.cpp` — processor pipeline template (Producer → ShmQueue → Processor → Consumer)
- [x] `examples/CMakeLists.txt` + `PYLABHUB_BUILD_EXAMPLES` opt-in CMake flag
- [x] ZMQ wire format documentation (HEP-CORE-0002 §7.1)

Completed:
- [x] Application-oriented README update: API layers, four binaries, CLI flags, config model, C++ vs Python paths,
  getting started guide, five communication planes — root `README.md` + `share/py-demo-single-processor-shm/README.md` (2026-03-05)

---

## Active Work Areas

| Area | Status | Detail Document | Notes |
|------|--------|----------------|-------|
| Security / Identity / Provenance | ✅ Complete | `docs/archive/transient-2026-03-02/SECURITY_TODO.md` | All 6 phases complete (2026-02-28). TODO archived. |
| Actor (pylabhub-actor) | ❌ Eliminated | — | **Eliminated (2026-03-01).** `src/actor/` + `tests/test_layer4_actor/` deleted. HEP-0010/0012/0014 archived. Replaced by three standalone binaries. |
| Producer Binary (`pylabhub-producer`) | ✅ Complete | `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` | **Phase 1 done (2026-03-01); Layer 4 tests done (2026-03-02):** 8 config + 6 CLI tests. **Integration test done:** `test_pipeline_roundtrip.cpp`. |
| Consumer Binary (`pylabhub-consumer`) | ✅ Complete | `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` | **Phase 1 done (2026-03-01); Layer 4 tests done (2026-03-02):** 6 config + 6 CLI tests. **Integration test done:** `test_pipeline_roundtrip.cpp`. |
| HubShell / HubConfig | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | All 6 phases done (2026-02-20): HubConfig, Python env, broker consolidation, PythonInterpreter, AdminShell, hubshell.cpp rewrite |
| RAII Layer | ✅ Complete | `docs/archive/transient-2026-03-02/RAII_LAYER_TODO.md` | Phase 3 complete; all code review items resolved. TODO archived; minor backlog absorbed into TESTING_TODO. |
| API / Primitives | 🟢 Ready | `docs/todo/API_TODO.md` | WriteAttach mode + `attach_datablock_as_writer_impl` added; timeout constants; ScopedDiagnosticHandle; **header layering refactor Phase A complete (2026-02-26)**; **P2 src/ split done (2026-02-27)**: `data_block.cpp` 3969L→2894L via `data_block_internal.hpp` + 3 new split files; **HEP-CORE-0002 restructured (2026-02-27)**: §6 RAII Abstraction Layer added, §7 Control Plane Protocol stub (→HEP-CORE-0007), stale §5.3/§5.4/§5.5 removed, §6-§15→§7-§16; **P4 messenger.cpp split done (2026-02-27)**: `messenger_internal.hpp` + `messenger_protocol.cpp`; `messenger.cpp` 1707L→811L |
| Platform / Windows | 🟢 Mostly done | `docs/todo/PLATFORM_TODO.md` | Major pass done; 2 Windows CI items in backlog |
| Testing | ✅ Complete | `docs/todo/TESTING_TODO.md` | **884/884 passing** (2026-03-07). SHM-C2 audit: +2 draining tests, fixed stalling DrainingTimeoutRestoresCommitted. ProcessorHandlerRemoval flake fixed (sleep→poll_until barrier). Full sleep audit: 8 races eliminated in hub_processor_workers.cpp. `test_sync_utils.h` shared facility created. |
| Memory Layout | ✅ Complete | `docs/archive/transient-2026-03-02/MEMORY_LAYOUT_TODO.md` | Single structure; alignment fixed; sub-4K slots. TODO archived; minor test backlog absorbed into TESTING_TODO. |
| Schema Validation | ✅ Complete | — | BLDS schema done; dual-schema producer/consumer validation working |
| Named Schema Registry | ✅ Complete | `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md` | All 5 phases done (2026-03-02). Script host helpers deduplicated into shared headers. |
| Processor Binary | ✅ Phase 3 complete | `docs/HEP/HEP-CORE-0015-Processor-Binary.md` | **Phase 1+2 done (2026-03-03). Phase 3 config+ScriptHost 2026-03-10:** timing policy, inbox (ROUTER), direct ZMQ PULL input, verify_checksum, zmq_packing/buffer from config. REVIEW_Processor_2026-03-10.md: all 20 items ✅ CLOSED 2026-03-10. **1078/1078 tests.** |
| Startup Coordination | ✅ Phase 1 complete | `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` | **HEP-0023 written 2026-03-10.** `startup.wait_for_roles` config field implemented in all 3 role configs + script hosts (poll before on_init, per-role timeout). 16 config tests. **1078/1078 tests 2026-03-11.** Deferred: DISC_ACK, ROLE_REGISTERED_NOTIFY. |
| Role Directory Service | 🟢 Implemented (Phases 1-4,6) | `docs/HEP/HEP-CORE-0024-Role-Directory-Service.md` | **HEP-0024 Phases 1-4+6 DONE 2026-03-12.** `RoleDirectory` + `role_cli.hpp` public API; all 3 `from_directory()` migrated; all 3 `do_init()`/`parse_args()` migrated; 26 new L2 tests. Deferred: Phase 5 (script-host `script_entry()` migration), Phase 7 (docs), Phase 8 (L4 tests). **1104 tests.** |
| Pipeline Architecture | ✅ Design | `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` | Design complete (2026-03-01, updated 2026-03-05). Five planes (Metrics added), four standalone binaries, topology patterns. |
| Metrics Plane | ✅ Complete | `docs/HEP/HEP-CORE-0019-Metrics-Plane.md` | **Implemented (2026-03-05).** All 5 phases. 19 tests. Heartbeat metrics extension, METRICS_REPORT_REQ, METRICS_REQ/ACK, Python bindings, AdminShell. |
| Interactive Signal Handler | ✅ Complete | `docs/HEP/HEP-CORE-0020-Interactive-Signal-Handler.md` | **Implemented (2026-03-02).** All 4 binaries integrated. Old signal handlers removed. 705/705 pass. |
| Recovery API | ✅ Complete | — | P8 recovery API done; DRAINING recovery restores COMMITTED |
| Messenger / Broker | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | Two-tier shutdown (CHANNEL_CLOSING_NOTIFY + FORCE_SHUTDOWN); Cat 1/Cat 2 health; event handlers; CHANNEL_NOTIFY_REQ relay; HEP-0007 §12 |
| ZMQ Virtual Channel Node | ✅ Complete | `docs/HEP/HEP-CORE-0021-ZMQ-Virtual-Channel-Node.md` | **HEP-0021 implemented (2026-03-06).** `data_transport`+`zmq_node_endpoint` in REG_REQ/DISC_ACK, ChannelHandle, hub::Producer/Consumer, ProcessorScriptHost. 12 L3 protocol tests (848/848 pass). Deferred: ZMQ data-plane runtime checksum+type-tag (HEP-0023). |
| Hub Federation Broadcast | ✅ Complete | `docs/HEP/HEP-CORE-0022-Hub-Federation-Broadcast.md` | **HEP-0022 fully implemented (2026-03-06).** HUB_PEER_HELLO/ACK/BYE, HUB_RELAY_MSG, dedup window, channel_to_peer_identities_ index, HubScript federation callbacks (on_hub_connected/disconnected/message, api.notify_hub). |

**Active code reviews:** None — all closed and archived to `docs/archive/transient-2026-03-12/` (2026-03-12).

Previously closed (archived):
- `REVIEW_FullSource_2026-03-06.md` — ✅ CLOSED 2026-03-12 (archived)
- `review_high_level.md` — ✅ CLOSED 2026-03-12 (all HIGH/MEDIUM/LOW resolved; archived)
- `REVIEW_DesignAndCode_2026-03-09.md` — ✅ CLOSED 2026-03-10 (archived 2026-03-12)
- `REVIEW_DataHubInbox_2026-03-09.md` — ✅ CLOSED 2026-03-09, archived `transient-2026-03-09/`
- `REVIEW_FullStack_2026-03-10.md`, `REVIEW_FullStack2_2026-03-10.md`, `REVIEW_Processor_2026-03-10.md`, `REVIEW_DeepStack_2026-03-10.md` — all ✅ CLOSED 2026-03-10 (archived 2026-03-12)
- `gemini_review.md` — triaged 2026-03-12 (5 stale/FP, 2 fixed, 1 accepted, 1 open→API_TODO; archived)

**Security fixes applied (2026-03-06):** SHM-C1 (heartbeat CAS uid/name write-before-CAS → data corruption), IPC-C3 (thread lambda `this`-capture → use-after-move), SVC-C1 (vault_crypto key not zeroed), SVC-C2/C3 (hub_vault sec+admin token buffers not zeroed), HDR-C1 (namespace outside `#ifdef __cplusplus`). See `REVIEW_codebase_2026-03-06.md` for full triage.

**Security fixes applied (2026-03-06, session 2):** IPC-H2 (BrokerService `server_secret_z85` + `cfg.server_secret_key` now zeroed in `~BrokerServiceImpl()` via `sodium_memzero`).

**Bugs fixed (2026-03-06, session 3):**
- #22 (zmq_context.cpp): Use-after-free — swapped `delete ctx` / `g_context.store(nullptr)` order. Now stores nullptr FIRST so no thread can observe a valid pointer to freed memory.
- #2 (python_interpreter.cpp): TOCTOU in `exec()` — added second `ready_` check after acquiring exec_mu AND GIL. Since `release_namespace()` needs the GIL, the check after GIL acquisition is authoritative and race-free.

**Closed non-issues (2026-03-06, session 2):** SHM-C2 (write_index burned on timeout) — analyzed and documented in `data_block.cpp`. For `Latest_only`: fully immune (reads commit_index). For `Sequential`: stale-data read is possible only with `acquire_timeout_ms==0` (non-blocking) + slow reader — NOT a supported production configuration. Documented per-policy impact in source.

**Remaining deferred items:** IPC-C2/H5 (zmq_context check-then-store — ✅ now fixed as #22); IPC-H3 (callback data race — documented design contract). Full backlog of 38 open review items in REVIEW_FullSource_2026-03-06.md.

**Legend:**  
🔴 Blocked | 🟡 In Progress | 🟢 Ready | ✅ Complete | 🔵 Deferred

---

## Subtopic TODO Documents

All detailed task tracking, completions, and phase-specific work is maintained in subtopic TODO documents.
See `docs/todo/README.md` for full list and archiving history.

### Active (have open items)
- **`docs/todo/API_TODO.md`** — Producer/consumer/processor binary work (Steps 4–5), header layering, API backlog
- **`docs/todo/TESTING_TODO.md`** — Layer 4 producer/consumer tests (pending), HP-C1/HP-C2/BN-H1, platform
- **`docs/todo/PLATFORM_TODO.md`** — Clang-tidy pass, Windows MSVC CI gaps (backlog)
- **`docs/todo/MESSAGEHUB_TODO.md`** — Broker feature backlog, schema registry (deferred)

### Archived (complete — no active items)
- `SECURITY_TODO.md` → `docs/archive/transient-2026-03-02/` (all 6 phases done 2026-02-28)
- `RAII_LAYER_TODO.md` → `docs/archive/transient-2026-03-02/` (RAII layer complete)
- `MEMORY_LAYOUT_TODO.md` → `docs/archive/transient-2026-03-02/` (memory layout complete)

### Pending / In-Progress

| Item | Status | Notes |
|------|--------|-------|
| Dual-hub bridge demo (`share/py-demo-dual-processor-bridge/`) | ✅ Complete | ProcessorConfig transport fields + ProcessorScriptHost ZMQ path + L3 ShmInZmqOut/ZmqInShmOut tests + 6-process demo configs/scripts/run_demo.sh — 2026-03-11. |
| HEP-0022 Phase 5+6: HubScript federation callbacks | ✅ Complete | `on_hub_connected`, `on_hub_disconnected`, `on_hub_message`, `api.notify_hub()` fully wired in `hub_script.cpp` + `hub_script_api.cpp` + `hubshell.cpp`. Confirmed 2026-03-06. |
| Security: IPC-H2 BrokerService key zeroing | ✅ Fixed (2026-03-06) | `~BrokerServiceImpl()` zeros `server_secret_z85` + `cfg.server_secret_key` via `sodium_memzero`. |

See `docs/HEP/HEP-CORE-0022-Hub-Federation-Broadcast.md`.

---

## How to Use This System

1. **Check this master TODO** for high-level status and current sprint focus
2. **Dive into subtopic TODOs** for detailed tasks and tracking
3. **Update subtopic TODOs** as you work (mark completions, add new tasks)
4. **Update this master TODO** when major milestones are reached or priorities shift
5. **Archive completed work** per `docs/DOC_STRUCTURE.md` guidelines

**Important**: Recent completions and detailed phase tracking belong in subtopic TODO documents, not here. Keep this document high-level and strategic.

### Maintenance Schedule

- **Weekly** (Monday): Update current focus, move completed tasks to "Recent Completions" in subtopic TODOs
- **Sprint End** (every 2 weeks): Clean up recent completions, groom backlog, update master status
- **Monthly** (1st Monday): Archive old completions (> 2 months), review all TODOs for duplicates
- **Quarterly** (every 3 months): Structural review, create/merge/archive subtopic TODOs

See **`docs/todo/README.md`** for detailed maintenance procedures and best practices.

---

## Quick Links

- Documentation Structure: `docs/DOC_STRUCTURE.md`
- TODO Maintenance Guide: `docs/todo/README.md`
- Implementation Guidance: `docs/IMPLEMENTATION_GUIDANCE.md`
- Code Review Process: `docs/CODE_REVIEW_GUIDANCE.md`
- Test Strategy: `docs/README/README_testing.md`
- Build Commands: `CLAUDE.md`
