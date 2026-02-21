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

### Priority 1: HubShell — Python + Admin Shell Integration
📍 **Status**: ✅ Complete (2026-02-20)
📋 **Details**: `docs/todo/MESSAGEHUB_TODO.md`

All 6 phases complete — 426/426 tests passing:
- ✅ **Phase 1**: `HubConfig` lifecycle module + layered JSON config
- ✅ **Phase 2**: CMake Python env (python-build-standalone 3.14, `prepare_python_env`)
- ✅ **Phase 3**: Removed `src/broker/` standalone; BrokerService stays in pylabhub-utils
- ✅ **Phase 4**: `PythonInterpreter` lifecycle module + pybind11 bindings
- ✅ **Phase 5**: `AdminShell` C++ ZMQ REP lifecycle module + `hubshell_client.py`
- ✅ **Phase 6**: `hubshell.cpp` rewrite (9 lifecycle modules, BrokerService bg thread, double-SIGINT)

### Priority 2: Test Coverage Gaps
📍 **Status**: Mostly complete
📋 **Details**: `docs/todo/TESTING_TODO.md`

Key tasks:
- Recovery scenario tests — facility layer ✅ done; broker-coordinated flow deferred (needs broker protocol)
- ✅ Messenger broker integration tests — Phase C complete (390/390 tests passing)
- ✅ Consumer registration protocol + E2E test — 397/397 tests passing
- ✅ hub::Producer + hub::Consumer active API — 15/15 tests; 417/417 total passing
- ✅ Broker health and notification — DatahubBrokerHealthTest 5/5 tests; 422/422 total passing
- ✅ RAII stress tests — DatahubStressRaiiTest 2/2 tests (full-capacity + back-pressure); 424/424 total passing
- ✅ Pluggable Slot-Processor API (HEP-0006) — push/synced_write/pull/set_write_handler/set_read_handler; WriteProcessorContext/ReadProcessorContext; 424/424 total passing

### Priority 3: Messenger Broker Protocol
📍 **Status**: ✅ Complete — health/notification layer implemented; 424/424 tests passing
📋 **Details**: `docs/todo/MESSAGEHUB_TODO.md`

Key tasks:
- [x] `pylabhub-broker` executable — `src/broker/` (ChannelRegistry + BrokerService + broker_main)
- [x] CurveZMQ server keypair; REG_REQ / DISC_REQ / DEREG_REQ handlers
- [x] Phase C broker integration tests — DatahubBrokerTest (6 tests)
- [x] Consumer registration protocol — CONSUMER_REG_REQ/ACK, CONSUMER_DEREG_REQ/ACK, consumer_count in DISC_ACK
- [x] DatahubBrokerConsumerTest (6 tests: 391–396) + DatahubE2ETest (1 test: 397)
- [x] hub::Producer + hub::Consumer active services (15 tests: 403–417)
- [x] Broker health/notification layer — Cat 1/Cat 2 error taxonomy; CONSUMER_DIED_NOTIFY; CHANNEL_ERROR_NOTIFY; per-channel Messenger callbacks; auto-wire in Producer/Consumer; DatahubBrokerHealthTest (5 tests: 418–422)
- [x] Cleanup: removed src/admin/ and src/python/ (outdated stubs); updated examples/ (raii_layer_example fixed; hub_active_service_example and hub_health_example added); broker_main updated with CryptoUtils + on_ready pubkey display

### Priority 4 (complete): Pluggable Slot-Processor API
📍 **Status**: ✅ Complete — 424/424 tests passing (2026-02-19)
📋 **Details**: `docs/todo/MESSAGEHUB_TODO.md` § "Pluggable Slot-Processor and Messenger Access"
📐 **HEP**: `docs/HEP/HEP-CORE-0006-SlotProcessor-API.md`

Two modes: Queue (`push`/`synced_write`/`pull`) and Real-time (`set_write_handler`/`set_read_handler`).
Fully-typed `WriteProcessorContext<F,D>` / `ReadProcessorContext<F,D>` bundles slot + FlexZone +
messaging + shutdown signal. Renamed APIs (`post_write`→`push`, `write_shm`→`synced_write`,
`read_shm`→`pull`). Upgraded `on_shm_data` to typed `set_read_handler`.
stop() notifies CVs in producer write_thread and consumer shm_thread for immediate wakeup.

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
| Security / Identity / Provenance | 🟡 In Progress | `docs/todo/SECURITY_TODO.md` | Phase 4 complete (SHM identity, C-API, ConsumerOptions, 426/426 tests); Phases 1–3, 5 pending |
| Actor (pylabhub-actor) | ✅ Complete | `docs/tech_draft/ACTOR_DESIGN.md` | Multi-role actor (2026-02-21): ActorHost, ProducerRoleWorker, ConsumerRoleWorker, decorator dispatch, ctypes zero-copy schema, examples; UID format (HUB-/ACTOR-prefix enforcement + auto-gen); SharedSpinLockPy Python API (api.spinlock(idx), context manager); 426/426 tests |
| HubShell / HubConfig | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | All 6 phases done (2026-02-20): HubConfig, Python env, broker consolidation, PythonInterpreter, AdminShell, hubshell.cpp rewrite |
| RAII Layer | ✅ Complete | `docs/todo/RAII_LAYER_TODO.md` | Phase 3 complete; all code review items resolved; 5 backlog enhancements |
| API / Primitives | 🟢 Ready | `docs/todo/API_TODO.md` | WriteAttach mode + `attach_datablock_as_writer_impl` added; timeout constants; ScopedDiagnosticHandle; **header layering refactor in backlog** |
| Platform / Windows | 🟢 Mostly done | `docs/todo/PLATFORM_TODO.md` | Major pass done; 2 Windows CI items in backlog |
| Testing | 🟢 Ongoing | `docs/todo/TESTING_TODO.md` | 426/426 passing; planned: Layer 4 hub+actor integration tests (log-file-based validation) |
| Memory Layout | ✅ Complete | `docs/todo/MEMORY_LAYOUT_TODO.md` | Single structure; alignment fixed |
| Schema Validation | ✅ Complete | — | BLDS schema done; dual-schema producer/consumer validation working |
| Recovery API | ✅ Complete | — | P8 recovery API done; DRAINING recovery restores COMMITTED |
| Messenger / Broker | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | Cat 1/Cat 2 health layer; Slot-Processor API (HEP-0006); 424/424 total |

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
- **`docs/todo/SECURITY_TODO.md`** — Hub vault, directory model, identity, connection policy, provenance chain
- **`docs/todo/RECOVERY_TODO.md`** — Recovery scenarios, diagnostics improvements (to be created)

### Active Design Drafts
- **`docs/tech_draft/ACTOR_DESIGN.md`** — pylabhub-actor multi-role design (config, Python API, ctypes schema, C++ architecture, runtime costs, gap analysis)

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
