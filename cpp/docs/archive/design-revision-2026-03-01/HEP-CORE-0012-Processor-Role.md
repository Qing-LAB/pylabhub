# HEP-CORE-0012: Processor Role

| Property      | Value                                                                  |
|---------------|------------------------------------------------------------------------|
| **HEP**       | `HEP-CORE-0012`                                                        |
| **Title**     | Processor Role — Data Transformation in the Pipeline                   |
| **Status**    | Phase 1 Implemented — 2026-03-01                                       |
| **Created**   | 2026-02-28                                                             |
| **Area**      | Actor Framework (`pylabhub-actor`)                                     |
| **Depends on**| HEP-CORE-0007 (DataHub Protocol), HEP-CORE-0010 (Actor Thread Model)  |

---

> **Phase 1 implemented (2026-03-01)**. Files changed: `actor_config.hpp/cpp`,
> `processor_role_api.hpp` (new), `actor_host.hpp/cpp`, `actor_role_workers.cpp`,
> `actor_module.cpp`. Test count: **595/595** (+8 `ActorConfigProcessor` config tests).
> Phases 2+ (multi-broker, Lua, input flexzone) remain future work.

---

## 1. Motivation

The current actor framework provides two role kinds:

| Kind | Direction | Channel ownership |
|------|-----------|-------------------|
| `producer` | write-only | creates SHM, registers with broker as writer |
| `consumer` | read-only  | attaches to SHM, registers with broker as reader |

A common pattern that cannot be expressed cleanly with these two kinds is **in-line data transformation**: read from one channel, transform, and write to another. Today this requires a single actor with one `producer` role and one `consumer` role operating independently with their loops decoupled. This works but:

- The two loops are not temporally coupled (consumer fires on input; producer fires on its own `interval_ms` timer).
- Coordinating the handoff requires shared state protected by Python locks.
- Overflow and back-pressure semantics must be implemented manually in Python.
- The intent ("transform A → B") is not expressed in configuration.

The **Processor** role provides a single, intent-clear role that owns exactly one input channel and one output channel, with a single callback (`on_process`) invoked for each input slot.

---

## 2. Topology Contract

```
Producer  →  [channel-A]  →  Processor  →  [channel-B]  →  Consumer
                                ↑ single input, single output
```

**Invariants:**

1. **One writer per channel** — a Processor's output channel follows the same single-writer rule as a Producer's channel. The broker enforces this at `REG_REQ` time.
2. **Multiple readers per channel** — a Processor's input channel can have zero or more co-readers (other Consumers or other Processors reading the same input).
3. **Chain always starts with a Producer, ends with a Consumer.** A channel cannot have a Processor as its only writer unless there is also a Producer upstream that originally created it. In other words, the root of any pipeline is always a `producer` role.
4. **Processor chains are valid.** `Producer → P1 → P2 → P3 → Consumer` is a legal topology. Each `P_i` reads from the channel written by `P_{i-1}` (or the root Producer).

---

## 3. Broker Protocol

**No broker protocol changes are required.** The broker already handles the two registration types independently:

| Processor side  | Existing broker message | Effect                        |
|-----------------|-------------------------|-------------------------------|
| Output channel  | `REG_REQ`               | Broker creates/tracks channel; enforces single-writer |
| Input channel   | `CONSUMER_REG_REQ`      | Broker tracks this reader; increments `consumer_count` |

From the broker's point of view, a Processor is simultaneously:
- A **producer** on its output channel (sends `REG_REQ`, keeps heartbeats, owns SHM).
- A **consumer** on its input channel (sends `CONSUMER_REG_REQ`, receives `DISC_ACK` with SHM path).

The actor host sends both registrations at startup using the existing `hub::Producer` and `hub::Consumer` APIs for output and input respectively, as it already does for separate producer/consumer roles.

---

## 4. Thread Model

One `ProcessorRoleWorker` uses **two threads**, consistent with HEP-CORE-0010 Phase 2:

