# HEP-CORE-0027: Inbox Messaging

**Status**: Implemented (documenting existing system); CURVE wiring deferred to HEP-CORE-0036 Phase 4+ (see §3.5 below).  Reachability + multi-hub advertisement section (§4.5) added 2026-05-06 to align with HEP-CORE-0033 §19 (multi-presence roles, planned — Wave A item A7) and HEP-CORE-0019 §2.3 (Phase 6 per-presence heartbeats).
**Created**: 2026-03-27
**Scope**: InboxQueue, InboxClient, peer-to-peer messaging side channel
**Depends on**: HEP-CORE-0007 §12.4 (ROLE_INFO_REQ/ACK — the inbox schema is discovered here, as JSON), HEP-CORE-0033 §8 (HubState entry types — `ChannelEntry` / `ConsumerEntry` hold per-presence inbox metadata), HEP-CORE-0033 §18 (broker routing classes — ROLE_INFO_REQ is Class B), HEP-CORE-0033 §19 (multi-presence roles — drives per-presence inbox advertisement), HEP-CORE-0036 §9.3 (CURVE wiring on inbox sockets — role identity keypair + per-channel allowlist inheritance)

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

The Inbox provides this as an optional side channel, available to all
**active** roles.

> **"Independent" means traffic, not lifecycle.**  Inbox messages never
> touch the data path and cannot disturb data flow — that is the
> independence above.  It does NOT mean the inbox is usable before the
> data plane exists.  The inbox is a **feedback pathway between ACTIVE
> roles**, not a general mailbox: a role earns its inbox by becoming
> active, so the roster that authorizes its inbox arrives with, and is
> ingested at, data-plane establishment (`apply_master_approval` for a
> producer, the consumer REG_ACK path for a consumer).  A role that has
> not established a data plane is not active, and its inbox correctly
> admits no one.
>
> This is deliberate and load-bearing: the gating is what keeps the
> inbox a peer-to-peer feedback channel among running roles instead of
> an always-on message box that would have its own independent
> authorization lifecycle to reason about.  Do not "fix" the coupling by
> merging the roster earlier — the deny-all window before activation is
> the intended state, not a gap.  §3.5's S1→S3 sequence is the normative
> statement of it: the ROUTER binds deny-all at S1 and is lifted off that
> default only at S3, when the roster arrives.
>
> Note this is orthogonal to the authorization SCOPE (§3.5): *when* the
> inbox becomes usable is gated on activation, while *who* may then reach
> it is hub-wide (any known role, not just channel peers).  The two are
> independent axes and both are intended.

> **Inbox is TYPED; for JSON messages use a Band.** The two role↔role messaging
> facilities carry different payload forms and must not be confused:
>
> | Facility | Payload | Shape | Validation | HEP |
> |---|---|---|---|---|
> | **Inbox** | **msgpack, schema-typed** (the same typed slot model as the data queue) | point-to-point (role→role, ACKed) | schema mandatory + always-on: `schema_tag` + field count/type + checksum, per frame | **HEP-CORE-0027** (this) |
> | **Band** | **JSON string** | pub/sub group (`!band` broadcast) | none (free-form `dict`/JSON body) | **HEP-CORE-0030** |
>
> An `InboxQueue`/`InboxClient` **cannot** be constructed without a non-empty
> schema (§3, "Schema validation is mandatory and always-on"), and every inbox
> frame is validated against that schema — there
> is no schemaless/JSON inbox mode. A role that needs to exchange free-form JSON
> with peers joins a **Band** (HEP-CORE-0030, *"All band message bodies are JSON"*).

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
| Bands (HEP-CORE-0030) | The **other** role↔role messaging facility, and the complement of the inbox: bands carry **JSON** bodies in a pub/sub group, whereas the inbox carries **msgpack schema-typed** payloads point-to-point (§1). A role uses the inbox for typed targeted messages and a band for free-form JSON broadcast. |
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

**Checksum verification** is an owner-chosen integrity **policy** — the same
`ChecksumPolicy` (None / Manual / Enforced) the SHM and ZmqQueue paths use —
applied to the **decrypted message content**, on top of the mandatory CURVE
transport (§3.5). The inbox OWNER dictates the policy and the sender adopts it,
advertised as `inbox_checksum` in ROLE_INFO_ACK (§4.2 step 8). The BLAKE2b-256
is computed over the message content (before CURVE encryption on send; after
CURVE decryption on receive — the same bytes either way):

- **Enforced** — sender auto-stamps the checksum; receiver auto-verifies.
  Mismatched checksums are dropped and logged (increments `checksum_error_count`).
- **Manual** — sender is responsible for stamping the checksum itself; the
  receiver always verifies, so a missing stamp is rejected.
