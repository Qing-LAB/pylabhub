# Archive: transient-2026-02-21

**Archived:** 2026-02-21 (doc consolidation and HEP consistency session)

---

## Files in this archive

| File | Original location | Disposition |
|---|---|---|
| `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` | `docs/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` | Implementation complete (Phase 3, 426/426 tests). Key content already merged into `IMPLEMENTATION_GUIDANCE.md` and `HEP-CORE-0007`. |
| `DATAHUB_TODO.md` | `docs/DATAHUB_TODO.md` | Legacy monolithic TODO (last updated 2026-02-13). Superseded by new subtopic TODO system under `docs/todo/`. Any open items were migrated to subtopic TODOs at time of archiving. |
| `DESIGN_VERIFICATION_RULE.md` | `docs/DESIGN_VERIFICATION_RULE.md` | Governance rule inlined into `docs/CODE_REVIEW_GUIDANCE.md` §2 (Design Verification Rule). References to this file should now point to CODE_REVIEW_GUIDANCE. |
| `BROKER_DATABLOCK_INTEGRATION.md` | `docs/tech_draft/BROKER_DATABLOCK_INTEGRATION.md` | Stable design reference as of 2026-02-18 (Phase C complete). Integration design superseded by `HEP-CORE-0002 §6` and `HEP-CORE-0007`. |
| `CHANNEL_EXPANSION_DESIGN.md` | `docs/tech_draft/CHANNEL_EXPANSION_DESIGN.md` | Approved design (2026-02-18) for ZMQ framing and broker channel expansion. Fully implemented; design now reflected in `HEP-CORE-0002 §6.2`. |
| `DATAHUB_NAMING_CONVENTIONS.md` | `docs/DATAHUB_NAMING_CONVENTIONS.md` | Outdated: used "Source"/"Terminal" semantic roles; did not cover actor UIDs (`ACTOR-{NAME}-{8HEX}`), hub UIDs (`HUB-{NAME}-{8HEX}`), or the directory model from Security Phase 4. Current naming conventions are in `src/include/utils/uid_utils.hpp` and `docs/tech_draft/ACTOR_DESIGN.md`. |
| `code_review_utils_2025-02-21.md` | `docs/code_review/code_review_utils_2025-02-21.md` | Untracked early code review file; did not follow naming convention. Items 1–3 were already fixed; items 4–13 (open) integrated into subtopic TODOs — see merge map below. |
| `REVIEW_cpp_src_hep_2026-02-20.md` | `docs/code_review/CPP_CODE_REVIEW.md` | 2026-02-20 comprehensive review; renamed to follow convention. Items 1–2 were already fixed; items 3–5 integrated into subtopic TODOs — see merge map below. |

---

## Open item merge map (code review findings → subtopic TODOs)

### From `code_review_utils_2025-02-21.md`

| Item | Description | Merged into |
|---|---|---|
| 1 | `--validate` stub → fixed | ✅ Verified fixed 2026-02-21 |
| 2 | `--keygen` placeholder → fixed | ✅ Verified fixed 2026-02-21 |
| 3 | HEP-0005 not marked as superseded → fixed | ✅ Verified fixed 2026-02-21 |
| 4 | `role.broker` parsed but not wired | `docs/todo/MESSAGEHUB_TODO.md` §Code Review Open Items |
| 5 | AdminShell: no request body size limit | `docs/todo/MESSAGEHUB_TODO.md` §Code Review Open Items |
| 6 | Structure remap APIs public but throw at runtime | `docs/todo/MEMORY_LAYOUT_TODO.md` §Code Review Finding |
| 7 | `register_consumer` listed as stub | ✅ Fully implemented 2026-02-18 |
| 8 | ChannelPattern duplicate string conversion | `docs/todo/API_TODO.md` §Code Review Open Items |
| 9 | `PyExecResult::result_repr` never set | `docs/todo/MESSAGEHUB_TODO.md` §Code Review Open Items |
| 10 | `g_py_initialized` never read | `docs/todo/MESSAGEHUB_TODO.md` §Code Review Open Items |
| 11 | `_registered_roles()` missing on_stop/on_stop_c | `docs/todo/MESSAGEHUB_TODO.md` §Code Review Open Items |
| 12 | Schema validation shallow (numpy dtype/shapes) | `docs/todo/MESSAGEHUB_TODO.md` §Code Review Open Items |
| 13 | Duplicate actor write loop trigger logic | `docs/todo/RAII_LAYER_TODO.md` §Code Review Open Items |

### From `REVIEW_cpp_src_hep_2026-02-20.md`

| Item | Description | Merged into |
|---|---|---|
| 1 | Platform `#undef` typo | ✅ Fixed |
| 2 | HEP-0005 needs Implementation Note | ✅ Fixed |
| 3 | Logger header two comment blocks | `docs/todo/API_TODO.md` §Code Review Open Items |
| 4 | HEP-CORE-0006 `send_ctrl` was `void` not `bool` | ✅ Fixed in HEP-CORE-0006 2026-02-21 |
| 5 | Actor worker common helpers refactor | `docs/todo/MESSAGEHUB_TODO.md` §Backlog |
