# Messenger / Broker TODO

**Purpose:** Track Messenger, BrokerService, HubShell, and binary (producer/consumer/processor) messaging open items.

**Master TODO:** `docs/TODO_MASTER.md`
**Implementation:** `src/utils/ipc/messenger.cpp`, `src/utils/ipc/broker_service.cpp`
**HEP:** `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` §6 (control-plane protocol)

---

## Current Status

✅ **1275/1275 tests (2026-04-14).** HEP-CORE-0023 Phase 2 complete:
heartbeat-multiplier role-liveness state machine, three-response DISC_REQ,
role-close cleanup hook (federation + band), `RoleStateMetrics` counters.
See commits `cf53ed3`, `3201e08`, `6558b2c`.

### Open: HEP-CORE-0033 Hub Character refactor (in progress)

**Implementation reference**: `docs/HEP/HEP-CORE-0033-Hub-Character.md`
(see §14 for 10 implementation phases; §15 for 8 open items deferred to
implementation).
**Prerequisites**: `docs/tech_draft/HUB_CHARACTER_PREREQUISITES.md` — 13
items (G1-G13).

**Completed**:
- G1 (host template): `RoleHostBase` = `EngineHost<ApiT>` template. `139b4ca`.
- G2 design ratified ("broker as single mutator" model; G3+G4 absorbed). `e9fc8f6`.
- G2.1 (HubState skeleton + entry types): compile-only landed. `8e1eadc`.
  - `src/include/utils/hub_state.hpp` — class + 7 entry types + enums
    + AuthContext + event subscription.
  - `src/utils/ipc/hub_state.cpp` — shared_mutex, friend-only `_set_*`
    mutators, handler fire outside state lock.
  - 17 L2 unit tests (`Layer2_HubState`) via `friend struct
    test::HubStateTestAccess` shim.
  - Not yet wired into BrokerService — that's G2.2.

