# TODO Documents

This directory contains **subtopic TODO documents** for tracking detailed tasks, completions, and work-in-progress for specific areas of the DataHub project.

## Purpose

The subtopic TODO system keeps the master TODO (`docs/TODO_MASTER.md`) concise and high-level while providing detailed tracking for each work area. This approach:

- **Prevents TODO bloat** – the master stays strategic; detail lives in subtopics
- **Improves focus** – Each TODO covers one cohesive area
- **Enables parallel work** – Multiple people can work on different TODOs without conflicts
- **Preserves history** – **in git and `docs/archive/`, not in the TODO files**
  (`DOC_STRUCTURE.md` §2.1.1)

*Corrected 2026-08-07: the last bullet used to read "Completions stay in
subtopic TODOs", which is the opposite of §2.1.1. The first bullet claimed the
master stays "under 100 lines"; it is 423 and the number was never a real gate.*

## Structure

Each subtopic TODO follows a consistent structure:

```markdown
# [Topic] TODO

**Purpose:** One-sentence description
**Master TODO:** Link to master
**Key References:** Design docs, HEPs, etc.

## Current Focus
High-priority tasks for this sprint

## [Section 1]
Detailed tasks, organized by subsection

## Backlog
Lower-priority or future work

## Notes
Design decisions, cross-references, etc.
```

