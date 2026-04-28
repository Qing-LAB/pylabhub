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
- **Schema Registry**: `docs/HEP/HEP-CORE-0034-Schema-Registry.md` (ratified 2026-04-26; supersedes HEP-CORE-0016)
- **Policy Reference**: `docs/HEP/HEP-CORE-0009-Policy-Reference.md` (active cross-reference)

---

## Current Sprint Focus

### Snapshot — 2026-04-23

**Full suite: 1500/1500.**  Branch `feature/lua-role-support`.  Last commits:
`8e1eadc` (HEP-0033 G2.1 HubState skeleton), `e9fc8f6` (HEP-0033 G2 "broker
as single mutator" doc ratified), `139b4ca` (HEP-0033 G1 `RoleHostBase` →
`EngineHost<ApiT>` template), `399fbfc` (HEP-0032 Phase C ABI fingerprint).

**Actively open (in priority order):**

1. **HEP-CORE-0033 Hub Character implementation** — in progress.
   - G1 (host template): ✅ `RoleHostBase` = `EngineHost<RoleAPIBase>` (`139b4ca`).
   - G2 design: ✅ ratified; `HubState` sole-mutator-through-broker model
     (`e9fc8f6`).
   - G2.1 (HubState skeleton + entry types): ✅ compile-only landed (`8e1eadc`);
     17 L2 unit tests; primitive `_set_*` mutators; not yet wired.
   - **Next**: G2.2 reframed around hub *capabilities* rather than broker
     map-by-map absorption. Five sub-commits: G2.2.0 plumb HubState +
     add `_on_*` capability-operation layer over primitives; G2.2.1
     registration lifecycle (deletes `ChannelRegistry`); G2.2.2 liveness;
     G2.2.3 membership routing (deletes `BandRegistry` + `inbound_peers_`);
     G2.2.4 observability (metrics data-model deferred).  See
     `HUB_CHARACTER_PREREQUISITES.md` §G2 "Capability-operation mutator
     layer" and "Phasing proposal" for the rationale.
   - Then: G2.3 (`HubAPI` read accessors) → G2.4 (`HubAPI` mutation wrappers
     + remove ad-hoc request queues) → G2.5 (`AdminService` shell).
   - Remaining prereqs beyond G2: G7 (HubConfig lifecycle-module vs main-owned),
     G5/G6/G8 spec gaps, G9-G13 ripples. See
     `docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md`.
2. **HEP-CORE-0032 ABI check facility** — ✅ all three phases landed
   (`c91ae84` Phase A, `34255be` Phase B, `399fbfc` Phase C).
3. **Subtopic backlogs** (see §Subtopic TODO Documents below):
   - API/ABI: Phases 2-7 of the `PYLABHUB_UTILS_TEST_EXPORT` rollout,
     `std::function`/`std::optional` ABI fixes, C API helpers.
   - Platform: CI macOS/Windows jobs + clang-tidy pass, MSVC gaps.
   - Testing: Lua V2-fixture cleanup tail, worker-helper unification,
     Script-API live-vs-frozen contract work.
   - MessageHub: HEP-0033 ancillary items (system-level L4 tests, 6
     hub-facing L3 Pattern-3 conversions folded into HEP-0033 scope).

**Closed this session (2026-04-21 / 22):**
- HEP-CORE-0024 Role Binary Unification (all 22 phases).
- HEP-CORE-0033 Hub Character design ratified.
- L3 role-api tests Pattern-3 converted + deepened.
- L2 depth review closed (tracker `21.3.5`): Pattern-3 compliance,
  vacuous-test sweep, stderr-capture fixes, assertion-quality tighten.
- 6 tech drafts archived to `docs/archive/transient-2026-04-21/`.

**Ratified 2026-04-26:**
- **HEP-CORE-0034 Schema Registry** — owner-authoritative model; supersedes
  HEP-CORE-0016. Namespace-by-owner records, owner-bound eviction (no
  refcount), cross-citation rejected even on hash match,
  fingerprint includes packing. HEP-0016 marked Superseded. HEP-0033 §7/§8/§9.4/§14
  cross-referenced. HEP-0024 §3.1/§3.5 updated for role-side `schemas/` cache.

