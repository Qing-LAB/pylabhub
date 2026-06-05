# AUTH_TODO completions archive — 2026-06-05

This file preserves verbatim prose for AUTH_TODO entries that were verified
shipped in code as of 2026-06-05.  Moved here so the active AUTH_TODO can
focus on open items.  No content summarized; full prose retained for
context.

Source file: `docs/todo/AUTH_TODO.md`.

---

## PeerAdmission Phase A — Abstraction (PeerAdmission interface)

**Verified shipped** — commit `d5a90f29` (interface lands; no behavior change).

## PeerAdmission Phase B — KnownRole + CLI

**Verified shipped** — commit `a6b44ff8`; HEP-0035 §4.8.3/§4.8.4 (operator
UX + KnownRolesStore + `<hub_dir>/vault/known_roles.json` mode 0600).

## PeerAdmission Phase C — ZapRouter + ZmqQueue CURVE

**Verified shipped + closed** — Phase C close-out commits `62bda863..47aa0374`
(5 commits: ZapRouter frame-level tests, ZmqQueue auth security tests,
KnownRolesStore strict JSON tests, CLI negative-path tests, design doc
update; tasks #132–#136).

## Phase D — Broker glue: D1 + D2 + D3 (shipped slice)

### D1 — `ChannelAccessIndex` in `HubState` (commit `cacea477`)

`ChannelAccessEntry` is two fields (`authorized_consumer_pubkeys`
+ `shm_secret`) per HEP-CORE-0036 §4.1; producer pubkey + endpoint
stay per-producer on `ChannelEntry::producers[i].zmq_pubkey` +
`zmq_node_endpoint` (hub_state.hpp:184/194) — no duplication so
fan-in (HEP-CORE-0023 §2.1.1) is preserved.  Mutators shipped:
`_on_channel_access_opened(channel, shm_secret)`,
`_on_channel_access_closed(channel)`,
`_on_consumer_authorized(channel, pubkey_z85)`,
`_on_consumer_revoked(channel, pubkey_z85)`.  All four idempotent.
Read accessor: `channel_access(name)` returning
`std::optional<ChannelAccessEntry>` under shared lock.  L2 coverage:
12 tests in `HubStateChannelAccess.*` exercising mutators + accessor
+ idempotence + multi-channel isolation + invalid-identifier counter
bump.  TestAccess forwarders added for friend access from tests.

### D2 — Broker CTRL ROUTER ZAP handler (commit `d18d2e91` + close-out)

HubHost startup loads `<hub_dir>/vault/known_roles.json` via
`KnownRolesStore` and copies the entries into
`BrokerService::Config::known_roles`.  `BrokerServiceImpl::run()`
builds the initial CTRL `PeerAllowlist` from the UNION of
`cfg.known_roles[].pubkey_z85` AND `cfg.peers[].pubkey_z85`
(federation peer DEALERs per HEP-CORE-0035 §4.2), wires it into a
`BrokerCtrlAdmission` (PeerAdmission impl backed by
`PortableAtomicSharedPtr`), installs via
`ZapRouter::instance().register_domain("broker.ctrl", ...)`, and
pumps `ZapRouter::pump_one(0ms)` after each `zmq::poll`.
`Config::enforce_ctrl_admission` defaults to `true` (production
deny-all); test L3 fixtures that use CURVE for wire encryption only
set it to `false`.  L4 `RoundTrip_PlhHubKeygenAndRunPlhRoleRegisters`
exercises the production path end-to-end: `plh_role --keygen` →
`RoleVault::open` → `plh_hub --add-known-role` → `plh_hub
<hub_dir>` → role connects + REG_REQ succeeds.

### D3 — Channel-auth wire frames (broker_proto 5 → 6)

Shipped 2026-06-04 (commit `d59d9e00` main + follow-ups `bdb71ff7`,
`4673894c` D3 hardening, `a3fe0a23` H1+H2 review fix, `b58c8ed8`
R1+R2, `0502561a` R3 §6.6 vocabulary alignment, `8a9b0368` M4
tripwire, `a875f3a1` phase-label scrub).  Notify-then-pull semantics
per HEP-0036 §6.5 amended 2026-06-04 (supersedes the 2026-06-02
snapshot-push-with-ACK design).  Two new message types:

- `CHANNEL_AUTH_CHANGED_NOTIFY` (broker → producer, fire-and-
  forget): `{channel_name, reason}`.  Same wire shape as existing
  `CHANNEL_CLOSING_NOTIFY` / `BAND_LEAVE_NOTIFY`.
- `GET_CHANNEL_AUTH_REQ` / `GET_CHANNEL_AUTH_ACK` (producer →
  broker → producer, sync request-reply): request carries
  `{channel_name, role_uid, corr_id}`; ACK carries
  `{status, allowlist?, error_code?}` with `allowlist` as a plain
  Z85 array on success.  Uses the existing
  `BrokerRequestComm::do_request` machinery (no new dispatcher).

Broker stays a pure responder; no skip-disconnected logic; no
`push_ack_timeout_ms` config.  Producer-offline recovers via the
existing `REG_ACK.initial_allowlist` reconnect re-sync.  Both
REG_REQ + CONSUMER_REG_REQ carry + hard-reject empty/non-40-char
`zmq_pubkey` per HEP-CORE-0036 §4.1 + §6.5 + HEP-CORE-0035 §2
unconditional-CURVE.  Client-side retry in
`RoleAPIBase::register_consumer` matches `awaiting_first_heartbeat`
and `heartbeat_stalled` reasons; CHANNEL_NOT_FOUND terminal.

---

## Phase D landing — HEP-0035 §4.6.5 "no-bypass" cleanup (2026-06-04)

Full-ctest sweep on 2026-06-04 surfaced 97 failures rooted in the
close-out-3 mismatch WARN + test fixtures that paired
`use_curve=false` with `enforce_ctrl_admission=true` (the new
default).  Audit concluded that HEP-CORE-0035 §2 mandates CURVE +
admission unconditionally — both production AND tests — and the
two flags are HEP violations.

**Steps 1+2 landed (commit `3a64e58c`):**

- HEP-CORE-0035 §2 — three new invariants: CURVE unconditional,
  admission unconditional whenever CURVE on, `HubHost::startup()`
  rejects empty `auth().client_pubkey`.
- HEP-CORE-0035 §4.6.5 (new) — no-bypass discipline.  Tests use
  real CURVE via `tests/test_framework/curve_test_setup.h`
  (~4 LOC / fixture, ~100 μs keypair-gen).  No test-only factory.
- HEP-CORE-0035 §7 closed question 5 — struck.
- HEP-CORE-0035 §1 banner — updated.
- HEP-CORE-0036 §7 — boxed note: admission + CURVE unconditional
  in tests too.
- HEP-CORE-0033 §4.2 step 2 — cross-references HubHost startup
  precondition.
- `broker_service.cpp:708-722` — close-out-3 mismatch WARN
  deleted; replaced with a placeholder note flagging both flags
  for removal in the landing phase.
- `role_identity_policy_workers.cpp:232` `base_cfg()` — both
  flags set false explicitly with a declared bypass block
  citing §8 Phase 6 (whole fixture slated for deletion).

ctest result after Steps 1+2: 97 → 1 failure (the remaining one is
the pre-existing `PlhRoleInitTest` 60s flake, task #93; passes in
isolation).

### Phase 1 known bugs surfaced during landing — BRC monitor CURVE blindspot (CLOSED)

Surfaced 2026-06-04 by migrating
`HubHostIntegrationTest.HubHost_Shutdown_BreaksClientConnection`
onto the harness.  Under NULL-mech, BRC's `on_hub_dead` callback
fires within ~3s of broker socket close.  Under CURVE, it never
fires (waited 40s with `apply_socket_policy` defaults —
heartbeat_ivl 5s + heartbeat_timeout 30s).

**Empirical findings (2026-06-04 stderr instrumentation):**
- During BRC connect, monitor receives 3 events: `CONNECT_DELAYED`
  (0x0002) → `CONNECTED` (0x0001) → `HANDSHAKE_SUCCEEDED` (0x1000).
- Broker thread DOES execute `router.close()` (confirmed via stderr
  trace `[DIAG-BROKER] router.close() pre/post` straddling the
  libzmq call at `broker_service.cpp:1054`).
- BRC monitor receives **ZERO events** for 40+ seconds after
  `router.close()` returns — not `DISCONNECTED`, not `CLOSED`,
  not `HANDSHAKE_FAILED_*`, not any event at all.

**Conclusion**: libzmq's CURVE engine does not emit
ZMQ_EVENT_DISCONNECTED on clean peer-socket close.  This is a
libzmq behaviour, not a pylabhub coding error.

**Validation result (2026-06-04):** the bug is in-process-
shared-context-only.  A standalone diagnostic
(`/tmp/test_curve_disconnect.cpp`, not retained) instantiates a
CURVE-server ROUTER and a CURVE-client DEALER in **separate**
`zmq::context_t` instances (mimicking the cross-process production
scenario where plh_hub and plh_role have independent libzmq
instances), runs the same close sequence, and observes:

```
[mon] CONNECT_DELAYED
[mon] CONNECTED
[mon] HANDSHAKE_SUCCEEDED
--- closing broker ROUTER socket ---
[mon] DISCONNECTED   ← fires within seconds
```

Under separate contexts, DISCONNECTED fires normally.  Under
shared context (test scenario), it doesn't.  Conclusion:

- **Production is not affected.**  HEP-CORE-0023 §2.5.3 holds in
  real plh_hub ↔ plh_role deployments.
- The failing migrated test
  (`HubHost_Shutdown_BreaksClientConnection`) is misleading: it
  runs both broker and BRC in one process under the shared
  `pylabhub::hub::get_zmq_context()`, hitting the libzmq
  shared-context quirk.
- **No production code change needed.**

Severity: **LOW** (test artifact, not production bug).

**Layer reclassification (designer call 2026-06-04):**
`HubHost_Shutdown_BreaksClientConnection` is verifying cross-process
behaviour ("broker shutdown breaks the client connection") — a
property that only holds when the broker and the BRC are in
different processes (and therefore different libzmq instances).
The in-process L3 scenario with shared
`pylabhub::hub::get_zmq_context()` cannot test this correctly —
the libzmq shared-context quirk produces a false negative
regardless of how the test is written.

**This is an L4 test, not L3.**  Action:

- **In L3** (`test_datahub_hub_host_integration.cpp`): **DELETE**
  the `HubHost_Shutdown_BreaksClientConnection` TEST_F entry when
  its sibling tests get re-migrated in step 3-revised-F.  The
  current `GTEST_SKIP` is a placeholder for that delete.  The L3
  worker body in
  `hub_host_integration_workers.cpp::hubhost_shutdown_breaksclientconnection`
  goes away with the TEST_F.
- **In L4** (new): add an equivalent test under
  `tests/test_layer4_plh_hub/` that spawns plh_hub and plh_role
  as separate processes, terminates plh_hub, asserts plh_role's
  BRC observes the disconnect within the heartbeat-timeout window.
  Owner: a follow-up L4 task (NOT folded into the HEP-0035 §4.6.5
  landing — it's independent test-coverage debt).

Step 3-revised-A is therefore **CLOSED — no production action
required.**

### Landing-phase slice commits

| Slice | Commit | Scope | ctest |
|---|---|---|---|
| 1 | `97b1ff25` | HEP doctrine (§4.6.5 + §2 invariants in HEP-0035/0036/0033) + Bucket 1 production `CurveKeypair` utility consolidating 4 inline keygen sites + harness foundation (`broker_test_harness.{h,cpp}`, `curve_test_setup.h`, `HubConfig::inject_keypair_for_test`) | — |
| 2 | `b8a24b65` | Pattern-A proof — `datahub_channel_group_workers.cpp` | 7/7 |
| 3 | `246a4647` | Pattern-B proof — `datahub_broker_request_comm_workers.cpp` (+ harness `known_roles.json` schema fix) | 4/4 |
| 4 | `2b94247d` | `hub_host_integration_workers.cpp` + `HubHostBrokerHandle::service()` accessor.  Surfaced **BRC monitor CURVE bug** (skipped one test with citation) | 2/3 pass + 1 skip |
| 5 | `f19d5dcd` | `broker_consumer_workers.cpp` | 5/5 |
| 6 | `1b979895` | `broker_schema_workers.cpp` | 5/5 |
| 7 | `04dfb199` | `broker_admin_workers.cpp` | 8/8 |
| 8 | `b2f4351b` | `zmq_endpoint_registry_workers.cpp` (with `StubBrcHandle` declared-bypass for the wire-level timeout fixture) | 9/9 |

### Stop-here decision (2026-06-04)

After 8 slices the harness was proven on both Pattern A and B.  Per
the stabilize-first designer call, the remaining migration (7 files,
~150 fixtures) paused; production API stabilization (3-revised
A through E) ran first to avoid touching tests twice.

### Revised order (decided 2026-06-04 — designer call: "stabilize API/design first")

| Step | Scope | Notes |
|---|---|---|
| 3-revised-A | **Fix the BRC monitor CURVE bug** (`broker_request_comm.cpp` socket-monitor poll path). | CLOSED above — no production action required; test artifact only. |
| 3-revised-B | `HubHost::startup()` throws `std::logic_error` on empty `auth().client_pubkey`. | Unblocks deleting the dead `bcfg.use_curve = !empty()` conditional. |
| 3-revised-C | Delete `BrokerService::Config::enforce_ctrl_admission` field + every `cfg.enforce_ctrl_admission = X` line in tests and CMake.  Unmigrated test files will fail to compile; that's expected. | Doesn't break the 7 migrated files (the harness sets the field but stops once it's gone). |
| 3-revised-D | Delete legacy `RoleIdentityPolicy` enum, `check_role_identity()`, `KnownRole`-with-strings, `ChannelPolicyOverride`, `RoleIdentityPolicyBrokerTest` (L3 fixture).  Per HEP-CORE-0035 §8 Phase 6.  Includes dead-code audit (grep). | Removes the deprecated machinery so tests can't depend on it. |
| 3-revised-E | Delete `BrokerService::Config::use_curve` field.  Broker always CURVE+ZAP. | Final field-deletion.  Shipped as part of commit `3a64e58c`. |

**3-revised-F (L3 test migration of the remaining 7 worker files)
and 3-revised-G (full ctest green from clean baseline) now tracked
as task #154** (re-create L3 broker tests against the refactored
lib code).

**Rationale for the reorder (recorded 2026-06-04 per designer
guidance):** if tests need to be migrated again when API changes,
that's pointless effort.  Stabilize production API/design first;
migrate tests once against the stable shape.

### Exclusion procedure during lib-stabilization window (3-revised-B → 3-revised-E)

Once we started deleting production fields and legacy code (steps
3-revised-B through 3-revised-E), the unmigrated L3 worker files
became **build-breaks**, not just runtime failures.  The exclusion
procedure below kept `ctest` green during the stabilization window
(tracked as task #153 — masking; the corresponding re-creation is
task #154).

- Removing `BrokerService::Config::enforce_ctrl_admission` (step C)
  breaks every `cfg.enforce_ctrl_admission = ...` line in unmigrated
  workers.
- Removing `BrokerService::Config::use_curve` (step E) breaks every
  `cfg.use_curve = ...` line.
- Removing `RoleIdentityPolicy` / `check_role_identity` /
  `ChannelPolicyOverride` (step D) breaks
  `role_identity_policy_workers.cpp` and its TEST_F driver.
- Making `HubHost::startup()` throw on empty pubkey (step B) breaks
  any worker that constructs HubHost without injecting a keypair.

**Exclusion is two-part — both halves are mandatory:**

1. **Worker .cpp**: comment out the entry in
   `tests/test_layer3_datahub/CMakeLists.txt` source list, with an
   inline marker:

   ```cmake
   # workers/datahub_metrics_workers.cpp        # SKIP — pending HEP-0035 §4.6.5 step 3-revised-F
   ```

2. **Test driver `TEST_F` entries**: comment out (or `GTEST_SKIP` —
   prefer comment-out so they don't appear in the test list) every
   `TEST_F` in the corresponding `test_datahub_*.cpp` driver, with a
   single citation comment block at the top of the file:

   ```cpp
   // === PENDING HEP-0035 §4.6.5 step 3-revised-F migration ===
   // Worker file is excluded from build (see CMakeLists.txt).
   // Restore each TEST_F when the worker file is re-included.
   ```

   Without this half, the driver still references worker dispatchers
   that no longer exist and either fails to link or returns -1 at
   `SpawnWorker(...)` time.

| Unmigrated worker | Unmigrated driver | Status under stabilization |
|---|---|---|
| `workers/datahub_metrics_workers.cpp` | `test_datahub_metrics.cpp` (17 TEST_F) | exclude before step C |
| `workers/datahub_broker_health_workers.cpp` | `test_datahub_broker_health.cpp` (11 TEST_F) | exclude before step C |
| `workers/datahub_broker_protocol_workers.cpp` | `test_datahub_broker_protocol.cpp` (29 TEST_F) | exclude before step C |
| `workers/datahub_broker_workers.cpp` | `test_datahub_broker.cpp` (40 TEST_F) | exclude before step C |
| `workers/datahub_role_state_workers.cpp` | `test_datahub_role_state_machine.cpp` (21 TEST_F) | exclude before step C |
| `workers/hub_federation_workers.cpp` | `test_datahub_hub_federation.cpp` | exclude before step C |
| `workers/datahub_e2e_workers.cpp` | `test_datahub_e2e.cpp` | exclude before step C; subprocess key-passing redesign in step F |
| `workers/role_identity_policy_workers.cpp` | `test_datahub_role_identity_policy.cpp` | **DELETE in step D** (not migration; legacy retirement per HEP-0035 §8 Phase 6) |

The 7 already-migrated worker files stayed enabled throughout:
`datahub_channel_group_workers`, `datahub_broker_request_comm_workers`,
`hub_host_integration_workers`, `broker_consumer_workers`,
`broker_schema_workers`, `broker_admin_workers`,
`zmq_endpoint_registry_workers`.

**Per-file restoration during the now-tracked task #154**: each
file's migration commit (a) un-comments the CMakeLists.txt source
entry, (b) restores the TEST_F entries in the driver (with any
add/move/delete decisions from the triage), and (c) confirms green
ctest scoped to that test class.  One file = one commit.

Branch state during the stabilization window:
- Build green (unmigrated files don't compile, but they're not in
  the build).
- ctest green (unmigrated TEST_F don't run, but they're not
  registered).
- Coverage temporarily reduced — that's the price of stabilization.

---

## Previous Step 3 plan (superseded by the revised order above; preserved for context)

| Sub-step | Scope | Status |
|---|---|---|
| 3A | Build `tests/test_framework/curve_test_setup.h` (gen_keypair + populate_known_role helpers) | shipped — slice 1 |
| 3B | Migrate the 10 broker-using test fixtures (~90 test bodies) onto the helper.  One commit per fixture class. | 7 of 10 files done (slices 2–8); remainder folded into task #154 |
| 3C | Delete `BrokerService::Config::enforce_ctrl_admission` field + all `cfg.enforce_ctrl_admission = X` lines.  Build + full ctest. | shipped as 3-revised-C |
| 3D | `HubHost::startup()` throws `std::logic_error` on empty `auth().client_pubkey`. | shipped as 3-revised-B |
| 3E | Move 5 algorithm-only tests from `MetricsPlaneTest` (L3) to a new L2 fixture under `test_layer2_service/test_metrics_query_engine.cpp` (or extend `test_metrics_api.cpp`). | folded into task #154 (per-file triage) |
| 3F | Delete `RoleIdentityPolicyBrokerTest` L3 fixture + production code: `RoleIdentityPolicy` enum, `check_role_identity()`, `KnownRole`-with-strings, `ChannelPolicyOverride`.  Per HEP-CORE-0035 §8 Phase 6.  Full dead-code audit (grep) before delete. | shipped as 3-revised-D |
| 3G | Delete `BrokerService::Config::use_curve` field.  Last to land; broker always CURVE+ZAP. | shipped as 3-revised-E |

---

## Resolved decisions (kept for cross-reference; phase D context)

| # | Decision | Where it landed |
|---|---|---|
| P-API | ZmqQueue auth shape — additive `*_with_auth` overloads | Phase C `7b7944e8` |
| P-Wire | Channel-auth sync semantics — notify-then-pull (broker fires `CHANNEL_AUTH_CHANGED_NOTIFY`; producer pulls via `GET_CHANNEL_AUTH_REQ`) | HEP-0036 §6.5 (amended 2026-06-04; supersedes 2026-06-02 snapshot-push-with-ACK + 2026-05-28 delta) |
| P-Vault | Where known roles live — separate `<hub_dir>/vault/known_roles.json` file mode 0600 | Phase B `a6b44ff8`; HEP-0035 §4.8 |
| P-Threading (CTRL) | Broker-side CTRL ROUTER ZAP — caller-pumped from broker poll thread, no internal thread | HEP-0036 §7.1 + D2 `d18d2e91` |
| P-Threading (data) | Producer-side data ROUTER ZAP — caller-pumped from BRC poll thread, no internal thread | HEP-0036 §7.1; Phase C `28a06046` + `827474f0`; producer-side install pending (task #103) |
| P-S3 | `current_allowlist_` atomic primitive — `PortableAtomicSharedPtr` | Phase C `7b7944e8` |
| P-Schema | `ChannelAccessEntry` shape — two fields, per-producer info per-producer | HEP-0036 §4.1 (locked) |
| P-Push | How broker pushes update — reuse CTRL DEALER/ROUTER in reverse direction | HEP-0036 §6.5 |
| P-Default | Empty allowlist semantics — deny-all | HEP-0036 §6.5 |
| P-Migration | Operators with pre-auth vaults — auto-derive pubkey; absent file = deny-all + admit-none until populated by CLI | HEP-0035 §4.8.4 |

---

## Considered-but-not-pursued (kept for design history)

| Idea | Source | Reason |
|---|---|---|
| `--allow-anonymous-roles` flag (S6 option c) | tech_draft §12.5 S6 | Empty `known_roles.json` already maps to deny-all per HEP-0035 §4.8.4; the friendly-bootstrap need is satisfied by the `--add-known-role` CLI + clear deny-all diagnostic.  Revisit only if Phase H demo migration finds it necessary. |
