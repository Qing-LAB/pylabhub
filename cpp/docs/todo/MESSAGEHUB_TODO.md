# Messenger / Broker TODO

**Purpose:** Track Messenger, BrokerService, HubShell, and pylabhub-actor open items.

**Master TODO:** `docs/TODO_MASTER.md`
**Implementation:** `src/utils/messenger.cpp`, `src/utils/broker_service.cpp`
**HEP:** `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` §6 (control-plane protocol)
**Actor Design:** `docs/tech_draft/ACTOR_DESIGN.md`

---

## Current Status

✅ **528/528 tests passing (2026-02-26).** HubShell all 6 phases, pylabhub-actor
multi-role system with per-role Python packages, broker health layer (Cat 1/Cat 2),
consumer registration, UID enforcement, SharedSpinLockPy, HEP-CORE-0010 Phases 1–3
(unified on_iteration, ZMQ thread consolidation, application-level heartbeat),
actor identity + hub_dir wiring, --init flows, embedded-mode unit tests — all complete.

---

## Current Focus

### Code Review Open Items — Status (2026-02-22)

These findings came from `docs/archive/transient-2026-02-21/code_review_utils_2025-02-21.md`:

- ✅ **FIXED 2026-02-22 (extended 2026-02-22)** — **role.broker field now fully wired** (B3):
  Each `ProducerRoleWorker` and `ConsumerRoleWorker` owns its own `hub::Messenger messenger_`
  (value, not reference). `start()` calls `messenger_.connect(role_cfg_.broker,
  role_cfg_.broker_pubkey)` before `Producer::create` / `Consumer::connect`. Failed connect
  logs a warning and continues (degraded mode: SHM channel still works). `broker_pubkey`
  (Z85, 40 chars) added to `RoleConfig` and parsed from JSON `"broker_pubkey"` field.
  `ActorHost` no longer holds a Messenger; `actor_main.cpp` no longer calls
  `GetLifecycleModule()` or `get_instance()`. ZMQ context stays process-wide.
  Files: `src/actor/actor_config.hpp`, `src/actor/actor_config.cpp`,
         `src/actor/actor_host.hpp`, `src/actor/actor_host.cpp`, `src/actor/actor_main.cpp`.

- ✅ **FIXED 2026-02-22** — **AdminShell: no request body size limit** (A1): Size check
  before `std::string raw` construction and `json::parse()`. Limit: 1 MB. REP sends
  `{"error":"request too large"}` and continues.
  File: `src/hub_python/admin_shell.cpp`

- ✅ **FIXED 2026-02-22** — **`PyExecResult::result_repr` kept with doc comment** (C2):
  Field retained; doc comment clarifies "Not yet implemented; reserved for future AdminShell
  exec path." User directive: do not remove.
  File: `src/hub_python/python_interpreter.hpp`

- ✅ **FIXED 2026-02-22** — **`g_py_initialized` evolved to `InterpreterReadiness`** (C1):
  Replaced `static std::atomic<bool>` with `static std::atomic<InterpreterReadiness>` enum
  (5 states: Uninitialized/Initializing/Ready/Degraded/Failed). `is_interpreter_ready()`
  internal accessor added for future exec() guards.
  File: `src/hub_python/python_interpreter.cpp`

- ✅ **FIXED 2026-02-22** — **`_registered_roles()` missing `on_stop` / `on_stop_c`** (B1):
  Added `on_stop` → `tbl.on_stop_p` and `on_stop_c` → `tbl.on_stop_c` branches.
  File: `src/actor/actor_module.cpp`

- ✅ **FIXED 2026-02-22** — **Actor schema validation is shallow** (B2): Added type-string
  validation against 13-element `kValidTypes` array and `count >= 1` check. Clear error
  messages at parse time (actor exits at startup).
  File: `src/actor/actor_schema.cpp`

- ✅ **FIXED 2026-02-22** — **CurveZMQ client keypair wired** (Phase 2): `Messenger::connect()`
  extended with optional `client_pubkey`/`client_seckey`. Plain TCP when `server_key` empty;
  CURVE with actor's own keypair (from `auth.keyfile`) or ephemeral when keyfile absent.
  `ActorAuthConfig::load_keypair()` reads keyfile JSON post-lifecycle. `ActorAuthConfig auth_`
  added to both worker types; wired through constructors and `ActorHost::start()`.
  Files: `src/include/utils/messenger.hpp`, `src/utils/messenger.cpp`,
         `src/actor/actor_config.hpp`, `src/actor/actor_config.cpp`,
         `src/actor/actor_host.hpp`, `src/actor/actor_host.cpp`, `src/actor/actor_main.cpp`.

- ✅ **FIXED 2026-02-22** — **Script error counter** (`api.script_error_count()`): Added
  `uint64_t script_error_count_` to `ActorRoleAPI`. `increment_script_errors()` called in
  every `py::error_already_set` catch block for user script callbacks (on_init, on_write,
  on_read, on_read_timeout, on_data, on_message, on_stop — both producer and consumer).
  `script_error_count()` exposed in pybind11 bindings. Resets to zero on role restart.
  Files: `src/actor/actor_api.hpp`, `src/actor/actor_host.cpp`, `src/actor/actor_module.cpp`.

