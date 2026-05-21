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

| Arc | Canonical doc | Status (verified 2026-05-21) |
|---|---|---|
| **Arc A — `plh_hub` renovation (HUB side)** | `docs/HEP/HEP-CORE-0033-Hub-Character.md` §15 Phase 1..10 | ✅ Phases 1-9 shipped; ⏳ Phase 10 doc closure (task #73, doc hygiene only) |
| **Arc B — Role-host renovation (ROLE side)** | `docs/tech_draft/role_host_template_design.md` §14 Wave-B M0..M9 | ✅ M0..M8 shipped (M8 dual-hub processor binary-validated 2026-05-21 by demo framework); ⏳ M9 (`RoleHostFrame<HostT>` CRTP, task #72) |

**End-state:** dual-hub-capable system — fully functional `plh_hub`
binary AND role binaries that register presences on multiple hubs,
so a processor can run with input on hub-A + output on hub-B end-
to-end.  M8 ships that capability today; M9 lifts the duplicated
`worker_main_` into one CRTP template (code-quality, not new
capability).

### Production-readiness gap

| Item | Status | Tracker |
|---|---|---|
| HEP-CORE-0035 auth implementation (7-phase plan in HEP-0035 §8) | 🚧 NOT IMPLEMENTED — `RoleIdentityPolicy` is a placeholder; CURVE is mandatory but admission policy not yet implemented | task #74 |

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

- **N1 HIGH** L3 test for `setup_infrastructure_` config→opts
  translation layer (closes the systemic gap B5 + B11 came from).
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

P1 cleanups (S each): #78 B3, #79 B4, #80 B6+B7, #82 B10, #85 N3+N4.
P2 high-leverage: #83 N1 L3 setup_infrastructure_ test.
P3 small infra: #81 B8 demo numpy, #88 N8+N9 bench variants, #89 N11 sig audit.
P4 substantial: #66 S1 Phase B, #72 M9 CRTP, #73 HEP-0033 Phase 10,
#74 HEP-0035 auth, #75 HUB_TARGETED_ACK, #76 script reload, #77 Tier 2
callbacks, #84 N2 NativeEngine HubAPI extension, #86 N5+N6
native plugin docs + CMake, #87 N7+N10 three-engine doc parity.

Recommended next-session ordering — N1 first (highest leverage,
closes the bug class), then the small Priority-1 cleanups, then
N5+N6 (native plugin docs while head-cache is fresh).

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
