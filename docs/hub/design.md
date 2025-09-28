# pyLabHub — Hub Design & Architecture

> Draft status: **Core Function Set (CFS) established**, details will evolve through **RFCs (Requests for Comments)**.

## 0. Context & Goals

The **Hub** is envisioned as the **central coordinator** within pyLabHub. Its primary function is to act as a communication, orchestration, and gatekeeping layer that both provides controlled ways to instruct hardware during experiments (via customized scripts or protocols for real‑time actuation or processing, with these instructions recorded alongside raw data) and isolates direct manipulation of source data between experiment hardware, software clients, and data services. In practice, this means:

* Providing a **low-latency single‑authorized‑client session** with deterministic command→ack and data channels, strict FIFO delivery, bounded buffers, and precise timestamps for all control and data.
* Acting as a **gatekeeper broker** that isolates raw data from downstream analysis while allowing controlled actuation/processing to be executed and recorded alongside acquisition.
* Serving as a **registry and coordination point** for adapters (hardware), connectors (external software), and services (e.g., persistence, scheduling, provenance).
* Enforcing consistent **session lifecycle management**, capturing both experiment instructions and outcomes so that runs can be replayed, audited, and shared with full provenance.

### Primary Goals

* A **hub** is the coordinator for a group of tasks or operations that need to run within the same time frame—either driven by real-time instructions from a client through the hub, or automatically following a client-defined plan or protocol.
* Deliver a **minimal but extensible hub core** where each hub handles one coordinated experiment or task set, identified by a unique **hub ID** (distinct from experiment/task IDs) to trace the source and delegation of data and inform clients where information originates. Clients may connect to multiple hubs, but hubs do not talk directly to each other.
* Define a **stable, language-agnostic messaging protocol** that distinguishes between **three classes**: (1) **control** messages (JSON, strict UTF‑8, schema‑validated) for commands/acks/config; (2) **context & event** messages (JSON) for metadata, logs, and slow/structured data with mixed units/types; and (3) **data stream** messages with a small JSON header plus an efficient binary payload (NumPy/Zarr/Arrow). In-memory transport is used for the Core Function Set (CFS), with IPC/network modes to follow later.
* Implement a **single‑client session router** with two prioritized channels: (1) control/commands (high priority) with acknowledgements, and (2) data/telemetry (high throughput). Internal services (e.g., persistence) attach to the hub, but additional external clients are not allowed concurrently in CFS.
* Provide **first-class persistence hooks** for Zarr (arrays) and Parquet (events), tied together with a manifest.
* Establish a **plugin system** to register and extend adapters, connectors, and services.

#### Message Classes & Formats (for protocol clarity)

We distinguish **four** kinds of messages with different format priorities:

1. **Control messages** — run/control commands, acknowledgements, configuration changes.

   * **Goal:** deterministic behaviour and easy debugging; high priority.
   * **Suggested format (CFS):** JSON (strict UTF‑8, schema‑validated) with explicit `schema` and `content-type` fields; carried on the `control` channel.
   * **Future option:** MsgPack (same schema) for compactness if needed.

2. **Context & Event messages** — experiment context, metadata, logs, and **slow/structured data** where fields are heterogeneous (mixed units/types) and human readability is valuable.

   * **Goal:** human‑readable, easy to debug, broadly compatible; suitable for **low‑rate, non‑uniform data**.
   * **Suggested format (CFS):** JSON (strict UTF‑8, schema‑validated) with `schema` and `content-type`. Carried on the `events` channel.

3. **State messages** — mark the condition of acquired data, including instrument state, calibration status, and any post‑acquisition processing (user‑defined or standard operations/scripts).

   * **Goal:** document the quality, transformations, or conditions of data for provenance and reproducibility.
   * **Suggested format (CFS):** JSON with clear `state` and `provenance` fields, carried on the `events` channel (or a dedicated `state` channel if separation is desired).

4. **Data Stream messages** — **high‑rate** streams or bulk blocks (e.g., waveforms, images).

   * **Goal:** efficiency and precise binary interpretation.
   * **Suggested format (CFS):** a small JSON header (topic, schema id, dtype/shape/units, chunk index, timestamps) + a **binary payload** buffer. The payload is encoded according to the stream spec (e.g., NumPy-compatible dtype layout; Zarr chunk bytes; Arrow IPC for tabular/event batches).
   * **Client/adapter responsibility:** encode/decode the payload per the declared schema and `content-type`.

---

## 1. Messaging Needs & Initial Implementation Proposal (CFS)

