# RoleAPIBase — Unified Role API Design

**Status**: Design (2026-04-03)
**Scope**: Unify ProducerAPI/ConsumerAPI/ProcessorAPI into one language-neutral C++ class
**Depends on**: Queue ownership refactor (done), SE-04 Lua parity (done)

---

## 1. Current Gaps

### Gap 1: Role logic duplicated 4 times

Role operations (query broker, manage inbox, report metrics, access spinlocks) are
implemented in four separate places:

- `ProducerAPI` (Python-specific, `py::object` types)
- `ConsumerAPI` (Python-specific, copy-pasted from ProducerAPI)
- `ProcessorAPI` (Python-specific, copy-pasted, superset of both)
- Lua engine `lua_api_*` static functions (reimplemented from scratch)

All four do the same thing: call `messenger_->list_channels()`, call
`core_->report_metric()`, call `core_->request_stop()`, etc. The only difference is
how the result is wrapped for the script language.

### Gap 2: API classes are Python-contaminated

ProducerAPI/ConsumerAPI/ProcessorAPI take `py::bytes`, return `py::dict`, hold
`py::object`. This makes the API classes unusable by Lua or Native engines. The Lua
engine must reimplement all ~40 methods independently.

### Gap 3: RoleContext carries too much

RoleContext has 14 fields (messenger, producer, consumer, inbox_queue, uid, name,
channel, out_channel, log_level, script_dir, role_dir, core, checksum_policy,
stop_on_script_error). The engine copies these into its API object field by field
(10+ `set_*` calls per role, 3 roles = 30+ calls in `build_api_`).

### Gap 4: open_inbox_client on wrong layer

`open_inbox_client()` lives on ScriptEngine because ScriptEngine held the messenger
pointer. But it's pure C++ broker logic — query broker for peer endpoint, create
InboxClient, cache it. No language involvement. Belongs at the role API level.

---

## 2. Design Principles

1. **RoleAPIBase is pure C++** — no pybind11, no Lua, no language-specific types.
   All method signatures use C++ standard types, nlohmann::json, and pylabhub types.
   Lives in `src/include/utils/` as part of `pylabhub-utils` shared library.

2. **One implementation, multiple bindings** — RoleAPIBase implements role logic once.
   Python, Lua, and Native C each provide thin binding layers that convert between
   language types and C++ types. The binding layers are the engine's responsibility,
   consistent with how ScriptEngine already handles invoke_produce/invoke_consume.

3. **Direction-agnostic with optional sides** — RoleAPIBase holds optional `Producer*`
   and `Consumer*`. Methods that operate on a missing side return safe defaults (0,
   empty string, nullptr). The role is defined by which pointers are wired at
   construction, not by class hierarchy. No subclasses.

4. **Data-driven metrics** — The metrics JSON structure (the one real difference
   between roles) is determined by which pointers are non-null:
   - Producer (has producer, no consumer) → `"queue": {...}`
   - Consumer (has consumer, no producer) → `"queue": {...}`
   - Processor (has both) → `"in_queue": {...}, "out_queue": {...}`
   No virtual methods needed.

5. **Pimpl for ABI stability** — mandatory for all public classes in pylabhub-utils:
   - All data members in Impl struct (strings, atomics, maps, pointers)
   - Destructor defined in .cpp
   - No inline method bodies in the header
   - Forward-declare implementation types; only include what public signatures need
   - No virtual methods — no vtable means no ABI-sensitive layout changes
   - Adding a method is ABI-compatible; adding a virtual would break ABI

6. **Language is a config concern** — `script.type` in JSON config determines which
   engine is instantiated. The role API has zero language awareness. ScriptEngine
   handles all language-specific binding, same as it does for data callbacks.

7. **Two-header architecture** — separates ABI-stable base from type-safe templates:
   - `role_api_base.hpp` — ABI-stable, Pimpl, exported from shared library. Uses
     `void*` for data, `size_t` for sizes. Used by all engines and role hosts.
   - `role_api.hpp` — header-only, NOT exported. Provides C++ template wrappers
     with compile-time type safety on top of RoleAPIBase. Used by C++ native users
     and the RAII template path (Group D). Not linked into the shared library.

   This is the same pattern as `data_block.hpp` (exported, ABI-stable) vs the
   template RAII factories in `hub_producer.hpp` (header-only, type-safe wrappers).

