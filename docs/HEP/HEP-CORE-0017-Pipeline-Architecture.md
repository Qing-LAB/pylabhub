# HEP-CORE-0017: Pipeline Architecture

| Property      | Value                                                                           |
|---------------|---------------------------------------------------------------------------------|
| **HEP**       | `HEP-CORE-0017`                                                                 |
| **Title**     | Pipeline Architecture — Components, Planes, Topologies, and Boundaries         |
| **Status**    | Implemented — 2026-03-03.  Doc text refreshed 2026-05-06 against current binaries: roles run via `plh_role --role <producer|consumer|processor>` (HEP-CORE-0024) and the hub binary is `plh_hub` (HEP-CORE-0033 §15).  Earlier per-role binaries (`pylabhub-producer/consumer/processor`) and the legacy `pylabhub-hubshell` have been retired and deleted from the tree. |
| **Created**   | 2026-03-01                                                                      |
| **Updated**   | 2026-03-01 (actor eliminated; producer/consumer binaries added); 2026-04-21 (binary unification — `pylabhub-producer/consumer/processor` retired in favor of unified `plh_role`) |
| **Area**      | Framework Architecture (`pylabhub-utils`, `pylabhub-scripting`, `plh_role` unified binary) |
| **Depends on**| HEP-CORE-0002 (DataHub), HEP-CORE-0007 (Protocol), HEP-CORE-0011 (ScriptHost), HEP-CORE-0034 (Schema Registry — supersedes HEP-CORE-0016), HEP-CORE-0024 (Role Directory Service — binary unification) |

> **Binaries (current, 2026-05-06)**: this document uses the current
> launch form throughout: roles run via
> `plh_role --role <producer|consumer|processor>` (HEP-CORE-0024) and
> the hub binary is `plh_hub` (HEP-CORE-0033 §15).  Where this HEP
> earlier referred to per-role binaries (`pylabhub-{producer,consumer,
> processor}`) or the legacy `pylabhub-hubshell`, those names are
> retained only in `## 12 Document History` for traceability.
> Protocol, topology, and architectural content is unchanged.

---

## 1. Motivation

As the DataHub system grows to include Producers, Consumers, Processors, and schema
management, it becomes useful to have a single document that captures:

1. The **five planes** of communication and their strict separation
2. The **component roles** and the boundary decisions behind each
3. The **topology patterns** that compose these components into pipelines
4. How **schema** integrates with pipeline construction
5. The **deployment model** that governs binary identity and config

This document does not re-specify any protocol or implementation detail — those
live in the per-component HEPs (0002, 0007, 0015, 0016, 0018). This document
provides the cross-cutting architectural view.

---

## 2. The Five Planes

Every channel in the DataHub system carries traffic on five independent planes.
These planes are strictly orthogonal: changes to one have no effect on the others.

| Plane | What flows | Mechanism | Where defined |
|-------|-----------|-----------|---------------|
| **Data plane** | Slot payloads (typed fields) | SHM ring buffer (`DataBlockProducer`/`Consumer`) or msgpack-encoded ZMQ frames (`hub::ZmqQueue`) | HEP-CORE-0002 §3, §7.1 |
| **Control plane** | HELLO / BYE / REG / DISC / HEARTBEAT | ZMQ ROUTER–DEALER ctrl sockets + Broker | HEP-CORE-0007 |
| **Message plane** | Inter-role messaging (broker-coordinated channel events + point-to-point inbox) | ZMQ via `BrokerRequestComm` (channel notifications, role discovery, broadcasts) and `InboxQueue` / `InboxClient` (point-to-point, HEP-CORE-0027) | HEP-CORE-0007 §6, HEP-CORE-0027 |
| **Timing plane** | Loop pacing — fixed rate, max rate, compensating | `LoopPolicy` on `DataBlockProducer`/`Consumer` | HEP-CORE-0008 |
| **Metrics plane** | Counter snapshots, custom KV pairs | Piggyback on per-presence `HEARTBEAT_REQ` (Phase 6 — every presence, including consumers, carries an optional `metrics` field on its own heartbeat); `METRICS_REQ/ACK` (admin query). `METRICS_REPORT_REQ` **RETIRED** in Wave M1.4 (2026-05-11). | HEP-CORE-0019 |

```mermaid
graph LR
    subgraph "Data Plane"
        DP_SHM["SHM Ring Buffer"]
        DP_ZMQ["ZMQ PUSH/PULL"]
    end

    subgraph "Control Plane"
        CP_BROKER["BrokerService<br/>(ROUTER–DEALER)"]
        CP_REG["REG / DISC / BYE"]
        CP_HB["HEARTBEAT"]
    end

    subgraph "Message Plane"
        MP_BRC["BrokerRequestComm<br/>(broker-coordinated)"]
        MP_INBOX["InboxQueue / InboxClient<br/>(point-to-point — HEP-0027)"]
    end

    subgraph "Timing Plane"
        TP_LP["LoopPolicy"]
        TP_METRICS["IterationMetrics"]
    end

    subgraph "Metrics Plane"
        MET_STORE["MetricsStore<br/>(broker aggregation)"]
        MET_API["api.report_metric()"]
    end

    DP_SHM -.->|"orthogonal"| CP_BROKER
    CP_BROKER -.->|"orthogonal"| MP_BRC
    MP_BRC -.->|"orthogonal"| MP_INBOX
    MP_INBOX -.->|"orthogonal"| TP_LP
    TP_LP -.->|"orthogonal"| MET_STORE
```

**Why this separation matters:**

- Replacing SHM with ZMQ on the data plane does not touch the control plane
  (HELLO/BYE/heartbeat still flow on the same ZMQ ctrl sockets).
- Changing `LoopTimingPolicy` from `FixedRate` to `MaxRate` has no effect on what
  messages flow on the message plane.
- A Processor that bridges two brokers must maintain control-plane connections
  to **both** brokers independently of whether its data transport is SHM or ZMQ.

The invariant: **the broker is always a control-plane coordinator, never a data relay.**
Data bytes never pass through the broker.

---

## 3. Component Roles and Boundaries

### 3.1 Producer role

| Property | Value |
|---|---|
| Direction | Write-only |
| Data transport | SHM **or** ZMQ (selected via `out_transport` in `producer.json`) |
| Channel ownership | Creates the channel; registers as producer with broker via `REG_REQ` |
| SHM-specific facilities (when `out_transport=shm`) | Spinlocks (`api.spinlock(i)`), zero-copy slot writes, flexzone R/W, acquire-timing metrics |
| Broker protocol | `REG_REQ` → `REG_ACK`; sends `HEARTBEAT_REQ` (per-presence — see HEP-CORE-0019 §2.3); handles consumer `BYE` events |
| Lives on | SHM: same host as the SHM segment. ZMQ: any host with TCP connectivity. |

The Producer role is implemented today as `ProducerRoleHost` (in
`src/producer/`); the role's data plane is reached via
`RoleAPIBase::build_tx_queue()` which selects `ShmQueue` or `ZmqQueue`
internally based on `out_transport`.  The historical `Producer`
wrapper class was retired in L3.γ A6.3 (2026-03-01); SHM-specific
facilities (spinlocks, flexzone) are now exposed directly through
`RoleAPIBase` for the SHM-side transport.

### 3.2 Consumer role

| Property | Value |
|---|---|
| Direction | Read-only |
| Data transport | SHM **or** ZMQ (selected via `in_transport` in `consumer.json`) |
| Channel ownership | Attaches to existing SHM or connects to ZMQ endpoint; registers as consumer via `CONSUMER_REG_REQ` |
| SHM-specific facilities (when `in_transport=shm`) | Spinlocks, zero-copy slot view, flexzone R/W, acquire-timing metrics |
| ZMQ-specific note | Endpoint discovery: `CONSUMER_REG_ACK.producers[]` array (HEP-CORE-0036 §6.4) for the channel's current producer set; `DISC_ACK` is for separate channel-observability queries (kLive vs kStalled).  Consumer never binds. |
| Broker protocol | `CONSUMER_REG_REQ` → `CONSUMER_REG_ACK`; sends HELLO to producer; `CONSUMER_DEREG_REQ` on exit |
| Lives on | SHM: same host as the SHM segment. ZMQ: any host with TCP connectivity. |

