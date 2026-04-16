# Tech Draft: Unified Role Loop Framework

**Status**: ⚠ **SUPERSEDED 2026-04-14** by `role_unification_design.md`.
See that document for the current plan. Kept here for historical context —
decisions reversed during the 2026-04-14 discussion include:
(a) CycleOps taking raw `Producer&`/`Consumer&` vs `RoleAPIBase&` (both
rejected — data-plane verbs are flat on RoleAPIBase),
(b) `writer()`/`reader()` accessors (rejected — flat API),
(c) keeping `hub::Producer`/`hub::Consumer` as internal types (rejected —
delete them),
(d) collapsing three RoleHost classes into one (rejected — keep as empty
extension-point subclasses),
(e) engine dispatching by `role_tag` (rejected — handle-based, engine
framework-agnostic).
**Branch**: `feature/lua-role-support`
**Relates to**: HEP-CORE-0011, HEP-CORE-0023 §2.5, `loop_design_unified.md`, `engine_thread_model.md`
**Baseline**: 1275/1275 tests (was 1323; net change: Step 7 deleted obsolete tests, then HEP-0023 added new ones)

---

## 0. What Has Already Landed Since 2026-04-06

The original 2026-04-06 draft assumed Messenger still existed and treated the
ctrl thread as not-yet-unified. Both have changed; this section is the truth
table for what's already done so the rest of the doc can be read as a plan
for what *remains*.

| Pillar from §1 | Status | Where it landed |
|---|---|---|
| **Callback wiring** (was §1.1) | ✅ DONE — `wire_event_callbacks()` per §9.5 | RoleAPIBase, Phase 1 |
| **ctrl thread** (was §1.2) | ✅ DONE — `start_ctrl_thread()` owns BRC connect, heartbeat, notification dispatch | RoleAPIBase (commit `e2ecd8e` and HEP-0023 Phase 2) |
| **Messenger removal** | ✅ DONE — replaced by `BrokerRequestComm` | Step 7 (`cf53ed3`) |
| **Role-state machine** | ✅ DONE — heartbeat-multiplier liveness + role-close cleanup hook | HEP-CORE-0023 §2.5 (commits `3201e08`, `6558b2c`) |
| **Data loop frame** (was §1.3) | ⚠️ NOT YET — still copy-pasted 3× | **This is the remaining work** |
| **Lifecycle sequence** (was §1.4) | ⚠️ PARTIAL — ctrl-thread / dereg sub-sequences extracted; outer skeleton still per-role | **Folds into the loop-frame work** |

Everywhere this doc says "Messenger" below, read "BrokerRequestComm". The
`messenger->enqueue_heartbeat(...)` calls cited in §3.2 are now
`BrokerRequestComm::send_heartbeat(...)` driven from `start_ctrl_thread()`.

---

## 1. Problem Statement

The three role hosts (producer, consumer, processor) each still implement:

3. **Data loop frame** — Timing setup, outer loop condition, inner retry
   acquire, deadline wait, drain, metrics, next_deadline computation. These
   steps are character-for-character identical across all 3 roles. Only the
   *slot* within the frame differs: what to acquire, what to invoke, what to
   commit/release.

4. **Lifecycle sequence** — Outer skeleton of `worker_main_()` (invoke_on_init
   → spawn ctrl_thread_ → data loop → stop_accepting → join → invoke_on_stop →
   finalize) is identical.

(Items 1 and 2 from the original draft — callback wiring and ctrl_thread
construction — landed in earlier phases, see §0.)

This duplication means every behavioral change must be applied 3 times. The
unified loop design (tech draft `loop_design_unified.md`) was originally
written as a correctness reference but was never promoted to a shared
implementation. With RoleAPIBase already centralizing callback wiring and
the ctrl thread, the loop frame is the last major piece left to unify.

---

## 2. Design Goals

1. **One implementation of the loop frame** — timing, retry, deadline, drain,
   metrics, overrun detection. Bug fixes apply once.

2. **One implementation of ctrl_thread_** — socket list derived from available
   pointers, heartbeat and metrics tasks derived from role configuration,
   `on_heartbeat` callback dispatched via engine.

3. **One implementation of callback wiring** — Consumer/Producer/Messenger
   event callbacks → `core_.enqueue_message()` / `core_.request_stop()`.

4. **Role-specific logic expressed as strategy** — producer, consumer, and
   processor provide only the role-specific *slot* (acquire, invoke,
   commit/release). No inheritance hierarchy; plain structs or lambdas.

5. **No change to external behavior** — identical timing, identical metrics,
   identical shutdown sequence. Pure internal refactor.

6. **Lives in pylabhub-utils** (shared library) — consistent with RoleAPIBase
   and RoleHostCore placement. Role hosts become thin config-parsers.

---

## 3. Current State Analysis

### 3.1 Data loop — shared frame vs role-specific slots

The data loop in all 3 roles follows this exact skeleton:

```
SETUP:
  period_us, is_max_rate, short_timeout_us, short_timeout_ms
  deadline = time_point::max()

OUTER LOOP: while (core_.should_continue_loop() && !process_exit)
  cycle_start = now()

  STEP A: Acquire slot(s)           ← ROLE-SPECIFIC
  STEP B: Deadline wait             ← SHARED (identical formula)
  STEP B': Shutdown check           ← SHARED
  STEP C: Drain messages + inbox    ← SHARED
  STEP D: Invoke engine callback    ← ROLE-SPECIFIC
  STEP E: Commit / discard / release← ROLE-SPECIFIC
  STEP F: Metrics                   ← SHARED (identical)
  STEP G: Next deadline             ← SHARED (identical)
```

**Role-specific slots (the ONLY differences):**

**Producer Step A**: `write_acquire(short_timeout)` with inner retry.
**Consumer Step A**: `read_acquire(short_timeout)` with inner retry.
**Processor Step A**: `read_acquire` (inner retry) + `write_acquire` (policy-dependent timeout). Input-hold across cycles in Block mode.

**Producer Step D**: `invoke_produce(InvokeTx{buf, sz, fz, fz_sz}, msgs)`.
Flexzone: output side, re-read each cycle.
**Consumer Step D**: `invoke_consume(InvokeRx{data, sz, fz, fz_sz}, msgs)`.
Flexzone: input side (const_cast), re-read each cycle.
**Processor Step D**: `invoke_process(InvokeRx{...}, InvokeTx{...}, msgs)`.
Flexzone: both sides, re-read each cycle.

**Producer Step E**: Commit → `write_commit()` + `inc_out_slots_written()`.
Discard → `write_discard()` + `inc_out_drop_count()`. Error → optional stop.
**Consumer Step E**: Release → `read_release()`. `inc_in_slots_received()` on
acquire (before invoke). Error → optional stop.
**Processor Step E**: Output commit/discard (same as producer). Input
release-or-hold: release if output succeeded or Drop mode; hold if Block mode
and output failed. `inc_in_slots_received()` on release. Error → optional stop.
Cleanup on loop exit: release held_input.

### 3.2 ctrl_thread_ — composition from available pointers

| Component | Source | Condition |
|-----------|--------|-----------|
| Producer peer_ctrl socket | `producer->peer_ctrl_socket_handle()` | producer != nullptr |
| Consumer ctrl socket | `consumer->ctrl_zmq_socket_handle()` | consumer != nullptr |
| Consumer data socket | `consumer->data_zmq_socket_handle()` | consumer != nullptr (returns nullptr when N/A) |
| Heartbeat | `messenger->enqueue_heartbeat(channel, metrics)` | always |
| Metrics report | `messenger->enqueue_metrics_report(ch, uid, metrics)` | consumer role only (currently) |
| on_heartbeat | `engine->invoke("on_heartbeat")` | if script defines it |
| ThreadEngineGuard | `ThreadEngineGuard(*engine)` | always |

### 3.3 Callback wiring — pure routing

All callbacks do one of two things:
- **Route to message queue**: construct `IncomingMessage`, call
  `core_.enqueue_message()`. Applied to: `on_channel_closing`,
  `on_zmq_data`, `on_producer_message`, `on_channel_error`.
- **Trigger shutdown**: call `core_.set_stop_reason()` +
  `core_.request_stop()`. Applied to: `on_force_shutdown`, `on_peer_dead`,
  `on_hub_dead`.

The only role-specific aspect is *which object* to wire (producer vs consumer
vs messenger), which is determined by which pointers are non-null.

### 3.4 Lifecycle sequence — identical across roles

```
invoke_on_init()
sync flexzone checksum (if output + flexzone)
set_running(true)
spawn ctrl_thread_
signal ready
run_data_loop_()
stop_accepting()
set_running(false)
join ctrl_thread_
invoke_on_stop()
finalize()
teardown infrastructure
```

---

## 4. Timing Model Verification

Cross-checked every line of code in all 3 role hosts against
`loop_design_unified.md` and `loop_timing_policy.hpp`.

### 4.1 Shared timing infrastructure (already in pylabhub-utils)

All timing functions live in `src/include/utils/loop_timing_policy.hpp`:

| Function | Formula | Used by |
|----------|---------|---------|
| `compute_short_timeout(period_us, ratio)` | `max(period_us * ratio, 10µs)` | All 3 roles: inner retry timeout |
| `compute_next_deadline(policy, prev, start, period_us)` | MaxRate→max, FRC→prev+period, FR→ideal or reset | All 3 roles: Step G |
| `resolve_period_us(rate_hz, period_ms, ctx)` | Validates and unifies rate/period to µs | Config parsers |

### 4.2 Code vs doc matrix (verified character-by-character)