---

## 3. What Differentiates Roles

| Aspect | Producer | Consumer | Processor |
|--------|----------|----------|-----------|
| Channel ownership | Creates (REG_REQ) | Subscribes (CONSUMER_REG_REQ) | Both |
| Data flow | Output only | Input only | Both |
| Script callback | invoke_produce(tx) | invoke_consume(rx) | invoke_process(rx, tx) |
| Data loop | Acquire output → invoke → commit | Acquire input → invoke → release | Dual-acquire with Block mode |
| Metrics queue key | "queue" | "queue" | "in_queue" / "out_queue" |
| Metrics role fields | out_written, drops | in_received | Both + nested ctrl_queue_dropped |

**Key insight**: The role differences are in the **data loop** (role host layer) and
**callback signatures** (ScriptEngine layer), NOT in the API class. The API class
provides the same operations regardless of role — it just accesses whichever
infrastructure side exists.

**What does NOT differ** (all in RoleAPIBase):
- Identity, control, logging, stop/critical_error, stop_reason
- Broker queries (notify_channel, broadcast_channel, list_channels, shm_info)
- Messaging (broadcast, send — any role can send to any role's inbox)
- Inbox client management (open_inbox_client, wait_for_role, close_all_inbox_clients)
- All diagnostics (script_errors, loop_overrun_count, last_cycle_work_us)
- Spinlock access, flexzone access, checksum operations
- Custom metrics (report_metric, report_metrics, clear_custom_metrics)
- Input-side operations (return 0/empty when no Consumer)
- Output-side operations (return 0/empty when no Producer)

---

## 4. RoleAPIBase Interface

### 4.1 Header: `src/include/utils/role_api_base.hpp`

```cpp
class PYLABHUB_UTILS_EXPORT RoleAPIBase
{
public:
    explicit RoleAPIBase(RoleHostCore &core);
    ~RoleAPIBase();

    RoleAPIBase(const RoleAPIBase &) = delete;
    RoleAPIBase &operator=(const RoleAPIBase &) = delete;
    RoleAPIBase(RoleAPIBase &&) noexcept;
    RoleAPIBase &operator=(RoleAPIBase &&) noexcept;

    // ── Host wiring (called once by role host after setup_infrastructure_) ────

    void set_producer(Producer *p);
    void set_consumer(Consumer *c);
    void set_messenger(Messenger *m);
    void set_inbox_queue(InboxQueue *q);
    void set_uid(std::string uid);
    void set_name(std::string name);
    void set_channel(std::string c);         // primary (or input for processor)
    void set_out_channel(std::string c);     // output (processor only)
    void set_log_level(std::string l);
    void set_script_dir(std::string d);
    void set_role_dir(std::string d);

    // ── Identity ──────────────────────────────────────────────────────────────

    const std::string &uid() const;
    const std::string &name() const;
    const std::string &channel() const;
    const std::string &out_channel() const;
    const std::string &log_level() const;
    const std::string &script_dir() const;
    const std::string &role_dir() const;
    std::string logs_dir() const;
    std::string run_dir() const;

    // ── Control ───────────────────────────────────────────────────────────────

    void log(const std::string &level, const std::string &msg);
    void stop();
    void set_critical_error();
    bool critical_error() const;
    std::string stop_reason() const;

    // ── Broker queries ────────────────────────────────────────────────────────

    void notify_channel(const std::string &target, const std::string &event,
                        const std::string &data);
    void broadcast_channel(const std::string &target, const std::string &msg,
                           const std::string &data);
    std::vector<nlohmann::json> list_channels();
    std::string request_shm_info(const std::string &channel = {});

    // ── Messaging (any role → any role's inbox) ───────────────────────────────

    bool broadcast(const void *data, size_t size);
    bool send(const std::string &identity_hex, const void *data, size_t size);
    std::vector<std::string> connected_consumers();

    // ── Inbox client management ───────────────────────────────────────────────

    struct InboxOpenResult
    {
        std::shared_ptr<InboxClient> client;
        SchemaSpec spec;
        size_t item_size{0};
    };

    std::optional<InboxOpenResult> open_inbox_client(const std::string &target_uid);
    bool wait_for_role(const std::string &uid, int timeout_ms = 5000);
    void close_all_inbox_clients();

    // ── Output side (safe defaults when no Producer) ──────────────────────────

    void *write_flexzone();
    const void *read_flexzone() const;
    size_t flexzone_size() const;
    bool update_flexzone_checksum();
    uint64_t out_slots_written() const;
    uint64_t out_drop_count() const;
    size_t out_capacity() const;
    std::string out_policy() const;

    // ── Input side (safe defaults when no Consumer) ───────────────────────────

    uint64_t in_slots_received() const;
    uint64_t last_seq() const;
    void update_last_seq(uint64_t seq);
    size_t in_capacity() const;
    std::string in_policy() const;
    void set_verify_checksum(bool enable);

    // ── Diagnostics ───────────────────────────────────────────────────────────

    uint64_t script_error_count() const;
    uint64_t loop_overrun_count() const;
    uint64_t last_cycle_work_us() const;
    uint64_t ctrl_queue_dropped() const;

    // ── Spinlocks (delegates to whichever side has SHM) ───────────────────────

    SharedSpinLock get_spinlock(size_t index);
    uint32_t spinlock_count() const;

    // ── Custom metrics ────────────────────────────────────────────────────────

    void report_metric(const std::string &key, double value);
    void report_metrics(const std::unordered_map<std::string, double> &kv);
    void clear_custom_metrics();

    // ── Metrics snapshot (data-driven, no virtual) ────────────────────────────

    nlohmann::json snapshot_metrics_json() const;

    // ── Shared script state ───────────────────────────────────────────────────

    void set_shared_data(const std::string &key, const nlohmann::json &value);
    nlohmann::json get_shared_data(const std::string &key) const;

    // ── Infrastructure access (for engine binding layers) ─────────────────────
    // These expose the internal pointers so engines can perform language-specific
    // wrapping (e.g. Python InboxHandle, Lua FFI cast). The pointers are owned
    // by the role host — RoleAPIBase does not manage their lifetime.

    RoleHostCore *core() const;
    Producer *producer() const;
    Consumer *consumer() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
```

### 4.2 Type-safe template header: `src/include/utils/role_api.hpp`

Header-only. NOT exported. Provides compile-time type safety on top of
RoleAPIBase's `void*`/`size_t` interface. Used by C++ native users and
the RAII template path.

```cpp
#pragma once
#include "utils/role_api_base.hpp"
#include <cstddef>
#include <span>
#include <type_traits>

namespace pylabhub::hub
{

/// Type-safe flexzone access.
template <typename T>
T *write_flexzone(RoleAPIBase &api)
{
    static_assert(std::is_standard_layout_v<T>, "Flexzone type must be standard-layout");
    if (api.flexzone_size() < sizeof(T)) return nullptr;
    return static_cast<T *>(api.write_flexzone());
}

template <typename T>
const T *read_flexzone(const RoleAPIBase &api)
{
    static_assert(std::is_standard_layout_v<T>, "Flexzone type must be standard-layout");
    if (api.flexzone_size() < sizeof(T)) return nullptr;
    return static_cast<const T *>(api.read_flexzone());
}

/// Type-safe broadcast/send.
template <typename T>
bool broadcast(RoleAPIBase &api, const T &msg)
{
    static_assert(std::is_standard_layout_v<T>, "Message type must be standard-layout");
    return api.broadcast(&msg, sizeof(T));
}

template <typename T>
bool send(RoleAPIBase &api, const std::string &identity_hex, const T &msg)
{
    static_assert(std::is_standard_layout_v<T>, "Message type must be standard-layout");
    return api.send(identity_hex, &msg, sizeof(T));
}

} // namespace pylabhub::hub
```

This header grows as the RAII template path is rewritten (Group D). Future additions:
- `push<SlotType, FlexType>(api, timeout)` — type-safe slot write transaction
- `pull<SlotType, FlexType>(api, timeout)` — type-safe slot read transaction
- `SlotRef<T>` / `ConstSlotRef<T>` integration with RoleAPIBase

### 4.3 Impl struct: `src/utils/role_api_base.cpp`

```cpp
struct RoleAPIBase::Impl
{
    RoleHostCore *core;

    Producer    *producer{nullptr};
    Consumer    *consumer{nullptr};
    Messenger   *messenger{nullptr};
    InboxQueue  *inbox_queue{nullptr};

    std::string uid;
    std::string name;
    std::string channel;        // primary (or input for processor)
    std::string out_channel;    // processor output (empty otherwise)
    std::string log_level;
    std::string script_dir;
    std::string role_dir;

    // Inbox client cache (keyed by target_uid).
    std::unordered_map<std::string, std::shared_ptr<InboxClient>> inbox_clients;

    // Consumer sequence tracking.
    std::atomic<uint64_t> last_seq{0};

    // Shared script state (nlohmann::json for type flexibility).
    std::unordered_map<std::string, nlohmann::json> shared_data;
    mutable std::mutex shared_data_mu;
};
```

### 4.3 Metrics JSON — data-driven structure

**Implementation note (2026-04-03):** Role counters (out_written, in_received,
drops, script_errors) are always emitted regardless of which infrastructure
pointers are set. These are core-level counters, always valid. Queue metrics
("queue" / "in_queue" / "out_queue") are gated on pointers. ctrl_queue_dropped
defaults to 0 when no infrastructure is connected.

```cpp
nlohmann::json RoleAPIBase::snapshot_metrics_json() const
{
    nlohmann::json result;
    const bool has_in  = (pImpl->consumer != nullptr);
    const bool has_out = (pImpl->producer != nullptr);

    // Queue metrics: key depends on which sides exist.
    if (has_in && has_out) {
        // Processor: dual queues.
        nlohmann::json iq, oq;
        queue_metrics_to_json(iq, pImpl->consumer->queue_metrics());
        queue_metrics_to_json(oq, pImpl->producer->queue_metrics());
        result["in_queue"] = std::move(iq);
        result["out_queue"] = std::move(oq);
    } else if (has_out) {
        nlohmann::json q;
        queue_metrics_to_json(q, pImpl->producer->queue_metrics());
        result["queue"] = std::move(q);
    } else if (has_in) {
        nlohmann::json q;
        queue_metrics_to_json(q, pImpl->consumer->queue_metrics());
        result["queue"] = std::move(q);
    }

    // Loop metrics (always present).
    nlohmann::json lm;
    loop_metrics_to_json(lm, pImpl->core->loop_metrics());
    result["loop"] = std::move(lm);

    // Role metrics (data-driven).
    nlohmann::json role;
    if (has_out) {
        role["out_written"] = pImpl->core->out_written();
        role["drops"]       = pImpl->core->drops();
    }
    if (has_in)
        role["in_received"] = pImpl->core->in_received();
    role["script_errors"] = pImpl->core->script_errors();

    // ctrl_queue_dropped: scalar for single-side, nested for dual.
    if (has_in && has_out) {
        role["ctrl_queue_dropped"] = {
            {"input",  pImpl->consumer->ctrl_queue_dropped()},
            {"output", pImpl->producer->ctrl_queue_dropped()}
        };
    } else if (has_out)
        role["ctrl_queue_dropped"] = pImpl->producer->ctrl_queue_dropped();
    else if (has_in)
        role["ctrl_queue_dropped"] = pImpl->consumer->ctrl_queue_dropped();
    result["role"] = std::move(role);

    // Inbox metrics (if configured).
    if (pImpl->inbox_queue) {
        nlohmann::json ib;
        inbox_metrics_to_json(ib, pImpl->inbox_queue->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    // Custom metrics.
    auto cm = pImpl->core->custom_metrics_snapshot();
    if (!cm.empty())
        result["custom"] = nlohmann::json(cm);

    return result;
}
```

---

## 5. RoleContext Changes

### Current:
```cpp
struct RoleContext
{
    std::string role_tag;              // "prod", "cons", "proc"
    std::string uid, name, channel, out_channel;
    std::string log_level, script_dir, role_dir;
    Messenger   *messenger{nullptr};
    Producer    *producer{nullptr};
    Consumer    *consumer{nullptr};
    InboxQueue  *inbox_queue{nullptr};
    ChecksumPolicy checksum_policy{Enforced};
    RoleHostCore *core{nullptr};
    bool stop_on_script_error{false};
};
```

### New:
```cpp
struct RoleContext
{
    std::string role_tag;              // "prod", "cons", "proc"
    RoleAPIBase *api{nullptr};         // fully-wired role API
    RoleHostCore *core{nullptr};       // core for engine error handling
    ChecksumPolicy checksum_policy{Enforced};
    bool stop_on_script_error{false};
};
```

Identity, messenger, producer, consumer, inbox_queue — all inside RoleAPIBase.
Engine accesses them through `ctx.api->uid()`, `ctx.api->channel()`, etc.

---

## 6. Engine Changes

### 6.1 Python Engine

Before (build_api_, 60+ lines of role-specific branching):
```cpp
if (ctx_.role_tag == "prod") {
    producer_api_ = make_unique<ProducerAPI>(*ctx_.core);
    api.set_producer(ctx_.producer);
    api.set_messenger(ctx_.messenger);
    // ... 10+ set_* calls, register ProducerAPI pybind11 class
}
else if (ctx_.role_tag == "cons") {
    // ... 10+ set_* calls, register ConsumerAPI pybind11 class
}
else if (ctx_.role_tag == "proc") {
    // ... 15+ set_* calls, register ProcessorAPI pybind11 class
}
```

After (one block, no branching):
```cpp
// ctx_.api is already fully wired by the role host.
// Register pybind11 bindings that wrap ctx_.api->method() calls.
auto &api = *ctx_.api;
m.def("uid",       [&api]() { return api.uid(); });
m.def("stop",      [&api]() { api.stop(); });
m.def("broadcast",  [&api](py::bytes data) {
    auto s = data.cast<std::string>();
    return api.broadcast(s.data(), s.size());
});
m.def("metrics",    [&api]() { return json_to_pydict(api.snapshot_metrics_json()); });
m.def("open_inbox", [&api, this](const std::string &uid) {
    auto result = api.open_inbox_client(uid);
    if (!result) return py::cast<py::object>(py::none());
    return build_inbox_handle(*result);  // Python-specific wrapping
});
// ... all methods, one registration, no role branching
```

### 6.2 Lua Engine

Before (40+ static functions reimplementing role logic):
```cpp
static int lua_api_stop(lua_State *L) {
    auto *self = get_self(L);
    self->ctx_.core->request_stop();     // reimplements ProducerAPI::stop()
    return 0;
}
static int lua_api_broadcast(lua_State *L) {
    auto *self = get_self(L);
    auto *m = self->ctx_.messenger;      // reimplements ProducerAPI::broadcast()
    if (!m) { lua_pushboolean(L, 0); return 1; }
    // ... extract Lua string, call messenger
}
```

After (delegates to RoleAPIBase):
```cpp
static int lua_api_stop(lua_State *L) {
    get_api(L)->stop();                  // one line — delegates to base
    return 0;
}
static int lua_api_broadcast(lua_State *L) {
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, get_api(L)->broadcast(data, len));
    return 1;
}
```

### 6.3 Native Engine

`PlhNativeContext` function pointers wire to RoleAPIBase:
```cpp
native_ctx._api = ctx_.api;  // store pointer for callback dispatch
native_ctx.request_stop = [](const PlhNativeContext *ctx) {
    static_cast<RoleAPIBase*>(ctx->_api)->stop();
};
native_ctx.report_metric = [](const PlhNativeContext *ctx, const char *k, double v) {
    static_cast<RoleAPIBase*>(ctx->_api)->report_metric(k, v);
};
```

---

## 7. Role Host Changes

### Current (producer_role_host.cpp, ~20 lines of RoleContext assembly):
```cpp
RoleContext ctx;
ctx.role_tag = "prod";
ctx.uid = config_.uid();
ctx.name = config_.name();
ctx.channel = channel_name_;
ctx.producer = out_producer_.get();
ctx.messenger = &messenger_;
ctx.inbox_queue = inbox_queue_.get();
ctx.core = &core_;
ctx.log_level = config_.log_level();
ctx.script_dir = config_.script_dir();
ctx.role_dir = config_.role_dir();
ctx.checksum_policy = config_.checksum().policy;
ctx.stop_on_script_error = config_.stop_on_script_error();
engine_->build_api(ctx);
```

### New:
```cpp
api_ = std::make_unique<RoleAPIBase>(core_);
api_->set_producer(out_producer_.get());
api_->set_messenger(&messenger_);
api_->set_inbox_queue(inbox_queue_.get());
api_->set_uid(config_.uid());
api_->set_name(config_.name());
api_->set_channel(channel_name_);
api_->set_log_level(config_.log_level());
api_->set_script_dir(config_.script_dir());
api_->set_role_dir(config_.role_dir());

RoleContext ctx;
ctx.role_tag = "prod";
ctx.api = api_.get();
ctx.core = &core_;
ctx.checksum_policy = config_.checksum().policy;
ctx.stop_on_script_error = config_.stop_on_script_error();
engine_->build_api(ctx);
```

Similar for consumer (set_consumer instead of set_producer) and processor (set both +
set_out_channel).

---

## 8. Shared Script State

**Implementation (2026-04-03):** RoleAPIBase delegates to RoleHostCore's existing
`StateValue = std::variant<int64_t, double, bool, std::string>` shared data.
No new storage — reuses the core's map with the core's mutex.

```cpp
using StateValue = RoleHostCore::StateValue;
void set_shared_data(const std::string &key, StateValue value);
std::optional<StateValue> get_shared_data(const std::string &key) const;
void remove_shared_data(const std::string &key);
void clear_shared_data();
```

Python `shared_data_` remains a `py::object` (dict) on each API wrapper class
for backward compatibility with existing scripts that use `api.shared_data`.
Phase 3 may migrate this to the base's StateValue map.

---

## 9. What Gets Deleted (Phase 3+)

| File | Status |
|------|--------|
| `src/producer/producer_api.hpp` | Phase 3: **DELETE** (thin wrapper, logic in RoleAPIBase) |
| `src/producer/producer_api.cpp` | Phase 3: **DELETE** (pybind11 registration moves to engine) |
| `src/consumer/consumer_api.hpp/.cpp` | Phase 3: **DELETE** |
| `src/processor/processor_api.hpp/.cpp` | Phase 3: **DELETE** |
| `ProducerSpinLockPy` / `ConsumerSpinLockPy` / `ProcessorSpinLockPy` | Phase 6: unify |
| 40+ `lua_api_*` reimplementations | Phase 5: **REPLACE** with RoleAPIBase delegation |
| `ScriptEngine::open_inbox_client()` | Phase 6: **DELETE** (duplicated in RoleAPIBase) |

---

## 10. What Was Created

| File | Description |
|------|-------------|
| `src/include/utils/role_api_base.hpp` | Public header — ABI-stable, Pimpl, exported |
| `src/utils/service/role_api_base.cpp` | Implementation — all method bodies |
| `src/include/utils/schema_types.hpp` | Schema types in hub:: namespace (from reorganization) |
| `src/include/utils/schema_utils.hpp` | Schema utilities moved to utils layer |
| `src/include/utils/role_api.hpp` | Planned — type-safe C++ templates (Group D) |

---

## 11. Execution Plan

### Phase 1: Create RoleAPIBase — ✅ DONE (2026-04-03)

- RoleAPIBase header (Pimpl, ABI-stable) + full implementation
- open_inbox_client() implemented with compute_schema_size (bug fixed)
- role_tag field added for consistent log format
- Schema reorganization: schema_types.hpp + schema_utils.hpp in hub:: namespace
- 1315 tests pass

### Phase 2: Migrate Python API classes — ✅ DONE (2026-04-03)

**Pattern used: composition (not inheritance).** Each API class holds a
`RoleAPIBase*` (non-owning, engine owns the base). PythonEngine creates
RoleAPIBase in `build_api_()` from RoleContext fields, then passes to the
role-specific wrapper.

- ProducerAPI, ConsumerAPI, ProcessorAPI all delegate to RoleAPIBase
- Python-specific wrapping (py::bytes, py::dict, InboxHandle, SpinLockPy) stays
- metrics() uses base->snapshot_metrics_json() → json.loads() → py::dict
- Role metrics always emit all core counters (data-driven, not role-gated)
- Direction objects (PyTxChannel, PyRxChannel, PyInboxMsg) preserved in modules
- Metrics API tests rewritten with TestContext helper
- All 1315 tests pass

### Phase 3: Wire role hosts to RoleAPIBase

**Step 10**: Modify ProducerRoleHost to create `RoleAPIBase` and pass it via
RoleContext (§7). PythonEngine's `build_api_` receives `ctx_.api` instead of
constructing ProducerAPI. The pybind11 registration wraps `ctx_.api->method()`
directly. Delete ProducerAPI class. Verify tests pass.

**Step 11**: Same for ConsumerRoleHost + ConsumerAPI deletion. Verify tests.

**Step 12**: Same for ProcessorRoleHost + ProcessorAPI deletion. Verify tests.

### Phase 4: Simplify RoleContext

**Step 13**: Remove redundant RoleContext fields (uid, name, channel, out_channel,
log_level, script_dir, role_dir, messenger, producer, consumer, inbox_queue).
Keep: role_tag, api, core, checksum_policy, stop_on_script_error.
Update all RoleContext consumers (engines, EngineModuleParams).

### Phase 5: Migrate Lua and Native engines

**Step 14**: Modify Lua engine to use `ctx_.api->method()` instead of
reimplementing role logic in `lua_api_*` functions. Each static function
becomes a thin Lua↔C++ type conversion + delegation. Verify Lua tests pass.

**Step 15**: Modify Native engine to wire `PlhNativeContext` function pointers
to `ctx_.api->method()`. Verify native engine tests pass.

### Phase 6: Cleanup

**Step 16**: Delete `ScriptEngine::open_inbox_client()` forwarding stub.
Delete `ScriptEngine::InboxOpenResult` (moved to RoleAPIBase).

**Step 17**: Delete all three SpinLockPy classes. Unify to one SpinLockPy
in the Python engine (or in a shared scripting header).

**Step 18**: Delete empty ProducerAPI/ConsumerAPI/ProcessorAPI files if not
already deleted in Phase 3.

**Step 19**: Update documentation: HEP-0011, API_TODO, MEMORY.md.

---

## 12. Testing Strategy

| Phase | Test impact | Expectation |
|-------|-----------|-------------|
| Phase 1 (create) | New L2 tests for RoleAPIBase | All existing tests unchanged |
| Phase 2 (bridge) | No test changes | All tests pass — bridge delegates to same logic |
| Phase 3 (wire) | No test changes | All tests pass — same methods called through base |
| Phase 4 (simplify) | No test changes | All tests pass — RoleContext fields removed |
| Phase 5 (Lua/Native) | No test changes | All tests pass — same logic, different call path |
| Phase 6 (cleanup) | No test changes | All tests pass — dead code removed |

**Key invariant**: At every step, all existing tests pass. No test modifications
needed until Phase 6 documentation. Python and Lua scripts never change — they
call the same method names, receive the same return types.

---

## 13. Risk Assessment

| Risk | Mitigation |
|------|------------|
| pybind11 registration changes break Python scripts | Phase 2 bridge keeps registrations identical |
| Lua type conversion differences | Phase 5 is isolated; Lua tests catch any mismatch |
| Metrics JSON structure changes | Phase 1 L2 test verifies exact JSON output |
| open_inbox_client move breaks inbox | Phase 1 step 4 uses forwarding stub for compatibility |
| Shared state (py::object → json) breaks scripts | Test with nested dicts, numeric types, strings |
| ABI break in pylabhub-utils.so | Pimpl — only forward declaration in header |
