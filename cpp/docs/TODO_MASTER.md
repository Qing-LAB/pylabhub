# Data Exchange Hub - Master TODO

**Purpose:** This is the **master execution plan** for the DataHub project. It provides a high-level overview of what needs to be done and references to detailed TODO documents for specific areas.

**Philosophy:** Keep this document concise and high-level. Detailed tasks, completion tracking, and phase-specific work belong in subtopic TODO documents.

---

## Overview

The Data Exchange Hub (DataHub) is a cross-platform IPC framework using shared memory for high-performance data transfer. Current focus is on Layer 4 test coverage for the standalone producer/consumer/processor binaries and Named Schema Registry integration.

**Key Documents:**
- **Design Spec**: `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`
- **Implementation Guidance**: `docs/IMPLEMENTATION_GUIDANCE.md`
- **Pipeline Architecture**: `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md`
- **Producer + Consumer Binaries**: `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md`
- **Named Schema Registry**: `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md` (Phase 1 done)
- **Policy Reference**: `docs/HEP/HEP-CORE-0009-Policy-Reference.md` (active cross-reference)

---

## Current Sprint Focus

### Priority 1: Layer 4 Producer + Consumer Tests
📍 **Status**: ✅ Complete (2026-03-02) — 26 new tests; **550/550 passing**
📋 **Details**: `docs/todo/TESTING_TODO.md` § "Layer 4: pylabhub-producer/consumer Tests"

Completed:
- [x] `tests/test_layer4_producer/` — config unit tests (8) + CLI integration tests (6)
- [x] `tests/test_layer4_consumer/` — config unit tests (6) + CLI integration tests (6)
- [ ] Integration test: producer + consumer + hubshell round-trip via live broker (deferred)

### Priority 2: Schema Registry (HEP-CORE-0016) Phases 2–5
📍 **Status**: ✅ Phase 5 complete (2026-03-02); all 5 phases done
📋 **Details**: `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md`

Completed:
- [x] Phase 1: SchemaLibrary + JSON format (2026-03-01)
- [x] Phase 2: C++ Integration — `has_schema_registry_v<T>`, `validate_named_schema<>()`,
              `ProducerOptions::schema_id`, `ConsumerOptions::expected_schema_id`; 7 tests (2026-03-02)
- [x] Phase 3: Broker protocol — `REG_REQ` schema_id/blds fields, Case A/B annotation,
              `SCHEMA_REQ/ACK`, consumer expected_schema_id validation; 7 tests (2026-03-02)
- [x] Phase 4: `SchemaStore` lifecycle singleton — thread-safe wrapper around SchemaLibrary,
              manual `reload()`, explicit `query_from_broker()`; 8 tests (2026-03-02)

- [x] Phase 5: Script integration — named schema strings in config resolve via
              `SchemaLibrary`; `resolve_schema()` dispatches string/object/null;
              `schema_entry_to_spec()` converts `SchemaFieldDef` → `FieldDef`;
              7 tests (2026-03-02)

### Priority 3: Processor Binary Tests + Phase 2 (HEP-CORE-0015)
📍 **Status**: ✅ Phase 2 complete (2026-03-03) — dual-broker + hub::Processor delegation; **750/750 passing**
📋 **Details**: `docs/HEP/HEP-CORE-0015-Processor-Binary.md`

Completed:
- [x] `tests/test_layer4_processor/` — config unit tests (10+5) + CLI integration tests (6)
- [x] Phase 2: dual-broker config (6 fields + 4 resolvers); ProcessorScriptHost delegates to hub::Processor
- [x] hub::Processor enhancements: timeout handler, pre-hook, zero-fill, critical error, iteration counter
- [x] ScriptHost dedup: RoleHostCore + PythonRoleHostBase extracted (~1600 lines deduped)

### Priority 4: Metrics Plane (HEP-CORE-0019)
📍 **Status**: ✅ Complete (2026-03-05) — 19 new tests; **828/828 passing**
📋 **Details**: `docs/todo/MESSAGEHUB_TODO.md` § "Metrics Plane (HEP-CORE-0019)"

Completed:
- [x] Phases 1–5: `report_metric()` / `snapshot_metrics_json()` API; heartbeat extension;
  `METRICS_REPORT_REQ`; `METRICS_REQ`/`METRICS_ACK`; Python bindings; `pylabhub.metrics()` AdminShell
