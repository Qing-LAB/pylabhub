# ScriptEngine Abstraction — Deep Code Review 2026-03-20

**Reviewer**: Claude Opus 4.6 (multi-pass audit + 3 external reviews cross-verified)
**Scope**: ScriptEngine/RoleHost refactor — engines, role hosts, API classes, configs, RoleDirectory, HEP docs, shutdown paths, broker
**Branch**: `feature/lua-role-support`
**Test baseline**: 1273/1273 passing

---

## Status Table

| ID | Sev | Category | Description | Status |
|----|-----|----------|-------------|--------|
| **SE-01** | HIGH | Lifecycle | PythonEngine::initialize() leaks live interpreter on `return false` after `interp_.emplace()` | ✅ FIXED 2026-03-20 |
| **SE-02** | HIGH | Lifecycle | All 3 role hosts: `engine_->finalize()` not called on `initialize()` failure | ✅ FIXED 2026-03-20 |
| **SE-03** | HIGH | Docs | HEP-CORE-0011 fundamentally stale — documents old inheritance hierarchy | ❌ OPEN — deferred to doc rewrite |
| **SE-04** | MEDIUM | API parity | Lua API surface ~30-40% of Python — missing custom metrics, queue state, spinlocks, messaging | ❌ OPEN — needs design decision |
| **SE-05** | HIGH | Contract | Return value contract: HEP-0018/0015 said `None=commit`, runtime treats as error | ✅ FIXED 2026-03-20 |
| **SE-06** | MEDIUM | Contract | Python ignores `entry_point` parameter — always imports __init__.py | ✅ FIXED 2026-03-20 (doc + LOGGER_WARN) |
| **SE-07** | MEDIUM | Validate | `--validate` is a stub in all 3 role hosts | ❌ DEFERRED — tracked in TODO |
| **SE-08** | MEDIUM | Docs | HEP-CORE-0018 and HEP-CORE-0015 partially stale class names | ❌ OPEN — batch with SE-03 |
| **SE-09** | MEDIUM | Residual | Legacy ScriptHost/LuaScriptHost in shared lib — unused by active code | ❌ DEFERRED — hubshell migration |
| **SE-13** | MEDIUM | Contract | Lua entry-point: `__init__.lua` vs `init.lua` split across RoleDirectory and role hosts | ✅ FIXED 2026-03-20 |
| **SE-14** | MEDIUM | Config | script.type not validated — typo silently falls to Python path | ❌ DEFERRED — config module redesign |
| **SE-10** | LOW | CMake | Stale CMake comments reference old class names | ✅ FIXED 2026-03-20 |
| **SE-11** | LOW | Metrics | ConsumerAPI snapshot missing `period_ms` — present in Producer and Processor | ✅ FIXED 2026-03-20 |
| **SE-12** | LOW | Residual | script_host.hpp references deleted ActorHost in comment | ✅ FIXED 2026-03-20 |
| **SE-15** | LOW | Docs | GIL comments contradictory — said per-invoke acquire/release, actually held for engine lifetime | ✅ FIXED 2026-03-20 |

---

## Detailed Findings

### SE-01: PythonEngine interpreter leak on init failure (HIGH)

**File**: `src/scripting/python_engine.cpp`
**Confirmed by**: 3 independent reviewers

After `interp_.emplace(&config)` at line 230 creates a live CPython interpreter, three paths return `false` without destroying it:
- Line 245: venv directory not found
- Line 271: venv site-packages not found
- Line 282: catch block for any std::exception

The role hosts (SE-02) don't call `finalize()` on init failure, so the interpreter is destroyed by `PythonEngine`'s destructor — potentially on a different thread. This turns a normal configuration failure (missing venv) into undefined teardown behavior.

**Proposed fix**: `interp_.reset()` before each `return false` after `emplace()`, AND `engine_->finalize()` in role host init-failure paths (belt and suspenders).

---

### SE-02: Role host init-failure doesn't call finalize() (HIGH)

**Files**: `producer_role_host.cpp:97-101`, `consumer_role_host.cpp:96-100`, `processor_role_host.cpp:99-103`

All three `worker_main_()` implementations return immediately on `engine_->initialize()` failure without calling `engine_->finalize()`. Both engines' `finalize()` is safe to call after partial init (guarded by `!state_` / `!interp_.has_value()`).

**Proposed fix**: Add `engine_->finalize()` before `return` in each init-failure path.

---

