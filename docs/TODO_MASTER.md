# Data Exchange Hub — Master TODO

**Scope:** strategic execution plan, current sprint focus, pointers
to subtopic detail.  Per `docs/DOC_STRUCTURE.md` §1.1 + §2.1.1:
**keep this concise (≤ 200 lines)** — detailed task tracking lives
in `docs/todo/<area>_TODO.md`.

For maintenance discipline (periodic quality checks, when to archive)
see `docs/DOC_STRUCTURE.md` §2.1.1.

---

## Resume point (2026-07-08)

The whole security-and-communication work chain sorts into **three
independent lines** that share only the SMS crypto primitives.  Keep
them separate when planning; do not mix scope.

### ⭐ Topology migration — Phases A + B + C COMPLETE (2026-07-09)

**Singular-side ownership migration.**  Every channel has exactly
one data-plane endpoint, owned by the singular side of its topology.
Three topologies (fan-in / fan-out / one-to-one) declared per
channel; cardinality + transport-compatibility gates enforced at
broker admission.

**Design status:** DESIGN LOCKED (tech draft rev 9 + rev 10 pending
for Q3a revision).

**Phases done:**
- ✅ **Phase A** — 10 coordinated HEP amendments (commits
  `007b749d..e6a80070`).
- ✅ **Phase B** — 10 code slices, atomic completion
  (`2c960cca`); rev 1 correctness bugs + rev 2 groups A/B/C.
- ✅ **Phase C step 1** — topology-parametric factory API +
  PUSH/PULL dispatch (`50ceb5b6`).
- ✅ **Phase C step 2** — PUB/SUB support, fan-out ZMQ live
  (`58c1a321`).
- ✅ **Phase C step 2 rev 2 A+B** — drift + defensive polish +
  PUB/SUB test coverage (`60fe0921`).
- ✅ **Phase C step 2 rev 2.3** — B1 fix (fan-in producer PUSH-
  connect CURVE wire) + unified peer-list wire shape
  (`b71dd9ec`).  Full ctest 2362/2362.

**Phase D — split-completed:**
- ✅ **D phase field** (`8655f2fe..ed0456d5`) —
  `CHANNEL_AUTH_CHANGED_NOTIFY` payload extended per HEP-CORE-0007
  §CHANNEL_AUTH_CHANGED_NOTIFY (lines 1803-1864); binding-side
  live_peers map + `consumer_count`/`producer_count` accessors +
  bindings landed across native/lua/python.  Existing plumbing
  extended, not duplicated (fire_channel_auth_changed_notify,
  handle_channel_auth_notifies, RolePresence.first_heartbeat_seen,
  ConsumerEntry.zmq_identity, allowed_peers binding pattern).  Full
  ctest 2362/2362.

**Active next:**
- ⏳ **D R6 gate symmetrization** (tech draft §5.4) — dialing-side
  REG_REQ pends on binding-side Live.  Mechanism now available
  (phase=live NOTIFY + live_peers map from the phase-field work).

**Ahead:** D R6 → E (retirements) → F (demos + L4 flip) → G
(fan-out ZMQ role-host integration; L4 slow-joiner test using
`api.consumer_count()` gate lands here) → H (verification).

**Consequences of migration:** HEP-CORE-0017 §3.3 multi-endpoint
PULL code (`2c604280`) retires in Phase E.  HEP-CORE-0042 §5 + §7.1
pre-attach coordination retires in Phase E.  HEP-CORE-0021 §16
reparametrized in Phase A step 6 (`adc448fe`).  Q3a REVISED to
OPTIONAL default `one-to-one` in rev 3.1.

**Fan-in binding-side reader correctness arc ✅ SHIPPED (2026-07-11).**
Five related fixes closing the correctness gaps under the
topology migration (`allowlist_cache` seed, binding-queue
resolution, loop-ready gate, broker authorization mirrors,
dial-side readiness pull).  Full detail in
`docs/todo/API_TODO.md` + `docs/todo/MESSAGEHUB_TODO.md` +
`docs/todo/TOPOLOGY_TODO.md` + `docs/todo/TESTING_TODO.md`.
L4 fan-in E2E passes end-to-end (was failing at 20 s timeout);
+1 new L2 test pinning the AND-composition invariant.  Design
authority: HEP-CORE-0011 §"Loop-ready gate" + HEP-CORE-0036
§4.3.4 / §6.5 step 6 / §6.6.1 / §6.6.2 / §6.6.3 + HEP-CORE-0042
§5.5.2 amendment.

- **Design authority:** `docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`
- **Migration plan + finding detail:** `docs/todo/TOPOLOGY_TODO.md`