- [x] 10 MetricsPlaneTest (protocol) + 9 MetricsApiTest (API unit, pybind11-linked)
- [x] pybind11 Default Parameter Rule codified in `IMPLEMENTATION_GUIDANCE.md`

### Priority 5 (backlog): Platform + Admin Shell Test Gaps
📍 **Status**: 🟢 Backlog only
📋 **Details**: `docs/todo/PLATFORM_TODO.md`, `docs/todo/TESTING_TODO.md`

Key tasks:
- HP-C1: `pylabhub.reset()` deadlock regression test
- HP-C2: stdout/stderr leak on exec() exception test
- BN-H1: Consumer binary ctypes round-trip test
- Clang-tidy full pass; Windows MSVC CI gaps

### Priority 5: HEP Document Review + Code Review + Source Polish
📍 **Status**: ✅ Complete (2026-03-03) — all 17 HEPs updated; code review passed; umbrella headers polished
📋 **Details**: Plan file `calm-herding-koala.md`

Completed:
- [x] Session 0: Housekeeping — archived HEP-0005, scrubbed 185 actor references across 12 HEPs
- [x] Batch 1-6: All 17 HEP documents updated with mermaid diagrams, source file references, status fields
- [x] Phase 2: 5-pass code review (L0-1, L2, L3, L4, cross-cutting) — 6 issues found and fixed
- [x] Phase 3: Source polish — doxygen fixes, umbrella header reorganization, orphan header inclusion
- [x] Example build fix — `0xBAD5ECRET` literal error in producer/consumer examples

### Backlog: C++ Templates + README Update
📍 **Status**: 🟡 Templates done (2026-03-03); README deferred
📋 **Details**: `docs/todo/API_TODO.md` § "C++ Pipeline Demo" and § "README Documentation Update"

Completed:
- [x] `examples/cpp_processor_template.cpp` — processor pipeline template (Producer → ShmQueue → Processor → Consumer)
- [x] `examples/CMakeLists.txt` + `PYLABHUB_BUILD_EXAMPLES` opt-in CMake flag
- [x] ZMQ wire format documentation (HEP-CORE-0002 §7.1)

Pending:
- [ ] Application-oriented README update: API layers, four binaries, config model, C++ vs Python paths, getting started guide

---

## Active Work Areas