**HEP-0034 Phase 1 landed 2026-04-27 (commit `d60ddf2`):**
- Fingerprint correction (`compute_schema_hash` and `SchemaInfo::compute_hash`
  now include packing); `parse_schema_json` mandates explicit packing;
  `PYLABHUB_SCHEMA_BEGIN_PACKED` macro added for opt-in packed C++ structs;
  share/ JSON configs + role `--init` templates emit explicit packing.
  +4 tests covering aligned-vs-packed-distinct fingerprints. 1602/1602.
- Five implementation phases remain (see §Priority 2).

### Priority 0 (Next Sprint — design ratified 2026-04-21): HEP-CORE-0033 Hub Character

📍 **Status**: Design ratified; implementation not started
📋 **Spec**: `docs/HEP/HEP-CORE-0033-Hub-Character.md` (normative)
📋 **Prerequisites**: `docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md`
📋 **Detail**: `docs/todo/MESSAGEHUB_TODO.md`

Replaces the deleted `pylabhub-hubshell` (legacy `src/hub_python/` stack
removed in the post-G2 cleanup pass) with a modern `plh_hub` binary
paralleling `plh_role`: composite `HubConfig`, `hub_cli`, `HubDirectory`,
`HubHost` + `HubState`, `AdminService` structured RPC, `ScriptEngine`-based
scripting + `HubAPI`, query-driven metrics (supersedes HEP-0019 §3-4). 10
implementation phases; see HEP §14.

### Priority 0 (DONE — 2026-04-21): HEP-CORE-0024 — Role Binary Unification COMPLETE

📍 **Status**: All 22 phases ✅; `test_layer4_plh_role/` 71 tests passing; full suite **1456/1456**
📋 **Branch**: `feature/lua-role-support`

- [x] Phases 15-17: `RoleHostBase` abstract class; `RoleRuntimeInfo` + `register_runtime()`; per-role bootstrap
- [x] Phase 18: `role_cli` extended with `--role` / `--log-maxsize` / `--log-backups`; mode exclusion
- [x] Phase 19: `plh_role` unified binary + CMake target
- [x] Phase 20: `pylabhub-producer/consumer/processor` binaries + per-role `CMakeLists.txt` + `*_main.cpp` deleted
- [x] Phase 21: L4 tests unified at `tests/test_layer4_plh_role/` (parametrized by role tag; 71 tests covering --init / --validate / --keygen / CLI error paths + round-trip init↔validate)
- [x] Phase 22: README / Deployment docs updated for `plh_role`

System-level L4 tests (broker round-trip, pipeline, channel broadcast, hub-dead,
inbox) are **out of scope** for HEP-0024 — they are system-integration tests
that require a hub binary, which is HEP-CORE-0033 work. Tracked in
`docs/todo/MESSAGEHUB_TODO.md`.

### Priority 0 (DONE — 2026-04-17): HEP-0024 Phases 13-14 — Logging
📍 **Status**: Complete; **1290/1290 tests**
📋 **Branch**: `feature/lua-role-support`

- [x] `RotatingFileSink::Mode::{Numeric,Timestamped}` two-mode extension
- [x] `RotatingLogConfig::timestamped_names` flag (default false → Numeric)
- [x] Timestamped filename: `<base>-YYYY-MM-DD-HH-MM-SS.uuuuuu.log` (lex-sort = chron-sort)
- [x] `format_tools::formatted_time(tp, use_dash_spacer=true)` dash-spacer variant
- [x] `LoggingConfig` category in `RoleConfig` + strict key whitelist
- [x] `max_backup_files` semantics: `>=1` explicit, `-1` → `kKeepAllBackups` sentinel, `0` invalid
- [x] `RotatingLogConfig` default aligned to 5 (matches `LoggingConfig`)
- [x] Producer `out_shm_secret=0` boilerplate removed from init template
- [x] `init_directory` stderr messages prefixed `init_directory: error:`
- [x] 10 new L2 LoggingConfig tests; 2 L1 `formatted_time` dash-spacer tests
- [x] HEP-0024 §12 (CLI↔Config boundary), §13 phase table updated

