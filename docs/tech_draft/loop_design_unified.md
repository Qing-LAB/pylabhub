# Unified Loop Design — Canonical Reference

**Status**: Reference document (2026-03-18)
**Purpose**: Precise pseudo-code for all 3 role data loops, error handling,
timing policies, and abstraction layer boundaries. This document is the
correctness reference for the ScriptEngine integration.

---

## 1. Existing Abstraction Layers

### 1.1 LoopTimingPolicy (loop_timing_policy.hpp)

Shared by all 3 roles, engine-agnostic.

```
enum LoopTimingPolicy { MaxRate, FixedRate, FixedRateWithCompensation }

compute_slot_acquire_timeout(explicit_ms, period_ms):
    if explicit_ms == 0:  return 0          # non-blocking
    if explicit_ms > 0:   return explicit_ms # user override
    # explicit_ms == -1 (derive):
    if period_ms > 0:     return max(period_ms / 2, 1)
    return 50  # kMaxRateDefaultMs
```

### 1.2 Timing Block (shared by all 6 loop implementations)

This exact block appears in all producer/consumer/processor loops:

```
TIMING_BLOCK(iter_start, next_deadline, period, is_fixed_rate, loop_timing):
    now = steady_clock::now()
    work_us = duration_cast<microseconds>(now - iter_start)
    store last_cycle_work_us = work_us

    if is_fixed_rate:
        if now < next_deadline:
            sleep_for(next_deadline - now)
            next_deadline += period
        else:  # overrun
            if loop_timing == FixedRateWithCompensation:
                next_deadline += period       # catch up
            else:  # FixedRate
                next_deadline = now + period   # reset from now
```

### 1.3 Deadline Advance on No-Slot Path

When acquire fails but callback is invoked (no-slot path), deadline is
advanced with the SAME logic as the normal timing block, minus the sleep:

```
ADVANCE_DEADLINE(next_deadline, period, is_fixed_rate, loop_timing):
    if is_fixed_rate:
        now = steady_clock::now()
        if now >= next_deadline:
            if loop_timing == FixedRateWithCompensation:
                next_deadline += period
            else:
                next_deadline = now + period
```

### 1.4 "Due" Check (should callback fire on no-slot?)

```
DUE_CHECK(is_fixed_rate, iter_start, next_deadline, msgs):
    due = !is_fixed_rate OR iter_start >= next_deadline
    return !msgs.empty() OR due
```

Rationale:
- MaxRate: always fire (script expects every iteration)
- FixedRate: fire only when deadline reached (avoid flooding on busy acquire)
- Messages pending: always fire (control events are time-critical)

### 1.5 Error Handling (shared pattern)

```
ON_SCRIPT_ERROR(error, config, core):
    increment script_errors
    log error
    if config.stop_on_script_error:
        core.shutdown_requested = true

ON_CRITICAL_ERROR(core):
    core.critical_error_ = true
    core.stop_reason_ = CriticalError
    core.shutdown_requested = true
```

### 1.6 RoleHostCore (role_host_core.hpp)

Engine-agnostic state:

```
struct RoleHostCore:
    running_threads: atomic<bool>        # set false by stop_role()
    shutdown_requested: atomic<bool>     # set by peer-dead/hub-dead/api.stop()/critical
    g_shutdown: atomic<bool>*            # signal handler flag (external)
    stop_reason_: atomic<int>            # StopReason enum
    critical_error_: atomic<bool>
    script_errors_: atomic<uint64_t>
    incoming_queue_: vector<IncomingMessage> + mutex
    drain_messages() -> vector<IncomingMessage>   # non-blocking FIFO dequeue
    set_critical_error()                          # atomically set error + shutdown
```

### 1.7 hub::Processor (hub_processor.cpp)

Existing loop abstraction for the processor role. Owns `process_thread_`.
Demand-driven: blocks on `in_queue->read_acquire()`.

Key features:
- Hot-swappable handler via `PortableAtomicSharedPtr`
- Separate timeout handler for no-input path
- Pre-hook callback (for GIL acquire etc.)
- Drop vs Block overflow policy
- zero_fill_output option
- Internal iteration_count, in_received, out_written, out_drops counters
- Does NOT handle: messages, inbox, timing policy, deadline tracking

The Python processor uses this; the Lua processor reimplements the loop manually.

---

## 2. Producer Loop — Canonical Pseudo-code

Source: producer_script_host.cpp:662-808 (Python), lua_producer_host.cpp:630-735 (Lua)

