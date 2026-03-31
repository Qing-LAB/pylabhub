# Script Engine as Lifecycle Dynamic Module

**Status**: Draft (2026-03-31)
**Scope**: All script engines managed as lifecycle dynamic modules
**Depends on**: Lifecycle dynamic module system (done), ScriptEngine interface (done)

---

## 1. Problem

Script engine shutdown is not timeout-guarded. If `plugin_finalize()` (native),
`Py_Finalize` (Python), or `lua_close` (Lua) hangs, the process hangs forever.
All other subsystems use lifecycle modules for ordered, timeout-guarded shutdown.
Script engines should too.

---

## 2. Design

### 2.1 Engine = dynamic lifecycle module

Each ScriptEngine subclass provides `make_lifecycle_module()` — same pattern as
`InteractiveSignalHandler::make_lifecycle_module()`.

The module's **startup** callback calls `engine->initialize(tag, core)`. This is
the minimum needed to put the engine into INITIALIZED state.

The module's **shutdown** callback calls `engine->finalize()`. This is timeout-guarded
by the lifecycle system. `finalize()` is a state-machine walker — it cleans up
exactly what was set up, regardless of how far initialization progressed.

### 2.2 Module naming and instance policy

Module naming encodes engine type + identity. The lifecycle system's duplicate
name rejection enforces instance constraints naturally:

| Engine | Module name pattern | Policy |
|--------|-------------------|--------|
| Python | `"ScriptEngine:python"` | **Process singleton.** Fixed name. Second registration fails. Matches CPython's one-interpreter-per-process constraint. |
| Lua | `"ScriptEngine:lua:<role_uid>"` | **Per-role.** Multiple Lua engines coexist (independent lua_States). Each role gets its own module. |
| Native | `"ScriptEngine:native:<lib_filename>:<role_uid>"` | **Per-role, library visible.** If two roles load the same .so, the shared library name is visible in both module names — the user can detect shared-state conflicts. |

The lifecycle system rejects duplicate module names at `RegisterDynamicModule()`.
No additional singleton logic needed in the engines.

### 2.3 Engine state machine

```
UNLOADED ─→ INITIALIZED ─→ SCRIPT_LOADED ─→ API_BUILT ─→ RUNNING
                                                              │
finalize() walks back from current state:                     │
  RUNNING    → invoke_on_stop, stop threads                   │
  API_BUILT  → plugin_finalize / release context              │
  SCRIPT_LOADED → dlclose / release interpreter state         │
  INITIALIZED → release core ref                              │
  UNLOADED   → no-op                                          ▼
                                                          FINALIZED
```

`finalize()` is idempotent. Calling it from any state is safe.

### 2.4 Lifecycle integration point

```cpp
// In main(), after LifecycleGuard and config are ready:

auto engine = create_engine(config.script().type);  // factory: returns unique_ptr

// Register as dynamic module — startup = initialize, shutdown = finalize (5s timeout)
RegisterDynamicModule(engine->make_lifecycle_module(log_tag, &core));
LoadModule(engine->lifecycle_module_name());
// Engine is now INITIALIZED.

// Create role host — it receives the engine and drives it forward:
RoleHost host(config, engine.get());
host.startup_();
// worker_main_() calls: load_script → register_slot_type → build_api → run

// On exit:
host.shutdown_();
// LifecycleGuard destructor → FinalizeApp() → UnloadModule → engine->finalize()
// Timeout-guarded: if finalize hangs, lifecycle marks contaminated after 5s.
```

### 2.5 Role host changes

The role host currently calls `engine_->initialize()` inside `worker_main_()`.
With lifecycle modules, `initialize()` is called by `LoadModule()` before the
role host starts. The role host only calls:

- `load_script()` — advance to SCRIPT_LOADED
- `register_slot_type()` — schema registration
- `build_api(ctx)` — advance to API_BUILT (needs infrastructure, called after setup_infrastructure_)
- `invoke_on_init()` / `invoke_produce()` / etc. — runtime
- `invoke_on_stop()` — called by role host before shutdown

The role host does NOT call `initialize()` or `finalize()` — lifecycle manages those.

### 2.6 make_lifecycle_module() per engine