| Step | Doc spec (`loop_design_unified.md`) | All 3 code implementations | Match? |
|------|-------------------------------------|---------------------------|--------|
| Setup: short_timeout | `max(period_us * ratio, 10µs)` | `compute_short_timeout(period_us, ratio)` | Exact |
| Setup: deadline init | `time_point::max()` | `Clock::time_point::max()` | Exact |
| A: Inner retry exit | slot, MaxRate, shutdown, `remaining <= short_timeout` | Same 4 conditions, same order | Exact |
| A: First-cycle retry | `deadline == max → always retry` | `if (deadline != max) { check remaining }` | Exact |
| B: Deadline wait gate | Prod/Cons: `!MaxRate && data && deadline != max && now < deadline` | Exact match | Exact |
| B: Processor gate | `!MaxRate && deadline != max && now < deadline` (no data check) | Exact match — maintains cadence on idle cycles | Exact |
| B': Shutdown cleanup | Prod: `write_discard()`, Cons: `read_release()`, Proc: release+discard | Each role matches its doc section | Exact |
| C: Drain | `drain_messages() + drain_inbox_sync()` | Identical | Exact |
| F: Overrun | `deadline != max && now > deadline → inc` | Identical in all 3 | Exact |
| G: Next deadline | `compute_next_deadline(policy, deadline, start, period_us)` | Same call signature | Exact |

### 4.3 Timing policies — behavior match

**FixedRate** (doc §2.3):
```
ideal = prev_deadline + period
if now() <= ideal: return ideal         ← on time: advance cleanly
return now() + period                   ← overrun: reset from now, no catch-up
```
Code (`loop_timing_policy.hpp:281-287`): exact match.

**FixedRateWithCompensation** (doc §2.3):
```
return prev_deadline + period           ← always advance from prev (steady average)
```
Code (`loop_timing_policy.hpp:268-272`): exact match. First cycle uses `cycle_start + period`.

**MaxRate** (doc §2.3):
```
return time_point::max()                ← no deadline, no sleep
```
Code (`loop_timing_policy.hpp:258-261`): exact match.

### 4.4 Processor-specific timing details

**Output acquire timeout** (doc §5.2, lines 444-454 vs code lines 759-776):

| Mode | Doc | Code | Match? |
|------|-----|------|--------|
| Drop | `write_acquire(0ms)` | `write_acquire(milliseconds{0})` | Exact |
| Block, first cycle | `short_timeout` (deadline==max fallback) | `output_timeout = short_timeout` (default) | Exact |
| Block, normal | `max(remaining, short_timeout)` | `if (remaining > short_timeout) output_timeout = remaining` | Exact |

**Input-hold** (doc §5.3): Code preserves `held_input` across cycles, skips
`read_acquire()` when held. Both SHM (lock held) and ZMQ (`current_read_buf_`
unchanged) are correct per doc.

### 4.5 Consumer error detection divergence (by design)

`invoke_consume` returns `InvokeResult` (C++ signature), but consumer
**discards the return value** and uses `script_error_count() > errors_before`
comparison. Producer and processor use the return value for commit/discard.
This is correct: consumer has no output to commit/discard; InvokeResult is
meaningless for read-only roles. Documented in `loop_design_unified.md` §4:
"invoke_consume is void. Error detection via error count comparison."

---

## 5. Proposed Framework

### 5.1 Design Principle: Shared Frame + Role Slots

The framework separates the loop into two concerns:

1. **The frame** (shared, in `RoleAPIBase`): outer loop condition, timing
   setup, inner retry mechanism, deadline management, drain, metrics,
   next deadline. The frame owns all timing state and provides the inner
   retry as a reusable utility.

2. **The slots** (role-specific, via `RoleCycleOps`): what to acquire, what
   to invoke, what to commit/release. Each role fills in only its specific
   behavior. The slots receive timing context from the frame — they do not
   compute timeouts or deadlines themselves.

### 5.2 AcquireContext — timing state passed to role slots

The frame computes timing values once per cycle and passes them to the
role's acquire function via a context struct:

```cpp
/// Timing context for the current cycle, computed by the shared frame.
/// Passed to RoleCycleOps::acquire() so role code never computes timeouts.
struct AcquireContext
{
    std::chrono::milliseconds              short_timeout;     ///< ms, rounded up
    std::chrono::microseconds              short_timeout_us;  ///< µs, exact
    std::chrono::steady_clock::time_point  deadline;          ///< current cycle deadline
    bool                                   is_max_rate;       ///< MaxRate policy active
};
```

### 5.3 retry_acquire — shared inner retry utility

The inner retry loop is character-identical in all 3 roles for the primary
acquire. The frame provides it as a free function:

```cpp
/// Shared inner retry logic.
///
/// Calls try_once(short_timeout) repeatedly until:
///   - try_once returns non-null (success)
///   - is_max_rate (single attempt)
///   - core signals shutdown
///   - remaining time until deadline < short_timeout_us
///
/// First cycle (deadline == time_point::max()): retries indefinitely
/// until success or shutdown (matches loop_design_unified.md §3 Step A).
///
/// Returns the acquired pointer, or nullptr.
void *retry_acquire(
    const AcquireContext &ctx,
    RoleHostCore &core,
    std::function<void *(std::chrono::milliseconds)> try_once);
```

Each role calls this with its own `try_once` lambda:
- Producer: `[&](ms t) { return producer.write_acquire(t); }`
- Consumer: `[&](ms t) { return (void*)consumer.read_acquire(t); }`
- Processor: same as consumer for the primary (input) acquire

The processor's secondary (output) acquire is a single call with a
policy-dependent timeout — it does NOT use `retry_acquire`. This is
role-specific code inside the processor's `acquire()` slot.

### 5.4 RoleCycleOps — the role-specific interface