- **None** — no checksum is computed or verified (sender sends zeros).

**Why it is a policy, not a mandate.** CURVE is the mandatory transport-integrity
baseline: every inbox message is encrypted and authenticated on the wire (§3.5),
and production **hard-refuses** a no-CURVE (unencrypted) inbox (`role_api_base.cpp`
— *"refusing plaintext inbox"*). So the app-level checksum is **defence-in-depth
on the decrypted content**: it mainly catches accidental corruption — a packing
bug, a bad memory write — *after* decryption, whereas deliberate tampering is
already rejected by CURVE's own per-frame authentication *before* the checksum
runs. Turning it off (`inbox_checksum: "none"`) trades that extra check for CPU
and never exposes payloads to an attacker. (Earlier drafts of this section
wrongly stated the inbox checksum was "mandatory, no toggle" — that contradicted
both the code and §4.2 step 8; corrected 2026-07-21.)

**ACK**: the ROUTER's reply after processing, carried as **the same 5-tuple
frame as the message it answers** — it rides the one typed-data codec
(HEP-CORE-0047 §3.0), it is not a bespoke shape:

```
[magic, ack_schema_tag, seq, [ack_code:uint8], checksum]
```

- `seq` **echoes the seq of the message being acknowledged**.  This is the
  correlation, and it costs nothing extra because `seq` is already an element
  of the envelope every frame carries.
- `ack_schema_tag` is derived from the ack field list exactly as every other
  tag is, so an ACK can never be mistaken for a data frame or vice versa.
- `checksum` is zeros: the ACK has no payload integrity of its own, and CURVE
  already authenticates the frame.

Codes are the **application's** vocabulary — the handler picks one and passes
it to `send_ack`; the transport never chooses:
- `0` = OK — **the only code anything in-tree currently emits**
- `1` = queue overflow — application-level backlog.  Transport back-pressure
  can *never* surface here; it appears at the sender as a refused send (§3.7).
- `2` = schema error
- `3` = handler error

> **Why the ACK is framed rather than a bare byte.**  A caller whose ACK wait
> expires leaves a receipt in flight.  With no way to tell whose receipt it is,
> the next send reads it and returns it as *its own* result — and because `0`
> is the only code production emits, the caller is handed a **stale SUCCESS for
> a message the receiver never processed**.  The sender therefore discards any
> receipt whose `seq` is not the one it just sent and keeps waiting, bounded by
> the caller's deadline, counting the discards in `ack_stale_count` (§8).

The DEALER/ROUTER envelope uses ZMQ's built-in identity routing, plus a
replay-metadata frame (§3.6):
- DEALER sets `ZMQ_IDENTITY` to the sender's pylabhub UID before connecting
- DEALER sends `[empty_delimiter, replay_meta, payload]` — the three parts are
  submitted individually, and a refusal on the first is handled differently
  from a refusal on a later one (§3.7 I-SEND-WHOLE)
- ROUTER receives `[identity, empty_delimiter, replay_meta, payload]`
- ROUTER sends ACK as `[identity, empty_delimiter, ack_frame]`

> **The routing identity is an address, not the sender's identity.**
> A DEALER chooses its own `ZMQ_IDENTITY`, so the frame-0 value is
> usable for addressing the ACK back and for nothing else.  The
> sender's real identity is the **principal** resolved from the key
> its CURVE handshake proved (HEP-CORE-0035 §4.2), and that is what
> the inbox MUST use for all three security-relevant purposes: the
> sender reported to the receiving application, the replay-guard key
> (§3.6), and the per-sender sequence state (§3).  Keying any of
> those on the routing id lets one sender be attributed as another,
> and lets a sender obtain a fresh replay window by changing the id
> it presents.  See HEP-CORE-0035 §2, "The routing id is a reply
> address, not a trust claim."

### 3.8 CURVE is unconstructable-without (I-INBOX-CURVE-MANDATORY)

**There is no unencrypted inbox, and no code path that produces one.**  Both
`InboxQueue::start()` and `InboxClient::start()` **PANIC** when no CURVE
identity has been armed.  Not "log and continue", not "return false" — abort.

This is stated as an invariant rather than a recommendation because of how the
hole behaved before it was closed.  The arm was guarded on
`if (!identity_key_name.empty())`, so a caller that simply forgot to arm got a
**working plaintext socket**: an inbox ROUTER accepting unauthenticated peers,
with no error, no warning, and a comment beside it describing the unarmed
branch as a supported legacy path.  Every public factory already rejected an
empty identity, so the branch was unreachable from production — and that is
exactly what made it dangerous.  Nothing exercised it, nothing tested it,
nothing would have failed if a future path fell into it.  An unreachable
security bypass is a backdoor waiting for a caller, not dead code.

