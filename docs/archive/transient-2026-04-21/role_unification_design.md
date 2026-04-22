# Tech Draft: Role Unification Design (L3) — CLOSED

**Status**: ✅ CLOSED 2026-04-21 — binary unification landed (HEP-CORE-0024
Phases 15-22). Architectural consolidation is considered **final and
complete**; L3.β and L3.ε are intentionally not pursued (rationale below).
**Branch**: `feature/lua-role-support`
**Supersedes**: `unified_role_loop.md` (archived 2026-04-16 → HEP-0011).
`loop_design_unified.md` (archived 2026-04-16 → HEP-0008 §2.2).
**Final baseline**: 1456/1456 tests (2026-04-21).

### Implementation Status (verified against code 2026-04-21)

| Phase | Status | Notes |
|-------|--------|-------|
| L3.α — flatten data-plane verbs | ✅ DONE | RoleAPIBase owns write_acquire/read_acquire etc. |
| L3.β — collapse 3 CycleOps → 1 | ⚪ **DEFERRED INDEFINITELY** | Code keeps `ProducerCycleOps` / `ConsumerCycleOps` / `ProcessorCycleOps` as `final` concrete classes in `src/utils/service/cycle_ops.hpp`, duck-typed via the `run_data_loop` template — see **"Why L3.β/L3.ε were not pursued"** below. |
| L3.γ — delete hub::Producer/Consumer | ✅ DONE | A6.1-A6.3 (2026-04-15) |
| L3.δ — generic RoleHost base | ✅ DONE | `RoleHostBase` added as part of HEP-CORE-0024 Phase 15 (2026-04-18); all three role hosts inherit. |
| L3.ε — ScriptEngine framework-agnostic | ⚪ **DEFERRED INDEFINITELY** | Code keeps typed `invoke_produce` / `invoke_consume` / `invoke_process` specializations alongside the generic `invoke(name[, args])` path — see below. |
| L3.ζ — documentation update | ✅ DONE | HEP merges complete; tech draft cleanup (2026-04-16); this final close-out (2026-04-21). |

### Why L3.β / L3.ε were not pursued

After Phases α / γ / δ landed, the remaining two consolidations were
evaluated against the actual code shape and found to conflict with two
invariants the framework relies on:

1. **Zero-copy `SlotView` semantics.** `InvokeTx` / `InvokeRx` carry raw
   pointers into shared memory that a slot write/read acquired. These
   pointers cannot survive a JSON round-trip, so unifying
   `invoke_produce/consume/process` into a single `invoke(args_json)` would
   either break zero-copy (serialise slot data → defeats the design) or
   require a parallel typed path — which is what the code already has.
2. **Template-specialized, duck-typed cycle operations.** `run_data_loop`
   is a template; the compiler generates specialized code for each
   concrete `CycleOps` without virtual dispatch. Collapsing the three
   classes into one requires either boolean flags (`is_processor`,
   `hold_on_backpressure`, `has_input_side`, …) or variant state — which
   obscures per-role intent and reintroduces runtime branching.

The processor specifically defeats unification cleanly: its cycle
acquires *two* slots, can *hold* an input across cycles when downstream
backpressure denies an output slot, and has two distinct timeout
policies (drop vs block mode). These behaviors have no producer/consumer
counterpart.

**Spirit of the draft's goal is achieved**: role identity is contained to
(a) `CycleOps` class, (b) role host subclass (`RoleHostBase` derivatives),
and (c) role config block. Everything between — `run_data_loop`,
`RoleAPIBase`, `RoleHostCore`, `ScriptEngine`'s generic `invoke(name)`
path, the broker/protocol layer — is role-neutral. The draft wanted the
CycleOps + typed-invoke methods unified *as well*; the code stops one step
earlier, and this has been accepted as the final design.

### Authoritative documentation pointer

