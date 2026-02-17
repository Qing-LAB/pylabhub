# TODO Documents

This directory contains **subtopic TODO documents** for tracking detailed tasks, completions, and work-in-progress for specific areas of the DataHub project.

## Purpose

The subtopic TODO system keeps the master TODO (`docs/TODO_MASTER.md`) concise and high-level while providing detailed tracking for each work area. This approach:

- **Prevents TODO bloat** – Master TODO stays under 100 lines
- **Improves focus** – Each TODO covers one cohesive area
- **Enables parallel work** – Multiple people can work on different TODOs without conflicts
- **Preserves history** – Completions stay in subtopic TODOs, not cluttering the master

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

## Recent Completions
Recent work (keep last 2-3 sprints, then archive)

## Notes
Design decisions, cross-references, etc.
```

## Available TODO Documents

### Core Implementation
- **`MEMORY_LAYOUT_TODO.md`** ✅ — Memory layout redesign, alignment, validation
- **`RAII_LAYER_TODO.md`** ✅ — C++ RAII patterns, transaction API, typed access
- **`API_TODO.md`** ✅ — Public API refinements, documentation gaps

### Integration and Testing
- **`TESTING_TODO.md`** ✅ — Test phases (A-D), coverage, multi-process scenarios
- **`PLATFORM_TODO.md`** 📝 — Cross-platform consistency, platform-specific issues (to be created)

### Supporting Systems
- **`MESSAGEHUB_TODO.md`** ✅ — MessageHub integration, broker protocol
- **`RECOVERY_TODO.md`** 📝 — Recovery scenarios, diagnostics improvements (to be created)

**Legend**: ✅ Created | 📝 Planned

## How to Use

1. **Start with the master TODO** (`docs/TODO_MASTER.md`) to understand current priorities
2. **Navigate to relevant subtopic TODO** for detailed tasks
3. **Update as you work**:
   - Mark tasks complete: `- [x]`
   - Add new tasks as they emerge
   - Move completions to "Recent Completions" section
4. **Keep it clean**:
   - Archive old completions (move to `docs/DOC_ARCHIVE_LOG.md` when a full sprint is done)
   - Remove duplicate or obsolete tasks
   - Link to design docs rather than duplicating content

## Creating New TODO Documents

When creating a new subtopic TODO:

1. Use the template structure above
2. Link back to `docs/TODO_MASTER.md`
3. Cross-reference related TODOs
4. Keep focused on one cohesive area
5. Add it to the master TODO's subtopic list

## Maintenance Guidelines

### Weekly Maintenance

**Frequency**: Every Monday or at sprint start

**Actions**:
1. **Update current focus** in each relevant subtopic TODO
   - Move completed tasks from "Current Focus" to "Recent Completions"
   - Promote new tasks from "Backlog" to "Current Focus" based on sprint planning
   - Update status indicators (🔴🟡🟢✅🔵) in master TODO

2. **Review master TODO**
   - Ensure it reflects actual priorities
   - Update "Current Sprint Focus" section
   - Check that all subtopic links are valid

**Example workflow**:
```bash
# Review what was completed last week
git log --since="1 week ago" --oneline

# Update subtopic TODOs
# Mark completed: - [x]
# Add new tasks discovered during implementation
# Move items between sections as needed

