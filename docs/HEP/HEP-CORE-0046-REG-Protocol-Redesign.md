# HEP-CORE-0046: REG/REG_ACK Protocol Redesign

| Property | Value |
|---|---|
| **HEP** | `HEP-CORE-0046` |
| **Title** | REG/REG_ACK Protocol Redesign — Typed Wire Envelope + Admission-Gate Pipeline |
| **Status** | 🚧 **DESIGN LOCKED; IMPLEMENTATION IN FLIGHT.**  Phase A (typed envelope + body classes) SHIPPED with L1 coverage (46 tests).  Phase C machinery (admission gates, REG pipeline, broker adapter) SHIPPED as islanded modules with L1/L2 unit coverage (23 + 5 + 14 tests + `HubState::nonce_seen` 6 tests).  **Phase B (dispatch rewire in `broker_service.cpp` + BRC envelope migration) PENDING** — the pipeline is dead code until Phase B lands.  Phases D (retirements), E (integration tests), F (federation follow-on) PENDING.  Promoted from `docs/tech_draft/DRAFT_reg_ack_protocol_redesign.md` on 2026-07-12. |
| **Created** | 2026-07-12 (design content dates back to earlier tech-draft revs) |
| **Depends on** | HEP-CORE-0017 §3.3.0 (topology-parametric queue factory — the abstraction that makes this cleanup possible), HEP-CORE-0036 §I11 (allowlist mutator locality — REG remains the only path that mutates channel membership), HEP-CORE-0035 §4 (Hub-Role Authentication — REG carries the CURVE identity used by ZAP), HEP-CORE-0040 §5 (KeyStore — the source of role identity keys REG proves), HEP-CORE-0023 §2 (Startup Coordination — REG is the first wire a role sends after CURVE handshake) |
| **Related — REG-family cross-refs** | HEP-CORE-0007 §12 (DataHub protocol catalog — REG_REQ/CONSUMER_REG_REQ/DEREG_REQ live here today; retirements listed in §3 land as amendments to §12), HEP-CORE-0021 (ZMQ Endpoint Registry — `ENDPOINT_UPDATE_REQ` is REG-family under this HEP's envelope), HEP-CORE-0042 §5.5 (Channel Attach Coordination — `CHANNEL_AUTH_APPLIED_REQ` and `CHANNEL_AUTH_CHANGED_NOTIFY` flow through this envelope), HEP-CORE-0033 (Hub Character — `broker_proto` version and REG dispatch site owned by BrokerServiceImpl), HEP-CORE-0018 (Producer/Consumer Binaries — role hosts send REG_REQ / CONSUMER_REG_REQ; `binding_role_type()` naming from HEP-CORE-0036 §I9.1 flows into role_type discriminator) |
| **Rule** | `docs/IMPLEMENTATION_GUIDANCE.md § "REG Protocol Wire Discipline (HEP-CORE-0046)"` — all REG-family wire work MUST go through the typed envelope + admission pipeline; direct `body.value("field", default)` JSON parsing in REG-family handlers is a violation. |
| **Trackers** | The single source of implementation status is §12 of this HEP.  Phase A shipped in the same commit tree as this HEP; Phase B is the next active commit. |

---

## Abstract

The queue-abstraction cleanup (HEP-CORE-0017 §3.3.0 shipped 2026-07-09;
HEP-CORE-0036 §I9.1 shipped 2026-07-12) settled that every channel has
exactly one data-plane endpoint owned by the topology-declared binding
side.  Several REG / REG_ACK wire messages designed for the pre-
abstraction "N producers each with their own endpoint" world are now
answering questions that no longer exist.

This HEP redesigns the REG-family wire around three principles:

1. **Do less.**  Retire per-producer endpoints, per-peer attach
   handshakes, and multi-endpoint arrays that the queue abstraction
   made obsolete.  REG-family becomes: admission, discovery,
   allowlist sync.  Nothing else.
2. **Type the wire.**  A locked 5-frame typed envelope (§14) plus
   one typed body class per message type (§14.3) replaces
   scattered `body.value("field", default)` JSON extraction across
   ~10 broker handlers + ~8 BRC methods.  Adding a required field
   becomes a compile error, not a silently defaulted string.
3. **Order security first.**  A shared 7-gate admission pipeline
   (§14.5) runs to completion BEFORE any state mutation and BEFORE
   any NOTIFY fires.  No handler re-implements a gate; no handler
   half-mutates state under a failing gate.  21 named invariants
   (§8.1) ride on this ordering.

Phase A (typed envelope + body classes) and the islanded Phase C
modules (gates + pipeline + broker adapter) have shipped as compile-
verified L1/L2-tested modules.  The load-bearing next step is Phase B
— rewire `broker_service.cpp` dispatch + BRC send/receive onto the
envelope in one atomic commit.  Until Phase B lands, the pipeline is
dead code and the security invariants do not hold in production.

## Cross-reference obligations for related HEPs

Any future work that touches a REG-family wire (REG_REQ, REG_ACK,
CONSUMER_REG_REQ, CONSUMER_REG_ACK, DEREG_REQ, DEREG_ACK,
ENDPOINT_UPDATE_REQ, GET_CHANNEL_AUTH_REQ, CHANNEL_AUTH_APPLIED_REQ,
CHANNEL_AUTH_CHANGED_NOTIFY, HEARTBEAT_REQ, DISC_REQ,
CHECK_PEER_READY_REQ) MUST cite this HEP as the wire authority.  In
particular:

- **Adding a wire field** — extend the relevant `XxxReqBody` /
  `XxxAckBody` typed body class (§14.3), not a `body[k] = v` at a
  handler call site.
- **Adding a new REG-family message** — define a new typed body
  class per §14.3, hook it into the dispatch table §14.4, and
  route it through the admission pipeline §14.5 if it mutates
  membership state.
- **Adding a new admission gate** — extend §14.5's ordered list;
  every handler that gates on it gets it uniformly via
  `reg_admission_pipeline`, never re-implemented per handler.
- **Retiring a wire field** — do it under §3's retirement catalogue;
  the retirement must ship atomically with the broker_proto bump
  (§14.6, `I-WIRE-VERSION-ATOMIC`).

Related HEPs SHOULD carry a one-line pointer at the site where they
describe REG-family wire behavior; see §12 sequencing for the exact
sites in HEP-CORE-0007, -0017, -0021, -0033, -0035, -0036, -0042.

---

## 0. Why this document exists

The queue abstraction (HEP-CORE-0017 §3.3.0) settled a load-bearing
question: every channel has exactly ONE data-plane endpoint, owned by
the topology-declared binding side.  That reshapes what the REG /
REG_ACK protocol needs to do — several wire messages designed for the
pre-abstraction "N producers each with their own endpoint" world are
now trying to answer questions that no longer exist.

Before we retire code or change wire, this document walks the whole
protocol against the topology model, checks each message against
every legal (topology × transport × role side) configuration, walks
error paths, and calls out open questions.  Nothing here is code
yet — the point is to agree on the shape, then implement.

## 1. What the protocol needs to accomplish

Three, and only three, things:

1. **Admission.**  Is this role allowed on this channel?  Enforces
   topology declaration, cardinality per side, transport ×
   topology compatibility, schema invariants, CURVE identity check
   against `known_roles`.
2. **Discovery.**  The dialing side needs the binding side's
   endpoint + CURVE identity pubkey to reach it.
3. **Continuous allowlist synchronization.**  As peers come and go,
   the binding side's ZAP allowlist has to stay in sync so
   CURVE handshakes succeed on first attempt.

Everything else the current wire carries — per-producer endpoint
resolution, per-peer attach handshakes, multi-endpoint arrays — is
now the queue abstraction's job or is dead weight.

## 2. The clean protocol — message by message

For each message: purpose, admission semantics, semantics under
each of the five legal (topology × transport) cells, what changes
vs. the current implementation.

> **Wire shape is normatively defined in §14** (typed wire envelope
> contract).  Every message is 5 frames: skeleton (Frames 0-3) +
> per-msg-type typed body class (Frame 4).  §2 subsections below
> reference the body class by name and describe admission
> semantics; they do NOT re-state wire schema (single source of
> truth in §14 avoids drift between prose and code).
>
> When §2 subsections list fields (for reader convenience), those
> fields are the accessors on the typed body class in §14.3; they
> are NOT a separate JSON-key contract.  Handlers read them via
> `body.field()`, never via `body.value("field", "")`.

### 2.1 REG_REQ / CONSUMER_REG_REQ

**Purpose.**  Ask the broker to admit this role on this channel.

**Body class.**  `RegReqBody` — see §14.3 for the field list.
Same class for producer (`REG_REQ`) and consumer
(`CONSUMER_REG_REQ`); `body.role_type()` disambiguates.

**Semantics under the clean model.**  Broker runs the admission
sequence in order:

1. Grammar validation (identifier shapes per HEP-CORE-0033).
2. ABI fingerprint check.
3. Topology wire parse; empty → `OneToOne` default.
4. **Transport × topology compatibility** — `(fan-in, shm)` refused
   with `TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT`.
5. Channel existence lookup.
   - If missing AND this role is the topology-legal binding side
     opener → open the channel with this role as first admitted.
     Fan-in consumer opens; fan-out / one-to-one producer opens.
   - If missing AND this role is NOT the legal opener → reject
     with `CHANNEL_NOT_FOUND`.  Under fan-out / one-to-one a
     consumer arriving before the producer must wait for the
     producer to open; under fan-in a producer arriving before
     the consumer pends via the R6 gate below.
6. Schema × transport invariant match (on existing channel).
7. Topology declaration match (immutable at creation).
8. Cardinality gate — per-side check against stored counts.
9. Same-uid re-registration detection (idempotent).
10. **R6 gate (dialing side only).**  If this role is the
    dialing side per §3.3.0:
    - Binding side must be Live (first heartbeat received).
    - `ChannelEntry.data_endpoint` must have a value.
    - `confirmed_version[K][P] >= my_version` (where `my_version`
      is the `channel_version` at the moment this role's pubkey
      was added to the allowlist).
    If any condition fails → pend the REG_REQ in a per-channel
    queue keyed on the missing condition; broker replies when
    the condition clears OR times out.
11. Add role to `ChannelEntry.producers[]` or `.consumers[]`.
12. Under fan-in producer path: add producer pubkey to the
    channel-scope allowlist; fire `CHANNEL_AUTH_CHANGED_NOTIFY(
    phase=admitted, role_type=producer)` to the binding-side
    consumer.
13. Under fan-out / one-to-one consumer path: add consumer pubkey
    to the channel-scope allowlist; fire notify to the
    binding-side producer.
14. Build REG_ACK per §2.2.

**No wire schema changes vs. today** — REG_REQ shape is already
topology-aware.  What changes is the broker-side logic (steps 5,
10, 12/13).  Same wire, different admission semantics.

### 2.2 REG_ACK / CONSUMER_REG_ACK

**Purpose.**  Confirm admission + deliver the peer-side artifacts
this role needs to establish the data plane.

**Body class.**  `RegAckBody` — see §14.3.  Same class for
producer (`REG_ACK`) and consumer (`CONSUMER_REG_ACK`).
`snapshot_version` is the `channel_version` at the moment
`initial_allowlist` was captured (kept as integrity witness — the
client cross-checks subsequent NOTIFY versions against this
baseline).

**`initial_allowlist` — the unified topology-driven peer field.**
Semantic depends on which side this ACK is going to:

- **Binding side** (fan-out / one-to-one producer; fan-in consumer):
  the list of pubkeys currently admitted to my ZAP allowlist.  Each
  entry `{role_uid?, pubkey_z85}`, `endpoint` field empty.
  0..N entries (N = number of dialing-side peers currently admitted
  to this channel).  Empty on a fresh channel where no dialing peer
  has arrived yet.
- **Dialing side** (fan-out / one-to-one consumer; fan-in producer):
  exactly one entry `{role_uid, endpoint, pubkey_z85}` — the peer
  I'm about to dial.  `endpoint` MUST be non-empty (guaranteed by R6
  gate at step 10 of REG_REQ handling above).

**Two schemas retire.**
- Retired: scalar `data_endpoint` + `data_pubkey` fields on the
  dialing-side ACK.  Redundant with `initial_allowlist[0]`.
- Retired: `CONSUMER_REG_ACK.producers[]` legacy N-endpoint array.
  Replaced by the same `initial_allowlist` shape.  Under the
  topology model the dialing-side entry count is always exactly 1.

**Error ACK shape** (unchanged): `status="error"` + `error_code` +
`message` + `correlation_id`.  Error codes worth calling out:
`INVALID_REQUEST`, `TOPOLOGY_MISMATCH`,
`TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT`,
`FAN_IN_IS_SINGLE_CONSUMER`, `FAN_OUT_IS_SINGLE_PRODUCER`,
`ONE_TO_ONE_CARDINALITY_VIOLATED`, `CHANNEL_NOT_FOUND`,
`SCHEMA_MISMATCH`, `TRANSPORT_MISMATCH`, `UID_CONFLICT`,
`abi_major_mismatch`.

### 2.3 ENDPOINT_UPDATE_REQ / _ACK

**Purpose.**  Binding side publishes the endpoint it actually bound
after `apply_master_approval` drove the queue to Active.  The broker
records it on `ChannelEntry.data_endpoint`; subsequent dialing-side
REG_ACKs use it in their `initial_allowlist`.

**Body classes.**  `EndpointUpdateReqBody` / `EndpointUpdateAckBody`
— see §14.3.  Under the clean model `endpoint_type = "zmq_node"`
is the only value (inbox retires).

**Sender validation — TOPOLOGY-AWARE.**  Under I-DEALER-IDENTITY,
`env.identity() == body.role_uid()`.  Under I-CHANNEL-SINGLE-
BINDING-SIDE, the check reduces to `env.identity() ==
ChannelEntry.binding_side_uid()`.  All three sender-side handlers
(§2.3, §2.5, §2.6) call the same `HubState::is_binding_side_sender`
primitive; no per-handler roster walk.

- fan-in     → sender must be in `ChannelEntry.consumers[]`
- fan-out    → sender must be in `ChannelEntry.producers[]`
- one-to-one → sender must be in `ChannelEntry.producers[]`

Reject with `NOT_CHANNEL_OWNER` otherwise.

**Idempotency (unchanged).**  Same endpoint value → `success` no-op.
Different value while `data_endpoint` already resolved AND at least
one dialing-side peer is attached → reject with `ENDPOINT_ALREADY_SET`
(binding-side rebind post-admission is not supported; role must
DEREG and re-REG).