The live ScriptEngine + CycleOps design should be documented authoritatively
in **HEP-CORE-0011 (ScriptHost Abstraction Framework) — rewrite pending**,
tracked in `docs/todo/API_TODO.md` SE-03. That rewrite will absorb this
draft's rationale and architecture; this draft is preserved as a historical
record of the original refactor plan and the divergence point.

Additional completed work not in original phases:
- ZMQ cppzmq migration + shared ZMQContext module
- ThreadManager → HEP-CORE-0031
- Flexzone api.flexzone(side) init-time cache (all 3 engines)
- Shutdown order fix (on_stop → finalize → stop → teardown → drain)

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

## 4. Loop semantics — INVARIANT through L3

**Critical constraint**: the refactor must preserve the main data loop's
timing behavior exactly. No observable timing change. No reordering of
steps. No change to when the deadline wait runs, when the inner retry
exits, when metrics are computed, or when the next deadline is advanced.

### 4.1 Authoritative reference

`docs/tech_draft/loop_design_unified.md` v4 is the canonical specification
for loop timing. It covers: unified inner retry-acquire formula,
`compute_short_timeout` / `compute_next_deadline`, MaxRate / FixedRate /
FixedRateWithCompensation policies, processor input-hold strategy, and the
7-step per-cycle skeleton (Setup / A / B / B' / C / D+E / F / G).

`docs/tech_draft/unified_role_loop.md` §4 recorded an exhaustive
code-vs-doc parity check at the 1323-test baseline that confirmed every
step matches `loop_design_unified.md` exactly. The loop logic migrated
into `RoleAPIBase::run_data_loop()` verbatim — no semantic change since.
This draft does not revise any timing behavior.

### 4.2 What L3 changes in the loop implementation

Three things, none of which alter timing:

- CycleOps stops holding `hub::Producer&` / `hub::Consumer&` / `ScriptEngine&`
  as raw fields; it holds `RoleAPIBase&` and calls `api_.write_*` /
  `api_.read_*` directly. **Same method calls, same order, same timeouts.**
- The three subclasses collapse into one concrete `CycleOps`. Processor
  input-hold behavior is derived from side presence + overflow policy.
  **Same branching, same transitions, same held-input preservation.**
- Script invocation goes through `engine_->invoke_cycle(cycle_handle_,
  make_rx(), make_tx(), msgs)`. The engine does not know which callback
  name is behind `cycle_handle_` — it was resolved at setup by
  `pylabhub::role::callback_name_for(config_.role_tag)`.
  **Same callback executes, same arguments, same dispatch cost
  (one cached handle lookup replaces one role-tag branch).**

### 4.3 Timing-parity verification checklist

After each phase that modifies loop code, run this checklist BEFORE
committing:

| Check | How to verify |
|---|---|
| Inner retry short_timeout unchanged | Grep `compute_short_timeout(` in CycleOps — formula and call shape must be identical to pre-refactor. |
| Deadline wait gate unchanged | Compare the 4-condition guard `!is_max_rate && has_data && deadline != max && Clock::now() < deadline` against `loop_design_unified.md` §3 Step B. |
| Drain order unchanged | `core.drain_messages()` then `drain_inbox_sync()` — Step C. No reordering. |
| Invoke occurs after drain, before commit | Step D+E sequence. Inspect flow in `CycleOps::invoke_and_commit`. |
| Commit/discard policy unchanged | Producer: Commit→commit / Error→discard / Discard→discard. Consumer: always release. Processor: commit/discard on output, release-or-hold on input. |
| Metrics computation unchanged | `work_us` = `now - cycle_start`. `loop_overrun` increment condition unchanged. |
| `compute_next_deadline` call unchanged | 4-param call with `(policy, deadline, cycle_start, period_us)`. |
| Processor held_input preserved across cycles | In Block + Tx-fail case, input stays held; next cycle sees `held_rx_ != nullptr` and skips Rx acquire. |
| `cleanup_on_shutdown` releases what `acquire` grabbed | Shutdown path releases any held Tx/Rx slot. |
| `cleanup_on_exit` releases held_input (processor) | Loop-exit path releases any held-over input slot. |

### 4.4 Regression protection

In addition to the checklist above, the following existing tests exercise
timing behavior and must remain green through every phase:

- `test_datahub_loop_policy.cpp` — exercises MaxRate / FixedRate /
  FixedRateWithCompensation via Producer, Consumer, Processor.
- `test_datahub_role_state_machine.cpp` — exercises metrics counters
  that would change if cycle-overrun timing changed.
- Processor integration tests in `test_layer3_datahub` / `test_layer4_processor`
  that rely on input-hold behavior.

**If a timing or behavior regression appears after a refactor commit**:
**analyze the discrepancy source before deciding whether to fix or revert.**
The refactor itself is almost certainly worth keeping — the issue is
almost certainly a specific translation error inside the new code.
See §4.6 for the diagnostic checklist.

### 4.6 Discrepancy-source checklist (post-L3.β specifically; applies whenever
loop behavior changes unexpectedly)