The Consumer role is implemented as `ConsumerRoleHost` (in
`src/consumer/`); data plane via `RoleAPIBase::build_rx_queue()`.  The
broker enforces transport compatibility (rejects with
`TRANSPORT_MISMATCH` if consumer and producer transports diverge).
The control plane (ctrl thread inside `RoleAPIBase`) is always active
regardless of data transport.

SHM-specific facilities (spinlocks, flexzone) are only available when
`in_transport=shm`.  For cross-machine reading, compose with a bridge
Processor (§4.3) or use `in_transport=zmq` with a ZMQ-transport
producer.

### 3.3 hub::QueueReader and hub::QueueWriter

> **Amendment (2026-07-08) — topology migration.**  This section
> gains a topology-parameterized model that supersedes the pre-2026-07-08
> "one producer binds, N consumers connect" hardcoding.  Three
> explicit topologies (`fan-in`, `fan-out`, `one-to-one`) declared on
> every REG_REQ.  The BINDING side of each topology owns the single
> `data_endpoint`; the DIALING side connects.  The pre-migration
> multi-endpoint PULL model (with `ProducerPeer` vector, per-peer
> `curve_serverkey` mutation, and `add_producer_peer` / `remove_producer_peer`
> APIs) is superseded by single-bind-single-connect per topology.
> The `ProducerEntry::zmq_node_endpoint` per-producer field retires;
> it becomes `ChannelEntry::data_endpoint` (scalar, owned by binding
> side).  Design authority: `docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`
> (status: DESIGN LOCKED).  Downstream consumers of this section
> (HEP-CORE-0007, HEP-CORE-0033, HEP-CORE-0036 amendments) reflect
> the new terminology.

### 3.3.0 Topology-parameterized model (2026-07-08 amendment)

The framework declares three topologies, each with an explicit
BINDING side and DIALING side (`docs/tech_draft/DRAFT_topology_singular_side_2026-07.md`
§2):

| Topology | Wire value | Transports | Binding side | Socket pair |
|---|---|---|---|---|
| Fan-in (N → 1) | `"fan-in"` | ZMQ only | Consumer | PULL (bind) ← PUSH (connect) |
| Fan-out (1 → N) | `"fan-out"` | ZMQ or SHM | Producer | ZMQ: PUB (bind) → SUB (connect); SHM: DataBlock (create) → capability-transport (attach) |
| 1-to-1 (1 → 1) | `"one-to-one"` | ZMQ or SHM | Producer | ZMQ: PUSH (bind) → PULL (connect); SHM: as fan-out |

The BINDING side of each topology owns the channel's single
`data_endpoint`, publishes it via `ENDPOINT_UPDATE_REQ` after S3 bind
(per HEP-CORE-0021 §16), maintains the ZAP allowlist (fed by
`CHANNEL_AUTH_CHANGED_NOTIFY(phase=admitted)`), and tracks its
live-peer set (fed by `CHANNEL_AUTH_CHANGED_NOTIFY(phase=live)`).
The DIALING side receives `data_endpoint` + `data_pubkey` on its
REG_ACK and dials.

**Queue factory signature (post-migration):**

```cpp
namespace pylabhub::hub {
    enum class ChannelTopology { FanIn, FanOut, OneToOne };

    // Consumer side (reader).
    std::unique_ptr<QueueReader>
    Queue::create_reader(ChannelTopology topology,
                         Transport       transport,      // "zmq" | "shm"
                         RxOptions       opts);

    // Producer side (writer).
    std::unique_ptr<QueueWriter>
    Queue::create_writer(ChannelTopology topology,
                         Transport       transport,
                         TxOptions       opts);
}
```

Three facts — side + topology + transport — uniquely determine the
socket configuration.  The role provides `topology` from config and
inherits `side` from the role kind; the queue picks socket type,
bind direction, CURVE role, and endpoint owner from the decision
matrix above.  Role code NEVER touches libzmq; role-host code
NEVER makes bind/connect decisions.  See tech draft §6 for the
full matrix.

**Options structs (post-migration):**

```cpp
struct RxOptions
{
    ChannelTopology topology;                   // NEW — from config
    Transport       transport;                  // NEW — from config
    SchemaSpec      slot_spec;
    // For BINDING side (fan-in consumer): endpoint_hint may be
    // "tcp://host:0" (ephemeral).  For DIALING side (fan-out /
    // 1-to-1 consumer): endpoint_hint is REJECTED at config-load
    // (endpoint arrives on CONSUMER_REG_ACK.data_endpoint).
    std::string     endpoint_hint;
    // ProducerPeer vector RETIRES 2026-07-08 — see §3.3-retired below.
};

struct TxOptions
{
    ChannelTopology topology;                   // NEW
    Transport       transport;                  // NEW
    SchemaSpec      slot_spec;
    // BINDING side (fan-out / 1-to-1 producer): endpoint_hint may be ephemeral.
    // DIALING side (fan-in producer): endpoint_hint REJECTED.
    std::string     endpoint_hint;
};
```

### 3.3.1 Framework integration (post-migration)

Both directions follow the same "build in Standby, ask the broker,
activate inside `apply_master_approval`" shape as before (per
HEP-CORE-0036 §3.5 + §6.7 — those sections amend in Phase A step 4
to symmetrize the R6 gate).  What changes:

**Binding side S3 flow** (fan-in consumer; fan-out or 1-to-1 producer):

1. `apply_master_approval(REG_ACK)` — no `data_endpoint` in ACK
   (binding side already has its own).
2. Queue picks socket type from topology matrix; binds; enters Configured.
3. Role host queries `queue->actual_endpoint()`, sends `ENDPOINT_UPDATE_REQ`
   with the resolved endpoint (per HEP-CORE-0021 §16.4-16.6).
4. Heartbeat task starts.  First heartbeat marks binding side Live.
5. From now on, `CHANNEL_AUTH_CHANGED_NOTIFY(phase=admitted)` fires
   for each dialing-side peer joining the allowlist; role host pulls
   via `GET_CHANNEL_AUTH_REQ` and applies to ZAP.
   `CHANNEL_AUTH_CHANGED_NOTIFY(phase=live)` fires when each dialing
   peer becomes Live; role host updates its `live_peers[channel]`
   map (feeds `api.consumer_count()` / `api.producer_count()`).

**Dialing side S3 flow** (fan-in producer; fan-out or 1-to-1 consumer):

1. REG_REQ pends at broker R6 gate until binding side is Live +
   `data_endpoint.has_value()` (endpoint published) +
   `confirmed_version >= role_registration_version` (all three
   conditions per tech draft §5.4).
2. REG_ACK arrives carrying `data_endpoint` + `data_pubkey`.
3. `apply_master_approval(REG_ACK)` — queue sets
   `curve_serverkey = data_pubkey`, connects to `data_endpoint`,
   enters Active.  For fan-out consumer (SUB): subscribes with empty
   topic.  For SHM consumers: runs AttachProtocol handshake per HEP-CORE-0044.
4. Heartbeat task starts.
5. No further peer-set coordination — the singular binding side is
   the only peer, and it's the one the dialing side connected to.

### 3.3.2 Script-facing accessors (2026-07-08 amendment, HEP-CORE-0028 sync)

The binding side's role host exposes the live-peer set via four
accessors on every role's api object:

```
api.consumer_count(channel_name: str) -> int
api.producer_count(channel_name: str) -> int
api.consumers(channel_name: str)      -> list[str]  # role_uids
api.producers(channel_name: str)      -> list[str]  # role_uids
```

Objective counts (self-inclusive when applicable).  Feed from the
binding-side `live_peers` map maintained by `phase=live` NOTIFY
events.  Script decides when to produce/consume based on peer
readiness (framework provides mechanisms, script decides policy —
see tech draft §7.6).  HEP-CORE-0028 amendment ships the accessor
bindings for Lua + Python + Native engines.

---

### 3.3-retired — Pre-2026-07-08 model (SUPERSEDED; archaeological reference)

The material below described the pre-migration model where `ZmqQueue`
PULL was multi-producer via per-peer `connect()` under a
`ProducerPeer` vector.  That model retires 2026-07-08 as part of the
topology migration.  It's preserved here for callers/tests that
haven't yet been updated (retirement lands in Phase E of the code
migration per tech draft §12).

