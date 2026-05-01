# REVIEW — Test-correctness audit (full codebase)

| Property        | Value                                                                |
|-----------------|----------------------------------------------------------------------|
| **Status**      | 🟡 OPEN — ground-truth tracker; multiple sessions of work            |
| **Created**     | 2026-05-01                                                           |
| **Trigger**     | Two silent-failure regressions shipped on this branch (slow-path `EXPECT_THROW`, envelope-only `status==ok`); a freshly-written catch swallowed an exception via dead "log only" comment.  User: "you motherfucker don't build houses on sand."  Audit covers every test under `tests/`. |
| **Scope**       | Every test file under `tests/`.  No file exempt.                     |
| **Disposition** | Transient — archive ONLY when every item in §6 is verdict'd, every §7 item is ✅ FIXED-with-commit-hash, and the suite-wide acceptance criteria in §3 are met. |
| **Owner**       | Whoever picks this up next.  This file is the handoff.               |

---

> **READ FIRST IF YOU ARE PICKING THIS UP**
>
> 1. Read §0 and §1 to understand the three bug classes and the
>    verification protocol that gates a `✅ FIXED` claim.
> 2. Read §3 (acceptance criteria) so you know what "done" actually
>    means before you start.
> 3. Pick the lowest-numbered open item in §6 / §7 you can finish.
>    Do NOT skip ahead.
> 4. Each fix is a single commit.  Update the row in §6 / §7 with
>    the commit hash in the SAME commit that fixes the file.
> 5. NEVER mark a row ✅ FIXED unless you ran the verification
>    protocol in §1.4 and it produced the expected red→green
>    transition.
> 6. If a fix reveals a real production bug (e.g. the LifecycleGuard
>    teardown race that LogCaptureFixture surfaced — see §4.2): file
>    it, do NOT paper over.  See §1.5 ("No silent papering").

---

## 0. Recap — three classes of test-correctness bugs

These are the failure modes this audit screens for, each with at
least one shipped incident on this branch:

| Class | Pattern | Why bad | Documented incident |
|-------|---------|---------|---------------------|
| **A**: Outcome-only assertions | `EXPECT_THROW(f(), std::exception)`, `EXPECT_EQ(status, "ok")`, `EXPECT_NO_THROW(f())` | Silent on wrong path | `HubHostTest.Startup_FailsCleanlyOnBusyPort` and `FailedStartupAllowsRetry` passed weeks while broker took the 5 s ready-timeout slow path; `AdminServiceTest.HubHost_AdminEnabled_RoundTripWorks` passed when ping handler returned `pong=false` (verified via mutation 2026-05-01) |
| **B**: Timing-by-sleep ordering | `std::this_thread::sleep_for(N ms)` followed by an assertion that depends on a state change happening within that window | Silent on slow regression; failures masquerade as correctness errors | `test_datahub_zmq_poll_loop.cpp` (multiple sites — see §6) |
| **C**: Discarded timeout-bearing return values | `c->send(1500ms)` returns a status byte; test discards it | Silent on operation timeout; manifests as confusing downstream failures | `test_datahub_hub_inbox_queue.cpp` lines 229, 390, 424, 462, 498, 531 (6 unchecked vs 3 checked at lines 100, 161, 307) |
| **D**: Missing log-noise gate | Test asserts outcome; production code logs WARN/ERROR on a fallback path; test passes despite the warning | Same silent-failure mode as Class A applied at the log boundary | The HubHost broker exception silent-swallow that `// surface via logs only` comment claimed but didn't (commit `5e472c6`) — no test caught it because no test asserted "no warnings emitted" |

Authoritative policy: `docs/IMPLEMENTATION_GUIDANCE.md` § "Assertion
Design — silent-failure prevention" and `CLAUDE.md` § "Testing
Practice (Mandatory)".

---

## 1. Verification protocol — what gates a ✅ FIXED claim

A test fix without a sensitivity verification is not a fix.  Every
row in §6 / §7 must satisfy this protocol before being marked ✅.

### 1.1 Class A fix — sensitivity by mutation

For every assertion that gates a contract:

1. Make the fix (path-pinned exception type+message, structural
   payload check, timing bound).
2. Build + run the test — must pass green.
3. **Mutation step**: deliberately break the production code that
   the assertion is supposed to gate (e.g. flip a return value,
   change a code path, return a stub).
4. Build + run the test — must FAIL with a message that points at
   the assertion.
5. Restore the production code.
6. Build + run — green again.
7. Record in §6 / §7: commit hash + `mutation: <one-line description>
   produced expected red`.

If step 4 does not fail, the assertion is shaped wrong — fix the
assertion before claiming ✅.

### 1.2 Class B fix — convert sleep to condition wait

For every `sleep_for` that orders concurrent operations:

1. Replace with `poll_until(predicate, timeout_ms)` from
   `tests/test_framework/test_sync_utils.h`.
2. Pick a deadline ≥ 10× the expected wait and assert
   `ASSERT_TRUE(poll_until(...))` with a failure message that
   names the predicate.
3. **Mutation step**: change the production code so the predicate
   becomes false (e.g. comment out the dispatch).  Test must FAIL
   on the `ASSERT_TRUE` with the named predicate, not on a
   downstream assertion.
4. Restore.  Test must pass green.
5. Record in §6 / §7 with mutation summary.

ZMQ TCP-establishment sleeps (post-`bind`/`connect`, pre-traffic)
are explicitly acceptable per the rule in
`feedback_test_correctness_principle.md`.  Mark such rows
**🟢 ACCEPTABLE** with a one-line note (no fix needed).

### 1.3 Class C fix — assert timeout-bearing return values

For every operation with a timeout that returns a status:

1. Capture the return value: `auto rc = op(timeout);`
2. Assert the success code: `ASSERT_EQ(rc, kOk) << "op timed out at <site>";`
3. **Mutation step**: shorten the timeout to ~0 (forces a timeout).
   Test must FAIL on the new ASSERT, not on a downstream
   assertion or a hung wait.
4. Restore the original timeout.  Test must pass green.
5. Record in §6 / §7.

### 1.4 Class D fix — LogCaptureFixture rollout per fixture

For every test fixture being retrofitted:

1. Have the fixture inherit `pylabhub::tests::LogCaptureFixture`.
2. In `SetUp`: `LogCaptureFixture::Install();`
3. In `TearDown`: `AssertNoUnexpectedLogWarnError(); LogCaptureFixture::Uninstall();`
4. Build + run — every test in the fixture must pass green.
5. **Mutation step (once per fixture, not per test)**: temporarily
   add `LOGGER_WARN("MUTATION: stray warn for sensitivity");` into
   the production code path that the fixture exercises.  At least
   one test must FAIL with the captured stray warning reported.
6. Restore.  Test suite green.
7. Record in §7.4 with mutation summary and the LogCaptureFixture
   commit hash.

If a real warning surfaces (not a mutation), follow §1.5.

### 1.5 No silent papering

When a fix surfaces a previously-hidden production bug (a real
warning the LogCaptureFixture didn't expect, a real timeout the
unchecked return-value would have hidden, a real wrong-path
exception that the message check now distinguishes):

1. Do NOT mark the surfaced bug as `Expect…` to make the test
   green.  That is exactly the silent-papering this audit exists
   to prevent.
2. File a tracking entry in §8 ("Real bugs surfaced") with file:line,
   reproduction recipe, and a triage note (`bug` / `intentional
   teardown noise` / `pre-existing race`).
3. Either:
   - **Fix the production bug** in the same commit if scope-bounded.
   - **Declare the warning expected** with a back-reference comment
     `// see REVIEW_TestAudit_2026-05-01.md §8 row Nxx — tracking
     fix in <issue/commit>` if scope is too large.  The §8 row
     must list the planned fix.
4. The §6 / §7 row stays 🟡 OPEN until §8 row is closed.

---

## 2. Status snapshot — what is verified-fixed today (2026-05-01)

This is the honest list.  Anything not in this section is unverified.

| Area | Status | Commit | Verification |
|---|---|---|---|
| Production catches in `src/` (226 catch blocks across 51 files) | ✅ swept | `f376cb3` | Manual read of every catch; logs added where silent; intentional swallows commented |
| `HubHostTest.Startup_FailsCleanlyOnBusyPort` (Class A) | ✅ FIXED | `6f54322` | Mutation: removed broker fail-fast wrapper → test failed at 5 s timeout assertion; restored → pass at 0.11 s |
| `HubHostTest.FailedStartupAllowsRetry` (Class A) | ✅ FIXED | `6f54322` | Same mutation as above → test failed; restored → pass at 0.12 s |
| `HubHostTest.StartupAfterShutdown_Throws` (Class A) | ✅ FIXED | `6f54322` | Message-substring pin added; type was already path-discriminating |
| `AdminServiceTest.HubHost_AdminEnabled_RoundTripWorks` (Class A) | ✅ FIXED | `6f54322` + `59dbef6` | Mutation: ping handler returned `pong=false`/`echo="wrong-echo"` → test failed (envelope check tightened); restored → pass |
| `AdminServiceTest.Run_PingRoundTrip_TokenGate` (Class A) | ✅ verified | (already correct) | Mutation: same as above → test failed; tightened structural payload check |
| `AdminServiceTest.Construct_TokenOff_NonLoopbackEndpoint_Throws` (Class A) | ✅ FIXED | `6f54322` | Two ctor invariants both throw `std::invalid_argument`; pinned distinct message substrings ("loopback" vs "admin_token is empty") |
| `AdminServiceTest.Construct_TokenOn_EmptyToken_Throws` (Class A) | ✅ FIXED | `6f54322` | Same as above |
| Broker bind-error fail-fast — production fix that the tests above were verifying | ✅ FIXED | `59dbef6` | Wrapped `broker.run()` in try/catch inside spawn lambda; forwards exception via `ready_promise->set_exception()`.  Two formerly-5 s tests dropped to 0.1 s |
| Broker post-startup death silent-swallow (catch-block) | ✅ FIXED | `5e472c6` | Inner `catch (std::future_error)` now logs the original broker exception via `LOGGER_ERROR` |
| Class D rule documented; LogCaptureFixture infrastructure shipped | ✅ verified | `db9f8f9` | Mutation: stray `LOGGER_WARN` in `AdminService::run()` → 2 admin tests failed with the captured warning; restored → 10/10 pass at 0.55 s wall |
| `LogCaptureFixture` applied to `AdminServiceTest` (Class D rollout, 1 of N fixtures) | ✅ FIXED | `db9f8f9` | Sensitivity verified above |