Consequences that are deliberate, not side effects:

- **A test may not opt out.**  Constructing an inbox without CURVE is not a
  lighter-weight test configuration; it is a configuration that must not
  exist, so a test needing an inbox must arm one (real keypair, a bound
  admission authority, a live ZAP pump).  When this invariant was introduced it aborted
  16 existing tests across two files — every one of them a place a plaintext
  ROUTER was being stood up unnoticed.  They were migrated, not exempted.
- **Fabricating a malformed frame still requires a real identity.**  Workers
  that hand-roll a raw DEALER to test the receiver's reject paths present a
  genuine admitted CURVE identity; the malformed *payload* is the thing under
  test, never the absence of authentication.
- **PANIC, not a return code.**  Reaching `start()` unarmed means a
  construction path bypassed factory validation — a programmer error, not a
  runtime condition a caller could sensibly handle.  A `false` return would be
  ignorable.

### 3.7 Send-side delivery semantics (I-SEND-BOUNDED, I-SEND-WHOLE)

**I-SEND-BOUNDED — sending never blocks the caller.**  `InboxClient::send`
hands its parts to the transport without waiting for room.  When the socket
has no peer it can write to, the message is **dropped** and the call returns
`255` immediately.  One condition covers three situations, and the sender
cannot tell them apart, because the transport reports all three the same way:

- the receiver is not draining its inbox and has hit its high-water mark;
- the peer is not connected, or has gone away;
- the peer's CURVE handshake was denied.

Dropping is the design, not a degradation.  A receiver that is not keeping up
is not helped by queueing more for it, and the alternative — wait for room —
hands the caller's liveness to the slowest peer on the channel.  What to do
about a refused send (retry, shed, escalate, stop) is the caller's decision;
the framework reports and does not decide.

> **Why this is stated as an invariant.**  The transport's default is to wait
> forever for room.  Under that default a denied or departed peer parked the
> calling thread permanently — no error, no timeout, no log — and it also made
> the documented "fire-and-forget returns immediately" contract false, because
> the wait happened before the call ever looked at its ACK timeout.  The
> non-blocking send is what makes the ACK timeout the whole bound on the call.

**I-SEND-WHOLE — a partial message never reaches the wire.**  The three parts
(`empty_delimiter`, `replay_meta`, `payload`) are submitted individually, and
which one is refused decides what happens next:

| Refused part | What the transport did | What the sender must do |
|---|---|---|
| first | wrote nothing | report the drop; nothing to clean up |
| second or third | rolled back what it wrote, and armed a **discard mode** that silently swallows following parts *while reporting success*, until one arrives marked as the last of a message | submit the remaining parts so discard mode clears **inside this message**, then report the drop |

Both halves are load-bearing.  Skipping the cleanup on a mid-message refusal
leaves discard mode armed, and it then consumes the **next** message whole
while reporting it as sent — a silent loss that fire-and-forget callers cannot
detect.  Performing that cleanup after a *first*-part refusal is the mirror
bug: nothing was written and discard mode is not armed, so the leftover parts
would queue and later emerge as a malformed two-part message.

**Back-pressure observability.**  The refusal is the only place either
condition is visible in-process — the transport exposes no receive-queue
depth, so a receiver **cannot** observe its own inbox filling.  Therefore:

- `InboxClient::send_blocked_count()` counts **every** refused send;
- the log is **edge-triggered** — one line when the path becomes blocked, one
  when it recovers.  A peer that stays blocked stays silent, because a line
  per refused send would bury the log at the exact moment an operator needs
  to read it.  Back-pressure oscillates, so blocked/recovered pairs repeating
  is normal and each pair is a real full → drained → full cycle.

**Sequence numbers are consumed by refused sends, deliberately.**  The `seq`
(§3) is taken when the frame is built, before the send, and is **not** reused
if the send is refused.  A dropped message therefore leaves a hole, and the
receiver's `recv_gap_count` (§8) is how that loss becomes visible on the far
side.  Renumbering densely on failure would make a lossy link look pristine —
the sender would know it had dropped traffic and the receiver never would.
Expect `recv_gap_count` to rise on a congested channel; that is the metric
working, not a defect.

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
  not duplicated.

**Clock source (security-critical).**  `ReplayGuard` prunes the dedup window
against a **monotonic clock it owns** — there is deliberately no per-call
timestamp argument, so the client-supplied `client_wall_ts` can never reach
the dedup window.  Pruning against a client stamp would let an authenticated
peer forward-stamp a frame to evict an earlier nonce and then replay it,
defeating this invariant; making the guard own its clock removes that footgun
structurally rather than by convention.  The client stamp is consumed ONLY by
the skew gate (check 1), which compares it to the receiver's wall clock.

