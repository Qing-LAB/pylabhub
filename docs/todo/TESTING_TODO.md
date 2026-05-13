# Testing TODO

**Purpose:** Track testing tasks, test phases (A-D), coverage gaps, and testing infrastructure improvements.

**Master TODO:** `docs/TODO_MASTER.md`  
**Test Strategy:** `docs/README/README_testing.md`

---

## Test Design Principles (MANDATORY — review every test against these)

Agreed framework, 2026-05-09.  Every existing test SHOULD be auditable against
this principle; every new test MUST be designed to fit it.  When a test fails
the audit, the right move is to migrate the test to use real production
classes (extending their observability surface if necessary), not to keep the
mock running.

### 1. Layer purpose

| Layer | Purpose |
|---|---|
| **L1** | Test correctness of individual functions / APIs and correct handling of error paths. |
| **L2** | Test collective behavior / function of a whole module. |
| **L3** | Test correct handling of higher-level protocol / design / events spanning modules. |
| **L4** | Final integration of multiple modules — the shipping system / binary. |

### 2. Maximize use of real production modules; mocks only when absolutely necessary

Creating production-shaped test scaffolding from scratch — mock modules,
hand-rolled wrappers reimplementing production wiring, parallel structures
mirroring a real class — is **only allowed when absolutely necessary**.

"Necessary" is narrow:

1. The real production class genuinely cannot expose the detail under test
   through its API, logs, or debug surface, **AND**
2. Extending the real class's observability surface to expose that detail
   isn't the right move (the detail is genuinely outside the class's
   responsibility, e.g. injected hardware peripherals, external network,
   wall-clock).

If neither holds, the mock is forbidden.  Project-internal classes (broker,
host, state, admin, role host, thread manager, queues, processor, engine,
…) **never** qualify — mocking them creates parallel production code in
the test tree that drifts, requires its own maintenance, and produces
false test signal (tests pass against the mock while production has
changed).

### 3. Audit checklist (apply to every test under review)

For each test, walk the checklist.  Any "no" is a finding.

- [ ] **Layer assignment is honest.**  The directory name (L1/L2/L3/L4)
      matches the test's actual purpose per item 1 above.  A test that
      validates cross-module event handling with real wire/event flow is
      L3, not L2; a test that just checks one class's API contract is L2,
      not L3 — even if it runs in a subprocess.
- [ ] **No hand-rolled wrappers for project-internal classes.**  Search
      the test file for `struct *Handle` / `struct *Wrapper` / `class Mock*`
      / `std::thread` instantiations.  Each one is a finding unless it
      meets the genuinely-necessary exceptions in item 2.
- [ ] **Real production assembly at the right layer.**  L3 tests use the
      real `HubHost` (or whatever the analogous module-owner is); L4 tests
      drive the real shipping binary (`plh_hub` / `plh_role` / etc.) via
      `WorkerProcess`.  No re-derivation of host wiring.
- [ ] **Friend-shim, not mock, when private surface is needed.**  L2 tests
      reaching private methods use a `friend struct *TestAccess` shim (the
      pattern `pylabhub::hub::test::HubStateTestAccess` already follows).
      That is an extension of the real class's observability — adding a
      `friend` declaration — not a fork.
- [ ] **Observability gap → extend real class, never mock.**  If the test
      can't assert what it needs to assert through the real class's public
      API, logs, or accessors, the fix is to add the missing accessor /
      log line / debug snapshot to the real class (which then helps
      production diagnostics too).  Writing a mock to expose the detail is
      the wrong direction.
- [ ] **Test scenario fits real production wire/event paths.**  A test
      that constructs request payloads (e.g., REG_REQ JSON) without going
      through the same wire-encoding helpers production uses is a finding
      — it pins the broker's behavior against test-only payload shapes
      that diverge from what production roles actually send.

### 4. Cautionary case (the reason this rule was crystallized)

The 8 hand-rolled `BrokerHandle` HubHost-mocks in
`tests/test_layer3_datahub/workers/datahub_*_workers.cpp` reimplement
HubHost's threading/lifecycle/state-ownership job from scratch.  They
drifted past post-refactor architectural changes (ThreadManager rollout,
AdminService, HubScriptRunner integration, `role_uid` derivation) and
produced false signal — tests pass against the mock while production
behavior had diverged.  Replacing them with real-`HubHost`-backed
scaffolding is open work (see "Audit work" subsection below).

### 5. Audit work (open)

- [x] **Suite-wide audit** completed 2026-05-09.  Findings below.
- [x] **Deleted `test_datahub_broker_shutdown.cpp`** (2026-05-09): 6 tests
      pinning the grace-escalation shutdown protocol that M1.2/M1.3 retired.
      The post-M1.2 close protocol is `CHANNEL_CLOSING_NOTIFY` (informational
      only — no FORCE_SHUTDOWN escalation; the channel is already torn down
      atomically when the notify is sent).  Role-side script-callback tests
      for this notification will be designed under the re-framed **M1.5**
      (`on_channel_closing` + auto-stop) — see
      `docs/tech_draft/M1.5_channel_closing_redesign_2026-05-12.md` §8 test
      matrix.
- [x] **Migrated all 8 BrokerHandle mock-host instances** to real `HubHost`
      (closed 2026-05-10).  All 6 `test_datahub_broker*.cpp` files (the
      `broker_shutdown` 7th was deleted) + their workers now use real
      `HubHost` via a `HubHostHandle`/`BrokerHandle` RAII wrapper that
      owns a `pylabhub::hub_host::HubHost` and translates legacy
      `BrokerService::Config` fields to `HubConfig` overrides written
      to a per-test on-disk `hub.json`.  Two production gaps surfaced
      and fixed in the migration: `HubHost` now wires
      `<hub_dir>/schemas/` per HEP-CORE-0034 §12; `HubBrokerConfig`
      gained a `checksum_repair_policy` field per HEP-CORE-0007 §12.4.

#### Migration result (2026-05-10) — broker test cluster fully migrated

All previously-known failing tests are now passing.  Test count:
**1782/1782 green** at HEAD `4e30618`.

The migration also surfaced two production gaps (closed in the same
session) and an audit-residual to-do:

- `HubHost` now wires `<hub_dir>/schemas/` to broker's
  `schema_search_dirs` per HEP-CORE-0034 §12 (commit `f472e4c`).
- `HubBrokerConfig::checksum_repair_policy` field added per
  HEP-CORE-0007 §12.4 (commit `62ca573`).

Remaining audit items are folded into §6 (Code review findings,
2026-05-10) below as actionable strands.
- [ ] **Adopt `BrokerRequestComm` for payload construction** in the
      same 15 test files affected by hand-rolled JSON `make_reg_opts`
      builders.  Most of the broker-cluster files now use the
      production helpers post-migration; the remaining inline payload
      construction at the broker-cluster boundary is intentional
      (raw-ZMQ tests verifying error paths the real `BrokerRequestComm`
      can't reach, e.g. `broker_dereg_pid_mismatch` testing wire
      pid-mismatch defense).  Audit and shrink to the genuinely-needed
      raw-ZMQ cases.
- [ ] **Add L2 BrokerService coverage** via a `BrokerServiceTestAccess`
      friend shim mirroring `HubStateTestAccess`.  L2 today has zero
      direct `BrokerService` coverage — every wire-protocol handler
      (REG_REQ / DISC_REQ / HEARTBEAT_REQ / DEREG_REQ /
      CONSUMER_REG_REQ / HUB_PEER_HELLO / HUB_TARGETED_MSG / METRICS_REQ
      / CHANNEL_BROADCAST_REQ) is tested only at L3 through real
      `HubHost`.  **List corrected 2026-05-12** — removed
      `FORCE_SHUTDOWN` (retired by M1.2 commit `a41ce71`; verified
      via `grep -rn FORCE_SHUTDOWN src/` returning zero matches); added
      `METRICS_REQ` and `CHANNEL_BROADCAST_REQ` which were missing.

### 6. Audit findings (2026-05-09)

| Severity | Count | Pattern |
|---|---|---|
| Severe | 8 | `BrokerHandle` / `LocalBrokerHandle` HubHost-mocks |
| Moderate | 0 | (none) |
| Minor | 15 | Hand-rolled JSON payload builders (`make_reg_opts` etc.) |

**Severe — 8 files** (`struct BrokerHandle` / `struct LocalBrokerHandle`
that wires `BrokerService` + `HubState` directly with a hand-rolled
`std::thread`, bypassing real `HubHost`/ThreadManager/AdminService/
HubScriptRunner/lifecycle integration).  Checklist violations: **(b) +
(c)**.

`tests/test_layer3_datahub/`:
- `test_datahub_broker_admin.cpp`
- `test_datahub_broker_schema.cpp`
- `test_datahub_broker_protocol.cpp`
- `test_datahub_broker_shutdown.cpp`
- `workers/datahub_broker_workers.cpp`
- `workers/datahub_broker_consumer_workers.cpp`
- `workers/datahub_broker_health_workers.cpp`
- `workers/datahub_broker_request_comm_workers.cpp`

**Minor — 15 files** with hand-rolled JSON payload builders.  Pattern:

```cpp
json make_reg_opts(const std::string &channel, const std::string &role_uid) {
    json opts;
    opts["channel_name"] = channel;
    opts["pattern"]      = "PubSub";
    // ... 8+ more manual assignments
    return opts;
}
```

This builds REG_REQ / CONSUMER_REG_REQ payloads by hand instead of going
through `BrokerRequestComm` (the production wire-encoding helper).  The
broker is then pinned against test-only payload shapes that don't match
what production roles actually send.  Checklist violation: **(f)**.

Test files: `test_datahub_broker_admin.cpp`, `test_datahub_broker_protocol.cpp`,
`test_datahub_broker_schema.cpp`, `test_datahub_broker_shutdown.cpp`,
`test_datahub_channel_access_policy.cpp`, `test_datahub_hub_host_integration.cpp`,
`test_datahub_metrics.cpp`, `test_datahub_zmq_endpoint_registry.cpp`,
`test_hub_lua_integration.cpp`.

Worker files: `datahub_broker_consumer_workers.cpp`, `datahub_broker_health_workers.cpp`,
`datahub_broker_request_comm_workers.cpp`, `datahub_broker_workers.cpp`,
`datahub_e2e_workers.cpp`, `datahub_role_state_workers.cpp`.

**Compliance baseline (no findings):** L1 tests (no broker-class touches);
L2 tests (real classes + `HubStateTestAccess` friend shim — exemplary);
L3 tests outside the broker cluster (`test_datahub_hub_host_integration.cpp`
host-positive paths, `test_hub_lua_integration.cpp`, `test_hub_python_integration.cpp`
all use real `HubHost`); L4 tests (drive real binaries via `WorkerProcess`);
test framework utilities (`LogCaptureFixture`, `TmpDir`, `IsolatedProcessTest`
— production-agnostic, not mocks).

---

### 8. Wave M2 — Multi-Producer Channel Bookkeeping coverage (2026-05-10)

Canonical plan in `docs/TODO_MASTER.md` "Wave M2".  Test-layer scope
in phase MP5:

- L2 (`tests/test_layer2_service/test_hub_state.cpp`):
  - Multi-producer REG_REQ admission: second producer-role-uid on the
    same channel appends a new `ProducerEntry`.
  - Same-uid restart: REG_REQ from the same role-uid replaces that
    one `ProducerEntry`, doesn't append.
  - SHM single-producer enforcement: second producer-role-uid on a
    `data_transport == "shm"` channel rejected with
    `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`.
  - Role_disconnected cascade unification: drop one producer-presence
    that is the role's last presence ⇒ role_disconnected fires once;
    drop one that is NOT the last presence ⇒ no fire; explicit
    `_set_role_disconnected` ⇒ fires once; combinations are
    idempotent.
  - Asymmetric expiry: A timeout / B still alive ⇒ channel stays,
    observable stays `kLive`.  Both timeout ⇒ atomic teardown.
  - `channel_to_json` shape: emits `producers: [...]` array.

- L3 (`tests/test_layer3_datahub/*.cpp`):
  - End-to-end multi-producer REG_REQ via `BrokerRequestComm` against
    real `HubHost`.
  - CHANNEL_ERROR_NOTIFY fan-out: schema-mismatch attempt notifies
    every producer registered on the channel.
  - CHANNEL_CLOSING_NOTIFY fan-out on atomic teardown: every producer
    + every consumer.
  - Sweep loop covers all producer-presences (asymmetric heartbeat
    timing among co-producers).

- Migration of existing test sites: any read of
  `entry.producer_role_uid` → either `entry.producers[k].role_uid`
  (specific producer) or a scan helper (any producer).

### 9. Unresolved test failures — MD1 race remains open; chain reordered 2026-05-12

#### `PlhHubCliTest.RoundTrip_PlhHubKeygenAndRunPlhRoleRegisters` — SIGSEGV exit 139 (MD1)

**Status (refreshed 2026-05-12):** root cause from 2026-05-10 gdb-batch
capture confirmed still valid.  Re-verified against current code on
2026-05-12: `BrokerRequestComm::stop()` at `broker_request_comm.cpp:598-620`
is still fire-and-forget (sets `stop_requested = true` + sends a
wake-up signal, returns immediately — does NOT wait for
`run_poll_loop` to exit).  `do_role_teardown` step ordering
(`role_host_lifecycle.cpp:43-56`) is unchanged: Step 12 stop signal,
Step 13 teardown_infrastructure, Step 14 thread drain.  The race
remains real.

Wave M2 has now closed (Wave M2.5 + Wave M3 + M1.4 all shipped
2026-05-11), so MD1 is unblocked.  TODO_MASTER dependency chain
reordered 2026-05-12 to land MD1 BEFORE M1.5 (the re-framed
`on_channel_closing` callback) because M1.5's auto-stop path adds
load on the same teardown sequence — stacking two layers on a
known-buggy substrate would mask failure modes.  See
`docs/tech_draft/M1.5_channel_closing_redesign_2026-05-12.md` §11
for the rationale.

Not Python/GIL related (the 2026-05-10 memory note about Python
flake was a separate incident, since resolved; this race is purely
the BRC ctrl-thread vs role-teardown ordering bug).

**Root cause — use-after-free race between role teardown and
BrokerRequestComm ctrl-thread poll loop.**

`do_role_teardown` (`src/utils/service/role_host_lifecycle.cpp`)
sequences:
```
Step 12: broker_comm->stop()        // sets stop flag; does NOT wait
Step 13: teardown_infrastructure()  // role-specific cleanup
Step 14: api.thread_manager().drain()  // joins ctrl thread
```

The producer-side `teardown_infrastructure_`
(`src/producer/producer_role_host.cpp:462-466`) destroys
`broker_comm_` (BrokerRequestComm) in Step 13:
```cpp
if (broker_comm_) {
    broker_comm_->disconnect();
    broker_comm_.reset();        // ← destroys BrokerRequestComm.pImpl
}
```
The comment immediately above this block claims "Ctrl thread already
joined" — that is wrong.  The ctrl thread is joined only in Step 14,
*after* this destruction.

