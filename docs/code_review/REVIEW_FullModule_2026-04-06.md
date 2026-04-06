# Full Module Code Review — 2026-04-06

**Reviewer**: Claude Opus 4.6 (4-agent parallel audit: engines+API, role hosts, tests, docs+build)
**Scope**: All modules — RoleAPIBase, engines, role hosts, config, tests (L0-L4), docs, build system
**Branch**: `feature/lua-role-support`
**Test baseline**: 1323/1323 passing

---

## TODO — Address Each Issue

- [ ] A-1: Verify processor data socket poll condition (comment/code mismatch)
- [ ] B-1: Remove or adopt `should_continue_loop()` / `should_exit_inner()` in RoleHostCore
- [ ] B-2: Remove redundant ternary in `engine_lifecycle_startup()`
- [ ] C-1: Extract `to_channel_side()` to shared location
- [ ] D-2: Update 3 stale doc references to point to HEP-0011
- [ ] E-1: Track native engine API parity gap (future sprint)
- [ ] F-1: Track native engine L2 test expansion (future sprint)
- [ ] F-2: Track L3 consumer/processor engine roundtrip tests (future sprint)
- [ ] F-3: Track sleep-to-poll_until migration (future sprint)
- [ ] F-4: Track test_schema_helpers.h adoption (future sprint)

---

## Status Table

| ID | Sev | Category | File(s) | Description | Status |
|----|-----|----------|---------|-------------|--------|
| **A-1** | HIGH | Logic | `processor_role_host.cpp:909-916` | Data socket poll condition: comment says "needed for ZMQ relay" but code adds socket when `!= "zmq"` (SHM mode). Consumer uses same pattern. Comment or code is wrong. | :x: OPEN |
| **B-1** | LOW | Dead code | `role_host_core.hpp:248-262` | `should_continue_loop()` and `should_exit_inner()` defined but never called by any role host | :x: OPEN |
| **B-2** | LOW | Dead code | `engine_module_params.cpp:24` | Redundant `p->api ? p->api->core() : nullptr` ternary — `p->api` already guaranteed non-null by line 20 throw | :x: OPEN |
| **C-1** | LOW | Duplication | `producer_api.cpp:102`, `consumer_api.cpp:72`, `processor_api.cpp:99` | `to_channel_side()` identical 6-line helper defined 3 times | :x: OPEN |
| **D-2** | LOW | Docs | `script_engine.hpp:27`, `python_engine.hpp:6`, `engine_module_params.hpp:10` | Comment references point to archived tech drafts instead of HEP-0011 | :x: OPEN |
| **E-1** | INFO | API gap | `native_engine_api.h` | Native C API missing queue diagnostics (policy, capacity, last_seq, verify_checksum) vs Python/Lua | :hourglass: DEFERRED — future sprint |
| **F-1** | INFO | Test gap | `test_scriptengine_native_dylib.cpp` | Native engine L2 tests: 22 vs Python 98 / Lua 101 | :hourglass: DEFERRED — future sprint |
| **F-2** | INFO | Test gap | `test_datahub_engine_roundtrip.cpp` | L3 engine roundtrip: producer-only; no consumer-role or processor-chain engine tests | :hourglass: DEFERRED — future sprint |
| **F-3** | INFO | Test quality | 10+ test files | ~110 `sleep_for` calls; `poll_until()` adopted in only 2 files | :hourglass: DEFERRED — future sprint |
| **F-4** | INFO | Test quality | 30+ test files | `test_schema_helpers.h` used by 6 files; applicable to 30+ | :hourglass: DEFERRED — future sprint |

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