### SE-03: HEP-CORE-0011 fundamentally stale (HIGH)

Sections requiring rewrite:
- **§3.2** Library Structure (lines 75-104): references PythonRoleHostBase, LuaRoleHostBase, ProducerScriptHost
- **§3.3** Class Hierarchy (lines 106-209): Mermaid diagram shows inheritance model
- **§4.2** Concrete Classes (lines 389-419): describes old PythonScriptHost threading model
- **§8** GIL Management (lines 726-770): shows interpreter thread + loop_thread_ (neither exists)
- **§8.2** Source File Reference (lines 828-850): lists removed files

Actual architecture: composition-based `RoleHost` + injected `ScriptEngine`; single working thread; no interpreter thread for Python. `tech_draft/script_engine_refactor.md` is the accurate reference.

---

### SE-04: Lua API parity gap (MEDIUM)

Carried forward from PARITY-01 in REVIEW_FullStack_2026-03-17.

Lua has basic control (log, stop, critical_error, uid, name, channel, counters) but is missing:
- **Custom metrics**: `report_metric()`, `report_metrics()`, `clear_custom_metrics()`
- **Metrics export**: `snapshot_metrics_json()`, `metrics()` dict
- **Queue state**: `last_seq()`, `in_capacity()`, `in_policy()`, `out_capacity()`, `out_policy()`
- **Spinlocks**: `spinlock()`, `spinlock_count()`
- **Inter-role**: `notify_channel()`, `broadcast_channel()`, `list_channels()`
- **Diagnostics**: `loop_overrun_count()`, `last_cycle_work_us()`
- **Accessors**: `flexzone()`, `logs_dir()`, `run_dir()`

Additionally, `open_inbox()` and `wait_for_role()` are duplicated across all 3 API classes + separately in Lua — a drift hotspot.

Design question: full parity or intentionally lightweight?

---

### SE-05: Return value contract mismatch (MEDIUM)

**Confirmed by**: 2 independent reviewers

Runtime (`parse_return_value_()` in python_engine.cpp:933-954):
- `True` → Commit
- `False` → Discard
- `None` → Error (LOGGER_WARN + inc_script_errors + possible shutdown if stop_on_script_error)

4 doc/template locations say `True/None=commit`:
- `producer_api.hpp:20`
- `processor_api.hpp:23`
- `producer_main.cpp:183` (starter script template)
- `processor_main.cpp:225` (starter script template)

This is not doc drift — it's a behavioral regression risk. A new user following the generated template with `stop_on_script_error=true` gets shutdown on first callback.

**Proposed fix**: Update all 4 locations to `True=commit, False=discard, None=error`.

---

### SE-06: Python ignores entry_point parameter (MEDIUM)

`ScriptEngine::load_script()` (script_engine.hpp:121) documents an arbitrary entry-point. Lua uses it directly (lua_engine.cpp:105). Python stores it (line 301) then imports via package layout from `script_dir` (line 328).

The abstraction is "shared surface, runtime-specific rules underneath" — weaker than it looks. Will matter if you add alternate Python entry points, reload tooling, or generic engine orchestration.

**Proposed fix**: Doc note on interface + LOGGER_WARN if entry_point != "__init__.py".

---

### SE-07: --validate is a stub (MEDIUM, DEFERRED)

All three role hosts return success after schema registration with `TODO: print layout via engine`. No engine-specific layout report, no API construction check, no callback contract validation. Deferred to higher-layer refactoring.

---

### SE-08: HEP-0018/0015 partially stale (MEDIUM)

- HEP-0018 §2: "Host a Python (or Lua) script via PythonScriptHost / LuaScriptHost"
- HEP-0018 §6: references ProducerScriptHost::do_initialize(), ConsumerScriptHost::do_initialize()
- HEP-0015 §3: Layer 4 described as "ProcessorScriptHost (PythonRoleHostBase)"

---

### SE-09: Legacy code in shared lib (MEDIUM, DEFERRED)

`src/utils/scripting/script_host.cpp` and `lua_script_host.cpp` compiled into `pylabhub-utils` but never instantiated by active code. Deferred until hubshell migrates to PythonEngine.

---

### SE-13: Lua entry-point convention split (MEDIUM)

**New finding from Round 3 review. Verified against code.**

`RoleDirectory::script_entry()` (role_directory.cpp:80) constructs path as:
```
resolved / "script" / type / ("__init__" + ext)
```
For Lua: `__init__.lua`

