# Data Exchange Hub — Master TODO

**Scope:** strategic execution plan, current sprint focus, pointers
to subtopic detail.  Per `docs/DOC_STRUCTURE.md` §1.1 + §2.1.1:
**keep this concise (≤ 200 lines)** — detailed task tracking lives
in `docs/todo/<area>_TODO.md`.

For maintenance discipline (periodic quality checks, when to archive)
see `docs/DOC_STRUCTURE.md` §2.1.1.

---

## Current Sprint Focus

### Ultimate goal — finish hub/broker renovation, ship dual-hub-capable plh_hub

| Arc | Canonical doc | Status |
|---|---|---|
| **Arc A — `plh_hub` renovation (HUB side)** | `docs/HEP/HEP-CORE-0033-Hub-Character.md` §15 Phase 1..10 | Phases 1-9 shipped; ⏳ Phase 10 doc closure (task #73, doc hygiene only) |
| **Arc B — Role-host renovation (ROLE side)** | HEP-CORE-0011 + HEP-CORE-0023 + HEP-CORE-0033 (RoleHostFrame + presence model + dual-hub control plane).  Wave-B M0..M9 design history archived at `docs/archive/transient-2026-06-02/role_host_template_design.md`. | M0..M9 shipped.  Dual-hub plh_hub + role binaries (processor in/out on different hubs) operational. |

**Remaining production gap:** HEP-CORE-0035 / HEP-CORE-0036 auth
chain — control-plane locked (D1+D2+D3), data-plane CURVE + role-
side dispatch + sibling-HEP code sync pending.

### Production-readiness gap — CURVE + auth gating control-to-data

| Item | Status | Tracker |
|---|---|---|
| HEP-CORE-0035 auth implementation (7-phase plan in HEP-0035 §8) | 🚧 partial — control plane locked + strict-CURVE cleanup + Locked Key Memory shipped.  Phase B + #101 (§4.6 ACL) + D1 (`ChannelAccessIndex`) + D2 (broker CTRL ZAP) + D3 (notify-then-pull wire, broker_proto 5→6) shipped; C1..C5 strict-CURVE cleanup chain (#157-#161) shipped 2026-06-09; HEP-CORE-0040 Locked Key Memory chain (#165–#176 + #187) shipped 2026-06-09.  Data-plane gate close (AUTH-1..7 per `AUTH_TODO.md` — D4/D5/D6/D7 + HB-2 + HB-4+5 + HB-6) pending.  Legacy `RoleIdentityPolicy` placeholder live; task #152 retires it. | task #74; detail in `docs/todo/AUTH_TODO.md` |
| HEP-CORE-0036 authenticated connection establishment | Design final (2026-05-28; amended 2026-06-04 for notify-then-pull).  🚧 implementation in flight — D1+D2+D3 + C-chain + HEP-0040 chain shipped; AUTH-1 (D4 + D5 + B1 + B2; task #103) in-progress + AUTH-2 (HB-2; #162) + AUTH-3 (HB-4+5; #163) + AUTH-4 (HB-6 + #79) + AUTH-5 (sibling-HEP doc sync; #104) + AUTH-6 (L3 broker revival; #154) + AUTH-7 (L4 end-to-end) pending.  #105 federation parallel + non-blocking. | tasks #74, #101, #102, #103, #104, #105, #106, #162, #163, #164 |

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
  doc; rungs 2 (#221 Registration), 3 (#223 Heartbeat),
  4 (#222 ConsumerLifecycle), 7 (#224 Deregistration), 8 (#225
  ChannelNotifies), 9 (#226 RegistrationError), 11 (#228 Bands),
  12 (#229 RoleIntrospection) are pending; rungs 5 (#162), 6
  (#163), 10 (#227) blocked on AUTH chain.  Rung 13 deferred on
  back-channel-pipe infra.  Full ladder + per-rung HEP pin in
  `docs/README/README_testing.md` § "Pattern 4 — ... — Test ladder".

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
- **AUTH-4** (task **#164** + **#79**) — Broker-issued random
  `shm_secret` end-to-end + `plh_role --init` SHM secret seed (HB-6).
  Independent of AUTH-1..3 but can land in any order.
- **AUTH-5** (task **#104**) — Sibling-HEP doc sync; 7 of 8 are pure
  doc edits.  L (multi-area).
- **AUTH-6** (task **#154**, in-progress) — Re-create L3 broker tests
  against refactored lib (D6).
- **AUTH-7** — L4 end-to-end auth-gated data flow (D7).  Closes #74.
- **#94** HEP-0021 §16.5 ephemeral binding — co-lands wire-shape per
  HEP-0036 §14.1 with AUTH-1; doesn't itself gate the auth goal.

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

- `docs/code_review/REVIEW_TestAudit_2026-05-01.md` — TOP PRIORITY
  full-codebase test-correctness audit; resume bookmark.
- `docs/code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` —
  authoritative for D2 + D3 open follow-ups; tracked in API_TODO.

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
