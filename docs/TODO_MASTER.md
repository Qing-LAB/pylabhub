# Data Exchange Hub — Master TODO

**Scope:** strategic execution plan, current status, pointers to subtopic
detail.  Per `docs/DOC_STRUCTURE.md` §1.1 + §2.1.1: **keep this concise
(≤ 200 lines)** — detailed task tracking lives in `docs/todo/<area>_TODO.md`;
git is the historical record.  Completed-phase narrative extracted to
`docs/archive/transient-2026-07-{18,22}/todo-completions/TODO_MASTER_completions_2026-07-{18,22}.md`
(07-18 verbatim pre-compression text at commit `633d51c0`; 07-22 = the
post-reconcile shipped-sprint detail).

---

## Current status (2026-07-22)

> **Reconciled 2026-07-22** against `git log b0aa0f51..fc08850f` (34 commits since
> the 2026-07-18 fact-check), the subtopic TODOs, and code.  Shipped-work detail
> (admin CURVE commits, vault/inbox-replay, schema two-zone, the 27 resolved
> review findings) extracted to
> `archive/transient-2026-07-22/todo-completions/TODO_MASTER_completions_2026-07-22.md`.

**Closed lines** (detail in the completions index above):
- **Line 1 — CURVE auth chain:** 🟢 Phase 1 production-ready (REVIEW-E); + vault
  `known_roles` (HEP-0035 §4.8) + inbox replay defense (HEP-0027 §3.6).
- **Line E — Admin-plane CURVE:** ✅ shipped 2026-07-19 (was the #1 open security
  surface).  Residual polish only — `AUTH_TODO.md` Line E.
- **Inbox:** ✅ CURVE + cross-engine parity + replay + schema two-zone.
- **Line 2 — SMS (HEP-0043):** ✅ shipped.  Residual: SEC-Fold-1b §8/§10 vault +
  script-crypto content migration (housekeeping).
- **Line 4 — IAttachChannel (HEP-0044):** ✅ shipped.

**Open lines:**
- **Line 3 — Broker SHM observer (HEP-0045):** 🚧 Phases A/B + D1/D2 + C.2.a/b
  shipped; **C.2.c–C.5 open** (below).
