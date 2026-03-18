# Tech Draft: ScriptEngine Abstraction Refactor

**Status**: Draft (2026-03-17)
**Branch**: `feature/lua-role-support`
**Relates to**: HEP-CORE-0011 (ScriptHost Abstraction Framework)

## 1. Problem Statement

The current role host architecture duplicates two concerns:

1. **Role infrastructure** (Messenger, Producer/Consumer, queues, ctrl_thread_, event
   wiring, wait_for_roles) — duplicated across Python and Lua hosts (~350 lines each
   in `start_role()`)
2. **Script engine operations** (load script, build slot views, invoke callbacks,
   build API object) — tightly coupled into each host's data loop

The Python path additionally wastes a thread: the interpreter thread sleeps in a
50ms loop after `start_role()` solely to keep `py::scoped_interpreter` in scope.

## 2. Design Principles

1. **The main thread handles signals and monitoring only.** It starts the working
   loop, provides config/shutdown flags, and waits. It never touches the script engine.

2. **The working loop owns the script engine.** All script operations (load, invoke,
   finalize) happen on the working loop thread. The engine is created, used, and
   destroyed on that thread.

3. **The role host owns infrastructure.** Messenger, Producer/Consumer, queues,
   ctrl_thread_ — these are engine-agnostic. One implementation, shared by all engines.

4. **The script engine is a stateful callback dispatcher.** It knows how to load a
   script, extract callbacks, build typed slot views, and invoke callbacks. It does
   NOT know about queues, messengers, or the data loop.

## 3. Thread Model

```
Main Thread                    Working Thread              ctrl_thread_
     |                              |                         |
  create RoleHost               (not started yet)             |
  host.configure(config)            |                         |
  host.start()                      |                         |
     |--- spawn working thread ---->|                         |
     |    block on ready_future     |                         |
     |                         engine.initialize(config)      |
     |                         engine.load_script(path)       |
     |                         engine.extract_callbacks()     |
     |                         host.setup_infrastructure()    |
     |                           connect messenger            |
     |                           create producer/consumer     |
     |                           create queue                 |
     |                           wire events                  |
     |                         engine.build_api(role_context) |
     |                         engine.invoke_on_init()        |
     |                         spawn ctrl_thread_ ----------->|
     |                         signal_ready_()                |
     |<--- unblocked                |                      ZmqPollLoop
     |                              |                      (pure C++)
  run_role_main_loop()         DATA LOOP:                     |
     while !g_shutdown:          while running:               |
       wait(100ms condvar)         slot = queue.acquire()     |
       check is_running            engine.invoke(slot, ...)   |
                                   queue.commit/discard       |
                                   drain_inbox(engine)        |
                                   timing                     |
     |                              |                         |
  signal_shutdown()            (loop exits)                   |
     |                         engine.invoke_on_stop()        |
     |                         join ctrl_thread_ <------------|
     |                         host.teardown_infrastructure() |
     |                         engine.finalize()              |
     |<--- join working thread      |                         |
  cleanup                      (thread exits)                 |
```

Key differences from current design:
- **Python**: No separate interpreter thread. The working thread creates
  `py::scoped_interpreter` as a member (or `std::optional`), runs the data loop
  directly, and destroys it at the end. Eliminates the idle sleep loop and
  `loop_thread_`.
- **Lua**: Same as current (working thread IS the data loop). No change.
- **Both**: `start_role()` infrastructure code is unified in the role host base
  class, not duplicated per engine.

## 4. ScriptEngine Interface