```cpp
/// Role-specific operations called by the shared loop frame.
///
/// Each method corresponds to one slot in the data loop. The frame
/// handles everything else: outer loop, timing, retry, drain, metrics.
///
/// Implementations are concrete classes (ProducerCycleOps, ConsumerCycleOps,
/// ProcessorCycleOps), not lambdas. This allows the compiler to devirtualize
/// when the concrete type is known, avoids std::function allocation, and
/// keeps role-specific state as plain member variables.
struct RoleCycleOps
{
    virtual ~RoleCycleOps() = default;

    // ── Step A: Acquire ──────────────────────────────────────────────

    /// Acquire data for this cycle. Use retry_acquire() for the primary
    /// acquire. Processor adds its secondary acquire here.
    ///
    /// Returns true if the cycle has data that gates the deadline wait.
    ///   - Producer/Consumer: true only if a slot was acquired.
    ///   - Processor: always true (maintains timing cadence on idle cycles).
    virtual bool acquire(const AcquireContext &ctx) = 0;

    // ── Step B': Shutdown cleanup ────────────────────────────────────

    /// Release/discard any acquired resources when the frame detects
    /// shutdown after the deadline wait.
    virtual void cleanup_on_shutdown() = 0;

    // ── Step D+E: Invoke + commit/release ────────────────────────────

    /// Invoke the engine callback with drained messages, then commit,
    /// discard, or release based on the result.
    ///
    /// The frame calls drain_messages() + drain_inbox_sync() before this.
    ///
    /// Returns false to request loop termination (stop_on_script_error).
    virtual bool invoke_and_commit(std::vector<IncomingMessage> &msgs) = 0;

    // ── Post-loop cleanup ────────────────────────────────────────────

    /// Called once after the outer loop exits. Processor releases held_input.
    virtual void cleanup_on_exit() = 0;
};
```

### 5.5 Concrete implementations

Each role provides a concrete `RoleCycleOps`:

```cpp
// ── Producer ────────────────────────────────────────────────────────

class ProducerCycleOps final : public RoleCycleOps
{
    hub::Producer &producer_;
    ScriptEngine  &engine_;
    RoleHostCore  &core_;
    bool           stop_on_error_;

    // Cached at construction (stable after queue start)
    size_t buf_sz_;
    void  *fz_ptr_;
    size_t fz_sz_;

    // Per-cycle state
    void  *buf_{nullptr};

  public:
    ProducerCycleOps(hub::Producer &p, ScriptEngine &e, RoleHostCore &c,
                     bool stop_on_error);

    bool acquire(const AcquireContext &ctx) override
    {
        buf_ = retry_acquire(ctx, core_,
            [&](auto t) { return producer_.write_acquire(t); });
        return buf_ != nullptr;
    }

    void cleanup_on_shutdown() override
    {
        if (buf_) { producer_.write_discard(); buf_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs) override
    {
        if (buf_) std::memset(buf_, 0, buf_sz_);
        if (core_.has_out_fz())
            fz_ptr_ = producer_.write_flexzone();

        auto result = engine_.invoke_produce(
            InvokeTx{buf_, buf_sz_, fz_ptr_, fz_sz_}, msgs);

        if (buf_)
        {
            if (result == InvokeResult::Commit)
            {
                producer_.write_commit();
                core_.inc_out_slots_written();
            }
            else
            {
                producer_.write_discard();
                core_.inc_out_drop_count();
            }
        }
        else
        {
            core_.inc_out_drop_count();
        }
        buf_ = nullptr;

        if (result == InvokeResult::Error && stop_on_error_)
        {
            core_.request_stop();
            return false;
        }
        return true;
    }

    void cleanup_on_exit() override {} // nothing held across cycles
};
```

```cpp
// ── Consumer ────────────────────────────────────────────────────────

class ConsumerCycleOps final : public RoleCycleOps
{
    hub::Consumer &consumer_;
    ScriptEngine  &engine_;
    RoleHostCore  &core_;
    bool           stop_on_error_;
    std::atomic<uint64_t> &last_seq_;   // ref to role host member

    size_t item_sz_;

    const void *data_{nullptr};

  public:
    bool acquire(const AcquireContext &ctx) override
    {
        data_ = static_cast<const void *>(retry_acquire(ctx, core_,
            [&](auto t) { return const_cast<void *>(consumer_.read_acquire(t)); }));
        return data_ != nullptr;
    }

    void cleanup_on_shutdown() override
    {
        if (data_) { consumer_.read_release(); data_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs) override
    {
        if (data_)
        {
            last_seq_.store(consumer_.last_seq(), std::memory_order_relaxed);
            core_.inc_in_slots_received();
        }

        void *fz_ptr = core_.has_in_fz()
            ? const_cast<void *>(consumer_.read_flexzone()) : nullptr;
        size_t fz_sz = core_.has_in_fz() ? consumer_.flexzone_size() : 0;

        uint64_t errors_before = engine_.script_error_count();
        engine_.invoke_consume(InvokeRx{data_, item_sz_, fz_ptr, fz_sz}, msgs);

        if (data_) { consumer_.read_release(); data_ = nullptr; }

        if (stop_on_error_ && engine_.script_error_count() > errors_before)
        {
            core_.request_stop();
            return false;
        }
        return true;
    }

    void cleanup_on_exit() override {} // nothing held across cycles
};
```