Next (HEP-0024 Phases 15-22): `RoleHostBase` abstract class, `RoleRuntimeInfo` +
`register_runtime()`, role CLI `--role`/`--log-*` flags, `plh_role` unified
binary, per-role binary deletion, L4 test migration, docs.

### Priority 0 (DONE — 2026-04-15/16): L3.γ/ζ Role Unification + ZMQ + Flexzone + Docs
📍 **Status**: Complete; **1278/1278 tests**
📋 **Branch**: `feature/lua-role-support`

L3.γ — Role unification:
- [x] A5i: role host worker_thread_ under ThreadManager ✅
- [x] A6.1–A6.3: delete hub::Producer/Consumer; abstract-only queue ownership ✅
- [x] ZMQ: cppzmq migration + shared ZMQContext module (all consumers) ✅
- [x] BrokerService::run() migrated to shared ZMQContext ✅
- [x] ThreadManager: drain(), no-op lifecycle thunk, instance_id, HEP-0031 ✅
- [x] Deprecated shims removed; shutdown order fixed ✅

L3.ζ — Flexzone:
- [x] InvokeTx/InvokeRx stripped to slot-only; .fz removed from PyTxChannel/PyRxChannel ✅
- [x] Python api.flexzone(side) init-time cache (all 3 API classes) ✅
- [x] Lua api.flexzone(side) with side arg + correct Rx ref selection ✅
- [x] Native engine: cached context, bridge-populated plh_tx_t.fz ✅
- [x] L3 tests T2/T3 (role-level SHM flexzone round-trip) ✅
- [x] has_out_fz/has_in_fz → has_tx_fz/has_rx_fz rename ✅
- [x] Demo scripts updated to 3-arg signature ✅

Documentation cleanup (2026-04-16):
- [x] 12 tech drafts archived with verified merges into HEPs ✅
- [x] HEP-0002 §17.2 rewritten (queue abstraction + flexzone access) ✅
- [x] HEP-0008 §2.2 + §11 (timeout formula + config single-truth) ✅
- [x] HEP-0011 (unified data loop: CycleOps, 14-step lifecycle) ✅
- [x] HEP-0016 §11.0 (schema layer Mermaid diagram) ✅
- [x] HEP-0030 appendix (band design rationale) ✅
- [x] HEP-0031 created (ThreadManager — Layer 2 utility) ✅

Deferred:
- [ ] L3 test T4: processor dual-flexzone distinctness
- [ ] Extract `create_zmq_socket()` factory (7 files, 1-line pattern)
- [ ] SequenceTracker utility (20 LOC duplication)

### Priority 0 (DONE — 2026-04-04/05): RoleAPIBase Refactor + Lifecycle + API Consistency
📍 **Status**: All phases complete; **1323/1323 tests**
📋 **Branch**: `feature/lua-role-support`

- [x] RoleAPIBase: pure C++ unified role API in pylabhub-utils ✅
- [x] All 3 API classes delegate to RoleAPIBase via composition ✅
- [x] RoleContext eliminated — engine uses api_ pointer directly ✅
- [x] All 3 engines (Python/Lua/Native) migrated: ctx_ → api_-> ✅
- [x] Lifecycle integration: engine_lifecycle_startup() replaces manual init ✅
- [x] ChannelSide enum (Tx/Rx): spinlock + schema size with explicit side ✅
- [x] Schema size API: slot_logical_size, flexzone_logical_size (all engines) ✅
- [x] Inbox packing: schema.packing as sole source, shared setup_inbox_facility() ✅
- [x] align_to_physical_page() utility + assert in lifecycle callback ✅
- [x] Counter rename: C++ internals + JSON keys + Lua/Python/Native consistent ✅
- [x] Native engine API v2: spinlock, schema sizes, messaging, C++ RAII ✅
- [x] HEP-0011 complete rewrite ✅
- [x] Multi-process spinlock test through RoleAPIBase ✅
- [x] Schema size tests (4 complex schemas, aligned/packed) ✅
- [x] Native engine v2 tests (counters, schema size, spinlock count) ✅
- ProducerAPI/ConsumerAPI/ProcessorAPI kept as Python translation layer (by design)

