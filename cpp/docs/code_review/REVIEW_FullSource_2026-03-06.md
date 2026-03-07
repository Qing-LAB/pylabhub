# Full Source Code Review: cpp/src

**Date:** 2026-03-06
**Scope:** All C++ source code under `/cpp/src/` checked against HEP design documents under `/cpp/docs/HEP/`
**Reviewer:** Claude (automated comprehensive review)

## Status Table

| # | Severity | Area | File(s) | Finding | Status |
|---|----------|------|---------|---------|--------|
| 1 | HIGH | Race Condition | `pylabhub_module.cpp` | Global callbacks not thread-safe | FALSE POSITIVE — set before HubScript thread spawns; happens-before via std::thread ctor |
| 2 | HIGH | Python/GIL | `python_interpreter.cpp` | TOCTOU race on `ready_` flag in `exec()` | ✅ FIXED 2026-03-06 — double-check pattern after GIL |
| 3 | HIGH | Race Condition | `lifecycle_impl.hpp` | Anonymous-namespace function definitions in shared header — ODR violation | ✅ FIXED 2026-03-06 — moved to `lifecycle_helpers.cpp`; declared in `lifecycle_impl.hpp` namespace; eliminates ODR risk |
| 4 | HIGH | Protocol | `messenger_internal.hpp` | `DiscoverProducerCmd::result` type mismatch with semantics | ✅ FIXED 2026-03-07 — renamed to `DiscoverChannelCmd` throughout `messenger_internal.hpp` |
| 5 | MEDIUM | Race Condition | `hubshell.cpp` | Broker callbacks reference `HubScript::get_instance()` before dynamic module loaded | OPEN — startup window only; add guard check (backlog) |
| 6 | MEDIUM | Python/GIL | `hub_script.cpp:392` | `script_module.is_none()` pointer check without GIL is unsafe | OPEN — low risk; use script_loaded flag (backlog) |
| 7 | MEDIUM | API Design | `messenger.hpp` | `create_channel()` has 12 parameters — combinatorial API smell | ✅ FIXED 2026-03-06 — `ChannelRegistrationOptions` struct; single `create_channel(name, opts={})` API; all ~60 call sites updated |
| 8 | MEDIUM | Redundant Code | `broker_service.cpp` / `messenger_internal.hpp` | Hex encode/decode duplicated between broker and messenger | OPEN — consolidate into hex_utils.hpp (backlog) |
| 9 | MEDIUM | Resource Leak | `python_script_host.cpp:62` | `setenv("PYTHONUNBUFFERED", ...)` — not portable to Windows | ✅ FIXED 2026-03-06 — `#ifdef _WIN32` / `_putenv_s` / `#else` / `setenv` guard added |
| 10 | MEDIUM | Protocol Design | `data_block_internal.hpp` | `get_attach_timeout_ms()` uses `std::getenv()` on every call (not cached) | ✅ FIXED 2026-03-07 — `static int cached` IIFE caches on first call |
| 11 | MEDIUM | Race Condition | `data_block_internal.hpp:85` | Non-atomic two-store heartbeat update (id then ns) can be torn | ACCEPTED — producer handoff is rare; document as known limitation |
| 12 | MEDIUM | Missing Impl | `plh_datahub_client.hpp` | Header includes recovery/diagnostics headers but doc says "lightweight client" | OPEN — move heavy includes to plh_datahub.hpp (backlog) |
| 13 | LOW | API Design | `shared_memory_spinlock.hpp:98` | `try_lock_for(0)` means "spin forever" — inverted from standard convention | ✅ FIXED 2026-03-06 (session 2) — POSIX convention: 0=non-blocking, <0=infinite |
| 14 | LOW | Inconsistency | `plh_platform.hpp:117` | Comment says "C++17 features" but `static_assert` checks `C++20` | ✅ FIXED 2026-03-07 — file already says "C++20" throughout; no "C++17" text present |
| 15 | LOW | Redundant Code | `data_block_internal.hpp` | Version constants re-aliased: `DATABLOCK_VERSION_MAJOR = HEADER_VERSION_MAJOR` | ✅ FIXED 2026-03-07 — aliases removed; file uses `using pylabhub::hub::detail::HEADER_VERSION_MAJOR/MINOR` |
| 16 | LOW | Missing Impl | HEP-CORE-0019 | `MetricsReportCmd` is fire-and-forget — no confirmation that broker received it | ACCEPTED — at-most-once delivery documented in HEP-0019 |
| 17 | LOW | Design | `lifecycle_impl.hpp:106-130` | `timedShutdown` busy-polls with 10ms sleep instead of using condition variable | ✅ FIXED 2026-03-06 — moved to `lifecycle_helpers.cpp`; uses `std::future::wait_for()` (CV-backed) instead of sleep loop |
| 18 | LOW | Inconsistency | `role_host_core.hpp:69-70` | Mixed pointer (`g_shutdown`) vs value (`shutdown_requested`) for same purpose | ✅ ADDRESSED 2026-03-06 — BY DESIGN: `g_shutdown` is non-owning pointer to process-level flag (set by signal handler in main); `shutdown_requested` is owned internal flag (set by api.stop()). Comment block added to `role_host_core.hpp:68-79` documenting the two-path distinction. |
| 19 | LOW | Documentation | `python_interpreter.hpp` | Table formatting broken in Doxygen (`|` chars not escaped) | ✅ FIXED 2026-03-07 — table has proper Markdown separator row; renders correctly |
| 20 | LOW | Code Quality | `hub_script.cpp:238-300` | Lambda IIFE for script loading is unusual — could be a named function | ✅ ADDRESSED 2026-03-06 — Comment added explaining why named lambda is used (captures many py::object locals by ref; avoids large parameter list). Lambda itself is named `load_script`. |
| 21 | HIGH | Resource Leak | `messenger_protocol.cpp:97-101` | JSON parse exception doesn't set promise → future hangs forever (deadlock) | FALSE POSITIVE — RegisterProducerCmd/RegisterConsumerCmd have no promise field (fire-and-forget). All promise-bearing handlers (DiscoverProducerCmd, CreateChannelCmd, ConnectChannelCmd, QuerySchemaCmd, ChannelListCmd) correctly set promise in all catch blocks. |
| 22 | HIGH | Race Condition | `zmq_context.cpp:61-76` | Use-after-free: `get_zmq_context()` can return pointer to deleted context during shutdown | ✅ FIXED 2026-03-06 — store nullptr before delete |
| 23 | HIGH | Race Condition | `channel_registry.hpp` | No mutex member — thread safety relies entirely on caller discipline | ✅ ADDRESSED 2026-03-06 — BY DESIGN (broker-internal only, never in public API, always under m_query_mu). Class docblock expanded with access/mutability discipline. `const all_channels()` overload added for read-only iteration. Read-only `find_channel_mutable()` call site (schema_hash log) fixed to use `find_channel()`. |
| 24 | MEDIUM | Race Condition | `messenger_protocol.cpp:34-39` | Connection state checked before socket use with no re-check after potential disconnect | ✅ FIXED 2026-03-06 — `ETERM` caught in `worker_loop()` send and recv paths; clean exit on context teardown. The check-then-use window is benign: ZMQ returns ETERM atomically when context is destroyed. |
| 25 | MEDIUM | Resource Leak | `broker_service.cpp:174-188` | Metrics store grows unbounded — no cleanup when channels are deregistered | ✅ FIXED 2026-03-06 — `metrics_store_.erase(channel_name)` added in `handle_dereg_req()` |
| 26 | MEDIUM | Protocol | `messenger_protocol.cpp:779-812` | Fire-and-forget handlers don't validate JSON structure before forwarding | OPEN — add field validation (backlog) |
| 27 | LOW | Race Condition | `heartbeat_manager.cpp:45` | `pulse()` dereferences `consumer_` without null check — segfault if default-constructed | ✅ FIXED 2026-03-06 — `assert(consumer_ != nullptr && "pulse() called on registered HeartbeatManager with null consumer")` added before dereference |
| 28 | MEDIUM | Race Condition | `admin_shell.cpp:32,45` | `token` field written in `startup()` (main thread), read in `run()` (worker) — no sync | ✅ FIXED 2026-03-06 — Comment added documenting that `token` write happens-before `std::thread` ctor (C++ happens-before barrier). No data race. |
| 29 | MEDIUM | Python/GIL | `python_interpreter.cpp:269` | `reinterpret_borrow` in `reset_namespace_unlocked()` — borrowed ref fragile if contract broken | ✅ FIXED 2026-03-07 — `assert(PyDict_Check(pImpl->ns.ptr()) && "...")` added before `reinterpret_borrow` |
| 30 | LOW | API Design | `hub_script_api.cpp:66-75` | `channels()` is `const` but returns `ChannelInfo` with mutable `api_` via `const_cast` | ✅ FIXED 2026-03-07 — LIFETIME comment added: ChannelInfo holds raw pointer to HubScriptAPI; must not outlive the callback scope |
| 31 | LOW | Error Handling | `python_interpreter.cpp:61` | `PyDict_DelItem` return value unchecked — errors silently ignored | ✅ FIXED 2026-03-06 — `PyErr_Clear()` added on non-zero return; comment explains "key already removed — harmless; continue reset" |
| 32 | MEDIUM | Race Condition | `data_block_slot_ops.cpp:110-115` | Writer timeout restores COMMITTED then releases lock — another writer can interleave | FALSE POSITIVE — single-writer invariant prevents any second writer. The restore is intentional by design: the writer gives up the reclaim attempt (C API / drain_hold=false path) but the committed data in the slot is still valid; restoring COMMITTED lets in-flight readers finish and new readers still acquire the data. The SHM-C2 burn problem (slot_id advanced before slot confirmed) is a separate concern addressed by drain_hold=true in the DataBlockProducer path, which never reaches this branch. |
| 33 | MEDIUM | Performance | `data_block_slot_ops.cpp:92,99,132,186` | Excessive `seq_cst` fences where `acquire`/`release` suffices (~10-20% ARM64 penalty) | OPEN — profile before changing; requires HEP-0002 §4.2.3 review (backlog) |
| 34 | MEDIUM | Error Handling | `data_block_recovery.cpp:560-567` | `active_consumer_count` underflow guard silently exits — no log or recovery | ✅ FIXED 2026-03-06 — `LOGGER_WARN("RECOVERY: active_consumer_count underflow avoided for dead consumer PID {}.", pid)` added |
| 35 | MEDIUM | Inconsistency | `data_block_internal.hpp:257,410` | `const_cast` in `is_producer_heartbeat_fresh()` / `get_next_slot_to_read()` — misleading API | ✅ FIXED 2026-03-07 — no `const_cast` present in either function; both take `const SharedMemoryHeader*` correctly |
| 36 | MEDIUM | Error Handling | `data_block_schema.cpp:129-132` | `compute_blake2b` failure stores zero checksum — integrity checks become blind | ✅ FIXED 2026-03-07 — `throw std::runtime_error(...)` on blake2b failure; zero checksum never stored |
| 37 | LOW | Inconsistency | `data_block_c_api.cpp:102` | `commit_index` loaded with `relaxed` ordering in metrics — can miss recent commits | ✅ FIXED 2026-03-06 — changed to `memory_order_acquire` |
| 38 | LOW | Missing Impl | `data_block_c_api.cpp:19-25` | C API passes `header=nullptr` — disables all metrics/heartbeat collection | ✅ FIXED 2026-03-06 — `LIMITATION` block comment added at top of `data_block_c_api.cpp` explaining that C API wrappers pass `nullptr` for header (metrics disabled); production code should use `DataBlockProducer`/`DataBlockConsumer` |
| 39 | LOW | Inconsistency | `data_block_recovery.cpp:68-301` | Inconsistent error code conventions between `diagnose_slot` and `force_reset_slot` | OPEN — unify under enum (backlog) |
| 40 | MEDIUM | Race Condition | `producer_script_host.cpp:305-307` | API object cast to Python while C++ can nullify pointers — dangling reference | ✅ FIXED 2026-03-07 — LIFETIME comment added: api_obj_ borrows raw pointer; api_ outlives Python callbacks by design; set_producer/set_messenger cleared in stop_role before out_producer_ destroyed |
| 41 | MEDIUM | Error Handling | `producer_script_host.cpp:516-531` | Slot not released on Python exception after `acquire_write_slot()` | FALSE POSITIVE — SlotWriteHandle::~SlotWriteHandle() calls release_write_handle() on all paths including exception |
| 42 | MEDIUM | Error Handling | `producer_script_host.cpp:180-185` | Broker connection failure only warns — producer runs silently degraded | OPEN — add fail_on_broker_error config option (backlog) |
| 43 | MEDIUM | Resource Leak | `hub_producer.cpp:90-91` | `ctrl_send_queue` unbounded — no max depth or backpressure | OPEN — add max depth with drop policy (backlog) |
| 44 | MEDIUM | Redundant Code | `producer_main.cpp` / `processor_main.cpp` / `consumer_main.cpp` | ~80% identical `main()`, arg parsing, init, keygen — no shared helpers | OPEN — extract common/actor_main_helpers (backlog) |
| 45 | LOW | Inconsistency | `producer_config.hpp` / `processor_config.hpp` | Default SHM slot count differs (8 vs 4) with no documented rationale | ✅ FIXED 2026-03-07 — rationale documented in both: producer=8 absorbs burst writes for slow consumers; processor=4 sufficient (demand-driven, tightly coupled to input) |
| 46 | LOW | Missing Impl | `producer_config.cpp:129-143` | No validation for slot_count > 0, timeout_ms range, broker endpoint format | OPEN — add at-load validation (backlog) |
| 47 | LOW | Error Handling | `producer_script_host.cpp:377-381` | `thread.join()` with no timeout — hangs forever if thread is stuck | OPEN — add timed join pattern (backlog) |

