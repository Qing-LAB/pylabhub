# HEP-CORE-0027: Inbox Messaging

**Status**: Implemented (documenting existing system); CURVE wiring deferred to HEP-CORE-0036 Phase 4+ (see §3.5 below).  Reachability + multi-hub advertisement section (§4.5) added 2026-05-06 to align with HEP-CORE-0033 §19 (multi-presence roles, planned — Wave A item A7) and HEP-CORE-0019 §2.3 (Phase 6 per-presence heartbeats).
**Created**: 2026-03-27
**Scope**: InboxQueue, InboxClient, peer-to-peer messaging side channel
**Depends on**: HEP-CORE-0007 §12.4 (ROLE_INFO_REQ/ACK), HEP-CORE-0034 §11.4 (inbox-as-schema-record), HEP-CORE-0033 §8 (HubState entry types — `ChannelEntry` / `ConsumerEntry` hold per-presence inbox metadata), HEP-CORE-0033 §18 (broker routing classes — ROLE_INFO_REQ is Class B), HEP-CORE-0033 §19 (multi-presence roles — drives per-presence inbox advertisement), HEP-CORE-0036 §9.3 (CURVE wiring on inbox sockets — role identity keypair + per-channel allowlist inheritance)

---

## 1. Motivation

The main data queue (SHM or ZMQ) is a one-to-many broadcast channel: one producer writes,
multiple consumers read. It is optimized for high-throughput, unidirectional streaming.

However, roles often need to exchange small, targeted messages with specific peers:
- A consumer sending calibration parameters to a producer
- A processor requesting a configuration change from another processor
- An orchestrator coordinating startup sequence across roles