### Priority 0 (DONE — 2026-04-02/03): DataBlock Ownership + Schema Validation + Checksum + SE-04
📍 **Status**: All ownership steps done (except RAII rewrite); **1279/1279 tests**
📋 **Branch**: `feature/lua-role-support`

Ownership refactor (all steps except template RAII):
- [x] ShmQueue owns DataBlock internally (create_writer/create_reader) ✅
- [x] Producer/Consumer: spinlock/identity delegating methods ✅
- [x] All `->shm()` external callers migrated + `shm()` public accessor removed ✅
- [x] `item_size`/`flexzone_size` removed from Options + `schema_slot_size_` from role hosts ✅
- [x] Schema size cross-validation in all 3 engines (vs compute_field_layout) ✅
- [x] Native engine: `native_sizeof_<T>` required export ✅
- [x] Checksum policy fix: Manual no-stamp + always-verify (SHM+ZMQ+Inbox unified) ✅
- [x] 8 new checksum policy tests (hub API + ZmqQueue + InboxQueue) ✅
- [x] `to_field_descs()` utility, `shm_blocks` → `shm_info` rename ✅
- [x] SE-04: Lua API parity complete (shm_info added, only as_numpy is Python-specific) ✅
- [ ] Template RAII rewrite on QueueWriter/QueueReader (Group D — separate sprint)

### Priority 0 (DONE — 2026-03-29/30): Metrics, Timing, Checksum & Config Unification
📍 **Status**: All items complete; **1181/1181 tests**
📋 **Branch**: `feature/lua-role-support`

Metrics systematization:
- [x] ContextMetrics: private fields, all-atomic, accessor API, renamed to `context_metrics.hpp` ✅
- [x] X-macros: queue (12 fields), loop (4), inbox (4) with JSON/pydict/Lua adapters ✅
- [x] ZmqQueue adopts ContextMetrics (replaces individual atomic counters) ✅
- [x] Lua `api.metrics()` — full hierarchical table ✅

Timing unification:
- [x] LoopTimingParams: single config truth, strict validation ✅
- [x] `loop_timing` required in config (no implicit default) ✅
- [x] DataBlock timing removal: `set_loop_policy` deleted, `LoopPolicy` enum deleted ✅
- [x] `configured_period_us` moved from queue to loop level ✅
- [x] `queue_period_us` removed from Options ✅

Checksum policy unification:
- [x] Per-role config (`"checksum": enforced/manual/none`) ✅
- [x] Unified queue API: `set_checksum_policy`, `update/verify_checksum`, flexzone variants ✅
- [x] InboxQueue/InboxClient checksum policy support ✅

Inbox protocol completion:
- [x] Consumer registration: CONSUMER_REG_REQ carries inbox fields ✅
- [x] ROLE_INFO_REQ: searches both producer and consumer entries ✅
- [x] RoleContext: typed pointers (void* removed), inbox_queue added, checksum_policy added ✅

Config validation:
- [x] Config key whitelist: validates all JSON keys at parse time ✅

Documentation:
- [x] HEP-0008 full rewrite ✅
- [x] HEP-0027 Inbox Messaging spec ✅

Bug fixes:
- [x] flexzone checksum returns true when no flexzone ✅
- [x] `init_metrics()` vs `reset_metrics()` separation (sequence state preserved) ✅

### Priority 0 (DONE — 2026-03-23): ScriptEngine + Config Module Redesign
📍 **Status**: ScriptEngine refactor + config module redesign COMPLETE; 1115/1115 tests
📋 **Branch**: `feature/lua-role-support`
📋 **Active review**: `docs/code_review/REVIEW_ConfigAndEngine_2026-03-21.md`

ScriptEngine refactor (DONE 2026-03-20):
- [x] `ScriptEngine` abstract interface + `LuaEngine` + `PythonEngine` ✅
- [x] Unified role hosts: `ProducerRoleHost`, `ConsumerRoleHost`, `ProcessorRoleHost` ✅
- [x] `RoleHostCore` encapsulated: all metrics/shutdown private with proper methods ✅
- [x] API classes take `RoleHostCore&` in constructor ✅
- [x] 38 Lua + 38 Python engine L2 tests ✅
- [x] Legacy host code removed (~5000 lines) ✅
- [x] Code review SE-01/02/05/06/10/11/12/13/14/15 fixed ✅ 2026-03-21

