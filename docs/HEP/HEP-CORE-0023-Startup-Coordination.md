# HEP-CORE-0023: Startup Coordination

| Property      | Value                                                              |
|---------------|--------------------------------------------------------------------|
| **HEP**       | `HEP-CORE-0023`                                                    |
| **Title**     | Startup Coordination — Deferred DISC_ACK and Role Presence Waiting |
| **Status**    | Phase 1 implemented (2026-03-11) — Phase 2 (deferred DISC_ACK) pending |
| **Created**   | 2026-03-10                                                         |
| **Area**      | Broker Protocol / Script Hosts / Config                            |
| **Depends on**| HEP-CORE-0007 (Protocol), HEP-CORE-0015 (Processor)               |

---

## 1. Problem Statement

When a pipeline starts, roles connect to the broker in arbitrary order. Without
coordination, a consumer may discover a channel before the producer has registered it
(CHANNEL_NOT_FOUND), or a processor may begin processing before its upstream producer
is ready. Clients must either poll/retry or have the broker manage readiness.

There are two complementary coordination mechanisms:

1. **Deferred DISC_ACK** (broker-managed): The broker holds a consumer's DISC_REQ until
   the upstream producer registers. No config needed; transparent to all clients.

2. **wait_for_roles** (config-managed): A role explicitly declares which other roles it
   must see registered before it begins its processing loop. Uses `ROLE_REGISTERED_NOTIFY`.

---

## 2. Deferred DISC_ACK Protocol

### 2.1 Behavior

When a consumer sends `DISC_REQ` for a channel that does not yet exist (or exists in
`PendingReady` state), the broker **defers** the reply instead of sending an immediate error:

```
Consumer                    Broker                      Producer
    |                          |                             |
    |── DISC_REQ ─────────────>|  (channel not found)        |
    |                          |  [enqueue in pending_disc_] |
    |                          |                             |
    |                          |<── REG_REQ ─────────────────|
    |                          |── REG_ACK ─────────────────>|
    |                          |                             |
    |                          |  [resolve pending_disc_]    |
    |<── DISC_ACK ─────────────|                             |
    |── CONSUMER_REG_REQ ─────>|                             |
    |<── CONSUMER_REG_ACK ─────|                             |
    |                          |                             |
```

### 2.2 Deferral Timeout

Pending DISC_REQ entries have a configurable timeout (default 30 s). If the producer does
not register within the timeout, the broker sends an ERROR reply:
```
{"error": "CHANNEL_NOT_FOUND", "message": "channel not registered after 30000ms"}
```

The client (`hub::Consumer` / `Messenger`) propagates this as an exception from
`discover_producer()`. The host process logs the error and terminates.

### 2.3 Chain Resolution (Multi-hop)

Deferred DISC_ACK resolves independently on each hub. For a chain
`Producer → Hub A → Processor-A → Hub B → Processor-B → Hub C → Consumer`:

1. Processor-A sends DISC_REQ to Hub A. Hub A defers until Producer registers.
2. Producer registers on Hub A → Hub A resolves Processor-A's DISC_ACK.
3. Processor-A registers its output channel on Hub B (CONSUMER_REG_REQ on Hub A,
   REG_REQ on Hub B). This is independent — no deadlock.
4. Processor-B sends DISC_REQ to Hub B. Hub B defers until Processor-A registers.
5. When Processor-A completes its Hub B REG_REQ → Hub B resolves Processor-B's DISC_ACK.

No `wait_for_roles` is needed for direct adjacency. Deferred DISC_ACK handles it.

### 2.4 Broker Configuration

```cpp
struct BrokerService::Config {
    std::chrono::milliseconds pending_disc_timeout{30000};  // deferral timeout
};
```

```json
"broker": {
  "pending_disc_timeout_ms": 30000
}
```

---

## 3. ROLE_REGISTERED_NOTIFY / ROLE_DEREGISTERED_NOTIFY

### 3.1 Purpose

Broadcast events that allow roles to react when other roles join or leave the hub.
Used by `wait_for_roles` to detect when upstream roles are ready.

### 3.2 ROLE_REGISTERED_NOTIFY

```
Direction:  Broker → ALL connected roles on this hub
Trigger:    Successful REG_REQ or CONSUMER_REG_REQ (role fully registered)
Delivery:   Unsolicited push (same as CHANNEL_CLOSING_NOTIFY)

Payload:
  role_uid          string   UID of the newly registered role
  role_type         string   "producer" | "consumer" | "processor"
  channel           string   Channel the role registered on
  hub_uid           string   UID of this hub (source_hub_uid in IncomingMessage)
```

