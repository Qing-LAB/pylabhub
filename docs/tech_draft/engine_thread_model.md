# Tech Draft: ScriptEngine Execution Model — Threading, Cross-Thread Dispatch, and Native Plugins

**Status**: Draft (2026-03-20)
**Branch**: `feature/lua-role-support`
**Relates to**: HEP-CORE-0011, `script_engine_refactor.md`
**Addresses**: SE-15 (GIL strategy), SE-04 (Lua API parity), SE-06 (entry_point contract),
future ctrl_thread script support, native plugin extension

## 1. Problem Statement

The current ScriptEngine design assumes all script invocations happen on the
owner (working) thread. This works for the data loop but prevents:

1. **Cross-thread script execution** — ctrl_thread or future admin threads
   cannot call script functions (e.g., `on_heartbeat`, `on_admin`, custom
   metrics callbacks).

2. **Multi-state Lua underuse** — LuaEngine supports `create_thread_state()`
   but the caller must manually call `build_api()` with the right context.
   No automatic thread-state management.

3. **Python GIL confusion** — comments say "acquire/release per invoke" but
   the GIL is actually held for the engine lifetime. The design intent for
   cross-thread access is undocumented.

4. **Script-level state sharing** — Lua thread states have independent global
   tables. Python uses module-level variables (fragile, undocumented). No
   proper mechanism for scripts to exchange state across threads.

## 2. Design Principles

1. **The engine is a script execution service.** Callers provide script
   function names or code strings. The engine resolves, dispatches, and
   executes through the appropriate runtime. No C++ callables cross the
   interface boundary.

2. **Thread dispatch is automatic and internal.** The caller calls
   `engine->invoke("on_heartbeat")` from any thread. The engine records the
   owner thread at `initialize()` time and decides how to dispatch:
   - Owner thread: direct call (zero overhead).
   - Lua (multi-state): get or create a thread-local state, call directly
     on the caller's thread.
   - Python (single-state): queue for owner, block until processed.

