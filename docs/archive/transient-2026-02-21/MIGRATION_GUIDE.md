# Migration Guide: Old DATAHUB_TODO.md to New TODO System

**Purpose**: Guide for migrating the monolithic `DATAHUB_TODO.md` (847 lines) into the new hierarchical TODO system.

**Target**: Split into `TODO_MASTER.md` (strategic) + subtopic TODOs (detailed)

---

## Migration Strategy

### Phase 1: Preparation ✅ Complete
- [x] Create new TODO system structure
- [x] Create master TODO (TODO_MASTER.md)
- [x] Create initial subtopic TODOs:
  - MEMORY_LAYOUT_TODO.md
  - RAII_LAYER_TODO.md
  - API_TODO.md
  - TESTING_TODO.md
  - MESSAGEHUB_TODO.md
- [x] Document maintenance procedures

### Phase 2: Content Mapping (Current)

Map sections of old `DATAHUB_TODO.md` to new homes:

| Old Section | New Location | Status |
|-------------|--------------|--------|
| Recent completions (2026-02-14) | subtopic "Recent Completions" | 🔵 Review and distribute |
| Recent completions (2026-02-13) | subtopic "Recent Completions" | 🔵 Review and distribute |
| Recent completions (2026-02-12) | subtopic "Recent Completions" | 🔵 Review and distribute |
| Recent completions (2026-02-11) | Archive or subtopic | 🔵 Archive old ones |
| Recent completions (2026-02-10) | Archive | 🔵 Archive (> 1 week) |
| Remaining plan (summary) | Subtopic "Current Focus" | 🟡 Distribute |
| API and documentation | API_TODO.md | 🟢 Ready to migrate |
| Functionality and design | RAII_LAYER_TODO.md, API_TODO.md | 🟢 Ready |
| Memory layout | MEMORY_LAYOUT_TODO.md | 🟢 Ready |
| Phase 1 (schema) | API_TODO.md (schema registry) | 🟢 Ready |
| Phase 2 (verify/complete) | RAII_LAYER_TODO.md | 🟢 Ready |
| Phase 3 and later | Various subtopics | 🟢 Ready |
| Foundational APIs | TESTING_TODO.md | 🟢 Ready |
| DataHub protocol testing | TESTING_TODO.md | 🟢 Ready |
| Quick Status | TODO_MASTER.md | ✅ Already updated |
| Cross-Platform | PLATFORM_TODO.md (to create) | 🔵 Pending |
| Phases 0-7 | Archive or distribute | 🔵 Review each |
| Timeline Summary | Archive | 🔵 Historical info |
| Blockers and Risks | TODO_MASTER.md or relevant TODO | 🔵 Review |
| Notes | Relevant subtopic TODOs | 🔵 Distribute |

### Phase 3: Extraction Process

For each section to migrate:

1. **Read section in old DATAHUB_TODO.md**
2. **Identify target subtopic TODO** based on content
3. **Check if already covered** in new TODO
4. **Extract unique tasks** not yet captured
5. **Add to appropriate section** in target TODO
6. **Mark as migrated** in this guide

### Phase 4: Cleanup

After migration complete:
1. Rename `DATAHUB_TODO.md` → `DATAHUB_TODO_LEGACY.md`
2. Add notice at top: "This file is legacy. See TODO_MASTER.md"
3. After 1 sprint of validation, move to archive
4. Update all references to point to new system

---

## Section-by-Section Migration

### API and Documentation (lines 83-90)

**Target**: `API_TODO.md`

**Content**:
```markdown
- [ ] **Consumer registration to broker** – See message_hub.cpp ~378
- [ ] **stuck_duration_ms in diagnostics** – See data_block_recovery.cpp ~114
- [ ] **Config explicit-fail test** – Test validation before memory creation
- [x] **release_write_slot** – Documented ✅
- [x] **Slot handle lifetime** – Documented ✅
- [x] **Recovery error codes** – Documented ✅
- [ ] **DataBlockMutex** – Factory vs direct ctor decision
```

**Migration Status**: ✅ All captured in API_TODO.md

### Functionality and Design (lines 92-95)

**Target**: `RAII_LAYER_TODO.md`, `API_TODO.md`

**Content**:
```markdown
- [ ] **DataBlockMutex not used** – Reintegrate for control zone
- [ ] **Consumer flexible_zone_info** – Document population rules
- [ ] **Integrity repair path** – Low-level repair option
```

**Migration Status**: ✅ Captured in API_TODO.md backlog

### Memory Layout (lines 97-98)

**Target**: `MEMORY_LAYOUT_TODO.md`

**Content**:
```markdown
- [x] **Single memory structure only** – Complete ✅
```

**Migration Status**: ✅ Marked complete in MEMORY_LAYOUT_TODO.md

### Phase 1 - Schema (lines 104-106)

