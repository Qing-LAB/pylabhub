# Tech Draft: Role Unification Design (L3)

**Status**: Design (2026-04-14)
**Branch**: `feature/lua-role-support`
**Supersedes**: `unified_role_loop.md` (kept for historical context; this doc is
the current plan). `loop_design_unified.md` remains canonical for loop timing
semantics and is referenced from §4 below.
**Baseline**: 1275/1275 tests.
**Promotes to**: HEP-CORE-0011 rewrite (post-implementation).

---

## 1. Problem statement

The framework has accumulated role-named abstractions (`hub::Producer`,
`hub::Consumer`, `ProducerRoleHost`, `ConsumerRoleHost`, `ProcessorRoleHost`,
three `CycleOps` subclasses, three `ScriptEngine::invoke_*` methods) that
**encode role identity as class shape**. After the Messenger removal (Step 7)
and HEP-CORE-0023 Phase 2 (broker-registration centralization), the work these
classes do has collapsed into nearly-identical code bodies, distinguished only
by strings and by which side of the data path is wired.

This draft replaces the accumulated role-named layers with a role-neutral
core, and relocates the legitimate places where role identity matters to the
three layers that genuinely care:

- The **user** — binary selection (`pylabhub-producer`), config files
  (`producer.json`), script callbacks (`on_produce`).
