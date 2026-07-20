# Authentication / PeerAdmission TODO

**Scope: Line 1 (Main auth chain — CURVE end-to-end across ZMQ + SHM)
of the three-line breakdown in `docs/TODO_MASTER.md`.**  For Line 2
(SMS consolidation) see HEP-CORE-0043 §0.4; for Line 3 (Broker SHM
observer) see HEP-CORE-0045 §10.

**Line 1 authoritative design lives in:**

- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md`
- `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md`
- `docs/HEP/HEP-CORE-0017-Queue-Abstraction.md` §3.3
- `docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md` (incl. §8.5.2
  NORMATIVE seckey representation at security-module boundary)
- `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md`
- `docs/HEP/HEP-CORE-0042-Channel-Attach-Coordination-Protocol.md`
- **`docs/HEP/HEP-CORE-0044-AttachProtocol.md`** — AttachProtocol
  primitive (Frame 1/2/3 wire spec + `IAttachChannel` seam + protocol
  helpers).  Consumed by HEP-0041 §5 for consumer-attach and by
  HEP-CORE-0045 for observer-attach.

**Status source of truth:** `docs/TODO_MASTER.md` § "Resume point"
— three-line summary + per-line remaining work (2026-07-08).

**⚠ Admin plane CURVE migration — DESIGN DONE, IMPL PENDING (2026-07-15).**
The AdminService REP socket is still plaintext + bearer token today; the
ctor even permits a non-loopback bind that sends the admin token in
cleartext. The CURVE-secured design (reuse the broker CURVE keypair to
`curve_server` the admin socket; keep the init-generated admin token as a
mandatory + now-encrypted gate; loopback default) is **finalized in
HEP-CORE-0033 §11.1 + §11.3**. Simple version only — per-operator
`known_admins` allowlist / streaming / ROUTER move are future expansion,
noted in §11. Implementation checklist:
`docs/tech_draft/DRAFT_curve_admin_protocol_2026-07-15.md`. Blocks the 3
admin-triggered tests in the HubHostBrokerHandle sweep
(`close_channel` ×2, `broadcast_hub_queue`) — they stay in L3 with a
RATIONALE pointing at §11 until the admin CURVE socket + `AdminWireClient`
land.

**Completed-work archives (verbatim prose retained for context):**

- `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
  — Phase A/B/C; D1+D2+D3; landing-phase §4.6.5 no-bypass cleanup.
