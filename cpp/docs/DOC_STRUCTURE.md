# Documentation Structure and Categorization Guide

**Purpose:** This document serves as the **authoritative guide** for where information should be categorized, stored, and updated. It defines the documentation taxonomy and provides rules for maintaining, merging, and archiving documents. Use this as your reference whenever you need to:
- Place new content in the correct location
- Find canonical documents for a topic
- Clean up and merge transient documents
- Archive completed or superseded documents

**Note:** For historical changes and archive activities, see **`docs/DOC_ARCHIVE_LOG.md`**. This document focuses on **structure and process**, not change history.

---

## 1. Documentation Taxonomy: Where Information Lives

### 1.1 Execution plan (TODO system)

| Role | Document | Use |
|------|----------|-----|
| **Master TODO** | **`docs/TODO_MASTER.md`** | High-level execution plan and current sprint focus. References subtopic TODOs for detailed tracking. Keep this concise (< 100 lines). |
| **Subtopic TODOs** | **`docs/todo/`** | Detailed task tracking for specific areas (memory layout, RAII layer, testing, etc.). Each subtopic TODO is self-contained with current focus, backlog, and recent completions. |

**Guidelines:**
- Master TODO provides strategic overview and current priorities
- Subtopic TODOs contain detailed tasks, completions, and technical notes
- Update subtopic TODOs as you work; update master TODO when priorities shift
- Do not duplicate TODO information across documents
- See `docs/todo/README.md` for the subtopic TODO system structure

**Legacy**: `docs/DATAHUB_TODO.md` is the old monolithic TODO (847 lines). It should be refactored into the new system and archived.

### 1.2 Single implementation guidance

| Role | Document | Use |
|------|----------|-----|
| **Implementation guidance** | **`docs/IMPLEMENTATION_GUIDANCE.md`** | **THE** reference during design and implementation (architecture, patterns, pitfalls, review checklist). Execution order and checklist remain in DATAHUB_TODO.md. |
| **Code review** | **`docs/CODE_REVIEW_GUIDANCE.md`** | **THE** reference when reviewing changes: first pass (build, style, includes, [[nodiscard]], exception specs), higher-level requirements (pImpl, lifecycle, concurrency), test integration. Use for every review; do not maintain a separate review checklist elsewhere. |

Topic-specific READMEs can summarize and link to IMPLEMENTATION_GUIDANCE and DATAHUB_TODO.

### 1.3 HEP (design specifications)

| Location | Purpose |
|----------|---------|
| **`docs/HEP/`** | Authoritative design specs: HEP-CORE-0001 (lifecycle), HEP-CORE-0002 (DataHub), HEP-CORE-0003 (filelock), HEP-CORE-0004 (logger), HEP-CORE-0005 (script interface). |

- Keep style and diagrams clear and logical.
- Each HEP should have a short **implementation status** section that states what is implemented and what is not, with a pointer: *"For current plan and priorities, see `docs/DATAHUB_TODO.md`."*
- For not-yet-implemented sections, add: *"Not yet implemented; see DATAHUB_TODO.md for plan and sync."*

### 1.4 Topic summaries (README/)

| Location | Purpose |
|----------|---------|
| **`docs/README/`** | Topic-specific summaries. Naming: **`README_<Topic>.md`**. Examples: README.md (index), README_DataHub.md, README_testing.md, README_utils.md, README_CMake_Design.md, README_ThirdParty_CMake.md, README_Versioning.md. |

- One README per major topic; all topic READMEs live only under `docs/README/`.
- Summarize design/pattern and point to DATAHUB_TODO and IMPLEMENTATION_GUIDANCE where relevant.
- Prefer DATAHUB_TODO and code over stale README text.

### 1.5 Testing documentation

| Document | Role |
|----------|------|
| **`docs/README/README_testing.md`** | Test suite architecture, how to run and add tests, **and** DataHub/MessageHub test plan (Phase A–D rationale, Phase D checklist, coverage). Execution order and priorities are in DATAHUB_TODO.md. MessageHub code review for DataHub integration is in **`docs/IMPLEMENTATION_GUIDANCE.md`** § MessageHub code review. |

### 1.6 Example code (usage examples)

| Location | Purpose |
|----------|---------|
| **`cpp/examples/`** | Example code and usage documentation for the C++ stack (e.g. DataHub producer/consumer, schema, RAII layer). Not part of the default build. |

When you want to add or update **example code for usage** (standalone `.cpp` files or `.md` documentation with code snippets showing how to use the APIs), put it in **`cpp/examples/`**. This directory lives at the codebase level, not under `docs/`. 

