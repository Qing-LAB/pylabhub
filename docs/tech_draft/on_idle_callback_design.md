# Tech Draft: `on_idle` Callback — Separating Control from Data Plane

**Status**: Draft for discussion
**Date**: 2026-03-13
**Related**: HEP-0011 (ScriptHost), HEP-0017 (Pipeline), HEP-0018 (Producer/Consumer)

## Problem

All three role script hosts (Producer, Consumer, Processor) conflate control-message
handling with data-plane slot operations. When a role is idle (waiting for a
broadcast, no input data, or output queue full), the current design either:

1. **Acquires and discards a slot** just to deliver messages (Producer, Processor timeout path)
2. **Calls the data callback with `None`** forcing scripts to guard against missing slots

This creates several issues:

- **Wasted slot operations**: Producer acquires+discards write slots while suspended,
  burning `write_index` under Sequential policy (root cause of the `fetch_sub` bug
  fixed 2026-03-13).
- **Muddled script logic**: `on_produce` must handle both "produce data" and "check
  for start broadcast" in the same callback, with `if out_slot is None` guards.
- **Unpredictable startup timing**: The E2E test relies on broadcast synchronization,
  but the producer is spinning on slot acquire/discard before the "start" signal
  arrives. There is no clean way for the script to say "I'm not ready to produce yet."

## Current Loop Structures

### Producer (today)
```
while running:
    buf = write_acquire(timeout)          # ALWAYS acquires
    msgs = drain_messages()
    if !buf:
        on_produce(None, fz, msgs)        # called with None slot
        continue
    commit = on_produce(slot, fz, msgs)
    commit ? write_commit() : write_discard()  # discard burns slot_id
```

### Consumer (today)
```
while running:
    data = read_acquire(timeout)          # blocks for input
    msgs = drain_messages()
    if !data:
        on_consume(None, fz, msgs)        # called with None slot
        continue
    on_consume(slot, fz, msgs)
    read_release()
```

### Processor (today — two-layer)
```
# hub::Processor C++ loop:
while running:
    in_data = read_acquire(input_timeout)
    if !in_data:
        out = write_acquire(out_timeout)  # acquires OUTPUT slot on timeout!
        commit = timeout_handler(out, fz)
        commit ? write_commit() : write_discard()
        continue
    out = write_acquire(out_timeout)
    commit = handler(in, fz, out, fz)
    ...

# ProcessorScriptHost timeout_handler:
timeout_handler(out_data, out_fz):
    msgs = drain_messages()
    on_process(None_in, out_slot, fz, msgs)   # input is None, output slot acquired
```

## Proposed Design: `on_idle` Callback

### Core Idea

Split each role's script interface into two callbacks:

- **`on_produce` / `on_consume` / `on_process`**: Called **only with valid slot(s)**.
  The script never sees `None` for its primary data parameter.
- **`on_idle(messages, api)`**: Called when no slot is available (timeout, queue full,
  or role not yet ready). Handles control messages, state transitions, and
  coordination — **no slot operations involved**.

### Proposed Loop Structures

#### Producer
```
while running:
    msgs = drain_messages()

    if msgs OR idle_interval_due:
        ready = on_idle(msgs, api)        # returns True/False

    if not ready:
        continue                          # skip slot acquire entirely

    buf = write_acquire(timeout)
    if !buf:
        continue                          # timeout, will call on_idle next iter

    commit = on_produce(slot, fz, api)    # always valid slot
    commit ? write_commit() : write_discard()
```

#### Consumer
```
while running:
    data = read_acquire(timeout)
    msgs = drain_messages()

    if !data:
        on_idle(msgs, api)                # no slot ops
        continue

    on_consume(slot, fz, msgs, api)       # always valid slot
    read_release()
```

#### Processor
```
while running:
    in_data = read_acquire(input_timeout)
    msgs = drain_messages()

    if !in_data:
        on_idle(msgs, api)                # no output slot acquired
        continue

    out = write_acquire(out_timeout)
    if !out: drops++; read_release(); continue

    commit = on_process(in_slot, out_slot, fz, api)  # both valid
    commit ? write_commit() : write_discard()
    read_release()
```

### `on_idle` Return Value (Producer only)

For Producer, `on_idle` can optionally **gate data production** via its return value:

| Return | Meaning |
|--------|---------|
| `True` (default) | Proceed to `write_acquire` + `on_produce` |
| `False` | Skip this iteration — stay idle |

This makes the "suspended until broadcast" pattern trivial:

```python
_ready = False

def on_idle(messages, api):
    global _ready
    for m in messages:
        if m.get('message') == 'start':
            _ready = True
    return _ready  # don't acquire slots until "start" received

def on_produce(out_slot, flexzone, api):
    # Always called with a valid slot — no None checks needed
    out_slot.counter = _count
    return True
```

For Consumer and Processor, `on_idle` is void — there is no slot to gate.

### Backward Compatibility

- If `on_idle` is not defined in the script, the framework falls back to the
  current behavior: calling `on_produce(None, ...)` / `on_consume(None, ...)`.
- If `on_idle` is defined, `on_produce`/`on_consume`/`on_process` are **never**
  called with `None` — the framework guarantees valid slots.
- Detection: `hasattr(module, 'on_idle')` at script load time.

### Processor: Timeout-Produce Pattern

The current Processor timeout handler can produce output without input (e.g.,
heartbeat injection). Under the new design:

- The common case (just handle messages) uses `on_idle`.
- If a script needs timeout-produce, it opts in via a separate callback
  `on_timeout_produce(out_slot, fz, api)` — called with an acquired output slot
  when input times out. This is the rare case and should be explicitly requested.
- If neither `on_idle` nor `on_timeout_produce` is defined, the processor simply
  retries (current no-timeout-handler behavior).

### Messages Parameter

With `on_idle`, messages are delivered to whichever callback fires:
- Idle iteration → messages go to `on_idle`
- Data iteration → messages go to `on_produce` / `on_consume` / `on_process`

Messages are **never lost** — `drain_messages()` is called every iteration regardless.

## Benefits

1. **No wasted slot operations**: Idle roles don't touch the data plane at all.
2. **Cleaner scripts**: Data callbacks always receive valid slots. Control logic
   lives in `on_idle`.
3. **Natural startup synchronization**: `on_idle` returns `False` until the role
   is ready, preventing premature slot acquisition.
4. **Eliminates the write_index bug class**: If `on_produce` is only called with
   valid slots and always commits, `write_discard` is reserved for genuine "drop
   this data" decisions, not idle-loop artifacts.

## Open Questions

1. **Should `on_idle` receive a `messages` list, or should messages be delivered
   separately via `on_message(msg, api)`?** Per-message callbacks would be cleaner
   but change the callback model significantly.

2. **FixedRate + idle**: When `on_idle` returns `False` (producer not ready), should
   the loop still respect `target_period_ms` for idle calls, or free-run until ready?
   Proposed: free-run with a floor (e.g., 10ms sleep) to avoid busy-wait.

3. **Processor output-on-timeout**: Is the timeout-produce pattern used in practice,
   or is it speculative? If unused, we can simplify by removing it entirely and
   only having `on_idle`.

4. **E2E test timing combinations**: With `on_idle` gating, the E2E test can
   deterministically control when each role enters data mode. Should we also add
   tests for:
   - Late-join consumer (producer already producing)
   - Slow consumer causing backpressure (Sequential policy)
   - Producer/processor startup order reversal