**REG/REG_ACK Protocol Redesign — HEP-CORE-0046 promoted (2026-07-12).**
Design tech draft (`DRAFT_reg_ack_protocol_redesign.md`, DESIGN
LOCKED, 21 invariants, typed wire envelope §14, admission-gate
pipeline §14.5, phase sequencing §12) promoted to normative
HEP-CORE-0046.  Cross-references added to HEP-CORE-0007, -0017,
-0021, -0023, -0033, -0035, -0036, -0042.  Wire discipline rule
added to `docs/IMPLEMENTATION_GUIDANCE.md`.
- Phase A (typed envelope + body classes) — SHIPPED (46 L1 tests).
- Phase C islanded modules (`admission_gates`,
  `reg_admission_pipeline`, `broker_reg_handler`,
  `HubState::nonce_seen`) — SHIPPED as compile-verified
  L1/L2-tested modules (23+5+14+6 tests).
- Phase B (broker dispatch rewire) — **task #57, independent /
  non-blocking**.  CORRECTED 2026-07-16: the envelope + typed bodies +
  admission gates + BRC are already LIVE (broker_proto 7); only the typed
  COMMIT (BrokerRegHandler) is unwired — a SKELETON (~15% producer, 0%
  consumer).  The live REG path stays the handcrafted handle_reg_req /
  handle_consumer_reg_req (complete + tested).  Phase B is a relocate-into-
  typed-form refactor (tech-debt under the wire-discipline rule), NOT a
  security/functional gap — full parity list on task #57.  Deliberately
  parked so we pursue the bigger picture (AUTH chain / topology) on the
  working JSON layer.
- Phases D (retirements), E (integration tests), F (federation
  follow-on) — PENDING per §12 sequencing (after B).

Design authority: `docs/HEP/HEP-CORE-0046-REG-Protocol-Redesign.md`.

**Queue-owned topology + layer cleanup arc — CLOSED 2026-07-12.**
Follow-on to the fan-in binding-side reader arc.  Reclaimed the layer:
`hub::Queue` owns topology and transport; role host uses uniform,
topology-agnostic `api.finalize_channel_connect(channel, timeout_ms,
is_cancelled)`.  Commits `c665de0c` (P1–P5) + `db2bbc21` (P7 HEP
sweep + trailing §I9.1 code fixes).  Draft archived to
`docs/archive/transient-2026-07-12/DRAFT_queue_owned_topology_and_layer_cleanup_2026-07-11.md`;
lasting design in the amended HEPs (HEP-CORE-0011 §"Loop-ready gate"
+ HEP-CORE-0036 §I9.1 / §6.5 step 6 / §6.6.3 + HEP-CORE-0042 §5.5.2).

**Landed:**
- P1 (HEP contract) — HEP-CORE-0036 §I9.1 NEW locality invariant +
  §6.5 step 6 + §6.6.3 amendment sub-blocks; HEP-CORE-0011
  Loop-ready gate consumer default reroute.
- P2 (correctness bugs) — C3 stale confirmed-allowlist fix in
  `HubState::_on_consumer_revoked`; C1 shutdown cancel; C2 error
  code split; C4 verified.  Six new L2 tests.
- P3 (layer surgery) — `hub::PeerReadinessOracle` + `QueueWriter::
  finalize_connect` + `QueueWriter::own_pubkey_z85` added; `dial_now`
  retired.  `RoleAPIBase::dial_now` + `check_peer_ready` public
  surface REMOVED; `RoleAPIBase::finalize_channel_connect` new.
  Producer / consumer / processor hosts call it uniformly.
  `wait_for_peer_ready` helper deleted.  `kLoopReadyPollInterval`
  split into `kLoopReadyGateInterval` + `kBrokerReadinessPollInterval`.
  Full L2 1657/1657 pass; L4 133/133 pass; L4 fan-in E2E 3.1 s.

**Deferred follow-ups** (tracked; not required for arc close):
- P6 — Replace `binding_side_confirmed_allowlist` full-set copy
  with version-tagged membership.  Detail in
  `docs/todo/MESSAGEHUB_TODO.md`.
- D-3 — clang-tidy / clang-query rule that fails the build on
  §I9.1 layer regressions in role-side code.  Detail in
  `docs/todo/API_TODO.md`.

**Legacy Line 1 remaining items** (#246 Phase 3a L4 close-out,
#275 S2, #257, REVIEW-C) are largely subsumed by the migration;
Phase E retirement wave collapses the pre-attach coordination they
depended on.

### Line 1 — Main auth chain (CURVE end-to-end across ZMQ + SHM)

Symmetric CURVE integration across the ZMQ and SHM data paths so
every role/broker connection is gated by authentication.  ZMQ uses
libzmq CURVE + ZAP at the socket layer.  SHM uses `AttachProtocol`
at the application layer (HEP-CORE-0044, promoted 2026-07-08).

**Authoritative HEPs:** HEP-0035 (Hub-Role Auth), HEP-0036 (ZMQ
CURVE), HEP-0041 (SHM channel auth), HEP-0042 (attach
coordination), HEP-0044 (AttachProtocol primitive).

**Shipped:** Phase D1/D2/D3 (HEP-0035); AUTH-1/2/3 (ZMQ role side);
HEP-0041 substeps 1a-1h + 1i-mig-1..5 + 1i-cleanup S1-S5 + 1k (SHM
channel auth); HEP-0042 Phases 0-3b (ZMQ pre-attach coordination);
#262 mutual-auth wire mechanism; AUTH-5 (doc sync); AUTH-6 batches
C0-C6 (L3 test revival); AUTH-7 SHM happy+deny + AUTH-7 ZMQ deny.

**Remaining, in dependency order:**
1. ~~#246 Phase 3a L4 close-out~~ ✅ **DONE 2026-07-02** — the
   cycle-driving producer L4 test `ZmqE2E_AuthorizedConsumerReceivesAllSlots`
   was un-skipped by the #246 Phase 3 close-out (commits `d8884e9f` /
   `3ea1bc4c` / `7aa831c2` / `e8cebe04`); green in the suite.  AUTH-7 ZMQ
   happy + deny + multi-producer fan-in all green (AUTH_TODO §AUTH-7).
   This line was stale (verified 2026-07-16).