```cpp
// ── Processor ───────────────────────────────────────────────────────

class ProcessorCycleOps final : public RoleCycleOps
{
    hub::Consumer &input_;
    hub::Producer &output_;
    ScriptEngine  &engine_;
    RoleHostCore  &core_;
    bool           stop_on_error_;
    bool           drop_mode_;

    size_t in_sz_, out_sz_;
    void  *out_fz_ptr_;
    size_t out_fz_sz_;
    void  *in_fz_ptr_;
    size_t in_fz_sz_;

    const void *held_input_{nullptr};  // persists across cycles (Block mode)
    void       *out_buf_{nullptr};

  public:
    /// acquire() always returns true — processor maintains timing cadence
    /// even on idle cycles (messages-only). See loop_design_unified.md §5.2.
    bool acquire(const AcquireContext &ctx) override
    {
        // Primary: input with retry (skip if held from previous cycle)
        if (!held_input_)
        {
            held_input_ = static_cast<const void *>(retry_acquire(ctx, core_,
                [&](auto t) { return const_cast<void *>(input_.read_acquire(t)); }));
        }

        // Secondary: output (only if input available, policy-dependent timeout)
        out_buf_ = nullptr;
        if (held_input_)
        {
            if (drop_mode_)
            {
                out_buf_ = output_.write_acquire(std::chrono::milliseconds{0});
            }
            else
            {
                auto output_timeout = ctx.short_timeout;
                if (ctx.deadline != std::chrono::steady_clock::time_point::max())
                {
                    auto remaining = std::chrono::duration_cast<
                        std::chrono::milliseconds>(ctx.deadline - Clock::now());
                    if (remaining > ctx.short_timeout)
                        output_timeout = remaining;
                }
                out_buf_ = output_.write_acquire(output_timeout);
            }
        }

        return true;  // always: processor maintains cadence
    }

    void cleanup_on_shutdown() override
    {
        if (held_input_) { input_.read_release(); held_input_ = nullptr; }
        if (out_buf_)    { output_.write_discard(); out_buf_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<IncomingMessage> &msgs) override
    {
        if (out_buf_) std::memset(out_buf_, 0, out_sz_);

        if (core_.has_out_fz()) out_fz_ptr_ = output_.write_flexzone();
        if (core_.has_in_fz())
            in_fz_ptr_ = const_cast<void *>(input_.read_flexzone());

        auto result = engine_.invoke_process(
            InvokeRx{held_input_, in_sz_, in_fz_ptr_, in_fz_sz_},
            InvokeTx{out_buf_, out_sz_, out_fz_ptr_, out_fz_sz_},
            msgs);

        // Output commit/discard
        if (out_buf_)
        {
            if (result == InvokeResult::Commit)
            { output_.write_commit(); core_.inc_out_slots_written(); }
            else
            { output_.write_discard(); core_.inc_out_drop_count(); }
        }
        else if (held_input_)
        {
            core_.inc_out_drop_count();
        }

        // Input release or hold
        if (held_input_)
        {
            if (out_buf_ || drop_mode_)
            {
                // Normal: processed or dropped. Advance input.
                input_.read_release();
                held_input_ = nullptr;
                core_.inc_in_slots_received();
            }
            // else: Block mode + output failed → hold for next cycle
        }
        out_buf_ = nullptr;

        if (result == InvokeResult::Error && stop_on_error_)
        { core_.request_stop(); return false; }
        return true;
    }

    void cleanup_on_exit() override
    {
        if (held_input_) { input_.read_release(); held_input_ = nullptr; }
    }
};
```

### 5.6 The shared loop frame (in RoleAPIBase)

```cpp
void RoleAPIBase::run_data_loop(const LoopConfig &cfg, RoleCycleOps &ops)
{
    auto &core = *core();

    // ── Timing setup (shared, computed once) ────────────────────────
    const double  period_us   = cfg.period_us;
    const bool    is_max_rate = (cfg.loop_timing == LoopTimingPolicy::MaxRate);
    const auto    short_timeout_us = compute_short_timeout(period_us, cfg.queue_io_wait_timeout_ratio);
    const auto    short_timeout    = std::chrono::duration_cast<std::chrono::milliseconds>(
        short_timeout_us + std::chrono::microseconds{999});

    core.set_configured_period(static_cast<uint64_t>(period_us));

    auto deadline = Clock::time_point::max();

    // ── Outer loop ──────────────────────────────────────────────────
    while (core.should_continue_loop())
    {
        if (core.is_process_exit_requested())
            break;

        const auto cycle_start = Clock::now();

        // ── Step A: Role-specific acquire ────────────────────────────
        AcquireContext ctx{short_timeout, short_timeout_us, deadline, is_max_rate};
        bool has_data = ops.acquire(ctx);

        // ── Step B: Deadline wait ────────────────────────────────────
        if (!is_max_rate && has_data &&
            deadline != Clock::time_point::max() && Clock::now() < deadline)
        {
            std::this_thread::sleep_until(deadline);
        }

        // ── Step B': Shutdown check after potential sleep ────────────
        if (!core.should_continue_loop() || core.is_process_exit_requested())
        {
            ops.cleanup_on_shutdown();
            break;
        }

        // ── Step C: Drain (shared) ──────────────────────────────────
        auto msgs = core.drain_messages();
        drain_inbox_sync();   // uses inbox_queue_ from RoleAPIBase

        // ── Step D+E: Role-specific invoke + commit ─────────────────
        if (!ops.invoke_and_commit(msgs))
            break;  // stop_on_script_error triggered

        // ── Step F: Metrics (shared) ────────────────────────────────
        const auto now     = Clock::now();
        const auto work_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cycle_start).count());
        core.set_last_cycle_work_us(work_us);
        core.inc_iteration_count();
        if (deadline != Clock::time_point::max() && now > deadline)
            core.inc_loop_overrun();

        // ── Step G: Next deadline (shared) ──────────────────────────
        deadline = compute_next_deadline(cfg.loop_timing, deadline,
                                          cycle_start, period_us);
    }

    // ── Post-loop cleanup ───────────────────────────────────────────
    ops.cleanup_on_exit();
}
```