# Update master TODO status indicators
# Commit changes
git add docs/TODO_MASTER.md docs/todo/
git commit -m "docs: update TODO status for week of YYYY-MM-DD"
```

### Sprint End (Every 2 weeks)

**Actions**:
1. **Clean up "Recent Completions"**
   - Keep only last 2-3 sprints worth of completions
   - Move older completions to a sprint summary section
   - Consider archiving very old completions to `DOC_ARCHIVE_LOG.md`

2. **Backlog grooming**
   - Review backlog items for relevance
   - Remove obsolete tasks
   - Re-prioritize based on project direction
   - Break down large backlog items into actionable tasks

3. **Update master TODO**
   - Review "Active Work Areas" table
   - Update status for each area
   - Adjust current sprint focus for next sprint

**Template for sprint completion notes**:
```markdown
### Sprint 2026-02-01 to 2026-02-14 Summary
- ✅ Memory layout single structure implementation
- ✅ Pitfall 10 test fixes (4 occurrences)
- ✅ FileLock barrier synchronization
- ⏭️ Deferred: RAII layer context API (moved to next sprint)
```

### Monthly Maintenance

**Frequency**: First Monday of each month

**Actions**:
1. **Archive old completions**
   - Move completions older than 2 months to `DOC_ARCHIVE_LOG.md`
   - Keep subtopic TODOs focused on recent and current work
   - Update archive with summary of major accomplishments

2. **Review all subtopic TODOs**
   - Check for duplicate tasks across TODOs
   - Merge or split TODOs if structure has changed
   - Ensure cross-references between TODOs are still valid

3. **Audit master TODO**
   - Verify all subtopic links work
   - Update project overview if scope has changed
   - Review status legend and update if needed

4. **Documentation sync**
   - Ensure `DOC_STRUCTURE.md` reflects current TODO structure
   - Update any references to TODOs in `IMPLEMENTATION_GUIDANCE.md`
   - Check that examples in documentation are still accurate

**Checklist**:
- [ ] Archive old completions (> 2 months)
- [ ] Remove duplicate tasks
- [ ] Update cross-references
- [ ] Sync with DOC_STRUCTURE.md
- [ ] Review backlog priorities
- [ ] Check for obsolete subtopic TODOs

### Quarterly Review (Every 3 months)

**Actions**:
1. **Structural improvements**
   - Evaluate if subtopic TODO organization still makes sense
   - Consider splitting large TODOs (> 500 lines)
   - Consider merging small/inactive TODOs
   - Review naming conventions

2. **Create new subtopic TODOs** if needed
   - Use template structure
   - Link from master TODO
   - Add cross-references from related TODOs

3. **Archive completed subtopic TODOs**
   - If an entire area is complete (e.g., "Schema Validation")
   - Move the TODO to `docs/archive/todo-YYYY-QQ/`
   - Update master TODO to remove archived area
   - Add entry to `DOC_ARCHIVE_LOG.md`

4. **Meta-review**
   - Is the TODO system helping or hindering?
   - Are people actually updating TODOs?
   - What improvements can be made?

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
✅ Mark complete and move to "Recent Completions"
- [x] **Config validation** – Implemented in PR #123

✅ Add completion date
### Recent Completions (2026-02-14)
- ✅ Fixed Pitfall 10 in recovery tests

✅ Include reference to where work was done
- ✅ Memory layout alignment – See data_block.cpp:245-260
```

**DON'T**:
```markdown
❌ Leave completed tasks in "Current Focus"
- Clutters the active work list

❌ Delete completed tasks without trace
- Loses historical context

❌ Mark incomplete work as done
- Be honest about actual completion state
```

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
- [ ] **Task description** 🔵 Deferred – reason: low priority, revisit Q3
```

**Option 3: Archive if completed elsewhere**
```markdown
# Move to Recent Completions with note
- ✅ **Task description** – Completed as part of PR #XYZ
```

### Archiving Old Completions

**When**: Completions older than 2 months

**Process**:
1. Create archive entry in `DOC_ARCHIVE_LOG.md`:
```markdown
### TODO Completions: 2026-01 to 2026-02

**Memory Layout**:
- Single flex zone implementation
- Alignment fixes (8-byte structured buffer)
- Layout validation updates

**Testing**:
- Phase B complete (all tests passing)
- Fixed Pitfall 10 in 4 test files
- Added barrier synchronization to FileLock tests
```

2. Remove from subtopic TODO "Recent Completions"
3. Commit with clear message

---

## Common Pitfalls in TODO Maintenance

### Pitfall 1: TODO Bloat
**Problem**: TODOs grow to 1000+ lines, become unmanageable

**Solution**:
- Split large TODOs into subtopics
- Archive old completions monthly
- Keep "Current Focus" to < 10 tasks per TODO

### Pitfall 2: Duplicate Tasks
**Problem**: Same task appears in multiple TODOs

**Solution**:
- Choose primary location based on main concern
- Add cross-reference in other TODOs
- Review for duplicates during monthly maintenance

### Pitfall 3: Orphaned Tasks
**Problem**: Task created but never updated, no owner

**Solution**:
- During sprint planning, assign owners to tasks
- Review orphaned tasks monthly
- Defer or remove tasks with no activity for 2+ months

### Pitfall 4: Out-of-Sync Master TODO
**Problem**: Master TODO doesn't reflect subtopic reality

**Solution**:
- Update master TODO whenever updating subtopics
- Weekly review of master TODO status
- Link checks during monthly maintenance

### Pitfall 5: No Context Preservation
**Problem**: Task marked done but no info on where/how

**Solution**:
- Always add reference when marking complete
- Include PR number, commit hash, or file location
- Keep "Recent Completions" for context

---

## Quick Reference Commands

```bash
# Weekly: Review changes and update TODOs
git log --since="1 week ago" --oneline
git log --since="1 week ago" --stat -- '*.cpp' '*.hpp'

# Find TODOs that need updating
grep -r "TODO\|FIXME" cpp/src cpp/tests --include="*.cpp" --include="*.hpp"

# Check subtopic TODO sizes
wc -l docs/todo/*.md

# Find duplicate tasks (requires careful manual review)
grep -h "^- \[ \]" docs/todo/*.md | sort | uniq -c | sort -rn

# Archive old completions
# Manual: move "Recent Completions" > 2 months to DOC_ARCHIVE_LOG.md
```

---

See `docs/DOC_STRUCTURE.md` for how TODOs fit into the overall documentation system.