**Window sizing.**  The dedup window MUST be **≥ 2 × the skew tolerance**.  A
captured frame stays skew-acceptable for up to `2 × skew` after the original
(the tolerance bounds both the original's acceptance and the replay's), so
the nonce must be remembered at least that long or a late-but-skew-valid
replay finds its nonce already pruned.  Skew tolerance defaults to 30 s and
the window to 60 s (`= 2 × skew`), on the inbox, REG, and admin planes alike.

### 3.5 CURVE Wiring (HEP-CORE-0036)

The msgpack frame in §3 is the inbox PAYLOAD; transport-layer
authentication of the inbox sockets uses **CURVE with the role's identity
keypair + HUB-WIDE `known_roles` authorization** (decided 2026-07-17).

**Authorization scope: hub-wide, NOT channel-scoped.**  The inbox is a
hub-wide role↔role messaging facility — any role may message any other role,
not just channel peers — so its authorization boundary is *"is the sender a
role this hub knows AND currently has registered"*, not *"is the sender on my
data channel."*  An earlier draft scoped it to the data channel's allowlist;
that was too narrow for the inbox's purpose.

Both halves of that boundary are load-bearing (HEP-CORE-0035 §4.9.2,
I-ROSTER-PRESENT).  Vault membership says the key is legitimate; current
registration says the role holding it is here to be the peer dialling in.
A configured role that is not running is admitted by neither, so its key
opens no mailbox — which matters because inbox traffic is role-to-role and
never passes the broker, so nothing else on this plane would notice it
being used.

- **Inbox ROUTER + DEALER use the role's IDENTITY keypair** on both sides
  (single-key model, per HEP-0036 I6 — same keypair the role uses on its data
  PUSH/PULL; broker mints NO data-plane CURVE keys).  No per-inbox keypair.
- **Hub-wide roster authorization.**  A role's data ZAP is channel-scoped,
  seeded from `REG_ACK.initial_allowlist`, so it cannot answer a hub-wide
  question.  The broker therefore **distributes the roster**: a
  `known_roles` field on `REG_ACK` / `CONSUMER_REG_ACK` carries the roles
  the hub knows and currently has registered, as `{uid, pubkey}` pairs, with
  `known_roles_version` alongside (HEP-CORE-0035 §4.9).  The role registers
  an inbox `zap_domain` whose PeerAdmission asks that roster — the inbox
  ROUTER admits any authenticated sender the roster vouches for and rejects
  everyone else (no anonymous / self-asserted senders, closing the plaintext
  gap).
- **Sender pins the receiver's identity pubkey.**  `ROLE_INFO_ACK` carries the
  receiver's identity pubkey alongside its inbox endpoint; the DEALER sets
  `curve_serverkey` to it.

**The roster is a replicated authority snapshot (HEP-CORE-0035 §4.9).**
The inbox is the first consumer of the general mechanism, and the
distribution described above is its instance, not its own protocol.
Three consequences bind here:

- The roster carries `{uid, pubkey}` pairs, not bare keys (§4.9.3).  A
  receiver that holds keys alone can admit a sender and cannot name it,
  which is what forces the sender field, the replay key, and the
  per-sender sequence state onto whatever string the sender chose to
  send.  Those three are all questions about *who sent this*, and they
  take their answer from the proven key via `attribute_sender`.
- The role's copy is replaced whole, never merged (I-ROSTER-REPLACE).  A
  merged inbox roster cannot drop a revoked key, so revocation would
  reach the hub's gate and stop there while every running role kept
  admitting the revoked sender.
- The routing identity on the ROUTER frame is an ACK return address and
  carries no authority.  It is chosen by the sender and proves nothing;
  §4.9.3 is why it does not need to.
- **Lifetime** — inbox lifetime ⊆ role lifetime (closes with role DEREG /
  HEP-0036 §5.7.2 cascade or BRC death, HEP-0036 I3).

**Roster freshness.**  The roster is delivered at registration and kept
current afterwards, because membership moves whenever any role starts or
stops (I-ROSTER-PRESENT).  Each presence rechecks its own side on every
periodic tick (HEP-CORE-0035 §4.9.7).  This gate never asks the hub — it
answers from the list the role holds, always.

**A refusal is terminal, so reachability is settled before the dial.**
A denied handshake is not retried by anything — not by the socket, not
by this layer.  A sender refused once has lost that connection and the
message it was carrying.

