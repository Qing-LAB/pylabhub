# Documentation archive log

**Purpose:** Work log of document clearances: when transient documents were merged into core docs and archived. Use this to find historical content or to see what was merged where. For **how** to clean up, merge, and archive correctly, see **`docs/DOC_STRUCTURE.md`**.

---

## Archive batches

### 2026-03-06 Batch 2 (Closed Reviews + Deferred Design Docs)

Archived all remaining open tech_draft/ review documents after verifying all items
fixed or deferred. ZmqQueue+Broadcast review had 22 items (all fixed; PC4 deferred
to HEP-0023). ZmqVirtualChannel+Federation review had 6 items (all fixed). Memory
layout redesign (single flex zone + re-mapping) remains a deferred future design.
Deferred security items tracked in `docs/TODO_MASTER.md`. Test count: 882 (881 pass; 1 flake).

**Archived to `docs/archive/transient-2026-03-06/` (Batch 2):**

| Archived | Reason |
|---|---|
| `REVIEW_codebase_2026-03-06.md` | Consolidated review CLOSED; deferred items in TODO_MASTER.md |
| `REVIEW-ZmqQueue-Broadcast-2026-03-06.md` | All 22 items fixed; PC4 deferred to HEP-0023 |
| `REVIEW-ZmqVirtualChannel-Federation-2026-03-06.md` | All 6 items fixed; CLOSED |
| `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` | Deferred design (not actively in progress); find in archive when ready to implement |

---

### 2026-03-06 Batch 1 (Old Code Review Triage + Security Fixes)

Triaged three code review documents from 2026-03-03 against current source (882 tests).
Fixes applied: SHM-C1 (heartbeat CAS corruption), IPC-C3 (thread lambda this-capture),
SVC-C1/C2/C3 (key material not zeroed), HDR-C1 (namespace outside __cplusplus).

**Consolidated source-of-truth review**: `docs/archive/transient-2026-03-06/REVIEW_codebase_2026-03-06.md` (archived Batch 2)

**Archived to `docs/archive/transient-2026-03-06/` (Batch 1):**

| Archived | Reason |
|---|---|
| `REVIEW_full-codebase_2026-03-03.md` | Superseded by consolidated 2026-03-06 review |
| `gemini_review_20260303_detailed.md` | Superseded by consolidated 2026-03-06 review |
| `gemini_review_20260303.md` | Superseded by consolidated 2026-03-06 review |

---

### 2026-03-03 (HEP-0005 Archived + Actor Terminology Scrub)

Session 0 housekeeping for the comprehensive HEP document review plan. Two activities:

1. **HEP-CORE-0005 archived** — Script Interface Abstraction Framework superseded by
   HEP-CORE-0011 (ScriptHost Abstraction Framework with RoleHostCore + PythonRoleHostBase).
2. **Actor terminology scrub** — ~185 "actor" references replaced across 11 HEP files with
   current standalone binary terminology. The `pylabhub-actor` multi-role container was
   eliminated 2026-03-01; this pass ensures all HEP documents reflect the current architecture.

**Archived to `docs/archive/transient-2026-03-03/`:**

| Archived | Reason |
|---|---|
| `HEP-CORE-0005-script-interface-framework.md` | Superseded by HEP-CORE-0011; abstract `IScriptEngine`/`IScriptContext` replaced by `RoleHostCore` + `PythonRoleHostBase` |

**HEP files updated (actor scrub):**

| Document | Key changes |
|---|---|
| HEP-CORE-0002 | Identity fields (`producer_uid`), API layers, connection policy, embedded-mode refs |
| HEP-CORE-0006 | Implementation note rewritten for standalone binaries |
| HEP-CORE-0008 | Area, config, metrics domain, file references — all actor → binary/script host |
| HEP-CORE-0009 | Policy interaction diagram, default stack, config headers — actor → per-binary |
| HEP-CORE-0011 | Abstract, motivation, threading references |
| HEP-CORE-0013 | UID format (PROD-/CONS-/PROC-), provenance chain, vault section, code snippets |
| HEP-CORE-0015 | Area, identity, API, GIL, vault note — actor comparisons removed |
| HEP-CORE-0016 | Area, depends-on, motivation, config references |
| HEP-CORE-0017 | Processor role worker → standalone binary |
| HEP-CORE-0018 | ActorVault legacy note clarified |

