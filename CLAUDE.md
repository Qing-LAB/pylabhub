# CLAUDE.md

This file defines my role in the pyLabHub C++ repository: who I work
with, how I work, and where authoritative information lives. Technical
details — build invocations, architecture, test patterns, design
contracts — live in the doc tree and source, not here. This file points
to them.

## Project context

pyLabHub C++ is a cross-platform IPC framework for scientific data
acquisition. HEP-driven design (`docs/HEP/HEP-CORE-*.md`).
Working tree: `/home/qqing/Work/pylabhub`.

## Behavioral rules

- **User messages are highest priority.** When the user sends a message
  during any in-flight task, STOP, engage with the substance, and wait
  for direction before resuming. Treat mid-operation input as a possible
  redirection of the work, not a sidebar to acknowledge and keep going.
- **Never write code without explicit user approval.** Present findings
  → discuss → get agreement → then implement. Proposing a fix does not
  equal approval to code it. "Seems good" or "go ahead" is approval;
  silence is not.
- **Refresh against the doc at the moment of starting work.** Before
  writing or modifying a test, open `docs/README/README_testing.md`
  § "Choosing a test pattern". Before writing or modifying code in a
  subsystem, open the relevant `docs/HEP/HEP-CORE-*.md` (and
  `docs/IMPLEMENTATION_GUIDANCE.md` when the change touches policy /
  core structures / error handling). Do not rely on recall — the doc
  is the source of truth.

## Work discipline

- **Never re-run a long-running command (build, ctest, lint, benchmark)
  to grep for different patterns.** Capture output once, query the
  saved file many times. Re-running wastes minutes per query.
- **Always wrap integration / layer-4 tests with a timeout.** A hang
  must not block the session.
- **Never run multiple `cmake` instances concurrently.** One at a time,
  wait for completion. Concurrent cmake invocations corrupt build
  artifacts (zero-sized object files; observed 2026-05-13).

## Authority map

| Question | Authoritative location |
|---|---|
| Build invocations / cmake flags / staging | `README.md` (repo root) + `docs/README/README_CMake_Design.md` |
| Running tests / test patterns / fixtures / antipatterns | `docs/README/README_testing.md` |
| Subsystem design contracts (lifecycle, threading, broker, schema registry, hub character, data hub, …) | `docs/HEP/HEP-CORE-*.md` — one HEP per subsystem |
| Implementation rules (test strategy, error taxonomy, core-structure change protocol, C API preservation, session hygiene) | `docs/IMPLEMENTATION_GUIDANCE.md` |
| Doc placement (where a new `.md` goes; transient vs permanent) | `docs/DOC_STRUCTURE.md` |
| Library / header / build structure | `src/include/plh_*.hpp` file-level comments; per-directory `CMakeLists.txt`; `cmake/` helpers |
| Current sprint / area status / active code reviews | `docs/TODO_MASTER.md` + `docs/todo/<area>_TODO.md` + `docs/code_review/REVIEW_<Module>_YYYY-MM-DD.md` |

## Project-specific NO-GO list

- **Do not modify** anything under `third_party/` unless explicitly instructed.
- **Do NOT delete C API tests** (slot_rw_coordinator, recovery_api). See
  `docs/IMPLEMENTATION_GUIDANCE.md` § "C API Test Preservation".
- **Core structure changes** (`SharedMemoryHeader`, `DataBlockConfig`,
  BLDS schema, etc.) require the mandatory review checklist in
  `docs/IMPLEMENTATION_GUIDANCE.md` § "Core Structure Change Protocol"
  before any modification.
- **Error handling in broker/producer/consumer** follows the two-category
  error taxonomy in `docs/IMPLEMENTATION_GUIDANCE.md`
  § "Error Taxonomy — Broker, Producer, and Consumer".
- **Documentation in `docs/` is treated as code** — update it when
  changing the design pattern or behavior it describes.
- **Code style and tidy lints** are enforced via `.clang-format` and
  `.clang-tidy`; clang-tidy warnings are treated as errors.
- Cross-reference `CMakeLists.txt` files and `cmake/` helpers **before**
  proposing build changes.

## Session hygiene (TODO bookkeeping)

Every implementation session must maintain the TODO system. Failure to do
this is what causes loss of open items across sessions.

### Before starting any task

1. `docs/TODO_MASTER.md` — current sprint and area status; lists any
   active code reviews.
2. `docs/todo/<area>_TODO.md` — detailed open items for the relevant area.
3. `docs/code_review/` — any active `REVIEW_<Module>_YYYY-MM-DD.md` with
   an ❌ OPEN status table.

### After implementing anything non-trivial, before responding "done"

1. **Update the subtopic TODO** (`docs/todo/*.md`):
   - Completed items → "Recent Completions" with date + one-line description.
   - Newly discovered items → "Current Focus" with file reference.
2. **Update `docs/TODO_MASTER.md`** status table if the area changed; update
   or remove `Active code review:` references when items resolve.
3. **Update any active code review** whose items were addressed
   (`docs/code_review/REVIEW_<Module>_YYYY-MM-DD.md` → ✅ FIXED with date).
   When all items are ✅, archive per `docs/DOC_STRUCTURE.md §2.2`.
4. **Never leave an open item only in chat history or an inline `// TODO:`
   comment** — it must be in a subtopic TODO to survive context resets.
5. **Transient documents** (design notes, plans, analyses, audits) live
   under `docs/tech_draft/` (in-progress design/impl) or
   `docs/code_review/` (in-progress reviews). Do NOT create free-floating
   `.md` files under `docs/` root. Any open item in a transient doc must
   also appear in the relevant subtopic TODO. When the transient work is
   complete + verified against code, merge lasting insight into the
   appropriate permanent doc, then archive the transient to
   `docs/archive/transient-YYYY-MM-DD/` and record in
   `docs/DOC_ARCHIVE_LOG.md`.

### Canonical subtopic TODOs

| Area | File |
|---|---|
| API / ABI / concurrency / lifecycle / RAII | `docs/todo/API_TODO.md` |
| Windows / MSVC / cross-platform / CMake | `docs/todo/PLATFORM_TODO.md` |
| Tests / coverage / new scenarios | `docs/todo/TESTING_TODO.md` |
| Memory layout / shared-memory structs | `docs/todo/MEMORY_LAYOUT_TODO.md` |
| MessageHub / broker protocol | `docs/todo/MESSAGEHUB_TODO.md` |

Full rules: `docs/IMPLEMENTATION_GUIDANCE.md` § "Session Hygiene" and
`docs/DOC_STRUCTURE.md` §1.7 (code review lifecycle).
