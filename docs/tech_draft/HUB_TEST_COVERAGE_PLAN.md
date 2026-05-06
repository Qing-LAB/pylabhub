# Hub Test Coverage Plan — End-to-End Picture

**Date:** 2026-05-05
**Scope:** Audit existing hub-related test coverage (L2 / L3 / L4) and
identify the gaps that must be filled to give a fully-functional
hub + role system (per HEP-CORE-0033) verified test coverage.

This document is the planning input for the F5 audit follow-up
("System-level L4 hub-pipeline tests" in `MESSAGEHUB_TODO.md`).
Reading the file inventory + per-test-name list is the source of
truth; this doc is the synthesis.

---

## Existing coverage map

### Layer 2 — per-class unit tests

| File | Tests | Surface covered |
|---|--:|---|
| `test_hub_cli.cpp`           | 30  | `parse_hub_args` shape: --init/--validate/--keygen mode exclusion, init-only flag rejection, log-flag parsing |
| `test_hub_config.cpp`        | 10  | `HubConfig::load{,from_directory}`, defaults, JSON whitelist, base_dir resolution |
| `test_hub_directory.cpp`     | 13  | Layout creation, vault perms, `init_directory` template emit, name-validation |
| `test_hub_state.cpp`         | 68  | Capability ops (`_on_*`), accessors, snapshot, subscription dispatch |
| `test_hub_vault.cpp`         | 18  | CURVE keypair create/open, encryption, wrong-password rejection, file-perm 0600, **publish_public_key (now correctly used in production after F1)** |
| `test_hub_host.cpp`          | 15  | Construct → startup → run_main_loop → shutdown lifecycle, phase FSM single-use, FailedStartup retry, `HubStubEngine` in-process script-host |
| `test_hub_api.cpp`           | 8   | Construction + `log` level routing + `metrics` empty-host fallback **— rest of HubAPI surface untested** |
| `test_admin_service.cpp`     | 22  | Token gate, REP socket round-trip, all §11.2 query/control RPCs, deferred-method shape |
| `test_engine_factory.cpp`    | 8   | Engine selection by `script.type`, native checksum wiring, venv flag |
| `test_lua_engine.cpp`        | 115 | Per-method Lua-engine surface |
| `test_python_engine.cpp`     | 103 | Per-method Python-engine surface |

### Layer 3 — subsystem integration

| File | Tests | Surface covered |
|---|--:|---|
| `test_datahub_broker.cpp`              | 33 | Broker REG_REQ/DEREG, CONSUMER_REG, heartbeat, channel lifecycle |
| `test_datahub_broker_protocol.cpp`     | 17 | Wire protocol envelopes, error frames, sender_uid, correlation_id |
| `test_datahub_broker_admin.cpp`        | 8  | Legacy `query_metrics_json_str`, `list_channels_json_str` (kept for §10 migration period) |
| `test_datahub_broker_health.cpp`       | 5  | Liveness states (Ready/Pending), heartbeat-miss timeout |
| `test_datahub_broker_consumer.cpp`     | 5  | Consumer registration, multi-consumer broadcast |
| `test_datahub_broker_schema.cpp`       | 5  | Schema-on-channel registration, mismatch rejection |
| `test_datahub_broker_shutdown.cpp`     | 6  | CHANNEL_CLOSING_NOTIFY, FORCE_SHUTDOWN, broker.stop() ordering |
| `test_datahub_broker_request_comm.cpp` | 4  | Role-side broker-request-comm wrapper |
| `test_datahub_metrics.cpp`             | 13 | `BrokerService::query_metrics` filter shapes (categories / channels / roles / etc.) |
| `test_datahub_hub_host_integration.cpp`| 3  | HubHost + spawned broker thread: reachable, REG_REQ round-trip, shutdown breaks clients |
| `test_datahub_hub_federation.cpp`      | 3  | Inter-hub HUB_PEER_HELLO / HUB_PEER_BYE handshake |
| `test_hub_lua_integration.cpp`         | 6  | Hub script: on_init/on_stop, syntax-error, on_tick (periodic + catch-up), read accessors, request_shutdown |
| `test_hub_python_integration.cpp`      | 1  | Hub Python script (single TEST_F by design — pybind11 `scoped_interpreter` re-init unsafe within one process) |