```
zmq_thread_   → handles all ZMQ sockets for BOTH input and output channels
                 (CONSUMER_REG broker handshake, Producer REG handshake,
                  HEARTBEAT_REQ, HELLO/BYE, incoming Messenger messages)

loop_thread_  → blocks on input SHM acquire_consume_slot()
                 → calls on_process(in_slot, out_slot, ...)
                 → if True/None: acquire_write_slot() + commit to output SHM
                 → if False: skip output; release input slot
```

The `zmq_thread_` manages all socket I/O via `start_embedded()` + `handle_*_events_nowait()` on both the internal `hub::Producer` and `hub::Consumer` objects, polling them in a shared `zmq_poll` loop (same pattern as existing role workers).

The `loop_thread_` holds the GIL only during the `on_process` call (same invariant as existing workers).

**Thread count per Processor role:** 2 threads (+ the shared Messenger worker thread).

---

## 5. Overflow Policy

When the output channel's ring buffer is full and `on_process` returns True, the Processor must decide what to do. This is configurable via `"overflow_policy"` in the role config:

| Policy | Behaviour | Use case |
|--------|-----------|----------|
| `"block"` (default) | `acquire_write_slot()` blocks until space is available. Applies back-pressure all the way to the input. | Lossless pipelines where all data must be processed |
| `"drop"` | Skip output emission for this input slot; increment `out_drop_count` metric. | High-rate pipelines where freshness > completeness |

The policy is intentionally binary for Phase 1. Additional policies (e.g. `"drop_oldest"`, adaptive) can be added later without breaking the config schema.

**Metric tracking** (always, regardless of policy):
- `in_slots_received` — slots acquired from input channel
- `out_slots_written` — output slots committed
- `out_drops` — output slots discarded due to overflow
- Standard loop metrics from HEP-CORE-0008: `loop_overrun_count`, `last_cycle_work_us`

---

## 6. Config Schema

A Processor role is declared in `actor.json["roles"]` with `"kind": "processor"`:

```json
{
  "kind": "processor",

  "in_channel":  "lab.raw.temperature",
  "out_channel": "lab.processed.temperature",

  "broker":          "tcp://127.0.0.1:5570",
  "broker_pubkey":   "",

  "overflow_policy": "block",
  "timeout_ms":      -1,

  "in_slot_schema": {
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },
  "out_slot_schema": {
    "fields": [
      {"name": "ts",        "type": "float64"},
      {"name": "value_out", "type": "float64"}
    ]
  },

  "in_shm":  {"enabled": true, "secret": 0},
  "out_shm": {"enabled": true, "slot_count": 8, "secret": 0},

  "in_flexzone_schema":  {"fields": []},
  "out_flexzone_schema": {"fields": []},

  "loop_timing": "fixed_pace",
  "validation":  {"on_python_error": "stop"},

  "script": {"module": "script", "path": "./roles/my_processor"}
}
```

**Field summary** (differences from producer/consumer):

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `kind` | yes | — | `"processor"` |
| `in_channel` | yes | — | Input channel name (this Processor reads from it) |
| `out_channel` | yes | — | Output channel name (this Processor writes to it) |
| `broker` | no | `tcp://127.0.0.1:5570` | Broker for both channels. Overridden by `hub_dir`. |
| `broker_pubkey` | no | `""` | CurveZMQ server key (Z85). Overridden by `hub_dir`. |
| `overflow_policy` | no | `"block"` | What to do when output ring is full: `"block"` or `"drop"` |
| `timeout_ms` | no | `-1` | `-1`=wait indefinitely for input; `N>0`=fire `on_process(None, ...)` after silence |
| `in_slot_schema` | yes | — | ctypes field list for input slot (same syntax as consumer `slot_schema`) |
| `out_slot_schema` | yes | — | ctypes field list for output slot (same syntax as producer `slot_schema`) |
| `in_shm` | no | `{enabled:true}` | Input SHM discovery parameters |
| `out_shm` | yes | — | Output SHM creation parameters (requires `slot_count`) |
| `in_flexzone_schema` | no | absent | Flexzone for input channel (read-only view) |
| `out_flexzone_schema` | no | absent | Flexzone for output channel (writable) |
| `loop_timing` | no | `"fixed_pace"` | Deadline advancement when `timeout_ms` fires (same as consumer) |
| `validation` | no | defaults | Error policies (same keys as producer/consumer) |
| `script` | yes* | — | Python package for callbacks. Falls back to actor-level `"script"`. |

