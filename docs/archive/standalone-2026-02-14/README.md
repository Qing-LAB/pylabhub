# Standalone Documents Archive (2026-02-14)

**Date**: February 14, 2026  
**Action**: Merged standalone guidance documents into IMPLEMENTATION_GUIDANCE.md

---

## Archived Documents

The following standalone documents have been merged into `docs/IMPLEMENTATION_GUIDANCE.md` to consolidate implementation guidance in a single location:

| Archived Document | Merged Into | New Location |
|-------------------|-------------|--------------|
| emergency_procedures.md | IMPLEMENTATION_GUIDANCE.md | § Emergency Recovery Procedures |
| NAME_CONVENTIONS.md | IMPLEMENTATION_GUIDANCE.md | § Naming Conventions |
| NODISCARD_DECISIONS.md | IMPLEMENTATION_GUIDANCE.md | § [[nodiscard]] Exception Sites |

---

## Rationale

**Problem**: Guidance scattered across multiple small files
- Hard to maintain consistency
- Easy to miss updates
- Unclear which file contains what
- Multiple places to check during implementation

**Solution**: Consolidate into IMPLEMENTATION_GUIDANCE.md
- Single reference for all implementation guidance
- Easier to search and navigate
- Better cross-referencing
- Consistent structure

---

## Content Mapping

### emergency_procedures.md → § Emergency Recovery Procedures
**Content**: 
- Recovery tools overview (datablock-admin CLI, Python API)
- Common failure scenarios (stuck writer, stuck readers, dead consumers, corruption)
- Diagnosis and recovery commands

**Changes**: 
- Reformatted for consistency with IMPLEMENTATION_GUIDANCE.md style
- Integrated with existing recovery API documentation

### NAME_CONVENTIONS.md → § Naming Conventions
**Content**:
- Display name format (`name() | pid:<pid>-<idx>`)
- Suffix marker rules (` | pid:`)
- logical_name() helper for comparison
- Usage summary

**Changes**:
- Streamlined formatting
- Added to IMPLEMENTATION_GUIDANCE.md after recovery procedures

### NODISCARD_DECISIONS.md → § [[nodiscard]] Exception Sites
**Content**:
- Test code exceptions (zombie writer, JsonConfig proxy)
- Production code exceptions (dtor release, logger sync, best-effort operations)
- Policy and summary

**Changes**:
- Reformatted as table for clarity
- Integrated with existing noexcept guidance

---

## References Updated

**Files that referenced these standalone documents**:
- ✅ `docs/DOC_STRUCTURE.md` - Updated § 1.9 and § 3.1
- ✅ `docs/IMPLEMENTATION_GUIDANCE.md` - Added new sections
- ✅ Other references in codebase - now point to IMPLEMENTATION_GUIDANCE.md

---

## Migration Notes

**Original file sizes**:
- emergency_procedures.md: 103 lines
- NAME_CONVENTIONS.md: 67 lines
- NODISCARD_DECISIONS.md: 46 lines
- **Total**: 216 lines

**After merge**:
- IMPLEMENTATION_GUIDANCE.md grew by ~200 lines (net after formatting)
- All content preserved
- Better organization and cross-referencing

**Benefits**:
- One file to reference during implementation
- Consistent formatting and style
- Easier to maintain
- Better discoverability

---

See `docs/DOC_ARCHIVE_LOG.md` for the record of this merge operation.