*The template used to include a `## Recent Completions` section ("keep last 2-3
sprints, then archive"). Removed 2026-08-07 — `DOC_STRUCTURE.md` §2.1.1 forbids
completion walls, and shipping the forbidden section inside the starter template
guaranteed every new file grew one.*

## Available TODO Documents

*Rebuilt 2026-08-07 from `ls docs/todo/`.  The previous version of this list
was five months stale: it named four files when seven exist, and the three it
omitted included `AUTH_TODO.md` — the security critical path, and the largest
open tracker in the directory.  An index that silently omits the top-priority
file is worse than no index.  **When adding or archiving a TODO, update this
table and the canonical table in `CLAUDE.md` in the same commit.***

| File | Area | State |
|---|---|---|
| **`AUTH_TODO.md`** | CURVE auth, vault, peer identity, the AUTH-1..7 critical path | 🟡 Open — priority band 1 |
| **`API_TODO.md`** | Public API, ABI, concurrency, lifecycle, RAII.  Also the home for memory-layout / shared-memory-struct items | 🟡 Open |
| **`MESSAGEHUB_TODO.md`** | Broker protocol, REG/wire, notify/broadcast | 🟡 Open |
| **`TESTING_TODO.md`** | Coverage gaps + the test-retirement ledger.  Design *rules* live in `README_testing.md` §1.3, not here | 🟡 Open |
| **`TOPOLOGY_TODO.md`** | Channel topology, binding sides, phase migrations | 🟡 Open |
| **`QUERY_LAYER_TODO.md`** | Hub-state query layer (HEP-0039), join patterns, type retirements | 🟡 Open |
| **`PLATFORM_TODO.md`** | Windows/MSVC, CMake, cross-platform.  Holds the clang-tidy *procedure*; results live in `code_review/LINT_FIXES_PLAN.md` | 🟡 Open |

### Archived TODO Documents

Archived to `docs/archive/transient-2026-03-02/` — all active work complete:

- `SECURITY_TODO.md` — 6 security phases complete (2026-02-28).  Note: current
  security work lives in `AUTH_TODO.md`, not here.
- `RAII_LAYER_TODO.md` — surviving backlog absorbed into TESTING_TODO + API_TODO
- `MEMORY_LAYOUT_TODO.md` — surviving items absorbed into TESTING_TODO + API_TODO.
  **`CLAUDE.md` pointed at this dead path until 2026-08-07**; memory-layout
  items now route to `API_TODO.md`.

**Legend**: 🟡 Has open items | 🟢 Backlog only

## How to Use

1. **Start with the master TODO** (`docs/TODO_MASTER.md`) to understand current priorities
2. **Navigate to relevant subtopic TODO** for detailed tasks
3. **Update as you work**:
   - Add new tasks as they emerge
   - When a task is done, **remove it** — git holds the history.  If the
     content is worth keeping, move it to `docs/archive/transient-YYYY-MM-DD/`
     and record a merge map in `docs/DOC_ARCHIVE_LOG.md`
4. **Keep it clean**:
   - Remove duplicate or obsolete tasks
   - Link to design docs rather than duplicating content

## Creating New TODO Documents

When creating a new subtopic TODO:

1. Use the template structure above
2. Link back to `docs/TODO_MASTER.md`
3. Cross-reference related TODOs
4. Keep focused on one cohesive area
5. Add it to the master TODO's subtopic list

## Maintenance

**The authoritative rule is `docs/DOC_STRUCTURE.md` §2.1.1 "Periodic TODO
quality check (MANDATORY)". Follow that; do not follow a second cadence
defined here.**

*Replaced 2026-08-07.* This section used to define its own Weekly / Monthly /
Quarterly maintenance regime, complete with a worked "Sprint 2026-02-01 to
2026-02-14" example. It conflicted with §2.1.1 on the one point that matters
most: it instructed maintainers to **"move completed tasks from Current Focus
to Recent Completions"** (ten separate mentions), while §2.1.1 states plainly
**"No 'Recent Completions' walls. No dated 'Closed' subsections piling up."**

That conflict was not theoretical — it is a plausible root cause of the
completion-wall bloat that a later pass had to trim out of these files by hand.
A process document that tells people to do the thing the structure document
forbids will win, because it is the one sitting next to the work.

The rule in one line: **a TODO file holds only open items plus the context
needed to act on them.** Completed work goes to git and, when the content is
worth preserving, to `docs/archive/transient-YYYY-MM-DD/` with a merge map in
`docs/DOC_ARCHIVE_LOG.md`. Verify completion by reading actual code — not
commit messages, not `✅` markers.

Two structural habits worth keeping from the old text, since they do not
conflict with §2.1.1:

- Reconsider a file's shape when it passes ~500 lines — split it, or check
  whether it is holding permanent guidance that belongs in a `README_*` doc.
- When an entire area completes, archive the whole file and remove it from
  the tables in this README and in `CLAUDE.md`, in the same commit.

---

## How to Update TODOs Correctly

### Adding New Tasks

**DO**:
```markdown
✅ Add tasks with clear, actionable descriptions
- [ ] **Implement feature X** – Brief explanation of what and why

✅ Include context and references
- [ ] **Fix alignment bug** – See Pitfall 6 in IMPLEMENTATION_GUIDANCE.md

✅ Use proper task hierarchy
- [ ] **Phase C tests**
  - [ ] Multi-process producer/consumer
  - [ ] Cross-platform verification

✅ Link to related work
- [ ] **Update RAII examples** – Related: TESTING_TODO.md transaction tests
```

**DON'T**:
```markdown
❌ Vague tasks without context
- [ ] Fix stuff
- [ ] Improve things

❌ Duplicate tasks across multiple TODOs
- Same task in both TESTING_TODO.md and API_TODO.md

❌ Tasks that are too large
- [ ] Implement entire DataHub system

❌ Tasks without ownership indication
- No way to know who should work on this
```

### Marking Tasks Complete

**DO**:
```markdown
✅ Verify against actual code before calling it done
   — read the source, not the commit message and not a ✅ marker

✅ Then REMOVE the item from the file
   — git is the historical record (DOC_STRUCTURE.md §2.1.1)

✅ If the content carries lasting insight, archive it with a merge map
   — docs/archive/transient-YYYY-MM-DD/ + an entry in DOC_ARCHIVE_LOG.md
   — say what moved where, and what evidence proved it complete
```

**DON'T**:
```markdown
❌ Accumulate a "Recent Completions" section
- Forbidden by DOC_STRUCTURE.md §2.1.1 — finished items blur what remains

❌ Leave completed tasks in "Current Focus"
- Clutters the active work list

❌ Mark incomplete work as done
- Be honest about actual completion state

❌ Trust a ✅ marker you did not verify
- A resolution note can be true while the code it names has moved
```

*Rewritten 2026-08-07: the DO block used to instruct exactly the pattern
§2.1.1 forbids, including a worked `### Recent Completions (2026-02-14)`
example.*

### Updating Task Status

Use clear status indicators in task descriptions:

```markdown
- [ ] **Not started** – Use plain checkbox
- [x] **Completed** – Use checked checkbox
- [ ] **In progress** 🟡 – Add indicator in description
- [ ] **Blocked** 🔴 – Note what's blocking
- [ ] **Deferred** 🔵 – Note why deferred
```

### Handling Stale Tasks

When you find a task that's no longer relevant:

**Option 1: Remove if truly obsolete**
```markdown
# Document why removed
# git commit message: "docs: remove obsolete task for feature X (superseded by Y)"
```

**Option 2: Defer if still valid but not priority**
```markdown
- [ ] **Task description** 🔵 Deferred – note WHAT would un-defer it
```

A deferral justified by a fact about the world ("no Windows CI", "waiting
on the federation design") must name that fact, so a later reader can
check whether it is still true. A deferral with no stated condition is
indistinguishable from an abandoned task.

**Option 3: Completed elsewhere → remove it, with evidence in the commit**
```markdown
# Verify against code first, then delete the item.
# git commit message: "docs: drop X from AUTH_TODO — shipped in <commit>, verified at <file:line>"
```

---

## Common Pitfalls in TODO Maintenance

*Rewritten 2026-08-07.  The previous version of this section prescribed a
weekly/monthly/quarterly regime and told maintainers to "archive old
completions monthly" and "keep Recent Completions for context" — the exact
practice `DOC_STRUCTURE.md` §2.1.1 forbids, and which the top of this same
file already said was wrong.  The pitfalls below are the ones this project
has actually hit.*

### Pitfall 1: The tracker misreports

**Problem**: The worst failure is not a long file — it is a file that says
open where the code says shipped, or shipped where the code says open. A
record is written when true, the code moves, nobody re-reads. Every
cleanup pass since March 2026 has found instances.

**Solution**: Verify by reading the source. Not the commit message, not a
`✅` marker, not this file's own summary of itself. When a note names a
*replacement* ("superseded by Y"), look up Y — several notes have named
successors that were never built.

### Pitfall 2: Duplicate and parallel tasks

**Problem**: The same work appears in two TODOs under two names, and gets
planned twice or scoped inconsistently.

**Solution**: One primary location, chosen by main concern; cross-reference
from the others. The codebase is an organic whole — extend an existing
surface rather than opening a parallel one, and the same goes for the
tracker.

### Pitfall 3: Task IDs that outlive their numbering

**Problem**: `AUTH_TODO.md` accumulated two incompatible ID spaces whose
ranges overlapped with different meanings — `#103` meant two different
things inside one file.

**Solution**: Cite live task IDs only. When a numbering scheme is
abandoned, do not build a mapping table — delete the IDs that point at
finished work (almost all of them) and re-anchor the survivors.

### Pitfall 4: Out-of-sync master TODO

**Problem**: `TODO_MASTER.md` doesn't reflect subtopic reality.

**Solution**: Update the master in the same commit as the subtopic. If an
area's status changed, the master's status table changed.

### Pitfall 5: Bloat from keeping finished work

**Problem**: Files grow past the point where anyone reads to the bottom.

**Solution**: Remove completed items — git is the history. Reconsider a
file's shape past ~500 lines: it is usually either holding closed work, or
holding permanent guidance that belongs in a `README_*` doc.

---

## Quick Reference Commands

```bash
# What changed since you last looked
git log --since="1 week ago" --oneline

# Inline TODOs that never made it into a tracker
grep -rn "TODO\|FIXME" src tests --include="*.cpp" --include="*.hpp"

# Which trackers have grown past the point of being read
wc -l docs/todo/*.md

# Candidate duplicates across trackers (needs manual review)
grep -h "^- \[ \]" docs/todo/*.md | sort | uniq -c | sort -rn
```

---

See `docs/DOC_STRUCTURE.md` for how TODOs fit into the overall documentation system.
