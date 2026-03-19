# Unified Loop Design — Canonical Reference

**Status**: v2 — Redesigned (2026-03-18)
**Purpose**: Precise pseudo-code for the unified data loop, config changes,
timing policies, and abstraction layer boundaries. This document is the
correctness reference for the ScriptEngine integration.

**Change from v1**: The loop is redesigned with an inner retry-acquire
pattern that maximizes data availability before deadline. Config gains
`target_rate_hz` (float) as an alternative to `target_period_ms` (float).

---

## 1. Config Changes

### 1.1 Rate/Period Config (all 3 roles)

Old:
```json
{"target_period_ms": 100, "slot_acquire_timeout_ms": -1}
```

New (accepts either, not both):
```json
{"target_rate_hz": 1000}       // → period = 1.0 ms
{"target_period_ms": 0.5}      // → period = 0.5 ms (2 kHz)
```

Validation:
- Cannot specify both `target_rate_hz` and `target_period_ms` (error)
- Neither specified → period = 0 (MaxRate), same as current default for consumer/processor
- For producer, neither specified → period = 100ms (current default, FixedRate)
- `target_period_ms` is now `double` (was `int`)
- `target_rate_hz` is `double`
- FixedRate / FixedRateWithCompensation: period >= kMinPeriodUs (100 μs = 10 kHz)
  - The floor is configurable via CMake: `PYLABHUB_MAX_LOOP_RATE_HZ` (default 10000)
  - Period computed as `1e6 / rate_hz` microseconds
- MaxRate: period must be 0 (unchanged)

Internal storage:
```cpp
double target_period_us;  // microseconds, computed from rate_hz or period_ms
```

### 1.2 slot_acquire_timeout_ms → Replaced by Inner Retry Logic

The `slot_acquire_timeout_ms` field is **replaced** by the new inner retry
pattern. The loop computes its own short timeout:
- MaxRate: `queue_check_timeout_ms` (new config field, default 50ms)
- FixedRate: `max(period_us * 0.1, 1000)` μs (10% of period, floor 1ms)

The `queue_check_timeout_ms` field gives MaxRate users explicit control
over how long the loop waits for data before calling the script.

For FixedRate, no user config needed — the timeout is derived from the period.

---

## 2. Timing Policy

### 2.1 LoopTimingPolicy (unchanged enum, updated semantics)

```
enum LoopTimingPolicy { MaxRate, FixedRate, FixedRateWithCompensation }
```

### 2.2 Short Timeout Derivation

```
compute_short_timeout(timing_policy, period_us, queue_check_timeout_ms):
    if timing_policy == MaxRate:
        return milliseconds(queue_check_timeout_ms)    # user-provided, never zero
    else:
        return microseconds(max(period_us * 0.1, 1000))  # 10% of period, floor 1ms
```

### 2.3 Deadline Calculation

```
compute_next_deadline(timing_policy, cycle_start, period_us):
    if timing_policy == MaxRate:
        return time_point::max()   # no deadline (bypass all deadline checks)
    if timing_policy == FixedRate:
        return now() + period_us   # reset from current time
    if timing_policy == FixedRateWithCompensation:
        return cycle_start + period_us  # advance from cycle start
```

Note: FixedRate uses `now()` (after script returns) to reset. FixedRateWith-
Compensation uses `cycle_start` (start of this cycle) to maintain steady
average rate. This matches the current behavior.

---

## 3. Unified Data Loop — Producer

This is the canonical loop that replaces both `ProducerScriptHost::run_loop_`
and `LuaProducerHost::run_data_loop_`.