Script host delivery (`source_hub_uid` identifies which hub):
```python
{"event": "role_registered", "role_uid": "PROD-SENSOR-A1B2C3D4",
 "role_type": "producer", "channel": "lab.raw", "source_hub_uid": "HUB-A-..."}
```

### 3.3 ROLE_DEREGISTERED_NOTIFY

```
Direction:  Broker → ALL connected roles on this hub
Trigger:    Successful DEREG_REQ or CONSUMER_DEREG_REQ; or broker-detected death

Payload:
  role_uid          string
  role_type         string   "producer" | "consumer" | "processor"
  channel           string
  reason            string   "graceful" | "heartbeat_timeout" | "process_dead"
  hub_uid           string
```

Script host delivery:
```python
{"event": "role_deregistered", "role_uid": "...", "role_type": "...",
 "channel": "...", "reason": "graceful", "source_hub_uid": "..."}
```

### 3.4 Delivery Policy

All `ROLE_REGISTERED_NOTIFY` / `ROLE_DEREGISTERED_NOTIFY` notifications are broadcast to
every connected role on the hub — no filtering, no subscription. This keeps the broker
simple. If volume becomes a concern, per-channel subscriptions can be added later.

---

## 4. ROLE_PRESENCE_REQ / ROLE_INFO_REQ (Polling)

For one-shot presence checks (used by `wait_for_roles` implementation):

```
ROLE_PRESENCE_REQ:
  role_uid          string   (or UID pattern with prefix e.g. "PROD-SENSOR-*")

ROLE_PRESENCE_ACK:
  status            string   "success"
  present           bool     true if role is currently registered

ROLE_INFO_REQ:
  role_uid          string   (exact match)

ROLE_INFO_ACK:
  status            string   "success"
  role_uid          string
  role_type         string
  channel           string
  inbox_endpoint    string   (empty if no inbox)
  inbox_schema_json string   (JSON string; empty if no inbox)
  inbox_packing     string
```

---

## 5. wait_for_roles Config

> **Implementation status**: Phase 1 implemented (2026-03-11). Pattern matching and UID prefix
> restrictions are deferred to Phase 2.

### 5.1 Field Definition

All three script host configs support `startup.wait_for_roles`. Each entry specifies an
**exact role UID** and an optional per-role timeout:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `uid` | string | required | Exact UID to wait for (e.g. `"PROD-SENSOR-A1B2C3D4"`) |
| `timeout_ms` | int | 10000 | Per-role timeout in milliseconds; must be > 0 |

All three role binaries (producer, consumer, processor) accept this field.
Deadlock prevention is the operator's responsibility (e.g. do not create mutual waits).

**Note on adjacent processor chains**: Two adjacent processors in a chain
(`Proc-A → Proc-B`) do not need `wait_for_roles`. Deferred DISC_ACK handles
their sequencing automatically (see §2.3).

### 5.2 Config Example

```json
"startup": {
  "wait_for_roles": [
    {"uid": "PROD-SENSOR-A1B2C3D4", "timeout_ms": 15000},
    {"uid": "PROC-FILTER-B5C6D7E8"}
  ]
}
```

Roles are waited for sequentially in list order. Each has an independent deadline.
Absent `timeout_ms` defaults to 10000 ms.

### 5.3 Implementation: Startup Wait Loop (C++)

Executed in each script host's `start_role()`, after the messenger connects but before
`on_init` is called and before any background threads start:

```cpp
static constexpr int kPollMs = 200;
for (const auto& wr : config_.wait_for_roles) {
    LOGGER_INFO("[role] Startup: waiting for role '{}' (timeout {}ms)...",
                wr.uid, wr.timeout_ms);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{wr.timeout_ms};
    bool found = false;
    while (std::chrono::steady_clock::now() < deadline) {
        py::gil_scoped_release rel;
        if (messenger_.query_role_presence(wr.uid, kPollMs)) {
            found = true;
            break;
        }
    }
    if (!found) {
        LOGGER_ERROR("[role] Startup wait failed: role '{}' not present after {}ms",
                     wr.uid, wr.timeout_ms);
        return false;  // triggers cleanup_on_start_failure()
    }
    LOGGER_INFO("[role] Startup: role '{}' found", wr.uid);
}
```

Uses `Messenger::query_role_presence()` (ROLE_PRESENCE_REQ polling, 200ms poll interval).
GIL is released during each 200ms poll so other Python threads remain unblocked.