```cpp
// ScriptEngine base class (or each subclass):
ModuleDef make_lifecycle_module(const std::string &tag, RoleHostCore *core)
{
    ModuleDef mod(lifecycle_module_name());  // "ScriptEngine:native:PROD-xxx"
    mod.add_dependency("Logger");

    // Startup: initialize the engine (puts it in INITIALIZED state)
    // The `this` pointer and args are captured via static storage
    // (same pattern as InteractiveSignalHandler).
    s_engine_instance_ = this;
    s_init_tag_ = tag;
    s_init_core_ = core;
    mod.set_startup(engine_lifecycle_startup, "");

    // Shutdown: finalize the engine (walks back from whatever state)
    mod.set_shutdown(engine_lifecycle_shutdown,
                     std::chrono::milliseconds{5000});

    return mod;
}
```

### 2.7 NativeEngine specifics

For NativeEngine, `finalize()` state-machine walk-back includes:
- API_BUILT → `plugin_finalize()` + release PluginContextStorage
- SCRIPT_LOADED → `DL_CLOSE(dl_handle_)`
- INITIALIZED → clear log_tag

If `plugin_finalize()` hangs, the 5s timeout fires, lifecycle marks the module
contaminated, and the process continues shutdown. The `.so` may leak — but the
process is exiting anyway.

### 2.8 Test environment

L2 tests don't use lifecycle. They create engines directly and call
`initialize → load_script → build_api → invoke → finalize` manually.
The `make_lifecycle_module()` is only used in production mains.
Tests continue to work unchanged.

---

## 3. Changes Required

### 3.1 ScriptEngine base class
- Add `lifecycle_module_name()` virtual method (returns unique string)
- Add `make_lifecycle_module(tag, core)` method
- Add engine state tracking enum (UNLOADED, INITIALIZED, SCRIPT_LOADED, API_BUILT, RUNNING, FINALIZED)
- Make `finalize()` state-aware (clean up based on current state)

### 3.2 Each engine subclass
- Override `lifecycle_module_name()` with engine-specific name
- Ensure `finalize()` handles partial initialization gracefully

### 3.3 Role host worker_main_()
- Remove `engine_->initialize()` call (lifecycle already did it)
- Remove `engine_->finalize()` call (lifecycle will do it)
- Keep: load_script, register_slot_type, build_api, invoke_*

### 3.4 main()
- Replace if/else engine creation with factory + lifecycle registration
- Engine lifecycle module registered after LifecycleGuard, before role host

### 3.5 NativeEngine
- Remove the current inline lifecycle code (s_native_engine_instance etc.)
- Use the base class make_lifecycle_module() pattern
- finalize_engine_() handles: plugin_finalize → dlclose → clear fn pointers

---

## 4. What Does NOT Change

- ScriptEngine pure virtual interface (invoke_produce, etc.)
- Engine implementations (load_script, build_api internals)
- Role host data loop
- L2 tests (use engines directly without lifecycle)
- LifecycleGuard in main (already exists)
- Lifecycle system itself (no changes needed)

---

## 5. Dependency Chain (no chicken-and-egg)

```
LifecycleGuard → static modules start (Logger, ZMQ, Crypto, etc.)
    ↓
Config loaded from disk
    ↓
Engine created (factory based on config.script.type)
    ↓
RegisterDynamicModule + LoadModule
    → startup callback: engine->initialize(tag, core)
    → engine state: INITIALIZED
    ↓
RoleHost created with engine pointer
    ↓
worker_main_():
    engine->load_script()           → state: SCRIPT_LOADED
    engine->register_slot_type()
    setup_infrastructure_()         → creates messenger, producer, inbox
    engine->build_api(ctx)          → state: API_BUILT (ctx has infrastructure ptrs)
    engine->invoke_on_init()
    run_data_loop_()                → state: RUNNING
    engine->invoke_on_stop()
    ↓
host.shutdown_()
    ↓
LifecycleGuard destructor → FinalizeApp()
    → UnloadModule("ScriptEngine:...")
    → shutdown callback: engine->finalize()  [timeout: 5s]
    → state: FINALIZED
```

No circular dependency. `initialize()` only needs tag + core (available early).
`build_api()` needs infrastructure (available after setup_infrastructure_).
`finalize()` needs nothing — it just walks back the state machine.
