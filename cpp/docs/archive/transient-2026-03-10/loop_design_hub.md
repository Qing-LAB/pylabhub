# Hub Role Design

**Status**: ✅ Fully implemented as of 2026-03-09. Archived 2026-03-10 → `docs/archive/transient-2026-03-10/`.
**Canonical reference**: HEP-CORE-0017 (Pipeline Architecture) + HEP-CORE-0022 (Federation).
**Scope**: Responsibilities, thread model, loop design, arbitration, directory structure.

---

## 1. Hub Responsibilities

The hub is a **control plane and service directory node**. It owns infrastructure
and arbitrates access, but never participates in the data plane.

### 1.1 SHM Block Ownership
The hub owns all SHM (DataBlock) blocks. When a producer registers a channel with
`transport=shm`, the hub is the authority that records the SHM name and secret.
Consumers can only attach to a SHM block if the hub grants them access via
`CONSUMER_REG_ACK`. No role can access a SHM block without hub approval.

### 1.2 ZMQ Virtual Channel Node Registry
The hub tracks ZMQ peer-to-peer channels (HEP-0021) as virtual nodes. A ZMQ virtual
node has no memory allocation — only an endpoint string stored in the broker's channel
registry. The hub is the service directory for these endpoints:
- Producer registers: `REG_REQ{transport=zmq, zmq_endpoint=...}`
- Consumer discovers: `CONSUMER_REG_REQ` → hub returns `zmq_endpoint` → consumer
  connects directly (peer-to-peer, hub not in the data path)

The hub **does not relay ZMQ data**. It only stores and echoes the endpoint.
Data flows peer-to-peer between roles, entirely outside the hub.

### 1.3 Connection Arbitration (Gating)
The hub says **yes or no** to every connection request. Current arbitration factors:

| Factor | Implemented | Mechanism |
|--------|-------------|-----------|
| Encryption key (CURVE25519) | Yes | ZMQ CURVE handshake on ROUTER socket |
| Schema compatibility | Yes | BLDS hash comparison on CONSUMER_REG_REQ |
| Connection type (1 writer, N readers) | Yes | Broker enforces single producer per channel |
| Connection policy (Open/Tracked/Verified) | Yes | `hub.json` connection_policy + known_actors |
| Per-channel policy overrides | Yes | `channel_policies` glob matching in hub.json |
| **Loop/timing compatibility (hard reject)** | Yes — TRANSPORT_MISMATCH (2026-03-09) | See §5 below |

### 1.4 Health Monitoring
The hub monitors the health of all registered roles:
- **Heartbeat collection**: producer sends `HEARTBEAT_REQ` periodically; broker checks
  timeout on every poll cycle. Dead channel → `CHANNEL_CLOSING_NOTIFY` + `FORCE_SHUTDOWN`.
- **Consumer liveness**: broker checks registered consumer PIDs are still alive (configurable interval).
- **Metrics aggregation**: `METRICS_REPORT_REQ` from roles → broker stores in MetricsStore;
  `METRICS_REQ` from admin → broker returns JSON snapshot.
- **Channel snapshot**: typed `ChannelSnapshot` available to HubScript tick loop at each tick.

### 1.5 Admin and Scripting Support
The hub provides two interfaces for admin and automation:
- **Admin shell** (ZMQ REP): interactive Python exec() — `pylabhub.channels()`,
  `pylabhub.metrics()`, `pylabhub.shutdown()`, etc.
- **Hub script** (Python tick loop): `on_start`/`on_tick`/`on_stop` callbacks with
  `HubScriptAPI` giving access to channel snapshots, close/broadcast/notify operations.

### 1.6 Broadcasting and Federation
- **Broadcast to channel** (HEP-0007): `CHANNEL_NOTIFY_REQ` → `CHANNEL_BROADCAST_NOTIFY`
  to all channel members (producer + consumers).
- **Hub federation** (HEP-0022): outbound DEALER connections to peer hubs;
  `HUB_PEER_HELLO/ACK`, `HUB_RELAY_MSG` (broadcast relay), `HUB_TARGETED_MSG` (direct).

---

## 2. Thread Model

The hub runs **three concurrent threads**:

```
Main thread (hubshell.cpp)
  ├── broker_thread_  ← BrokerService::run()
  │     ROUTER socket, 100ms poll
  │     All protocol messages: REG_REQ, HEARTBEAT_REQ, CONSUMER_REG_REQ, etc.
  │     Heartbeat timeout checks
  │     Metrics store writes
  │     Federation DEALER sockets
  │     → Queues events for hub_thread_ (thread-safe)
  │
  ├── hub_thread_     ← HubScript::do_python_work()
  │     Python interpreter
  │     Tick loop (tick_interval_ms, default 1000ms)
  │     GIL released between ticks (AdminShell can exec())
  │     Dispatches federation events queued from broker_thread_
  │     Calls on_start / on_tick / on_stop
  │
  └── admin_thread_   ← AdminShell (ZMQ REP)
        Accepts exec() requests
        Runs in shared interpreter namespace
        GIL-safe: interleaves with hub_thread_ GIL releases
```

**Key**: The broker_thread_ is the one that collects heartbeats and handles all
ZMQ protocol messages. The hub_thread_ is the scripting/management loop. These are
genuinely separate concerns running concurrently.

---

## 3. Hub Loops

The hub has **two loops** running concurrently on separate threads:

### 3.1 Broker Event Loop (broker_thread_)
```
loop:
    zmq_poll(ROUTER socket, timeout=100ms)
    if message received:
        dispatch: REG_REQ | HEARTBEAT_REQ | CONSUMER_REG_REQ |
                  DEREG_REQ | CONSUMER_DEREG_REQ |
                  METRICS_REPORT_REQ | CHANNEL_NOTIFY_REQ |
                  CHANNEL_BROADCAST_REQ | ...
    check heartbeat timeouts (every poll cycle)
    check consumer liveness (configurable interval)
    drain admin request queue (close_channel, broadcast, metrics queries)
    drain federation outbox (relay messages, targeted messages)
```

This loop is **event-driven with a bounded poll timeout** (100ms). It is not
configurable by the user. It is always running while the hub is alive.

### 3.2 Script Tick Loop (hub_thread_)
```
loop:
    sleep in 10ms slices until next_tick
    query broker snapshot (no GIL)
    periodic health log (no GIL)
    acquire GIL:
        dispatch queued federation events
        on_tick(api, tick_info)
    release GIL
    next_tick = tick_start + tick_interval_ms
```

This loop is **time-driven**. Period: `tick_interval_ms` (default 1000ms, configurable
in `hub.json["python"]["tick_interval_ms"]`).

**Timing policy**: FixedPace (deadline advances from actual tick start, not intended
deadline). This is correct for a management loop — overruns are absorbed, not compensated.

**Note on naming**: The equivalent of `target_period_ms` for the hub tick loop is
`tick_interval_ms`. The name is different from producer/consumer deliberately — the hub
tick is not a data-processing rate but a management polling interval.

---

## 4. Hub Directory Structure

The hub directory is the hub's filesystem home. It currently contains:

```
hub-dir/
  hub.json          — hub configuration (identity, broker, policy, script, peers)
  hub.pubkey        — broker CURVE25519 public key (written at startup)
  hub.vault         — encrypted keypair (AES-256-GCM, protected by master password)
  script/
    python/
      __init__.py   — optional hub Python script (on_start/on_tick/on_stop)
```

**Gaps — not yet formally defined with a HEP:**

```
hub-dir/
  known_keys/       — [MISSING] public keys of known roles (producers/consumers/processors)
                        currently embedded in hub.json["connection"]["known_actors"]
                        should be separate files: <uid>.pubkey or <uid>.z85
  schemas/          — [MISSING] named schema JSON files for this hub's channels
                        SchemaLibrary already has search dirs but hub doesn't own them formally
  channels/         — [MISSING] per-channel policy config (currently all in hub.json)
```

**Action required**: A new HEP (e.g. HEP-CORE-0024) should define the hub directory
layout formally — analogous to how HEP-0018 defines the producer/consumer directory.

---

## 5. Design Gaps

### 5.1 Loop/Timing Compatibility Arbitration ✅ Hard-reject done 2026-03-09

The **hard reject** case is implemented (Phase 6 / 2026-03-09):
- `consumer_loop_driver` declared in `CONSUMER_REG_REQ`
- Broker rejects with `TRANSPORT_MISMATCH` when `consumer.loop_driver != producer.transport`
- `ConsumerScriptHost` explicitly sets `loop_driver="shm"` or `"zmq"` from config
- `ProcessorScriptHost` leaves `loop_driver=""` (transport-agnostic; broker skips check)
- This also fixed a Phase 6 regression where the processor unconditionally sent `"zmq"`

