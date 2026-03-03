# Messenger / Broker TODO

**Purpose:** Track Messenger, BrokerService, HubShell, and binary (producer/consumer/processor) messaging open items.

**Master TODO:** `docs/TODO_MASTER.md`
**Implementation:** `src/utils/ipc/messenger.cpp`, `src/utils/ipc/broker_service.cpp`
**HEP:** `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` §6 (control-plane protocol)

---

## Current Status

✅ **705/705 tests passing (2026-03-02).** HubShell all 6 phases, standalone binaries
(`pylabhub-producer`, `pylabhub-consumer`, `pylabhub-processor`) with Python script hosts,
broker health layer (Cat 1/Cat 2), consumer registration, UID enforcement, SharedSpinLockPy,
GIL/signal handler unified interface, Hub Script Package + tick thread — all complete.
**`pylabhub-actor` eliminated (2026-03-01)** — replaced by standalone producer/consumer/processor.

---

## Recent Completions — GIL / Signal Handler Unified Interface (2026-02-28)

- ✅ **GIL/signal handler unified interface** (2026-02-28): Fixed all GIL-race and signal
  handler override bugs across hub and actor. Introduced `std::optional<py::gil_scoped_release>
  main_thread_release_` in both `HubScript` and `ActorHost` for encapsulated GIL management.
  - **Signal fix**: `PyConfig.install_signal_handlers = 0` applied in `python_interpreter.cpp`
    (hub, via lifecycle) and `actor_main.cpp` (actor, direct `py::scoped_interpreter`) —
    prevents Python's `Py_Initialize` from overriding C++ SIGINT/SIGTERM handlers.
  - **Hub GIL fix**: `HubScript::startup_()` uses IIFE `[&]()->void{...}()` pattern so
    `main_thread_release_.emplace()` fires unconditionally despite early returns (no script dir,
    no `__init__.py`, import error). `shutdown_()` calls `main_thread_release_.reset()` first,
    then `py::gil_scoped_release` around `tick_thread_.join()` to prevent deadlock.
  - **Actor GIL fix**: `ActorHost::start()` calls `main_thread_release_.emplace()` before
    returning. `ActorHost::stop()` calls `main_thread_release_.reset()` then `py::gil_scoped_release`
    around all worker `stop()` + `join()` calls.
  - **Callers clean**: `hubshell.cpp` and `actor_main.cpp` have zero `py::` GIL management —
    plain wait loop and plain `host.stop()` call.
  - Files: `src/hub_python/hub_script.hpp`, `src/hub_python/hub_script.cpp`,
           `src/actor/actor_host.hpp`, `src/actor/actor_host.cpp`, `src/actor/actor_main.cpp`,
           `src/hubshell.cpp`.

---

## Recent Completions — Hub Script Package + Tick Thread (2026-02-27)

- ✅ **Hub script package structure** (2026-02-27): Replaced flat `startup_script` with
  `<hub_dir>/script/__init__.py` Python package pattern (mirrors actor role scripts).
  - `HubConfig`: `hub_script_dir()` / `tick_interval_ms()` / `health_log_interval_ms()` added;
    `python_startup_script()` removed. JSON key: `python.script` (relative path).
  - `do_init()`: creates `script/` dir + `__init__.py` template (on_start / on_tick / on_stop).
  - `hub.json` written with `"python": {"script": "./script", "tick_interval_ms": 1000, …}`.
  - Files: `src/include/utils/hub_config.hpp`, `src/utils/config/hub_config.cpp`, `src/hubshell.cpp`.

- ✅ **HubScriptAPI, ChannelInfo, HubTickInfo** (2026-02-27): Typed Python API analogous to
  `ActorRoleAPI`. Exposed via `hub_script_api` embedded module (PYBIND11_EMBEDDED_MODULE).
  - `HubScriptAPI`: `hub_name()`, `hub_uid()`, `log()`, `shutdown()`, `channels()`,
    `ready_channels()`, `pending_channels()`, `channel(name)`.
  - `ChannelInfo`: typed snapshot (name, status, consumer_count, producer_pid, schema_hash,
    producer_actor_name/uid) + `request_close()` back-pointer pattern.
  - `HubTickInfo`: tick_count, elapsed_ms, uptime_ms, channels_ready/pending/closing.
  - Files: `src/hub_python/hub_script_api.hpp`, `src/hub_python/hub_script_api.cpp`.