Config module redesign (DONE 2026-03-23):
- [x] Phase 1: Categorical config headers + shared parsers ✅ cb7e4b5
- [x] Phase 2: RoleConfig unified class with JsonConfig backend ✅ 36f1902
- [x] Phase 3: Migrate role hosts + mains to RoleConfig ✅ c0100d1, a445dca
- [x] Phase 4: Remove monolithic config structs ✅ 9dbfa59
- [x] Dead field cleanup: ValidationConfig merged, period_ms → configured_period_us ✅ cc4c581, f2a805e
- [x] HEP/README doc sync ✅ fcaaf33

Naming cleanup (DONE 2026-03-23):
- [x] ActorVault → RoleVault + vault integrated into RoleConfig ✅ 38172de
- [x] KnownActor → KnownRole, all "actor" terminology → "role" ✅ 154c1c3
- [x] `generate_uid(prefix, name)` unified core ✅ 154c1c3

Deferred (after code stabilizes):
- [ ] SE-03: HEP-0011 rewrite for composition model
- [ ] SE-04: Lua API parity (design decision pending)
- [ ] SE-07: --validate implementation
- [ ] SE-08: HEP-0018/0015 class name refs update
- [ ] Engine thread model: 6 phases — invoke/eval, cross-thread dispatch, shared state, NativeEngine (see `docs/tech_draft/engine_thread_model.md`)
- [ ] ScriptEngine cleanup: RoleHostCore encapsulation (CR-03), RoleContext const char*→string. (Hubshell migration superseded — legacy `src/hub_python/` deleted in post-G2 cleanup; replacement is HEP-CORE-0033 §15 Phase 7 ScriptEngine integration on the new `plh_hub` binary.)

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
- [x] `tools/plh_pyenv.py` + bash/PowerShell wrappers; CMake `stage_pyenv_tools` target ✅ (renamed from `pylabhub-pyenv` on 2026-04-17)
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

### Priority 2: Schema Registry — superseded model (HEP-CORE-0016 → HEP-CORE-0034)
📍 **Status**: HEP-CORE-0016 5 phases shipped 2026-03-02 → **superseded by HEP-CORE-0034 (ratified 2026-04-26)**.
📋 **New spec**: `docs/HEP/HEP-CORE-0034-Schema-Registry.md`
📋 **Historical spec**: `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md` (kept for reference)

HEP-0016 historical phases (all shipped, retained context):
- [x] Phase 1: SchemaLibrary + JSON format (2026-03-01)
- [x] Phase 2: C++ Integration — `has_schema_registry_v<T>`, `validate_named_schema<>()`,
              `ProducerOptions::schema_id`, `ConsumerOptions::expected_schema_id`; 7 tests (2026-03-02)
- [x] Phase 3: Broker protocol — `REG_REQ` schema_id/blds fields, Case A/B annotation,
              `SCHEMA_REQ/ACK`, consumer expected_schema_id validation; 7 tests (2026-03-02)
- [x] Phase 4: `SchemaStore` lifecycle singleton (**to be removed in HEP-0034 Phase 4** — file watcher + broker query fallback no longer fit hub-mutator model)
- [x] Phase 5: Script integration — named schema strings in config resolve via `SchemaLibrary`