**Everything else is OPEN.  See §6 and §7.**

---

## 3. Acceptance criteria — when can this audit close?

Archive this document to `docs/archive/transient-YYYY-MM-DD/` ONLY
when ALL of these hold:

1. Every row in §6 (Class A/B/C census) is either ✅ FIXED with
   commit hash + mutation summary, or 🟢 ACCEPTABLE with one-line
   justification.
2. Every fixture in §7 (LogCaptureFixture rollout) is ✅ FIXED.
3. Every row in §8 (real bugs surfaced) is closed with a
   production fix or has a tracked deferral with explicit owner +
   ETA in `docs/TODO_MASTER.md`.
4. Full suite passes in CI configuration (build + ctest -j2 with
   no unexpected warnings, no test taking >2 s except the
   intentional stress-test set documented in §9).
5. Lasting findings (failure-mode catalog entries, helper APIs,
   fixture patterns) merged into
   `docs/IMPLEMENTATION_GUIDANCE.md` § "Assertion Design —
   silent-failure prevention" so they survive after this transient
   doc is archived.

Until ALL five hold, the doc stays 🟡 OPEN.  Partial completion is
not "closed".

---

## 4. Methodology — how to actually audit a file

This is the per-file recipe.  Follow it; do not improvise.

### 4.1 Setup

1. Read the production code that the test exercises.  List the
   contracts the test should be gating: error paths, boundaries,
   sequence integrity, cross-field consistency, fast-fail
   guarantees.  This list is the rubric for §4.3.
2. Find the existing test in the file with the most rigorous
   assertions; use it as the depth-of-verification reference.
   New / fixed assertions in this file must match or exceed it.

### 4.2 Class A scan

For every `EXPECT_THROW`, `EXPECT_NO_THROW`, `EXPECT_EQ`,
`EXPECT_TRUE`, `ASSERT_*` in the file:

1. Identify which contract from §4.1 it claims to gate.
2. Ask: "If the production code returned the right outcome via the
   wrong path, would this assertion fail?"  If no → fix per §1.1.
3. Ask: "If the production code became 100× slower, would this
   assertion fail?"  If no AND speed is part of the contract → fix
   per §1.2 (timing bound).
4. Ask: "If a structured response had the right envelope but wrong
   payload, would this assertion fail?"  If no → fix per the
   payload rule.
5. After every fix: run §1.1 mutation sweep.

### 4.3 Class B scan

For every `sleep_for`, `usleep`, `nanosleep` in the file:

1. Is it post-bind/connect for a ZMQ socket, before the first
   traffic?  → 🟢 ACCEPTABLE per the existing rule.  Annotate the
   row in §6 with `category: zmq-tcp-establishment`.
2. Is it followed (within a few lines) by an assertion on a state
   change that the production code is supposed to drive?  → bad
   pattern; fix per §1.2.
3. Is it followed by an action that *requires* the wait to have
   happened (e.g. send to a not-yet-ready peer)?  → bad pattern;
   replace with a positive readiness signal (e.g.
   `wait_for_ready_event`, `poll_until_connected`).
4. After every fix: run §1.2 mutation sweep.

### 4.4 Class C scan

For every call to a function with a timeout-bearing parameter
(`recv`, `send`, `try_lock_for`, `wait_for`, `try_acquire_for`,
`wait_until`, ...):

1. Is the return value captured?  If no → fix per §1.3.
2. Is the captured value asserted to be the success code (not just
   used for downstream logic)?  If no → fix per §1.3.
3. Common timeout signal codes (per project conventions):
   - `c->send(...)` returns 0 on ack=0; 255 on timeout; other
     non-zero on remote ack codes.
   - `try_lock_for(...)` returns false on timeout.
   - `wait_for(...)` returns `std::future_status::timeout` on timeout.
   - Lock acquire helpers — see file-by-file.