When a test that passed pre-refactor fails post-refactor, work through this
list in order. Each item is a known source of translation error during a
CycleOps-style unification. Identify which one explains the failure, fix
that specific issue, and re-verify — do NOT revert the refactor on first
failure.

| # | Possible discrepancy source | How to detect | How to fix |
|---|---|---|---|
| 1 | Branching order changed — unified CycleOps evaluates conditionals in a different order than the per-role originals. | Source-diff each conditional path in the new unified methods against the three originals. | Reorder branches to match original source order. |
| 2 | Field caching differences — original cached `buf_sz_`/`fz_sz_` at construction; unified may re-read per cycle (or vice versa). | Check the constructor init list and the per-cycle accesses against the three originals. | Preserve the original caching decision verbatim. |
| 3 | Flexzone re-read site shifted — `fz_ptr_` is re-read each cycle in original (SHM may move it); unified might cache it once, causing stale pointers. | Compare the per-cycle `api_.write_flexzone()` / `api_.read_flexzone()` call site in `invoke_and_commit`. | Restore the per-cycle re-read conditional. |
| 4 | Processor input-hold logic altered — the conditional `if (out_buf_ \|\| drop_mode_) { release }` is subtle; collapsing it may break edge cases. | Hand-trace with a state-machine diagram: held input × Tx acquire success/failure × Drop/Block mode. | Restore the exact conditional structure. |
| 5 | Metric increment site shifted — `inc_out_slots_written` / `inc_out_drop_count` / `inc_in_slots_received` must increment at exactly the same sites as before. | Source-diff every metric call against the three originals. | Restore original sites. |
| 6 | Engine-callback dispatch path — after β and before ε, still calls `invoke_produce/consume/process` via role_tag; any branching error sends to the wrong callback. | Verify `invoke_X` chosen by branch matches role_tag. Unit-test with a role that should never invoke_produce and confirm it never does. | Fix the dispatch switch. |
| 7 | Shutdown-cleanup asymmetry — `cleanup_on_shutdown` / `cleanup_on_exit` must release exactly what's held; unified logic might over-release or under-release. | Check the guards on each release against the original three. | Restore the original guard conditions. |
| 8 | Timing constant / duration_cast narrowing — if types or cast directions changed, small-magnitude rounding could shift a deadline by one step. | Compare `std::chrono::duration_cast<milliseconds>` / microseconds call sites. | Restore the original cast direction. |

**Revert only if**: none of (1)–(8) explains the failure AND the failure is
reproducible AND we cannot identify any other translation error. Reverting
without root cause leaves us blind to the actual bug.

### 4.5 Baseline test suite — BRANCH-COVERAGE REQUIREMENT

**Correctness of the L3.β refactor is evaluated by exhaustive branch
coverage comparison**: every logical branch in the three original
`*CycleOps` classes must be mapped to a location in the unified `CycleOps`,
exercised by a baseline test on pre-β code, and re-verified on post-β code
to produce identical built-in-metric values and identical slot outcomes.

The branches and their slot/decision outcomes are enumerated below. Each
branch has a stable ID used in baseline-test comments to identify which
branch a test exercises.