**DOC_STRUCTURE.md updated:** HEP index expanded to 0001–0020; statuses refreshed for
0011, 0015, 0016, 0018, 0019, 0020.

**Remaining actor references (intentional):**
- HEP-0017 "Updated" field (historical note)
- HEP-0018 §1 Motivation (explains *why* actor was eliminated)
- HEP-0018 Supersedes table (historical cross-reference)

---

### 2026-03-02 (Completed TODO files — RAII, Memory Layout, Security)

Routine quarterly-style cleanup: three subtopic TODO files whose tracked work is fully complete
have been archived. Surviving open backlog items absorbed into remaining active TODOs.

**Archived to `docs/archive/transient-2026-03-02/`:**

| Archived | Reason | Surviving items relocated |
|---|---|---|
| `SECURITY_TODO.md` | All 6 security phases complete (2026-02-28) | None — all done |
| `RAII_LAYER_TODO.md` | RAII layer fully implemented; 3 minor backlog items survive | FlexZone example + move audit + zero-cost check → `TESTING_TODO.md` low priority |
| `MEMORY_LAYOUT_TODO.md` | Memory layout complete; layout checksum tests + stub doc | Layout tests → `TESTING_TODO.md`; stub doc → `API_TODO.md` backlog |

**Active TODO files after cleanup:** `API_TODO.md`, `TESTING_TODO.md`, `MESSAGEHUB_TODO.md`, `PLATFORM_TODO.md`

---

### 2026-03-01b (Actor Elimination — Design Revision)

Architectural decision: eliminate `pylabhub-actor` (multi-role container) in favour of
standalone `pylabhub-producer` and `pylabhub-consumer` binaries, each owning their own
directory, identity, vault, and PID lock — consistent with the existing `pylabhub-processor`
standalone model. This removes multi-broker identity ambiguity and multi-machine deployment
confusion inherent in the actor container design.

**Archived to `docs/archive/design-revision-2026-03-01/`:**

| Archived | Reason |
|---|---|
| `HEP-CORE-0010-Actor-Thread-Model-and-Unified-Script-Interface.md` | Actor eliminated. Thread model lives in HEP-CORE-0018 §7 |
| `HEP-CORE-0012-Processor-Role.md` | ProcessorRole-inside-actor eliminated. Standalone: HEP-CORE-0015 |
| `HEP-CORE-0014-Actor-Framework-Design.md` | Actor framework eliminated. Superseded by HEP-CORE-0018 |
| `REVISION_SUMMARY.md` | AI-generated transient session summary; no canonical content |

**New canonical documents:**

| Document | Content |
|---|---|
| `HEP-CORE-0018-Producer-Consumer-Binaries.md` | Full spec for `pylabhub-producer` and `pylabhub-consumer` |

**Updated canonical documents:**

| Document | Change |
|---|---|
| `HEP-CORE-0011` | Library structure, config examples, directory layouts updated for all four components; actor section removed |
| `HEP-CORE-0015` | Status updated to Phase 1 implemented; script path fixed (`script/python/__init__.py`); actor comparison removed; §1 motivation updated |
| `HEP-CORE-0017` | §6.1 binary table replaced (actor → producer + consumer); §6.2 config hierarchy updated for all four; §6.3 rewritten; cross-ref index updated |
| `DOC_STRUCTURE.md` | HEP index updated with archived/new/updated status |

See **`docs/archive/design-revision-2026-03-01/README.md`** for design decision record.

---

### 2026-03-01 (Code Review Round 2 — Complete)

Code Review Round 2 resolved. 7 confirmed bugs fixed; 11 items classified as false positives.
`ExponentialBackoff` renamed to `ThreePhaseBackoff` (Phase 3 is linear); HEP-0003/0012 doc fixes applied.

**Archived to `docs/archive/transient-2026-03-01/`:**

| Archived | Reason |
|---|---|
| `code_review/CODE_REVIEW_2026-03-01_hub-python-actor-headers.md` | All items resolved: HP-C1 HP-C2 AF-H2 ✅ FIXED; PH-C1 PH-C2 PH-C3 AF-H1 PH-H5 ❌ FALSE POSITIVE |
| `code_review/CODE_REVIEW_2026-03-01_utils-actor-hep.md` | All items resolved: NC3 NC4 NH1 NH2 ✅ FIXED; NC1 NC2 NH4 NH5 NM11 NM12 NM13 ❌ FALSE POSITIVE |

