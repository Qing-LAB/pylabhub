# Archive: transient-2026-03-06

Archived on 2026-03-06 in two batches.

## Batch 1: Old Code Review Triage + Security Fixes

Triaged three code review documents from 2026-03-03 against current source.
Fixes applied: SHM-C1, IPC-C3, SVC-C1/C2/C3, HDR-C1.
Consolidated source-of-truth review: `REVIEW_codebase_2026-03-06.md` (also archived in Batch 2 below).

| File | Original location | Reason archived |
|------|------------------|-----------------|
| `REVIEW_full-codebase_2026-03-03.md` | `docs/code_review/` | Superseded by consolidated 2026-03-06 review |
| `gemini_review_20260303_detailed.md` | `docs/code_review/` | Superseded by consolidated 2026-03-06 review |
| `gemini_review_20260303.md` | `docs/code_review/` | Superseded by consolidated 2026-03-06 review |

All findings triaged against current source (882 tests):
- All ACT-* findings: OBSOLETE (actor eliminated 2026-03-02)
- SHM-C1, IPC-C3, SVC-C1-C3, HDR-C1: FIXED
- IPC-C1, SVC-H1, ODR-SlotRWState: Already fixed in prior sessions
- Remaining findings: invalid, lifecycle-protected, or deferred as known limitations

---

## Batch 2: Closed Reviews + Deferred Design Docs

Closed all active tech_draft/ review documents. Security fixes and deferred items
tracked in `docs/TODO_MASTER.md`. Memory layout redesign (single flex zone) remains
a deferred future design — not actively in progress.

| File | Original location | Reason archived |
|------|------------------|-----------------|
| `REVIEW_codebase_2026-03-06.md` | `docs/code_review/` | Consolidated review CLOSED — all items fixed or deferred; deferred items tracked in TODO_MASTER.md |
| `REVIEW-ZmqQueue-Broadcast-2026-03-06.md` | `docs/tech_draft/` | All 22 items fixed (PC4 deferred to HEP-0023); 882/881 tests pass; 5 new ZmqQueue data-integrity tests |
| `REVIEW-ZmqVirtualChannel-Federation-2026-03-06.md` | `docs/tech_draft/` | All 6 items (D1-D6) fixed; CLOSED |
| `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` | `docs/tech_draft/` | Deferred future design (single flex zone + re-mapping protocol); not actively in progress; find when ready to implement |

### Deferred items from Batch 2 reviews (tracked in TODO_MASTER.md):

- **SHM-C2**: write_index burned on timeout — known limitation
- **IPC-H2**: BrokerService server_secret_key not zeroed — needs SecureString/zeroing dtor
- **IPC-H3**: Messenger callback TOCTOU race — documented design contract
- **PC4 / HEP-0023**: ZMQ schema not propagated through broker for cross-validation
- **Memory layout redesign**: Single flex zone + structure re-mapping protocol — see `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` in this archive