### 4.5a Branch inventory — original CycleOps classes

#### ProducerCycleOps (10 branches)

```
acquire():
  P-A-1   buf_ != nullptr  → has_data=true
  P-A-2   buf_ == nullptr  → has_data=false (no deadline wait)

cleanup_on_shutdown():
  P-S-1   buf_ != nullptr  → write_discard() + null buf_
  P-S-2   buf_ == nullptr  → no-op

invoke_and_commit():
  P-I-M   buf_ != nullptr                      → memset(buf_, 0, buf_sz_)
  P-I-FZ  core.has_out_fz()                    → re-read fz_ptr_
  slot decision (after engine.invoke_produce):
    P-I-C     buf_ && result==Commit           → write_commit + inc_out_slots_written
    P-I-D     buf_ && result==Discard          → write_discard + inc_out_drop_count
    P-I-E     buf_ && result==Error            → write_discard + inc_out_drop_count
    P-I-N     !buf_                            → no slot op + inc_out_drop_count
  stop decision:
    P-I-STOP  result==Error && stop_on_error   → request_stop + return false

cleanup_on_exit():
  P-X     no-op (nothing held across cycles)
```

Slot outcomes in producer path: **commit**, **discard**, **no-slot-drop**.

#### ConsumerCycleOps (8 branches; error detection via count delta)

```
acquire():
  C-A-1   data_ != nullptr  → has_data=true
  C-A-2   data_ == nullptr  → has_data=false

cleanup_on_shutdown():
  C-S-1   data_ != nullptr  → read_release + null data_
  C-S-2   data_ == nullptr  → no-op

invoke_and_commit():
  C-I-INC  data_ != nullptr                     → inc_in_slots_received
  C-I-FZ   core.has_in_fz()                     → read_flexzone() as fz_ptr, else null
  errors_before = script_error_count()
  // (engine.invoke_consume returns void)
  slot decision:
    C-I-R    data_ != nullptr                    → read_release + null data_
    C-I-N    data_ == nullptr                    → no slot op
  stop decision (NOTE: uses error-count delta, not return value):
    C-I-STOP stop_on_error && script_error_count() > errors_before  → request_stop + return false

cleanup_on_exit():
  C-X     no-op
```

Slot outcomes in consumer path: **release**, **no-slot-drop**.

#### ProcessorCycleOps (17 branches — most complex; covers input-hold)

```
acquire():
  PR-A-ACQ   !held_input_                        → retry_acquire(read) → held_input_
  PR-A-TX-D  held_input_ && drop_mode_           → write_acquire(0ms)   → out_buf_
  PR-A-TX-B  held_input_ && !drop_mode_          → write_acquire(deadline-aware)
  PR-A-NOTX  !held_input_                        → out_buf_ stays null
  // always returns true (processor maintains cadence on idle cycles)

cleanup_on_shutdown():
  PR-S-R   held_input_ != null                   → read_release
  PR-S-D   out_buf_     != null                  → write_discard

invoke_and_commit():
  PR-I-M     out_buf_ != null                     → memset(out_buf_, 0, out_sz_)
  PR-I-FZ-o  core.has_out_fz()                    → re-read out_fz_ptr_
  PR-I-FZ-i  core.has_in_fz()                     → re-read in_fz_ptr_
  result = engine.invoke_process(rx, tx, msgs)

  OUTPUT slot decision:
    PR-I-C       out_buf_ && result==Commit      → write_commit + inc_out_slots_written
    PR-I-Do      out_buf_ && result==Discard     → write_discard + inc_out_drop_count
    PR-I-Eo      out_buf_ && result==Error       → write_discard + inc_out_drop_count
    PR-I-TXFAIL  !out_buf_ && held_input_        → inc_out_drop_count (tx acquire failed)
    PR-I-IDLE    !out_buf_ && !held_input_       → no action (idle cycle)

  INPUT slot decision (the processor-specific hold logic):
    PR-I-REL     held_input_ && (out_buf_ || drop_mode_)     → read_release + inc_in_slots_received
    PR-I-HOLD    held_input_ && !out_buf_ && !drop_mode_     → hold across cycles (key!)
    PR-I-NOHELD  !held_input_                                 → no input action

  stop decision:
    PR-I-STOP    result==Error && stop_on_error              → request_stop + return false

cleanup_on_exit():
  PR-X-R   held_input_ != null                  → read_release (released one cycle late)
  PR-X-N   held_input_ == null                  → no-op
```

