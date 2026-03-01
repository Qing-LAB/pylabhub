# Documentation archive log

**Purpose:** Work log of document clearances: when transient documents were merged into core docs and archived. Use this to find historical content or to see what was merged where. For **how** to clean up, merge, and archive correctly, see **`docs/DOC_STRUCTURE.md`**.

---

## Archive batches

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