These use cases require:
- **Point-to-point** addressing (to a specific role, not broadcast)
- **Bidirectional** communication (request + acknowledgement)
- **Schema-validated** payloads (same typed slot model as the data queue)
- **Independence** from the data plane (inbox messages don't interfere with data flow)

The Inbox provides this as an optional side channel, available to all roles.

---

## 2. Architecture

### 2.1 Component Model

```
Role A (sender)                          Role B (receiver)
┌────────────────┐                       ┌────────────────────┐
│                │                       │                    │
│  InboxClient   │  ZMQ DEALER ────────► │  InboxQueue        │
│  (DEALER)      │  ◄──── ACK ───────── │  (ROUTER)          │
│                │                       │                    │
│  Acquired via  │                       │  Bound at startup  │
│  api.open_     │                       │  inbox_thread_     │
│  inbox(uid)    │                       │  receives + ACKs   │
└────────────────┘                       └────────────────────┘
        │                                         │
        │ ROLE_INFO_REQ                          │ REG_REQ
        │ (discover endpoint)                    │ (advertise endpoint)
        ▼                                         ▼
    ┌──────────┐                             ┌──────────┐
    │  Broker  │ ◄─── endpoint metadata ───► │  Broker  │
    └──────────┘                             └──────────┘
```

- **InboxQueue** (ROUTER): Binds a ZMQ ROUTER socket. Receives typed messages from
  any connected DEALER. Sends ACK after processing. One per role (optional).
- **InboxClient** (DEALER): Connects to a remote InboxQueue. Fills a typed buffer,
  sends, waits for ACK. Created on demand via `api.open_inbox(target_uid)`.
- **Broker**: Stores inbox metadata (endpoint, schema, packing) from REG_REQ.
  Serves it via ROLE_INFO_REQ. Not involved in actual message flow.

### 2.2 Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Direct P2P (no broker relay) | Low latency; broker is not a bottleneck for inbox traffic |
| ROUTER/DEALER (not PUB/SUB) | Need per-sender identity for ACK routing |
| Request/ACK (not fire-and-forget) | Sender knows if message was processed |
| Same wire format as ZmqQueue | Reuse msgpack encoding, schema validation, checksum |
| Independent from data queue | Inbox availability doesn't depend on data transport (SHM/ZMQ) |
| Optional per role | Not all roles need peer messaging; zero overhead if unconfigured |

### 2.3 Relationship to Other Components

| Component | Relationship |
|-----------|-------------|
| Data queue (SHM/ZMQ) | Orthogonal. Inbox is a separate ZMQ channel. Both can be active simultaneously. |
| BrokerRequestComm | Discovery only.  Routes ROLE_INFO_REQ to broker; not involved in inbox data flow.  Per HEP-CORE-0033 §18, the lookup is a Class B fall-through across all the asker's hub connections. |
| Broker | Metadata storage only.  Stores `inbox_endpoint` + schema fields per-presence — on `ChannelEntry.producers[i]` for producers and on the `ConsumerEntry` row for consumers (NOT channel-wide; supports Fan-In where each producer has its own inbox).  Populated from REG_REQ / CONSUMER_REG_REQ (§4.1); served via ROLE_INFO_REQ.  Not in the inbox data path. |
| RoleHostCore | Owns the InboxQueue (one per role) + the inbox cache for outbound `InboxClient`s.  Inbox metrics serialised through the RoleHostCore metrics pipeline (HEP-CORE-0019 §5.4). |

---

## 3. Wire Protocol

Inbox uses the same msgpack fixarray[5] wire format as ZmqQueue:

```
[magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N), checksum:bin32]
```

| Element | Type | Description |
|---------|------|-------------|
| magic | uint32 | `0x51484C50` — frame validation |
| schema_tag | bin8 | 8-byte schema hash — receiver validates match |
| seq | uint64 | Monotonic per-sender sequence number |
| payload | array(N) | Schema fields packed per `ZmqSchemaField` definitions |
| checksum | bin32 | BLAKE2b-256 of decoded payload data |

**Schema validation** is mandatory and always-on (same as ZmqQueue):
- Both InboxQueue and InboxClient require a non-empty `ZmqSchemaField` list at construction
- Field types, sizes, and counts are validated at factory time (non-empty, valid type strings,
  numeric counts >= 1, string/bytes lengths > 0)
- The schema tag (8-byte BLAKE2b hash of canonical field definitions) is computed at factory
  time from the `ZmqSchemaField` list and embedded in every frame. The receiver validates the
  tag matches its own computed tag; mismatched tags are rejected (increments `recv_frame_error_count`).
- Payload array size must match the receiver's schema field count; rejected if different

**Checksum verification** is mandatory and always-on (same as ZmqQueue):
- Sender computes BLAKE2b-256 over the raw payload data before msgpack packing
- Receiver verifies BLAKE2b-256 after msgpack decoding; frames with mismatched checksums
  are dropped and logged (increments `checksum_error_count`)
- Unlike SHM's `ChecksumPolicy` (None/Manual/Enforced), inbox has no policy toggle —
  checksum is always computed and verified

**ACK**: Single byte sent from ROUTER to DEALER after processing:
- `0` = OK
- `1` = queue overflow
- `2` = schema error
- `3` = handler error

The DEALER/ROUTER envelope uses ZMQ's built-in identity routing, plus a
replay-metadata frame (§3.6):
- DEALER sets `ZMQ_IDENTITY` to the sender's pylabhub UID before connecting
- DEALER sends `[empty_delimiter, replay_meta, payload]`
- ROUTER receives `[identity, empty_delimiter, replay_meta, payload]`
- ROUTER sends ACK as `[identity, empty_delimiter, ack_byte]`

### 3.6 Replay defense (I-REPLAY-BOUND)

Transport-layer CURVE (§3.5) authenticates and encrypts the inbox
sockets, so an outsider cannot capture or replay a frame.  The residual
concern is application-level replay — a frame delivered twice (a
duplicate, or an authenticated peer resending) would run the receiver's
`on_inbox` handler twice, applying a side effect (e.g. a parameter
update) more than once.  The per-sender `seq` field (§3) is tracked for
gap *metrics* only and does NOT defend replay: it resets on a sender
reconnect, so enforcing monotonicity would wrongly reject a reconnected
sender's fresh frames.

Instead, each inbox message carries a **replay-metadata frame** — a
fixed 24-byte frame **separate from the msgpack payload** so the payload
codec stays byte-identical to the data-plane ZmqQueue frame it shares:

```
replay_meta = [ client_nonce : 16 bytes ][ client_wall_ts : uint64 big-endian, 8 bytes ]
```

- **Sender** (`InboxClient`) stamps a fresh random 16-byte `client_nonce`
  and the current `client_wall_ts` (ms since epoch) on every send.
- **Receiver** (`InboxQueue`) rejects the frame — dropping it, no handler
  call, incrementing `recv_replay_reject_count` — when either:
  1. wall-clock **skew** `|now - client_wall_ts|` exceeds the tolerance
     (bounds how long a captured frame stays replayable), OR
  2. the `(sender_identity, client_nonce)` pair was **already seen**
     within the dedup window.
- The dedup uses `pylabhub::utils::ReplayGuard` — the SAME sliding-window
  mechanism the hub REG/admin plane uses via `HubState::nonce_seen`; the
  inbox owns its own role-side instance (the receiver is a separate
  process from the hub), but the check-and-record logic is one component,
  not duplicated.  Skew and window default to 30 s.

### 3.5 CURVE Wiring (HEP-CORE-0036)

The msgpack frame in §3 is the inbox PAYLOAD; transport-layer
authentication of the inbox sockets uses **CURVE with the role's identity
keypair + HUB-WIDE `known_roles` authorization** (decided 2026-07-17).

**Authorization scope: hub-wide, NOT channel-scoped.**  The inbox is a
hub-wide role↔role messaging facility — any role may message any other role,
not just channel peers — so its authorization boundary is *"is the sender a
role this hub knows"* (`known_roles` membership), not *"is the sender on my
data channel."*  An earlier draft scoped it to the data channel's allowlist;
that was too narrow for the inbox's purpose.

- **Inbox ROUTER + DEALER use the role's IDENTITY keypair** on both sides
  (single-key model, per HEP-0036 I6 — same keypair the role uses on its data
  PUSH/PULL; broker mints NO data-plane CURVE keys).  No per-inbox keypair.
- **Hub-wide `known_roles` authorization.**  The role does NOT hold the hub's
  `known_roles` roster today (its data ZAP is channel-scoped, seeded from
  `REG_ACK.initial_allowlist`).  So the broker **distributes the roster**: a
  `known_roles` field on `REG_ACK` / `CONSUMER_REG_ACK` carries the hub's
  authorized role pubkeys.  The role registers an inbox `zap_domain` whose
  PeerAdmission is seeded from that roster — the inbox ROUTER admits any
  authenticated `known_role` and rejects everyone else (no anonymous /
  self-asserted senders, closing the plaintext gap).
- **Sender pins the receiver's identity pubkey.**  `ROLE_INFO_ACK` carries the
  receiver's identity pubkey alongside its inbox endpoint; the DEALER sets
  `curve_serverkey` to it.
- **Lifetime** — inbox lifetime ⊆ role lifetime (closes with role DEREG /
  HEP-0036 §5.7.2 cascade or BRC death, HEP-0036 I3).

**Roster freshness (MVP wrinkle).**  The roster is delivered at registration,
so a role that registers *after* you is not in your roster until a refresh.
`known_roles` is static operator config (changes rarely); a change-notify to
refresh live rosters is a later refinement.

`hub_inbox_queue.cpp` has zero CURVE references today; task **#191**
(P-InboxQueue) implements the wiring above.  See `docs/todo/AUTH_TODO.md`.

---

## 4. Data Flow

### 4.1 Receiver Setup (role host startup)

Under HEP-CORE-0036 §3.5.1 ("nothing happens behind the auth door
before auth"), inbox ROUTER bind and CURVE/ZAP arm are data-plane
footprint that MUST happen post-REG, inside `apply_*_reg_ack` (S3
of §3.5.5).  Since the inbox endpoint is config-determined (per
§3.4 wire fields), the role can put the endpoint into REG_REQ
without binding first.  The bind itself defers to S3 once the
broker has accepted the role onto the channel(s) the inbox will
serve.

```
S1 (setup_infrastructure_) — BIND + CURVE-ARM DENY-ALL:
  1. Role host reads inbox config (schema, endpoint, buffer_depth, packing).
  2. Build InboxQueue, arm CURVE-server auth (role identity keypair via
     `set_curve_server_identity(kRoleIdentityName, "<uid>:inbox")`), and
     bind the ROUTER — with an EMPTY (deny-all) ZAP allowlist.  The
     socket is CURVE-armed the instant it binds, so no unauthenticated
     peer can complete a handshake before the roster arrives; deny-all
     means NO peer is admitted yet.  Binding at S1 (rather than deferring
     to S3) resolves port-0 endpoints before S2 advertises them in
     REG_REQ.  The inbox `zap_domain` ("<uid>:inbox") is DISTINCT from
     the data channel's — hub-wide known_roles authorization, not the
     channel allowlist (§3.5).

S2 (registration) — FATAL on failure:
  3. For EACH presence the role registers (one for producer/consumer
     roles; two for processor — see HEP-CORE-0033 §19), the inbox
     metadata block is appended to that presence's registration
     payload:
       - producer presence  → REG_REQ          (ProducerRegInputs)
       - consumer presence  → CONSUMER_REG_REQ (ConsumerRegInputs)
     Fields per presence: inbox_endpoint, inbox_schema_json,
     inbox_packing, inbox_checksum.  Same `inbox_endpoint` string is
     sent in every presence's payload — there is one InboxQueue per
     role, regardless of how many hubs the role registers with.
  4. Broker stores the metadata once **per producer-presence / per
     consumer-presence** — inbox lives on the party row, NOT on the
     channel:
       - producer-presence registration → `ChannelEntry.producers[i].inbox_*`
       - consumer-presence registration → `ConsumerEntry.inbox_*`
     For Fan-In (multi-producer) channels each `ProducerEntry` carries
     its own inbox fields; a second producer joining the channel does
     NOT overwrite the first one's inbox.  For dual-hub processor,
     this means in_hub holds the ConsumerEntry copy (under
     in_channel) and out_hub holds the producer-row copy on the
     corresponding `ChannelEntry.producers[*]` (under out_channel) —
     both with identical inbox_endpoint strings.

S3 (apply_*_reg_ack) — SEED THE ROSTER (lift deny-all):
  5. `merge_inbox_known_roles(ack)` unions this presence's
       REG_ACK/CONSUMER_REG_ACK `known_roles` into the role's hub-wide
       inbox roster, then calls
       `inbox_queue->set_peer_allowlist(<roster as curve PeerIdentities>)`
       — lifting the ROUTER off its S1 deny-all default.  The inbox is a
       hub-wide role<->role facility, so it admits any authenticated
       known_role (single-key model I6), NOT just channel peers.  Roster
       and ZAP allowlist move together on every merge.
       (The ROUTER bind + CURVE arm already happened at S1; S3 only
       installs the authorization set.)
```

The receive thread (`inbox_thread_`: loop { recv_one() → invoke_on_inbox()
→ send_ack() }, under ThreadManager scope per HEP-CORE-0036 §3.5.4
invariant 4) runs from S1 — but until S3 seeds the roster the ROUTER is
deny-all, so no message reaches the handler before authorization is
installed.

**Port-0 inbox endpoints remain unsupported.**  HEP-CORE-0021 §16
(adopted 2026-07-08, closes task #94) enables post-bind endpoint
resolution for the data PUSH side via `ENDPOINT_UPDATE_REQ`, but
that path is scoped to `endpoint_type == "zmq_node"` only.  The
handler explicitly rejects `endpoint_type == "inbox"` with
`INBOX_UPDATE_NOT_SUPPORTED` per HEP-CORE-0021 §16.5.  Inbox
endpoints stay one-time-set at REG_REQ; port-0 inbox is
rejected at config-load with a clear error.

Rationale: the data PUSH endpoint change is inexpensive to
coordinate because consumer discovery happens exclusively via
`CONSUMER_REG_ACK` after CONSUMER_REG_REQ — the broker mediates
every read.  Inbox endpoints, by contrast, are discovered via
`ROLE_INFO_REQ` on demand from any peer at any time; a post-bind
update would create a window in which some peers cached the
port-0 placeholder and others the resolved port.  Not worth the
complexity for a message-per-second control path.

**Why advertise on every presence.**  ROLE_INFO_REQ (used by senders
to discover a target's inbox — see §4.2) is a Class B fall-through
query (HEP-CORE-0033 §18).  Each hub answers ROLE_INFO_REQ
from its own local view; a sender connected to in_hub will not find
a target that's only registered on out_hub.  By advertising the
inbox metadata on every presence's registration, the role becomes
discoverable from any hub it registers with, with zero hub-side
federation required.

### 4.2 Sender Connection (on demand)

```
1. Script calls api.open_inbox("TARGET-UID-1234")
2. ScriptEngine → RoleHostCore::open_inbox() (cached, thread-safe)
3. BrokerRequestComm sends ROLE_INFO_REQ to a broker.
   • Single-hub sender: ROLE_INFO_REQ goes to the role's only hub.
   • Multi-hub sender (e.g. dual-hub processor as the asker):
     Class B fall-through (HEP-CORE-0033 §18) — the asker
     queries each of its hub connections in turn; first hub that
     answers "found" wins.  If no hub returns a match within the
     timeout, the call fails with "uid not found".
4. The hub that answers searches its own `ChannelEntry.producers[]`
   (by ProducerEntry.role_uid; covers all 1..N producers per
   HEP-CORE-0023 §2.1.1), then its `ChannelEntry.consumers[]`
   (by ConsumerEntry.role_uid).  Returns the first match.  Other
   hubs (if the target is registered there too) hold a duplicate
   copy with the same inbox_endpoint string — first answer wins.
5. ROLE_INFO_ACK: inbox_endpoint, inbox_schema, inbox_packing, inbox_checksum
6. InboxClient::connect_to(endpoint, my_uid, schema, packing) → shared_ptr
7. client->start()                          — connect DEALER socket
8. client->set_checksum_policy(owner's policy from inbox_checksum)
   The inbox OWNER dictates the checksum policy. The sender adopts it.
9. InboxHandle wraps client for script use
```

**Sender ↔ receiver are direct ZMQ.**  Once the sender has
`inbox_endpoint`, the DEALER↔ROUTER connection is direct
TCP/IPC — no hub is in the data path.  Cross-hub inbox messaging
works automatically as long as the endpoint is network-routable
from the sender's host (see §13).

### 4.3 Message Exchange

```
Sender (InboxClient):                    Receiver (InboxQueue):
  buf = client->acquire()                  item = inbox_queue_->recv_one(timeout)
  // fill buf with typed fields            // item->data = decoded payload
  ack = client->send(timeout)              // item->sender_id = sender UID
  // ack == 0 means OK                     // item->seq = sequence number
                                           // engine->invoke_on_inbox(...)
                                           inbox_queue_->send_ack(0)
```

### 4.4 Shutdown

```
1. Role host signals inbox_thread_ to stop
2. inbox_queue_->stop()                    — closes ROUTER socket
3. Connected InboxClients get ZMQ disconnect; send() returns 255
4. InboxClient::stop() called in RoleHostCore cache cleanup
```

### 4.5 Multi-Hub Reachability

The inbox is a **direct** ZMQ DEALER↔ROUTER channel: once a sender
discovers the receiver's `inbox_endpoint` (via ROLE_INFO_REQ —
§4.2), the connection bypasses every hub.  This means inbox traffic
crosses host boundaries iff the endpoint URL is routable from the
sender's host.  Three deployment cases:

| Topology | Sender host | Receiver bind | Works? |
|---|---|---|---|
| Same-host single-hub | localhost | `tcp://127.0.0.1:NNNN` or `ipc://...` | ✓ |
| Same-host single-hub | localhost | `tcp://0.0.0.0:NNNN` | ✓ (loopback works on the all-interfaces bind) |
| Cross-host single-hub | host-A | `tcp://127.0.0.1:NNNN` | ✗ (loopback unreachable from outside) |
| Cross-host single-hub | host-A | `tcp://0.0.0.0:NNNN` (or `tcp://<NIC-IP>:NNNN`) | ✓ (routable bind) |
| Dual-hub processor (sender on `in_hub`, receiver on both hubs) | wherever the sender is | bind address must be reachable from BOTH hubs' senders | ✓ if bind is routable from both networks |

**Operator responsibility — bind address.**  The inbox `endpoint`
field in `inbox_config` is the role's chosen bind URL.  For
deployments where senders may live on hosts that can reach one but
not all of the role's hubs, the operator MUST bind to an address
routable from every sender host.  Loopback binds are appropriate
only when all senders are on the same host.

**Per-presence advertisement (recap from §4.1).**  The same
`inbox_endpoint` string is sent in every presence's REG_REQ /
CONSUMER_REG_REQ payload.  For a dual-hub processor, both hubs
hold independent copies of the inbox metadata — senders connected
to either hub can discover the role via the local hub's
ROLE_INFO_REQ.  No hub-to-hub federation is required for
discovery; reachability is the operator's network configuration.

**Why no hub-side federation here.**  HEP-CORE-0022 (Hub Federation
Broadcast) is a separate concern (cross-hub broadcasts, peer
relay).  Inbox is intentionally simpler: every interested hub
holds its own metadata copy; ROLE_INFO_REQ is a Class B
fall-through query at the role-side (HEP-CORE-0033 §18).
The inbox's data path is not hub-routed at all, so there's nothing
for federation to relay.

**Failure semantics.**

- Endpoint unreachable from sender's host: `InboxClient::start()`
  returns success (DEALER `connect()` is non-blocking + idempotent),
  but `send()` returns 255 (timeout) on every attempt.  The
  receiver never sees the sender.  Operator-side fix: change the
  receiver's bind to a routable address.
- Endpoint reachable but receiver process is down: same observable
  symptom (255 on send) until the receiver restarts and rebinds.
- Receiver restarts with a new fixed port (config change or
  operator-driven redeployment): cached `InboxClient` instances
  are stale.  Senders should refresh via a fresh
  `api.open_inbox(uid)` after detecting repeated 255 acks, which
  re-runs ROLE_INFO_REQ and gets the new endpoint.  Note: inbox
  endpoints cannot use port-0 ephemeral binding — see §4.1
  "Port-0 inbox endpoints remain unsupported" above.

---

## 5. Threading Model

```
                          Role Host Process
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  main_thread_ (data loop)                                   │
│  ├─ drain_inbox_sync(inbox_queue_)                          │
│  │   ├─ inbox_queue_->recv_one(0ms)  [non-blocking poll]    │
│  │   ├─ engine->invoke_on_inbox(data, size, sender_id)      │
│  │   └─ inbox_queue_->send_ack(code)                        │
│  └─ [continue with on_produce/on_consume/on_process]        │
│                                                             │
│  ctrl_thread_ (broker protocol)                             │
│  └─ [heartbeat, ctrl messages — no inbox interaction]       │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**Threading contract:**
- `recv_one()` and `send_ack()` MUST be called from the same thread (ZMQ ROUTER
  socket is not thread-safe)
- Currently called from the main data loop via `drain_inbox_sync()` (non-blocking
  poll before each data callback)
- InboxClient is single-threaded per instance (one client per target, used from
  the script thread via InboxHandle)

---

## 6. Configuration

Inbox configuration is specified as flat top-level keys in the role's JSON config:

```json
{
    "inbox_schema": {
        "fields": [
            {"name": "cmd", "type": "int32"},
            {"name": "value", "type": "float64"}
        ]
    },
    "inbox_endpoint": "tcp://0.0.0.0:0",
    "inbox_buffer_depth": 64,
    "inbox_overflow_policy": "drop",
    "inbox_zmq_packing": "aligned"
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `inbox_schema` | yes (to enable inbox) | — | Slot layout for inbox messages |
| `inbox_endpoint` | yes (if inbox) | — | ZMQ ROUTER bind endpoint. Port 0 = OS-assigned. |
| `inbox_buffer_depth` | no | `64` | ZMQ RCVHWM. 0 = unlimited. |
| `inbox_overflow_policy` | no | `"drop"` | `"drop"` (finite HWM) or `"block"` (unlimited HWM) |
| `inbox_zmq_packing` | no | `"aligned"` | `"aligned"` (C-struct natural) or `"packed"` (no padding) |

When `inbox_schema` is absent or empty, no inbox is created. The role operates
without peer messaging capability.

---

## 7. Script API

### 7.1 Receiving (on_inbox callback)

```python
def on_inbox(slot, sender, api):
    """Called once per inbox message, before the main data callback.

    Args:
        slot:   ctypes struct view of the decoded payload (schema-typed)
        sender: str — pylabhub UID of the sender
        api:    role API object (ProducerAPI/ConsumerAPI/ProcessorAPI)
    """
    if slot.cmd == 1:
        api.log(f"Received command from {sender}: value={slot.value}")
```

### 7.2 Sending (InboxHandle)

```python
handle = api.open_inbox("PROD-SENSOR-A1B2C3D4")
if handle is None:
    api.log("Target offline or has no inbox")
    return

handle.acquire()           # populate buffer from schema
handle.slot.cmd = 1
handle.slot.value = 3.14
ack = handle.send(1000)    # send with 1s ACK timeout
if ack == 0:
    api.log("Message delivered")
```

### 7.3 InboxHandle Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `acquire()` | ctypes slot | Get write buffer (zeroed); fill fields before send |
| `send(timeout_ms)` | int | Send + wait for ACK. 0=OK, 255=timeout/error |
| `discard()` | None | Discard buffer without sending |
| `is_ready()` | bool | True if client is connected |
| `close()` | None | Disconnect client |

### 7.4 InboxHandle Caching

`api.open_inbox(uid)` is cached per (role, target_uid):
- First call: broker round-trip (ROLE_INFO_REQ) → create InboxClient → cache
- Subsequent calls: return cached handle
- Cache is per-RoleHostCore (shared across script reloads)
- `api.clear_inbox_cache()` forces fresh broker lookups

---

## 8. Metrics

InboxQueue exposes four diagnostic counters:

| Metric | Description |
|--------|-------------|
| `recv_frame_error_count` | Frames rejected: bad magic, schema tag mismatch, field type/size error |
| `ack_send_error_count` | ZMQ send errors when sending ACK response |
| `recv_gap_count` | Sequence number gaps (per-sender tracking; indicates dropped frames) |
| `checksum_error_count` | BLAKE2b verification failures after successful decode |

These are available via:
- `InboxQueue::inbox_metrics()` → `InboxMetricsSnapshot` (C++ struct)
- `PYLABHUB_INBOX_METRICS_FIELDS` X-macro for serialization (see HEP-0008 §6.1)
- Hierarchical metrics output under `"inbox"` key (see HEP-0008 §6.1, HEP-0019 §5.4)

InboxQueue is exposed to the script engine via `RoleContext::inbox_queue` pointer
(non-owning; role host retains ownership). The pointer is set during role setup
and is nullptr when no inbox is configured.

---

## 9. Common Use Cases

### 9.1 Parameter Update

A control role sends updated calibration parameters to a running producer:

```python
# Controller (consumer with inbox client):
handle = api.open_inbox("PROD-SENSOR-A1")
handle.acquire()
handle.slot.param_id = 42
handle.slot.new_value = 1.234
handle.send(1000)

# Producer (on_inbox handler):
def on_inbox(slot, sender, api):
    update_calibration(slot.param_id, slot.new_value)
    api.log(f"Param {slot.param_id} updated by {sender}")
```

### 9.2 Coordination Signal

A processor signals readiness to a downstream consumer:

```python
# Processor:
handle = api.open_inbox("CONS-DISPLAY-B2")
handle.acquire()
handle.slot.signal = READY_SIGNAL
handle.send(500)

# Consumer (on_inbox handler):
def on_inbox(slot, sender, api):
    if slot.signal == READY_SIGNAL:
        api.log(f"Upstream {sender} ready")
```

### 9.3 Request/Response

A monitoring role queries status from another role and reads the ACK:

```python
handle = api.open_inbox("PROC-FILTER-C3")
handle.acquire()
handle.slot.request_type = STATUS_QUERY
ack = handle.send(2000)
if ack == 0:
    api.log("Status query acknowledged")
elif ack == 255:
    api.log("Target did not respond in time")
```

---

## 10. Source File Reference

| Component | File | Description |
|-----------|------|-------------|
| InboxQueue | `src/include/utils/hub_inbox_queue.hpp` | ROUTER receiver API |
| InboxClient | `src/include/utils/hub_inbox_queue.hpp` | DEALER sender API |
| Implementation | `src/utils/hub/hub_inbox_queue.cpp` | ZMQ + msgpack + checksum |
| Wire helpers | `src/utils/hub/zmq_wire_helpers.hpp` | Shared msgpack pack/unpack |
| Script handle | `src/scripting/python_helpers.hpp` | `InboxHandle` wrapper |
| Drain helper | `src/scripting/role_host_helpers.hpp` | `drain_inbox_sync()` |
| Discovery | `src/include/utils/broker_request_comm.hpp` | `query_role_info()` for ROLE_INFO_REQ (Class B fall-through — HEP-CORE-0033 §18) |
| Protocol | HEP-CORE-0007 §12.4 | ROLE_INFO_REQ/ACK message format |
| Metrics X-macro | `src/include/utils/hub_inbox_queue.hpp` | `PYLABHUB_INBOX_METRICS_FIELDS` |
| Metrics adapters | `metrics_json.hpp`, `metrics_pydict.hpp`, `metrics_lua.hpp` | Serialization helpers |
| Tests | `tests/test_layer3_datahub/test_datahub_hub_inbox_queue.cpp` | 11 L3 tests |

---

## 11. Cross-References

- **HEP-CORE-0007 §12.4**: ROLE_INFO_REQ/ACK protocol for inbox endpoint discovery
- **HEP-CORE-0008 §6.1**: Hierarchical metrics schema (inbox group)
- **HEP-CORE-0019 §2.3**: Per-presence heartbeat protocol (Phase 6) — inbox metadata flows on every presence's registration as part of the same per-presence model
- **HEP-CORE-0019 §5.4**: Metrics serialization architecture
- **HEP-CORE-0023 §2.5.2**: Per-presence heartbeat contract — explains the "every presence registers on its hub" pattern that §4.1 relies on
- **HEP-CORE-0033 §8**: HubState entry types — broker-side inbox metadata lives on `ChannelEntry.producers[i].inbox_*` (per-producer) and `ConsumerEntry.inbox_*` (per-consumer); **not** at channel scope.  This per-party placement is required for Fan-In: each producer on a multi-producer channel keeps its own inbox endpoint.
- **HEP-CORE-0033 §18**: Broker message routing classes — ROLE_INFO_REQ is Class B (role-bound, fall-through query)
- **HEP-CORE-0033 §19**: Multi-presence roles — defines presence list + per-hub registration that drives §4.1's per-presence advertisement
- **HEP-CORE-0015 §4, §6.4** (SUPERSEDED — see HEP-CORE-0033 §19): historical processor inbox config fields + InboxHandle API
- **HEP-CORE-0018 §15.6** (SUPERSEDED): historical inbox plane overview — superseded by this document
- **HEP-CORE-0034 §11.4**: Inbox schemas integrate into the hub's owner-authoritative
  schema registry as `(receiver_uid, "inbox")` records. Receiver-as-authority model
  (this HEP §4.1 step 7-8) maps directly onto HEP-0034 ownership rules; existing wire
  fields (`inbox_endpoint`, `inbox_schema_json`, `inbox_packing`, `inbox_checksum`) are
  retained, with broker-side storage unified into `HubState.schemas`. The broker
  computes the SchemaRecord fingerprint using the same canonical bytes as
  `compute_inbox_schema_tag` so `SchemaRecord.hash[0..7] == wire schema_tag`. Inbox
  records cascade-evict via `_on_channel_closed` when the producer's data channel
  closes (DEREG_REQ or heartbeat timeout) — see HEP-0034 §7.2.