Slot outcomes in processor path: **commit**, **discard**, **tx-fail-drop**,
**idle-cycle**, **release**, **hold-across-cycles**.

### 4.5b Post-β mapping requirement — IDENTICAL EFFECT

Every branch ID above must be addressable in the unified `CycleOps` code,
and **produce identical observable effect** as the pre-β version. Not
similar, not functionally equivalent, not "close enough" — **bit-identical
in observable state**.

Observable state means:

1. **Identical slot action** at the same branch condition.
   E.g., if pre-β at P-I-E calls `write_discard()` and increments
   `out_drop_count`, post-β at the corresponding branch must call
   `write_discard()` and increment `out_drop_count`. Same method, same
   order.
2. **Identical metric increment site** within the cycle.
   E.g., `out_slots_written++` is called *after* `write_commit()`, not
   before. `inc_in_slots_received()` fires at the exact same point
   (at release time for processor, at acquire time for consumer —
   whatever the pre-β code did). Ordering between slot action and
   metric increment is observable via metric-snapshot timing and must
   not shift.
3. **Identical metric final values** at cycle-loop completion.
   After running N cycles of a scenario, all metric counters equal the
   pre-β values *exactly*. Not bounds; not "within tolerance"; exact
   equality.
4. **Identical queue contents** from the harness side.
   - Producer: queue contains exactly `[0..N-1]` in that order.
   - Consumer: received exactly the producer-emitted N slots in that order.
   - Processor Block: input sequence 1:1-mapped to output sequence.
   - Processor Drop: received output is a monotone subset of input with
     exactly `out_drop_count` gaps; the *set of dropped input indices*
     must be consistent between pre-β and post-β for the same scenario.
5. **Identical RecordingEngine call log.**
   Same number of `invoke_*` calls, in the same order, with same argument
   snapshots (slot pointer is position-equivalent; slot contents bit-exact),
   and same return values recorded.

**Review artifact required at L3.β submission**: a table of all 35 branch
IDs with their post-β line numbers and a one-line outcome statement.
Any branch that cannot be mapped, or whose effect differs, is a blocker.

### 4.5c Commit/abandon/hold decision tables

Slot disposition is the most regression-prone aspect of L3.β. The
baseline suite must verify each of these decision tables by observation —
both via built-in metrics AND via harness-side queue inspection.

**Producer — commit/abandon decision** (result of `invoke_produce`):

| Slot acquired? | result        | Action on slot  | out_slots_written++ | out_drop_count++ |
|----------------|---------------|-----------------|---------------------|------------------|
| Yes            | Commit        | write_commit    | ✅                  | —                |
| Yes            | Discard       | write_discard   | —                   | ✅               |
| Yes            | Error         | write_discard   | —                   | ✅               |
| No             | (any)         | (none)          | —                   | ✅               |

**Consumer — release decision** (no commit concept):

| Slot acquired? | Action on slot | in_slots_received++ |
|----------------|----------------|---------------------|
| Yes            | read_release   | ✅                  |
| No             | (none)         | —                   |

**Processor output — commit/abandon decision** (result of `invoke_process`):

| out_buf_? | held_input_? | result  | Output action  | out_slots_written++ | out_drop_count++ |
|-----------|--------------|---------|----------------|---------------------|------------------|
| Yes       | Yes          | Commit  | write_commit   | ✅                  | —                |
| Yes       | Yes          | Discard | write_discard  | —                   | ✅               |
| Yes       | Yes          | Error   | write_discard  | —                   | ✅               |
| No        | Yes          | (any)   | (none)         | —                   | ✅               |
| No        | No           | (any)   | (none)         | —                   | —                |