```
PRODUCER_DATA_LOOP(queue, engine, core, config, inbox_queue):
    # ── Setup ──
    period_us = config.target_period_us
    is_max_rate = (config.loop_timing == MaxRate)
    short_timeout = compute_short_timeout(config.loop_timing, period_us,
                                           config.queue_check_timeout_ms)
    buf_sz = queue.item_size()

    # ── Loop ──
    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        cycle_start = now()

        # ── Step A: Acquire data with retry ──────────────────────────
        #
        # Inner retry loop: attempt acquire with short timeout.
        # Retry until data arrives or deadline approaches.
        # This gives ~9-10 chances per period (FixedRate) vs 1 chance
        # in the old design.
        #
        buf = nullptr
        while true:
            buf = queue.write_acquire(short_timeout)
            if buf != nullptr: break              # got data

            if is_max_rate: break                  # MaxRate: one attempt

            # Check shutdown between retries
            if NOT core.running_threads OR core.shutdown_requested
               OR core.critical_error_:
                break

            remaining = deadline - now()
            if remaining <= short_timeout: break   # not enough time to retry
            # else: go back and retry acquire

        # ── Step B: Deadline wait (FixedRate with early data) ────────
        #
        # If we got data before deadline, sleep until deadline.
        # This ensures predictable callback timing.
        # The slot is held during sleep — acceptable for FixedRate
        # (users choosing FixedRate accept periodic scheduling).
        #
        if NOT is_max_rate AND buf != nullptr AND now() < deadline:
            sleep_until(deadline)

        # ── Step C: Drain everything right before script call ────────
        #
        # Freshest possible: messages + inbox drained at the last
        # moment before the callback. No messages are dropped.
        #
        msgs = core.drain_messages()         # non-blocking, FIFO
        drain_inbox_sync_(engine)            # non-blocking, all in sequence

        # ── Step D: Prepare and invoke callback ──────────────────────
        #
        if buf != nullptr:
            memset(buf, 0, buf_sz)

        result = engine.invoke_produce(buf, buf_sz, flexzone, fz_sz,
                                        fz_type, msgs)
        # Engine handles: GIL (Python), pcall (Lua), error catch,
        # return value → InvokeResult{Commit, Discard, Error}

        # ── Step E: Commit/discard ───────────────────────────────────
        #
        if buf != nullptr:
            if result == Commit:
                queue.write_commit()
                ++out_written
            else:
                queue.write_discard()
                ++drops
        else:
            ++drops   # no slot acquired

        if result == Error:
            ON_SCRIPT_ERROR(config, core)

        # ── Step F: Metrics ──────────────────────────────────────────
        #
        work_us = duration_us(now() - cycle_start)
        store last_cycle_work_us = work_us
        ++iteration_count

        # ── Step G: Compute next deadline ────────────────────────────
        #
        deadline = compute_next_deadline(config.loop_timing,
                                          cycle_start, period_us)

    # ── Exit ──
    log exit state (running_threads, shutdown_requested, critical_error)
```

### Key differences from current design

| Aspect | Current (v1) | New (v2) |
|--------|-------------|----------|
| Acquire attempts | 1 per cycle | ~9-10 per cycle (FixedRate) |
| No-slot path | Separate code path with DUE_CHECK | Same path — script always called at deadline |
| Inbox drain | After acquire (or separate thread) | Right before callback (freshest) |
| Message drain | After acquire | Right before callback (freshest) |
| MaxRate timeout | 50ms magic constant | User-provided `queue_check_timeout_ms` |
| FixedRate timeout | period/2 | 10% of period (retried) |
| Deadline sleep | After callback | Before callback (predictable timing) |
| DUE_CHECK | Complex condition | Eliminated (retry loop handles it) |
| ADVANCE_DEADLINE | Separate function | Eliminated (compute_next_deadline at end) |
| Period config | `int target_period_ms` | `double target_period_ms` or `double target_rate_hz` |

---

## 4. Unified Data Loop — Consumer

Same structure as producer, with read semantics:

```
CONSUMER_DATA_LOOP(queue_reader, engine, core, config, inbox_queue):
    period_us = config.target_period_us
    is_max_rate = (config.loop_timing == MaxRate)
    short_timeout = compute_short_timeout(...)
    item_sz = queue_reader.item_size()

    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        cycle_start = now()

        # ── Step A: Acquire data with retry ──────────────────────────
        data = nullptr
        while true:
            data = queue_reader.read_acquire(short_timeout)
            if data != nullptr: break
            if is_max_rate: break
            if NOT core.running_threads OR core.shutdown_requested
               OR core.critical_error_: break
            remaining = deadline - now()
            if remaining <= short_timeout: break
            # retry

        # ── Step B: Deadline wait (FixedRate with early data) ────────
        if NOT is_max_rate AND data != nullptr AND now() < deadline:
            sleep_until(deadline)

        # ── Step C: Drain everything right before callback ───────────
        msgs = core.drain_messages()
        drain_inbox_sync_(engine)

        # ── Step D: Invoke callback ──────────────────────────────────
        #
        # Consumer: read-only slot, no return value.
        # Engine increments script_errors internally on error.
        #
        if data != nullptr:
            update last_seq = queue_reader.last_seq()
            ++in_received

        engine.invoke_consume(data, item_sz, flexzone, fz_sz,
                               fz_type, msgs)

        # ── Step E: Release slot ─────────────────────────────────────
        if data != nullptr:
            queue_reader.read_release()

        # ── Step F: Metrics ──────────────────────────────────────────
        work_us = duration_us(now() - cycle_start)
        store last_cycle_work_us = work_us
        ++iteration_count

        # ── Step G: Next deadline ────────────────────────────────────
        deadline = compute_next_deadline(config.loop_timing,
                                          cycle_start, period_us)

    log exit state
```

