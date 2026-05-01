# REVIEW — Test-correctness audit (full codebase)

| Property        | Value                                                                |
|-----------------|----------------------------------------------------------------------|
| **Status**      | 🟡 OPEN — partial; plan for the rest awaiting user direction         |
| **Created**     | 2026-05-01                                                           |
| **Trigger**     | After the catch-block silent-failure audit, the user surfaced a deeper concern: tests across the suite likely have shallow assertions, timing-by-sleep ordering, and discarded timeout-bearing return values.  Two such failures already shipped on this branch (logged as the catalyst for `feedback_test_outcome_vs_path.md`). |
| **Scope**       | Every test file under `tests/`.  No file exempt.                     |
| **Disposition** | Transient — archive when every flagged item has a verdict and the failure-mode catalog reaches a `green` state for the whole suite. |

---

## 1. Three classes of test-correctness bug

This audit screens for three concrete failure modes, each of which has
already shipped at least once on this branch:

### Class A — Outcome-only assertions (silent on wrong path)

**Pattern**: `EXPECT_THROW(f(), std::exception)`, `EXPECT_EQ(status, "ok")`,
`EXPECT_NO_THROW(f())` — the test verifies *that something happened*
but not *which path produced it*.

**Documented incidents**:
- `HubHostTest.Startup_FailsCleanlyOnBusyPort` and
  `HubHostTest.FailedStartupAllowsRetry` passed for weeks while the
  broker was hitting a 5 s ready-timeout slow path.
- `AdminServiceTest.HubHost_AdminEnabled_RoundTripWorks` passed when
  the ping handler returned `{"status":"ok", "result":{"pong":false}}`
  (verified via mutation 2026-05-01).

**Policy**: `docs/IMPLEMENTATION_GUIDANCE.md` § "Assertion Design —
silent-failure prevention" §§ "Path discrimination", "Timing bound when
'fast' is part of the contract", "Structural payload, not just envelope".

### Class B — Timing-by-sleep ordering (silent on slow regression)

**Pattern**: `std::this_thread::sleep_for(N ms)` followed by an
assertion that depends on a state change happening within that
window, instead of `poll_until(pred, timeout)`.

**Why bad**: the sleep is a hidden timing assumption.  If the
production code regresses to take longer, the assertion fails
WITHOUT a clear message about *why* — it just fails as if the state
change never happened.  Worse: if the assertion checks
`EXPECT_GE(count, 1)` and the count is 0 only because the sleep
expired before dispatch, the test is reporting a *timeout* as a
*correctness failure* — same outcome, different root cause.

**Existing rule** (`feedback_test_correctness_principle.md` in
auto-memory and CLAUDE.md): "Never use `sleep_for` to order
concurrent operations.  Use condition-based waits.  ZMQ socket setup
`sleep_for(100ms)` after bind/start is acceptable (TCP establishment)."

**Census**: 164 occurrences of `sleep_for` / `usleep` across the
test suite (excluding `test_framework/`).

| File | Count | First-pass triage |
|---|---|---|
| `test_layer3_datahub/test_datahub_hub_zmq_queue.cpp` | 33 | All are `sleep_for(50ms)` after `q->start()` / `push->start()` / `pull->start()` with comments like "connection setup" / "TCP handshake" — the documented acceptable category.  Pre-checked. |
| `test_layer3_datahub/test_datahub_zmq_poll_loop.cpp` | 11 | Mixed.  Several are `sleep_for(50ms); EXPECT_GE(count, 1)` — the bad pattern.  **Needs fix.** |
| `test_layer3_datahub/test_datahub_hub_inbox_queue.cpp` | 10 | `sleep_for(30ms); c->send(...)` — the sleep is to let `connect` establish (ZMQ acceptable).  But `c->send()` returns are sometimes discarded — see Class C below. |
| `test_layer3_datahub/workers/role_api_loop_policy_workers.cpp` | 9 | Not yet inspected. |
| `test_layer3_datahub/test_datahub_metrics.cpp` | 9 | Not yet inspected. |
| `test_layer3_datahub/workers/datahub_broker_health_workers.cpp` | 8 | Not yet inspected. |
| `test_layer2_service/workers/role_data_loop_workers.cpp` | 8 | Not yet inspected. |
| `test_layer3_datahub/workers/datahub_mutex_workers.cpp` | 7 | Not yet inspected. |
| `test_layer3_datahub/workers/datahub_channel_group_workers.cpp` | 6 | Not yet inspected. |
| `test_layer3_datahub/test_datahub_hub_monitored_queue.cpp` | 6 | Not yet inspected. |
| (~30 other files, 1–5 each, total ~57) | | Not yet inspected. |

### Class C — Discarded timeout-bearing return values (silent on operation timeout)

**Pattern**: a function with a timeout parameter returns a status code
that the test discards.  When the operation times out at the boundary,
no signal reaches the assertion logic; the test continues against
half-completed state.

**Confirmed example** (this audit, not yet fixed):
`test_datahub_hub_inbox_queue.cpp` lines 229, 390, 424, 462, 498, 531
call `c->send(ms{1500})` and discard the returned `uint8_t` ack code.
Lines 100, 161, 307 capture and assert it.  Six call sites of the
same API: 6 unchecked, 3 checked.  An ack timeout (return code 255)
on the unchecked sites would not cause the test to fail at the send
boundary; it would manifest later as a confusing `fut.get()` value
mismatch with no breadcrumb back to "send timed out".

**Census**: not yet enumerated across the suite.  Each operation
with a timeout parameter (`recv`, `send`, `wait_for`, `try_lock_for`,
`try_acquire`, ...) needs a check for "is the return value
asserted?" at every call site.