### Layer 4 — binary integration

| File | Tests | Surface covered |
|---|--:|---|
| `test_layer4_plh_role/*` (4 files)     | 71 | plh_role no-hub tier: --init / --validate / --keygen / CLI errors, parametrized over producer/consumer/processor |
| `test_layer4_plh_hub/*` (4 files)      | 17 | plh_hub no-hub tier: --init / --validate / --keygen / CLI errors |

**Total across all hub-touching tests: ~1230.**

---

## Gaps by layer

### L2 gaps — `HubAPI` Phase 8c surface is **completely untested**

The Phase 8c (commit `3c65dfa` + follow-ups) added 7 methods to HubAPI;
ZERO have unit-level tests:

- `post_event(name, data)` — name validation (C-identifier check),
  enqueue onto `RoleHostCore` queue, fire-and-forget semantics.
- `augment_query_metrics(params, response&)` — has_callback probe,
  invoke_returning routing, return-value capture, null/error fallback.
- `augment_list_roles(response&)` — same.
- `augment_get_channel(name, response&)` — same.
- `augment_peer_message(peer_uid, msg, response&)` — same (broker-side
  wiring deferred but the HubAPI method itself ships).
- `augment_timeout_ms()` / `set_augment_timeout(ms)` — atomic
  store/load semantics, project-convention values (-1 / 0 / >0).

Plus the **pre-Phase-8c read accessors and control delegates** are
also missing L2 coverage — those landed in Phases 8a/8b but only get
exercised through the L3 integration tests, which is below the right
layer for unit testing:

- `list_channels` / `get_channel` / `list_roles` / `get_role` /
  `list_bands` / `get_band` / `list_peers` / `get_peer` / `query_metrics`
  / `config` / `name` — pre-host-wiring fallbacks (return empty
  array / null / object), happy-path delegation to host.state().
- `close_channel` / `broadcast_channel` / `request_shutdown` —
  delegate to broker.request_*, no-op pre-wire.

**L2 surface is the right layer for these.**  Each method's contract
is a thin transformation over a `host_->...` call; mocking or stubbing
the HubHost dependency lets us pin behavior without spinning up a
full broker.  These tests would have caught the F1 audit finding
(test would have asserted that `create_keypair` produces `hub.pubkey`
but had no caller — Class D log-warning gate).

### L3 gaps — Phase 8c integration is untested + role↔hub script events untested

`test_hub_lua_integration.cpp` is the natural home for hub-script
Phase 8c integration — it already has the HubLua fixture (HubHost +
real LuaEngine + real init.lua).  Missing scenarios:

- **Augmentation hook flow** — script defines `on_query_metrics`;
  test triggers the augmentation via direct HubAPI call (or admin
  RPC); verify mutated response is returned.
- **post_event / on_app_\<name\> dispatch** — script calls
  `api.post_event("foo", {x=1})`; W-thread drain fires
  `on_app_foo`; observable side effect (log emit, state mutation
  the test reads back).
- **Augmentation timeout** — script defines a slow callback,
  `set_augment_timeout(50)`, verify caller times out with
  `InvokeStatus::TimedOut`.
- **Event observers** — register a role with the broker, verify
  hub-script's `on_role_registered` fires (currently there's NO
  test that any of the 11 §12.2.1 event observers actually fire
  — the dispatch path is exercised only by `dispatch_event`
  unit-level tests, not via real broker events).
- **Control delegates from on_tick** — script calls
  `api.close_channel("X")` from `on_tick`; verify the channel is
  removed from HubState.

These should fold into `test_hub_lua_integration.cpp` (Lua side) or
extend `test_hub_python_integration.cpp` to a per-process worker
pattern (one Python TEST_F per spawned subprocess) so we can cover
the same scenarios on the Python engine without hitting the
`scoped_interpreter` re-init constraint.

### L3 gaps — federation + admin

- **Federation** has 3 tests covering the connection handshake but
  none for the `HUB_TARGETED_MSG` peer wire frame end-to-end
  (would unblock the deferred `on_peer_message` augment hook).
