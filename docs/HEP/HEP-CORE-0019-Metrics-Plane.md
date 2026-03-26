# HEP-CORE-0019: Metrics Plane

| Property       | Value                                                                            |
|----------------|----------------------------------------------------------------------------------|
| **HEP**        | `HEP-CORE-0019`                                                                  |
| **Title**      | Metrics Plane — Passive SHM Metrics, Broker-Initiated Pull, Global Role Table |
| **Status**     | Phase 1 Implemented — 2026-03-05; **Phase 2 redesign** — 2026-03-25 (heartbeat/metrics separation) |
| **Created**    | 2026-03-02                                                                        |
| **Area**       | Framework Architecture (`pylabhub-utils`, all binaries, `BrokerService`)          |
| **Depends on** | HEP-CORE-0002 (DataHub), HEP-CORE-0007 (Protocol), HEP-CORE-0017 (Pipeline)     |

---

## 1. Motivation

The current system has metrics scattered across three independent mechanisms:

1. **SHM-level** (`DataBlockMetrics` in `SharedMemoryHeader`): contention counters,
   race detection, reader peaks — baked into the memory layout, always updated,
   but only readable by processes that have the SHM segment mapped.

2. **Per-binary counters** (`in_slots_received`, `out_slots_written`, `drops`,
   `script_error_count`): maintained by each API object, accessible from Python
   scripts, but never transmitted anywhere.

3. **Heartbeat** (`HEARTBEAT_REQ`): carries `{channel_name, producer_pid}` — a
   bare liveness signal with no metrics payload.

**What's missing:**

- No centralized view of pipeline health — an operator cannot ask the broker
  "what's the throughput on channel X?" or "how many drops has the processor seen?"
- No way for scripts to report custom domain metrics (e.g., "events above threshold",
  "calibration drift") to a central collector.
- SHM metrics are siloed per-segment; cross-segment comparisons require mapping
  every SHM block manually.

**This HEP introduces the Metrics Plane** — a fifth communication plane (extending
HEP-CORE-0017 §2) that unifies passive SHM monitoring and voluntary ZMQ reporting
through the broker as the single aggregation point.

---

## 2. Design Principles

1. **Passive SHM, active ZMQ**: SHM metrics are always being collected (they're
   embedded in `SharedMemoryHeader`). ZMQ metrics are collected by roles and
   reported on demand when the broker requests them.

2. **Broker maintains a global role table**: The broker stores the latest metrics
   snapshot per (channel, role UID), with timestamps. Monitoring tools query the
   broker. The broker never relays metrics between participants.

3. **Heartbeat is liveness only**: `HEARTBEAT_REQ` carries `channel_name`, `uid`,
   and `role_type` — enough to identify the sender and confirm it is alive. No
   metrics payload. All roles (producer, consumer, processor) send heartbeats
   using the same message format.

4. **Metrics are broker-initiated pull**: When a monitoring tool sends `METRICS_REQ`,
   the broker requests metrics from each active role via `METRICS_COLLECT_REQ`.
   Roles respond with their current `snapshot_metrics_json()`. The broker stores
   the response in its global table and returns the aggregated view to the requester.

5. **Scripts extend via `api.report_metric()`**: Custom key-value metrics are
   accumulated on the API object and included in the next metrics response.
   Zero configuration — just call the API.

6. **SHM metrics read locally**: Processes that have SHM mapped can read
   `DataBlockMetrics` directly (zero-copy, no ZMQ). The broker also reads SHM
   metrics for channels it knows about, merging them into the aggregated view.

### 2.1 Phase 2 changes (2026-03-25)

Phase 1 piggybacked metrics on heartbeats (producer/processor) and used a separate
`METRICS_REPORT_REQ` for consumers. This created:
- Inconsistency: three different reporting paths for the same logical operation
- Heartbeat bloat: liveness pings carried large JSON payloads
- No broker-to-role pull: the broker stored whatever was last pushed

Phase 2 separates concerns:
- **Heartbeat** → lightweight liveness ping (all roles, same format)
- **Metrics** → broker pulls from roles on demand (all roles, same format)
- **Global role table** → broker indexes by (channel, UID) with role_type and timestamps