4. After every fix: run §1.3 mutation sweep.

### 4.5 Class D scan

If the fixture does not yet inherit `LogCaptureFixture`:

1. Add inheritance + `SetUp` / `TearDown` hooks per §1.4.
2. Build + run.  Every captured WARN/ERROR is either:
   - A test-driven warning → declare via `ExpectLogWarn(...)` in
     the test that drives it.  Do NOT declare it at fixture scope.
   - A pre-existing production warning → §1.5 (file in §8).
3. After fixture-wide rollout: run §1.4 mutation sweep.

### 4.6 Per-file commit format

Each file's audit closes with one commit:

```
test(<file_basename>): silent-failure audit — <classes touched> + LogCapture

<one-paragraph summary of what was found and what was changed>

Class A:
  + <count> assertions tightened (path/timing/payload)
  - mutation: <description> -> expected red transition observed

Class B:
  + <count> sleeps converted to poll_until
  - <count> sleeps annotated as ZMQ-TCP-establishment (acceptable)
  - mutation: <description> -> expected red transition observed

Class C:
  + <count> timeout-bearing returns now asserted
  - mutation: <description> -> expected red transition observed

Class D:
  + LogCaptureFixture installed
  - mutation: stray LOGGER_WARN in <production path> -> failed N tests

REVIEW_TestAudit row(s) updated to ✅ FIXED with this commit hash.

Co-Authored-By: ...
```

The commit body and the §6/§7 row update happen TOGETHER.  Never
mark a row ✅ in advance; never leave a fixed row 🟡.

---

## 5. Sleep_for full census (snapshot 2026-05-01)

Total: **164 occurrences across 35 files** (excluding `test_framework/`).

This is the COMPLETE list, file-by-file, sorted by count.  Every
file gets a row in §6.B; the count here is the inventory.

| # | File | Count |
|---|------|-------|
| 1 | `tests/test_layer3_datahub/test_datahub_hub_zmq_queue.cpp` | 33 |
| 2 | `tests/test_layer3_datahub/test_datahub_zmq_poll_loop.cpp` | 11 |
| 3 | `tests/test_layer3_datahub/test_datahub_hub_inbox_queue.cpp` | 10 |
| 4 | `tests/test_layer3_datahub/workers/role_api_loop_policy_workers.cpp` | 9 |
| 5 | `tests/test_layer3_datahub/test_datahub_metrics.cpp` | 9 |
| 6 | `tests/test_layer3_datahub/workers/datahub_broker_health_workers.cpp` | 8 |
| 7 | `tests/test_layer2_service/workers/role_data_loop_workers.cpp` | 8 |
| 8 | `tests/test_layer3_datahub/workers/datahub_mutex_workers.cpp` | 7 |
| 9 | `tests/test_layer3_datahub/workers/datahub_channel_group_workers.cpp` | 6 |
| 10 | `tests/test_layer3_datahub/test_datahub_hub_monitored_queue.cpp` | 6 |
| 11 | `tests/test_layer3_datahub/workers/datahub_stress_raii_workers.cpp` | 5 |
| 12 | `tests/test_layer2_service/workers/logger_workers.cpp` | 5 |
| 13 | `tests/test_layer2_service/workers/filelock_workers.cpp` | 5 |
| 14 | `tests/test_layer2_service/workers/jsonconfig_workers.cpp` | 4 |
| 15 | `tests/test_layer2_service/test_shared_memory_spinlock.cpp` | 4 |
| 16 | `tests/test_layer3_datahub/workers/datahub_c_api_draining_workers.cpp` | 3 |
| 17 | `tests/test_layer3_datahub/workers/datahub_broker_workers.cpp` | 3 |
| 18 | `tests/test_layer3_datahub/workers/datahub_broker_consumer_workers.cpp` | 3 |
| 19 | `tests/test_layer4_integration/test_admin_shell.cpp` | 2 |
| 20 | `tests/test_layer3_datahub/workers/role_api_raii_workers.cpp` | 2 |
| 21 | (15 more files at 1–2 each — listed in §6.B) | ~13 |

Re-run `grep -rln "sleep_for\|::sleep\|usleep" tests --include='*.cpp' --include='*.h' | grep -v test_framework | xargs -I{} sh -c 'echo "$(grep -c "sleep_for\|::sleep\|usleep" {}) {}"' | sort -rn`
to refresh.

---

## 6. Per-file audit checklist

Mark each row with the verdict from §1 once verified.  Status legend:
🟡 OPEN | 🟢 ACCEPTABLE (no fix needed) | ✅ FIXED (commit-hash) | ⚠ PARTIAL.

Each row has a verification sub-row that must reference the commit
that ran the §1 mutation sweep.

### 6.A — Layer 2 service tests

