# Data Exchange Hub — Master TODO

**Scope:** strategic execution plan, current status, pointers to subtopic
detail.  Per `docs/DOC_STRUCTURE.md` §1.1 + §2.1.1: **keep this concise
(≤ 200 lines)** — detailed task tracking lives in `docs/todo/<area>_TODO.md`;
git is the historical record.  Completed-phase narrative extracted to
`docs/archive/transient-2026-07-{18,22}/todo-completions/TODO_MASTER_completions_2026-07-{18,22}.md`
(07-18 verbatim pre-compression text at commit `633d51c0`; 07-22 = the
post-reconcile shipped-sprint detail).

---

## Current status (2026-08-03)

> **Shipped-work narrative lives in git and in
> `archive/transient-2026-07-22/todo-completions/`.**  Task-list validations
> (2026-08-03, 2026-08-07) are recorded in `DOC_ARCHIVE_LOG.md`; what they
> found that is still OPEN is in the next-actions table below.

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
  BRC shape-table arm).  Federation ingress is parked with #69.
- **Full-system audit (`REVIEW_FullSystem_2026-07-20`):** ✅ 56 findings, **52
  resolved / 4 open** after the #72 reconciliation pass (2026-07-24) closed the
  hep-gap + dead-residue clusters.  Every ✅ note independently verified against
  source 2026-08-07 — all held; three residual doc drifts filed as #110.  The three test-coverage items (inbox-worker
  magic/gap pins, hub_vault known_roles L2 round-trip, logger StressLog
  diagnostic) were validated and closed 2026-08-02 — see band 1.  The fourth —
  federation ingress bypass — is parked with #69 and is not counted as open
  work.

---

## Active / next work

> **RANKED PRIORITY — set by the project owner 2026-07-27.**  Work the
> bands in order; inside a band, use leverage.  This ordering overrides
> the "roughly by leverage" heuristic that governed this section before.
>
> | # | Band | What it covers |
> |---|---|---|
> | **1** | **Security items** | **Open:** vault hot reload (HEP-0035 §4.8.5) — its stated prerequisite (§4.9 roster replication) shipped 2026-08-06, so it is now buildable; #103 admin anti-hijack L2 test; #89 SMS/vault script surface; #87 privilege audit.  The impersonation arc (#83/#95/#96 + inbox sender) is CLOSED on every plane — narrative in git, not here.  **Federation is NOT in this band** — see the parked entry below. |
> | **2** | **Shared-memory observer feature** | HEP-CORE-0045 Line 3 remaining phases: `PeerDeathWatcher` → broker dial worker + fd cache → opt-out → `collect_shm_info` → L4 tests → pointer refresh. |
> | **3** | **Backlog of smaller polish items** | The P0/P1 batches below (startup log lines, config defaults, per-area subtopic items) — small, independently shippable. |
> | **4** | **Role-program unification (C++ RAII framework)** | #292 collapse of the three role-host files, taken together with the Template-RAII layer (Phase 2b: `TypedInboxClient`, `SimpleRoleHost`) since both reshape the same surface; #55 test re-homing rides along. |
> | **5** | **The rest** | Topology T4/T5 residuals, the Pattern-4 test migration (#52), Windows/CI coverage, and everything else in "Open work by area". |

### Next actions — code-verified 2026-08-07

Each row was checked against source in this pass.  Nothing here rests on a
`✅` marker, a commit message, or a comment.

| Next | Why now | Evidence checked |
|---|---|---|
| **Vault hot reload** (HEP-0035 §4.8.5) | The biggest open security item, and it just became buildable.  §4.8.5 states its own precondition — *"Implement §4.9 first, or reload ships a guarantee it does not have"* — because re-reading the vault fixes only the hub's own gate while every running role keeps deciding on the roster it got at registration.  §4.9 shipped 2026-08-06. | No reload method exists: the admin surface has 14 `kAdmin*` msg_types and none of them reload.  `grep -i reload` over `admin_service.*` + `hub_vault.hpp` returns nothing. |
| **#103 — admin anti-hijack L2 test** | Small, and it is the last thing standing between #83 and closed. | Only `test_admin_session.cpp` exercises the fact-match, at module level.  The L2 fixture already mints consoles with distinct routing ids, so no new harness is needed. |
| **`origin_uid`: build the scoped origin, or amend §11.0.5** | A doc-vs-code split, and the doc currently promises the larger thing.  Owner's call which way it resolves. | `origin_uid` is hand-threaded into two queue records and emitted on two notifies (`broker_service.cpp:1434`, `:1472`).  The scoped "current actuation origin" §11.0.5 describes — inherited automatically by every log line and NOTIFY in a teardown cascade — does not exist anywhere in `src/`. |
| **#98 — `channel_broadcast` three-engine parity test** | A shipped feature whose per-engine bindings are compile-verified only, against an explicit project rule that each engine is verified directly. | The four L2 dispatcher tests drive a **fake** engine: `test_dispatch_notifications.cpp` defines its own `invoke_on_channel_broadcast` that records calls.  No real Lua/Python/Native binding is exercised anywhere. |
| **Topology Phase E retirement** (T4) | Unblocked since 2026-07-25; the legacy message is still carried. | `CONSUMER_ATTACH_REQ_ZMQ` live in 4 files / 10 references (`broker_service.cpp` ×7, `wire_dispatch.hpp`, `broker_request_comm.cpp`, `plh_version_registry.hpp`). |

**Deliberately not promoted:** #102 (LOW — three branches, two of them the
same loop body at a different index; explicitly *not* via two hub
processes), and #87 / #89, which were not re-verified in this pass and
should be re-scoped against code before anyone starts them.