- The **config loader** — validates role-specific invariants ("producer must
  have an output schema").
- The **role host subclass** — hosts role-specific extensions when a future
  role introduces behaviors that don't fit the generic path.

Everything between is role-neutral.

---

## 2. Design principles (from iterated discussion)

### 2.1 Role identity is **data**, not class shape

Role-specific concerns live where role-specific information is semantic:
in the config, in the binary's identity, in the script callback names users
write. They do not live as branching in generic classes, and they do not live
as parallel class hierarchies restating what the config already says.

Three legitimate appearances of role vocabulary remain:

1. Binary names (`pylabhub-producer` / `-consumer` / `-processor`).
2. Config-level loader functions that encode each role's field requirements.
3. The `role_tag` string value ("producer" / "consumer" / "processor"), used
   by the role-layer helper `callback_name_for()` to look up the script
   callback. Not by the engine, not by the loop, not by the data-plane API.

### 2.2 The script engine is **framework-agnostic**

The engine's job is to load scripts and invoke callbacks by opaque handle.
It does not know the framework has roles. It does not know `on_produce` is
special. This keeps engines reusable outside this framework and keeps
framework conventions where they belong — at the role layer.

### 2.3 `RoleAPIBase` exposes the **four concern groups**, flattened

The role-facing API surface divides into four orthogonal groups:

- **Identity** — uid, name, channel, tag, directories.
- **Data-sides (Tx and Rx)** — write/read verbs flat on `RoleAPIBase`.
  `nullptr`-style defaults when a side is not wired. No sub-object accessors
  (no `producer()`, no `consumer()`, no `tx()`, no `rx()`).
- **Messaging** — band ops, inbox client, broker notifications.
- **Diagnostics / metrics** — counters, custom metrics, snapshot.

The data-sides group is deliberately flat because the caller already knows
which side it intends to operate on — adding a named container to forward
through is syntactic tax without design value. The earlier proposal to expose
`writer()`/`reader()` accessors that wrap `QueueWriter*`/`QueueReader*` was
rejected for the same reason.

### 2.4 Role-specific extension lives at the **role-host subclass** layer

A generic `RoleHost` class does 100 % of today's work for all three existing
roles. But role-specific extensibility (future role adds a web interface,
custom listener, extra thread, bespoke telemetry) is a real concern. We
preserve that extension point by keeping three empty subclasses —
`ProducerRoleHost`, `ConsumerRoleHost`, `ProcessorRoleHost` — that inherit
from `RoleHost` and provide a structural home for future role-specific
behavior without re-encoding role identity elsewhere.

Role-specific extension **data** (not behavior) lives as a **plain-struct
member** of the role-specific subclass. No polymorphic `ExtConfig` base
class, no `unique_ptr` indirection, no `dynamic_cast`. By-value, typed,
compile-time checked, zero heap allocation, no lifetime concerns.

### 2.5 Loop behavior is **derived from queue configuration**, not from a
separate policy field

Three loop-behavior concerns we considered:

1. `LoopTimingPolicy` — already in `TimingConfig`, role-agnostic. Unchanged.
2. Output overflow (Block / Drop) — a property of the output queue writer,
   set at queue creation. Already in the output-side transport config.
3. Processor input-hold — derivable at runtime from
   `api_.writer() != nullptr && api_.reader() != nullptr &&
   writer_overflow_policy == Block`. No separate field on `RoleConfig`.

There is no `loop_policy` field on `RoleConfig`. The unified `CycleOps`
reads what it needs from already-present information.

---

## 3. Target architecture

### 3.1 Layers and responsibilities

```
─────────────────────────────────────────────────────────────────────
USER-FACING LAYER (role-named; this is where "role" genuinely lives)
─────────────────────────────────────────────────────────────────────
pylabhub-producer   pylabhub-consumer   pylabhub-processor    (binaries)
producer.json       consumer.json       processor.json        (config files)
on_produce()        on_consume()        on_process()          (script callbacks)

─────────────────────────────────────────────────────────────────────
BINARY ENTRY POINTS (thin, role-named only because they ARE binaries)
─────────────────────────────────────────────────────────────────────
Each main.cpp:
    1. Parse CLI args.
    2. Construct role-specific RoleHost subclass (which loads its config).
    3. Invoke host.run().

─────────────────────────────────────────────────────────────────────
ROLE-HOST LAYER (generic base + empty subclasses as extension hooks)
─────────────────────────────────────────────────────────────────────
RoleHost                    — generic, holds RoleAPIBase + engine + CycleOps;
                              runs lifecycle via RoleAPIBase::run_role().
                              Provides on_extra_{setup,teardown,runtime} hooks.

ProducerRoleHost : RoleHost — empty today; holds ProducerExtConfig ext_.
                              Loader: RoleHost::load_common + producer-specific.
ConsumerRoleHost : RoleHost — same shape, empty ConsumerExtConfig.
ProcessorRoleHost : RoleHost — same shape, empty ProcessorExtConfig.

─────────────────────────────────────────────────────────────────────
ROLE-API LAYER (fully generic)
─────────────────────────────────────────────────────────────────────
RoleAPIBase
    - Identity: uid, name, channel, tag, directories
    - Data-sides: write_acquire/commit/discard/flexzone, read_acquire/release/
                  flexzone, last_seq, set_verify_checksum (all flat)
    - Messaging: band_*, open_inbox_client, wait_for_role, broker notifications
    - Diagnostics: counters, report_metric, snapshot_metrics_json
    - Lifecycle: start_ctrl_thread, deregister_from_broker, run_role,
                 run_data_loop, drain_inbox_sync, thread manager

CycleOps                    — single class, policy-parameterized by derived
                              data (side presence, overflow policy).
                              Reads/writes via api_.write_*/read_* directly.

─────────────────────────────────────────────────────────────────────
ENGINE LAYER (framework-agnostic)
─────────────────────────────────────────────────────────────────────
ScriptEngine (abstract)
    - resolve_callback(name) → ScriptHandle
    - invoke_cycle(ScriptHandle, const InvokeRx*, InvokeTx*, msgs)
    - invoke_event(ScriptHandle, msgs)
    - load_script, register_type, initialize, finalize
    No knowledge of role vocabulary. Callable outside pylabhub.

PythonEngine / LuaEngine / NativeEngine — concrete.

─────────────────────────────────────────────────────────────────────
ROLE-PROTOCOL HELPER (framework-aware, tiny)
─────────────────────────────────────────────────────────────────────
pylabhub::role::callback_name_for(role_tag) → const char*
    // one three-entry table; the framework's role convention
    // lives here, not in any engine and not in any class.

─────────────────────────────────────────────────────────────────────
HUB LAYER (queue infrastructure only; no role names anywhere)
─────────────────────────────────────────────────────────────────────
hub::QueueWriter / hub::QueueReader   — side-based abstractions (unchanged).
hub::ShmQueue / hub::ZmqQueue         — concrete transports (unchanged).
hub::InboxQueue / hub::InboxClient    — inbox (unchanged).

hub::Producer and hub::Consumer       — DELETED. Their state migrates into
                                        RoleAPIBase::Impl (queue, event
                                        callback slots, realtime-mode handler
                                        state, registration lifetime).
```

### 3.2 Config shape

```cpp
// src/include/utils/config/role_config.hpp — unchanged in structure.
// All existing categorical sub-configs remain. Represents the maximum
// common set for all three current roles.
struct RoleConfig {
    IdentityConfig     identity;
    ScriptConfig       script;
    TimingConfig       timing;
    BrokerConfig       broker;
    TransportConfig    transport;
    ChecksumConfig     checksum;
    InboxConfig        inbox;
    StartupConfig      startup;
    MonitoringConfig   monitoring;

    std::optional<SchemaConfig> output_schema;   // present for producer/processor
    std::optional<SchemaConfig> input_schema;    // present for consumer/processor

    std::string        role_tag;
    // NO extension pointer. NO loop_policy field.
};

// Per-role extension configs — plain structs, owned by value by the
// role-specific RoleHost subclass. Empty today; ready for future extension.
struct ProducerExtConfig  { /* empty */ };
struct ConsumerExtConfig  { /* empty */ };
struct ProcessorExtConfig { /* empty */ };
```

Rationale:

- Keeping `RoleConfig` as the maximum common set avoids restructuring the
  categorical sub-configs (which are already well-organized).
- `std::optional` for side-specific schemas because presence/absence IS the
  role-shape signal the rest of the code uses.
- No `ExtConfig` base class or pointer. Role-specific extension state is
  a plain-by-value member of the role-specific `RoleHost` subclass. This
  eliminates heap allocation, `dynamic_cast`, virtual dtor concerns, and
  null-check paths.

### 3.3 Role-host layer shape

```cpp
class RoleHost {
protected:
    RoleConfig               config_;
    std::unique_ptr<RoleAPIBase>  api_;
    std::unique_ptr<ScriptEngine> engine_;
    std::unique_ptr<CycleOps>     ops_;
    // plus handles resolved once at setup: cycle_handle_, init_handle_,
    // stop_handle_, heartbeat_handle_, inbox_handle_.

    // Extension hooks — virtual, non-pure, default empty.
    // Called from RoleAPIBase::run_role() via callbacks, or from the
    // ctrl thread, as documented for each hook.
    virtual void on_extra_setup()    {}   // after standard setup,  before engine invoke_on_init
    virtual void on_extra_teardown() {}   // before standard teardown, after invoke_on_stop
    virtual void on_extra_runtime()  {}   // periodic from ctrl thread (optional, may be no-op)

public:
    explicit RoleHost(RoleConfig cfg);
    int run();                             // non-virtual driver
    virtual ~RoleHost();

    static RoleConfig load_common(const RoleCliArgs&);   // shared loader for common fields
};

// Three empty subclasses — each holds a typed extension struct and
// encodes role-specific config validation in a static load function.

class ProducerRoleHost : public RoleHost {
    ProducerExtConfig ext_;
public:
    explicit ProducerRoleHost(const RoleCliArgs& args)
        : RoleHost(load_config_(args)),
          ext_(load_producer_ext_(args))
    {}
    // on_extra_* overrides arrive here when future producer-specific
    // behavior is introduced. Access ext_ directly — no cast needed.

private:
    static RoleConfig          load_config_(const RoleCliArgs&);   // common + producer-specific validation
    static ProducerExtConfig   load_producer_ext_(const RoleCliArgs&);
};

// ConsumerRoleHost and ProcessorRoleHost mirror this shape.
```

---

## 4. Loop semantics (reference only)

The data loop structure — inner retry acquire, deadline wait, drain, invoke,
commit/release, metrics, next_deadline — is already implemented in
`RoleAPIBase::run_data_loop()` per `loop_design_unified.md` v4. That document
remains canonical for the timing and policy details; this draft does not
revise them.

What changes in the loop implementation for this refactor:

- CycleOps stops holding `hub::Producer&` / `hub::Consumer&` / `ScriptEngine&`
  as raw fields; it holds `RoleAPIBase&` and calls `api_.write_*` /
  `api_.read_*` directly.
- The three subclasses collapse into one concrete `CycleOps`. Processor
  input-hold behavior is derived from `api_.writer() && api_.reader() &&
  overflow_policy == Block`.
- Script invocation goes through `engine_->invoke_cycle(cycle_handle_,
  make_rx(), make_tx(), msgs)`. The engine does not know which callback
  name is behind `cycle_handle_` — it was resolved at setup by
  `pylabhub::role::callback_name_for(config_.role_tag)`.

---

## 5. Implementation phases

Each phase is a single commit. Each ends with 1275/1275 tests green before
the next begins. Each is revertable in isolation.

### Phase L3.α — flatten data-plane verbs onto `RoleAPIBase`

Move `write_acquire`, `write_commit`, `write_discard`, `write_flexzone`,
`update_flexzone_checksum`, `read_acquire`, `read_release`, `read_flexzone`,
`set_verify_checksum`, `last_seq` onto `RoleAPIBase` as flat methods.
`RoleAPIBase::Impl` internally holds the queue pointers and forwards.
`RoleAPIBase::producer()` / `consumer()` accessors become private.

CycleOps classes migrate from `producer_.write_acquire(t)` to
`api_.write_acquire(t)`.

**Files**: role_api_base.{hpp,cpp}, the 3 *_role_host.cpp CycleOps sections.

### Phase L3.β — collapse 3 `CycleOps` → 1

Single `CycleOps` class. Branches on side presence (`api_.writer() != nullptr`
etc. — now spelled differently since the public API is flattened, but the
same semantic). Input-hold behavior derived at runtime, not configured.

**Files**: role_api_base.{hpp,cpp} if CycleOps lives there; otherwise a new
`cycle_ops.{hpp,cpp}`. The 3 *_role_host.cpp files lose their CycleOps
class bodies.

### Phase L3.γ — delete `hub::Producer` and `hub::Consumer`

Their internal state (queue pointer, event-callback slots, realtime-mode
handler thread + job queue + handler pointer, registration lifetime tokens,
ZMQ socket handles for ctrl/peer/data) migrates to `RoleAPIBase::Impl`.

Examples that depended on the standalone Queue/RealTime-mode API of
`hub::Producer` / `hub::Consumer` (e.g., `hub_active_service_example.cpp`,
`cpp_processor_template.cpp`) are removed or rewritten to use `RoleAPIBase`
directly. The framework no longer supports "use hub::Producer standalone";
that was a capability nobody outside the framework depended on.

Tests that instantiate `hub::Producer::create(...)` for queue-behavior
testing migrate to a small hub-layer test helper that creates a `QueueWriter`
directly.

**Files**: DELETE `src/include/utils/hub_producer.hpp`,
`src/utils/hub/hub_producer.cpp`, `src/include/utils/hub_consumer.hpp`,
`src/utils/hub/hub_consumer.cpp`. Migrate ~30-50 caller sites.

### Phase L3.δ — introduce generic `RoleHost` base with extension hooks

`RoleHost` becomes a concrete class holding `RoleAPIBase`, `ScriptEngine`,
`CycleOps`, and the handle set. Its `run()` is the former `worker_main_()`,
lifted up.

Three empty subclasses — `ProducerRoleHost`, `ConsumerRoleHost`,
`ProcessorRoleHost` — inherit from `RoleHost`. Each:

- Holds its typed `*ExtConfig` struct by value (empty today).
- Provides role-specific `load_config_()` static (calls
  `RoleHost::load_common` + adds role-specific validation).
- Provides role-specific `load_*_ext_()` static (empty today).

Three `main.cpp` entry points unchanged in structure but now construct the
subclass. The actual `worker_main_` function bodies shrink to near-nothing
because everything is in `RoleHost::run()`.

**Files**: role_api_base.{hpp,cpp} gains `RoleHost` + hooks. Three
*_role_host.{hpp,cpp} shrink to empty-subclass declarations + loaders.

### Phase L3.ε — `ScriptEngine` becomes framework-agnostic

Introduce `ScriptEngine::resolve_callback(name) → ScriptHandle` and
`invoke_cycle(ScriptHandle, const InvokeRx*, InvokeTx*, msgs)`. All three
engines (Python, Lua, Native) implement them. Default implementation:
resolve looks up the named callback; invoke_cycle calls it with the two
data args and messages.

Introduce `pylabhub::role::callback_name_for(role_tag)` helper in
`src/include/utils/role_protocol.hpp` (new small header).

Migrate `RoleHost::setup()` to resolve the handle once and pass it to
`CycleOps`. CycleOps calls `engine_.invoke_cycle(cycle_handle_, ...)`.

After the migration lands and tests pass, delete the legacy
`ScriptEngine::invoke_produce`, `invoke_consume`, `invoke_process` methods
and their concrete overrides. All script engines now expose only the
handle-based API.

Native plugin C ABI: the plugin-side function pointer slots (on_produce,
on_consume, on_process) are unchanged — the Native engine's internal
dispatch resolves which slot to return when the framework asks
`resolve_callback("on_produce")` etc. **No Native ABI break.**

**Files**: script_engine.hpp + 3 concrete engine .cpp files + role_protocol.hpp
new + RoleHost::setup changes + delete legacy engine methods.

### Phase L3.ζ — documentation update

- Rewrite HEP-CORE-0011 (`Threading Model`, `Invocation Model`, and
  `Integration Contract` sections) against the finalized surface.
- Update `loop_design_unified.md` — no semantic changes; refresh field/method
  names in its pseudo-code.
- Archive `unified_role_loop.md` (kept for history) and this draft.
- Update `API_TODO.md` to close the HEP-0011 rewrite TODO.

**Files**: docs-only.

---

## 6. Testing strategy per phase

Each phase ends with `cmake --build build -j2 && ctest --test-dir build -j2`
producing 1275/1275 green.

Phase-specific test focus:

- **α**: existing CycleOps tests exercise `api_.write_*` / `read_*` via the
  new flat surface. No new tests.
- **β**: existing role-host integration tests cover producer/consumer/processor
  scenarios. Add 1 test: processor with Block mode + Tx-full demonstrating
  input-hold (already covered, but explicit assertion via metrics).
- **γ**: existing queue tests migrate to new helper. Add no new tests —
  deleting code, not adding semantics.
- **δ**: existing lifecycle tests cover `run_role()` path. Add 1 test:
  a `RoleHost` subclass with a non-empty `on_extra_setup` / `on_extra_teardown`
  to prove the hook mechanism works.
- **ε**: existing script-callback tests cover invoke_cycle. Add 1 test:
  `engine.resolve_callback("nonexistent")` returns an invalid handle and
  `invoke_cycle` with it fails cleanly.
- **ζ**: doc-only.

Projected test count after L3: around 1278-1280 (1275 existing + 3-5 new).

---

## 7. What this refactor does NOT do

- Does NOT change user-facing script surface. `on_produce`, `on_consume`,
  `on_process`, `on_init`, `on_stop`, `on_heartbeat`, `on_inbox` remain.
- Does NOT change config JSON schemas.
- Does NOT change the broker protocol.
- Does NOT change the Native plugin ABI's function-pointer shape.
- Does NOT change queue semantics, timing semantics, or metrics schemas.

The refactor is entirely a C++-internal restructuring. External surfaces
(user scripts, config files, broker, native plugins, Python/Lua module APIs)
are stable.

---

## 8. Rationale traceback (to cross-reference against the discussion)

| Decision | Why |
|---|---|
| Delete `hub::Producer` / `hub::Consumer` | They provide no polymorphism, no composition, no unique behavior beyond "bundle a queue with some setup/event callbacks". Their "Queue vs Real-time mode" distinction is unused by the role-host framework (only by sample code that will be updated). They entangle role names into the hub layer. |
| `RoleAPIBase` data-plane verbs flat (no `tx()`/`rx()`/`writer()`/`reader()` sub-accessors) | Callers already know which side they intend to operate on. A named container to forward through is syntactic tax. |
| Keep 3 empty `*RoleHost` subclasses | Open/closed extension point for future role-specific behavior (extra thread, custom listener, bespoke telemetry). Cost today is ~3 lines per subclass. |
| Plain-struct `*ExtConfig` owned by value in subclass | Compile-time typed, zero heap alloc, no virtual dtor concern, no dynamic_cast. User confirmed no runtime polymorphism needed. |
| Engine handle-based (`resolve_callback` + `invoke_cycle`) | Engine stays framework-agnostic. Framework's role-to-callback mapping lives at role layer as a three-line helper, not inside the engine. |
| No `loop_policy` field on `RoleConfig` | Timing is already in `TimingConfig`. Overflow is already in transport/queue config. Input-hold is derivable from side presence + overflow policy at runtime. |
| Keep binary names (producer/consumer/processor) | The user genuinely selects a role by running a specific binary. This IS where role belongs. |
| Keep config loader functions role-specific | Role-specific validation invariants ("producer must have output schema") encode the role convention in the role-specific file where it's findable. |

---

## 9. Open questions before implementation begins

None. The design is fully specified pending user go-ahead on the phase plan.

If any of the phase boundaries feel wrong (e.g., combining β and γ into one
commit for simplicity), adjust before starting. Otherwise, implementation
proceeds L3.α → ζ in order with approval at each phase boundary.