- ✅ **HubScript tick thread** (2026-02-27): Dedicated `tick_thread_` for periodic
  health logging + `on_tick` callback dispatch. No GIL held during non-Python work.
  - Automatic health log every `health_log_interval_ms` (no script needed).
  - Script-requested closes collected after `on_tick` returns; dispatched without GIL
    via `BrokerService::request_close_channel()`.
  - `HubScript::startup_()`/`shutdown_()` managed manually in `do_run()` (like BrokerService).
  - Files: `src/hub_python/hub_script.hpp`, `src/hub_python/hub_script.cpp`, `src/CMakeLists.txt`.

- ✅ **BrokerService**: `query_channel_snapshot()` + `request_close_channel()` (2026-02-27):
  - `ChannelSnapshotEntry` / `ChannelSnapshot` structs: typed thread-safe channel snapshot.
  - `close_request_queue_` (mutex-protected deque) drained in run() post-poll phase.
  - Files: `src/include/utils/broker_service.hpp`, `src/utils/ipc/broker_service.cpp`.

- ✅ **pylabhub module**: `hub_uid()` added; `paths()` updated (`hub_script_dir` replaces
  `python_startup_script`). File: `src/hub_python/pylabhub_module.cpp`.

- ✅ **Docs**: `docs/README/README_Deployment.md` §4.5 rewritten (hub script package,
  HubScriptAPI methods, ChannelInfo, HubTickInfo, example).

---

## Recent Completions — Interactive Signal Handler (2026-03-02)

- ✅ **Processor timeout + heartbeat fix** (2026-03-02): Processor timeout path now acquires
  a real output slot (instead of passing `None`) and calls `on_process(None, out_slot, ...)`,
  letting idle processors produce output. Both processor and consumer now always advance
  `iteration_count_` on timeout, fixing the heartbeat stall bug where idle loops were
  falsely declared dead by the broker.
  - Files: `src/processor/processor_script_host.cpp`, `src/consumer/consumer_script_host.cpp`

- ✅ **InteractiveSignalHandler** (2026-03-02): Jupyter Lab-style Ctrl-C handler with status
  display, confirmation prompt, timeout/resume. Cross-platform (POSIX self-pipe + Windows events).
  Reusable API — each binary registers a status callback with role-specific fields.
  - HEP: `docs/HEP/HEP-CORE-0020-Interactive-Signal-Handler.md`
  - Header: `src/include/utils/interactive_signal_handler.hpp`
  - Implementation: `src/utils/core/interactive_signal_handler.cpp`
  - Integrated into all four binaries: `hubshell.cpp`, `producer_main.cpp`, `consumer_main.cpp`, `processor_main.cpp`
  - Removed hand-rolled signal handlers from all four binaries

---

## Current Focus

### Active: Metrics Plane (HEP-CORE-0019)

**Design doc**: `docs/HEP/HEP-CORE-0019-Metrics-Plane.md` (2026-03-02)

Adds a fifth communication plane: passive SHM metrics + voluntary ZMQ reporting → broker aggregation.

- [ ] Phase 1: C++ infrastructure — `report_metric()` API on all three APIs + `MetricsStore` in broker
- [ ] Phase 2: Heartbeat extension — piggyback base counters + custom KV on `HEARTBEAT_REQ`
- [ ] Phase 3: Consumer metrics — `METRICS_REPORT_REQ` (fire-and-forget)
- [ ] Phase 4: Query API — `METRICS_REQ`/`METRICS_ACK` + AdminShell binding
- [ ] Phase 5: Python bindings — `api.report_metric()` in pybind11 modules

### Active: Layer 4 Producer + Consumer Tests (2026-03-02)

Producer and consumer binaries need Layer 4 test coverage (config parsing + CLI). Tracked in
`docs/todo/TESTING_TODO.md` § "Layer 4: pylabhub-producer Tests" + "Layer 4: pylabhub-consumer Tests".

### Active: Integration Test (2026-03-02)

End-to-end integration test: `pylabhub-producer` + `pylabhub-hubshell` + `pylabhub-consumer`
round-trip via live broker. Tracked in `docs/todo/API_TODO.md` § Step 5.

