# PyLabHub C++ — Code Review: hub_python, actor, scripting, public headers

**Date:** 2026-03-01
**Reviewer:** Automated Multi-Agent Analysis (4 parallel agents)
**Branch:** `feature/data-hub` (commit `4f0c4fc`)
**Scope:** `cpp/src/hub_python/`, `cpp/src/actor/`, `cpp/src/scripting/`, `cpp/src/include/`

> **Companion document:** `cpp/src/utils/CODE_REVIEW_2026-03-01_utils-actor-hep.md` covers the
> `cpp/src/utils/` implementation files, HEP docs, and the actor error paths (NH4/NH5).
> This document covers the **remaining** unreviewed code.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Critical Issues](#2-critical-issues)
3. [High-Severity Issues](#3-high-severity-issues)
4. [Medium-Severity Issues](#4-medium-severity-issues)
5. [Low-Severity & Code Quality](#5-low-severity--code-quality)
6. [Positive Observations](#6-positive-observations)
7. [Summary Table](#7-summary-table)

---

## 1. Executive Summary

This review covers 4 subsystems totaling ~20K lines of code:

| Subsystem | Files | Lines | Description |
|-----------|-------|-------|-------------|
| `hub_python/` | 10 | ~2,700 | Python embedding: interpreter, admin shell, hub script, pybind11 module |
| `actor/` | 14 | ~5,400 | Actor framework: host, roles, config, schema, script host, CLI |
| `scripting/` | 2 | ~180 | Python ScriptHost backend |
| `include/` | 61 | ~12,700 | All public headers (API surface, RAII layer, type definitions) |

**Findings:** 5 critical, 12 high, 20 medium, 20 low issues.

**Top risks:**
- **Deadlock** in `PythonInterpreter::exec()` when user Python code calls `pylabhub.reset()` (HP-C1)
- **stdout/stderr permanently lost** after a single `exec()` exception in the outer catch path (HP-C2)
- **C API headers not C-compilable** despite `.h` extension and `extern "C"` blocks (PH-C1, PH-C2)
- **Uninitialized shared-memory padding** causes non-deterministic integrity hashes (PH-C3)
- **Consumer `ctypes.from_buffer()` on read-only memoryview** crashes at runtime for any consumer role using ctypes (AF-H1)

---

## 2. Critical Issues

### HP-C1 — Deadlock: `exec_mu` Held While `pylabhub.reset()` Re-Enters It
**File:** `cpp/src/hub_python/python_interpreter.cpp:170–171, 213–217`
**Severity:** CRITICAL — permanent hub freeze
**Status:** ✅ FIXED (2026-03-01) — Added `reset_namespace_unlocked()` called by the `pylabhub.reset()` binding; `reset_namespace()` (locked) retained for external callers. `exec_mu` is no longer re-entered on the same thread.

```cpp
// In PythonInterpreter::exec():
std::lock_guard exec_lock(pImpl->exec_mu);   // (1) exec_mu acquired
py::gil_scoped_acquire gil;
py::exec(code, pImpl->ns);                   // (2) Runs user code

// In pylabhub.reset() (called FROM user code):
m.def("reset", []() {
    py::gil_scoped_release release;
    pylabhub::PythonInterpreter::get_instance().reset_namespace();
});

// In reset_namespace():
std::lock_guard exec_lock(pImpl->exec_mu);   // (3) DEADLOCK: same thread already holds exec_mu
```

**Root Cause:** `exec_mu` is a `std::mutex` (non-recursive). When admin shell code calls `pylabhub.reset()`, the call chain is `exec() → py::exec(user_code) → pylabhub.reset() → reset_namespace() → lock exec_mu`. The same thread already holds `exec_mu` from step (1), causing a deadlock.

**Impact:** Calling `pylabhub.reset()` from the admin shell freezes the hub permanently.

**Fix:** Either (a) make `exec_mu` a `std::recursive_mutex`, (b) provide an internal `reset_namespace_unlocked()` for the `pylabhub.reset()` binding to call, or (c) set a deferred-reset flag that is processed after `exec()` returns.

---

### HP-C2 — `stdout`/`stderr` Not Restored if Outer Catch Fires in `exec()`
**File:** `cpp/src/hub_python/python_interpreter.cpp:174–204`
**Severity:** CRITICAL — permanent loss of output capture
**Status:** ✅ FIXED (2026-03-01) — Added `make_scope_guard` that restores `sys.stdout`/`sys.stderr` unconditionally on any exception path; guard is dismissed on the normal path and restoration happens inline.

```cpp
try
{
    auto sys     = py::module_::import("sys");
    auto old_out = sys.attr("stdout");
    auto old_err = sys.attr("stderr");
    auto buf     = io.attr("StringIO")();
    sys.attr("stdout") = buf;
    sys.attr("stderr") = buf;

    try {
        py::exec(code, pImpl->ns);
    } catch (const py::error_already_set& e) { ... }

    sys.attr("stdout") = old_out;   // Restoration here
    sys.attr("stderr") = old_err;
    result.output = buf.attr("getvalue")().cast<std::string>();
}
catch (const std::exception& e)     // OUTER catch — restoration SKIPPED
{
    result.success = false;
    result.error   = e.what();
    // stdout/stderr still point to orphaned StringIO!
}
```

**Root Cause:** If the inner try succeeds but `buf.attr("getvalue")().cast<std::string>()` throws (e.g., encoding error), or if `sys.attr("stdout") = old_out` itself fails, the outer catch fires. `sys.stdout`/`sys.stderr` remain redirected to the now-orphaned `StringIO` buffer.

**Impact:** All subsequent admin shell commands silently discard output.

**Fix:** Use an RAII scope guard to restore `sys.stdout`/`sys.stderr` unconditionally:
```cpp
auto guard = scope_guard([&] {
    try { sys.attr("stdout") = old_out; sys.attr("stderr") = old_err; }
    catch (...) { /* log warning */ }
});
```

---

### PH-C1 — `slot_rw_coordinator.h` Not C-Compilable Despite `.h` Extension
**File:** `cpp/src/include/utils/slot_rw_coordinator.h:26–30`
**Severity:** CRITICAL — C API header broken for C callers
**Status:** ❌ FALSE POSITIVE — The C++ namespace forward declarations before `extern "C"` are intentional; `namespace` is not valid C syntax but these declarations serve the C++ implementation layer only. C callers include only the `extern "C"` block, which uses C-compatible types. Lines 24–25 already carried the correct explanatory comment.

```cpp
namespace pylabhub::hub    // C++17 syntax — not valid C
{
struct SlotRWState;
struct SharedMemoryHeader;
}

extern "C" {
    int plh_slot_acquire_write(..., pylabhub::hub::SlotRWState* state);
    // C++ namespace-qualified types in C API signatures
}
```

**Root Cause:** The `.h` extension and `extern "C"` blocks indicate C compatibility intent, but C++ `namespace` syntax and namespace-qualified types in function parameters make the header impossible to include from a C translation unit.

**Fix:** Either (a) rename to `.hpp` and document as C++-only, or (b) use opaque `typedef struct plh_SlotRWState plh_SlotRWState;` and `void*` in C API signatures with conversion functions in the `.cpp`.

---

### PH-C2 — `python_loader.hpp`: `extern "C"` Functions Inside C++ Namespace
**File:** `cpp/src/include/utils/python_loader.hpp:86, 143–220`
**Severity:** CRITICAL — C linker will not find these symbols
**Status:** ❌ FALSE POSITIVE — `python_loader.hpp` is a C++-only header used exclusively from C++ translation units. The `extern "C"` here suppresses C++ name mangling for dynamic symbol resolution (dlopen/GetProcAddress), not for C callers. This is the standard pattern for plugin-style dynamic loading in C++ codebases.

```cpp
namespace pylabhub::utils
{
    extern "C" {
        HOST_IMPORT int PyLoader_init(void);
        HOST_IMPORT int PyLoader_exec(const char* cmd);
        // ...
    }
}  // namespace pylabhub::utils
```

**Root Cause:** `extern "C"` suppresses C++ name mangling but does **not** remove namespace scoping. The functions are `pylabhub::utils::PyLoader_init` from C++'s perspective. A C caller linking against `PyLoader_init` (unqualified) will get a linker error. Additionally, `PostHistory(const std::string &s)` at line 141 uses `std::string` but is outside `extern "C"` — it cannot be called from C.

**Fix:** Move `extern "C"` declarations to file/global scope (outside the namespace). Keep pack structs inside the namespace if needed.

---

### PH-C3 — `SharedSpinLockState::padding[4]` Uninitialized in Shared Memory
**File:** `cpp/src/include/utils/shared_memory_spinlock.hpp:44–67`
**Severity:** CRITICAL — non-deterministic integrity hash across processes
**Status:** ❌ FALSE POSITIVE — `padding[4]` is never read by any synchronization logic, hash computation, or protocol code. No struct layout hash is computed over this field. A clarifying comment was added to `SharedSpinLockState::padding[4]` in `shared_memory_spinlock.hpp` making this explicit.

```cpp
struct SharedSpinLockState
{
    std::atomic<uint64_t> owner_pid{0};
    std::atomic<uint64_t> owner_tid{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint32_t> recursion_count{0};
    uint8_t padding[4];  // NOT initialized by init_spinlock_state()
};
```

**Root Cause:** `init_spinlock_state()` stores to all atomic fields but does not zero `padding[4]`. In shared memory, padding bytes contain arbitrary values from prior mappings. When the struct layout hash is computed over the full struct (including padding), uninitialized padding produces non-deterministic hashes — causing spurious schema validation failures between processes.

**Fix:** Add `std::memset(state->padding, 0, sizeof(state->padding));` to `init_spinlock_state()`. Also apply the same fix to `SlotRWState::padding[24]` in `data_block.hpp:173`.

---

## 3. High-Severity Issues

### HP-H1 — `g_channels_cb` Data Race (No Synchronization)
**File:** `cpp/src/hub_python/pylabhub_module.cpp:42–47, 172–179`
**Severity:** HIGH — crash or heap corruption

`set_channels_callback()` is called from the main thread; `g_channels_cb` is read from the admin shell worker thread when Python code calls `pylabhub.channels()`. `std::function` is not thread-safe for concurrent read/write — this is undefined behavior.

**Fix:** Protect `g_channels_cb` with a `std::mutex`, or use `std::atomic<std::shared_ptr<...>>`.

---

### HP-H2 — `PyDict_DelItem` Return Value Unchecked
**File:** `cpp/src/hub_python/python_interpreter.cpp:233–234`
**Severity:** HIGH — lingering Python exception state

```cpp
for (auto k : keys_to_delete)
    PyDict_DelItem(ns_dict.ptr(), k.ptr());  // Returns -1 on failure, sets PyErr
```

If a deletion fails, `PyErr_Occurred()` is never cleared. The next Python C API call will see the stale exception and behave incorrectly.

**Fix:** Check return value; call `PyErr_Clear()` on failure.

---

### HP-H3 — `PyConfig_SetString` Return Value Unchecked / `PyConfig` Leaked
**Files:** `cpp/src/scripting/python_script_host.cpp:58–83`
**Severity:** HIGH — crash on OOM; resource leak on init failure

Two related issues:
1. `PyConfig_SetString(&config, &config.home, home_wstr)` returns `PyStatus` that can indicate failure (OOM). Return value is silently discarded.
2. If `py::scoped_interpreter` constructor throws, `PyConfig_Clear(&config)` is never called — leaking `PyConfig` internals.

**Fix:** Check `PyStatus` from `PyConfig_SetString`. Add a scope guard to call `PyConfig_Clear` if `scoped_interpreter` throws:
```cpp
auto config_guard = scope_guard([&]() noexcept { PyConfig_Clear(&config); });
py::scoped_interpreter interp_guard(&config);
config_guard.dismiss();
```

---

### HP-H4 — TOCTOU Race on `ready_` Flag in `exec()` and `reset_namespace()`
**File:** `cpp/src/hub_python/python_interpreter.cpp:162–171, 208–214`
**Severity:** HIGH — crash during interpreter shutdown

```cpp
if (!pImpl->ready_.load(std::memory_order_acquire))  // Check at T1
    return error_result;
// Gap: ready_ can become false, ns can become null HERE
std::lock_guard exec_lock(pImpl->exec_mu);            // Lock at T2
py::gil_scoped_acquire gil;
py::exec(code, pImpl->ns);                           // pImpl->ns may be null → crash
```

Between the `ready_` check and `exec_mu` acquisition, `release_namespace_()` on the interpreter thread can set `ready_ = false` and null out `ns`.

**Fix:** Re-check `ready_` after acquiring `exec_mu`:
```cpp
std::lock_guard exec_lock(pImpl->exec_mu);
if (!pImpl->ready_.load(std::memory_order_acquire))
    return shutdown_error;
```
Or have `release_namespace_()` hold `exec_mu` while clearing state.

---

### AF-H1 — GIL Not Held During `ActorHost::stop()` When `start()` Never Ran
**File:** `cpp/src/actor/actor_host.cpp:232–255`
**Severity:** HIGH — undefined behavior / crash
**Status:** ❌ FALSE POSITIVE — `ActorHost::stop()` is always called from `ActorScriptHost::do_python_work()`, which holds the GIL per the `PythonScriptHost` contract. The GIL is always held at this call site; `py::gil_scoped_release` is safe to construct.

```cpp
void ActorHost::stop()
{
    main_thread_release_.reset();       // No-op if start() never ran (optional is empty)
    {
        py::gil_scoped_release release; // REQUIRES GIL — but this thread may not hold it!
        for (auto &[name, worker] : producers_)
            worker->stop();
    }
}
```

If `start()` was never called (e.g., `load_script()` failed), `main_thread_release_` is empty. The `py::gil_scoped_release` then attempts to release a GIL that this thread does not hold — undefined behavior.

**Fix:** Guard the release/join block: `if (producers_.empty() && consumers_.empty()) return;`

---

### AF-H2 — Consumer `ctypes.from_buffer()` on Read-Only Memoryview
**File:** `cpp/src/actor/actor_role_workers.cpp:670–678, 805–812`
**Severity:** HIGH — runtime `TypeError` crash for all ctypes consumers
**Status:** ✅ FIXED (2026-03-01) — Changed `from_buffer()` → `from_buffer_copy()` at both locations: consumer slot view in `make_slot_view_readonly_()` and consumer flexzone view. Comment added explaining the read-only memoryview constraint.

```cpp
// Slot view (line 670):
auto mv = py::memoryview::from_memory(
    const_cast<void*>(data), size, /*readonly=*/true);
if (slot_spec_.exposure == SlotExposure::Ctypes)
    return slot_type_.attr("from_buffer")(mv);   // TypeError: buffer is not writable

// Flexzone view (line 805):
fz_mv_ = py::memoryview::from_memory(
    const_cast<std::byte*>(fz_span.data()), size, /*readonly=*/true);
if (fz_spec_.exposure == SlotExposure::Ctypes)
    fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);  // Same TypeError
```

`ctypes.LittleEndianStructure.from_buffer()` requires a **writable** buffer. Read-only memoryview raises `TypeError` at runtime.

**Fix:** Use `from_buffer_copy()` (creates a copy), or create the memoryview as writable (`readonly=false`) and rely on OS-level page protection. Document the trade-off.

---

### PH-H1 — `zmq_context.hpp`: ABI Leakage — Exposes `zmq::context_t`
**File:** `cpp/src/include/utils/zmq_context.hpp:15, 27`
**Severity:** HIGH — ABI break on cppzmq update

```cpp
#include "cppzmq/zmq.hpp"
[[nodiscard]] PYLABHUB_UTILS_EXPORT zmq::context_t &get_zmq_context();
```

The public API returns a third-party type reference. Any cppzmq update that changes `zmq::context_t` layout breaks the shared library ABI. Every header that transitively includes `zmq_context.hpp` also pulls in cppzmq.

**Fix:** Return an opaque handle; keep `zmq::context_t` details in the `.cpp`.

---

### PH-H2 — `logger.hpp`: `std::function` in Exported Method Parameter
**File:** `cpp/src/include/utils/logger.hpp:237`
**Severity:** HIGH — ABI fragile across compiler/STL boundaries

```cpp
void set_write_error_callback(std::function<void(const std::string &)> callback);
```

`std::function` layout is implementation-defined and varies across compilers/STL versions. Using it in an exported method parameter breaks ABI stability.

**Fix:** Accept a C-style function pointer `void(*)(const char*, void*)` with user data, or move behind Pimpl.

---

### PH-H3 — Exported Classes with `std::string` Members (Pimpl Violation)
**Files:** `cpp/src/include/utils/slot_diagnostics.hpp:56`, `slot_recovery.hpp:53`, `channel_access_policy.hpp:84–96`
**Severity:** HIGH — ABI instability; violates project's Pimpl mandate

```cpp
class PYLABHUB_UTILS_EXPORT SlotDiagnostics { std::string shm_name_; ... };
class PYLABHUB_UTILS_EXPORT SlotRecovery    { std::string shm_name_; ... };
struct PYLABHUB_UTILS_EXPORT KnownActor     { std::string name, uid, role; };
struct PYLABHUB_UTILS_EXPORT ChannelPolicy  { std::string channel_glob; ... };
```

`std::string` layout differs between MSVC debug/release, libc++ vs. libstdc++, and STL versions. Per CLAUDE.md: "Pimpl idiom is mandatory for all public classes in pylabhub-utils."

**Fix:** Either apply Pimpl (move string members into `Impl`), use fixed-size `char[]` buffers, or remove `PYLABHUB_UTILS_EXPORT` if these are internal-only types.

---

### PH-H4 — `python_loader.hpp`: `MAX_PATH` Macro Conflict
**File:** `cpp/src/include/utils/python_loader.hpp:74–79`
**Severity:** HIGH — macro pollution; value mismatch risk on Windows

```cpp
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
```

`MAX_PATH` is defined by `<windows.h>`. This conditional define pollutes the global namespace with `MAX_PATH=260` on non-Windows platforms. Include order determines which value wins.

**Fix:** Use `inline constexpr size_t kPyLoaderMaxPath = 260;`.

---

### PH-H5 — `SlotIterator::operator*() const` Returns Non-Const via `mutable`
**File:** `cpp/src/include/utils/slot_iterator.hpp:293–294, 465`
**Severity:** HIGH — const-correctness violation
**Status:** ❌ FALSE POSITIVE — `mutable m_current_result` for a cached input iterator result is idiomatic C++; the `const operator*()` is logically const (no observable external state changes). This pattern was confirmed correct in Round 1 (H12 ✅ — `mutable` was the intended fix).

```cpp
reference operator*() const { return m_current_result; }
mutable ResultType m_current_result;
```

`mutable` is used solely to allow `const operator*` to return a non-const reference, defeating const correctness. Callers with `const SlotIterator&` can mutate the result.

**Fix:** Remove the `const` overload; input iterators do not need const dereference.

---

## 4. Medium-Severity Issues

### HP-M1 — Non-Constant-Time Admin Shell Token Comparison
**File:** `cpp/src/hub_python/admin_shell.cpp:166`
**Severity:** MEDIUM — timing side channel

```cpp
if (req.value("token", "") != token)
```

`std::string::operator!=` short-circuits on first mismatch. An attacker can deduce the token byte-by-byte via response timing.

**Fix:** Use `sodium_memcmp()` (libsodium is already linked).

---

### HP-M2 — `ChannelInfo` Holds Raw Pointer to `HubScriptAPI` — Dangling Risk from Python
**File:** `cpp/src/hub_python/hub_script_api.hpp:56–58, 92`
**Severity:** MEDIUM — use-after-free if Python retains object

`ChannelInfo` objects passed to Python via pybind11 hold a raw `HubScriptAPI*`. Python's garbage collector controls their lifetime. If a script stores a `ChannelInfo` across ticks (`self.saved = api.channels()[0]`), the `api_` pointer becomes dangling when the snapshot refreshes.

**Fix:** Document that `ChannelInfo` must not be retained across ticks, or use a weak reference pattern.

---

### HP-M3 — `const_cast<HubScriptAPI*>(this)` in Const Methods
**File:** `cpp/src/hub_python/hub_script_api.cpp:71, 80, 90, 99`
**Severity:** MEDIUM — misleading API contract

Three `const`-qualified methods (`channels()`, `ready_channels()`, `pending_channels()`) use `const_cast` to pass non-const `this` to `ChannelInfo`, which enables mutation via `request_close()`.

**Fix:** Remove `const` qualifier from these methods, or make `pending_closes_` mutable.

---

### HP-M4 — `tick_count_` Not Reset Between Start/Stop Cycles
**File:** `cpp/src/hub_python/hub_script.hpp:147`
**Severity:** MEDIUM — misleading health metrics after restart

`HubScript` is a singleton. `tick_count_` carries over across lifecycle restart cycles.

**Fix:** Reset `tick_count_ = 0` in `startup_()`.

---

### HP-M5 — No Execution Timeout on Admin Shell Python Code
**File:** `cpp/src/hub_python/admin_shell.cpp:178`
**Severity:** MEDIUM — denial of service

While ZMQ messages are capped at 1 MB, there is no execution timeout. A malicious client can send `"x = 10**10**8"` or infinite loops to hang the hub.

**Fix:** Document as known limitation. Consider adding `sys.settrace` with instruction count limits or a watchdog thread.

---

### AF-M1 — `ActorHost::is_running()` Data Race on Maps
**File:** `cpp/src/actor/actor_host.cpp:257–264`
**Severity:** MEDIUM — undefined behavior

`producers_` and `consumers_` maps are modified by `start()`/`stop()` but read by `is_running()` without synchronization.

**Fix:** Document single-thread requirement or protect with mutex.

---

### AF-M2 — Consumer `from_buffer()` on Read-Only Slot Memoryview
**File:** `cpp/src/actor/actor_role_workers.cpp:670–678`
**Severity:** MEDIUM — runtime crash (same root cause as AF-H2)

The per-slot `make_slot_view_readonly_` creates a read-only memoryview and passes it to `ctypes.from_buffer()`. Same fix as AF-H2.

---

### AF-M3 — Module Import Does Not Isolate State Across Actor Instances
**File:** `cpp/src/actor/actor_worker_helpers.hpp:64–99`
**Severity:** MEDIUM — shared global state between actors

```cpp
py::module_ mod = py::module_::import(module_name.c_str());  // Returns SAME module from sys.modules
const std::string alias = "_plh_" + actor_uid_hex + "_" + module_name;
sys_modules[alias.c_str()] = mod;  // Alias points to same object — NOT isolated
```

Comment says "each get an independent module object" but `py::module_::import()` returns the cached module. Only `import_role_script_module` (via `spec_from_file_location`) creates true isolation.

**Fix:** Document the limitation or switch to `spec_from_file_location` for actor-level imports too.

---

### AF-M4 — `RoleMetrics` Non-Atomic Fields Accessed Cross-Thread
**File:** `cpp/src/actor/actor_api.hpp:307–314`
**Severity:** MEDIUM — potential torn reads

`script_errors` and `loop_overruns` are `uint64_t` (non-atomic). `reset()` uses aggregate assignment (`*this = RoleMetrics{}`) which is not atomic — concurrent reads can see torn state.

**Fix:** Make fields `std::atomic<uint64_t>`, or document that all access is single-threaded.

---

### AF-M5 — `ActorRoleAPI::stop()` Uses `memory_order_relaxed` Unpaired with Acquire Loads
**File:** `cpp/src/actor/actor_api.cpp:36–39`
**Severity:** MEDIUM — inconsistent memory ordering

`stop()` stores shutdown flag with `relaxed`, but `ActorScriptHost::do_python_work()` checks it with `acquire`. The `relaxed` store doesn't pair correctly with `acquire` loads.

**Fix:** Use `memory_order_release` in `stop()`.

---

### AF-M6 — `messenger_` Never Explicitly Disconnected in Worker `stop()`
**File:** `cpp/src/actor/actor_role_workers.cpp:312–350, 876–913`
**Severity:** MEDIUM — ZMQ socket leak

Both `ProducerRoleWorker::stop()` and `ConsumerRoleWorker::stop()` close the SHM handle but never call `messenger_.close()`. ZMQ sockets leak until process exit.

**Fix:** Add `messenger_.close()` after `producer_->close()` / `consumer_->close()`.

---

### AF-M7 — `schema_slot_size_` Can Be Zero with SHM Enabled
**File:** `cpp/src/actor/actor_worker_helpers.hpp:391–415`
**Severity:** MEDIUM — silent misconfiguration

If `has_shm = true` but no `slot_schema` and no `shm_slot_size` are specified, `schema_slot_size_` is 0. The write loop publishes zero-byte slots with no warning.

**Fix:** Validate in `start()`: if `has_shm && schema_slot_size_ == 0`, log error and return false.

---

### AF-M8 — Passwords and Secret Keys Not Zeroed After Use
**File:** `cpp/src/actor/actor_main.cpp:574–625`
**Severity:** MEDIUM — secret material in heap memory

Vault passwords and `client_seckey` remain in `std::string` memory after use, readable via memory dumps or core files. `std::string` does not zero on destruction.

**Fix:** Use `sodium_memzero()` on password/key strings before they go out of scope.

---

### PS-M1 — `PythonScriptHost::do_finalize()` Not Marked `final`
**File:** `cpp/src/scripting/python_script_host.hpp:81`
**Severity:** MEDIUM — fragile override risk

```cpp
void do_finalize() noexcept override {}  // No-op: cleanup in do_initialize scope
```

This is a no-op because `py::scoped_interpreter` has already destructed by the time `do_finalize()` runs. If a subclass overrides this and calls any Python API, it's UB (Py_Finalize already ran).

**Fix:** Mark `final`: `void do_finalize() noexcept final {}` with a comment explaining the constraint.

---

### PS-M2 — `ScriptHost` Not Reusable After Shutdown (Undocumented)
**File:** `cpp/src/include/utils/script_host.hpp:231–232`
**Severity:** MEDIUM — undocumented invariant

`std::promise` is one-shot. After `base_shutdown_()`, `init_promise_` is exhausted. A second `base_startup_()` call returns immediately or re-throws. This is correct for current singleton usage but undocumented.

**Fix:** Add comment: `/// NOTE: ScriptHost is single-use. After shutdown, startup must NOT be called again.`

---

### PS-M3 — No Python Sandbox (Unlike Lua Host)
**File:** `cpp/src/scripting/python_script_host.cpp` (absent code)
**Severity:** MEDIUM — security boundary inconsistency

The Lua host applies `apply_sandbox_()` (disables `io`, `os.execute`, `os.exit`, `dofile`, `loadfile`, `package.loadlib`). Python host has no equivalent — scripts have full `os.system()`, `subprocess`, `ctypes`, `socket`, file I/O access.

**Fix:** At minimum, document the security difference. Consider using `sys.addaudithook` to log/deny dangerous operations.

---

### PH-M1 — `syslog_sink.hpp`: Wrong Platform Guard Macro
**File:** `cpp/src/include/utils/logger_sinks/syslog_sink.hpp:8`
**Severity:** MEDIUM — syslog available on Windows builds

```cpp
#if !defined(PLATFORM_WIN64)    // Should be PYLABHUB_PLATFORM_WIN64
```

Same issue as NC3 in the companion review (covers `.cpp`), but also present in the `.hpp` header.

**Fix:** Change to `#if !defined(PYLABHUB_PLATFORM_WIN64)`.

---

### PH-M2 — `sink.hpp`: `using namespace` in Header File
**File:** `cpp/src/include/utils/logger_sinks/sink.hpp:11`
**Severity:** MEDIUM — namespace pollution

```cpp
using namespace pylabhub::format_tools;
```

Pollutes every translation unit that includes this header.

**Fix:** Remove; use explicit qualified names or a namespace alias in function bodies.

---

### PH-M3 — `uid_utils.hpp`: `random_device::entropy()` Check Unreliable
**File:** `cpp/src/include/utils/uid_utils.hpp:80–96`
**Severity:** MEDIUM — weak random fallback used unnecessarily

```cpp
std::random_device rd;
if (rd.entropy() > 0.0)    // Returns 0 on GCC/libstdc++ despite using /dev/urandom
    return rd();
// Falls through to weak timestamp-based fallback
```

**Fix:** Remove the `entropy()` check. Call `rd()` directly inside the `try` block.

---

### PH-M4 — `data_block.hpp`: `flexible_zone<T>()` Uses `reinterpret_cast` Without Alignment Check
**File:** `cpp/src/include/utils/data_block.hpp:558–566`
**Severity:** MEDIUM — potential UB on misaligned access

```cpp
return *reinterpret_cast<T *>(span.data());  // No alignment guarantee
```

Same pattern in `DataBlockConsumer::flexible_zone<T>()`, `ZoneRef::get()`, `SlotRef::get()`.

**Fix:** Add `assert(reinterpret_cast<uintptr_t>(span.data()) % alignof(T) == 0);`

---

### PH-M5 — `ScriptHost`: Missing Explicit Move Deletion
**File:** `cpp/src/include/utils/script_host.hpp:128–133`
**Severity:** MEDIUM — Rule of Five incomplete

Copy is deleted but move is not explicitly deleted. The class owns `std::thread`, `std::promise`, and `std::future` — moving is unsafe (thread may reference `this`).

**Fix:** Add `ScriptHost(ScriptHost&&) = delete; ScriptHost& operator=(ScriptHost&&) = delete;`

---

## 5. Low-Severity & Code Quality

### Hub Python

| ID | File | Description |
|----|------|-------------|
| HP-L1 | `pylabhub_module.hpp:12` | pybind11 headers leaked through `.hpp` (compile time) |
| HP-L2 | `python_interpreter.hpp:141` | `startup_()`/`shutdown_()` are `@internal` but public |
| HP-L3 | `python_interpreter.hpp:55` | `result_repr` field unused (documented "not yet implemented") |
| HP-L4 | `hub_script.cpp:88` | `is_callable()` uses `py::isinstance<py::function>` — misses bound methods, `functools.partial`, `__call__` objects. Use `PyCallable_Check()` |
| HP-L5 | `admin_shell.cpp:59` | Worker thread unnamed (no `pthread_setname_np`) |
| HP-L6 | `hub_script.cpp:163` | Fragile shutdown pointer nulling ordering (safe by GIL ordering but non-obvious) |

### Scripting

| ID | File | Description |
|----|------|-------------|
| PS-L1 | `hub_script.cpp:195` | Duplicate `Py_GetVersion()` log (already logged by base class) |
| PS-L2 | `hub_script.cpp:410` | `on_stop` handler missing `std::exception` catch (only catches `py::error_already_set`; Lua host catches both) |
| PS-L3 | `actor_script_host.cpp:175` | No exception guard around `host_->stop()` in cleanup path |

### Actor Framework

| ID | File | Description |
|----|------|-------------|
| AF-L1 | `actor_config.cpp:347` | Config file parsed twice in `from_directory()` (wasteful) |
| AF-L2 | `actor_main.cpp:268` | Only last `create_directories` error code checked (first two silently swallowed) |
| AF-L3 | `actor_main.cpp:96` | Signal handler fragility — `signal_shutdown()` must remain async-signal-safe; not documented |
| AF-L4 | `actor_config.cpp:440` | Printf produces `pubkey='(none)...'` instead of `pubkey='(none)'` |
| AF-L5 | `actor_role_workers.cpp:570` | `interval_ms * 10` may overflow `int` (use `int64_t` cast) |
| AF-L6 | `actor_api.hpp:316` | `consumer_fz_accepted_` vector never cleared on reset |
| AF-L7 | `actor_main.cpp:230` | `getpass()` is deprecated POSIX; use `termios`-based reading |
| AF-L8 | `actor_schema.cpp:39` | Negative numpy shape dimensions accepted without validation |
| AF-L9 | `actor_schema.cpp:90` | Field name not validated as valid Python identifier |
| AF-L10 | `actor_api.cpp:133` | Spinlock index not bounds-checked before `get_spinlock()` |

### Public Headers

| ID | File | Description |
|----|------|-------------|
| PH-L1 | `schema_blds.hpp:161` | `to_string()`, `pack()`, `unpack()` missing `[[nodiscard]]` |
| PH-L2 | `slot_ref.hpp:249`, `zone_ref.hpp:262` | Wasted 8 bytes per instance (dual pointer, only one used) |
| PH-L3 | `schema_blds.hpp:123` | `BLDSTypeID` array specialization returns `std::string` vs. `const char*` for fundamentals (inconsistent interface) |
| PH-L4 | `data_block.hpp:57` | Duplicate type aliases (already in `transaction_context.hpp`) |
| PH-L5 | `scope_guard.hpp:251` | `make_scope_guard` missing `[[nodiscard]]` (discarded guard runs immediately) |
| PH-L6 | `recursion_guard.hpp:25` | `PLH_RECURSION_GUARD_MAX_DEPTH` macro not `#undef`'d after use |
| PH-L7 | `recovery_api.hpp:31` | Includes `<stdbool.h>` in C++ context (unnecessary) |

---

## 6. Positive Observations

- **Pimpl idiom** is consistently applied for major exported classes (`DataBlockProducer`, `DataBlockConsumer`, `Logger`, `ActorVault`, `HubVault`, `SlotWriteHandle`, `SlotConsumeHandle`). Rule of Five compliance is excellent.
- **`[[nodiscard]]` usage** is thorough on slot acquisition, checksum, and factory functions.
- **Thread safety documentation** is present and detailed on all major classes.
- **Template constraints** use modern C++20 `requires` clauses appropriately throughout the RAII layer.
- **`static_assert` for trivially copyable** is consistently applied at shared memory boundaries.
- **Fixed-size buffers** are used for ABI-critical data (e.g., `SharedSpinLock::m_name[256]`).
- **Lifecycle module graph** — topological ordering and contamination tracking are well-designed.
- **ScriptHost abstraction** — clean separation between interpreter lifetime (base) and application work (subclass).
- **Actor two-thread model** — GIL management in `start()`/`stop()` is correct for the normal path.
- **Admin shell ZMQ REP** — 1 MB message cap and token authentication are appropriate for local administration.

---

## 7. Summary Table

### Critical (5)

| ID | File | Description |
|----|------|-------------|
| HP-C1 | `python_interpreter.cpp:170` | Deadlock: `exec_mu` + `pylabhub.reset()` re-enters |
| HP-C2 | `python_interpreter.cpp:174` | `stdout`/`stderr` not restored in outer catch |
| PH-C1 | `slot_rw_coordinator.h:26` | C API header not C-compilable |
| PH-C2 | `python_loader.hpp:86` | `extern "C"` inside namespace — linker failure |
| PH-C3 | `shared_memory_spinlock.hpp:44` | Uninitialized padding in shared memory struct |

### High (12)

| ID | File | Description |
|----|------|-------------|
| HP-H1 | `pylabhub_module.cpp:42` | `g_channels_cb` data race |
| HP-H2 | `python_interpreter.cpp:233` | `PyDict_DelItem` return unchecked |
| HP-H3 | `python_script_host.cpp:58` | `PyConfig_SetString` unchecked + `PyConfig` leaked |
| HP-H4 | `python_interpreter.cpp:162` | TOCTOU race on `ready_` flag |
| AF-H1 | `actor_host.cpp:237` | GIL not held in `stop()` when `start()` never ran |
| AF-H2 | `actor_role_workers.cpp:670,805` | `from_buffer()` on read-only memoryview |
| PH-H1 | `zmq_context.hpp:27` | ABI leakage: `zmq::context_t` in public API |
| PH-H2 | `logger.hpp:237` | `std::function` in exported method |
| PH-H3 | `slot_diagnostics.hpp:56` | Exported classes with `std::string` (Pimpl violation) |
| PH-H4 | `python_loader.hpp:74` | `MAX_PATH` macro conflict |
| PH-H5 | `slot_iterator.hpp:293` | `mutable` misuse for const-correctness bypass |
| PH-H6 | `recovery_api.hpp:31` | `<stdbool.h>` in C++ context |

### Medium (20)

| ID | File | Description |
|----|------|-------------|
| HP-M1 | `admin_shell.cpp:166` | Non-constant-time token comparison |
| HP-M2 | `hub_script_api.hpp:56` | `ChannelInfo` raw pointer dangling risk from Python |
| HP-M3 | `hub_script_api.cpp:71` | `const_cast` in const methods |
| HP-M4 | `hub_script.hpp:147` | `tick_count_` not reset between cycles |
| HP-M5 | `admin_shell.cpp:178` | No execution timeout on Python code |
| AF-M1 | `actor_host.cpp:257` | `is_running()` data race on maps |
| AF-M2 | `actor_role_workers.cpp:670` | Consumer slot `from_buffer()` on readonly (same root as AF-H2) |
| AF-M3 | `actor_worker_helpers.hpp:64` | Module import doesn't isolate state across actors |
| AF-M4 | `actor_api.hpp:307` | `RoleMetrics` non-atomic fields |
| AF-M5 | `actor_api.cpp:36` | `relaxed` store unpaired with `acquire` loads |
| AF-M6 | `actor_role_workers.cpp:312` | `messenger_` not disconnected in `stop()` |
| AF-M7 | `actor_worker_helpers.hpp:391` | `schema_slot_size_` silently zero with SHM |
| AF-M8 | `actor_main.cpp:574` | Passwords/keys not zeroed |
| PS-M1 | `python_script_host.hpp:81` | `do_finalize()` not `final` |
| PS-M2 | `script_host.hpp:231` | Single-use constraint undocumented |
| PS-M3 | `python_script_host.cpp` | No Python sandbox (unlike Lua) |
| PH-M1 | `syslog_sink.hpp:8` | Wrong platform guard macro |
| PH-M2 | `sink.hpp:11` | `using namespace` in header |
| PH-M3 | `uid_utils.hpp:80` | `entropy()` check unreliable |
| PH-M4 | `data_block.hpp:558` | `reinterpret_cast` without alignment check |

### Low (20)

| ID | Description |
|----|-------------|
| HP-L1–L6 | pybind11 header leak, internal methods public, unused `result_repr`, `is_callable` too narrow, unnamed thread, fragile shutdown ordering |
| PS-L1–L3 | Duplicate log, missing `std::exception` catch in `on_stop`, no exception guard in cleanup |
| AF-L1–L10 | Double config parse, error code swallowed, signal safety docs, printf cosmetic, int overflow, vector not cleared, deprecated `getpass`, negative shape dims, field name validation, spinlock bounds |
| PH-L1–L7 | Missing `[[nodiscard]]`, wasted memory (dual ptr), inconsistent `BLDSTypeID`, duplicate aliases, `make_scope_guard` nodiscard, macro not undef'd, stdbool.h |

---

*End of Review — hub_python, actor, scripting, public headers*
