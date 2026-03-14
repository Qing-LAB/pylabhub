# Transient documents archived 2026-02-13

**Purpose:** These documents were merged into the standard documentation. After integration was confirmed, they were moved here per **`docs/DOC_STRUCTURE.md`** (document update / clearance: merge first, then archive).

**Do not use for current execution.** Reference only for history or to see the original analysis. For current guidance use **`docs/IMPLEMENTATION_GUIDANCE.md`**, **`docs/DATAHUB_TODO.md`**, and **`docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`**.

---

## Merge map (what was merged where)

| Archived document | Merged into | What was integrated |
|-------------------|-------------|----------------------|
| **DATAHUB_CPP_ABSTRACTION_DESIGN.md** | IMPLEMENTATION_GUIDANCE | C++ layer map (Layer 0–2), principles, transaction API as recommended default. Table and “when to use” consolidated in § C++ Abstraction Layers. |
| **DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md** | IMPLEMENTATION_GUIDANCE | Required parameters (policy, consumer_sync_policy, physical_page_size, ring_buffer_capacity) and “fail if unset” rationale; § Config validation and Pitfall 7. |
| **DATAHUB_DATABLOCK_CRITICAL_REVIEW.md** | IMPLEMENTATION_GUIDANCE | Cross-platform note (platform table); API and layering already reflected. Design review summary and gaps referenced in IMPLEMENTATION_GUIDANCE § Deferred refactoring. |
| **DATAHUB_DESIGN_DISCUSSION.md** | IMPLEMENTATION_GUIDANCE | Flexible zone “definition and agreement” semantics; integrity “lighter repair” design note. § Deferred refactoring. |
| **CODE_QUALITY_AND_REFACTORING_ANALYSIS.md** | IMPLEMENTATION_GUIDANCE | § Deferred refactoring now self-contained with completion status; full analysis remains here for reference. |
| **CODE_REVIEW_REPORT.md** | — | 2026-02-13 full review; follow-ups completed. Kept for history. |
---

## Documents in this folder

- DATAHUB_CPP_ABSTRACTION_DESIGN.md  
- DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md  
- DATAHUB_DATABLOCK_CRITICAL_REVIEW.md  
- DATAHUB_DESIGN_DISCUSSION.md  
- CODE_QUALITY_AND_REFACTORING_ANALYSIS.md  
- CODE_REVIEW_REPORT.md  

---

**Revision:** 2026-02-13 — Added CODE_QUALITY_AND_REFACTORING_ANALYSIS and CODE_REVIEW_REPORT after merge/archive.
