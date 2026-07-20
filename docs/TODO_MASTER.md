# Data Exchange Hub — Master TODO

**Scope:** strategic execution plan, current status, pointers to subtopic
detail.  Per `docs/DOC_STRUCTURE.md` §1.1 + §2.1.1: **keep this concise
(≤ 200 lines)** — detailed task tracking lives in `docs/todo/<area>_TODO.md`;
git is the historical record.  Completed-phase narrative extracted 2026-07-18
to `docs/archive/transient-2026-07-18/todo-completions/TODO_MASTER_completions_2026-07-18.md`
(verbatim pre-compression text at commit `633d51c0`).

---

## Current status (2026-07-18)

- **Line 1 — CURVE auth chain:** 🟢 **PHASE 1 PRODUCTION-READY** (2026-07-17,
  REVIEW-E).  Single-hub CURVE end-to-end across ZMQ (libzmq CURVE+ZAP) and
  SHM (`AttachProtocol`, HEP-0044).  Replay hardened + live.  Out of Phase-1
  scope + tracked below: admin plane (Line E), inbox ✅ done, federation (#105).
- **Inbox:** ✅ CURVE-authenticated (hub-wide `known_roles`) + full cross-engine
  parity (native send added, ABI v10) — 2026-07-18.
- **Line 2 — SMS consolidation (HEP-0043):** ✅ shipped 2026-07-07.  Access is
  `secure().keys()`.  Residual: SEC-Fold-1b §8/§10 vault + script-crypto content
  migration (housekeeping).
- **Line 3 — Broker SHM observer (HEP-0045):** 🚧 Phases A/B + D1/D2 + C.2.a/b
  shipped; C.2.c–C.5 open (below).
- **Line 4 — IAttachChannel foundation (HEP-0044):** ✅ shipped; no next-action.
- **Topology migration:** Phases A/B/C-step1-2 + D-phase-field ✅; C step 3 ✅;
  **C step 7, D R6 gate, Phases E–H open.**  Design LOCKED
  (`DRAFT_topology_singular_side_2026-07.md`); tracker `TOPOLOGY_TODO.md`.
- **REG protocol redesign (HEP-0046):** Phase A + islanded Phase C shipped;
  **Phase B (typed-envelope commit rewire) = task #57**, tech-debt not a
  functional gap (envelope + gates + BRC already live on broker_proto 7).

---

## Active / next work (open, roughly by leverage)

**Security (top open surface):**
- **Admin-plane CURVE (Line E)** — admin plane is still plaintext/loopback-only;
  the #1 open security surface per REVIEW-E.  Simple-version spec: HEP-0033 §11 +
  `DRAFT_curve_admin_protocol_2026-07-15.md`.

**In-flight arcs:**
- **#52** HubHostBrokerHandle → Pattern 4 sweep (in progress; ~21 in-process
  co-host workers across ~6 files remain; Round 1 recipe proven).
- **#57** HEP-0046 Phase B — migrate REG handlers to the typed WireEnvelope
  commit path (relocate-into-typed-form refactor; parity list on the task).
- **Topology C step 7 + D R6 gate symmetrization + Phases E–H** (E retires
  HEP-0017 §3.3 multi-endpoint PULL + HEP-0042 §5/§7.1 pre-attach; G lands the
  fan-out ZMQ role-host + slow-joiner L4 test).  Detail: `TOPOLOGY_TODO.md`.
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
- **#55** — re-home the 4 `role_api_base_*` L3 tests during that unification
  (deferred, `TESTING_TODO` Group B; all 4 confirmed still-valid 2026-07-16).
- **Phase 2b** — Template RAII Phases 2/4/5 (`TypedInboxClient<MsgT>`,
  `TypedBand<EventT>`, `SimpleRoleHost<SlotT>` — verified absent from `src/`;
  Phase 3 MaxRate pacing already shipped in `slot_iterator.hpp`).
- **Phase 3 (#155, in flight)** — CLI `--init` one-shot bundling + 24+ L4
  test-site migration (`--init` mode flag parses; bundling incomplete).

(Note: #235 Python band-accessor fix and #238 log-format standardization were
verified SHIPPED 2026-06-27 against code — `consumer_api.cpp:75` anchor;
`event=` format deployed — the old master's "HIGH-priority open" listing was
stale.  Only residual: #235 L3 parity regression tests, fold into #232.)

---

## Open work by area (detail in subtopic TODOs)

- **API / ABI / concurrency / lifecycle (`API_TODO.md`)** — #232 engine
  parity-test contract (incl. #235 band-accessor L3 regression tests);
  demo-harness follow-ups #78-#87; Wave-MD1 ThreadManager shutdown-
  contract sweep; #66 `ZmqQueue`+`InboxQueue` → `apply_socket_policy`;
  Connection/Inbox/Band review D2+D3 follow-ups (C2,C4,C5,I1,I3,X1-X6); HEP-0032
  ABI-compat broader impl (fingerprint chain shipped); deferred: Python client
  SDK, script-spawned worker threads, `src/` restructure.
- **MessageHub / broker protocol (`MESSAGEHUB_TODO.md`)** — #92 `_REQ`-frame
  half-mix audit; H43 federation role-disconnect propagation; Wave-M2 MP4
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
- **P4 long tail:** #66, #75 `HUB_TARGETED_ACK`, #76 script reload (promote
  `SCRIPT_RELOAD_DESIGN` → HEP + impl), #77 Tier-2 dynamic callbacks
  (`engine_callback_tiers.md`), #81, #88, #89.
- **Parallel / post-MVP:** #106 HEP-0038 script-vault keystore (needs #104 +
  HEP-0040 storage); **#105 Federation / HEP-0037 — explicitly post-MVP**
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

**Parked git stashes (inventory 2026-07-18):** 5 stashes exist; 4 are MOOT —
`stash@{1}` (zap_router CMake, landed), `stash@{2}` (phase6.1 AuthContext,
superseded by the CURVE-admin design), `stash@{3}` (unified CycleOps — landed
instead into `cycle_ops.hpp`), `stash@{4}` (Gemini, obsolete `cpp/` layout).
Only **`stash@{0}`** (AUTH-2 #162 producer-side BRC ZAP-pump PeriodicTask) may
still be wanted: AUTH_TODO marks AUTH-2 shipped, but that producer-side pump is
NOT on the branch (broker ships a single per-cycle `pump_one`, not the stash's
drain-loop) — reconcile before dropping.

---

## Validation infrastructure (closes #44)

Demo framework — `share/demo_framework/runner.py` + 9 demo manifests under
`share/py-demo-*/` — is the L4 data-pipeline reference; clone + tweak for new
scenarios.  Inventory: `TESTING_TODO.md` § "Test infrastructure inventory".

---

## Active code reviews (5 — updated 2026-07-20)

- `code_review/REVIEW_FullSystem_2026-07-20.md` — **NEW, full-system HEP-vs-code
  audit** (multi-agent, 47 verified findings + cross-cutting synthesis). ❌ OPEN.
  Load-bearing: (1) `ReplayGuard` prunes on client `wall_ts` — replay defeatable on
  all 3 planes when window==skew (`replay_guard.hpp:58`, hand-verified, from #64);
  (2) four dead/no-op identity validators; (3) federation peer-DEALER ingress skips
  the admission gate chain; (4) consumer teardown leaks on failure paths;
  (5) masked HubHost lifecycle FSM suite; (6) systemic HEP↔code drift across 8+ HEPs.
  Tasks #67–#72 filed for the highest-severity items. No fixes applied yet.
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