```
PRODUCER_DATA_LOOP(queue, engine, core, config, inbox_queue):
    # ── Setup ──
    acquire_timeout = compute_slot_acquire_timeout(config.slot_acquire_timeout_ms,
                                                    config.target_period_ms)
    is_fixed_rate = (config.loop_timing != MaxRate)
    period = milliseconds(config.target_period_ms)
    next_deadline = now() + period
    buf_sz = queue.item_size()

    # ── Loop ──
    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        iter_start = now()

        # 1. Acquire write slot (may timeout → nullptr)
        buf = queue.write_acquire(acquire_timeout)

        # 2. Drain messages AFTER acquire (never lost on acquire failure)
        msgs = core.drain_messages()

        # 3. No-slot path
        if buf == nullptr:
            if DUE_CHECK(is_fixed_rate, iter_start, next_deadline, msgs):
                result = engine.invoke_produce(nullptr, 0, flexzone, fz_sz, fz_type, msgs)
                if result == Error:
                    ON_SCRIPT_ERROR(...)
                ADVANCE_DEADLINE(next_deadline, period, is_fixed_rate, config.loop_timing)
            increment drops
            increment iteration_count
            # Lua only: drain_inbox_sync_(engine)
            continue

        # 4. Zero-fill output slot
        memset(buf, 0, buf_sz)

        # 5. Invoke callback
        result = engine.invoke_produce(buf, buf_sz, flexzone, fz_sz, fz_type, msgs)

        # 6. Commit or discard
        if result == Commit:
            queue.write_commit()
            increment out_written
        else:  # Discard or Error
            queue.write_discard()
            increment drops
            if result == Error:
                ON_SCRIPT_ERROR(...)

        # 7. Increment iteration
        increment iteration_count

        # 8. Inbox drain (Lua: synchronous; Python: separate thread)
        if inbox_queue AND engine is synchronous:
            drain_inbox_sync_(engine)

        # 9. Timing
        TIMING_BLOCK(iter_start, next_deadline, period, is_fixed_rate, config.loop_timing)

    # ── Exit ──
    log exit state
```

### Producer: Differences between Python and Lua

| Aspect | Python | Lua |
|--------|--------|-----|
| GIL | acquire/release around invoke | none |
| Error catch | py::error_already_set → catch | lua_pcall return code → InvokeResult |
| Inbox | separate inbox_thread_ | drain_inbox_sync_() in loop (steps 3, 8) |
| Metrics store | api_.increment_* | atomic fetch_add directly |
| Flexzone view | py::object (ctypes/numpy) | FFI cdata |

With ScriptEngine: the engine handles GIL and error translation internally.
The loop pseudo-code above is engine-agnostic.

---

## 3. Consumer Loop — Canonical Pseudo-code

Source: consumer_script_host.cpp:602-738 (Python), lua_consumer_host.cpp:578-681 (Lua)

```
CONSUMER_DATA_LOOP(queue_reader, engine, core, config, inbox_queue):
    # ── Setup ──
    timeout = compute_slot_acquire_timeout(config.slot_acquire_timeout_ms,
                                            config.target_period_ms)
    item_sz = queue_reader.item_size()
    is_fixed_rate = (config.loop_timing != MaxRate)
    period = milliseconds(config.target_period_ms)
    next_deadline = now() + period

    # ── Loop ──
    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        iter_start = now()

        # 1. Block until data arrives (or timeout → nullptr)
        data = queue_reader.read_acquire(timeout)
        update last_seq = queue_reader.last_seq()

        # 2. Drain messages
        msgs = core.drain_messages()

        # 3. No-data path
        if data == nullptr:
            if DUE_CHECK(is_fixed_rate, iter_start, next_deadline, msgs):
                engine.invoke_consume(nullptr, 0, flexzone, fz_sz, fz_type, msgs)
                # Note: invoke_consume has no return value (void)
                # Error handling inside engine (increments script_errors)
                ADVANCE_DEADLINE(next_deadline, period, is_fixed_rate, config.loop_timing)
            increment iteration_count
            # Lua only: drain_inbox_sync_(engine)
            continue

        # 4. Increment received counter
        increment in_received

        # 5. Early exit check (between data arrival and callback)
        if NOT core.running_threads OR core.critical_error_:
            queue_reader.read_release()
            break

        # 6. Invoke callback (read-only slot)
        engine.invoke_consume(data, item_sz, flexzone, fz_sz, fz_type, msgs)
        # Error handling inside engine

        # 7. Release slot
        queue_reader.read_release()

        # 8. Increment iteration
        increment iteration_count

        # 9. Inbox drain
        if inbox_queue AND engine is synchronous:
            drain_inbox_sync_(engine)

        # 10. Timing
        TIMING_BLOCK(iter_start, next_deadline, period, is_fixed_rate, config.loop_timing)

    # ── Exit ──
    log exit state
```

