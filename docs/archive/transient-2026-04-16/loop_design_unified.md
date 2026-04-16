# Unified Loop Design — Canonical Reference

**Status**: v4 — Consolidated (2026-03-19); **API verified current 2026-04-14**
**Purpose**: Precise pseudo-code for the unified data loop, config changes,
timing policies, and abstraction layer boundaries. This document is the
correctness reference for the ScriptEngine integration.

**Verification 2026-04-14**: Spot-checked all referenced functions and method
names against the current code at baseline 1275/1275 tests:

- `compute_short_timeout`, `compute_next_deadline`, `resolve_period_us`,
  `LoopTimingPolicy::{MaxRate,FixedRate,FixedRateWithCompensation}` — all
  present in `src/include/utils/loop_timing_policy.hpp` with the documented
  signatures.
- `QueueReader::{read_acquire,read_release,last_seq,read_flexzone}`,
  `QueueWriter::{write_acquire,write_commit,write_discard,write_flexzone,
  sync_flexzone_checksum}` — all present in `src/include/utils/hub_queue.hpp`.
- No references to removed types (Messenger, ChannelHandle) anywhere in this
  doc — the design was always at the timing/queue-API layer, which is stable.

This doc remains the correctness reference for the upcoming role-host
unification (Phase 2 of `unified_role_loop.md`).

**Change log**:
- v1 (2026-03-18): Initial extraction of existing loop logic from 6 host files.
- v2 (2026-03-18): Inner retry-acquire pattern, `target_rate_hz` config.
- v3 (2026-03-19): `queue_io_wait_timeout_ratio`, processor output timing.
- v4 (2026-03-19): Unified timeout formula (one formula, no branching),
  processor input-hold strategy, `compute_next_deadline` 4-parameter version,
  corrected pseudo-code consolidated from all discussion points.
- API verification (2026-04-14): no doc changes needed; all named symbols
  still exist with documented semantics.

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

### 1.2 Queue I/O Timeout Config

The old `slot_acquire_timeout_ms` field (with its confusing `-1 = derive`
convention) is **replaced** by `queue_io_wait_timeout_ratio`.

```json
"queue_io_wait_timeout_ratio": 0.1
```

This is the fraction of the period budget allocated to each queue I/O
attempt in the inner retry-acquire loop. It controls how long the loop
waits per attempt to acquire a queue slot (read or write).

**Unified formula** (ONE formula for ALL timing policies, no branching):

```
short_timeout_us = max(period_us * queue_io_wait_timeout_ratio, kMinQueueIoTimeoutUs)
```

Where `kMinQueueIoTimeoutUs = 10` (10 μs — reasonable floor for modern
CPU/OS scheduler granularity on both Linux and Windows).

| Policy | period_us | ratio=0.1 | short_timeout_us |
|--------|----------|-----------|-----------------|
| MaxRate | 0 | 0.1 | 10 μs (floor) |
| FixedRate 10 kHz | 100 | 0.1 | 10 μs (floor) |
| FixedRate 1 kHz | 1000 | 0.1 | 100 μs |
| FixedRate 100 Hz | 10000 | 0.1 | 1000 μs = 1 ms |
| FixedRate 10 Hz | 100000 | 0.1 | 10000 μs = 10 ms |

No special cases. The floor (10 μs) handles MaxRate naturally:
`0 * ratio = 0 → clamped to 10 μs`.

Validation:
- Range: 0.1–0.5 (10%–50% of period per attempt)
- Default: 0.1 (gives ~10 retry attempts per period for FixedRate;
  gives 10 μs attempts for MaxRate)
- The old `slot_acquire_timeout_ms` field emits `LOGGER_WARN` if set

#### Design rationale: why "I/O" in the name

The timeout controls the **I/O side** of the queue — the point where the
role interacts with the data transport:

- **Producer**: `write_acquire()` — waiting for a free slot to write into.
  The producer acquires first, then the script generates data directly into
  the slot (zero-copy). This ensures (a) data freshness — no stale gap
  between generation and publish, and (b) early backpressure — if the ring
  is full, we know before wasting time on I/O.

- **Consumer**: `read_acquire()` — waiting for data to arrive from upstream.

- **Processor input**: `read_acquire()` — same as consumer.