```cpp
namespace pylabhub::scripting
{

/// Role-specific context passed to the engine for API construction.
/// Contains pointers to the C++ objects the API closures need.
struct RoleContext
{
    const char*       role_tag;       // "prod", "cons", "proc"
    const char*       uid;
    const char*       name;
    const char*       channel;
    hub::Messenger*   messenger;      // for open_inbox, wait_for_role
    hub::QueueWriter* queue_writer;   // for update_flexzone_checksum (producer/processor)
    hub::QueueReader* queue_reader;   // for set_verify_checksum (consumer/processor)
    // ... other role-specific pointers
};

/// Result of invoking on_produce / on_process.
enum class InvokeResult { Commit, Discard, Error };

class ScriptEngine
{
public:
    virtual ~ScriptEngine() = default;

    // ── Lifecycle (called from working thread) ───────────────────────

    /// Create interpreter/state, apply sandbox.
    virtual bool initialize(const RoleConfig& config) = 0;

    /// Load script file and extract callbacks.
    virtual bool load_script(const std::filesystem::path& script_dir,
                             const char* required_callback) = 0;

    /// Build the API object/table using role-specific context.
    virtual void build_api(const RoleContext& ctx) = 0;

    /// Release all script objects, close interpreter/state.
    virtual void finalize() = 0;

    // ── Queries ──────────────────────────────────────────────────────

    virtual bool has_callback(const char* name) const = 0;
    virtual bool has_required_callback() const = 0;

    // ── Schema / type building ───────────────────────────────────────

    /// Register a slot type from SchemaSpec. Returns opaque type handle.
    virtual bool register_slot_type(const SchemaSpec& spec,
                                    const char* type_name,
                                    const std::string& packing) = 0;

    /// Query the size of a registered type.
    virtual size_t type_sizeof(const char* type_name) const = 0;

    // ── Callback invocation (called from working thread) ─────────────

    virtual void invoke_on_init() = 0;
    virtual void invoke_on_stop() = 0;

    /// Invoke on_produce(out_slot, flexzone, messages, api).
    /// Returns Commit, Discard, or Error.
    virtual InvokeResult invoke_produce(
        void* out_slot, size_t out_sz,
        void* flexzone, size_t fz_sz,
        std::vector<IncomingMessage>& msgs) = 0;

    /// Invoke on_consume(in_slot, flexzone, messages, api).
    virtual void invoke_consume(
        const void* in_slot, size_t in_sz,
        const void* flexzone, size_t fz_sz,
        std::vector<IncomingMessage>& msgs) = 0;

    /// Invoke on_process(in_slot, out_slot, flexzone, messages, api).
    virtual InvokeResult invoke_process(
        const void* in_slot, size_t in_sz,
        void* out_slot, size_t out_sz,
        void* flexzone, size_t fz_sz,
        std::vector<IncomingMessage>& msgs) = 0;

    /// Invoke on_inbox(slot, sender, api).
    virtual void invoke_on_inbox(
        const void* data, size_t sz,
        const char* sender_uid) = 0;

    // ── Error state ──────────────────────────────────────────────────

    virtual uint64_t script_error_count() const = 0;

    // ── Threading capability ─────────────────────────────────────────

    /// True if the engine supports creating independent states for other
    /// threads (e.g., Lua can create a separate lua_State per thread).
    /// False if the engine uses a single shared interpreter with a global
    /// lock (e.g., Python's GIL — calling from another thread risks
    /// contention or requires explicit GIL acquire, which blocks the
    /// working loop).
    ///
    /// When true, the framework may:
    /// - Create a secondary engine state for ctrl_thread_ (on_heartbeat)
    /// - Create a secondary engine state for the main loop (respond to
    ///   external requests via script)
    /// - Allow user scripts to spawn additional engine states via
    ///   api.spawn_thread()
    ///
    /// When false, ALL script invocations must happen on the working
    /// thread. Other threads must enqueue requests to the working thread
    /// for script processing.
    virtual bool supports_multi_state() const noexcept = 0;

    /// Create an independent engine state for use on another thread.
    /// Loads the same script, extracts the same callbacks, but operates
    /// on a separate interpreter/state with no shared script objects.
    /// Returns nullptr if !supports_multi_state().
    virtual std::unique_ptr<ScriptEngine> create_thread_state() = 0;
};

} // namespace pylabhub::scripting
```

## 5. Engine Implementations

### 5.1 LuaEngine

```cpp
class LuaEngine : public ScriptEngine
{
    LuaState state_;           // RAII, created in initialize()
    int ref_on_produce_{LUA_NOREF};
    int ref_on_consume_{LUA_NOREF};
    int ref_on_process_{LUA_NOREF};
    int ref_on_init_{LUA_NOREF};
    int ref_on_stop_{LUA_NOREF};
    int ref_on_inbox_{LUA_NOREF};
    int ref_api_{LUA_NOREF};
    // ...
};
```

- `initialize()`: `LuaState::create()`, sandbox, package.path
- `load_script()`: `state_.load_script()`, extract callback refs from globals
- `build_api()`: build Lua table with closures capturing RoleContext pointers
- `invoke_produce()`: push args → `lua_pcall` → interpret return → pop
- `finalize()`: unref all, `state_ = LuaState{}`
- No GIL, no locking. All calls on the working thread.

### 5.2 PythonEngine