### 5.4 Deferred: UID Prefix Restrictions (Phase 2)

The original design proposed prefix restrictions to prevent deadlocks:
- Producer: not allowed to wait for any role
- Consumer: may wait for `PROD-*` or `PROC-*` only
- Processor: may wait for `PROD-*` only

These restrictions are deferred to Phase 2. Current implementation accepts any UID
in any role type.

### 5.5 Dual-Hub Processor: Broker Selection for wait_for_roles

For a processor with `in_hub_dir` ≠ `out_hub_dir` (dual-broker configuration), the
startup wait queries **`out_messenger_` only** (the output broker). This means:

- Roles registered on the **output hub** are correctly detected.
- Roles registered only on the **input hub** (e.g. an upstream SHM producer) will
  **not** be found and the wait will time out.

**Consequence**: In dual-hub setups, configure `startup.wait_for_roles` only with UIDs
of roles on the same hub as `out_hub_dir`. For single-hub processors (`in_hub_dir ==
out_hub_dir`, or `hub_dir` only), both producer and consumer are on the same broker and
this distinction does not apply.

**Phase 2 note**: A `broker: "in"|"out"` per-role field is deferred to Phase 2 to
allow explicit broker selection when waiting for roles on the input hub.

---

## 6. Complete Startup Sequence

### Phase 1: Process launch

Hub brokers are assumed to be running before any role starts.

### Phase 2: Hub A registrations (producer + processor input-side)

```
Producer:
  bind P2C ROUTER + XPUB sockets
  → REG_REQ (Hub A)  [role_type="producer"]
  ← REG_ACK
  → start heartbeat

Processor:
  send CONSUMER_REG_REQ → wait DISC_ACK  [broker defers until producer registers]
  wait_for_roles: ["PROD-SENSOR-*"]      [optional explicit wait]
```

### Phase 3: Processor data plane (after DISC_ACK resolves)

```
Processor:
  attach to in_shm (if in_transport="shm") OR connect ZMQ PULL socket
  start in_queue_
```

### Phase 4: Hub B registration (processor output-side)

```
Processor:
  bind out P2C ROUTER + XPUB sockets (if out_transport="shm")
  OR bind ZMQ PUSH socket (if out_transport="zmq")
  → REG_REQ (Hub B)  [role_type="processor"]
  ← REG_ACK
  → start heartbeat on Hub B
```

When `startup.hub_b_after_input_ready = true`, Phase 4 executes after Phase 3 completes.
When `false` (default), Phases 3 and 4 execute in parallel.

### Phase 5: Consumer (Hub B)

```
Consumer:
  → DISC_REQ (Hub B)  [broker defers until processor registers its output]
  ← DISC_ACK          [released when Processor's REG_REQ on Hub B succeeds]
  → CONSUMER_REG_REQ
  ← CONSUMER_REG_ACK
  attach to out_shm
  → HELLO (P2P to processor)
  on_consumer_joined fires in processor's ctrl_thread_
```

### Phase 6: Steady state

All roles are registered. Deferred DISC_ACKs resolved. `wait_for_roles` conditions met.
Processing loops started. Heartbeats flowing.

---

## 7. source_hub_uid in IncomingMessage

When a processor connects to two hubs, control messages from both arrive on the same
`messages` list in `on_process`. The `source_hub_uid` field identifies the origin:

```cpp
struct IncomingMessage {
    std::string       event;        // event type or empty for P2P data
    std::string       sender_uid;   // sender role UID (for relay events)
    std::string       source_hub_uid; // hub that generated this message
    nlohmann::json    details;      // event payload
    std::vector<char> data;         // P2P binary payload
};
```

This is populated in `ctrl_thread_` from the messenger that received the event:
- Messages from `in_messenger_` → `source_hub_uid = in_hub_uid_`
- Messages from `out_messenger_` → `source_hub_uid = out_hub_uid_`

For single-hub roles (producer, consumer), `source_hub_uid` is always the one connected hub.

---

## 8. Protocol Index

| Message | Direction | §12.x in HEP-0007 |
|---------|-----------|-------------------|
| ROLE_REGISTERED_NOTIFY | Broker → All roles | Added §12.5 |
| ROLE_DEREGISTERED_NOTIFY | Broker → All roles | Added §12.5 |
| ROLE_PRESENCE_REQ/ACK | Role → Broker → Role | Added §12.3 |
| ROLE_INFO_REQ/ACK | Role → Broker → Role | Added §12.3 |
| DISC_REQ deferral | Consumer → Broker | Modified §12.3 |