### 5.7 The shared ctrl thread (in RoleAPIBase)

```cpp
void RoleAPIBase::start_ctrl_thread(const CtrlConfig &cfg)
{
    ctrl_thread_ = std::thread([this, cfg]
    {
        ThreadEngineGuard guard(*engine_);
        const bool has_hb = engine_->has_callback("on_heartbeat");

        ZmqPollLoop loop{*core(), log_prefix()};

        // Build socket list from available pointers
        if (auto *p = producer())
            loop.sockets.push_back({p->peer_ctrl_socket_handle(),
                                     [p] { p->handle_peer_events_nowait(); }});

        if (auto *c = consumer())
        {
            loop.sockets.push_back({c->ctrl_zmq_socket_handle(),
                                     [c] { c->handle_ctrl_events_nowait(); }});
            if (void *ds = c->data_zmq_socket_handle())
                loop.sockets.push_back({ds,
                                         [c] { c->handle_data_events_nowait(); }});
        }

        loop.get_iteration = [this] { return core()->iteration_count(); };

        // Heartbeat (all roles)
        loop.periodic_tasks.emplace_back(
            [this, has_hb, &cfg]
            {
                auto ch = out_channel().empty() ? channel() : out_channel();
                messenger()->enqueue_heartbeat(ch, snapshot_metrics_json());
                if (has_hb) engine_->invoke("on_heartbeat");
            },
            cfg.heartbeat_interval_ms);

        // Metrics report (consumer only, or any role if configured)
        if (cfg.report_metrics)
        {
            loop.periodic_tasks.emplace_back(
                [this] {
                    messenger()->enqueue_metrics_report(
                        channel(), uid(), snapshot_metrics_json());
                },
                cfg.heartbeat_interval_ms);
        }

        loop.run();
    });
}

void RoleAPIBase::join_ctrl_thread()
{
    if (ctrl_thread_.joinable())
        ctrl_thread_.join();
}
```

### 5.8 The shared lifecycle (in RoleAPIBase)

```cpp
void RoleAPIBase::run_role(const LoopConfig &loop_cfg,
                            const CtrlConfig &ctrl_cfg,
                            RoleCycleOps     &ops,
                            std::promise<bool> &ready)
{
    // Step 5: invoke on_init
    engine_->invoke_on_init();

    // Sync flexzone checksum after on_init
    if (producer() && core()->has_out_fz())
        producer()->sync_flexzone_checksum();

    // Step 6: Spawn ctrl_thread, signal ready
    core()->set_running(true);
    start_ctrl_thread(ctrl_cfg);

    // Step 7: Signal ready
    ready.set_value(true);

    // Step 8: Run data loop (blocks until shutdown)
    run_data_loop(loop_cfg, ops);

    // Step 9: Stop accepting cross-thread invokes
    engine_->stop_accepting();

    // Step 10: Join ctrl_thread
    core()->set_running(false);
    core()->notify_incoming();
    join_ctrl_thread();

    // Step 11: Final script callback
    engine_->invoke_on_stop();

    // Step 12: Engine finalize
    engine_->finalize();
}
```

### 5.9 Role host becomes thin

```cpp
void ProducerRoleHost::worker_main_()
{
    // Steps 1-4: config, schema, infrastructure, engine (role-specific, unchanged)
    ...

    api_->wire_event_callbacks();

    ProducerCycleOps ops(*out_producer_, *engine_, core_,
                          config_.script().stop_on_script_error);

    api_->run_role(
        LoopConfig{tc.period_us, tc.loop_timing, tc.queue_io_wait_timeout_ratio,
                   sc.stop_on_script_error},
        CtrlConfig{tc.heartbeat_interval_ms, /*report_metrics=*/false},
        ops, ready_promise_);

    // Step 13: Teardown infrastructure (role-specific, unchanged)
    ...
}
```

### 5.10 What stays in role hosts