### Completions (2026-02-23)

- ✅ **FIXED 2026-02-23** — **LoopTimingPolicy for producer/consumer deadline scheduling**:
  Replaced sleep-at-top loop pattern with deadline-based scheduling in both
  `run_loop_shm()` and `run_loop_zmq()`. `LoopTimingPolicy` enum added to `RoleConfig`
  with two options: `fixed_pace` (default — `next = now() + interval`; no catch-up,
  rate ≤ target) and `compensating` (`next += interval`; fires immediately after overrun,
  average rate converges to target). Applies to both producer `interval_ms` and consumer
  `timeout_ms`. Consumer `last_slot_time` policy is symmetric. Parsed from JSON
  `"loop_timing"` field in role config.
  Files: `src/actor/actor_config.hpp`, `src/actor/actor_config.cpp`,
         `src/actor/actor_host.cpp`.

- ✅ **FIXED 2026-02-23** — **RoleMetrics supervised diagnostics API**:
  Grouped all diagnostic counters into a private `RoleMetrics` struct in `ActorRoleAPI`.
  C++ host writes through controlled methods; Python script gets individual read-only
  getters. Three counters: `script_error_count()` (exceptions in all callbacks),
  `loop_overrun_count()` (write cycles where interval_ms deadline was already past),
  `last_cycle_work_us()` (µs of active work — acquire + on_write + commit — in the last
  write cycle). All are per-role-run: reset via `reset_all_role_run_metrics()` at role
  start. Python cannot reset — by design (script is under supervision).
  Overrun counter wired in both producer loops' overrun `else`-branch. Work time capture
  added before acquire in both loops. `reset_all_role_run_metrics()` called in both
  `ProducerRoleWorker::start()` and `ConsumerRoleWorker::start()`.
  Files: `src/actor/actor_api.hpp`, `src/actor/actor_host.cpp`, `src/actor/actor_module.cpp`.

### Additional Completions (2026-02-22)

- ✅ **FIXED 2026-02-22** — **Broker payload size limit** (A2): Added 1 MB size check
  before `frames[3].to_string()` and JSON parse in broker run loop. ROUTER silently drops.
  File: `src/utils/broker_service.cpp`

- ✅ **FIXED 2026-02-22** — **ChannelPattern deduplication** (D1): Shared
  `channel_pattern_to_str()` / `channel_pattern_from_str()` moved to `channel_pattern.hpp`.
  Removed duplicate `pattern_to_wire/from_wire` in `messenger.cpp` and
  `pattern_to_str/from_str` in `broker_service.cpp`.
  Files: `src/include/utils/channel_pattern.hpp`, `src/utils/messenger.cpp`,
  `src/utils/broker_service.cpp`

- ✅ **FIXED 2026-02-22** — **PylabhubEnv getters** (F1/F2/F3): Added `actor_name()`,
  `channel()`, `broker()`, `kind()`, `log_level()`, `script_dir()` to `ActorRoleAPI`.
  Wired from constructor (channel/broker/kind) and `ActorHost::start()` (actor_name,
  log_level, script_dir). All getters registered in pybind11 bindings.
  Files: `src/actor/actor_api.hpp`, `src/actor/actor_host.hpp`, `src/actor/actor_host.cpp`,
  `src/actor/actor_module.cpp`

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

### Deferred / Backlog — Actor Thread Model

- [ ] **Embedded-mode integration tests** — layer 3 tests using `run_gtest_worker()` with lifecycle modules.
  Tests: `start_embedded()` true/idempotent, `peer_ctrl_socket_handle()` non-null,
  `ctrl_zmq_socket_handle()` non-null, `data_zmq_socket_handle()` null for Bidir.
  Deferred from Phase 2: requires live broker in test infrastructure.
- [ ] **`wake_send`/`wake_recv` inproc PAIR** — low-latency loop→zmq_thread_ notification (Phase 4 if needed).
  Currently `zmq_thread_` wakes every `messenger_poll_ms` (5ms); wake socket reduces outbound latency for
  `api.broadcast()` calls inside `on_iteration`.

### Deferred / Blocked

- [ ] **Schema registry** — Broker stores and serves schema_hash for producer channels.
  Currently schema_hash is validated at REG_REQ time but not persisted for consumer query.

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

## Backlog

### Broker Features (not yet started)

- [ ] **File-based discovery** — Alternative to broker for single-machine use (no ZMQ)
- [ ] **Version negotiation** — Broker/client protocol version handshake
- [ ] **Embedded broker mode** — Run broker in-process for testing (avoids bind/port)
- [ ] **Connection pooling** — Reuse ZMQ DEALER sockets across channels

### Actor / HubShell Enhancements

- [x] **Actor worker common helpers** ✅ FIXED 2026-02-23 — `step_write_deadline_()` and
  `check_read_timeout_()` extracted. See RAII_LAYER_TODO.md.

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