---

## Historical Completions — Actor Code Review (2026-02-22, actor eliminated 2026-03-01)

The `pylabhub-actor` multi-role container was eliminated on 2026-03-01 (HEP-CORE-0018).
The actor code review items below (all ✅ fixed) are preserved as historical reference.
The actor files (`src/actor/`, `tests/test_layer4_actor/`) have been deleted from disk.

**Code review items resolved (2026-02-22):**
- ✅ Per-role `Messenger` wiring (B3) — each role worker owns `hub::Messenger messenger_`
- ✅ AdminShell 1 MB request size limit (A1) — `src/hub_python/admin_shell.cpp`
- ✅ `InterpreterReadiness` enum (C1) — 5-state atomic enum in `python_interpreter.cpp`
- ✅ `_registered_roles()` on_stop/on_stop_c (B1) — `src/actor/actor_module.cpp`
- ✅ Actor schema validation (B2) — type-string + count≥1 check
- ✅ CurveZMQ client keypair wiring — `Messenger::connect()` + actor `ActorAuthConfig`
- ✅ Script error counter `api.script_error_count()` — per-role uint64_t
- ✅ LoopTimingPolicy fixed_pace/compensating deadline scheduling
- ✅ RoleMetrics supervised diagnostics: script_error_count, loop_overrun_count, last_cycle_work_us

### Recent Completions — Embedded-Mode Tests + ZMQ_BLOCKY Fix (2026-02-25)

- ✅ **Layer 4 embedded-mode tests pass (2026-02-25)** — 10 new tests in
  `tests/test_layer4_actor/test_actor_embedded_mode.cpp` (4 Producer, 6 Consumer).
  All 517 tests pass; each embedded-mode test completes in ~120ms.
- ✅ **`zmq_context_shutdown()` hang fixed — `ZMQ_BLOCKY=0` at context creation (2026-02-25)**.
  Root cause: all ZMQ sockets default to `LINGER=-1`; `zmq_ctx_term()` blocked indefinitely
  waiting for ZMQ's I/O threads to drain pending sends (BYE ctrl frame, SUB unsubscription,
  heartbeats). Fix: `zmq_ctx_set(ctx, ZMQ_BLOCKY, 0)` in `zmq_context_startup()` — all
  subsequently created sockets inherit `LINGER=0`. This makes `zmq_close()` and therefore
  `zmq_ctx_term()` return immediately without waiting for message delivery.
  The `g_context->shutdown()` call added in the same session (belt-and-suspenders) handles
  the edge case where a socket is explicitly set to `LINGER > 0` after creation.
  Production safe: the lifecycle module ordering guarantees messages are delivered before
  `zmq_ctx_term()` is called; only tests without a full lifecycle are affected.
  File: `src/utils/zmq_context.cpp`

### Recent Completions — Per-Role Script Packages (2026-02-25)

- ✅ **Per-role script support** (2026-02-25):
  Each role now resolves its own Python package instead of sharing a single actor-level
  module. Per-role `"script": {"module": "script", "path": "./roles/<role_name>"}` in the
  role config. Actor-level `"script"` block still supported as a fallback for roles that
  omit the per-role key.
  - `RoleConfig::script_module` / `script_base_dir` fields added and parsed from role `"script"` object.
    Bare string format throws `std::runtime_error` (must be an object).
  - `ActorHost::role_modules_` map (`role_name → py::module_`) replaces single `script_module_`.
  - New `import_role_script_module()` helper: resolves `<base_dir>/<module>/__init__.py` (package)
    or `<base_dir>/<module>.py` (flat); registers in `sys.modules` under role-unique alias
    `_plh_{uid_hex}_{role_name}`; sets `submodule_search_locations` to enable `from . import helpers`.
  - `do_init()` creates `roles/data_out/script/__init__.py` with template callbacks; actor.json
    template uses per-role `"script"` key.
  - Standard directory layout: `roles/<role_name>/script/__init__.py` (script/ subdirectory is
    the Python package; separates Python source from other config files in the role dir).
  - 6 new Layer 4 unit tests (`ActorConfigPerRoleScript` suite): absent key, object parse,
    module-only parse, string throws, consumer role, multi-role distinct paths.
  - `docs/tech_draft/ACTOR_DESIGN.md §3` fully rewritten: §3.1 (package structure + isolation),
    §3.2 (import guide), §3.3 (callback example with relative imports), §3.4 (callbacks table).
  - `docs/README/README_DirectoryLayout.md §3` updated with current actor directory layout.
  - `docs/HEP/HEP-CORE-0005-script-interface-framework.md` updated: per-role package convention note.
  - 507/507 tests pass.
  - Files: `actor_config.hpp`, `actor_config.cpp`, `actor_host.hpp`, `actor_host.cpp`,
           `actor_main.cpp`, `tests/test_layer4_actor/test_actor_config.cpp`.