### Differences from Producer

1. `read_acquire` / `read_release` instead of `write_acquire` / `write_commit`
2. No commit/discard decision — slot is released after callback
3. No drops counter
4. `invoke_consume` is void (no InvokeResult)
5. `last_seq` updated when data arrives
6. Read-only slot (`const void*`)

### Note: Early exit check removed

The current design has an early exit check between `read_acquire` success
and callback (consumer_script_host.cpp:679-683). In the new design, the
inner retry loop already checks shutdown between retries, and the outer
while condition catches it before the next cycle. The early exit check is
redundant — if shutdown was requested during `read_acquire`, the outer
loop condition catches it at the top of the next iteration. The slot is
released normally after the callback.

However, for safety, we could add a check after Step B (deadline wait):
```
if NOT core.running_threads OR core.critical_error_:
    if data: queue_reader.read_release()
    break
```
This catches shutdowns that arrive during the deadline sleep.

---

## 5. Unified Data Loop — Processor

Dual-queue loop. Does NOT use `hub::Processor` (see §5.3 for rationale).

```
PROCESSOR_DATA_LOOP(in_q, out_q, engine, core, config, inbox_queue):
    period_us = config.target_period_us
    is_max_rate = (config.loop_timing == MaxRate)
    short_timeout = compute_short_timeout(...)
    drop_mode = (config.overflow_policy == Drop)
    # Output timeout: for Drop mode, non-blocking (don't wait for output);
    # for Block mode, use same short_timeout (keeps loop responsive).
    output_timeout = drop_mode ? 0ms : short_timeout

    in_sz = in_q.item_size()
    out_sz = out_q.item_size()

    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        cycle_start = now()

        # ── Step A: Acquire input with retry ─────────────────────────
        in_buf = nullptr
        while true:
            in_buf = in_q.read_acquire(short_timeout)
            if in_buf != nullptr: break
            if is_max_rate: break
            if NOT core.running_threads OR core.shutdown_requested
               OR core.critical_error_: break
            remaining = deadline - now()
            if remaining <= short_timeout: break

        # ── Step B: Acquire output ───────────────────────────────────
        #
        # Always attempt output acquire (even if input is nil —
        # on_process can produce output without input).
        #
        out_buf = out_q.write_acquire(output_timeout)

        # ── Step C: Deadline wait (FixedRate) ────────────────────────
        if NOT is_max_rate AND now() < deadline:
            sleep_until(deadline)

        # ── Step D: Drain everything right before callback ───────────
        msgs = core.drain_messages()
        drain_inbox_sync_(engine)

        # ── Step E: Prepare and invoke callback ──────────────────────
        if out_buf != nullptr:
            memset(out_buf, 0, out_sz)

        result = engine.invoke_process(in_buf, in_sz, out_buf, out_sz,
                                        flexzone, fz_sz, fz_type, msgs)

        # ── Step F: Commit/discard output, release input ─────────────
        if out_buf != nullptr:
            if result == Commit:
                out_q.write_commit()
                ++out_written
            else:
                out_q.write_discard()
                ++out_drops
        else:
            ++out_drops  # no output slot acquired

        if in_buf != nullptr:
            in_q.read_release()
            ++in_received

        if result == Error:
            ON_SCRIPT_ERROR(config, core)

        # ── Step G: Metrics + next deadline ──────────────────────────
        work_us = duration_us(now() - cycle_start)
        store last_cycle_work_us = work_us
        ++iteration_count
        deadline = compute_next_deadline(config.loop_timing,
                                          cycle_start, period_us)

    log exit state
```

### 5.1 Processor: Output Acquire Strategy

For the processor, output acquire is always attempted (even without input).
This differs from the current Lua processor which only acquires output
after input succeeds.

