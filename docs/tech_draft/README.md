# Tech draft (design and implementation drafts)

**Purpose:** This directory is for **draft** design documents and implementation notes—ideas, options, and how-to-implement sketches that are not yet part of the canonical docs. Use it so that draft material does **not** pile up in the root `docs/` directory or mix with approved HEP, IMPLEMENTATION_GUIDANCE, or README content.

**What to put here:**

- Draft design notes (e.g. "DRAFT: broker protocol extension", "DRAFT: recovery policy options")
- Implementation thoughts and alternatives (e.g. "DRAFT: typed flexible zone API")
- Exploratory or spike write-ups before they are folded into a HEP or IMPLEMENTATION_GUIDANCE

**Lifecycle:**

1. **Create** draft docs here with a clear name (e.g. `DRAFT_<Topic>_YYYY-MM.md` or `tech_draft/<topic>_draft.md`).
2. When the content is **agreed and finalized**, **merge** it into the appropriate standard document (HEP, IMPLEMENTATION_GUIDANCE, TODO_MASTER, or README) per **`docs/DOC_STRUCTURE.md`**.
3. **Move** the draft to **`docs/archive/`** (e.g. in a dated folder such as `archive/transient-YYYY-MM-DD/`) and **record** the activity in **`docs/DOC_ARCHIVE_LOG.md`**. Optionally delete the draft if it was fully integrated and the archive is not needed for history.

Keep **`docs/tech_draft/`** for work-in-progress drafts only. Do not use it for long-term storage; once merged, drafts leave this folder (archive or remove). For the full documentation layout and merge/archive rules, see **`docs/DOC_STRUCTURE.md`**.

**Every item here must be traceable to a tracked item.** A draft with no
pointer from `TODO_MASTER.md` or a `docs/todo/*.md` file is indistinguishable
from active work when read cold, which is how a superseded plan gets executed.
Current inventory and where each is tracked (checked 2026-08-07):

| Draft | Tracked as |
|---|---|
| `DRAFT_HEP-0031-bounded-thread_2026-06.md` | task #108 |
| `SCRIPT_RELOAD_DESIGN_2026-05-20.md` | task #107 |
| `DRAFT_HEP-0036-implementation-guideline_2026-05.md` | `API_TODO.md` |
| `DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md` | `AUTH_TODO.md` |
| `raii_layer_redesign.md` | `API_TODO.md` + `TODO_MASTER.md` (band 4) |
| `engine_callback_tiers.md` | `API_TODO.md` + `TODO_MASTER.md` |
| `future-persistence-and-discovery/` | **Not tracked, and correctly so** — see below |

`future-persistence-and-discovery/` is the one deliberate exception: Python
prototypes from the original Sep 2025 design, kept as an idea bank for a future
persistence role and hub-discovery service. It carries **no open items**, so
there is nothing for a TODO to track; it is reference material, not
work-in-progress. Read its own `README.md` before treating anything in it as a
plan.
