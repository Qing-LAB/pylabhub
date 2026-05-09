# Broker test migration plan — retire BrokerHandle, adopt real HubHost

**Status:** Approved 2026-05-09, executing.
**Date:** 2026-05-09
**Driver:** Test design principle in `docs/todo/TESTING_TODO.md`
§"Test Design Principles" (no parallel-production-code in tests; real
modules at every layer).

## Answered open questions (2026-05-09)

1. **Scope:** Migrate all 8 broker test files first (full M0→M3 sweep) before
   resuming M1.2 Phases 5-8.
2. **Phase 4 disposition:** Commit Phase 4 with a tests-pending note in
   `TESTING_TODO.md`; strip the 2 diagnostic `LOGGER_INFO` traces from
   `broker_service.cpp::handle_disc_req` (gdb-investigation scaffolding,
   debug-only, redundant with existing INFO/DEBUG logs).
3. **Shutdown tests:** Delete `test_datahub_broker_shutdown.cpp` outright
   before migration (6 tests pin grace-escalation behavior that M1.3
   retires).  Fresh tests for the post-M1.3 force-disconnect notification
   semantic will be designed when M1.5 lands.
4. **In-process vs subprocess:** Per-file decision driven by what each test
   exercises — lifecycle behavior under test → subprocess (Pattern 3,
   `IsolatedProcessTest`); individual API call → in-process (Pattern 1/2,
   suite-level `LifecycleGuard`).  No new framework; use the existing
   `run_gtest_worker` / `LifecycleGuard` patterns.  Per-file pattern flips
   surfaced for user sanity-check during M2.
5. **No `make_hub_host_for_test()` helper.**  `HubHost host(cfg);` is the
   API; wrapping it in a renamed factory would just be indirection.  Tests
   construct `HubHost` directly using existing `HubConfig` API.  If config
   defaults repeat, an inline `make_test_hub_config()` function in the
   test file (or a tiny shared header) suffices — no class, no fixture.

## Execution sequencing

1. Strip the 2 diagnostic `LOGGER_INFO` traces from `broker_service.cpp`.
2. Commit Phase 4 with a `TESTING_TODO.md` tests-pending note.
3. Delete `test_datahub_broker_shutdown.cpp` outright.
4. Verify build + remaining test baseline.
5. M1 — migrate `test_datahub_broker_consumer.cpp` (5 tests, simplest)
   as the pattern template.
6. **PAUSE for user sanity-check on the M1 pattern.**
7. M2 — migrate the remaining 6 files (`test_datahub_broker.cpp`,
   `_admin`, `_health`, `_protocol`, `_request_comm`, `_schema`).  For
   currently-subprocess files: principle audit per test before pattern
   flip; surface flip decisions to user.
8. M3 — delete dead `BrokerHandle` / `LocalBrokerHandle` /
   `start_broker_in_thread` / `make_reg_opts` scaffolding.
9. Resume M1.2 Phases 5-8 production-code work.

---

## 1. Goal

Replace the 8 hand-rolled `BrokerHandle` / `LocalBrokerHandle`
HubHost-mocks in `tests/test_layer3_datahub/` with real `HubHost`-backed
test scaffolding.  Adopt `BrokerRequestComm` (the real production
wire-encoding helper) for payload construction in the same files.

After migration:

- L3 broker tests run against the **real** broker thread launched by the
  **real** `HubHost` through the **real** `ThreadManager`, with
  **real** lifecycle integration.  No hand-rolled assembly.
- Wire-protocol round-trip pattern is preserved (the broker is reachable
  via real ZMQ on a bound endpoint, just like in production).
- Test-only payload shapes are eliminated; tests pin broker behavior
  against the same wire encodings production roles emit.

---

## 2. What needs to be added (real-class observability)

Before any test migration, `HubHost` likely needs a small test-friendly
extension.  This is an extension of the **real class's observability
surface**, not a mock.

Audit (to verify against actual `hub_host.hpp` before the work starts):

- **Bound endpoint** is already exposed via `HubHost::broker_endpoint()`.
- **Broker pubkey** is already exposed via `HubHost::broker_pubkey()`.
- **Configurable random port** is already supported (cfg endpoint
  `tcp://127.0.0.1:0`).
- **Disable AdminService / HubScriptRunner** for tests that don't need
  them — `HubConfig::admin().enabled` is already toggleable.
- **Wait until ready** — `HubHost::startup()` already blocks until
  on_ready fires (5s deadline).