2. #275 S2 remainder — 4-5 datahub worker files still to scan for
   legacy `secret` params.
3. #257 (1j) — L3 broker tests for HEP-0041 (success / denied /
   divergence-WARN).
4. REVIEW-C (#276) — gates after 1j + #275 close.
5. #262 close-out — ✅ DONE 2026-07-16.  Default flipped
   `shm_require_mutual_auth` → `true`.  No version-axis bump: the Frame
   2/3 exchange is a producer↔consumer AttachProtocol handshake, not a
   broker control-plane message — interop is the config knob.  Light
   attack coverage is L2 `MutualAuth_RejectsWrongProducerPubkey` (an L4
   squatter was scrapped as redundant — the baseline handshake already
   binds the producer key at Frame 2).  HEP-0044 §8.4 updated.
6. ~~REVIEW-D (#277)~~ ✅ **CLOSED 2026-07-17**
   (`docs/code_review/REVIEW_AUTH_ReviewD_2026-07-17.md`) — PID-liveness
   sweep removed (`a00a4188`); allowlist+revocation cycle pinned end-to-end
   at L3 (`ConsumerAttach_DeniedAfterDereg` #2369 + supporting) + L2 gate +
   ledger unit; passive-revocation contract confirmed; L4 data-plane cycle
   explicitly deferred.
7. ~~REVIEW-E (#278)~~ ✅ **CLOSED 2026-07-17 — 🟢 PHASE 1 PRODUCTION-READY**
   (`docs/code_review/REVIEW_AUTH_ReviewE_2026-07-17.md`) — 8-threat model
   fact-checked against live code; no Cat-1 gap in the single-hub CURVE chain;
   mutual auth closes the impersonation window; replay confirmed live +
   hardened + E2E-tested (`546bc115`).  Out of Phase 1 scope + tracked: admin
   plane (Line E — top open surface), inbox (#191), federation (#105).
   **Next security work: admin-plane CURVE (Line E).**
8. AUTH-6 bookkeeping — File 10 Suite 2 delete on #152 (housekeeping).

### Line 2 — Security Module (SMS) consolidation

One lifecycle module (`SecureSubsystem`) owns libsodium init, all
sodium wrappers, KeyStore, and encryption/decryption API.  Replaces
the scattered sodium_init sites + fragmented crypto surface.

**Authoritative HEP:** HEP-0043 (Security Subsystem).

**Shipped end-to-end (commits `5a24b410` + `ab944b55`, 2026-07-07;
SEC-Fold-2 finale + review fixes):**
- `SecureMemorySubsystem` → `SecureSubsystem` rename.
- `pylabhub::crypto` namespace deleted (Category 1a/1b/1c on SMS).
- `Crypto` sub-container collapsed.
- `key_store()` / `key_store_ready()` shims deleted; access is
  `secure().keys()`.
- `KeyStore` is member of `SecureSubsystem::Impl` (private ctor).
- Gate softened: only `keys()` requires SMS `Initialized`.
- `box_encrypt_using` / `box_decrypt_using` shipped as the
  name-based cited-seckey Category 1c methods.
- HEP-0043 §0-§7 + §11-§13 authoritative.

**Remaining:** SEC-Fold-1b — HEP-0043 §8 vault content + §10
script-crypto content migration from HEP-0038 (housekeeping).

### Line 3 — Broker SHM observer (broker probes producer's SHM metrics)

Broker gains authenticated read-only access to producer SHM header
pages (metrics) under HEP-0041's capability-transport model.  Uses
AttachProtocol with `role_type="observer"` and a distinct trust
anchor (broker's ephemeral observer pubkey).

**Authoritative HEP:** HEP-0045 (Broker SHM Channel Observer,
promoted 2026-07-08 from `DRAFT_broker_shm_observer_2026-07.md`).

**Shipped:** Phase A (`b3d5e36d`) — `datablock_get_metrics_from_fd`;
Phase B (`da2a5e76`) — `DataBlockObserverHandle`; D1 slice A
(`d6f5d621`) — `KeyStore::generate_and_add_identity`; D2 slice
(`f7d3a51e`) — REG_ACK extraction + `RoleAPIBase` setter;
C.2.a (`029bbe31`) — broker generates + emits observer pubkey;
C.2.b (`ce956972`) — producer verify path.

**Remaining, in dependency order (per HEP-0045 §10):**
1. C.2.c — `PeerDeathWatcher` interface + Linux `epoll` backend.
2. C.2.d — Broker dial worker + fd cache map + teardown.
3. D5 — Producer `startup.shm_metrics_observer` opt-out.
4. C.3 — `collect_shm_info` fd lookup + `metrics_source` field.
5. C.4 — L4 tests (e2e, opt-out, crash-safety, broker restart,
   multi-producer).
6. C.5 — HEP-0043 §9.3 + HEP-0041 §10.5 pointer refresh.

### Line 4 (foundation, no active work)

`IAttachChannel` seam + `run_producer_handshake` /
`run_consumer_handshake` transport-agnostic helpers.  Shipped
2026-07-07 as refactor of the SHM AttachProtocol internals; now
authoritative as HEP-0044.  Both Line 1 SHM and Line 3 observer
compose over the same helpers.  No standalone next-action.

### Session-start reading order

1. `docs/HEP/HEP-CORE-0044-AttachProtocol.md` — AttachProtocol
   primitive (protocol, `IAttachChannel`, `run_*_handshake`).
2. `docs/HEP/HEP-CORE-0045-Broker-SHM-Observer.md` — Line 3.
3. `docs/HEP/HEP-CORE-0043-Security-Subsystem.md` (§0-§7) — SMS
   API surface for all lines.
4. `docs/HEP/HEP-CORE-0041-SHM-Channel-Auth.md` — Line 1 SHM.
5. `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md`
   (§5b canonical wire schema) — Line 1 ZMQ.
6. `docs/HEP/HEP-CORE-0042-Channel-Attach-Coordination-Protocol.md`
   — Line 1 attach coordination.
7. `docs/todo/AUTH_TODO.md` — Line 1 tracker detail.
8. `docs/README/README_testing.md` § "framework contract (absolute)".

Prior SEC-Fold-2 planning drafts
(`DRAFT_sec_fold_2_*_2026-07.md`) archived to
`docs/archive/transient-2026-07-06/` — SEC-Fold-2 is complete;
those drafts are historical.

**Test suite:** 2332/2332 passing (2026-07-08; -11 from 2343 =
speculative ZMQ AttachProtocol code + its tests deleted this
session as YAGNI, none of the three lines needed them).
- #262 SHM mutual auth L4 squatter test — Frame 3 wire mechanism +
  role wiring + producer/consumer helpers all shipped; only the L4
  squatter test + default-`require_mutual_auth=true` flip remain.
- SEC-Fold-1b (HEP-0043 §3-§10 content migration) — §3-§7 authoritative
  as of this session; §8-§10 remain stubs.

**HEP-0032 ABI fingerprint chain:** fully shipped (broker echo, role
verify, strict-mode reject, tests).  Documented in HEP-CORE-0041
§D4.5 status sync (`678a5868`).

---

## Current Sprint Focus

### Ultimate goal — CURVE auth gate fully closed + CLI tool complete

**Locked execution plan (2026-06-27):**

| Phase | Goal | Trackers |
|---|---|---|
| **Phase 0** ✅ DONE | TODO/archive hygiene — close shipped-but-mislabeled items, archive 6 closed reviews, refresh sprint focus.  Phase 0a (commit `dfe86a61`) + Phase 0b 2026-06-27 (this commit) — AUTH_TODO compressed 1616→564 lines (archive index at `docs/archive/transient-2026-06-27/todo-completions/AUTH_TODO_completions.md`); Core Structure Change Protocol walk for #275 S5 at `docs/code_review/REVIEW_S5_CoreStructure_2026-06-27.md`. | shipped |
| **Phase 1** — CURVE chain close | AUTH-5 ✅ → AUTH-6 ✅ → #257 (1j) ✅ → AUTH-7 ✅ → #275 S2..S5 ✅ → REVIEW-C ✅ → #262 ✅ → REVIEW-D ✅ → REVIEW-E ✅ — **🟢 PHASE 1 PRODUCTION-READY (2026-07-17)** | #104, #154, #257, AUTH-7, #275, #276, #262, #277, #278 |
| **Phase 2a** — Role-host unification | Collapse `ProducerRoleHost` (549 LOC) + `ConsumerRoleHost` (456 LOC) + `ProcessorRoleHost` (649 LOC) into a single canonical `worker_main_()` skeleton inside `RoleHostFrame`. | #292 (new) |
| **Phase 2b** — Template RAII Phases 2-5 | `SlotIterator` over QueueWriter/Reader, timing parity with `run_data_loop`, `TypedInboxClient<MsgT>` / `TypedBand<EventT>`, `SimpleRoleHost<SlotT>` template parameterising the unified skeleton. | API_TODO §"Template RAII" Phases 2-5 |
| **Phase 3** — CLI `--init` one-shot bundling | `do_init()` wiring (`get_required_uid` + password chain + `cfg.create_keypair()`) + `HubDirectory::init_directory()` signature extension + 24+ L4 test-site migration. | #155 (in flight) |

**Earlier sprint state (preserved for context):** Arc A (`plh_hub`
renovation) shipped Phases 1-9; ⏳ Phase 10 doc closure (#73, doc
hygiene).  Arc B (Role-host renovation) Wave-B M0..M9 shipped.
RoleHostFrame plain class operational; dual-hub plh_hub + role
binaries functional.

**Remaining production gap:** HEP-CORE-0035 / HEP-CORE-0036 auth
chain — control-plane locked (D1+D2+D3), data-plane CURVE + role-
side dispatch + sibling-HEP code sync pending (covered by Phase 1).

### Production-readiness gap — CURVE + auth gating control-to-data

| Item | Status | Tracker |
|---|---|---|
| HEP-CORE-0035 auth implementation (7-phase plan in HEP-0035 §8) | 🚧 partial — control plane locked + strict-CURVE cleanup + Locked Key Memory shipped.  Phase B + #101 (§4.6 ACL) + D1 (`ChannelAccessIndex`) + D2 (broker CTRL ZAP) + D3 (notify-then-pull wire, broker_proto 5→6) shipped; C1..C5 strict-CURVE cleanup chain (#157-#161) shipped 2026-06-09; HEP-CORE-0040 Locked Key Memory chain (#165–#176 + #187) shipped 2026-06-09.  Data-plane gate close (AUTH-1..7 per `AUTH_TODO.md` — D4/D5/D6/D7 + HB-2 + HB-4+5 + HB-6) pending.  Legacy `RoleIdentityPolicy` placeholder live; task #152 retires it. | task #74; detail in `docs/todo/AUTH_TODO.md` |
| HEP-CORE-0036 authenticated connection establishment | Design final (2026-05-28; amended 2026-06-04 for notify-then-pull).  🚧 implementation in flight — D1+D2+D3 + C-chain + HEP-0040 chain shipped; AUTH-1 (D4 + D5 + B1 + B2; task #103) in-progress + AUTH-2 (HB-2; #162) + AUTH-3 (HB-4+5; #163) + AUTH-4 (HB-6 + #79) + AUTH-5 (sibling-HEP doc sync; #104) + AUTH-6 (L3 broker revival; #154) + AUTH-7 (L4 end-to-end) pending.  #105 federation parallel + non-blocking. | tasks #74, #101, #102, #103, #104, #105, #106, #162, #163, #164 |
| **HEP-CORE-0042 Channel Attach Coordination Protocol** *(NEW 2026-07-01)* | Design ✅ adopted 2026-07-01 (task #246 Phase 1 promotion).  Transport-agnostic coordination protocol: broker mediates channel attach so consumer's data-plane handshake succeeds against caught-up producer cache.  §6.1 Bindings.SHM (relocated from HEP-0041 §5.4) + §6.2 Bindings.ZMQ (new).  Instance-epoch guard (`instance_id`) closes stale-APPLIED_REQ race durably.  P4 security invariant + guardrails locked.  Impl phases 2-4 pending (L2 broker unit tests → L3 role-broker integration → L4 e2e + cross-engine parity for 4 accessors).  Sibling HEPs updated: HEP-0011 gains `on_channel_ready` callback; HEP-0028 gains 4 pending Native accessors; HEP-0036 §I5 + §6.5 gain pointers. | task #246 (design ✅; impl phases 2-4 pending); test-fixture follow-on for stale-instance APPLIED_REQ synthesis |

### Label hygiene — read before reading any "M*" label below

| Label prefix | Means | Examples |
|---|---|---|
| `Wave-B M0..M9` | Sequential phases of Arc B | `Wave-B M2`, `Wave-B M8` |
| `HEP-0033 §15 Phase N` | Sequential phases of Arc A | `Phase 7 D4.2`, `Phase 9` |
| `Wave-M2 / Wave-M2.5 / Wave-M3` | Closed side-arcs (multi-producer + controlled-access) | — |
| `M1.2 / M1.4 / M1.5 / MD1 / MD1.5` | Closed FSM-consolidation + race-fix side-arcs | — |

If a sentence says "M3" with no prefix, it's almost certainly
**Wave-B M3** — but verify against context.  `Wave-B M3` (RoleHandler
skeleton) is NOT `Wave-M3` (RoleEntry controlled-access, side-arc).

---

## Validation infrastructure (closes Task #44)

Demo framework — `share/demo_framework/runner.py` + 9 demo manifests
under `share/py-demo-*/` — delivers the L4 data-pipeline coverage
that earlier plans tracked as "L4 processor + consumer test
infrastructure (Wave-D)".  Use as the L4 reference for any new
pipeline scenario; clone + tweak.  See
`docs/todo/TESTING_TODO.md` § "Test infrastructure inventory" for
the demo inventory + manifest schema.

---

## Open work by area (pointers — detail lives in subtopic TODOs)

### API / ABI / concurrency / lifecycle (`docs/todo/API_TODO.md`)

- **HIGH PRIORITY** — task #238 — standardize key log-message
  format (test-contract markers).  Pattern 4 + future ladder rungs
  grep LOGGER substrings as FSM-transition contracts; standardize
  BEFORE the 7 pending rungs (#224-#229) ship so they're born in
  the standard format.  Surfaced 2026-06-16 during AUTH-1 close-out
  finding #13 discussion.  See `docs/todo/API_TODO.md` "Current
  Focus".
- **HIGH PRIORITY** — task #235 — Python `band_member_contains` /
  `_count` always return false/0 (wrong JSON nesting; 6 buggy
  sites, silent wrong answer, no test coverage; surfaced
  2026-06-15 during AUTH-1 close-out finding #11 discussion).
  See `docs/todo/API_TODO.md` "Current Focus" for full context.
- Demo-harness audit follow-ups (B3 / B4 / B6+B7 / B10 / B8 / N2 /
  N3+N4 / N5+N6 / N7+N10) — tasks #78-#87.
- Wave-MD1 ThreadManager Thread Shutdown Contract adoption sweep
  (BrokerService ctrl/admin, AdminService worker, HubHost admin).
- S1 Phase B: migrate `ZmqQueue` + `InboxQueue` sender to
  `apply_socket_policy` (task #66).
- Connection / Inbox / Band review D2 + D3 follow-ups (C2, C4, C5,
  I1, I3, X1, X3, X4, X6).
- ABI Compatibility (HEP-CORE-0032) — design ready, implementation
  not started.
- Deferred: pylabhub Python client SDK, script-spawned worker
  threads, `src/` + `src/include/` restructure.

### MessageHub / broker protocol (`docs/todo/MESSAGEHUB_TODO.md`)

- #92 audit remaining `_REQ` frames against HEP-0007 §12.2.1
  half-mix contract.
- H43 federation propagation of role-disconnect (trigger-gated).
- Wave M2 MP4 broker-handler residual items.
- 2026-05-03 `IncomingMessage` `sender` semantics for hub events.
- Hub State Query Layer
  (`docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md`; promoted from
  tech_draft 2026-06-02).  Phase A shipped; Phases B+ open.
- HEP-CORE-0035 auth implementation (task #74) — detail in
  `docs/todo/AUTH_TODO.md`.

### Tests / coverage (`docs/todo/TESTING_TODO.md`)

- B8 demo numpy pin via `plh_pyenv install --requirements`.
- N8+N9 bench variants (scalar dispatch + multi-size sweep).
- N11 cross-engine `on_band_message` signature parity audit.
- #154 re-create L3 broker tests against refactored lib code.
- **Pattern 4 multi-process test ladder** (12 in-scope rungs +
  1 deferred).  #220 shipped rung 1 (smoke) + the design pattern
  doc; #221 shipped rung 2 (Registration); #223 shipped rung 3
  (Heartbeat — REG_ACK cadence negotiation + first-tick + rate-
  band + symmetry); rungs 4 (#222 ConsumerLifecycle), 7 (#224
  Deregistration), 8 (#225 ChannelNotifies), 9 (#226
  RegistrationError), 11 (#228 Bands), 12 (#229 RoleIntrospection)
  are pending; rungs 5 (#162), 6 (#163), 10 (#227) blocked on
  AUTH chain.  Rung 13 deferred on back-channel-pipe infra.  Full
  ladder + per-rung HEP pin in `docs/README/README_testing.md`
  § "Pattern 4 — ... — Test ladder".

### Windows / MSVC / cross-platform / CMake (`docs/todo/PLATFORM_TODO.md`)

- N6 (#86) USER-ORIENTED `cmake/pylabhubNativePlugin.cmake` helper.
- CI is Linux-only vs README support claims — resolve.
- Clang-tidy quality pass.
- MSVC: `/Zc:preprocessor` propagation audit + `/W4 /WX` CI gate.

---

## Pending harness tasks (snapshot — TaskList is authoritative)

**P0 — do next** (ready, highest leverage):
- **#93** Instrument the producer validate-path with per-step log
  lines.  Cheap, unblocks future CI-flake diagnosis.  S.
- **#95** SCHEMA_REQ + METRICS_REQ — handlers exist
  (`broker_service.cpp:3147-3210` + `:1176`) but zero callers in
  `src/`.  KEEP-RESERVED vs DELETE decision; survey HEP-0034 §10.3 +
  federation schema requirements (#105) before deciding.  S.

**P1 — small cleanups, batch together** (S each):
- **#79** B4 `plh_role --init` non-zero SHM secret default.
- **#80** B6+B7 `rx.fz` binding + processor flexzone side doc.
- **#82** B10 `band_join` from `on_init` surface failure.
- **#85** N3+N4 native plugin `on_init`/`on_stop` signature + lifecycle
  module cleanup.

**P2 — high leverage, M effort**:
- **#86** N5+N6 `README_NativePlugins.md` + user-oriented
  `cmake/pylabhubNativePlugin.cmake` helper.

**P3 — substantial / multi-day**:
- **#94** HEP-CORE-0021 §16.5 ephemeral-binding production path —
  unlocks the design that the ENDPOINT_UPDATE sync API is for.
- **#84** N2 NativeEngine `build_api_(HubAPI&)` surface extension.
- **#87** N7+N10 three-engine doc parity
  (`README_Scripting_{Python,Lua,Native}.md`).
- **#73** HEP-CORE-0033 Phase 10 doc closure.

**P3 — HEP-0036 auth implementation chain** (production-readiness
blocker; detailed execution plan in `docs/todo/AUTH_TODO.md` under the
AUTH-1..7 numbering).

**2026-06-09 state:** The C1..C5 strict-CURVE cleanup chain shipped
(#157-#161 + close-out #186/#187).  The HEP-CORE-0040 Locked Key
Memory chain shipped (#165–#176).  Live work is AUTH-1..7 against the
broker glue (Phase D close + downstream).

Current critical path (each step blocks the next unless noted):

- **AUTH-1** (task **#103**, in-progress) — Role-side dispatch + D5
  `CONSUMER_REG_ACK.producers[]` emission + D4 BRC notify dispatch +
  consumer-side switch to authed factory + B1 (`awaiting_endpoint`)
  + B2 (`zmq_msg_gets("User-Id")`).  Closes HB-3.  Blocks AUTH-2..7.
- **AUTH-2** (task **#162**) — Producer-side `ZapRouter::pump_one`
  on BRC poll thread + multi-peer backlog drain (HB-2).
- **AUTH-3** (task **#163**) — `RegistrationState::Authorized` FSM
  state + `any_presence_authorized()` + data-loop outer guard
  (HB-4 + HB-5; also satisfies HEP-CORE-0036 §14.3 portion of #104).
- **~~AUTH-4~~** (tasks **#164** + **#79**) — **SUPERSEDED 2026-06-16
  by HEP-CORE-0041 (#244).**  Replaced by capability-transport
  (`memfd_create` + `SCM_RIGHTS` + pre-attach `CONSUMER_ATTACH_REQ_SHM`
  per HEP-0041 §9 D4).  **Phase 1 status as of 2026-06-23:** substeps
  1a-1h ✅ (#248-#255); 1i-mig-1/2a/2b-1/2b-2/2c review-fixes/2c M3/3
  ✅ (commits `e283a4ac → 6f31a346`); 1i-mig-M3.5 (#266), 1i-doc-sync
  (#267), 1i-prod-hardening (#268), 1i-api-scope (#269) ✅; 1i-mig-4
  consumer dial (#272) ✅ (commit `2793a394`); 1i-mig-5 cutover
  (#273) ✅; #281 broker `data_transport` strict ✅ (commit
  `ecc72337`); **REVIEW-A (#271) ✅; REVIEW-B (#274) ✅ close-out
  2026-06-23 — B1 strip_unix_scheme fix + B3 worker-thread label fix
  + B2 deferred to #275 scope + 5 medium items carried to REVIEW-C
  per AUTH_TODO §"REVIEW-B (#274) close-out"**; **1i-cleanup (#275)
  🟡 in flight** (S1+S2a+S2b+S2c-1..6+S3 ✅ shipped; S4+S5 ⏸
  pending Pattern 4 reform #285 so coverage doesn't regress);
  **1i-coverage (#270) ⏭ embedded in #258 (layer pivot 2026-06-25)**
  — initial L2 implementation reverted (`e8ca91b5`); shim was a
  parallel production scaffold.  **L2 contracts NOT dropped** —
  embedded inside #258 L4 via production marker grepping; complete
  checklist in #258 task description.  Layer migration ≠ contract
  drop.  1j (#257) + 1k (#258) + #262 mutual auth ⏸.
  **Five REVIEW-A..E milestones (#271/#274/#276/#277/#278) gate the
  remaining chain**; REVIEW-E is the Phase 1 production-ready final
  gate.  Full chain + milestone schedule in `docs/todo/AUTH_TODO.md`
  § "HEP-0041 implementation chain" + live tracker in HEP-0041 §10.1.
  Phases 2-3 (cross-platform #259-#261), Phase 4 (#247 framework
  crypto), Phase 5 (#246 — design ✅ 2026-07-01 as HEP-CORE-0042; ZMQ retrofit impl pending) follow.  #245
  (POSIX 0600 interim hardening) KILLED 2026-06-17.
- **AUTH-5** (task **#104**) — Sibling-HEP doc sync; 7 of 8 are pure
  doc edits.  L (multi-area).
- **AUTH-6** (task **#154**, in-progress) — Re-create L3 broker tests
  against refactored lib (D6).
- **AUTH-7** — L4 end-to-end auth-gated data flow (D7).  Closes #74.
- **#94** HEP-0021 §16.5 ephemeral binding — co-lands wire-shape per
  HEP-0036 §14.1 with AUTH-1; doesn't itself gate the auth goal.
- **HEP-0036 §5b canonical wire schema unification** (tracks #286 +
  sub-phases #287/#288/#289/#290; adjacent #291).  Opened 2026-06-25
  after L4 SHM e2e (#258) surfaced a silent gate — broker producer
  REG_ACK emitted `channel_id` while role read `channel_name`, silently
  skipping `Registered → Authorized`.  Phase A (HEP §5b normative
  authoring) and phase B-1 (immediate three-line fix) shipped together;
  B-2..B-5 deferred as separately-tracked sub-tasks.  Details:
  `docs/todo/AUTH_TODO.md` § "HEP-0036 §5b canonical wire schema
  unification".

Parallel / independent (any order, no AUTH-1 dependency):

- **#101** key-file ACL discipline — already shipped.
- **#102** runtime key handling — **SUPERSEDED 2026-06-05** by the
  HEP-CORE-0040 chain (#165–#176 all shipped 2026-06-09); closes
  when stragglers reach steady state.
- **#106** HEP-CORE-0038 script-vault keystore — depends on #104
  shipping first AND on HEP-0040 storage layer (#167 + #170 shipped).
  L.
- **#105** Federation / HEP-CORE-0037 — explicitly post-MVP per
  HEP-0036 §13.1.  L+.

**P4 — long tail (interleave when context permits)**:
- **#66** S1 Phase B `ZmqQueue + InboxQueue` migrate to
  `apply_socket_policy`.
- **#75** `HUB_TARGETED_ACK` wire frame.
- **#76** Script reload — promote tech_draft to HEP + implement.
- **#77** Tier 2 dynamic callbacks.
- **#81** B8 `plh_pyenv install --requirements` in demo setup.
- **#88** N8+N9 bench variants (scalar dispatch + multi-size sweep).
- **#89** N11 cross-engine `on_band_message` signature parity audit.

**Recommended next-session ordering** (refreshed 2026-05-28 after
HEP-0036 lock-in): start the HEP-0036 auth chain with #101 + #102
(mechanically independent of #74; can ship first), then #74 (the
production gate).  Batch P1 small cleanups + #93 / #95 in parallel.

---

## Active code reviews

- `docs/code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` —
  authoritative for D2 + D3 open follow-ups; tracked in API_TODO.
  Open items reproduce in code (e.g. X6 `ChecksumRepairPolicy::Repair`
  still a no-op `broker_service.cpp:6219`; X2 dead `query_shm_info`).
- `docs/code_review/REVIEW_CatchBlocks_2026-05-01.md` — full-codebase
  silent-failure sweep; §2/§3/§4 finding sections never populated
  (unstarted, not resolved).
- `docs/code_review/REVIEW_FullModule_2026-04-06.md` — mostly moot
  (subsystems refactored away) BUT C-1 (`to_channel_side` duplicated
  ×3: consumer/producer/processor `_api.cpp`) + D-2 (stale ref
  `engine_module_params.hpp:10`) still reproduce — trivially closable.
- `docs/code_review/LINT_FIXES_PLAN.md` — §1 applied; §2.1–2.7 lint
  dispositions never decided (partly moot — the `hub_config.cpp`
  singleton it targeted was deleted 2026-04-29).  Needs a NOLINT-or-
  defer pass, then archive.

`REVIEW_TestAudit_2026-05-01.md` was already archived (2026-05-02,
`docs/archive/transient-2026-05-02/`) — the prior "TOP PRIORITY active"
entry here was stale and is removed (2026-07-18 hygiene).

Closed reviews archived per `docs/DOC_ARCHIVE_LOG.md`.

---

## Quick links

- Build invocations, CMake, staging: `README.md` (repo root) +
  `docs/README/README_CMake_Design.md`.
- Running tests + test patterns: `docs/README/README_testing.md`.
- Subsystem design contracts: `docs/HEP/HEP-CORE-*.md`.
- Implementation rules + error taxonomies + session hygiene:
  `docs/IMPLEMENTATION_GUIDANCE.md`.
- Doc placement + lifecycle: `docs/DOC_STRUCTURE.md` (incl. §2.1.1
  periodic TODO quality check).
- Archive log: `docs/DOC_ARCHIVE_LOG.md`.

---

## Maintenance rule (see `DOC_STRUCTURE.md` §2.1.1)

This file MUST stay ≤ 200 lines.  Subtopic TODOs MUST stay focused
on open items.  Periodic quality check at minimum at the end of
every sprint / major commit batch: verify completion claims against
**code + log** (not commit messages), then archive completed
content per `DOC_STRUCTURE.md` §2.1.  Finished items dragged in
TODOs for too long blur what to focus on.  TODOs are *for what to
do, not what has been done.*  Git is the historical record.