| # | File | Class A | Class B | Class C | Class D | Verification |
|---|------|---------|---------|---------|---------|--------------|
| L2-01 | `test_layer2_admin_service.cpp` | ✅ `db9f8f9` | n/a (no sleeps) | n/a | ✅ `db9f8f9` | Mutations recorded in §2 |
| L2-02 | `test_layer2_hub_host.cpp` | ⚠ PARTIAL `6f54322` (FSM tests + 3 startup tests fixed) | 🟡 not yet scanned | 🟡 not yet scanned | 🟡 not yet retrofitted | — |
| L2-03 | `test_layer2_role_host_base.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-04 | `test_layer2_role_data_loop.cpp` | 🟡 | 🟡 (workers: 8 sleeps) | 🟡 | 🟡 | — |
| L2-05 | `test_layer2_logger.cpp` (+ workers) | 🟡 | 🟡 (workers: 5 sleeps) | 🟡 | 🟡 | — |
| L2-06 | `test_layer2_file_lock.cpp` (+ workers) | 🟡 | 🟡 (workers: 5 sleeps) | 🟡 | 🟡 | — |
| L2-07 | `test_layer2_json_config.cpp` (+ workers) | 🟡 | 🟡 (workers: 4 sleeps) | 🟡 | 🟡 | — |
| L2-08 | `test_layer2_shared_memory_spinlock.cpp` | 🟡 | 🟡 (4 sleeps) | 🟡 | 🟡 | — |
| L2-09 | `test_layer2_lifecycle.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-10 | `test_layer2_role_config.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-11 | `test_layer2_hub_config.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-12 | `test_layer2_role_directory.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-13 | `test_layer2_role_cli.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-14 | `test_layer2_hub_cli.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-15 | `test_layer2_hub_directory.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-16 | `test_layer2_hub_vault.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-17 | `test_layer2_role_vault.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-18 | `test_layer2_python_engine.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-19 | `test_layer2_lua_engine.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-20 | `test_layer2_metrics_api.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-21 | `test_layer2_schema_validation.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-22 | `test_layer2_role_host_core.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-23 | `test_layer2_role_registry.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-24 | `test_layer2_loop_timing_policy.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-25 | `test_layer2_backoff_strategy.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-26 | `test_layer2_crypto_utils.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-27 | `test_layer2_slot_rw_coordinator.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-28 | `test_layer2_scriptengine_native_dylib.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L2-29 | other L2 files (run `ls tests/test_layer2_service/*.cpp` to enumerate) | 🟡 | 🟡 | 🟡 | 🟡 | — |

### 6.B — Layer 3 datahub tests

(One target `test_layer3_datahub`, many source/worker files.)

| # | File | Class A | Class B | Class C | Class D | Verification |
|---|------|---------|---------|---------|---------|--------------|
| L3-01 | `test_datahub_hub_zmq_queue.cpp` | 🟡 | 🟡 (33 sleeps; first-pass check suggests all are post-`start()` ZMQ-TCP-establishment — needs per-line confirmation) | 🟡 | 🟡 | — |
| L3-02 | `test_datahub_zmq_poll_loop.cpp` | 🟡 | 🔴 KNOWN BAD (multiple `sleep_for(50ms); EXPECT_GE(count, 1)` ordering sites — see §0 incident) | 🟡 | 🟡 | — |
| L3-03 | `test_datahub_hub_inbox_queue.cpp` | 🟡 | 🟡 (10 sleeps; mostly `connect`-establishment but verify each) | 🔴 KNOWN BAD (6 unchecked `c->send(1500ms)` at lines 229/390/424/462/498/531) | 🟡 | — |
| L3-04 | `test_datahub_metrics.cpp` (+ workers) | 🟡 | 🟡 (9 sleeps) | 🟡 | 🟡 | — |
| L3-05 | `workers/role_api_loop_policy_workers.cpp` | 🟡 | 🟡 (9 sleeps) | 🟡 | 🟡 | — |
| L3-06 | `workers/datahub_broker_health_workers.cpp` | 🟡 | 🟡 (8 sleeps) | 🟡 | 🟡 | — |
| L3-07 | `workers/datahub_mutex_workers.cpp` | 🟡 | 🟡 (7 sleeps) | 🟡 | 🟡 | — |
| L3-08 | `workers/datahub_channel_group_workers.cpp` | 🟡 | 🟡 (6 sleeps) | 🟡 | 🟡 | — |
| L3-09 | `test_datahub_hub_monitored_queue.cpp` | 🟡 | 🟡 (6 sleeps) | 🟡 | 🟡 | — |
| L3-10 | `workers/datahub_stress_raii_workers.cpp` | 🟡 | 🟡 (5 sleeps) | 🟡 | 🟡 | — |
| L3-11 | `workers/datahub_c_api_draining_workers.cpp` | 🟡 | 🟡 (3 sleeps) | 🟡 | 🟡 | — |
| L3-12 | `workers/datahub_broker_workers.cpp` | 🟡 | 🟡 (3 sleeps) | 🟡 | 🟡 | — |
| L3-13 | `workers/datahub_broker_consumer_workers.cpp` | 🟡 | 🟡 (3 sleeps) | 🟡 | 🟡 | — |
| L3-14 | `workers/role_api_raii_workers.cpp` | 🟡 | 🟡 (2 sleeps) | 🟡 | 🟡 | — |
| L3-15 | other L3 source/worker files (run `ls tests/test_layer3_datahub/*.cpp tests/test_layer3_datahub/workers/*.cpp` to enumerate) | 🟡 | 🟡 | 🟡 | 🟡 | — |

### 6.C — Layer 4 integration tests

| # | File | Class A | Class B | Class C | Class D | Verification |
|---|------|---------|---------|---------|---------|--------------|
| L4-01 | `test_layer4_plh_role/test_plh_role_init.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L4-02 | `test_layer4_plh_role/test_plh_role_validate.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L4-03 | `test_layer4_plh_role/test_plh_role_keygen.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L4-04 | `test_layer4_plh_role/test_plh_role_round_trip.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | — |
| L4-05 | other L4 files (run `find tests/test_layer4*/ -name '*.cpp'`) | 🟡 | 🟡 | 🟡 | 🟡 | — |

### 6.D — Layer 1 base tests

(Likely smallest scope; finish last.)

| # | File | Class A | Class B | Class C | Class D | Verification |
|---|------|---------|---------|---------|---------|--------------|
| L1-01 | every file under `tests/test_layer1_*/` (run `find` to enumerate) | 🟡 | 🟡 | 🟡 | 🟡 | — |

### 6.E — Layer 0 platform tests

| # | File | Class A | Class B | Class C | Class D | Verification |
|---|------|---------|---------|---------|---------|--------------|
| L0-01 | every file under `tests/test_layer0_platform/` | 🟡 | 🟡 | 🟡 | 🟡 | — |

---

## 7. Phased rollout plan

Each phase delivers a coherent partial state; finishing a phase
flips a count of rows in §6 to ✅.

### Phase 1 — Foundation: L2 service tests for code I touched

**Goal**: every fixture I created or heavily edited in this branch
gets the full §1 protocol applied, including LogCaptureFixture.

| # | Item | Owner | Status |
|---|------|-------|--------|
| 7.1.1 | `test_layer2_admin_service.cpp` — full audit (already partial) | tbd | ✅ |
| 7.1.2 | `test_layer2_hub_host.cpp` — full audit (FSM tests fixed; remainder pending) | tbd | 🟡 |
| 7.1.3 | `test_layer2_role_host_base.cpp` — full audit | tbd | 🟡 |
| 7.1.4 | `test_layer2_role_data_loop.cpp` — full audit | tbd | 🟡 |

### Phase 2 — Highest sleep_for + known-bad files (L3)

| # | Item | Owner | Status |
|---|------|-------|--------|
| 7.2.1 | `test_datahub_zmq_poll_loop.cpp` — fix Class B ordering sites | tbd | 🟡 |
| 7.2.2 | `test_datahub_hub_inbox_queue.cpp` — fix 6 Class C unchecked sends + sweep | tbd | 🟡 |
| 7.2.3 | `test_datahub_hub_zmq_queue.cpp` — confirm 33 sleeps are all ZMQ-TCP-establishment, annotate | tbd | 🟡 |

### Phase 3 — Remaining L3 worker files

(Expand as files are touched; one row per file once started.)

### Phase 4 — Remaining L2 fixtures + LogCaptureFixture rollout

(One row per fixture.  Each gets §1.4 protocol.)

### Phase 5 — L4 integration tests

### Phase 6 — L0 / L1 base tests

### Phase 7 — Suite-wide mutation sweep on guard assertions

(For tests that gate behavior contracts but we have not yet
mutated.  Walk §6 and verify every ✅ row's mutation entry.)

---

## 8. Real bugs surfaced — DO NOT silently paper over

When the audit surfaces a real production bug (per §1.5), it lands
here.  Each row stays open until the production fix or a tracked
deferral with explicit owner+ETA exists in `docs/TODO_MASTER.md`.

| # | Surfaced from | Production bug | Status |
|---|---------------|---------------|--------|
| N01 | `AdminServiceTest.HubHost_Admin*` LogCaptureFixture rollout (`db9f8f9`) | `LifecycleGuard` dynamic-shutdown worker logs `processOneUnloadInThread: 'ThreadManager:HubHost:<uid>' userdata validation failed` because HubHost destroys its ThreadManager (auto-registered as a dynamic LifecycleGuard module) before the worker finishes the unload.  Worker skips the now-redundant callback (work was done by HubHost dtor).  Real lifecycle ordering issue but not a memory/safety bug.  Currently declared expected via `ExpectLogWarn("userdata validation failed")` with a tracking comment. | 🟡 OPEN — fix HubHost / LifecycleGuard ordering OR document the race as accepted; decide owner |

When fixing N01 (and any future row), record the fix commit here AND
remove the `ExpectLogWarn` declaration from any test that was
declaring it.  The audit isn't done until those declarations are
gone.

---

## 9. Documented exceptions (long-running tests, intentional)

Per `docs/IMPLEMENTATION_GUIDANCE.md` and observed at suite-run time
(2026-05-01), the following tests are intentionally slow and
documented as acceptable:

- `LoggerTest.ConcurrentLifecycleChaos` — ~10 s; concurrent stress.
- `SlotRWCoordinatorTest.HighContentionWritersAndReadersStress` — ~10 s; SHM stress.
- `FileLockTest.MultiProcessBlockingContention` — ~8 s; multi-process file lock.
- `DatahubE2ETest.ProducerToConsumerViaRealBroker` — ~5 s; full pipeline e2e.

These are exempt from the §3 "no test taking >2 s" criterion if
documented here.

---

## 10. Pickup recipe — exact commands to resume this audit

```bash
# 1. Confirm where we are
cat /home/qqing/Work/pylabhub/docs/code_review/REVIEW_TestAudit_2026-05-01.md \
  | grep -E "🟡|⚠|🔴" | head -40   # everything still open