**Retires with this clean-up:** `endpoint_type="inbox"` mutation path
(inbox endpoints resolved at REG_REQ time, never updated).  Broker
handler still validates the endpoint_type but the inbox branch dies.

### 2.4 CHANNEL_AUTH_CHANGED_NOTIFY

**Purpose.**  Fire-and-forget doorbell from broker to the binding
side when the channel's allowlist changed (a peer was admitted, left,
or transitioned to Live).

**Body class.**  `ChannelAuthChangedNotifyBody` — see §14.3.
`body.role_uid()` names the subject peer whose state changed;
`body.phase()` distinguishes admitted / live / left.

**Target dispatch — TOPOLOGY-AWARE.**  Broker fans the notify to the
BINDING side of the channel.

- fan-in     → to consumers (single consumer under fan-in cardinality)
- fan-out    → to producers (single producer under fan-out cardinality)
- one-to-one → to producers

Under the pre-migration wire this fan was hardcoded to producers.
That has to become topology-driven; the machinery for it already
exists in `fire_channel_auth_changed_notify` in the current code.

**Phase semantics.**
- `admitted` — dialing peer's REG was accepted, its pubkey added
  to the binding side's allowlist.  Binding side pulls
  GET_CHANNEL_AUTH_REQ → applies to ZAP → APPLIED_REQ.
- `live` — dialing peer transitioned to Live at the broker (first
  heartbeat received).  Purely observational; binding-side role
  updates its `live_peers[channel]` map.  No allowlist mutation.
- `left` — dialing peer DEREG'd or heartbeat-timeout'd.  Its pubkey
  removed from the allowlist.  Binding side pulls fresh allowlist.

**Missed notify tolerance.**  Fire-and-forget — if the binding side
misses one (broker crash, network partition), the drift resolves
next time the binding side reconnects: REG_ACK.initial_allowlist is
the authoritative snapshot.

### 2.5 GET_CHANNEL_AUTH_REQ / _ACK

**Purpose.**  Binding-side role pulls the current allowlist after
receiving `CHANNEL_AUTH_CHANGED_NOTIFY(phase=admitted|left)`.  Also
used for defensive re-sync on any perceived drift.

**Body classes.**  `GetChannelAuthReqBody` /
`GetChannelAuthAckBody` — see §14.3.  The ACK's `allowlist` is
an array of Z85 pubkey strings; `channel_version` is the current
`channel_version[K]` at snapshot capture time.

**Sender validation — TOPOLOGY-AWARE.**  Same as §2.3:
`env.identity() == ChannelEntry.binding_side_uid()` via
`HubState::is_binding_side_sender`.  Caller MUST be the binding
side regardless of topology.

### 2.6 CHANNEL_AUTH_APPLIED_REQ / _ACK

**Purpose.**  Binding side reports "I've applied allowlist version N
to my ZAP."  Broker advances `confirmed_version[K][P]` and releases
any dialing-side REG_REQs pending on `confirmed_version >= my_version`
(R6 gate condition 3).

**Body classes.**  `ChannelAuthAppliedReqBody` /
`ChannelAuthAppliedAckBody` — see §14.3.  `body.instance_id()` on
the REQ is the stale-guard per HEP-CORE-0042 §5.5.3.

**Sender validation — TOPOLOGY-AWARE.**  Same as §2.5.

### 2.7 Lifecycle messages

- **HEARTBEAT_REQ / HEARTBEAT_ACK** — presence maintenance
  (HEP-CORE-0023).  Bodies: `HeartbeatReqBody` /
  `HeartbeatAckBody` (see §14.3).  Topology-independent.
- **DEREG_REQ / DEREG_ACK**, **CONSUMER_DEREG_REQ / CONSUMER_DEREG_ACK**
  — voluntary teardown.  Bodies: `DeregReqBody` / `DeregAckBody`.
  Broker fires `CHANNEL_AUTH_CHANGED_NOTIFY(phase=left)` to the
  binding side and, when the last binding-side peer leaves, tears
  the channel down and fires `CHANNEL_CLOSING_NOTIFY` to all
  remaining dialing-side peers.
- **DISC_REQ / DISC_ACK** — discovery query.  Bodies:
  `DiscReqBody` / `DiscAckBody`.  Topology-independent.

## 3. What retires

Everything below dies with the clean-up.  The queue abstraction +
the unified `initial_allowlist` field answer the questions these
mechanisms were designed to answer.

**Wire messages retired:**
- `CONSUMER_ATTACH_REQ_ZMQ`
- `CONSUMER_ATTACH_ACK_ZMQ`
- `CHANNEL_PRODUCERS_CHANGED_NOTIFY` (already superseded by the
  phased NOTIFY above)
- `GET_CHANNEL_PRODUCERS_REQ` / `_ACK` (already superseded)

**Wire fields retired:**
- `REG_ACK.data_endpoint` scalar (subsumed by
  `initial_allowlist[0].endpoint` on dialing side)
- `REG_ACK.data_pubkey` scalar (subsumed by
  `initial_allowlist[0].pubkey_z85`)
- `CONSUMER_REG_ACK.producers[]` legacy N-endpoint array (already
  superseded by `initial_allowlist`)

**Broker code retired:**
- `handle_consumer_attach_req_zmq` handler
- `pending_attach_queue_` per-channel/per-producer queues
- `drain_pending_attach_queue_*` helpers
- `sweep_pending_attach_timeouts_` periodic drain

**HubState fields / methods retired:**
- `ProducerEntry::zmq_node_endpoint` (per-producer endpoint field)
- `HubState::_set_producer_zmq_node_endpoint`
- `ProducerEntry::producer_zmq_node_endpoint(role_uid)` lookup

**Role code retired:**
- The HEP-CORE-0042 §7.1 pre-attach loop inside
  `RoleAPIBase::apply_consumer_reg_ack`
- The N-entry `producers[]` filter walk that feeds `filtered_ack`
- `attach:begin` / `attach:success` / `attach:complete` log
  markers

**BRC client method retired:**
- `BrokerRequestComm::consumer_attach_zmq`

**Options struct fields retired:**
- `RxQueueOptions::producer_peers` (per-peer connect list)
- `hub::ProducerPeer` struct
- `TxQueueOptions::zmq_bind` / `RxQueueOptions::zmq_bind`
- `RoleConfig::TransportConfig::zmq_bind` (source of the above)

**Queue API retired:**
- `ZmqQueue::add_producer_peer`
- `ZmqQueue::remove_producer_peer`
- `ZmqQueue::set_producer_peers`

## 4. What consolidates or renames

**`ChannelAccessEntry::authorized_consumer_pubkeys`** → rename to
`authorized_peer_pubkeys` (or `admitted_pubkeys`).  The field is
topology-agnostic in what it holds — the pubkeys admitted to the
binding side's ZAP allowlist.  Under fan-out / one-to-one those are
consumers; under fan-in they are producers.  The current name is a
pre-topology misnomer.

**`HubState::_on_consumer_authorized` / `_on_consumer_revoked`** →
`_on_peer_authorized` / `_on_peer_revoked`.  Same story — the mutator
is topology-agnostic, the name lies.

**`handle_get_channel_auth_req` doc-comment** currently says
"producer pulls the allowlist"; should say "binding-side role
pulls its allowlist."

**HEP-CORE-0036 §6.5** — the auth-refresh section still describes
the pre-topology "producer is binding" pattern.  Body rewrite to
say "binding side" throughout.

## 5. State model per topology

For every legal (topology × transport) cell, list who's on the
binding side, who publishes what, and where the allowlist lives.

| Topology   | Transport | Binding side | data_endpoint owner | allowlist holds |
|------------|-----------|--------------|---------------------|-----------------|
| fan-in     | ZMQ       | consumer     | consumer (PULL bind)| producer pubkeys |
| fan-in     | SHM       | (refused at REG_REQ — gate 1) | — | — |
| fan-out    | ZMQ       | producer     | producer (PUB bind) | consumer pubkeys |
| fan-out    | SHM       | producer     | producer (cap socket bind) | consumer pubkeys |
| one-to-one | ZMQ       | producer     | producer (PUSH bind) | consumer pubkeys |
| one-to-one | SHM       | producer     | producer (cap socket bind) | consumer pubkeys |

Two invariants fall out:

- **The binding side's ZAP allowlist always holds the dialing side's
  pubkeys.**  Renaming `authorized_consumer_pubkeys` reflects this.
- **`data_endpoint` is always owned by the binding side, always
  scalar, always published via `ENDPOINT_UPDATE_REQ` from the
  binding side after `apply_master_approval` completes.**

## 6. Sequence per (topology × transport) — verify the flow

### 6.1 Fan-in ZMQ (verified as design intent)

```
Consumer (binding)                Broker                  Producer (dialing)
   |                                |                          |
   | CONSUMER_REG_REQ (topology=    |                          |
   |   fan-in, transport=zmq)       |                          |
   |------------------------------->|                          |
   |    channel opens (consumer     |                          |
   |    is opener under fan-in)     |                          |
   | <==== CONSUMER_REG_ACK         |                          |
   |    initial_allowlist=[]        |                          |
   |    (no producers admitted yet) |                          |
   |                                |                          |
   | apply_master_approval          |                          |
   | queue: Standby → Configured    |                          |
   |        → Active (PULL bind)    |                          |
   |                                |                          |
   | ENDPOINT_UPDATE_REQ            |                          |
   |------------------------------->|                          |
   |    ChannelEntry.data_endpoint  |                          |
   |    set                         |                          |
   | <==== ENDPOINT_UPDATE_ACK      |                          |
   |                                |                          |
   | HEARTBEAT_REQ (first) -------->|                          |
   |    consumer marked Live         |                          |
   |                                |                          |
   |                                |    REG_REQ (topology=    |
   |                                |    fan-in, transport=zmq)|
   |                                |<-------------------------|
   |                                |    R6 gate check:        |
   |                                |    - consumer Live ✓     |
   |                                |    - endpoint resolved ✓ |
   |                                |    - my_version needs    |
   |                                |      confirmed_version   |
   |                                |    admit producer,       |
   |                                |    bump channel_version, |
   |                                |    pend REG_REQ           |
   | <==== CHANNEL_AUTH_CHANGED_    |                          |
   |    NOTIFY(admitted, prod, P1)  |                          |
   |                                |                          |
   | GET_CHANNEL_AUTH_REQ           |                          |
   |------------------------------->|                          |
   | <==== GET_CHANNEL_AUTH_ACK     |                          |
   |    allowlist=[P1]              |                          |
   |                                |                          |
   | set_peer_allowlist([P1])       |                          |
   |                                |                          |
   | CHANNEL_AUTH_APPLIED_REQ       |                          |
   |------------------------------->|                          |
   |    confirmed_version bumped    |                          |
   |    R6 gate satisfied for       |                          |
   |    pending producer REG_REQ    |                          |
   | <==== CHANNEL_AUTH_APPLIED_ACK |                          |
   |                                |==== REG_ACK              |
   |                                |    initial_allowlist=    |
   |                                |    [{cons_uid, endpoint, |
   |                                |      cons_pubkey}]       |
   |                                |------------------------->|
   |                                |                          |
   |                                |    apply_master_approval |
   |                                |    queue: Standby → Conf |
   |                                |    → Active (PUSH connect|
   |                                |    to endpoint,          |
   |                                |    curve_serverkey=      |
   |                                |    cons_pubkey)          |
   |                                |                          |
   |                                |    CURVE handshake       |
   | <--------- data plane -------------------------------- |
   |    consumer's ZAP admits       |                          |
   |    P1 (already in allowlist)   |                          |
   |                                |                          |
   |                                |    HEARTBEAT_REQ (first) |
   |                                |<-------------------------|
   |                                |    producer marked Live  |
   | <==== CHANNEL_AUTH_CHANGED_    |                          |
   |    NOTIFY(live, prod, P1)      |                          |
   |    (local update to            |                          |
   |     live_peers only, no pull)  |                          |
   |                                |                          |
   |  ... data flows (P1 → cons) ...                          |
```

Second producer arrival re-runs the middle segment; consumer's
allowlist grows by one entry; second producer's queue reaches
Active with the same consumer endpoint but its own CURVE handshake.

### 6.2 Fan-out ZMQ (symmetric — verify)

Structurally identical to §6.1 with producer/consumer roles
swapped.  Producer opens, binds PUB, publishes endpoint, gets
Live.  Consumer arrives, R6 pends until producer admits it,
NOTIFY(admitted, cons, C1) fires to producer, producer refreshes
allowlist, APPLIED_REQ closes the loop, consumer receives REG_ACK
with producer's endpoint, dials SUB, CURVE succeeds.  Consumer
subsequently must gate `on_produce` on `consumer_count()` (PUB
slow-joiner rule per HEP-CORE-0017 §4.7.6).

### 6.3 Fan-out SHM (verify — capability handshake)

Same as §6.2 up to `apply_master_approval` on the consumer side.
There the consumer connects the capability socket, runs
AttachProtocol (HEP-CORE-0044), receives the memfd via
`SCM_RIGHTS`, and maps the DataBlock.  The CURVE handshake in
§6.2 is replaced by AttachProtocol's `crypto_box` handshake.
`initial_allowlist` semantics unchanged — `endpoint` on dialing
side is the capability socket path.

### 6.4 One-to-one ZMQ (subset of §6.2)

Identical to fan-out ZMQ but with PUSH/PULL sockets and
cardinality-1 guard on both sides.  Second CONSUMER_REG_REQ →
`ONE_TO_ONE_CARDINALITY_VIOLATED`.  Second REG_REQ (producer) →
same.

### 6.5 One-to-one SHM

Identical to fan-out SHM with cardinality-1 guard on the consumer
side.

## 7. Error paths — walk each systematically

### 7.1 Admission-time rejections (REG_REQ synchronous errors)

Under the topology model there is no "channel doesn't exist" hard
error for the dialing side.  A dialing peer arriving before its
peer exists is a **wait condition** (§7.2), not a rejection.
`CHANNEL_NOT_FOUND` retires as an admission-time outcome for
dialing-side REG_REQ; it survives only for the DISC_REQ / other
observability queries.

