# Archive: transient-2026-02-27

**Archived:** 2026-02-27
**Context:** End-of-sprint documentation cleanup after src/utils/ subdirectory restructure
(P1–P8 source splits) and security/identity phase completion (Phases 1–4).

---

## Archived Documents

### `REVIEW_2026-02-26_data-hub-branch.md`

**Origin:** `docs/code_review/REVIEW_2026-02-26_data-hub-branch.md`
**Created:** 2026-02-26 | **Archived:** 2026-02-27
**Summary:** Comprehensive review of `feature/data-hub` branch covering HEP-CORE-0008 Pass 2–3
(LoopPolicy/ContextMetrics) and Phase 3 connection policy. 21 files reviewed, 5 review dimensions.

**Item disposition:**

| ID | Severity | Topic | Resolution |
|----|----------|-------|------------|
| 1.1 | HIGH | `std::_Exit()` non-portable | ✅ FALSE POSITIVE — C++11 standard |
| 1.2 | HIGH | Memory ordering in thread loops (hub_producer/consumer) | ✅ FIXED 2026-02-26 — `acq_rel` |
| 1.3 | HIGH | `start_embedded()` CAS ordering mismatch | ✅ FIXED 2026-02-26 — `acq_rel`/`relaxed` |
| 1.4 | HIGH | `getpass()` deprecated, no Windows fallback | ✅ PARTIALLY FALSE POSITIVE — guard exists; full `termios` fix deferred to Platform TODO |
| 1.5 | HIGH | File write errors unchecked | ✅ FALSE POSITIVE — `if (!f)` guards present |
| 2.1 | MED | Signal handler memory ordering | ✅ FIXED 2026-02-26 — `release`/`acquire` |
| 2.2 | MED | Thread join TOCTOU in `stop()` | ✅ FALSE POSITIVE for hub_producer/consumer |
| 2.3 | MED | Broker producer identity stale on reconnect | ⚠️ DEFERRED — known limitation, documented in MESSAGEHUB_TODO |
| 2.4 | MED | `connection_policy.hpp` → `channel_access_policy.hpp` | ✅ FIXED 2026-02-27 — file renamed; review doc updated |
| 2.5 | MED | CurveZMQ secret key not zeroed on cleanup | ⚠️ DEFERRED — tracked in SECURITY_TODO |
| 3.x | LOW | Various P3 items | ✅ FIXED or ⚠️ DEFERRED (all documented) |

**Key fixes merged to codebase before archival:** All P0/P1/P2 items resolved.
Deferred items tracked in: `MESSAGEHUB_TODO.md`, `SECURITY_TODO.md`, `PLATFORM_TODO.md`.

---

### `CODE_REVIEW.md`

**Origin:** `docs/code_review/CODE_REVIEW.md`
**Created:** 2026-02-27 | **Archived:** 2026-02-27
**Summary:** Comprehensive review of `src/utils/` after the P1–P8 subdirectory restructure.
Reviewed all modified files including data_block.cpp split, lifecycle split, messenger split,
actor_host split, and channel_access_policy (formerly connection_policy).

**Item disposition:**

| Category | Count | Disposition |
|----------|-------|-------------|
| C (Critical) | All reviewed | ✅ FALSE POSITIVE — no critical issues found |
| H (High) | All reviewed | ✅ FALSE POSITIVE — flagged issues already handled by existing code |
| M (Medium) | Multiple | ⚠️ DEFERRED — design considerations or future improvements; tracked in subtopic TODOs |

**Notable M items deferred to subtopic TODOs:**
- `messenger_internal.hpp` `inline` function bloat risk → `API_TODO.md` (future audit item)
- `using namespace internal` at file scope → accepted pattern (same as data_block*.cpp, lifecycle*.cpp)
- Potential future `data_block_config.hpp` extraction → `API_TODO.md` backlog

No ❌ OPEN items at time of archival — all items either ✅ resolved or ⚠️ documented+deferred.

---

## Open Items Migrated

All deferred (⚠️) items have been tracked in the relevant subtopic TODOs before archival.
No open items remain in these reviews that are not captured elsewhere.