- `docs/archive/transient-2026-06-09/todo-completions/AUTH_TODO_completions.md`
  — HB-1..HB-6 audit + 2026-06-05 PM REFRAME + C1..C5 cleanup chain
  + HEP-CORE-0040 impl chain (#165–#176 + #187).
- `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md`
  — AUTH-1 full narrative + AUTH-2/3 detailed + AUTH-4 SUPERSEDED
  block + HEP-0041 Phase 1 substep 1a-1k narratives + REVIEW-A/B
  close-outs + §5b parallel track table.

---

## Design principles — single source of truth for AUTH-N execution

**Every entry in this doc — current or future — anchors to these HEP
sections.  Implementations that contradict them are wrong; design
discussions that re-litigate them are wasted.**

1. **HEP-CORE-0036 §I11 — Framework provides protocol; scripts
   provide coordination.**  The framework guarantees validated
   identity, asynchronous notification of membership changes, atomic
   allowlist updates, and observable list state.  It does NOT
   synchronise roles' decisions.  Cross-role coordination (when to
   start, who's ready) is the script's job, implemented via the
   observable allowlist + band + inbox.
2. **HEP-CORE-0036 §6.5 producer-side handler flow (normative).**
   Notify-then-pull via the existing BRC → IncomingMessage queue →
   worker-thread drain.  BRC poll thread enqueues; worker thread
   drains, fires sync `GET_CHANNEL_AUTH_REQ` via the BRC, applies the
   result via `ZmqQueue::set_peer_allowlist`.  On pull failure: log
   + return; recovery via next notify or hub-dead → re-register.  No
   priority dispatch, no critical-error escalation, no new threads.
3. **HEP-CORE-0036 §I3 + §I5 + §8.2 — Race-window behaviour is
   PROTOCOL.**  Between broker decision and producer cache update,
   the producer's PUSH socket continues to serve handshakes using
   its current ZAP cache.  Consumer joining during the gap retries
   CURVE until cache converges; existing CURVE sessions trusted for
   their lifetime.  This is the contract — not a bug.
4. **HEP-CORE-0036 §I9 — Three-tier separation.**  Scripts never
   touch transport primitives.  Broker emits events; framework reacts
   via queue APIs; queue handles transport plumbing; script sees
   membership state, not sockets.
5. **HEP-CORE-0040 §8.5.2 — Seckey representation at module
   boundary.**  Raw 32 bytes inside the security module; Z85 only at
   file/wire/display.  No mixed conventions across the boundary.

If a proposed change appears to require new threading, priority
dispatch, or critical-error escalation around the auth flow, re-read
§I11 + §6.5 first — almost always it doesn't.

---

## Current PeerAdmission state

| Phase | Status | Notes |
|---|---|---|
| A — Abstraction (PeerAdmission interface) | ✅ | 2026-06-05 archive |
| B — KnownRole + CLI | ✅ | 2026-06-05 archive |
| C — ZapRouter + ZmqQueue CURVE | ✅ | 2026-06-05 archive |
| C-chain — Strict-CURVE cleanup (C1..C5) | ✅ | 2026-06-09 archive |
| HEP-0040 — Locked Key Memory impl chain | ✅ | 2026-06-09 archive |
| HEP-0040 §8.5.2 — seckey raw-32 contract | ✅ | shipped 2026-06-26 (#291 fix) |
| HEP-0036 §5b — canonical wire schema unification | ✅ | shipped 2026-06-25/26; archive §1 |
| D — Broker glue (gate close) | ✅ | AUTH-1/2/3 ✅; AUTH-5 ✅ (#104, 2026-06-27); AUTH-7 ✅ (L4 `ZmqE2E_Authorized/Unauthorized/MultiProducer` green).  **Residual OPEN: AUTH-6 File-10-Suite-2 delete (#152)** — verified 2026-07-18 |
| E — Admin CURVE + operator console | 🟢 **REP SERVER SHIPPED 2026-07-19; console = design-of-record** | **Shipped (transitional):** admin REP now `curve_server` (`kHubIdentityName`, use-not-export) + **`zap_enforce_domain=1`** (§11.1: keeps admin off the broker's live §7.4 ZAP handler — empty-domain alone is rejected "no domain registered for ''"; proven by neutered-flag negative check).  `token_required` toggle deleted (mandatory token); loopback default-not-required.  `test_layer2_admin_service` 29/29 green; real-broker guard = `HubHost_AdminEnabled_RoundTripWorks`.  **Design-of-record now the operator console** (HEP-0033 §11.0 end-to-end framework, consolidated 2026-07-19): `ROUTER`/`DEALER` persistent session, sealed connection-bound session id (token once at establishment), reverse notification path, `origin_uid` provenance (Option-A cascade), `script:@hub_uid` tags.  Deferred: per-operator privilege (RBAC), pubkey `known_admins` allowlist.  **ENVELOPE = typed `WireEnvelope`** (decided 2026-07-19): console built native on HEP-0046 typed envelope (new admin msg_types + `wire_bodies` bodies), NO JSON `{method,token,params}` surface — admin is the **first fully-typed pathway** + reference for **#57** (broker REG migration); status notes in `wire_dispatch.hpp` + HEP-0033 §11.1.  Reuse map (4-area API scan 2026-07-19): CURVE arm = extract shared helper (dup'd broker/admin); NOTIFY = `send_to_identity`; queue = `MonitoredQueue<T>`; seal = secretbox+KeyStore+format_tools.  **DONE**: (slice 1) sealed session identity `admin_session.{hpp,cpp}` + `test_layer2_admin_session` 7/7 (HEP-0033 §11.0.5); (2a) FULL typed admin protocol — 13 bodies covering establishment + all 11 methods + typed `ADMIN_ERROR` (`AdminHello/Ping/CloseChannel/Session/Named/BroadcastChannel/QueryMetrics Req` + `Result/Status/Error` Ack) + `ADMIN_*` msg_type constants in `wire_bodies.{hpp,cpp}`; 16 body tests, wire suite 61/61.  **Body→param mapping for the transport (all 11 handlers stay unchanged)**: ping/list_channels/list_roles/list_bands/list_peers/request_shutdown → `AdminSessionReqBody` (params={}); get_channel → `AdminNamedReqBody` name→`params.channel`; get_role → name→`params.uid`; query_metrics → `AdminQueryMetricsReqBody` filter IS params; close_channel → `params.channel`; broadcast_channel → `params.{channel,message,data}`.  Queries reply `AdminResultAckBody{result}`, controls `AdminStatusAckBody{status}`, ping `AdminPingAckBody{status}`; (2b) shared `arm_curve_server(sock,key)` helper `curve_socket.hpp` — admin REP converted, 29/29 green (broker + inbox convergence = dedup follow-up); Peer-Address extraction verified available over TCP (keep both facts); cppzmq audit clean (migrated `zmq_ctx_set`→`ctx.set(ctxopt::blocky)`; `zmq_socket_monitor` documented-justified for immediate-disconnect poll integration).  **(2c) DONE** — ROUTER transport rewrite (`admin_service.cpp` run loop + `dispatch_typed`: `parse_router_recv`→typed body→`verify_session_id`→unchanged handler→`build_router_send`), Peer-Address from frame 0, `zmq_addon.hpp` for multipart; `AdminWireClient` (`tests/test_framework/admin_wire_client.h`); `test_admin_service` rewritten REP→typed console, **8/8 green** (establish/wrong-token/no-session/ping/list_channels/query_metrics/close-not_found/unknown-msg-type against a real started HubHost = §7.4 regression guard).  **FINDING (for CURVE review)**: §11.0.4 says control commands are fire-and-forget but `handle_close_channel` does a synchronous existence check → `not_found`; doc↔code tension, unresolved.  **REMAINING**: (3) reverse `MonitoredQueue` notify path (console notifications, §11.0.1 layer 6); (4) `origin_uid` provenance plumbing (needs broker `request_close/broadcast_channel` to carry origin_uid + scoped stamp); (5) migrate 3 admin-triggered sweep tests onto `AdminWireClient`.  Design: HEP-0033 §11 + `DRAFT_curve_admin_protocol` |
| CURVE-integration review (2026-07-19) | 🔶 3 DECIDED security work items | Full docs+code review — `docs/code_review/REVIEW_CURVE_Integration_2026-07-19.md`.  Verdict: **no CURVE bypass; code sound; findings = doc drift** + these 3 code gaps the maintainer DECIDED to close: **(a) encrypt `known_roles`** into the hub vault (HEP-0035 §4.8 design is intended; shipped plaintext `known_roles.json` is the gap); **(b) add inbox replay defense** (HEP-0027 msgpack frame is integrity-only — no nonce/ts/envelope_hash; a captured frame replays verbatim); **(c) add admin in-session replay** (per-command nonce/ts on admin `wire_bodies`; §11.0.5 currently defends only cross-connection).  **Verified: full ctest 2592/2592 pass** after fixes.  **Doc fixes applied**: HEP-0036 §11.2 dev_mode tombstone, HEP-0040 §5.2 `with_seckey` raw-32, HEP-0033 §11.0.4 control-semantics (synchronous `not_found`), §11.0.5 session-key home + use-not-export, 10× `§7.4`→`HEP-CORE-0036 §7.4`, §11.0.2 typed-envelope.  Doc-reconciliation remainder (CONSUMER_REG_ACK field names, HEP-0038 banner, HEP-0041 §6.4/§D4.5 fencing, §14.3 admin bodies) listed in the review doc. |
| F — Federation peer ZAP parity | ⏸ | depends on E + #105 (post-MVP) |
| G — SHM auth migration | ✅ | HEP-0041 Phase 1 ✅; #262 mutual-auth ✅ (default flipped 2026-07-16); 1j ✅; **#275 S3/S4/S5 SHIPPED** (`set_shm_secret` deleted `hub_shm_queue.cpp`); REVIEW-C/D/E ✅ → **🟢 Phase 1 PRODUCTION-READY**.  Residual OPEN: **#275 S2 worker-scan** (datahub_producer_consumer/metrics/schema/query_engine) |
| H — Demo migration | ⏸ | last; needs D shipped end-to-end |
| **SEC-Fold — Consolidated Security Module + HEP** | 🔴 **PLANNING** | filed 2026-07-04 — see `SEC-Fold` section below |

---

## SEC-Fold — Consolidated Security Module + unified HEP (filed 2026-07-04)

**Trigger:** 2026-07-04 CI failure exposed a `sodium_init()` gap —
no single module owned libsodium initialization; every consumer file
`#include`d `<sodium.h>` and called sodium directly.  Fixed for the
narrow case in commit `9d0a7eb4` (module-boundary gate at KeyStore
public API + init flag on SMS + init call in SMS ctor + debug logs),
but the fix is a stopgap.  The underlying design smell — **security
concerns spread across 9 files and 6 HEPs, with no single owner** —
remains.

**Two tasks filed for the correct architecture.  Both blocked by
neither of them being started; do in the order below.**

**SEC-Fold-1 — HEP consolidation (docs-only, do FIRST).**  Fold
HEP-CORE-0035, -0036, -0038, -0040, -0041, -0042 plus two live tech
drafts (`DRAFT_keystore_ephemeral_and_script_crypto_2026-07`,
`DRAFT_broker_shm_observer_2026-07`) into ONE unified HEP.
Structure sketched in
`docs/tech_draft/DRAFT_security_module_and_hep_consolidation_2026-07.md`
Part 2.  Old HEPs marked SUPERSEDED and archived per
`DOC_STRUCTURE.md §2.2`.  Sizing: 1-2 commits.
Ships BEFORE SEC-Fold-2 so the module refactor is guided by the
unified design, not the reverse.

**SEC-Fold-2 — Module fold (C++ refactor, do SECOND).**  One module
(rename `SecureMemorySubsystem` → `SecureSubsystem`, or new class
`Secure`) owns EVERY libsodium API.  Nothing else `#include
<sodium.h>` in production.  Every consumer changes from raw sodium
call → `secure().X(...)`.  KeyStore becomes a submodule of `Secure`,
not a peer.  Wrapper API sketched in tech draft Part 1.  Sizing:
~9 production files + tests, 500-800 LOC touched, ~3-5 commits.

**Design record:**
`docs/tech_draft/DRAFT_security_module_and_hep_consolidation_2026-07.md`.

**Why it matters — from the 2026-07-04 user message:** "we have
real application and needs for key management and security
management and our code has scattered design all over the place."
The current design produces runtime bugs (2026-07-04 sodium_init
gap), design questions requiring 3-HEP paging, and no compile-time
guarantee that consumers use sodium correctly.  All three problems
close structurally after SEC-Fold-2.

**Interaction with in-flight work:**

- Task #262 (SHM mutual auth) — can complete before SEC-Fold-2.
  Wire mechanism + config knob already shipped `5c8d04c1`.
- Task #317 C.2 (broker SHM observer) — CURRENTLY IN FLIGHT.
  D1 slice A (`d6f5d621`), D2 (`f7d3a51e`), C.2.a (`029bbe31`),
  C.2.b (`ce956972`) already shipped.  Remaining C.2.c/d + D5 +
  C.3-5 can complete before SEC-Fold-2.
- Task #247 (script crypto API) — SEC-Fold-2 IS partly this task.
  Do SEC-Fold-2 first; #247 collapses into it.
- Task #262 mutual auth L4 test + default flip — folds into
  unified HEP §7.3.

---

## Phase 1 — CURVE chain close (active critical path)

Per `docs/TODO_MASTER.md`, the locked execution order is:

```
AUTH-5 (#104) doc sync  ──┐
AUTH-6 (#154) L3 tests  ──┼──► AUTH-7 L4 e2e  ──► #275 S2..S5  ──► REVIEW-C (#276)
#257 (1j) L3 broker tests │                                            │
                          │                                            ▼
                                                              #262 mutual auth
                                                                       │
                                                                       ▼
                                                              REVIEW-D (#277)
                                                                       │
                                                                       ▼
                                                              REVIEW-E (#278)
                                                                       │
                                                                       ▼
                                              🟢 PHASE 1 PRODUCTION-READY
```

**AUTH-1 / AUTH-2 / AUTH-3 / AUTH-4 status:**
- **AUTH-1** ✅ SHIPPED 2026-06-10 through 2026-06-13.  Closed
  as #103 + #211/212/213/214/215/217/219.  See archive §2.
- **AUTH-2** ✅ SHIPPED 2026-06-16.  Closed as #162.  See archive §3.
- **AUTH-3** ✅ SHIPPED 2026-06-13.  Closed as #163.  See archive §4.
- **AUTH-4** SUPERSEDED 2026-06-16 by HEP-CORE-0041 (#244).  See
  archive §5.  Original tasks #164 + #79 closed as SUPERSEDED.
- **AUTH-5** (sibling-HEP doc sync) ✅ SHIPPED 2026-06-27 (#104).  Detail
  extracted 2026-07-18 (commit `633d51c0`; index
  `docs/archive/transient-2026-07-18/todo-completions/`).

### AUTH-6 — L3 broker test revival

> **Tracker:** task **#154** (in-progress).
> **HEP anchors:** §6.5 producer-side handler flow + §I11.
> **Audit:** `docs/code_review/REVIEW_AUTH6_TestDisposition_2026-06-27.md`
> (per-TEST_F disposition table; 194 TEST_F's across 15 files).

**Goal.**  Bring the L3 datahub test suite back to green against the
post-renovation lib, applying README_testing §1.2 rules 6 (RETIRE
obsolete tests) + 7 (layer placement follows what the test
exercises) — NOT a mechanical unmask-and-migrate.

**Audit findings (2026-06-27).**  Per the audit doc:

| Disposition | Count | Notes |
|---|---|---|
| **UNMASK + MIGRATE** (surfaces intact, tests correct) | 142 | Files 1-8 + 11-15 of the audit table |
| **DEFER** (blocked on a not-yet-shipped piece) | 3 | File 9 federation — gated on #105 |
| **DELETE** (slated for outright deletion) | 7 | File 10 Suite 2 — gated on #152 |
| **RE-LAYER to L2** (Pattern-1 helpers; production-consumed) | 4 | File 10 Suite 1 |

Files already unmasked (11-15) **retroactively confirmed valid** —
no rework needed, including the 2026-06-27 commit `86b7b209`
(`test_datahub_hub_host_integration`).

**Foundational primitive — task #177.**  Task #177 (CurveKeyStoreFixture +
HEP-CORE-0040 §172 use-not-export discipline + first batch of migrated
files) ✅ SHIPPED 2026-06-27 (commits `6e819b73` + `db774840`).  Files
already migrated via the fixture: L2 zmq_queue_auth + L2 hub_state; L3
broker_request_comm + channel_group (2026-06-07); L3 hub_host_integration
+ hub_lua_integration + hub_python_integration (2026-06-27).
**Remaining batches under AUTH-6:** batch 2a (broker + broker_protocol +
role_state_machine) → batch 2b (broker_health + metrics + endpoint_registry;
absorbs former trackers #293/#294) → Phase 3 (file 9 DEFER + file 10
DELETE/RE-LAYER) → close out.

**Batch-2a progress (2026-06-29):**
- C0 RoleHandler L2 re-layer ✅ shipped.
- C1 broker_protocol_workers (28 L3 tests) ✅ shipped.
- C2 broker_workers (40 tests) ✅ shipped: 39 unmasked + 1 retired
  (`broker_sch_consumer_citation_match` — verified duplicate of
  `broker_schema_workers.cpp::consumer_schema_id_match_succeeds` by
  reading both bodies + HEP-CORE-0034 §10.3, no contract loss).
  Migration shape: per-test `CurveKeyStoreFixture` seed of every uid the
  test uses, `raw_req` accepts a wire-identity name + auto-decorates
  REG_REQ / CONSUMER_REG_REQ with §5b canonical fields when the seeded
  role identity matches, and an R6 heartbeat is sent between producer
  REG_REQ and consumer CONSUMER_REG_REQ in every test that exercises
  both handlers.  All 2230/2230 ctest green.
- C3 role_state_machine ✅ shipped 2026-06-30 (commit `0a75d9f6`): 12
  single-broker tests unmasked + 7 dual-broker tests retired with L4
  successor handoffs (#296 / #228 / #229 / #299; blocked on #298 multi-hub
  Pattern4Setup extension).  Migration shape mirrors C2.
- C4 broker_health ✅ shipped 2026-06-30 (commit `9bbd549a`): full rewrite
  of 10 single-broker tests against canonical `start_hubhost_broker` +
  `CurveKeyStoreFixture`.  Two tests that need
  `broker.consumer_liveness_check_interval_seconds` (not exposed via
  hub.json) use `start_health_direct_broker` helper.  `ctrl_zap_deny_path`
  builds the broker with empty `role_keys` (deny-all by construction);
  test client's keypair is still seeded in the keystore.
  `dead_consumer_*` 2-subprocess test extends temp-file shape from 3 to
  6 lines (now includes consumer Z85 keypair).
- C6 Phase 3 federation DEFER + RoleIdentityPolicy Suite 1 RE-LAYER
  ✅ shipped 2026-06-30: `test_datahub_hub_federation.cpp` driver
  unmasked with all 3 TEST_F's gated on `GTEST_SKIP() << "#105"`;
  workers file stays masked.  `test_datahub_role_identity_policy.cpp`
  Suite 1 (4 enum helper TEST_F's) moved to
  `tests/test_layer2_service/test_role_identity_policy.cpp`; Suite 2
  (7 broker TEST_F's) stays at L3 and stays MASKED awaiting #152.
  Helpers themselves stay in production for use by
  `broker_service.cpp::check_role_identity` WARN logs + error
  responses.  File 10 Suite 2 DELETE is the only remaining AUTH-6
  bookkeeping item; it lands as part of #152 ship.
- C5 metrics + zmq_endpoint_registry ✅ shipped 2026-06-30 (this commit):
  17 metrics tests + 8 zmq_endpoint_registry tests migrated.
  ZmqEndpointRegistryTest.ReqShape_SyncReqTimesOutOnNoReply RETIRED
  (handoff #307) — its `StubBrcHandle` drove BRC into the timeout branch
  via plain-TCP, which HEP-CORE-0035 §2 strict-CURVE refuses at
  `BrokerRequestComm::connect()`.  Metrics tests now drive the producer
  to kLive via a probe heartbeat with a marker metric before consumer
  registration (HEP-CORE-0036 §5.2 R6 producer-kLive gate).  Local
  `BrcHandle` / `make_test_hub_dir` / `make_reg_opts` / `make_cons_opts`
  / `raw_request` rewritten to canonical harness shape.  All 2282/2282
  ctest green.

**Out of scope (per §I11).**

- Tests that pin priority dispatch, queue-blocking semantics, or
  critical-error escalation — none are in the contract.
- Re-deriving disposition decisions — the audit table is the source
  of truth.

**Closed-as-merged into AUTH-6:** tasks #293, #294, #295 (originally
opened 2026-06-27 as separate trackers for metrics, ZMQ endpoint
registry, and Lua/Python integration revivals — the audit found all
three are uniform UNMASK+MIGRATE alongside files 1-4, so separate
trackers are unnecessary).

**Depends on:** AUTH-1 shipped ✅; task **#177** (KeyStore fixtures)
✅ shipped 2026-06-27 — no remaining blockers; AUTH-6 batches 2a/2b
ready to start.

### AUTH-7 — L4 end-to-end gate close

> **HEP anchors:** §I11 + §5.2 sequence (end-to-end auth flow).

**Goal.**  Full dual-hub auth-gated data flow under the demo
framework.  Final proof that the data-plane CURVE gate is closed
end-to-end and that scripts can build coordination on top.

**Scope (do not expand).**

- Demo manifest with producer + consumer roles, real `plh_role`
  binaries, real CURVE handshakes, end-to-end data flow.
- Assertion: data flows iff consumer is in producer's allowlist;
  data stops iff consumer dereg.
- Optional second demo: a script-side "wait until N peers ready"
  pattern using `api.allowed_peers` — illustrative of §I11.

**Out of scope (per §I11).**

- Framework changes — by AUTH-7, all framework work has shipped.

**Depends on:** AUTH-1 + AUTH-2 + AUTH-3 + AUTH-6 shipped.  SHM gate
close via HEP-0041 Phase 1; AUTH-7's L4 manifest gains a SHM variant
only after #275 S2..S5 + #258 1k ship.

**Progress (2026-07-08 — AUTH-7 ZMQ + SHM both fully green):**

- **SHM variant ✅ shipped 2026-06-26** as `ShmE2E_AuthorizedConsumerReceivesAllSlots` + `ShmE2E_UnauthorizedConsumerDeniedByBroker` (commit `0a341a5b`, task #258).  Both green.
- **ZMQ variant infra shipped 2026-06-30**:
  - **Commit** `de38c541`: extracted `role_e2e_harness.h` from the SHM file — 8 transport-agnostic helpers (log/marker waiters, plh_role binary path, vault keygen, known_roles) into `tests/test_layer4_plh_hub/role_e2e_harness.h`.  SHM tests stayed green.
  - **Commit** `ab6c89d8`: added `test_plh_hub_role_zmq_e2e.cpp` with `ZmqE2E_AuthorizedConsumerReceivesAllSlots` (initially skipped on #246) + `ZmqE2E_UnauthorizedConsumerDeniedByBroker` (✅ green).
- **ZMQ authorized scenario ✅ un-skipped 2026-07-02** by the #246 Phase 3 close-out commits (`d8884e9f` + `3ea1bc4c` + `7aa831c2` — producer emits `PRODUCER_REG_ACK.instance_id` + `snapshot_version` + `CHANNEL_AUTH_APPLIED_REQ`; `e8cebe04` — consumer's §7.1 pre-attach loop).  Test now pins Phase 3a producer markers (`event=ProducerInstanceIdCaptured`, `event=ChannelAuthApplied`) + Phase 3b.2 §7.1 loop markers (`attach:begin`, `attach:success`, `attach:complete admitted=1/1`) alongside the data-flow assertion.
- **ZMQ multi-producer fan-in ✅ shipped 2026-07-08** as `ZmqE2E_MultiProducer_TwoAuthorized` (Scenario C in the same file).  Pins the §7.1 loop under fan-in (`attach:begin producers=2` → `attach:success` per producer → `attach:complete admitted=2/2`) + both producers emitting `event=ChannelAuthApplied` + data flow through the queue.
- **HEP-CORE-0017 §3.3 multi-endpoint PULL ✅ shipped 2026-07-08** — the L4 fan-in test above initially surfaced that `ZmqQueue::start()` connected only to `peer[0].endpoint`.  Same-session close-out: `start()` PULL branch now iterates `producer_peers_` and issues per-peer `connect()` with per-peer `curve_serverkey`; libzmq fair-queues data from all N connected PUSH peers.  `is_configured()` + the peer[0]-promotion in `apply_master_approval` preserved as the HEP-0036 §6.7 Option B "apply_master_approval has run" flag.  L4 test now activates the distinguisher-value protocol requiring `offsets_seen=0,100`.

**Critical-path implication.**  AUTH-7 ZMQ happy path + deny path + multi-producer fan-in loop all green today.  Downstream `REVIEW-D` / `REVIEW-E` unblocked.

---

## HEP-0041 implementation chain — capability transport

Replaces the retired AUTH-4 `shm_secret`-mirror design.

**Status (2026-06-27):** Phase 1 substeps 1a-1h ✅; 1i-mig 1-5 ✅;
1k ✅ (#258, commit `0a341a5b`); 1i-cleanup S1+S2a+S2b+S2c-1..6 ✅;
**S3+S4+S5 + 1j (#257) + #262 mutual auth + REVIEW-C/D/E all OPEN**.
See archive §6 for shipped substep narratives + commit hashes.

### Phase 1 critical path (active rows only)

| Step | Task | Status | Scope |
|---|---|---|---|
| 1k | #258 | ✅ shipped 2026-06-26 | L4 e2e SHM auth-gated data flow.  DISABLED_ prefix dropped; fs::absolute path-resolution fix; read_role_log() switch; #291 root-cause fixed via HEP-CORE-0040 §8.5.2 normative seckey contract. |
| 1i-cleanup S2 | #275 | 🟡 | Migrate ~16 L3 datahub workers off `find_datablock_consumer_impl(name, secret, ...)` / `attach_datablock_as_writer_impl(name, secret, ...)`.  Retire secret-gate tests (e.g. `writer_attach_validates_secret`) with Rule-6 doc-blocks.  Migrate schema/checksum/roundtrip tests.  **Progress 2026-06-30:** `slot_protocol_workers` ✅ (commit `424ccc87`); `c_api_draining_workers` ✅ (added `open_datablock_for_diagnostic_from_fd(int)` lib API + 14 call sites migrated); `slot_allocation_workers` ✅ (3 functions → `make_fd_backed_producer`/`make_fd_backed_pair` + fd-source diag); `c_api_recovery_workers` ✅ (only `heartbeat_manager_registers_and_pulses` was in scope — migrated to `make_fd_backed_pair`; driver `attached (consumer-from-fd)` assertion updated).  **Still to scan:** `datahub_producer_consumer_workers`, `datahub_metrics_workers`, `datahub_schema_*_workers`, `datahub_query_engine_workers` — verify no remaining secret-param sites. |
| 1i-cleanup S3 | #275 | ✅ | **All three sub-steps shipped 2026-06-30.**  **S3a (commit `b1a2d9ce` + followup `864f4104`):** deleted `ShmConfig::secret` + the `<dir>_shm_secret` JSON parse, `ChannelAccessEntry::shm_secret`, dropped `shm_secret` param from `_on_channel_access_opened`, retired 2 L2 `HubStateChannelAccess` tests via Rule-6 doc-blocks, fixed README_Deployment.md broken examples + HEP-CORE-0041/0033/0007/0035 stale references.  **S3b (commit `7026f0cc`):** masked `test_datahub_hub_queue.cpp` + `workers/datahub_hub_queue_workers.cpp` from L3 CMakeLists with top-of-file Rule-6 doc-blocks; TESTING_TODO retirement row captures the 25 TEST_F's multi-destination handoff to L2 capability + L3 fd-source workers + L4 #258 e2e.  **S3c (commit `<this commit>`):** deleted `ShmQueue::create_writer(name,..secret..)` + `create_reader(name,secret,..)` (~150 LOC body), `set_shm_secret` (~25 LOC), the legacy branch in `start()` (~55 LOC), `ShmQueueImpl::{pending_shm_secret, has_shm_secret}` fields, mutual-exclusion guards in `set_shm_capability_fd`, the `shm_secret` artifact read in `apply_master_approval` (now no-op true), stop() reset of pending_shm_secret; retired 2 mutual-exclusion tests in `test_hub_shm_queue_capability.cpp` via Rule-6 doc-block; refreshed header docstrings throughout `hub_shm_queue.hpp`. |
| 1i-cleanup S4 | #275 | ✅ | **Shipped 2026-06-30 (commit `<this commit>`).**  Deleted `shared_secret` parameter from `find_datablock_consumer_impl` + `attach_datablock_as_writer_impl` + public template `find_datablock_consumer<F,D>`; deleted the memcmp secret-validation gates in both impl bodies; deleted the `pylabhub::crypto::generate_random_bytes` + `if (config.shared_secret != 0) memcpy` block in `DataBlock` ctor — header field stays (renamed in S5).  Updated callers: `data_block_recovery.cpp` recovery API + 2 sites in `datahub_stress_raii_workers.cpp`.  Verified: stage_all + ctest -L layer3 (373/373) + full ctest (2261/2261), exit 0. |
| 1i-cleanup S5 | #275 | ✅ | **Shipped 2026-06-30 (commit `<this commit>`).**  Core Structure Change Protocol walked at `docs/code_review/REVIEW_S5_CoreStructure_2026-06-27.md` — all 9 impact-matrix items verified.  Renamed field in `SharedMemoryHeader` (data_block.hpp:214) + `PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS` macro (data_block.hpp:345); updated the sole consumer in `data_block_recovery.cpp`; refreshed HEP-CORE-0002 §"Security and Schema" layout diagram + sentinel-semantics paragraph + HEP-CORE-0041 S5 status row; ran full ctest -j 2 (2261/2261 green).  Layout preserved (`static_assert(sizeof(SharedMemoryHeader) == 4096)` passes).  Schema hash bumps via macro (field name participates in hash input) — intentional per protocol §"Producer Registration". |
| 1j | #257 | ✅ | **Resolved 2026-07-16 — scope refined during design discussion.**  The "three L3 broker tests" framing was wrong.  **success/denied** are validated by the REAL framework end-to-end at L4 (`test_plh_hub_role_shm_e2e.cpp` — `ShmE2E_AuthorizedConsumerReceivesAllSlots` / `ShmE2E_UnauthorizedConsumerDeniedByBroker`; role host frame dispatches `ShmAttachOrchestrator` internally on producer REG_ACK).  **divergence-WARN** is a transient-race SPECIAL CASE the real framework cannot produce deterministically (window between broker ledger mutation and producer `CHANNEL_AUTH_CHANGED_NOTIFY` cache refresh) → correctly pinned at L2 by *surgical value-injection* of `cache_lookup`/`broker_query` (`test_shm_attach_orchestrator.cpp` — all 4 D4 cells + 4 fail-closed, real orchestrator/acceptor/transport/consumer).  Legitimate mock per README §1.2 rule 2 (value, not code path); L2 per rule 7.  Deliverable = heavy RATIONALE block (contract maintenance-coupling) + stale `#291` DISABLED-comment cleanup in the L4 e2e.  A hand-rolled L3 `BrokerWireClient` divergence test was explicitly rejected — it validates neither framework nor real coordination. |
| **★ REVIEW-C** | **#276** | **✅** | **CLOSED 2026-07-16** — `docs/code_review/REVIEW_HEP0041_ReviewC_2026-07-16.md`.  #275 deletion confirmed complete (zero live residue).  5 carried items re-derived from current code: MemfdConsumer RAII + §6.4 factory = non-issues; §11/§10.1 stale rows fixed; SCM_RIGHTS `MSG_CTRUNC` fail-closed hardening added; D3 retry control-flow gap closed with 2 L2 tests (`ConnectToUnboundEndpoint_ReturnsNulloptNotThrow` + `EmptyEndpoint_ThrowsNotNullopt`).  Full ctest 2553/2553.  → REVIEW-D unblocked. |
| Mutual auth | #262 | ✅ | **Closed 2026-07-16.** 3-frame handshake shipped; default flipped `shm_require_mutual_auth` false→true (HEP-0044 §8.4).  No version-axis bump — the Frame 2/3 exchange is a producer↔consumer AttachProtocol handshake, not a broker control-plane message; interop is the config knob.  Light attack coverage = L2 `MutualAuth_RejectsWrongProducerPubkey` (an L4 squatter was scrapped — baseline Frame 2 already `box_encrypt`s to `producer_pk`, binding the producer key before Frame 3). |
| **★ REVIEW-D** | **#277** | **✅** | **CLOSED 2026-07-17** — `docs/code_review/REVIEW_AUTH_ReviewD_2026-07-17.md`.  Surfaced + removed the broken consumer PID-liveness sweep (`a00a4188`; heartbeat now sole liveness, revoke on reclaim → `phase="left"`).  Allowlist+revocation cycle pinned end-to-end at L3 with real components (`ConsumerAttach_DeniedAfterDereg` #2369 = admit→revoke→deny; `GetChannelAuth_ReturnsAllowlist` #2365; `ConsumerHeartbeatTimeout_NotifyBodyShape` #2396) + L2 gate (`Swap_BlocksOldPeer_PinsData`) + ledger unit (#2480 clamp).  Test-pins-reality audit = `COVERAGE_AUDIT_Broker_Queue_CURVE_2026-07-17.md` (2 security-test gaps fixed).  Passive-revocation contract confirmed correct in code — no impersonation risk.  L4 data-plane cycle (`MixedAdmitDeny`) explicitly deferred (logic covered at L3; data-plane timing-fragile).  Full ctest 2555/2555.  → REVIEW-E unblocked. |
| **★ REVIEW-E** | **#278** | **✅** | **CLOSED 2026-07-17 — 🟢 PHASE 1 PRODUCTION-READY** — `docs/code_review/REVIEW_AUTH_ReviewE_2026-07-17.md`.  8-threat model fact-checked against live code (auth, authz, impersonation, replay, envelope-tamper, revocation, downgrade, data confidentiality) — no Cat-1 gap in the single-hub CURVE chain.  Mutual auth closes the impersonation window.  Replay confirmed LIVE (not islanded — stale HEP-0046 note corrected) + hardened (window≥skew) + E2E-tested (`546bc115`).  Out of Phase 1 scope + tracked: admin plane (Line E, plaintext), inbox (#191), federation (#105).  Full ctest 2557/2557. |

**Historical note (RESOLVED):** on 2026-06-27 S3 was *not* yet shipped
(`set_shm_secret()` still present).  It shipped 2026-06-30 (row 1i-cleanup
S3 above).  Re-verified against code 2026-07-18: `set_shm_secret` and the
`shared_secret` `create_writer`/`create_reader` overloads are **deleted**
from `hub_shm_queue.cpp` — S3/S4/S5 are DONE; only **#275 S2** (16-worker
scan) remains.

**#285 ↔ #275 S4+S5 dependency (verified 2026-06-27):** SOFT.  S3+S4+S5
operate on the C API + SharedMemoryHeader; #285 Pattern 4 reform is
test STRUCTURE.  S2 (16 L3 worker migration) is the actual S4
prerequisite, not Pattern 4 reform.

### #275 1i-cleanup migration shape

Per the #273 retirement precedent + the doc-blocks at
`role_api_flexzone_workers.cpp:755-853`:

1. Rewrite `make_producer_opts` / `make_consumer_opts` /
   `build_payload_pair` to drive the capability path (the
   `data_transport=="shm" && shm_shared_secret==0` branch at
   `role_api_base.cpp:537-589`).  Producer-side: helper creates a
   memfd as a DataBlock writer (use `MemfdShmCapability::create_*`
   primitives directly, not the full handshake — in-process so no
   SCM_RIGHTS step).  Consumer-side: helper pre-populates
   `RxQueueOptions::shm_capability_fd` with a dup of that fd.
2. Drop the `secret` parameter from helper signatures (and from all
   call sites).
3. The migrated tests exercise the surviving capability path with
   the same per-field assertions; checksum-verify and roundtrip
   semantics unchanged.
4. Retain the L2 capability fast-path (`set_shm_capability_fd` +
   `start`) as production-mirror surface — covered already by
   `tests/test_utils_security/test_hub_shm_queue_capability.cpp`.

Estimated scope: ~80 LOC helper rewrite + 7 test sites that each
lose the `secret` arg.  Lands inside #275's commit — deletion + test
migration MUST move together so no commit leaves the suite broken.

### HEP-0041 Phases 2-5 (post-Phase-1)

| Phase | Tracker | Scope |
|---|---|---|
| 2 — macOS backend | to file once Phase 1 ships | `shm_open(SHM_ANON)` replaces `memfd_create`; `SCM_RIGHTS` over Unix sockets identical to Linux |
| 3 — Windows backend | to file once Phase 1 ships | Named pipe replaces Unix socket; `DuplicateHandle` replaces `SCM_RIGHTS`; producer queries broker for `consumer_process_id` |
| 4 — Framework crypto primitives | #247 (script-side); native part of Phase 4 | `PYLABHUB_UTILS_EXPORT` AEAD (ChaCha20-Poly1305) + HKDF wrappers; sibling `api.crypto.*` bindings (Python/Lua/Native) |
| 5 — HEP-0036 ZMQ retrofit | #246 (design ✅ shipped 2026-07-01 as HEP-CORE-0042; impl phases 2+ pending) | Retrofit ZMQ to pre-attach broker-confirmation pattern; producer's ZAP handler stops being load-bearing; cache becomes observability for ZMQ too |

**AttachProtocol primitive extraction — SHIPPED 2026-07-07 (SHM
refactor).**

SHM AttachProtocol internals were refactored to sit behind a
transport-agnostic seam so both consumer-attach (this sub-chain)
and broker-observer (Line 3 / HEP-CORE-0045) compose over the same
Frame 1/2/3 crypto helpers.  **Not a new capability; an internal
factoring.**  Authoritative design: **HEP-CORE-0044 (AttachProtocol
primitive)**, promoted 2026-07-08.

| Piece | Status | Where |
|---|---|---|
| `IAttachChannel` interface + `ShmAttachChannel` | ✅ SHIPPED 2026-07-07 | `attach_channel.hpp` + `attach_channel_shm.hpp/cpp` — HEP-0044 §6 |
| `run_producer_handshake` / `run_consumer_handshake` | ✅ SHIPPED 2026-07-07 | `attach_protocol.hpp/cpp` — HEP-0044 §7 |
| Name-based key citation (`own_seckey_name` replaces `SeckeyAccessor` callback) | ✅ SHIPPED 2026-07-07 | `AttachProtocolAcceptor` ctor + `ConsumerAuthMaterial` — HEP-0043 §1.4 + §6, HEP-0044 §4.1 |
| Frame 3 mutual auth (#262) preserved through refactor | ✅ SHIPPED | HEP-0044 §8 |
| `role_type="observer"` extension preserved | ✅ SHIPPED | HEP-0044 §9 |

**Speculative ZMQ AttachProtocol code REVERTED 2026-07-08.**  A
previous session (`5a24b410`) shipped `ZmqAttachChannel`,
`ZmqAttachProtocolAcceptor`, `initiate_zmq_consumer_handshake`,
plus two L2 test binaries — as speculative foundation for a
hypothetical belt-and-braces layer on top of CURVE.  On review,
none of the three lines of work (main auth chain / SMS
consolidation / broker observer) needs it: HEP-0036 makes CURVE +
ZAP + broker allowlist the whole ZMQ auth story; HEP-0044 §10
formalizes why AttachProtocol is not deployed on ZMQ.  Code and
tests deleted; TODO row cleaned up.

**Sequencing rationale.**  Phase 1 establishes the pre-confirm
contract on Linux/FreeBSD where the threat model is sharpest;
Phase 5 (#246) lands AFTER Phase 1 so ZMQ retrofit replicates a
proven pattern rather than co-evolving.

**#246 Phase 1 design landed 2026-07-01 as HEP-CORE-0042.**  The
transport-agnostic Channel Attach Coordination Protocol replaces the
"HEP-0036 amendment" framing.  Both SHM and ZMQ pre-attach coordinate
via the same protocol (HEP-0042 §5 abstract, §6 per-transport
bindings).  HEP-0041 §5.4 SHM CONSUMER_ATTACH_REQ_SHM relocated to
HEP-0042 §6.1.  Impl phases 2-4 (L2 broker unit tests → L3 role-broker
integration → L4 e2e) are the next work chain for #246.  Test-fixture
follow-on (broker-side helper to synthesize APPLIED_REQ with arbitrary
`instance_id` for the stale-instance guard test) tracked as a
follow-on task filed at HEP-0042 §11.

**#245 KILLED 2026-06-17.**  POSIX `kShmModeRw 0666 → 0600` interim
hardening rejected — HEP-0041 deletes the named-SHM `shm_open` path
entirely.  No mode bits to harden.

---

## Cross-cutting future change (#282 / #283)

`#272`'s self-review surfaced that `apply_consumer_reg_ack_shm_`
blocks the consumer-role-host worker thread for up to ~3.9s during
the dial (#282).  The blocked thread is the worker, not the BRC poll
thread.  Bigger than #272 alone — same pattern recurs in producer L2c
broker pre-confirm, future #262 mutual auth Frame 3, future
HUB_TARGETED_REQ (#75), and future api.crypto.* (#247).

**Generalized solution drafted under #283:** HEP-CORE-0031 amendment
adding a `spawn_bounded` primitive (single sweeper thread +
step-function body contract).  Tech draft at
`docs/tech_draft/DRAFT_HEP-0031-bounded-thread_2026-06.md`.

**Migration plan (#283 §8) lands in two passes:**

- **Pass 1** — convert affected sites to bounded-sync (no FSM
  change).  Migrates work onto framework-managed threads with uniform
  observability + clean teardown integration; worker-thread block
  PERSISTS in this pass.
- **Pass 2** — convert to truly-async with FSM amendment (insert
  `RegistrationState::RegAckPending` between `Registered` and
  `Authorized`; teach §8.2 outer guard to wait for `Authorized`).
  This actually lifts the worker-thread block.

Pass 2 for #272 (consumer dial) sequences under REVIEW-B (#274) or
as part of #262.  Pass 2 for #262 (mutual auth) designs async from
day one once #283 ships.

---

## Design audit 2026-06-10 — open items only

Closed items (A1/A2/A3 trade-offs; F1/F2 federation) archived in §9.
Open items below are anchored to active AUTH-N entries.

### Gaps in the framework surface (close before AUTH-7)

- **G1 — `api.allowed_peers(channel)` polling accessor.**  ✅
  shipped 2026-06-10 in Lua + Python; Native deferred per #84 MVP.
- **G2 — `api.producers(channel)` polling accessor.**  ✅ shipped
  2026-06-15 in Lua + Python; Native deferred per #84 MVP.
- **G3 — Internal-only `NotificationId` pattern.**  ✅ shipped
  2026-06-10 — `cycle_ops.hpp` documents the "callback_name == nullptr
  ⇒ internal" rule at the dispatch loop.

### Race conditions / failure modes pinned

- **R1 — Queue overflow.**  `kMaxIncomingQueue=64`.  Verified BENIGN
  via `enqueue_message` drop-on-full + WARN log at
  `role_host_core.cpp:23`.  Operators can grep.  No counter needed.
- **R2 — Worker stuck → queue fills → notify dropped.**  Direct
  consequence of R1.  Operational mitigation: bounded script callback
  time.  Documented under AUTH-5 sibling sweep.
- **R3 — GET pull timeout.**  ✅ enforced — `BrokerRequestComm::get_channel_auth`
  takes 5000ms default like `register_channel`.
- **R4 — GET returns `CHANNEL_NOT_FOUND` / `PRODUCER_NOT_AUTHORIZED`.**
  ✅ handled — handler logs + continues; L3 pin
  `GetChannelAuthRejectsNonProducer` covers the non-producer side.
- **R5 — `set_peer_allowlist` exception safety.**  ✅ wrapped in
  try/catch at the dispatcher's native-handler call site
  (`handle_channel_auth_notifies`).

### Test gaps (open)

- **T1 — L4 demos for §I11 examples.**  Folded into AUTH-7 scope.
- **T2 — Queue-overflow stress test.**  Folded into AUTH-6 scope.
- **T3 — Pull failure paths (CHANNEL_NOT_FOUND / PRODUCER_NOT_AUTHORIZED
  / timeout).**  Folded into AUTH-6 scope.
- **T4 — libzmq CURVE auto-retry timing bound.**  Folded into AUTH-6
  scope.

### Honest assessment (carry-forward 2026-06-10)

The contract is sound for the threat model HEP-0036 specifies
(operator-trusted roles + manual key distribution + accepted
revocation-race window).  Original audit found 5 medium/high concrete
issues (G1, G2, R1, R3, R4) that ALL closed by 2026-06-15.  No
remaining design gaps that re-open §I11 / §6.5.

If a future request looks like it wants to revisit "should worker
prioritise auth notifies" / "should pull failure be critical" /
"should the queue block" — re-read §I11 + §6.5 first; the audit
considered these.

---

## Backlog — open items NOT on the AUTH-1..7 critical path

### Test infrastructure / coverage gaps

- **Allow-path L3 pin for D2.**
  `DatahubBrokerHealthTest.CtrlZapDenyPath` pins the deny path.
  Symmetric allow-path L3 needs a BRC client whose `client_pubkey`
  is added to the broker's `known_roles[]` before connect; requires
  the test infrastructure to thread explicit CURVE keys into the
  worker's broker config.  Smallest fix: extend the
  `BrokerService::Config` test-side construction to include a
  pre-generated `known_roles[]` entry built from the test client's
  keypair.  Effort: S.

### Operator workflow gaps

- **`plh_role --keygen` does not publish `<vault>.pub`.**
  HEP-CORE-0035 §4.8.3 specifies `plh_hub --add-known-role <role.pub>`
  as the canonical operator workflow; requires the role binary to
  publish a sibling `.pub` file alongside the vault (the way
  `plh_hub --keygen` publishes `hub.pubkey`).  Currently the L4
  RoundTrip test opens the role vault programmatically to extract the
  pubkey — a test backdoor.  Mirror `HubVault::publish_public_key`
  for `RoleVault` (atomic O_EXCL + O_NOFOLLOW + mode 0644 + symlink
  defense per HEP-CORE-0035 §4.6.4).  Effort: M.  **Closes alongside
  Phase 3 CLI work (#155).**

- **Hot-reload of `known_roles.json` on a running hub** (HEP-CORE-0035
  §4.8.5).  `BrokerCtrlAdmission::set_peer_allowlist` exists with no
  caller; the admin RPC (`/admin/reload-known-roles` or similar) is
  the missing wiring.  Operators that run `--add-known-role` against
  a running hub today must restart it to pick up the change.
  Effort: M.

### Phase E/F/G/H staging

- **Phase E — Admin loopback enforcement.**  AdminService refuses
  non-loopback bind for v1; CURVE-wrap is HEP-CORE-0035 §5 future
  work.  Unblocked once AUTH-1..7 ship.
- **Phase F — Federation peer ZAP parity.**  Depends on Phase E +
  Federation HEP (task #105).
- **Phase G — SHM auth migration.**  HEP-CORE-0041 is the foundation;
  Phase G is the broader migration to capability transport (existing
  SHM consumers gain capability-fd path).
- **Phase H — Demo migration.**  24 role configs across 11 demo dirs
  ship `"auth": { "keyfile": "" }` (pre-dates Phase C, broken since
  early May 2026; see 2026-06-09 archive).  Belongs alongside #155
  (CLI --init bundling) as coordinated refresh wave.

---

## Deferred decisions (each tied to its phase)

| # | Decision | Affects | Tentative direction |
|---|---|---|---|
| P-InboxQueue | InboxQueue admission policy location | Phase E | **RE-DECIDED 2026-07-17 (HUB-WIDE, supersedes the 2026-06-10 channel-scoped answer):** the inbox is a hub-wide role<->role facility, so it admits any authenticated `known_role`, NOT just the parent channel's allowlist.  Broker distributes the roster on `REG_ACK`/`CONSUMER_REG_ACK.known_roles` (committed `90ec48a3`); the role unions it into a hub-wide inbox roster.  Single-key model — inbox ROUTER/DEALER reuse the role identity keypair (HEP-0036 §I6).  See HEP-CORE-0027 §3.5.  Task **#191**.  **Slice progress:** (0) broker roster distribution ✅ `90ec48a3`; L4 plaintext delivery + two schema-shape bug fixes ✅ `563cf6f4`; (1) role captures `known_roles` into the hub-wide inbox roster ✅ `26d5fcb3`; (2) InboxQueue ROUTER implements PeerAdmission + arms curve_server (`<uid>:inbox` domain, deny-all until seeded) and InboxClient DEALER arms curve_serverkey; role seeds the ROUTER ZAP from the roster in `merge_inbox_known_roles` ✅; (3) `ROLE_INFO_ACK` carries receiver identity pubkey, `open_inbox_client` pins it as curve_serverkey (hard-refuse plaintext) ✅; (4) L3 CURVE tests (`InboxQueueTest.CurveAuthorizedDelivers` / `CurveUnknownSenderDenied`) + L4 `ZmqE2E_InboxDelivery` over CURVE with `event=InboxAllowlistSeeded` ✅.  **DONE — inbox is CURVE-authenticated end-to-end, hub-wide known_roles.** |
| P-Admin | AdminService — CURVE-wrap or loopback-only? | Phase E | Hard loopback-only for v1; CURVE-wrap is HEP-CORE-0035 §5 future work |
| P-SHM-Identity | What is a PeerIdentity for SHM? | HEP-0041 Phase 1 | Capability path: peer identity = consumer pubkey verified during pre-attach `CONSUMER_ATTACH_REQ_SHM`.  Superseded the original "broker-issued `shm_secret`" answer. |
| P-Demos | How existing demos migrate | Phase H | Transitional `--allow-anonymous-data` flag, gated to refuse-bind on non-loopback endpoints; demos updated incrementally |
| P-HEP | When to sync HEPs vs hold tech_draft | Close-out | Tech_drafts active for in-progress work; promote to HEP at landing.  See `docs/tech_draft/README.md`. |

---

## Parallel / adjacent tracks

These have their own task IDs but touch the same surface — do NOT
sequence them into the AUTH-1..7 chain.

- **#74** — HEP-CORE-0035 auth implementation umbrella tracker.
- **#102** — HEP-CORE-0035 §4.7 runtime key handling — SUPERSEDED
  2026-06-05 by HEP-CORE-0040 chain (#165–#176 all shipped 2026-06-09).
- **#103** — HEP-CORE-0017 §3.3 + HEP-CORE-0036 implementation
  (AUTH-1 carrier; ✅ shipped).
- **#104** — Sibling HEP updates per HEP-CORE-0036 §14 (AUTH-5).
- **#105** — Federation protocol design — explicitly post-MVP per
  HEP-CORE-0036 §13.1.
- **#106** — HEP-CORE-0038 + impl script-accessible vault keystore
  — depends on #104 + HEP-0040 storage (now shipped).
- **#120** — Windows pathway hardening for HEP-CORE-0035 §4.6 floor
  — blocked by missing Windows CI.
- **#152** — Delete legacy `RoleIdentityPolicy` (hygiene; independent).
- **#154** — Re-create L3 broker tests (AUTH-6).
- **#162** — HB-2 wiring (AUTH-2; ✅ shipped).
- **#163** — HB-4+5 wiring (AUTH-3; ✅ shipped).
- **#164** — HB-6 broker-side shm_secret (SUPERSEDED by HEP-0041).
- **#79**  — `plh_role --init` SHM secret (SUPERSEDED by HEP-0041).
- **#258** — HEP-0041 1k L4 e2e SHM (✅ shipped 2026-06-26).
- **#275** — HEP-0041 1i-cleanup: S3/S4/S5 ✅ shipped; only **S2** (16-worker scan) open.
- **#262** — HEP-0041 mutual auth (3rd handshake frame).
- **#291** — SHM consumer-attach handshake failure (✅ closed via
  KeyStore raw 32-byte seckey representation; HEP-0040 §8.5.2
  NORMATIVE).
- **#286/287/288/289/290** — HEP-0036 §5b canonical wire schema
  unification (✅ all shipped).

### Items NOT on the critical path

- **#75** HUB_TARGETED_ACK — scope ambiguous; needs design first.
- **#76** Script reload — independent feature; tech_draft exists.
- **#77** Tier 2 dynamic callbacks — independent feature.
- **#94** HEP-0021 §16.5 ephemeral binding — paired with AUTH-1 per
  HEP-0036 §14.1 wire-shape coupling but production-caller wiring is
  about multi-hub processor, not auth gating.
- **#155** CLI Phase 3 (`--init` one-shot bundling) — CLI UX,
  auth-adjacent but not auth-gating.  This is Phase 3 of the locked
  execution plan in `docs/TODO_MASTER.md`.

---

## Decision log — key calls captured for audit trail

Pre-2026-06-27 entries preserved verbatim in archive §10 (see
`AUTH_TODO_completions.md`).  New entries below.

- **2026-06-05 — Strict-mode cleanup must precede A3** (now AUTH-1).
  C1..C5 sequenced before AUTH-1 to surface dependents as
  compile/link/run errors instead of silent miscompiles.  Chain closed
  2026-06-09.
- **2026-06-05 PM — Separate STORAGE from API.**  HEP-CORE-0040
  KeyStore + LockedKey owns storage (one owner per process); API
  exposes OPERATIONS (`with_seckey(name, cb)`), not byte exports.
- **2026-06-06 — Use-not-export discipline (round-5).**  RoleAPIBase
  + HubAPI lose seckey accessor entirely; no legitimate caller
  exists.  HEP-CORE-0040 §8.2.
- **2026-06-09 — Queue-level gate, NOT transport-level.**  Broker
  auth gate sits at QUEUE level (before data channel construction),
  not at SHM/ZMQ transport level (CURVE handshake / shm_secret
  memcmp).
- **2026-06-09 — Mechanism enum collapsed from 3 to 2 states.**
  `Plaintext` was structurally unreachable after C1.  Collapsed to
  `Uninitialized` / `Curve`.  Later widened in HEP-0041 to add
  `ShmCapability` per #279.
- **2026-06-09 — Demo refresh deferred.**  Phase C did NOT introduce
  new demo breakage; the `auth.keyfile = ""` breakage pre-dates Phase
  C (#78 / B3 in early May 2026).
- **2026-06-16 — HEP-0041 supersedes AUTH-4.**  Capability transport
  via `memfd_create` + `SCM_RIGHTS` replaces the `shm_secret` model.
  Tasks #164 + #79 closed as SUPERSEDED.
- **2026-06-26 — KeyStore seckey representation contract** (HEP-0040
  §8.5.2 NORMATIVE).  Raw 32 bytes at security-module boundary; Z85
  only at file/wire/display.  Closed #291; commit `e0f16a44`.
- **2026-06-27 — AUTH_TODO compression.**  1616 → ~500 lines;
  completed-phase narratives extracted to
  `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md`.
  Verified line 1137 mis-claim ("S3 ✅ shipped") was false against
  code.

---

## Memory rules adopted during the auth track

These survive in the user-level MEMORY.md; included here for audit
trail.

- **Audit stale silent-fallback patterns whenever a contract changes.**
  `if (X.empty()) return /* skip security */` is a contract-violation
  candidate, not a clean default.  Source: 2026-06-05 audit.
- **Separate STORAGE design from API design.**  When asked "where
  should X live", split into (a) where the bytes live (lowest
  reasonable level, single owner) and (b) what API exposes access
  (grouped logically per consumer).  Source: 2026-06-05 PM REFRAME.
- **Refresh against persistent docs at the moment work begins.**  Do
  not re-derive plans from HEPs when AUTH_TODO already has the agreed
  plan.  Source: 2026-06-09 user correction.
- **No flake explanations.**  Different tests failing across runs is
  a RACE SIGNATURE, not a flake signature.  "Passes when I re-run it"
  is not investigation, it's re-sampling.  Source: 2026-06-13
  AUTH-2 concurrent-pumper PANIC.
- **Read log BEFORE rerun.**  Re-running first overwrites the failure
  log you needed.  Companion to the no-flake-explanations rule.
  Source: 2026-06-25 RunMode_LogShowsCorrectStartupAndShutdownOrdering
  failure.
- **Seckey representation at module boundary is NORMATIVE.**  Raw
  bytes inside the security module; encoded only at file/wire/display.
  HEP-CORE-0040 §8.5.2.  Source: 2026-06-26 #291 root cause.

---

## How to retrieve archived content

```bash
# Full pre-compression AUTH_TODO (verbatim, all 1616 lines):
git show dfe86a61:docs/todo/AUTH_TODO.md | less

# Specific narrative section (AUTH-1 sub-deliverables, lines 139-647):
git show dfe86a61:docs/todo/AUTH_TODO.md | sed -n '139,647p'
```

See `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md`
for the index of what was extracted and where each section lives.