The system therefore does not let a sender knock at a mailbox that
cannot yet admit it (HEP-CORE-0035 §4.9.7, I-INBOX-REACHABLE).  An inbox
is located through `ROLE_INFO_REQ` — address, schema, and receiver
public key all arrive in that answer — so the hub already stands between
the two parties at the moment it matters.  It discloses the coordinates
only when the receiver has confirmed a roster naming the sender, and
otherwise sends the receiver the current list and tells the sender to
ask again.

The consequence to design against is therefore at *discovery*, not at
send: **opening an inbox to a role that started after you may report
"not reachable yet."**  That is an ordinary outcome of a call that can
already fail for several reasons, and it clears once the receiver
converges — which the hub prompts rather than waits for.

---

## 4. Data Flow

### 4.0 Inbox initiation & execution (authoritative summary)

An inbox is **receiver-authoritative** and discovered as **JSON via
`ROLE_INFO_REQ`** — never through the schema registry (`HubState.schemas`) and
never via `SCHEMA_REQ`. The inbox `schema` is a mailbox message layout, not a
channel datablock/flexzone schema (HEP-CORE-0034 §11.4). End-to-end:

1. **Receiver setup** (§4.1). The receiver declares `inbox_schema` (+ packing,
   checksum policy) in config, builds a typed `InboxQueue` (ROUTER) bound to its
   inbox endpoint — which validates the schema locally
   (`hub_inbox_queue.cpp::validate_inbox_schema`) — and advertises
   `inbox_endpoint` / `inbox_schema_json` / `inbox_checksum` on REG_REQ
   (producer) or CONSUMER_REG_REQ (consumer).  Packing travels ONCE, inside
   the §6 canonical schema object (`"packing"` is REQUIRED in-object per
   HEP-CORE-0034 §6.2); the historical separate `inbox_packing` REG wire
   field is retired (2026-07-24, HEP-0046 B.2).  The advertised
   `inbox_schema_json` is parsed + validated ONCE at the typed-body
   boundary (`hub::parse_schema_json` in the wire body constructor —
   malformed → `BODY_SCHEMA_VIOLATION`); the broker stores the string
   verbatim on the sender-visible `ProducerEntry` / `ConsumerEntry` with
   `inbox_packing` derived from the parsed spec.  It does **not** create a
   schema-registry record.
2. **Sender initiation** (§4.2). A role calls `open_inbox(target_uid)`, which
   sends **`ROLE_INFO_REQ`** for the target and reads back **`ROLE_INFO_ACK`**:
   `inbox_endpoint`, `inbox_schema` (JSON object), `inbox_packing`,
   `inbox_checksum`, and the receiver's CURVE `inbox_receiver_pubkey_z85`. From
   the JSON schema it builds an `InboxClient` (DEALER), pinning that pubkey.
3. **Execution** (§4.3). Sender `acquire()` → fill the typed slot → `send()`.
   The wire frame is `msgpack fixarray[5] = [magic, schema_tag, seq, payload,
   checksum]`, where `schema_tag` is the first 8 bytes of BLAKE2b-256 over the
   canonical schema (`compute_inbox_schema_tag`). The receiver's `InboxQueue`
   validates magic, `schema_tag` (drift), field count, replay nonce, and
   per-sender sequence, then dispatches to the `on_inbox` handler and ACKs.

The rest of §4 details each step.

### 4.1 Receiver Setup (role host startup)

The inbox ROUTER binds — CURVE-armed, admitting nobody — at **S1**
(role-host setup), BEFORE registration.  Binding early is what resolves
a port-0 endpoint so S2 can advertise the real port in REG_REQ; the
deny-all arm preserves HEP-CORE-0036 §3.5.1's "nothing happens behind
the auth door before auth" — the socket exists but admits NO peer until
the roster arrives.

**The gate asks the role; it holds no list of its own.**  What lifts
deny-all is the ROLE's roster changing (S3), not a list being pushed
down to the socket.  A copy parked on the queue would be a second
representation of the hub's key list, free to disagree with the role's
own — and the one the ZAP handler consults would be the one nobody
re-reads after a revocation (HEP-CORE-0035 §4.9.6).  So the queue is
given a question to ask, once, when the role wires it up, and answers
`no` to everything until the role has an authority that says otherwise.
The S1/S2/S3 listing below is the normative sequence.

