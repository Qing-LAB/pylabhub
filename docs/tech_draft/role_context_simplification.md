# RoleContext Simplification — Single Structure Design

**Status**: Design (2026-04-03)
**Depends on**: Phase 2 complete (RoleAPIBase + API class composition)

---

## 1. Problem

Two structures carry the same information:

```
RoleContext (14 fields)          RoleAPIBase (Pimpl, 12+ fields)
├── role_tag                     ├── role_tag
├── uid, name, channel, ...      ├── uid, name, channel, ...
├── producer*                    ├── producer*
├── consumer*                    ├── consumer*
├── messenger*                   ├── messenger*
├── inbox_queue*                 ├── inbox_queue*
├── core*                        ├── core*
├── checksum_policy              │
├── stop_on_script_error         │
└── out_channel                  └── out_channel
```

The engine copies from RoleContext into RoleAPIBase in `build_api_()`:
```cpp
role_api_base_->set_uid(ctx_.uid);       // copy
role_api_base_->set_name(ctx_.name);     // copy
role_api_base_->set_producer(ctx_.producer); // copy
// ... 10+ more copies
```

This is redundant. Both structures exist because RoleContext predates RoleAPIBase.

---

## 2. Design: RoleAPIBase IS the context

From first principles, the engine needs:

| Need | Source | When |
|------|--------|------|
| Identity (uid, name, channel, etc.) | RoleAPIBase | build_api, log messages |
| Infrastructure (producer, consumer, messenger, inbox) | RoleAPIBase | API methods |
| Core (error counting, shutdown) | RoleAPIBase | invoke failures, finalize |
| Role tag ("prod"/"cons"/"proc") | RoleAPIBase | module selection |
| Checksum policy | Config-level | invoke (checksum on commit) |
| Stop on script error | Config-level | invoke (abort on error) |