**Processor input — release/hold decision** (independent of invoke result):

| held_input_? | out_buf_?  | drop_mode_ | Input action   | in_slots_received++ |
|--------------|------------|------------|----------------|---------------------|
| Yes          | Yes        | —          | read_release   | ✅                  |
| Yes          | No         | Yes        | read_release   | ✅                  |
| Yes          | No         | No         | **hold**       | —                   |
| No           | —          | —          | (none)         | —                   |

**Why these tables matter**: these are the answers the tests must observe.
If post-β behavior differs at any row, a metric or contents assertion
will fire. The decision logic has never been extracted into a test
oracle before — this is the gap the baseline suite fills.

### 4.5d Test mapping — which test covers which branches

Each of the 6 baseline tests exercises a subset of the 35 branches. The
coverage must be complete: every ID from §4.5a appears in at least one
test's coverage tag. Suggested coverage (reviewer must verify no gap):

| Test # | Scenario | Branches exercised (IDs) |
|---|---|---|
| 1 | Producer happy path, N=100 all-Commit | P-A-1, P-I-M, P-I-FZ (if fz), P-I-C, P-X |
| 1b| Producer with mix of Commit/Discard/Error | adds P-I-D, P-I-E, P-I-STOP |
| 1c| Producer with Tx-full (buf_ null) | adds P-A-2, P-I-N |
| 1d| Producer shutdown with held buf_ | adds P-S-1, P-S-2 |
| 2 | Consumer happy path, N=100 reads | C-A-1, C-I-INC, C-I-FZ, C-I-R, C-X |
| 2b| Consumer with script errors | adds C-I-STOP |
| 2c| Consumer with empty queue (no data) | adds C-A-2, C-I-N |
| 2d| Consumer shutdown with held slot | adds C-S-1, C-S-2 |
| 3 | Processor Block + Tx-capacity=4 + throttled consumer, 100 inputs | PR-A-ACQ, PR-A-TX-B, PR-I-M, PR-I-FZ-o/i, PR-I-C, PR-I-REL, PR-I-TXFAIL (forces input-hold), PR-I-HOLD, PR-X-N |
| 4 | Processor Drop + same setup | adds PR-A-TX-D, PR-I-Do, PR-I-Eo (with simulated errors) |
| 4b| Processor idle cycles (no input available) | adds PR-A-NOTX, PR-I-IDLE, PR-I-NOHELD |
| 4c| Processor shutdown mid-hold | adds PR-S-R, PR-S-D, PR-X-R |
| 5 | FixedRate 100Hz × N cycles timing | revisits P-* with timing assertions |
| 6 | Deliberate handler overrun | P-* + loop_overrun_count assertions |

Test count revised from 6 to ~13 (the ".b/.c/.d" variants are small
extensions of the main 6, not net-new scaffolding). Baseline suite total:
~600 LOC.

### 4.5e1 Exact-equality assertion language (NOT bounded)

Every baseline-test assertion is **exact equality** between a scenario-derived
predicted value and an observed value. Bounded or inequality assertions
(`EXPECT_GE`, `EXPECT_LT`) tolerate regressions and are therefore forbidden
except for wall-clock timing tolerance, where cycle-jitter makes exact
equality impossible.

The predicted values are derived by **walking the branch decision trees
in §4.5a and the decision tables in §4.5c** against the test scenario
parameters (N cycles, script return pattern, queue capacity, throttle
rate). They are NOT recorded from the pre-β run as magic numbers to be
matched — they are symbolic predictions that both pre-β and post-β code
must satisfy.