> **Note on dual-broker:** Phase 1 uses a single `broker` / `broker_pubkey` pair for both
> channels. If input and output channels live on different hubs, use `in_broker` /
> `out_broker` / `in_broker_pubkey` / `out_broker_pubkey` override fields (Phase 2).

---

## 7. Python Callback Interface

The script package at `roles/<role>/script/__init__.py` implements:

```python
def on_init(api) -> None:
    """Called once before the loop starts."""

def on_process(in_slot, out_slot, flexzone, messages, api) -> bool:
    """
    Called for each input slot received.

    in_slot:   read-only ctypes.LittleEndianStructure (input schema), or None on timeout.
    out_slot:  writable ctypes.LittleEndianStructure (output schema).
    flexzone:  persistent ctypes struct for this Processor's flexzone (output flexzone), or None.
    messages:  list of (sender: str, data: bytes) ZMQ messages from this role's Messenger.
    api:       ProcessorRoleAPI — same API surface as ActorRoleAPI plus processor-specific getters.

    Return True or None  → commit out_slot to output channel.
    Return False         → discard out_slot; no output emitted for this input.

    If in_slot is None (timeout): return value still controls out_slot emission
    (e.g. return a heartbeat slot, or return False to emit nothing).
    """
    if in_slot is None:
        return False
    out_slot.value_out = float(in_slot.value) * 2.0
    out_slot.ts        = in_slot.ts
    return True

def on_stop(api) -> None:
    """Called once after the loop exits."""
```

### ProcessorRoleAPI additions (beyond ActorRoleAPI)

```python
api.in_channel()       # → str: input channel name
api.out_channel()      # → str: output channel name
api.in_slots_received()  # → int: total input slots consumed
api.out_slots_written()  # → int: total output slots committed
api.out_drop_count()   # → int: output slots discarded due to overflow
```

All other `ActorRoleAPI` methods (`log`, `send`, `broadcast`, `role_name`, `uid`,
`actor_name`, `metrics`, `set_critical_error`, etc.) are inherited unchanged.

---

## 8. C++ Implementation Plan

### New types

| File | What |
|------|------|
| `src/actor/actor_config.hpp` | Add `RoleConfig::Kind::Processor`; add `in_channel`, `out_channel`, `overflow_policy`, `in_slot_schema`, `out_slot_schema`, `in_shm`, `out_shm`, `in_flexzone_schema`, `out_flexzone_schema` fields to `RoleConfig` |
| `src/actor/actor_config.cpp` | Parse `"processor"` kind; validate `in_channel ≠ out_channel`; validate `out_shm.slot_count > 0` |
| `src/actor/processor_role_worker.hpp/.cpp` | `ProcessorRoleWorker` class (mirrors structure of `ProducerRoleWorker` / `ConsumerRoleWorker`) |
| `src/actor/processor_role_api.hpp` | `ProcessorRoleAPI` extending `ActorRoleAPI` with `in_channel()`, `out_channel()`, `out_drop_count()`, `in_slots_received()`, `out_slots_written()` |
| `src/actor/actor_role_workers.cpp` | Wire `ProcessorRoleWorker` instantiation in `ActorHost::start_roles()` |

### `ProcessorRoleWorker` internal structure

```
hub::Producer out_producer_  ← owns output SHM + broker registration
hub::Consumer in_consumer_   ← reads input SHM + CONSUMER_REG

std::thread   zmq_thread_    ← zmq_poll over both producers' and consumer's sockets
std::thread   loop_thread_   ← acquire_consume_slot → on_process → acquire_write_slot + commit

std::atomic<bool>  stop_
std::atomic<uint64_t>  in_slots_received_
std::atomic<uint64_t>  out_slots_written_
std::atomic<uint64_t>  out_drops_
```

The `zmq_thread_` polls:
1. `out_producer_.peer_ctrl_socket_handle()` — ROUTER (HELLO/BYE from downstream consumers)
2. `in_consumer_.ctrl_zmq_socket_handle()` — DEALER (CONSUMER_REG acks, DISC_ACK)
3. Messenger's internal socket (incoming ZMQ messages for this role's Messenger)

