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

| Arc | Canonical doc | Status (verified 2026-05-26) |
|---|---|---|
| **Arc A — `plh_hub` renovation (HUB side)** | `docs/HEP/HEP-CORE-0033-Hub-Character.md` §15 Phase 1..10 | ✅ Phases 1-9 shipped; ⏳ Phase 10 doc closure (task #73, doc hygiene only) |
| **Arc B — Role-host renovation (ROLE side)** | HEP-CORE-0011 + HEP-CORE-0023 + HEP-CORE-0033 (RoleHostFrame + presence model + dual-hub control plane).  Wave-B M0..M9 design history archived at `docs/archive/transient-2026-06-02/role_host_template_design.md`. | ✅ **M0..M9 shipped** (M9 closed 2026-05-26, tasks #72 + #100).  `RoleHostFrame` plain class with shared `setup_infrastructure_` / `teardown_infrastructure_` / `wire_api_for_presences_` / `build_presences_`.  Single-resolve schemas via `presences_`.  `FlexzoneInfoCache` on `RoleAPIBase` (logical + physical sizes; replaces legacy `RoleHostCore::*_fz_spec_` storage).  Q1+Q2+Q3 L2 coverage shipped (commit `53cf11be`). |

**End-state achieved.**  Dual-hub-capable plh_hub + role binaries
that register presences on multiple hubs (processor in/out on
different hubs).  M8 shipped the capability; M9 collapsed the
per-role worker_main_ duplication + retired legacy schema storage.
Remaining production gap: HEP-CORE-0035 auth (task #74).

### Production-readiness gap

| Item | Status | Tracker |
|---|---|---|
| HEP-CORE-0035 auth implementation (7-phase plan in HEP-0035 §8) | 🚧 PARTIAL — Phase B `a6b44ff8` + #101 (§4.6 ACL) + D1 `cacea477` (ChannelAccessIndex in HubState) + D2 `d18d2e91` (broker CTRL ZAP) shipped.  Legacy `RoleIdentityPolicy` placeholder still live.  Layer-2 federation trust gate + §4.7 runtime key hardening + D3-D7 (CHANNEL_AUTH_UPDATE wire + role-side dispatch + CONSUMER_REG_ACK extension) pending. | task #74; sub-steps in `docs/todo/AUTH_TODO.md` |
| HEP-CORE-0036 authenticated connection establishment | ✅ DESIGN FINAL (2026-05-28; T1+T2+T3+T5+I9+all D-tier+M-tier locked).  🚧 IMPLEMENTATION IN FLIGHT — D1+D2 shipped; D3 (`CHANNEL_AUTH_UPDATE` wire frame, broker_proto 5→6) is next.  Chain: #101 ✅ + #102 (foundations) → #74 (D1+D2 ✅; D3-D7 ⏳, subsumes HEP-CORE-0035 §4.8 known-roles-in-vault + `--add-known-role`/`--revoke-known-role`/`--list-known-roles` CLI) → #94 + #103 → #104 → #106 (HEP-CORE-0038 script-vault) → done.  #105 federation parallel + non-blocking.  **Implementation guideline:** `docs/tech_draft/DRAFT_HEP-0036-implementation-guideline_2026-05.md`. | tasks #74, #101, #102, #103, #104, #105, #106 |

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

- B3 (#78) hard-error empty `hub.auth.keyfile`.
- H43 federation propagation of role-disconnect (trigger-gated).
- Wave M2 MP4 broker-handler residual items.
- 2026-05-03 `IncomingMessage` `sender` semantics for hub events.
- Hub State Query Layer (new design;
  `docs/tech_draft/hub_state_query_layer_design.md`).
- HEP-CORE-0035 auth implementation (task #74).

### Tests / coverage (`docs/todo/TESTING_TODO.md`)

- ~~**N1 HIGH** L3 test for `setup_infrastructure_` config→opts
  translation layer~~ — ✅ shipped 2026-05-22 as L2
  `test_layer2_setup_infrastructure_translation` (6 tests × roles ×
  transports, mutation-sweep verified against the B5 + B11 bugs).
- B8 demo numpy pin via `plh_pyenv install --requirements`.
- N8+N9 bench variants (scalar dispatch + multi-size sweep).
- N11 cross-engine `on_band_message` signature parity audit.

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
- **#95** SCHEMA_REQ + METRICS_REQ — KEEP-RESERVED vs DELETE decision.  S.

**P1 — small cleanups, batch together** (S each):
- ~~**#78** B3 empty `auth.keyfile`~~ → bundled into #101 sub-phase 1D
  (2026-05-30; see planning doc §5.1.1).
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
blocker; detailed plan in
`docs/tech_draft/DRAFT_HEP-0036-implementation-guideline_2026-05.md`):
- **#101** key-file ACL discipline (M; indep of #74; ship first).
- **#102** runtime key handling (M; indep of #101 + #74).
- **#74** HEP-0035 auth impl (L; the gate).  Subsumes HEP-0035 §4.8
  known-roles-in-vault + `--add-known-role`/`--revoke-known-role`/
  `--list-known-roles` CLI.
- **#103 + #94** ZmqQueue dyn peer API + HEP-0021 §16.5 ephemeral
  binding (M-L; land together per HEP-0036 §14.1).
- **#104** Sibling-HEP code updates per HEP-0036 §14 (L).
- **#106** HEP-CORE-0038 script-vault keystore (L; after #104).
- **#105** Federation / HEP-CORE-0037 (L+; not blocking).

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