```cpp
class PythonEngine : public ScriptEngine
{
    std::optional<py::scoped_interpreter> interp_;
    py::object module_;
    py::object py_on_produce_;
    py::object py_on_init_;
    py::object py_on_stop_;
    py::object api_obj_;
    // ...
};
```

- `initialize()`: `interp_.emplace(&pyconfig)`, configure PYTHONHOME, optional venv
- `load_script()`: `py::module_::import()`, extract callbacks
- `build_api()`: `py::cast(&api)` where api wraps RoleContext
- `invoke_produce()`: `py::gil_scoped_acquire` → build slot view → pcall → release
- `finalize()`: clear all py::objects, `interp_.reset()` → Py_Finalize
- GIL acquire/release around each invoke call.

**Critical**: `py::scoped_interpreter` is a member, not a stack local. Its lifetime
is controlled explicitly by `initialize()`/`finalize()`. This eliminates the need for
a dedicated interpreter thread and the 50ms sleep loop.

**GIL note**: The working thread creates the interpreter (holds the GIL initially).
After `build_api()`, it releases the GIL for the data loop. Each `invoke_*()` call
re-acquires the GIL, calls Python, and releases. This is the same pattern as the
current `loop_thread_` — just without the extra thread.

## 6. Unified Role Host

```cpp
template <typename Config>
class RoleHost
{
protected:
    RoleHostCore core_;
    Config config_;
    std::unique_ptr<ScriptEngine> engine_;

    // Infrastructure (engine-agnostic, one implementation)
    hub::Messenger messenger_;
    std::optional<hub::Producer> producer_;   // or Consumer
    std::unique_ptr<hub::QueueWriter> queue_; // or QueueReader
    std::unique_ptr<hub::InboxQueue> inbox_queue_;
    std::thread ctrl_thread_;

    // Working thread
    std::thread worker_thread_;
    std::promise<void> ready_promise_;

    // Called on the working thread
    void worker_main_()
    {
        // 1. Initialize engine
        if (!engine_->initialize(config_))
        { signal_ready_fail(); return; }

        if (!engine_->load_script(script_dir, required_callback()))
        { signal_ready_fail(); return; }

        // 2. Setup infrastructure (engine-agnostic)
        if (!setup_infrastructure_())
        { engine_->finalize(); signal_ready_fail(); return; }

        // 3. Build API (engine-specific, using infrastructure pointers)
        RoleContext ctx = build_role_context_();
        engine_->build_api(ctx);
        engine_->invoke_on_init();

        // 4. Signal ready
        core_.running = true;
        ready_promise_.set_value();

        // 5. Data loop (role-specific: produce vs consume vs process)
        run_data_loop_();  // pure virtual — calls engine_->invoke_*()

        // 6. Teardown
        engine_->invoke_on_stop();
        teardown_infrastructure_();
        engine_->finalize();
        core_.running = false;
    }

    // Engine-agnostic infrastructure
    bool setup_infrastructure_();    // connect, create producer, create queue, wire events
    void teardown_infrastructure_(); // join ctrl, stop queues, deregister, close

    // Role-specific data loop (calls engine_->invoke_produce/consume/process)
    virtual void run_data_loop_() = 0;
};
```

The three concrete role hosts (`ProducerHost`, `ConsumerHost`, `ProcessorHost`)
override `run_data_loop_()` only — all infrastructure is in the base class.

## 7. Data Loop Example (Producer)

```cpp
void ProducerHost::run_data_loop_()
{
    const auto timeout = std::chrono::milliseconds{config_.timeout_ms};
    const auto period  = std::chrono::milliseconds{config_.target_period_ms};

    while (core_.running_threads.load() &&
           !core_.shutdown_requested.load() &&
           !core_.critical_error_.load())
    {
        void* buf = queue_->write_acquire(timeout);
        if (!buf) continue;  // timeout or shutdown

        auto msgs = core_.drain_messages();

        auto result = engine_->invoke_produce(
            buf, schema_slot_size_,
            flexzone_ptr_, core_.schema_fz_size,
            msgs);

        switch (result)
        {
        case InvokeResult::Commit:  queue_->write_commit(); break;
        case InvokeResult::Discard: queue_->write_discard(); break;
        case InvokeResult::Error:
            queue_->write_discard();
            if (config_.stop_on_script_error)
                core_.shutdown_requested.store(true);
            break;
        }

        // Synchronous inbox drain
        if (inbox_queue_)
            drain_inbox_();

        // Timing
        apply_loop_timing_(period);
    }
}
```