The ctrl thread is spawned by
`RoleAPIBase::start_ctrl_thread`
(`src/utils/service/role_api_base.cpp:597-612`) and runs
`bc->run_poll_loop(...)` where `bc = pImpl->broker_channel`.  After
`run_poll_loop`'s internal `loop.run()` returns
(`src/utils/network_comm/broker_request_comm.cpp:592`), the thread
falls through to:
```cpp
// broker_request_comm.cpp:594
pImpl->poll_loop_running.store(false, std::memory_order_release);
```
By the time this store executes, Step 13 has already destroyed
`broker_comm_`, freeing `pImpl`.  The atomic store dereferences freed
memory → SIGSEGV.

**Captured stack trace (gdb-batch, 2026-05-10):**
```
Thread 9 SIGSEGV:
  #0  std::__atomic_base<bool>::store (this=0x7ff81bf66dd2 — freed memory)
  #1  std::atomic<bool>::store
  #2  pylabhub::hub::BrokerRequestComm::run_poll_loop  ← broker_request_comm.cpp:594
  #3  RoleAPIBase::start_ctrl_thread lambda            ← role_api_base.cpp:607

Thread 5 (concurrent teardown):
  ZmqQueue::stop  ← (called from RoleAPIBase::close_queues)
  RoleAPIBase::close_queues                            ← role_api_base.cpp:321
  scripting::do_role_teardown                          ← role_host_lifecycle.cpp:52
  ProducerRoleHost::worker_main_
```

(Crash log preserved at `/tmp/last_role_crash.log` and
`/tmp/captured_crash.log` for the session.)

**Reproduction:**
- Standalone harness: 0/30 crashes when run sequentially.
- 2-parallel harness (mimics `ctest -j2`): 1/13 crashes.  The race
  needs concurrent CPU pressure to expose.

**Fix (deferred to post-M2) — design principle locked 2026-05-10:**

> **Stop the machine before disassembling it.**  No object may be
> destroyed while another thread is still using it.  The owning side
> must guarantee that all in-flight uses have observed "stop" *and
> returned* before destruction proceeds.

Two acceptable shapes — both enforce the invariant; choice is local
ergonomics:

1. **`BrokerRequestComm::disconnect()` becomes synchronous w.r.t.
   external poll loops.**  It already signals `stop_requested`;
   extend it to also *wait* until any thread currently inside
   `run_poll_loop` has exited the function.  After `disconnect()`
   returns, `BrokerRequestComm` is guaranteed quiescent and safe to
   destroy.  Caller (the role host) keeps its current single-shot
   `disconnect(); reset();` idiom.  This is the more locally-correct
   shape because the object's own contract enforces the invariant.

2. **Split `teardown_infrastructure_` into two phases.**  Pre-drain:
   signal-only (broker_comm->stop, queue stop signals).  Drain ctrl
   thread (`api.thread_manager().drain()`).  Post-drain: destructive
   (`broker_comm_.reset()`, queue .reset()).  Requires editing
   `do_role_teardown` to call into the role host twice with the drain
   in between.

Option 1 is preferred: it makes the invariant a property of the
class, so future callers can't reintroduce the race.

Producer / consumer / processor host comments at the
`broker_comm_.reset()` site falsely claim "Ctrl thread already joined"
— must be removed (option 1) or made true (option 2) when the fix
lands.

**Confirmed: NOT related to:**
- M1.2 atomic-deletion sweep (`a41ce71`).
- `disconnected_fired` patch (`c2973ef`).
- Python GIL / interpreter lifecycle (Python is loaded in the trace
  but not on either of the relevant stacks).

**Discipline note (2026-05-10):**  Two earlier commit messages
(`a41ce71`, `c2973ef`) referred to this failure as a "known flake" /
"pre-existing race" without evidence.  This entry is now backed by a
real stack trace — refer to it as a *use-after-free race in role
teardown* in future commit messages, not a "flake".

### 7. Code review findings (2026-05-10) — strand plan

A 3-agent review of the test suite + production code at HEAD
`4e30618` identified 4 severe + 9 moderate + 2 minor issues.  The
remediation clusters into 5 strands.  Recommended ordering: **S1 →
S2 → S4 → S3 (parallel with S4) → S5**.

**Detailed file/line references and suggested assertion shapes for
each finding live in this section so they survive context resets.**

#### Strand S1 — Severe immediate fixes (< 1 hour)

- [ ] **S1a. Replace 4 `EXPECT_TRUE(brc.deregister_*(...))` patterns**.
      Post-Bucket-C, these methods return `optional<json>`; `optional →
      bool` is `has_value()`, true for both ACK and ERROR — silent
      false-pass risk if production starts emitting an error here.
      Sites:
        - `tests/test_layer3_datahub/workers/datahub_broker_workers.cpp:461`
        - `tests/test_layer3_datahub/workers/datahub_broker_health_workers.cpp:307, 367`
        - `tests/test_layer3_datahub/workers/datahub_role_state_workers.cpp:337`
      Fix shape: `auto r = brc.deregister_*(...); ASSERT_TRUE(r.has_value());
      EXPECT_EQ(r->value("status",""), "success");`
- [ ] **S1b. ROLE_PRESENCE_REQ + ROLE_INFO_REQ error envelope drift**.
      Both handlers emit ad-hoc `resp["error"] = "missing role_uid"`
      instead of HEP-CORE-0007 §12.3 standard envelope (`status: "error",
      error_code, message, correlation_id`).  Inconsistent with every
      other handler (which uses `make_error()`).
      Sites:
        - `src/utils/ipc/broker_service.cpp:2860` (ROLE_PRESENCE_REQ)
        - `src/utils/ipc/broker_service.cpp:2918` (ROLE_INFO_REQ)
      Fix shape: replace inline `resp["error"] = ...` with
      `return make_error(corr_id, "MISSING_ROLE_UID", "missing role_uid");`
      (the §12.4a taxonomy already enumerates `MISSING_ROLE_UID`).
      Update HEP-CORE-0007 §"ROLE_PRESENCE_ACK" / "ROLE_INFO_ACK"
      missing-uid response examples to match.

#### Strand S2 — M1.2 Phase 8 + observable coverage (0.5–1 day)

- [ ] **S2a. `ConsumerHeartbeat_DoesNotRefreshProducerPresence`** — the
      canonical regression test for the original cross-presence
      bookkeeping bug (HEP-CORE-0019 §2.3 pre-Phase-6).  Per the M1
      handoff doc this was scheduled as Phase 8 work and was never
      landed.  Without it, a future regression that re-introduces
      cross-presence bookkeeping passes silently.
      Setup: producer + consumer registered, both alive.  Stop
      producer heartbeats but keep consumer heartbeats.  Assert:
      producer-presence transitions Connected → Pending → Disconnected
      as if consumer heartbeats didn't exist.  Belongs in
      `tests/test_layer3_datahub/test_datahub_broker_protocol.cpp`.
- [ ] **S2b. `DiscReq_ChannelStalled_ReturnsDiscPendingWithReason`**
      (HEP-CORE-0023 §2.2).  Phase 4 introduced the `kStalled`
      observable + the `"heartbeat_stalled"` DISC_PENDING reason.
      Other 3 observables (`absent`, `registering`, `live`) have
      coverage; `stalled` does not.
      Setup: producer registered + first heartbeat seen; let
      `ready_timeout` expire so producer-presence transitions to
      Pending; DISC_REQ → assert `status == "pending"` AND
      `reason == "heartbeat_stalled"`.
- [ ] **S2c. Atomic-teardown contract assertion** (HEP-CORE-0023 §2.1).
      Tests verify CHANNEL_CLOSING_NOTIFY delivery but don't assert
      the channel is removed atomically.  Modify
      `ClosingNotify_DeliveredToProducerAndConsumer` (in
      `tests/test_layer3_datahub/test_datahub_broker_protocol.cpp`)
      to also assert `query_channel_snapshot()` shows no entry for
      the channel immediately after the teardown trigger.

      **Partially addressed by Wave M3 step 5b+e (2026-05-11):**
      - L2 atomic-teardown is pinned by
        `HubStateChannelClosed.ConsumerPresence_AtomicallyTransitionsDisconnected`
        (`tests/test_layer2_service/test_hub_state.cpp`) — verifies
        producer AND consumer presences transition Disconnected
        atomically on channel close.
      - L3 role-entry erase post-teardown pinned by
        `RoleEntry_TerminalCleanup_OnLastPresenceDisconnect`
        + `RoleEntry_TerminalCleanup_OnConsumerLeftLast`
        (`tests/test_layer3_datahub/test_datahub_role_state_machine.cpp`).
      - **Still TODO**: the specific channel-snapshot assertion on
        `ClosingNotify_DeliveredToProducerAndConsumer` per the
        original S2c scope.
- [ ] **S2d. `ChannelEntry_HasNoStoredFSMFields`** structural test
      (also from M1 handoff Phase 8).  Add after Phase 6 deletion
      lands: `static_assert` (or runtime equivalent) that
      `ChannelEntry` has no `status`, `last_heartbeat`, `state_since`,
      `closing_deadline` members.

#### Strand S3 — Error-code taxonomy coverage (1–2 days)

- [ ] **New `tests/test_layer3_datahub/test_datahub_error_codes.cpp`**.
      Per HEP-CORE-0007 §12.4a, 30 distinct `error_code` values
      exist; only 6 (`CHANNEL_NOT_FOUND`, `TRANSPORT_MISMATCH`,
      `NOT_REGISTERED`, `SCHEMA_MISMATCH`, `SCHEMA_HASH_MISMATCH_SELF`,
      `SCHEMA_ID_MISMATCH`) have dedicated coverage today.  The other
      24 have no test verifying both the trigger condition AND the
      surfaced `error_code` on the wire response.

      **Codes to cover** (one TEST_F per code, preferably grouped by
      handler):
      - General: `UNKNOWN_MSG_TYPE`, `INVALID_REQUEST`,
        `IDENTITY_REQUIRED`, `MISSING_ROLE_UID`, `NOT_IN_KNOWN_ROLES`
      - Channel state: `CHANNEL_NOT_READY`, `NOT_CHANNEL_OWNER`
      - Schema: `SCHEMA_FORBIDDEN_OWNER`, `SCHEMA_UNKNOWN`,
        `SCHEMA_CITATION_REJECTED`, `FINGERPRINT_INCONSISTENT`,
        `MISSING_BLDS`, `MISSING_PACKING`, `MISSING_HASH`,
        `MISSING_HASH_FOR_NAMED_CITATION`,
        `MISSING_BLDS_FOR_ANONYMOUS_CITATION`,
        `MISSING_PACKING_FOR_ANONYMOUS_CITATION`
      - Inbox: `INVALID_INBOX_ENDPOINT`, `INVALID_INBOX_PACKING`,
        `INBOX_SCHEMA_INVALID`, `INBOX_UPDATE_NOT_SUPPORTED`
      - Endpoint: `INVALID_ENDPOINT`, `UNKNOWN_ENDPOINT_TYPE`,
        `ENDPOINT_ALREADY_SET`

      Assertion shape: `EXPECT_EQ(resp->value("status",""), "error");
      EXPECT_EQ(resp->value("error_code",""), "<expected>");
      EXPECT_FALSE(resp->value("message","").empty());`

#### Strand S4 — M1.2 Phase 5-7 production cleanup — **CLOSED 2026-05-10 (commit `a41ce71`)**

All three phases landed in a single atomic-sweep commit (24 files,
+551/-1385), exceeding the 2026-05-09 punch list because the cleanup
went further than originally scoped: `FORCE_SHUTDOWN` was REMOVED
WHOLESALE rather than being rewired as best-effort notification per
the 2026-05-09 plan.  The wire-message slot vanished; the surviving
substitute is `CHANNEL_CLOSING_NOTIFY` (post-fact "the channel is
gone" — see `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md`
§12 "FORCE_SHUTDOWN — REMOVED 2026-05-07").

What the M1.2 sweep actually shipped (verified 2026-05-12 by
`grep -rn FORCE_SHUTDOWN src/` returning zero matches in production
code):

- [x] **Phase 5** — `_on_heartbeat` body refit; legacy writes dropped;
      `_set_role_disconnected` walks `role.presences` only.
- [x] **Phase 6** — `enum class ChannelStatus` + `ChannelEntry.{status,
      last_heartbeat, state_since, closing_deadline}` +
      `RoleEntry.{state, last_heartbeat, latest_metrics,
      metrics_collected_at}` + `to_string(ChannelStatus)` +
      `_set_channel_status` + `_set_channel_closing_deadline` +
      `_update_role_heartbeat` + `_update_role_metrics` +
      `observable_from_legacy_status` shim + dual channel-JSON
      serializer + transitional `j["status"]` emission — ALL deleted.
- [x] **Phase 7 (M1.3)** — `BrokerServiceImpl::check_closing_deadlines`
      + `cfg.effective_grace` + `cfg.grace_heartbeats` +
      `kDefaultGraceHeartbeats` + `PYLABHUB_DEFAULT_GRACE_HEARTBEATS`
      + `grace_heartbeats` / `grace_ms` config + `case
      ChannelStatus::Closing` arms + `send_force_shutdown` paths —
      ALL deleted.  `FORCE_SHUTDOWN` was NOT rewired as best-effort
      notification (the 2026-05-09 plan); it was removed completely.
      The role-side ergonomics gap left by atomic teardown is the
      scope of re-framed **M1.5** (`on_channel_closing` callback +
      optional auto-stop) — see
      `docs/tech_draft/M1.5_channel_closing_redesign_2026-05-12.md`.

#### Strand S5 — Coverage broadening (lower priority)

- [ ] **HEP-CORE-0034 §12 multi-file hub-globals test**.  Drop 2+
      JSON schema files into `<hub_dir>/schemas/`, verify all loaded
      and accessible via path-C citation.  Test belongs in
      `tests/test_layer3_datahub/test_datahub_broker_schema.cpp` (now
      on real `HubHost`, so the canonical location is reachable).
- [ ] **Admin RPC positive-path tests for `close_channel` +
      `broadcast_channel`** (HEP-CORE-0033 §11.2).  Today only
      error-path tests exist (missing params, 404).  Add positive
      paths verifying the channel actually closes / message actually
      fans out to subscribers.
- [ ] **Processor two-presence asymmetric-failure test** (HEP-CORE-0023
      §2.6).  Processor with `(uid, "consumer")` on in-channel and
      `(uid, "producer")` on out-channel: stop one side's heartbeats,
      keep the other; assert the stalled presence transitions
      Pending → Disconnected while the live one stays Connected.
- [ ] **HEP-CORE-0030 band protocol round-trip tests**.  State cleanup
      on role close is tested; BAND_JOIN/LEAVE/SEND wire-frame
      contract isn't pinned.
