# Testing TODO

**Scope:** Test architecture, coverage gaps, new scenarios, regression
hardening.
**Source of truth for test pattern choices:**
`docs/README/README_testing.md` § "Choosing a test pattern".
**Authoritative test rigor + mocking + layering rules:** memory files
under `/home/qqing/.claude/projects/-home-qqing-Work-pylabhub/memory/`
(`feedback_test_rigor.md`, `feedback_test_outcome_vs_path.md`,
`feedback_no_mocks_via_observability.md`,
`feedback_test_layering_and_no_mocks.md`,
`feedback_tests_replicate_production_scenarios.md`).

---

## Test Design Principles (MANDATORY — apply to every new + reviewed test)

### Layer purpose (L1 / L2 / L3 / L4)
| Layer | Purpose | Allowed |
|---|---|---|
| **L1** | Pure-function unit | Direct function call.  No threads, no sockets, no SHM. |
| **L2** | Single class / module | Real production class instance.  Pattern 1+ (`BinaryLifecycleEnvironment`) or in-process workers; no broker. |
| **L3** | In-process integration | Real broker + role components in-process via `IsolatedProcessTest` + `SpawnWorker` (Pattern 3).  Cross-thread + cross-component contract verification. |
| **L4** | Real-binary subprocess | Drives the staged binaries (`plh_hub`, `plh_role`) via subprocess.  Verifies CLI / config-load / file-system paths.  **Data-pipeline coverage now lives in the demo framework** (`share/demo_framework/runner.py` + `share/py-demo-*/`). |

### Maximize real production modules; mocks only when absolutely necessary
Per `feedback_no_mocks_via_observability.md`: extend real classes
with observability hooks rather than writing parallel-production
scaffolding.  A mock is allowed only when it satisfies a narrow
protocol contract that the real class cannot construct cheaply in
the test environment.

### Tests pin path + timing + payload, not just outcome
Per `feedback_test_outcome_vs_path.md`: outcome-only assertions
(`EXPECT_TRUE(result.has_value())`) hide regressions where the
right outcome is reached via the wrong path.  Pin payload shape,
sequence, error_code AND outcome.  Add mutation-sweep verification
(flip the code under test in both directions; confirm the test
fails both ways).

### Tests replicate production scenarios
Per `feedback_tests_replicate_production_scenarios.md`: mirror the
real path.  No synthetic stress harnesses that diverge from how
production actually drives the code.

### No `SetUpTestSuite`-owned `LifecycleGuard` antipattern
Migration wave closed 2026-05-13 (21 files).  Two `SetUpTestSuite`
sites remain — `test_slot_view_helpers.cpp` (one-shot
`py::scoped_interpreter`) + `test_role_init_directory.cpp`
(idempotent module registration); both legitimate.  Any NEW test
must use Pattern 1+ (`BinaryLifecycleEnvironment`) or Pattern 3
(`IsolatedProcessTest` + `SpawnWorker`).

---