### Consumer: Key differences from Producer

1. **Demand-driven**: blocks on `read_acquire()` — data arrives when producer writes
2. **No commit/discard**: `read_release()` returns the slot (automatic)
3. **No drops counter**: reads what arrives
4. **Extra early-exit check** (step 5): between `read_acquire` success and callback,
   re-check shutdown. This prevents calling into script after shutdown signal during
   a blocking read.
5. **Read-only slot**: `invoke_consume` receives `const void*`
6. **No return value**: `invoke_consume` is void (no commit/discard decision)
7. **last_seq update**: consumer tracks sequence for API exposure

---

## 4. Processor Loop — Canonical Pseudo-code

### 4.1 hub::Processor Loop (used by Python ProcessorScriptHost)

Source: hub_processor.cpp:67-209

```
PROCESSOR_HUB_LOOP(in_queue, out_queue, opts, handler, timeout_handler):
    drop_mode = (opts.overflow_policy == Drop)
    out_timeout = drop_mode ? 0ms : opts.input_timeout
    out_item_sz = out_queue.item_size()

    while NOT stop AND NOT critical_error:
        ++iteration_count

        # Load hot-swappable handler
        h = handler.load()
        if h == nullptr:
            sleep(10ms)
            continue

        # 1. Read input (blocking)
        in_data = in_queue.read_acquire(opts.input_timeout)

        if in_data == nullptr:
            # TIMEOUT PATH
            th = timeout_handler.load()
            if th == nullptr: continue

            out_data = out_queue.write_acquire(out_timeout)
            if out_data AND zero_fill: memset(out_data, 0, out_item_sz)
            out_fz = out_queue.write_flexzone()

            pre_hook()  # GIL acquire hook
            commit = th(out_data, out_fz)

            if out_data:
                if commit: write_commit(); ++out_written
                else:      write_discard(); ++out_drops
            continue

        # NORMAL PATH
        ++in_received

        # Early exit check
        if stop OR critical_error:
            in_queue.read_release()
            break

        # 2. Acquire output (Drop=0ms, Block=input_timeout)
        out_data = out_queue.write_acquire(out_timeout)
        if out_data == nullptr:
            ++out_drops
            in_queue.read_release()
            continue

        if zero_fill: memset(out_data, 0, out_item_sz)

        in_fz = in_queue.read_flexzone()
        out_fz = out_queue.write_flexzone()

        pre_hook()  # GIL acquire hook
        commit = h(in_data, in_fz, out_data, out_fz)

        if commit: write_commit(); ++out_written
        else:      write_discard(); ++out_drops

        in_queue.read_release()
```

### 4.2 Lua Processor Manual Loop

Source: lua_processor_host.cpp:865-1020