HEP-0034 implementation phases:
- [x] Phase 1 (2026-04-27, commit `d60ddf2`): Fingerprint correction — `compute_schema_hash` and `SchemaInfo::compute_hash` include packing; `parse_schema_json` rejects missing packing; `PYLABHUB_SCHEMA_BEGIN_PACKED` macro added; share/ JSONs + role init templates updated to declare packing explicitly. +4 tests (`ParseError_MissingPacking`, `FingerprintIncludesPacking_*`). Follow-up `dc9b6ef`: `compute_inbox_schema_tag` now includes packing; `PackingMacro_DistinctHashesFromAligned` test pins macro hash distinction; HEP §14.2 doc aligned to two-macro design. 1603/1603.
- [x] Phase 2 (2026-04-27, commit `e23e33e`): HubState schema records — `SchemaRecord` (`schema_record.hpp`) + `HubState.schemas` map keyed `(owner_uid, schema_id)` + `ChannelEntry.schema_owner`; capability ops `_on_schema_registered` / `_on_schemas_evicted_for_owner` / `_validate_schema_citation`; cascade eviction wired into `_set_role_disconnected`; three new counters (`schema_{registered,evicted,citation_rejected}_total`). +15 tests in `test_hub_state.cpp` covering namespace-by-owner conflict policy, idempotent re-registration, hash/packing-mismatch rejection, cross-citation rejection on hash-equality, hub-global immunity. 1618/1618.
- [x] Phase 3 (2026-04-27, commits `92775ac` + `87390c8` + follow-up `2619b17`): Wire protocol + broker dispatch — `REG_REQ` with `schema_packing` creates path-B records; `CONSUMER_REG_REQ` with full named/anonymous citation rule per HEP-0034 §10.3 (named: id+hash with optional structure for defense-in-depth; anonymous: full structure required, hash optional with self-consistency check); `SCHEMA_REQ` accepts `(owner, schema_id)` keying alongside legacy `channel_name`; inbox metadata creates `(role_uid, "inbox")` records cascade-evicted via `_on_channel_closed`. Broker recomputes every claimed fingerprint (Stage-2 verification) — `compute_canonical_hash_from_wire` helper pinned the wire canonical form. Channel-mismatch gate moved before schema-record creation (no orphan records on failed REG_REQ). 13 NACK reasons documented in HEP-0034 §10.4. +20 Pattern-3 L3 tests across the three commits. Backward compat preserved. 1638/1638.
- [x] Phase 4a (2026-04-28, commit `4b83636`): SchemaStore deletion + bridge helper + flexzone-in-wire bug fix. Deleted `schema_registry.hpp` / `schema_registry.cpp` / `test_datahub_schema_registry.cpp`. Added `to_hub_schema_record(SchemaEntry)` helper in `schema_utils.hpp` (uses wire-form canonical hash, distinct from HEP-0002 SchemaInfo SHM-header hash). Fixed broker Stage-2 verification to include flexzone fields (was slot-only, would have NACKed any producer with a flexzone — caught while writing the helper). +2 tests (`WireForm_HashMatchesSchemaSpecHash`, `WireForm_FlexzoneIncluded`). 1630/1630.
- [ ] Phase 4b: hub-startup global loader — `plh_hub` walks `<hub_dir>/schemas/*.json`, calls `to_hub_schema_record` per file, registers via `_on_schema_registered({owner: "hub", ...})`. Blocked on the `plh_hub` binary (HEP-CORE-0033 Phase 6+).
- [ ] Phase 5: Client-side citation API + role-host refactors. Sub-phases:
  - [x] Phase 5a (2026-04-28, commit `dc8b517`): wire-fields population — `WireSchemaFields` + `make_wire_schema_fields` / `apply_producer_schema_fields` / `apply_consumer_schema_fields` helpers in `schema_utils.hpp`; producer / consumer / processor role hosts populate full HEP-0034 §10 schema fields on REG_REQ / CONSUMER_REG_REQ; broker-side consumer-flexzone fix (mirror of Phase 4a). +7 tests. 1637/1637.
  - [ ] Phase 5b: extract `build_producer_reg_payload()` / `build_consumer_reg_payload()` helpers from the three role hosts (~50 LOC dedup).  Phase 5a's helpers cover the schema fields, but channel/uid/transport fields are still duplicated.
  - [ ] Phase 5c: extract role-host main-loop boilerplate (Steps 4-14) into a shared `RoleHostBase` template (~200 LOC dedup).
  - [ ] Phase 5d: `ProducerOptions::{schema_owner, schema_id}` + `ConsumerOptions::expected_*` C++ API surface; `create<F,D>()` issues `SCHEMA_REQ` for path C, sends BLDS for path B (HEP-0034 §14).  Currently producers populate the wire from config JSON; this lands a typed C++ API for the same fields.