- [ ] **Header-comment cleanup sweep** (cosmetic).  Migrated files
      still have stale references to the retired `LocalBrokerHandle`
      pattern in their top-of-file comments
      (`test_datahub_broker_admin.cpp:8`,
      `test_datahub_broker_protocol.cpp:15`).  Scan all migrated
      broker test files and update the descriptive header text to
      match the post-migration `HubHostHandle` shape.

---

## Current Focus

### 🔥 Open 2026-05-13: Pattern-3 migration debt — 23 files own in-process `LifecycleGuard`

Audit (2026-05-13) found 23 files in `tests/test_layer{2,3,4}_*` using
the `SetUpTestSuite`-owned `LifecycleGuard` antipattern explicitly
ruled out by `docs/README/README_testing.md` § "Choosing a test
pattern" / "Antipatterns" and `docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md`
§ "Testing implications".  The contract: any test whose body
transitively reaches a lifecycle module MUST run in a worker
subprocess (`IsolatedProcessTest` + `run_gtest_worker`).  Canonical
example: `tests/test_layer2_service/test_role_data_loop.cpp` +
`workers/role_data_loop_workers.cpp`.

**Why this matters now:** the failure mode is "hides the 60 s
static-dtor hang behind 'it passed on my machine'."  Each file in
the list below works today only because each gtest binary has one
suite and exits before static-dtor order matters in the specific
failure modes the antipatterns table calls out.  Adding any new
suite, fixture, or test that re-runs `initialize()` (death tests,
re-entrant lifecycle, etc.) trips the half-init failure mode
immediately.

**Migration unit per file**: move every `TEST_F` body into a worker
function under `workers/<file>_workers.cpp` calling
`run_gtest_worker(lambda, name, Logger::GetLifecycleModule(), …)`;
replace each parent `TEST_F` with `SpawnWorker("file.scenario") +
ExpectWorkerOk(w)`; register the worker dispatcher; add the new
source to the test target's `CMakeLists.txt`.

**Files (sorted by `TEST_F` count, smallest first to validate
mechanics before tackling the big ones):**

| File | TEST_F count |
|---|---|
| `test_layer3_datahub/test_datahub_hub_python_integration.cpp` | 1 |
| `test_layer3_datahub/test_datahub_hub_federation.cpp` | 3 |
| `test_layer3_datahub/test_datahub_hub_host_integration.cpp` | 3 |
| `test_layer3_datahub/test_datahub_broker_consumer.cpp` | 5 |
| `test_layer3_datahub/test_datahub_broker_schema.cpp` | 5 |
| `test_layer3_datahub/test_datahub_zmq_endpoint_registry.cpp` | 5 |
| `test_layer3_datahub/test_datahub_broker_admin.cpp` | 8 |
| `test_layer2_service/test_engine_factory.cpp` | 8 |
| `test_layer4_plh_hub/test_hub_host.cpp` | 10 |
| `test_layer3_datahub/test_datahub_channel_access_policy.cpp` | 11 |
| `test_layer3_datahub/test_hub_lua_integration.cpp` | 11 |
| `test_layer3_datahub/test_datahub_schema_loader.cpp` | 12 plain TEST + 4 TEST_F (**also has mixed-suites violation**) |
| `test_layer2_service/test_log_capture_fixture.cpp` | 13 |
| `test_layer2_service/test_slot_view_helpers.cpp` | 15 |
| `test_layer3_datahub/test_datahub_hub_inbox_queue.cpp` | 16 |
| `test_layer3_datahub/test_datahub_metrics.cpp` | 17 |
| `test_layer3_datahub/test_datahub_zmq_poll_loop.cpp` | 17 (across 2 suites — **two STS guards in one binary**) |
| `test_layer4_plh_hub/test_admin_service.cpp` | 22 |
| `test_layer3_datahub/test_datahub_broker_protocol.cpp` | 23 |
| `test_layer2_service/test_hub_api.cpp` | 23 |
| `test_layer2_service/test_role_host_core.cpp` | 34 |
| `test_layer2_service/test_role_config.cpp` | 47 |
| `test_layer3_datahub/test_datahub_hub_zmq_queue.cpp` | 65 |

**Plus** the two ThreadManager test files touched in MD1.5 work
(`test_layer2_service/test_thread_manager_active_loop.cpp` 17 TEST_F,
`test_layer2_service/test_thread_manager_join_named.cpp` 7 TEST_F) —
both currently use the same antipattern and must migrate first as
prototypes of the mechanics.

**Tracking**: one commit per migrated file (bisectable); update the
table above as each file ships.  Open question for the user: scope
of single session (do we migrate all 25 in one wave, or batch into
phases?).

---

### Open 2026-05-03: PlhRoleCliTest.LogBackupsBelowSentinelRejectedByValidate stalls under parallel load (framework concern)

`PlhRoleCliTest.LogBackupsBelowSentinelRejectedByValidate` passes solo
in ~0.11 s but intermittently times out at the 10 s ctest deadline
when running under `ctest -j2`.  Failure mode: subprocess returns
SIGTERM (exit 143) — `test_process_utils.cpp::wait_for_worker_and_get_exit_code`
hits the deadline (50 ms poll, 10 s budget), sends SIGTERM, then
SIGKILL after a 2 s grace.  Reproduces ~1 in 3 full-suite runs.

**This is NOT a "just flake" — there is no logical reason a 100 ms
test should stall 100× under modest parallel load.**

**D2.1 audit (2026-05-03) — preliminary findings.**  Walked through
the test, fixture, and framework + the `--init` code path:

- *Test code is clean.*  Two-step spawn → `wait_for_exit(10)` for each.
  No shared state between the spawn and the wait.
- *Temp dirs are PID + counter unique* (`make_tmp_dir`), so cross-test
  filesystem path collisions are ruled out.
- *`wait_for_exit` is correct:* polls `waitpid(WNOHANG)` every 50 ms,
  SIGTERM at deadline, SIGKILL after 2 s grace.  No bug there.
- *Solo plh_role suite under `-j2` runs 5/5 clean* — the contention
  is from OTHER suites running concurrently with plh_role tests,
  not within plh_role's own L4 set.
- *`--init` code path:* parses CLI args, then runs the full
  `LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules())`
  with **6 modules**: Logger (spawns worker thread), FileLock,
  CryptoUtils (libsodium init), JsonConfig (flag-flip), ZMQContext
  (`zmq_ctx_new` — spawns IO threads), DataBlock (flag-flip).  Then
  calls `RoleDirectory::init_directory` (writes JSON).  In isolation
  this is well under 100 ms; under heavy parallel load several of
  these can each cost 10–100 ms more.

**Most likely root causes (untested, ranked by probability):**

1. *Process-fork + dlopen overhead.*  Spawning a fresh `plh_role`
   binary loads `libpylabhub-utils.so` + libpython + libsodium +
   libluajit + libzmq.  Under `-j2` with concurrent test binaries
   each doing the same, ld.so + glibc lock contention can multiply
   startup cost by 10×.
2. *Logger worker-thread spawn races with sibling tests' Logger inits.*
   pthread_create takes a runtime-internal lock; multiple processes
   doing it simultaneously serialize.
3. *ZMQ context creation* — `zmq_ctx_new` spawns N IO threads,
   default 1.  Same pthread-internal serialization argument.
4. *Filesystem contention* on `/tmp` during simultaneous file lock
   creation.  tmpfs is usually fine, but kernel inode/lock-block
   contention is observable under stress.

**Next steps for a focused investigation session:**

- *Reproduce reliably:* run the full suite 10× with `-j2` while
  another long-running ctest binary saturates one core (e.g.,
  `test_layer3_datahub` in a tight loop).  Capture stderr from the
  failed `--init` child via `--stop-on-failure --output-on-failure`.
- *Time the binary's startup phases.*  Add a temporary
  `LOGGER_INFO("[plh_role] phase=lifecycle_started", ...)` etc. at
  each phase boundary in `plh_role_main.cpp` and compare wall-clock
  in the slow run vs the fast run.
- *Strace*: `strace -e trace=open,mmap -tt -f plh_role --init ...`
  during a stressed run to see where the binary blocks.
- *Try a smaller lifecycle for `--init` mode* — `--init` only writes
  JSON; it doesn't need ZMQContext or DataBlock or even
  CryptoUtils.  A leaner init-mode lifecycle (Logger + FileLock +
  JsonConfig only) would shave most of the modules and likely make
  the timeout flake disappear.  HEP-0024 §12 hoisted the full
  lifecycle above `--init` for uniform LOGGER_* semantics; perhaps
  there's a middle ground (full lifecycle for run/validate, light
  for init only).

**Action:** schedule a focused 2-hour session to land one of the
above next steps and either fix the root cause or raise the timeout
to 30 s with a recorded justification.  Do NOT mask with a retry
loop — that would mask real regressions in the same shape.

Confirmed flake pattern: D2.1 build (1723 tests) and D2.2 build
(1728 tests) — `ctest -j2` shows this as the sole intermittent
failure in roughly 1-of-3 full runs; solo re-run always passes
in 0.1–0.2 s.

### Open 2026-05-03: Strict must-fire migration in LogCaptureFixture-using tests

`LogCaptureFixture` was extended in HEP-0033 Phase 7 Commit D1.5 (commit
TBD) with strict `ExpectLogWarnMustFire` / `ExpectLogErrorMustFire`
declarations.  These FAIL the test if the declared substring never
matches an emitted WARN/ERROR line — closing the audit §1.1 Class A
gap where `ExpectLogWarn` was a permissive allowlist that silently
accepted "expected warn was not emitted" regressions.

**Migration target.** Existing tests using `ExpectLogWarn` /
`ExpectLogError` should be reviewed and converted to the strict
`MustFire` variants where the declared warn/error is genuinely an
expected production-code emission (i.e. the test is verifying a
specific error path that MUST log).

**Out of scope for migration.** Some tests use `ExpectLogWarn`
intentionally for warns that are RACE-CONDITIONAL (e.g.
timing-dependent ACK-timeout warns that may-or-may-not fire on a
given run).  Those should stay permissive; converting them would
introduce flakes.

**Inventory of tests using `ExpectLog*`** (from grep across `tests/`,
2026-05-03 — review each for must-fire fitness):

  tests/test_layer2_service/test_admin_service.cpp
  tests/test_layer2_service/test_role_host_core.cpp
  tests/test_layer3_datahub/test_datahub_broker_admin.cpp
  tests/test_layer3_datahub/test_datahub_broker_protocol.cpp
  tests/test_layer3_datahub/test_datahub_broker_schema.cpp
  tests/test_layer3_datahub/test_datahub_broker_shutdown.cpp
  tests/test_layer3_datahub/test_datahub_channel_access_policy.cpp
  tests/test_layer3_datahub/test_datahub_hub_host_integration.cpp
  tests/test_layer3_datahub/test_datahub_hub_inbox_queue.cpp
  tests/test_layer3_datahub/test_datahub_hub_zmq_queue.cpp
  tests/test_layer3_datahub/test_datahub_metrics.cpp
  tests/test_layer3_datahub/test_datahub_schema_loader.cpp
  tests/test_layer3_datahub/test_datahub_zmq_poll_loop.cpp

Migration policy: per-file review, per-needle decision.  No bulk
sed — each `ExpectLogWarn` call is a judgment call about whether the
warn is "definitely fires" vs "may-or-may-not fire".  Convert
generously toward strict where the production code path is
deterministic (the test exercises a guaranteed-error input); stay
permissive for race/timeout paths.

Verification per migrated file: build → run → mutation sweep
(deliberately suppress the production warn → matching test must
fail at `ExpectLogWarnMustFire`); restore.  Same protocol as audit
§1.4.  Updated row goes in
`docs/code_review/REVIEW_TestAudit_2026-05-01_inventory.md` (or
successor if archived) with the migration commit hash.

### Closed 2026-04-22: L2 depth review (tracker `21.3.5`)

L2 tier was audited in two passes — first against Pattern-3 discipline and
the REVIEW_FullStack_2026-03-17 gap list, then a second fresh pass with no
preassumptions about what to look for.

**Pass 1 — Pattern-3 compliance + REVIEW_FullStack gap list**:
- `test_logger.cpp::SetRotatingLogfileFailure` had an in-process
  `LifecycleGuard` alongside sibling workers; converted to subprocess
  worker `logger::test_set_rotating_logfile_failure`.
- `HubVaultTest.OpenTruncatedVaultThrows` added for short-file rejection
  (bit-flip case was already covered by `OpenCorruptedVaultThrows`).
- ZMQ context concurrent/double-start: superseded (LifecycleGuard
  framework owns singleton init/shutdown).
- DataBlockMutex `WAIT_ABANDONED`: deferred to platform coverage
  (Windows-robust-mutex-only path; POSIX CI cannot exercise).

**Pass 2 — fresh sweep for assertion quality and vacuous tests**:
- `test_role_directory.cpp::WarnIfKeyfileInRoleDir_*` — 4 tests used only
  `EXPECT_NO_THROW` on a function whose sole observable behaviour is a
  stderr warning.  Rewrote to capture stderr via
  `testing::internal::CaptureStderr` / `GetCapturedStderr` and assert on
  warning content: `_InsideRoleDir_Absolute` and `_RelativePath_Resolved`
  now pin `"PYLABHUB SECURITY WARNING"` + offending keyfile path;
  `_NoWarnWhenEmpty` and `_OutsideRoleDir_NoWarn` now assert stderr is
  empty.  Before the rewrite, deleting the entire function body would
  have still passed all 4.
- `test_crypto_utils.cpp::GetLifecycleModule_ReturnsValidModule` — used
  `SUCCEED() << "returns without crashing"` with no assertion on the
  returned `ModuleDef` (which has no public getters beyond
  `userdata_key()` anyway).  Deleted — `Lifecycle_FunctionsWorkAfterInit`
  exercises the returned module through a real LifecycleGuard and would
  fail at init time if `GetLifecycleModule()` returned a broken module.
- `test_role_host_core.cpp` cross-thread metric test had a redundant
  `EXPECT_NO_THROW(std::get<int64_t>(*final_val))` after the preceding
  `EXPECT_EQ(std::get<int64_t>(*final_val), ...)` — the std::get in the
  EXPECT_EQ line would throw first if the type were wrong, so the second
  check was dead weight.  Deleted, with a comment noting where the
  type-correctness is actually pinned.

**Other findings** (verified non-issues): `EXPECT_NO_THROW` is used
legitimately in install/uninstall idempotency tests (the no-throw IS the
claim under test); `SetUpTestSuite`/`py::scoped_interpreter` scope in
`test_slot_view_helpers.cpp` is correct by pybind11's one-interpreter-per-
process constraint; pure-unit test files (`test_role_cli`, `test_role_host_core`,
`test_role_directory`) correctly run in-process without lifecycle deps;
platform-guarded `GTEST_SKIP` sites are legitimate.

Full L2 tier: **778/778** (net: +1 from HubVault truncated, −1 from
deleted vacuous crypto module test).  Full suite: **1463/1463**.