Correct pattern:
```cpp
// Scenario: producer, N=100, script returns Commit for all cycles, queue
// capacity = 16 (never full), no shutdown. From §4.5a tree: every cycle
// enters P-A-1 → P-I-M → P-I-FZ(if fz) → P-I-C → return true.
constexpr uint64_t kExpectedWritten = 100;
constexpr uint64_t kExpectedDropped = 0;
constexpr uint64_t kExpectedIter    = 100;

// Metric assertions — exact equality.
EXPECT_EQ(core.iteration_count(),    kExpectedIter);
EXPECT_EQ(core.out_slots_written(),  kExpectedWritten);
EXPECT_EQ(core.out_drop_count(),     kExpectedDropped);

// Harness assertion — exact contents.
auto slots = drain_producer_queue_to_vec();
ASSERT_EQ(slots.size(), kExpectedWritten);
for (uint64_t i = 0; i < kExpectedWritten; ++i) {
    EXPECT_EQ(slots[i].seq, i);   // exact
}

// Cross-check — counter and contents agree.
EXPECT_EQ(core.out_slots_written(), slots.size());
```

Forbidden pattern:
```cpp
EXPECT_GE(core.out_slots_written(), 95u);  // tolerates up to 5 slot loss
EXPECT_GT(slots.size(), 0u);               // "some slots arrived"
```

The exception: `last_cycle_work_us` and wall-clock timing assertions
use tolerance bands because cycle-jitter is unavoidable at sub-ms
scale. But cycle-count and slot-count assertions always use exact
equality — those quantities have no jitter source.

### 4.5e Test-evaluation principles

These principles govern every test in the baseline suite (to be implemented
before L3.β as a separate work item).

**Tests evaluate runtime state, not return values.** Calling the function
under test and asserting it returned cleanly is **not** evidence of
correctness. A regression can hide behind a clean return. The only valid
evidence is what the system reports about itself during and after the
scenario.

**Two sources of truth — cross-checked, never singular:**

**(a) Built-in metrics — the framework's self-reports**

These are counters and gauges the framework produces during normal
operation, always on, in production:

- `RoleHostCore` counters: `iteration_count`, `out_slots_written`,
  `out_drop_count`, `in_slots_received`, `last_cycle_work_us`,
  `loop_overrun_count`
- `engine.script_error_count()`
- `ContextMetrics` queue/loop/inbox counters (the X-macro-defined fields
  surfaced through `snapshot_metrics_json()`)
- Broker-side `RoleStateMetrics` (ready_to_pending, pending_to_deregistered,
  pending_to_ready)

Reading these in a test reads what a production system would also report.
Assertions on these prove behaviors that hold in production, not behaviors
that only exist under test harness.

**(b) Test-specific measurements — harness-side instruments**

These are observable quantities the test harness computes independently
of the framework's self-reports, to cross-check them:

- Wall-clock duration of a scenario (for timing-fidelity tests)
- Data contents captured at the receiving queue — bit-exact byte comparison
  against what was produced
- Sequence integrity (monotonicity, gaps, duplicates) computed from
  captured contents
- Callback call counts + argument snapshots from a RecordingEngine stub
- Queue-state snapshots at critical moments (e.g., "Tx queue was full here")

Each independently measures what happened.

**The cross-check pattern (applies to every test):**

1. (a) Built-in metric equals expected scenario outcome.
2. (b) Test-side measurement independently confirms.
3. (a) ↔ (b) agree — e.g., `out_slots_written == queue_contents.size()`.

Tests asserting only (a) prove internal-consistency of counters but not
correctness. Tests asserting only (b) miss counter-accuracy regressions.
Both-and catches both.

**Forbidden:**

- Using a function's return value (e.g., `run_data_loop` returning) as
  primary evidence of success. Necessary but never sufficient.
- Asserting on test-only synthetic state that doesn't exist in production.
- Using wall-clock `sleep(N)` as a proxy for cycle-count claims. Cycle
  counts come from `iteration_count` (a) or direct event-counts in the
  harness (b), never from "we slept long enough".

**Why this matters for L3.β specifically:** L3.β moves metric-increment
sites between CycleOps branches. A regression might land a `out_slots_written++`
before a discard path. The counter and the actual queue contents would
diverge. The cross-check catches exactly this. Counter-only or
contents-only assertions do not.