---

## 3. Architecture

### Phase 2 architecture (2026-03-25)

```
┌─────────────┐   HEARTBEAT_REQ              ┌──────────────────────┐
│  Producer    │  {channel, uid, role_type}   │                      │
│              │─────────────────────────────►│                      │
└─────────────┘                               │     Broker           │
                                              │                      │
┌─────────────┐   HEARTBEAT_REQ              │  ┌────────────────┐  │   METRICS_REQ
│  Processor   │  {channel, uid, role_type}   │  │ Global Role    │  │◄─────────────┐
│              │─────────────────────────────►│  │ Table          │  │              │
└─────────────┘                               │  │                │  │   ┌──────────┤
                                              │  │ channel → {    │  │   │  Admin   │
┌─────────────┐   HEARTBEAT_REQ              │  │   uid → {      │  │   │  Shell / │
│  Consumer    │  {channel, uid, role_type}   │  │     role_type  │  │   │  CLI /   │
│              │─────────────────────────────►│  │     heartbeat  │  │   │  Monitor │
└─────────────┘                               │  │     metrics    │  │   └──────────┘
                                              │  │     timestamp  │  │
        ┌─────────────────────────────────────│  │   }            │──┤
        │  METRICS_COLLECT_REQ (broker→role)  │  │ }              │  │ METRICS_ACK
        │  METRICS_COLLECT_ACK (role→broker)  │  └────────────────┘  │──────────────►
        └─────────────────────────────────────│                      │
                                              │  ┌────────────────┐  │
                                              │  │ SHM direct     │  │
                                              │  │ read (on query)│  │
                                              │  └────────────────┘  │
                                              └──────────────────────┘
```

### 3.1 Message flows

**Heartbeat** (role → broker, periodic, all roles):

| Field | Description |
|-------|-------------|
| `channel_name` | Channel this role is registered on |
| `uid` | Role's unique identity (e.g. "PROD-Sensor-AABBCCDD") |
| `role_type` | "producer", "consumer", or "processor" |

Lightweight liveness ping. No metrics payload. All roles use the same format.
The broker updates `last_heartbeat` in its global role table.

**Metrics collection** (nudge + enriched heartbeat):

| Step | Message | Direction | Content |
|------|---------|-----------|---------|
| 1 | `METRICS_REQ` | Admin → Broker | `{channel_name}` (optional — omit for all channels) |
| 2 | `METRICS_COLLECT_REQ` | Broker → each active role | `{channel_name, uid}` (one-way nudge, no response expected) |
| 3 | (role sets internal flag: "include metrics in next heartbeat") | | |
| 4 | `HEARTBEAT_REQ` | Role → Broker (next heartbeat cycle) | `{channel_name, uid, role_type, metrics: {...}}` (enriched) |
| 5 | Broker stores metrics in global table | | |
| 6 | `METRICS_ACK` | Broker → Admin | Current table contents + live SHM data |

The `METRICS_COLLECT_REQ` is a one-way nudge — no response expected. The role
sets an internal flag and includes `snapshot_metrics_json()` in its next heartbeat.
The broker returns immediately to the admin with whatever is currently in the
global table (may be stale). The admin can poll again after one heartbeat interval
(~1-2s) to get fresh data.

**SHM metrics are always fresh**: the broker reads `DataBlockMetrics` directly
from SharedMemoryHeader on every `METRICS_REQ`. These are low-level SHM counters
(contention, timeouts, reader peaks). Role-level metrics (timing, drops, loop
overrun) require the nudge+heartbeat path because they are process-local.

**Heartbeat with vs without metrics**: most heartbeats are bare `{channel_name,
uid, role_type}`. Only the heartbeat immediately after a `METRICS_COLLECT_REQ`
nudge carries the `metrics` field. The flag is cleared after one enriched heartbeat.

**SHM metrics** (broker reads directly, on query):

The broker reads `DataBlockMetrics` from SharedMemoryHeader for channels it
can access. These are merged into the `METRICS_ACK` response under the `shm`
key (unchanged from Phase 1).