All three are the same concern: waiting for the queue to become ready for
the next I/O operation. The ratio controls how aggressively the loop
retries within each period.

#### Processor output timeout — a different concern

The processor has a **second** acquire on the output side. This timeout
is **not** controlled by `queue_io_wait_timeout_ratio` because it represents
a different concern: **pipeline backpressure**, not I/O timing.

If the processor's output queue is full, it means the downstream consumer
can't keep up. The correct response depends on the overflow policy:

| Policy | Output timeout | Behavior |
|--------|---------------|----------|
| Drop (default) | 0 ms (non-blocking) | Immediate failure. Drops counted. No cascading delay. |
| Block | `max(remaining_until_deadline, short_timeout)` | Wait up to the remaining cycle budget. On overrun, fall back to `short_timeout` (10 μs floor for MaxRate). |

```
PROCESSOR_OUTPUT_TIMEOUT:
    if overflow_policy == Drop:
        return 0ms

    # Block mode (all timing policies — unified):
    remaining = deadline - now()
    return max(remaining, short_timeout)  # bounded by deadline, floor = short_timeout
```

---

## 2. Timing Policy

### 2.1 LoopTimingPolicy (unchanged enum)

```
enum LoopTimingPolicy { MaxRate, FixedRate, FixedRateWithCompensation }
```

### 2.2 Short Timeout Derivation

One formula, no branching on timing policy:

```
kMinQueueIoTimeoutUs = 10   # 10 μs floor

compute_short_timeout(period_us, ratio):
    return microseconds(max(period_us * ratio, kMinQueueIoTimeoutUs))
```

### 2.3 Deadline Calculation

```
compute_next_deadline(policy, prev_deadline, cycle_start, period_us):
    if policy == MaxRate:
        return time_point::max()               # no deadline

    period = microseconds(period_us)

    if policy == FixedRateWithCompensation:
        if prev_deadline == time_point::max():  # first cycle
            return cycle_start + period
        return prev_deadline + period           # advance from prev (steady rate)

    # FixedRate:
    if prev_deadline == time_point::max():      # first cycle
        return cycle_start + period
    ideal = prev_deadline + period
    if now() <= ideal:
        return ideal                            # on time — advance cleanly
    return now() + period                       # overrun — reset from now
```

Note: The function takes 4 parameters: `policy`, `prev_deadline`,
`cycle_start`, and `period_us`. For the first cycle, `prev_deadline`
is `time_point::max()` — the function detects this and uses
`cycle_start + period` to establish the initial deadline.

### 2.4 Period Definition

**Period** = time between successive script calls (start-to-start).

For all three roles, the script is called once per cycle. The period
governs when the next call should happen, measured from the start of
the current cycle (`cycle_start`).

For the **processor**, the period means the same thing: time between
successive `on_process` calls, regardless of whether input or output
was available.

---

## 3. Unified Data Loop — Producer