1. **Config parsing** — each role has a different config structure
2. **Schema resolution** — producer resolves output, consumer input, processor both
3. **Infrastructure creation** — messenger, Producer/Consumer, InboxQueue
4. **CycleOps construction** — one line, passing role-specific references
5. **Infrastructure teardown** — stop, close, reset in correct order

---

## 6. Implementation Plan

### Phase 1: wire_event_callbacks() (smallest, testable independently)

Move the 6-8 callback wiring calls from each role host into
`RoleAPIBase::wire_event_callbacks()`. The method inspects which pointers
are non-null (producer, consumer, messenger) and wires the appropriate
callbacks to `core_.enqueue_message()` / `core_.request_stop()`.

**Files changed:**
- `role_api_base.hpp` — add `wire_event_callbacks()` declaration
- `role_api_base.cpp` — implement (uses producer(), consumer(), messenger())
- `producer_role_host.cpp` — replace inline wiring with `api_->wire_event_callbacks()`
- `consumer_role_host.cpp` — same
- `processor_role_host.cpp` — same

**Verification:** Build + full test suite. No behavioral change.

### Phase 2: RoleCycleOps + run_data_loop()

Extract the shared loop frame into `RoleAPIBase::run_data_loop()`. Create
the three concrete `RoleCycleOps` implementations. The `retry_acquire`
utility lives in the same header.

**Files changed:**
- `role_api_base.hpp` — add `AcquireContext`, `RoleCycleOps`, `LoopConfig`,
  `run_data_loop()`, `retry_acquire()` free function
- `role_api_base.cpp` — implement shared frame + retry_acquire
- `role_cycle_ops.hpp` — declare `ProducerCycleOps`, `ConsumerCycleOps`,
  `ProcessorCycleOps`
- Each role host .cpp — replace `run_data_loop_()` with ops construction +
  `api_->run_data_loop(cfg, ops)`

**Verification:** Build + full test suite. Identical loop behavior.

### Phase 3: set_engine() + start/join_ctrl_thread()

✅ **DONE 2026-04-14.** Thread manager + ctrl thread centralization in RoleAPIBase.
- `start_ctrl_thread(CtrlThreadConfig)` owns BrokerRequestComm connect,
  heartbeat dispatch, notification poll, deregister sequencing.
- All 3 role hosts use `api_->start_ctrl_thread()` and
  `api_->join_all_threads()`.
- Messenger removed entirely (commit `cf53ed3`); `broker_request_comm` is the
  broker protocol DEALER. `broker_and_comm_channel_design.md` is now historical.

### Phase 4: run_role() lifecycle unification

⚠️ **NOT STARTED.** Combine steps 5-12 of `worker_main_()` into
`RoleAPIBase::run_role()`. With BRC and the role-state machine landed,
this is unblocked.

Sub-tasks before coding:
- Audit each role's `worker_main_()` for any role-specific lifecycle hook
  that doesn't fit the §5.8 template.
- ✅ **Finalize/teardown ordering verified 2026-04-14**: all three role hosts
  already use the identical 5-step shutdown sequence:
  ```
  engine_->stop_accepting()
  api_->deregister_from_broker()
  api_->join_all_threads()
  engine_->finalize()
  teardown_infrastructure_()
  ```
  The §5.8 `RoleAPIBase::run_role()` template can lift this verbatim.
  No standardization commit needed — the sequence is already canonical.

### Phase 5: Cleanup + docs

- Adopt `should_continue_loop()` in the shared frame.
- Rewrite HEP-0011 §"Threading Model" against the now-unified surface.
- Archive this tech draft + `broker_and_comm_channel_design.md` once
  Phase 4 lands and is verified against the test suite.
- Dedup `BrcHandle`/`BrokerHandle` test helpers (currently flagged in
  API_TODO: present in both `datahub_broker_health_workers.cpp` and
  `datahub_role_state_workers.cpp`).

---

## 7. Open Questions

1. **ZmqPollLoop location**: Currently in `src/scripting/zmq_poll_loop.hpp`.
   RoleAPIBase is in pylabhub-utils. Options:
   (a) Move to `src/include/utils/` — clean, but exposes ZMQ in public API.
   (b) Keep in scripting dir, include from `role_api_base.cpp` — works because
   the .cpp links against both utils and scripting.
   (c) Absorb into RoleAPIBase Impl — eliminates the dependency but duplicates.
   **Recommendation**: (b) for Phase 3; consider (a) if other consumers appear.

2. **Metrics report asymmetry**: Consumer sends `enqueue_metrics_report()`,
   others don't. `CtrlConfig::report_metrics` flag handles this. No change
   needed — the flag is the right abstraction.

3. **on_zmq_data registration**: `wire_event_callbacks()` should always
   register `on_zmq_data` when consumer is present. If no data arrives on
   the socket, the callback simply never fires. This eliminates the
   SHM-vs-ZMQ conditional and the inconsistency between consumer (conditional)
   and processor (unconditional).

4. **Virtual vs std::function overhead**: `RoleCycleOps` uses virtual methods
   on concrete final classes. The compiler can devirtualize the calls when the
   concrete type is visible at the call site (e.g., `ProducerCycleOps ops;
   api_->run_data_loop(cfg, ops);`). Even without devirtualization, the
   overhead is one indirect call per cycle (~1ns) vs the acquire (~10-100µs).
   Negligible.