> **Review findings are not the plan.**  A finding produced by a review pass
> is a CLAIM until someone validates it against code and the owner accepts it.
> Two rules, both learned the hard way on 2026-08-02:
>
> 1. **Findings live in `docs/code_review/REVIEW_*.md` until triaged.**  They
>    do not get copied into this file as work.  This file carries what the
>    owner decided; mixing the two makes a robot's suggestion indistinguishable
>    from a decision, and the suggestion then gets worked on first.
> 2. **Validate before scheduling, not after.**  For any finding of the form
>    "X is untested" or "Y is unchecked": find where X is implemented, find
>    whether that place is already covered, and read the design doc that says
>    what X is supposed to do.  Of the three findings that had sat at the head
>    of band 1 for two weeks, two evaporated under one grep each.

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
- **"Three security-adjacent coverage gaps" — VALIDATED AGAINST CODE
  2026-08-02.  Two of them do not exist.**  They were carried here verbatim
  from REVIEW_FullSystem and never checked.  What is actually true:
  - **Frame-magic "validation is untested" — MISLEADING; the validation is
    tested.**  Magic is checked in exactly one place,
    `wire_detail::decode_frame` (`zmq_wire_helpers.hpp`), shared by the inbox
    and the data plane, and it is pinned at L1 by
    `ZmqWireFrameTest.RejectsWrongMagic`.  What the review actually
    established is narrower and true: the L3 worker named for it could never
    reach the decoder — a DEALER's single frame arrives as two, and the
    four-frame envelope guard rejects it first — so the INBOX's own
    `!env.valid → count + drop` branch is never entered via a bad magic.
    That branch's siblings (schema-tag mismatch, payload-size mismatch,
    envelope shape) are each pinned by existing workers and all do the same
    thing, so re-entering it through a fourth trigger is low value; it is
    NOT blocked, though — a bad magic short-circuits `decode_frame` before
    the schema-tag check, so no schema tag has to be forged.  Action taken:
    the test was renamed to `WrongFrameCount_Drops` after what it does pin.
    Adding a bad-magic L3 worker remains optional and unscheduled.
  - **"Reset seq mistaken for a replay" — FALSE.**  HEP-0027 §3.6 states
    that `seq` is metrics-only and explicitly must NOT gate replay, because
    it resets on reconnect; replay is defended by nonce + skew.  The receive
    path matches.  There is no code path in which a reset seq could be read
    as a replay, so there is nothing to pin.
  - **`recv_gap_count` untested — TRUE, now CLOSED** (2026-08-02).  An earlier
    pass in this same session called it blocked, reasoning that a test cannot
    forge a frame the receiver accepts because `compute_inbox_schema_tag` is
    file-static.  That was the wrong question: the counter is not reached by
    FORGING a frame, it is reached by CAUSING a loss.  A small `rcvhwm` plus a
    receiver that stops draining makes `InboxClient::send` drop — its
    documented behaviour — and because `send()` consumes a sequence number
    before it attempts the write, every drop burns a seq that never reaches
    the wire, so the next delivered message's gap equals the number lost
    exactly.  `InboxQueueTest.GapCount_TracksDroppedSends`, mutation-checked.
    Also note this is a METRICS counter; "security-adjacent" was mislabelled.
  - **`HubVault` `known_roles` round-trip — TRUE, now closed** (2026-08-02):
    five L2 pins at the vault seam — deny-all bootstrap, in-memory-only
    `set_known_roles`, save/reopen round-trip, keypair+token preserved across
    save, and the roster living inside the ENCRYPTED payload with no
    plaintext sidecar (the one that would catch a re-plaintexted allowlist).
  - **Logger stress diagnostic — TRUE, now closed** (2026-08-02): the line
    had no `{}` placeholder AND counted a different marker than the
    assertion, so it could only ever print nothing.
  **Process note:** review output was written straight into this file as if
  it were decided work.  It is not.  See "Review findings are not the plan".
