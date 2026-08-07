# REVIEW — Silent-failure audit: catch blocks + test assertions

| Property        | Value                                                                |
|-----------------|----------------------------------------------------------------------|
| **Status**      | 🟡 OPEN — full-codebase silent-failure sweep                         |
| **Created**     | 2026-05-01                                                           |
| **Trigger**     | Two silent-failure regressions shipped (slow-path `EXPECT_THROW`, envelope-only `status==ok`); my own freshly-written catch swallowed an exception via dead "log only" comment.  Grep heuristics are unreliable — every catch needs human read. |
| **Scope**       | All 226 catch blocks in `src/`; all `EXPECT_THROW` / outcome-only assertions in `tests/`.  No deferral. |
| **Disposition** | Transient — archive when every catch + every flagged test is verdict'd and fixed. |

---

## 1. Verdict legend

- ✅ **OK** — body does the right thing (rethrow, return error, set state with logging, intentional swallow at noexcept boundary with comment).
- ⚠ **NEEDS-COMMENT** — intentional swallow but no comment explaining why.  Add comment, no behavior change.
- 🔴 **NEEDS-LOG** — silently drops the exception; should log via `LOGGER_ERROR` so the failure surfaces.
- ❌ **NEEDS-RETHROW** — caught the wrong thing; should propagate.
- 🟢 **FIXED** — addressed in a commit (cite hash).

## 2. Catch-block findings

(populated below, file-by-file)

## 3. Test assertion findings

(populated below, file-by-file)

## 4. Followup

(populated as items resolve)
