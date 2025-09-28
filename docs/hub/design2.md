# Hub Design

The Hub module provides the **core coordination point** for pyLabHub.
It defines how messages are exchanged between producers (e.g. adapters, instruments) and consumers (e.g. services, persistence, analysis), with explicit rules for **queueing, acknowledgments, and message structure**.

The design is deliberately narrow: the Hub is not a general framework, but the minimal component that guarantees consistency, determinism, and extensibility.

---

## Context

In practice, different types of information must be handled differently:

* **Control commands and acknowledgments** must never be dropped and must always be correlated.
* **Events and state messages** provide diagnostic and contextual information; they must be logged reliably but can be lower priority.
* **Data streams** may be high-volume (e.g. images, waveforms); they must be delivered without loss, but throughput is more important than latency.

The Hub formalizes this separation by exposing **three channels** with different queueing semantics and delivery priorities.
This allows the system to scale without ambiguity and ensures users know where to send and receive different kinds of information.

---

## 1. Goals

* **Deterministic behavior**: bounded queues with blocking puts, explicit acknowledgments, ordered delivery.
* **Channel separation**: control, events, and data each have dedicated bounded queues.
* **Extensibility**: adapters and services can be registered; services receive all messages synchronously.
* **Persistence hooks**: services can record events and state to Parquet, and data streams to Zarr.
* **Future-proofing**: shared-memory APIs exist as stubs for later high-speed transport extensions.

---

## 1.1 Messaging Model

* **Control channel**
  Priority channel for commands and acks. Queue size = 128. Blocking put on full.

* **Events channel**
  For logs, diagnostics, and state reports. Queue size = 512. Blocking put on full.

* **Data channel**
  For high-volume data streams (arrays/buffers). Queue size = 1024. Blocking put on full.

**Delivery order:** `recv_next()` returns the next message with priority **control → events → data**.

---

## 1.2 Message Envelope

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

* `msg_id` (unique per message)
* `trace_id` (defaults to `msg_id`)
* `ts.wall_clock` and `ts.monotonic`

---

## 1.3 Control Channel

* Commands are sent with `send_control(header, payload)`.
* Each command has a unique `msg_id`.
* The Hub registers a **waiter** keyed by `msg_id`.
* A handler must call `post_ack(Ack)` with the same `msg_id`.
* `await_ack(msg_id)` resolves deterministically to that Ack.

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

---

## 1.4 Events Channel

Two message kinds:

* **Events**:
  `emit_event(name, payload)` → `kind="event"`, schema default `pylabhub/events.log@1`

* **State**:
  `emit_state(scope, fields)` → `kind="state"`, schema default `pylabhub/state.instrument@1`

Events and state are typically persisted to **Parquet** by a service.

---

## 1.5 Data Channel

* Declare streams with `create_stream(name, StreamSpec)` (dtype, shape, encoding).
* Append data with `send_data(stream, header, buffer)` or `append(stream, shard)`.
* Buffers are blocking on full queue, ensuring no silent loss.
* Each message should specify `dtype` and `shape` in its header.

Data is typically persisted to **Zarr arrays**, one sample per append.

---

## 1.6 Services and Adapters

* **Adapters**: handle control commands and post acks.
* **Services**: receive all messages synchronously; used for persistence, monitoring, logging.

---

## 1.7 Shared Memory (stubbed)

API surface:

* `shm_create(stream, slots, slot_bytes)`
* `shm_write(handle, header, payload_ptr)`
* `shm_read(handle)`
* `shm_close(handle)`

These are no-ops in the CFS, reserved for future use.

---

## 1.8 Health and Timebase

* `get_health()` → queue sizes and status.
* `get_timebase()` → wall-clock, monotonic time, device offsets (for future sync).

---

## 2. Quick Start

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

* Events and state saved to `persist_out/events.parquet`
* Data saved to `persist_out/zarr/camera_raw`