Rationale: the script may want to produce output based on timer/messages
alone (e.g., heartbeat output without input). The script receives
`in_slot=nil, out_slot=nil_or_buf` and decides what to do.

If output acquire fails:
- Drop mode: `out_buf = nullptr`, script called with nil output, drops incremented
- Block mode: `out_buf` blocks up to `short_timeout`, may still be nullptr

### 5.2 Processor: Input Release Timing

Input is released AFTER the callback returns (same as consumer). The
script accesses the input data during the callback; releasing before
the callback would invalidate the pointer.

### 5.3 Why Not hub::Processor

`hub::Processor` (hub_processor.cpp) does NOT handle:
- Control messages (`core.drain_messages()`)
- Inbox drain
- Timing policy (FixedRate / FixedRateWithCompensation)
- The inner retry-acquire pattern
- `compute_next_deadline` at cycle end

It handles: read→handler→write with Drop/Block, hot-swap handler,
pre-hook. These are insufficient for the unified design.

`hub::Processor` remains available for C++ embedded usage (no script
engine, no inbox, no timing policy).

---

## 6. Inbox Drain — Unified Design

```
DRAIN_INBOX_SYNC(inbox_queue, engine):
    if inbox_queue == nullptr: return

    while true:
        item = inbox_queue.recv_one(0ms)         # non-blocking
        if item == nullptr: break                 # no more messages

        engine.invoke_on_inbox(item.data, item.size,
                                inbox_type_name, item.sender_id)
        # Engine handles: GIL (Python), pcall (Lua), errors

        ack_code = 0     # success
        # If engine reported error: ack_code stays 0
        # (we don't drop inbox messages on script errors)
        inbox_queue.send_ack(ack_code)
```

All messages are preserved in FIFO order. No dropping.

Inbox is drained in Step C (right before the script callback) for
maximum freshness. The script receives the most up-to-date inbox
state at call time.

---

## 7. ctrl_thread_ — Unchanged

```
RUN_CTRL_THREAD(core, role_tag, sockets, heartbeat_fn):
    ZmqPollLoop loop(core, role_tag)
    loop.sockets = [
        {peer_ctrl_socket, handler: handle_peer_events_nowait()},
        {ctrl_zmq_socket,  handler: handle_ctrl_events_nowait()},
    ]
    loop.on_heartbeat = heartbeat_fn
    loop.run()
```

Already engine-agnostic. No changes needed.

---

## 8. Error Handling

### 8.1 Error Sources and Layers

| Error | Source | Layer | Action |
|-------|--------|-------|--------|
| Script exception | L1 (engine) | Engine catches → `InvokeResult::Error` | L2 checks, applies `stop_on_script_error` |
| Wrong return type | L1 (engine) | Engine logs, returns `InvokeResult::Error` | Same as above |
| `api.set_critical_error()` | L1 (engine→L0) | Engine calls `core.set_critical_error()` | Outer loop exits on `critical_error_` |
| `stop_on_script_error` | L2 (loop) | `core.shutdown_requested = true` | Outer loop exits |
| Peer dead | L3 (host) | Callback: `core.stop_reason_ = PeerDead; core.shutdown_requested = true` | Outer loop exits |
| Hub dead | L3 (host) | Callback: `core.stop_reason_ = HubDead; core.shutdown_requested = true` | Outer loop exits |
| Queue nullptr | L2 (loop) | Early return before loop starts | Host reports startup failure |
| Config invalid | L3 (host) | Throws during `setup_infrastructure_()` | Host catches, signals failure |

### 8.2 ON_SCRIPT_ERROR (in data loop)

```
ON_SCRIPT_ERROR(config, core):
    # script_errors already incremented by engine
    if config.stop_on_script_error:
        core.shutdown_requested = true
```

### 8.3 Shutdown Responsiveness

Shutdown is checked at:
1. Outer loop condition (every cycle)
2. Inner retry loop (between acquire attempts, every `short_timeout`)
3. After deadline sleep (optional safety check)

Worst-case latency: `short_timeout` (10% of period, or `queue_check_timeout_ms`
for MaxRate).

For a 100ms period: worst-case 10ms response. For MaxRate with 50ms
queue_check_timeout: worst-case 50ms response. Both acceptable.

---

## 9. Abstraction Layer Design