### 3.2 Global role table

The broker maintains a table indexed by `(channel_name, uid)`:

```json
{
  "sensor.data": {
    "PROD-Sensor-AABBCCDD": {
      "role_type": "producer",
      "last_heartbeat": "2026-03-25T10:30:00.123Z",
      "metrics": {
        "base": {
          "out_written": 50042,
          "drops": 3,
          "iteration_count": 50045,
          "loop_overrun_count": 0,
          "last_cycle_work_us": 150,
          "script_errors": 0,
          "data_drop_count": 0,
          "last_iteration_us": 10012,
          "max_iteration_us": 15200,
          "last_slot_wait_us": 45,
          "last_slot_exec_us": 9800,
          "configured_period_us": 10000,
          "context_elapsed_us": 500420000,
          "ctrl_queue_dropped": 0
        },
        "custom": {
          "events_above_threshold": 127
        }
      },
      "metrics_timestamp": "2026-03-25T10:29:58.456Z"
    },
    "CONS-Display-11223344": {
      "role_type": "consumer",
      "last_heartbeat": "2026-03-25T10:30:01.789Z",
      "metrics": { ... },
      "metrics_timestamp": "2026-03-25T10:29:59.012Z"
    }
  }
}
```

**Lifecycle:**
- Entry created on first `HEARTBEAT_REQ` from a role
- `last_heartbeat` updated on each heartbeat
- `metrics` + `metrics_timestamp` updated on each `METRICS_COLLECT_ACK`
- Entry removed on `DISC_ACK` (role deregistered) or heartbeat timeout

### 3.3 Removed from Phase 2

- **`METRICS_REPORT_REQ`**: voluntary push from consumer — replaced by nudge+heartbeat
- **Always-on heartbeat metrics piggybacking**: metrics are included only after a `METRICS_COLLECT_REQ` nudge, not on every heartbeat
- **Different heartbeat formats**: all roles now use the same `{channel_name, uid, role_type}` base format
- **`METRICS_COLLECT_ACK`**: no separate response message — metrics come via the next enriched heartbeat

---

## 4. Protocol Extensions

### 4.1 `HEARTBEAT_REQ` (Phase 2 — liveness only)

All roles send the same format:

```json
{
  "msg_type": "HEARTBEAT_REQ",
  "channel_name": "sensor.data",
  "uid": "PROD-Sensor-AABBCCDD",
  "role_type": "producer"
}
```

No metrics payload. The broker updates `last_heartbeat` in the global role table.
`role_type` is one of `"producer"`, `"consumer"`, `"processor"`.

### 4.2 `METRICS_COLLECT_REQ` (broker → role, one-way nudge)

Triggered by `METRICS_REQ` from admin. Broker sends to each active role on the
requested channel:

```json
{
  "msg_type": "METRICS_COLLECT_REQ",
  "channel_name": "sensor.data",
  "uid": "PROD-Sensor-AABBCCDD"
}
```

**No response expected.** The role sets an internal flag (`metrics_requested_`).
On its next heartbeat cycle, the role includes `snapshot_metrics_json()` in the
heartbeat payload, then clears the flag.

### 4.3 Enriched `HEARTBEAT_REQ` (role → broker, after nudge)

When `metrics_requested_` is set, the role sends:

```json
{
  "msg_type": "HEARTBEAT_REQ",
  "channel_name": "sensor.data",
  "uid": "PROD-Sensor-AABBCCDD",
  "role_type": "producer",
  "metrics": {
    "base": { ... },
    "custom": { ... }
  }
}
```

The `metrics` field is present ONLY when the flag is set. The broker stores it
in the global role table with a timestamp and clears the pending flag.

Normal heartbeats (no nudge) carry no `metrics` field.

### 4.3 `METRICS_REQ` / `METRICS_ACK` (admin → broker)

Request:
```json
{
  "msg_type": "METRICS_REQ",
  "channel_name": "ch"
}
```

If `channel_name` is omitted, returns metrics for **all channels**.

