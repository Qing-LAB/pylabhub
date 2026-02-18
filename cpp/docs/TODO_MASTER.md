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

### Priority 1: Test Coverage Gaps
📍 **Status**: Ongoing
📋 **Details**: `docs/todo/TESTING_TODO.md`

Key tasks:
- Recovery scenario tests — facility layer ✅ done (384/384); broker-coordinated flow deferred (needs broker protocol)
- Messenger broker integration tests (when broker protocol is defined)

### Priority 2: Messenger Broker Protocol
📍 **Status**: Deferred (protocol not yet defined)
📋 **Details**: `docs/todo/MESSAGEHUB_TODO.md`

Key tasks:
- Define consumer registration to broker protocol
- Implement `register_consumer` (currently a stub in `messenger.cpp`)
- Messenger error paths with broker

### Priority 3: Platform / Windows Verification
📍 **Status**: Mostly done
📋 **Details**: `docs/todo/PLATFORM_TODO.md`

Key tasks (backlog only):
- `/Zc:preprocessor` PUBLIC propagation — Windows CI build verification
- MSVC warnings-as-errors gate (`/W4 /WX` in CI)

---

## Active Work Areas

| Area | Status | Detail Document | Notes |
|------|--------|----------------|-------|
| RAII Layer | ✅ Complete | `docs/todo/RAII_LAYER_TODO.md` | Phase 3 complete; all code review items resolved; 5 backlog enhancements |
| API / Primitives | 🟢 Ready | `docs/todo/API_TODO.md` | WriteAttach mode + `attach_datablock_as_writer_impl` added; timeout constants; ScopedDiagnosticHandle |
| Platform / Windows | 🟢 Mostly done | `docs/todo/PLATFORM_TODO.md` | Major pass done; 2 Windows CI items in backlog |
| Testing | 🟢 Ongoing | `docs/todo/TESTING_TODO.md` | 384/384 passing; remaining: slot-checksum repair, broker-coordinated recovery, Messenger integration |
| Memory Layout | ✅ Complete | `docs/todo/MEMORY_LAYOUT_TODO.md` | Single structure; alignment fixed |
| Schema Validation | ✅ Complete | — | BLDS schema done; dual-schema producer/consumer validation working |
| Recovery API | ✅ Complete | — | P8 recovery API done; DRAINING recovery restores COMMITTED |
| Messenger | 🟢 Core done | `docs/todo/MESSAGEHUB_TODO.md` | Renamed from MessageHub; async queue + ZMQContext module; lifecycle ownership fixed; HEP updated; broker protocol pending |

**Active code reviews:** None. All reviews resolved and archived.

Archive each review to `docs/archive/` once all its items are ✅ fixed; record in `docs/DOC_ARCHIVE_LOG.md`.

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
- **`docs/todo/MESSAGEHUB_TODO.md`** — Messenger integration, broker protocol
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