```
S1 (setup_infrastructure_) — BIND + CURVE-ARM DENY-ALL:
  1. Role host reads inbox config (schema, endpoint, buffer_depth, packing).
  2. Build InboxQueue, arm CURVE-server auth (role identity keypair via
     `set_curve_server_identity(kRoleIdentityName, "<uid>:inbox")`), and
     bind the ROUTER — with NO admission authority bound.  The
     socket is CURVE-armed the instant it binds, so no unauthenticated
     peer can complete a handshake before the roster arrives; an unbound
     gate means NO peer is admitted yet.  Binding at S1 (rather than
     deferring to S3) resolves port-0 endpoints before S2 advertises them
     in REG_REQ.  The inbox `zap_domain` ("<uid>:inbox") is DISTINCT from
     the data channel's — hub-wide known_roles authorization, not the
     channel allowlist (§3.5).
  2a. The role wires the gate to itself (`set_admission_authority`) as it
     takes ownership of the queue.  From here the ROUTER answers out of
     the role's rosters, which are empty — so the posture is unchanged,
     and there is never a moment where the socket is up with nobody to
     ask.

S2 (registration) — FATAL on failure:
  3. For EACH presence the role registers (one for producer/consumer
     roles; two for processor — see HEP-CORE-0033 §19), the inbox
     metadata block is appended to that presence's registration
     payload:
       - producer presence  → REG_REQ          (ProducerRegInputs)
       - consumer presence  → CONSUMER_REG_REQ (ConsumerRegInputs)
     Fields per presence: inbox_endpoint, inbox_schema_json (packing
     rides in-object — the separate inbox_packing REG field is retired,
     HEP-0046 B.2), inbox_checksum.  Same `inbox_endpoint` string is
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

S3 (apply_*_reg_ack) — ADOPT THE ROSTER (the gate's answers change):
  5. `adopt_inbox_roster(ack, side)` REPLACES what this SIDE holds with
       the `known_roles` this hub just sent — the input side for
       CONSUMER_REG_ACK, the output side for REG_ACK.  Nothing is pushed
       to the socket: the gate wired up at S1 was already asking, and
       from this point it gets a different answer.  The inbox is a
       hub-wide role<->role facility, so it admits any authenticated
       known_role (single-key model I6), NOT just channel peers.
       A role is admitted if EITHER side's hub vouches for it
       (HEP-CORE-0035 §4.9 I-ROSTER-COMBINE-ADMIT): a dual-hub processor
       holds one roster per hub, and neither hub's list may erase the
       other's.
```

The receive thread (`inbox_thread_`: loop { recv_one() → invoke_on_inbox()
→ send_ack() }, under ThreadManager scope per HEP-CORE-0036 §3.5.4
invariant 4) runs from S1 — but until S3 the role's rosters are empty and
the ROUTER denies everyone, so no message reaches the handler before
authorization exists.

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

#### 4.2.1 Why opening is two phases, and why the first one can refuse

Opening an inbox is **discover, then dial**, and the hub is deliberately
between the two.  The reason is a property of the transport rather than a
policy choice:

> **A refused handshake is terminal.**  When the receiver's gate says no,
> its ZMQ layer answers the authentication exchange with an error and tears
> the connection down.  The sender's session is *terminated*, not retried —
> by the library, and independently by this project's socket policy, which
> stops retrying a connection whose handshake failed.  Nothing reconnects at
> any layer.  A sender refused once has lost that connection **and the
> message it was carrying**.

If a sender could dial before the receiver was able to admit it, the cost of
being early would not be a delay — it would be a silently dropped message
and a dead socket that never heals.  That is why reachability is settled
during discovery, where a caller is already prepared for an answer of "no",
instead of being discovered by attempting.

The receiver's gate answers from the roster it holds, which is the roles its
hub knows **and currently has registered** (HEP-CORE-0035 §4.9.2).  A role
that starts later is therefore genuinely absent from an already-running
peer's list until that peer converges — and the hub, which knows both which
roster version admitted the sender and which version the receiver has
confirmed applying, is the only party that can tell whether dialling will
work.  So it withholds the address until it will (HEP-CORE-0035 §4.9.7,
I-INBOX-REACHABLE), prompting the receiver rather than waiting for its next
scheduled check.

**What the hub does NOT promise.**  The asking role's identity on
`ROLE_INFO_REQ` is a claim, not a proof — that message tier deliberately
does not bind the body to the proven key, because its subject is a third
party.  Reachability is therefore an *availability* mechanism, never an
access control.  The access control is the receiver's own gate, which
decides on the key the sender proved during the handshake.  A caller that
misidentifies itself gets a reachability answer about someone else and is
then refused at the door — it gains nothing and spends its own first
attempt.  Stated exactly: **the first dial succeeds for a caller that
identified itself honestly.**

#### 4.2.2 What can fail, and what each failure means

`open_inbox` returns nothing for four distinct reasons.  They are not
interchangeable, and a caller that treats them alike will either give up on
something transient or retry something permanent forever.