**Active review table in `TODO_MASTER.md` cleared.**

See **`docs/archive/transient-2026-03-01/README.md`** for full item disposition and fix summary.

---

### 2026-02-28 (Actor Framework HEP Promotion)

Promoted `docs/tech_draft/ACTOR_DESIGN.md` to canonical HEP status. Created
**HEP-CORE-0014** (Actor Framework Design) as the authoritative developer-facing spec
for the actor framework API. Trimmed HEP-CORE-0010 §4 and §8 to cross-references.

**Archived to `docs/archive/transient-2026-02-28/`:**

| Archived | Reason |
|---|---|
| `tech_draft/ACTOR_DESIGN.md` | Promoted to HEP-CORE-0014; §11 Gap Analysis dropped (stale) |

**New HEP:** `docs/HEP/HEP-CORE-0014-Actor-Framework-Design.md` — covers config format,
Python script interface, C++ class architecture, auth/security, schema validation, and
failure model. HEP-CORE-0010 retains threading internals; §4 and §8 now cross-reference 0014.

See **`docs/archive/transient-2026-02-28/README.md`** for full merge map.

---

### 2026-02-27 (Code Review Archive — post P1–P8 restructure + security phases)

End-of-sprint cleanup after `src/utils/` subdirectory restructure (P1–P8 source splits)
and HEP-CORE-0002 restructuring. Both active code reviews confirmed complete.

**Archived to `docs/archive/transient-2026-02-27/`:**

| Archived | Reason |
|---|---|
| `REVIEW_2026-02-26_data-hub-branch.md` | All P0/P1/P2/P3 items ✅ FIXED or ⚠️ DEFERRED (documented); 550/550 tests |
| `CODE_REVIEW.md` | All C/H items ✅ FALSE POSITIVE; M items ⚠️ DEFERRED in subtopic TODOs; no ❌ OPEN items |

**Active review table in `TODO_MASTER.md` cleared.**

See **`docs/archive/transient-2026-02-27/README.md`** for full item disposition.

---

### 2026-02-21 (Doc Consolidation + HEP Consistency Session)

Audited and consolidated todo files, code review docs, and HEP consistency.

**Archived to `docs/archive/transient-2026-02-21/`:**

| Archived | Reason |
|---|---|
| `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` | Phase 3 RAII complete; content in IMPL_GUIDANCE + HEP-0007 |
| `DATAHUB_TODO.md` | Legacy monolithic TODO; superseded by `docs/todo/` subtopic system |
| `DESIGN_VERIFICATION_RULE.md` | Content inlined into `CODE_REVIEW_GUIDANCE.md` §2 |
| `tech_draft/BROKER_DATABLOCK_INTEGRATION.md` | Stable; content in HEP-CORE-0002 §6 |
| `tech_draft/CHANNEL_EXPANSION_DESIGN.md` | Implemented; design in HEP-CORE-0002 §6.2 |
| `DATAHUB_NAMING_CONVENTIONS.md` | Outdated (old "Source/Terminal" roles); new conventions in `uid_utils.hpp` + ACTOR_DESIGN.md |
| `code_review/code_review_utils_2025-02-21.md` | Early untracked review; open items migrated to subtopic TODOs |
| `code_review/CPP_CODE_REVIEW.md` | 2026-02-20 review (renamed); open items migrated to subtopic TODOs |

**Promoted:**
- `docs/DATAHUB_PROTOCOL_AND_POLICY.md` → `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md`
  (full HEP header added; Mermaid diagrams added for state machine, protocol flows, heartbeat, DRAINING)

**Restored to `docs/tech_draft/` (prematurely archived, design not implemented):**
- `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` (structure re-mapping APIs throw at runtime)

**Key doc updates:**
- `HEP-CORE-0002`: fixed incorrect state machine transitions (COMMITTED→FREE, DRAINING→FREE both wrong); added HEP-0007 cross-references
- `HEP-CORE-0006`: corrected `send_ctrl` return type `void` → `bool`
- `CODE_REVIEW_GUIDANCE.md`: restructured — principles + pitfall reference table; removed technical detail duplication
- `MESSAGEHUB_TODO.md`: compacted 489→~150 lines; added 6 open code review items
- `API_TODO.md`, `RAII_LAYER_TODO.md`, `MEMORY_LAYOUT_TODO.md`: open code review items integrated

