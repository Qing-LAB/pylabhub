# TODO_MASTER completions index — 2026-07-18 compression

**Purpose:** On 2026-07-18 `docs/TODO_MASTER.md` was compressed from 582 lines
toward its ≤200-line rule (`DOC_STRUCTURE.md §2.1.1`).  Completed-phase
narrative was removed from the live file.  **Nothing is lost** — the verbatim
pre-compression text lives in git at commit **`633d51c0`** (`docs/TODO_MASTER.md`,
the state right before this compression).  This index says what was extracted
and where its lasting design record now lives, so a future reader can fetch the
narrative on demand without paging the live file.

Companion to the 2026-06-27 `AUTH_TODO_completions.md` index (same method).

## What was extracted (all COMPLETED — verified against code 2026-07-18)

| Section (pre-compression) | Disposition | Lasting record |
|---|---|---|
| "Resume point" → Topology migration Phases A / B / C-step1-2 / D-phase-field | ✅ COMPLETE | Design: `DRAFT_topology_singular_side_2026-07.md` (KEPT — C7/D-R6/E–H still open) + HEP-CORE-0017 §4.7 + `README_topology_channels.md`.  Live tracker: `TOPOLOGY_TODO.md`. |
| "Resume point" → Fan-in binding-side reader correctness arc | ✅ SHIPPED 2026-07-11 | HEP-CORE-0011 §"Loop-ready gate" + HEP-CORE-0036 §4.3.4/§6.5/§6.6 + HEP-CORE-0042 §5.5.2. |
| "Resume point" → Queue-owned topology + layer cleanup arc | ✅ CLOSED 2026-07-12 (`c665de0c`,`db2bbc21`) | HEP-CORE-0036 §I9.1 + HEP-CORE-0011 Loop-ready gate.  Draft already archived `transient-2026-07-12/`. |
| "Resume point" → Line 1 auth chain (AUTH-1..7, REVIEW-A..E) | ✅ PHASE 1 PRODUCTION-READY 2026-07-17 | `AUTH_TODO.md` (active rows only) + HEP-0035/0036/0041/0042/0044.  REVIEW-D/E archived `transient-2026-07-18/`. |
| "Resume point" → Line 2 SMS consolidation (SEC-Fold-2) | ✅ SHIPPED 2026-07-07 (`5a24b410`,`ab944b55`) | HEP-CORE-0043 §0-§7,§11-§13.  Residual SEC-Fold-1b §8/§10 content-migration tracked in the compressed master's open list. |
| "Resume point" → Line 4 IAttachChannel foundation | ✅ SHIPPED 2026-07-07 | HEP-CORE-0044.  No standalone next-action. |
| "Current Sprint Focus" → Phase 0 hygiene + Phase 1 table | ✅ DONE | Phase 1 = production-ready (above).  Phases 2a/2b/3 kept in the compressed master (open). |
| "Production-readiness gap" tables (HEP-0035/0036/0042 in-flight rows) | ⚠ language stale | HEP-0035 control-plane locked + HEP-0036 D1-D3+C-chain+HEP-0040 shipped; the AUTH-1..7 data-plane close is Phase-1-done.  Kept as brief status in compressed master; detail in `AUTH_TODO.md`. |
| "Pending harness tasks" → AUTH-1..7 critical-path narrative (lines ~454-517) | ✅ Phase 1 done | `AUTH_TODO.md` critical-path table (active rows).  Genuinely-open harness tasks (#93/#95/#79/#80/#82/#85/#86/#94/#84/#87/#73/#66/#75/#76/#77/#81/#88/#89/#105/#106) retained in the compressed master. |

## Retained in the live TODO_MASTER (still open — NOT extracted)
Admin-plane CURVE (Line E, top security surface); topology C7 / D-R6 gate / Phases E–H;
Line 3 broker SHM observer C.2.c–C.5; HEP-0046 Phase B (#57); Phase 2a role-host
unification (#292); Phase 2b template RAII; Phase 3 CLI `--init` (#155); the
per-area open-work pointers; the pending harness task list (open items only);
the 4 active code reviews; federation (#105) + SDK deferred items.  (#238 and
#235 were NOT retained as open — verified SHIPPED, see correction below.)

## Subtopic-TODO extractions (same 2026-07-18 batch)

Shipped-phase narrative also extracted from the subtopic TODOs (verbatim at
commit `633d51c0`; each left a one-line pointer in place):

| File | Extracted (all ✅ SHIPPED, code-verified) | Lines |
|---|---|---|
| `API_TODO.md` | Queue-owned topology P1-P5, Loop-ready/fan-in arc, #238 log-format, #235 Python band-accessor fix (+ historical bug detail) | 660 → 437 |
| `MESSAGEHUB_TODO.md` | Queue-owned topology P1-P3, Loop-ready/fan-in arc | 587 → ~505 |
| `TESTING_TODO.md` | Queue-owned topology test coverage P1-P3 | 1199 → ~1094 |
| `TOPOLOGY_TODO.md` | Phase A (HEP amendments), Phase B (state+wire+admission), Phase B rev 1+2 findings | 572 → ~421 |
| `AUTH_TODO.md` | AUTH-5 sibling-HEP doc-sync detail | 790 → ~745 |

Lasting design records: HEP-CORE-0011 §"Loop-ready gate", HEP-CORE-0017 §4.7,
HEP-CORE-0036 §I9.1, HEP-CORE-0042 §5.5.2, `README_topology_channels.md`.

**Correction folded into TODO_MASTER this batch:** the old master listed **#238**
and **#235** as "HIGH-priority open" — verified SHIPPED against code
(`consumer_api.cpp:75` #235 anchor; `event=` format deployed for #238; both
closed 2026-06-27 per API_TODO).  The stale open-listing was removed; only
residual is #235 L3 parity regression tests (fold into #232).