# 2. Refresh the sleep_for census (in case more were added)
grep -rln "sleep_for\|::sleep\|usleep" /home/qqing/Work/pylabhub/tests \
  --include='*.cpp' --include='*.h' \
  | grep -v test_framework \
  | xargs -I{} sh -c 'echo "$(grep -c "sleep_for\|::sleep\|usleep" {}) {}"' \
  | sort -rn

# 3. Confirm catch-block sweep is still clean (no new silent-swallow
#    catches added since `f376cb3`)
grep -rn -A 6 "catch\s*(" /home/qqing/Work/pylabhub/src \
  --include="*.cpp" --include="*.hpp" > /tmp/catches.txt

# 4. Pick the lowest-numbered open row in §6 / §7 you can finish.
#    Apply §4 methodology and §1 verification protocol.
#    Make ONE commit per file per §4.6.

# 5. Run the targeted tests + their mutation sweep before claiming ✅:
cmake --build /home/qqing/Work/pylabhub/build -j2 --target stage_tests
ctest --test-dir /home/qqing/Work/pylabhub/build -j2 -R "<target_test_filter>" \
  --output-on-failure 2>&1 | tee /tmp/test_<file>.txt
# Apply mutation, re-run, observe red, restore, re-run, observe green.

# 6. Update §6/§7 row + commit per §4.6 format.
```

---

## 11. Hub-implementation state — where to resume after the audit

The audit blocks new feature work in tests-touching scopes, but it
does NOT change what was already shipped or the next target.  This
section is the bookmark for hub work so the implementation context
survives the multi-session audit.

### 11.1 Hub work shipped on this branch (in order)

| Commit | Phase | What it shipped |
|--------|-------|-----------------|
| `e59bb90` | 6.1a | `BrokerService` ctor takes `HubState&` by reference; HubState ownership externalized from broker (HEP-0033 §4 alignment) |
| `72da2db` | 6.1b | `HubHost` concrete class — owns `HubConfig` (value), `HubState` (value), `BrokerService` + `ThreadManager` (unique_ptrs).  Public surface: `startup()` / `run_main_loop()` / `shutdown()` / `request_shutdown()` / `is_running()` + const accessors |
| `a0fd3a8` | 6.1 fix-up | Startup rollback on partial-init failure; `request_shutdown` async-only contract; init/shutdown step lists pinned in HEP §4.1/§4.2 |
| `0d728ea` | 6.1 FSM | 3-phase start/stop FSM (`Constructed → Running → ShutDown`) on EngineHost + HubHost; CAS-driven, single-use after shutdown, retryable on failed startup |
| `70cd6cc` | (cosmetic) | `PrintTo(RoleSpec)` so CTest names show role tag instead of byte dump |
| `536e129` + `9822ce4` | (docs) | HEP-0033 §4 phase FSM doc; §4.1 LifecycleGuard clarification |
| `9376601` | (TODO) | TODO_MASTER + MESSAGEHUB_TODO snapshot rolled forward |
| `5f652d2` | 6.2a | `AdminService` skeleton — REP socket, JSON envelope, run loop, token gate, localhost-bind invariant, `ping` round-trip; HubHost integration via `unique_ptr<AdminService> admin_svc`, init step 9 / shutdown step 3; HubAdminConfig::admin_token plumbed via `HubConfig::load_keypair` from `HubVault::admin_token()` |
| `59dbef6` | (correctness) | Broker bind-error fail-fast + tighter admin integration test (related to 6.2a) |
| `d34b3d1` | (hygiene) | gitignore `.claude/` |
| `218bc28` | (audit doc) | `REVIEW_AdminService_2026-05-01.md` — Phase 6.2 pre-implementation audit, 6.2a/b/c sub-phase split |
| `6f54322` | (test+docs) | Silent-failure prevention assertion-design rules + audit fixes (Class A) |
| `5e472c6` | (correctness) | HubHost broker post-startup death silent-swallow fix (catch-block) |
| `f376cb3` | (audit fixes) | All 226 catches in `src/` swept for silent-failure |
| `db9f8f9` | (test fxr) | LogCaptureFixture infrastructure + applied to AdminServiceTest |
| `7320e19` + (this commit) | (audit doc) | Test-audit plan (this document) |

### 11.2 Phase 6.2 status

Source of truth: `docs/code_review/REVIEW_AdminService_2026-05-01.md` §3 (sub-phase split).

- **6.2a** ✅ shipped (`5f652d2`, `db9f8f9`).  `AdminService` skeleton +
  LogCaptureFixture for AdminServiceTest.
- **6.2b** 🟡 PENDING.  Query methods that map onto existing
  BrokerService / HubState accessors:
  `list_channels`, `get_channel`, `query_metrics`, `list_roles`,
  `get_role`, `list_bands`, `list_peers`.  All have backing accessors;
  blocked only by §3 audit acceptance criteria for any test files
  touched in the implementation.
- **6.2c** 🟡 PENDING.  Control methods that map onto existing
  mutators: `close_channel`, `broadcast_channel`, `request_shutdown`.
  Same audit-criteria block.
- **Deferred (out of 6.2 scope, do NOT attempt here)**: `revoke_role`
  (needs new `_on_role_revoked` mutator), `reload_config` (HEP §16
  item 9 whitelist design), `add/remove/list_known_roles` (HEP-0035
  Hub-Role Auth — separate HEP, not started), `exec_python`
  (HEP-0033 Phase 7 HubScriptRunner).  See
  `REVIEW_AdminService_2026-05-01.md` §2.2 for the full readiness
  matrix.

### 11.3 What blocks Phase 6.2b / 6.2c

Per the audit acceptance criteria in §3 above, new test work cannot
ship in this branch's tests until at least **Phase 1 of this audit
plan** (§7.1) is done — the L2 service tests I touched must pass
the full §1 verification protocol, including LogCaptureFixture, so
new 6.2b/c tests don't inherit the same silent-failure modes.

### 11.4 Resume recipe — when the audit is done

1. Open `docs/code_review/REVIEW_AdminService_2026-05-01.md` §3 to
   confirm sub-phase scope and §4.1 action items.
2. Phase 6.2b: implement the 7 query methods on `AdminService`,
   wire them through `HubHost` → `BrokerService` accessors.
3. Add tests under `tests/test_layer2_service/test_admin_service.cpp`
   following the §1 protocol and using `LogCaptureFixture`.
4. Mutation-sweep each query method's test (per §1.1) before
   marking the method ✅ in `REVIEW_AdminService_2026-05-01.md` §2.2.
5. Update `docs/TODO_MASTER.md` snapshot + `docs/todo/MESSAGEHUB_TODO.md`
   when 6.2b ships.
6. Repeat for 6.2c.

### 11.5 Hub state SHA pin (for unambiguous resume)

If `git log` shows that any commit after `7320e19` (or after this
audit-plan commit) touched `src/include/utils/admin_service.hpp`,
`src/utils/ipc/admin_service.cpp`, `src/include/utils/hub_host.hpp`,
`src/utils/service/hub_host.cpp`, `src/include/utils/config/hub_admin_config.hpp`,
or `src/utils/config/hub_config.cpp`, the resume context shifted —
re-read those files before continuing.

---

## 12. Lifecycle

Transient code review per `docs/DOC_STRUCTURE.md` §1.7.

When archived:
- Move to `docs/archive/transient-YYYY-MM-DD/REVIEW_TestAudit_2026-05-01.md`.
- Record in `docs/DOC_ARCHIVE_LOG.md`.
- Merge any failure-mode catalog additions (new anti-patterns
  discovered, new helper APIs introduced) into
  `docs/IMPLEMENTATION_GUIDANCE.md` § "Assertion Design —
  silent-failure prevention".
- Open a follow-up `REVIEW_TestAudit_<later-date>.md` if periodic
  re-audit is needed (e.g. after a major refactor that adds a lot
  of new tests).

The companion `docs/code_review/REVIEW_CatchBlocks_2026-05-01.md`
must also be closed before this one archives — they are paired.