| Reason | Meaning | Clears by itself? |
|---|---|---|
| `no_such_role` | No role by that uid is registered on any hub this sender can reach. | Only if that role starts. |
| `no_inbox` | The role exists but runs no mailbox — nothing to send to. | No.  It is a property of how that role was configured. |
| `not_reachable_yet` | The role exists and has a mailbox, but has not yet confirmed a roster naming this sender.  The hub has sent it one. | **Yes** — typically within one hub round trip. |
| `sender_not_registered` | This sender holds no registration on the hub that owns the target's mailbox, so no roster that hub issues can ever name it. | No.  Retrying is a livelock. |

The transport can also fail the discovery outright (no answer within the
timeout), which is a connectivity problem rather than an answer.

Once the address is in hand, dialling and sending have their own outcomes.
`send` returns `0` on an acknowledged delivery and a non-zero code
otherwise; the two ways it fails are worth telling apart when reading logs:

- **No writable peer** — the connection is gone or was never established, so
  the frame is refused immediately and dropped.  Fast.
- **No acknowledgement** — the frame went out and nothing came back within
  the caller's budget.  Costs the full timeout.

#### 4.2.3 Who retries what

**The framework does not retry anything, and this is deliberate.**  It
reports outcomes and leaves the decision to the script, because only the
script knows whether a particular message is still worth sending by the time
it could be resent.

What the framework *does* guarantee is that retrying is possible and
informed: the reachability answer says whether waiting will help, and a
condition that clears is distinguished from one that does not.

The shape a sending script should have:

```
# Retry at the OPEN, not at the send.
handle = api.open_inbox(target)
if handle is None:
    return                 # try again next cycle; the condition may clear
slot = handle.acquire()
slot.value = ...
rc = handle.send()
if rc != 0:
    # The message is gone.  Sending again is a NEW message, and that is
    # the script's decision — the framework will not make it silently.
    ...
```

Two properties this relies on:

- **Loss is reported, never repaired.**  A message's sequence number is
  consumed when the send is attempted and is *not* reused if it fails, so a
  dropped message leaves a visible gap in the receiver's per-sender numbering
  (§4.3).  Renumbering densely on failure would make a lossy link look
  pristine — the sender would know it dropped traffic and the receiver never
  would.
- **Redelivery is not automatic.**  Because the framework never resends, a
  receiver sees each message at most once, and `on_inbox` needs no
  idempotence for the framework's benefit.  A script that chooses to resend
  is creating a second message, with its own sequence number, and owns the
  duplicate-handling that implies.

**Why the retry belongs at the open rather than at the send.**  The
condition that clears — `not_reachable_yet` — clears in the *hub's* state,
not in the socket's.  Re-opening consults the hub again; re-sending on an
existing handle does not, and if the handle was never obtained there is
nothing to send on anyway.  A script that loops at the open therefore needs
no backoff logic of its own for the ordinary startup race: the peer becomes
reachable and the next cycle succeeds.

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

#### 4.3.1 The complete sequence, both levels

Written out because the application-level exchange and the transport-level
handshake interleave, and a fault in one surfaces as a symptom in the other.
Reading only one level is how "the sender waited five seconds and reported no
acknowledgement" gets diagnosed as a slow receiver when the connection had
been refused a millisecond after it opened.

**Receiver, at startup.**  The ROUTER is created, given the house socket
policy, armed as a CURVE server under its own ZAP domain, and — *before*
`bind()` — that domain is registered with the process's ZAP router, so no
handshake can arrive un-gated.  Until the role binds its roster in, the gate
denies everything (§3.5).  The role registers with its hub, adopts the
roster the acknowledgement carries, and reports the version it now holds.

**Sender, on `open_inbox`.**

| Step | Level | What happens |
|---|---|---|
| 1 | app | `ROLE_INFO_REQ` to each of the sender's hubs until one answers |
| 2 | app | The hub tests reachability; withholds or discloses (§4.2.1) |
| 3 | app | `ROLE_INFO_ACK` carries endpoint, schema, packing, checksum policy, and the receiver's public key |
| 4 | — | A client object is built.  **No socket yet** — only layout and buffers |
| 5 | transport | DEALER created; socket policy applied; routing id set to the sender's uid; CURVE armed with the sender's keypair and the receiver's key as server key |
| 6 | transport | `connect()` returns immediately — the connection is asynchronous, and a pipe to it exists *before* the handshake completes |

**Then, concurrently.**

| Step | Level | What happens |
|---|---|---|
| 7 | transport | Greeting, then the CURVE exchange; the receiver extracts the sender's long-term key |
| 8 | transport | The receiver's ZMQ layer asks its ZAP router about that key |
| 9 | app | The gate answers from the roster the role holds — no hub is consulted |
| 10 | transport | On yes: the handshake completes and the sender's queued frame flows.  On no: an error is returned to the sender, the connection is destroyed, **and any frame already queued on it is discarded** |