- [ ] Phase 6: Docs sweep + HEP-0016 closure — code review of HEP-0034 implementation; verify cross-references consistent. *Includes:* prune stale `Messenger` citations in HEP-0001/0002/0006/0011/0017/0019/0021/0027/0033 (the class was deleted long ago; what remains is just outdated prose still mentioning it as if active — HEP-0007 was pruned 2026-04-28 as a starting point, the rest are reference-grade text edits).

### Priority 2.5: User-facing demo / example refresh (deferred — after `plh_hub` ships)

📍 **Status**: 🔵 Deferred — touch only after HEP-CORE-0033 hub-binary refactoring stabilizes
📋 **Audit found 2026-04-28**:

The user-facing entry points have not been refreshed for HEP-CORE-0024 (per-role binaries deleted) or HEP-CORE-0034 (config field renames `hub_dir`→`in_/out_hub_dir`, `channel`→`in_/out_channel`, top-level `validation` block removed, `slot_schema`→`in_/out_slot_schema`, mandatory `packing` in schema JSON).  L4 tests use synthetic configs (`plh_role_fixture.h::write_minimal_config`) so CI doesn't catch the drift, but anyone running the in-tree examples will hit immediate parse errors.

Items to address as a single commit once `plh_hub` lands:
- [ ] `README.md` §"The Four Binaries" — replace with `plh_role <dir> --role <tag>` + `plh_hub <dir>` description.  All CLI examples updated.
- [ ] `share/py-demo-single-processor-shm/run_demo.sh` — REQUIRED_BINS list + every binary invocation updated to `plh_role` / `plh_hub`.
- [ ] `share/py-examples/{producer_counter,consumer_logger,sensor_node}.json` — migrate to current field names (`in_/out_*`, drop `validation` block in favour of top-level `checksum` + `stop_on_script_error`, drop top-level `broker` since it's read from hub.json, etc.).
- [ ] `share/py-demo-single-processor-shm/{producer,consumer,processor,hub}/*.json` — same migration.
- [ ] `share/py-demo-dual-processor-bridge/**/*.json` — same migration.
- [ ] In-line `_comment` blocks in those JSONs that say `"Run with: pylabhub-producer <dir>"` etc. — update.
- [ ] Verify each example actually loads via `plh_role --validate` after migration.

Scope rationale (deferred): the hub-side refactoring (HEP-0033 plh_hub binary, Phase 6+) will likely add or rename more config fields; doing the demo refresh once at the end avoids two churns of user-facing files.

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
| Testing | ✅ Complete | `docs/todo/TESTING_TODO.md` | **1181/1181 passing** (2026-03-30). SHM-C2 audit: +2 draining tests, fixed stalling DrainingTimeoutRestoresCommitted. ProcessorHandlerRemoval flake fixed (sleep→poll_until barrier). Full sleep audit: 8 races eliminated in hub_processor_workers.cpp. `test_sync_utils.h` shared facility created. |
| Memory Layout | ✅ Complete | `docs/archive/transient-2026-03-02/MEMORY_LAYOUT_TODO.md` | Single structure; alignment fixed; sub-4K slots. TODO archived; minor test backlog absorbed into TESTING_TODO. |
| Schema Validation | ✅ Complete | — | BLDS schema done; dual-schema producer/consumer validation working |
| Schema Registry | 🟡 In flight (HEP-0034 ratified 2026-04-26) | `docs/HEP/HEP-CORE-0034-Schema-Registry.md` | HEP-0016 (5 phases shipped 2026-03-02) **superseded**. New owner-authoritative model: namespace-by-owner records `(owner_uid, schema_id)`, owner-bound eviction (no refcount), cross-citation rejected even on hash match, `<hub_dir>/schemas/` for hub-globals, fingerprint corrected to include packing. Six implementation phases pending; tracked in §Priority 2. |
| Processor Binary | ✅ Phase 3 complete | `docs/HEP/HEP-CORE-0015-Processor-Binary.md` | **Phase 1+2 done (2026-03-03). Phase 3 config+ScriptHost 2026-03-10:** timing policy, inbox (ROUTER), direct ZMQ PULL input, verify_checksum, zmq_packing/buffer from config. REVIEW_Processor_2026-03-10.md: all 20 items ✅ CLOSED 2026-03-10. **1078/1078 tests.** |
| Startup Coordination | ✅ Phase 1+2 complete | `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` | **Phase 1 (2026-03-11):** `startup.wait_for_roles` config + per-role timeout. **Phase 2 (2026-04-14):** three-response DISC_REQ state machine + heartbeat-multiplier liveness timeouts (no skip; floored at 1 heartbeat) + role-close cleanup hook (federation + band) + `RoleStateMetrics` counters. Heartbeat-death path dereg's immediately (no Closing/grace); voluntary-close keeps grace+FORCE_SHUTDOWN. **1275/1275 tests** (commits `cf53ed3`, `3201e08`, `6558b2c`). |
| Role Directory Service | 🟢 Implemented (Phases 1-4,6) | `docs/HEP/HEP-CORE-0024-Role-Directory-Service.md` | **HEP-0024 Phases 1-4+6 DONE 2026-03-12.** `RoleDirectory` + `role_cli.hpp` public API; all 3 `from_directory()` migrated; all 3 `do_init()`/`parse_args()` migrated; 26 new L2 tests. Deferred: Phase 5 (script-host `script_entry()` migration), Phase 7 (docs), Phase 8 (L4 tests). **1104 tests.** |
| Pipeline Architecture | ✅ Design | `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` | Design complete (2026-03-01, updated 2026-03-05). Five planes (Metrics added), four standalone binaries, topology patterns. |
| Metrics Plane | ✅ Complete | `docs/HEP/HEP-CORE-0019-Metrics-Plane.md` | **Implemented (2026-03-05).** All 5 phases. 19 tests. Heartbeat metrics extension, METRICS_REPORT_REQ, METRICS_REQ/ACK, Python bindings, AdminShell. |
| Interactive Signal Handler | ✅ Complete | `docs/HEP/HEP-CORE-0020-Interactive-Signal-Handler.md` | **Implemented (2026-03-02).** All 4 binaries integrated. Old signal handlers removed. 705/705 pass. |
| Recovery API | ✅ Complete | — | P8 recovery API done; DRAINING recovery restores COMMITTED |
| Messenger / Broker | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | Two-tier shutdown (CHANNEL_CLOSING_NOTIFY + FORCE_SHUTDOWN); Cat 1/Cat 2 health; event handlers; CHANNEL_NOTIFY_REQ relay; HEP-0007 §12 |
| ZMQ Endpoint Registry | ✅ Complete | `docs/HEP/HEP-CORE-0021-ZMQ-Endpoint-Registry.md` | **HEP-0021 implemented (2026-03-06).** `data_transport`+`zmq_node_endpoint` in REG_REQ/DISC_ACK, hub::Producer/Consumer, ProcessorScriptHost. 12 L3 protocol tests (848/848 pass). Deferred: ZMQ data-plane runtime checksum+type-tag (HEP-0023). |
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

### Active (have open items) — as of 2026-04-22

- **`docs/todo/API_TODO.md`** — HEP-CORE-0032 ABI check facility (design
  ready, implementation not started); `PYLABHUB_UTILS_TEST_EXPORT`
  Phases 2-7; std::function/std::optional ABI fixes; C API helpers;
  `src/`+`src/include/` restructure plan (deferred); HEP-0002 architecture
  diagrams tail.
- **`docs/todo/MESSAGEHUB_TODO.md`** — HEP-CORE-0033 Hub Character impl
  (13 prereqs G1-G13); system-level L4 tests folded into HEP-0033 scope;
  6 hub-facing L3 Pattern-3 conversions also folded into HEP-0033 scope
  (from retired 21.L5 tracker).
- **`docs/todo/TESTING_TODO.md`** — Lua V2-fixture cleanup tail
  (~65 remaining V2 tests across chunks 7+); worker-helper unification
  (deferred until Python engine converted); Script-API live-vs-frozen
  contract design; schema/packing round-trip gap; broker-protocol timing
  audit.
- **`docs/todo/PLATFORM_TODO.md`** — Linux-only CI vs documented
  platform-support claim (widen CI or narrow docs); clang-tidy pass;
  MSVC `/Zc:preprocessor` propagation audit; MSVC `/W4 /WX` CI gate.

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