But all 3 role hosts (producer_role_host.cpp:119, consumer:118, processor:121) pass:
```cpp
const char *entry_point = (config_.script_type == "lua") ? "init.lua" : "__init__.py";
```

Additionally, `has_standard_layout()` (role_directory.cpp:165-172) only checks for `script/python/` — no Lua directory check.

This is a whole-project contract split. When RoleDirectory is actually used for Lua roles (HEP-0024 integration), it will produce wrong paths.

**Proposed fix**: Align on one convention. Either `init.lua` everywhere (matching Lua community convention) or `__init__.lua` everywhere (matching Python's package convention). Update RoleDirectory + role hosts + has_standard_layout to be consistent. The Lua community convention is `init.lua` — recommend that as canonical.

---

### SE-14: script.type not validated in config (MEDIUM)

**New finding from Round 3 review. Verified against code.**

All 3 config parsers accept `script.type` as any string (producer_config.cpp:275, consumer:238, processor:379):
```cpp
cfg.script_type = s.value("type", std::string{"python"});
```

The binaries only distinguish `"lua"` from "everything else":
```cpp
const char *entry_point = (config_.script_type == "lua") ? "init.lua" : "__init__.py";
```

A typo like `"pytho"` or `"Python"` silently falls through to the Python path and fails at script load with a confusing import error instead of a clear config validation error.

**Proposed fix**: Validate `script_type` ∈ {"python", "lua"} in config parsing. Throw `std::invalid_argument` for unknown types, consistent with how transport, timing, and overflow_policy are validated.

---

### SE-10: Stale CMake comments (LOW)

- `src/scripting/CMakeLists.txt:5-6`: said "PythonScriptHost"
- `src/scripting/CMakeLists.txt:60`: status message listed old class names
- `src/CMakeLists.txt:51-54`: disabled hubshell description outdated
- `src/CMakeLists.txt:63`: references nonexistent `scripting/python_script_host.cpp`
- `hub_script.hpp:68`: includes `python_script_host.hpp` which no longer exists

---

### SE-11: Consumer snapshot missing period_ms (LOW)

`ConsumerAPI::snapshot_metrics_json()` omits `period_ms`. Producer includes it. Processor includes `in_period_ms`/`out_period_ms`. Consumer is demand-driven but field should appear for schema consistency.

---

### SE-12: Stale ActorHost comment (LOW)

`src/include/utils/script_host.hpp:19`: references `ActorHost` (eliminated 2026-03-01).

---

### SE-15: GIL strategy comments contradictory (LOW)

**New finding from Round 3 review. Verified against code.**

- `python_engine.hpp:9`: "GIL acquire/release inside every invoke_*() call"
- `python_engine.cpp:5`: "All invoke methods acquire/release the GIL internally"
- `python_engine.hpp:165`: "GIL stays held on the worker thread...reentrant (no-op)"

The first two comments describe a design that doesn't match reality. The GIL is held for the engine lifetime on the worker thread. `py::gil_scoped_acquire` in each invoke is reentrant and does nothing. This isn't a bug today but will mislead anyone adding concurrency.

**Proposed fix**: Update header and .cpp file-level comments to say "GIL held for engine lifetime; per-invoke acquire is reentrant no-op for safety."

---

## Investigated and Dismissed

These were raised in external reviews, investigated against code, and found to be non-issues or acceptable:

### Shutdown ordering race (Round 2 R2)

**Investigated**: `teardown_infrastructure_()` calls `set_running(false)` before `ctrl_thread_.join()`. ZmqPollLoop has up to 5ms poll latency before it sees the flag.

**Verdict**: Not a use-after-free. The join completes BEFORE any infrastructure objects are destroyed. The worst case is one extra heartbeat with stale metrics during the 5ms window. Acceptable.

### request_stop() during invoke_on_init() (Round 2 R2)

**Investigated**: `set_running(true)` is called on the line before the data loop starts. `request_stop()` only sets `shutdown_requested_`, not `running_threads_`. The loop starts, sees `shutdown_requested_==true`, and exits immediately.

**Verdict**: Orderly fast exit. Not a bug.

### Queue acquire blocks on shutdown (Round 2 R2)

**Investigated**: Inner retry loop blocks for at most one `short_timeout` period (typically <10ms for MaxRate). Shutdown check happens between retries.

**Verdict**: Acceptable latency. Not a bug.

### Broker complexity (Round 2 R3)

**Investigated**: Broker handles 19 message types but uses single-threaded post-poll architecture with one mutex. All registry/metrics access serialized. No data races possible.

**Verdict**: Latency hazards exist (100ms poll timeout) but are documented and configurable. No correctness bugs.

### DataBlock drain-hold dead reader (Round 2 R4)

**Investigated**: Dead-reader in drain-hold mode is documented in code comments (data_block_slot_ops.cpp:46-48). Only affects Latest_only policy. Manual recovery exists (`datablock_release_zombie_readers()`). No automatic detection.

**Verdict**: Correctly characterized as operational liveness edge case. Known, documented, recovery available. Not a new finding.

---

## Code Changes Applied (build verified, 1273/1273 tests passing)

1. SE-01: `interp_.reset()` on 3 early-return paths in `python_engine.cpp`
2. SE-02: `engine_->finalize()` in 3 role host init-failure paths
3. SE-05: Corrected HEP-CORE-0018 §6.1 and HEP-CORE-0015 §6 (`None` → error, not commit)
4. SE-06: Doc note on `script_engine.hpp:123-125` + LOGGER_WARN in `python_engine.cpp:310-314`
5. SE-10: Updated CMake comments in `src/scripting/CMakeLists.txt` and `src/CMakeLists.txt`
6. SE-11: Added `period_ms` to `ConsumerAPI::snapshot_metrics_json()` (consumer_api.cpp:151)
7. SE-12: Removed ActorHost reference from `script_host.hpp:19`
8. SE-13: `RoleDirectory::script_entry()` returns `init.lua` for Lua (role_directory.cpp:80)
9. SE-15: Updated GIL comments in `python_engine.hpp:9` and `python_engine.cpp:5`

## Design Documents Produced

1. **`docs/tech_draft/engine_thread_model.md`** — ScriptEngine execution model:
   thread ownership, cross-thread dispatch, generic invoke(name), shared JSON state,
   NativeEngine plugin (C/C++ tiers), build_info ABI verification
2. **`docs/tech_draft/config_module_design.md`** — Unified config module:
   RoleDirectory + JsonConfig, categorical config structs + parsers, migration plan

---

## Architectural Observations (informational, not actionable items)

These are structural notes from the layer review, not bugs:

1. **Abstraction gap**: The ScriptEngine interface looks cleaner than implementations actually are. Python and Lua encode different policy in return-value semantics, entry-point handling, state cloning, and API shape. The abstraction is "shared surface, runtime-specific rules underneath."

2. **Config validation asymmetry**: Transport, timing, and overflow_policy are carefully validated. Script-language fields (type, path, entry point) are barely checked. SE-14 addresses the most concrete instance.

3. **API duplication**: `open_inbox()`, `wait_for_role()`, `report_metric()` and broker helpers are duplicated across all 3 API classes + separately in Lua. `script_host_helpers.hpp` is the right consolidation point for future dedup.

4. **Transitional dual-stack**: Role binaries on ScriptEngine, hubshell on legacy ScriptHost. Increases maintenance and behavioral drift risk until hubshell is migrated.

5. **on_idle callback**: `docs/tech_draft/on_idle_callback_design.md` is a draft proposal (2026-03-13), not implemented. Status must remain clearly "draft."

---

## Positive Findings

- **Metrics ownership**: All RoleHostCore metrics properly exposed via encapsulated methods. No raw atomic access outside RoleHostCore.
- **Data loops**: Correct and consistent across all 3 role hosts. Metrics incremented at right points. Inner retry-acquire correct. Shutdown ordering correct.
- **pybind11 bindings**: Complete — every public method in all 3 API classes is bound.
- **Custom metrics**: Consistent across all 3 Python APIs (report_metric/report_metrics/clear_custom_metrics with InProcessSpinState protection).
- **Thread lifecycle**: No double-join risks. Promise/future synchronization correct.
- **No copy-paste drift**: Differences between role hosts are intentional (role-specific behavior).
- **Broker**: Single-threaded post-poll architecture prevents data races. 19 message types handled correctly under one mutex.
- **Loop design**: `loop_design_unified.md` (v4, 2026-03-19) accurately reflects current code. Both `target_rate_hz` and `queue_io_wait_timeout_ratio` are real, validated config fields used in all 3 role hosts.
- **QueueReader/QueueWriter split**: Strong design keeping role hosts transport-agnostic.
- **LuaState wrapper**: Well-judged RAII wrapper with thread-ownership checks and cached ffi.typeof path.
