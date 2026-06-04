# Authentication / PeerAdmission TODO

**Scope:** Open items for HEP-CORE-0035 (Hub-Role auth + federation
trust) and the PeerAdmission feature track that closes out the
data-channel CURVE auth gate.

**Authoritative design lives in:**
- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` — Layer-1 ZAP + Layer-2 federation trust gate + key-file ACL discipline (§4.6) + runtime key handling (§4.7).
- `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md` — Three-tier auth (transport / identity / authorization), `ChannelAccessIndex` (§4.1), `CHANNEL_AUTH_UPDATE` snapshot wire (§6.5), per-producer pubkey + endpoint (§6.4).
- `docs/HEP/HEP-CORE-0017-Queue-Abstraction.md` §3.3 — `RxQueueOptions::producer_peers` queue auth contract.

**Status source of truth:** `docs/TODO_MASTER.md` (when PeerAdmission
work is the active sprint).

---

## Current PeerAdmission state (2026-06-02)

| Phase | Status | Notes |
|---|---|---|
| A — Abstraction (PeerAdmission interface) | ✅ shipped | commit `d5a90f29` |
| B — KnownRole + CLI | ✅ shipped | commit `a6b44ff8`; HEP-0035 §4.8.3/§4.8.4 |
| C — ZapRouter + ZmqQueue CURVE | ✅ shipped + closed | Phase C close-out commits `62bda863..47aa0374` |
| D — Broker glue (gate closes) | 🚧 in flight — D1 + D2 ✅; D3–D7 ⏳ | D1 commit `cacea477` (ChannelAccessIndex in HubState); D2 commit `d18d2e91` + close-out (CTRL ZAP install + federation peers in allowlist + L4 roundtrip fix) |
| E — Admin loopback enforcement | ⏸ planned | Unblocked once D ships |
| F — Federation peer ZAP parity | ⏸ planned | Depends on E + Federation HEP (#105) |
| G — SHM auth migration | ⏸ planned | Independent of D/E/F; can interleave |
| H — Demo migration | ⏸ planned | Last; needs D shipped end-to-end |
| X — Runtime key hardening | ⏸ planned | HEP-0035 §4.7; task #102 |

---

## Phase D — Broker glue

`HubState` holds the `ChannelAccessIndex` (HEP-CORE-0036 §4.1 line 388);
`BrokerServiceImpl` installs the CTRL ROUTER ZAP handler against the
operator-defined allowlist and (in D3+) pushes `CHANNEL_AUTH_UPDATE`
snapshots when consumer membership changes.

Steps:

1. **D1 — `ChannelAccessIndex` in `HubState`** ✅ shipped (`cacea477`).
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
2. **D2 — Broker CTRL ROUTER ZAP handler** ✅ shipped (`d18d2e91` +
   close-out).  HubHost startup loads
   `<hub_dir>/vault/known_roles.json` via `KnownRolesStore` and copies
   the entries into `BrokerService::Config::known_roles`.
   `BrokerServiceImpl::run()` builds the initial CTRL `PeerAllowlist`
   from the UNION of `cfg.known_roles[].pubkey_z85` AND
   `cfg.peers[].pubkey_z85` (federation peer DEALERs per
   HEP-CORE-0035 §4.2), wires it into a `BrokerCtrlAdmission`
   (PeerAdmission impl backed by `PortableAtomicSharedPtr`), installs
   via `ZapRouter::instance().register_domain("broker.ctrl", ...)`,
   and pumps `ZapRouter::pump_one(0ms)` after each `zmq::poll`.
   `Config::enforce_ctrl_admission` defaults to `true` (production
   deny-all); test L3 fixtures that use CURVE for wire encryption
   only set it to `false`.  L4 `RoundTrip_PlhHubKeygenAndRunPlhRoleRegisters`
   exercises the production path end-to-end:
   `plh_role --keygen` → `RoleVault::open` → `plh_hub --add-known-role`
   → `plh_hub <hub_dir>` → role connects + REG_REQ succeeds.
3. **D3 — `CHANNEL_AUTH_UPDATE` wire frame.**  broker_proto 5 → 6.
   Snapshot semantics per HEP-0036 §6.5 (locked 2026-06-02): single
   `allowlist[]` array of plain Z85 strings; receiver REPLACES cache.
4. **D4 — Role-side dispatch.**  `BrokerRequestComm` recognizes the
   inbound message type; `RoleAPIBase` looks up the matching tx_queue
   by channel name and calls `set_peer_allowlist(snapshot)`.
5. **D5 — `CONSUMER_REG_ACK.producers[]` array.**  One entry per
   producer of the channel — supports fan-in (HEP-CORE-0023 §2.1.1).
   Each entry: `{role_uid, pubkey, endpoint}`; SHM transport also
   carries `shm_secret`.
6. **D6 — L3 tests.**  Broker pushes allowlist on consumer reg /
   dereg; producer applies; consumer with wrong pubkey rejected;
   revocation propagates within the contract bound.
7. **D7 — L4 test.**  Full dual-hub data flow with auth gates closed.

Tracked as task #126.  Sub-tasks (D1–D7) are commits inside the Phase
D landing window.

## Resolved decisions (for reference)

| # | Decision | Where it landed |
|---|---|---|
| P-API | ZmqQueue auth shape — additive `*_with_auth` overloads | Phase C `7b7944e8` |
| P-Wire | CHANNEL_AUTH_UPDATE semantics — snapshot, not delta | HEP-0036 §6.5 (locked 2026-06-02) |
| P-Vault | Where known roles live — separate `<hub_dir>/vault/known_roles.json` file mode 0600 | Phase B `a6b44ff8`; HEP-0035 §4.8 |
| P-Threading (CTRL) | Broker-side CTRL ROUTER ZAP — caller-pumped from broker poll thread, no internal thread | HEP-0036 §7.1 + D2 `d18d2e91` |
| P-Threading (data) | Producer-side data ROUTER ZAP — caller-pumped from BRC poll thread, no internal thread | HEP-0036 §7.1; Phase C `28a06046` + `827474f0`; producer-side install pending (task #103) |
| P-S3 | `current_allowlist_` atomic primitive — `PortableAtomicSharedPtr` | Phase C `7b7944e8` |
| P-Schema | `ChannelAccessEntry` shape — two fields, per-producer info per-producer | HEP-0036 §4.1 (locked) |
| P-Push | How broker pushes update — reuse CTRL DEALER/ROUTER in reverse direction | HEP-0036 §6.5 |
| P-Default | Empty allowlist semantics — deny-all | HEP-0036 §6.5 |
| P-Migration | Operators with pre-auth vaults — auto-derive pubkey; absent file = deny-all + admit-none until populated by CLI | HEP-0035 §4.8.4 |

## Deferred decisions (each tied to its phase)

| # | Decision | Affects | Tentative direction |
|---|---|---|---|
| P-InboxQueue | InboxQueue admission policy location | Phase E | InboxQueue implements `PeerAdmission` directly, no queue inheritance — preserves REQ/REP nature |
| P-Admin | AdminService — CURVE-wrap or loopback-only? | Phase E (task #127) | Hard loopback-only enforce (refuse non-loopback bind) for v1; CURVE-wrap is HEP-CORE-0035 §5 future work |
| P-SHM-Identity | What is a PeerIdentity for SHM? | Phase G (task #129) | Broker-issued `shm_secret` primary; optional uid guard if operator sets it; broker controls the gate via secret issuance |
| P-Demos | How existing demos migrate | Phase H (task #130) | Transitional `--allow-anonymous-data` flag, gated to refuse-bind on non-loopback endpoints; demos updated incrementally |
| P-HEP | When to sync HEPs vs hold tech_draft | Close-out (task #131) | Tech_draft was archived 2026-06-02; HEPs are now the authoritative source.  No further sync needed unless H surfaces gaps |

## Considered-but-not-pursued

| Idea | Source | Reason |
|---|---|---|
| `--allow-anonymous-roles` flag (S6 option c) | tech_draft §12.5 S6 | Empty `known_roles.json` already maps to deny-all per HEP-0035 §4.8.4; the friendly-bootstrap need is satisfied by the `--add-known-role` CLI + clear deny-all diagnostic.  Revisit only if Phase H demo migration finds it necessary |

## Phase D landing — HEP-0035 §4.6.5 "no-bypass" cleanup (2026-06-04)

Full-ctest sweep on 2026-06-04 surfaced 97 failures rooted in the
close-out-3 mismatch WARN + test fixtures that paired
`use_curve=false` with `enforce_ctrl_admission=true` (the new
default).  Audit concluded that HEP-CORE-0035 §2 mandates CURVE +
admission unconditionally — both production AND tests — and the
two flags are HEP violations.

**Steps 1+2 landed in the working tree (NOT yet committed):**

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

ctest result after Steps 1+2: 97 → 1 failure
(the remaining one is the pre-existing `PlhRoleInitTest` 60s
flake, task #93; passes in isolation).

**Phase 1 known bugs (surfaced during landing, deferred to Phase 2):**

- **BRC socket monitor does not detect ZMQ_EVENT_DISCONNECTED under CURVE.**
  Surfaced 2026-06-04 by migrating
  `HubHostIntegrationTest.HubHost_Shutdown_BreaksClientConnection`
  onto the harness.  Under NULL-mech, BRC's `on_hub_dead` callback
  fires within ~3s of broker socket close.  Under CURVE, it never
  fires (waited 10s; `is_connected()` stays `true`).  This breaks
  HEP-CORE-0023 §2.5.3 "disconnect is terminal" production semantic
  — a role connected via CURVE would not learn its broker has gone
  down.  The test is currently `GTEST_SKIP`'d with a citation;
  un-skip when fixed.  Likely site: `src/utils/network_comm/broker_request_comm.cpp`
  socket-monitor setup / poll path; check whether the monitor is
  installed on the DEALER socket before vs. after CURVE handshake
  setup, and whether libzmq emits DISCONNECTED for CURVE sockets the
  same way it does for NULL.  Severity: **HIGH** — Phase 2 review
  must address before HEP-0035 §4.6.5 landing closes.

**Landing-phase progress (commits on `feature/lua-role-support`):**

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

**Stop-here decision (2026-06-04):**

After 8 slices the harness is proven on both Pattern A and B, but
the remaining migration (7 files, ~150 fixtures) is uniformly bigger
than what's done.  Continuing to migrate tests onto an API that will
itself change (when 3C / 3D / 3F / 3G land) is wasted effort — tests
get touched twice.

**Revised order (decided 2026-06-04 — designer call: "stabilize
API/design first"):**

| Step | Scope | Notes |
|---|---|---|
| 3-revised-A | **Fix the BRC monitor CURVE bug** (`broker_request_comm.cpp` socket-monitor poll path). | Unblocks the skipped fixture in slice 4 + restores HEP-CORE-0023 §2.5.3 production semantic.  Independent of other steps. |
| 3-revised-B | `HubHost::startup()` throws `std::logic_error` on empty `auth().client_pubkey`. | Unblocks deleting the dead `bcfg.use_curve = !empty()` conditional. |
| 3-revised-C | Delete `BrokerService::Config::enforce_ctrl_admission` field + every `cfg.enforce_ctrl_admission = X` line in tests and CMake.  Unmigrated test files will fail to compile; that's expected. | Doesn't break the 7 migrated files (the harness sets the field but stops once it's gone). |
| 3-revised-D | Delete legacy `RoleIdentityPolicy` enum, `check_role_identity()`, `KnownRole`-with-strings, `ChannelPolicyOverride`, `RoleIdentityPolicyBrokerTest` (L3 fixture).  Per HEP-CORE-0035 §8 Phase 6.  Includes dead-code audit (grep). | Removes the deprecated machinery so tests can't depend on it. |
| 3-revised-E | Delete `BrokerService::Config::use_curve` field.  Broker always CURVE+ZAP. | Final field-deletion. |
| 3-revised-F | Now the production API is stable.  Migrate the remaining 7 worker files onto the (stable) harness.  Each file: triage fixtures (keep at L3 / move to L2 / delete redundant) before mechanical migration. | One commit per file.  Some files will need L2 reorganization (5 metrics algorithm tests → L2; possibly more after triage). |
| 3-revised-G | Full ctest green from clean baseline.  Confirm no regressions. | This is the close-out gate. |
| Phase 2 | Full-codebase (a) review against HEPs. | Per design conversation: only meaningful on a stable baseline. |

The bug recorded in **"Phase 1 known bugs"** above (BRC monitor CURVE
blindspot) is now elevated to **the first step (3-revised-A)** of the
new ordering since:
1. It's a real production correctness bug independent of test work.
2. Fixing it unblocks `HubHostIntegrationTest.HubHost_Shutdown_BreaksClientConnection`.
3. Migration of any remaining test that exercises hub-dead detection
   would be dishonest until this is fixed (the test would falsely
   pass-through the broken path).

**Rationale for the reorder (recorded 2026-06-04 per designer
guidance):** if tests need to be migrated again when API changes,
that's pointless effort.  Stabilize production API/design first;
migrate tests once against the stable shape.

### Exclusion procedure during lib-stabilization window (3-revised-B → 3-revised-E)

Once we start deleting production fields and legacy code (steps
3-revised-B through 3-revised-E), the unmigrated L3 worker files
will be **build-breaks**, not just runtime failures:

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

**To keep `ctest` green throughout the stabilization window**, the
unmigrated worker files MUST be excluded from the build before any
of those deletions land.  Exclusion is two-part — both halves are
mandatory:

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

The 7 already-migrated worker files stay enabled throughout:
`datahub_channel_group_workers`, `datahub_broker_request_comm_workers`,
`hub_host_integration_workers`, `broker_consumer_workers`,
`broker_schema_workers`, `broker_admin_workers`,
`zmq_endpoint_registry_workers`.

**Per-file restoration during step 3-revised-F**: each file's
migration commit (a) un-comments the CMakeLists.txt source entry,
(b) restores the TEST_F entries in the driver (with any
add/move/delete decisions from the triage), and (c) confirms green
ctest scoped to that test class.  One file = one commit.

Branch state during the stabilization window:
- Build green (unmigrated files don't compile, but they're not in
  the build).
- ctest green (unmigrated TEST_F don't run, but they're not
  registered).
- Coverage temporarily reduced — that's the price of stabilization.

---

**Previous Step 3 plan (superseded above; kept for context):**

| Sub-step | Scope | Status |
|---|---|---|
| 3A | Build `tests/test_framework/curve_test_setup.h` (gen_keypair + populate_known_role helpers) | ✅ slice 1 |
| 3B | Migrate the 10 broker-using test fixtures (~90 test bodies) onto the helper.  One commit per fixture class. | 🚧 7 of 10 files done (slices 2–8); paused per stabilize-first decision |
| 3C | Delete `BrokerService::Config::enforce_ctrl_admission` field + all `cfg.enforce_ctrl_admission = X` lines.  Build + full ctest. | moved to 3-revised-C |
| 3D | `HubHost::startup()` throws `std::logic_error` on empty `auth().client_pubkey`. | moved to 3-revised-B |
| 3E | Move 5 algorithm-only tests from `MetricsPlaneTest` (L3) to a new L2 fixture under `test_layer2_service/test_metrics_query_engine.cpp` (or extend `test_metrics_api.cpp`). | folded into 3-revised-F (per-file triage) |
| 3F | Delete `RoleIdentityPolicyBrokerTest` L3 fixture + production code: `RoleIdentityPolicy` enum, `check_role_identity()`, `KnownRole`-with-strings, `ChannelPolicyOverride`.  Per HEP-CORE-0035 §8 Phase 6.  Full dead-code audit (grep) before delete. | moved to 3-revised-D |
| 3G | Delete `BrokerService::Config::use_curve` field.  Last to land; broker always CURVE+ZAP. | moved to 3-revised-E |

## Phase D close-out follow-ons (test + spec gaps surfaced 2026-06-03)

These are tracked here so they survive context resets per CLAUDE.md
§"Session hygiene" — open items must live in a subtopic TODO, not
only in chat history.

- **Allow-path L3 pin for D2.**  `DatahubBrokerHealthTest.CtrlZapDenyPath`
  pins the deny path.  Symmetric allow-path L3 needs a BRC client
  whose `client_pubkey` is added to the broker's `known_roles[]`
  before connect; that requires the test infrastructure to thread
  explicit CURVE keys into the worker's broker config (the existing
  L3 worker pattern uses ephemeral BRC keys, which the worker
  process cannot know ahead of time to pre-register).  Smallest
  fix: extend the `BrokerService::Config` test-side construction to
  include a pre-generated `known_roles[]` entry built from the test
  client's keypair.  Effort: S.
- **`plh_role --keygen` does not publish `<vault>.pub`.**
  HEP-CORE-0035 §4.8.3 specifies `plh_hub --add-known-role <role.pub>`
  as the canonical operator workflow; that requires the role binary
  to publish a sibling `.pub` file alongside the vault (the way
  `plh_hub --keygen` publishes `hub.pubkey`).  Currently the L4
  RoundTrip test opens the role vault programmatically to extract
  the pubkey — a test backdoor.  Mirror `HubVault::publish_public_key`
  for `RoleVault` (atomic O_EXCL + O_NOFOLLOW + mode 0644 +
  symlink defense per HEP-CORE-0035 §4.6.4).  Effort: M.
- **Hot-reload of `known_roles.json` on a running hub** (HEP-CORE-0035
  §4.8.5).  `BrokerCtrlAdmission::set_peer_allowlist` exists with
  no caller; the admin RPC (`/admin/reload-known-roles` or
  similar) is the missing wiring.  Operators that run
  `--add-known-role` against a running hub today must restart it
  to pick up the change.  Effort: M.
- **Multi-peer ZAP backlog draining.**  `ZapRouter::pump_one(0ms)`
  is called once per broker poll cycle.  Under N-peer simultaneous
  reconnect (e.g. hub restart) handshake latency is
  ~`kPollTimeout * N`.  Convert to `while (pump_one(0ms)) {}` to
  drain backlog.  Effort: trivial.

---

## Parallel / adjacent tracks

These have their own task IDs but touch the same surface:

- **Task #102** — HEP-CORE-0035 §4.7 runtime key handling (mlock + no-core-dump + zeroing, cross-platform).
- **Task #103** — HEP-CORE-0017 §3.3 + HEP-CORE-0036 implementation: `RxQueueOptions::producer_peers` + ZmqQueue dynamic peer API.
- **Task #104** — Sibling HEP updates: schema / FSM / CURVE-wiring per HEP-CORE-0036 §14.
- **Task #105** — Federation protocol design + cross-hub reg/comm verification.
- **Task #106** — HEP-CORE-0038 + impl: script-accessible vault keystore (`api.vault_save/load`).
- **Task #120** — Windows pathway hardening for HEP-CORE-0035 §4.6 floor.