5. **drain_inbox_sync in the frame**: Currently implemented as a free function
   in `role_host_helpers.hpp`. RoleAPIBase already has `inbox_queue()`.
   The frame calls `drain_inbox_sync(inbox_queue(), engine_)` — no new
   dependency.

---

## 8. Risk Assessment

- **Low risk**: Phase 1 is a mechanical move of callback wiring.
- **Medium risk**: Phase 2 is the largest — loop extraction + 3 CycleOps.
  Mitigated by the verified timing analysis (§4) proving code-doc parity.
- **Low risk**: Phase 3 (ctrl thread) is mechanical + well-understood.
- **Low risk**: Phase 4 (lifecycle) wraps existing sequence.
- **Mitigation**: Each phase is a separate commit against 1323 tests.

---

## 9. Design Decisions (2026-04-06 discussion)

### 9.1 Thread Manager — lightweight registry (Option A)

Instead of special-purpose `start_ctrl_thread()`/`join_ctrl_thread()` methods,
RoleAPIBase will provide a general-purpose thread registry:

```cpp
void spawn_thread(const std::string &name, std::function<void()> body);
void join_all_threads();   // reverse spawn order
size_t thread_count() const;
```

Every spawned thread automatically gets:
- `ThreadEngineGuard` (engine cross-thread registration)
- Shutdown check via `core_.is_running()`
- Named logging prefix

The ctrl_thread is the first managed thread. Future worker threads (e.g.,
script-requested background tasks) use the same interface. The data loop
thread (owner) is NOT managed — it drives the framework, not the other way.

**Thread inventory per role** (runtime):

| Layer | Threads | Managed by |
|-------|---------|-----------|
| Messenger worker | 1 per messenger | Messenger (internal) |
| ZmqQueue recv/send | 0-2 per role (ZMQ transport only) | ZmqQueue (internal) |
| Data loop | 1 (worker_thread_) | Role host (owner thread) |
| Ctrl thread | 1 | ThreadManager (spawned) |
| Future workers | 0+ | ThreadManager (spawned) |

**Shutdown sequence**:
```
run_data_loop() returns
engine_->stop_accepting()       ← rejects cross-thread invokes
core_.set_running(false)        ← signals all managed threads
api_->join_all_threads()        ← joins ctrl + workers in reverse order
engine_->invoke_on_stop()       ← safe: no other threads
engine_->finalize()
teardown_infrastructure_()      ← stops Messenger, Producer, Consumer
```

### 9.2 Metrics: last_seq — eliminate the copy

`last_seq` had three copies: QueueReader (authoritative), RoleAPIBase::Impl
(atomic copy), and consumer role host (another atomic copy). The copies
existed because the old data loop stored `consumer.last_seq()` into a local
atomic after each `read_acquire()`.

**Decision**: Remove the copies. `RoleAPIBase::last_seq()` forwards directly
to `consumer->last_seq()` → `queue_reader_->last_seq()`. This is already
thread-safe:
- ShmQueue: plain uint64 written only by the data loop thread, read by
  metrics snapshot (relaxed read is safe — no torn read on aligned uint64)
- ZmqQueue: `std::atomic<uint64_t>` with relaxed ordering

**Cost**: ~3ns per call (2 Pimpl dereferences + 1 virtual dispatch + 1
relaxed load). Called once per heartbeat interval (5s), not per data cycle.
The old per-cycle atomic store was actually more expensive.

Also remove `update_last_seq()` from RoleAPIBase — no longer needed. The
CycleOps no longer calls it; the QueueReader is the single source of truth.

### 9.3 CycleOps takes RoleAPIBase& (deferred)

CycleOps currently takes raw `Producer&`/`Consumer&`/`ScriptEngine&`
references directly. The alternative — taking `RoleAPIBase&` and calling
pass-through methods — adds ~12 forwarding methods to RoleAPIBase for
queue I/O operations (`write_acquire`, `read_acquire`, `write_commit`,
`read_release`, `write_discard`, `queue_item_size` × 2 sides).

**Decision**: Defer. Keep raw references for Phase 2. In Phase 3 (ctrl
thread + thread manager), reassess which pointers truly need to be public
on RoleAPIBase. The goal is to make `producer()`/`consumer()` private
eventually, with all access going through typed RoleAPIBase methods.

### 9.4 drain_inbox_sync placement

The shared frame calls `core.drain_messages()` (Step C). The inbox drain
(`drain_inbox_sync`) requires `ScriptEngine*` which the frame doesn't hold.

**Decision**: Each CycleOps calls `drain_inbox_sync(inbox_queue, &engine)`
as the first line of `invoke_and_commit()`. This preserves the "drain right
before invoke" invariant without adding engine dependency to the shared frame.

### 9.5 Phase 1 status

**Completed**: `wire_event_callbacks()` implemented and deployed. All 3 role
hosts use it. 1323/1323 tests pass. Processor dual-messenger hub-dead wired
explicitly. Consumer on_zmq_data always registered (intentional
simplification). Processor channel_closing/channel_error include
`details["channel"]` and `details["source"]` for dual-role disambiguation.