This is identical to the existing role workers, which call `handle_peer_events_nowait()` and `handle_ctrl_events_nowait()` in their `zmq_thread_` loop.

### Python bindings

`ProcessorRoleAPI` is registered as a pybind11 embedded module class with the same pattern as `ActorRoleAPI`. The `on_process` callback is looked up by name in the loaded module and stored as `py::object on_process_fn_`.

---

## 9. Phased Delivery

### Phase 1 (initial implementation)

- Config: `kind = "processor"`, `in_channel`, `out_channel`, `broker`, `overflow_policy`, `in_slot_schema`, `out_slot_schema`, `in_shm`, `out_shm`
- Thread model: `zmq_thread_` + `loop_thread_`; single-broker; SHM-only transport
- Callbacks: `on_init`, `on_process`, `on_stop`
- Metrics: `in_slots_received`, `out_slots_written`, `out_drops`
- No per-input flexzone (out flexzone only)
- Broker protocol: unchanged (uses existing `REG_REQ` + `CONSUMER_REG_REQ`)

### Phase 2 — Dual-Broker, Transport Selection, Cross-Machine Bridge

#### 9.2.1 Transport Selection

Phase 1 uses SHM-only transport on a single broker. Phase 2 adds explicit
transport selection per channel side, and support for two independent brokers.

**Config additions to `processor.json` (or `actor.json` role entry):**

```json
{
  "in_broker":        "tcp://sensor-hub:5570",
  "in_broker_pubkey": "...",
  "out_broker":       "tcp://analysis-hub:5570",
  "out_broker_pubkey": "...",

  "in_transport":  "shm",
  "out_transport": "zmq"
}
```

| Field | Default | Description |
|-------|---------|-------------|
| `in_broker` | same as `broker` | Broker managing the input channel |
| `out_broker` | same as `broker` | Broker managing the output channel |
| `in_transport` | `"shm"` | Transport for input: `"shm"` or `"zmq"` |
| `out_transport` | `"shm"` | Transport for output: `"shm"` or `"zmq"` |

**Transport selection rule — explicit, fail-fast:**

Transport is **not** inferred automatically. The user declares what they expect.
The broker, in `DISC_ACK` / `REG_ACK`, advertises what is available for each
channel (SHM path, ZMQ endpoint, or both). The Processor validates at startup:

```
Requested "shm"  but broker only offers ZMQ endpoint → hard fail:
  "in_transport=shm requested but channel 'lab.raw.temp' on tcp://sensor-hub:5570
   has no SHM path — writer is on a remote host; use in_transport=zmq"

Requested "zmq"  but broker only offers SHM path → hard fail:
  "in_transport=zmq requested but channel 'lab.raw.temp' has no ZMQ endpoint —
   writer has not published a ZMQ PUSH socket; add out_zmq_endpoint to producer config"
```

**Key constraint**: `"shm"` requires the Processor and the channel writer to be on
the **same host**. The broker location is irrelevant — the broker is always a
coordinator, never a data relay. A remote broker can serve a local SHM channel
(the `DISC_ACK` carries the SHM path which the Processor opens locally).

#### 9.2.2 Queue as Data Plane

`ProcessorRoleWorker` uses `hub::Queue` as its data plane (→ HEP-CORE-0002 §17.3).
After the broker handshake completes, the worker builds the appropriate Queue type
from what the broker advertised and what was configured:

```cpp
// Input side — built after DISC_ACK
if (cfg.in_transport == Transport::Shm && disc_ack.has_shm_path())
    in_q_ = ShmQueue::from_consumer(std::move(in_consumer_dbc), item_sz, fz_sz);
else if (cfg.in_transport == Transport::Zmq && disc_ack.has_zmq_endpoint())
    in_q_ = ZmqQueue::pull_from(disc_ack.zmq_endpoint, item_sz);
else
    throw StartupError("transport mismatch — see reason above");

// Output side — built after REG_ACK (same pattern)
```

The `loop_thread_` operates exclusively on `Queue` interface:
`in_q_->read_acquire()` / `out_q_->write_acquire()` / commit / release.
It has no knowledge of whether the underlying transport is SHM or ZMQ.