**Guidelines:**
- Use `cpp/examples/` for runnable example code and API usage demonstrations
- Use `cpp/examples/README.md` to index all examples with brief descriptions
- Keep pure prose documentation in `docs/`; keep code-demonstrating examples in `cpp/examples/`
- Examples are not part of the default build but should be maintained to stay current with API changes

### 1.7 Transient code review (module-targeted)

| Location | Purpose |
|----------|---------|
| **`docs/code_review/`** | **Transient** review comments and reports that target a **specific module** (e.g. DataHub, MessageHub, FileLock). These are working documents for in-progress reviews, not the general review process guidance. |

**Guidelines:**
- Use this directory for in-progress, module-specific code reviews
- The general code review process is documented in **`docs/CODE_REVIEW_GUIDANCE.md`**
- **Naming convention:** `REVIEW_<Module>_YYYY-MM-DD.md`
  - `<Module>` — area being reviewed (e.g. `utils`, `datahub`, `filelock`, `lifecycle`)
  - `YYYY-MM-DD` — date the review was created
  - Example: `REVIEW_utils_2026-02-15.md`, `REVIEW_datahub_2026-03-01.md`
  - The date in the filename is the **creation date**, not the resolution date
- **Discovering active reviews:** Any `REVIEW_*.md` file present in `docs/code_review/` (not archived) is active. The master TODO (`docs/TODO_MASTER.md`) also maintains a table of active reviews for quick lookup.
- Each active review file must have a **status table at the top** listing each finding as ✅ FIXED or ❌ OPEN. This is the authoritative status at a glance.
- Each ❌ OPEN item must also be tracked in the appropriate subtopic TODO (`docs/todo/*.md`). The review file is transient; the subtopic TODO is permanent.
- Reference follow-up work in the relevant TODO documents (e.g., `docs/todo/RAII_LAYER_TODO.md`)
- After the review is completed and addressed:
  1. **Merge** relevant findings into core documents (IMPLEMENTATION_GUIDANCE, HEP, etc.)
  2. **Move** the review document to **`docs/archive/`** in a dated folder
  3. **Record** the activity in **`docs/DOC_ARCHIVE_LOG.md`**
  4. **Remove** the entry from the `TODO_MASTER.md` active reviews table
- Keep this directory clean—only active, in-progress reviews should remain here

See **`docs/code_review/README.md`** for the complete lifecycle process.

### 1.8 Tech draft (design and implementation drafts)

| Location | Purpose |
|----------|---------|
| **`docs/tech_draft/`** | **Draft** design documents and implementation notes—ideas, exploration, options, and how-to-implement sketches that are not yet finalized or canonical. |

**Guidelines:**
- Use this directory for work-in-progress design and implementation notes
- Prevents drafts from cluttering the root `docs/` directory or mixing with canonical documents (HEP, IMPLEMENTATION_GUIDANCE, README)
- Recommended naming: `DRAFT_<Topic>_YYYY-MM.md`
- When content is **agreed upon and finalized**:
  1. **Merge** the content into the appropriate canonical document (HEP, IMPLEMENTATION_GUIDANCE, DATAHUB_TODO, or README)
  2. **Move** the draft to **`docs/archive/`** in a dated folder
  3. **Record** the activity in **`docs/DOC_ARCHIVE_LOG.md`**
- Keep this directory for work in progress only

See **`docs/tech_draft/README.md`** for the complete lifecycle process.

### 1.9 Other design / analysis docs (by topic)

Keep topic-specific design or analysis in **designated files** with **consistent, recognizable names**:

| Topic | Location |
|-------|----------|
| DataHub design, critical review, policy, C++ abstraction, memory layout | **`docs/IMPLEMENTATION_GUIDANCE.md`** (and, for memory layout, **`docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`** §3). |
| Emergency procedures, naming conventions, [[nodiscard]] exceptions | **`docs/IMPLEMENTATION_GUIDANCE.md`** § Emergency Recovery, § Naming Conventions, § [[nodiscard]] Exception Sites |
| Code review process | **`docs/CODE_REVIEW_GUIDANCE.md`** |

When creating **new** organizing or guidance docs, place them in a designated file by topic and use a consistent naming pattern (e.g. `README_<Topic>.md` in README/, or `*_design.md` / `*_review.md` under docs/).

### 1.10 Active working documents (temporary)

Some documents may temporarily exist in the root `docs/` directory during active implementation work. These should be merged into canonical documents and archived once the work is complete:

| Document | Purpose | Target for merging |
|----------|---------|-------------------|
| `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` | Active design work for memory layout implementation | Merge into HEP-CORE-0002 §3 and IMPLEMENTATION_GUIDANCE when finalized |
| `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` | Active design work for RAII layer implementation | Merge into IMPLEMENTATION_GUIDANCE when finalized |

**Note:** Keep the root `docs/` directory clean. Active working documents should either move to `docs/tech_draft/` if still exploratory, or be merged into canonical documents promptly after implementation.

---

## 2. Document Lifecycle: Merging and Archiving Process

| Location | Purpose |
|----------|---------|
| **`docs/archive/`** | Superseded or historical documents (old HEP drafts, session summaries, consolidated TODOs, merged transient docs, completed code reviews). Reference only for history; do not use for current execution. |

### 2.1 Archive Process

**Principle:** Transient or one-off documents must be **merged** into canonical documents **before** archiving. Do not leave duplicate or superseded content in active documentation.

**Standard workflow:**

1. **Merge** transient document content into the appropriate canonical documents:
   - Design content → HEP or IMPLEMENTATION_GUIDANCE
   - Tasks and priorities → DATAHUB_TODO or relevant TODO
   - Topic summaries → relevant README documents
   - Test plans → README_testing.md

2. **Move** the original transient document to **`docs/archive/`**:
   - Create a dated subfolder: `archive/transient-YYYY-MM-DD/`
   - Move all documents from that batch into the subfolder
   - Add a `README.md` in the dated folder listing what was merged where (merge map)

3. **Record** the activity in **`docs/DOC_ARCHIVE_LOG.md`**:
   - Date of the archive batch
   - Summary of what was archived
   - Pointer to the archive folder and its README
   - Optionally, copy or summarize the merge map

### 2.2 Special Cases

**Code review reports:** When a module-targeted review is completed and all follow-ups are addressed, move the review document from `docs/code_review/` to archive and record in DOC_ARCHIVE_LOG. Core findings should be merged into IMPLEMENTATION_GUIDANCE or other guidance documents.

**Tech drafts:** When a design draft is finalized and merged into canonical documents (HEP, IMPLEMENTATION_GUIDANCE, etc.), move the draft from `docs/tech_draft/` to archive and record in DOC_ARCHIVE_LOG.

---

## 3. Quick Reference Guide

Use this section for rapid lookup of where to find or place specific types of information.

### 3.1 Finding Information

| Looking for... | Go to... |
|----------------|----------|
| What to do next / what's left | **`docs/TODO_MASTER.md`** (high-level) or **`docs/todo/[topic]_TODO.md`** (detailed) |
| How to implement / patterns / checklist | **`docs/IMPLEMENTATION_GUIDANCE.md`** |
| How to review code | **`docs/CODE_REVIEW_GUIDANCE.md`** |
| DataHub design spec (incl. memory layout) | **`docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`** |
| Memory layout implementation (active work) | **`docs/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md`** |
| RAII layer design (active work) | **`docs/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md`** |
| Test plan and Phase D detail | **`docs/README/README_testing.md`** |
| Topic summary (DataHub, testing, utils, CMake, versioning) | **`docs/README/README_<Topic>.md`** |
| Example code for usage (APIs, RAII, producer/consumer) | **`cpp/examples/`** |
| Emergency recovery procedures | **`docs/IMPLEMENTATION_GUIDANCE.md`** § Emergency Recovery Procedures |
| Naming conventions (DataBlock, logical_name) | **`docs/IMPLEMENTATION_GUIDANCE.md`** § Naming Conventions |
| [[nodiscard]] intentional non-checks | **`docs/IMPLEMENTATION_GUIDANCE.md`** § [[nodiscard]] Exception Sites |
| Archive history / what was merged when | **`docs/DOC_ARCHIVE_LOG.md`** |

### 3.2 Placing New Information

| Type of information | Where to put it |
|---------------------|-----------------|
| New task or priority | **`docs/TODO_MASTER.md`** (high-level) or appropriate **`docs/todo/[topic]_TODO.md`** |
| Design pattern or implementation guidance | **`docs/IMPLEMENTATION_GUIDANCE.md`** |
| Formal design specification | **`docs/HEP/`** (create new HEP document) |
| Topic summary or overview | **`docs/README/README_<Topic>.md`** |
| Module-specific code review (in progress) | **`docs/code_review/`** |
| Draft design or exploratory notes | **`docs/tech_draft/`** |
| Usage examples (code) | **`cpp/examples/`** |
| Completed work (superseded documents) | **`docs/archive/`** (with entry in DOC_ARCHIVE_LOG) |