Probable ADDS:

- A test convenience: `HubHostFixture` or `make_hub_host_for_test(cfg)`
  that returns a configured `HubHost` with sensible defaults
  (random port, admin disabled, no script runner) — purely a constructor
  helper, not a wrapper class.  Lives in
  `tests/test_framework/hub_host_test_helpers.{h,cpp}`.

NOT mocks; `make_hub_host_for_test` only configures and returns the
real `HubHost` instance.

---

## 3. Phasing

The migration touches 8 test files + 6 worker files + ~91 individual
tests.  Phasing keeps each step's blast radius bounded.

### Phase M0 — `make_hub_host_for_test` helper

- Add `tests/test_framework/hub_host_test_helpers.{h,cpp}`.
- Returns a configured `HubHost` (real class) with: random
  `tcp://127.0.0.1:0`, no admin, no curve auth (or per-caller toggle).
- Return value is `std::unique_ptr<HubHost>` already started up — caller
  uses `host->broker_endpoint()` to point `BrokerRequestComm` /
  `raw_req` clients at the right place.
- Build a single-test smoke check that uses the helper to verify the
  bound endpoint is reachable.  This proves the helper is sound before
  we touch any existing test.

**Exit criterion:** smoke test passes; existing 1788/1788 unaffected.

### Phase M1 — migrate one file end-to-end

Pick the simplest of the 8: `test_datahub_broker_consumer.cpp` +
`workers/datahub_broker_consumer_workers.cpp`.  It has the smallest
test count (5) and the simplest scenarios (CONSUMER_REG_REQ /
CONSUMER_DEREG_REQ / DISC_REQ shape).

For each test:
1. Replace `start_broker_in_thread(cfg)` → `make_hub_host_for_test()`.
2. Replace ad-hoc `nlohmann::json reg_req; reg_req["channel_name"] = ...`
   payload construction with `BrokerRequestComm::register_channel(opts)`
   (using the real production helper).
3. Replace `BrokerHandle` struct usage with the returned `HubHost`
   handle directly.
4. Drop `stop_and_join()` — `~HubHost()` handles teardown via
   `ThreadManager::drain()`.

**Exit criterion:** all 5 tests in the migrated file pass against the
real `HubHost` shape.  Any test that breaks the migration reveals a
real semantic change — discuss with user before re-shaping the test.

### Phase M2 — migrate the remaining 7 files

Apply the same pattern.  Order:
- `test_datahub_broker.cpp` (33 tests) — biggest single migration
- `test_datahub_broker_admin.cpp` (8 tests, `LocalBrokerHandle` variant
  — uses in-process broker; need to confirm if `make_hub_host_for_test`
  covers in-process semantics)
- `test_datahub_broker_health.cpp` (5 tests)
- `test_datahub_broker_protocol.cpp` (24 tests, also `LocalBrokerHandle`)
- `test_datahub_broker_request_comm.cpp` (4 tests)
- `test_datahub_broker_schema.cpp` (7 tests, `LocalBrokerHandle`)
- `test_datahub_broker_shutdown.cpp` (6 tests, `LocalBrokerHandle`)

**Exit criterion:** all 91 broker tests pass against real `HubHost`.

### Phase M3 — delete the dead scaffolding

- Remove `struct BrokerHandle` / `struct LocalBrokerHandle`
  definitions from all 8 worker/test files.
- Remove `start_broker_in_thread()` helpers.
- Remove ad-hoc `make_reg_opts()` / `make_cons_opts()` payload
  builders that were replaced by `BrokerRequestComm`.

**Exit criterion:** `grep -rn "BrokerHandle\|LocalBrokerHandle\|start_broker_in_thread" tests/`
returns no results; build clean; full suite passes.

---

## 4. Risk areas to flag with user before starting

### 4.1 Test-only behaviors that depend on the mock's drift from real

The mock-host shape lets some tests pin behaviors that real `HubHost`
doesn't actually support.  Each of these is a **finding to discuss
with user**, not a migration mechanic:

- **REG_REQ with empty `role_uid`** (the case that broke during my
  Phase 4 work).  Real `HubHost` always populates `role_uid` (HEP-0033
  G2.2.0b).  Tests that bypass uid generation are pinning a fictional
  pre-M0 wire shape.  Either tests need to use a real `BrokerRequestComm`
  (which generates uids), or the broker needs an explicit reject path
  for empty uid.  This is the Phase 4 semantic question — bring to
  user during migration.