- **Federation — 🅿 PARKED (owner, 2026-08-02).  NOT security work, NOT
  next work, not to be re-raised.**  Federation is an unactivated
  proposal: no running hub or role uses it, `cfg.peers` is empty in every
  real deployment, and nothing behind the peer path can be reached until
  someone configures a peer.  Every open federation item stays parked
  under #69 until the owner decides what federation is and when it is
  needed — no design pass, no piecemeal patches, no promotion on the
  strength of a latent finding.  Parked scope: the peer-DEALER ingress
  bypassing `receive_and_validate`, the HUB_PEER_HELLO/BYE +
  HUB_TARGETED_MSG control-envelope bypass (task #99 — an admitted role
  can register itself as a configured peer hub; `HUB_RELAY_MSG` is NOT
  affected, it arrives on the dedicated outbound peer DEALER so its
  sender is fixed by the socket), H43 role-disconnect propagation, #75
  HUB_TARGETED_ACK, and the #105/HEP-0037 post-MVP scope + skipped
  federation tests.  Detail: MESSAGEHUB_TODO "Federation — CONSOLIDATED".
  *(Admin-plane CURVE — the former #1 surface — ✅ SHIPPED 2026-07-19; residual
  polish only, AUTH_TODO Line E.)*
- **FullSystem-review remediation (4 open of 56)** — 3 test-coverage items are
  the live remainder; the fourth (federation ingress) is parked with #69.  See
  "Active code reviews" for the breakdown.

**In-flight arcs:**
- **#98 Channel broadcast — make it reachable from scripts (owner-approved
  2026-08-02, ACTIVE).**  The facility is unreachable in BOTH directions
  today: `BrokerRequestComm::send_broadcast` has no `RoleAPIBase` method and
  no engine binding, so nothing can send it; and the broker's
  `CHANNEL_BROADCAST_DELIVER_NOTIFY` to producer + consumers hits a
  `parse_notification_id` with no arm for that string, so it classifies as
  `Unknown` and nothing can receive it.  Binding one end alone would ship a
  call that goes nowhere.  Named `channel_broadcast` / `on_channel_broadcast`;
  payload stays `message` + `data` (already the wire, and what the #96 typed
  body requires).  Full HEP-CORE-0011 Sync Matrix sweep in one commit — role
  API, both dispatch halves, Lua, Python, Native (ABI 13→14), `.pyi` ×3,
  three-engine parity tests, HEP-0030/0007 docs.
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

## Active code reviews (3 — updated 2026-08-07)

> **Review output is not the plan.** These records are candidate findings,
> not decided work. Nothing here is scheduled until it has been validated
> against code and a HEP, and then chosen. The validation record below is
> the current state of that check.

- `code_review/REVIEW_VALIDATION_2026-08-02.md` — the validation pass over the
  other records. Of ~26 items that were carried as open, 12 were already fixed
  and never marked, 2 were stale, 2 were misreads, 5 were advisory, and 5 were
  genuinely valid. **Three carried forward, all LOW:** B-1 (loop helpers, now in
  `API_TODO.md`), O1b (`query_shm_info`, settle inside band 2 — now also
  tracked as #112), O3 (three forwarders — recorded, not recommended).  Its
  standing caveat about the FullSystem review's 50 unverified ✅ notes is
  **discharged**: that verification ran 2026-08-07 and every note held.
- `code_review/REVIEW_FullSystem_2026-07-20.md` — ✅ **VERIFICATION COMPLETE
  2026-08-07; ARCHIVABLE.**  The count reconciliation stands (the "29 OPEN by
  cluster" figure predated #72's 15 and #57's 4 — stale, not a competing
  measurement; the header's 4 are three verified-present test-coverage items
  plus the federation-ingress bypass #69 already names).  What was missing is
  now done: **all ~50 self-reported ✅ notes were independently re-checked
  against source, and every one held on substance.**  The pass corrected two
  notes whose *evidence* had gone stale (they named
  `KnownRolesStore::as_peer_allowlist`, retired under #83, and
  `zmq_wire_helpers.hpp`, a file that does not exist) and filed three residual
  doc drifts as **#110** — cases where the ✅ closed the site the finding named
  and a sibling survived.  Effectively 0 open outside the parked federation
  task.
- `code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` — ✅ **VERIFICATION
  COMPLETE 2026-08-07; ARCHIVABLE.**  All 16 findings re-checked against
  source: 8 resolved, 2 closed as accepted design, 6 open.  The 6 are carried
  in `API_TODO.md` and as tasks **#111** (C5/B4/X5/S4 — the multi-presence
  model is half-built: structures carry the topology, operations still assume
  presence 0) and **#112** (X2 dead `query_shm_info`, X4 phase-label
  comments).  X6 is resolved — `ChecksumRepairPolicy::Repair` is a comment,
  not an enumerator (`broker_service.hpp:48`).
- `code_review/LINT_FIXES_PLAN.md` — §2 lint dispositions undecided (partly
  moot); needs a NOLINT-or-defer pass, then archive.

`REVIEW_FullModule_2026-04-06.md` was archived 2026-08-03 — all ten rows
dispositioned against code; its one live finding (B-1) moved to `API_TODO.md`
first. Closed reviews archived per `docs/DOC_ARCHIVE_LOG.md`.

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