### Open: Review-deferred items from the test-framework sweep

Items surfaced by the static review before Lua chunk 4 (see commit
`e7f0296` message body for the full review diagnosis). Deferred so
each chunk commit stays a focused refactor+review-and-augment pass
with no structural-cleanup mixed in.

**Policy — delete V2 duplicates in the same commit that lands their P3
replacement** (not at sweep-end).  Rationale: test-registry names
`LuaEngineTest.X` and `LuaEngineIsolatedTest.X` coexist without GTest
complaining, so deferred deletion silently runs duplicate coverage
and confuses later coverage/triage reviews.  **Established
2026-04-19 during chunk-6a cleanup.**

- [x] ~~Rename `LuaEngineIsolatedTest` fixture~~ — **done 2026-04-19** in
  the pre-chunk-6 rename commit. Renamed to `LuaEngineIsolatedTest`;
  all 30 tests across chunks 1-5 and the two in-file doc-banner
  references updated.

- [ ] Delete the V2 `LuaEngineTest` fixture WRAPPER (class body,
  helpers `setup_engine`, `make_api`, `write_script`, and now-unused
  includes `<atomic>`, `<unistd.h>`, `test_patterns.h`) when the
  LAST V2 test inside it is converted. Chunk 6a cleanup
  **2026-04-19** deleted 7 duplicate V2 tests (ApiStop, ApiSetCrit,
  ApiStopReason_Default, Api_CriticalError_Default,
  ApiStopReason_ReflectsPeerDead, ApiStopReason_AfterCriticalError,
  Api_IdentityFields_MatchContext) — the fixture still holds the
  remaining ~65 unconverted V2 tests and stays for later chunks.

**Deferred until both Lua + Python engine conversions complete**:

- [ ] Unify the worker-side `produce_worker_with_script` /
  `consume_worker_with_script` / `process_worker_with_script`
  helper templates. They share ~70% logic today (different required
  callback + different slot-type name + different role tag). A
  single `script_worker(scenario_name, required_cb, tag, slot_regs,
  lua_src, body)` helper would fold all three. Wait until after
  Python (which has an analogous trio) so the helper can be shared
  across both engine test files if that proves sensible. Files:
  `workers/lua_engine_workers.cpp`, `workers/python_engine_workers.cpp`.

**Cross-engine design — Script API live-vs-frozen contract**

Discovered during chunk-12 review (2026-04-20). The api.* surface
exposed to role scripts currently has an **implicit** and
**inconsistent** "live vs frozen" behavior per field, decided
independently per engine at its binding layer.  Example:
`api.log_level` is SNAPSHOTTED at build_api time in all three
engines (Lua field, Python pybind11 member, Native C-struct
`const char*`) — so `api->set_log_level(...)` after build_api does
NOT propagate to running scripts.

**Desired design (Quan, 2026-04-20):**
  - `log_level` → LIVE (scripts can observe `api.log_level`
    changing mid-session when C++ mutates it; intentional for
    runtime verbosity control).
  - `script_dir`, `role_dir`, `logs_dir`, `run_dir` → FROZEN (role
    identity / config; must not change mid-session).
  - Other accessors (`uid`, `name`, `channel`, `stop_reason`,
    `critical_error`, `metrics`, etc.) remain as they are
    (closures = live; all already correct).

**Implementation sketch (scope: cross-engine, ~3 days):**
  1. Spec: add a new section to `docs/HEP/HEP-CORE-0011-*.md` (or
     `docs/README/README_scripting.md`) — a table of every api.*
     field with its mode (FROZEN / LIVE) and rationale.  This is
     the SINGLE AUTHORITATIVE source the 3 engines honor.
  2. Lua engine (`src/scripting/lua_engine.cpp:1192-1212`): promote
     `log_level` from `lua_setfield(..., "log_level")` to
     `push_closure("log_level", ...)` with a closure body reading
     `self->api_->log_level()` live. Keep `script_dir`, `role_dir`,
     `logs_dir`, `run_dir` as fields (frozen).
  3. Python engine: check `pylabhub_producer` / `_consumer` /
     `_processor` embedded-module pybind11 bindings (the role-API
     classes). Expose `log_level` as a method or `@property`
     reading live; keep directories as frozen members.
  4. Native engine (`src/utils/service/native_engine.cpp:302-313`):
     `wire()` caches `log_level` string once then pointer-copies
     `.c_str()` into `ctx.log_level`.  For live semantics, either
     (a) re-populate `ctx.log_level` at every invoke (simplest), or
     (b) change the C-API struct from `const char *log_level` to
     `const char *(*log_level_cb)(void *core)` (callback — ABI
     change).  Option (a) preferred unless ABI versioning already
     supports (b).
  5. Tests: update Lua scripts that read `api.log_level` as a
     field — change to `api.log_level()`.  Chunk-12 test
     `Api_EnvironmentStrings_ReflectSetters` deliberately avoided
     pinning log_level's frozen/live direction; once the design
     lands, strengthen that test to pin LIVE for log_level and
     FROZEN for directories (both directions).
  6. Script migration note: any user scripts reading
     `api.log_level` as a field break at this point.  Document in
     release notes.

**Why this is deferred:**  Making this change mid-test-framework-
sweep would destabilize the 773 L2 tests we just stabilized, and
requires a coordinated spec-first design pass.  Proper workstream
after sweep completes.

**Coverage gaps from the review** (add in whichever chunk touches
the same area, or a dedicated "error-paths" chunk after the
callback chunks):

- [x] ~~Test: `invoke_consume` returning Discard~~ — **done 2026-04-22**
  (commit `fffd095`).  `InvokeConsume_DiscardOnFalse_NoErrorBump` pins
  `return false → Discard` + no script_error_count bump across 10
  iterations.  Parallel `InvokeOnInbox_DiscardOnFalse_NoErrorBump`
  added for the inbox path.

- [x] ~~Tests: `engine.load_script` error paths~~ — **done 2026-04-20**
  in chunk 7b.  Three P3 tests landed (missing file, missing required
  callback, syntax error).  Each strengthened with "engine is
  reusable after failure" retry.  Same gap still open for Python;
  convert alongside Python chunk 7b.

- [x] ~~Test: `engine.finalize()` idempotence~~ — **done 2026-04-20**
  in chunk 7b.  P3 test `Finalize_DoubleCallIsSafe` pins:
  double-finalize is a no-op (no crash, no throw), post-finalize
  `is_accepting()` returns false, and post-finalize invoke returns
  `InvokeResult::Error` (not a crash).

- [x] ~~Test: `RegisterSlotType_CustomName_*`~~ — **done 2026-04-22**
  (commit `fffd095`, reframed from original premise).  The TODO was
  stale: `register_slot_type` was since hardened (lua_engine.cpp:660-671)
  to reject unknown canonical names up front with ERROR + false return,
  so the "fall-through" scenario no longer exists.  New test
  `RegisterSlotType_UnknownName_RejectsWithNoSideEffect` pins current
  behaviour (false return + ERROR log + `type_sizeof == 0` proving no
  FFI side effect + engine remains usable for subsequent valid
  registrations).

- [x] ~~Test: rx-write-in-invoke_on_inbox~~ — **done 2026-04-22** (commit
  `fffd095`).  `InvokeOnInbox_DataReadonly_WriteFailsAndBufferUnchanged`
  pins the three coupled invariants (buffer unchanged, `InvokeResult::
  Error`, `script_error_count == 1`) against the InboxFrame const* chain
  (lua_engine.cpp:726 → lua_state.cpp:291 → lua_engine.cpp:1023).

**Cross-cutting cleanup deferred to end of entire L2 sweep**:

- [x] ~~Trim redundant `GTest::gmock` link additions~~ — **done 2026-04-22**
  (commit `469dfe4`).  7 of 15 targets had explicit `GTest::gmock` even
  though `pylabhub::test_framework` already exports it PUBLIC.  Trimmed
  the 7 redundant ones (shared_memory_spinlock, crypto_utils,
  role_logging_roundtrip, configure_logger, lua_engine, python_engine,
  metrics_api).  Kept on 8 targets whose source or linked worker
  actually uses gmock matchers.

- [x] ~~Grep for stale test-name references across the repo~~ —
  **done 2026-04-22**. Searched all `.cpp|.h|.hpp|.md|.txt|.sh|.py|.cmake`
  for the 5 renamed test names (`LuaEngineTest.InitializeFailsGracefully`
  and siblings): **zero external references**. Chunk 13 cleanup was
  thorough. Also searched for retired class names
  (`LuaProducerHost` / `LuaConsumerHost` / `LuaProcessorHost`) and
  legacy per-role binary targets (`pylabhub-producer` / `-consumer` /
  `-processor`): only leftover references are in
  `tests/test_layer4_integration/CMakeLists.txt` inside an `if(FALSE)`
  block (held for HEP-0033 reference). Also found one stale comment in
  `role_api_flexzone_workers.cpp` referencing the pre-split
  `role_api_loop_policy_raii_workers.cpp` — fixed in the same commit.

- [x] ~~Final audit re-grep after L2+L3 conversions land~~ —
  **done 2026-04-22**. L2+L3 conversions completed this session
  (Pattern-3 compliance audit + depth review closed); re-grep above
  covers the full repo and found no stale patterns.

### Open: Stress-level calibration across converted Pattern-3 tests

The test framework already exposes `STRESS_TEST_LEVEL` (via
`shared_test_helpers.h`: `get_stress_num_threads()`,
`get_stress_num_writers()`, `get_stress_num_readers()`,
`get_stress_duration_sec()`, `get_stress_iterations(high, low)`).
Several race-hunting tests converted in the test-framework sweep
kept the originals' hardcoded thread/iter counts instead of
adopting the helpers, so CI defaults run hotter than Low-stress
values and developers running `-DSTRESS_TEST_LEVEL=High` don't get
the amplification the helpers would provide.

Deferred here (not in the converting-sweep commits) so each
sweep commit stays a pure refactor with no semantic change.

Affected tests that should adopt the helpers:

| File | Test | Current | Should be |
|---|---|---|---|
| `workers/filelock_singleprocess_workers.cpp` | `multi_threaded_contention` | `kThreads = 10` | `get_stress_num_threads()` |
| `workers/role_config_workers.cpp` | `multi_thread_file_contention` | 16 threads × 25 iters | `get_stress_num_threads()`, `get_stress_iterations(25, 8)` |
| `workers/role_config_workers.cpp` | `multi_thread_shared_object_contention` | 4 writers + 8 readers × 1s duration | `get_stress_num_writers()`, `get_stress_num_readers()`, duration scaled from `get_stress_duration_sec()` (divide by a constant to keep sub-second at Low) |
| `test_filelock.cpp` | `MultiProcessBlockingContention` | 8 procs × 100 iters | `get_stress_num_threads()` capped at a hard max (process count has real IPC overhead), `get_stress_iterations(100, 25)` |

When done: run `ctest -j2 --repeat until-pass:3 -L layer2` at
each stress level (Low / Medium / High) to confirm no new flakes.

### Open: Native engine — user-settable `supports_multi_state()` (2026-04-20)

Static-review Finding #7 flagged `NativeEngine::invoke()`/`eval()`
as missing an `is_accepting()` gate for cross-thread admin calls.
User decision 2026-04-20: **native is not single-state by default**
— the user fully manages its own threading in the plugin.
`supports_multi_state()` should become a user-settable flag on
NativeEngine (default `false`, can be overridden via a plugin
export, e.g. `native_supports_multi_state`).  Native's missing
`is_accepting()` gate on generic invoke stays open until the
user-settable threading policy is wired — they're the same work
item.

Design rationale (per user): native is designed to fully exploit
C/C++ capacity and the user owns the threading-model decision.
Simplification/restriction is the user's call, not the framework's.