| Area | Status | Detail Document | Notes |
|------|--------|----------------|-------|
| Security / Identity / Provenance | ✅ Complete | `docs/archive/transient-2026-03-02/SECURITY_TODO.md` | All 6 phases complete (2026-02-28). TODO archived. |
| Actor (pylabhub-actor) | ❌ Eliminated | — | **Eliminated (2026-03-01).** `src/actor/` + `tests/test_layer4_actor/` deleted. HEP-0010/0012/0014 archived. Replaced by three standalone binaries. |
| Producer Binary (`pylabhub-producer`) | 🟡 Phase 1+tests | `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` | **Phase 1 done (2026-03-01); Layer 4 tests done (2026-03-02):** 8 config + 6 CLI tests. **Pending Phase 2:** integration test (live broker round-trip). |
| Consumer Binary (`pylabhub-consumer`) | 🟡 Phase 1+tests | `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` | **Phase 1 done (2026-03-01); Layer 4 tests done (2026-03-02):** 6 config + 6 CLI tests. **Pending Phase 2:** integration test (live broker round-trip). |
| HubShell / HubConfig | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | All 6 phases done (2026-02-20): HubConfig, Python env, broker consolidation, PythonInterpreter, AdminShell, hubshell.cpp rewrite |
| RAII Layer | ✅ Complete | `docs/archive/transient-2026-03-02/RAII_LAYER_TODO.md` | Phase 3 complete; all code review items resolved. TODO archived; minor backlog absorbed into TESTING_TODO. |
| API / Primitives | 🟢 Ready | `docs/todo/API_TODO.md` | WriteAttach mode + `attach_datablock_as_writer_impl` added; timeout constants; ScopedDiagnosticHandle; **header layering refactor Phase A complete (2026-02-26)**; **P2 src/ split done (2026-02-27)**: `data_block.cpp` 3969L→2894L via `data_block_internal.hpp` + 3 new split files; **HEP-CORE-0002 restructured (2026-02-27)**: §6 RAII Abstraction Layer added, §7 Control Plane Protocol stub (→HEP-CORE-0007), stale §5.3/§5.4/§5.5 removed, §6-§15→§7-§16; **P4 messenger.cpp split done (2026-02-27)**: `messenger_internal.hpp` + `messenger_protocol.cpp`; `messenger.cpp` 1707L→811L |
| Platform / Windows | 🟢 Mostly done | `docs/todo/PLATFORM_TODO.md` | Major pass done; 2 Windows CI items in backlog |
| Testing | 🟡 Ongoing | `docs/todo/TESTING_TODO.md` | **809/809 passing** (2026-03-04). Two-tier shutdown protocol + ZmqPollLoop + protocol gap closure. Active gaps: integration test (producer+consumer+hubshell), HP-C1/HP-C2/BN-H1. |
| Memory Layout | ✅ Complete | `docs/archive/transient-2026-03-02/MEMORY_LAYOUT_TODO.md` | Single structure; alignment fixed; sub-4K slots. TODO archived; minor test backlog absorbed into TESTING_TODO. |
| Schema Validation | ✅ Complete | — | BLDS schema done; dual-schema producer/consumer validation working |
| Named Schema Registry | ✅ Complete | `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md` | All 5 phases done (2026-03-02). Script host helpers deduplicated into shared headers. |
| Processor Binary | ✅ Phase 2 done | `docs/HEP/HEP-CORE-0015-Processor-Binary.md` | **Phase 1+2 done (2026-03-03):** 15 config + 6 CLI tests. Dual-broker config; ProcessorScriptHost delegates to hub::Processor; ScriptHost dedup. |
| Pipeline Architecture | ✅ Design | `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` | Design complete (2026-03-01). Implements four planes, four standalone binaries, topology patterns. |
| Metrics Plane | 🟡 Design | `docs/HEP/HEP-CORE-0019-Metrics-Plane.md` | Design drafted (2026-03-02). Fifth plane: passive SHM + voluntary ZMQ → broker aggregation. 5 phases. |
| Interactive Signal Handler | ✅ Complete | `docs/HEP/HEP-CORE-0020-Interactive-Signal-Handler.md` | **Implemented (2026-03-02).** All 4 binaries integrated. Old signal handlers removed. 705/705 pass. |
| Recovery API | ✅ Complete | — | P8 recovery API done; DRAINING recovery restores COMMITTED |
| Messenger / Broker | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | Two-tier shutdown (CHANNEL_CLOSING_NOTIFY + FORCE_SHUTDOWN); Cat 1/Cat 2 health; event handlers; CHANNEL_NOTIFY_REQ relay; HEP-0007 §12 |

**Active code reviews:** None. (2026-02-27 reviews archived to `docs/archive/transient-2026-02-27/`; 2026-03-01 Round 2 reviews archived to `docs/archive/transient-2026-03-01/`; SECURITY/RAII/MEMORY_LAYOUT TODOs archived to `docs/archive/transient-2026-03-02/`; all logged in `docs/DOC_ARCHIVE_LOG.md`.)

**Legend:**  
🔴 Blocked | 🟡 In Progress | 🟢 Ready | ✅ Complete | 🔵 Deferred

---

## Subtopic TODO Documents

All detailed task tracking, completions, and phase-specific work is maintained in subtopic TODO documents.
See `docs/todo/README.md` for full list and archiving history.

### Active (have open items)
- **`docs/todo/API_TODO.md`** — Producer/consumer/processor binary work (Steps 4–5), header layering, API backlog
- **`docs/todo/TESTING_TODO.md`** — Layer 4 producer/consumer tests (pending), HP-C1/HP-C2/BN-H1, platform
- **`docs/todo/PLATFORM_TODO.md`** — Clang-tidy pass, Windows MSVC CI gaps (backlog)
- **`docs/todo/MESSAGEHUB_TODO.md`** — Broker feature backlog, schema registry (deferred)

### Archived (complete — no active items)
- `SECURITY_TODO.md` → `docs/archive/transient-2026-03-02/` (all 6 phases done 2026-02-28)
- `RAII_LAYER_TODO.md` → `docs/archive/transient-2026-03-02/` (RAII layer complete)
- `MEMORY_LAYOUT_TODO.md` → `docs/archive/transient-2026-03-02/` (memory layout complete)

### Active Design Drafts
- *(no active design drafts — ACTOR_DESIGN.md superseded by HEP-CORE-0018; actor eliminated)*

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