Items 1-4 are already in RoleAPIBase. Items 5-6 are behavior flags that
belong on RoleAPIBase too (they're role-level config, not engine-specific).

**Solution**: Add `checksum_policy` and `stop_on_script_error` to RoleAPIBase.
Eliminate RoleContext entirely. The engine receives `RoleAPIBase*` directly.

---

## 3. New Data Flow

```
                    ┌─────────────────┐
                    │   Role Host     │
                    │ (producer/      │
                    │  consumer/      │
                    │  processor)     │
                    └────────┬────────┘
                             │ creates + wires
                             ▼
                    ┌─────────────────┐
                    │  RoleAPIBase    │  ← single source of truth
                    │                 │
                    │  Identity:      │
                    │    role_tag     │
                    │    uid, name    │
                    │    channel      │
                    │    out_channel  │
                    │    log_level    │
                    │    script_dir   │
                    │    role_dir     │
                    │                 │
                    │  Infrastructure:│
                    │    producer*    │
                    │    consumer*    │
                    │    messenger*   │
                    │    inbox_queue* │
                    │    core*        │
                    │                 │
                    │  Config:        │
                    │    checksum_    │
                    │    policy       │
                    │    stop_on_     │
                    │    script_error │
                    │                 │
                    │  Operations:    │
                    │    log()        │
                    │    stop()       │
                    │    metrics()    │
                    │    spinlock()   │
                    │    open_inbox() │
                    │    ...          │
                    └────────┬────────┘
                             │ passes pointer
                             ▼
                    ┌─────────────────┐
                    │  ScriptEngine   │
                    │                 │
                    │  api_ : ptr     │──→ RoleAPIBase
                    │                 │
                    │  initialize()   │  uses api_->core()
                    │  build_api_()   │  uses api_->role_tag()
                    │  invoke_*()     │  uses api_->core()
                    │  finalize()     │  nulls api_
                    └────────┬────────┘
                             │ creates wrapper (Python only)
                             ▼
                    ┌─────────────────┐
                    │ ProducerAPI     │  ← thin Python wrapper
                    │ (or Consumer/   │     holds RoleAPIBase*
                    │  Processor)     │     py::bytes → void*
                    │                 │     json → py::dict
                    └─────────────────┘
```

**Ownership**: Role host creates and owns `RoleAPIBase`. Engine holds a
non-owning pointer. Engine's lifetime is bounded by role host's lifetime
(role host calls `engine->finalize()` before destroying the base).

---

## 4. RoleAPIBase Additions

```cpp
// Add to role_api_base.hpp:

    // ── Role behavior config (set by role host from config) ───────────────

    void set_checksum_policy(hub::ChecksumPolicy p);
    void set_stop_on_script_error(bool v);
    [[nodiscard]] hub::ChecksumPolicy checksum_policy() const;
    [[nodiscard]] bool stop_on_script_error() const;
```

These are set once by the role host during setup and read by the engine
during invoke callbacks.

---

## 5. ScriptEngine Changes

### Current:
```cpp
class ScriptEngine
{
protected:
    RoleContext ctx_;           // 14-field struct, copied from build_api()
    ...
};

bool ScriptEngine::build_api(const RoleContext &ctx)
{
    auto *saved_core = ctx_.core;
    ctx_ = ctx;                // copy all 14 fields
    if (ctx_.core == nullptr)
        ctx_.core = saved_core;
    ...
}
```

### New:
```cpp
class ScriptEngine
{
protected:
    RoleAPIBase *api_{nullptr};  // non-owning, set by build_api()
    ...
};

bool ScriptEngine::build_api(RoleAPIBase &api)
{
    api_ = &api;
    ...
}
```

Engine accesses identity via `api_->uid()`, core via `api_->core()`,
role tag via `api_->role_tag()`, etc. No copying. Single source.

### Engine error handling:
```cpp
// Old: ctx_.core->inc_script_errors()
// New: api_->core()->inc_script_errors()
```

### Engine module selection:
```cpp
// Old: if (ctx_.role_tag == "prod") ...
// New: if (api_->role_tag() == "prod") ...
```

---

## 6. Role Host Changes

### Current (producer_role_host.cpp):
```cpp
RoleContext ctx;
ctx.role_tag = "prod";
ctx.uid = config_.uid();
ctx.producer = out_producer_.get();
ctx.messenger = &messenger_;
ctx.core = &core_;
// ... 10+ more fields
engine_->build_api(ctx);
```

### New:
```cpp
api_ = std::make_unique<RoleAPIBase>(core_);
api_->set_role_tag("prod");
api_->set_uid(config_.uid());
api_->set_producer(out_producer_.get());
api_->set_messenger(&messenger_);
api_->set_checksum_policy(config_.checksum().policy);
api_->set_stop_on_script_error(config_.stop_on_script_error());
// ... all wiring on the base
engine_->build_api(*api_);
```

The role host owns the base. The engine uses it. No intermediate struct.

---

## 7. RoleContext Elimination

`RoleContext` struct is deleted from `script_engine.hpp`. All consumers
migrate to `RoleAPIBase*`:

| Consumer | Old access | New access |
|----------|-----------|------------|
| ScriptEngine::build_api | `RoleContext ctx` | `RoleAPIBase &api` |
| PythonEngine::build_api_ | `ctx_.role_tag`, `ctx_.uid`, etc. | `api_->role_tag()`, `api_->uid()` |
| LuaEngine::build_api_ | `ctx_.messenger`, `ctx_.core` | `api_->messenger()` (if needed), `api_->core()` |
| NativeEngine::build_api_ | `ctx_.*` | `api_->*()` |
| EngineModuleParams | `RoleContext role_ctx` member | `RoleAPIBase *api` member |
| engine_lifecycle_startup | `p->role_ctx` | `p->api` |

---

## 8. EngineModuleParams Changes

### Current:
```cpp
struct EngineModuleParams
{
    ScriptEngine *engine{nullptr};
    RoleHostCore *core{nullptr};
    std::string tag;
    std::filesystem::path script_dir;
    std::string entry_point;
    std::string required_callback;
    SchemaSpec in_slot_spec, out_slot_spec, in_fz_spec, out_fz_spec, inbox_spec;
    std::string in_packing{"aligned"}, out_packing{"aligned"};
    RoleContext role_ctx;          // full context for build_api
    std::string module_name;
};
```

### New:
```cpp
struct EngineModuleParams
{
    ScriptEngine *engine{nullptr};
    RoleAPIBase  *api{nullptr};     // replaces both core and role_ctx
    std::string tag;
    std::filesystem::path script_dir;
    std::string entry_point;
    std::string required_callback;
    SchemaSpec in_slot_spec, out_slot_spec, in_fz_spec, out_fz_spec, inbox_spec;
    std::string in_packing{"aligned"}, out_packing{"aligned"};
    std::string module_name;
};
```

`core` is accessible via `api->core()`. `tag` could come from `api->role_tag()`
but kept separate because it's needed before `api` is set in some init paths.

---

## 9. Execution Steps

1. Add `checksum_policy` and `stop_on_script_error` to RoleAPIBase
2. Change `ScriptEngine::build_api()` signature: `RoleContext → RoleAPIBase&`
3. Replace `ctx_` member with `api_` pointer on ScriptEngine
4. Update all `ctx_.field` references to `api_->field()` in all 3 engines
5. Update role hosts to create RoleAPIBase and call `engine_->build_api(*api_)`
6. Update EngineModuleParams: replace `core` + `role_ctx` with `api`
7. Update engine_lifecycle_startup to use `p->api`
8. Delete RoleContext struct from script_engine.hpp
9. Build + test at each step

---

## 10. Impact Assessment

| File | Change type |
|------|-------------|
| `role_api_base.hpp/cpp` | Add 2 fields + 4 methods |
| `script_engine.hpp` | Delete RoleContext, change build_api signature, ctx_ → api_ |
| `python_engine.hpp/cpp` | ctx_ → api_ references throughout |
| `lua_engine.hpp/cpp` | ctx_ → api_ references throughout |
| `native_engine.hpp/cpp` | ctx_ → api_ references throughout |
| `engine_module_params.hpp/cpp` | core + role_ctx → api |
| `producer_role_host.cpp` | Create RoleAPIBase, new build_api call |
| `consumer_role_host.cpp` | Same |
| `processor_role_host.cpp` | Same |
| All engine L2 tests | RoleContext construction → RoleAPIBase construction |