The data loop has **zero engine-specific code**. It calls `engine_->invoke_produce()`
which internally handles GIL (Python) or direct pcall (Lua).

## 8. Multi-State Threading Model

The `supports_multi_state()` flag enables the framework to use the script engine
from threads other than the working thread:

```
                          LuaEngine              PythonEngine
supports_multi_state()    true                   false
```

### Lua (multi-state capable)

```
Working Thread:  engine_primary  → on_produce, on_init, on_stop, on_inbox
ctrl_thread_:    engine_ctrl     → on_heartbeat, on_peer_event (optional)
Main Thread:     engine_admin    → respond to external admin requests (optional)
```

Each `create_thread_state()` returns a new `LuaEngine` with its own `LuaState`
(independent `lua_State*`). They load the same script independently. Shared data
goes through C++ atomics and sync primitives — no Lua objects cross states.

### Python (single-state)

```
Working Thread:  engine_ → ALL script invocations (on_produce, on_init, etc.)
ctrl_thread_:    C++ only (no script)
Main Thread:     C++ only (no script)
```

Other threads that need script results must enqueue a request to the working thread
and wait for the response. This is the same model as today — the GIL makes
cross-thread script calls safe but serialized, and acquiring from another thread
blocks the working loop.

**Future**: Python 3.13+ sub-interpreters (PEP 684) with per-interpreter GIL
could enable `supports_multi_state() = true` for Python. Each sub-interpreter
gets its own GIL, enabling true parallelism identical to Lua's model. Our
pybind11 3.1.0-alpha supports this via `py::subinterpreter`. This is a future
enhancement — the interface is ready for it.

## 9. What Gets Eliminated

| Current code | After refactor |
|---|---|
| `PythonScriptHost` + `ScriptHost` base class | Not needed (engine owns interpreter directly) |
| `PythonRoleHostBase::do_python_work()` 50ms sleep loop | Eliminated (working thread runs data loop) |
| `ProducerScriptHost::start_role()` ~350 lines | Moves to `RoleHost::setup_infrastructure_()` (shared) |
| `LuaRoleHostBase::do_lua_work_()` monolithic function | Split into `worker_main_()` staged calls |
| `LuaProducerHost::start_role()` ~220 lines | Moves to shared `setup_infrastructure_()` |
| `loop_thread_` (Python) | Eliminated (working thread IS the data loop) |
| `interpreter thread` sleep loop (Python) | Eliminated |
| Duplicated `run_loop_()` / `run_data_loop_()` per engine | One `run_data_loop_()` per role (engine-agnostic) |
| Duplicated inbox setup, event wiring, queue creation | One implementation in base class |

## 10. Migration Plan

### Phase 1: ScriptEngine interface + LuaEngine
- Create `script_engine.hpp` (abstract interface)
- Create `lua_engine.cpp` (wraps LuaState, implements all invoke methods)
- Create `ProducerHost` using ScriptEngine (Lua path only initially)
- Verify 1220/1220 tests pass

### Phase 2: PythonEngine
- Create `python_engine.cpp` (wraps py::scoped_interpreter as member)
- Migrate ProducerScriptHost to use PythonEngine via ProducerHost
- Verify 1220/1220 tests pass

### Phase 3: Consumer + Processor
- Extend RoleHost for ConsumerHost and ProcessorHost
- Migrate all 6 host classes (3 roles × 2 engines)

### Phase 4: Cleanup
- Remove `PythonScriptHost`, `PythonRoleHostBase`, `LuaRoleHostBase`
- Remove `ScriptHost` abstract base class
- Remove `do_python_work()`, `do_lua_work_()`
- Update HEP-0011

## 11. Risks

1. **`py::scoped_interpreter` as member**: This changes the interpreter lifetime
   from a stack-scoped local to an explicitly-managed member. All py::objects must
   still be destroyed before `interp_.reset()`. The `finalize()` method handles this.

2. **GIL on working thread**: The working thread creates the interpreter, so it
   initially holds the GIL. It must release before entering the data loop (or
   acquire/release per invoke call). The current `loop_thread_` already does this.

3. **inbox_thread_ elimination (Python)**: Currently inbox runs in parallel with
   the data loop. Moving to synchronous drain (like Lua) adds latency of one loop
   period. For most use cases this is acceptable. If not, a separate inbox thread
   can be retained as an optimization, acquiring the GIL per message.

4. **Backward compatibility**: The main.cpp dispatch code
   (`producer_main.cpp:293-348`) will change. Config format and script interface
   remain identical.