**The consequence to internalise:** because a pipe exists from step 6, a
`send` issued before step 10 *succeeds locally* — it is accepted into a
connection that may then be torn down.  The sender learns nothing at that
moment; it learns at its acknowledgement deadline, which is why an
early-and-refused send is expensive rather than instant.  §4.2.1 is how the
system avoids reaching step 10 with a "no".

**On success**, the receiver's ROUTER hands the frame up: replay metadata is
checked, the payload decoded, the sender named from the key it proved rather
than from anything it claimed, and `on_inbox` invoked.  The acknowledgement
is sent by the *application* after the handler returns — there is no
transport-level acknowledgement anywhere in this protocol, which is why "the
receiver never got it" and "the receiver got it and did not answer" are
indistinguishable to the sender.

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
discovery.

**Reachability is two conditions, not one.**  The network must route
(this section), AND the answering hub must be willing to disclose the
address (§4.2.1).  The second is per hub: each hub keeps its own roster,
so a receiver spanning two hubs holds and confirms one roster per side,
and a sender is disclosed the address only by a hub whose roster the
receiver has confirmed naming *that* sender.  A sender registered on hub A
therefore cannot reach the receiver "through" hub B even when B also holds
the metadata — B's roster does not name it.

That is self-consistent rather than a limitation: a sender queries only
hubs it holds a connection to, and it holds a connection only where it has
a presence, so any hub that can answer it is a hub where it is registered
and therefore rosterable.  Worth stating because it reads like a gap until
traced.

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

**A failed open caches nothing.**  Any of the four refusals in §4.2.2
returns without creating a client, so a script that loops at the open is
re-asking the hub each time rather than being handed the same refusal from
memory — which is what makes `not_reachable_yet` clear on its own.  The
cache holds successful opens only.

**A cached handle is not a liveness claim.**  It means this sender once
obtained the target's coordinates and completed a handshake; the peer may
have stopped since.  `send` reports the outcome per call, and that is the
only current answer.

---

## 8. Metrics

InboxQueue exposes four diagnostic counters:

| Metric | Description |
|--------|-------------|
| `recv_frame_error_count` | Frames rejected: bad magic, schema tag mismatch, field type/size error |
| `ack_send_error_count` | ZMQ send errors when sending ACK response |
| `recv_gap_count` | Sequence number gaps (per-sender tracking; indicates dropped frames) |
| `checksum_error_count` | BLAKE2b verification failures after successful decode |

InboxClient exposes two, on the sending side:

| Metric | Description |
|--------|-------------|
| `send_blocked_count` | Sends refused because the socket had no writable peer (§3.7) |
| `ack_stale_count` | Receipts discarded because their `seq` belonged to an earlier, timed-out send (§3) |

A non-zero `ack_stale_count` is not an error — it means `ack_timeout` is
tighter than the receiver's real turnaround, so sends are succeeding and the
caller simply is not waiting long enough to hear so.  Either widen the timeout
or accept the ambiguity.  The value matters because before the ACK carried a
`seq`, every one of these was silently returned as the *following* message's
result.

Read the two together.  `send_blocked_count` rising on the sender and
`recv_gap_count` rising on the receiver are the **same** event seen from both
ends: back-pressure or a lost peer caused a drop, and the sequence hole is how
the receiver learns of it (§3.7).  A gap count that climbs with no matching
`send_blocked_count` on any sender means something else lost the frame — a
schema or checksum rejection, or a replay-guard drop — so check the other
counters before suspecting the link.

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
- **HEP-CORE-0034 §11.4**: Inbox message layouts are **NOT** schema-registry
  records. The registry (`HubState.schemas`) holds channel datablock/flexzone
  schemas only. The inbox schema is stored on the sender-visible
  `ProducerEntry` / `ConsumerEntry` (from `inbox_schema_json` /
  `inbox_endpoint` / `inbox_checksum` on REG_REQ; the stored `inbox_packing`
  is derived from the schema object's in-object `packing`) and discovered by
  senders as **JSON via `ROLE_INFO_REQ`** (§4.0, §4.2) — never via
  `SCHEMA_REQ`. Validation happens ONCE at the typed-body boundary
  (HEP-0046 B.2: `hub::parse_schema_json` in the wire body constructor,
  malformed → `BODY_SCHEMA_VIOLATION`); the broker handler files no record
  and never re-parses the string. The per-message drift `schema_tag`
  (`compute_inbox_schema_tag`) is a data-plane concern of this HEP, not a
  HEP-0034 registry fingerprint.
