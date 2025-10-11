# Hub Design and Architecture

The Hub module is the **core coordination point** for pyLabHub, responsible for orchestrating communication, ensuring data integrity, and enabling extensibility. It is a critical component of the pyLabHub framework, which focuses on laboratory data acquisition, hardware control, and experiment management.

---

## Context and Goals

The Hub acts as a communication, orchestration, and gatekeeping layer to:

- Provide controlled ways to instruct hardware during experiments through customized scripts or protocols for real-time actuation or processing, with these instructions recorded alongside raw data.
- Isolate direct manipulation of source data between experiment hardware, software clients, and data services.
- Define how messages are exchanged between producers (e.g., adapters, instruments) and consumers (e.g., services, persistence, analysis), with explicit rules for **queueing, acknowledgments, and message structure**.

The design is deliberately narrow: the Hub is not a general framework but the minimal component that guarantees consistency, determinism, and extensibility.

### Primary Goals

1. **Deterministic Behavior**: Ensure bounded queues with blocking puts, explicit acknowledgments, and ordered delivery.
2. **Channel Separation**: Provide dedicated bounded queues for control, events, and data, each with distinct priorities.
3. **Extensibility**: Enable registration of adapters and services, ensuring they receive messages synchronously.
4. **Persistence Hooks**: Allow services to record events and state to Parquet, and data streams to Zarr.
5. **Future-Proofing**: Stub shared-memory APIs for later high-speed transport extensions.

---

## Messaging Model

The Hub formalizes the separation of information types by exposing **three channels** with different queueing semantics and delivery priorities:

1. **Control Channel**: Priority channel for commands and acknowledgments. Queue size = 128. Blocking put on full.
2. **Events Channel**: For logs, diagnostics, and state reports. Queue size = 512. Blocking put on full.
3. **Data Channel**: For high-volume data streams (e.g., arrays, waveforms). Queue size = 1024. Blocking put on full.

### Message Envelope

All messages share a standard envelope:

```json
{
  "header": {
    "msg_id": "uuid",
    "trace_id": "uuid",
    "hub_id": "hub-1",
    "kind": "command | ack | event | state | data",
    "channel": "control | events | data",
    "schema": "pylabhub/...@version",
    "content-type": "application/json | application/octet-stream | application/x-numpy",
    "ts": {
      "wall_clock": 1737763335.123,
      "monotonic": 83291.44
    }
  },
  "buffer": "<binary payload, optional>"
}
```

The Hub automatically stamps missing fields:

- `msg_id` (unique per message)
- `trace_id` (defaults to `msg_id`)
- `ts.wall_clock` and `ts.monotonic`

---

## Implementation Details

### Control Channel

- Commands are sent with `send_control(header, payload)`.
- Each command has a unique `msg_id`.
- The Hub registers a **waiter** keyed by `msg_id`.
- A handler must call `post_ack(Ack)` with the same `msg_id`.
- `await_ack(msg_id)` resolves deterministically to that Ack.

Ack example:

```json
{
  "msg_id": "uuid",
  "ok": true,
  "code": "ok",
  "message": "",
  "data": {}
}
```

### Events Channel

Two message kinds:

- **Events**: `emit_event(name, payload)` → `kind="event"`, schema default `pylabhub/events.log@1`
- **State**: `emit_state(scope, fields)` → `kind="state"`, schema default `pylabhub/state.instrument@1`

Events and state are typically persisted to **Parquet** by a service.

### Data Channel

- Declare streams with `create_stream(name, StreamSpec)` (dtype, shape, encoding).
- Append data with `send_data(stream, header, buffer)` or `append(stream, shard)`.
- Buffers are blocking on full queue, ensuring no silent loss.
- Each message should specify `dtype` and `shape` in its header.

Data is typically persisted to **Zarr arrays**, one sample per append.

---

## Services and Adapters

- **Adapters**: Handle control commands and post acknowledgments.
- **Services**: Receive all messages synchronously; used for persistence, monitoring, and logging.

---

## Shared Memory (Future Extension)

For high-demand real-time tasks, the client and hardware adapter may exchange data/control via **managed shared memory**:

- **Allocation & Layout**: Hub creates named shared memory regions (e.g., `/pylabhub/<hub_id>/<run_id>/<stream>`), with one or more **ring buffers** of fixed-size slots.
- **Ownership & Lifetimes**: Slots cycle through states: `free → writing(adapter) → readable(client) → free`. The Hub arbitrates state transitions to avoid races.
- **Zero-Copy**: Adapters write directly into shared memory; clients read by mapping the same segment. Only headers/indices traverse the control channel for acknowledgments.
- **Backpressure**: When the ring is full, the adapter **pauses** until space is available (default) or drops by explicit policy. Control messages still preempt via the control channel.

---

## Health and Timebase

- `get_health()` → queue sizes and status.
- `get_timebase()` → wall-clock, monotonic time, device offsets (for future sync).

---

## Quick Start Example

Example of Hub + PersistenceService:

```python
import asyncio
import numpy as np
from api import Hub, StreamSpec
from persistence_service import PersistenceService

async def main():
    hub = Hub("hub-1")
    svc = PersistenceService(base_dir="./persist_out")
    hub.register_service(svc)

    await hub.open_session(token="dev")

    await hub.create_stream(
        "camera_raw",
        StreamSpec(name="camera_raw", dtype="uint8", shape=(480, 640), encoding="application/x-numpy")
    )

    await hub.emit_state("instrument/camera", {"exposure_ms": 10, "gain": 2})
    await hub.emit_event("capture_started", {"sequence": 1})

    frame = np.random.randint(0, 255, size=(480, 640), dtype=np.uint8)
    await hub.send_data("camera_raw", {"dtype": "uint8", "shape": (480, 640)}, frame.tobytes())

    await asyncio.sleep(0.05)

    await svc.close()
    await hub.close_session()

if __name__ == "__main__":
    asyncio.run(main())
```

**Result:**

- Events and state saved to `persist_out/events.parquet`
- Data saved to `persist_out/zarr/camera_raw`

