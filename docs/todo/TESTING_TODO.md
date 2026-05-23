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

## Current Focus — Open coverage gaps

### Recent Completions

- **N1 (#83) — config→opts translation L2 round-trip test** —
  2026-05-22, commits TBD.  Closed the systemic gap that B5 + B11
  came from.  Translation extracted into testable static methods
  `ProducerRoleHost::make_tx_opts` / `ConsumerRoleHost::make_rx_opts` /
  `ProcessorRoleHost::make_{rx,tx}_opts`.  New L2 test
  `tests/test_layer2_service/test_setup_infrastructure_translation.cpp`
  exercises the full production round-trip:
  `RoleDirectory::init_directory` → on-disk JSON → user edit → 
  `RoleConfig::load_from_directory` → `make_*_opts` → assert all
  fields.  Mutation-sweep verified (deliberately breaking the
  shared_secret copy / shm_name copy / data_transport="zmq" copy
  each triggers the test with the correct B5/B11 regression
  message).  6 tests (3 roles × 2 transports), all pass; demos
  regress clean.

  **Known-gap follow-up — folded into M9 (task #72)**: per the fresh-
  eye review 2026-05-22, three quality concerns surfaced from the
  `eb3eed36` extraction and have been incorporated into M9's expanded
  scope rather than addressed in isolation (because M9 will collapse
  the per-role static methods into shared free functions, making the
  concerns disappear mechanically or one-test-fixes-three).  Concerns:
  - **Q1**: `zmq_buffer_depth` placement inconsistency between
    Consumer's and Processor's `make_rx_opts` — Consumer sets it
    only inside `if (zmq)`, Processor sets it unconditionally.
    Resolved at M9 collapse by unified `make_rx_opts` free function.
  - **Q2**: SchemaSpec propagation test currently passes empty
    `SchemaSpec` and asserts size-of-empty-vs-empty (always passes
    even if the translation drops the copy).  Replaced at M9 by a
    test with non-empty `SchemaSpec`.
  - **Q3**: All current tests pass `has_fz=true`; the
    `flexzone_checksum = config.flexzone && has_fz` expression is
    never exercised with `has_fz=false`.  Replaced at M9 by adding
    at least one `has_fz=false` case per direction.

  Design doc: `docs/tech_draft/role_host_template_design.md` §11.6.

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