Response:
```json
{
  "msg_type": "METRICS_ACK",
  "status": "success",
  "channels": {
    "ch": {
      "producer": {
        "uid": "PROD-SENSOR-A1B2C3D4",
        "pid": 1234,
        "last_report": "2026-03-02T14:30:01.234Z",
        "base": {
          "out_written": 50042,
          "drops": 3,
          "script_errors": 0,
          "iteration_count": 50045
        },
        "custom": {
          "events_above_threshold": 127,
          "avg_processing_ms": 2.3
        }
      },
      "consumers": [
        {
          "uid": "CONS-LOGGER-A1B2C3D4",
          "pid": 5678,
          "last_report": "2026-03-02T14:30:00.891Z",
          "base": {
            "in_received": 49980,
            "script_errors": 0,
            "iteration_count": 49981
          },
          "custom": {
            "bytes_logged": 2048576
          }
        }
      ],
      "shm": {
        "write_lock_contention": 12,
        "writer_timeout_count": 0,
        "reader_race_detected": 3,
        "reader_peak_count": 2
      }
    }
  }
}
```

The `shm` section is populated on-demand when the broker can access the SHM
segment. If the SHM is unavailable (e.g., ZMQ-only channel), `shm` is omitted.

---

## 5. Script API

### 5.1 `api.report_metric(key, value)`

Available on all three API objects (`ProducerAPI`, `ConsumerAPI`, `ProcessorAPI`).

```python
def on_produce(out_slot, fz, msgs, api):
    # ... write data ...
    api.report_metric("events_above_threshold", count)
    api.report_metric("avg_processing_ms", elapsed)
    return True
```

- `key`: string, max 64 chars, alphanumeric + `._-`
- `value`: numeric (`int` or `float`), stored as `double`
- Metrics accumulate in a `std::unordered_map<std::string, double>` on the API
  object, guarded by a lightweight spinlock (updated from the script thread,
  read from the zmq thread that builds heartbeat payloads).
- The map is **snapshot-and-clear** on each heartbeat: the zmq thread takes a
  copy, clears the map, and serializes the copy into the heartbeat JSON.

### 5.2 `api.report_metrics(dict)`

Batch variant for efficiency:

```python
api.report_metrics({
    "events_above_threshold": count,
    "avg_processing_ms": elapsed,
    "calibration_drift_ppm": drift,
})
```

### 5.3 `api.clear_metrics()`

Clears all custom metrics. Base counters are never cleared.

---

## 6. Broker-side Storage

### 6.1 `MetricsStore`

A simple in-memory store keyed by `(channel_name, participant_uid)`:

```cpp
struct ParticipantMetrics
{
    std::string uid;
    uint64_t    pid          = 0;
    TimePoint   last_report;

    // Base counters (atomic snapshot from last report)
    uint64_t in_received     = 0;
    uint64_t out_written     = 0;
    uint64_t drops           = 0;
    uint64_t script_errors   = 0;
    uint64_t iteration_count = 0;

    // Custom KV (last reported values)
    std::unordered_map<std::string, double> custom;
};

struct ChannelMetrics
{
    ParticipantMetrics           producer;
    std::vector<ParticipantMetrics> consumers;
};

// Keyed by channel name
std::unordered_map<std::string, ChannelMetrics> store_;
```

- Updated on every `HEARTBEAT_REQ` (producer/processor) and every
  `METRICS_REPORT_REQ` (consumer).
- Entries removed when a channel is deregistered or a consumer deregisters.
- **No history** — only the latest snapshot is retained. Time-series collection
  is the responsibility of external monitoring tools that poll `METRICS_REQ`.

### 6.2 SHM metrics read

On `METRICS_REQ`, the broker attempts to open the channel's SHM segment
read-only and call `slot_rw_get_metrics()` to populate the `shm` section.
If the SHM segment is unavailable or the channel uses ZMQ-only transport,
the `shm` field is omitted.

---

## 7. Integration with HEP-CORE-0017 (Pipeline Architecture)

HEP-CORE-0017 §2 now defines five planes: Data, Control, Message, Timing, and Metrics.

This HEP adds a **fifth plane: Metrics**:

| Plane | What flows | Mechanism | Where defined |
|-------|-----------|-----------|---------------|
| **Metrics plane** | Counter snapshots, custom KV pairs | Piggyback on HEARTBEAT (producer/processor), `METRICS_REPORT_REQ` (consumer), `METRICS_REQ/ACK` (query) | HEP-CORE-0019 |

The metrics plane is **read-only from the broker's perspective** — participants
report in, the broker aggregates and serves queries, but never pushes metrics
to participants or alters behavior based on metric values.

---

## 8. Data Flow Summary

```mermaid
sequenceDiagram
    participant Script as Python Script
    participant API as ProducerAPI / ProcessorAPI
    participant ZMQ as zmq_thread_
    participant Broker as BrokerService
    participant Store as MetricsStore
    participant Admin as AdminShell

    Script->>API: report_metric("key", value)
    Note over API: custom_metrics_ map<br/>(spinlock-guarded)

    loop Every heartbeat interval
        ZMQ->>API: snapshot_and_clear_metrics()
        API-->>ZMQ: {base counters + custom KV}
        ZMQ->>Broker: HEARTBEAT_REQ + metrics{}
        Broker->>Store: update(channel, uid, metrics)
    end

    Admin->>Broker: METRICS_REQ(channel)
    Broker->>Store: lookup(channel)
    opt SHM available
        Broker->>Broker: slot_rw_get_metrics() from SHM
    end
    Broker-->>Admin: METRICS_ACK(channels{...})
```

---

## 9. Implementation Phases

```mermaid
graph LR
    P1["Phase 1<br/>C++ infra"] --> P2["Phase 2<br/>Heartbeat ext."]
    P2 --> P3["Phase 3<br/>Consumer report"]
    P3 --> P4["Phase 4<br/>Query API"]
    P4 --> P5["Phase 5<br/>Python bindings"]

    style P1 fill:#2a4a2a,stroke:#333
    style P2 fill:#2a4a2a,stroke:#333
    style P3 fill:#2a4a2a,stroke:#333
    style P4 fill:#2a4a2a,stroke:#333
    style P5 fill:#2a4a2a,stroke:#333
```

### Phase 1: C++ infrastructure ✅

1. Added `custom_metrics_` map + `InProcessSpinState` to `ProducerAPI`, `ConsumerAPI`,
   `ProcessorAPI`
2. Added `report_metric()`, `report_metrics()`, `clear_custom_metrics()` methods
3. Added `snapshot_metrics_json()` for zmq thread consumption (snapshot-and-clear)
4. Added `MetricsStore` to `BrokerService` (Pimpl-internal, guarded by `m_query_mu`)

### Phase 2: Heartbeat extension (producer + processor) ✅

1. Extended `Messenger::enqueue_heartbeat(channel, json metrics)` overload
2. In `run_zmq_thread_()`: `api_.snapshot_metrics_json()` → pass to `enqueue_heartbeat()`
   via `HeartbeatTracker` periodic task (`ZmqPollLoop`)
3. Broker: `handle_heartbeat_req()` extracts `metrics` field, updates `MetricsStore`

### Phase 3: Consumer metrics reporting ✅

1. Added `METRICS_REPORT_REQ` message type (`MetricsReportCmd` in messenger_internal.hpp)
2. Consumer `run_zmq_thread_()`: periodic `HeartbeatTracker` sends `METRICS_REPORT_REQ`
   with base counters + custom metrics
3. Broker: `handle_metrics_report_req()` updates `MetricsStore`

### Phase 4: Query API ✅

1. Added `METRICS_REQ`/`METRICS_ACK` handler to `BrokerService`
2. SHM `DataBlockMetrics` read deferred (not included in Phase 4 — query returns ZMQ-reported metrics only)
3. Exposed via AdminShell: `pylabhub.metrics("channel_name")` → JSON

### Phase 5: Python bindings ✅

1. Bound `api.report_metric(key, value)`, `api.report_metrics(dict)`,
   `api.clear_custom_metrics()` in all three `PYBIND11_EMBEDDED_MODULE` blocks