**Remaining**: Rate mismatch warning (consumer period << producer period) — low priority.
The warning would fire when `consumer.target_period_ms < producer.target_period_ms`; this
is cosmetic only (does not cause data corruption). Deferred until consumer role design.

### 5.2 Hub Directory Structure (not formally defined)

See §4 above. A HEP is needed for the hub directory layout.

### 5.3 Broker Event Loop Period (not configurable)

The 100ms poll timeout in `BrokerService::run()` is hardcoded. For most use cases this
is fine. If needed, it could be exposed as `hub.json["broker"]["poll_interval_ms"]`.
Not a priority.

### 5.4 Tick Overrun Detection ✅ Done 2026-03-09

`LOGGER_WARN` added in `hub_script.cpp` when `elapsed_ms > tick_interval_ms_` at the
start of a tick (fires on tick #2+, skips the startup interval). Log message:
`"HubScript: tick overrun: elapsed=Xms target=Yms (tick #N)"`

---

## 6. What Is Complete

| Feature | HEP | Status |
|---------|-----|--------|
| SHM block ownership + registry | HEP-0002 | Done |
| ZMQ virtual channel node | HEP-0021 | Done |
| Schema arbitration (BLDS hash) | HEP-0016 | Done |
| Connection policy (Open/Tracked/Verified) | HEP-0013 | Done |
| Heartbeat + channel lifecycle | HEP-0007 | Done |
| Two-tier shutdown (CLOSING_NOTIFY + FORCE) | HEP-0007 | Done |
| Metrics plane | HEP-0019 | Done |
| Admin shell | HEP-0011 | Done |
| Hub scripting (on_start/on_tick/on_stop) | HEP-0011 | Done |
| Hub federation (HEP-0022) | HEP-0022 | Done |
| Loop/timing compatibility arbitration (hard reject) | Phase 6 | Done 2026-03-09 |
| Rate mismatch warning (cosmetic) | — | Deferred |
| Tick overrun detection (LOGGER_WARN) | §5.4 | Done 2026-03-09 |
| Hub directory structure HEP | — | Gap |
| Known keys / schemas / channels dirs | — | Gap |

---

## 7. Prohibited Combinations (Hub-Enforced)

These are configuration combinations the hub SHOULD reject at connection time.
Currently only the schema and key checks are enforced; the rest are future work.

| Combination | Severity | Reason | Currently enforced? |
|-------------|----------|--------|---------------------|
| `consumer.loop_driver=zmq` + `producer.transport=shm` | **Hard error** | Consumer has no ZMQ queue to read from | Yes — TRANSPORT_MISMATCH (2026-03-09) |
| `consumer.loop_driver=shm` + `producer.transport=zmq` | **Hard error** | Consumer has no SHM block to read from | Yes — TRANSPORT_MISMATCH (2026-03-09) |
| Schema hash mismatch | **Hard error** | Data corruption | Yes |
| Unknown producer key (Verified policy) | **Hard error** | Security | Yes |
| Two producers on same channel | **Hard error** | 1-writer invariant | Yes |
| `consumer.target_period_ms` << `producer.target_period_ms` | **Warning** | Consumer starved, wastes CPU | No |

---

## 8. Config Reference

### hub.json (current)
```json
{
  "hub": {
    "uid":  "HUB-MYHUB-AABBCCDD",
    "name": "mylab.hub.main"
  },
  "script": { "path": ".", "type": "python" },
  "python": {
    "tick_interval_ms":       1000,
    "health_log_interval_ms": 60000
  },
  "broker": {
    "endpoint":               "tcp://0.0.0.0:5570",
    "channel_timeout_s":      10,
    "channel_shutdown_grace_s": 5
  },
  "connection": {
    "policy": "open",
    "known_actors": [],
    "channel_policies": []
  },
  "peers": []
}
```

### Target hub directory layout (proposed, pending HEP)
```
hub-dir/
  hub.json
  hub.pubkey
  hub.vault
  known_keys/
    <PROD-UID>.z85      — known producer public keys
    <CONS-UID>.z85      — known consumer public keys
  schemas/
    <schema-name>.json  — named schema definitions (SchemaLibrary search path)
  channels/
    <channel>.json      — per-channel policy override (future)
  script/
    python/
      __init__.py
```

---

*Next: `loop_design_producer.md`*