```
PRODUCER_DATA_LOOP(queue, engine, core, config, inbox_queue):
    # ── Setup ──
    period_us     = config.target_period_us
    is_max_rate   = (config.loop_timing == MaxRate)
    ratio         = config.queue_io_wait_timeout_ratio   # 0.1–0.5
    short_timeout = compute_short_timeout(period_us, ratio)
    buf_sz        = queue.item_size()
    deadline      = time_point::max()                    # first cycle: no deadline

    # ── Loop ──
    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        cycle_start = now()

        # ── Step A: Acquire slot with inner retry ─────────────────
        #
        # Retry acquire with short_timeout. Gives multiple chances to
        # get a slot before deadline (FixedRate), or single attempt
        # with 10μs timeout (MaxRate).
        #
        buf = nullptr
        while true:
            buf = queue.write_acquire(short_timeout)
            if buf != nullptr: break

            if is_max_rate: break    # MaxRate: one attempt

            # Check shutdown between retries
            if NOT core.running_threads OR core.shutdown_requested
               OR core.critical_error_:
                break

            # Skip remaining-time check on first cycle (deadline == max)
            if deadline != time_point::max():
                remaining = deadline - now()
                if remaining <= short_timeout: break
            # else: retry

        # ── Step B: Deadline wait ─────────────────────────────────
        #
        # FixedRate with early data: sleep until deadline for
        # predictable callback timing. Slot held during sleep.
        #
        if NOT is_max_rate AND buf != nullptr
           AND deadline != time_point::max() AND now() < deadline:
            sleep_until(deadline)

        # Shutdown check after potential sleep
        if NOT core.running_threads OR core.shutdown_requested
           OR core.critical_error_:
            if buf: queue.write_discard()
            break

        # ── Step C: Drain right before callback ───────────────────
        #
        # Freshest possible: messages + inbox drained at the last
        # moment. All messages in FIFO order, no dropping.
        #
        msgs = core.drain_messages()
        drain_inbox_sync(engine)

        # ── Step D: Invoke callback ───────────────────────────────
        #
        if buf != nullptr:
            memset(buf, 0, buf_sz)

        result = engine.invoke_produce(buf, buf_sz, flexzone, fz_sz,
                                        fz_type, msgs)

        # ── Step E: Commit/discard ────────────────────────────────
        #
        if buf != nullptr:
            if result == Commit:
                queue.write_commit()
                ++out_written
            else:
                queue.write_discard()
                ++drops
        else:
            ++drops

        if result == Error:
            ON_SCRIPT_ERROR(config, core)

        # ── Step F: Metrics ───────────────────────────────────────
        #
        work_us = duration_us(now() - cycle_start)
        store last_cycle_work_us = work_us
        ++iteration_count

        # ── Step G: Next deadline ─────────────────────────────────
        #
        deadline = compute_next_deadline(config.loop_timing,
                                          deadline, cycle_start, period_us)

    # ── Exit ──
    log exit state
```

---

## 4. Unified Data Loop — Consumer

Same structure as producer, with read semantics:

```
CONSUMER_DATA_LOOP(queue_reader, engine, core, config, inbox_queue):
    period_us     = config.target_period_us
    is_max_rate   = (config.loop_timing == MaxRate)
    ratio         = config.queue_io_wait_timeout_ratio
    short_timeout = compute_short_timeout(period_us, ratio)
    item_sz       = queue_reader.item_size()
    deadline      = time_point::max()

    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        cycle_start = now()

        # ── Step A: Acquire data with inner retry ─────────────────
        data = nullptr
        while true:
            data = queue_reader.read_acquire(short_timeout)
            if data != nullptr: break
            if is_max_rate: break
            if NOT core.running_threads OR core.shutdown_requested
               OR core.critical_error_: break
            if deadline != time_point::max():
                remaining = deadline - now()
                if remaining <= short_timeout: break

        # ── Step B: Deadline wait ─────────────────────────────────
        if NOT is_max_rate AND data != nullptr
           AND deadline != time_point::max() AND now() < deadline:
            sleep_until(deadline)

        # Shutdown check after potential sleep
        if NOT core.running_threads OR core.shutdown_requested
           OR core.critical_error_:
            if data: queue_reader.read_release()
            break

        # ── Step C: Drain right before callback ───────────────────
        msgs = core.drain_messages()
        drain_inbox_sync(engine)

        # ── Step D: Invoke callback ───────────────────────────────
        if data != nullptr:
            update last_seq = queue_reader.last_seq()
            ++in_received

        engine.invoke_consume(data, item_sz, flexzone, fz_sz, fz_type, msgs)
        # invoke_consume is void. Error detection via error count comparison.

        # ── Step E: Release slot ──────────────────────────────────
        if data != nullptr:
            queue_reader.read_release()

        # ── Step F: Metrics ───────────────────────────────────────
        work_us = duration_us(now() - cycle_start)
        store last_cycle_work_us = work_us
        ++iteration_count

        # ── Step G: Next deadline ─────────────────────────────────
        deadline = compute_next_deadline(config.loop_timing,
                                          deadline, cycle_start, period_us)

    log exit state
```

### Differences from Producer

1. `read_acquire` / `read_release` instead of `write_acquire` / `write_commit`
2. No commit/discard — slot released after callback
3. No drops counter
4. `invoke_consume` is void (error detected via error count comparison)
5. `last_seq` updated when data arrives
6. Read-only slot (`const void*`)

---

## 5. Unified Data Loop — Processor

Dual-queue loop with input-hold strategy for Block mode.

### 5.1 Queue Acquire Semantics (transport-dependent)