| Cause | Error code | Which cell(s) |
|---|---|---|
| REG_REQ missing / malformed `channel_name`, `role_uid`, `role_type`, `zmq_pubkey`, `data_transport` | `INVALID_REQUEST` | all |
| `channel_topology` is a non-empty non-parseable string | `INVALID_REQUEST` | all |
| `(fan-in, shm)` | `TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT` | fan-in SHM |
| Non-first-arrival with different `channel_topology` than stored | `TOPOLOGY_MISMATCH` | all (existing channel) |
| Second producer on fan-out or one-to-one | `FAN_OUT_IS_SINGLE_PRODUCER` / `ONE_TO_ONE_CARDINALITY_VIOLATED` | fan-out / 1-to-1 |
| Second consumer on fan-in or one-to-one | `FAN_IN_IS_SINGLE_CONSUMER` / `ONE_TO_ONE_CARDINALITY_VIOLATED` | fan-in / 1-to-1 |
| Schema hash / version / id / blds / owner mismatch on existing channel | `SCHEMA_MISMATCH` | all (existing channel) |
| `data_transport` mismatch on existing channel | `TRANSPORT_MISMATCH` | all (existing channel) |
| Same `role_uid` already registered with different pubkey | `UID_CONFLICT` | all |
| ABI major-axis mismatch | `abi_major_mismatch` | all |
| `zmq_pubkey` not in `known_roles` | pre-CURVE rejection (ZAP DENY at CTRL); no REG_ACK ever sent | all |

The dialing-side "channel doesn't exist yet" cases are handled by
§7.2, not by rejection.

### 7.2 R6 pending: the topology-symmetric wait mechanism

**Who pends.**  Any role that is the dialing side for its topology,
per HEP-CORE-0017 §3.3.0 matrix:

- fan-in     — producer
- fan-out    — consumer
- one-to-one — consumer

**Four possible pending conditions**, checked in order.  When any
condition fails, the REG_REQ is parked in the broker's per-channel
pending queue with the failing condition as its reason.  On any
wake event, the entry re-checks from the top; if a later condition
now fails, the reason updates and the entry stays parked.

1. **`awaiting_channel_created`** — the channel doesn't exist at
   all yet.  Broker has admitted no binding-side role for this
   channel_name.  Dialing peer parks; when the binding-side REG_REQ
   creates the channel (see §7.2.2 wake triggers), the pending
   entry is re-evaluated against the freshly-created ChannelEntry
   (topology, schema, transport invariants) before moving on.
2. **`awaiting_binding_side_live`** — channel exists but the
   binding-side role has not sent its first heartbeat yet.