**Scope of this proposal:** Define the **needs** for hub messaging and present an **initial implementation** approach suitable for the Core Function Set. Details can evolve via RFCs.

### 1.1 Messaging Needs

* **Channels & Addressing**: a small, fixed set of channels per session — `control`, `data`, and `events` — addressed to the current client and internal services. Each hub also exposes `hub.health` for liveness/metrics.
* **Envelope & Schemas**: every message carries a small header with `schema` and `content-type` so clients know how to parse payloads.
* **Ordering**: per-topic FIFO delivery within a hub.
* **Backpressure**: bounded ring buffers per channel with strict flow control; default policy is pause writers when the buffer is full, preventing data loss. Control channel preempts data channel when needed.
* **Timing**: timestamps that record both the actual date/time of day (wall-clock) and a steadily increasing internal counter (monotonic), with optional device clock information for alignment.
* **Shared memory path**: optional zero‑copy data/control via hub-managed shared memory (ring buffers, explicit slot ownership, bounded, timestamped).
* **Provenance hooks**: trace IDs and run IDs for auditability.
* **Errors**: structured error messages with `code`, `message`, `data`.
* **Interoperability**: language-agnostic formats; forward-compatible schema versioning.

### 1.2 Initial Implementation (CFS)

* **Transport (CFS)**: In‑process Python (asyncio queues). IPC/UDS/TCP can be added later without changing message schemas or channel priorities.
* **Serialization**:

  * **Control & Events (slow/structured data):** JSON (UTF‑8). Fields: `msg_id`, `kind`, `channel: "control"|"events"`, `schema`, `content-type`, `ts`, `trace_id`, `payload`.
  * **Data**: JSON header + binary payload buffer. `channel: "data"`. `content-type` advertises payload encoding such as:

    * `application/x-numpy` (raw ndarray bytes with dtype/shape in header)
    * `application/vnd.zarr.chunk` (compressed chunk bytes)
    * `application/vnd.apache.arrow.stream` (Arrow IPC)
* **Backpressure**: single authorized client; no subscriber credits. Use per-channel queue depth and blocking puts; optional drop policy only under explicit configuration.
* **Reliability**: at-least-once within a process; no persistence of the bus itself in CFS (persistence handled by services).
* **Security**: not in CFS beyond process-local trust; add in RFCs for IPC/network modes.

#### 1.2.a In‑memory transport design (CFS) — single‑client session

1. **ClientSession**

   * Holds two channels: `control_q` and `data_q` (asyncio Queues with distinct priorities).
   * Maintains **session token** and **hub ID** for authorization and tracing.

2. **Router**

   * `send_control(cmd)`: high‑priority path; preempts data when necessary; waits for explicit **ack** message.
   * `send_data(shard)`: high‑throughput path; blocks when `data_q` is full (prevents data loss).

3. **Timestamps**

   * Attach both wall‑clock and monotonic timestamps to every message; devices may include source‑clock info.

4. **Persistence service (internal)**

   * Attached directly to the hub; receives data synchronously from the hub (not via external subscription) to minimize copies and ensure ordering.

**Sketch (Python, simplified):**

```python
@dataclass
class Message:
    header: dict  # msg_id, kind, channel, schema, ts, content-type, trace_id
    buffer: memoryview | None = None

class ClientSession:
    def __init__(self, hub_id: str, token: str, max_data: int = 1024):
        self.hub_id = hub_id
        self.token = token
        self.control_q: asyncio.Queue[Message] = asyncio.Queue(maxsize=128)
        self.data_q: asyncio.Queue[Message] = asyncio.Queue(maxsize=max_data)

class Hub:
    def __init__(self, hub_id: str):
        self.hub_id = hub_id
        self.session: ClientSession | None = None

    async def open_session(self, token: str):
        if self.session is not None:
            raise RuntimeError("session-already-open")
        self.session = ClientSession(self.hub_id, token)

    async def send_control(self, msg: Message) -> Message:
        await self.session.control_q.put(msg)   # higher priority lane
        ack = await self._wait_for_ack(msg.header["msg_id"])  # deterministic
        return ack

    async def send_data(self, msg: Message):
        await self.session.data_q.put(msg)      # blocks when full to prevent loss
```
### 1.2.b Messaging Protocol Goals (CFS)