### Recent Completions — Actor Thread Model Redesign (HEP-CORE-0010)

**HEP**: `docs/HEP/HEP-CORE-0010-Actor-Thread-Model-and-Unified-Script-Interface.md`

- ✅ **Phase 1: Unified script interface (callback-routing approach)** (2026-02-24)
  - Replaced decorator dispatch table (`actor_dispatch_table.hpp` deleted) with module-based
    `on_iteration` / `on_init` / `on_stop` attribute lookup
  - ZMQ callbacks (on_consumer_message, on_zmq_data) now push to `incoming_queue_` (mutex +
    condvar); loop thread drains before GIL — GIL race eliminated
  - Unified `on_iteration(slot, flexzone, messages, api)` for both producer and consumer
  - Script format: `"script": {"module": "...", "path": "..."}` (object only; string rejected)
  - New config fields: `loop_trigger` (shm/messenger), `messenger_poll_ms`, `heartbeat_interval_ms`
  - `api.set_critical_error()` latch added; `api.trigger_write()` removed
  - `run_loop_messenger()` added to both workers for Messenger-triggered loops
  - Module imported via importlib with synthetic alias `_plh_{uid_hex}_{module_name}`
  - Layer 4 tests: 66/66 passing (added LoopTrigger, Script, critical_error test sections)
  - `docs/tech_draft/ACTOR_DESIGN.md` updated: §1, §3, §4.3, §4.4, §6, §7 — all decorator
    references replaced; section numbering 3.1–3.9 consistent
  - Files: `actor_config.hpp/cpp`, `actor_host.hpp/cpp`, `actor_api.hpp/cpp`,
           `actor_module.cpp`, `actor_main.cpp`, `actor_dispatch_table.hpp` (deleted)

- ✅ **Phase 2: Full ZMQ thread consolidation (EmbeddedMode)** (2026-02-24)
  - Embedded-mode API added to Producer: `start_embedded()`, `peer_ctrl_socket_handle()`,
    `handle_peer_events_nowait()` — refactored from `run_peer_thread()` helpers
  - Embedded-mode API added to Consumer: `start_embedded()`, `data_zmq_socket_handle()`,
    `ctrl_zmq_socket_handle()`, `handle_data_events_nowait()`, `handle_ctrl_events_nowait()`
  - Actor's `zmq_thread_` (one per role worker) drives `zmq_poll` directly; eliminates
    `peer_thread` / `data_thread` / `ctrl_thread` — true 2-thread-per-role model
  - `iteration_count_` (atomic uint64) incremented by `loop_thread_` after each iteration;
    read by `zmq_thread_` (Phase 3: triggers application-level heartbeat when count advances)
  - SHM acquire timeout fixed: producer uses `interval_ms` (or 5ms max-rate); consumer uses
    `timeout_ms` (or 5ms max-rate, or 5000ms indefinite) — was hardcoded 100ms
  - Consumer `timeout_ms > 0`: `on_iteration(slot=None,...)` now called on slot miss (watchdog use)
  - `zmq_thread_` launched BEFORE `call_on_init()` to match old `peer_thread`/`ctrl_thread` timing
  - `stop()` guard updated to check `loop_thread_.joinable() && zmq_thread_.joinable()` — prevents
    `std::terminate` when `api.stop()` called from `on_init`
  - Design docs updated: HEP-CORE-0010 §3.5 init sequence, §3.6 mermaid diagram;
    ACTOR_DESIGN.md §4.3 members, §4.5 thread interaction section added
  - Files: `hub_producer.hpp/cpp`, `hub_consumer.hpp/cpp`, `actor_host.hpp/cpp`