Implementation sketch:
  1. Add optional symbol `native_supports_multi_state()` → bool,
     resolved during load_script (like `native_is_thread_safe`).
  2. `NativeEngine::supports_multi_state()` returns the resolved
     value (default false if symbol not exported).
  3. `invoke()`/`eval()` gate on `is_accepting()` and — if
     `!supports_multi_state()` — reject non-owner-thread calls
     (match Lua/Python's queued-dispatch model on Python side,
     or match Lua's thread-local state model — user choice).
  4. HEP-CORE-0028 updates: document the optional symbol and
     its semantics.

### Resolved (no action) — type_sizeof aliases on Native (2026-04-20)

Static-review Finding #15 flagged inconsistency: `type_sizeof(
"SlotFrame")` returns a valid size on Lua/Python but 0 on Native.
User decision 2026-04-20: **no action**.  Native focuses on
compile-time type binding (templates, direct struct definitions)
rather than runtime name-table lookup.  Alias handling is a
scripting-engine convenience (dynamic name resolution) that doesn't
map onto native's design point.

HEP-CORE-0028 already lists the 5 canonical names as the native
registration targets.  No doc change needed.

### Resolved (no action) — has_callback for non-canonical names on Native (2026-04-20)

Static-review Finding #14 flagged inconsistency: Lua/Python's
`has_callback("custom_hook")` probes for arbitrary names via
`lua_getglobal`/`py::getattr`; Native returns false because
`type_sizes_` only holds canonical entries.  User decision
2026-04-20: **no action — restrict to canonical names at the core
API level**.

The native engine's plugin is free to `dlsym` arbitrary internal
functions via C/C++; the framework just doesn't expose non-
canonical symbols through the core API.  This matches the
full-type / dynamic-lib loading check model.

HEP-CORE-0011's `has_callback` contract should be documented to
say "canonical names only" when the HEP gets a refresh (deferred).

### Resolved (no action) — inbox_cache_ "duplication" claim (2026-04-20)

Static-review Finding #20 claimed the per-Python-API `inbox_cache_`
members duplicate `RoleHostCore::inbox_cache_` and should be
consolidated.  **Incorrect reading** — confirmed 2026-04-20.  These
are two layered caches holding different things:

  - **Core (`RoleHostCore::inbox_cache_`)** — engine-agnostic
    discovery: `{uid → shared_ptr<InboxClient> + SchemaSpec +
    item_size}`.  Reused by all engines via the core API path.
  - **Python API (`ProducerAPI/ConsumerAPI/ProcessorAPI::inbox_cache_`)**
    — pybind11-specific: `{uid → py::object wrapping InboxHandle
    with ctypes Structure type}`.  Caches the expensive-to-build
    Python ctypes type.  Cannot be shared with Lua (which uses FFI
    cdef refs in LuaJIT's global FFI registry) or Native (which
    uses compile-time types).

User decision (Quan) 2026-04-20: no consolidation.  Native
correctly has no inbox_cache_ — "native is directly called, no api
layer per se."  Only possible LOW-priority DRY nit: extract a
shared pybind11 open_inbox helper across the three Python API
classes (~40 identical lines × 3).  Not a design defect.

### Open: Missing-callback script_error_count wiring (2026-04-20, updated post-F11)

Post-F11 state (commit adding LOGGER_ERROR on is_accepting()):

| Engine | Missing-callback return | ERROR log | `script_error_count++`? |
|---|---|---|---|
| Lua | `Error` | ✅ if is_accepting() | ❌ no |
| Python | `Error` | ✅ if is_accepting() | ❌ no |
| Native | `Error` | ✅ if is_accepting() | ❌ no |

The log + Error return cover 2 of 3 observable channels.  The
counter increment is the remaining gap — script_error_count still
stays 0 after a dispatch-bug invoke, so metrics collectors won't
see the anomaly.

Options when we pick this up:

1. **Add the counter increment** uniformly (preferred — ships with
   the native error-accounting design).  Pairs naturally with
   stop_on_script_error: the dispatch-bug path should also honor
   the shutdown policy since it's a programmer bug, not a script
   contract violation.
2. **Use `assert`** instead, treating the path as an invariant
   violation (silent in release — weaker than option 1).
3. **Do nothing** — keep log + Error as the signal (current state).

Decision deferred per user direction 2026-04-20 ("error counts can
be added later"): revisit when native error-accounting (review
Finding #3) is designed.  HEP-CORE-0011 § "Error Handling & Log
Conventions" (added 2026-04-20) documents the current state.

### Deferred: System-level L4 tests + hub-facing L3 Pattern-3 conversion — owned by HEP-CORE-0033

Two categories of test work are folded into the HEP-0033 Hub Character
refactor rather than tracked standalone here:

1. **System-level L4 integration tests** (run-mode lifecycle, broker
   round-trip, channel broadcast, processor pipeline, hub-dead
   detection, inbox round-trip). They test the full system including
   the hub binary, which is HEP-0033 work. Become writable at Phase 9
   when `plh_hub` lands. HEP-0024 closes with the no-hub-tier suite at
   `tests/test_layer4_plh_role/` (71 tests: `--init` / `--validate` /
   `--keygen` / CLI error paths).

2. **Pattern-3 conversion of 6 hub-facing L3 tests** (folded in from the
   retired `21.L5` test-harness tracker 2026-04-22): `hub_config_script`,
   `hub_zmq_queue`, `hub_inbox_queue`, `zmq_endpoint_registry`, `metrics`,
   `hub_federation`. Each exercises a hub-owned subsystem that HEP-0033
   will rewrite or heavily touch; lifecycle conversion is coupled with
   the refactor and is best done as tests are rebuilt against the new
   shape rather than chased through the old one. The two role-side files
   originally in this tracker (`test_datahub_role_flexzone.cpp` and
   `test_datahub_loop_policy.cpp`) were converted during HEP-0024 closure
   and are no longer part of this work.

**Authoritative tracker for both categories**: `docs/todo/MESSAGEHUB_TODO.md`
→ "Open: HEP-CORE-0033 Hub Character refactor".

`tests/test_layer4_integration/test_admin_shell.cpp` (disabled via
`if(FALSE)`) is preserved as reference material until the new L4
tests are written — it covers the admin-shell ZMQ protocol the hub
refactor will likely change.

### Phase C: Integration Tests
**Status**: ✅ Complete (424/424 as of 2026-02-19; suite grown to **1181/1181** by 2026-03-30)

- [x] **MessageHub and broker tests** – Phase C broker integration + consumer registration complete
- [x] **Multi-process IPC tests** – Producer/consumer across process boundaries (E2E test)
- [x] **hub::Producer + hub::Consumer active API** – 15 tests; HELLO/BYE tracking, SHM callbacks, ctrl messaging, idempotency, destructor-BYE regression
- [ ] **Cross-platform consistency** – Run same tests on Linux (done), Windows, macOS, FreeBSD

### New Gaps Discovered (2026-03-30)

- [x] ~~**ZMQ checksum policy execution tests**~~ — **covered 2026-04-22**.
  `test_datahub_hub_zmq_queue.cpp` has `ChecksumEnforced_Roundtrip`
  (enforced round-trip matches), `ChecksumManual_NoStamp_ReceiverRejects`
  (uncheck-summed frame → `checksum_error_count` increments), and
  `ChecksumNone_Roundtrip` (None path works).  Corruption-in-flight is
  not applicable to in-process ZMQ frames (immutable post-send).
- [x] ~~**Config key whitelist edge case tests**~~ — **done 2026-04-22**
  (commit `fffd095`).  Top-level exact-match whitelist makes
  empty/unicode/prefix cases trivially safe (std::unordered_set lookup).
  The real gap was nested keys: 4 sub-parsers (script/auth/identity/startup)
  accepted unknown inner keys silently.  Fixed with validators + test
  `NestedUnknownKey_Throws` covering 5 probes (script typo, role-tag
  typo, auth typo, startup typo, startup-entry typo).

### Schema/Packing Round-Trip Coverage Gap (closed 2026-04-22)

- [x] ~~L3 gap: aligned-packing round-trip with padding-sensitive schema~~ —
  **done 2026-04-21**. `ShmRoundTrip_PaddingSensitive` in
  `tests/test_layer3_datahub/test_role_api_flexzone.cpp` writes
  `{float64 ts, uint8 flag, int32 count}` aligned (16 bytes, 3-byte pad)
  and asserts bit-exact read-back at layout-computed offsets.
- [x] ~~L3 gap: SHM round-trip with complex schema~~ — **done 2026-04-21**.
  `ShmRoundTrip_AllTypes` (13 fields across every scalar type,
  56 bytes aligned) and `ShmRoundTrip_ArrayField` (uint32 + float64[2])
  close this at the role-API boundary. Both pin `compute_schema_size`
  totals against the documented aligned/packed values.
- [x] ~~L3 gap: No aligned-vs-packed same-data comparison~~ — **done
  2026-04-22** (commit `fffd095`).  `Packing_SameLogicalData_RoundtripsBitExactInBothModes`
  in `test_datahub_hub_zmq_queue.cpp` writes `{uint8 flag, int64 value}`
  through aligned (16B, offset 8) and packed (9B, offset 1), asserts
  item_size differs per packing, values round-trip bit-exact, and pins
  the `write_acquire` zero-init invariant (hub_zmq_queue.cpp:823) that
  Enforced-checksum round-trip depends on.

### BrokerProtocolTest Timing Audit (2026-03-23; closed 2026-04-22)

- [x] ~~Audit + conversion~~ — **done 2026-04-22** (commit `d0ef300`).
  Audited 8 broker-test files (54 tests); 5 were already SOLID
  (worker-subprocess + pinned substrings).  11 sites across 3 files
  reclassified case-by-case:
    - 8 sites converted to `poll_until` on the actual observable
      (channel removed, status transition, notification counter).
    - 1 site removed entirely (padding between two deregs with no
      intermediate state to observe).
    - 2 sites retained the sleep (legitimate silent-async-processing
      windows where no event fires on the happy path —
      `ChecksumErrorReport_UnknownChannel_Silent` and
      `CloseChannel_NonExistent`) but their vacuous `(void)snap`
      assertions were upgraded to real `EXPECT_NO_THROW` liveness
      probes.
  Full suite: 1470/1470; broker-suite runtime unchanged (poll_until
  returns on condition, so padded 300-500ms intervals no longer
  contribute wall-clock cost).

### Phase D: High-Load and Edge Cases
**Status**: 🔵 Partial — RAII stress tests added; extended/platform tests deferred

- [x] **RAII multi-process ring-buffer stress** — `DatahubStressRaiiTest` (tests 423–424):
  - `MultiProcessFullCapacityStress`: 500 × 4KB slots, ring=32 (15 wraparounds), 2 racing consumers,
    enforced BLAKE2b + app-level XOR-fold, random 0–5ms write / 0–10ms read delays
  - `SingleReaderBackpressure`: 100 slots, ring=8, consumer 0–20ms delays force producer to block
- [ ] **High-load extended stress** – Hours-long soak tests; multiple producers simultaneously
- [ ] **Edge case scenarios** – Wraparound at 2^64, slot_id rollover, capacity exhaustion
- [ ] **Broker-coordinated recovery** – Cross-process zombie detection (blocked on broker protocol extension)
- [ ] **Slot-checksum in-place repair** – Blocked: existing repair reinitialises header; needs WriteAttach approach

### Code Review REVIEW_FullStack_2026-03-17 — Testing gaps

- [x] ~~**PARITY-01 HIGH: No Lua role integration tests**~~ — **retired
  2026-04-22 (superseded by HEP-0033 scope)**. Original framing named
  `LuaProducerHost` / `LuaConsumerHost` / `LuaProcessorHost` classes
  that no longer exist (eliminated in HEP-CORE-0024). Post-unification
  there is one `RoleAPIBase` wrapping a `ScriptEngine` plugin; a
  Lua-backed run-mode test is just `plh_role --role <tag>` with
  `script.type=lua`. That test infrastructure requires a hub binary
  to broker the channel — not yet available (HEP-0033 Phase 9). When
  HEP-0033 system-level L4 tests (`plh_role + broker round-trip`,
  `plh_role cross-role processor pipeline`, etc.) land, parametrize
  them by `script.type` so Python + Lua + Native all get coverage.
  Authoritative tracker: `docs/todo/MESSAGEHUB_TODO.md` under the
  HEP-0033 refactor.
- [x] ~~**L0 gap: No `uuid_utils` unit tests**~~ — **already covered**.
  `tests/test_layer2_service/test_uid_utils.cpp` has 18 tests (L2, not
  L0 as originally filed). The item was stale at filing time.
- [x] ~~**L0 gap: No `bytes_to_hex`/`bytes_from_hex` tests**~~ —
  **already covered**. `tests/test_layer0_platform/test_uuid_and_format.cpp`
  tests empty / single-byte / multi-byte / case-insensitivity /
  invalid hex / round-trip. Item was stale at filing time.
- [x] **L2 gap: No vault corruption detection test** — `OpenCorruptedVaultThrows` (bit-flip in ciphertext) and `OpenTruncatedVaultThrows` (short-file rejection) both present in `test_hub_vault.cpp` (2026-04-22 depth review).
- [ ] **L2 gap: ZMQ context concurrent start/stop, double-start** — superseded by LifecycleGuard framework tests (singleton init/shutdown is Lifecycle's concern, not a ZMQContext-layer test); keeping the note to revisit if the ZMQContext ever grows independent init/shutdown semantics.
- [ ] **L2 gap: No DataBlockMutex WAIT_ABANDONED test** for Windows robust mutex path — POSIX build can't exercise this; needs a Windows CI runner. Tracked separately as a platform-coverage item.

### Watchlist: ShmQueueWriteFlexzone intermittent timeout (2026-03-16)

- [ ] **DatahubShmQueueTest.ShmQueueWriteFlexzone** — intermittently times out at 60s under
  `-j2` but passes instantly in isolation. Deep analysis (2026-03-16) confirmed: test logic is
  entirely non-blocking (create DataBlockProducer, call write_flexzone(), assert non-null);
  shared_secret 70007 is unique; channel name has nanosecond timestamp; no SHM contention
  possible. **Likely root cause identified:** uncapped ThreePhaseBackoff Phase 3
  (`iteration * 10us` with no ceiling) could grow to multi-second sleeps if SharedSpinLock
  contention occurred during LifecycleGuard shutdown under parallel load. Fixed by capping
  Phase 3 at 10ms (kMaxPhase3DelayUs, commit 1d3e584). **If this recurs after the cap fix,**
  investigate: (1) Logger cv_.notify_one miss, (2) fork/exec scheduling starvation,
  (3) whether the *same* test consistently fails or different tests rotate.
  - Also seen on `DatahubSlotDrainingTest.DrainHoldTrueNeverReturnsNullptr` (same session).
  - Also seen on `DatahubHeaderStructureTest.SchemaHashesPopulatedWithTemplateApi` (2026-03-18,
    exit code 143 = SIGTERM after 60s, child stderr shows test lambda completed successfully;
    possible lifecycle teardown hang under `-j2` load). Failed once, passed 10/10 in isolation
    and 1/1 on full-suite rerun. Same class of issue: non-reproducing under load.

### Codex Review: Testing docs staleness (2026-03-15)

- [x] ~~**README_testing.md stale** — Phase C still said "To be implemented"~~ —
  **done 2026-04-22** (commit `469dfe4`).  Coverage table rewritten to
  reflect current state: Phase C (MessageHub + broker) Yes, Phase D
  (concurrency/multi-process) Yes, role-API integration row added,
  scenario matrix extended with bidirectional flexzone + aligned-vs-packed
  coverage.  Summary paragraph distinguishes in-process broker tests
  (done) from hub-binary-dependent L4 tests (HEP-0033 scope).  Platform
  claim still needs narrowing separately (tracked under PLATFORM_TODO
  Codex review).

---

## Test Phase Checklist

### Phase A: Protocol/API Correctness ✅
- [x] Flexible zone access (empty when no zones, populated when configured)
- [x] Checksum validation (false when no zones, true when valid)
- [x] Consumer config matching (expected_config validation)
- [x] Schema validation tests

### Phase B: Slot Protocol (Single Process) ✅
- [x] Write/read basic flow
- [x] Checksum enforced mode
- [x] Layout smoke test (checksum + flexible zone)
- [x] Diagnostic handle access
- [x] Error handling (timeouts, bounds, double-release)

### Phase C: Integration (Multi-Process)
- [x] Basic producer/consumer IPC
- [x] ConsumerSyncPolicy variants (Latest_only, Sequential, Sequential_sync)
- [x] High-load single reader integrity test
- [x] MessageHub broker integration ✅ complete (2026-02-18)
- [x] Consumer registration to broker ✅ complete (2026-02-18)
- [ ] Cross-process recovery scenarios (broker-coordinated; facility-layer tests ✅ done separately)

### Phase D: High-Load and Edge Cases
- [ ] Extended duration stress tests (hours)
- [ ] Multiple producers, multiple consumers
- [ ] Slot wraparound at 2^64
- [ ] Capacity boundary conditions
- [ ] Race condition scenarios
- [ ] Platform-specific behavior verification

---

## Test Infrastructure

### Multi-Process Test Framework ✅
- [x] Worker process pattern established
- [x] ExpectWorkerOk with stderr validation
- [x] Lifecycle management in workers
- [x] Test framework shared utilities

### Platform Coverage
- [x] **Linux** – Primary development platform; 1181/1181 tests ✅
- [ ] **Windows** – Build and test (basic coverage)
- [ ] **macOS** – Build and test (basic coverage)
- [ ] **FreeBSD** – Build and test (pending)

### Sanitizer Coverage
- [x] **ThreadSanitizer** – Enabled, passing (except known EOWNERDEAD false positive)
- [ ] **AddressSanitizer** – Enable and verify
- [ ] **UndefinedBehaviorSanitizer** – Enable and verify

---

## Coverage Gaps

### Layer 4: pylabhub-producer Tests ✅ Complete (2026-03-02)
**Status**: ✅ 14 tests passing — `tests/test_layer4_producer/`

#### Config unit tests (no lifecycle, no Python) — 8 tests
- [x] **ProducerConfig FromJsonFile_Basic** — all fields parsed; uid, name, channel, interval_ms, shm, script, validation
- [x] **ProducerConfig FromJsonFile_UidAutoGen** — PROD- prefix auto-generated when uid absent
- [x] **ProducerConfig FromJsonFile_SchemaFields** — slot_schema + flexzone_schema field names, types, count
- [x] **ProducerConfig FromJsonFile_MissingChannel** — throws std::runtime_error
- [x] **ProducerConfig FromJsonFile_MalformedJson** — throws std::runtime_error
- [x] **ProducerConfig FromJsonFile_FileNotFound** — throws std::runtime_error
- [x] **ProducerConfig FromDirectory_Basic** — resolves script_path to absolute path
- [x] **ProducerConfig StopOnScriptError_DefaultFalse** — default false; update_checksum default true

#### CLI integration tests (binary invoked via WorkerProcess) — 6 tests
- [x] **`--init` creates structure** — `producer.json`, `script/python/__init__.py`, `vault/`, `logs/`, `run/`
- [x] **`--init` default values** — uid has PROD- prefix, script.path=".", stop_on_script_error=false
- [x] **`--keygen` writes keypair** — creates vault file; stdout "Producer vault written to" + public_key
- [x] **`--validate` exits 0** — loads Python script, prints "Validation passed.", exits 0
- [x] **Config malformed JSON** — "Config error" in stderr, non-zero exit
- [x] **Config file not found** — stderr non-empty, non-zero exit

Note: `--keygen` parent-dir-creation and overwrite-entropy tests deferred to backlog.

### Layer 4: pylabhub-consumer Tests ✅ Complete (2026-03-02)
**Status**: ✅ 12 tests passing — `tests/test_layer4_consumer/`

#### Config unit tests — 6 tests
- [x] **ConsumerConfig FromJsonFile_Basic** — all fields parsed; uid, name, channel, timeout_ms, shm, script, validation
- [x] **ConsumerConfig FromJsonFile_UidAutoGen** — CONS- prefix auto-generated
- [x] **ConsumerConfig FromJsonFile_SchemaFields** — slot_schema + flexzone_schema field names, types
- [x] **ConsumerConfig FromJsonFile_MissingChannel** — throws std::runtime_error
- [x] **ConsumerConfig FromJsonFile_MalformedJson** — throws std::runtime_error
- [x] **ConsumerConfig FromDirectory_Basic** — resolves script_path to absolute path

#### CLI integration tests — 6 tests
- [x] **`--init` creates structure** — `consumer.json`, `script/python/__init__.py`, `vault/`, `logs/`, `run/`
- [x] **`--init` default values** — uid has CONS- prefix, script.path=".", stop_on_script_error=false
- [x] **`--keygen` writes keypair** — creates vault file; stdout "Consumer vault written to" + public_key
- [x] **`--validate` exits 0** — loads Python script, prints "Validation passed.", exits 0
- [x] **Config malformed JSON** — "Config error" in stderr, non-zero exit
- [x] **Config file not found** — stderr non-empty, non-zero exit

### Layer 4: pylabhub-actor Tests — ARCHIVED (actor eliminated 2026-03-01)

`pylabhub-actor` and its test suite (`tests/test_layer4_actor/`) were removed from the build
and deleted from disk on 2026-03-02 (HEP-CORE-0018 decision). The completed unit tests
(config parsing, role metrics, CLI keygen/register-with, 98 tests) are preserved in git history.
Replaced by standalone `pylabhub-producer` + `pylabhub-consumer` + `pylabhub-processor` binaries.

LoopPolicy C++ metrics tests (HEP-CORE-0008) are fully covered in
`tests/test_layer3_datahub/test_datahub_loop_policy.cpp` (tests 6–16, secrets 80006–80016).

### ScriptHost / PythonScriptHost threading model (tests done — 2026-02-28)
**Status**: ✅ Complete — 10 tests in `tests/test_layer2_service/test_script_host.cpp`

- [x] **ScriptHost base threading** — threaded startup/shutdown, idempotent shutdown, early-stop
- [x] **ScriptHost exception in do_initialize** — exception propagated via future to caller
- [x] **ScriptHost returns false without signal** — set exception on promise; base_startup_ throws
- [x] **ScriptHost direct mode (Lua path)** — do_initialize on calling thread; signal_ready by base

### PythonInterpreter / Admin Shell / Consumer ctypes ✅ Complete (2026-03-05)
**Status**: ✅ All three items covered by existing L4 integration tests.

- [x] **HP-C1 — `pylabhub.reset()` deadlock regression** — `test_admin_shell.cpp:271–338`:
  `HP_C1_Reset_NoDeadlock` (no hang) + `HP_C1_Reset_ClearsNamespace` (vars cleared, builtins preserved).
- [x] **HP-C2 — stdout/stderr leak on exec() exception** — `test_admin_shell.cpp:342–394`:
  `HP_C2_Exception_StdoutRestored` (output works after exception) + `HP_C2_Exception_ErrorReturned`.
- [x] **BN-H1 — Consumer binary ctypes.from_buffer_copy round-trip** — `test_pipeline_roundtrip.cpp`:
  consumer reads `in_slot.counter` and `in_slot.doubled` (ctypes fields from `from_buffer_copy()`)
  and verifies transformation correctness (`doubled == counter * 10.0 * 2.0`).

### HubConfig script-block fields (tests done — 2026-02-28; **superseded 2026-04-29**)
**Status**: ⚪ Retired with the legacy singleton (2026-04-29). The 9 tests below
covered `pylabhub::HubConfig`, which was deleted with its file. Coverage for
the new `pylabhub::config::HubConfig` (HEP-0033 §6.1) lives in
`tests/test_layer2_service/test_hub_config.cpp` (Pattern 3, 9 tests).

- [x] **`hub_script_dir()` from JSON** — resolves to `<hub_dir>/my_script/python`
- [x] **`script_type()` from JSON** — reads `"type"` field correctly
- [x] **`tick_interval_ms()` override** — `"tick_interval_ms":500`; verify 500
- [x] **`health_log_interval_ms()` override** — `"health_log_interval_ms":30000`; verify 30000
- [x] **`hub_dir()` matches config path parent** — lifecycle sets hub_dir correctly
- [x] **`tick_interval_ms()` default** — omit key; verify 1000 ms default
- [x] **`health_log_interval_ms()` default** — verify 60000 ms default
- [x] **`hub_script_dir()` default absent** — no `"script"` block; verify empty path
- [x] **`script_type()` default absent** — no `"script"` block; verify empty string

### High Priority
- [x] Consumer registration to broker — ✅ done (test_datahub_broker_consumer.cpp)
- [x] Broker schema registry tests — ✅ done (test_datahub_broker_schema.cpp, 7 tests, HEP-CORE-0016 Phase 3)
- [ ] MessageHub error paths with broker
- [ ] Recovery: cross-process zombie detection (broker-coordinated) — requires broker protocol
- [ ] Recovery: slot-checksum in-place repair (current repair path reinitialises header; needs WriteAttach mode instead of create)

### Medium Priority
- [ ] Flexible zone by-name access edge cases
- [ ] Transaction API exception safety (comprehensive)
- [ ] Diagnostic API comprehensive coverage

### Low Priority
- [ ] stuck_duration_ms in diagnostics (requires timestamp on acquire)
- [ ] Schema versioning and compatibility rules
- [ ] Migration path testing
- [ ] **FlexZone atomic-ref usage example** — test demonstrating `std::atomic_ref<T>` pattern
  for out-of-transaction lock-free FlexZone access (from RAII_LAYER_TODO backlog)
- [ ] **Layout checksum stability** — attach with mismatched layout (should fail); structured
  buffer alignment test (8-byte, 16-byte types); large flex zone (multi-page); zero flex zone
  (slots only) — (from MEMORY_LAYOUT_TODO backlog; low priority, layout is stable)
- [ ] **RAII move semantics audit** — confirm all RAII handles support efficient move (no copy)
- [ ] **Zero-cost abstraction verification** — profile `with_transaction` vs primitive API
  with optimizations; confirm no overhead on hot path

---

## Recent Completions

### 2026-03-30 (Metrics/Timing/Checksum unification session)
- ✅ ContextMetrics all-atomic + ZmqQueue adoption, X-macro adapters (JSON/pydict/Lua)
- ✅ LoopTimingParams strict validation, `loop_timing` required, DataBlock timing removal
- ✅ Checksum policy unification (per-role config, unified queue API, inbox support)
- ✅ Config key whitelist validation
- ✅ Consumer inbox registration, ROLE_INFO_REQ both entries
- **Total: 1181/1181 tests passing**

### 2026-03-12 (MR-01 wire dedup + MR-09 is_running + LOW-2 deprecated remap stubs)
- ✅ **MR-01**: `zmq_wire_helpers.hpp` internal header; `hub_zmq_queue.cpp` + `hub_inbox_queue.cpp` deduped (~260 lines removed)
- ✅ **MR-09**: `ShmQueue::is_running()` override — returns false on moved-from (null pImpl) instance; `DatahubShmQueueTest.ShmQueueIsRunning` added
- ✅ **LOW-2**: `[[deprecated("Not implemented — always throws std::runtime_error")]]` on 4 DataBlock remap stubs; `DatahubShmQueueTest.DataBlockProducerRemapStubsThrow` + `DatahubShmQueueTest.DataBlockConsumerRemapStubsThrow` added
- ✅ **MR-08**: stale `run_loop_shm_` reference removed from `consumer_script_host.hpp` comment
- **Total: 1120/1120 tests passing** (3 new)

### 2026-03-10 (ProcessorConfig/ScriptHost Phase 3 + InboxQueue + ShmQueue + API parity)
- ✅ **ProcessorConfig Phase 3 tests** (+24 tests): `target_period_ms`/`loop_timing`, `in_transport`/`zmq_in_endpoint`, `in_zmq_buffer_depth`/`out_zmq_buffer_depth`, inbox fields, `verify_checksum`, `script_path` default, timing-policy cross-field validation → **1045/1045 tests**
- ✅ **9 L3 ShmQueue test scenarios** (`test_datahub_hub_queue.cpp`): multiple_consumers, flexzone_round_trip, ref_factories, latest_only, ring_wrap, destructor_safety, last_seq monotonic, capacity_policy, verify_checksum_mismatch → **988/988**
- ✅ **Consumer inbox config tests** (+4): ConsumerConfig `inbox_schema_json`, `inbox_endpoint`, `inbox_buffer_depth`, `inbox_zmq_packing`
- ✅ **InboxQueue per-sender seq fix** (A11/A18): `unordered_map<string,uint64_t>` per sender_id
- ✅ **LoopTimingPolicy rename tests** (+10): MaxRate/FixedRate/FixedRateWithCompensation cross-field validation → **1021/1021**
- ✅ **FullStack2 config tests** (+15 A6): `verify_checksum` field (ConsumerConfig), `heartbeat_interval_ms` (both), `zmq_buffer_depth` (ConsumerConfig), `inbox_overflow_policy` (both) → **1011/1011**

### 2026-04-03 (Schema validation coverage audit)
- ✅ **L2 schema validation** (+22 tests): parse_schema_json good/error paths, compute_schema_size all 13 types, to_field_descs correctness → **1303/1303**
- ✅ **Engine has_schema=false** (+2 tests): Python + Lua register_slot_type returns false
- ✅ **Bug fix**: open_inbox_client item_size computed by summing f.length (wrong for numeric types). Fixed to use compute_schema_size(spec, packing). Same bug fixed in both RoleAPIBase and ScriptEngine.
- [x] InboxQueue schema mismatch: DifferentType + DifferentSize (both drop frame) ✅
- [x] NativeEngine register_slot_type has_schema=false ✅
- [x] open_inbox_client full broker path: complex 6-field schema, send+recv with field verification ✅
- [x] ShmQueue create_writer with zero-size schema (bytes length=0) → nullptr ✅
- [x] SHM + ZMQ packed roundtrip: 6-field schema with packed packing ✅
- [x] Checksum Enforced + complex 6-field schema round-trip ✅
- [x] Engine register_slot_type packed packing (Python + Lua): bool+int32 packed=5B ✅
- [ ] **L4 DEFERRED**: Config schema error paths (invalid schema in JSON config → role host aborts). Schema parsing validation covered at L2 by test_schema_validation.cpp.

### 2026-03-07 (Port formula audit — overflow + cross-binary collision fixes)
- ✅ **Root cause found**: Two parallel test failures (`ZmqQueueTest.SchemaTag_Match_DeliversItem`
  port 49980 and `PipelineRoundtripTest.ProducerProcessorConsumer_E2E` "hub.pubkey not written")
  were caused by logic errors in port formulas, not random collisions:
  1. **L4 overflow**: `kBasePort=16570/17570` + `(pid%5000)*10` → max port 67560 > 65535.
     For `pid%5000 >= 4897` (~2% of PIDs), hub broker silently fails to bind → test timeout.
  2. **L3 schema_ep overflow**: `48000 + (pid%2000)*12` → max 71988 > 65535.
     For `pid%2000 >= 1461` (~27% of PIDs), ZmqQueue schema-mode tests get invalid ports.
  3. **Cross-binary range overlap**: L4 tests (15570–65560) overlapped with L3 ZmqQueue (33000–65005).
     Two parallel CTest processes with suitable PIDs can compute the same TCP port simultaneously.
- ✅ **Fixes applied** (4 files, no API changes):
  - `test_admin_shell.cpp`: kBasePort=10000, pid%500*6 → range 10000–12999
  - `test_pipeline_roundtrip.cpp`: kBasePort=13000, pid%500*6 → range 13000–15999
  - `test_channel_broadcast.cpp`: kBasePort=16000, pid%500*6 → range 16000–18999
  - `test_datahub_hub_zmq_queue.cpp`: `schema_ep()` uses pid%1460 → max port 65518
  - L3 tests (33000–65535) and L4 tests (10000–18999) are now **non-overlapping**.
  - **884/884 still passing** after fix.

### 2026-03-03 (HEP Document Review + Code Review + Source Polish)
- ✅ **All 17 HEP documents updated** — mermaid diagrams, source file references, status fields
- ✅ **5-pass code review** (L0-1, L2, L3, L4, cross-cutting) — all passes EXCELLENT/PASS
  - Fixed: `debug_info.hpp` duplicate @file header, `scope_guard.hpp` missing @file,
    `recursion_guard.hpp` wrong filename in @file, `hub_queue.hpp`/`hub_processor.hpp` stale
    HEP-0012 cross-references → HEP-0015, `data_block.hpp` include path inconsistency
- ✅ **Umbrella header reorganization** — 15 orphan headers added to appropriate umbrella:
  `result.hpp` → L1; `uid_utils/uuid_utils/interactive_signal_handler/zmq_context` → L2;
  `heartbeat_manager/recovery_api/slot_diagnostics/slot_recovery/integrity_validator` → L3a;
  `channel_access_policy/channel_pattern/data_block_mutex` → L3b
- ✅ **Example build fix** — `0xBAD5ECRET` invalid literal → `0xBAD5EC` in both datahub examples
  **Total: 750/750 passing (no changes to test code).**

### 2026-03-03 (hub::Processor Enhancements + Dual-Broker + ScriptHost Dedup + C++ Templates)
- ✅ **hub::Processor enhanced API tests** (11 tests) — `test_datahub_hub_processor.cpp`:
  `TimeoutHandler_ProducesOutput`, `TimeoutHandler_NullOutputOnDrop`, `IterationCount_AdvancesOnTimeout`,
  `CriticalError_StopsLoop`, `CriticalError_FromTimeoutHandler`, `PreHook_CalledBeforeHandler`,
  `PreHook_CalledBeforeTimeout`, `ZeroFill_OutputZeroed`, `ZmqQueue_Roundtrip`,
  `ZmqQueue_NullFlexzone`, `ZmqQueue_TimeoutHandler`.
- ✅ **Dual-broker config tests** (5 tests) — `test_processor_config.cpp`:
  `DualBroker_BothPresent`, `DualBroker_FallbackToSingle`, `DualBroker_InHubDir`,
  `DualBroker_OutHubDir`, `DualBroker_MixedConfig`.
- ✅ **ScriptHost dedup** — `RoleHostCore` (engine-agnostic infrastructure) + `PythonRoleHostBase`
  (Python common layer with ~15 virtual hooks); three role subclasses reduced from ~790 to ~150 lines each.
- ✅ **C++ processor pipeline template** — `examples/cpp_processor_template.cpp`; demonstrates
  LifecycleGuard → BrokerService → Producer → Consumer → ShmQueue → Processor → typed handler.
  Build with `PYLABHUB_BUILD_EXAMPLES=ON`.
- ✅ **ZMQ wire format documentation** — HEP-CORE-0002 §7.1 added.
  **Total: 750/750 passing (734 + 16 new tests).**

### 2026-03-02 (Test Gap Closure + Script Host Deduplication)
- ✅ **Script host deduplication** — Extracted 14 shared functions + 3 types from
  processor/producer/consumer `*_script_host.cpp` into `schema_types.hpp` (types)
  and `schema_utils.hpp` + `python_helpers.hpp` (inline functions). Types now in `pylabhub::hub` namespace.
  Per-component `*_schema.hpp` files reduced to thin `using` aliases.
- ✅ **B1: Messenger hex codec tests** (8 tests) — new `test_datahub_messenger_protocol.cpp`;
  `hex_encode_schema_hash`/`hex_decode_schema_hash` roundtrip, empty, invalid chars,
  too short/long, case-insensitive, known vectors (all-zero, all-0xFF).
- ✅ **B2: Messenger not-connected guard tests** (4 tests) — added to `test_datahub_messagehub.cpp`;
  `query_channel_schema`, `create_channel`, `connect_channel` return nullopt when not connected;
  `heartbeat_noop_not_running` (suppress/enqueue no-op).
- ✅ **B3: InteractiveSignalHandler lifecycle tests** (7 tests) — new
  `test_interactive_signal_handler.cpp`; constructor stores config, `set_status_callback` before
  install, install/uninstall toggles `is_installed()`, install idempotent, uninstall idempotent,
  RAII destructor on installed handler, force_daemon config cycle. All use `force_daemon=true`.
- ✅ **B4: Messenger callback registration tests** (3 tests) — added to `test_datahub_messagehub.cpp`;
  `on_channel_closing` global register, per-channel register/deregister (nullptr), `on_consumer_died` register.
  **Total: 734/734 passing (712 + 22 new tests).**

### 2026-03-02 (L0–L3 Test Gap Closure — Phase 2, Audit-Driven)
- ✅ **L1 BackoffStrategy tests** (10 tests) — new `test_backoff_strategy.cpp`; ThreePhaseBackoff
  (3 phase transitions), ConstantBackoff (default/custom/iteration-independent), NoBackoff (instant),
  AggressiveBackoff (phase1/capped), free function. Wide timing tolerances for cross-platform.
- ✅ **L1 ModuleDef tests** (16 tests) — new `test_module_def.cpp`; Constants, Constructor
  (valid/empty/max/too-long), Move (ctor/assign), AddDependency (valid/empty/too-long), SetStartup
  (no-arg/with-arg/too-long), SetShutdown (no-arg/too-long), SetAsPersistent. Builder API only.
- ✅ **L1 DebugInfo tests** (7 tests) — new `test_debug_info.cpp`; PrintStackTrace (with/without
  external tools), PLH_PANIC (aborts/includes source location via EXPECT_DEATH), debug_msg_rt
  (no crash/format error swallowed), SRCLOC_TO_STR format.
- ✅ **L0 PlatformShm edge cases** (5 tests) — extended `test_platform_shm.cpp`; ShmClose_NullHandle,
  ShmClose_AlreadyClosed, ShmAttach_NullName, ShmUnlink_NullName, ShmUnlink_Nonexistent.
- ✅ **L2 ZmqContext concurrency** (1 test) — MultiThread_GetContext_Safe (4 threads, same pointer).
- ✅ **L2 HubVault/ActorVault** (4 tests) — Create_OverExisting + MoveConstructor for both vault types.
- ✅ **L2 SharedSpinLock** (3 tests) — IsLockedAfterUnlock, BlockingLock_WaitsForRelease, ExcessUnlock_Throws.
- ✅ **L2 ScriptHost** (1 test) — FinalizeNotCalledAfterFailedInit.
- ✅ **L3 Processor** (3 tests) — ProcessorClose, ProcessorHandlerHotSwap (handler swap after
  input_timeout cycle), ProcessorHandlerRemoval (nullptr→re-install).
- ✅ **L3 ZmqQueue overflow** (2 tests) — PullFrom_BufferFull_DropsOldest (depth-4 buffer),
  PullFrom_BufferFull_NoDeadlock (rapid push, no hang).
- ✅ **L3 SchemaRegistry search dirs** (2 tests) — SetSearchDirs_LoadsFromCustomPath,
  SetSearchDirs_OverridesDefault. Use temp directories with real schema files.
  **Total: 705/705 passing (651 + 54 new tests).**

### 2026-03-02 (L0–L3 Test Gap Closure — cross-platform aware)
- ✅ **L1 format_tools tests** (8 tests) — `make_buffer` basic/empty, `make_buffer_rt` basic/empty,
  `filename_only` unix/windows/no-separator/empty. Added to `test_formattable.cpp`.
- ✅ **L2 uid_utils tests** (18 tests) — new `test_uid_utils.cpp`; generators (5 roles), generators
  with name (3), validators (5), `sanitize_name_part` (5: normal/special chars/empty/too long/dashes).
- ✅ **L2 ActorVault tests** (12 tests) — new `test_actor_vault.cpp`; mirrors HubVault pattern;
  creation (4: writes file, restricted perms, valid Z85, empty password), opening (4: correct password,
  wrong password throws, corrupted throws, missing throws), encryption (2: secrets not plaintext,
  different UID different ciphertext), identity (2: UID roundtrip, different UIDs different keys).
  Permission test guarded with `#ifndef PYLABHUB_PLATFORM_WIN64`.
