# Data Exchange Hub - Master TODO

**Purpose:** This is the **master execution plan** for the DataHub project. It provides a high-level overview of what needs to be done and references to detailed TODO documents for specific areas.

**Philosophy:** Keep this document concise and high-level. Detailed tasks, completion tracking, and phase-specific work belong in subtopic TODO documents.

---

## Overview

The Data Exchange Hub (DataHub) is a cross-platform IPC framework using shared memory for high-performance data transfer. Current focus is on implementing the memory layout redesign and RAII layer improvements.

**Key Documents:**
- **Design Spec**: `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`
- **Implementation Guidance**: `docs/IMPLEMENTATION_GUIDANCE.md`
- **Memory Layout Design**: `docs/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` (active)
- **RAII Layer Design**: `docs/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` (active)

---

## Current Sprint Focus

### Priority 1: Memory Layout Implementation
📍 **Status**: In Design  
📋 **Details**: `docs/todo/MEMORY_LAYOUT_TODO.md`

Key tasks:
- Finalize single memory structure design (flex zone + ring buffer)
- Implement structured buffer alignment (8-byte aligned)
- Update layout validation and checksum logic

### Priority 2: RAII Layer Improvements
📍 **Status**: In Design  
📋 **Details**: `docs/todo/RAII_LAYER_TODO.md`

Key tasks:
- Refine transaction API patterns
- Improve typed access helpers
- Update examples and documentation

### Priority 3: Testing and Validation
📍 **Status**: Ongoing  
📋 **Details**: `docs/todo/TESTING_TODO.md`

Key tasks:
- Complete Phase C (integration) tests
- Phase D (high-load, edge cases) tests
- Recovery scenario tests
- Fixed: Pitfall 10 violations (4 occurrences), FileLock barrier sync

---

## Active Work Areas

| Area | Status | Detail Document | Notes |
|------|--------|----------------|-------|
| Memory Layout | 🟡 In Design | `docs/todo/MEMORY_LAYOUT_TODO.md` | Single flex zone, alignment fixes |
| RAII Layer | 🟡 In Design | `docs/todo/RAII_LAYER_TODO.md` | Transaction API improvements |
| API Refinements | 🟢 Ready | `docs/todo/API_TODO.md` | Documentation gaps, consistency |
| Testing | 🟢 Ongoing | `docs/todo/TESTING_TODO.md` | Phase C/D coverage, test fixes |
| MessageHub | 🔵 Deferred | `docs/todo/MESSAGEHUB_TODO.md` | Broker protocol pending |
| Schema Validation | ✅ Complete | — | P9 schema validation done |
| Recovery API | ✅ Complete | — | P8 recovery API done |
| Platform Support | 🟢 Stable | (to be created) | Cross-platform consistency |

**Legend:**  
🔴 Blocked | 🟡 In Progress | 🟢 Ready | ✅ Complete | 🔵 Deferred

---

## Subtopic TODO Documents

All detailed task tracking, completions, and phase-specific work is maintained in subtopic TODO documents:

### Core Implementation
- **`docs/todo/MEMORY_LAYOUT_TODO.md`** — Memory layout redesign, alignment, validation
- **`docs/todo/RAII_LAYER_TODO.md`** — C++ RAII patterns, transaction API, typed access
- **`docs/todo/API_TODO.md`** — Public API refinements, documentation gaps

### Integration and Testing  
- **`docs/todo/TESTING_TODO.md`** — Test phases (A-D), coverage, multi-process scenarios
- **`docs/todo/PLATFORM_TODO.md`** — Cross-platform consistency, platform-specific issues (to be created)

### Supporting Systems
- **`docs/todo/MESSAGEHUB_TODO.md`** — MessageHub integration, broker protocol
- **`docs/todo/RECOVERY_TODO.md`** — Recovery scenarios, diagnostics improvements (to be created)

---

## How to Use This System

1. **Check this master TODO** for high-level status and current sprint focus
2. **Dive into subtopic TODOs** for detailed tasks and tracking
3. **Update subtopic TODOs** as you work (mark completions, add new tasks)
4. **Update this master TODO** when major milestones are reached or priorities shift
5. **Archive completed work** per `docs/DOC_STRUCTURE.md` guidelines

**Important**: Recent completions and detailed phase tracking belong in subtopic TODO documents, not here. Keep this document high-level and strategic.

### Maintenance Schedule

- **Weekly** (Monday): Update current focus, move completed tasks to "Recent Completions" in subtopic TODOs
- **Sprint End** (every 2 weeks): Clean up recent completions, groom backlog, update master status
- **Monthly** (1st Monday): Archive old completions (> 2 months), review all TODOs for duplicates
- **Quarterly** (every 3 months): Structural review, create/merge/archive subtopic TODOs

See **`docs/todo/README.md`** for detailed maintenance procedures and best practices.

---

## Quick Links

- Documentation Structure: `docs/DOC_STRUCTURE.md`
- TODO Maintenance Guide: `docs/todo/README.md`
- Implementation Guidance: `docs/IMPLEMENTATION_GUIDANCE.md`
- Code Review Process: `docs/CODE_REVIEW_GUIDANCE.md`
- Test Strategy: `docs/README/README_testing.md`
- Build Commands: `CLAUDE.md`