| | SHM | ZMQ |
|---|---|---|
| `read_acquire` | Shared lock, zero-copy, ring NOT advanced | Copy to internal buffer, ring advanced immediately |
| `read_release` | Releases lock, advances index | No-op (data already consumed) |
| Data valid until | `read_release()` called | Next `read_acquire()` called |

**Both transports support input-hold**: skip `read_acquire()` on the next
cycle and the pointer remains valid (SHM: lock held; ZMQ: `current_read_buf_`
unchanged).

### 5.2 Processor Loop

```
PROCESSOR_DATA_LOOP(in_q, out_q, engine, core, config, inbox_queue):
    period_us     = config.target_period_us
    is_max_rate   = (config.loop_timing == MaxRate)
    ratio         = config.queue_io_wait_timeout_ratio
    short_timeout = compute_short_timeout(period_us, ratio)
    drop_mode     = (config.overflow_policy == Drop)

    in_sz         = in_q.item_size()
    out_sz        = out_q.item_size()
    deadline      = time_point::max()

    # Input-hold state for Block mode (see §5.3)
    held_input    = nullptr

    while core.running_threads AND NOT core.shutdown_requested
          AND NOT core.critical_error_:

        cycle_start = now()

        # ── Step A: Acquire input ─────────────────────────────────
        #
        # If held_input is set (Block mode, previous output failed),
        # skip acquire — reuse the held data.
        #
        # Otherwise, inner retry acquire with short_timeout.
        #
        if held_input == nullptr:
            while true:
                held_input = in_q.read_acquire(short_timeout)
                if held_input != nullptr: break
                if is_max_rate: break
                if NOT core.running_threads OR core.shutdown_requested
                   OR core.critical_error_: break
                if deadline != time_point::max():
                    remaining = deadline - now()
                    if remaining <= short_timeout: break

        # ── Step B: Acquire output (only if input available) ──────
        #
        # No input → no point acquiring output. Script called with
        # both nil (messages-only cycle).
        #
        # Input available → try output with policy-dependent timeout.
        #
        out_buf = nullptr
        if held_input != nullptr:
            if drop_mode:
                out_buf = out_q.write_acquire(0ms)
            else:
                # Block mode: use remaining time until deadline.
                # On overrun or first cycle, fall back to short_timeout.
                if deadline != time_point::max():
                    remaining = deadline - now()
                    output_timeout = max(remaining, short_timeout)
                else:
                    output_timeout = short_timeout
                out_buf = out_q.write_acquire(output_timeout)

        # ── Step C: Deadline wait ─────────────────────────────────
        if NOT is_max_rate
           AND deadline != time_point::max() AND now() < deadline:
            sleep_until(deadline)

        # Shutdown check after potential sleep
        if NOT core.running_threads OR core.shutdown_requested
           OR core.critical_error_:
            if held_input: in_q.read_release(); held_input = nullptr
            if out_buf:    out_q.write_discard()
            break

        # ── Step D: Drain right before callback ───────────────────
        msgs = core.drain_messages()
        drain_inbox_sync(engine)

        # ── Step E: Invoke callback ───────────────────────────────
        if out_buf != nullptr:
            memset(out_buf, 0, out_sz)

        result = engine.invoke_process(held_input, in_sz,
                                        out_buf, out_sz,
                                        flexzone, fz_sz, fz_type, msgs)

        # ── Step F: Commit/discard output, release/hold input ─────
        #
        # Output:
        if out_buf != nullptr:
            if result == Commit:
                out_q.write_commit()
                ++out_written
            else:
                out_q.write_discard()
                ++out_drops
        elif held_input != nullptr:
            ++out_drops   # had input but no output slot

        # Input:
        if held_input != nullptr:
            if out_buf != nullptr OR drop_mode:
                # Normal: data processed or dropped. Advance input.
                in_q.read_release()
                held_input = nullptr
                ++in_received
            else:
                # Block mode + output failed: HOLD input for next cycle.
                # SHM: lock still held, pointer valid.
                # ZMQ: current_read_buf_ unchanged, pointer valid.
                # (held_input stays set → Step A skips acquire next cycle)
                pass

        if result == Error:
            ON_SCRIPT_ERROR(config, core)

        # ── Step G: Metrics + next deadline ───────────────────────
        work_us = duration_us(now() - cycle_start)
        store last_cycle_work_us = work_us
        ++iteration_count
        deadline = compute_next_deadline(config.loop_timing,
                                          deadline, cycle_start, period_us)

    log exit state
```