```
┌──────────────────────────────────────────┐
│  Layer 4: Role Main (producer_main.cpp)  │  Signal handling, config, dispatch
├──────────────────────────────────────────┤
│  Layer 3: Role Host                      │  Infrastructure setup/teardown,
│  (ProducerRoleHost)                      │  ctrl_thread_, worker_thread_,
│                                          │  engine lifecycle
├──────────────────────────────────────────┤
│  Layer 2: Data Loop                      │  Inner retry acquire, deadline
│  (run_data_loop_ per role)               │  wait, drain, invoke, commit,
│                                          │  timing, metrics
├──────────────────────────────────────────┤
│  Layer 1: ScriptEngine                   │  invoke_produce/consume/process,
│  (LuaEngine / PythonEngine)              │  slot views, GIL, return parsing,
│                                          │  child instances, engine metrics
├──────────────────────────────────────────┤
│  Layer 0: Shared Utilities               │  LoopTimingPolicy, RoleHostCore,
│  (loop_timing_policy.hpp etc)            │  compute_short_timeout,
│                                          │  compute_next_deadline
└──────────────────────────────────────────┘
```

### 9.1 ScriptEngine Responsibilities (Layer 1)

The engine is a **stateful callback dispatcher**:
- Manages all script instances (owner + children)
- Ownership is single-threaded (move-only, mutex-protected transfer)
- Child instances can be spawned for other threads (Lua), with the
  engine tracking all instances for unified metrics/health
- Handles GIL (Python) or direct pcall (Lua) internally
- Returns `InvokeResult` from produce/process callbacks
- Catches all script errors → `InvokeResult::Error` + increment counter
- Builds typed slot views in the engine's type system

### 9.2 Data Loop Responsibilities (Layer 2)

One implementation per role (producer/consumer/processor). Engine-agnostic.
Steps A through G as described in §3-§5.

### 9.3 Role Host Responsibilities (Layer 3)

One implementation per role. Engine-agnostic. Owns:
- `worker_thread_` lifecycle (spawn, ready_promise, join)
- Engine creation: `config.script_type` → `LuaEngine` or `PythonEngine`
- Infrastructure: Messenger, Producer/Consumer, queue(s)
- Event wiring: peer-dead, hub-dead → `core_` flags
- ctrl_thread_: `ZmqPollLoop` (pure C++)
- Heartbeat: `snapshot_metrics_json()`
- `wait_for_roles_()` coordination
- Engine lifecycle calls on worker thread:
  `initialize → load_script → register_slot_type → build_api → invoke_on_init
   → [data loop] → invoke_on_stop → finalize`

---

## 10. Implementation Sequence

### Phase 1: Config + Utilities
- Add `target_rate_hz` (double) to all 3 configs
- Change `target_period_ms` from `int` to `double`
- Add `queue_check_timeout_ms` (int, default 50) to all 3 configs
- Remove `slot_acquire_timeout_ms`
- Add `PYLABHUB_MAX_LOOP_RATE_HZ` CMake option (default 10000)
- Implement `compute_short_timeout()` and `compute_next_deadline()`
- Update `parse_loop_timing_policy()` validation

### Phase 2: ProducerRoleHost
- Implement `ProducerRoleHost` (Layer 3 + Layer 2)
- Wire with LuaEngine first
- Verify behavior matches existing LuaProducerHost
- All tests pass

### Phase 3: PythonEngine
- Implement `PythonEngine` (wraps py::scoped_interpreter)
- Wire ProducerRoleHost with PythonEngine
- Verify behavior matches existing ProducerScriptHost
- All tests pass

### Phase 4: Consumer + Processor RoleHosts
- ConsumerRoleHost (Layer 3 + Layer 2)
- ProcessorRoleHost (Layer 3 + Layer 2)
- Wire both with LuaEngine + PythonEngine

### Phase 5: Cleanup
- Delete old hosts: LuaProducerHost, LuaConsumerHost, LuaProcessorHost,
  LuaRoleHostBase, ProducerScriptHost, ConsumerScriptHost,
  ProcessorScriptHost, PythonRoleHostBase, PythonScriptHost
- Delete old ScriptHost base class
- Update HEP-0011

### Phase 6: Multi-state + Rate Config
- Implement `create_thread_state()` for Lua ctrl_thread_ callbacks
- Add `target_rate_hz` config support end-to-end
- Update all config tests
