# Full Module Code Review — 2026-04-06

**Reviewer**: Claude Opus 4.6 (4-agent parallel audit: engines+API, role hosts, tests, docs+build)
**Scope**: All modules — RoleAPIBase, engines, role hosts, config, tests (L0-L4), docs, build system
**Branch**: `feature/lua-role-support`
**Test baseline**: 1323/1323 passing

---

## Disposition — all items validated against code 2026-08-02/03

Every row below was re-checked against the current tree before being
closed. Four findings had already been fixed or had their subject deleted
without anyone updating this table, which is why the table said ten open
items when one was open.

The single surviving item, **B-1**, is now carried in
`docs/todo/API_TODO.md` so it is not lost when this review is archived.

---

## Status Table

| ID | Sev | Category | File(s) | Description | Status |
|----|-----|----------|---------|-------------|--------|
| **A-1** | HIGH | Logic | `processor_role_host.cpp:909-916` | Data socket poll condition: comment says "needed for ZMQ relay" but code adds socket when `!= "zmq"` (SHM mode). | :white_check_mark: STALE 2026-08-02 — `data_transport()` no longer exists anywhere in `src/processor/` or `src/consumer/`; the `!= "zmq"` construct went with the topology/transport rework. Nothing to fix. |
| **B-1** | LOW | Dead code | `role_host_core.hpp:516,525` | `should_continue_loop()` and `should_exit_inner()` defined but never called by any role host | :x: **OPEN — the only one.** Both still defined, still zero production callers; the only references are in `test_role_host_core.cpp`, which tests them. Tested-but-uncalled is worse than plain dead code: the tests make it look live. Adopt-or-delete belongs inside band 4 (role-host unification), which collapses the three loops this would touch. Tracked in `API_TODO.md`. |
| **B-2** | LOW | Dead code | `engine_module_params.cpp:24` | Redundant `p->api ? p->api->core() : nullptr` ternary | :white_check_mark: STALE 2026-08-02 — `src/scripting/engine_module_params.cpp` no longer exists. |
| **C-1** | LOW | Duplication | `producer_api.cpp`, `consumer_api.cpp`, `processor_api.cpp` | `to_channel_side()` identical helper defined 3 times | :white_check_mark: FIXED 2026-08-02 (`d92a7b0b`) — one `inline` definition in `scripting/json_py_helpers.hpp`, where the other shared py-argument conversions already live; the three file-static copies deleted. |
| **D-2** | LOW | Docs | `script_engine.hpp`, `python_engine.hpp`, `engine_module_params.hpp` | Comment references point to archived tech drafts instead of HEP-0011 | :white_check_mark: RESOLVED 2026-08-02 — no `script_engine_refactor` or `script_engine_lifecycle_module` reference survives in the two headers that still exist; the third file is deleted. |
| **E-1** | INFO | API gap | `native_engine_api.h` | Native C API missing queue diagnostics vs Python/Lua | :white_check_mark: RESOLVED 2026-08-02 — 6 of the 8 named methods shipped with the #194 parity work (`in_policy`, `in_capacity`, `out_policy`, `out_capacity`, `last_seq`, `set_verify_checksum`). The other two, `update_last_seq` and `ctrl_queue_dropped`, exist nowhere in `src/include/` at all — retired framework-wide, so not a native gap. |
| **F-1** | INFO | Test gap | `test_scriptengine_native_dylib.cpp` | Native engine L2 tests: 22 vs Python 98 / Lua 101 | :grey_exclamation: NOT A FINDING — a raw count across engines with different surface areas. Test-count parity is not a defect; a named uncovered behaviour would be. Nothing actionable as written. |
| **F-2** | INFO | Test gap | `test_datahub_engine_roundtrip.cpp` | L3 engine roundtrip: producer-only | :grey_exclamation: SUPERSEDED — three-engine L3 parity is tracked concretely in `TESTING_TODO.md` (the #98 channel-broadcast parity gap), which names the behaviour instead of a count. |
| **F-3** | INFO | Test quality | 10+ test files | ~110 `sleep_for` calls; `poll_until()` adopted in only 2 files | :white_check_mark: LARGELY RESOLVED 2026-08-02 — both named offenders are gone: `test_datahub_hub_zmq_queue.cpp` no longer exists and `test_datahub_broker_protocol.cpp` now contains **zero** `sleep_for` (was 21). `poll_until` adoption is up from 2 files to 14. The standing rule lives in `README_testing.md`, not in this review. |
| **F-4** | INFO | Test quality | 30+ test files | `test_schema_helpers.h` used by 6 files; applicable to 30+ | :grey_exclamation: NOT A FINDING — same counting exercise as F-1. "Applicable to 30+" was never established per-file. |

---

## Detailed Findings

### A-1: Processor data socket poll condition — comment/code mismatch (HIGH)

**File**: `src/processor/processor_role_host.cpp:909-916`

```cpp
// Consumer data socket is only needed when data comes via ZMQ relay (SHM transport
// uses the DataBlock directly, not the data socket).
if (in_consumer_->data_transport() != "zmq")
{
    loop.sockets.push_back(
        {in_consumer_->data_zmq_socket_handle(),
         [&] { in_consumer_->handle_data_events_nowait(); }});
}
```

The comment says the data socket is "only needed when data comes via ZMQ relay", which
reads as `== "zmq"`. But the code uses `!= "zmq"`, adding the socket when transport is SHM.

**Cross-reference**: Consumer role host (`consumer_role_host.cpp:349-360`) uses the same
inverted-looking pattern:
```cpp
// ZMQ data routing for SHM transport: ZMQ frames -> message queue.
if (!is_zmq) { in_consumer_->on_zmq_data(...); }
```

Consumer's comment is clearer: "ZMQ data routing **for** SHM transport". This suggests the
code is correct (SHM mode needs ZMQ relay socket polling) but the processor comment is
misleading. Needs design verification: what exactly does `data_zmq_socket_handle()` carry
in each transport mode?

---

### B-1: Unused RoleHostCore convenience methods (LOW)

**File**: `src/include/utils/role_host_core.hpp:248-262`

```cpp
[[nodiscard]] bool should_continue_loop() const noexcept { ... }
[[nodiscard]] bool should_exit_inner() const noexcept { ... }
```

All 3 role hosts use inline equivalents instead:
```cpp
while (core_.is_running() && !core_.is_shutdown_requested() && !core_.is_critical_error())
```

Options: (a) adopt these in role hosts to reduce repetition, or (b) remove them.

---

### B-2: Redundant null-check ternary (LOW)

**File**: `src/scripting/engine_module_params.cpp:24`

```cpp
if (!p->engine->initialize(p->tag, p->api ? p->api->core() : nullptr))
```

Line 20 throws if `!p->api`, so by line 24 `p->api` is guaranteed non-null. The ternary
is dead code. Should be: `p->api->core()`.

---

### C-1: `to_channel_side()` duplicated 3 times (LOW)

**Files**: `producer_api.cpp:102-107`, `consumer_api.cpp:72-77`, `processor_api.cpp:99-104`

Identical helper:
```cpp
static std::optional<scripting::ChannelSide> to_channel_side(std::optional<int> side)
{
    if (!side.has_value()) return std::nullopt;
    return static_cast<scripting::ChannelSide>(*side);
}
```

Move to `src/scripting/python_helpers.hpp` to eliminate duplication.

---

### D-2: Stale documentation references (LOW)

Three source files reference archived tech drafts instead of the current HEP:

| File | Line | Old reference | Should be |
|------|------|---------------|-----------|
| `src/include/utils/script_engine.hpp` | 27 | `docs/tech_draft/script_engine_refactor.md` | `docs/HEP/HEP-CORE-0011-ScriptHost-Abstraction-Framework.md` |
| `src/scripting/python_engine.hpp` | 6 | `script_engine_refactor.md` | HEP-CORE-0011 |
| `src/scripting/engine_module_params.hpp` | 10 | `docs/tech_draft/script_engine_lifecycle_module.md` | HEP-CORE-0011 |

---

### E-1: Native engine API incomplete vs Python/Lua (INFO — future sprint)

**File**: `src/include/utils/native_engine_api.h`

Native C API has core counters and spinlock/schema/messaging but lacks:
- `in_policy()` / `in_capacity()` / `out_policy()` / `out_capacity()`
- `last_seq()` / `update_last_seq()`
- `set_verify_checksum()`
- `ctrl_queue_dropped()`

By design for initial release — extend in future native API sprint.

---

### F-1 through F-4: Test coverage and quality (INFO — future sprint)

**F-1**: Native engine L2 tests (22) are ~78% fewer than Python (98) / Lua (101).
Missing: error-return variants, multi-role edge cases, queue diagnostics, custom metrics.

**F-2**: L3 engine roundtrip (`test_datahub_engine_roundtrip.cpp`) only tests
producer role. No consumer-role or processor-chain engine tests at L3.

**F-3**: ~110 `sleep_for` calls across 10+ test files. `poll_until()` from
`test_sync_utils.h` adopted in only 2 files. Main offenders:
`test_datahub_hub_zmq_queue.cpp` (32), `test_datahub_broker_protocol.cpp` (21).

**F-4**: `test_schema_helpers.h` (shared schema factories) used by 6 files but
applicable to 30+ engine/schema test files.

---

## Verified Non-Issues (agent false positives)

| Claimed issue | Verification | Result |
|---------------|-------------|--------|
| ProducerAPI missing `as_numpy` | `grep as_numpy *_api.cpp` | All 3 register it |
| ConsumerAPI missing `last_cycle_work_us` | `grep last_cycle_work_us *_api.cpp` | All 3 register it |
| ConsumerAPI missing `broadcast()`/`send()` | Design review | By design: consumers receive, producers send |

---

## Cross-references

- **SE-09** (hubshell `python_script_host.cpp` missing): tracked in `REVIEW_ScriptEngine_2026-03-20.md`
- **Plan file** `joyful-shimmying-fox.md`: all 3 items done, can be closed
- **TODO_MASTER.md**: Priority 0 RoleAPIBase section complete (1323/1323)