- ✅ **L2 ZmqContext tests** (3 tests) — new `test_zmq_context.cpp`; lifecycle fixture with
  `GetZMQContextModule()`; `GetContext_ReturnsValid`, `GetContext_SameInstance`, `CreateSocket_Works`.
- ✅ **L3 ZmqQueue tests** (11 tests) — new `test_datahub_hub_zmq_queue.cpp`; factory (2), lifecycle (3),
  read/write roundtrip (3: single item, multiple items, read timeout returns null), write semantics (2:
  abort not sent, item_size correct), metadata (1: name returns endpoint). All use `tcp://127.0.0.1:<port>`.
- ✅ **L3 SchemaLibrary file-loading tests** (3 tests) — added `DatahubSchemaFileLoadTest` to
  `test_datahub_schema_library.cpp`; `LoadFromDir_SingleFile`, `LoadFromDir_NestedPath`,
  `LoadFromDir_InvalidJson_Skipped`. Uses temp directories with real `.json` schema files.
- ✅ **L3 BrokerService admin API tests** (8 tests) — new `test_datahub_broker_admin.cpp`; in-process
  `LocalBrokerHandle` pattern; `ListChannels_Empty/OneChannel/FieldPresence` (3),
  `Snapshot_Empty/OneChannel/AfterConsumer` (3), `CloseChannel_Existing/NonExistent` (2).