- **`Sch_HubGlobalsLoadedAtStartup` + `Sch_PathC_*`** tests rely on
  schema_search_dirs being loaded at startup.  Real `HubHost` does
  this via `HubConfig::schema_search_dirs()`; tests need to pass the
  dir list through real config rather than directly to the broker.
- **`HeartbeatKeying_ProducerVsConsumer_DistinctRows`** asserts on
  presence-row keying — needs real heartbeat path through real
  `BrokerRequestComm::send_heartbeat(channel, uid, role_type, ...)`.
- **`test_datahub_broker_shutdown.cpp` shutdown tests** depend on
  M1.3 not yet being done — they assert on the FORCE_SHUTDOWN /
  grace-deadline path which Phase 7 of M1.2 is going to retire.
  These tests may need to be deleted entirely (the behavior they
  test is going away) or rewritten against the new atomic-teardown
  contract.  Bring to user.

### 4.2 In-process vs subprocess pattern

Some tests (`test_datahub_broker_admin.cpp`,
`test_datahub_broker_protocol.cpp`, etc.) use `LocalBrokerHandle` for
**in-process** broker access without a subprocess worker.  This pattern
also fails the L3 audit (still mock-host scaffolding) but the
remediation differs slightly: `make_hub_host_for_test()` returns a
real `HubHost` that starts its own background thread, so in-process
tests can talk to it via `BrokerRequestComm` over real ZMQ on the
local random port.  Same shape as subprocess tests; just no fork.

### 4.3 Phase 4 holdback decision

Phase 4's production-code change (DISC_REQ derives from
producer-presence) is correct per HEP-0023 §2.2 — the test failures
are due to the mock-host scaffolding, not the production code.

Two options:

- **(a) Hold Phase 4 commit** until M1 broker-cluster migration lands.
  Cleanest history; no broken tests on any committed branch.
- **(b) Commit Phase 4** with a note in `TESTING_TODO.md` that
  `DeregPidMismatch`, `ConsumerDeregHappyPath`, `ConsumerDeregPidMismatch`
  fail under the mock-host shape and are pending the broker-cluster
  migration.  Other production-code work (M1.2 phases 5-8) can resume.

Bring to user explicitly before any commit.

---

## 5. Effort estimate

- M0 helper + smoke test: **0.5 day**.
- M1 single-file migration: **0.5 day** (used as pattern template).
- M2 remaining 7 files: **2-3 days** (some scenarios may surface real
  semantic mismatches per §4.1; each one is a stop-and-discuss with
  user, so timeline is sensitive to discovery rate).
- M3 cleanup: **0.5 day**.

**Total: ~4-5 days of focused work**, assuming no semantic surprises
trigger architectural redesigns.

---

## 6. Open questions for user (decide before starting)

1. **Scope confirmation.** Migrate all 8 broker test files at once
   (full M0→M3 sweep) before any other work, or interleave with
   resuming M1.2 Phase 4-8 production-code phases?
2. **Phase 4 holdback.** Hold (option a) or commit-with-known-bad
   (option b) per §4.3?
3. **In-scope deletions.** Confirm that
   `test_datahub_broker_shutdown.cpp` tests are slated for deletion
   when M1.3 retires FORCE_SHUTDOWN/grace, so we don't migrate
   tests we're about to delete.
4. **Subprocess vs in-process.** Tests using `LocalBrokerHandle`
   (in-process broker access) — preference for keeping them
   in-process via `make_hub_host_for_test()` returning a started
   `HubHost`, or moving them to subprocess workers for consistency
   with the other broker tests?
5. **Helper location.** OK to put `hub_host_test_helpers.{h,cpp}` in
   `tests/test_framework/`, alongside `LogCaptureFixture` and
   `IsolatedProcessTest`?

---

## 7. Out of scope (deliberately deferred)

- L2 `BrokerServiceTestAccess` friend shim and zero-network L2
  broker-class tests.  Tracked separately in `TESTING_TODO.md`
  §"Audit work" item 3.
- M1.2 Phase 5-8 production code changes (heartbeat sweep cleanup,
  legacy field deletion, FSM consolidation completion).
- M1.3 (FORCE_SHUTDOWN retirement) — depends on Phase 4-8 landing first.
- The minor 15-file payload-builder finding outside the broker
  cluster (e.g. `test_datahub_metrics.cpp`,
  `test_datahub_zmq_endpoint_registry.cpp`) — handle as a follow-up
  sweep using `BrokerRequestComm` after the broker cluster is clean.