- **Admin RPC over the wire** — `test_admin_service.cpp` is L2
  (in-process REP socket round-trip).  An L3 test that runs the
  admin RPC against a HubHost-spawned admin thread + real ZMQ
  context would verify: token gate end-to-end, response shape over
  wire, error code marshaling, timeout behavior.

### L4 gaps — F5 deferred suite

The four `MESSAGEHUB_TODO.md` items still open:

1. **`plh_role` run-mode lifecycle (no hub)** — smallest first slice.
   Spawn `plh_role producer <dir>` (no hub configured, SHM-only or
   producer with broker disabled).  Verify: enters data loop,
   accepts SIGTERM, exits 0, no `[ERROR ]` log lines.

2. **`plh_hub` run-mode lifecycle** — symmetric to (1).  Spawn
   `plh_hub <dir>` with a minimal config (CURVE off,
   `token_required: false`).  Verify: bound endpoint reachable
   (test connects via a probe), accepts SIGTERM, exits 0.

3. **`plh_hub` + `plh_role` broker round-trip** — the F1 regression
   test.  Spawn `plh_hub <hub_dir>` (run `--keygen` first to
   produce vault + pubkey), then spawn `plh_role producer <prod_dir>`
   pointing at hub_dir via `out_hub_dir`.  Verify: producer's
   REG_REQ succeeds, hub script (if loaded) sees `on_role_registered`,
   admin RPC `list_roles` shows the producer.

4. **`plh_hub` + `plh_role` channel-broadcast** — extends (3) with
   a consumer; verify CHANNEL_BROADCAST_REQ → CHANNEL_BROADCAST_NOTIFY
   reaches the consumer.

5. **Cross-role processor pipeline** — producer → processor →
   consumer chain; data integrity end-to-end.

6. **Hub-dead detection** — kill `plh_hub` mid-flight; roles' hub-
   dead monitor wakes their cleanup path; roles exit cleanly.

7. **`plh_hub` admin RPC over wire** — spawn `plh_hub`, send a
   structured admin RPC via raw ZMQ REQ socket, parse the response.
   Verifies the §11 admin-side wire contract end-to-end.

---

## Recommended ordering

Target principle: **fill L2 gaps first, then L3, then L4.**  Lower
layers stop regressions earlier and reduce L4 surface area.

### Slice 1 — L2 HubAPI Phase 8c surface (~1 day's work)

New file `tests/test_layer2_service/test_hub_api_phase8c.cpp` (or
extend `test_hub_api.cpp` if file size allows).

Test groups:
- `post_event` — name validation (10+ name shapes), enqueue
  observable through a stub `RoleHostCore` (build a message with
  expected event="app_<name>" and details=data).
- `augment_*` — use a `MockEngine` (subclass of `ScriptEngine` with
  recorded `has_callback` / `invoke_returning` calls) wired into
  `HubAPI::set_engine`; verify happy-path replacement, null-return
  default-keep, status!=Ok default-keep, no-callback skip.
- `set_augment_timeout` / `augment_timeout_ms` — atomic round-trip,
  -1/0/>0 boundary values pass through unchanged.
- Read accessors / control delegates pre-host-wiring fallbacks
  (empty/null returns).

~30 tests.  No subprocess, no broker, fast.

### Slice 2 — L3 Phase 8c integration on Lua

Extend `test_hub_lua_integration.cpp`:

- `OnAppEvent_FromOnInit_FiresOnWorkerThread` — script's `on_init`
  calls `api.post_event("ping", {seq=1})`; script defines
  `on_app_ping(args)` that bumps a counter via shared state;
  test waits for `on_tick` to observe counter incremented.
- `Augment_QueryMetrics_AddsCustomField` — script defines
  `on_query_metrics(args)` that adds `args.response.custom = 42`
  and returns; test calls `host.hub_api()->augment_query_metrics(...)`
  directly; verifies response gained the field.
- `AugmentTimeout_LongCallback_TimesOutGracefully` — script's
  `on_query_metrics` does `os.execute("sleep 1")`; test sets
  `api.set_augment_timeout(50)`; verifies augment returns TimedOut
  and default response unchanged.