The Lua processor reimplements the loop manually because:
1. `lua_State` must be accessed from the working thread only
2. Messages need to be drained per-iteration (hub::Processor doesn't do this)
3. Inbox needs synchronous drain
4. Timing policy needs to be applied (hub::Processor doesn't do this)

```
LUA_PROCESSOR_DATA_LOOP(in_q, out_q, engine, core, config, inbox_queue):
    input_timeout = compute_slot_acquire_timeout(config.slot_acquire_timeout_ms,
                                                  config.target_period_ms)
    drop_mode = (config.overflow_policy == Drop)
    output_timeout = drop_mode ? 0ms : input_timeout
    is_fixed_rate = (config.loop_timing != MaxRate)
    period = milliseconds(config.target_period_ms)
    next_deadline = now() + period

    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        iter_start = now()

        # 1. Read input
        in_buf = in_q.read_acquire(input_timeout)
        msgs = core.drain_messages()

        # 2. No-input path
        if in_buf == nullptr:
            if DUE_CHECK(is_fixed_rate, iter_start, next_deadline, msgs):
                # Try to acquire output for timeout callback
                out_buf = out_q.write_acquire(output_timeout)
                if out_buf:
                    memset(out_buf, 0, out_sz)
                    result = engine.invoke_process(nullptr, 0, out_buf, out_sz,
                                                    flexzone, fz_sz, fz_type, msgs)
                    if result == Commit: out_q.write_commit(); ++out_written
                    else:               out_q.write_discard(); ++out_drops
                    if result == Error: ON_SCRIPT_ERROR(...)
                else:
                    # No output slot either — call with nil out
                    result = engine.invoke_process(nullptr, 0, nullptr, 0,
                                                    flexzone, fz_sz, fz_type, msgs)
                    if result == Error: ON_SCRIPT_ERROR(...)
                ADVANCE_DEADLINE(...)
            increment iteration_count
            drain_inbox_sync_(engine)
            continue

        # 3. Acquire output
        out_buf = out_q.write_acquire(output_timeout)
        if out_buf == nullptr:
            in_q.read_release()
            ++in_received
            ++out_drops
            drain_inbox_sync_(engine)
            continue

        # 4. Both slots — invoke callback
        memset(out_buf, 0, out_sz)
        result = engine.invoke_process(in_buf, in_sz, out_buf, out_sz,
                                        flexzone, fz_sz, fz_type, msgs)

        if result == Commit: out_q.write_commit(); ++out_written
        else:               out_q.write_discard(); ++out_drops
        if result == Error: ON_SCRIPT_ERROR(...)

        in_q.read_release()
        ++in_received
        increment iteration_count

        # 5. Inbox + timing
        drain_inbox_sync_(engine)
        TIMING_BLOCK(iter_start, next_deadline, period, is_fixed_rate, config.loop_timing)

    log exit state
```

### 4.3 Processor: Design Decision for Unified Host

The Python path delegates to `hub::Processor` which does NOT handle:
- Messages (drain_messages)
- Inbox
- Timing policy (FixedRate/Compensation)
- DUE_CHECK for no-input path

The Lua path reimplements everything and adds these.

**Decision**: The unified ProcessorRoleHost should NOT use `hub::Processor`.
Instead, it should use the same pattern as the Lua processor — a manual
dual-queue loop that handles messages, inbox, and timing. This:
1. Keeps all 3 role loops structurally identical (same base loop pattern)
2. Eliminates the GIL pre-hook workaround
3. Allows direct `engine.invoke_process()` calls
4. Enables synchronous inbox drain for all engines

`hub::Processor` remains available for C++ embedded usage where no script
engine is involved.

---

## 5. Inbox Drain — Canonical Pseudo-code

### 5.1 Synchronous Drain (Lua, and unified future)

Source: lua_role_host_base.cpp:806-858

```
DRAIN_INBOX_SYNC(inbox_queue, engine):
    if inbox_queue == nullptr: return

    while true:
        item = inbox_queue.recv_one(0ms)    # non-blocking
        if item == nullptr: break

        engine.invoke_on_inbox(item.data, item.size, type_name, item.sender_id)
        # Note: item.data is valid ONLY until next recv_one()

        ack_code = 0  # success
        # (if invoke threw: ack_code = 3)
        inbox_queue.send_ack(ack_code)
```

### 5.2 Parallel Inbox Thread (Python current)

Source: producer_script_host.cpp:840-880

```
RUN_INBOX_THREAD(inbox_queue, engine):
    while running:
        item = inbox_queue.recv_one(100ms)  # blocking with timeout
        if item == nullptr: continue        # timeout, retry

        GIL_ACQUIRE
        try:
            engine.invoke_on_inbox(item.data, item.size, type_name, item.sender_id)
            ack_code = 0
        catch:
            ack_code = 3
        GIL_RELEASE

        inbox_queue.send_ack(ack_code)
```

**Unified design**: Use synchronous drain for all engines. The engine
handles GIL internally if needed. This eliminates inbox_thread_ entirely.
Latency tradeoff: inbox messages processed once per data loop iteration
instead of continuously. For most workloads this is acceptable.

---

## 6. ctrl_thread_ — Canonical Pseudo-code

Source: All 6 hosts use the same pattern via `scripting::ZmqPollLoop`.

```
RUN_CTRL_THREAD(core, role_tag, sockets, heartbeat_fn, metrics_fn):
    ZmqPollLoop loop(core, role_tag)
    loop.sockets = [
        {peer_ctrl_socket, handler: handle_peer_events_nowait()},
        # consumer/processor also poll: data_zmq_socket if SHM transport
        {ctrl_zmq_socket, handler: handle_ctrl_events_nowait()},
    ]
    loop.on_heartbeat = heartbeat_fn    # sends metrics report
    loop.run()  # blocks until core.shutdown_requested
```

This is already engine-agnostic. No changes needed.

---

## 7. Abstraction Layer Design

Based on the analysis above, the loop has these distinct layers:

```
┌──────────────────────────────────────────┐
│  Layer 4: Role Main (producer_main.cpp)  │  Signal handling, config, dispatch
├──────────────────────────────────────────┤
│  Layer 3: Role Host                      │  Infrastructure setup/teardown,
│  (ProducerRoleHost)                      │  ctrl_thread_, worker_thread_
├──────────────────────────────────────────┤
│  Layer 2: Data Loop                      │  Acquire/release, commit/discard,
│  (run_data_loop_ per role)               │  timing, inbox drain, metrics
├──────────────────────────────────────────┤
│  Layer 1: ScriptEngine                   │  invoke_produce/consume/process,
│  (LuaEngine / PythonEngine)              │  slot views, return parsing
├──────────────────────────────────────────┤
│  Layer 0: Shared Utilities               │  LoopTimingPolicy, RoleHostCore,
│  (loop_timing_policy.hpp etc)            │  compute_slot_acquire_timeout
└──────────────────────────────────────────┘
```

### 7.1 Error Isolation by Layer

| Error | Layer | Handling |
|-------|-------|----------|
| Slot acquire timeout | L2 | No-slot path (invoke with nullptr, DUE_CHECK) |
| Script exception | L1 | Engine catches, returns InvokeResult::Error |
| Wrong return type | L1 | Engine logs, returns InvokeResult::Error |
| Critical error (user) | L1→L0 | Engine calls core.set_critical_error() |
| stop_on_script_error | L2 | Loop checks InvokeResult::Error, sets shutdown |
| Peer dead | L3 | Callback sets core.stop_reason_ + shutdown |
| Hub dead | L3 | Callback sets core.stop_reason_ + shutdown |
| Queue not initialized | L2 | Early return before loop |
| Config validation | L3 | Throws during setup (before loop starts) |

### 7.2 What Each Layer Owns

**Layer 3 (Role Host)** — one per role, engine-agnostic:
- worker_thread_ lifecycle (spawn, ready_promise, join)
- Engine creation based on config.script_type
- Infrastructure: Messenger, Producer/Consumer, queue creation
- Event wiring: peer-dead, hub-dead callbacks
- ctrl_thread_ setup + ZmqPollLoop
- Heartbeat metrics snapshot
- wait_for_roles_() coordination
- Engine lifecycle: initialize → load_script → build_api → finalize

**Layer 2 (Data Loop)** — one per role, engine-agnostic:
- Slot acquire/release (read or write)
- Message drain timing (after acquire)
- DUE_CHECK for no-slot/no-data path
- Invoke engine callback (returns InvokeResult or void)
- Commit/discard based on InvokeResult
- Synchronous inbox drain
- Timing block (sleep, deadline advance)
- Metrics counters (out_written, in_received, drops, work_us)
- Loop exit logging

**Layer 1 (ScriptEngine)** — one per engine type:
- Script loading + callback extraction
- Slot view creation (FFI cdata / ctypes struct)
- Callback invocation (pcall / py::call)
- GIL management (Python only)
- Return value interpretation → InvokeResult
- Error catching → InvokeResult::Error + log
- API table/object construction from RoleContext

**Layer 0 (Shared)** — engine-agnostic utilities:
- LoopTimingPolicy enum + parse + default
- compute_slot_acquire_timeout()
- RoleHostCore (flags, message queue, stop reason)

---

## 8. Implementation Sequence

1. **ProducerRoleHost** (Layer 3 + Layer 2 for producer)
   - Extract `setup_infrastructure_()` from existing ProducerScriptHost::start_role()
   - Implement `run_data_loop_()` matching pseudo-code §2 exactly
   - Wire with LuaEngine first (simpler — no GIL)
   - Verify against existing LuaProducerHost behavior

2. **PythonEngine** (Layer 1)
   - Extract from existing ProducerScriptHost callback code
   - GIL acquire/release inside invoke methods
   - py::scoped_interpreter as member (no dedicated thread)

3. **ConsumerRoleHost** (Layer 3 + Layer 2 for consumer)
   - Implement `run_data_loop_()` matching pseudo-code §3 exactly
   - Pay attention to step 5 (early exit check unique to consumer)

4. **ProcessorRoleHost** (Layer 3 + Layer 2 for processor)
   - Implement `run_data_loop_()` matching pseudo-code §4.2 exactly
   - Do NOT use hub::Processor (it lacks messages/inbox/timing)
   - Dual-queue acquire logic with drop/block output timeout

5. **Delete old hosts** (cleanup)