See **`docs/archive/transient-2026-02-21/README.md`** for full merge map and open-item disposition.

---

### 2026-02-17 (Code Review Archived — REVIEW_utils_2026-02-15.md)

All 11 items in `REVIEW_utils_2026-02-15.md` are ✅ FIXED (last items resolved 2026-02-17).
Review moved to `docs/archive/transient-2026-02-17/`. Active review table in `TODO_MASTER.md` cleared.

---

### 2026-02-17 (docs/ Root Cleanup — 29 non-core documents)

Audited all .md files directly under `docs/` root. Identified 29 non-core documents
(design notes, audit reports, session summaries, test refactoring plans, API analyses).
Content verified against codebase; key governance rules and open items merged into core docs;
all 29 archived.

**Key merges:**
- `C_API_TEST_POLICY.md` → `IMPLEMENTATION_GUIDANCE.md` § "C API Test Preservation" + `CLAUDE.md`
- `CORE_STRUCTURE_CHANGE_PROTOCOL.md` → `IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol"
- `TEST_REFACTOR_TODO.md` + test audit docs → `TESTING_TODO.md` (coverage gaps + completions)
- `API_ISSUE_NO_CONFIG_OVERLOAD.md` → `API_TODO.md` (verified resolved)
- Transient document rule → `IMPLEMENTATION_GUIDANCE.md` § Session Hygiene + `CLAUDE.md`

Moved to: **`docs/archive/transient-2026-02-17/`**
See **`docs/archive/transient-2026-02-17/README.md`** for the full merge map.

**docs/ root now contains only canonical core documents.**

---

### 2026-02-17 (code_review/ Normalization — Session/Phase Docs)

Archived 20 non-conforming session and phase documents from `docs/code_review/` that did not follow
the `REVIEW_<Module>_YYYY-MM-DD.md` naming convention. All were verified as processed/obsolete before
archiving. Key implementations confirmed in codebase: (1) `DataBlockSlotIterator`/`with_next_slot()`
removed, (2) dual schema hashes (`flexzone_schema_hash`, `datablock_schema_hash`) in `SharedMemoryHeader`,
(3) factory functions generate and validate both hashes.

Moved to: **`docs/archive/transient-2026-02-15/`**
See **`docs/archive/transient-2026-02-15/README.md`** for the full list and disposition of each file.

**`docs/code_review/` now contains only the active review:**
- `REVIEW_utils_2026-02-15.md` — 11 items open, tracked in subtopic TODOs

---

### 2026-02-14 (Standalone Documents Merge)

Merged standalone guidance documents into **`docs/IMPLEMENTATION_GUIDANCE.md`** for consolidation; originals moved to **`docs/archive/standalone-2026-02-14/`**.

| Archived document | Merged into | Notes |
|-------------------|-------------|--------|
| emergency_procedures.md | IMPLEMENTATION_GUIDANCE.md § Emergency Recovery Procedures | Recovery tools, failure scenarios, diagnosis and recovery commands |
| NAME_CONVENTIONS.md | IMPLEMENTATION_GUIDANCE.md § Naming Conventions | Display name format, logical_name() helper, suffix marker rules |
| NODISCARD_DECISIONS.md | IMPLEMENTATION_GUIDANCE.md § [[nodiscard]] Exception Sites | Test and production code that intentionally ignores [[nodiscard]] returns |

**Rationale**: Consolidate all implementation guidance in single reference document. Easier to maintain, search, and cross-reference. See **`docs/archive/standalone-2026-02-14/README.md`** for details.

### 2026-02-14 (Test Plan Cleanup)

Cleanup: Test plan document archived. Memory layout and RAII layer design documents remain active for ongoing implementation work.

| Archived document | Merged into | Notes |
|-------------------|-------------|--------|
| DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md | README_testing.md, IMPLEMENTATION_GUIDANCE | Test plan (Phase A–D, checklist, coverage) → README_testing.md § DataHub and MessageHub test plan. MessageHub review (Part 2) → IMPLEMENTATION_GUIDANCE § MessageHub code review. `docs/testing/` removed. |

**Active documents (not yet archived):**
- `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` — Key design document for ongoing memory layout implementation
- `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` — Key design document for ongoing RAII layer implementation

These documents will be merged into canonical docs (HEP, IMPLEMENTATION_GUIDANCE) and archived after implementation is complete.

---

### 2026-02-13

Transient docs merged into IMPLEMENTATION_GUIDANCE (and where noted, DATAHUB_TODO, HEP); originals moved to **`docs/archive/transient-2026-02-13/`**.

| Archived document | Merged into | Notes |
|-------------------|-------------|--------|
| DATAHUB_CPP_ABSTRACTION_DESIGN.md | IMPLEMENTATION_GUIDANCE | C++ layer map (Layer 0–2), transaction API as recommended default. |
| DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md | IMPLEMENTATION_GUIDANCE | Required parameters, config validation, Pitfall 7. |
| DATAHUB_DATABLOCK_CRITICAL_REVIEW.md | IMPLEMENTATION_GUIDANCE | Cross-platform table; design review summary and gaps in § Deferred refactoring. |
| DATAHUB_DESIGN_DISCUSSION.md | IMPLEMENTATION_GUIDANCE | Flexible zone semantics; integrity "lighter repair" note. |
| CODE_QUALITY_AND_REFACTORING_ANALYSIS.md | IMPLEMENTATION_GUIDANCE | § Deferred refactoring is the active summary; full analysis in archive. |
| CODE_REVIEW_REPORT.md | — | 2026-02-13 full review; follow-ups completed. Kept for history. |

See **`docs/archive/transient-2026-02-13/README.md`** for the full merge map.

---

### 2026-02-12

Spinlock/guards, flexible zone flow, FileLock test patterns, versioning/ABI, test pattern and CTest docs, and testing supporting material merged into IMPLEMENTATION_GUIDANCE, README_Versioning, README_testing, or HEP; originals moved to **`docs/archive/transient-2026-02-12/`**.

See **`docs/archive/transient-2026-02-12/README.md`** for the list of archived files and merge targets.

---

## Quick reference: where to find historical content

| Looking for | Location |
|-------------|----------|
| Phase 3 RAII layer implementation history | `docs/archive/transient-2026-02-15/` (20 session/phase docs) |
| Dual schema validation design (FlexZone + DataBlock) | `docs/archive/transient-2026-02-15/PHASE4_DUAL_SCHEMA_API_DESIGN.md` |
| FlexZone schema gap root cause analysis | `docs/archive/transient-2026-02-15/ROOT_CAUSE_ANALYSIS.md` |
| Phase 2 cleanup plan (DataBlockSlotIterator removal) | `docs/archive/transient-2026-02-15/POST_PHASE3_CLEANUP_PLAN.md` |
|-------------|----------|
| Review findings / follow-up actions from last full review | `docs/archive/transient-2026-02-13/CODE_REVIEW_REPORT.md` (2026-02-13; follow-ups done) |
| Full code quality / refactoring analysis | `docs/archive/transient-2026-02-13/CODE_QUALITY_AND_REFACTORING_ANALYSIS.md` (summary in IMPLEMENTATION_GUIDANCE § Deferred refactoring) |
| Memory layout / single flex zone design (superseded) | `docs/archive/transient-2026-02-14/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` (merged into HEP §3 and IMPLEMENTATION_GUIDANCE) |
| DataHub C++ abstraction layer (original) | `docs/archive/transient-2026-02-13/DATAHUB_CPP_ABSTRACTION_DESIGN.md` |
| DataHub policy & schema analysis (original) | `docs/archive/transient-2026-02-13/DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md` |
| DataHub critical review / design discussion (originals) | `docs/archive/transient-2026-02-13/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md`, DATAHUB_DESIGN_DISCUSSION.md |
| RAII layer draft (not merged) | `docs/archive/transient-2026-02-14/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` |
| DataHub/MessageHub test plan and MessageHub review (original) | `docs/archive/transient-2026-02-14/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` (merged into README_testing + IMPLEMENTATION_GUIDANCE; docs/testing/ removed) |

For current documentation layout and where to put or find active content, use **`docs/DOC_STRUCTURE.md`**.