The control plane (ZMQ sockets: peer ROUTER, consumer DEALER, Messenger) is
**independent of** the data Queue and unchanged regardless of transport choice.

#### 9.2.3 Dual-Broker Startup Protocol

When `in_broker` ≠ `out_broker`, the `zmq_thread_` sends two registrations
independently — one to each broker. Both must succeed before the `loop_thread_`
is released:

```
zmq_thread_ startup:
  ├─ REG_REQ          → out_broker   (Processor claims output channel)
  └─ CONSUMER_REG_REQ → in_broker    (Processor registers as input reader)

Wait for both ACKs (in parallel — neither depends on the other):
  ├─ REG_ACK          ← out_broker   (output SHM path or ZMQ endpoint)
  └─ DISC_ACK         ← in_broker    (input SHM path or ZMQ endpoint)

Rollback on partial failure:
  REG_ACK timeout/NACK  → abort (nothing registered on in_broker yet)
  DISC_ACK timeout/NACK → send DEREG_REQ to out_broker, then abort

Only when both ACKs received:
  → build in_q_ and out_q_ (Queue type per transport config)
  → validate transport vs broker-advertised availability
  → send HELLO to upstream Producer (input ctrl socket)
  → release loop_thread_
```

**Confirmation back to both brokers is implicit**: the Processor starts sending
`HEARTBEAT_REQ` to both brokers after startup. Both brokers will detect Processor
death via heartbeat timeout using the existing Cat 1/Cat 2 health mechanism
(HEP-CORE-0007). No explicit "I am ready" message is needed.

#### 9.2.4 Cross-Machine Bridge Pattern

A Processor with `in_transport=shm` and `out_transport=zmq` (or vice versa) acts
as a **bridge** between SHM-local and ZMQ-networked data paths:

```
Machine A:  hub::Producer → ShmQueue → Processor(bridge) → ZmqQueue ──[net]──►
Machine B:                            ◄──[net]── ZmqQueue → Processor(bridge) → ShmQueue → hub::Consumer
```

The bridge Processor typically uses a pass-through handler:

```python
def on_process(in_slot, out_slot, flexzone, messages, api):
    # Pass-through: copy all fields from in_slot to out_slot
    # (or use a zero-copy no-op if schemas are identical)
    out_slot.ts    = in_slot.ts
    out_slot.value = in_slot.value
    return True
```

Or in pure C++ (no Python overhead):

```cpp
proc.set_process_handler<void, SensorSlot, void, SensorSlot>(
    [](ProcessorContext<void, SensorSlot, void, SensorSlot>& ctx) {
        ctx.output() = ctx.input();  // struct copy
        return true;
    });
```

`hub::Producer` and `hub::Consumer` at both ends remain SHM-local and
SHM-specific — they are not modified for cross-machine support.

#### 9.2.5 Remaining Phase 2 Items

- In-flexzone: read-only view of input channel flexzone passed as second argument
  to `on_process(in_slot, out_slot, in_flexzone, out_flexzone, messages, api)`
- Additional overflow policies: `"drop_oldest"`, rate-limited sampling
- `api.in_latency_us()` — time from slot available to `on_process` invocation
- Schema integration: `in_schema` / `out_schema` ID fields in config (→ HEP-CORE-0016)

---

## 10. Open Questions

| # | Question | Decision |
|---|----------|----------|
| 1 | Should `on_process` receive both in-flexzone and out-flexzone? | Phase 2: add `in_flexzone` as a second flexzone argument |
| 2 | What happens if output channel doesn't exist yet (Processor starts before upstream Producer)? | Same as Consumer: block on `DISC_ACK`; upstream Producer must start first |
| 3 | Can a Processor write to a channel that already has a Producer? | No — enforced by broker single-writer rule at `REG_REQ` |
| 4 | Should `timeout_ms` fire `on_process(None, out_slot, ...)` or have its own callback? | Use `on_process(None, ...)` for simplicity (matches Consumer's `on_iteration(None, ...)` pattern) |
| 5 | Should `overflow_policy="block"` impose back-pressure on input? | Yes — naturally: `loop_thread_` blocks in `acquire_write_slot()`; does not re-acquire from input until output space is available |