---

## 2. What is verified and committed on this branch so far

- ✅ Class A — fixed in commits `6f54322` (assertion-design policy +
  test fixes), `5e472c6` (silent-swallow in fail-fast), `db9f8f9`
  (LogCaptureFixture, sensitivity verified by mutation).  Applies
  only to the tests I touched in this branch (HubHost FSM tests,
  AdminService Phase 6.2a tests).  **Does not retroactively fix the
  pre-existing 1687-test suite.**

- ✅ Class B + C — sleep_for census collected (above table).  No
  pre-existing bad patterns fixed yet.  **Confirmed bad pattern
  exists** (test_datahub_zmq_poll_loop.cpp Class B; inbox_queue Class C).

- ✅ Catch-block sweep across all 226 catches in `src/` — commit
  `f376cb3`.  Separate from this audit but the same silent-failure
  family.

---

## 3. What is NOT yet verified

This is the honest list of what remains.  No spin.

1. **Class A audit across the existing 1687-test suite.**  Every
   `EXPECT_THROW`, `EXPECT_NO_THROW`, `EXPECT_EQ(status, "ok")`,
   `EXPECT_TRUE(has_value())` call site needs a read.  Estimate: low
   thousands of call sites at ~30 s each = days.

2. **Class B audit of the remaining ~57 sleep_for files** (the
   ~30 files with 1–5 occurrences each, plus the higher-count files
   I have not yet inspected).  Each needs a per-occurrence verdict:
   *acceptable* (ZMQ TCP establishment, etc.) vs *ordering anti-pattern*
   (replace with `poll_until`).

3. **Class C census across the suite.**  Need to enumerate every
   timeout-bearing operation (`recv`, `send`, `try_*_for`, `wait_for`,
   `wait_until`) and check assertion coverage at every call site.

4. **LogCaptureFixture rollout.**  Currently applied only to
   `AdminServiceTest`.  The remaining ~50 test fixtures need to
   adopt it incrementally.

5. **Mutation-sweep verification of "guard" assertions.**  Many
   tests claim to gate behavior contracts.  Per the policy, every
   such claim should be verified by a 30-second mutation sweep.
   Doing this for 1687 tests is multiple sessions of work.

---

## 4. Plan — proposed order

Given the scale and the user's "no deferral" directive: the audit is
the work; "no deferral" means "no silent deferral" — the work is
recorded here so it cannot be lost, then progressed file by file
without skipping ahead to claim victory.

Proposed order (depth-first per layer to keep partial state coherent):

**Phase 1 — Layer 2 service tests (highest churn, my recent work):**
- 1.1 — Apply `LogCaptureFixture` to `test_layer2_hub_host.cpp`,
  `test_layer2_role_host_base.cpp`, `test_layer2_role_data_loop.cpp`
  (highest-touch fixtures from the FSM rework).  Sensitivity-verify
  each via mutation.
- 1.2 — Audit Class A patterns in those three files.  Tighten weak
  assertions.
- 1.3 — Audit Class B / C patterns in those three files.

**Phase 2 — Layer 3 datahub tests (highest sleep_for count):**
- 2.1 — `test_datahub_zmq_poll_loop.cpp` — known Class B violations
  in §1.  Convert to `poll_until`.
- 2.2 — `test_datahub_hub_inbox_queue.cpp` — known Class C unchecked
  sends in §1.  Audit and fix the 6 unchecked sites + sweep for more.
- 2.3 — Remaining L3 worker files (datahub_broker_health,
  datahub_mutex, datahub_channel_group, datahub_stress_raii, ...).

**Phase 3 — Remaining tests (Layer 1, 2 small files, Layer 4):**
- 3.1 — File-by-file with the same triage rubric.

**Phase 4 — Suite-wide LogCaptureFixture rollout:**
- 4.1 — Each fixture inherits `LogCaptureFixture`.  Each
  pre-existing warning in production code surfaces as a test
  failure (catalog and triage as it surfaces).

**Phase 5 — Suite-wide mutation sweep on guard assertions:**
- 5.1 — Identify "guard" assertions (those that gate contracts vs.
  plain sanity checks).  Mutation-test each.

Each phase produces:
- A commit per file (or per logical group) with the fixes.
- An update to this document marking that file's row as
  ✅ FIXED with the commit hash.

---

## 5. Action items

### 5.1 OPEN

- [ ] **A1** — Get explicit user direction on plan order (§4).
  Should I do Phase 1 in this session and stop, or push deeper?
- [ ] **A2** — Phase 1.1: LogCaptureFixture on `test_layer2_hub_host.cpp`.
- [ ] **A3** — Phase 1.1: LogCaptureFixture on `test_layer2_role_host_base.cpp`.
- [ ] **A4** — Phase 2.1: convert `test_datahub_zmq_poll_loop.cpp`
  Class B sites to `poll_until`.
- [ ] **A5** — Phase 2.2: fix `test_datahub_hub_inbox_queue.cpp` 6
  unchecked send call sites.
- [ ] **A6** — All other phases per §4.

### 5.2 RESOLVED

(populated as items land)

---

## 6. Lifecycle

This is a **transient** code review per `docs/DOC_STRUCTURE.md` §1.7.
Archive to `docs/archive/transient-YYYY-MM-DD/` when every row in
§1's tables is verdict'd, every §5.1 item is ✅, and the
`docs/code_review/REVIEW_CatchBlocks_2026-05-01.md` companion is
also closed.  Lasting findings (failure-mode catalog entries, new
helpers, fixture patterns) merge into
`docs/IMPLEMENTATION_GUIDANCE.md` § "Assertion Design — silent-failure
prevention" before archival.