**Completed-work archive:** `docs/archive/transient-2026-06-05/todo-completions/TESTING_TODO_completions.md`
(Wave-B M9 RoleHostFrame Q1+Q2+Q3 closure; N1 #83 L2 round-trip test;
task #44 demo framework — inventory pointer still in this file below).

## Current Focus — Open coverage gaps

### Pattern 4 test ladder — multi-process wire-protocol coverage

Pattern 4 = subprocess-per-role + observing parent.  Canonical:
`docs/README/README_testing.md` § "Pattern 4 — ...".  The ladder
rungs each pin one HEP contract clause, with focused failure
diagnostics per rung.

- **Rung 1 — `Pattern4SmokeTest`** ✅ shipped (task #220).
  Broker CURVE bind + role CURVE connect; sabotage-verified.
  Reference impl at `tests/test_layer3_pattern4/`.
- **Rung 2 — `Pattern4RegistrationTest`** ⏳ task #221.
  Pins `REG_REQ`/`REG_ACK` wire shape + Presence FSM
  `Unregistered → RegRequestPending → Registered`.
- **Rung 3 — `Pattern4HeartbeatTest`** ✅ task #223.
  Pins four axes: REG_ACK `heartbeat_interval_ms` negotiation,
  broker first-tick observability, rate-band cadence
  (±25% on a 2 s measurement window, CI 4 s), and role-sent ↔
  broker-received symmetry.  Role-side counter via shutdown
  summary `[role.x] heartbeat counter: sent=N over Mms`;
  broker-side counter via test-fixture periodic snapshot
  thread reading `state.snapshot()` (no per-tick production log).
- **Rung 4 — `Pattern4ConsumerLifecycleTest`** ✅ task #222.
  Pins `CONSUMER_REG_REQ`/`ACK` + consumer channel
  `Standby → master_approval → Active` across 3 subprocesses
  (broker + producer with bound tx queue + consumer).  7-marker
  forward-only sequence covers consumer FSM transitions, broker
  REQ accept + ACK send, and rx-queue Standby→Configured→Active.
  Shipped 2026-06-15 (see commit log) — closed alongside the
  Z85PublicKey lib bug fix (#231) the test surfaced.
- **Rung 5 — `Pattern4ProducerLifecycleTest`** ⏳ blocked on
  task #162 (AUTH-2).  Producer PUSH channel `Standby→Active`.
- **Rung 6 — `Pattern4DataFlowTest`** ⏳ blocked on task #163
  (AUTH-3).  `Authorized` + data-loop guard + payload.
- **Rung 7 — `Pattern4DeregistrationTest`** ⏳ task #224.
  Pins `DEREG_REQ`/`ACK`, `CONSUMER_DEREG_REQ`/`ACK`,
  `DISC_REQ`/`ACK`/`PENDING` (HEP-0023 §2.2).
- **Rung 8 — `Pattern4ChannelNotifiesTest`** ⏳ task #225.
  Pins fire-and-forget notify family (`CHANNEL_CLOSING_NOTIFY`,
  `CHANNEL_ERROR_NOTIFY`, `CONSUMER_DIED_NOTIFY`,
  `CHANNEL_EVENT_NOTIFY`, `CHANNEL_PRODUCERS_CHANGED_NOTIFY`).
- **Rung 9 — `Pattern4RegistrationErrorTest`** ⏳ task #226.
  Pins REG_REQ rejection paths (bad pubkey, bad uid claim,
  length violation; HEP-0036 §I10 / §I1 negative cases).
- **Rung 10 — `Pattern4AuthUpdateTest`** ⏳ task #227, depends
  on rung 5.  Pins `CHANNEL_AUTH_CHANGED_NOTIFY` →
  `GET_CHANNEL_AUTH_REQ`/`ACK` (HEP-0036 §6.5).
- **Rung 11 — `Pattern4BandsTest`** ⏳ task #228.  Pins
  `BAND_*_REQ`/`ACK`/`NOTIFY` family (HEP-0033 §12).
- **Rung 12 — `Pattern4RoleIntrospectionTest`** ⏳ task #229.
  Pins `ROLE_PRESENCE_REQ`/`ACK`, `ROLE_INFO_REQ`/`ACK`,
  `SHM_BLOCK_QUERY_REQ`/`ACK`.
- **Rung 13 — `Pattern4HeartbeatTimeoutTest`** ⏳ deferred.
  Pins HEP-0023 §2 dead-detection + recovery FSM.  Needs
  back-channel pause/resume signal in Pattern 4 helpers.

**Off-ladder** until blockers clear: federation
(`HUB_PEER_HELLO_ACK`, `HUB_RELAY_MSG`, `HUB_TARGETED_MSG` —
tasks #75 + #105); reserved/undecided (`SCHEMA_REQ`/`ACK`,
`METRICS_REQ`/`ACK` — task #95).  `ENDPOINT_UPDATE_REQ`/`ACK`
has L3 coverage from #90–#92; Pattern-4 rung only if a
multi-process gap surfaces.

Verification floor (mandatory for every rung): four axes
(sequence + timing + payload + state) + mutation discipline.
Logging discipline: INFO is one-shot only; hot paths get
counters or consequence-pinning.

### Test-faithfulness lesson from #270 layer pivot (2026-06-25)

The HEP-0041 1i-coverage (#270) effort initially shipped an L2 test
that subclassed `RoleHostFrame` with a `RoleHostFrameTestShim`
exposing protected `prepare_tx_capability_` + `spawn_shm_auth_listener_`
+ `cleanup_tx_capability_` as one-line public forwards (commits
`8716d91a` + `31b1d2af`).  Designer review surfaced the smell: the
shim's `worker_main_` override mirrored production's
`teardown_infrastructure_` ordering, making it a **parallel production
scaffold** rather than a test of production.  Reverted same day; #270
folded into #258 L4 e2e where real `plh_hub` + `plh_role` binaries
exercise the same methods in their real production context.

Generalisable rule (candidate addition to README_testing.md §
"Mocking discipline"):

> When the methods you want to test are setup-step contributors to a
> multi-process flow — not unit-testable behaviors in isolation —
> the natural test unit IS the multi-process scenario, not the
> individual method.  Building a thin subclass with one-line forwards
> "just to invoke the methods" is a parallel production scaffold,
> even when each forward is mechanically trivial.  The scaffold's
> own lifecycle (worker_main_, teardown ordering, fake api
> construction) becomes test code with no production analogue —
> precisely what test-faithfulness §2 says to avoid.  Test at the
> layer where the methods actually do meaningful work.

Promotion path: when a second instance of this antipattern is
identified, fold the rule into README_testing.md § "Mocking
discipline" with both incidents as precedent.

### B8 (#81) — Demo-setup numpy pin

Demos that use numpy currently rely on ad-hoc `pip install numpy`.
Add `requirements.txt` per demo dir + chain
`plh_pyenv install --requirements <demo>/requirements.txt` into
`setup_commands`.

### N8 + N9 (#88) — Bench variants

- **N8** scalar / no-fill bench — pure dispatch overhead measurement.
  Current 4 KB-with-random bench measures fill cost as much as
  dispatch cost.  Strip random-fill to isolate per-slot framework
  cost (acquire + commit + checksum + log-flag check) across engines.
- **N9** multi-slot-size sweep — rerun the three-engine bench at
  slot sizes 16 B / 4 KB / 64 KB / 1 MB to characterize per-slot-
  size cost curve.  Reveals where checksum dominates vs script
  work.

### N11 (#89) — Cross-engine on_band_message signature parity audit

`Native` signature was wrong in my bench plugin this session
(used 3 separate `const char *` args; correct is
`const plh_band_message_args_t *args`).  Audit Python + Lua signatures
to confirm they're documented unambiguously and pinned by tests.
Add explicit doc note + L2/L3 regression that fails if the engine-
side dispatch signature changes.

### #93 — PlhRoleInitTest.InitOutputValidates/producer 60s hang

Flake-rate 1/N on CI: validate subprocess runs 60s without exiting,
parent kills with SIGTERM (143).  Last visible stderr line is
"Switching log sink to: RotatingFile..." — subsequent logs go to
the per-role file which the test harness can't read post-failure.

Hang is in worker thread's `worker_main_` (parent blocks on
`EngineHost::startup_()`'s `ready_future.get()`, `engine_host.cpp:148`).
Producer's validate-path skips `setup_infrastructure_` (no broker
connection) so this is NOT related to the 2026-05-21 ENDPOINT_UPDATE
sync REQ/REP change.

Candidate hang sites:
1. `scripting::engine_lifecycle_startup` — Python script load.  Slow
   imports?  GIL handoff bug between main (Py_InitializeFromConfig)
   and worker (PythonGilLease)?
2. `engine.finalize()` in validate-only exit path.
3. `cli::get_password` blocking on stdin (no TTY) — unlikely; init
   template generates `auth.keyfile = ""` and the unlock path is
   gated on non-empty.

**Cheap first-step diagnostic** — instrument the validate path: add
a log line at each major step entry/exit in
`producer_role_host.cpp::worker_main_` (Step 0 engine ctor / Step 1
schema / Step 2a api wiring / Step 4 engine_lifecycle_startup
entry+exit / engine.finalize() entry+exit).  The next CI flake will
pinpoint which step hangs.  ~10 LOC; risk-free.

Effort: M (diagnosis + likely fix).

### Open coverage items from 2026-05-20 discovery + 2026-05-05 HubAPI coverage plan (migrated 2026-06-02)

Carried over from archived `DISCOVERY_2026-05-20.md` §3.3 and
`HUB_TEST_COVERAGE_PLAN.md` (both under
`docs/archive/transient-2026-06-02/`).  Verify each before adding;
some may have been closed by Wave-A.5 / band-authority / HEP-0039
test work already.

**Broker / band protocol gate-mutation tests** (typo in
`expected_tags` initializers compiles silently today; one focused
commit can cover these):
- DEREG_REQ — expects `{prod, proc}`.
- CONSUMER_DEREG_REQ — expects `{cons, proc}`.
- HEARTBEAT_REQ — tag derived from `role_type`; mismatch path not pinned.
- ROLE_INFO_REQ + ROLE_PRESENCE_REQ — `{prod, cons, proc}`.
- BAND_JOIN_REQ + BAND_LEAVE_REQ + BAND_BROADCAST_REQ — `{prod, cons, proc}`.

**S4 broker-side band tests** (3 missing):
- `BAND_BROADCAST_REQ` sender-not-member drop (`broker_service.cpp:4685-4697`).
- `BAND_LEAVE_REQ` `NOT_A_MEMBER` typed-error path (`broker_service.cpp:4605-4622`).
- `BAND_BROADCAST` band-doesn't-exist drop (`broker_service.cpp:4677-4684`).

**S4 role-side bookkeeping tests** (3 missing):
- `band_join` on `{status:error}` → erase index entry (`role_api_base.cpp:1551-1560`).
- `band_leave` on `{status:error}` → erase index entry (`role_api_base.cpp:1597-1605`).
- `mark_connection_disconnected` band_index_ sweep returning `bands_lost` (`role_handler.cpp:337-349`).

**L3 / engine-parity gaps:**
- L3 hub-dead → `on_band_lost` cascade end-to-end (`role_api_base.cpp:1166-1176`).
- Real-engine parity tests for the 4 band callbacks (Python / Lua / Native) and `on_hub_dead`.
- `api.is_in_band()` — untested at any layer.
- **Cross-role observation surface parity test** (task #232) — per
  HEP-CORE-0011 §"Cross-Engine Surface Parity" Read-only observation
  surface principle (added 2026-06-15): each engine MUST keep one
  parity test that exercises every observation surface
  (`allowed_peers`, `producers`, `band_members`, `is_channel_ready`,
  `queue_mechanism`, etc.) on every role kind and pins both the
  wrong-side sentinel shape AND the right-side empty-but-correct
  shape.  Without this, new surfaces silently degrade the principle
  — Native ABI missing `producers` after AUTH-1 was the inciting
  incident.  When a new row is added to the HEP-0011 surface table,
  the parity tests must extend to cover it.

**HubAPI Phase 8c L2 surface — completely untested** (from 2026-05-05
coverage plan; pre-host-wiring fallbacks + happy-path delegation):
- `post_event(name, data)` — name validation + enqueue + fire-and-forget.
- `augment_query_metrics` / `augment_list_roles` / `augment_get_channel` /
  `augment_peer_message` — `has_callback` probe + `invoke_returning`
  routing + return-value capture + null/error fallback.
- `augment_timeout_ms()` / `set_augment_timeout(ms)` — atomic load/store
  + project-convention values (-1 / 0 / >0).
- Phases 8a/8b read accessors + control delegates — currently only
  exercised through L3 integration tests; should pin at L2.

**Hub Lua/Python integration gaps** (extend
`test_hub_lua_integration.cpp` + `test_hub_python_integration.cpp`):
- Augmentation hook flow (`on_query_metrics` mutates response).
- `post_event` / `on_app_<name>` dispatch (W-thread drain).
- Augmentation timeout path (slow callback + `set_augment_timeout(50)`).
- Event observers — none of the 11 §12.2.1 observers have an
  end-to-end fire-from-real-broker test.
- Control delegates from `on_tick` (e.g. `api.close_channel`).

**L3 federation + admin-RPC-over-wire:**
- `HUB_TARGETED_MSG` peer wire frame end-to-end (unblocks deferred
  `on_peer_message` augment hook).
- L3 admin-RPC-over-wire (token gate, response shape, error
  marshaling, timeout) — currently L2 (in-process REP) only.

**L4 lifecycle gaps:** several of the 2026-05-05 plan's L4 items
(`plh_role`/`plh_hub` run-mode lifecycle, broker round-trip, channel
broadcast, processor pipeline, hub-dead detection, admin-RPC-over-wire)
are subsumed by the demo framework + 9 demo manifests under
`share/demo_framework/` (task #44 — closed 2026-05-26).  Audit
which scenarios are NOT exercised by demo manifests; pin those.

### Code-review-deferred items from earlier sweeps

- **2026-05-03 `PlhRoleCliTest.LogBackupsBelowSentinelRejectedByValidate`
  parallel-load flake** — open, low priority; framework concern,
  not the test itself.
- **Native engine — user-settable `supports_multi_state()`**
  (2026-04-20) — open feature for native plugin opt-in to
  multi-state mode (script-spawned worker threads).

### Phase D — high-load and edge cases (long-running)

Long-running edge cases that aren't blocking but should land
eventually.  Items tracked under HEP-CORE-0033 phase scope; see
`docs/code_review/REVIEW_TestAudit_2026-05-01.md` § Phase D for
the resume bookmark.

---

## Test infrastructure inventory

### Demo framework (`share/demo_framework/runner.py`) — closes harness task #44

Delivers the L4 data-pipeline coverage that earlier plans tracked as
"L4 processor + consumer test infrastructure (Wave-D)".  Provides a
manifest-driven harness with built-in evaluators (`all_processes_
started`, `log_marker_present`, `log_marker_min_count`,
`no_unexpected_errors`, `clean_shutdown`, `count_sequence_e2e`).
Demo dirs under `share/py-demo-*/`:

* Python single + dual hub × SHM + ZMQ (4 demos)
* Lua single-hub SHM (`py-demo-lua-single-hub`)
* Native single-hub SHM (`py-demo-native-single-hub`)
* Hub-side Native (`py-demo-hub-native`) — validates the
  hub_script_runner B12+B13 fixes end-to-end
* Three-engine throughput bench (`py-demo-bench-{python,lua,native}`)

Use these as the L4 reference for any new pipeline scenario; clone
+ tweak rather than re-rolling the launcher.

### Multi-process test framework — `IsolatedProcessTest` + `SpawnWorker`

Pattern 3 mechanics canonical in
`docs/README/README_testing.md` § "Choosing a test pattern".
Pattern 4 (multi-process wire-protocol; subprocess-per-role +
observing parent) is the extension introduced for tests that
require honoring the HEP-CORE-0036 §7.4 single-pumper-per-process
invariant; helpers live in
`tests/test_framework/pattern4_helpers.{h,cpp}` and the smoke
reference at `tests/test_layer3_pattern4/test_pattern4_smoke.cpp`.

### Platform / sanitizer coverage

Tracked in `docs/todo/PLATFORM_TODO.md`.

---

## Phase checklist (high-level)

- **Phase A — Protocol / API correctness** ✅
- **Phase B — Slot Protocol (single process)** ✅
- **Phase C — Integration (multi-process)** ✅ (demo framework
  covers the long tail)
- **Phase D — High-load + edge cases** ⏳ ongoing

---

## Related Work

- `docs/README/README_testing.md` — test architecture + pattern
  selection.
- `docs/IMPLEMENTATION_GUIDANCE.md` § Test Strategy.
- `docs/code_review/REVIEW_TestAudit_2026-05-01.md` — resume
  bookmark for the test-correctness audit.
- `share/demo_framework/runner.py` — L4 demo-driven coverage.