- ✅ **XPLAT comments** — 4 cross-platform documentation comments added: `data_block_mutex.cpp` (CLOCK_REALTIME),
  `actor_vault.hpp` (getpass), `backoff_strategy.hpp` (sleep resolution), `shared_memory_spinlock.cpp` (PID reuse).
  **Total: 651/651 passing (588 + 63 new tests).**

### 2026-03-02 (HEP-CORE-0016 Phase 4: SchemaStore lifecycle)
- ✅ **SchemaStore lifecycle singleton tests: `SchemaRegistryTest`** (8 tests) —
  `test_datahub_schema_registry.cpp` in `tests/test_layer3_datahub/`; lifecycle fixture
  (Logger + SchemaStore); covers `GetInstance_SameAddress`, `LifecycleInitialized_True`,
  `RegisterAndGet`, `IdentifyByHash`, `GetUnknown_Nullopt`, `IdentifyUnknown_Nullopt`,
  `Reload_ClearsAndReloads`, `ListSchemas`. **Total: 588/588 passing.**

### 2026-03-02 (Layer 4 processor tests)
- ✅ **Layer 4 config unit tests: `ProcessorConfigTest`** (10 tests) — `test_processor_config.cpp`
  in `tests/test_layer4_processor/`; `PureApiTest` fixture; `processor_config.cpp` compiled directly
  into test binary; covers `from_json_file` (basic fields, uid autogen, in/out schemas, missing
  in_channel, missing out_channel, malformed JSON, file not found), `from_directory` (with script_path
  resolution), `overflow_policy` default (block) and invalid (throws).
- ✅ **Layer 4 CLI integration tests: `ProcessorCliTest`** (6 tests) — `test_processor_cli.cpp`;
  `IsolatedProcessTest` fixture; spawns `pylabhub-processor` binary via `WorkerProcess` (path
  derived as `g_self_exe_path/../bin/pylabhub-processor`); covers `--init` (directory structure +
  default values including both in/out channels and overflow_policy), `--keygen` (vault file created,
  stdout mentions `Processor vault written to` + `public_key`), `--validate` (exits 0, prints
  "Validation passed"), malformed JSON (non-zero exit + "Config error" in stderr), and missing
  config file (non-zero exit). **Total: 580/580 passing.**

### 2026-03-02 (HEP-CORE-0016 Phase 3 — Broker Schema Protocol Tests)
- ✅ **7 `BrokerSchemaTest` tests** — `test_datahub_broker_schema.cpp` in `tests/test_layer3_datahub/`:
  - `SchemaId_StoredOnReg` — producer registers with schema_id; query_channel_schema echoes it back
  - `SchemaBlds_StoredOnReg` — BLDS string stored at REG_REQ time and returned via SCHEMA_REQ
  - `SchemaHash_ReturnedOnQuery` — raw 32-byte hash → hex-encoded round-trip via broker
  - `SchemaReq_UnknownChannel_ReturnsNullopt` — query for unregistered channel returns nullopt
  - `ConsumerSchemaId_IdMatch_Succeeds` — consumer expected_schema_id matches producer's → connect succeeds
  - `ConsumerSchemaId_Mismatch_Fails` — consumer expected_schema_id differs → connect_channel returns nullopt
  - `ConsumerSchemaId_EmptyProducer_Fails` — anonymous producer + consumer expects schema_id → fails
  - Uses in-process `LocalBrokerHandle` pattern (ephemeral port, empty SchemaLibrary)
  - Bug fix: `messenger_protocol.cpp` CONSUMER_REG error now returns nullopt (was fire-and-forget)
  **Total: 564/564 passing.**

### 2026-03-02 (HEP-CORE-0016 Phase 2 — Schema Integration Tests)
- ✅ **`validate_named_schema<DataT, FlexT>(schema_id, lib)`** — template free function added to
  `schema_library.hpp`; performs: (1) forward lookup via `lib.get(schema_id)`, (2) `sizeof(DataT)`
  vs `slot_info.struct_size` size check, (3) BLDS hash check when `PYLABHUB_SCHEMA_BEGIN/MEMBER/END`
  macros used (detected by `has_schema_registry_v<DataT>`), (4) flexzone size + hash check when
  `FlexT != void`. Throws `SchemaValidationException` on any mismatch or unknown ID; no-op when
  `schema_id` is empty. Companion `validate_named_schema_from_env<>()` builds library from
  `default_search_dirs()` + `load_all()` then delegates.
- ✅ **`has_schema_registry<T>` trait** added to `schema_blds.hpp` — detects `SchemaRegistry<T>`
  specialization via `std::void_t`; exported as `has_schema_registry_v<T>`.
- ✅ **`ProducerOptions::schema_id`** and **`ConsumerOptions::expected_schema_id`** — new `std::string`
  fields (default `""`); when non-empty, `Producer::create<F,D>()` / `Consumer::connect<F,D>()`
  call `validate_named_schema_from_env<DataBlockT, FlexZoneT>(schema_id)` at entry.
- ✅ **7 `DatahubSchemaPhase2Test` tests** — `MatchingStruct_NoThrow`, `EmptySchemaId_NoCheck`,
  `UnknownId_Throws`, `SlotSizeMismatch_Throws`, `SlotHashMismatch_Throws`,
  `FlexzoneSizeMismatch_Throws`, `MatchingFlexzone_NoThrow`. Use in-memory `SchemaLibrary`
  (no file I/O, no env vars). **Total: 557/557 passing.**

### 2026-03-02 (Layer 4 producer + consumer tests)
- ✅ **Layer 4 config unit tests: `ProducerConfigTest`** (8 tests) — `test_producer_config.cpp`
  in `tests/test_layer4_producer/`; `PureApiTest` fixture; `producer_config.cpp` compiled directly
  into test binary; covers `from_json_file` (basic fields, uid autogen, schema fields, missing channel,
  malformed JSON, file not found), `from_directory` (with omitted `hub_dir`), and
  `stop_on_script_error` default false.
- ✅ **Layer 4 CLI integration tests: `ProducerCliTest`** (6 tests) — `test_producer_cli.cpp`;
  `IsolatedProcessTest` fixture; spawns `pylabhub-producer` binary via `WorkerProcess` (path
  derived as `g_self_exe_path/../bin/pylabhub-producer`); covers `--init` (directory structure +
  default values), `--keygen` (vault file created, stdout mentions `public_key`), `--validate`
  (exits 0, prints "Validation passed"), malformed JSON (non-zero exit + "Config error" in
  stderr), and missing config file (non-zero exit). Key fix: `script_path` must be the *parent*
  of `script/<type>/`, not `script/` itself — binary appends `script/<type>/__init__.py`.
