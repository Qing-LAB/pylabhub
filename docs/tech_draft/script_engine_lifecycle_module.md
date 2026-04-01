# Script Engine as Lifecycle Dynamic Module

**Status**: Design revised (2026-03-31)
**Scope**: All script engines managed as lifecycle dynamic modules

---

## 1. Separation of Concerns

**Role host's job:** setup_infrastructure + run data loop + teardown_infrastructure.

**Engine's job:** initialize + load_script + register_slot_type + build_api + invoke + finalize.

The only coupling: role host assembles a RoleContext (containing infrastructure pointers)
and passes it to the engine's build_api. The engine does not create infrastructure.

---

## 2. Two Independent Lifecycles

```
Role host:   setup_infrastructure()  ←→  teardown_infrastructure()
Engine:      LoadModule (startup)    ←→  UnloadModule (shutdown)
```

Both managed by the lifecycle system. Infrastructure (DataExchangeHub, ZMQ, etc.)
is already a static lifecycle module. Engine becomes a dynamic lifecycle module.
The role host sequences them: infrastructure first, engine second, then run.

---

## 3. EngineModuleParams

A struct that bundles everything the engine needs. Created by the role host,
passed as userdata to the lifecycle module.

```cpp
struct EngineModuleParams {
    // Engine instance
    ScriptEngine *engine;
    RoleHostCore *core;

    // Startup parameters (Group A: from config)
    std::string tag;                    // "prod" / "cons" / "proc"
    std::filesystem::path script_dir;
    std::string entry_point;            // "init.lua" / "__init__.py"
    std::string required_callback;      // "on_produce" / "on_consume" / "on_process"
    SchemaSpec slot_spec;
    SchemaSpec fz_spec;                 // empty if no flexzone
    SchemaSpec inbox_spec;              // empty if no inbox
    std::string packing;

    // Infrastructure context (Group C: filled after setup_infrastructure)
    RoleContext role_ctx;

    // Validation
    uint64_t magic{0x454E4750};         // "ENGP"
    uint64_t lifecycle_key{0};          // generation key from lifecycle
};
```

---

## 4. Flow

```
worker_main_() [on worker thread]:

    // 1. Role host: create infrastructure (no engine needed)
    setup_infrastructure_();

    // 2. Role host: assemble params
    auto params = make_engine_params();     // fills from config + infrastructure

    // 3. Register + Load engine as lifecycle module
    ModuleDef mod("ScriptEngine:lua:PROD-A", params.get(), validate_fn);
    params->lifecycle_key = mod.userdata_key();
    mod.add_dependency("Logger");
    mod.set_startup(engine_startup);
    mod.set_shutdown(engine_shutdown, 5000ms);
    RegisterDynamicModule(std::move(mod));
    LoadModule("ScriptEngine:lua:PROD-A");
    // → engine_startup: initialize → load_script → register_slot_type → build_api
    // Engine fully ready.

    // 4. Run
    engine_->invoke_on_init();
    run_data_loop_();
    engine_->invoke_on_stop();

    // 5. Shutdown (on correct thread, before infrastructure teardown)
    engine_->finalize();
    UnloadModule("ScriptEngine:lua:PROD-A");    // shutdown callback is no-op

    // 6. Teardown infrastructure
    teardown_infrastructure_();
```

---

## 5. Startup Callback

```cpp
void engine_startup(const char * /*arg*/, void *userdata) {
    auto *p = static_cast<EngineModuleParams*>(userdata);

    if (!p->engine->initialize(p->tag, p->core))
        throw std::runtime_error("engine initialize failed");

    if (!p->engine->load_script(p->script_dir, p->entry_point, p->required_callback))
        throw std::runtime_error("load_script failed");

    if (p->slot_spec.has_schema)
        if (!p->engine->register_slot_type(p->slot_spec, "SlotFrame", p->packing))
            throw std::runtime_error("register SlotFrame failed");

    if (p->fz_spec.has_schema)
        if (!p->engine->register_slot_type(p->fz_spec, "FlexFrame", p->packing))
            throw std::runtime_error("register FlexFrame failed");

    if (p->inbox_spec.has_schema)
        if (!p->engine->register_slot_type(p->inbox_spec, "InboxFrame", p->packing))
            throw std::runtime_error("register InboxFrame failed");

    if (!p->engine->build_api(p->role_ctx))
        throw std::runtime_error("build_api failed");
}
```

Exceptions are caught by loadModuleInternal → module marked FAILED.

---

## 6. Shutdown Callback

```cpp
void engine_shutdown(const char * /*arg*/, void *userdata) {
    auto *p = static_cast<EngineModuleParams*>(userdata);
    p->engine->finalize();  // idempotent
}
```

Normal path: engine already finalized by role host → no-op.
Crash path: lifecycle calls this with timeout guard → finalize runs.

---

## 7. Module Naming

| Engine | Name | Policy |
|--------|------|--------|
| Python | `"ScriptEngine:python"` | Process singleton |
| Lua | `"ScriptEngine:lua:<role_uid>"` | Per-role |
| Native | `"ScriptEngine:native:<lib>:<role_uid>"` | Per-role |

---

## 8. Engine State Machine

```
Unloaded → Initialized → ScriptLoaded → ApiBuilt → Finalized
```

finalize() is idempotent. It checks the state and cleans up whatever was set up.

---

## 9. Refactor: Move inbox register_slot_type out of setup_infrastructure

Currently `setup_infrastructure_()` calls `engine_->register_slot_type(InboxFrame)`.
This is engine work misplaced in infrastructure code. Move it to the engine
startup callback. The inbox schema comes from config (Group A). The inbox
queue buffer size validation should use the schema spec directly.

---

## 10. Params Lifetime

- Created: by role host at start of worker_main_(), on worker thread
- Filled: Group A from config, Group C after setup_infrastructure_()
- Consumed: by LoadModule startup callback (synchronous, same thread)
- Shutdown: by UnloadModule shutdown callback (idempotent finalize)
- Destroyed: when role host is destroyed (unique_ptr member)

Normal path: role host calls finalize() + UnloadModule before destruction → safe.
Crash path: validate function checks magic + key → stale params → callback skipped.

---

## 11. Changes Required

1. **LifecycleCallback signature** (DONE): `void(*)(const char*, void*)`
2. **ModuleDef userdata constructor** (DONE): `ModuleDef(name, userdata, validate)`
3. **EngineModuleParams struct**: new, in ScriptEngine header or separate header
4. **engine_startup / engine_shutdown**: static functions, shared by all roles
5. **Role host worker_main_()**: replace direct engine calls with LoadModule/UnloadModule
6. **Move inbox register_slot_type**: from setup_infrastructure_() to startup callback
7. **ScriptEngine**: add EngineState enum, make finalize() idempotent
8. **NativeEngine**: remove internal lifecycle code (replaced by base mechanism)