3. **Thread-local states are fully managed.** For Lua, `create_thread_state()`
   returns a fully initialized engine (init + load_script + build_api with
   the parent's RoleContext). The engine maintains a thread-state cache
   internally. Callers never manage states.

4. **Shared script state via JSON.** All engines expose an atomic JSON state
   at the API level for cross-thread script communication. Lives in
   RoleHostCore (C++ layer), accessible from any thread, any engine type.

## 3. Interface Changes to ScriptEngine

### 3.1 Generic script execution

```cpp
class ScriptEngine
{
public:
    // ── Generic script execution (thread-safe) ──────────────────────

    /// Invoke a named script function with no arguments.
    /// Thread dispatch is automatic (see §4).
    /// Returns false if function not found or engine shutting down.
    virtual bool invoke(const char* function_name) = 0;

    /// Invoke a named script function with JSON arguments.
    /// The engine unpacks args into the script's native format
    /// (Python dict, Lua table) before calling.
    virtual bool invoke(const char* function_name,
                        const nlohmann::json& args) = 0;

    /// Evaluate a script code string. For admin/debug use.
    /// Returns result as JSON, or empty on error.
    virtual nlohmann::json eval(const char* code) = 0;
```

### 3.2 Pending request drain (Python only)

```cpp
    /// Process pending cross-thread requests.
    /// Called by the data loop at safe points (next to drain_inbox_sync).
    /// PythonEngine: drains request queue.
    /// LuaEngine: no-op (cross-thread calls run on caller's thread).
    virtual void drain_pending_requests() {}
```

### 3.3 Existing hot-path methods (unchanged)

The specialized invoke methods remain for the data loop hot path. They deal
with raw memory pointers, slot views, and typed return contracts. They are
always called from the owner thread and bypass the generic dispatch:

```cpp
    // These are NOT changed — owner-thread-only, zero overhead
    virtual void invoke_on_init() = 0;
    virtual void invoke_on_stop() = 0;
    virtual InvokeResult invoke_produce(void* out, size_t out_sz, ...) = 0;
    virtual void invoke_consume(const void* in, size_t in_sz, ...) = 0;
    virtual InvokeResult invoke_process(const void* in, size_t in_sz,
                                         void* out, size_t out_sz, ...) = 0;
    virtual void invoke_on_inbox(const void* data, size_t sz, ...) = 0;
```

### 3.4 Thread model queries (unchanged)

```cpp
    virtual bool supports_multi_state() const noexcept = 0;
    virtual std::unique_ptr<ScriptEngine> create_thread_state() = 0;

protected:
    std::thread::id owner_thread_id_;  // set in initialize()
};
```

## 4. Thread Dispatch Model

### 4.1 Decision tree

```
engine->invoke("on_heartbeat") called from thread T

    Is T the owner thread?
    ├── YES: call script function directly (zero overhead)
    └── NO:
         Does engine support multi-state?
         ├── YES (Lua): get/create thread-local state for T
         │              call script function on T's state directly
         │              (no queuing, no blocking, no owner involvement)
         └── NO (Python): enqueue {function_name, args}
                          notify owner via core_->notify_incoming()
                          block on condition variable
                          owner drains at next safe point
                          caller unblocked with result
```

### 4.2 Owner thread detection

```cpp
bool ScriptEngine::invoke(const char* function_name)
{
    if (std::this_thread::get_id() == owner_thread_id_)
        return invoke_direct_(function_name);       // zero overhead
    return invoke_cross_thread_(function_name);     // virtual, engine-specific
}
```

The `invoke_direct_()` path is identical in cost to calling the script
function today — one function name lookup + pcall/pybind11 call. The
thread ID comparison is a single integer compare (~1ns).

### 4.3 Safe drain point in the data loop

The role host data loop already has a defined safe point where
`drain_inbox_sync()` runs — between inbox message processing and script
callback invocation. `drain_pending_requests()` is called at the same point:

```
    DATA LOOP iteration:
      Step A: acquire slot (may block with short timeout)
      Step B: timing (sleep_until if needed)
      Step C: drain_inbox_sync(inbox, engine)         ← existing
              engine->drain_pending_requests()         ← NEW
      Step D: engine->invoke_produce(slot, ...)        ← script callback
      Step E: commit/discard based on result
```

At Step C, the engine is guaranteed not in a script call. The owner thread
processes any queued cross-thread requests here.

## 5. LuaEngine Thread-State Management

### 5.1 Automatic thread-state lifecycle

```cpp
class LuaEngine : public ScriptEngine
{
    // Thread-state cache: thread_id → fully initialized child engine
    std::unordered_map<std::thread::id,
                       std::unique_ptr<LuaEngine>> thread_states_;
    std::mutex thread_states_mu_;

    bool invoke_cross_thread_(const char* function_name) override
    {
        auto* state = get_or_create_thread_state_();
        return state->invoke_direct_(function_name);
    }

    LuaEngine* get_or_create_thread_state_()
    {
        auto tid = std::this_thread::get_id();
        std::lock_guard lk(thread_states_mu_);
        auto it = thread_states_.find(tid);
        if (it != thread_states_.end())
            return it->second.get();

        // Create fully initialized child state
        auto child = std::make_unique<LuaEngine>();
        child->initialize(log_tag_.c_str(), ctx_.core);
        child->load_script(script_dir_, entry_point_.c_str(),
                           required_callback_.c_str());
        child->build_api(ctx_);     // ← automatic, uses parent's context
        auto* ptr = child.get();
        thread_states_.emplace(tid, std::move(child));
        return ptr;
    }
};
```

### 5.2 What is shared vs independent

| Aspect | Shared (process-wide) | Independent (per state) |
|--------|----------------------|------------------------|
| `RoleHostCore` (metrics, shutdown) | Shared via `ctx_.core` pointer | — |
| Infrastructure (messenger, queues) | Shared via `RoleContext` pointers | — |
| Script code | Same file loaded independently | — |
| Script global variables | — | Each state has own global table |
| Registered types (ffi.typeof cache) | — | Each state builds own ctypes |
| API closures | — | Each state has own api table |

The `RoleContext` is safe to share because all its members are either
immutable after init (identity strings) or thread-safe (RoleHostCore,
Messenger, QueueReader/QueueWriter).

### 5.3 Cleanup

`finalize()` on the parent engine also destroys all child thread states:

```cpp
void LuaEngine::finalize()
{
    {
        std::lock_guard lk(thread_states_mu_);
        thread_states_.clear();  // destroy all child states
    }
    // ... existing cleanup ...
}
```

## 6. PythonEngine Cross-Thread Queue

### 6.1 Request structure

```cpp
struct PendingRequest
{
    std::string          function_name;
    nlohmann::json       args;         // empty if no args
    nlohmann::json       result;       // filled by owner after execution
    bool                 completed{false};
    bool                 success{false};
};
```

### 6.2 Internals

```cpp
class PythonEngine : public ScriptEngine
{
    std::deque<PendingRequest*>  pending_;
    std::mutex                   pending_mu_;
    std::condition_variable      pending_cv_;
    std::atomic<bool>            accepting_{true};

    bool invoke_cross_thread_(const char* function_name) override
    {
        if (!accepting_.load(std::memory_order_acquire))
            return false;

        PendingRequest req;
        req.function_name = function_name;

        {
            std::lock_guard lk(pending_mu_);
            pending_.push_back(&req);
        }

        ctx_.core->notify_incoming();  // wake owner from queue/sleep

        // Block until owner processes this request
        {
            std::unique_lock lk(pending_mu_);
            pending_cv_.wait(lk, [&] {
                return req.completed || !accepting_.load();
            });
        }
        return req.success;
    }

    void drain_pending_requests() override
    {
        std::lock_guard lk(pending_mu_);
        for (auto* req : pending_)
        {
            req->success = invoke_direct_(req->function_name.c_str());
            req->completed = true;
        }
        pending_.clear();
        pending_cv_.notify_all();
    }
};
```

### 6.3 Shutdown sequence

```cpp
void PythonEngine::finalize()
{
    // 1. Stop accepting new requests
    accepting_.store(false, std::memory_order_release);

    // 2. Drain any remaining (process or cancel)
    drain_pending_requests();

    // 3. Wake any blocked waiters (they see accepting_=false, return false)
    pending_cv_.notify_all();

    // 4. Existing cleanup (clear py::objects, destroy interpreter)
    // ...
}
```

### 6.4 Latency characteristics

Cross-thread request latency = time until the owner thread reaches the
drain point. This depends on loop timing:

| Loop policy | Typical drain interval | Max latency |
|-------------|----------------------|-------------|
| MaxRate (period=0) | 1 iteration = acquire + invoke | ~100us-1ms |
| FixedRate (period=10ms) | 1 period | ~10ms |
| FixedRateWithCompensation | 1 period | ~10ms |

This is acceptable for the target use cases (admin commands, heartbeat
callbacks, hot-reload notifications) which are all low-frequency operations.

## 7. Shared Script State (RoleHostCore)

### 7.1 Storage

```cpp
// In RoleHostCore (private)
nlohmann::json shared_state_ = nlohmann::json::object();
mutable std::mutex shared_state_mu_;

// Public API
void set_shared_state(nlohmann::json state);
nlohmann::json get_shared_state() const;
void update_shared_state(const std::string& key, nlohmann::json value);
```

### 7.2 Script-facing API

Exposed in all role API classes (ProducerAPI, ConsumerAPI, ProcessorAPI)
and Lua closures:

```python
# Python — all operations are atomic
api.set_shared_state({"counter": 0, "mode": "warmup"})
state = api.get_shared_state()           # returns deep copy
api.update_shared_state("counter", 42)   # atomic per-key update
```

```lua
-- Lua — same semantics, JSON ↔ Lua table conversion
api.set_shared_state({counter = 0, mode = "warmup"})
local state = api.get_shared_state()
api.update_shared_state("counter", 42)
```

### 7.3 Thread safety contract

- `set_shared_state()` — atomic full replace under mutex
- `get_shared_state()` — returns deep copy under mutex (no dangling refs)
- `update_shared_state()` — atomic single-key merge under mutex
- No partial reads or writes visible to any thread
- Size limit: optional `kMaxSharedStateBytes` (default 64KB) with LOGGER_WARN

### 7.4 Use cases

1. **Worker ↔ ctrl_thread** (Lua): Worker script sets mode/status, ctrl_thread
   script reads it in `on_heartbeat()` to include in heartbeat payload.

2. **Cross-callback state** (Python): Replaces the module-level variable
   pattern. `on_init` sets initial state, `on_produce` reads/updates it.
   Properly documented and API-supported.

3. **Admin interaction**: Admin thread sets a command in shared state, worker
   script picks it up on next iteration.

## 8. Runtime Cost Analysis

### 8.1 Owner-thread invocations (hot path) — ZERO additional cost

Owner-thread calls (the data loop) go through:

```cpp
if (std::this_thread::get_id() == owner_thread_id_)  // ~1ns, integer compare
    return invoke_direct_(function_name);              // identical to current path
```

The `invoke_direct_()` path is the same code as today's `invoke_produce()`,
`invoke_consume()`, etc. The only addition is one thread-ID comparison per
generic `invoke()` call (~1 nanosecond). The specialized hot-path methods
(`invoke_produce`, etc.) are unchanged and bypass this check entirely.

**Net hot-path cost: zero.** Specialized invoke methods are not modified.

### 8.2 Generic invoke() from owner thread — ~1ns overhead

If a script callback is invoked through the generic `invoke("name")` path
from the owner thread:

| Operation | Cost | Notes |
|-----------|------|-------|
| `std::this_thread::get_id()` | ~1ns | Single syscall-free read (glibc caches TLS) |
| Thread ID comparison | ~1ns | Integer compare |
| Function name lookup | ~50-200ns | Hash table lookup in script namespace |
| Script function call | ~1-10us | Depends on script complexity |

The thread-ID check is negligible compared to the script call itself.

### 8.3 Lua cross-thread (multi-state) — first call ~1ms, subsequent ~200ns

| Operation | First call | Subsequent calls |
|-----------|-----------|-----------------|
| Thread-state cache lookup | ~50ns (mutex + hash map) | ~50ns |
| Create child LuaEngine | ~500us (init + load script + build_api) | — (cached) |
| Function name lookup | ~50ns | ~50ns |
| Script function call | ~1-10us | ~1-10us |
| **Total** | **~500us + script** | **~100ns + script** |

After the first call, the thread state is cached. Subsequent calls from
the same thread are near-zero overhead: one mutex lock + hash map lookup +
direct script call.

### 8.4 Python cross-thread (queue) — ~10us + drain latency

| Operation | Cost | Notes |
|-----------|------|-------|
| Push to request queue | ~50ns | mutex lock + deque push |
| `notify_incoming()` | ~200ns | condition variable notify |
| Block on CV | ~drain latency | See §6.4 (100us-10ms) |
| Owner drains: function lookup + call | ~1-10us | Same as direct call |
| CV notify result | ~200ns | Wake caller |
| **Total** | **~drain_latency + script** | Dominated by drain wait |

The actual Python execution cost is identical to today. The overhead is
the queuing round-trip (~500ns) plus the wait for the next drain point.

### 8.5 Shared state operations — ~100-500ns per call

| Operation | Cost | Notes |
|-----------|------|-------|
| `get_shared_state()` | ~100-500ns | Mutex lock + JSON deep copy |
| `set_shared_state()` | ~100-500ns | Mutex lock + JSON move |
| `update_shared_state()` | ~100-300ns | Mutex lock + single key update |

Cost scales with JSON document size. For typical state (10-50 keys,
scalar values), ~200ns per operation.

### 8.6 Comparison with current model

| Scenario | Current model | Proposed model | Delta |
|----------|--------------|---------------|-------|
| Data loop invoke_produce | Direct call | Direct call (unchanged) | 0 |
| Data loop invoke_consume | Direct call | Direct call (unchanged) | 0 |
| Generic invoke from owner | N/A (not supported) | ~1ns + direct call | New capability |
| Cross-thread invoke (Lua) | Not possible | ~100ns + direct call | New capability |
| Cross-thread invoke (Python) | Not possible | ~500ns + drain wait + call | New capability |
| Script state sharing | Module-level vars (Python only) | ~200ns per get/set (all engines) | New capability |

**No regression on existing paths. All new costs are for new capabilities.**

## 9. Integration Points

### 9.1 Role host data loop

One new call per iteration at the drain point:

```cpp
// In run_data_loop_() — right after drain_inbox_sync():
drain_inbox_sync(inbox_queue_.get(), engine_.get(), inbox_type);
engine_->drain_pending_requests();   // ← NEW (no-op for Lua)
```

### 9.2 RoleHostCore

New shared state members (private, mutex-protected):
```cpp
nlohmann::json shared_state_;
mutable std::mutex shared_state_mu_;
```

New public methods:
```cpp
void set_shared_state(nlohmann::json state);
nlohmann::json get_shared_state() const;
void update_shared_state(const std::string& key, nlohmann::json value);
```

### 9.3 Role API classes

Three new methods in each of ProducerAPI, ConsumerAPI, ProcessorAPI
(delegates to `core_->`):
```cpp
void set_shared_state(const nlohmann::json& state);
nlohmann::json get_shared_state() const;
void update_shared_state(const std::string& key, const nlohmann::json& value);
```

Plus pybind11 bindings and Lua closures for each.

### 9.4 LuaEngine

- `create_thread_state()`: add automatic `build_api(ctx_)` call
- New: `thread_states_` cache with `get_or_create_thread_state_()`
- `finalize()`: clear thread state cache before closing parent state

### 9.5 PythonEngine

- New: `pending_` queue, `pending_mu_`, `pending_cv_`, `accepting_`
- New: `drain_pending_requests()` override
- `finalize()`: set `accepting_=false`, drain remaining, notify waiters
- Update header/file-level GIL comments to match this design

## 10. Implementation Phases

**Phase 1** (immediate — SE-15 fix):
Update GIL comments in python_engine.hpp/cpp to describe this design
direction. No code change beyond comments.

**Phase 2** (engine infrastructure):
- Add `owner_thread_id_` to ScriptEngine, set in `initialize()`
- Add `invoke(name)`, `invoke(name, args)`, `eval(code)` to interface
- Add `drain_pending_requests()` virtual (default no-op)
- Implement `invoke_direct_()` in both engines (function name lookup + call)

**Phase 3** (cross-thread dispatch):
- LuaEngine: thread-state cache + automatic `build_api()` in
  `create_thread_state()`
- PythonEngine: request queue + drain implementation
- Add `drain_pending_requests()` call to all 3 role host data loops

**Phase 4** (shared state):
- `RoleHostCore`: shared_state_ + mutex + public methods
- All 3 API classes: `set/get/update_shared_state` + pybind11 bindings
- Lua closures for shared state
- Tests

**Phase 5** (ctrl_thread script support):
- Add optional script callbacks: `on_heartbeat()`, `on_admin(cmd)`
- ctrl_thread calls `engine->invoke("on_heartbeat")` in periodic tasks
- For Lua: automatic thread-local state, direct call
- For Python: queued to worker, processed at next drain point

**Phase 6** (native plugin engine):
- Implement `NativeEngine` (see §11)
- Add `"native"` to config validation
- Plugin API header (`pylabhub/plugin_api.h`)
- Tests: load .so, invoke callbacks, verify zero-copy data access

## 11. NativeEngine — Dynamic C++ Library Extension

### 11.1 Motivation

The ScriptEngine abstraction is really a **loadable code execution service**.
The interface — load, look up entry points by name, invoke with data, manage
lifecycle — applies equally to interpreted scripts and compiled shared
libraries. A `NativeEngine` completes the abstraction by providing a
zero-overhead path for performance-critical roles.

The key insight: the slot data in shared memory IS a C struct. Lua and Python
must marshal it through ffi.cast / ctypes. A native library includes the
same struct header and casts the raw pointer directly — zero conversion cost.

### 11.2 Interface mapping

The existing ScriptEngine interface maps directly to `dlopen`/`dlsym`:

| ScriptEngine method | NativeEngine implementation |
|--------------------|-----------------------------|
| `initialize(tag, core)` | `dlopen(path)` + `dlsym("plugin_init")` → call with core ptr |
| `load_script(dir, entry)` | `dlsym` for all known symbols: `on_produce`, `on_consume`, etc. |
| `register_slot_type(spec, name, packing)` | No-op or validation. Library knows its own struct layout. |
| `build_api(ctx)` | `dlsym("plugin_build_api")` → call with `RoleContext*`. Library stores pointers it needs. |
| `has_callback(name)` | `dlsym(name) != nullptr` |
| `invoke_produce(ptr, sz, ...)` | Direct function pointer call. No marshaling. |
| `invoke("name")` | `dlsym(name)` → function pointer call |
| `invoke("name", args)` | `dlsym(name)` → call with serialized args (JSON string or struct) |
| `eval(code)` | Returns false. Not applicable to compiled code. |
| `finalize()` | `dlsym("plugin_finalize")` → call + `dlclose()` |

### 11.3 Plugin symbol convention

A native plugin is a standard shared library (`.so` / `.dll` / `.dylib`)
exporting C-linkage symbols with a known naming convention:

```cpp
// my_fast_producer.cpp — compiled to libmy_fast_producer.so
#include <pylabhub/plugin_api.h>

// ── Required ────────────────────────────────────────────────────────
extern "C"
{

/// Called once at engine init. Store the context for later use.
/// Return true on success, false to abort.
bool plugin_init(const pylabhub::scripting::RoleContext* ctx);

/// Called once at shutdown. Release resources.
void plugin_finalize();

// ── Role callbacks (provide whichever the role needs) ───────────────

/// Producer callback. Write to out_slot, return true to commit.
bool on_produce(void* out_slot, size_t out_sz,
                void* flexzone, size_t fz_sz);

/// Consumer callback. Read from in_slot.
void on_consume(const void* in_slot, size_t in_sz,
                const void* flexzone, size_t fz_sz);

/// Processor callback. Read in_slot, write out_slot, return true to commit.
bool on_process(const void* in_slot, size_t in_sz,
                void* out_slot, size_t out_sz,
                void* flexzone, size_t fz_sz);

/// Optional lifecycle callbacks.
void on_init();
void on_stop();
void on_inbox(const void* data, size_t sz, const char* sender_uid);

// ── Optional metadata ───────────────────────────────────────────────

/// If true, any thread may call the plugin's functions concurrently.
/// Default (symbol absent): false (single-thread, same as Python).
bool plugin_is_thread_safe();

/// Human-readable plugin name for logging.
const char* plugin_name();

/// Plugin version string.
const char* plugin_version();

}  // extern "C"
```

### 11.4 Thread model

The plugin declares its own threading capability via `plugin_is_thread_safe()`:

- **Thread-safe plugin** (`returns true`): `supports_multi_state()` = true.
  Any thread may call the plugin's functions directly. The engine does not
  create separate states — there is one loaded library, and all threads
  share it. The plugin is responsible for its own internal synchronization
  if needed.

- **Single-thread plugin** (`returns false` or symbol absent):
  `supports_multi_state()` = false. Same queue-for-owner pattern as Python.
  Only the owner thread calls plugin functions. Cross-thread requests go
  through `drain_pending_requests()`.

Note: unlike Lua's `create_thread_state()` which creates independent
interpreter states, a thread-safe native plugin is a single shared instance.
This is correct — compiled code doesn't need separate "states" for thread
safety; it uses standard C++ synchronization (mutexes, atomics, TLS) as
needed.

### 11.5 Data access — zero copy

The critical advantage over interpreted engines:

```
┌─────────────────────────────────────────────────────────┐
│ Shared Memory Slot (DataBlock)                          │
│ ┌─────────────────────────────────────────────────────┐ │
│ │ Raw bytes: struct SensorData { double x, y, z; ... }│ │
│ └──────────────────────┬──────────────────────────────┘ │
└────────────────────────┼────────────────────────────────┘
                         │ void* ptr
                         │
     ┌───────────────────┼───────────────────────────────┐
     │                   │                               │
  Lua path           Python path               Native path
  ffi.cast(          ctypes.cast(              auto* s =
    ctype_ref,         POINTER(SensorData),      static_cast<
    ptr)               ptr)                        SensorData*>(ptr);
  ~50-100ns          ~200-500ns                ~0ns (compiler inline)
```

The native path is a static_cast at compile time — the compiler can inline
the callback entirely. No runtime type construction, no string operations,
no interpreter dispatch. The plugin operates on the same struct definition
it was compiled against.

### 11.6 Schema validation (optional safety layer)

Even though the plugin knows its own struct layout, the engine can optionally
validate that the DataBlock's schema matches the plugin's expectations:

```cpp
bool NativeEngine::register_slot_type(const SchemaSpec& spec,
                                       const char* type_name,
                                       const std::string& packing)
{
    // Compute expected size from schema
    size_t schema_size = compute_struct_size(spec, packing);

    // Ask plugin for its compiled struct size
    auto fn = dlsym_typed<size_t()>("plugin_sizeof_" + std::string(type_name));
    if (fn)
    {
        size_t plugin_size = fn();
        if (plugin_size != schema_size)
        {
            LOGGER_ERROR("Schema/plugin size mismatch for {}: schema={}, plugin={}",
                         type_name, schema_size, plugin_size);
            return false;
        }
    }
    return true;  // no validation symbol = trust the plugin
}
```

This catches the case where the plugin was compiled against an old struct
definition but the DataBlock uses a newer schema — a common ABI drift bug
in plugin systems.

### 11.7 Configuration

```json
{
    "script": {
        "type": "native",
        "path": "./plugins/libmy_fast_producer.so"
    }
}
```

For native plugins, `script.path` points directly to the shared library
file (not a directory). The role host detects `type == "native"` and
creates a `NativeEngine` instead of `LuaEngine` or `PythonEngine`.

### 11.8 Runtime cost comparison (complete)

| Operation | Lua | Python | Native |
|-----------|-----|--------|--------|
| Interpreter/library init | ~1ms (luaL_newstate) | ~50ms (Py_Initialize) | ~1ms (dlopen) |
| Script/library load | ~100us (luaL_dofile) | ~10ms (import) | ~10us (dlsym × N) |
| Type registration | ~50us (ffi.cdef + typeof) | ~100us (ctypes struct) | ~0 (no-op or size check) |
| Slot pointer cast | ~50-100ns (ffi.cast) | ~200-500ns (ctypes cast) | ~0 (static_cast, inlined) |
| Callback invoke overhead | ~200ns (lua_pcall) | ~500ns (py::object call) | ~5ns (function pointer) |
| Generic invoke("name") | ~250ns (registry + pcall) | ~600ns (getattr + call) | ~50ns (dlsym cache + call) |
| Cross-thread (multi-state) | ~100ns (cached state) | ~drain latency (queue) | ~0 (direct, if thread-safe) |
| Shared state get/set | ~200ns (JSON ↔ table) | ~200ns (JSON ↔ dict) | ~100ns (JSON parse/emit) |

Native plugins eliminate ALL interpreter overhead. The callback invoke cost
drops from hundreds of nanoseconds to a single indirect function call (~5ns).
For high-frequency data loops (>100kHz), this is the difference between
the framework being the bottleneck and being invisible.

### 11.9 Plugin API header

The framework provides a stable C API header that plugins include:

```cpp
// pylabhub/plugin_api.h — installed to include/pylabhub/
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to the role's core state.
/// Plugins call these functions to interact with the framework.
typedef struct plh_core plh_core_t;

// ── Framework functions (provided by pylabhub-utils) ────────────────

uint64_t plh_core_out_written(const plh_core_t* core);
uint64_t plh_core_in_received(const plh_core_t* core);
uint64_t plh_core_script_errors(const plh_core_t* core);
void     plh_core_inc_out_written(plh_core_t* core);
void     plh_core_inc_in_received(plh_core_t* core);
void     plh_core_inc_script_errors(plh_core_t* core);
void     plh_core_request_stop(plh_core_t* core);
void     plh_core_set_critical_error(plh_core_t* core);
int      plh_core_is_shutdown_requested(const plh_core_t* core);

// Shared state
const char* plh_core_get_shared_state(const plh_core_t* core);   // returns JSON string
void        plh_core_set_shared_state(plh_core_t* core, const char* json);
void        plh_core_update_shared_state(plh_core_t* core,
                                          const char* key,
                                          const char* value_json);

// Logging (routes to pylabhub Logger)
void plh_log_info(const char* fmt, ...);
void plh_log_warn(const char* fmt, ...);
void plh_log_error(const char* fmt, ...);

// Identity
const char* plh_core_uid(const plh_core_t* core);
const char* plh_core_name(const plh_core_t* core);
const char* plh_core_channel(const plh_core_t* core);

#ifdef __cplusplus
}  // extern "C"
#endif
```

This is a stable C ABI. Plugins compiled with any C/C++ compiler can link
against it. The `plh_core_t` opaque handle maps to `RoleHostCore*` inside
the engine — the plugin never sees the C++ class, only the C function API.

### 11.10 Comparison of engine types

| Capability | Lua | Python | Native |
|-----------|-----|--------|--------|
| Language | Lua 5.1 (LuaJIT) | Python 3.x (CPython) | C/C++ (any compiler) |
| Runtime overhead | Low (~200ns/call) | Medium (~500ns/call) | Near-zero (~5ns/call) |
| Slot data access | FFI cast (~100ns) | ctypes cast (~300ns) | Direct pointer (~0) |
| Thread model | Multi-state (independent) | Single-state (GIL) | Plugin-declared |
| Hot-reload | Possible (new state) | Possible (re-import) | Requires dlclose+dlopen |
| Debugging | print + LOGGER | Full Python debugger | GDB/LLDB native |
| Rapid prototyping | Good (dynamic) | Excellent (ecosystem) | Poor (compile cycle) |
| Production perf | Good | Adequate | Optimal |
| Dependency mgmt | Embedded (LuaJIT) | venv / system | System libraries |

**Recommended usage pattern:**
- **Python**: prototyping, complex logic, rich ecosystem needs, data science
- **Lua**: lightweight real-time roles, minimal dependencies, embedded systems
- **Native**: performance-critical inner loops, existing C/C++ algorithms,
  hardware interface code, latency-sensitive data paths

### 11.11 C++ Plugin API — Type-Safe Tier

The C API (§11.9) provides maximum portability (any compiler, stable ABI).
When the plugin is compiled with the same compiler and C++ standard library
as the framework — which is the common case in controlled deployments — a
C++ API tier provides full type safety with zero additional runtime cost.

#### Type-safe slot access via template

```cpp
// pylabhub/plugin_api.hpp — C++ tier, same-compiler ABI
#pragma once
#include <pylabhub/plugin_api.h>
#include <cstddef>
#include <type_traits>

namespace pylabhub
{

/// Typed reference to a slot in shared memory.
/// Validates sizeof(T) and alignof(T) against the schema at load time.
/// Provides operator-> and operator* for direct typed member access.
/// Zero runtime cost — the pointer cast is resolved at compile time.
template <typename T>
class SlotRef
{
    static_assert(std::is_standard_layout_v<T>,
                  "Slot types must be standard-layout for SHM compatibility");
    T* ptr_;
public:
    explicit SlotRef(void* raw) noexcept : ptr_(static_cast<T*>(raw)) {}
    T* operator->() noexcept { return ptr_; }
    T& operator*()  noexcept { return *ptr_; }
    T* get()         noexcept { return ptr_; }
};

template <typename T>
class ConstSlotRef
{
    static_assert(std::is_standard_layout_v<T>,
                  "Slot types must be standard-layout for SHM compatibility");
    const T* ptr_;
public:
    explicit ConstSlotRef(const void* raw) noexcept
        : ptr_(static_cast<const T*>(raw)) {}
    const T* operator->() const noexcept { return ptr_; }
    const T& operator*()  const noexcept { return *ptr_; }
    const T* get()         const noexcept { return ptr_; }
};

/// Typed reference to the flexzone region.
class FlexzoneRef
{
    void*  ptr_;
    size_t sz_;
public:
    FlexzoneRef(void* p, size_t s) noexcept : ptr_(p), sz_(s) {}
    void*  data()  noexcept { return ptr_; }
    size_t size()  const noexcept { return sz_; }

    template <typename T>
    T* as() noexcept { return static_cast<T*>(ptr_); }
};

/// Role API handle — wraps the C plh_core_t with typed C++ methods.
class RoleApi
{
    plh_core_t* core_;
public:
    explicit RoleApi(plh_core_t* c) noexcept : core_(c) {}

    // Identity
    const char* uid()     const { return plh_core_uid(core_); }
    const char* name()    const { return plh_core_name(core_); }
    const char* channel() const { return plh_core_channel(core_); }

    // Metrics
    uint64_t out_written()   const { return plh_core_out_written(core_); }
    uint64_t in_received()   const { return plh_core_in_received(core_); }
    uint64_t script_errors() const { return plh_core_script_errors(core_); }
    void inc_out_written()  { plh_core_inc_out_written(core_); }
    void inc_in_received()  { plh_core_inc_in_received(core_); }

    // Control
    void request_stop()      { plh_core_request_stop(core_); }
    void set_critical_error(){ plh_core_set_critical_error(core_); }
    bool is_shutdown_requested() const
    { return plh_core_is_shutdown_requested(core_) != 0; }

    // Shared state
    const char* get_shared_state() const
    { return plh_core_get_shared_state(core_); }
    void set_shared_state(const char* json)
    { plh_core_set_shared_state(core_, json); }
    void update_shared_state(const char* key, const char* value_json)
    { plh_core_update_shared_state(core_, key, value_json); }

    // Logging
    template <typename... Args>
    void log_info(const char* fmt, Args&&... args)
    { plh_log_info(fmt, std::forward<Args>(args)...); }
    template <typename... Args>
    void log_warn(const char* fmt, Args&&... args)
    { plh_log_warn(fmt, std::forward<Args>(args)...); }
    template <typename... Args>
    void log_error(const char* fmt, Args&&... args)
    { plh_log_error(fmt, std::forward<Args>(args)...); }
};

}  // namespace pylabhub

/// Generate sizeof/alignof validation symbols for a slot struct.
/// The engine checks these at load time against the DataBlock schema.
/// Catches ABI drift between plugin compilation and runtime schema.
#define PLH_REGISTER_SLOT_TYPE(T)                                     \
    static_assert(std::is_standard_layout_v<T>,                       \
                  #T " must be standard-layout for SHM");             \
    extern "C" size_t  plugin_sizeof_##T()  { return sizeof(T); }     \
    extern "C" size_t  plugin_alignof_##T() { return alignof(T); }
```

#### Example: type-safe producer plugin

```cpp
// my_sensor_producer.cpp
#include <pylabhub/plugin_api.hpp>

struct SensorData
{
    double   x, y, z;
    uint32_t flags;
    uint64_t timestamp_ns;
};

PLH_REGISTER_SLOT_TYPE(SensorData)

static pylabhub::RoleApi* g_api = nullptr;

extern "C" bool plugin_init(const pylabhub::scripting::RoleContext* ctx)
{
    // Framework provides a typed C++ wrapper around the core handle
    g_api = new pylabhub::RoleApi(reinterpret_cast<plh_core_t*>(ctx->core));
    return true;
}

extern "C" bool on_produce(void* out_raw, size_t out_sz,
                            void* fz_raw, size_t fz_sz)
{
    pylabhub::SlotRef<SensorData> slot(out_raw);
    pylabhub::FlexzoneRef fz(fz_raw, fz_sz);

    // Direct typed member access — zero overhead, compile-time checked
    slot->x = read_sensor_x();
    slot->y = read_sensor_y();
    slot->z = read_sensor_z();
    slot->flags = 0x01;
    slot->timestamp_ns = current_time_ns();

    return true;   // commit
}

extern "C" void on_stop()
{
    delete g_api;
    g_api = nullptr;
}

extern "C" bool plugin_is_thread_safe() { return false; }
extern "C" const char* plugin_name() { return "SensorProducer"; }
```

#### ABI safety model (two tiers)

| Tier | Header | ABI guarantee | Type safety | Use case |
|------|--------|--------------|-------------|----------|
| C | `plugin_api.h` | Stable across compilers | Opaque handle + C functions | Third-party plugins, cross-compiler |
| C++ | `plugin_api.hpp` | Same compiler + stdlib required | `SlotRef<T>`, `RoleApi`, templates | In-house plugins, controlled builds |

The C++ tier builds on top of the C tier — `RoleApi` wraps `plh_core_t*`,
`SlotRef<T>` wraps `void*`. If ABI compatibility breaks (compiler upgrade,
stdlib change), the plugin falls back to the C tier without framework changes.

The `std::is_standard_layout_v<T>` static_assert in `SlotRef` and
`PLH_REGISTER_SLOT_TYPE` ensures the struct is SHM-safe: no vtable, no
virtual bases, predictable layout. Combined with the runtime sizeof/alignof
check at load time, this gives three layers of protection:

1. **Compile-time**: `static_assert` — struct must be standard-layout
2. **Load-time**: sizeof/alignof check — struct must match schema
3. **Type-time**: `SlotRef<T>` template — wrong struct type won't compile

#### Compiler ABI verification via build_info

The framework captures the full build configuration at CMake configure time
into a generated header. This is not just the compiler version — it includes
every detail that affects ABI compatibility.

**Generated header** (`cmake/build_info.hpp.in`):

```cpp
// cmake/build_info.hpp.in — processed by configure_file(@ONLY)
#pragma once

namespace plh::build_info
{
    // Compiler identity
    constexpr const char* compiler_id      = "@CMAKE_CXX_COMPILER_ID@";
    constexpr const char* compiler_version = "@CMAKE_CXX_COMPILER_VERSION@";
    constexpr const char* compiler_path    = "@CMAKE_CXX_COMPILER@";

    // Compile/link flags (everything that affects code generation)
    constexpr const char* cxx_flags        = "@PLH_ALL_CXX_FLAGS@";
    constexpr const char* linker_flags_shared = "@CMAKE_SHARED_LINKER_FLAGS@";
    constexpr const char* linker_flags_exe    = "@CMAKE_EXE_LINKER_FLAGS@";

    // Language standard and build type
    constexpr int         cxx_standard     = @CMAKE_CXX_STANDARD@;
    constexpr const char* build_type       = "@CMAKE_BUILD_TYPE@";

    // Target platform
    constexpr const char* system_name      = "@CMAKE_SYSTEM_NAME@";
    constexpr const char* system_processor = "@CMAKE_SYSTEM_PROCESSOR@";

    // Sanitizer (affects ABI: ASan adds redzones, changes struct padding)
    constexpr const char* sanitizer        = "@PYLABHUB_USE_SANITIZER@";

    // Compact JSON representation for export/comparison
    #define PLH_BUILD_INFO_JSON                                       \
        "{\"compiler_id\":\"@CMAKE_CXX_COMPILER_ID@\","              \
        "\"compiler_version\":\"@CMAKE_CXX_COMPILER_VERSION@\","     \
        "\"cxx_standard\":@CMAKE_CXX_STANDARD@,"                     \
        "\"cxx_flags\":\"@PLH_ALL_CXX_FLAGS@\","                     \
        "\"build_type\":\"@CMAKE_BUILD_TYPE@\","                     \
        "\"system\":\"@CMAKE_SYSTEM_NAME@/@CMAKE_SYSTEM_PROCESSOR@\","\
        "\"sanitizer\":\"@PYLABHUB_USE_SANITIZER@\"}"
}
```

**CMake wiring** (`cmake/GenerateBuildInfo.cmake`):

```cmake
# Combine all CXX flags into a single string for the header
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
set(PLH_ALL_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}}")
string(STRIP "${PLH_ALL_CXX_FLAGS}" PLH_ALL_CXX_FLAGS)

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/build_info.hpp.in
    ${CMAKE_BINARY_DIR}/generated/pylabhub/build_info.hpp
    @ONLY
)

# Make generated header available to all targets
target_include_directories(pylabhub-utils PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>)

# Install for plugin developers
install(FILES ${CMAKE_BINARY_DIR}/generated/pylabhub/build_info.hpp
        DESTINATION include/pylabhub)
```

**Example output** (what gets generated for a real build):

```cpp
namespace plh::build_info
{
    constexpr const char* compiler_id      = "GNU";
    constexpr const char* compiler_version = "13.2.0";
    constexpr const char* compiler_path    = "/usr/bin/g++-13";
    constexpr const char* cxx_flags        = "-Wall -Wextra -Werror -O2 -DNDEBUG";
    constexpr const char* linker_flags_shared = "";
    constexpr const char* linker_flags_exe    = "";
    constexpr int         cxx_standard     = 20;
    constexpr const char* build_type       = "Release";
    constexpr const char* system_name      = "Linux";
    constexpr const char* system_processor = "x86_64";
    constexpr const char* sanitizer        = "";
}
```

**Integration with existing Layer 0 platform API:**

The version handler (`plh_platform.hpp`) already exposes `plh_version()`,
`plh_version_string()`, etc. Add `plh_build_info_json()` as a new function
that returns `PLH_BUILD_INFO_JSON`. This requires no new mechanism — it
extends the existing version API with build configuration detail.

```cpp
// In plh_platform.hpp (or a new plh_build_info.hpp umbrella include)
#include <pylabhub/build_info.hpp>

inline const char* plh_build_info_json()
{
    return PLH_BUILD_INFO_JSON;
}
```

**Plugin-side verification:**

The plugin is compiled against the installed pylabhub headers, which include
the generated `build_info.hpp`. The plugin exports the build info that was
visible at its own compile time:

```cpp
// In the plugin source
#include <pylabhub/build_info.hpp>

extern "C" const char* plugin_build_info_json()
{
    return PLH_BUILD_INFO_JSON;
}
```

At `dlopen` time, `NativeEngine` loads and compares:

```cpp
bool NativeEngine::verify_abi_compatibility_()
{
    auto fn = dlsym_typed<const char*()>("plugin_build_info_json");
    if (!fn)
    {
        LOGGER_WARN("Plugin does not export build info — treating as C-tier");
        cpp_tier_enabled_ = false;
        return true;  // still loadable via C API
    }

    const char* plugin_info = fn();
    auto plugin_json = nlohmann::json::parse(plugin_info);
    auto framework_json = nlohmann::json::parse(plh_build_info_json());

    // Critical fields that must match for C++ ABI safety
    bool abi_safe =
        plugin_json["compiler_id"]      == framework_json["compiler_id"] &&
        plugin_json["compiler_version"] == framework_json["compiler_version"] &&
        plugin_json["cxx_standard"]     == framework_json["cxx_standard"] &&
        plugin_json["sanitizer"]        == framework_json["sanitizer"];

    if (!abi_safe)
    {
        LOGGER_WARN("Plugin ABI mismatch — restricting to C tier\n"
                    "  framework: {}\n  plugin:    {}",
                    plh_build_info_json(), plugin_info);
        cpp_tier_enabled_ = false;
        return true;  // still loadable via C API
    }

    // Non-critical: warn if build type differs (e.g. Debug plugin on Release framework)
    if (plugin_json["build_type"] != framework_json["build_type"])
    {
        LOGGER_WARN("Plugin build type ({}) differs from framework ({})",
                    plugin_json["build_type"].get<std::string>(),
                    framework_json["build_type"].get<std::string>());
    }

    cpp_tier_enabled_ = true;
    return true;
}
```

**What gets checked and why:**

| Field | ABI-critical? | Reason |
|-------|:---:|--------|
| `compiler_id` | YES | Different compilers (GCC vs Clang) may use different name mangling, vtable layout, exception handling |
| `compiler_version` | YES | Major version changes can alter struct layout, calling conventions, STL internals |
| `cxx_standard` | YES | C++17 vs C++20 can change ABI (e.g., `std::string` layout changed in GCC's C++11 transition) |
| `sanitizer` | YES | ASan adds redzones to allocations, changes struct padding and alignment — mixing ASan/non-ASan is UB |
| `cxx_flags` | NO (warn) | Optimization level rarely affects ABI, but `-fpack-struct` or `-fno-exceptions` can |
| `build_type` | NO (warn) | Debug vs Release may differ in assertions/checks but ABI is usually stable |
| `system` | NO (info) | Cross-platform plugins are rare; mismatch would fail at dlopen anyway |