The data plane abstraction is split into two independent abstract classes:

| Class | Role | Methods |
|-------|------|---------|
| `hub::QueueReader` | Read-only access | `read_acquire`, `read_release`, `read_flexzone`, `last_seq`, `capacity`, `policy_info`, `set_verify_checksum` + metadata |
| `hub::QueueWriter` | Write-only access | `write_acquire`, `write_commit`, `write_discard`, `write_flexzone`, `capacity`, `policy_info`, `set_checksum_options` + metadata |

Concrete implementations use C++ multiple inheritance:
```
ShmQueue : QueueReader, QueueWriter  (internal — wraps DataBlockConsumer/Producer)
ZmqQueue : QueueReader, QueueWriter  (internal — wraps ZMQ PULL/PUSH socket)
```

`ShmQueue` and `ZmqQueue` are internal implementation details; public API exposes
only `unique_ptr<QueueReader>` or `unique_ptr<QueueWriter>`. There is no combined
`Queue` interface: the old `hub::Queue` class is eliminated.

**No runtime cost**: metadata methods declared in both abstract classes (item_size,
flexzone_size, name, metrics, start, stop, is_running, capacity, policy_info) are
implemented once in the concrete class. Virtual dispatch costs ~2 ns vs 100–500 ns
for the actual SHM acquire — < 1% overhead.

QueueReader and QueueWriter are designed for independent ownership and
composition.  A processor role obtains BOTH (one per side) via
RoleAPIBase; a producer role uses only a QueueWriter; a consumer role
uses only a QueueReader.

| Property | Value |
|---|---|
| Used by | `ProducerRoleHost` (`QueueWriter` only, via `RoleAPIBase::build_tx_queue`); `ConsumerRoleHost` (`QueueReader` only, via `build_rx_queue`); `ProcessorRoleHost` (both — one per side) |
| Does NOT carry | Control plane, message plane, timing plane |

#### ZmqQueue — API contract and schema requirements

`hub::ZmqQueue` always operates in **schema mode**. A non-empty
`std::vector<ZmqSchemaField>` and a packing rule are **required** at construction.

```cpp
// Factories return unique_ptr<ZmqQueue>.  Both flavors are CURVE-only
// (HEP-CORE-0035 §2 unconditional-CURVE invariant); the public surface
// matches HEP-CORE-0040 §8.4 endpoint shape.  Identity bytes are NOT
// passed by value — `identity_key_name` is the KeyStore lookup key
// that the factory body resolves via `key_store().with_seckey(name, cb)`
// + `key_store().pubkey(name)`.
std::unique_ptr<ZmqQueue> ZmqQueue::push_to(
    const std::string& endpoint,
    std::vector<ZmqSchemaField> schema,  // REQUIRED: must not be empty
    std::string packing,                 // REQUIRED: "aligned" or "packed"
    std::string_view identity_key_name = security::kRoleIdentityName,
    std::string zap_domain = {},         // empty → derive from instance_id
    bool bind = true,
    std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
    /* sndhwm, send_buffer_depth, overflow_policy, retry_ms, instance_id */ ...);

std::unique_ptr<ZmqQueue> ZmqQueue::pull_from(
    const std::string& endpoint,
    security::Z85PublicKey server_pubkey, // PULL/connect: producer pubkey;
                                          // PULL/bind: empty sentinel ok
    std::vector<ZmqSchemaField> schema,   // REQUIRED: must not be empty
    std::string packing,                  // REQUIRED: "aligned" or "packed"
    std::string_view identity_key_name = security::kRoleIdentityName,
    bool bind = false,
    size_t max_buffer_depth = 64,
    std::optional<std::array<uint8_t, 8>> schema_tag = std::nullopt,
    /* instance_id */ ...);
```

**Violation of these requirements** causes the factory to log a `LOGGER_ERROR` and
return `nullptr`. When called through `ProducerOptions`/`ConsumerOptions`, the outer
`Producer::create` / `Consumer::connect` propagates the failure as `std::nullopt`.

Key differences from `ShmQueue`:

| Property | ShmQueue | ZmqQueue |
|---|---|---|
| Encoding | Raw slot bytes (no overhead) | msgpack field-by-field |
| Type safety | Schema hash checked at attach | Type tag checked per frame |
| Flexzone | Supported | Not supported (fz=None in scripts) |
| Alignment | SHM slot alignment rules | ctypes-compatible (per `packing`) |
| Overflow | `OverflowPolicy::Block` or `Drop` | Bounded internal buffer (drop oldest) |
| Checksum (write) | `set_checksum_options()` → BLAKE2b on commit | `set_checksum_options()` → no-op (TCP integrity) |
| Checksum (read) | `set_verify_checksum()` → BLAKE2b on acquire | `set_verify_checksum()` → no-op |
| last_seq() | `SlotConsumeHandle::slot_id()` (commit_index) | Wire frame `seq` field |
| Cross-machine | No (SHM is host-local) | Yes (TCP, PGM, etc.) |

See HEP-CORE-0002 §7.1 for the complete wire format specification.

See HEP-CORE-0002 §17.3 for detailed rationale.

#### ZmqQueue — Dynamic peer membership (HEP-CORE-0036)

A `ZmqQueue` PULL side is intrinsically multi-producer-capable
(ZMQ PULL fair-queues data from any number of connected PUSH peers;
see §4.6 Fan-In Pipeline).  Under HEP-CORE-0036, the channel's
producer set can change at runtime — producers join and leave via
REG_REQ / DEREG_REQ — and the framework drives those changes into
the queue via a small dynamic-peer API.  Roles never see ZMQ socket
operations; the queue handles bind/connect direction, per-peer
connection lifecycle, ZAP cache, and fair-queue accounting
internally.

**Extended `RxQueueOptions`** (`role_api_base.hpp`):

The single `zmq_node_endpoint` is replaced by a vector of producer
peer descriptors, one per producer in `CONSUMER_REG_ACK.producers[]`
(HEP-CORE-0036 §6.4).  Single-producer channels are the N=1 case
with no special-cased options struct.

```cpp
struct ProducerPeer
{
    std::string role_uid;       ///< Producer's role uid (HEP-CORE-0033 §G2.2.0b).
    std::string endpoint;       ///< tcp://host:port (HEP-CORE-0021 §16.3 per-producer scope).
    std::string pubkey_z85;     ///< Producer's identity pubkey
                                ///<  (HEP-CORE-0036 I6 — used by consumer-side
                                ///<   ZAP wiring if any; the broker-side ZAP
                                ///<   on the producer's PUSH is what gates
                                ///<   consumer admission).
};

struct RxQueueOptions
{
    // ... existing fields (slot_spec, fz_spec, packing, etc.) ...

    // Transport (HEP-CORE-0021 + HEP-CORE-0036).
    // For ZMQ transport: one entry per producer of the channel.
    // For SHM transport: at most 1 entry (SHM is physically single-producer).
    std::vector<ProducerPeer> producer_peers;
    // ...
};
```

**Dynamic add/remove methods** on the queue interface:

```cpp
class ZmqQueue : public QueueReader, public QueueWriter
{
public:
    // Add a producer peer to the PULL side.  ZmqQueue internally
    // performs the corresponding socket operation (current
    // implementation: socket.connect(peer.endpoint); see §4.6 for the
    // bind/connect direction discussion — choice is internal).
    void add_producer_peer(const ProducerPeer& peer);

    // Remove a producer peer.  Does NOT close any data already
    // received; only affects future connections (consistent with
    // HEP-CORE-0036 I5: revocation is forward-looking).
    void remove_producer_peer(const std::string& role_uid);
};
```

**Framework integration** (staged per HEP-CORE-0036 §3.5 +
§6.7 "Role-host integration pattern").  Both directions follow the
same shape — build in Standby, ask the broker, activate inside
`apply_master_approval`.

#### Consumer side (PULL / rx queue)

- **S1 — `setup_infrastructure_`**: `build_rx_queue(opts)`
  constructs the queue in **Standby** state per HEP-CORE-0036 §6.7.
  At this point `opts.producer_peers` may be empty — no PULL
  `connect()` happens, no PULL worker is spawned.  The queue object
  exists; the socket side is dormant.