- **Topology migration:** STATIC layer ✅ + live (topology factories, role-code
  migration C step 6 shipped `3d4fe07a`, multi-producer fan-in DATA plane
  proven green at L4 — code-verified 2026-07-25).  **DYNAMIC layer ✅
  CODE-COMPLETE 2026-07-26 (T3): S1 owner-locked `ChannelEntry` +
  `AWAITING_OWNER` role-host retry, S2 owner-death teardown, S3 dialer
  fast-fail shipped as one unit** (S4 peer-join callbacks 2026-07-25; S5
  objective counts = task #74).  L3 fan-in wire tests flipped
  consumer-first (C step 7 L3 half).  **The R6 broker-pends gate is
  RETIRED 2026-07-25 — do NOT build** (superseded by the owner-first
  contract).  Remaining: Phase E retirements (T4, unblocked) + Phase F
  demos incl. the L4 producer-first-spawn keystone (T5).  Detail:
  `TOPOLOGY_TODO.md`; permanent design = HEP-CORE-0017 §4.7 (draft retires
  with Phase F).
- **REG protocol redesign (HEP-0046):** ✅ **Phase B COMPLETE 2026-07-24**
  (task #57): all nine REG-family handlers typed; `to_legacy` bridge +
  `BrokerRegHandler` skeleton retired; `inbox_schema_json` → typed
  `SchemaSpec` boundary-parse (separate `inbox_packing` wire field
  retired); B.3 audit found the BRC/ACK flip already landed via the
  envelope/adapter arcs; `receive_and_validate` drift-guard L1 suite
  landed.  Residual REG-adjacent tracks: EnvelopeOnly-tier body classes
  (per-msg_type follow-ons; the #72 pass itself COMPLETED 2026-07-24 —
  wire-inventory audit 2026-07-26 names the uncovered inbound-notify
  bodies: CHANNEL_COUNT_NOTIFY, CHANNEL_EVENT_NOTIFY,
  CHANNEL_ERROR_NOTIFY, CHANNEL_BROADCAST_DELIVER_NOTIFY,
  BAND_BROADCAST_DELIVER_NOTIFY — each needs a wire_bodies class + a
  BRC shape-table arm), #69 federation ingress.
- **Full-system audit (`REVIEW_FullSystem_2026-07-20`):** 🚧 56 findings, **52
  resolved / 4 open** after the #72 reconciliation pass (2026-07-24) closed the
  hep-gap + dead-residue clusters.  Remaining open: federation ingress bypass
  (= #69) and three test-coverage items (inbox-worker magic/gap pins, hub_vault
  known_roles L2 round-trip, logger StressLog diagnostic).

---

## Active / next work

> **RANKED PRIORITY — set by the project owner 2026-07-27.**  Work the
> bands in order; inside a band, use leverage.  This ordering overrides
> the "roughly by leverage" heuristic that governed this section before.
>
> | # | Band | What it covers |
> |---|---|---|
> | **1** | **Security items** | ✅ CLOSED: dead/no-op identity + authority validators (#68, verified 2026-07-27); #66 Cat-A vault harness (2026-08-02); **#83 registration now decides on the key the peer PROVED, not the key it claimed** (2026-08-02) — malformed roster entries are fatal, the attested key rides the envelope, and two Pattern-4 cases pin a live impersonation attempt.  #95 (same defect on DEREG / ENDPOINT_UPDATE / CHANNEL_AUTH_APPLIED, 2026-08-02) — `gate_attested_role_ownership` now runs there, sharing one implementation with the registration check.  **#96 (2026-08-02)** — the last two homes of the same defect: the channel broadcast took its `sender_uid` from the request body and forwarded it verbatim, so any handshaked peer could forge message origin; and the control tier checked a claimed `role_uid` only against the routing id, which the same client picks.  Now the broadcast request carries no sender at all (broker_proto 7→8) and the broker stamps it from the proven key via `PeerAuthority::attribute_sender`, while `run_control_gates` binds every caller's-own-uid claim — heartbeat, band join/leave/broadcast — to that key.  Three Pattern-4 cases, each mutation-checked to fail on the harm (a forged broadcast delivered, a forged heartbeat's metrics landing on the victim's presence) rather than on a missing error reply.  **Open, in order:** the three security-adjacent coverage gaps (inbox frame-magic / gap-count / seq-reset-after-reconnect; hub_vault `known_roles` round-trip), #83 residual slices (inbox roster protocol, admin session binding), then federation (#69, design-first). |
> | **2** | **Shared-memory observer feature** | HEP-CORE-0045 Line 3 remaining phases: `PeerDeathWatcher` → broker dial worker + fd cache → opt-out → `collect_shm_info` → L4 tests → pointer refresh. |
> | **3** | **Backlog of smaller polish items** | The P0/P1 batches below (startup log lines, config defaults, per-area subtopic items) — small, independently shippable. |
> | **4** | **Role-program unification (C++ RAII framework)** | #292 collapse of the three role-host files, taken together with the Template-RAII layer (Phase 2b: `TypedInboxClient`, `SimpleRoleHost`) since both reshape the same surface; #55 test re-homing rides along. |
> | **5** | **The rest** | Topology T4/T5 residuals, the Pattern-4 test migration (#52), Windows/CI coverage, and everything else in "Open work by area". |

### Detail (open, within the bands above)

**Security (top open surface):**
- **Dead/no-op identity + authority validators (#68) — ✅ CLOSED 2026-07-21,
  re-verified against code 2026-07-27.**  All four are gone or armed: the
  schema-citation validator runs on every joiner path including the consumer
  one (four production call sites in `broker_service.cpp`, reject-counter
  increments pinned in `test_hub_state.cpp`); the key-rotation gate and the
  `RoleIdentityPolicy` string gate were deleted as redundant with the ZAP
  pubkey allowlist; the stale-SHM `producer_uid` cross-check was retired as
  dead wire, superseded by capability-fd attach (HEP-CORE-0013 banner).
  The task sat `pending` after the fix landed — closed on verification, not
  on the tracker's word.
- **Three security-adjacent coverage gaps (from REVIEW_FullSystem, all still
  reproduce):** (a) the inbox "bad magic" worker sends a 2-frame message that
  is rejected by the frame-count guard before the magic check ever runs, so
  frame-magic validation is untested; `recv_gap_count` has no test at all; and
  the HEP-0027 §3.6 rule that a reconnected sender's reset sequence number must
  NOT be mistaken for a replay is unpinned.  (b) `HubVault`'s `known_roles`
  save/reload round-trip has no L2 pin — only the L4 CLI test covers it, which
  cannot separate a vault regression from a CLI one.  (c) a broken diagnostic
  line in the logger stress test (one-line fix).
- **Federation — design-first, CONSOLIDATED under task #69 (ratified
  2026-07-24):** a full top-down design (HEP: hub↔hub trust model, peer
  lifecycle, wire, security) comes BEFORE any protocol work — no piecemeal
  patches.  #69 now owns every open federation item: the peer-DEALER
  ingress bypassing `receive_and_validate` (the last unvalidated broker
  ingress, REVIEW_FullSystem high), the HUB_PEER_HELLO/BYE +
  HUB_TARGETED/RELAY_MSG control-envelope bypass, H43 role-disconnect
  propagation, #75 HUB_TARGETED_ACK, and the #105/HEP-0037 post-MVP scope
  + skipped federation tests.  Detail: MESSAGEHUB_TODO "Federation —
  CONSOLIDATED".
  *(Admin-plane CURVE — the former #1 surface — ✅ SHIPPED 2026-07-19; residual
  polish only, AUTH_TODO Line E.)*
- **FullSystem-review remediation (4 open of 56)** — remaining: #69 federation
  ingress + 3 test-coverage items; see "Active code reviews" for the breakdown.

**In-flight arcs:**
- **#52** HubHostBrokerHandle → Pattern 4 sweep (in progress; ~21 in-process
  co-host workers across ~6 files remain; Round 1 recipe proven).
- **#57** HEP-0046 Phase B — ✅ COMPLETE 2026-07-24 (see "REG protocol
  redesign" above; full record in `MESSAGEHUB_TODO.md`).
- **Topology dynamic residual (consolidated plan T2-T5)** — **T3 = S1–S3
  owner-first contract code catch-up ✅ SHIPPED 2026-07-26** (with the T2/C
  step 7 L3 test flips folded in).  **The R6 broker-pends gate is RETIRED
  2026-07-25 (superseded by the owner-first contract; do NOT build)**; open:
  T4 Phase E retirements (pre-attach `CONSUMER_ATTACH_REQ_ZMQ`,
  `producer_peers` vector + Tier-2 leak, `ProducerEntry.zmq_node_endpoint` —
  now unblocked) and T5 Phase F demos incl. the L4 producer-first-spawn
  keystone + draft retirement.  Detail: `TOPOLOGY_TODO.md`.
- **Line 3 observer remaining** (HEP-0045 §10): C.2.c `PeerDeathWatcher`
  (epoll) → C.2.d broker dial worker + fd cache → D5 opt-out → C.3
  `collect_shm_info` → C.4 L4 tests → C.5 pointer refresh.

**Role-binary unification (Phase 2a — the next major structural arc, #292 + #55):**
- **#292** — collapse the THREE still-separate role-host `.cpp` files
  (`producer_role_host.cpp` 591 LOC + `consumer_role_host.cpp` 506 +
  `processor_role_host.cpp` 716, each `final : public RoleHostFrame`) into one
  canonical `worker_main_()` in `RoleHostFrame`.  (The RoleAPI unification it
  rides on largely landed — CycleOps already unified into
  `src/utils/service/cycle_ops.hpp`, RoleAPIBase is Messenger-free — but the
  host collapse itself is NOT done.)  Design anchor: `raii_layer_redesign.md §2`.
  **New requirement (2026-07-26, from the schema/metrics-query design):** the
  unified host must admit an OBSERVER role kind — control-plane-only (BRC +
  heartbeat, no data channel) — so monitoring/exporter roles can pull metrics
  on the role plane; today every host fatals without channel establishment.
  See the observer-role rationale in `docs/archive/transient-2026-07-27/tech_drafts/DRAFT_schema_metrics_query_integration_2026-07-26.md` (archived 2026-07-27 — arc complete; the requirement itself lives in this entry).
- **#55** — re-home the 4 `role_api_base_*` L3 tests during that unification
  (deferred, `TESTING_TODO` Group B; all 4 confirmed still-valid 2026-07-16).
- **Phase 2b** — Template RAII Phases 2/4/5 (`TypedInboxClient<MsgT>`,
  `TypedBand<EventT>`, `SimpleRoleHost<SlotT>` — verified absent from `src/`;
  Phase 3 MaxRate pacing already shipped in `slot_iterator.hpp`).
- **Phase 3 (#155, in flight)** — CLI `--init` one-shot bundling + 24+ L4
  test-site migration (`--init` mode flag parses; bundling incomplete).

---

## Open work by area (detail in subtopic TODOs)

- **API / ABI / concurrency / lifecycle (`API_TODO.md`)** — #232 engine
  parity-test contract (incl. #235 band-accessor L3 regression tests);
  demo-harness follow-ups #78-#87; Wave-MD1 ThreadManager shutdown-
  contract sweep; #66 `ZmqQueue`+`InboxQueue` → `apply_socket_policy`;
  Connection/Inbox/Band review D2+D3 follow-ups (C2,C4,C5,I1,I3,X1-X6); HEP-0032
  ABI-compat broader impl (fingerprint chain shipped); #86 last-resort trace ✅ SHIPPED 2026-07-29 — buffer moved into the debug
  module (`debug_info.hpp`/`.cpp`) with a set-only dirty latch, freeze-on-print,
  `PLH_DEBUG_TRACE_BYTES` (16384); `panic()` no longer allocates before
  emitting; clients are lifecycle (`LifecycleManager::critical_report`), Logger,
  ZMQContext, ThreadManager, ZapPumpThread and the SIGTERM watcher; lifecycle
  owns no storage.  Residual: release clean-silence branch needs a subprocess
  test; **#85 teardown stall — cause STILL UNKNOWN** (likely the same open bug as
  #93/#242 in HEP-CORE-0004); its diagnosability half is now closed by #86, so
  the next recurrence should name the step that hung;
  #88 thread-spawn resource failure escaping the non-throwing failure channel
  ✅ SHIPPED 2026-07-29 (`std::thread` ctor threw out of
  `ThreadManager::spawn`'s `bool` and out of `timedShutdown` into
  `~LifecycleGuard() noexcept` → `std::terminate` during teardown; now routed
  through the existing `bool` / `ShutdownOutcome` channels, with three
  discarded `spawn()` returns fixed);
  deferred: Python client SDK, script-spawned worker threads, `src/` restructure.
- **Security / vault (`API_TODO.md`, task #89)** — **#89 SMS expansion + vault
  design**: retained vault key in SMS, script vault surface (HEP-CORE-0038 /
  #106, confirmed NOT implemented from `key_store.hpp:215,297`), and runtime
  config reload — one task because all three need the vault openable after
  startup without the password being script-reachable.  Includes the live hole
  that the master password sits in the environment in cleartext while
  `os.getenv` is unsandboxed in Lua and Python has no sandbox at all.
- **MessageHub / broker protocol (`MESSAGEHUB_TODO.md`)** — #92 `_REQ`-frame
  half-mix audit; (H43 federation role-disconnect → folded into #69); Wave-M2 MP4
  residuals; HEP-0039 Hub State Query Layer Phases B+ (Phase A shipped);
  native-engine inbox parity ✅ CLOSED 2026-07-18.
- **Tests / coverage (`TESTING_TODO.md`)** — Pattern-4 ladder rungs 4/5/6/7/8/9/10/12
  pending (classes absent; rungs 5/6/10 now unblocked by Phase 1; rung 11 partial —
  band contract covered in `test_pattern4_channel_group`/`_broker_protocol`; rungs
  2/3 shipped; rung 13 deferred on back-channel-pipe infra); Pattern-4 sweep #52
  (Round 7 = #56 `datahub_broker_workers`); **#58** audit — confirm every L3/L4
  test-worker file is actually in a real ctest run; #296 hub-death L4; N8/N9 bench
  variants; N11 `on_band_message` parity; B8 numpy pin.
- **Windows / MSVC / cross-platform (`PLATFORM_TODO.md`)** — CI is Linux-only vs
  README support claims; MSVC `/W4 /WX` gate + `/Zc:preprocessor` audit;
  clang-tidy quality pass; #86 native-plugin cmake helper.

---

## Pending harness tasks (open only — TaskList is authoritative)

- **P0:** #93 producer validate-path per-step log lines; #95 SCHEMA_REQ +
  METRICS_REQ keep-reserved-vs-delete (survey HEP-0034 §10.3 + federation #105).
- **P1 (batch):** #79 `--init` non-zero SHM secret default; #80 `rx.fz` binding +
  processor flexzone doc; #82 `band_join` from `on_init` failure surface; #85
  native `on_init`/`on_stop` signature + lifecycle cleanup.
- **P2:** #86 `README_NativePlugins.md` + user-oriented cmake helper.
- **P3:** #94 HEP-0021 §16.5 ephemeral-binding production path (unlocks the
  ENDPOINT_UPDATE sync API — incl. port-0 inbox endpoints); #84 NativeEngine
  `build_api_(HubAPI&)` extension; #87 three-engine doc parity; #73 HEP-0033
  Phase 10 doc closure.
- **P4 long tail:** #66, (#75 `HUB_TARGETED_ACK` → folded into #69), #76 script reload (promote
  `SCRIPT_RELOAD_DESIGN` → HEP + impl), #77 Tier-2 dynamic callbacks
  (`engine_callback_tiers.md`), #81, #88, #89.
- **Parallel / post-MVP:** #106 HEP-0038 script-vault keystore (needs #104 +
  HEP-0040 storage); **#105 Federation / HEP-0037 — explicitly post-MVP, consolidated under #69 (design-first)**
  (federation tests skipped in-suite today).

Deferred follow-ups (tracked, non-blocking): topology **P6** version-tagged
membership (replace full-set allowlist copy); **D-3** clang-query build-fail
rule for §I9.1 layer regressions; native-*sender* inbox L4 delivery test
(needs native-L4-role harness; transport already proven via L3 CURVE + L4
Python); AUTH-6 File 10 Suite 2 delete (#152 housekeeping).

**Doc-debt:** **HEP-0011 D1** — HEP-CORE-0011 is fundamentally stale (documents
the pre-composition inheritance hierarchy + wrong threading model / class
names).  Rewrite tracked here so it isn't lost (was only in a since-deleted
review memory).

**Parked git stashes (2026-07-18):** 5 exist; `stash@{1..4}` MOOT (landed or
superseded).  Only **`stash@{0}`** (AUTH-2 #162 producer-side BRC ZAP-pump
PeriodicTask) may still be wanted — that pump is NOT on the branch (broker ships
per-cycle `pump_one`, not the stash's drain-loop); reconcile before dropping.

---

## Validation infrastructure (closes #44)

Demo framework — `share/demo_framework/runner.py` + 9 demo manifests under
`share/py-demo-*/` — is the L4 data-pipeline reference; clone + tweak for new
scenarios.  Inventory: `TESTING_TODO.md` § "Test infrastructure inventory".

---

## Active code reviews (5 — updated 2026-07-31)

- `code_review/REVIEW_FullSystem_2026-07-20.md` — full-system HEP-vs-code audit
  (56 findings). 🚧 **52 RESOLVED (07-20→07-24), 4 OPEN** (#69 + 3
  test-coverage items — see the summary bullet above).  Resolved-finding
  detail (#67–#72 + schema) in the 07-22 completions index; per-finding evidence
  in the review doc's `✅` blocks.  **29 OPEN, by cluster** (files in review doc):
  - **Federation ingress bypass** (1, high) — peer-DEALER skips the admission
    gate chain; consolidated under the #69 design-first federation task.
  - **Systemic HEP↔code drift** (7, high) — governing HEPs describe superseded
    models (HEP-0032/0026/0033/0027/0020/0019 + synthesis).
  - **Dead-residue, post-CURVE/vault cutover** (8, med) — broker/inbox/vault/
    keystore residue + authoritative-storage doc contradictions.
  - **Typed-envelope BRC bypass** (4, med) — raw-JSON scatter off the typed path;
    folds into **#57** (HEP-0046 Phase B).
  - **Test-coverage / design gaps** (5) — harness + pattern4 + inbox-worker +
    vault + logger.
  - **Misc hep-gap** (4) — hub_cli, hub_state, consumer_api, script_engine_factory.
- `code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` — D2+D3 follow-ups
  (X6 `ChecksumRepairPolicy::Repair` no-op `broker_service.cpp:6219`; X2 dead
  `query_shm_info`); tracked in API_TODO.
- `code_review/REVIEW_CatchBlocks_2026-05-01.md` — full-codebase silent-failure
  sweep; finding sections never populated (unstarted).
- `code_review/REVIEW_FullModule_2026-04-06.md` — mostly moot; C-1
  (`to_channel_side` ×3) + D-2 (stale ref `engine_module_params.hpp:10`) still
  reproduce — trivially closable.
- `code_review/LINT_FIXES_PLAN.md` — §2 lint dispositions undecided (partly
  moot); needs a NOLINT-or-defer pass, then archive.

Closed reviews archived per `docs/DOC_ARCHIVE_LOG.md` (latest batch 2026-07-18).

---

## Label hygiene — read before reading any "M*" label

| Label prefix | Means |
|---|---|
| `Wave-B M0..M9` | Sequential phases of Arc B (role-host renovation) |
| `HEP-0033 §15 Phase N` | Sequential phases of Arc A (`plh_hub` renovation) |
| `Wave-M2 / Wave-M2.5 / Wave-M3` | Closed side-arcs (multi-producer + controlled-access) |
| `M1.2 / M1.4 / M1.5 / MD1 / MD1.5` | Closed FSM-consolidation + race-fix side-arcs |

A bare "M3" is almost certainly **Wave-B M3** (RoleHandler skeleton) — verify
against context; NOT `Wave-M3` (RoleEntry controlled-access side-arc).

---

## Quick links

- Build / CMake / staging: `README.md` + `docs/README/README_CMake_Design.md`.
- Running tests + patterns: `docs/README/README_testing.md`.
- Subsystem design contracts: `docs/HEP/HEP-CORE-*.md`.
- Implementation rules + error taxonomies + session hygiene:
  `docs/IMPLEMENTATION_GUIDANCE.md`.
- Doc placement + lifecycle: `docs/DOC_STRUCTURE.md` (incl. §2.1.1 TODO quality
  check).  Archive log: `docs/DOC_ARCHIVE_LOG.md`.

---

## Maintenance rule (see `DOC_STRUCTURE.md §2.1.1`)

Keep this file ≤ 200 lines; subtopic TODOs focused on OPEN items.  At the end of
every sprint / major commit batch, verify completion claims against **code + log
(not commit messages)**, then extract completed content to a dated completions
index.  TODOs are *for what to do, not what has been done.*