**Target**: `API_TODO.md`, `MESSAGEHUB_TODO.md`

**Content**:
```markdown
- [ ] **1.4 Broker schema registry** – Broker stores schema info
- [ ] **1.5 Schema versioning policy** – Compatibility rules
```

**Migration Status**: ✅ Captured in MESSAGEHUB_TODO.md

### Phase 2 - RAII (lines 108-111)

**Target**: `RAII_LAYER_TODO.md`

**Content**:
```markdown
- [x] **2.1 Slot RW C API** – Complete ✅
- [x] **2.2 Template wrappers** – Complete ✅
- [ ] **2.3 Transaction guards** – Add exception-safety tests
```

**Migration Status**: ✅ Captured in RAII_LAYER_TODO.md

### Recent Completions - Keep or Archive?

**Decision criteria**:
- Keep in subtopic "Recent Completions" if < 2 weeks old
- Archive if > 2 weeks old

**2026-02-14** (current week): Keep all in subtopics  
**2026-02-13** (1 week ago): Keep in subtopics  
**2026-02-12** (2 weeks ago): Keep major items, archive details  
**2026-02-11** (3 weeks ago): Archive to DOC_ARCHIVE_LOG.md  
**2026-02-10** (4 weeks ago): Archive to DOC_ARCHIVE_LOG.md

---

## Migration Checklist

### Content Distribution
- [ ] Extract all active tasks from DATAHUB_TODO.md
- [ ] Distribute to appropriate subtopic TODOs
- [ ] Verify no tasks lost in migration
- [ ] Check for duplicate tasks

### Recent Completions
- [ ] Keep recent completions (< 2 weeks) in subtopics
- [ ] Archive old completions (> 2 weeks) to DOC_ARCHIVE_LOG.md
- [ ] Summarize archived completions by theme

### Documentation
- [ ] Update all docs referencing DATAHUB_TODO.md
- [ ] Point to TODO_MASTER.md or specific subtopic TODO
- [ ] Update IMPLEMENTATION_GUIDANCE.md references
- [ ] Update DOC_STRUCTURE.md

### Validation
- [ ] Review TODO_MASTER.md completeness
- [ ] Verify all subtopic TODOs have recent completions
- [ ] Check cross-references between TODOs
- [ ] Ensure no broken links

### Archive Old TODO
- [ ] Rename DATAHUB_TODO.md → DATAHUB_TODO_LEGACY.md
- [ ] Add deprecation notice at top
- [ ] Move to archive after 1 sprint
- [ ] Update DOC_ARCHIVE_LOG.md

---

## Example: Migrating a Task

**From** `DATAHUB_TODO.md`:
```markdown
### Phase 2 (verify / complete)
- [ ] **2.3 Transaction guards** – WriteTransactionGuard, ReadTransactionGuard 
      already implemented; add exception-safety tests and usage guidance so 
      guards are the default entry-point.
```

**To** `RAII_LAYER_TODO.md`:
```markdown
## Current Focus

### Transaction API Refinements
- [ ] **Exception safety tests** – Comprehensive tests for exception propagation 
      through transaction lambdas (Phase 2.3 from old TODO)
- [ ] **Guard API improvements** – WriteTransactionGuard, ReadTransactionGuard 
      usability enhancements
```

**Notes**:
- Task broken into specific actions
- Context preserved (Phase 2.3)
- Grouped with related tasks
- Clear ownership (Current Focus)

---

## Timeline

**Week 1** (Current): 
- Create new TODO structure ✅
- Migrate high-priority active tasks ✅
- Update documentation references 🟡

**Week 2**: 
- Complete task migration
- Archive old completions
- Validate cross-references

**Week 3**: 
- Deprecate old DATAHUB_TODO.md
- Monitor for issues
- Adjust based on feedback

**Week 4**: 
- Move old TODO to archive
- Update DOC_ARCHIVE_LOG.md
- Migration complete

---

## Common Issues

### Issue 1: Task appears in multiple places
**Solution**: Choose primary location, add cross-reference in others

### Issue 2: Task doesn't fit any subtopic
**Solution**: Either create new subtopic or add to TODO_MASTER.md with note to create subtopic

### Issue 3: Historical context lost
**Solution**: Add note in "Recent Completions" with link to archive

### Issue 4: Cross-references broken
**Solution**: Update all references when moving tasks, use grep to find references

---

## Post-Migration

After migration complete:

1. **Monitor usage** - Are people updating new TODOs?
2. **Gather feedback** - Is structure working?
3. **Adjust as needed** - Create/merge/split TODOs based on actual usage
4. **Document lessons** - Update maintenance guide with learnings

---

## Contact

Questions about migration? Check:
- `docs/todo/README.md` - TODO system documentation
- `docs/DOC_STRUCTURE.md` - Overall documentation structure
- `docs/TODO_MASTER.md` - Start here for TODO system