- **S3 — `apply_master_approval(CONSUMER_REG_ACK)`**: when the
  consumer's REG_REQ is approved, the framework hands the broker's
  `CONSUMER_REG_ACK.producers[]` to the queue via the single
  polymorphic mutator
  `queue->apply_master_approval(CONSUMER_REG_ACK)`.  That mutator
  seeds `producer_peers` (merged with any S2→S3 buffered
  `set_producer_peers` calls per HEP-CORE-0036 §6.7 Option B —
  `apply_master_approval` payload wins on overlap), per-producer
  `connect()`s with `curve_serverkey = producer.pubkey` (HEP-CORE-0036
  §3.5.4 INV4), spawns the PULL worker under ThreadManager scope,
  and transitions the queue Standby → Configured → Active.  The
  queue does **NOT** call `connect()` or spawn any worker until
  `apply_master_approval` runs.
- **Runtime add/remove (post-S3, queue Active)**: on a "producer
  joined channel X" broadcast (HEP-CORE-0033 §12 channel event),
  the role-host framework looks up the queue for X and calls
  `queue.add_producer_peer(new_producer)`.  On a "producer left
  channel X" broadcast, the framework calls
  `queue.remove_producer_peer(role_uid)`.  These operations apply
  to an already-Active queue; see §4.6.1 for the full runtime flow.

#### Producer side (PUSH / tx queue) — symmetric

- **S1 — `setup_infrastructure_`**: `build_tx_queue(opts)`
  constructs the queue in **Standby** state.  The bind endpoint is
  carried in `opts.zmq_node_endpoint` but the socket is NOT bound
  yet, the ZAP handler is NOT armed, and no PUSH worker thread
  exists.  This is the AUTH-gate principle from HEP-CORE-0036 §3.5
  applied to the producer side — no listening socket exists before
  the broker has authorized the channel.