* Ensure **deterministic control**: every command receives an acknowledgement in order.
* **Prevent data loss**: use bounded queues with blocking behavior; control messages always take priority.
* Maintain **timestamp integrity**: all messages include wall‑clock and monotonic time, with device offsets when available.
* Provide a **clear separation** between acquisition and analysis: only one authorized client per session; internal services (like persistence) connect directly.
* **State reporting**: emit explicit state messages (instrument condition, calibration status, post‑acquisition processing) to document data condition alongside acquisition.
* Support **future extensibility**: allow for a future offline/replay mode where multiple clients can attach safely without affecting acquisition. Use JSON on the `events` channel for low‑rate, non‑uniform streams; reserve the `data` channel for high‑throughput binary blocks.

#### 1.2.c Managed Shared Memory (CFS option)

For **high‑demand real‑time tasks**, the client and hardware adapter may exchange data/control via **managed shared memory**:

* **Allocation & layout**: hub creates named shared memory regions (e.g., `/pylabhub/<hub_id>/<run_id>/<stream>`), with one or more **ring buffers** of fixed‑size slots. Each slot holds a small JSON header (timestamps, dtype/shape, seqno) and a binary payload region.
* **Ownership & lifetimes**: slots cycle through states: `free → writing(adapter) → readable(client) → free`. The hub arbitrates state transitions to avoid races.
* **Zero‑copy**: adapters write directly into shared memory; clients read by mapping the same segment. Only headers/indices traverse the control channel for acks.
* **Timestamps & ordering**: each slot carries wall‑clock + monotonic timestamps and a sequence number; readers verify monotonicity.
* **Backpressure**: when the ring is full, the adapter **pauses** until space is available (default) or drops by explicit policy. Control messages still preempt via the control channel.
* **Safety**: memory is zeroed on allocation; segments are reference‑counted and cleaned on session close or crash recovery; access is limited to the single authorized client.
* **API sketch**:

  * `shm_create(stream, slots, slot_bytes) -> ShmHandle`
  * `shm_write(handle, hdr, payload_ptr)` (adapter)
  * `shm_read(handle) -> (hdr, payload_ptr)` (client)
  * `shm_close(handle)`

### 1.3 Sample Envelopes

**(A) Control (JSON)**

```json
{
  "msg_id": "550e8400-e29b-41d4-a716-446655440000",
  "kind": "command",
  "channel": "control",
  "schema": "pylabhub/run.open@1",
  "content-type": "application/json",
  "ts": "2025-09-23T19:00:00Z",
  "trace_id": "a1b2c3d4",
  "payload": {
    "run_id": "run-2025-09-23-001",
    "base_dir": "./runs",
    "description": "DAQ step test"
  }
}
```

**(B) Events — slow/structured data (JSON)**

```json
{
  "msg_id": "0a1b2c3d-1111-2222-3333-444455556666",
  "kind": "event",
  "channel": "events",
  "schema": "pylabhub/events.log@1",
  "content-type": "application/json",
  "ts": "2025-09-23T19:00:05.123456Z",
  "trace_id": "a1b2c3d4",
  "payload": {
    "name": "gain_change",
    "params": {"gain_db": 20, "units": "dB"},
    "notes": "Applied during step test"
  }
}
```

**(C) State (JSON)**

```json
{
  "msg_id": "77778888-9999-aaaa-bbbb-ccccdddd0000",
  "kind": "state",
  "channel": "events",
  "schema": "pylabhub/state.instrument@1",
  "content-type": "application/json",
  "ts": "2025-09-23T19:00:06.000000Z",
  "trace_id": "a1b2c3d4",
  "payload": {
    "instrument_id": "ni6321-01",
    "calibration": {"file": "cal/ni6321-2025-09-01.yaml", "status": "valid"},
    "processing": [{"name": "baseline_subtract", "version": "1.0", "kind": "post-acq"}]
  }
}
```

**(D) Data (header + binary payload)**

```json
{
  "msg_id": "f1d2d2f9-2a7b-4b2c-9a12-123456789abc",
  "kind": "data",
  "channel": "data",
  "schema": "pylabhub/stream.append@1",
  "content-type": "application/x-numpy",
  "ts": "2025-09-23T19:00:05.123456Z",
  "trace_id": "a1b2c3d4",
  "payload": {
    "dtype": "int16",
    "shape": [2000000, 4],
    "units": "V",
    "t0": "2025-09-23T19:00:05Z",
    "dt": 5e-06,
    "chunk_index": 42
  },
  "buffer": "<bytes>"
}
```

**Notes:** The `buffer` is transported as raw bytes by the transport; the header remains JSON for readability and compatibility.