### 5.3 Input-Hold Strategy (Block Mode)

When the processor operates in **Block mode** and the output acquire fails
(downstream congestion), the input data is **held** across cycles:

1. `held_input` pointer remains set
2. Next cycle: Step A skips `read_acquire()` — reuses the held pointer
3. The script is called again with the **same input data** and a fresh
   output attempt
4. This continues until output becomes available, at which point the input
   is released normally

**Why this works for both transports:**
- **SHM**: `read_release()` was never called, so the shared lock is held
  and the pointer remains valid. The upstream producer cannot overwrite the slot.
- **ZMQ**: `read_acquire()` copied data into `current_read_buf_`. Since we
  skip the next `read_acquire()`, that buffer is never overwritten. The
  pointer remains valid.

**Why not dequeue and copy?** Because holding is zero-cost — no extra
allocation, no memcpy. The existing queue abstraction already supports it.

**Why hold instead of dropping?** In Block mode, the user explicitly chose
backpressure over data loss. Dropping the input would violate that contract.
The upstream should slow down naturally because the input ring stays full
(SHM: slot locked; ZMQ: ring entry consumed but no new recv completes).

### 5.4 hub::Processor (deleted 2026-03-26)

`hub::Processor` was a standalone data-loop class (`hub_processor.hpp/cpp`)
that wrapped two queues with a typed handler callback. It was 100% superseded
by `ProcessorRoleHost::run_data_loop_()` which handles all the features
`hub::Processor` lacked: control messages, inbox, timing policy, inner
retry-acquire, input-hold, and deadline management. Deleted in commit 929649b.

---

## 6. Script Callback Contract

This section defines the exact parameter combinations each callback receives,
and the expected script behavior for each. This is the contract between the
C++ framework (Layer 2) and the user script.

### 6.1 Producer: `on_produce(out_slot, fz, msgs, api)`

| `out_slot` | `msgs` | Situation | Correct action |
|------------|--------|-----------|---------------|
| buffer | empty | Normal: write slot acquired | Fill slot with data. Return `true` to commit. |
| buffer | non-empty | Slot acquired + control events arrived | Process messages first (e.g., check for `channel_closing`), then fill slot. Return `true`. |
| nil | empty | Acquire timeout — ring full or no consumer | Idle cycle. Return `false` (discard). Script can do housekeeping. |
| nil | non-empty | Timeout + control events | Process messages. Return `false`. |

**Return value** (strict — explicit return required):
- `return true` → Commit (publish the slot)
- `return false` → Discard (do not publish)
- No return / `return nil` → **Error** (script must explicitly return true or false;
  omitting return is treated as a bug — logged as WARN, counted as error, treated as discard)
- Wrong type (number, string, table) → **Error** (logged as ERROR, counted, treated as discard)

**Flexzone** (`fz`): Writable. Persists across cycles. Script can write during
`on_init` and update on any `on_produce` call. Checksum synced after `on_init`.

```lua
-- Example: producer script (Lua)
function on_produce(out_slot, fz, msgs, api)
    -- 1. Process control messages
    for _, m in ipairs(msgs) do
        if m.event == "channel_closing" then
            api.log("info", "Channel closing, finishing up")
            api.stop()
            return false
        end
    end

    -- 2. No slot available — nothing to produce
    if out_slot == nil then
        return false
    end

    -- 3. Fill the output slot
    out_slot.timestamp = api_clock()
    out_slot.value     = read_sensor()
    return true  -- commit
end
```

### 6.2 Consumer: `on_consume(in_slot, fz, msgs, api)`

| `in_slot` | `msgs` | Situation | Correct action |
|-----------|--------|-----------|---------------|
| data | empty | Normal: new data from producer | Process data. |
| data | non-empty | Data + control events | Process messages, then process data. |
| nil | empty | Read timeout — no new data | Idle cycle. Script can do periodic work. |
| nil | non-empty | Timeout + control events | Process messages. |

**No return value**. The slot is released automatically after the callback.
Errors are detected by the framework via error count comparison.

**Flexzone** (`fz`): Read-only. Points to the producer's flexzone data.