- **S3 — `apply_master_approval(REG_ACK)`**: when the producer's
  REG_REQ is approved, the framework hands the broker's
  `REG_ACK.initial_allowlist` (array of Z85 pubkey strings per
  HEP-CORE-0036 §6.2 / §6.5) to the queue via the same single
  polymorphic mutator: `queue->apply_master_approval(REG_ACK)`.
  Internally the PUSH path does, in order:
    1. **Load the allowlist** — `set_peer_allowlist(initial_allowlist)`
       writes the broker-supplied admit set into the queue's
       allowlist atomic.
    2. **`start()`** — registers the ZAP domain with the process
       ZapRouter BEFORE the socket binds (so an early peer connect
       can't race into an unregistered domain), then binds the PUSH
       socket and spawns the PUSH worker.  start() preserves the
       pre-set allowlist; the queue lands Active with the broker-
       supplied admit set already in force, not a transient empty
       one.  See HEP-CORE-0011 § "Role Host `worker_main_()` Steps"
       Step 6d.
  Transitions Standby → Configured → Active.  As on the PULL side:
  the queue does **NOT** bind or spawn any worker until
  `apply_master_approval` runs.
- **Runtime authorization changes (post-S3)**: when the broker's
  per-channel allowlist changes (consumer joined / revoked), the
  broker fires `CHANNEL_AUTH_CHANGED_NOTIFY` to the producer; the
  role-host's BRC handler pulls the new allowlist via
  `GET_CHANNEL_AUTH_REQ` and **snapshot-replaces** the queue's
  allowlist with `set_peer_allowlist` (HEP-CORE-0036 §6.5
  notify-then-pull, amendment 2026-06-04 — supersedes the retired
  snapshot-push `CHANNEL_AUTH_UPDATE` wire frame).  Replace, not
  merge: the new allowlist fully replaces the prior one atomically.
  The queue stays Active; future handshakes consult the refreshed
  cache.

#### PULL vs PUSH symmetry — and where they differ

Both sides build in Standby, defer all socket I/O to
`apply_master_approval`, and present scripts a queue interface
that hides the auth machinery.  Two asymmetries are worth calling
out so readers don't try to apply one side's runtime API to the
other:

- **PULL side has per-peer add/remove**:
  `queue.add_producer_peer(...)` and `queue.remove_producer_peer(...)`
  let the role-host framework register a new producer or evict a
  departed one without resetting the rest of the peer set.  Used by
  the runtime CHANNEL_NOTIFY broadcast path (§4.6.1).
- **PUSH side is allowlist-only, snapshot-replace**: no per-peer
  add/remove on the PUSH side.  Allowlist refreshes are always full
  snapshot replacements via `set_peer_allowlist`.  Rationale: the
  producer-side ZAP handler enforces a flat pubkey set, not
  per-connection state — there's nothing to "add" beyond writing a
  new snapshot.  See HEP-CORE-0036 §6.5 design rationale.
- **Two caches, one per script-observable + one per ZAP**: the
  PUSH-side `set_peer_allowlist` write feeds the local ZAP cache
  (transport-specific enforcement); the SAME wire event also writes
  the script-observable `RoleAPIBase::allowlist_cache` which is
  transport-agnostic.  For the cache architecture (which cache, who
  reads, who writes, who is the authority) see HEP-CORE-0036 §I11.1.
  SHM channels reuse the same script-observable cache but have NO
  local enforcement cache — broker pre-confirm per attach is the
  gate (HEP-CORE-0041 §9 D4).

#### Script visibility — both sides

Scripts never see this API.  They see only `queue.write_acquire()`
/ `queue.write_commit()` (producer) or `queue.read_acquire()` /
`queue.read_release()` (consumer).  Slots route through whatever
peer set the framework has populated; the underlying ZMQ socket
multiplexes per its native semantics — round-robin among
connected PUSH peers on the PULL side, load-balanced across
connected PULL consumers on the PUSH side.

**Pattern-neutrality**: HEP-CORE-0017 does NOT specify whether
ZmqQueue uses Pattern A (PULL binds, peers' PUSH connect to it) or
Pattern B (PULL connects to each peer's bound PUSH endpoint).  Both
are valid implementations of the multi-producer surface; the
choice is internal to ZmqQueue.  The current code uses Pattern B
(consumer's PULL connects per producer endpoint); future
implementations may switch to Pattern A for simpler dynamic-membership
semantics without changing this interface.

**ShmQueue parallel**: for SHM transport `producer_peers.size() ≤ 1`
(SHM is physically single-producer per HEP-CORE-0007 §12.4a
`MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`).  `add_producer_peer` /
`remove_producer_peer` on a ShmQueue are no-ops post-attach;
ShmQueue lifecycle is bound to the single DataBlock attach.

> **SHM auth attaches via a different mechanism — not via this surface.**
> Both transports share the same control-plane registration + NOTIFY
> flow (canonical side-by-side at HEP-CORE-0036 §3.6); they diverge
> only in the data-plane attach step.  ZMQ consumers populate
> `producer_peers` from `CONSUMER_REG_ACK.producers` and the PULL
> socket connects to each.  SHM consumers do NOT use this queue-level
> peer surface for authorization.  Instead, on
> `apply_consumer_reg_ack` the consumer reads `shm_capability_endpoint`
> + `producer_pubkey_z85` from the ACK, dials the producer's Unix
> socket, runs the `crypto_box` challenge-response (HEP-CORE-0041
> §5.5), and receives an SHM fd via `SCM_RIGHTS`.  The ShmQueue is
> then handed that fd and never sees the legacy ZMQ-style
> apply_master_approval allowlist path.  See HEP-CORE-0041 §9 D4 for
> the full sequence.

### 3.4 Processor role transform loop

| Property | Value |
|---|---|
| Direction | Read-from-one-Queue, write-to-another-Queue |
| Data transport | Any QueueReader / QueueWriter combination (SHM, ZMQ, or mixed) |
| Channel ownership | Both sides — input via `RoleAPIBase::build_rx_queue`, output via `build_tx_queue` |
| Control plane | One BrokerRequestComm per hub the processor participates in (1 in single-hub deployment after dedup; 2 in dual-hub — see HEP-CORE-0033 §19) |
| Timing | Inner retry on `read_acquire(short_timeout)`; deadline-driven outer loop per `LoopTimingPolicy` (HEP-CORE-0008) |
| Lives on | Any host — transport constraints are per-Queue, not per-role |

The processor's transform body is implemented by `ProcessorCycleOps`
(see `src/utils/service/cycle_ops.hpp`) plugged into the shared
`run_data_loop` template (`src/utils/service/data_loop.hpp`).  Per
cycle: acquire input slot → conditionally acquire output slot → call
the script's `on_process(rx, tx, msgs, api)` → commit or discard
output → release input.  The cycle ops know nothing about schema,
broker protocol, or Python GIL — those concerns are handled by
`ProcessorRoleHost` (in `src/processor/`), which builds both queues
via `RoleAPIBase` and owns the BRC(s) + ctrl thread(s).

(The pre-L3.γ standalone `hub::Processor` C++ class — which took
`Queue&` references and ran independently of any role host — was
retired in L3.γ A6.3, 2026-03-01.  Today the role host IS the
processor.)

### 3.5 Operation modes

(Historical note: the pre-L3.γ `Producer` / `Consumer`
classes had two operation modes — "standalone" with internal threads
and "embedded" external-poll.  After L3.γ A6.3 (2026-03-01) those
classes were retired and the role-host is the single owner of every
thread it needs; "embedded" is no longer a separate mode of the data-
plane object.  The text below describes the historical contract, kept
for context — current behaviour is "the role host's threads", with
ThreadManager bounded join.)

**Standalone mode** (`start()` / `stop()`): The object launches its own internal
threads. Producer spawns `peer_thread` (ctrl socket polling, peer-dead check) and
`write_thread` (SHM slot processing). Consumer spawns `ctrl_thread`, `data_thread`,
and `shm_thread`. External code must not call `handle_*_events_nowait()` or obtain
socket handles — those sockets are owned by the internal threads.

**Embedded mode** (`start_embedded()` + `handle_*_events_nowait()`): No threads are
launched. The calling code drives ZMQ polling from its own thread, inserting
`handle_peer_events_nowait()` / `handle_ctrl_events_nowait()` / `handle_data_events_nowait()`
into its own poll loop. Raw socket handles are available via `peer_ctrl_socket_handle()` /
`ctrl_zmq_socket_handle()` / `data_zmq_socket_handle()` for use in `zmq::poll()`.
All four standard binaries use embedded mode internally.

**Critical invariant**: In embedded mode, all socket-touching calls (`handle_*`,
socket-handle accessors, `pull()`, `synced_write()`) must be issued from the
**same thread** that owns the poll loop. ZMQ sockets are not thread-safe.

**Callback registration ordering**: All `on_*` callbacks (`on_peer_dead`,
`on_consumer_joined`, `on_channel_closing`, etc.) must be registered before
`start()` or before the first `handle_*_events_nowait()` call. Registering after
the poll loop has started creates a data race on the callback storage.

For detailed API usage, threading diagrams, and worked examples including a custom
data recorder, a hardware acquisition loop, and a dual-hub bridge, see
`docs/README/README_EmbeddedAPI.md`.

---

## 4. Topology Patterns

### 4.0 Topology Overview

```mermaid
graph TD
    subgraph "§4.1 Local Linear"
        A1["Producer"] -->|SHM| A2["Consumer"]
    end

    subgraph "§4.2 Local Transform"
        B1["Producer"] -->|SHM| B2["ShmQ"] --> B3["Processor"] --> B4["ShmQ"] -->|SHM| B5["Consumer"]
    end

    subgraph "§4.3 Cross-Machine Bridge"
        C1["Producer"] -->|SHM| C2["Proc A"] -->|ZMQ net| C3["Proc B"] -->|SHM| C4["Consumer"]
    end

    subgraph "§4.4 Chained Transform"
        D1["Producer"] -->|SHM| D2["P1"] -->|SHM| D3["P2"] -->|SHM| D4["P3"] -->|SHM| D5["Consumer"]
    end

    subgraph "§4.5 Fan-Out"
        E1["Producer"] -->|SHM| E2["Consumer"]
        E1 -->|SHM| E3["Processor A"]
        E1 -->|SHM| E4["Processor B"]
    end
```

### 4.1 Local Linear Pipeline

All components on the same host, single broker:

```
  Producer ──[SHM]──► Consumer
```

- Producer creates the SHM segment and registers with broker.
- Consumer attaches and registers as reader.
- Both use `LoopPolicy` for timing; both have full spinlock access.
- Broker coordinates registration and monitors health via heartbeat.

This is the baseline topology. No Queue, no Processor involved.

### 4.2 Local Transform Pipeline

All components on the same host; Processor transforms in-line:

```
  Producer ──[SHM]──► ShmQueue(read) ──► Processor ──► ShmQueue(write) ──[SHM]──► Consumer
```

- Producer and Consumer own their SHM segments and broker registrations.
- The processor's transform body (`ProcessorCycleOps`) is a pure
  data-plane unit: it knows nothing about brokers or schemas.
- When the processor runs as `plh_role --role processor`,
  `ProcessorRoleHost` builds the input + output queues via RoleAPIBase
  and owns the broker connection(s); the cycle ops then run inside
  the shared `run_data_loop` template.

### 4.3 Cross-Machine Bridge

Processor bridges SHM on Machine A to SHM on Machine B via ZMQ:

```
Machine A:
  Producer ──[SHM]──► ShmQueue(read) ──► Processor(bridge A) ──► ZmqQueue(write) ──[net]──►

Machine B:
  ──[net]──► ZmqQueue(read) ──► Processor(bridge B) ──► ShmQueue(write) ──[SHM]──► Consumer
```

Each bridge Processor:
- On Machine A: `in_transport=shm`, `out_transport=zmq`
- On Machine B: `in_transport=zmq`, `out_transport=shm`
- Has a pass-through handler (copy in_slot → out_slot) or a lightweight transform
- Maintains separate control-plane connections to its respective broker(s)

`Producer` and `Consumer` at both ends are unchanged — they remain
SHM-local and expose the full set of SHM-specific facilities to their callers.

A runnable 6-process dual-hub bridge demo (2 hubs, 1 producer, 2 processors, 1 consumer)
is provided at `share/py-demo-dual-processor-bridge/`. Run with `bash share/py-demo-dual-processor-bridge/run_demo.sh`.

### 4.4 Chained Transform Pipeline

Multiple Processors in series for staged processing:

```
  Producer ──[SHM]──► P1(normalize) ──[SHM]──► P2(filter) ──[SHM]──► P3(compress) ──[SHM]──► Consumer
```

Each Processor reads from one SHM channel and writes to the next.  Each runs as
its own `plh_role --role processor` instance with its own ProcessorRoleHost,
its own broker registration(s), and its own `on_process` script callback.  The
intermediate SHM segments have no direct Producer/Consumer — they are created
by P1/P2/P3 as their output channels.

Note: In this topology each intermediate channel has only one writer (Processor Pn)
and one reader (Processor Pn+1). The Producer/Consumer heartbeat model handles
liveness for each hop.

### 4.5 Fan-Out Pipeline (topology: `"fan-out"`)

One producer, multiple consumers.  Post-2026-07-08 topology
migration, this is one of three explicitly-declared topologies:

```
                                       ──► plh_role --role consumer  (local monitor)
  plh_role --role producer ──[SHM/ZMQ]──► plh_role --role processor (archive)
                                       ──► plh_role --role processor (analysis, cross-machine)
```

**Binding side:** producer (owns the `data_endpoint`).  Under ZMQ,
producer binds a PUB socket; consumers connect SUB and subscribe
with an empty topic filter to receive the full stream.  Under SHM,
producer creates the DataBlock and binds a capability-transport
socket; consumers attach via AttachProtocol (HEP-CORE-0044).

**Cardinality:** exactly 1 producer, 1..N consumers.  Broker
rejects a second `REG_REQ` with `FAN_OUT_IS_SINGLE_PRODUCER`
(HEP-CORE-0007 §12.4a).  Each consumer registers independently;
the producer's live-consumer set is delivered via
`CHANNEL_AUTH_CHANGED_NOTIFY(phase=live, role_type="consumer")` per
consumer.

**Channel life:** channel exists as long as the producer (binding
side) is alive.  Producer death → channel torn down; all consumers
receive `CHANNEL_CLOSING_NOTIFY`.

### 4.5a One-to-One Pipeline (topology: `"one-to-one"`)

Exactly one producer, exactly one consumer.  Introduced 2026-07-08
as a distinct topology from fan-out because the broker enforces
cardinality-1 on BOTH sides (a second consumer's REG_REQ is rejected
with `ONE_TO_ONE_CARDINALITY_VIOLATED`).

```
  plh_role --role producer ──[SHM/ZMQ]──► plh_role --role consumer
```

**Binding side:** producer (same convention as fan-out).  Under ZMQ
uses PUSH bind / PULL connect (no PUB/SUB overhead needed for a
single subscriber).  Under SHM: same DataBlock + capability model as
fan-out.

**Channel life:** same as fan-out — producer death → channel dies.

### 4.6 Fan-In Pipeline (topology: `"fan-in"` — ZMQ only)

Multiple producers, exactly one consumer.  The 2026-07-08 topology
migration made this an explicitly-declared topology with the
CONSUMER as the binding side:

```
  plh_role --role producer (sensor A) ─┐
                                       ├─[ZMQ]──► plh_role --role consumer (aggregator)
  plh_role --role producer (sensor B) ─┘
```

**Binding side:** consumer (owns the `data_endpoint`).  Consumer
binds PULL; producers connect PUSH.  This is the inverse of fan-out —
because the singular side under fan-in is the consumer, the consumer
is what stays put while producers may come and go.  libzmq's PULL
socket fair-queues messages from all connected PUSH peers natively;
no framework-level fair-queue accounting needed.

**Cardinality:** 1..N producers, exactly 1 consumer.  A second
`CONSUMER_REG_REQ` is rejected with `FAN_IN_IS_SINGLE_CONSUMER`
(HEP-CORE-0007 §12.4a).  Each producer registers independently.

Each producer issues its own `REG_REQ` on the same `channel_name`
with `channel_topology: "fan-in"`.  The broker admits producers as
additional `ProducerEntry` on `ChannelEntry.producers`.  All producers
MUST agree on the channel-wide schema invariant (`schema_hash` /
`schema_blds` / `packing`); REG_REQ that fails this gate is rejected
with `SCHEMA_MISMATCH`.

Cross-tag producers are allowed — a processor's `out_channel` makes
it a producer side, so `prod.X` + `proc.Y` may both be producers of
the same channel (e.g. data sensor X interleaved with a synthetic
data injector that is itself a processor's output).

**Channel life:** channel exists as long as the CONSUMER (binding
side) is alive.  Consumer death → channel torn down; all producers
receive `CHANNEL_CLOSING_NOTIFY`.  Individual producer drops do NOT
close the channel; those are just live_peers decrements.
**Amended 2026-07-08:** the pre-migration rule ("LAST producer's
transition to Disconnected triggers atomic teardown") is superseded
by the binding-side rule.  Under fan-in the binding side is the
consumer; under fan-out and 1-to-1 the binding side is the producer.
See HEP-CORE-0023 §2.1.1 for the generalized rule.

**SHM + fan-in is unsupported** — the shared-memory ring buffer is
physically bound to one writer.  The broker rejects `channel_topology:
"fan-in"` with `data_transport: "shm"` at REG_REQ entry with
`TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT` (HEP-CORE-0007 §12.4a).  The
pre-2026-07-08 `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM` code stays live
for legacy handlers.

> **Transport-agnostic principle.**  Role/hub code operates against
> the abstract queue base classes (HEP-CORE-0008 `QueueReader` /
> `QueueWriter`).  Whether a channel admits N producers is a
> queue-pattern property of the chosen transport, not a control-
> plane assumption — the broker-side bookkeeping (this section's
> Fan-In) matches the queue's natural admit-count.

#### 4.6.1 Dynamic membership under HEP-CORE-0036 (post-2026-07-08 topology migration)

Under the binding/dialing model, dynamic peer membership is handled
by the binding side maintaining a local `live_peers[channel]` map
fed by `CHANNEL_AUTH_CHANGED_NOTIFY` events from the broker (HEP-CORE-0007
§12.5 for the wire schema; HEP-CORE-0036 §6.5 for semantics).

**Fan-in binding side (consumer) flow:**

0. **Initial setup at S3.**  Consumer receives `CONSUMER_REG_ACK`
   (empty of dial targets — under fan-in the consumer is BINDING).
   Queue picks PULL socket, binds ephemeral endpoint, enters Configured.
   `ENDPOINT_UPDATE_REQ` publishes the resolved endpoint.  Heartbeat
   task starts; broker marks consumer Live.
1. **Broker** is the authoritative source of `ChannelEntry::producers[]`
   allowlist.  On dialing-side (producer) REG_REQ admission, broker
   fires `CHANNEL_AUTH_CHANGED_NOTIFY(phase=admitted, role_uid=P,
   role_type="producer")` to the consumer.  On producer first
   heartbeat, broker fires `phase=live` NOTIFY.  On DEREG /
   heartbeat-timeout / revocation, broker fires `phase=left`.
2. **Role-host framework** receives NOTIFY on BRC poll thread:
   - `phase=admitted` → pulls `GET_CHANNEL_AUTH_REQ`, applies to
     ZAP via `queue->set_peer_allowlist(...)`, sends
     `CHANNEL_AUTH_APPLIED_REQ`.  May wake R6-pending producer
     REG_REQs on the broker.
   - `phase=live` → local update to `live_peers[channel]` set.
     `api.producer_count(channel)` reflects the new size.  No wire
     round-trip; does not wake R6.
   - `phase=left` → removes from both `zap_allowlist` and
     `live_peers` maps.  Applies to ZAP.
3. **ZmqQueue** handles the underlying ZAP allowlist updates.  The
   PULL socket itself needs no per-peer `connect()` — libzmq's PULL
   accepts all connected PUSH peers admitted by ZAP.  No
   `add_producer_peer` / `remove_producer_peer` calls; those APIs
   retire (superseded by ZAP allowlist updates).
4. **Script** sees the new peer set via
   `api.producer_count(channel)` / `api.producers(channel)`
   accessors (HEP-CORE-0028 amendment).  `api.rx.acquire()` keeps
   returning the next slot from any producer currently admitted.

**Fan-out / 1-to-1 binding side (producer) flow:** symmetric with
producer↔consumer swapped.  Producer's PUB (fan-out ZMQ) or PUSH
(1-to-1 ZMQ) or DataBlock capability endpoint (SHM) is the binding
target.  Consumers dial in; broker fires
`CHANNEL_AUTH_CHANGED_NOTIFY(phase=..., role_type="consumer")` per
consumer to the producer.  `api.consumer_count(channel)` /
`api.consumers(channel)` accessors expose the live consumer set.

**Dialing side (across all topologies):** no runtime peer-set
tracking needed.  The dialing side has exactly one peer (the
binding side), addressed by `data_endpoint` + `data_pubkey` from
the REG_ACK.  If the binding side dies, the whole channel dies
(dialing side receives `CHANNEL_CLOSING_NOTIFY` — see §4.6 channel-
life rule).

This three-tier separation is required and load-bearing:

| Tier | Responsibility | What it knows |
|---|---|---|
| Broker (`HubState`) | Authoritative state + broadcasts | The truth |
| Framework (role-host) | Event routing + queue updates | Channel + peer descriptors + `live_peers` map |
| Queue (Zmq/ShmQueue) | Transport plumbing | Sockets, bind/connect (per topology matrix), ZAP allowlist, subscription |
| Script | Application logic | Queue read/write API + peer-count accessors |

Cross-references: HEP-CORE-0036 §3 (I9 invariant — three-tier
separation); HEP-CORE-0036 §6.5 (allowlist + readiness NOTIFY chain
with phase field); HEP-CORE-0028 (script API accessors); HEP-CORE-0007
§12.3 (`REG_ACK.data_endpoint` schema) + §12.5
(`CHANNEL_AUTH_CHANGED_NOTIFY.phase` schema).

---

#### 4.6.1-retired — Pre-2026-07-08 dynamic membership (SUPERSEDED; archaeological reference)

The material below described the pre-migration model where each
consumer maintained a per-producer `ProducerPeer` vector and applied
runtime `add_producer_peer` / `remove_producer_peer` on
`CHANNEL_PRODUCERS_CHANGED_NOTIFY` events (with `GET_CHANNEL_PRODUCERS_REQ`
as the follow-up pull).  Both wire messages retire 2026-07-08 (HEP-CORE-0007
§12.3, §12.5).  Retained for archaeological reference:

Under the pre-migration HEP-CORE-0036, the producer set was dynamic —
producers joined and left during the channel's lifetime.  The flow
that kept the consumer's queue in sync with the channel's current
producer set relied on `producers[]` array in `CONSUMER_REG_ACK`
(retired 2026-07-08 per HEP-CORE-0007 §12.3) and the
`CHANNEL_PRODUCERS_CHANGED_NOTIFY` → `GET_CHANNEL_PRODUCERS_REQ`
notify-then-pull chain (retired 2026-07-08 per HEP-CORE-0007 §12.5).
Runtime `add_producer_peer` / `remove_producer_peer` calls applied
each change to the queue's per-peer `connect()` list.  The
`ProducerPeer` vector structure in `RxQueueOptions::producer_peers`
retires alongside those wires.

---

## 5. Schema Integration

### 5.1 Schema at Pipeline Construction Time

When constructing a pipeline, each channel endpoint declares its expected slot layout.
This layout drives:

1. **C++ struct size** passed as `item_size` to `ShmQueue::from_consumer/producer()`
2. **BLDS string + packing** registered with the broker in `REG_REQ`
   (HEP-CORE-0034 §10.1; fingerprint = BLAKE2b-256 over canonical fields + packing)
3. **Schema record key** `(schema_owner, schema_id)` carried in `REG_REQ`
   (HEP-CORE-0034 §4) — owner is `hub` for adoption of a hub-global, or self
   for a producer registering its own private schema

Schema validation occurs at the control-plane level (broker checks fingerprint
match against the cited schema record) before the data plane is even established.
By the time `read_acquire()` returns the first slot, the schema contract has
already been verified.

### 5.2 Schema Ownership and Citation (HEP-CORE-0034)

Every schema in flight is owned by exactly one party — either a registered
producer (private schema) or the hub itself (`<hub_dir>/schemas/` global).
Records are keyed `(owner_uid, schema_id)`; two producers may both register a
private schema named `frame` without colliding (HEP-CORE-0034 §8 namespace-by-owner).

| Citation path | Wire payload | Authority check |
|---|---|---|
| **Path A — cite by id** | `schema_owner` + `schema_id` + expected hash | Hub looks up record; owner must equal channel authority |
| **Path B — register-and-cite** (producer only) | `schema_owner=self` + `schema_id` + BLDS + hash + packing | Hub creates record under producer ownership |
| **Path C — adopt hub global** (producer only) | `schema_owner="hub"` + `schema_id` | Hub validates global exists with matching fingerprint |

Cross-citation (citing a third role's schema while connecting to a different
producer) is rejected even when fingerprints match — see HEP-CORE-0034 §9.1.

### 5.3 C++ Type ↔ Schema Linkage

In C++ code, struct registration via `PYLABHUB_SCHEMA_BEGIN/MEMBER/END` macros
produces a `SchemaInfo` that contains the BLDS string, packing, and the
fingerprint hash (BLAKE2b-256 over canonical fields + packing per HEP-0034
§6.3). The hash is passed in `ProducerOptions::schema_hash` — the struct IS
the schema at compile time. No runtime JSON parsing is involved.

For scripted roles (Python), `SchemaLibrary` (HEP-CORE-0034 §4 — stateless
file loader) reads named schema JSON and produces records that match the
runtime ctypes layout exactly (alignment determined by the explicit `packing`
field, no longer inferred). See HEP-CORE-0034 §6 for the JSON file format.

---

## 6. Deployment Model

### 6.1 Binary Interconnection

```mermaid
graph LR
    subgraph PLH_HUB["plh_hub"]
        BROKER["BrokerService<br/>(ROUTER)"]
        ADMIN["AdminService<br/>(ZMQ REP)"]
        HRUNNER["HubScriptRunner<br/>(EngineHost&lt;HubAPI&gt;)"]
    end

    subgraph PROD["plh_role --role producer"]
        P_RH["ProducerRoleHost"]
        P_API["RoleAPIBase + ProducerAPI"]
        P_TXQ["Tx queue<br/>(Shm or Zmq)"]
        P_BRC["BrokerRequestComm"]
    end

    subgraph PROC["plh_role --role processor"]
        PR_RH["ProcessorRoleHost"]
        PR_API["RoleAPIBase + ProcessorAPI"]
        PR_RXQ["Rx queue<br/>(in side)"]
        PR_TXQ["Tx queue<br/>(out side)"]
        PR_BRC["BrokerRequestComm<br/>(per presence in dual-hub —<br/>see HEP-0033 §19)"]
    end

    subgraph CONS["plh_role --role consumer"]
        C_RH["ConsumerRoleHost"]
        C_API["RoleAPIBase + ConsumerAPI"]
        C_RXQ["Rx queue"]
        C_BRC["BrokerRequestComm"]
    end

    P_BRC -->|ctrl| BROKER
    C_BRC -->|ctrl| BROKER
    PR_BRC -->|ctrl| BROKER

    P_TXQ ===|SHM/ZMQ| C_RXQ
    P_TXQ ===|SHM/ZMQ| PR_RXQ
    PR_TXQ ===|SHM/ZMQ| C_RXQ
```

### 6.2 Binary Types

The framework ships two user-facing binaries.  `plh_role` is
parameterised by `--role <tag>`; `plh_hub` is the single hub binary.

| Binary invocation | Identity format | Config file | Description |
|---|---|---|---|
| `plh_hub <hub_dir>` | `hub.{name}.{8hex}` | `hub.json` | Data hub: broker service + AdminService + hub script runtime (HEP-CORE-0033 §15) |
| `plh_role --role producer <role_dir>` | `prod.{name}.{8hex}` | `producer.json` | Producer role: writes to one channel |
| `plh_role --role consumer <role_dir>` | `cons.{name}.{8hex}` | `consumer.json` | Consumer role: reads from one channel |
| `plh_role --role processor <role_dir>` | `proc.{name}.{8hex}` | `processor.json` | Processor role: reads from in-side, transforms, writes to out-side; may bridge two hubs (HEP-CORE-0033 §19) |

Each launched instance:
- Owns its directory: one config file, one vault, one script package, one PID lock
- Has exactly one UID — immutable after generation
- Hosts exactly one role kind (selected by `--role` for `plh_role`)

### 6.3 Configuration Hierarchy

```
<hub_dir>/
  hub.json           ← plh_hub config (broker, admin, script, vault)
  vault/             ← Encrypted keypair store (HubVault)
  script/
    python/
      __init__.py    ← on_start(api) / on_tick(api, tick) / on_stop(api)
  logs/
  run/

<producer_dir>/
  producer.json      ← producer name, channel, broker, schema, script
  vault/             ← Encrypted keypair store (RoleVault)
  script/
    python/
      __init__.py    ← on_init(api) / on_produce(out_slot, fz, msgs, api) / on_stop(api)
  logs/
  run/
    producer.pid

<consumer_dir>/
  consumer.json      ← consumer name, channel, broker, schema, script
  vault/             ← Encrypted keypair store (RoleVault)
  script/
    python/
      __init__.py    ← on_init(api) / on_consume(in_slot, fz, msgs, api) / on_stop(api)
  logs/
  run/
    consumer.pid

<processor_dir>/
  processor.json     ← processor name, in_channel, out_channel, broker(s), schema, script
  vault/             ← Encrypted keypair store (RoleVault)
  script/
    python/
      __init__.py    ← on_init(api) / on_process(in_slot, out_slot, fz, msgs, api) / on_stop(api)
  logs/
  run/
    processor.pid
```

**Script path rule (all four components):**
`script.type` is required; C++ resolves `<script.path>/<script.type>/__init__.py`.
Standard config: `"script": {"type": "python", "path": "./script"}`.

### 6.4 Deployment Topology Examples

**Single-machine pipeline:**
```
<hub_dir>/hub.json          ← one broker
<producer_dir>/producer.json  ← PROD-SENSOR-A3F7C219, connects to hub
<consumer_dir>/consumer.json  ← CONS-LOGGER-B7E3A142, connects to hub
```

**Multi-channel transform pipeline (same machine):**
```
<hub_dir>/hub.json
<sensor_producer>/producer.json    ← writes lab.raw.temperature
<norm_processor>/processor.json    ← reads lab.raw.temperature, writes lab.norm.temperature
<archive_consumer>/consumer.json   ← reads lab.norm.temperature
<monitor_consumer>/consumer.json   ← reads lab.norm.temperature (second consumer, same channel)
```

**Cross-machine bridge:**
```
Machine A: <hub_a>/hub.json + <sensor_producer>/producer.json
           <bridge_proc_a>/processor.json  (in=shm:lab.raw, out=zmq:bridge_endpoint)

Machine B: <hub_b>/hub.json
           <bridge_proc_b>/processor.json  (in=zmq:bridge_endpoint, out=shm:lab.norm)
           <archive_consumer>/consumer.json
```

Each process has its own directory and UID regardless of which machine it runs on.
Multi-machine coordination is via broker registrations, not shared config directories.

---

## 7. Design Invariants

The following invariants must hold across all topology patterns and all future
component additions:

1. **The broker never relays data.** All data bytes flow directly between endpoints
   via SHM or ZMQ point-to-point. The broker only coordinates registration and
   monitors liveness.

2. **Data plane and control plane are independent.** The data
   transport (SHM or ZMQ) is selected per-side via `in_transport` /
   `out_transport` in the role config; ZMQ-side transports work
   cross-machine while SHM-side transports require same-host
   placement.  The role's control-plane connection to the broker
   (the ctrl thread inside `RoleAPIBase`) is always active
   regardless of data transport.  Cross-machine SHM flow is achieved
   either via bridge processors (§4.3) or via `in_transport=zmq` on
   the consumer when the producer uses `out_transport=zmq`.

3. **hub::QueueReader and hub::QueueWriter carry the data plane only.** Queue
   implementations do not manage control-plane sockets, BrokerRequestComm
   instances, or LoopPolicy. Those are the responsibility of the component
   that owns the Queue (today: `RoleAPIBase` for the data-plane queue
   handles; the role host's derived class for the BRC).  The old
   `hub::Queue` combined class is eliminated; access-mode enforcement is
   by type at compile time.

4. **The checksum is the schema truth.** The broker always validates the BLAKE2b-256
   hash of the BLDS string. Schema names are human-readable aliases; they never
   override the structural hash check.

5. **Explicit transport selection, fail-fast.** Transport is declared in config, not
   inferred. A mismatch between declared transport and broker-advertised availability
   is a hard startup failure with a diagnostic message. Silent fallback is not allowed.

6. **Control-plane connections are per-broker.** A Processor using two brokers
   maintains two independent control-plane connections. There is no multiplexing or
   proxying at the control-plane level.

---

## 8. Cross-Reference Index

| Topic | Authoritative document |
|-------|----------------------|
| SHM memory layout, ring buffer, slot state machine | HEP-CORE-0002 |
| Five planes (data/control/message/timing/metrics) rationale | HEP-CORE-0002 §17, HEP-CORE-0019 |
| Producer/Consumer SHM-specific design principle | HEP-CORE-0002 §17.2 |
| hub::Queue data plane abstraction detail | HEP-CORE-0002 §17.3 |
| HELLO/BYE/REG/DISC/HEARTBEAT protocol | HEP-CORE-0007 |
| LoopPolicy and iteration metrics | HEP-CORE-0008 |
| Connection policy (ConsumerSyncPolicy, etc.) | HEP-CORE-0009 |
| Script engine abstraction (RoleHostBase, RoleAPIBase, ScriptEngine) | HEP-CORE-0011 |
| Channel identity and UID provenance | HEP-CORE-0013 |
| Role binary unification (`plh_role --role <tag>`) | HEP-CORE-0024 (supersedes HEP-CORE-0018 producer+consumer binaries and HEP-CORE-0015 processor binary) |
| Hub binary (`plh_hub`) + AdminService + HubScriptRunner | HEP-CORE-0033 |
| Schema records, ownership, citation rules | HEP-CORE-0034 (supersedes HEP-CORE-0016) |
| Inbox messaging (point-to-point ROUTER/DEALER side channel) | HEP-CORE-0027 |
| Metrics plane (per-presence keying — see HEP-0019 §2.3) | HEP-CORE-0019 |
| Startup coordination + heartbeat-derived liveness timeouts | HEP-CORE-0023 |

---

## 9. Source File Reference

This is an architecture overview document. The source files that implement each component:

| Component | Key Source Files |
|---|---|
| **hub::QueueReader / hub::QueueWriter** | `src/include/utils/hub_queue.hpp` (abstract bases; the historical `Producer` / `Consumer` wrapper classes were retired in L3.γ A6.3, 2026-03-01 — the data plane is now reached via `RoleAPIBase`'s `build_tx_queue()` / `build_rx_queue()`) |
| **hub::ShmQueue** | `src/include/utils/hub_shm_queue.hpp`, `src/utils/hub/hub_shm_queue.cpp` |
| **hub::ZmqQueue** | `src/include/utils/hub_zmq_queue.hpp`, `src/utils/hub/hub_zmq_queue.cpp` |
| **hub::InboxQueue / hub::InboxClient** | `src/include/utils/hub_inbox_queue.hpp`, `src/utils/hub/hub_inbox_queue.cpp` (HEP-CORE-0027) |
| **BrokerService** | `src/include/utils/broker_service.hpp`, `src/utils/ipc/broker_service.cpp` |
| **BrokerRequestComm** | `src/include/utils/broker_request_comm.hpp`, `src/utils/network_comm/broker_request_comm.cpp` (role-side DEALER + ctrl-thread poll loop) |
| **plh_hub binary** | `src/plh_hub/plh_hub_main.cpp` + `src/plh_hub/CMakeLists.txt` (HEP-CORE-0033 §15) |
| **plh_role binary** | `src/plh_role/plh_role_main.cpp` + per-role `worker_main_()` in `src/{producer,consumer,processor}/*_role_host.cpp` (HEP-CORE-0024) |
| **RoleAPIBase / RoleHostCore / EngineHost** | `src/include/utils/role_api_base.hpp` + `src/include/utils/role_host_core.hpp` + `src/include/utils/engine_host.hpp` (HEP-CORE-0011) |
| **HubState** | `src/include/utils/hub_state.hpp` — authoritative role + channel + schema registry (HEP-CORE-0033 §8 + HEP-CORE-0034) |
| **schema_loader** | `src/include/utils/schema_loader.hpp`, `src/utils/schema/schema_loader.cpp` (stateless file parsers; HEP-CORE-0034 §2.4 I5) |
| **LoopPolicy** | `src/include/utils/loop_timing_policy.hpp` (loop cadence enum + math) |

---

## Document Status

**Implemented** — 2026-03-03

This document captures architectural decisions made during the Queue Abstraction and
Processor design phase (2026-03-01). All components described here are implemented:
unified `plh_role` binary (HEP-CORE-0024), `plh_hub` binary (HEP-CORE-0033),
QueueReader / QueueWriter abstraction (`hub::ShmQueue` + `hub::ZmqQueue`),
Schema Registry (HEP-CORE-0034), and the five-plane architecture.  The
historical `hub::Producer`, `hub::Consumer`, `hub::Processor`, and `hub::Queue`
wrapper classes were retired in L3.γ A6.3 (2026-03-01); their roles are now
fulfilled by `RoleAPIBase::build_tx_queue` / `build_rx_queue` (data plane) +
`ProcessorRoleHost` (transform).

**Resolved topics:**
- HEP-CORE-0018: producer + consumer binaries — superseded by HEP-CORE-0024 unification
- HEP-CORE-0015: processor binary — superseded by HEP-CORE-0024 unification
- HEP-CORE-0016: Named Schema Registry — superseded by HEP-CORE-0034 (owner-authoritative model, 2026-04-26)

**Pending extension:**
- HEP-CORE-0019: Metrics Plane — fifth plane (implemented 2026-03-05; 19 tests)
