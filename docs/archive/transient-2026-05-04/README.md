# Archive batch — 2026-05-04

## Contents

| File | Origin | Reason |
|---|---|---|
| `engine_thread_model.md` | `docs/tech_draft/` (created 2026-03-20) | Fully superseded by HEP-CORE-0011 + HEP-CORE-0028 + README_NativePlugin.md.  Section-by-section content has landed in canonical docs (see merge map below). |

## Merge map — `engine_thread_model.md`

The draft predates Phase 7 and was the design reference for the
ScriptEngine threading model (Lua multi-state, Python single-state
queue, GIL strategy, lifecycle, native engine extension API).  Each
section's content has been canonicalized:

| Draft section | Canonical home |
|---|---|
| §1 Problem statement, §2 Design principles | HEP-CORE-0011 § Architecture Overview, § Design Decisions |
| §3 ScriptEngine interface (status types, generic execution, hot-path methods, thread queries) | HEP-CORE-0011 § ScriptEngine Interface, § State Machine, § Key Methods |
| §4 Execution model (queue, mutex, owner / non-owner paths, GIL, runtime overhead, lifecycle) | HEP-CORE-0011 § Thread Safety, § ThreadManager |
| §5 LuaEngine thread-state management (auto lifecycle, what's shared vs independent, cleanup) | HEP-CORE-0011 § Thread Safety (Lua section); implementation in `src/scripting/lua_engine.cpp::get_or_create_thread_state_` (live code is the authoritative reference for the algorithm) |
| §6 PythonEngine single-state serialized execution (invoke impl, process_pending_, shutdown) | HEP-CORE-0011 § Thread Safety (Python section); implementation in `src/scripting/python_engine.cpp` |
| §7 Shared resources (RoleHostCore inbox cache, shared script state) | HEP-CORE-0011 § Shared Script State |
| §8 Runtime cost analysis | (No canonical home — implementation-detail benchmark numbers; preserved here for historical reference if perf regressions need a baseline) |
| §9 Integration points (data loop, RoleHostCore, Role API classes, engines) | HEP-CORE-0011 § Initialization Protocol, § Unified Data Loop Architecture |
| §10 Implementation phases | HEP-CORE-0011 § Implementation Status (which carries the actual shipped phase status) |
| §11 NativeEngine — Dynamic C++ Library Extension (motivation, interface mapping, symbol convention, threading, zero-copy data, schema validation, configuration, runtime cost, API header, comparison) | **HEP-CORE-0028 Native Plugin Engine** (full normative spec, 722 lines) + `docs/README/README_NativePlugin.md` (developer-facing guide, 829 lines) + `src/include/utils/native_engine_api.h` (the C ABI header itself) |

## Why this archive batch (rationale)

Static review of the post-Phase-7-closure tree (commit `b4c00c3`)
flagged `engine_thread_model.md` as "Med" severity for archival
review (finding F2).  Audit confirmed the draft's content was fully
absorbed into HEP-0011 (core engine framework), HEP-0028 (native
extension), and README_NativePlugin (developer guide) — all three
shipped with implementation.  Per `docs/DOC_STRUCTURE.md` §2.1, a
draft whose content is fully merged into canonical docs moves to
archive.

The draft is preserved here for historical reference — primarily
the §8 runtime-cost-analysis numbers (Owner-thread invocations
zero-cost, Lua cross-thread first-call ~1ms / subsequent ~200ns,
Python cross-thread ~10us + drain latency, shared state ~100-500ns)
which haven't been re-baselined into the canonical docs.  If a
future performance regression triages against these numbers, this
file is the source.

## See also

- `docs/HEP/HEP-CORE-0011-ScriptHost-Abstraction-Framework.md` — current normative engine framework
- `docs/HEP/HEP-CORE-0028-Native-Plugin-Engine.md` — current normative native plugin spec
- `docs/README/README_NativePlugin.md` — developer guide
- `docs/DOC_ARCHIVE_LOG.md` — repository-wide archive history