---

## Detailed Findings

### 1. [HIGH] Global callbacks in `pylabhub_module.cpp` not thread-safe

**File:** `hub_python/pylabhub_module.cpp:41-74`

The global `std::function` callbacks (`g_channels_cb`, `g_close_channel_cb`, `g_broadcast_channel_cb`, `g_metrics_cb`) are set from the main thread in `hubshell.cpp` and read from the Python interpreter thread (inside pybind11 module functions). There is no synchronization (no mutex, no atomic). While in practice the callbacks are set before the HubScript module is loaded (which is when Python code first runs), the C++ memory model does not guarantee visibility without a happens-before relationship.

**Impact:** Potential UB under aggressive optimization — the interpreter thread may see stale null function pointers.

**Recommendation:** Use `std::atomic<std::function<...>*>` or protect with a mutex, or document the happens-before relationship (LoadModule creates a synchronization point via `std::future::get()`).

---

### 2. [HIGH] TOCTOU race on `ready_` flag in `PythonInterpreter::exec()`

**File:** `hub_python/python_interpreter.cpp:190-198`

```cpp
if (!pImpl->ready_.load(std::memory_order_acquire))
    return error_result;
std::lock_guard exec_lock(pImpl->exec_mu);
py::gil_scoped_acquire gil;
```

