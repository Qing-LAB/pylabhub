# Tech draft (design and implementation drafts)

**Purpose:** This directory is for **draft** design documents and implementation notes—ideas, options, and how-to-implement sketches that are not yet part of the canonical docs. Use it so that draft material does **not** pile up in the root `docs/` directory or mix with approved HEP, IMPLEMENTATION_GUIDANCE, or README content.

**What to put here:**

- Draft design notes (e.g. "DRAFT: broker protocol extension", "DRAFT: recovery policy options")
- Implementation thoughts and alternatives (e.g. "DRAFT: typed flexible zone API")
- Exploratory or spike write-ups before they are folded into a HEP or IMPLEMENTATION_GUIDANCE

**Lifecycle:**

1. **Create** draft docs here with a clear name (e.g. `DRAFT_<Topic>_YYYY-MM.md` or `tech_draft/<topic>_draft.md`).
2. When the content is **agreed and finalized**, **merge** it into the appropriate standard document (HEP, IMPLEMENTATION_GUIDANCE, DATAHUB_TODO, or README) per **`docs/DOC_STRUCTURE.md`**.
3. **Move** the draft to **`docs/archive/`** (e.g. in a dated folder such as `archive/transient-YYYY-MM-DD/`) and **record** the activity in **`docs/DOC_ARCHIVE_LOG.md`**. Optionally delete the draft if it was fully integrated and the archive is not needed for history.

Keep **`docs/tech_draft/`** for work-in-progress drafts only. Do not use it for long-term storage; once merged, drafts leave this folder (archive or remove). For the full documentation layout and merge/archive rules, see **`docs/DOC_STRUCTURE.md`**.