```lua
-- Example: consumer script (Lua)
function on_consume(in_slot, fz, msgs, api)
    -- 1. Process control messages
    for _, m in ipairs(msgs) do
        if m.event == "channel_closing" then
            api.log("info", "Producer shutting down")
            return
        end
    end

    -- 2. No data — idle
    if in_slot == nil then
        return
    end

    -- 3. Process incoming data
    local value = in_slot.value
    api.log("debug", string.format("Received: %.2f", value))
    store_to_database(value)
end
```

### 6.3 Processor: `on_process(in_slot, out_slot, fz, msgs, api)`

The processor has the most complex callback contract due to dual queues
and the input-hold strategy.

| `in_slot` | `out_slot` | Situation | Correct action |
|-----------|-----------|-----------|---------------|
| data | buffer | **Normal**: input + output available | Transform `in_slot` → `out_slot`. Return `true` (commit output). |
| data | nil | **Output full** (Drop mode): input available but output ring full | Cannot write. Log/count. Return `false`. Input is released (Drop mode). |
| data (held) | buffer | **Block retry**: same input re-presented after output became available | Transform same data. Return `true`. |
| data (held) | nil | **Still blocked**: output still unavailable (Block mode) | Still cannot write. Return `false`. Input stays held for next cycle. |
| nil | nil | **No input**: read timeout, no output attempted | Idle/messages-only cycle. Return `false`. |

**Key**: `in_slot=nil, out_slot=buffer` **never happens** — the framework
only acquires output when input is available (§5.2 Step B).

**Return value**: `true`/`nil` = commit output. `false` = discard output.
Same contract as producer.

**Commit/release logic** (framework, not script):
- Output: committed or discarded based on return value.
- Input:
  - If `out_slot` was available (commit or discard happened) → input released, queue advances.
  - If `drop_mode` → input always released regardless of output.
  - If Block mode + output nil → input **held** for next cycle.

**The script does NOT control input release.** It only controls output
commit/discard via the return value. The framework manages input lifecycle
based on the overflow policy and output availability.

**Flexzone** (`fz`): Writable, from the output queue. Persists across cycles.

**How the script knows it's a retry (held input)**: The data in `in_slot`
is identical to the previous call. The script can detect this by comparing
a sequence number or timestamp field in the slot, or by maintaining a
flag in its own state.

```lua
-- Example: processor script (Lua)
local last_processed_seq = -1

function on_process(in_slot, out_slot, fz, msgs, api)
    -- 1. Process control messages
    for _, m in ipairs(msgs) do
        if m.event == "channel_closing" then
            api.log("info", "Pipeline shutting down")
            api.stop()
            return false
        end
    end

    -- 2. No input — idle cycle
    if in_slot == nil then
        return false
    end

    -- 3. No output slot — blocked or dropped
    if out_slot == nil then
        api.log("warn", "Output full, data held for retry")
        return false
    end

    -- 4. Check if this is a retry (same data re-presented)
    if in_slot.seq == last_processed_seq then
        api.log("debug", "Retry: re-processing held data")
    end
    last_processed_seq = in_slot.seq

    -- 5. Transform input → output
    out_slot.timestamp = in_slot.timestamp
    out_slot.value     = in_slot.value * calibration_factor
    return true  -- commit output
end
```

### 6.4 Inbox: `on_inbox(slot, sender, api)`

Called synchronously during Step C drain. All inbox messages are presented
in FIFO order, one at a time.

| `slot` | `sender` | Situation |
|--------|----------|-----------|
| typed data | sender UID | Normal: typed inbox message |
| raw bytes | sender UID | Schemaless inbox message |

No return value. No ack control — framework always acks success.

---

## 7. Inbox Drain — Unified Design

```
DRAIN_INBOX_SYNC(inbox_queue, engine):
    if inbox_queue == nullptr: return

    while true:
        item = inbox_queue.recv_one(0ms)         # non-blocking
        if item == nullptr: break                 # no more messages

        engine.invoke_on_inbox(item.data, item.size,
                                inbox_type_name, item.sender_id)

        inbox_queue.send_ack(0)                   # always ack success
```