3. **`awaiting_endpoint_resolved`** — binding side is Live but
   `ChannelEntry.data_endpoint` is still unset (binding side
   hasn't sent `ENDPOINT_UPDATE_REQ` yet).
4. **`awaiting_allowlist_confirmed`** — binding side is Live +
   endpoint resolved, but `confirmed_version[K][B] < my_version`
   (where B is the binding-side role_uid).  The binding side
   hasn't confirmed applying the allowlist mutation that admitted
   this dialing peer's pubkey.

Under the topology model the second index of `confirmed_version`
is the binding-side role_uid — one binding side per channel by
cardinality, so the mapping is unambiguous.

#### 7.2.1 Pending-entry contents

Each pending entry stores enough state to complete admission
whenever it wakes:

```
{ channel_name, role_uid, role_type, zmq_identity, pubkey_z85,
  declared_topology, schema_invariants, transport_invariants,
  my_version (0 if not yet assigned — assigned at wake for
  awaiting_channel_created; assigned at initial pend otherwise),
  pending_reason, expiry, correlation_id }
```

The `my_version` field is filled at the moment the peer's pubkey
is added to the allowlist.  For pending reasons 2/3/4 that happens
at initial pend (broker admits the peer + adds pubkey +
optimistically bumps `channel_version` at REG_REQ arrival).  For
`awaiting_channel_created` the admission is deferred to wake
time — no channel means no allowlist to add to.

#### 7.2.2 Wake triggers

Broker re-scans the pending queue for the affected channel_name on
each of these events:

- **Channel created.**  A binding-side REG_REQ opened a new
  `ChannelEntry`.  All pending entries with
  `awaiting_channel_created` for that channel_name re-evaluate:
  broker runs admission (topology + cardinality + schema + transport)
  against the fresh channel; if admitted, optimistically add pubkey,
  bump `channel_version`, assign `my_version`, transition to the
  next R6 condition.
- **Heartbeat arrived from binding side.**  Any entry parked on
  `awaiting_binding_side_live` re-checks presence state.
- **`ENDPOINT_UPDATE_REQ` processed.**  Any entry parked on
  `awaiting_endpoint_resolved` re-checks `data_endpoint`.
- **`CHANNEL_AUTH_APPLIED_REQ` processed.**  Any entry parked on
  `awaiting_allowlist_confirmed` re-checks `confirmed_version[K][B]`
  against its `my_version`.
- **Channel closed** (binding side deregistered or heartbeat-timed
  out).  All pending entries for this channel drain with terminal
  error (`CHANNEL_CLOSING` or equivalent); channel teardown fans
  `CHANNEL_CLOSING_NOTIFY` to any dialing peers that had already
  been released.

#### 7.2.3 Pending budget and BRC-timeout coupling

Broker holds each pending entry for at most `pending_budget_ms`.
On expiry: sweep removes the entry, sends error reply with
`CHANNEL_NOT_READY` and a reason string naming the last unmet
condition.  If the peer's pubkey had been optimistically added to
the allowlist (any pending reason except `awaiting_channel_created`
at the moment of expiry), sweep also removes the pubkey, bumps
`channel_version`, and fires `CHANNEL_AUTH_CHANGED_NOTIFY(left)`
to the binding side — cleaning up the optimistic admission so
a compromised keypair cannot exploit the expired slot.

**INVARIANT.**  Client BRC's REG_REQ ACK timeout must be
**greater than or equal to** the broker's `pending_budget_ms`.
Otherwise the client gives up while broker is still holding the
pending entry, and retry REG_REQs collide with the parked
original.  The two knobs are jointly tuned by operators:

- Short (5 s / 5 s) — cheap coordination on a single host or a
  quiet LAN; roles fail-fast if their peers aren't already up.
- Long (60 s / 60 s) — tolerates coordinated multi-machine
  startup over congested networks; peers wait for each other
  without fail-fast.

#### 7.2.4 Duplicate REG_REQ handling

Under the long-window model a client may retry REG_REQ (with
possibly-changed zmq_identity if its BRC socket cycled) while
the broker is still holding the original pending entry.  Broker
detects same-uid arrivals in the pending queue and applies:

- **Same uid + same pubkey + same zmq_identity** → network-layer
  duplicate.  Drop silently; original pending entry stays.
- **Same uid + same pubkey + new zmq_identity** → BRC reconnect
  with fresh identity.  Replace the pending entry's `zmq_identity`
  (so the eventual REG_ACK routes correctly), keep the optimistic
  pubkey admission, do NOT bump `channel_version` (allowlist
  unchanged).
- **Same uid + different pubkey** → `UID_CONFLICT` reject.
  Fresh REG_REQ with a mutated pubkey looks like an impersonation
  attempt (or a keypair rotation without proper DEREG); reject
  synchronously.

### 7.3 Runtime failure modes

**Dialing peer's CURVE handshake fails.**  Under the clean model this
should not happen if R6 gated correctly — the binding side's
allowlist admitted the pubkey before REG_ACK was sent.  If it does
happen: libzmq retries every ~100 ms; a
`CHANNEL_AUTH_CHANGED_NOTIFY` fire due to a subsequent event may
re-seed and unblock.  Symptom: producer's PUSH stays connected but
sends drop; ZAP router log shows repeated denials.

**Consumer / producer heartbeat times out (`kStalled`).**  Broker
transitions its presence to `Pending`.  If it recovers, presence
goes back to `Connected`.  If it stays `Pending` beyond the timeout,
Broker fires `CHANNEL_AUTH_CHANGED_NOTIFY(phase=left)` to the
binding side (allowlist removes pubkey) and, if this was the last
binding-side presence, tears the channel down.

**Broker crashes mid-registration.**  Client's BRC retries.  On
reconnect, REG_ACK carries a fresh `initial_allowlist` snapshot;
whatever was in-flight before the crash is re-synced by that
snapshot.  `channel_version` semantics survive by being
monotonic-in-`instance_id` per HEP-CORE-0042 §5.2.

**Binding-side role restarts (crash-restart).**  Its `role_uid`
returns with a new `instance_id`; broker treats the fresh REG_REQ
as re-admission; `producer_instance[P]` bumps; any in-flight
`CHANNEL_AUTH_APPLIED_REQ` from the pre-crash instance is dropped
by the stale-instance guard.  Dialing-side peers on the channel
lose their sockets when the binding side dies; their queues either
reconnect (libzmq layer) and re-authenticate against the fresh
allowlist, or their role dies and re-REGs entirely.

**Consumer opens fan-in channel then disconnects before any
producer arrives.**  Channel teardown (binding side owns lifetime).
No dialing peers exist yet; nothing to fan CHANNEL_CLOSING_NOTIFY
to.  Clean state, clean teardown.

**Two consumers race to open the same fan-in channel simultaneously.**
Broker's writer lock serializes admission.  First wins; second sees
existing channel with `topology=FanIn`, `consumers.size()==1`, hits
the cardinality gate → `FAN_IN_IS_SINGLE_CONSUMER`.

**Producer arrives at fan-in channel before consumer publishes
endpoint.**  R6 pends (`awaiting_endpoint_resolved`).  When
consumer's ENDPOINT_UPDATE_REQ lands, broker wakes the pending
queue; producer's REG_ACK finally sent with the resolved endpoint.

**Role that's a consumer of channel A and a producer of channel B.**
Two independent REG cycles.  Nothing in the protocol couples them.
Presence rows are keyed on `(channel, role_type)`.

### 7.4 Wire-schema-mismatch resilience

**Broker sends new REG_ACK shape to old client** — old client
looks for `data_endpoint` scalar, doesn't find it, treats as
malformed and fails startup.  Verdict: the wire scalars must stay
during a transition period OR clients must be upgraded first.
Given the queue abstraction migration has already touched every
role-side callsite, the cleanest cut is: retire the scalars in
the same commit that retires `CONSUMER_ATTACH_REQ_ZMQ`.  No
partial-transition tolerance.

**Old broker sends legacy ACK to new client** — reverse case.
Also disallowed; both sides ship the retire in one atomic
version bump.

## 8. Symmetric R6 gate — the load-bearing piece

The R6 gate is the single mechanism that guarantees CURVE
handshakes succeed on first attempt.  It works by holding a
dialing peer's REG_ACK until the binding side has confirmed
applying that peer's pubkey to its ZAP.  Under the topology model
the gate is symmetric across which side dials — the same four
conditions apply regardless of topology; only the "binding side"
label changes.

| Topology   | Dialing side | Binding side (owns `data_endpoint` + allowlist) |
|------------|--------------|-------------------------------------------------|
| fan-in     | producer     | consumer                                         |
| fan-out    | consumer     | producer                                         |
| one-to-one | consumer     | producer                                         |

Full four-condition gate on any dialing-side REG_REQ (repeat from
§7.2 for cross-reference):

1. Channel with matching `channel_name` + `topology` exists.
2. Binding-side role is Live (first heartbeat received).
3. `ChannelEntry.data_endpoint` is resolved
   (binding side sent `ENDPOINT_UPDATE_REQ`).
4. `confirmed_version[K][B] >= my_version` — the binding-side role
   B has applied an allowlist snapshot that includes this dialing
   peer's pubkey.

The `confirmed_version` map is keyed on `[K][binding-side-uid]` —
one binding side per channel by cardinality, so the second index
is unambiguous.

**Wake triggers** re-run the gate from the top on:

- Channel created (satisfies condition 1).
- Heartbeat that transitions binding-side presence to Live
  (condition 2).
- `ENDPOINT_UPDATE_REQ` processed (condition 3).
- `CHANNEL_AUTH_APPLIED_REQ` processed (condition 4).
- Channel closed → pending entries drain with terminal error.

The gate must fire for all four conditions across all three
topologies — the pre-migration wiring supported only the
producer-binding path.  The clean-up extends the wake triggers to
be topology-driven, not role-type-hardcoded.

### 8.1 Protocol invariants (load-bearing)

These are not tunable — they are the assumptions the design's
correctness rests on.  Any implementation change that violates
one of these breaks the protocol.  21 invariants grouped by
concern: state / ordering (4), identity (4), security (3), wire
envelope (2), delivery (3), state model (2), policy (3).

#### State / ordering

**I-ROUTER-SERIAL.**  Broker's ROUTER dispatch runs on a single
thread.  All admission mutations, pending-queue mutations, and
wake-trigger scans execute serialized by that thread.  Any future
thread that also mutates channel state (federation relay,
background sweep) must either pipe through the ROUTER thread or
add its own synchronization to the pending queue.

**I-STATE-MUTATION-ATOMIC.**  Every mutation on HubState that
affects admission runs under HubState's writer lock (unique_lock).
Every read that participates in an admission decision runs under
shared_lock or is captured to a value copy under one such lock.
The writer lock is the sole ordering primitive for allowlist +
channel_version + confirmed_version consistency.  I-ROUTER-SERIAL
ensures dispatch-thread serialization; I-STATE-MUTATION-ATOMIC
ensures lock ownership.

**I-OPT-ADMIT.**  When a dialing-side REG_REQ passes the topology,
cardinality, and schema gates, its CURVE pubkey is added to the
binding side's channel-scope allowlist BEFORE REG_ACK is sent
(optimistic admission).  This lets the version-bump → NOTIFY →
pull → APPLIED_REQ chain converge before the dialing peer's CURVE
handshake fires.  Rollback on pending expiry or DEREG /
heartbeat-timeout.  Window bounded by `pending_budget_ms`.

**I-MONOTONIC-VERSION.**  `channel_version[K]` monotonically
increases per channel over its lifetime (from creation to close).
Bumped on any allowlist mutation (admit or revoke); unchanged on
`phase=live` NOTIFYs (pure observability).
`confirmed_version[K][B]` monotonically increases per binding-side
role instance; resets on binding-side role re-registration
(new `instance_id`).

#### Identity

**I-DEALER-IDENTITY.**  For every control-plane DEALER↔ROUTER
connection between a role and a broker, the DEALER MUST set
`ZMQ_ROUTING_ID` to its `role_uid` before `connect()`.  The
broker MUST verify at REG admission that the ROUTER-captured
identity frame equals the payload's `role_uid`; mismatch →
`INVALID_REQUEST error_code="IDENTITY_MISMATCH"`.  Extended to
hub-to-hub federation DEALERs (routing_id = sending hub's
`hub_uid`) per §11.  Rationale: libzmq's default DEALER identity
is `rand()`-derived and collides across fresh processes; without
a stable per-role identity, unsolicited sends silently misroute.

**I-PUBKEY-BINDING.**  Every entry in `known_roles` names a
distinct `(role_uid, zmq_pubkey)` pair.  The pair is
bidirectionally unique: no two entries share a role_uid, no two
share a zmq_pubkey.  Broker startup rejects a config that violates
this.  At REG admission the broker verifies (a) the payload's
`(role_uid, zmq_pubkey)` matches an entry and (b) the DEALER's
presented CURVE certificate (curve_publickey in ZAP AUTH) equals
the payload's `zmq_pubkey`.

**I-CURVE-IS-DECLARED.**  A role's CURVE identity is a single
value applied uniformly: (a) BRC DEALER's `curve_publickey` at
CTRL ROUTER connect, (b) data-plane socket's `curve_publickey`
at connect / bind, (c) the `zmq_pubkey` field on every REG-family
message.  Reading the pubkey from more than one config field
produces silent "authenticates on control plane, denied on data
plane" splits and is a design violation.  Role-side keystore is
the single source (`secure().keys().pubkey(keystore_name)`).

**I-CHANNEL-SINGLE-BINDING-SIDE.**  For every channel K at every
instant t, `ChannelEntry.binding_side_uid()` returns exactly one
role_uid or `nullopt` (channel not yet opened).  Enforced by the
cardinality gate at REG admission (fan-in: 1 consumer; fan-out /
one-to-one: 1 producer).  Every `confirmed_version[K][B]` write
uses B = `binding_side_uid()` at write time.

#### Security

**I-REPLAY-BOUND.**  Every REG_REQ, CONSUMER_REG_REQ,
ENDPOINT_UPDATE_REQ, CHANNEL_AUTH_APPLIED_REQ, DEREG_REQ,
CONSUMER_DEREG_REQ carries a `client_nonce` (16 bytes,
cryptographically random) AND a `client_wall_ts` (uint64,
milliseconds since epoch).  Broker rejects with
`error_code=REPLAY_OR_SKEW` when (a) `client_nonce` reuse within
a sliding window of `2 * pending_budget_ms` OR (b)
`|broker_wall_ts - client_wall_ts| > 30_000` ms.  Nonce dedup
structure per-role_uid, in-memory, sliding-window pruned.
Rationale: CURVE encrypts frames; it does NOT prevent replay of a
captured message after the legitimate role has DEREG'd.

**I-ENVELOPE-BODY-BINDING.**  Every msg_type body (Frame 4)
includes an `envelope_hash` field computed as
`BLAKE2b-256(Frame0_identity || Frame2_msg_type ||
Frame3_correlation_id)` by the sender.  `WireEnvelope::parse`
recomputes and rejects with `error_code=ENVELOPE_TAMPERED` on
mismatch.  Rationale: CURVE encrypts each frame independently; an
attacker with captured messages could splice a Frame 4 body from
message X onto Frame 3 correlation_id / Frame 2 msg_type from
message Y.  Hash binding refuses spliced bodies.

**I-KEY-ROTATION-VIA-DEREG.**  A role's CURVE pubkey is immutable
for a given `(role_uid, broker_lifetime)` pair.  Any REG-family
request whose `zmq_pubkey` differs from the role's currently-
registered pubkey is rejected with `error_code=UID_CONFLICT`
(already-registered role reregistering under new pubkey) or
`KEY_ROTATION_REQUIRES_DEREG` (post-DEREG re-REG with pubkey
different from known_roles).  Key rotation is an operator action:
(1) DEREG the role, (2) update `known_roles` config, (3) role
re-REG with new key.  No in-band rotation, no dual-pubkey window.

#### Wire envelope

**I-WIRE-ENVELOPE.**  Every control-plane message is 5 ZMQ frames:
4 skeleton frames (Frame 0 = identity, Frame 1 =
`kFrameTypeControl` byte `'C'`, Frame 2 = msg_type ≤64 bytes ASCII,
Frame 3 = correlation_id ≤64 bytes ASCII) + 1 body frame (Frame 4,
per-msg-type JSON schema parsed via a typed body class).  Skeleton
frames are identical shape across every REQ / ACK / NOTIFY.  NO
handler reads a wire field via JSON key extraction; every wire
field is exposed as a typed accessor on either `WireEnvelope`
(Frames 0/2/3) or a msg_type-specific body class (Frame 4 fields).
Adding a new msg_type = one new body class + one dispatch-table
entry.  Full frame layout + body class catalog in §14.

**I-WIRE-VERSION-ATOMIC.**  The typed envelope is not additive
over pre-migration wire.  Every deployed component ships the
envelope on the same version cut.  `broker_proto` on the REG
envelope MUST match the current threshold; mismatch →
`UNSUPPORTED_PROTO` reject at envelope parse.  No runtime
tolerance for mixed-envelope deployments — mixed old/new breaks
security invariants for the duration of the mix.

#### Delivery

**I-CORRELATION-STABLE.**  Every REQ MUST carry a non-empty
`correlation_id`; every ACK MUST echo it.  Empty correlation_id
on a REQ is rejected at `WireEnvelope::parse` with
`error_code=INVALID_REQUEST`.  BRC's `pending_requests` map keys
on `(msg_type, correlation_id)`.  No grace-period generation on
the broker side — grace periods are documented attack windows
under the CURVE frame.

**I-NOTIFY-BEST-EFFORT.**  Fire-and-forget NOTIFYs are best-
effort.  `ZMQ_ROUTER_MANDATORY` is left at default (0), so a send
to a currently-disconnected identity is silently dropped.
Recovery: (a) next NOTIFY for the same channel_name pulls current
state, (b) pending-queue timeout releases the dialing peer with
`CHANNEL_NOT_READY` triggering client-side re-REG, (c) binding-
side reconnect resyncs via `REG_ACK.initial_allowlist`.

**I-ROUTER-NOT-MANDATORY.**  Broker's CTRL ROUTER MUST NOT set
`ZMQ_ROUTER_MANDATORY`.  The pending-queue + I-NOTIFY-BEST-EFFORT
design relies on silent drops of sends to disconnected identities
for transient reconnect windows; mandatory-mode would surface
`EHOSTUNREACH` exceptions the current design neither catches nor
benefits from.

#### State model

**I-INSTANCE-ID-EPHEMERAL.**  `instance_id` is monotonic within
one broker process lifetime; not persisted across broker restart.
Stale-instance guards (HEP-CORE-0042 §5.5.3) protect against
in-flight ACK reordering within a broker lifetime, not across
restarts.  Broker restart drops all in-flight admissions; clients
re-register and receive fresh instance_ids.

**I-PENDING-EPHEMERAL.**  The broker's pending REG_REQ queue is
in-memory and lost on broker restart.  Clients whose REG_REQ was
pending re-REG on their next BRC reconnect; broker's fresh
pending queue accepts them.  No persistence of pending state
across restart.

#### Policy

**I-BRC-BUDGET.**  `client_brc_reg_ack_timeout >= pending_budget_ms`.
Enforced by operator configuration; violation causes retry storms
and admission ambiguity (§7.2.4).

**I-REQ-IDEMPOTENT.**  Every control-plane REQ is idempotent under
the `(role_uid, correlation_id)` key.  A duplicate REQ with the
same key is either dropped as network-layer duplicate (broker
already replied) or re-processed as a retry (broker still
pending); in neither case does state mutation occur twice.
Coupled with I-CORRELATION-STABLE + I-REPLAY-BOUND.

**I-MSG-TYPE-TAXONOMY.**  msg_type suffix `_NOTIFY` ⇒ fire-and-
forget (no ACK, best-effort per I-NOTIFY-BEST-EFFORT); suffix
`_REQ` ⇒ request-reply requiring `_ACK` or `ERROR`.  Any deviation
(e.g., a NOTIFY that expects an ACK) is a wire violation.
Enforced by BRC shape-conformance check per HEP-CORE-0007 §12.2.1.

## 9. Open questions

**Q1 — Where does the R6 gate's `my_version` come from for the
fan-in producer path?**  Under the old model, when a producer
admits a consumer, the broker bumps `channel_version[K]` and
assigns that value to the incoming consumer as `my_version`.
Symmetrically for fan-in, when broker admits a producer's pubkey
to the consumer's allowlist, `channel_version[K]` bumps and the
producer's `my_version` is the post-bump value.  Then the
consumer's APPLIED_REQ advances `confirmed_version[K][consumer]`.
Producer's REG_ACK is released when `confirmed_version >=
my_version`.  Same mechanism; just symmetrical role labeling.
Verify: this is what the current broker code does when the
`_on_consumer_authorized` call is invoked with a producer pubkey
under fan-in (the misnamed field mutation).

**Q2 — Does the retire of `CONSUMER_ATTACH_REQ_ZMQ` break the
fan-in-producer-pending flow if the producer arrives before the
consumer publishes endpoint?**  No — that flow is handled by R6
pending REG_REQ, not by attach.  Attach was for per-producer
endpoint resolution AFTER admission; R6 pends admission itself.
Different mechanisms; retire is orthogonal.

**Q3 — Under fan-in with N producers, is `channel_version`
bumped once per producer admission or once per allowlist
mutation?**  Once per allowlist mutation (per HEP-CORE-0042 §5.2).
N producers admitted in quick succession may result in one
version bump per producer (each bump triggers a NOTIFY and pull;
each pull may see later-admitted producers already in the
allowlist — no correctness issue, just some redundant notifies).

**Q4 — When a fan-in producer's REG_REQ pends, when should its
pubkey be added to the allowlist?**  Two options:
(a) At pend time — optimistic; consumer's ZAP admits pubkey
    before producer's REG_ACK sent.  Simpler; requires the
    stale-pubkey cleanup on pend timeout described in §7.2.
(b) At release time — after R6 clears.  Cleaner rollback; but
    then consumer's pull happens AFTER release, so the producer
    has to wait an extra RTT before its REG_ACK lands.
Verdict: (a) is what the current code does and what the
symmetric case should do too.  (a) matches HEP-CORE-0042 §5.4's
optimism model.

**Q5 — Should the wire-field `initial_allowlist` be renamed given
its dialing-side semantic is "the one peer to dial"?**  Rename
would break older clients; the semantic-on-role-side pattern is
already documented (HEP-CORE-0036 §6.2 rev 2.3).  Keep the name;
document the topology-driven semantic clearly.

**Q6 — Does the ROUTER routing / BRC handler pattern generalize
cleanly to fan-in consumer receiving producer-side notifies?**
Empirically no (P2 uncovered the gap).  Root cause investigation
needed: whether it's a subscription/filter issue in the BRC poll
handler, or a `zmq_identity` mismatch between the identity broker
holds and the identity BRC's REP socket receives on, or something
else.  This is a bug to fix, not a design question — the design
says the notify must reach the binding side regardless of which
side that is.

**Q7 — What ordering guarantees does the protocol give for
NOTIFY vs. next REG_ACK from a related REG?**  Broker's ROUTER
dispatch thread serializes.  Under fan-in the flow is:
producer REG_REQ arrives → broker mutates state (add pubkey +
bump version) → broker fires NOTIFY to consumer → broker pends
producer REG_REQ.  Consumer receives NOTIFY, pulls, applies,
sends APPLIED_REQ.  Broker's APPLIED_REQ handler advances
`confirmed_version`, releases producer REG_REQ.  The producer
sees REG_ACK come AFTER the consumer has applied its pubkey.
No client-side coordination needed.

## 10. Test implications

**Tests that delete with the code:**
- All 8 `Pattern4AttachCoordinationTest` cases.  They pin the
  retiring `CONSUMER_ATTACH_REQ_ZMQ` protocol.  The scenarios
  they cover (broker admits attach, pends on producer readiness,
  denies stale, drains on timeout) reduce to R6 gate scenarios
  on the REG_REQ path itself, tested elsewhere.
- `IdleProducerYieldsTimeoutDrainToConsumer` — same reason.
- `consumer_reg_ack_emits_producers_zmq` — pins the retired
  `producers[]` array shape.

**Tests that refactor onto topology-model equivalents:**
- `multi_producer_partial_pending_timeout` — scenario ("some but
  not all peers reach R6-condition-satisfied") is still real for
  fan-in with N producers registering simultaneously.  Refactor
  onto R6-pending-timeout observation on the REG path.
- L4 `ZmqE2E_MultiProducer_TwoAuthorized` — the SCENARIO ("two
  producers into one consumer, verify data arrives from both") is
  precisely fan-in.  Marker refactor already done; blocked on
  Q6's NOTIFY-routing bug.

**Tests that gain coverage the clean protocol creates:**
- Symmetric R6: fan-in producer REG_REQ pends on
  `awaiting_binding_side_live`, `awaiting_endpoint_resolved`, and
  `awaiting_allowlist_confirmed`.  Test each pending condition +
  wake trigger + pending timeout.
- Topology-driven ENDPOINT_UPDATE_REQ acceptance: fan-in consumer
  is legal sender; fan-in consumer's peer producer is rejected as
  `NOT_CHANNEL_OWNER`.  Symmetric across three legal cells.
- Topology-driven GET_CHANNEL_AUTH_REQ / _APPLIED_REQ acceptance:
  same rule.
- Unified `initial_allowlist` shape on REG_ACK across all five
  legal cells — verify binding side gets 0..N pubkey-only entries;
  dialing side gets exactly 1 `{endpoint, pubkey_z85}` entry.
- ROUTER routing coverage: NOTIFY reaches the binding-side role
  regardless of which side (Q6 bug).

## 11. What this document does not cover

- SHM capability handshake internals — HEP-CORE-0041, HEP-CORE-0044.
  Unaffected.
- Schema registry mechanics — HEP-CORE-0034.  Unaffected.
- Federation / cross-hub — HEP-CORE-0033 §12.  Deferred; not
  affected by REG shape.
- Inbox — HEP-CORE-0027.  Endpoint field retires from
  ENDPOINT_UPDATE_REQ (inbox is set at REG time and immutable
  thereafter).
- Bands — HEP-CORE-0030.  Independent of channel REG.

## 12. Sequencing to implement

The design is locked; sequencing follows the invariant-first
discipline — wire skeleton before handlers, security invariants
before topology admission, atomic wire cut for the whole chain.

**Phase A — Typed envelope skeleton (foundation).**

1. `WireEnvelope` class (utils/network_comm/wire_envelope.hpp) —
   build / parse the 5-frame envelope per §14.2.  Validates
   envelope↔body hash (I-ENVELOPE-BODY-BINDING) at parse.  L1
   unit-tested.
2. Per-msg-type typed body classes
   (utils/network_comm/wire_bodies.hpp) — one class per msg_type
   per §14.3.  Every REG-family body exposes `client_nonce()`,
   `client_wall_ts()`, `envelope_hash()`.  L1 unit-tested.

**Phase B — BRC + broker rewired to envelope.**

3. BRC: DEALER `routing_id` set from `Config::role_uid`
   (I-DEALER-IDENTITY); refuse-connect on empty.  Send + receive
   paths route through `WireEnvelope::build` / `parse`.  ACK
   matching keys on `(msg_type, correlation_id)`.  Every REQ
   constructor stamps `client_nonce` + `client_wall_ts`
   (I-REPLAY-BOUND).
4. Broker ROUTER poll-loop: parse via `WireEnvelope`, dispatch on
   `env.msg_type()`.
5. Handler-by-handler sweep: replace every JSON-key extraction
   (`req.value("channel_name")`, `body.value("role_uid")`, etc.)
   with typed accessors on `WireEnvelope` (skeleton) or the
   msg_type body class.  Every handler signature becomes
   `handle_XXX(const WireEnvelope& env, const XxxBody& body, ...)`.

**Phase C — Admission-gate ordering (security first).**

6. Broker REG-family handlers: run gates 1-7 per §14.5 BEFORE any
   state mutation — envelope hash, broker_proto, identity match,
   grammar, known_roles binding, key-rotation, anti-replay.
7. `HubState` API additions: `ChannelEntry::binding_side_uid()`,
   `HubState::is_binding_side_sender()`, `HubState::nonce_seen()`.
8. §2.3 / §2.5 / §2.6 handlers: replace bespoke sender-validation
   with `is_binding_side_sender`.
9. Broker startup: `known_roles` reverse-uniqueness check
   (I-PUBKEY-BINDING); reject config with duplicate pubkeys.

**Phase D — Retirements enabled by the invariants.**

10. Retire `ConsumerEntry::zmq_identity`,
    `ProducerEntry::zmq_identity`, `BandMember::zmq_identity`
    (D1).  Every call site `x.zmq_identity` → `x.role_uid`.
    Every `send_to_identity(x.zmq_identity, ...)` becomes
    `WireEnvelope::build(x.role_uid, ...)` + ROUTER send.
11. Rename misnomers: `authorized_consumer_pubkeys` →
    `authorized_peer_pubkeys`, `_on_consumer_authorized` →
    `_on_peer_authorized`.
12. Retire attach protocol atomically: broker code + role code +
    BRC method + 10 Flavor-A tests + wire schema doc.
13. Refactor the 2 Flavor-B tests onto R6-pending observations.
14. Retire scalar `data_endpoint` / `data_pubkey` on REG_ACK
    (subsumed by `initial_allowlist`).
15. Retire `ProducerEntry.zmq_node_endpoint` +
    `_set_producer_zmq_node_endpoint` + per-producer endpoint API.
16. Symmetric R6 gate: extend pending-REG queue + wake triggers
    for the fan-in producer path.
17. Retire `RxQueueOptions.producer_peers` + `ProducerPeer` struct
    + `set_producer_peers` / `add_producer_peer` /
    `remove_producer_peer` on `ZmqQueue`.
18. Retire `zmq_bind` config field + `Rx/TxQueueOptions.zmq_bind`
    + `TransportConfig.zmq_bind`.

**Phase E — Coverage tests.**

19. L2: BRC's DEALER `routing_id` equals `Config::role_uid`.
20. L2: broker rejects identity-mismatched REG_REQ
    (`IDENTITY_MISMATCH`).
21. L2: `WireEnvelope::parse` rejects tampered envelope
    (`ENVELOPE_TAMPERED`).
22. L2: broker rejects replay (nonce reuse) and skew (clock)
    (`REPLAY_OR_SKEW`).
23. L2: broker rejects in-band key rotation
    (`KEY_ROTATION_REQUIRES_DEREG`).
24. L3: broker startup rejects duplicate-pubkey `known_roles`.
25. L4: fan-in end-to-end passes without any identity workaround
    (task #95 falls out here).
26. L4: BAND_JOIN with 3 members — each receives its own NOTIFY.
27. L4: DEREG + config update + re-REG succeeds (A11 operator
    procedure).

**Phase F — Federation follow-on.**  I-DEALER-IDENTITY extended
to hub-to-hub DEALERs (G1); HEP-CORE-0022 addendum tracks.

Each phase passes ctest before the next lands.  Phases A + B are
one atomic commit (envelope + handler sweep together — mixed
old/new envelope on the wire breaks I-CORRELATION-STABLE +
I-REPLAY-BOUND for the mix duration).  Phases C-E can ship as
sequential commits with `broker_proto` unchanged.

## 13. Scenario walkthrough + validation findings

This section walks each legal (topology × transport) cell end-to-end
under both happy path and every non-trivial error path.  For each
sub-scenario the walk states: what messages fly in what order, what
state the broker + roles + queues are in at each step, and whether
the design produces the correct outcome.  Findings are called out
inline as **[F#]** and consolidated in §13.6.

### 13.1 Fan-in ZMQ (the most complex cell)

**Baseline — 1 consumer, 1 producer, consumer arrives first, R6 clears cleanly.**

- Consumer CONSUMER_REG_REQ arrives at broker.  Channel doesn't
  exist; consumer is the topology-legal opener under fan-in;
  broker creates ChannelEntry with `topology=FanIn`,
  `consumers=[C1]`, empty allowlist.  CONSUMER_REG_ACK sent with
  `initial_allowlist=[]` (empty on binding side when no dialing
  peer has arrived yet).
- Consumer's role host calls `apply_master_approval`.  Queue
  drives Standby → Configured → Active.  PULL binds `tcp://host:0`,
  OS picks port 38815.
- Consumer sends `ENDPOINT_UPDATE_REQ(zmq_node, tcp://host:38815)`.
  Broker sender-validates: consumer C1 is in `consumers[]`,
  topology=FanIn ⇒ sender is binding side ⇒ accept.  Broker sets
  `ChannelEntry.data_endpoint = tcp://host:38815`.
- Consumer sends first HEARTBEAT_REQ.  Broker marks C1 Live.
- Producer P1 REG_REQ arrives.  Broker admits: topology matches,
  cardinality OK (1..N producers), schema matches.  Adds P1 to
  `producers[]`.  R6 gate check:
  - Consumer Live? Yes.
  - `data_endpoint` resolved? Yes.
  - Allowlist confirms P1? P1's pubkey is not yet in allowlist —
    broker adds it (optimistic add), bumps `channel_version` to 1.
    Sets P1's `my_version=1`.  `confirmed_version[K][C1]` is still 0
    (consumer hasn't applied anything yet).  R6 condition 3 fails.
  - Pend P1's REG_REQ in per-channel queue keyed on
    `awaiting_allowlist_confirmed`.
- Broker fires
  `CHANNEL_AUTH_CHANGED_NOTIFY(admitted, role_type=producer,
  role_uid=P1, channel_version=1)` to C1's `zmq_identity`.
- C1's BRC handler processes NOTIFY.  Sends
  `GET_CHANNEL_AUTH_REQ`.  Broker replies with `allowlist=[P1]`,
  `channel_version=1`.
- C1's role host calls `set_peer_allowlist([P1])` on rx_queue.
  Queue's ZAP admission map updates.
- C1 sends `CHANNEL_AUTH_APPLIED_REQ(applied_version=1)`.  Broker's
  handler: bumps `confirmed_version[K][C1]` to 1.  Broker scans
  the pending REG_REQ queue for channel K: P1's `my_version=1 <=
  confirmed_version=1` — condition satisfied.  Builds REG_ACK
  for P1: `initial_allowlist=[{role_uid: C1, endpoint:
  tcp://host:38815, pubkey_z85: C1_pubkey}]`.  Sends to P1.
- P1's role host calls `apply_master_approval`.  Queue drives
  Standby → Configured → Active.  PUSH connects `tcp://host:38815`
  with `curve_serverkey=C1_pubkey`.  CURVE handshake fires; C1's
  ZAP has P1 in allowlist ⇒ ALLOW ⇒ handshake succeeds.
- P1 sends first HEARTBEAT_REQ.  Broker marks P1 Live.  Fires
  `CHANNEL_AUTH_CHANGED_NOTIFY(live, producer, P1)` to C1.
- C1's role host updates `live_peers[K]["producer"].insert(P1)`.
  `api.producer_count()` returns 1.
- Data flows P1 → C1.  End of baseline.

**Outcome:** correct.  No races, no drops.  **[F1] Verified.**

---

**Scenario 13.1.a — Producer arrives BEFORE consumer.**

- P1 REG_REQ arrives.  Channel doesn't exist.  P1 is a producer;
  under fan-in the producer is DIALING side, not the topology-legal
  opener.  Broker's admission logic checks: channel missing +
  declared_topology=FanIn + role is not the legal opener.
  Correct behavior: pend the REG_REQ in the per-channel queue
  keyed on `awaiting_channel_created` (a new pending reason we
  need since the channel is missing entirely).

  **[F2] The current draft doesn't enumerate `awaiting_channel_created`
  as a pending reason.  Add it to §7.2 — R6 has FOUR conditions
  under fan-in, not three: (0) channel exists AT ALL, (1) binding
  side Live, (2) endpoint resolved, (3) confirmed_version reaches
  my_version.**  Also: the broker currently rejects this case with
  CHANNEL_NOT_FOUND (not fan-in-aware).  Per the design, fan-in
  producer's missing-channel case is pending, not rejection.
  The `CHANNEL_NOT_FOUND` reject path stays for the wrong-topology
  case (fan-out consumer or 1-to-1 consumer with missing channel —
  those cannot open the channel).

- Later C1 CONSUMER_REG_REQ arrives.  Channel doesn't exist; C1
  is the legal opener.  Broker creates ChannelEntry, admits C1,
  publishes empty allowlist ACK.
- Broker's channel-creation path should NOW wake any pending
  REG_REQ queued on this channel with `awaiting_channel_created`.

  **[F3] The channel-open wake trigger is a new mechanism.  The
  current pending-queue wake is triggered by heartbeat, ENDPOINT_UPDATE,
  and APPLIED_REQ — not by channel creation.  Add a wake trigger to
  `_on_channel_opened` handler chain that scans the pending queue.**

- P1's REG_REQ resumes.  Re-runs admission (topology and cardinality
  now check against the fresh channel).  P1 admitted.  Continues
  into `awaiting_binding_side_live` (C1 hasn't heartbeated yet)
  → `awaiting_endpoint_resolved` (C1 hasn't published endpoint yet)
  → `awaiting_allowlist_confirmed`.  Same sequence as baseline
  from consumer's ENDPOINT_UPDATE onward.

**Outcome:** correct after F2 and F3 fixes.  Without them, P1's
REG_REQ is rejected outright and P1's role host must retry — burns
CPU and adds latency but eventually converges via BRC's
`CHANNEL_NOT_READY` retry loop.  Cleaner to pend.

---

**Scenario 13.1.b — Two producers admitted concurrently.**

- C1 registered, endpoint published, Live.
- Broker's writer lock serializes.  P1 REG_REQ processed first:
  admitted, version bumped 0 → 1, pending on
  `awaiting_allowlist_confirmed`, NOTIFY fired.  P2 REG_REQ
  processed second: admitted, version bumped 1 → 2, pending, NOTIFY
  fired.
- C1's BRC handler processes NOTIFYs in receive order.  First
  NOTIFY: pull returns `allowlist=[P1, P2], channel_version=2`
  (broker's pull is atomic against the current state, not the
  notified state).  C1 applies both.  APPLIED_REQ(v=2).
- Broker's APPLIED_REQ handler: bumps `confirmed_version[K][C1]`
  from 0 to 2.  Scans pending queue: P1's my_version=1 <= 2 ✓,
  P2's my_version=2 <= 2 ✓.  Both released.  REG_ACKs sent to
  both.
- Second NOTIFY (for P2's admission) arrives at C1.  Consumer
  pulls again — sees `[P1, P2], v=2`.  Applies (no-op since
  already applied).  APPLIED_REQ(v=2) — redundant but harmless.

**Outcome:** correct.  Two producers admitted in one applied-version
round.  Small inefficiency: consumer does two pulls when one would
suffice; broker gets a redundant APPLIED_REQ.

**[F4] Optional optimization — coalesce consecutive pulls in the
consumer's BRC handler if allowlist and version haven't changed
since last APPLIED_REQ.  Not correctness-affecting; note for
follow-up.**

---

**Scenario 13.1.c — Race between producer admission and consumer's pull.**

- C1 registered, Live, endpoint published.
- P1 REG_REQ admitted, version → 1, NOTIFY(v=1) fires.
- C1 receives NOTIFY(v=1), issues GET_CHANNEL_AUTH_REQ.
- Between C1's GET_CHANNEL_AUTH_REQ dispatch and broker's reply:
  P2 REG_REQ admitted, version → 2, NOTIFY(v=2) fires.
- Broker's GET_CHANNEL_AUTH_REQ handler snapshots CURRENT state:
  `allowlist=[P1, P2], channel_version=2`.  Reply to C1.
- C1 applies `[P1, P2]`.  Sends APPLIED_REQ(v=2).
- APPLIED_REQ handler: `confirmed_version` 0 → 2.  Both P1
  (my_version=1) and P2 (my_version=2) released in one scan.
- Consumer subsequently receives P2's NOTIFY.  Pulls again —
  sees `[P1, P2], v=2` (unchanged).  Applies (no-op).  APPLIED_REQ
  (v=2) — redundant.

**Outcome:** correct.  Same optimization opportunity as 14.1.b.

---

**Scenario 13.1.d — Producer DEREGs while pending.**

- C1 Live, endpoint published.
- P1 REG_REQ pends on `awaiting_allowlist_confirmed`.
- Before C1 sends APPLIED_REQ, P1's role host crashes /
  disconnects.  BRC times out on P1's REG_REQ (client-side),
  gives up.  Producer role process exits.  Broker's pending
  entry for P1 is still in queue.
- Broker's pending-queue sweep runs on interval, or:
  broker's heartbeat-timeout handler detects P1's CTRL socket
  went silent... but wait: P1 never sent its first heartbeat
  (it's still pending REG_ACK).  So there's no presence row
  to time out.

  **[F5] Pending-side stale-uid cleanup.  A dialing peer whose
  REG_REQ pends may crash before REG_ACK is sent.  There's no
  presence row (heartbeat cadence requires REG_ACK first).  The
  broker's pending-queue sweep must expire pending entries by
  wall-clock budget alone.  On expiry: remove the peer's pubkey
  from the allowlist (bump version + NOTIFY to binding side —
  it's a "phase=left" for a pubkey that never went Live), erase
  the pending entry.  The current draft §7.2 mentions
  "stale-pubkey cleanup on pend timeout"; verify the sweep loop
  actually runs on the timescale of the REG_REQ budget (~5 s
  today per BRC config).**

- After sweep: allowlist reverts to what it was before P1.
  channel_version bumps again.  NOTIFY(left, producer, P1) fires
  to C1.  C1 pulls, gets updated allowlist without P1, applies,
  APPLIED_REQ.  Consistent state.

---

**Scenario 13.1.e — Consumer DEREGs before any producer arrives.**

- C1 registered, endpoint published, Live.  No producers yet.
- C1 sends CONSUMER_DEREG_REQ.  Broker's handler: last consumer
  (fan-in has exactly one consumer) leaving → this is the
  binding side's departure → tear the channel down.
- `_on_channel_closed` fires.  Handler cascade removes ChannelEntry.
- No dialing peers exist; nothing to send CHANNEL_CLOSING_NOTIFY
  to.  Clean.

**Outcome:** correct.

---

**Scenario 13.1.f — Consumer crashes while producers are attached.**

- C1 Live, P1 Live, data flowing.
- C1's role process exits (SIGKILL).  Broker's CTRL socket sees
  C1's identity go silent (or eventually times out on heartbeat).
  Actually the more likely trigger: heartbeat timeout —
  presence Connected → Pending → Disconnected.
- On C1's presence going Disconnected: broker fires
  `_on_channel_closed` (fan-in's binding side departure).  Channel
  torn down.
- Broker fires CHANNEL_CLOSING_NOTIFY to P1's `zmq_identity`.
- P1's role host processes CLOSING notify.  Role should handle:
  stop the data loop for this channel, tear down the tx_queue.
- P1's data-plane PUSH socket loses its peer (C1's PULL socket
  gone).  libzmq's PUSH internally reports EAGAIN or drops silently
  depending on HWM.

**Outcome:** correct.  Verify P1's role host actually handles
CHANNEL_CLOSING_NOTIFY for its own producer role (I know
consumer-side handling exists; producer-side needs verification).

**[F6] Verify producer-side CHANNEL_CLOSING_NOTIFY handling path
exists.  Under pre-migration model, channel closing was
consumer-only because only the consumer was the dialing side that
would care about the producer's death.  Under topology model, the
producer under fan-in is dialing and needs the same handler for
consumer death.  Symmetric implementation may or may not already
be in place.**

---

**Scenario 13.1.g — Producer heartbeat timeout while attached.**

- C1 Live, P1 Live, data flowing.
- P1's heartbeat stops (network partition to broker's CTRL, or role
  hang).
- Broker's presence FSM: P1 Connected → Pending → Disconnected.
- `_on_producer_dropped` fires (fan-in permits N producers; drop
  one, channel survives unless it was the last dialing peer AND
  fan-in has zero producers... actually under fan-in, channel life
  depends on CONSUMER life, not producer count — so channel
  survives regardless).
- Broker fires `CHANNEL_AUTH_CHANGED_NOTIFY(left, producer, P1)`
  to C1.  Removes P1's pubkey from allowlist.  Bumps version.
- C1 pulls, applies, APPLIED_REQ.  C1's ZAP allowlist no longer
  admits P1.  Any subsequent CURVE handshake attempt from P1 (if
  it came back and tried to reconnect on its data-plane socket)
  would be rejected until P1 REG'd again.

**Outcome:** correct.

---

**Scenario 13.1.h — Concurrent P1 admission + C1 crash mid-flight.**

- C1 Live, endpoint published.
- P1 REG_REQ pending on `awaiting_allowlist_confirmed`.
- Broker sent NOTIFY to C1; C1 issued GET_CHANNEL_AUTH_REQ; broker's
  reply is in transit.
- C1 crashes.  BRC socket goes silent.  Broker's heartbeat timeout
  fires → C1 presence Disconnected → channel teardown.
- P1's pending REG_REQ is on a channel that's being torn down.
  Broker's teardown handler must scan pending queues and reply to
  each with a terminal error (`CHANNEL_NOT_FOUND` or a distinct
  `CHANNEL_CLOSING` code).

  **[F7] Pending-queue drain on channel teardown.  Broker's
  channel-close path must drain any pending REG_REQs on that
  channel with an error reply.  Otherwise the pending entry
  outlives the channel and confuses subsequent state.  Verify:
  either the current pending-queue sweep catches this (via
  channel-existence recheck on wake), or an explicit drain must
  fire from `_on_channel_closed`.**

- P1's BRC times out on its REG_REQ → gives up → role exits.

**Outcome:** correct after F7.

---

**Scenario 13.1.i — CHANNEL_AUTH_CHANGED_NOTIFY dropped.**

- P1 admitted, NOTIFY(admitted, P1, v=1) fired to C1.  Network
  drops the message (or ROUTER queue is full and message is
  discarded per libzmq's per-message HWM).
- C1 never pulls.  `confirmed_version[K][C1]` stays 0.  P1's
  REG_REQ stays pending on `awaiting_allowlist_confirmed`.
- Eventually P1's pending entry times out (§13.1.d path).  P1
  gets error reply.  Or:
- C1 experiences a NOTIFY for a different peer (P2 arrives later);
  same NOTIFY handler pulls fresh allowlist — sees `[P1, P2]` this
  time.  Applies both.  APPLIED_REQ(v=2).  Both released.

**Outcome:** self-healing.  The NOTIFY is a doorbell; the pull sees
the current state.  A dropped NOTIFY delays admission until the
next NOTIFY fires (for some other peer) OR until pending timeout.

**[F8] The design is resilient to lost NOTIFYs as long as some
subsequent NOTIFY fires OR the pending sweep runs.  If the channel
is quiescent (no new admissions, no departures) after a lost
NOTIFY, the pending REG_REQ starves until its budget expires.  On
timeout the client's BRC retries the REG_REQ, which re-runs
admission from scratch — resolving the drift.  This is the
"eventual consistency" property called out in HEP-CORE-0036 §6.5
amendment 2026-06-04.**

---

**Scenario 13.1.j — Two consumers race to open the channel.**

- Consumer A and Consumer B send CONSUMER_REG_REQ simultaneously
  with `channel_topology="fan-in"` on the same channel_name.
  Neither has opened the channel yet.
- Broker's writer lock serializes.  A's REG_REQ processed first:
  channel-open path fires (A is legal fan-in opener), ChannelEntry
  created, A added to consumers[].
- B's REG_REQ processed next: channel now exists with topology=FanIn.
  Cardinality gate: `check_cardinality(FanIn, Consumer, producers=0,
  consumers=1)` → `FAN_IN_IS_SINGLE_CONSUMER`.  B rejected.

**Outcome:** correct — cardinality gate holds under race.

---

**Scenario 13.1.k — Producer arrives during consumer's channel-open transaction.**

- C1 CONSUMER_REG_REQ arrives.  Broker takes writer lock, creates
  ChannelEntry, releases writer lock.  Between writer-lock release
  and CONSUMER_REG_ACK send:
- P1 REG_REQ arrives on broker's ROUTER dispatch thread.  Broker
  takes writer lock (same thread — but ROUTER is single-threaded
  in the current design, so serialization by design).  Actually
  no, current design is single-threaded ROUTER dispatch, so this
  race doesn't exist.

**[F9] The single-threaded ROUTER dispatch is a load-bearing
assumption for the pending-queue + writer-lock serialization
model.  Explicit assumption; should be documented as an INVARIANT
in HEP-CORE-0036.  Any future federation-relay thread or
background sweep thread that mutates channel state must respect
this or add its own serialization.**

---

**Scenario 13.1.l — Consumer's ENDPOINT_UPDATE_REQ races with pending producer wake.**

- C1 admitted, sends first heartbeat → Live.
- P1 REG_REQ arrives before C1's ENDPOINT_UPDATE_REQ.  Pending on
  `awaiting_endpoint_resolved`.
- C1's ENDPOINT_UPDATE_REQ arrives.  Handler sets data_endpoint,
  ACKs C1, then wakes pending queue.  P1's entry re-evaluates:
  endpoint resolved ✓, but allowlist confirmed?  Still no —
  no APPLIED_REQ has advanced `confirmed_version` for this
  producer's my_version yet.  P1 stays pending, now on
  `awaiting_allowlist_confirmed`.
- The pending transition (endpoint→allowlist) must not lose the
  pending entry.  Verify: broker's wake logic re-checks all
  conditions in order; if next condition unmet, entry stays
  queued with updated reason.

**Outcome:** correct.  Multi-condition pending is a chain.

---

**Scenario 13.1.m — CURVE handshake fails due to allowlist drift.**

- P1 admitted, NOTIFY fired to C1, but C1's `set_peer_allowlist`
  call raced with something (e.g., C1's role host is under load
  and hasn't processed the NOTIFY yet).
- Broker released P1's REG_REQ prematurely?  No — R6 gate holds
  until APPLIED_REQ.  So this scenario can't happen under R6.
- Under R6-broken paths (e.g., an older client that bypassed R6
  by using a legacy REG shape): CURVE handshake at ZAP fails.
  libzmq retries every ~100 ms.  Eventually C1's allowlist
  updates, handshake succeeds.

**Outcome:** R6 prevents this scenario.  If R6 is bypassed, libzmq's
handshake retry provides eventual convergence.  Trust R6.

---

### 13.2 Fan-out ZMQ

Structurally symmetric with fan-in ZMQ with producer/consumer roles
swapped.  Baseline, cardinality, DEREG, heartbeat timeout,
NOTIFY-drop scenarios all mirror.

**Additional concern — fan-out PUB slow-joiner.**  Under fan-out
ZMQ, PUB drops messages sent before any SUB subscribes.  The
CURVE handshake happens FIRST (blocking on ZAP admission), then
SUB sends subscription frames, THEN producer's PUB messages become
deliverable to that SUB.  If producer's `on_produce` fires between
consumer's REG_ACK arrival and subscription frames arriving at
producer's PUB, messages are dropped.

**[F10] The slow-joiner-gate at `on_produce` (documented in
HEP-CORE-0017 §4.7.6) is a SCRIPT-level responsibility, not a
protocol-level guarantee.  The REG protocol has no obligation to
serialize `producer emits` against `consumer's SUB is subscribed`.
The `consumer_count()` accessor + script-side gate is what closes
this window.  Verify: the accessor's underlying `live_peers` map
increments on `phase=live` NOTIFY, which fires only after
consumer's first heartbeat, which fires only after consumer's
data-plane bind AND subscription per HEP-CORE-0036 §3.5.5 S3.
So `consumer_count() > 0` is a strong guarantee that at least one
SUB is subscribed.  Confirmed.**

**Additional concern — second producer under fan-out.**  Broker
rejects with `FAN_OUT_IS_SINGLE_PRODUCER`.  If two producers race:
- Writer lock serializes.  First becomes producer of the channel.
  Second sees existing channel with topology=FanOut, producers=1
  → cardinality gate → rejected.

**Outcome:** correct.

---

### 13.3 Fan-out SHM

Baseline identical to fan-out ZMQ up to `apply_master_approval`.
There:
- Producer creates DataBlock on prepare-hook.  Binds capability
  socket at `ipc://<path>`.  publishes cap-socket path via
  ENDPOINT_UPDATE_REQ.
- Consumer receives REG_ACK with `initial_allowlist[0].endpoint =
  ipc://<path>`, `pubkey_z85 = producer_pubkey`.  Consumer's rx
  queue in Standby.  Consumer connects capability socket.  Runs
  AttachProtocol (HEP-CORE-0044): crypto_box handshake, receives
  memfd via SCM_RIGHTS, maps DataBlock.  Queue transitions to
  Active.

**Difference from ZMQ fan-out:** the "CURVE handshake" is replaced
by AttachProtocol's crypto_box handshake, and the "ZAP allowlist"
role is played by an equivalent trust check inside AttachProtocol
(HEP-CORE-0044 §D4.5).  The REG-level allowlist mechanism
(`CHANNEL_AUTH_CHANGED_NOTIFY` etc.) still fires the same way —
producer's allowlist grows as consumers register — but the
consuming code inside `apply_master_approval` on the SHM producer
must apply the allowlist to AttachProtocol's admission map, not
to a ZAP router.

**[F11] The producer-side wire-up: does the SHM producer's
`apply_master_approval` currently apply the allowlist to
AttachProtocol's admission map, or does it silently ignore the
allowlist (since AttachProtocol has its own admission logic)?
Verify code path in role_api_base's SHM tx branch.  If it's a
gap, add it — the design says the allowlist controls admission
uniformly across transports.**

Second consumer arrival: broker admits (fan-out permits N),
NOTIFY(admitted, consumer, C2) fires to producer, producer's
allowlist grows, producer's AttachProtocol admits C2 on next
attach handshake.  C2's REG_ACK comes back with the same cap-socket
path.  Two consumers each hold their own fd to the same memfd
(kernel refcounts).

---

### 13.4 One-to-one ZMQ

Baseline is a degenerate case of fan-out ZMQ (single consumer).
Cardinality gate rejects second peer on either side:
- Second producer REG_REQ → `ONE_TO_ONE_CARDINALITY_VIOLATED`
  (matches the `check_cardinality` return under 1-to-1 + Producer
  with `existing_producers >= 1`).
- Second consumer CONSUMER_REG_REQ → same code, with
  `existing_consumers >= 1`.

Socket types differ from fan-out (PUSH/PULL instead of PUB/SUB),
but the REG-level protocol is identical.

**No additional findings for one-to-one ZMQ.**

---

### 13.5 One-to-one SHM

Same as fan-out SHM with cardinality-1 guard on consumer side.
No additional findings.

---

### 13.6 Consolidated findings

| ID | Finding | Type | Action |
|---|---|---|---|
| F1 | Fan-in ZMQ baseline flow works end-to-end correctly | verified | — |
| F2 | R6 pending needs a fourth condition: `awaiting_channel_created` for the fan-in producer arriving before consumer opens channel.  §7.2 currently lists three conditions.  Rewrite. | **design gap** | Update §7.2 + broker's admission logic |
| F3 | Broker needs a wake trigger on channel-open (fires when the binding side's REG_REQ creates the channel).  Not currently wired. | **design gap** | Add wake trigger to `_on_channel_opened` handler chain |
| F4 | Consumer may issue redundant GET/APPLIED_REQ under concurrent admissions.  Correctness OK; small inefficiency. | optimization | Optional follow-up |
| F5 | Pending-side stale-uid cleanup: verify the pending-queue sweep runs on REG_REQ budget timescale (~5 s) and correctly removes the pubkey from allowlist on expiry. | verification | Audit current pending-sweep code |
| F6 | Producer-side CHANNEL_CLOSING_NOTIFY handling: verify the producer's role host handles it (symmetric to pre-migration consumer-only path). | verification | Audit role_api_base producer handler |
| F7 | Pending-queue drain on channel teardown: verify `_on_channel_closed` scans pending queues and replies with a terminal error to each pending entry on the closed channel. | verification | Audit `_on_channel_closed` chain |
| F8 | NOTIFY-loss resilience: design is eventually-consistent via BRC retry on `CHANNEL_NOT_READY`.  Sufficient for practical operation; noted as a known drift window. | verified | Document as invariant |
| F9 | Single-threaded ROUTER dispatch is a load-bearing invariant for pending-queue + writer-lock serialization.  Should be stated explicitly as an INVARIANT. | invariant | Add to HEP-CORE-0036 |
| F10 | Fan-out ZMQ slow-joiner gate at `on_produce` is a script-level responsibility, correctly guaranteed by `consumer_count() > 0` (which requires `phase=live` NOTIFY which requires post-subscription first heartbeat).  Confirmed. | verified | — |
| F11 | SHM producer's `apply_master_approval` allowlist application: verify it applies to AttachProtocol's admission map, not silently ignored. | verification | Audit SHM tx branch of `apply_master_approval` |
| F12 | Dialing side arriving at missing channel PENDS on `awaiting_channel_created` regardless of topology (generalizes F2 to fan-out / one-to-one consumers). `CHANNEL_NOT_FOUND` reject retires as an admission outcome. | **design refinement** | Update §7.1 (drop the reject on missing channel for dialing side) + §7.2 (rename first condition) |
| F13 | Client BRC ACK timeout must be ≥ broker pending budget. Retry from same uid+pubkey at NEW zmq_identity = "reconnect with fresh identity" (replace ROUTER target, keep pubkey admission). Same identity = drop as duplicate. | **design refinement** | Document coupling in HEP-CORE-0036 §6.5; add retry-handling rule to broker pending-queue logic |
| F14 | Pending entry for the missing-channel case carries deferred admission state; broker runs admission at wake time, not at initial pend. | design refinement | Update §7.2 pending-entry contents |
| F15 | Optimistic pubkey admission drift bounded by pending budget. Known acceptable condition; documented not fixed. | documented invariant | Add to HEP-CORE-0036 §I8-adjacent note |
| F16 | Broker crash recovery under long pending: HubState persistence model decides behavior. Under stateless restart, all clients re-register — correct behavior. | verified | Confirm HubState persistence model matches expectation |
| F17 | No queue-depth cap in current design. Bounded by `known_roles` + F13 dedup. Recommend soft cap with `PENDING_QUEUE_FULL` reject as defense-in-depth. | optional hardening | Track separately |

### 13.7 Long-window pending timeout — multi-machine coordinated startup

Baseline analysis assumed the pending timeout was on the order of
the current BRC REG_REQ budget (~5 s).  Extending it to 60 s to
accommodate coordinated multi-machine startup over a congested
network exposes classes of issue that were latent-but-invisible at
5 s.  Walking those explicitly.

**13.7.a — Symmetric "channel doesn't exist yet" pending for
fan-out and one-to-one consumers.**

At 5 s, a consumer arriving before its producer under fan-out or
one-to-one gets `CHANNEL_NOT_FOUND` (terminal per HEP-CORE-0036
§6.6), BRC gives up, role exits.  Ops mitigation: "start producer
first."  Tolerable for hand-orchestrated single-host tests.

At 60 s, that ordering assumption breaks under multi-machine
coordinated startup where roles come up in whatever order Kubernetes
/ systemd / SSH launches them.  Consumer that arrives 2 s before
producer shouldn't have to fail-and-restart.

**[F12] — F2 is a special case of a general rule.**  Under the
long-window model the design rule is: **the dialing side arriving
at a missing channel PENDS on `awaiting_channel_created`,
regardless of topology.**  Under fan-in that's the producer; under
fan-out and one-to-one that's the consumer.  Both should pend.
`CHANNEL_NOT_FOUND` retires as an admission-time reject entirely —
under the topology model it's always either "wait a bit longer"
(pending) or "impossible under this topology" (cardinality reject
or transport-topology gate).  Update §7.1 and §7.2 accordingly.

The mirror consequence: fan-out and one-to-one consumers no
longer need BRC's retry-on-CHANNEL_NOT_READY loop for this case
(one client-side loop retires with one wire-level pending
mechanism).  Simpler client, cleaner semantics.

---

**13.7.b — Client-side BRC ACK timeout vs. broker pending
budget.**

If BRC's REG_REQ ACK timeout is 5 s and broker's pending budget is
60 s: client gives up at t=5, sends a fresh REG_REQ at t=5+backoff.
Broker still holds the first pending entry (t=1 → t=61 budget).
Broker gets the retry REG_REQ — sees same `role_uid` already in the
pending queue.

Three possible behaviors:

- **Ignore the retry** (drop as duplicate).  Client waits and
  re-retries; eventually broker's original pending clears via wake
  and sends REG_ACK.  But client's zmq_identity has changed if
  its BRC socket cycled (which happens on some transports on
  timeout) — REG_ACK dead-letters.
- **Replace the pending entry** with the retry's fresh zmq_identity.
  First pending entry's optimistic pubkey add stays (same pubkey);
  only the ROUTER routing target updates.
- **Reject the retry** with `UID_CONFLICT`.  Confuses the client
  (it thinks its own retry conflicted with itself).

**[F13] — The design should be that client BRC timeout ≥ broker
pending budget.**  Otherwise client retries stack on top of broker
pending entries and generate the ambiguity above.  Under the long
timeout, BRC must be configured with a proportionally longer ACK
timeout; the two are coupled.  Doc this coupling in HEP-CORE-0036
§6.5.  A retry from the SAME uid+pubkey pair at a NEW zmq_identity
should be treated as "reconnect with fresh identity" — replace the
pending entry's ROUTER target, keep the pubkey admission, don't
bump `channel_version`.  A retry from the SAME uid+pubkey pair at
the SAME zmq_identity is a network-layer duplicate; drop silently.

---

**13.7.c — Wake-time admission for missing-channel pending.**

Under §7.2 baseline, optimistic pubkey admission happens at
initial pend time (dialing peer's REG_REQ passes topology +
cardinality gates, pubkey is added to allowlist, R6 fails on
another condition, entry pends).

But when the channel doesn't exist yet, there is no allowlist to
add the pubkey to.  The pending entry has to defer the pubkey
admission until wake — when the channel gets created.

**[F14] — The pending-queue entry carries deferred state.**  For
the missing-channel case:
- Pending entry stores: `{channel_name, role_uid, zmq_identity,
  role_type, pubkey_z85, schema_invariants, transport_invariants,
  declared_topology, pending_reason, expiry, correlation_id}`.
- On wake for "channel created": broker re-runs admission
  (topology + cardinality + schema check against the freshly-
  created channel).  If admission fails, reply with the
  appropriate reject.  If admission succeeds, add the peer to the
  roster + add pubkey to allowlist + bump `channel_version` +
  assign `my_version` + re-evaluate the next R6 condition.  If
  next condition unmet, re-pend with the new reason.  Otherwise
  release with REG_ACK.

This changes the pending entry from "just wait for one condition"
to "wait for a chain of conditions."  Not new logic — draft §13.1.l
already had it (endpoint-then-allowlist chain).  But the missing-
channel case pushes the chain start further back.

---

**13.7.d — Optimistic-pubkey drift window widens.**

Under §7.2 F5 baseline: pubkey added at pending time, removed on
sweep timeout.  At 5 s, drift window is 5 s max.  At 60 s, drift
window is 60 s max.

Security implication: a legitimate role sends REG_REQ, crashes at
t=1, pubkey stays in allowlist until t=60.  Any peer that has the
crashed role's CURVE keypair could authenticate as it during that
60 s.  Attack surface: whoever had access to the crashed role's
secret key.

This isn't a NEW attack surface — the CURVE keypair is always the
authenticator.  Whoever has it can impersonate the role.  The
window just extends how long a stale admission entry sits around.
Under the pre-migration model there was no channel-scope allowlist
at all for many paths; pubkey admission was per-connection at ZAP.
Under the topology model the channel-scope allowlist gives the
attacker a persistent admission slot.

**[F15] — The pubkey admission drift is a KNOWN acceptable
condition, bounded by the pending budget.**  No design change; but
should be documented in HEP-CORE-0036 as: "channel-scope allowlist
admission persists for up to `pending_budget_ms` after a REG_REQ
whose ACK was never delivered.  This is a small drift window
whose duration operators tune via `pending_budget_ms` alongside
BRC ACK timeout (per F13).  Longer window increases coordinated-
startup tolerance at the cost of a larger stale-admission window
for a compromised role's keypair."

---

**13.7.e — Broker crash during long pending.**

Broker crashes at t=30 s with 100 pending entries.  Pending queue
is transient (in-memory); lost.  On restart:
- HubState rebuilt from persistent state (or empty if the process
  is stateless-restart).
- Any pubkey adds that had been persisted to `ChannelEntry` via
  `_on_peer_authorized` remain in the allowlist.
- Clients' BRC socket may or may not still be alive; each will
  either time out on its own REG_REQ ACK (per F13 configured
  proportionally to `pending_budget_ms`) and retry, or the client
  has already given up and the role exited.

**[F16] — Broker recovery from long-pending crash: the ONLY
persistent state that survives is the optimistic pubkey adds
(if `_on_peer_authorized` writes to persistent HubState).  All
other pending state is lost.  On recovery:**

- Clients retry REG_REQ.  Fresh pending entries.  Each retry's
  optimistic add is IDEMPOTENT (adding the same pubkey to a set is
  a no-op — the version bump only fires on NEW admissions per
  `_on_peer_authorized`'s "if inserted, bump" logic in `hub_state.cpp`).
- Consumers' `confirmed_version` state: if this is per-instance
  and instance_id bumps on the recovered broker, all confirmed
  versions reset to 0 — which forces every dialing peer to
  re-flow the NOTIFY → GET → APPLIED chain.  That's actually the
  correct behavior after a broker restart.
- If the confirmed version was persisted BEFORE the crash, and
  the same instance_id is used on restart, then the previously-
  admitted peers' REG_REQs would immediately succeed on retry.

Verify HubState's persistence model matches this expectation:
under stateless-restart (no persistent state, instance_id bumps),
the recovery path is "everyone re-registers."  Under persistent
restart, the recovery is "resume."  Draft doesn't need to change;
HubState's persistence model is what it is.

---

**13.7.f — Pending-queue depth under bursty startup.**

100 machines coming up in the same 60 s window, each with 3
channels, average 5 roles per machine per channel = ~1500 pending
entries theoretical peak.  Each entry ~few hundred bytes; peak
memory ~1 MB.  Not a scaling concern.

Per-wake-event scan cost: linear in pending queue depth for the
CHANNEL_NAME being woken.  Well-clustered — a wake for channel K
only scans entries for K.  Total wake work per event bounded by
"peers on this channel."  Fine.

**[F17] — No queue-depth cap in the design.**  A hostile client
could spam REG_REQs with fresh uids to fill the pending queue
during the 60 s window.  Bounded by (a) known_roles allowlist (only
uids on that list get past initial validation), (b) per-uid
deduplication (F13).  Attack surface is low if `known_roles` is
enforced.  Recommend: add a soft cap (per-channel + global) with a
`PENDING_QUEUE_FULL` reject as defense-in-depth.

---

**13.7.g — Pending entry lifecycle summary.**

Under the long-window model each pending entry has three
possible exit paths:

1. **Wake → release** — all R6 conditions clear; REG_ACK sent;
   entry removed from queue.
2. **Wake → re-pend** — one R6 condition cleared, next unmet;
   entry stays queued with updated reason.
3. **Sweep → expire** — budget elapsed without clearing; sweep
   removes entry; on expiry the optimistic pubkey admission is
   ROLLED BACK (allowlist entry removed, `channel_version` bumped,
   NOTIFY(left) fires to binding side); reply-with-error sent to
   the dialing client (if its zmq_identity is still alive).

The rollback on expiry is what keeps the drift window bounded to
`pending_budget_ms`.  Verify this rollback is atomic against
wake — i.e., if a wake event fires just as expiry runs, one wins
and the other is a no-op.  Under single-threaded ROUTER dispatch
(F9) this is naturally serialized.

---

### 13.8 Overall verdict

The clean protocol shape in §2 is architecturally sound.  The
symmetric R6 gate (§8) is the load-bearing mechanism and it works
correctly under all walked scenarios EXCEPT for the two design gaps
in F2 + F3 — the "channel doesn't exist yet" R6 condition and the
"channel-open triggers a wake" mechanism.  Both are small
additions, not redesigns.

Six items (F5, F6, F7, F9, F11 + F1 as a re-verification) require
auditing existing code paths, not new design.  If audits reveal
gaps, they become code fixes at implementation time, not protocol
changes.

Two items (F4, F10) are non-issues — F4 is an optional
optimization, F10 is already correctly handled.

F8 is documentation of an existing invariant.

**Recommended next step:** address F2 and F3 in the draft
directly — update §7.2 with the four-condition R6 gate and note
the channel-open wake trigger.  Then re-walk each scenario against
the updated draft to confirm no cascading effects.

## 14. Typed wire envelope contract

Every control-plane message uses one shape.  Handlers never carve
fields out of a JSON body; every wire field is exposed through a
typed accessor.  §14 is normative — the code implements exactly
this contract; §2 subsections describe admission semantics that
reference the typed body classes defined here.

### 14.1 Frame layout

Every message is 5 ZMQ frames:

```
Frame 0 : identity         (bytes; role_uid or hub_uid per
                            I-DEALER-IDENTITY; ROUTER-managed on
                            receive, DEALER-set via ZMQ_ROUTING_ID
                            on send)
Frame 1 : control marker   ('C', single byte; kFrameTypeControl)
Frame 2 : msg_type         (ASCII, ≤64 bytes)
Frame 3 : correlation_id   (ASCII, ≤64 bytes; empty for
                            fire-and-forget NOTIFY only)
Frame 4 : msg_type body    (JSON; schema per-msg-type; parsed via
                            the typed body class named by msg_type)
```

Frames 0-3 are the **skeleton** — identical shape across every
REQ / ACK / NOTIFY, decidable without JSON parse.  Frame 4 is the
**body** — msg_type-specific.  The whole 5 frames together are the
**envelope**.

BRC's ACK-matching keys on `(Frame 2 msg_type, Frame 3
correlation_id)` without parsing Frame 4.  Handler dispatch keys
on Frame 2 alone.  Every routing / reject / correlate decision is
made on skeleton frames alone.

### 14.2 WireEnvelope class

Single owner of frame layout.  BRC + broker both go through it;
no other code path constructs or consumes wire frames.

```cpp
class WireEnvelope {
public:
    // Build the 5-frame envelope.  Sender-side omits Frame 0
    // (ROUTER prepends identity on receive).  Stamps envelope_hash
    // on the body per I-ENVELOPE-BODY-BINDING before returning.
    template <typename BodyT>
    static zmq::multipart_t build(const std::string& identity,
                                    const std::string& msg_type,
                                    const std::string& correlation_id,
                                    BodyT              body);

    // Parse a 5-frame inbound envelope.  Validates envelope↔body
    // hash (I-ENVELOPE-BODY-BINDING); rejects with std::nullopt +
    // ENVELOPE_TAMPERED WARN on mismatch.  Rejects empty
    // correlation_id on a REQ (I-CORRELATION-STABLE) with
    // std::nullopt + INVALID_REQUEST.
    static std::optional<WireEnvelope> parse(zmq::multipart_t&& msg);

    std::string_view identity()       const;   // Frame 0
    std::string_view msg_type()       const;   // Frame 2
    std::string_view correlation_id() const;   // Frame 3

    // Typed body cast.  Chooses the body class by msg_type;
    // throws on schema-shape mismatch or missing required fields.
    template <typename BodyT>
    BodyT body_as() const;
};
```

### 14.3 Typed body classes

Each msg_type owns a C++ class in
`utils/network_comm/wire_bodies.hpp`.  Each class exposes ONLY the
fields its msg_type carries via named accessors; validates required
fields at construction.  No `body.value("field", ...)` scatter
anywhere; the class is the schema.

Every REG-family body (any msg_type that mutates admission state)
carries the security triple `{client_nonce, client_wall_ts,
envelope_hash}` per I-REPLAY-BOUND + I-ENVELOPE-BODY-BINDING.
Every body (REQ / ACK / NOTIFY) carries `envelope_hash`.  For
brevity the class catalog below notes "+ security triple" or
"+ envelope_hash only" instead of repeating.

- **RegReqBody** (msg_type = `REG_REQ`, `CONSUMER_REG_REQ`):
  `channel_name`, `role_uid`, `role_type`, `role_name`,
  `channel_topology`, `data_transport`, `zmq_pubkey`,
  `broker_proto`, `schema_hash`, `schema_version`, `schema_id`,
  `schema_blds`, `schema_owner`, `abi_fingerprint` + security triple.

  > ⚠ **ERRATUM PENDING — supersede this entry with a split into
  > two body classes.**
  >
  > HEP-CORE-0034 §10.2 is the authority on consumer-side schema
  > wire fields and explicitly requires the `expected_` prefix
  > (`expected_schema_id`, `expected_schema_hash`,
  > `expected_schema_blds`, `expected_schema_packing`, plus
  > optional `expected_flexzone_blds` / `expected_flexzone_packing`).
  > §10.2 last paragraph: *"The form `expected_blds` /
  > `expected_packing` (no `schema_` infix) was used in pre-Phase-4d
  > code and is no longer accepted."*  The unified `RegReqBody`
  > catalog above with a `schema_hash` accessor on both REQ shapes
  > conflicts with that.  Producer's `schema_hash` is a declaration;
  > consumer's `expected_schema_hash` is a citation — the wire-level
  > prefix carries the semantic distinction.
  >
  > Pending amended catalog entries (to replace the RegReqBody line
  > above when this HEP is next edited):
  > - **ProducerRegReqBody** (`REG_REQ`): `channel_name`, `role_uid`,
  >   `role_type`, `channel_topology`, `data_transport`,
  >   `zmq_pubkey`, plus HEP-0034 §10.1 producer schema fields
  >   (`schema_id`, `schema_hash`, `schema_blds`, `schema_packing`,
  >   `schema_owner`, optional `flexzone_blds`, `flexzone_packing`),
  >   plus `producer_pid`, `zmq_node_endpoint` (required when
  >   `data_transport == "zmq"` AND producer is binding side per
  >   HEP-CORE-0017 §3.3.0 topology matrix),
  >   `shm_capability_endpoint` (required when
  >   `data_transport == "shm"`), optional inbox companion fields
  >   per HEP-CORE-0027 §4.1, `abi_fingerprint`, optional `build_id`
  >   + security triple.  Note: HEP-CORE-0036 §5b.4's single
  >   `data_transport` string is the transport discriminator;
  >   HEP-CORE-0007 §12.3's older `has_shared_memory` +
  >   `shm_name` shape is superseded.
  > - **ConsumerRegReqBody** (`CONSUMER_REG_REQ`): `channel_name`,
  >   `role_uid`, `role_type`, `channel_topology`,
  >   `data_transport`, `zmq_pubkey` (consumer's own identity
  >   pubkey), plus HEP-0034 §10.2 consumer schema fields
  >   (`expected_schema_id`, `expected_schema_hash`,
  >   `expected_schema_blds`, `expected_schema_packing`, optional
  >   `expected_flexzone_blds`, `expected_flexzone_packing`), plus
  >   `consumer_pid`, `consumer_hostname`, optional inbox companion
  >   fields, `abi_fingerprint`, optional `build_id` + security
  >   triple.
  >
  > **role_name is OPTIONAL** on both ProducerRegReqBody and
  > ConsumerRegReqBody.  It is a redundant human-friendly label —
  > `role_uid` per HEP-CORE-0033 §G2.2.0b already embeds a name
  > component (`<tag>.<name>.<unique>`), so `role_name` adds no
  > correctness or discrimination.  §7.1's required-field table
  > (`channel_name, role_uid, role_type, zmq_pubkey, data_transport`)
  > is the authoritative required list; wire body classes MUST match
  > it.  Validation follows the "when non-empty" pattern from
  > HEP-CORE-0023 §2.5.4: absent is OK; when present, MUST be a
  > string (identifier-grammar check runs at gate_grammar
  > downstream, not at wire body class construction).  Implementers
  > use `d::validate_if_present(body_, "role_name", JsonKind::String)`
  > in the body ctor.
  >
  > `schema_version` on the current catalog is not a separate wire
  > field.  Version rides inside `schema_id` per HEP-CORE-0033
  > §G2.2.0b naming grammar (`$base.v<N>` form).  Contract identity
  > is `(owner_uid, schema_id)` where `schema_id` includes the
  > version; the fingerprint (`schema_hash`) is a separate CONTENT
  > check.  Two contracts with the same fingerprint but different
  > versions produce different `schema_id` strings and are rejected
  > as different contracts by string-equality on `schema_id` alone.
  > All schema-id construction / parsing / validation MUST go
  > through the unified API in `naming.hpp`:
  > `pylabhub::hub::make_schema_id(base, version)` (canonical
  > constructor; to be added),
  > `pylabhub::hub::parse_schema_id(id_view)` (canonical parser),
  > `pylabhub::hub::is_valid_identifier(id, IdentifierKind::Schema)`
  > (grammar check).  Removed `schema_version` from both amended
  > entries.  See C2 in the pending errata batch.
  >
  > `broker_proto` scalar on the current catalog is retired for
  > REG_REQ / CONSUMER_REG_REQ.  The canonical wire-version + ABI
  > carrier per HEP-CORE-0032 §8 is the `abi_fingerprint` object
  > — 7 axes (`library`, `shm`, `broker_proto`, `zmq_frame`,
  > `script_api`, `script_engine`, `config`), each major/minor,
  > verified via `pylabhub::version::verify_peer_versions()` with
  > the §8.5 policy taxonomy (`Ok` / `BuildOnly` / `MinorMismatch`
  > / `MajorMismatchAccepted` / `MajorMismatchRejected` / `Absent`
  > / `InvalidEnvelope`).  Strict mode opt-in via
  > `broker.strict_abi_mismatch`.  Removed the scalar
  > `broker_proto` field from both amended entries.  Scalar
  > `broker_proto` IS retained on HEP-CORE-0036 §6.5 auth-family
  > messages (`CHANNEL_AUTH_CHANGED_NOTIFY`,
  > `GET_CHANNEL_AUTH_REQ`, `GET_CHANNEL_AUTH_ACK`) — that is a
  > per-auth-message protocol version, distinct from the
  > initial-REG ABI verification.  See C3 in the pending errata
  > batch.
- **RegAckBody** (`REG_ACK`, `CONSUMER_REG_ACK`): `status`,
  `error_code`, `message`, `channel_name`, `instance_id`,
  `snapshot_version`, `heartbeat`, `initial_allowlist`,
  `broker_abi_fingerprint`, `broker_build_id`,
  `broker_observer_pubkey_z85` + envelope_hash only.
- **ChannelAuthChangedNotifyBody**: `channel_name`, `role_uid`
  (subject), `role_type` (subject), `phase`, `channel_version`
  + envelope_hash only.
- **ChannelClosingNotifyBody**: `channel_name`, `reason`
  + envelope_hash only.
- **ConsumerDiedNotifyBody**: `channel_name`, `role_uid`, `reason`,
  `target_role` + envelope_hash only.
- **EndpointUpdateReqBody**: `channel_name`, `endpoint_type`,
  `endpoint` + security triple.
- **EndpointUpdateAckBody**: `status`, `message`
  + envelope_hash only.
- **GetChannelAuthReqBody**: `channel_name`, `role_uid`
  + envelope_hash only (pure query, no state mutation).
- **GetChannelAuthAckBody**: `status`, `allowlist`, `channel_version`
  + envelope_hash only.
- **ChannelAuthAppliedReqBody**: `channel_name`, `role_uid`,
  `applied_version`, `instance_id` + security triple.
- **ChannelAuthAppliedAckBody**: `status`, `confirmed_version`
  + envelope_hash only.
- **HeartbeatReqBody**: `channel_name`, `role_uid` + envelope_hash
  only (presence maintenance; no state mutation).
- **HeartbeatAckBody**: `status` + envelope_hash only.
- **DeregReqBody**: `channel_name`, `role_uid` + security triple.
  **DeregAckBody**: `status` + envelope_hash only.
- **DiscReqBody**: `channel_name` + envelope_hash only.
  **DiscAckBody**: `status` + discovery payload + envelope_hash only.
- **BandJoinNotifyBody / BandLeaveNotifyBody**: `band`, `role_uid`,
  `role_name` + envelope_hash only.

### 14.4 Handler dispatch pattern

```cpp
// Broker ROUTER poll loop:
auto env = WireEnvelope::parse(std::move(frames));
if (!env) { LOGGER_WARN(...); continue; }

// Dispatch table maps msg_type string → typed handler:
switch_on(env.msg_type()) {
    case "REG_REQ":
        handle_reg_req(*env, env->body_as<RegReqBody>()); break;
    case "CONSUMER_REG_REQ":
        handle_consumer_reg_req(*env, env->body_as<RegReqBody>()); break;
    case "ENDPOINT_UPDATE_REQ":
        handle_endpoint_update(*env, env->body_as<EndpointUpdateReqBody>()); break;
    case "HEARTBEAT_REQ":
        handle_heartbeat(*env, env->body_as<HeartbeatReqBody>()); break;
    // ... one case per msg_type; no default fallthrough (unknown
    // msg_types are wire violations, dropped with WARN).
}
```

Every handler is:

```cpp
void handle_reg_req(const WireEnvelope& env, const RegReqBody& body,
                     /* broker context */)
{
    // Skeleton fields via env:
    //   env.identity()        → Frame 0, ROUTER-managed role_uid
    //   env.correlation_id()  → Frame 3, for reply matching
    // Body fields via typed accessors, no JSON key scatter:
    //   body.role_uid()       → self-declared; MUST == env.identity()
    //                           (I-DEALER-IDENTITY check runs once
    //                           right after parse, per §14.5)
    //   body.channel_name()   → routing key
    //   body.role_type()      → admission-side determination
    //   body.zmq_pubkey()     → CURVE identity check
    //   body.schema_*()       → schema invariants
    //   body.client_nonce() / body.client_wall_ts() → I-REPLAY-BOUND
    //   body.envelope_hash()  → I-ENVELOPE-BODY-BINDING
    //   body.broker_proto()   → I-WIRE-VERSION-ATOMIC
}
```

### 14.5 Admission-gate ordering

Every REG-family handler runs these gates BEFORE any state
mutation, in this order:

1. `WireEnvelope::parse` — envelope↔body hash validated
   (I-ENVELOPE-BODY-BINDING); empty correlation_id rejected
   (I-CORRELATION-STABLE); unknown msg_type dropped.
2. `body.broker_proto() == kBrokerProtoVersion` else
   `UNSUPPORTED_PROTO` (I-WIRE-VERSION-ATOMIC).
3. `env.identity() == body.role_uid()` else `IDENTITY_MISMATCH`
   (I-DEALER-IDENTITY).
4. Grammar validation on role_uid / role_name / channel_name
   (HEP-CORE-0033).
5. `verify_known_role_binding(body.role_uid(), body.zmq_pubkey())`
   else known-roles-mismatch (I-PUBKEY-BINDING).
6. Key-rotation gate: pubkey ≠ role's currently-registered pubkey
   → `UID_CONFLICT` or `KEY_ROTATION_REQUIRES_DEREG`
   (I-KEY-ROTATION-VIA-DEREG).
7. Anti-replay: `HubState::nonce_seen(body.role_uid(),
   body.client_nonce(), body.client_wall_ts())` OR wall-clock skew
   > 30 s → `REPLAY_OR_SKEW` (I-REPLAY-BOUND).
8. Topology / cardinality / schema / transport gates per §2.1
   admission sequence.

Gates 1-7 are wire-level integrity + identity; gate 8 is protocol
admission.  Failure at any gate stops processing and replies with
the named error code before touching HubState.

### 14.6 Backward-incompatible cut

Pre-migration wire is 3 frames: `['C', msg_type, json_body]`.
The typed envelope is not additive.  Every deployed component
ships the envelope on the same version cut per I-WIRE-VERSION-
ATOMIC.  Mixed old/new deployments break I-DEALER-IDENTITY,
I-CORRELATION-STABLE, I-REPLAY-BOUND, and I-ENVELOPE-BODY-BINDING
for the duration of the mix.  No runtime tolerance.  Old clients
after the cut receive `UNSUPPORTED_PROTO` on their first REQ.