- ✅ **Layer 4 config unit tests: `ConsumerConfigTest`** (6 tests) — `test_consumer_config.cpp`
  in `tests/test_layer4_consumer/`; symmetric to producer; `CONS-` prefix; no `shm_slot_count`,
  no `update_checksum`; covers `from_json_file` (5 variants) and `from_directory`.
- ✅ **Layer 4 CLI integration tests: `ConsumerCliTest`** (6 tests) — `test_consumer_cli.cpp`;
  spawns `pylabhub-consumer` binary; same 6 scenarios as producer CLI tests; "Consumer vault
  written to" in stdout for keygen. **Total: 550/550 passing.**

### 2026-02-28 (Actor CLI integration tests)
- ✅ **Layer 4 CLI tests: `pylabhub-actor` CLI integration** (12 tests) — `test_layer4_actor_cli`
  executable using `pylabhub::test_framework` (`WorkerProcess`/`g_self_exe_path`); actor binary
  derived from staged path (`../bin/pylabhub-actor` relative to test binary); covers:
  `--keygen` (write/missing-keyfile/create-parent-dir/overwrite), `--register-with`
  (append/idempotent/missing-actor/missing-hub), config error paths (malformed JSON,
  missing roles, invalid kind, file not found). Fixed: `auth.keyfile` is inside `actor` block,
  not top-level. All Tier 1 (no Python/broker). Total: **585/585 passing**.

### 2026-02-28 (ScriptHost + HubConfig script-block tests + CMake option)
- ✅ **Layer 2 tests: ScriptHost threading model** (10 tests) — `tests/test_layer2_service/test_script_host.cpp`:
  `test_layer2_script_host` executable; mock subclasses; threaded mode (startup/shutdown/idempotent/
  early-stop/exception propagation/false-without-signal); direct mode (startup-ready/shutdown-finalizes/
  failure-throws). Note: `ThreadedEarlyStop` checks `is_ready()` only AFTER `shutdown()` (join provides
  happens-before; earlier check is racy vs thread_fn_ clearing `ready_`).
- ✅ **Layer 3 tests: HubConfig script-block fields** (9 tests) — `tests/test_layer3_datahub/test_datahub_hub_config_script.cpp`:
  two lifecycle fixtures (Logger + CryptoUtils + FileLock + JsonConfig + HubConfig); configured fixture
  (hub.json with all script/python fields; 5 tests); defaults fixture (absent script/python block; 4 tests).
  *(Retired 2026-04-29 — file deleted with the legacy singleton; replaced by
  `tests/test_layer2_service/test_hub_config.cpp`.)*
- ✅ **CMake: `PYLABHUB_PYTHON_REQUIREMENTS_FILE`** — `cmake/ToplevelOptions.cmake` FILEPATH cache var;
  `third_party/cmake/python.cmake` uses variable (+ existence check); `CMakeLists.txt` status message.
  **Total: 573/573 passing**

### 2026-02-27 (LoopPolicy edge-case tests + code review fixes)
- ✅ **Layer 3 tests: LoopPolicy edge cases** (11 tests) — extended `test_datahub_loop_policy.cpp`:
  - 7 edge-case tests (secrets 80006–80012): ZeroOnCreation, MaxRateNoOverrun, LastSlotWorkUsPopulated,
    LastIterationUsPopulated, MaxIterationUsPeak, ContextElapsedUsMonotonic, CtxMetricsPassThrough
  - 4 RAII-specific tests (secrets 80013–80016): RaiiProducerLastSlotWorkUsMultiIter,
    RaiiProducerMetricsViaSlots, RaiiProducerOverrunViaSlots, RaiiConsumerLastSlotWorkUs
  - Fixed RAII release path: `last_slot_exec_us` set in `release_write_handle()` and
    `release_consume_handle()` (symmetric RAII destructor + explicit call paths)
  - Fixed RAII multi-iter producer race: `SlotWriteHandle`/`SlotConsumeHandle` store per-handle
    `t_slot_acquired_` (not `owner->t_iter_start_` which gets overwritten between iterations)
  **Total: 550/550 passing**
- ✅ **CODE_REVIEW.md triage complete** (2026-02-27): All 8 critical + 12 high items verified
  as false positives or pre-existing fixes; deferred medium items documented in review.

### 2026-02-26 (Connection policy + security identity)
- ✅ **Layer 3 tests: ConnectionPolicy enforcement** (11 tests) — new `test_datahub_connection_policy.cpp`:
  - Suite 1 (4 tests): `to_str`/`from_str` round-trips, unknown-string fallback to Open
  - Suite 2 (7 tests): Open/Required/Verified broker enforcement, per-channel glob override,
    ephemeral port (`tcp://127.0.0.1:0`) prevents parallel ctest-j collisions
  **Total: 539/539 passing** (528 pre-existing + 11 new Phase 3 connection policy tests)

### 2026-02-23
- ✅ **Layer 2 tests: HubVault** (15 tests) — `tests/test_layer2_service/test_hub_vault.cpp`:
  create/open/publish_public_key basics, file permissions (0600 vault, 0644 pubkey),
  Z85 keypair and 64-char hex token validation, entropy (two creates differ), wrong password throws,
  corrupted vault throws, missing vault throws, `VaultFileDoesNotContainPlaintextSecrets`,
  `EncryptDecryptRoundTrip`, `DifferentHubUidProducesDifferentCiphertext` (cross-uid open fails).
  No lifecycle; uses `gtest_main`; Argon2id ~0.5s/call → 120s timeout.
  **Total: 488/488 passing** (479 pre-existing + 15 new Layer 2 HubVault tests — 1 flaky lifecycle timeout pre-existing, passes in isolation).
- ✅ **Layer 4 tests: ActorConfig parsing** (32 tests) — `tests/test_layer4_actor/test_actor_config.cpp`:
  `loop_timing` (fixed_pace/compensating/default/invalid), `broker_pubkey`, `broker` endpoint,
  `interval_ms`/`timeout_ms`, all four `ValidationPolicy` fields + defaults + invalid values,
  uid auto-gen / non-conforming (warning, no throw), multi-role, empty roles map, all error cases.
  No lifecycle init; `LOGGER_COMPILE_LEVEL=0`.
- ✅ **Layer 4 tests: ActorRoleAPI metrics** (21 tests) — `tests/test_layer4_actor/test_actor_role_metrics.cpp`:
  initial-zero invariant, `increment_script_errors`, `increment_loop_overruns`,
  `set_last_cycle_work_us`, `reset_all_role_run_metrics` (all 3 counters; does not reset identity
  fields; accumulation after reset; no-op on fresh API), `slot_valid` flag, all identity getters,
  instance independence. All inline methods; no Python interpreter init.
  **Total: 479/479 passing** (426 pre-existing + 53 new Layer 4 tests).

### 2026-02-21 (gap-fixing session)
- ✅ **426/426 tests pass** — no regressions after all gap-fix changes (demo scripts, --keygen, schema hash, timeout constants)
- ✅ **Layer 4 test gap identified** — actor integration test plan updated with schema hash mismatch test and --keygen test
- ✅ **426/426 tests pass** — no regressions after pylabhub-actor, UID enforcement, SharedSpinLockPy additions (earlier in same day)

### 2026-02-17 (Integrity validation tests)
- ✅ **Integrity repair test suite** (`test_datahub_integrity_repair.cpp` + workers) — 3 tests:
  fresh ChecksumPolicy::Enforced block validates successfully (slot checksum path exercised);
  layout checksum corruption detected (FAILED on both repair=false and repair=true — not repairable);
  magic number corruption detected (FAILED). Secrets 78001–78003.
  Slot-checksum in-place repair deferred: existing repair path uses `create_datablock_producer_impl`
  which reinitialises the header — incompatible with in-place repair testing.
  **Total: 384/384 passing.**

### 2026-02-17 (Recovery scenario facility tests)
- ✅ **Recovery scenario test suite** (`test_datahub_recovery_scenarios.cpp` + workers) — 6 tests:
  zombie writer (dead PID in write_lock → release_zombie_writer → FREE);
  zombie readers (reader_count injected → release_zombie_readers → 0);
  force_reset on dead writer (dead write_lock → force_reset succeeds without force flag);
  dead consumer cleanup (fake heartbeat with dead PID → cleanup_dead_consumers removes it);
  is_process_alive sentinel (kDeadPid=INT32_MAX → false; self PID → true);
  force_reset safety guard (alive write_lock → RECOVERY_UNSAFE; recoveryAPI logs ERROR).
  Secrets 77001–77004, 77006. **Scope: facility layer only** — full broker-coordinated
  zombie detection remains deferred (Phase C, requires broker protocol).
  **Total: 381/381 passing.**

### 2026-02-17 (WriteAttach mode tests)
- ✅ **WriteAttach test suite** (`test_datahub_write_attach.cpp` + workers) — 4 tests:
  basic roundtrip (hub creates, source attaches R/W and writes, creator consumer reads);
  secret mismatch → nullptr; schema mismatch → nullptr; segment persists after writer detach.
  Secrets 76001–76004. Verifies broker-owned shared memory model.
  **Total: 375/375 passing.**

### 2026-02-17 (coverage gap tests completed)
- ✅ **Config validation test** (`test_datahub_config_validation.cpp` + workers) — 5 tests:
  all four mandatory-field throw cases + valid config succeeds. Secrets 73001–73005.
- ✅ **Header structure test** (`test_datahub_header_structure.cpp` + workers) — 3 tests:
  template API populates both schema hashes; impl with nullptr zeroes them; different types
  produce different hashes. Secrets 74001–74004. Fix: `flex_zone_size = sizeof(FlexZoneT)` required.
- ✅ **C API validation test** (`test_datahub_c_api_validation.cpp` + workers) — 5 tests:
  `datablock_validate_integrity` succeeds on fresh; fails on nonexistent (allow ERROR logs);
  `datablock_get_metrics` shows 0 commits; `datablock_diagnose_slot` shows FREE;
  `datablock_diagnose_all_slots` returns capacity entries. Secrets 75001–75005.
  **Total: 371/371 passing.**

### 2026-02-17 (docs audit — test refactoring status verified)
- ✅ **Test refactoring complete** — All Phase 1-3 and Phase 4 (T4.1-T4.5) tasks from the
  test refactoring plan are done: shared test types (`test_datahub_types.h`), removed obsolete
  non-template tests, all enabled tests compile; new tests added for schema validation,
  c_api checksum, exception safety, handle semantics. Phase 5 renaming also complete (all
  files follow `test_datahub_*` convention). Verified: 358/358 passing.
  — All transient test planning docs archived to `docs/archive/transient-2026-02-17/`

### 2026-03-07
- ✅ `DatahubSlotDrainingTest` extended to 9 tests (SHM-C2 audit):
  - `DrainHoldTrueNeverReturnsNullptr`: directly tests SHM-C2 core invariant — `DataBlockProducer::acquire_write_slot()` (drain_hold=true) never returns nullptr on drain timeout; verified still blocked after 4 × timeout_ms intervals
  - `DrainHoldTrueMetricsAccumulated`: verifies `writer_reader_timeout_count` and `writer_blocked_total_ns` both accumulate on each drain-hold timer reset
  - Fixed pre-existing stalling test `DrainingTimeoutRestoresCommitted` (#431): replaced `DataBlockProducer::acquire_write_slot()` (drain_hold=true → deadlock) with C API `slot_rw_acquire_write()` (drain_hold=false → SLOT_ACQUIRE_TIMEOUT); documented C API passes header=nullptr so metrics not updated via this path
  — `tests/test_layer3_datahub/workers/datahub_c_api_draining_workers.cpp` secrets 72008-72009
- ✅ **Sleep-based race condition audit — all 8 occurrences fixed** in `datahub_hub_processor_workers.cpp`:
  - All `std::this_thread::sleep_for(Nms)` used to ORDER concurrent operations replaced with `poll_until(condition, deadline)`
  - Key patterns used: `poll_until([&proc] { return proc.iteration_count() >= N; })` for processor loop sync; `poll_until([&proc] { return proc.out_drop_count() >= 1; })` for counter lag; `iteration_count > n_before + 1` barrier for handler hot-swap and handler removal
  - Fixed `ProcessorHandlerRemoval` flaky test: was a race between `sleep_for(300ms)` and handler load; now uses `iteration_count` barrier to guarantee null-handler path entered before asserting output queue empty
  - 5 `sleep_for(100ms)` calls after ZMQ `start()` intentionally retained — these wait for TCP connection establishment (no synchronous callback available), not ordering of operations
- ✅ **`test_sync_utils.h` shared facility created** — `tests/test_framework/test_sync_utils.h`; `poll_until(pred, timeout, poll_ms)` template; lightweight (only `<chrono>` + `<thread>`); no gtest/lifecycle dependency; available transitively via `shared_test_helpers.h`; correctly placed in test framework (sleep-based polling is test-only — production code uses busy-spin)
  — 884/884 tests passing

### 2026-02-17
- ✅ `DatahubSlotDrainingTest` (7 tests): DRAINING state machine tests — entered on wraparound,
  rejects new readers, resolves after reader release, timeout restores COMMITTED, no reader races
  on clean wraparound; plus ring-full barrier proof tests for Sequential and Sequential_sync
  — `tests/test_layer3_datahub/test_datahub_c_api_slot_protocol.cpp`,
    `tests/test_layer3_datahub/workers/datahub_c_api_draining_workers.cpp`
- ✅ Proved DRAINING structurally unreachable for Sequential / Sequential_sync
  (ring-full check before fetch_add creates arithmetic barrier) — documented in
  `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` § 11, `docs/IMPLEMENTATION_GUIDANCE.md` Pitfall 11

### 2026-02-14
- ✅ Writer timeout metrics split test (lock vs reader timeout)
- ✅ Unified metrics API tests (total_slots_written, state snapshot)

### 2026-02-13
- ✅ Config validation tests (explicit parameters required)
- ✅ Shared spinlock API tests (get_spinlock, spinlock_count)

### 2026-02-12
- ✅ ConsumerSyncPolicy tests (all three modes)
- ✅ High-load single reader integrity test
- ✅ MessageHub Phase C groundwork (no-broker paths)

---

## Notes

- Test pattern choice: PureApiTest (no lifecycle), LifecycleManagedTest (shared lifecycle), WorkerProcess (multi-process or finalizes lifecycle). See `docs/README/README_testing.md`.
- CTest runs each test in separate process; direct execution runs all in one process. Use WorkerProcess for isolation.
- When test fails: scrutinize before coding. Is it revealing a bug or is the test wrong? See `docs/IMPLEMENTATION_GUIDANCE.md` § Responding to test failures.