- ✅ **Phase 3: Application-level heartbeat via zmq_thread_** (2026-02-24)
  - `Messenger::suppress_periodic_heartbeat(channel, suppress=true)` — disables 2s periodic timer per channel
  - `Messenger::enqueue_heartbeat(channel)` — thread-safe fire-and-forget HEARTBEAT_REQ via Messenger worker queue
  - Both implemented via new `SuppressHeartbeatCmd` / `HeartbeatNowCmd` command variants in messenger.cpp
  - `HeartbeatEntry::suppressed` flag added; `send_heartbeats()` skips suppressed entries
  - `ProducerRoleWorker::start()`: calls `suppress_periodic_heartbeat` + `enqueue_heartbeat` after `start_embedded()`
  - `ProducerRoleWorker::run_zmq_thread_()`: sends heartbeat when `iteration_count_` advances,
    throttled by `hb_interval` (derived from `heartbeat_interval_ms` / `interval_ms` / default 2000ms)
  - First iteration advance fires immediately (initialised to `now - hb_interval`)
  - Consumer roles unchanged — consumers don't own channels; no heartbeat responsibility
  - No broker protocol changes required: existing heartbeat-timeout → CHANNEL_CLOSING_NOTIFY path enforces liveness
  - 501/501 tests pass
  - Files: `src/include/utils/messenger.hpp`, `src/utils/messenger.cpp`, `src/actor/actor_host.cpp`

### Deferred / Blocked

- [ ] **Schema registry** — Broker stores and serves schema_hash for producer channels.
  Currently schema_hash is validated at REG_REQ time but not persisted for consumer query.
  Tracked as HEP-CORE-0016 Phases 2–5 (see `docs/todo/API_TODO.md` § Named Schema Registry).

### By design — explicitly out of scope

- **Broker reconnection** — A broker crash is a catastrophic failure: all channel registration
  state, consumer records, and heartbeat timers are lost. There is no correct way to
  reconstruct this state post-crash — any reconnect attempt would operate on stale, partially
  valid knowledge. The correct response is a clean exit. The existing heartbeat timeout →
  `CHANNEL_CLOSING_NOTIFY` → `on_channel_closing` callback chain already achieves this.
  Do not add reconnect logic.

- **Consumer reconnect after producer restart** — Same reasoning. When the producer process
  restarts, the SHM segment is unlinked and recreated. A consumer holding the old SHM handle
  is in an undefined state. No reconnect is possible or meaningful; clean exit is correct.

- **Partial state recovery after any IPC crash** — All useful information (SHM state, channel
  registry, consumer bitmap, sequence numbers) belongs to the crashed process. Restoring a
  subset of it would create an inconsistent view worse than starting fresh. Operators restart
  the pipeline; the system reinitialises cleanly.

---

### Recent Completions — Processor Timeout + Consumer Heartbeat Fix (2026-03-02)

- ✅ **Processor timeout path provides output slot** (2026-03-02): On input timeout,
  `run_loop_shm_()` now acquires a real output slot (via `acquire_write_slot()`) and passes it
  to `on_process(None, out_slot, fz, msgs, api)` — letting the processor produce output even
  without input. Commit/discard + checksum logic mirrors the normal path. Previously passed
  `None` for both input and output.
  - File: `src/processor/processor_script_host.cpp`

- ✅ **Processor/consumer `iteration_count_` always advances on timeout** (2026-03-02):
  Moved `iteration_count_.fetch_add(1)` outside the `if (timeout_ms > 0 || !msgs.empty())`
  guard in both processor and consumer `run_loop_shm_()`. Previously, when the input queue was
  idle and `timeout_ms <= 0` with no messages, `iteration_count_` never advanced, causing the
  zmq_thread_ to stop sending heartbeats — the broker falsely declared healthy-but-idle
  processors/consumers as dead.
  - Files: `src/processor/processor_script_host.cpp`, `src/consumer/consumer_script_host.cpp`

---

## Backlog

### Broker Features (not yet started)

- [ ] **File-based discovery** — Alternative to broker for single-machine use (no ZMQ)
- [ ] **Version negotiation** — Broker/client protocol version handshake
- [ ] **Embedded broker mode** — Run broker in-process for testing (avoids bind/port)
- [ ] **Connection pooling** — Reuse ZMQ DEALER sockets across channels

