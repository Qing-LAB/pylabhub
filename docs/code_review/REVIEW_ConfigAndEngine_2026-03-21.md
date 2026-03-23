# Code Review: Config Module + ScriptEngine Post-Refactor

**Date**: 2026-03-21
**Scope**: Config module Phase 1-2, ScriptEngine lifecycle, role hosts, design docs
**Status**: CLOSED (config complete 2026-03-23; deferred items tracked in API_TODO)

## Findings

| ID | Sev | Description | Status |
|----|-----|-------------|--------|
| SE-01 | HIGH | PythonEngine init leak: interp alive after return false | ✅ FIXED 2026-03-21 |
| SE-02 | HIGH | Role hosts don't call engine->finalize() on init failure | ✅ FIXED 2026-03-21 |
| SE-03 | HIGH | HEP-CORE-0011 fundamentally stale (old inheritance model) | ⏳ DEFERRED (after code stabilizes) |
| SE-05 | HIGH | Return contract mismatch: docs say None=commit, runtime says None=error | ✅ FIXED 2026-03-21 |
| SE-06 | MED | Python ignores entry_point parameter | ✅ FIXED 2026-03-21 (doc + LOGGER_WARN) |
| SE-07 | MED | --validate stub in all 3 role hosts | ⏳ DEFERRED (higher-layer refactoring) |
| SE-08 | MED | HEP-0018/0015 partially stale | ⏳ DEFERRED (batch with SE-03) |
| SE-10 | LOW | Stale CMake comments (PythonScriptHost) | ✅ FIXED 2026-03-21 |
| SE-11 | LOW | Consumer snapshot missing period_ms | ✅ FIXED 2026-03-21 |
| SE-12 | LOW | Stale ActorHost ref in script_host.hpp | ✅ FIXED 2026-03-21 |
| SE-13 | MED | Lua entry-point __init__.lua vs init.lua | ✅ FIXED 2026-03-21 |
| SE-14 | MED | script.type not validated | ✅ FIXED 2026-03-21 (via config::parse_script_config) |
| SE-15 | LOW | GIL comments contradictory | ✅ FIXED 2026-03-21 |
| CR-01 | MED | config::AuthConfig::load_keypair declared but not implemented | ✅ FIXED 2026-03-21 |
| CR-02 | LOW | Unused <filesystem> include in auth_config.hpp | ✅ FIXED 2026-03-21 |
| CR-03 | MED | RoleHostCore has 6 public plain members (should be private) | ⏳ DEFERRED (ScriptEngine cleanup) |
| CR-04 | — | should_continue_loop() memory ordering: CORRECT | ✅ VERIFIED |
| CR-05 | — | PYLABHUB_BUILD_TESTS guard: CORRECT | ✅ VERIFIED |
| CR-06 | LOW | 3 identical role-specific AuthConfig::load_keypair() | ✅ FIXED (RoleConfig::load_keypair) |
| CR-07 | — | No config constant duplication | ✅ VERIFIED |

## Deferred Items Tracked In

- SE-03/08: `docs/todo/API_TODO.md` (HEP doc rewrite)
- SE-07: `docs/todo/API_TODO.md` (--validate implementation)
- CR-03: Config module Phase 3 (RoleHostCore encapsulation)
- CR-06: Config module Phase 3 (consolidate auth)

## Config Module Progress

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Categorical config headers + shared parsers | ✅ cb7e4b5 |
| 2 | RoleConfig unified class with JsonConfig backend | ✅ 36f1902 |
| 3 | Migrate role hosts + mains to RoleConfig | ✅ c0100d1, a445dca |
| 4 | Remove monolithic config structs | ✅ 9dbfa59 |
| 5 | Dead field cleanup + struct merges | ✅ cc4c581, f2a805e |
| 6 | HEP/README doc sync | ✅ fcaaf33 |

## Design Documents Created

- `docs/tech_draft/engine_thread_model.md` — ScriptEngine thread model, cross-thread execution, shared state, NativeEngine, build_info ABI verification
- `docs/tech_draft/config_module_design.md` — Categorical config redesign (if created)
