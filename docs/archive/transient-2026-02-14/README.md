# Transient documents archived 2026-02-14

**Purpose:** These documents were merged into the standard documentation (HEP, IMPLEMENTATION_GUIDANCE, README_DataHub, DATAHUB_TODO). After integration was confirmed, they were moved here per **`docs/DOC_STRUCTURE.md`** (document update / clearance: merge first, then archive).

**Do not use for current execution.** Reference only for history. For current guidance see **`docs/DOC_STRUCTURE.md`**; for archive batches and merge history see **`docs/DOC_ARCHIVE_LOG.md`**.

---

## Merge map (what was merged where)

| Archived document | Merged into | What was integrated |
|-------------------|-------------|----------------------|
| **DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md** | HEP-CORE-0002 §3, IMPLEMENTATION_GUIDANCE | Memory layout §3.1: single layout diagram (header → control padded to 4K → data region 4K-aligned: flex zone + ring buffer). Implementation status and flex zone/config text in HEP. Dual-chain diagram and “Single flex zone” summary in IMPLEMENTATION_GUIDANCE. Re-mapping protocol (§3) remains deferred; not yet in HEP. |
| **MEMORY_AND_CPP_ABSTRACTION_CONSISTENCY.md** | IMPLEMENTATION_GUIDANCE, DATAHUB_TODO, HEP | Design compliance and API layering summary already reflected in IMPLEMENTATION_GUIDANCE (single flex zone, layers). DATAHUB_TODO “Single memory structure only” marked done. HEP implementation status bullet for memory layout. No new standalone section; consistency is now the norm. |
| **DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md** | — (not merged) | Draft described a different API (context-centric, `with_transaction<FlexZoneT, DataBlockT>`). Current implementation uses `with_write_transaction` / `with_read_transaction` and guards per IMPLEMENTATION_GUIDANCE and HEP. Kept in archive for possible future reference. |
| **DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md** | README_testing.md, IMPLEMENTATION_GUIDANCE | Test plan (Part 0, Part 1, Phase A–D, Phase D checklist, coverage, scenario matrix) merged into **`docs/README/README_testing.md`** § DataHub and MessageHub test plan. MessageHub code review (Part 2) merged into **`docs/IMPLEMENTATION_GUIDANCE.md`** § MessageHub code review. `docs/testing/` directory removed; testing docs now live only under `docs/README/`. |

---

## Documents in this folder

- DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md
- MEMORY_AND_CPP_ABSTRACTION_CONSISTENCY.md
- DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md
- DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md