- `EventObservers_RoleRegistration_FiresOnRoleRegistered` — start
  HubHost, register a role via direct broker call, verify hub
  script's `on_role_registered` fired (script writes a flag we
  read post-hoc).
- `Control_CloseChannel_FromOnTick_RemovesChannel` — register a
  channel, then have `on_tick` call `api.close_channel("X")`,
  verify channel gone.

~5-7 tests.

### Slice 3 — L3 Python parity (per-process worker pattern)

`test_hub_python_integration.cpp` currently has 1 test by design.
Per the file header note ("pybind11 `scoped_interpreter` re-init
in one process is unsafe"), extending it requires a per-process
worker pattern (each test spawns the test binary itself in a
worker mode, runs ONE Python interpreter per worker, exits).  This
is the same pattern plh_role L4 tests use via `WorkerProcess`.
Cost: scaffold the worker dispatch + entry-point.  Benefit: every
Phase 8c Lua test gets a Python parallel.

### Slice 4 — L4 plh_role run-mode (smallest L4 first)

Concrete: `tests/test_layer4_plh_role/test_plh_role_runmode.cpp`.
Spawn `plh_role producer <dir>` against a config with broker
disabled (or pointing at a no-op embedded broker fixture).
Validate:
- Reaches "data loop entered" log line within 2s.
- Responds to SIGTERM within `kMidTimeoutMs` (5s).
- Exits 0.
- No `[ERROR ]` lines in stderr.

### Slice 5 — L4 plh_hub run-mode

Symmetric to slice 4 for `plh_hub`.  After slice 4 the test
infrastructure (signal-spawn-wait pattern) is reusable.

### Slice 6 — L4 plh_hub + plh_role broker round-trip

The F1 regression test.  Sketch:
```
1. Spawn plh_hub --init, --keygen (PYLABHUB_HUB_PASSWORD env)
2. Spawn plh_hub <hub_dir>; wait for "broker ready" log line.
3. Spawn plh_role producer <prod_dir> pointing at hub_dir.
4. Send admin RPC list_roles over a raw ZMQ REQ socket; expect
   the producer to appear within heartbeat_interval.
5. SIGTERM both; verify clean exits.
```

This is the test that would have caught F1 (no hub.pubkey ⇒ role
fails to connect with CURVE).  Currently F1 is fixed but
unverified at integration level.

### Slice 7+ — channel broadcast, processor pipeline, hub-dead

Per `MESSAGEHUB_TODO.md` items 4-7.  Each is its own file +
fixture once the slice-6 spawn pattern exists.

---

## Estimated coverage delta after the plan

| Layer | Today | After plan | Δ |
|---|--:|--:|--:|
| L2 | 1230 | ~1260 (+30 HubAPI 8c) | +30 |
| L3 | ~80 hub-related | ~95 (+5-7 Lua + parallel Python) | +12-15 |
| L4 | 88 | ~110+ (run-mode + broker round-trip + …) | +25+ |
| **Total** | ~1400 | ~1495 | +~95 |

---

## What this catches that today's coverage doesn't

- **F1 at integration level** — slice 6 forces a CURVE handshake.
- **Phase 8c regressions** — slices 1+2 catch any `augment_*` /
  `post_event` / `augment_timeout_ms` regression at the layer it
  belongs to.
- **Event observer dispatch** — slice 2 verifies the bridge from
  broker → `IncomingMessage` queue → script callback works for
  real role registrations.
- **plh_hub binary lifecycle** — slice 5 catches startup-failure
  /  shutdown-hang regressions in the binary itself (the no-hub
  L4 tier doesn't enter run mode).
- **End-to-end pipeline** — slices 6-7 verify the operator-
  documented flow works without manual integration.

---

## Out of this plan

These are noted but out of scope for "fully functional hub" pinning:

- HEP-CORE-0034 schema registry tests — separate HEP, separate
  test plan when implementation lands.
- HEP-CORE-0035 auth / federation trust tests — known_roles[] /
  federation_trust_mode are placeholders today.
- `HUB_TARGETED_ACK` wire frame + `on_peer_message_augment`
  end-to-end — gated on the wire-frame slice.
- Stress / soak tests — hub-side load testing is its own work
  track, currently no infrastructure for it.
