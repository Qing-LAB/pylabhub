# Archive Batch: transient-2026-02-17

**Date archived:** 2026-02-17
**Reason:** docs/ root cleanup — 29 non-core documents audited, content merged into canonical docs, then archived.

---

## Merge Map

| Archived document | Content merged into | Status of content |
|---|---|---|
| `API_ISSUE_NO_CONFIG_OVERLOAD.md` | `docs/todo/API_TODO.md` (2026-02-17 completion entry) | ✅ RESOLVED — dangerous overload removed; template always requires `expected_config` |
| `API_CLEANUP_COMPLETE.md` | — | ✅ PURE_ARCHIVE — API cleanup complete; outcomes in codebase |
| `API_CLEANUP_SUMMARY.md` | — | ✅ PURE_ARCHIVE — planning doc superseded by _COMPLETE |
| `API_SURFACE_DOCUMENTATION.md` | — | ✅ PURE_ARCHIVE — v1.0.0 API reference; use `data_block.hpp` comments as authoritative |
| `C_API_TEST_POLICY.md` | `docs/IMPLEMENTATION_GUIDANCE.md` § "C API Test Preservation"; `CLAUDE.md` § "Project Rules" | ✅ MERGED — mandatory rule now in core guidance |
| `CHECKSUM_ARCHITECTURE.md` | — | ✅ PURE_ARCHIVE — design reference; principle covered in IMPLEMENTATION_GUIDANCE § "Facility access" |
| `CORE_STRUCTURE_CHANGE_PROTOCOL.md` | `docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol" | ✅ MERGED — impact matrix and mandatory checklist now in core guidance |
| `DESIGN_VERIFICATION_CHECKLIST.md` | — | ✅ PURE_ARCHIVE — Phase 4 verification complete; principle already in IMPLEMENTATION_GUIDANCE |
| `FACILITY_CLASS_TEST_GAPS.md` | `docs/todo/TESTING_TODO.md` § "Coverage Gaps" | ✅ MERGED — config validation test and header structure test added as open items |
| `METRICS_API_SUMMARY.md` | — | ✅ PURE_ARCHIVE — metrics implementation guide; principle in IMPLEMENTATION_GUIDANCE § "Unified metrics" |
| `PHASE4_COMPLETION_REPORT.md` | `docs/todo/API_TODO.md` (2026-02-17 completion entry) | ✅ MERGED — resolved items noted; known limitations were already resolved |
| `PRODUCER_CONSUMER_API_CONSISTENCY.md` | `docs/todo/API_TODO.md` (2026-02-17 completion entry) | ✅ MERGED — deprecated declarations verified removed |
| `REFACTORING_PLAN_2026-02-15.md` | — | ✅ PURE_ARCHIVE — transient planning doc; outcomes in codebase |
| `SESSION_SUMMARY_2026-02-14.md` | — | ✅ PURE_ARCHIVE — historical session record |
| `SESSION_SUMMARY_PHASE3_2026-02-15.md` | `docs/todo/API_TODO.md` (obsolete code verified removed) | ✅ MERGED — obsolete code (DataBlockSlotIterator, with_next_slot) verified gone |
| `TEST_AUDIT_FINDINGS.md` | `docs/todo/TESTING_TODO.md` (Phase refactoring completion entry) | ✅ MERGED — all findings resolved; test refactoring complete |
| `TEST_AUDIT_DETAILED.md` | — | ✅ PURE_ARCHIVE — intermediate audit doc; findings captured in TEST_AUDIT_FINDINGS |
| `TEST_AUDIT_PLAN.md` | — | ✅ PURE_ARCHIVE — planning doc; plan executed |
| `TEST_ADDITIONAL_FILES.md` | — | ✅ PURE_ARCHIVE — intermediate analysis |
| `TEST_IMPLEMENTATION_ROADMAP.md` | — | ✅ PURE_ARCHIVE — superseded by TEST_REFACTOR_TODO |
| `TEST_MODERNIZATION_PLAN.md` | `docs/todo/TESTING_TODO.md` (completion entry) | ✅ MERGED — Phase 4 T4.1-T4.5 status recorded; open items added to Coverage Gaps |
| `TEST_MODERNIZATION_STATUS.md` | — | ✅ PURE_ARCHIVE — status snapshot; tests are now passing (358/358) |
| `TEST_ORGANIZATION_STRUCTURE.md` | `docs/todo/TESTING_TODO.md` (completion entry) | ✅ MERGED — Phase 5 renaming complete; target structure achieved |
| `TEST_RATIONALIZATION.md` | — | ✅ PURE_ARCHIVE — intermediate analysis doc |
| `TEST_REFACTOR_LOG.md` | — | ✅ PURE_ARCHIVE — execution log |
| `TEST_REFACTOR_PLAN_COMPLETE.md` | — | ✅ PURE_ARCHIVE — planning doc; plan executed |
| `TEST_REFACTOR_STATUS.md` | — | ✅ PURE_ARCHIVE — status snapshots; tests passing |
| `TEST_REFACTOR_TODO.md` | `docs/todo/TESTING_TODO.md` (Coverage Gaps and completions) | ✅ MERGED — all open Phase 4 items (config validation, header structure, c_api validation) added to TESTING_TODO; Phase 1-5 completion recorded |
| `V1_COMPLETE_STATUS_REPORT.md` | — | ✅ PURE_ARCHIVE — v1.0.0 release status snapshot |

---

## Core Docs Updated by This Batch

- `docs/IMPLEMENTATION_GUIDANCE.md` — Added § "C API Test Preservation" and § "Core Structure Change Protocol" (from C_API_TEST_POLICY + CORE_STRUCTURE_CHANGE_PROTOCOL); added Transient Document Rule to § Session Hygiene
- `docs/todo/TESTING_TODO.md` — Added open test items (config validation, header structure, c_api validation); recorded test refactoring completion
- `docs/todo/API_TODO.md` — Recorded resolution of API_ISSUE_NO_CONFIG_OVERLOAD, deprecated declarations removal, obsolete code removal
- `CLAUDE.md` — Added C API test preservation and core structure change protocol rules; added transient document rule
- `docs/DOC_ARCHIVE_LOG.md` — This batch recorded

---

## docs/ Root After This Batch

Only core documents remain under `docs/` root (per `docs/DOC_STRUCTURE.md`):
`TODO_MASTER.md`, `IMPLEMENTATION_GUIDANCE.md`, `CODE_REVIEW_GUIDANCE.md`,
`DATAHUB_PROTOCOL_AND_POLICY.md`, `DESIGN_VERIFICATION_RULE.md`, `DOC_ARCHIVE_LOG.md`,
`DOC_STRUCTURE.md`, `DATAHUB_TODO.md` (legacy), `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md`
(active working doc), `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` (active working doc)