2. Defaults applied at pybind11 level only (per pybind11 Default Parameter Rule —
   `docs/IMPLEMENTATION_GUIDANCE.md` § "pybind11 Default Parameter Rule")

### Test coverage

- 10 `MetricsPlaneTest` (protocol round-trip: heartbeat with metrics, METRICS_REPORT_REQ,
  METRICS_REQ/ACK, consumer metrics, all-channels query, unknown channel, update overwrites,
  multi-consumer, empty custom, backward compat)
- 9 `MetricsApiTest` (API unit: report_metric, report_metrics, snapshot_metrics_json,
  snapshot clears, clear_custom_metrics, base counters, thread safety, pybind11-linked)
- **Total: 19 new tests; 828/828 passing (2026-03-05)**

---

## 10. Non-Goals

- **Time-series storage**: The broker holds only the latest snapshot. Historical
  data is the responsibility of external tools.
- **Alerting / thresholds**: The broker does not evaluate metric values. A future
  HEP could add threshold-based `CHANNEL_WARNING_NOTIFY` messages.
- **Metric push to participants**: The broker never sends metrics to producers
  or consumers. Information flows upward (participant → broker → query tool).
- **Prometheus / OpenTelemetry integration**: Out of scope for this HEP. A thin
  adapter that polls `METRICS_REQ` and exposes a scrape endpoint would be
  straightforward but is a separate concern.

---

## 11. Relationship to Existing Metrics

| Existing mechanism | Status after this HEP |
|---|---|
| `DataBlockMetrics` in SHM header | **Unchanged** — still updated passively. Broker reads on query. |
| Per-API counters (`in_received`, etc.) | **Extended** — now reported to broker via heartbeat/metrics report. |
| `iteration_count_` (internal) | **Included** in base metrics — proves liveness quantitatively. |
| SHM heartbeat pool (consumer PIDs in header) | **Unchanged** — data-plane liveness, orthogonal to metrics plane. |

---

## 12. Source File Reference

Files modified during implementation:

| Component | Source File | Impact |
|-----------|------------|--------|
| **ProducerAPI** | `src/producer/producer_api.hpp/cpp` | `report_metric()`, `custom_metrics_` map, `snapshot_metrics_json()`, pybind11 bindings |
| **ConsumerAPI** | `src/consumer/consumer_api.hpp/cpp` | Same as ProducerAPI |
| **ProcessorAPI** | `src/processor/processor_api.hpp/cpp` | Same as ProducerAPI |
| **BrokerService** | `src/utils/ipc/broker_service.cpp` | `MetricsStore` (Pimpl-internal), `handle_metrics_report_req()`, `handle_metrics_req()` |
| **Messenger protocol** | `src/utils/ipc/messenger_protocol.cpp` | `METRICS_REPORT_REQ` handler, `METRICS_REQ/ACK` dispatch |
| **Messenger internal** | `src/utils/ipc/messenger_internal.hpp` | `MetricsReportCmd` struct added to `MessengerCommand` variant |
| **Messenger** | `src/include/utils/messenger.hpp` | `enqueue_heartbeat(channel, json)` overload, `enqueue_metrics_report()` |
| **AdminShell module** | `src/hub_python/pylabhub_module.hpp/cpp` | `pylabhub.metrics(channel="")` binding |
| **ProducerScriptHost** | `src/producer/producer_script_host.cpp` | zmq_thread_ heartbeat now includes `api_.snapshot_metrics_json()` |
| **ConsumerScriptHost** | `src/consumer/consumer_script_host.cpp` | zmq_thread_ periodic `METRICS_REPORT_REQ` via `HeartbeatTracker` |
| **ProcessorScriptHost** | `src/processor/processor_script_host.cpp` | zmq_thread_ heartbeat now includes `api_.snapshot_metrics_json()` |
| **Tests (protocol)** | `tests/test_layer3_datahub/test_datahub_metrics_plane.cpp` | 10 `MetricsPlaneTest` tests |
| **Tests (API)** | `tests/test_layer4_producer/test_metrics_api.cpp` | 9 `MetricsApiTest` tests (pybind11-linked) |
