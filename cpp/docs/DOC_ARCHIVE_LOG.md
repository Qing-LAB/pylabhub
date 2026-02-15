# Documentation archive log

**Purpose:** Work log of document clearances: when transient documents were merged into core docs and archived. Use this to find historical content or to see what was merged where. For **how** to clean up, merge, and archive correctly, see **`docs/DOC_STRUCTURE.md`**.

---

## Archive batches

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
| Review findings / follow-up actions from last full review | `docs/archive/transient-2026-02-13/CODE_REVIEW_REPORT.md` (2026-02-13; follow-ups done) |
| Full code quality / refactoring analysis | `docs/archive/transient-2026-02-13/CODE_QUALITY_AND_REFACTORING_ANALYSIS.md` (summary in IMPLEMENTATION_GUIDANCE § Deferred refactoring) |
| Memory layout / single flex zone design (superseded) | `docs/archive/transient-2026-02-14/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` (merged into HEP §3 and IMPLEMENTATION_GUIDANCE) |
| DataHub C++ abstraction layer (original) | `docs/archive/transient-2026-02-13/DATAHUB_CPP_ABSTRACTION_DESIGN.md` |
| DataHub policy & schema analysis (original) | `docs/archive/transient-2026-02-13/DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md` |
| DataHub critical review / design discussion (originals) | `docs/archive/transient-2026-02-13/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md`, DATAHUB_DESIGN_DISCUSSION.md |
| RAII layer draft (not merged) | `docs/archive/transient-2026-02-14/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` |
| DataHub/MessageHub test plan and MessageHub review (original) | `docs/archive/transient-2026-02-14/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md` (merged into README_testing + IMPLEMENTATION_GUIDANCE; docs/testing/ removed) |

For current documentation layout and where to put or find active content, use **`docs/DOC_STRUCTURE.md`**.