### 4.5f Baseline test suite — the six core tests

All six tests adhere to §4.5e. Every assertion comes from (a) built-in
metrics OR (b) test-harness measurements, and cross-checked where possible.
See §4.5d for the branch-coverage expansion to ~13 tests total.

| # | Scenario | Built-in-metric assertions | Harness-measurement assertions |
|---|---|---|---|
| 1 | Producer, MaxRate, N=100 cycles, all-Commit | `iteration == N`; `out_slots_written == N`; `out_drop_count == 0`; `loop_overrun == 0` | queue contains N slots with contents `[0..N-1]`; RecordingEngine logs N produce calls |
| 2 | Consumer drains N producer slots | `in_slots_received == N`; `last_seq == N-1` | received contents match producer's exactly; RecordingEngine logs N consume calls with expected rx data |
| 3 | Processor Block + Tx-capacity=4 + throttled consumer + 100 inputs | `in_slots_received == 100`; `out_slots_written == 100`; `out_drop_count == 0` | consumer-received contents == producer-emitted `[0..99]` in order; RecordingEngine logs 100 process calls |
| 4 | Processor Drop + same setup | `in_slots_received == 100`; `out_slots_written + out_drop_count == 100`; `out_drop_count > 0` | received contents are monotonic subset of `[0..99]` (gaps OK); RecordingEngine logs 100 process calls |
| 5 | FixedRate 100Hz × N=50 cycles | `iteration == N`; `loop_overrun == 0` | wall-clock elapsed ∈ [N × 10ms × 0.9, N × 10ms × 1.5] (tolerance band) |
| 6 | Deliberate handler overrun (handler sleeps > period) | `iteration == N`; `loop_overrun == N - first_cycle` (first cycle has no deadline) | wall-clock elapsed reflects handler slowness, not target period |

**Implementation components required before tests can be written:**

1. **RecordingEngine stub** — subclass of `ScriptEngine` that records every
   invoke call (args + return value) and returns test-configurable results.
2. **Public accessibility of `*CycleOps`** — the current anonymous-namespace
   classes must be moved to a header (e.g., `cycle_ops.hpp`) so tests can
   instantiate them directly. Post-L3.β the unified `CycleOps` is already public.
3. **Test-harness queue pair** — in-process SHM queue creation helper so
   tests can wire real queues without broker/ZMQ/full lifecycle.
4. **Metric snapshot helper** — tests read `snapshot_metrics_json()` at scenario
   end and parse the fields they assert on.

**Ordering guarantee**: The baseline suite is written FIRST — against
pre-β code (L3.α state) — and must pass green there. Only then is L3.β
implemented. Post-β, the same suite runs again with the same assertions;
any changed counter value is a translation-error signal pointing at the
§4.6 checklist.

### 4.5 Phase-specific timing risk

| Phase | Timing risk | Mitigation |
|---|---|---|
| L3.α | **Zero** — pure API-surface rename. The verbs being added to `RoleAPIBase` forward to the same internal queue pointers as before. | Verify test suite. No code-body changes in the loop. |
| L3.β | **Medium** — CycleOps collapse. Branch structure changes. | Run the §4.3 checklist item-by-item before commit. Inspect the collapsed CycleOps against each original CycleOps subclass line-by-line. |
| L3.γ | **Low** — `hub::Producer`/`Consumer` deletion. Their methods were already pure forwarders to the queue. | Verify test suite. Timing code was never in Producer/Consumer themselves. |
| L3.δ | **Zero** — `RoleHost` restructure is lifecycle, not loop. | Verify test suite. |
| L3.ε | **Low** — engine dispatch goes from `invoke_produce/consume/process` to `invoke_cycle(handle, ...)`. One pointer deref vs one method-name-based dispatch — cost is the same order of magnitude. | Measure cycle work_us in a tight-loop test at MaxRate. If mean work_us changes by more than ±1 µs, investigate. |
| L3.ζ | **Zero** — docs only. | — |

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