### HubShell Enhancements

- [ ] **Python SDK (user-facing)** — Non-template `hub::Producer` / `hub::Consumer` Python
  bindings (pip-installable `pylabhub` package). Requires non-template write/read path and
  GIL management in C++ thread callbacks. Separate from the `pylabhub_module` embedded admin SDK.

---

## Design Notes

### Error Taxonomy (Cat 1 / Cat 2)

| Category | Trigger | Broker action | Producer callback |
|---|---|---|---|
| Cat 1 | Schema mismatch on REG_REQ | CHANNEL_ERROR_NOTIFY | `on_channel_error` |
| Cat 1 | Heartbeat timeout (channel gone) | CHANNEL_CLOSING_NOTIFY | `on_channel_closing` |
| Cat 2 | Consumer PID dead (liveness check) | CONSUMER_DIED_NOTIFY | `on_consumer_died` |

See `docs/IMPLEMENTATION_GUIDANCE.md` § "Error Taxonomy — Broker, Producer, and Consumer".

### Key Design Decisions

- **Messenger ownership model** — ZMQ context is process-wide (singleton via
  `GetZMQContextModule()`). HubShell uses the singleton `Messenger` (one broker connection,
  shared across all admin operations). Actor role workers each own a private `hub::Messenger`
  value member; each connects to its own `role.broker` endpoint in `start()`. This enables
  multi-broker actor deployments and eliminates the need for a singleton Messenger in actor.
- **JSON control plane** — human-readable, easy schema evolution; acceptable for control plane
  (not data plane); see HEP-CORE-0002 §6 for message formats.
- **Graceful degradation** — DataBlock creation succeeds even if broker is down; applications
  function with reduced features (no discovery, no schema validation).
- **DataBlock fully decoupled from Messenger** — factory functions take no hub parameter;
  broker registration is caller-initiated.

---

## Recent Completions (Summary)

| Date | What |
|---|---|
| 2026-02-23 | LoopTimingPolicy: fixed_pace/compensating deadline scheduling for producer interval_ms and consumer timeout_ms loops |
| 2026-02-23 | RoleMetrics supervised diagnostics: script_error_count, loop_overrun_count, last_cycle_work_us — read-only from Python; reset_all_role_run_metrics() on role restart |
| 2026-02-22 | Script error counter: api.script_error_count() — per-role uint64_t; incremented on every Python callback exception; exposed in pybind11 |
| 2026-02-22 | CurveZMQ client keypair wired: Messenger::connect() accepts client_pubkey/seckey; plain TCP when server_key empty; ActorAuthConfig::load_keypair() reads keyfile JSON |
| 2026-02-22 | Per-role Messenger: workers own messenger_, connect to role.broker+broker_pubkey in start(); ActorHost/actor_main singleton removed |
| 2026-02-21 | Gap fixes: demo.sh, --keygen (zmq_curve_keypair), schema hash, timeout_constants.hpp; ACTOR_DESIGN.md §12-13 |
| 2026-02-21 | pylabhub-actor multi-role; ctypes zero-copy schema; SharedSpinLockPy; UID enforcement; 426/426 tests |
| 2026-02-20 | HubShell phases 3-6: broker thread, PythonInterpreter, AdminShell, hubshell.cpp full rewrite |
| 2026-02-20 | HubConfig phase 1+2: layered JSON, hub.default.json.in, prepare_python_env target |
| 2026-02-19 | Broker health layer: Cat 1/Cat 2 notification; per-channel Messenger callbacks; Producer/Consumer auto-wire |
| 2026-02-19 | SlotProcessor API (HEP-0006): push/synced_write/pull, set_write/read_handler, WriteProcessorContext |
| 2026-02-18 | Consumer registration protocol: CONSUMER_REG/DEREG; E2E multi-process test; pylabhub-broker |
| 2026-02-18 | Phase C broker integration tests: REG/DISC/DEREG/schema-mismatch/channel-not-found; ChannelHandle tests |
| 2026-02-17 | MessageHub→Messenger rename; ZMQContext lifecycle; DataBlock decoupled from Messenger |
