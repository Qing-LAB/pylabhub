# Documentation structure and usage

**Purpose:** One execution plan, one implementation guidance, topic-based reference docs. Avoid fragmented or competing documents.

---

## 1. Single execution plan

| Role | Document | Use |
|------|----------|-----|
| **Execution plan** | **`docs/DATAHUB_TODO.md`** | **THE** todo list and pathway. When tracking or executing the plan, update and follow this document only. It contains the checklist, priorities, design considerations, and next steps. Other docs (test plan, HEP, critical review) provide rationale and detail; they do **not** override priorities or the checklist here. |

All “what to do next” and “what’s done / remaining” lives in **DATAHUB_TODO.md**. Do not maintain a separate roadmap or todo in other files; reference DATAHUB_TODO from other docs, not the other way around.

---

## 2. Single implementation guidance

| Role | Document | Use |
|------|----------|-----|
| **Implementation guidance** | **`docs/IMPLEMENTATION_GUIDANCE.md`** | **THE** reference during design and implementation (architecture, patterns, pitfalls, review checklist). Use it like a “GEMINI.md”: the one place to look for how to implement correctly. Execution order and checklist remain in DATAHUB_TODO.md. |

Guidance that belongs in “how to implement” (patterns, testing approach, error handling, ABI) is merged or summarized here. Topic-specific READMEs (e.g. testing, DataHub) can summarize and link to this and to DATAHUB_TODO.

---

## 3. HEP (design specifications)

| Location | Purpose |
|----------|---------|
| **`docs/hep/`** | Authoritative design specs: HEP-CORE-0001 (lifecycle), HEP-CORE-0002 (DataHub), HEP-CORE-0003 (filelock), HEP-CORE-0004 (logger), HEP-CORE-0005 (script interface). |

- **Keep style and diagrams clear and logical.**
- **Sync with code:** Each HEP should have a short **implementation status** section (or table) that states what is implemented and what is not, with a pointer: *“For current plan and priorities, see `docs/DATAHUB_TODO.md`.”*
- For **not-yet-implemented** sections in the HEP, add a note: *“Not yet implemented; see DATAHUB_TODO.md for plan and sync.”*

---

## 4. Topic summaries (README/)

| Location | Purpose |
|----------|---------|
| **`docs/README/`** | Topic-specific summaries. Naming: **`README_<Topic>.md`**. Current: README_DataHub.md, README_testing.md, README_utils.md, README_CMake_Design.md, README_ThirdParty_CMake.md, README_Versioning.md. |

- One README per major topic; **consistent naming:** `README_<Topic>.md` (e.g. README_DataHub, README_testing, README_utils, README_CMake_Design, README_Versioning). All topic READMEs live **only** under `docs/README/`; none in `docs/hep/` or other subdirs.
- Summarize design/pattern and point to the canonical plan (DATAHUB_TODO) and implementation guidance (IMPLEMENTATION_GUIDANCE) where relevant.
- Use up-to-date information; when in doubt, prefer DATAHUB_TODO and code over stale README text.

---

## 5. Testing documentation

| Document | Role |
|----------|------|
| **`docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`** | Test plan and Phase A–D rationale; Phase D checklist. **Execution order and priorities** are defined in **DATAHUB_TODO.md**; this doc provides test rationale and detailed test descriptions. |
| Other files in `docs/testing/` | Supporting material (audit, refactoring summary, layered architecture, process sync design). Reference from the test plan or from DATAHUB_TODO; do not duplicate checklist or priorities here. |

---

## 6. Other design / analysis docs (by topic)

Keep topic-specific design or analysis in **designated files** with **consistent, recognizable names**:

| Topic | Location | Notes |
|-------|----------|--------|
| DataHub design / critical review | `docs/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md`, `docs/DATAHUB_DESIGN_DISCUSSION.md` | Referenced from DATAHUB_TODO. |
| DataHub policy & config (explicit required, single point) | `docs/DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md` | Rationale for fail-if-unset params; which params are required; reference from IMPLEMENTATION_GUIDANCE and DATAHUB_TODO. |
| DataHub C++ abstraction layer | `docs/DATAHUB_CPP_ABSTRACTION_DESIGN.md` | Layer map, patterns, alignment with C API. Merge into IMPLEMENTATION_GUIDANCE/HEP when stable. |
| Emergency / procedures | `docs/emergency_procedures.md` | Operational guidance; keep. |
| Code review | `docs/CODE_REVIEW_GUIDANCE.md` | Instructions for thorough/critical review: first pass, higher-level requirements, test integration. Draft; refine with discussion. |
| Code quality / refactoring | `docs/CODE_QUALITY_AND_REFACTORING_ANALYSIS.md` | Duplication, redundancy, obsolete code; C++20/layer design; refactor targets; naming/comments; Doxygen gaps; actionable priorities. |
| [[nodiscard]] exceptions | `docs/NODISCARD_DECISIONS.md` | Call sites that intentionally do not check a [[nodiscard]] return; rationale and discussion. |
| Name conventions | `docs/NAME_CONVENTIONS.md` | DataBlock producer/consumer display name format, suffix ` \| pid:...`, and logical_name() for comparison. |

**Archived (2026-02-12):** Spinlock/guards, flexible zone flow, FileLock test patterns, versioning/ABI detail, test pattern/CTest docs, and testing supporting material were merged into IMPLEMENTATION_GUIDANCE, README_Versioning, README_testing, or HEP, then moved to **`docs/archive/transient-2026-02-12/`**. See that folder’s README for the list.

When creating **new** organizing or guidance docs, place them in a designated file by topic and use a **consistent naming** pattern (e.g. `README_<Topic>.md` in README/, or `*_design.md` / `*_review.md` under docs/).

---

## 7. Archive

| Location | Purpose |
|----------|---------|
| **`docs/archive/`** | Superseded or historical docs (e.g. old HEP drafts, session summaries, consolidated TODOs from past dates). Do not use for current execution; reference only for history. |

---

## 8. Quick reference

- **What to do next / what’s left?** → **`docs/DATAHUB_TODO.md`**
- **How to implement / patterns / checklist?** → **`docs/IMPLEMENTATION_GUIDANCE.md`**
- **DataHub design spec?** → **`docs/hep/HEP-CORE-0002-DataHub-FINAL.md`** (and status sync with DATAHUB_TODO)
- **Test plan and Phase D detail?** → **`docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`** (priorities still in DATAHUB_TODO)
- **Topic summary (DataHub, testing, utils, CMake, versioning)?** → **`docs/README/README_DataHub.md`**, **`docs/README/README_testing.md`**, **`docs/README/README_utils.md`**, etc.
- **How to review code (first pass, design, tests)?** → **`docs/CODE_REVIEW_GUIDANCE.md`**
- **Duplication, refactoring, Doxygen, code quality?** → **`docs/CODE_QUALITY_AND_REFACTORING_ANALYSIS.md`**
- **Where do we intentionally not check [[nodiscard]] returns?** → **`docs/NODISCARD_DECISIONS.md`**