All messages preserved in FIFO order. No dropping. Drained in Step C
(right before callback) for maximum freshness.

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
| Wrong return type | L1 (engine) | Engine logs → `InvokeResult::Error` | Same |
| `api.set_critical_error()` | L1→L0 | `core.set_critical_error()` | Outer loop exits |
| `stop_on_script_error` | L2 (loop) | `core.shutdown_requested = true` | Outer loop exits |
| Peer dead | L3 (host) | `core.stop_reason_ = PeerDead; core.shutdown_requested = true` | Outer loop exits |
| Hub dead | L3 (host) | `core.stop_reason_ = HubDead; core.shutdown_requested = true` | Outer loop exits |
| Queue nullptr | L2 (loop) | Early return before loop | Startup failure |
| Config invalid | L3 (host) | Throws during setup | Startup failure |

### 8.2 ON_SCRIPT_ERROR

```
ON_SCRIPT_ERROR(config, core):
    # script_errors already incremented by engine
    if config.stop_on_script_error:
        core.shutdown_requested = true
```

For `invoke_consume` (void return): detect via error count comparison
(before/after invoke).

### 8.3 Shutdown Responsiveness

Checked at:
1. Outer loop condition (every cycle)
2. Inner retry loop (every `short_timeout`)
3. After deadline sleep (safety check)

Worst-case latency: `short_timeout`.
- MaxRate: 10 μs (practically instant)
- FixedRate 100 Hz (period=10ms, ratio=0.1): 1 ms
- FixedRate 10 Hz (period=100ms, ratio=0.1): 10 ms

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
│                                          │  timing, metrics, input-hold
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

---

## 10. Implementation Status

### Done
- ✅ `compute_short_timeout(period_us, ratio)` — unified formula, 10 μs floor
- ✅ `compute_next_deadline(policy, prev_deadline, cycle_start, period_us)` — 4 parameters
- ✅ `kMinQueueIoTimeoutUs`, `kDefaultQueueIoWaitRatio`, `kMinQueueIoWaitRatio`, `kMaxQueueIoWaitRatio`
- ✅ `PYLABHUB_MAX_LOOP_RATE_HZ` CMake option
- ✅ `resolve_period_us()` for rate_hz/period_ms unification
- ✅ `ProducerRoleHost` + `LuaEngine` (wired, tested)
- ✅ `ConsumerRoleHost` + `LuaEngine` (wired, tested)
- ✅ `ProcessorRoleHost` + `LuaEngine` (wired, tested, input-hold §5.2 implemented)
- ✅ `drain_inbox_sync()`, `wait_for_roles()` — shared helpers
- ✅ `queue_io_wait_timeout_ratio` in all 3 config structs, parsed, validated (0.1–0.5)
- ✅ `target_rate_hz` (double) in all 3 config structs, parsed, mutual exclusion validated
- ✅ `target_period_ms` changed from `int` to `double`
- ✅ Config ratio wired into `compute_short_timeout()` in all 3 role hosts
- ✅ Consumer `last_seq` tracking (stored as atomic, updated from `queue_reader_->last_seq()`)
- ✅ `slot_acquire_timeout_ms` deprecated with LOGGER_WARN
- ✅ Config tests updated: `Validation_QueueIoWaitRatioOutOfRange` replaces old timeout test
- ✅ 1220/1220 tests pass

### Remaining
- [ ] `PythonEngine` implementation
- [ ] Delete old hosts (LuaProducerHost, LuaConsumerHost, LuaProcessorHost,
      LuaRoleHostBase, ProducerScriptHost, ConsumerScriptHost,
      ProcessorScriptHost, PythonRoleHostBase, PythonScriptHost)
- [ ] `validate_only` mode layout printing
- [ ] Update HEP-0011 with ScriptEngine architecture
- [ ] Timing loop tests (config interpretation + runtime behavior)

### Notes on doc vs code
- Code uses `config_.target_period_ms * kUsPerMs` for period derivation.
  Doc says `config.target_period_us` (a pre-computed field). Both are correct;
  `target_period_us` as a pre-computed config field is a future cleanup.
- Code checks `core_.g_shutdown` (external signal handler flag) in the outer
  loop and inner retry. The doc omits these checks for clarity but the code
  is correct to include them.
- Processor post-loop cleanup releases any `held_input` — not shown in §5
  pseudo-code but present in code. Should be added to pseudo-code.