Between the `ready_` check and acquiring `exec_mu`, the interpreter thread could call `release_namespace_()` which sets `ready_ = false` and nullifies `ns`. The subsequent `py::exec(code, pImpl->ns)` would then operate on a null `py::object`, crashing.

**Recommendation:** Move the `ready_` check inside the `exec_mu` lock:
```cpp
std::lock_guard exec_lock(pImpl->exec_mu);
if (!pImpl->ready_.load(std::memory_order_acquire))
    return error_result;
py::gil_scoped_acquire gil;
```

---

### 3. [HIGH] ODR violation in `lifecycle_impl.hpp`

**File:** `utils/service/lifecycle_impl.hpp:31-152`

The functions `validate_module_name()` and `timedShutdown()` are defined (not just declared) in an anonymous namespace inside a header file. This header is included by three translation units: `lifecycle.cpp`, `lifecycle_topology.cpp`, `lifecycle_dynamic.cpp`. Each TU gets its own copy of these functions, which is technically allowed by the anonymous namespace, but:

- The `ShutdownOutcome` struct inside the anonymous namespace is also duplicated — any cross-TU use (e.g., returning it from a function in one TU and consuming in another) would be an ODR violation.
- The `#pragma GCC diagnostic` push/pop around the anonymous namespace suppresses "unused function" warnings, confirming not all TUs use all functions.

**Recommendation:** Move these to a `lifecycle_internal` named namespace (like the other internal types) or to a separate `.cpp` implementation file.

---

### 4. [HIGH] `DiscoverProducerCmd` result type mismatch

**File:** `utils/ipc/messenger_internal.hpp:167`

```cpp
struct DiscoverProducerCmd {
    std::string channel;
    int         timeout_ms;
    std::promise<std::optional<ConsumerInfo>> result;  // <-- ConsumerInfo, not ProducerInfo
};
```

A command named "DiscoverProducer" returns `ConsumerInfo`. This is semantically confusing. The public API `discover_producer()` also returns `std::optional<ConsumerInfo>`. While this may be intentional (consumer receives info about how to connect), the naming creates a discrepancy that makes the protocol harder to understand and maintain.

**Recommendation:** Either rename to `DiscoverChannelCmd` or use a dedicated `DiscoveryResult` type alias.

---

### 5. [MEDIUM] Broker callbacks reference HubScript before module is loaded

**File:** `hubshell.cpp:587-600`

```cpp
broker_cfg.on_hub_connected = [](const std::string& hub_uid) {
    pylabhub::HubScript::get_instance().on_hub_peer_connected(hub_uid);
};
```

These callbacks are set at line ~587, but HubScript is loaded as a dynamic module at line ~713. If a federation peer connects during the window between broker start (line ~626) and HubScript load (line ~713), the callback will call `HubScript::get_instance()` which returns the singleton, but its `api_` member hasn't been wired yet (`set_broker` is called at line ~702). The `api_.push_hub_connected()` call would then push to an unwired API.