**Next** (in order) — grouped by hub capability, not by broker-map structure
(see `HUB_CHARACTER_PREREQUISITES.md` §G2 "Capability-operation mutator
layer" for full rationale):
- **G2.2.0** — Plumb `hub::HubState` into `BrokerServiceImpl`; add
  capability-operation layer (`_on_*` methods composing primitive
  `_set_*` setters); add `const HubState& hub_state()` accessor. No
  handler touches yet. ~150 LOC.
- **G2.2.1** — Registration lifecycle: `REG/DEREG/CONSUMER_REG/
  CONSUMER_DEREG` handlers → `_on_channel_registered` / `_on_channel_closed` /
  `_on_consumer_joined` / `_on_consumer_left`. Delete `ChannelRegistry`
  (`channel_registry.hpp/.cpp`). RoleEntry + ShmBlockRef populated
  inside the ops. Readers flip to `hub_state_`.
  - G2.2.1.a (channel dual-write): ✅ done.
  - G2.2.1.b (flip readers + heartbeat capability-op): ✅ done.
  - G2.2.1.c phase 1 (read-only handlers source from HubState; helper
    signatures take `hub::ChannelEntry`/`hub::ConsumerEntry`): ✅ done
    (this commit). 11 `find_channel`, 2 `find_consumers`, 4 const
    `all_channels()` migrated. 4 test files updated to valid uid format.
  - G2.2.1.c phase 2 (mutating sweeps absorbed into HubState
    capability ops): pending — requires sweep-cursor primitives or
    full snapshot+iterate replacements for `find_timed_out_*`,
    dead-consumer iteration, closing-deadline iteration.
  - G2.2.1.c phase 3 (delete `ChannelRegistry`, `channel_registry.{hpp,cpp}`,
    translator helpers): pending — gated on phase 2.
- **G2.2.2** — Liveness: `HEARTBEAT_REQ` + timeout sweep → `_on_heartbeat` /
  `_on_heartbeat_timeout` / `_on_pending_timeout`. Counter bumps absorbed.
- **G2.2.3** — Membership routing: bands + federation peers. Delete
  `BandRegistry` and `inbound_peers_`. Keep `channel_to_peer_identities_`
  and `hub_connected_notified_` broker-private (transport-layer, not
  state).
- **G2.2.4** — Observability: `_on_message_processed` absorbs per-msg-type
  counter bumps. `metrics_store_` absorption deferred pending data-model
  decision (role-centric vs secondary channel-metrics map vs leave
  broker-private).
- G2.3 — `HubAPI` read accessors only (script can query state, not mutate).
- G2.4 — `HubAPI` mutation wrappers; remove `close_request_queue_` /
  `broadcast_request_queue_` / `hub_targeted_queue_` ad-hoc queues.
- G2.5 — `AdminService` shell using same broker mutators.
- G7 — HubConfig lifecycle-module vs main-owned decision.
- G5/G6/G8 spec gaps; G9-G13 ripples.

Summary of what this HEP introduces:
- `plh_hub` unified binary (replaces disabled `pylabhub-hubshell`).
- `HubConfig` composite (parallel to `RoleConfig`), `hub_cli`, `HubDirectory`.
- `HubHost` class owning `HubState` (channels/roles/bands/peers/shm_blocks/counters).
- `AdminService` structured RPC (replaces `AdminShell` Python-eval-only).
- `ScriptEngine`-based scripting (retires bespoke `PythonInterpreter`).
- `HubAPI` pybind11 + Lua bindings (via `ScriptEngine`).
- Query-driven metrics model (supersedes HEP-0019 §3-4; diagram in HEP-0033 §9).

Implementation not started — role unification (HEP-0024 Phases 1-22) is now
the precedent completed work.

**System-level L4 tests** (become writable at HEP-0033 Phase 9 when
`plh_hub` binary lands). Designed-from-scratch once the hub binary is
re-enabled in `src/CMakeLists.txt`:

- [ ] **plh_role run-mode lifecycle**: spawn `plh_role <dir>`, verify it
  reaches the data loop, accepts SIGTERM, exits 0 with clean shutdown
  diagnostics. Cover all 3 roles. Replaces the deleted
  `test_layer4_{producer,consumer,processor}/` run-mode tests.
- [ ] **plh_role + broker round-trip**: producer → broker → consumer
  end-to-end via plh_role binaries; verify SHM data flow + control-plane
  HELLO/BYE/registration tracking. Replaces deleted
  `tests/test_layer4_integration/test_pipeline_roundtrip.cpp`.
- [ ] **plh_role channel broadcast**: hub broadcast/notify control
  plane reaching multiple plh_role consumers. Replaces deleted
  `tests/test_layer4_integration/test_channel_broadcast.cpp`.
- [ ] **plh_role cross-role processor pipeline**: producer → processor →
  consumer chain through broker; verify data transformation + ordering
  preserved end-to-end.
- [ ] **plh_role hub-dead detection**: kill hub mid-flight; verify
  plh_role detects via BrokerRequestComm socket monitor and shuts down
  with `StopReason::HubDead`.
- [ ] **plh_role inbox round-trip**: consumer with inbox → producer
  posts to inbox via `api.open_inbox(uid)`; verify message receipt and
  ack flow.

`tests/test_layer4_integration/test_admin_shell.cpp` (disabled via
`if(FALSE)`) preserved as reference for the admin-shell ZMQ protocol
that HEP-0033 will replace with structured RPC (`AdminService`).

**Pattern-3 conversion of existing hub-facing L3 tests** (folded in
from the retired `21.L5` test-harness tracker 2026-04-22 — these test
files exercise hub-owned subsystems that HEP-0033 will rewrite or
heavily touch, so their lifecycle rework is coupled with the refactor
and is best done as tests are rebuilt against the new shape rather than
chased through the old one):

- [ ] `test_datahub_hub_config_script.cpp` (6 `LifecycleGuard`
      violations) — HubConfig script-block parsing; couples to HEP-0033
      §6 `HubConfig` composite.
- [ ] `test_datahub_hub_zmq_queue.cpp` (4) — ZmqQueue at hub layer;
      couples to HEP-0033 §7 broker-owned queue integration.
- [ ] `test_datahub_hub_inbox_queue.cpp` (4) — inbox at hub layer; likely
      rewritten against the new `HubAPI` inbox surface.
- [ ] `test_datahub_zmq_endpoint_registry.cpp` (3) — endpoint tracking;
      couples to HEP-0033 §8 `HubState`.
- [ ] `test_datahub_metrics.cpp` (3) — broker metrics; couples to HEP-0033
      §9 query-driven metrics (supersedes HEP-0019 §3-4).
- [ ] `test_datahub_hub_federation.cpp` (3) — federation peer map;
      couples to HEP-0033 `HubState` federation view.

Conversion pattern per file: each TEST_F spawns a worker subprocess via
`IsolatedProcessTest::SpawnWorker` + `run_gtest_worker(..., module_list)`
(Logger + Crypto + DataBlock + whatever the subsystem needs).  Worker
bodies in `workers/<file-base>_workers.{cpp,h}`, milestones
`[WORKER_BEGIN]`/`[WORKER_END_OK]`/`[WORKER_FINALIZED]`, dispatcher
registrar in anonymous namespace.  See `test_role_api_flexzone.cpp` +
`workers/role_api_flexzone_workers.cpp` (converted 2026-04-21) for the
reference shape.  Two other files in the original 21.L5 list —
`test_datahub_role_flexzone.cpp` → `test_role_api_flexzone.cpp` and
`test_datahub_loop_policy.cpp` → `test_role_api_loop_policy.cpp` +
`test_role_api_raii.cpp` — were converted during HEP-0024 closure; they
do not block HEP-0033.

### Completed 2026-04-14: HEP-CORE-0023 Phase 2 — Role-Liveness State Machine

- Two-pass `check_heartbeat_timeouts`: Ready→Pending (transient demotion,
  recovers on heartbeat) → Pending→deregistered immediately (no Closing/grace
  for heartbeat-death; producer presumed dead).
- Voluntary close path unchanged: Ready→Closing+grace→FORCE_SHUTDOWN.
- All effective timeouts derive from `heartbeat_interval` (default 500ms = 2 Hz)
  × miss-count (default 10/10), with std::optional overrides. Floored at one
  heartbeat — misconfiguration cannot create permanent dangling state.
- `BrokerServiceImpl::on_channel_closed` / `on_consumer_closed` hooks called at
  every dereg site. Fans out to `federation_on_channel_closed` (drops
  `channel_to_peer_identities_`) + `band_on_role_closed`
  (`band_registry.remove_from_all` + `BAND_LEAVE_NOTIFY` to remaining members).
- `RoleStateMetrics` snapshot via `query_role_state_metrics()` exposes
  `ready_to_pending_total`, `pending_to_deregistered_total`,
  `pending_to_ready_total`. Tests assert state transitions counter-based,
  no wall-clock sleeps.
- Removed: `channel_timeout`, `channel_shutdown_grace`, JSON keys
  `broker.channel_timeout_s`, `broker.channel_shutdown_grace_s` (HubConfig
  THROWS on these obsolete keys).
- HEP-CORE-0023 §2.1 (state diagram), §2.5 (config + metrics) rewritten.
- HEP-CORE-0030 §8.2 — added "Best practice for targeted broadcasts" note
  (fire-and-forget; broker keeps membership correct via cleanup hook;
  use inbox for guaranteed delivery).

✅ **1084/1085 tests (2026-03-12).** Hub-dead ZMQ monitor path implemented; StopReason ordering restored. 1 pre-existing timing flake (`BackoffStrategyTest.ThreePhaseBackoff_Phase3_LinearSleep`) unrelated to messenger changes.

### Completed 2026-03-12: Hub-Dead ZMQ Socket Monitor

Replaced the application-level silence timer (`m_last_broker_recv_epoch_ms_`) with ZMQ socket monitor (`ZMQ_EVENT_DISCONNECTED`). Also restored two functions (`handle_command(ConnectCmd)` and `send_heartbeats()`) that were accidentally deleted by a prior automated agent. The deletions were masked by the shared library's deferred symbol resolution.

**Files changed:**
- `src/utils/ipc/messenger.cpp`: restored ConnectCmd handler (with ZMTP heartbeat sockopts + `zmq_socket_monitor()` setup); restored `send_heartbeats()`; added `process_monitor_events()` / `close_monitor()`; removed `m_last_broker_recv_epoch_ms_` update
- `src/utils/ipc/messenger_internal.hpp`: removed `m_last_broker_recv_epoch_ms_`; added `m_on_hub_dead_cb`, `m_monitor_sock_`, `m_monitor_endpoint_`; added `<cstring>`
- `src/include/utils/messenger.hpp`: replaced `hub_last_contact_ms()` with `on_hub_dead(std::function<void()>)`
- `src/scripting/python_role_host_base.hpp`: `StopReason` corrected to `Normal=0, PeerDead=1, HubDead=2, CriticalError=3`
- `src/producer/producer_api.cpp`, `src/consumer/consumer_api.cpp`, `src/processor/processor_api.cpp`: `stop_reason()` switch cases 1/2/3 restored; pybind11 docstrings updated
- `src/producer/producer_script_host.cpp`: `on_hub_dead()` wired on `out_messenger_`; deregistered in `stop_role()`
- `src/consumer/consumer_script_host.cpp`: `on_hub_dead()` wired on `in_messenger_`; deregistered in `stop_role()`
- `src/processor/processor_script_host.cpp`: `on_hub_dead()` wired on BOTH `in_messenger_` + `out_messenger_`; both deregistered in `stop_role()`
- All 3 role configs: `hub_dead_timeout_ms` field removed

**Static code review findings fixed:**
- Frame-size check in `process_monitor_events()`: `< 2` → `< 6` (ZMQ monitor frame 1 = uint16 + uint32 = 6 bytes)
- Monitor setup failure: LOGGER_WARN → LOGGER_ERROR with errno

**Known gap:** No automated tests for `on_hub_dead()` / `process_monitor_events()` (would require killing a broker mid-run). Noted as future test scenario.

---

✅ **HEP-0023 Phase 1 — `startup.wait_for_roles` implemented (2026-03-11):** `WaitForRole` struct in `startup_wait.hpp`; config parsing in all 3 role configs (uid + timeout_ms per role, exact UID matching); poll loop in all 3 script hosts before `on_init` (GIL-released 200ms polls, per-role deadline). 16 new config tests → **1078/1078 tests**.

✅ **Docs cleanup — stale actor/interval_ms references fixed (2026-03-11):** `README_DirectoryLayout.md` rewritten (role directories, UID formats); `README_testing.md` stale test binary names fixed; `HEP-0008` `interval_ms` → `target_period_ms`; `channel_access_policy.hpp` + `actor_vault.hpp` comments updated. `review_high_level.md` MEDIUM-3 ✅ FIXED.

✅ **Code review REVIEW_DataHubInbox_2026-03-09.md CLOSED (2026-03-09):** 13 actionable items fixed (CR-02 inbox thread join order, CR-03 ShmQueue checksum ordering, HR-01 atomic script_errors_, HR-02 atomic reader_, HR-03 ZMQ_RCVTIMEO caching, HR-05 GIL release in open_inbox, HR-06, MR-05, MR-08, MR-10 send_stop_ guard, LR-04 memory_order_release, LR-05 error counting, LR-07 comment, IC-04 docstring). MR-04 confirmed false positive. 975/975 tests passing.

✅ **Code reviews REVIEW_Processor_2026-03-10.md (20 items) + REVIEW_DeepStack_2026-03-10.md (16 items) CLOSED (2026-03-10):** ProcessorAPI accessors (last_seq, in_capacity, in_policy, out_capacity, out_policy, set_verify_checksum), ConsumerAPI set_verify_checksum, ProducerAPI loop_overrun_count rename, consumer metrics snapshot schema, HEP-0015/0018 corrections, README_Deployment.md rewrite. 1045/1045 tests passing.

Security fixes applied (session 1): SHM-C1, IPC-C3, SVC-C1/C2/C3, HDR-C1.
Security fix applied (session 2): IPC-H2 — `~BrokerServiceImpl()` zeros `server_secret_z85` + `cfg.server_secret_key` via `sodium_memzero`.
HEP-0022 Phase 5+6 confirmed fully implemented: `on_hub_connected/disconnected/message`, `api.notify_hub()` all wired.

✅ **848 tests (2026-03-05/06).** HEP-CORE-0021 ZMQ Endpoint Registry implemented.
Security fixes: SHM-C1 (heartbeat CAS), IPC-C3 (thread lambda), SVC-C1/C2/C3 (key zeroing), HDR-C1 (C-safety).

✅ **828 tests (2026-03-05).** HEP-CORE-0019 Metrics Plane fully implemented.
All event handlers wired, CHANNEL_NOTIFY_REQ broker relay, `notify_channel()` on all 3 APIs.
L4 integration test validation hardened: ASSERT on file existence/non-empty before content checks.

✅ **Two-tier graceful shutdown (2026-03-04):** CHANNEL_CLOSING_NOTIFY → queued FIFO event
(script handles, calls `api.stop()`). FORCE_SHUTDOWN → side-channel flag after broker grace
period expires. 6 dedicated L3 tests in `test_datahub_broker_shutdown.cpp`.
- Files: `broker_service.cpp`, `messenger.cpp`, `hub_producer.cpp`, `hub_consumer.cpp`,
  all 3 script hosts, `hub_config.cpp`, `hubshell.cpp`
- HEP-CORE-0007 §12.7 Sequence B updated with two-tier protocol diagrams

✅ **HubConfig singleton re-init fix (2026-03-04):** Added `reset_to_defaults()` in
`HubConfig::Impl::load()` so lifecycle re-initialization starts with clean state.
Fixes `TickIntervalFromJson` test ordering flake.

✅ **ZmqPollLoop refactor (2026-03-04):** Extracted shared ZMQ poll+dispatch+heartbeat
logic into `scripting::ZmqPollLoop` + `scripting::HeartbeatTracker` (header-only,
`src/scripting/zmq_poll_loop.hpp`). Each role's `run_zmq_thread_()` reduced from ~60
to ~10 lines. 18 unit tests added. See HEP-CORE-0011 §13.

✅ **Protocol gap closure (2026-03-04):** 12 new BrokerProtocolTest cases covering:
- CHECKSUM_ERROR_REPORT forwarding (NotifyOnly policy) — producer, consumer, unknown channel
- CHANNEL_CLOSING_NOTIFY delivery to ALL members (producer + 2 consumers) + channel removal
- Duplicate REG_REQ — same hash succeeds, different hash → SCHEMA_MISMATCH + ERROR_NOTIFY
- HEARTBEAT_REQ — PendingReady→Ready transition, explicit heartbeat keeps channel alive
- HELLO/BYE peer handshake — consumer_joined/left callbacks, multi-consumer independence

---

## Recent Completions — Event Handlers + Channel Notify Relay (2026-03-03)

- ✅ **Phase 1: IncomingMessage enhancement** (2026-03-03): Extended `IncomingMessage` with
  `event` (string) and `details` (nlohmann::json) fields. Updated `build_messages_list_()` in
  both base class and consumer override to emit event dicts for event messages.
  - Files: `src/scripting/role_host_core.hpp`, `src/scripting/python_role_host_base.cpp`,
           `src/consumer/consumer_script_host.cpp`

- ✅ **Phase 2: Wire all unwired event handlers** (2026-03-03): Connected all unhandled C++
  callbacks to the Python `msgs` queue as tagged dicts:
  - Producer: `on_consumer_joined`, `on_consumer_left`, `on_consumer_died`, `on_channel_error`
  - Consumer: `on_producer_message`, `on_channel_error`
  - Processor: all of the above (producer-side + consumer-side, with `source` tag)
  - Files: `src/producer/producer_script_host.cpp`, `src/consumer/consumer_script_host.cpp`,
           `src/processor/processor_script_host.cpp`

- ✅ **Phase 3: CHANNEL_NOTIFY_REQ broker relay** (2026-03-03): New fire-and-forget message
  type relayed by broker to target channel's producer as `CHANNEL_EVENT_NOTIFY`.
  - `ChannelNotifyCmd` struct + `Messenger::enqueue_channel_notify()` public API
  - Broker `handle_channel_notify_req()` — looks up target channel, forwards to producer
  - `notify_channel()` method on all 3 role APIs (`ProducerAPI`, `ConsumerAPI`, `ProcessorAPI`)
  - Python bindings: `api.notify_channel(target_channel, event, data="")`
  - Files: `src/utils/ipc/messenger_internal.hpp`, `src/include/utils/messenger.hpp`,
           `src/utils/ipc/messenger.cpp`, `src/utils/ipc/messenger_protocol.cpp`,
           `src/utils/ipc/broker_service.cpp`,
           `src/producer/producer_api.hpp/cpp`, `src/consumer/consumer_api.hpp/cpp`,
           `src/processor/processor_api.hpp/cpp`,
           `src/producer/producer_script_host.cpp`, `src/consumer/consumer_script_host.cpp`,
           `src/processor/processor_script_host.cpp`

- ✅ **Phase 5: HEP-CORE-0007 §12 documentation** (2026-03-03): Comprehensive ZMQ Control Plane
  Protocol documentation covering all message types, categories, payload specs, protocol
  sequences, script host event delivery model, and design notes on non-interference.
  - Files: `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md`,
           `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` (§7.1 table updated)

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

### ✅ Complete: Metrics Plane (HEP-CORE-0019) — 2026-03-05

**Design doc**: `docs/HEP/HEP-CORE-0019-Metrics-Plane.md`

Adds a fifth communication plane: passive SHM metrics + voluntary ZMQ reporting → broker aggregation.
19 new tests (10 protocol-level MetricsPlaneTest + 9 API-level MetricsApiTest). 828/828 pass.

- [x] Phase 1: C++ infrastructure — `report_metric()`, `report_metrics()`, `clear_custom_metrics()`,
  `snapshot_metrics_json()` on ProducerAPI, ConsumerAPI, ProcessorAPI; InProcessSpinState guard
- [x] Phase 2: Heartbeat extension — optional `metrics` JSON piggybacked on `HEARTBEAT_REQ`;
  `MetricsStore` in BrokerServiceImpl; producer+processor zmq thread passes snapshot
- [x] Phase 3: Consumer metrics — `METRICS_REPORT_REQ` (fire-and-forget) + `enqueue_metrics_report()`
  on Messenger; consumer zmq thread sends periodic reports
- [x] Phase 4: Query API — `METRICS_REQ`/`METRICS_ACK` broker dispatch; `query_metrics_json_str()`
  public API; `pylabhub.metrics()` AdminShell binding wired in hubshell.cpp
- [x] Phase 5: Python bindings — `api.report_metric()`, `api.report_metrics()`,
  `api.clear_custom_metrics()` in all three pybind11 embedded modules
- [x] Fix: `notify_channel()`/`broadcast_channel()` pybind11 `data` arg missing default → added
  `py::arg("data") = ""` and removed C++ header default per new pybind11 Default Parameter Rule
  (see `docs/IMPLEMENTATION_GUIDANCE.md` § "pybind11 Default Parameter Rule")

### Active: Layer 4 Producer + Consumer Tests (2026-03-02)

Producer and consumer binaries need Layer 4 test coverage (config parsing + CLI). Tracked in
`docs/todo/TESTING_TODO.md` § "Layer 4: pylabhub-producer Tests" + "Layer 4: pylabhub-consumer Tests".

### ✅ Complete: Integration Test (2026-03-04)

Full pipeline round-trip test: `pylabhub-hubshell` + `pylabhub-producer` + `pylabhub-processor`
+ `pylabhub-consumer` via live broker. See `tests/test_layer4_integration/test_pipeline_roundtrip.cpp`.

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

- [x] **Schema registry** — ✅ Complete (2026-03-02). All 5 phases of HEP-CORE-0016 done.
  SchemaLibrary + SchemaStore + broker protocol + script integration.

- [ ] **ZMQ data-plane runtime safety (deferred, HEP-CORE-0023)** — ZMQ channels currently
  have no data-plane safety beyond compile-time struct layout. Two gaps to close:
  1. **Per-frame checksum**: Append BLAKE2b-32 (4 bytes) to every ZMQ frame so the receiver
     can detect corruption / mismatched item_size silently introduced by a config error.
     `ZmqQueue::push()` appends the checksum; `ZmqQueue::pop()` verifies it; mismatch →
     logged + frame dropped (not crash).
  2. **Type-tag handshake**: On ZmqQueue connection, sender sends a one-time `TYPE_INFO`
     frame containing `{schema_blds, item_size}` before the data stream. Receiver compares
     against its own compiled-in values; mismatch → `LOGGER_ERROR` + connection refused.
     This mirrors the SHM `DataBlockConfig` validation at connect time.
  Design note: both features are optional flags on `ZmqQueue` (default off for raw-bytes
  compat); `ProcessorScriptHost` enables them when `out_transport="zmq"`.
  **Precondition**: ZmqQueue must expose a `type_info_t` derived from `DataBlockT` BLDS —
  requires schema BLDS to flow into ZmqQueue options (not wired today).
  Reference: HEP-CORE-0017 §4.3 "Known Limitations".

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
| Cat 2 | Checksum error report | CHANNEL_EVENT_NOTIFY | `on_channel_error` |
| App | CHANNEL_NOTIFY_REQ (any role) | CHANNEL_EVENT_NOTIFY relay | `on_channel_error` |

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