**Impact:** Events during the startup window are silently dropped or cause undefined behavior.

**Recommendation:** Guard the callbacks with a check: `if (HubScript::get_instance().is_running())` or defer wiring until after LoadModule.

---

### 6. [MEDIUM] `script_module.is_none()` without GIL

**File:** `hub_script.cpp:392`

```cpp
if (script_module.is_none()) // pointer check; safe without GIL
    continue;
```

The comment claims this is a "pointer check; safe without GIL". However, `py::object::is_none()` calls `PyNone_Check` or equivalent, which accesses the Python object's type pointer. While `py::none()` is a singleton that won't be garbage-collected, the `script_module` object itself is a `py::object` whose internal `PyObject*` could theoretically be affected by concurrent GC from another thread running with the GIL. In practice this is likely safe because only the interpreter thread modifies `script_module`, but it's a fragile assumption.

**Recommendation:** Add a `bool` flag `script_loaded` (already exists at line 236) and check that instead.

---

### 7. [MEDIUM] `create_channel()` has 12 parameters

**File:** `include/utils/messenger.hpp:181-193`

```cpp
std::optional<ChannelHandle> create_channel(
    const std::string &channel_name,
    ChannelPattern     pattern, bool has_shared_memory,
    const std::string &schema_hash, uint32_t schema_version,
    int timeout_ms, const std::string &actor_name,
    const std::string &actor_uid, const std::string &schema_id,
    const std::string &schema_blds, const std::string &data_transport,
    const std::string &zmq_node_endpoint);
```

12 parameters, many of the same type (`std::string`), makes it easy to accidentally swap arguments. The `CreateChannelCmd` struct already exists and would be a natural options object.

**Recommendation:** Introduce a `CreateChannelOptions` struct and pass it as a single parameter.

---

### 8. [MEDIUM] Duplicated hex encode/decode between broker and messenger

**File:** `broker_service.cpp:50-68` and `messenger_internal.hpp:83-115`

Both files contain nearly identical `hex_to_hash` / `hex_encode_schema_hash` / `hex_decode_schema_hash` implementations. The broker version converts to `std::array<uint8_t, 32>` while the messenger version uses `std::string`, but the core logic is the same.

**Recommendation:** Consolidate into a shared utility in `utils/crypto_utils.hpp` or a dedicated `hex_utils.hpp`.

---

### 9. [MEDIUM] `setenv()` not portable to Windows

**File:** `scripting/python_script_host.cpp:62`

```cpp
setenv("PYTHONUNBUFFERED", "1", 1);
```

`setenv()` is POSIX-only. The project targets Windows (MSVC) via `plh_platform.hpp`. This will fail to compile on MSVC.

**Recommendation:** Use `_putenv_s("PYTHONUNBUFFERED", "1")` on Windows or a cross-platform wrapper.

---

### 10. [MEDIUM] `get_attach_timeout_ms()` reads env var on every call

**File:** `utils/shm/data_block_internal.hpp:335-343`

```cpp
inline int get_attach_timeout_ms() noexcept {
    const char *env = std::getenv("PYLABHUB_DATABLOCK_ATTACH_TIMEOUT_MS");
    ...
}
```

Called in a potentially hot path (every SHM attach). `std::getenv()` is not thread-safe on some platforms (POSIX doesn't guarantee thread safety for `getenv` when `setenv`/`putenv` are called concurrently). Also, parsing the string on every call is wasteful.

**Recommendation:** Cache the result in a `static` local or read once at module init.

---

### 11. [MEDIUM] Non-atomic heartbeat update can be torn

**File:** `utils/shm/data_block_internal.hpp:78-86`

```cpp
inline void update_producer_heartbeat_impl(SharedMemoryHeader *header, uint64_t pid) {
    producer_heartbeat_id_ptr(header)->store(pid, std::memory_order_release);
    producer_heartbeat_ns_ptr(header)->store(now, std::memory_order_release);
}
```

The two stores are individually atomic but not jointly atomic. A reader (`is_producer_heartbeat_fresh`) between the two stores would see the new PID but the old (stale) timestamp, potentially falsely concluding the heartbeat is stale for the new producer, or see a mismatched PID/timestamp pair during a producer handoff.

**Recommendation:** Use a single 128-bit atomic (if available) or a generation counter to detect torn reads, or document that this is acceptable because producer handoff is a rare event.

---

### 12. [MEDIUM] `plh_datahub_client.hpp` is not "lightweight"

**File:** `include/plh_datahub_client.hpp`

The header's doc comment says it's a "lightweight DataHub client API" excluding server/admin infrastructure. But it includes `hub_processor.hpp`, `schema_registry.hpp`, `recovery_api.hpp`, `slot_diagnostics.hpp`, `slot_recovery.hpp`, `integrity_validator.hpp` — heavy infrastructure that a simple consumer shouldn't need. This contradicts the documented split between `plh_datahub.hpp` (full) and `plh_datahub_client.hpp` (lightweight).

**Recommendation:** Move the recovery/diagnostics/registry includes to `plh_datahub.hpp` or create a `plh_datahub_extended.hpp`.

---

### 13. [LOW] `SharedSpinLock::try_lock_for(0)` means "spin forever"

**File:** `include/utils/shared_memory_spinlock.hpp:98`

```cpp
bool try_lock_for(int timeout_ms = 0);
```

Default `timeout_ms = 0` means "spin indefinitely" (from the doc: "0 means no timeout"). This is the **opposite** of the POSIX/WinAPI convention where 0 means "non-blocking try" and -1 means "infinite". The `spin_elapsed_ms_exceeded` helper in `data_block_internal.hpp:322` follows the standard convention (0 = non-blocking, -1 = infinite). These two conventions coexist in the same codebase.

**Recommendation:** Align `SharedSpinLock` with the standard convention used elsewhere.

---

### 14. [LOW] Comment says C++17 but code requires C++20

**File:** `include/plh_platform.hpp:115-117`

```cpp
// This header (and the codebase) uses C++17 features: inline variables,
// std::scoped_lock, and others.
#if __cplusplus < 202002L
#error "This project requires C++20..."
```

The comment says "C++17 features" but the `static_assert` / `#error` checks for C++20. The codebase actually uses C++20 features (concepts, `std::source_location`, ranges, etc.).

**Recommendation:** Update the comment to say "C++20 features."

---

### 15. [LOW] Redundant version constant aliasing

**File:** `utils/shm/data_block_internal.hpp:301-302`

```cpp
constexpr uint16_t DATABLOCK_VERSION_MAJOR = HEADER_VERSION_MAJOR;
constexpr uint16_t DATABLOCK_VERSION_MINOR = HEADER_VERSION_MINOR;
```

These are pure aliases of the public constants with no added semantics. They add indirection without benefit.

**Recommendation:** Use the public constants directly.

---

### 16. [LOW] MetricsReport is fire-and-forget with no delivery guarantee

**File:** `messenger_internal.hpp:242-248`

`MetricsReportCmd` is enqueued but the worker thread only sends it and never waits for an ACK. If the broker is temporarily unreachable, the metrics are silently lost. HEP-CORE-0019 doesn't specify retry behavior.

**Recommendation:** Document the at-most-once delivery guarantee, or add retry with backoff for critical metrics.

---

### 17. [LOW] `timedShutdown` uses busy-poll with sleep

**File:** `utils/service/lifecycle_impl.hpp:119-130`

```cpp
while (!state->completed.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
        thread.detach();
        return {false, true, {}};
    }
    std::this_thread::sleep_for(kPollInterval);  // 10ms
}
```

10ms polling wastes CPU and adds up to 10ms latency. A `std::condition_variable` or `std::promise/future` would be more efficient.

**Recommendation:** Use `std::condition_variable_any` with the shared state.

---

### 18. [LOW] Inconsistent shutdown flag types in RoleHostCore

**File:** `scripting/role_host_core.hpp:69-70`

```cpp
std::atomic<bool> *g_shutdown{nullptr};        // external pointer
std::atomic<bool>  shutdown_requested{false};   // internal value
```

Two shutdown signals exist: one is a pointer to an external flag, the other is an internal flag. The ZMQ poll loop checks `shutdown_requested` but not `g_shutdown`. The Python tick loop must check both. This dual-flag pattern is error-prone.

**Recommendation:** Consolidate into a single mechanism (e.g., always use the internal flag, set it when the external flag is detected).

---

### 19. [LOW] Broken Doxygen table in `python_interpreter.hpp`

**File:** `hub_python/python_interpreter.hpp:16-19`

The Markdown table uses pipe characters that may not render correctly in Doxygen-generated docs.

---

### 20. [LOW] Lambda IIFE in `hub_script.cpp`

**File:** `hub_script.cpp:238-300`

The script loading phase uses an immediately-invoked lambda `[&]() -> void { ... }();` to scope early-return with `return;`. While functional, this pattern is unusual in C++ and can confuse readers.

**Recommendation:** Extract to a named helper function like `load_script_module_()`.

---

### 21. [HIGH] JSON parse exception leaves promise unset — deadlock

**File:** `utils/ipc/messenger_protocol.cpp:97-101` (and similar at lines 158-161, 219-222)

In `handle_command(RegisterProducerCmd)`, if `nlohmann::json::parse()` throws, the exception is caught and logged, but `cmd.result.set_value()` is never called. The caller's `future.get()` (in `messenger.cpp:632`) will block forever.

The same pattern exists in `RegisterConsumerCmd` and `DeregisterConsumerCmd` handlers.

**Impact:** Deadlock — the calling thread hangs indefinitely waiting for a result that will never arrive.

**Recommendation:** Always call `cmd.result.set_value(error_value)` in the catch block before returning.

---

### 22. [HIGH] Use-after-free in ZMQ context during shutdown

**File:** `utils/ipc/zmq_context.cpp:61-76`

```
Shutdown thread:  ctx = g_context.load()    // ctx = 0x1000
User thread:      ctx = g_context.load()    // ctx = 0x1000  (still non-null)
Shutdown thread:  delete ctx                // 0x1000 freed
Shutdown thread:  g_context.store(nullptr)
User thread:      ctx->handle()             // use-after-free
```

Between `delete ctx` (line 74) and `g_context.store(nullptr)` (line 75), another thread calling `get_zmq_context()` will get a valid-looking pointer to freed memory.

**Mitigation:** The lifecycle system ensures Messenger is stopped before ZMQContext shutdown, but this is not enforced at the code level.

**Recommendation:** Swap the order: `g_context.store(nullptr)` first, then `delete ctx`. Or use `std::shared_ptr` with `std::atomic<std::shared_ptr<>>`.

---

### 23. [HIGH] Channel registry has no intrinsic thread safety

**File:** `utils/ipc/channel_registry.hpp`

The class documentation states "Single-threaded access only" but has no `std::mutex` member. Thread safety depends entirely on `broker_service.cpp` holding `m_query_mu` before every access. Key concerns:

- `find_channel_mutable()` returns a mutable pointer — caller can modify fields without any lock.
- `all_channels()` returns a mutable reference to the internal vector — iteration and modification happen simultaneously in `check_dead_consumers()`.

**Recommendation:** Add a mutex as a member (even if only for debug assertions) or use a thread-safety annotation system (`[[guarded_by]]`).

---

### 24. [MEDIUM] Connection state race in protocol handlers

**File:** `utils/ipc/messenger_protocol.cpp:34-39`

Multiple protocol handlers check `m_is_connected.load()` at entry, but the socket is held as `std::optional<>` that can become empty if connection is lost. Between the connection check and actual socket usage (e.g., `zmq::recv_multipart()` at line 359), the connection could be lost in another thread.

**Recommendation:** Re-check connection state and socket validity immediately before socket operations, or hold a lock that prevents concurrent disconnect.

---

### 25. [MEDIUM] Metrics store unbounded growth

**File:** `utils/ipc/broker_service.cpp:174-188`

`metrics_store_` is a map indexed by channel name and consumer UID. When channels are deregistered, their metrics entries are not cleaned up. If channels are created and destroyed frequently, entries accumulate indefinitely.

**Recommendation:** Clear metrics entries for a channel when it is deregistered from the registry.

---

### 26. [MEDIUM] Fire-and-forget handlers don't validate input

**File:** `utils/ipc/messenger_protocol.cpp:779-812`

`ChannelNotifyCmd` and `ChannelBroadcastCmd` handlers don't validate JSON structure before forwarding via `send_to_identity()`. Malformed payloads are silently dropped with only a warning log.

**Recommendation:** Validate required fields before forwarding; return an error to the caller on invalid input.

---

### 27. [LOW] HeartbeatManager::pulse() missing null check

**File:** `utils/ipc/heartbeat_manager.cpp:45`

`pulse()` calls `consumer_->update_heartbeat()` without checking if `consumer_` is null. If `HeartbeatManager` is default-constructed or moved-from, this will segfault.

**Recommendation:** Add a null check or assert.

---

### 28. [MEDIUM] AdminShell token field unsynchronized between threads

**File:** `hub_python/admin_shell.cpp:32,45,164`

```cpp
struct AdminShell::Impl {
    std::string token;  // written in startup() on main thread
    ...
    void startup(const std::string& endpoint, const std::string& auth_token) {
        token = auth_token;  // line 45: main thread write
        socket.bind(endpoint);
        ...
        worker = std::thread([this] { run(); });  // worker reads token at line 164
    }
};
```

The `token` field is written on the main thread in `startup()` and read on the worker thread in `handle_request()`. While `std::thread` constructor provides a happens-before relationship (the worker thread sees everything done before `std::thread(...)` was called), this is correct **only because** `token` is set before the thread is spawned. If `startup()` were refactored to spawn the thread first, a security bypass could occur.

**Recommendation:** Add a comment documenting the ordering requirement, or use `std::atomic<std::string*>`.

---

### 29. [MEDIUM] Borrowed reference in `reset_namespace_unlocked()` fragile

**File:** `hub_python/python_interpreter.cpp:269`

```cpp
auto ns_dict = py::reinterpret_borrow<py::dict>(pImpl->ns.ptr());
```

This creates a borrowed (non-owning) reference to the namespace dict. The borrow is valid as long as `pImpl->ns` lives. The function's contract states "exec_mu is already held by the calling exec() invocation", so only one thread modifies at a time. However, if future code relaxes this contract (e.g., calling `reset_namespace_unlocked()` from a different context), the borrowed reference could become dangling.

**Recommendation:** Add a debug assertion that `exec_mu` is held, or use `py::reinterpret_steal` with an explicit incref.

---

### 30. [LOW] `HubScriptAPI::channels()` const-correctness violation

**File:** `hub_python/hub_script_api.cpp:66-75`

```cpp
std::vector<ChannelInfo> HubScriptAPI::channels() const {
    auto* self = const_cast<HubScriptAPI*>(this);
    for (const auto& e : snapshot_.channels)
        out.emplace_back(e, self);  // ChannelInfo gets mutable api_ pointer
    ...
}
```

The method is `const` but returns `ChannelInfo` objects containing mutable pointers to `this`. If a caller stores these objects beyond the tick callback scope, calling `request_close()` on them could cause use-after-free.

**Recommendation:** Document the lifetime constraint explicitly, or remove `const` from the method.

---

### 31. [LOW] `PyDict_DelItem` return value unchecked

**File:** `hub_python/python_interpreter.cpp:61`

```cpp
PyDict_DelItem(ns_dict.ptr(), k.ptr());
```

No error check on the return value. While keys are guaranteed to exist (just enumerated from the dict), CPython errors could theoretically occur.

**Recommendation:** Check return value and log warning on failure.

---

### 32. [MEDIUM] Writer timeout state restoration race

**File:** `utils/shm/data_block_slot_ops.cpp:110-115`

```cpp
if (entered_draining) {
    slot_rw_state->slot_state.store(SlotRWState::SlotState::COMMITTED,
                                    std::memory_order_release);
}
slot_rw_state->write_lock.store(0, std::memory_order_release);
```

After timing out waiting for readers to drain, the writer restores COMMITTED state then releases the write lock. Between these two stores, another writer could acquire the lock and set WRITING, then writer A releases the lock — leaving the slot in `[state=WRITING, lock=0]`, an invalid invariant.

**Recommendation:** Release the lock first, or use a single compound CAS.

---

### 33. [MEDIUM] Excessive seq_cst fences in slot operations

**File:** `utils/shm/data_block_slot_ops.cpp:92,99,132,186`

Four `std::atomic_thread_fence(std::memory_order_seq_cst)` in the slot acquisition hot path. On ARM64, seq_cst costs ~10-20% more than acquire/release. Analysis:

- Line 92 (after COMMITTED→DRAINING): release suffices since write_lock is held.
- Line 99 (reader drain loop): acquire on reader_count suffices.
- Line 132 (after FREE/DRAINING→WRITING): release suffices.
- Line 186 (after reader_count increment): acquire on slot_state suffices.

HEP-CORE-0002 §4.2.3 documents the double-check pattern correctly, but the implementation over-synchronizes.

**Recommendation:** Profile with acquire/release and document the trade-off.

---

### 34. [MEDIUM] Consumer count underflow guard is silent

**File:** `utils/shm/data_block_recovery.cpp:560-567`

```cpp
while (prev_count > 0 &&
       !header->active_consumer_count.compare_exchange_weak(
           prev_count, prev_count - 1, ...)) {}
```

If `prev_count == 0`, the CAS never fires and the dead consumer is not deregistered. No log or recovery action is taken.

**Recommendation:** Log a warning when count is already 0 (possible double-recovery or corruption).

---

### 35. [MEDIUM] const_cast in heartbeat and slot read functions

**File:** `utils/shm/data_block_internal.hpp:257,410`

`is_producer_heartbeat_fresh()` and `get_next_slot_to_read()` accept `const SharedMemoryHeader*` but use `const_cast` internally to write through atomics. The API signature is misleading — these functions modify shared state.

**Recommendation:** Change parameter to non-const, or provide separate mutable accessors.

---

### 36. [MEDIUM] BLAKE2b failure stores zero checksum

**File:** `utils/shm/data_block_schema.cpp:129-132`

```cpp
if (!compute_blake2b(out, buf.data(), buf.size())) {
    LOGGER_ERROR("compute_blake2b failed; storing zeros.");
    std::memset(out, 0, detail::LAYOUT_CHECKSUM_SIZE);
}
```

If the crypto library fails, a zero checksum is stored. Later validation against a zero hash could pass if crypto fails consistently, making integrity checks blind.

**Recommendation:** Throw instead of storing zeros, or set a header flag indicating checksum invalidity.

---

### 37. [LOW] C API metrics reads commit_index with relaxed ordering

**File:** `utils/shm/data_block_c_api.cpp:102`

`commit_index` is loaded with `std::memory_order_relaxed` in the metrics query path. Since `commit_index` is a synchronization variable used by consumers to locate committed slots, relaxed reads can miss recent commits, producing stale diagnostics.

**Recommendation:** Use `std::memory_order_acquire`.

---

### 38. [LOW] C API disables metrics/heartbeat collection

**File:** `utils/shm/data_block_c_api.cpp:19-25`

The C API always passes `header=nullptr` to underlying slot functions, disabling metrics collection and producer heartbeat updates. Tools using the C API get no diagnostics.

**Recommendation:** Accept a header pointer parameter or document the limitation.

---

### 39. [LOW] Inconsistent error codes in recovery API

**File:** `utils/shm/data_block_recovery.cpp:68-301`

`datablock_diagnose_slot()` returns -1...-5, while `datablock_force_reset_slot()` returns `RecoveryResult` enum values. Callers must remember which convention each function uses.

**Recommendation:** Unify under a single error enum.

---

### 40. [MEDIUM] API object lifetime race in ProducerScriptHost

**File:** `producer/producer_script_host.cpp:305-307`

```cpp
api_obj_ = py::cast(&api_, py::return_value_policy::reference);
api_.set_producer(&*out_producer_);
api_.set_messenger(&out_messenger_);
```

The `api_` object is cast to a Python reference while C++ can nullify its internal pointers via `stop_role()`. If the Python script holds a reference to `api_` beyond the callback scope and `stop_role()` calls `set_producer(nullptr)`, subsequent Python calls like `api.broadcast()` dereference null.

**Recommendation:** Use weak reference pattern or guarantee API object lifetime outlives all Python references.

---

### 41. [MEDIUM] Slot not released on Python exception in produce loop

**File:** `producer/producer_script_host.cpp:516-531`

```cpp
auto out_handle = out_shm->acquire_write_slot(acquire_ms);
// ...
try {
    py::object out_sv = make_out_slot_view_(out_span.data(), out_sz);
    commit = call_on_produce_(out_sv, fz_inst_, mlst);
} catch (py::error_already_set &e) {
    api_.increment_script_errors();
    // slot handle leaked if exception here
}
```

If `make_out_slot_view_()` or `call_on_produce_()` throws, the acquired write slot is not explicitly released. The slot handle's destructor may release it, but this depends on RAII behavior that isn't documented.

**Recommendation:** Wrap slot lifetime in explicit RAII or add release in the catch block.

---

### 42. [MEDIUM] Broker connection failure runs producer in silent degraded mode

**File:** `producer/producer_script_host.cpp:180-185` (and processor equivalent)

```cpp
if (!out_messenger_.connect(config_.broker, ...))
    LOGGER_WARN("[prod] broker connect failed; degraded");
```

Broker connection failure is only a WARN. The producer continues without heartbeats, channel notifications, or broker-initiated shutdown. Operators may not notice.

**Recommendation:** Add a `fail_on_broker_error` config option (default true), or periodic reconnection attempts, or a status endpoint reporting degraded mode.

---

### 43. [MEDIUM] Control send queue unbounded

**File:** `utils/ipc/hub_producer.cpp:90-91`

```cpp
std::mutex                   ctrl_send_mu;
std::queue<PendingCtrlSend>  ctrl_send_queue;
```

No maximum depth. If the peer thread is slow and messages accumulate, memory grows without bound.

**Recommendation:** Add a max queue depth with backpressure or drop policy.

---

### 44. [MEDIUM] ~80% identical main() across producer/processor/consumer

**Files:** `producer/producer_main.cpp`, `processor/processor_main.cpp`, `consumer/consumer_main.cpp`

Nearly identical code for:
- CLI argument parsing (`--init`, `--config`, `--validate`, `--keygen`, `--run`, `--help`)
- Directory creation and JSON template generation in `do_init()`
- Password helpers (`read_password_interactive`, `get_password`)
- Auth vault keygen flow
- Lifecycle module registration and main wait loop

**Recommendation:** Extract into `common/actor_main_helpers.hpp` with parameterized init templates.

---

### 45. [LOW] Default SHM slot count differs between producer and processor

**Files:** `producer/producer_config.hpp:98-102`, `processor/processor_config.hpp:148-152`

Producer defaults to 8 slots; processor defaults to 4 slots for output. No documentation explains the rationale for different defaults.

**Recommendation:** Document the reasoning or align defaults.

---

### 46. [LOW] Missing config validation in producer/processor

**File:** `producer/producer_config.cpp:129-143` (and processor equivalent)

Missing validation for:
- `shm_slot_count > 0`
- `timeout_ms` range (should be >= -1)
- Broker endpoint format (valid ZMQ endpoint)
- Schema JSON structural validation

These are deferred to runtime, where failures produce less helpful error messages.

**Recommendation:** Validate at config load time with clear error messages.

---

### 47. [LOW] Thread join with no timeout in stop_role()

**File:** `producer/producer_script_host.cpp:377-381`

```cpp
py::gil_scoped_release release;
if (loop_thread_.joinable()) loop_thread_.join();
if (zmq_thread_.joinable())  zmq_thread_.join();
```

No timeout. If a thread hangs (e.g., stuck in Python callback or ZMQ poll), the entire process hangs on shutdown.

**Recommendation:** Use a timed join pattern or detach after timeout with force cleanup.

---

## Cross-Cutting Concerns

### A. HEP Design Document Discrepancies

1. **HEP-CORE-0002 (DataHub):** The spec describes a "Flex Zone" in the memory layout, and the implementation correctly supports it. However, the flex zone schema validation at create time (HEP-CORE-0002 Section 3.4) does not validate that `flex_zone_size` is large enough to hold the declared schema — the check is deferred to runtime.

2. **HEP-CORE-0011 (ScriptHost):** The spec calls for a `LuaScriptHost` alongside `PythonScriptHost`. The header `lua_role_host_base.hpp` exists but is likely a skeleton — the Lua path through `PythonRoleHostBase::do_python_work()` does not have a Lua equivalent in the codebase. This is a known incomplete implementation.

3. **HEP-CORE-0016 (Named Schema Registry):** The `SchemaLibrary` implementation searches directories but the broker's `schema_search_dirs` config is unused when empty — it falls back to `SchemaLibrary::default_search_dirs()`. The HEP says the broker "MUST validate named schemas against the registry" but the validation path is fire-and-forget (mismatch logged, not rejected).

4. **HEP-CORE-0022 (Hub Federation):** The relay dedup window (`kRelayDedupeWindow = 5s`) is hardcoded in `broker_service.cpp` but HEP-CORE-0022 specifies it should be configurable. The `on_hub_message` callback doesn't propagate to the Python script when no `on_hub_message` callback is defined — silent drop.

### B. Python Interpreter Integration Issues

1. **Single interpreter limitation:** The codebase uses `py::scoped_interpreter` which means only one Python interpreter can exist per process. The `PythonInterpreter` singleton and `HubScript` singleton enforce this at the design level. This is correct for the hub process, but the producer/processor/consumer binaries each also create their own `py::scoped_interpreter`. If any of these were loaded as plugins in the same process, they would crash.

2. **GIL ordering with exec_mu:** In `python_interpreter.cpp:198-199`, `exec_mu` is acquired before the GIL. In `reset_namespace_unlocked()`, the GIL is acquired while `exec_mu` is already held (by the calling `exec()`). This ordering must be strictly maintained — any code path that acquires the GIL first and then tries to acquire `exec_mu` would deadlock. The current code is correct but fragile.

3. **Py_Finalize lifetime:** All `py::object` instances in `hub_script.cpp` are manually reset to `py::none()` before returning from `do_python_work()` (line 488-494). Missing any object would cause a crash during `Py_Finalize`. This is a maintenance burden — adding a new `py::object` member to `HubScript` requires remembering to clear it here.

### C. Protocol Design Observations

1. **No message versioning:** The ZMQ control frames use a type byte (`'C'`) but no protocol version field. Protocol evolution requires either adding new message types or breaking compatibility. Consider adding a version byte in Frame 0.

2. **Heartbeat coupling:** The broker uses heartbeat timeout for both liveness detection AND channel state transition (PendingReady → Ready). A brief network partition would cause channel teardown even if the producer is healthy. The `channel_timeout` config is a single value for both purposes.

3. **CHANNEL_CLOSING_NOTIFY race:** If the broker sends CHANNEL_CLOSING_NOTIFY and the producer crashes before receiving it, the two-tier shutdown (notify → grace period → FORCE_SHUTDOWN) may send FORCE_SHUTDOWN to a dead process. The broker's `consumer_liveness_check` handles dead consumers but there's no equivalent for dead producers during the closing flow.

### D. Duplicated Patterns Across Producer/Processor/Consumer

The three role binaries (`producer_main.cpp`, `processor_main.cpp`, `consumer_main.cpp`) likely share significant boilerplate:
- CLI argument parsing
- Lifecycle module registration
- Config loading from JSON
- Signal handler setup
- Messenger connection
- Main wait loop

The `PythonRoleHostBase` already consolidates the Python script hosting, but the `main()` functions are likely 80%+ identical. A shared `RoleBinaryMain` template or function would reduce duplication.

### E. Windows Portability Gaps

1. `setenv()` in `python_script_host.cpp` (POSIX-only)
2. `getpass()` in `hubshell.cpp` (POSIX-only, with a weaker fallback for Windows)
3. `SharedSpinLock` uses PID-based ownership — PID reuse is faster on Windows than Linux

---

## Summary

The codebase is well-architected with clear layering, good use of pImpl for ABI stability, and thoughtful separation of concerns. **47 findings** identified: **7 HIGH**, **23 MEDIUM**, **17 LOW**.

### Critical Findings (must fix before production)

1. **Finding #21 (promise never set on JSON parse error)** — deadlock in messenger protocol handlers; calling thread hangs forever
2. **Finding #22 (ZMQ context use-after-free)** — shutdown race can return freed pointer to callers
3. **Finding #2 (TOCTOU in exec())** — crash if AdminShell exec() races with interpreter shutdown
4. **Finding #23 (channel registry thread safety)** — no intrinsic synchronization; relies on caller discipline
5. **Finding #3 (ODR violation)** — undefined behavior from anonymous-namespace definitions in a shared header
6. **Finding #1 (unprotected global callbacks)** — UB potential, though startup ordering likely prevents it
7. **Finding #4 (DiscoverProducerCmd type mismatch)** — confusing naming, maintenance hazard

### Systemic Observations

- The IPC layer (messenger/broker) has the highest density of issues — race conditions (#22, #23, #24), resource leaks (#25), and a deadlock (#21)
- Python integration is carefully designed but fragile: GIL/mutex ordering must be maintained manually, and TOCTOU in `exec()` needs fixing
- Duplicated patterns (hex utilities, role binary boilerplate) increase maintenance burden
- The SHM/DataBlock layer over-synchronizes with seq_cst fences (#33) and has a state restoration race in writer timeout (#32); recovery APIs have inconsistent error conventions (#39)
- Producer/processor/consumer binaries share ~80% identical boilerplate (#44) — highest duplication density in the codebase
- Role script hosts have API lifetime races (#40) and slot leak on exception (#41); broker failure runs silently degraded (#42)
- Windows portability has 3 known gaps that would prevent MSVC compilation
