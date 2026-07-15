---
title: "REG/REG_ACK protocol redesign — addendum: addressing invariants + typed wire envelope"
status: DESIGN LOCKED (findings A1..G3; decisions locked under CURVE-authenticated frame; not yet merged into main draft)
parent: DRAFT_reg_ack_protocol_redesign.md
target permanent docs on adoption:
  - Main draft §8.1 (invariants list — expand from 4 to 21)
  - Main draft §2 (message shapes — restate as typed body classes per msg_type)
  - Main draft new §14 (typed wire envelope contract)
  - HEP-CORE-0033 §9 (wire framing) + role identity §
  - HEP-CORE-0036 §6.3 (role identity binding) + §6.5 (auth refresh)
---

# REG/REG_ACK protocol redesign — addendum: addressing invariants + typed wire envelope

## 0. Why this document exists

The main draft locks the message-payload shapes and admission
sequences.  It does not lock the **addressing primitives** that
carry those messages, the **typed envelope** that structures them,
or the **security invariants** (anti-replay, envelope↔body
binding, key rotation) that a CURVE-authenticated system needs on
top of frame encryption.

The gap is not academic.  The fan-in NOTIFY-routing bug (Q6 in the
main draft) reduced, on inspection, to: every DEALER connecting to
the broker's ROUTER presents libzmq's `rand()`-derived default
identity.  In fresh unseeded processes that value is always
`\x00\x6b\x8b\x45\x67` — every role's DEALER collides on the SAME
identity, so unsolicited NOTIFYs targeted at role X are delivered to
whichever DEALER connected most recently (typically role Y).  The
broker was addressing a well-defined role_uid; the wire couldn't
carry the address to the right socket because no invariant said
"the DEALER identifies itself by role_uid."

That is one instance of a class of gap.  This document enumerates
the class.  For every addressing primitive the protocol depends on,
state:
1. What value identifies each socket / peer / channel / role
   instance.
2. What guarantees its uniqueness.
3. Where mismatch is wire-rejected.
4. What data structures / API surfaces the invariant collapses
   (redundant cached copies retire).

Then replace scattered JSON key extraction with a typed wire
envelope: 4 skeleton frames identical across every message, plus
1 body frame parsed via a per-msg-type typed body class with
named accessors.  No handler ever calls `body.value("field")`.
The typed envelope is also the anchor for the security invariants
(A9 anti-replay nonce/wall_ts, A10 envelope↔body cryptographic
binding, A11 key-rotation gate) — every REG-family body class
exposes the accessors, `WireEnvelope::parse` validates the hash.

## 1. Scope of audit

Reviewed:
- `docs/tech_draft/DRAFT_reg_ack_protocol_redesign.md` end-to-end
  (1772 lines; §0-14, findings F1-F17).
- Cross-references into HEP-CORE-0007 §12, HEP-CORE-0036 §5b/§6.5,
  HEP-CORE-0017 §4.7 (as merged surfaces of the draft).
- Existing code: broker_service.cpp REG handlers,
  broker_request_comm.cpp DEALER init, fire_channel_auth_changed_
  notify fan-out, send_to_identity primitive.

Not reviewed (in scope for later, out of scope for this audit):
- HEP-CORE-0044 SHM capability handshake internals.
- HEP-CORE-0030 band routing (flagged as suspected same-shape bug;
  see G2).
- HEP-CORE-0022 hub federation (flagged; see G1).

## 2. Audit method

For each addressing primitive in the wire and in HubState:
- Where does the value come from?
- What makes it unique across the population that shares the
  namespace?
- Where is uniqueness wire-enforced?
- What does the receiver assume, and would that assumption fail
  under adversarial inputs?

For each message shape:
- Which fields decide "should I process this?" (dispatch + reject
  gates)?
- Are they at the beginning of the wire representation?
- Can the receiver dispatch/reject without deserializing every field?

## 3. Findings summary

Seven categories, 29 individual findings.  Every finding is a
**design gap** or **missing invariant**, not a code bug — the code
fix follows the invariant statement.

| Category | Count | Nature |
|---|---|---|
| A. Missing invariants (addressing + security primitives) | 10 | Invariants unstated; wire-enforcement gaps (A7 subsumed by B1) |
| B. Typed envelope, not JSON scatter | 5 | Wire skeleton + per-msg-type typed body classes |
| C. Implicit runtime magic | 3 | Behaviors relied on but not documented as invariants |
| D. Data-structure redundancy | 3 | Cached duplicates of invariant-derived values (D2/D3 = KEEP as integrity witnesses) |
| E. Sender-validation duplication | 1 | Three handlers doing the same check with bespoke code |
| F. Undocumented policy | 4 | Idempotency, retry, shape taxonomy, wire versioning |
| G. Symmetric coverage extensions | 3 | Federation + bands must share the invariants |

Detailed findings below.  Each finding follows the shape:
`<Finding ID> — <one-line statement>` / `Invariant text (ready to
graft)` / `Wire-enforcement point` / `Retirements this enables`.

---

## 4. Category A — Missing invariants (addressing primitives)

### A1 — DEALER routing_id ≡ role_uid

**Statement.** Every DEALER connecting to the broker's ROUTER MUST
set `routing_id = role_uid` before `connect()`.  Every REG_REQ /
CONSUMER_REG_REQ MUST arrive with a ROUTER identity frame whose
bytes equal the JSON payload's `role_uid`.  Mismatch is
`INVALID_REQUEST` at wire admission.

**Invariant text (for §8.1).**

> **I-DEALER-IDENTITY.**  For every control-plane DEALER↔ROUTER
> connection between a role and a broker, the DEALER MUST set
> `ZMQ_ROUTING_ID` to its `role_uid` before `connect()`.  The
> broker's REG / CONSUMER_REG / any-subsequent admission handler
> MUST verify that the ROUTER-captured identity frame equals the
> payload's `role_uid` field; mismatch → `INVALID_REQUEST` reject
> with `error_code="IDENTITY_MISMATCH"`.  Rationale: libzmq's
> default DEALER identity is `rand()`-derived and collides across
> fresh processes; a ROUTER cannot address distinct DEALERs without
> a stable per-role identity, and unsolicited sends
> (`CHANNEL_AUTH_CHANGED_NOTIFY`, `CHANNEL_CLOSING_NOTIFY`, etc.)
> silently misroute otherwise.

**Wire-enforcement point.**  `handle_reg_req` /
`handle_consumer_reg_req` / any handler that captures identity
today (broker_service.cpp:3445 and its producer-side twin).  Reject
BEFORE `_on_consumer_joined` / `_on_producer_added`.

**Retirements this enables (see D1).**
- `ConsumerEntry::zmq_identity`
- `ProducerEntry::zmq_identity`
- `BandMember::zmq_identity`
- Every `send_to_identity(x.zmq_identity, ...)` reduces to
  `send_to_identity(x.role_uid, ...)`.
- Every `for (const auto& c : ch->consumers) fan_to(c.zmq_identity)`
  reduces to `fan_to(c.role_uid)`.

---

### A2 — role_uid ⇔ zmq_pubkey is a fixed admin-declared binding

**Statement.**  For every role process, the pair
`(role_uid, zmq_pubkey)` is invariant for the role's lifetime and
matches exactly one entry in the broker's `known_roles`.  Currently
the broker checks role_uid → pubkey (verify_known_role_binding).
Not stated: the reverse mapping — a given pubkey MAY NOT appear
under multiple role_uids in `known_roles`.

**Invariant text.**

> **I-PUBKEY-BINDING.**  Every entry in `known_roles` names a
> distinct `(role_uid, zmq_pubkey)` pair.  The pair is
> bidirectionally unique: no two entries share a role_uid, and no
> two entries share a zmq_pubkey.  At REG admission the broker MUST
> verify (a) the payload's (role_uid, zmq_pubkey) matches an entry
> and (b) the DEALER's presented CURVE certificate (curve_publickey
> in the ZAP AUTH) equals the payload's zmq_pubkey.

**Wire-enforcement point.**  `verify_known_role_binding` extended
to cover reverse mapping.  ZAP handler already validates (b) —
document the coupling.

**Retirements this enables.**  None directly; hardens against
config bugs where the same pubkey is copied under two role_uids
(silent identity aliasing).

---

### A3 — Data-plane CURVE identity ≡ zmq_pubkey

**Statement.**  Every data-plane socket (PUSH/PULL/PUB/SUB/capability)
owned by a role MUST present `curve_publickey = role.zmq_pubkey`.
The role's REG-declared `zmq_pubkey` is the same value as its
data-plane `curve_publickey`, which is the same value as its
control-plane `curve_publickey` (BRC DEALER's CURVE cert).

**Invariant text.**

> **I-CURVE-IS-DECLARED.**  A role's CURVE identity is a single
> value applied uniformly: (a) BRC DEALER's `curve_publickey` at
> connect() to the broker's CTRL ROUTER, (b) data-plane socket's
> `curve_publickey` at connect()/bind(), (c) the `zmq_pubkey` field
> on all REG-family messages the role emits.  Any implementation
> that reads the pubkey from more than one config field breaks
> this and produces silent "authenticates on control plane, denied
> on data plane" splits.

**Wire-enforcement point.**  Role-side keystore is the single
source (`secure().keys().pubkey(keystore_name)`).  BRC + tx_queue +
rx_queue MUST read from the same keystore entry; adding a config
knob that "override" the data-plane pubkey is a design violation.

**Retirements this enables.**  Per-socket pubkey config fields
(if any exist as split knobs).

---

### A4 — At most one binding-side role per channel

**Statement.**  For any channel K, the cardinality gate enforces
at most one binding-side role (fan-in: 1 consumer; fan-out /
one-to-one: 1 producer).  The `confirmed_version[K][B]` map indexes
by binding-side role_uid; well-defined because B is unambiguous.

**Invariant text.**

> **I-CHANNEL-SINGLE-BINDING-SIDE.**  For every channel K at every
> instant t, `ChannelEntry.binding_side_uid()` returns exactly one
> role_uid or `nullopt` (channel not yet opened).  This is enforced
> by the cardinality gate at REG admission.  Every `confirmed_
> version[K][B]` write uses B = `binding_side_uid()` at write time.

**Wire-enforcement point.**  Cardinality gate at `_on_producer_
added` / `_on_consumer_joined`.  Already present; missing is the
statement that this is what makes B unambiguous throughout.

**Retirements this enables.**  Any code that iterates a
"producers list" or "consumers list" looking for "the binding one"
reduces to `binding_side_uid()`.

---

### A5 — instance_id is broker-instance-scoped monotonic

**Statement.**  The broker's `producer_instance[role_uid]` /
`consumer_instance[role_uid]` counter bumps on every fresh
admission of that role_uid within one broker process lifetime.
Resets to 0 when the broker restarts (whether the broker persists
HubState or not).  Cross-broker-restart, previously-issued
instance_ids are NOT recognized as stale by the reborn broker —
they're recognized as "never seen."

**Invariant text.**

> **I-INSTANCE-ID-EPHEMERAL.**  `instance_id` is monotonic within
> one broker process lifetime; not persisted across broker restart.
> Stale-instance guards (HEP-CORE-0042 §5.5.3) protect against
> in-flight ACK reordering within a broker lifetime, not against
> broker restarts.  Broker restart drops all in-flight admissions;
> clients re-register and receive fresh instance_ids.

**Wire-enforcement point.**  `producer_instance()` accessor +
`_on_producer_added` bump.  Document its ephemerality.

**Retirements this enables.**  Any code that persists instance_id
across broker restart (if such code exists) is broken.

---

### A6 — Correlation ID is the ACK-matching key

**Statement.**  Currently BRC matches ACKs by msg_type
(broker_request_comm.cpp:231).  Under §7.2.4 duplicate REG_REQ
handling, two REG_REQs from the same role may be in flight; two
in-flight REG_ACKs collide on msg_type.  Under long-window pending
(§13.7.b), retry-under-timeout produces stacked ACKs.
msg_type-only matching is insufficient.

**Invariant text.**

> **I-CORRELATION-STABLE.**  Every REQ MUST carry a non-empty
> `correlation_id`; every ACK MUST echo it.  Client-side ACK
> matching uses (msg_type, correlation_id) as the key, not
> msg_type alone.  Broker's late-reply / duplicate-request handling
> uses correlation_id to disambiguate.  msg_type-only matching is
> the pre-migration behavior and produces cross-wire ambiguity
> under retry.

**Wire-enforcement point.**  BRC's `pending_requests` map keyed on
`(msg_type, correlation_id)`, not msg_type alone.  Empty
correlation_id on a REQ is a wire violation rejected at
`WireEnvelope::parse` with `error_code=INVALID_REQUEST` — no
grace-period generation, no broker-side synthesis.  Under the
CURVE-authenticated frame (§13 decision #4), grace periods on
correlation are documented attack windows; the wire cut ships
correlation_id enforcement from day one.

**Retirements this enables.**  The msg_type-only ACK-matching path
in broker_request_comm.cpp and the abandoned-request cross-wire
workarounds (broker_request_comm.cpp:236-249) built to paper over
it.

---

### A7 — (Subsumed by B1 typed envelope)

A7 in an earlier draft of this addendum proposed a 4-frame
preamble.  Superseded by B1's typed wire envelope (4 skeleton
frames + 1 body frame) + per-msg-type typed body classes.
Retained here as an ID placeholder to keep numbering stable
across the addendum.

---

### A8 — HubState mutation atomicity

**Statement.**  All admission-affecting mutations (channel
create/close, roster add/remove, allowlist add/remove,
channel_version bump, confirmed_version write) execute under
HubState's writer lock.  Reads use shared_lock.  §13.1.b, §13.1.j
implicitly rely on this; I-ROUTER-SERIAL only ensures dispatch-thread
serialization, not lock ownership.

**Invariant text.**

> **I-STATE-MUTATION-ATOMIC.**  Every mutation on HubState that
> affects admission runs under HubState's writer lock (unique_lock).
> Every read that participates in an admission decision runs under
> shared_lock or is captured to a value copy under one such lock.
> The writer lock is the sole ordering primitive for allowlist +
> channel_version + confirmed_version consistency.

**Wire-enforcement point.**  Not wire — data-structure API
discipline.  Encode via named methods that take the lock; forbid
raw-field access from outside HubState.

**Retirements this enables.**  Any code that reads
`ChannelEntry.channel_version` outside a HubState accessor +
subsequently makes an admission decision on the stale value.

---

### A9 — Anti-replay on REG-family messages

**Statement.**  CURVE frame encryption prevents forgery of REG_REQ
content; it does NOT prevent an attacker who captured a valid past
REG_REQ from replaying it after the legitimate role has DEREG'd
(or crashed).  The replay would authenticate against ZAP, pass the
identity check (identity + role_uid match the captured pair), and
re-admit the attacker as the legitimate role.

**Invariant text.**

> **I-REPLAY-BOUND.**  Every REG_REQ, CONSUMER_REG_REQ,
> ENDPOINT_UPDATE_REQ, CHANNEL_AUTH_APPLIED_REQ, DEREG_REQ,
> CONSUMER_DEREG_REQ carries a `client_nonce` (16 bytes,
> cryptographically random) AND a `client_wall_ts` (uint64,
> milliseconds since epoch).  Broker rejects with
> `error_code=REPLAY_OR_SKEW` when either:
> (a) `client_nonce` reuse within a sliding window of `2 *
> pending_budget_ms` (bounds "pending REG_REQ still parked" +
> "just cleared, ACK in flight" cases);
> (b) `|broker_wall_ts - client_wall_ts| > 30_000` ms (clock
> skew tolerance).  Nonce dedup structure is per-role_uid,
> in-memory, sliding-window pruned.

**Wire-enforcement point.**  New handler-side check at REG-family
admission, BEFORE any state mutation.  Frame 4 body class
(`RegReqBody` etc.) exposes `client_nonce()` and `client_wall_ts()`
accessors.

**Retirements this enables.**  Any implicit reliance on "CURVE
prevents replay" — it doesn't; only anti-replay does.

---

### A10 — Envelope↔body cryptographic binding

**Statement.**  CURVE encrypts each ZMQ frame as a discrete unit.
Nothing at the wire prevents an attacker who captured multiple
valid messages from splicing a Frame 4 body from message X into a
Frame 3 correlation_id / Frame 2 msg_type from message Y.  Under
CURVE the splice would decrypt to plaintext, both frames would be
individually valid, but the SEMANTIC binding between them
("this correlation_id belongs to this body") would be false.

**Invariant text.**

> **I-ENVELOPE-BODY-BINDING.**  Every msg_type body (Frame 4)
> includes an `envelope_hash` field computed as
> `BLAKE2b-256(Frame0_identity || Frame2_msg_type ||
> Frame3_correlation_id)` by the sender.  Receiver recomputes
> and rejects with `error_code=ENVELOPE_TAMPERED` on mismatch.
> Binds the msg_type body to the specific envelope it was sent
> in; a spliced or replayed body detached from its origin
> envelope fails the check.

**Wire-enforcement point.**  `WireEnvelope::parse` computes the
expected hash from Frames 0/2/3, compares against the body's
`envelope_hash` field, rejects on mismatch.  Every body class
carries `envelope_hash()` as an accessor + `set_envelope_hash()`
on the sender side.

---

### A11 — Key rotation only via DEREG + known_roles update

**Statement.**  A role's CURVE keypair may rotate.  Under the
static `known_roles` binding, in-band key rotation is not
supported — a role that presents a new pubkey silently mid-lifetime
would be a hostile identity swap (attacker who compromised the
role's process could rotate the key to lock out the legitimate
operator).

**Invariant text.**

> **I-KEY-ROTATION-VIA-DEREG.**  A role's CURVE pubkey is
> immutable for a given (`role_uid`, `broker_lifetime`) pair.
> Any REG-family request whose `zmq_pubkey` differs from the
> role's currently-registered pubkey is rejected with
> `error_code=UID_CONFLICT` (already-known role reregistering
> under new pubkey) or `KEY_ROTATION_REQUIRES_DEREG` (post-
> DEREG re-REG with pubkey different from known_roles).  Key
> rotation is an operator action: (1) DEREG the role, (2) update
> `known_roles` config to the new pubkey, (3) role re-REG with
> the new key.  No in-band rotation, no dual-pubkey window.

**Wire-enforcement point.**  `verify_known_role_binding` +
existing UID_CONFLICT detection at REG admission.  Doc the
operator procedure explicitly.

---

## 5. Category B — Typed envelope, not JSON scatter

The user directive: "the protocol message should be designed to
contain essential information at the beginning" — AND — "we don't
need ad hoc hand-crafted message/setup of variables that scatters
along all the pathways.  this is toxic and fragile."

The two directives compose into one contract: **essential fields
live in a typed envelope at fixed wire positions, accessed through
a single-owner typed accessor.  No handler carves fields out of a
JSON body.  A field that today appears as `body.value("role_uid",
"")` scattered across N handlers is a design bug — the field
belongs at a fixed envelope position, read once through the
envelope accessor.**

Reordering JSON keys does NOT satisfy this contract; every handler
still hand-carves.  The fix is structural, not cosmetic.

### B1 — The wire envelope (4 skeleton frames + 1 body frame)

**Statement.**  Every control-plane message is 5 ZMQ frames total:
**4 skeleton frames** identical across every REQ / ACK / NOTIFY
(Frames 0-3), plus **1 body frame** (Frame 4) whose JSON schema is
per-msg-type.  Only fields the RECEIVER uses on EVERY message
(dispatch + correlation) live at fixed skeleton-frame positions.
Every msg_type-specific field — including `channel_name`,
`role_uid`, `role_type`, `phase`, `channel_version`, `instance_id`,
admission params, `client_nonce`, `client_wall_ts`,
`envelope_hash` — lives inside Frame 4 and is accessed through a
per-msg-type typed body class with named accessors.

Terminology used throughout the addendum:
- **Skeleton** = Frames 0-3 (always same shape)
- **Body** = Frame 4 (per-msg-type schema, typed class)
- **Envelope** = the whole 5-frame message (skeleton + body)

**Rationale for the split.**  A previous draft of this addendum
proposed a fixed 7-key envelope header (Frame 3) shared across
every message, with "empty-string / zero where not applicable."
That mixed two concerns:

- (a) fields the receiver dispatches / correlates on for EVERY
  message (`msg_type`, `correlation_id`) — these belong at fixed
  frame positions so BRC's ACK-matching + broker's dispatch don't
  need to parse msg_type-specific bodies;
- (b) admission-required params only available at REQ time
  (`channel_name`, `role_uid`, `role_type`, `channel_topology`,
  `abi_fingerprint`, `schema_*`) — these belong in the REG_REQ
  body specifically, not in a shared envelope header where they
  are empty on 80% of messages.

Padding shared-envelope keys with empties is the same class of
scatter the user rejected: every handler still branches on
"is this field populated for MY msg_type?"  The right factoring
is that ONLY (a)-class fields live at fixed frames; every
(b)-class field lives inside a msg_type-specific typed body.

**Frame layout.**

```
Frame 0 : identity           (bytes; role_uid or hub_uid per A1/G1;
                              ROUTER-managed on receive, DEALER-set
                              via ZMQ_ROUTING_ID on send)
Frame 1 : control marker     ('C', single byte; kFrameTypeControl)
Frame 2 : msg_type           (ASCII, ≤64 bytes)
Frame 3 : correlation_id     (ASCII, ≤64 bytes; empty for fire-and-forget NOTIFY)
Frame 4 : msg_type body      (JSON blob; schema is per-msg-type;
                              parsed via typed body class)
```

Four skeleton frames hold "always-present" primitives (Frames 0,
1, 2, 3).  Frame 4 holds the per-msg-type body.  BRC's ACK-matching
keys on `(Frame 2 msg_type, Frame 3 correlation_id)` without
parsing Frame 4.  Handler dispatch keys on Frame 2 alone.

**Per-msg-type typed body classes.**  Frame 4 is parsed via a
typed body class chosen by msg_type.  Every field has a named
accessor.  Handlers receive the typed body by reference; no
handler ever calls `body.value("field", ...)`.

```cpp
// In utils/network_comm/wire_bodies.hpp — one class per msg_type

class RegReqBody {
public:
    explicit RegReqBody(const nlohmann::json& j);   // validates + stores
    // Admission-required fields:
    std::string channel_name()   const;
    std::string role_uid()       const;
    std::string role_type()      const;
    std::string role_name()      const;
    std::string channel_topology() const;
    std::string data_transport() const;
    std::string zmq_pubkey()     const;
    uint32_t    broker_proto()   const;   // I-WIRE-VERSION-ATOMIC gate
    // Schema invariants:
    std::string schema_hash()    const;
    uint32_t    schema_version() const;
    std::string schema_id()      const;
    std::string schema_blds()    const;
    std::string schema_owner()   const;
    const nlohmann::json& abi_fingerprint() const;
    // Security fields (mandatory on every REG-family body per A9/A10):
    std::string client_nonce()   const;   // 16 random bytes (Z85 or hex)
    uint64_t    client_wall_ts() const;   // milliseconds since epoch
    std::string envelope_hash()  const;   // BLAKE2b-256 of Frames 0/2/3
private:
    // ... implementation extracts and caches at construction
};

class RegAckBody {
public:
    explicit RegAckBody(const nlohmann::json& j);
    std::string status()              const;   // "success" | "error"
    std::string error_code()          const;   // empty on success
    std::string message()             const;
    std::string channel_name()        const;
    uint64_t    instance_id()         const;
    uint64_t    snapshot_version()    const;
    const nlohmann::json& heartbeat() const;
    const nlohmann::json& initial_allowlist() const;
    const nlohmann::json& broker_abi_fingerprint() const;
    std::string broker_build_id()     const;
    std::string broker_observer_pubkey_z85() const;
};

class ChannelAuthChangedNotifyBody {
public:
    explicit ChannelAuthChangedNotifyBody(const nlohmann::json& j);
    std::string channel_name()    const;
    std::string role_uid()        const;   // subject role
    std::string role_type()       const;   // subject role type
    std::string phase()           const;   // "admitted" | "live" | "left"
    uint64_t    channel_version() const;
};

class HeartbeatReqBody {
public:
    explicit HeartbeatReqBody(const nlohmann::json& j);
    std::string channel_name() const;   // often the only field
    std::string role_uid()     const;
};

// ... one class per msg_type, each with only the fields IT carries
```

**Wire skeleton class.**

```cpp
class WireEnvelope {
public:
    // Build the 5-frame envelope (4 skeleton + 1 body).  Sender-side
    // omits Frame 0 (ROUTER prepends identity on receive); ROUTER-side
    // includes it.  Stamps envelope_hash on the body per A10.
    template <typename BodyT>
    static zmq::multipart_t build(const std::string& identity,
                                    const std::string& msg_type,
                                    const std::string& correlation_id,
                                    BodyT              body);

    // Parse the 5-frame inbound envelope; validates envelope↔body
    // hash (A10) and rejects with std::nullopt + WARN on tamper.
    static std::optional<WireEnvelope> parse(zmq::multipart_t&& msg);

    std::string_view identity()       const;   // Frame 0
    std::string_view msg_type()       const;   // Frame 2
    std::string_view correlation_id() const;   // Frame 3

    // Typed body cast — throws on schema-shape mismatch or missing
    // required fields (per-body-class validation).
    template <typename BodyT>
    BodyT body_as() const;
};
```

**Handler dispatch pattern.**

```cpp
// Broker ROUTER poll loop:
auto env = WireEnvelope::parse(std::move(frames));
if (!env) { LOGGER_WARN(...); continue; }

// I-DEALER-IDENTITY check happens ONCE, right here, on the shared
// skeleton — not scattered inside each handler.  For REG_REQ /
// CONSUMER_REG_REQ, the identity check compares Frame 0 against
// the typed body's role_uid() accessor.  For other messages,
// role_uid may not be in the body — the identity check compares
// Frame 0 against the sender the broker already knows.

switch_on(env.msg_type()) {
    case "REG_REQ":                      handle_reg_req(env, env.body_as<RegReqBody>());
    case "CONSUMER_REG_REQ":              handle_consumer_reg_req(env, env.body_as<RegReqBody>());
    case "ENDPOINT_UPDATE_REQ":           handle_endpoint_update(env, env.body_as<EndpointUpdateReqBody>());
    case "HEARTBEAT_REQ":                 handle_heartbeat(env, env.body_as<HeartbeatReqBody>());
    ...
}
```

Every handler is:

```cpp
void handle_reg_req(const WireEnvelope& env, const RegReqBody& body) {
    // env.identity() → known-good role_uid (Frame 0, ROUTER-managed)
    // env.correlation_id() → for reply
    // body.role_uid() → self-declared; MUST equal env.identity() (I-DEALER-IDENTITY)
    // body.channel_name() / body.role_type() / body.zmq_pubkey() / ... → admission params
    // No JSON key-picking anywhere in the handler body.
}
```

**Invariant text (this becomes new draft §14).**

> **I-WIRE-ENVELOPE.**  Every control-plane message is 5 ZMQ
> frames: 4 skeleton frames (Frame 0 = identity, Frame 1 =
> `kFrameTypeControl`, Frame 2 = msg_type, Frame 3 =
> correlation_id) + 1 body frame (Frame 4).  Skeleton frames are
> identical shape across every REQ / ACK / NOTIFY.  Frame 4
> carries a msg_type-specific JSON body parsed via a per-msg-type
> typed body class with named accessors.  NO handler reads a wire
> field via JSON key extraction; every wire field is exposed as a
> typed accessor on either `WireEnvelope` (Frames 0/2/3) or a
> msg_type-specific body class (Frame 4 fields).  Adding a new
> msg_type means defining one new body class + registering it in
> the dispatch table; no scatter across handlers.

**Wire-enforcement point.**
- `WireEnvelope::build(identity, msg_type, correlation_id, body)`
  is the ONLY code path that constructs an outbound wire message.
- `WireEnvelope::parse(frames)` is the ONLY code path that
  consumes an inbound wire message.
- Every body class validates its expected fields at construction;
  malformed body → typed exception, caller replies with ERROR
  envelope.
- I-DEALER-IDENTITY check runs once in the ROUTER poll loop's
  post-parse hook for admission-family messages; other messages
  match Frame 0 against the sender's known-state binding.

### B2 — Per-msg-type dispatch on skeleton alone

**Consequence.**  Every routing / reject / correlate decision is
made on the 4 skeleton frames alone.  Frame 4 is parsed via the
typed body class only after dispatch.  Handlers become
uniformly-shaped:

```cpp
void handle_reg_req(const WireEnvelope& env, const RegReqBody& body,
                     /* broker context */)
{
    // Skeleton fields (via env):
    //   env.identity()        → Frame 0, ROUTER-managed role_uid
    //   env.correlation_id()  → Frame 3, for reply matching
    // Body fields (via typed accessors, no JSON key scatter):
    //   body.role_uid()       → self-declared; MUST == env.identity()
    //   body.channel_name()   → routing key
    //   body.role_type()      → admission-side determination
    //   body.zmq_pubkey()     → CURVE identity check
    //   body.schema_*()       → schema invariants
    //   body.client_nonce() / body.client_wall_ts() → anti-replay (A9)
    //   body.envelope_hash()  → envelope↔body binding (A10)
}
```

No `body.value("role_uid", "")` scatter.  No `body.value(
"channel_name", "")`.  Handlers cannot even TYPE those calls — the
typed body class doesn't expose a JSON-key accessor.  Adding a new
REQ type picks up the envelope discipline by shape, not by
copy-paste from an adjacent handler.

### B3 — Retirements this collapses

Every existing site that does:

```cpp
const std::string channel_name = req.value("channel_name", "");
const std::string role_uid     = req.value("role_uid",     "");
const std::string role_type    = req.value("role_type",    "");
const std::string phase        = req.details.value("phase", "");
const std::string corr_id      = req.value("correlation_id", "");
```

(dozens of such extractions in broker_service.cpp,
role_api_base.cpp, broker_request_comm.cpp) reduces to a single
`WireEnvelope& env` parameter and typed accessors.  The extraction
site scatter is exactly what the user called "toxic and fragile" —
the invariant sits in the envelope, and no handler re-implements
it.

### B4 — Body schema per msg_type (defined by the typed body class)

Every msg_type owns its own body class.  The body class is the
schema — the fields it exposes as accessors ARE the wire schema
for that msg_type.  No handler reads any field from JSON directly;
if the accessor doesn't exist, the field doesn't exist on that
msg_type.

Every REG-family body (any msg_type that mutates admission state)
carries the security triple `{client_nonce, client_wall_ts,
envelope_hash}` per A9 + A10.  Every body (REQ, ACK, NOTIFY)
carries `envelope_hash` since A10 binds every message.  For brevity
the field lists below note "+ security triple" or "+ envelope_hash
only" instead of repeating.

Frame 4 body content per msg_type:

- **RegReqBody** (REG_REQ, CONSUMER_REG_REQ): channel_name,
  role_uid, role_type, role_name, channel_topology, data_transport,
  zmq_pubkey, broker_proto, schema_hash, schema_version, schema_id,
  schema_blds, schema_owner, abi_fingerprint + security triple.
- **RegAckBody** (REG_ACK, CONSUMER_REG_ACK): status, error_code,
  message, channel_name, instance_id, snapshot_version, heartbeat,
  initial_allowlist, broker_abi_fingerprint, broker_build_id,
  broker_observer_pubkey_z85 + envelope_hash only.
- **ChannelAuthChangedNotifyBody**: channel_name, role_uid,
  role_type, phase, channel_version + envelope_hash only.
- **ChannelClosingNotifyBody**: channel_name, reason + envelope_hash
  only.
- **ConsumerDiedNotifyBody**: channel_name, role_uid, reason,
  target_role + envelope_hash only.
- **EndpointUpdateReqBody**: channel_name, endpoint_type, endpoint
  + security triple.
- **EndpointUpdateAckBody**: status, message + envelope_hash only.
- **GetChannelAuthReqBody**: channel_name, role_uid + envelope_hash
  only.  (Pure query, no state mutation — no nonce needed.)
- **GetChannelAuthAckBody**: status, allowlist, channel_version
  + envelope_hash only.
- **ChannelAuthAppliedReqBody**: channel_name, role_uid,
  applied_version, instance_id + security triple.
- **ChannelAuthAppliedAckBody**: status, confirmed_version
  + envelope_hash only.
- **HeartbeatReqBody**: channel_name, role_uid + envelope_hash
  only.  (Presence maintenance; no state mutation — nonce not
  required.  If future work adds admission side-effects to HB,
  promote to security triple.)
- **HeartbeatAckBody**: status + envelope_hash only.
- **DeregReqBody**: channel_name, role_uid + security triple.
  **DeregAckBody**: status + envelope_hash only.
- **DiscReqBody**: channel_name + envelope_hash only.
  **DiscAckBody**: status + discovery payload + envelope_hash only.
- **BandJoinNotifyBody / BandLeaveNotifyBody**: band, role_uid,
  role_name + envelope_hash only.
- **HubPeerHelloBody** (federation, out of scope for this cycle):
  hub_uid, hub_pubkey, ... + security triple (federation is a
  cross-hub trust boundary; anti-replay + envelope-binding apply
  symmetrically per G1).

Each class is small (5-15 fields typical).  Adding a new field
means adding one accessor to one class; no scatter, no missed
sites.

### B5 — Backward-incompatible wire change (transition is atomic)

The typed envelope is not additive over the current shape (which
is 3-frame `['C'][msg_type][json_body]`).  Every client + broker
ships the new envelope in one atomic version cut.  Coordinated
with:
- retire `CONSUMER_ATTACH_REQ_ZMQ` (main draft §3)
- retire scalar `data_endpoint` / `data_pubkey` (main draft §3)
- I-DEALER-IDENTITY enforcement (A1)

Post-cut, `broker_proto` version-gates the envelope shape.  Old
clients get an ERROR ACK with `error_code=UNSUPPORTED_PROTO`.

**Invariant text.**

> **I-WIRE-VERSION-ATOMIC.**  The wire envelope is not additive
> over pre-migration wire.  Every deployed component ships the
> envelope on the same version cut.  `broker_proto` on the REG
> envelope MUST match the current threshold; mismatch →
> `UNSUPPORTED_PROTO` reject.  There is no runtime tolerance for
> mixed-envelope deployments.

---

## 6. Category C — Implicit runtime magic that should be invariants

### C1 — NOTIFY delivery is best-effort under ROUTER default

**Statement.**  Under `ZMQ_ROUTER_MANDATORY=0` (libzmq default),
sending to an unknown or currently-disconnected identity silently
drops the message.  The draft §13.1.i notes this scenario ("NOTIFY
dropped") but as recovery scenario, not as invariant.

**Invariant text.**

> **I-NOTIFY-BEST-EFFORT.**  Fire-and-forget NOTIFYs are best-effort.
> `ZMQ_ROUTER_MANDATORY` is left at default (0), so a send to a
> disconnected identity is silently dropped.  Recovery paths:
> (a) next NOTIFY for the same channel_name pulls the current
> state; (b) pending-queue timeout releases the dialing peer with
> `CHANNEL_NOT_READY` which triggers client-side re-REG;
> (c) binding-side reconnect resyncs via REG_ACK.initial_allowlist.

**Wire-enforcement point.**  Not enforcement — documented behavior
that the pending-queue design already handles.

### C2 — Broker MUST NOT set ZMQ_ROUTER_MANDATORY

**Statement.**  If broker sets `ZMQ_ROUTER_MANDATORY=1`,
`send_to_identity` throws when target is disconnected.  Current
implementation must remain at default 0 for the pending-queue
model in §13.1.i to work.

**Invariant text.**

> **I-ROUTER-NOT-MANDATORY.**  Broker's CTRL ROUTER MUST NOT set
> `ZMQ_ROUTER_MANDATORY`.  The design relies on silent drops of
> sends to disconnected identities for transient reconnect windows;
> mandatory-mode would surface EHOSTUNREACH exceptions that the
> current code neither catches nor benefits from.

### C3 — Pending queue is transient across broker restart

**Statement.**  §13.7.e implies this but doesn't declare it.

**Invariant text.**

> **I-PENDING-EPHEMERAL.**  The broker's pending REG_REQ queue is
> in-memory and lost on broker restart.  Clients whose REG_REQ was
> pending re-REG on their next BRC reconnect; broker's fresh
> pending queue accepts them.  No persistence of pending state
> across restart.

---

## 7. Category D — Data-structure redundancy (invariant-derived duplicates)

### D1 — Retire `zmq_identity` fields (implied by A1)

Under I-DEALER-IDENTITY, the following fields hold copies of
role_uid:

- `pylabhub::hub::ConsumerEntry::zmq_identity`
- `pylabhub::hub::ProducerEntry::zmq_identity`
- `pylabhub::hub::BandMember::zmq_identity`

Retire.  Every call site `x.zmq_identity` becomes `x.role_uid`.
Every `send_to_identity(socket, x.zmq_identity, ...)` becomes
`send_to_identity(socket, x.role_uid, ...)`.  The
identity-capture line at broker_service.cpp:3445 (and its producer
+ band twins) becomes an ASSERT-then-drop: assert that
identity_frame bytes equal payload role_uid, then drop the local
identity_frame variable — never stored.

### D2 — `snapshot_version` on REG_ACK — KEEP (integrity witness)

`snapshot_version` on REG_ACK names the channel_version at the
moment `initial_allowlist` was captured.  Under I-STATE-MUTATION-
ATOMIC, the allowlist snapshot and its version bump happen under
one writer lock, so `snapshot_version = channel_version` at
snapshot time.

**Verdict: KEEP.**  Under the CURVE-authenticated / integrity-
critical frame, `snapshot_version` is an **integrity witness** for
the allowlist the client just received: it lets the client
cross-check subsequent CHANNEL_AUTH_CHANGED_NOTIFY /
GET_CHANNEL_AUTH_ACK versions against a known baseline captured at
admission time.  Without it, the client has no version reference
for the initial snapshot — a NOTIFY arriving with `version=N`
gives no information about whether the client's state is behind or
ahead of that baseline.  8 bytes; measurable security value; keep.

**No retirement.**

### D3 — REG_ACK `heartbeat` block — KEEP (broker-authoritative refresh)

Broker's tolerated heartbeat cadence is set at broker startup and
stable within one broker lifetime.  Every REG_ACK re-ships the
value.

**Verdict: KEEP.**  Under I-INSTANCE-ID-EPHEMERAL (broker restart
→ clients re-REG), re-shipping the cadence on every REG_ACK
closes a "broker restarted with new cadence" window without a
full HELLO renegotiation.  A role that reconnects mid-lifetime
after a broker restart picks up the current cadence on the very
first REG_ACK; the alternative (ship-once-at-HELLO) requires an
additional HELLO/HELLO_ACK exchange to refresh cadence, which is
strictly more moving parts for the same information.  Cost is
bytes; value is refresh discipline + one-round-trip admission.
Keep.

**No retirement.**

---

## 8. Category E — Sender-validation collapse

### E1 — Unify binding-side sender check

§2.3 (ENDPOINT_UPDATE_REQ), §2.5 (GET_CHANNEL_AUTH_REQ), §2.6
(CHANNEL_AUTH_APPLIED_REQ) all perform the same check: "sender's
identity is the binding-side role of the channel."  Currently
three separate handlers implement this with per-handler code
walking `consumers[]` or `producers[]` based on topology.

Under I-DEALER-IDENTITY + I-CHANNEL-SINGLE-BINDING-SIDE, the check
reduces to:

```
router_identity == ChannelEntry.binding_side_uid()
```

**Locked data-structure API** (per §13 decision #5).

```cpp
// On ChannelEntry:
std::optional<std::string> binding_side_uid() const;
    // Returns the single role_uid on the binding side per topology,
    // or nullopt if the channel is not yet opened (no binding-side
    // role admitted).

// On HubState:
bool is_binding_side_sender(const std::string& channel_name,
                             const std::string& router_identity) const;
    // shared_lock; single-lookup implementation.
```

Every handler that today walks the roster reduces to one call.
Bug-preventing: adding a new REQ that needs the same check picks
up the API by shape, not by copy-paste.

---

## 9. Category F — Undocumented policy

### F1 — Idempotency taxonomy

Not stated in draft.  Enumerate per REQ type:

| REQ | Idempotent? | Retry-safe? | Notes |
|---|---|---|---|
| REG_REQ / CONSUMER_REG_REQ | Yes (same uid+pubkey → same admission) | Yes | §7.2.4 duplicate handling |
| DEREG_REQ / CONSUMER_DEREG_REQ | Yes | Yes | Removes-if-present |
| ENDPOINT_UPDATE_REQ | Yes on same value; UPDATE-then-reject on different value | Yes on same value | §2.3 already states |
| GET_CHANNEL_AUTH_REQ | Yes (pure query) | Yes | |
| CHANNEL_AUTH_APPLIED_REQ | Yes (monotonic version advance) | Yes | |
| HEARTBEAT_REQ | Yes (updates last-seen) | Yes | |
| DISC_REQ | Yes | Yes | Pure query |

**Invariant text.**

> **I-REQ-IDEMPOTENT.**  Every control-plane REQ is idempotent
> under the `(role_uid, correlation_id)` key.  A duplicate REQ with
> the same key is either dropped as network-layer duplicate (broker
> already replied) or re-processed as a retry (broker still
> pending); in neither case does state mutation occur twice.
> Coupled with I-CORRELATION-STABLE + I-REPLAY-BOUND.

### F2 — Fire-and-forget vs request-reply shape taxonomy

HEP-CORE-0007 §12.2.1 has a shape contract (broker_request_comm.cpp
mentions §12.2.1 shape-conformance check).  Draft should reference
+ restate.

Fire-and-forget (NOTIFY family): CHANNEL_AUTH_CHANGED_NOTIFY,
CHANNEL_CLOSING_NOTIFY, CONSUMER_DIED_NOTIFY, BAND_JOIN_NOTIFY,
BAND_LEAVE_NOTIFY, HUB_PEER_HELLO / _BYE.  No ACK.  Best-effort
delivery per C1.

Request-reply (REQ/ACK): REG_REQ ↔ REG_ACK / ERROR, and every
other REQ.

**Invariant text.**

> **I-MSG-TYPE-TAXONOMY.**  msg_type suffix `_NOTIFY` ⇒ fire-and-
> forget (no ACK, best-effort per I-NOTIFY-BEST-EFFORT); suffix
> `_REQ` ⇒ request-reply requiring `_ACK` or `ERROR`.  Any
> deviation (e.g., a NOTIFY that expects an ACK) is a wire
> violation.  Enforced by BRC shape-conformance check per
> HEP-CORE-0007 §12.2.1.

### F3 — Retry policy per REQ type

Not stated.  Client-side retry semantics:

- BRC-level: REG_REQ retried on ACK timeout up to
  `client_brc_reg_ack_timeout` (I-BRC-BUDGET); ≥ broker
  pending_budget_ms.
- HEARTBEAT_REQ: not retried; missed HB → presence FSM handles.
- DEREG_REQ: retried on ACK timeout; idempotent so safe.
- ENDPOINT_UPDATE_REQ: retried on ACK timeout; idempotent for
  same value.
- GET_CHANNEL_AUTH_REQ / APPLIED_REQ: fired on NOTIFY receipt;
  retry-on-timeout is 1-2 attempts with backoff; then log-and-move-on
  (next NOTIFY retriggers).

F3 is not a single invariant — it's a per-REQ policy table.
Merged into the main draft as a doc block adjacent to §8.1
invariants; no single-line invariant statement fits.

### F4 — Wire-schema versioning gate (subsumed by I-WIRE-VERSION-ATOMIC)

§7.4 says atomic wire-retirement.  Under **I-WIRE-VERSION-ATOMIC**
(stated in B5), `broker_proto` on the REG envelope MUST match the
current threshold; mismatch → `UNSUPPORTED_PROTO` reject.  No
runtime tolerance for mixed-envelope deployments.

**No separate invariant needed** — I-WIRE-VERSION-ATOMIC covers
the wire-versioning gate.  `RegReqBody` exposes `broker_proto()`
accessor; broker rejects at envelope-parse time.

---

## 10. Category G — Symmetric coverage extensions

### G1 — Federation DEALER routing_id ≡ hub_uid

Main draft §11 excludes federation.  But I-DEALER-IDENTITY, if
stated only for role↔broker, leaves hub-to-hub DEALER connections
under the same collision bug (peer hubs coming up in fresh
processes with unseeded `rand()`).  Any HEP-CORE-0022 relay path
that uses ROUTER identity for send-back is broken by the same
mechanism.

**Invariant text (extends I-DEALER-IDENTITY).**

> **I-DEALER-IDENTITY (extended).**  For hub-to-hub federation
> DEALER↔ROUTER connections, the DEALER MUST set routing_id to
> the sending hub's `hub_uid`.  Same identity-capture rules apply
> at the receiving broker's ROUTER admission.  Failure to set
> collides across peer hubs identically to the role-side bug.

**Wire-enforcement point.**  Wherever peer-hub DEALERs are
initialized (HEP-CORE-0022 relay code path).  Out of scope for the
REG draft's implementation cycle but MUST be flagged as an
addendum-to-HEP-0022 dependency.

### G2 — Band routing (BAND_JOIN_NOTIFY dispatch)

`send_to_identity(m.zmq_identity, "BAND_JOIN_NOTIFY", ...)` at
broker_service.cpp:7174 uses BandMember::zmq_identity — cached
copy of the identity frame captured at band-join.  Same collision
mechanism.  A band with 3 members, all in fresh processes, would
have their notifies all misroute.

**Statement.**  Under I-DEALER-IDENTITY, `BandMember::zmq_identity`
retires (D1 already lists it).  BAND_JOIN_NOTIFY dispatch reduces
to `send_to_identity(m.role_uid, ...)`.

**Wire-enforcement point.**  BAND_JOIN handler validates
identity_frame == payload role_uid, same discipline as REG_REQ.

### G3 — Data plane already OK; addressing-primitive summary

Data-plane sockets use CURVE identity for admission (`curve_
publickey` on the connecting DEALER/SUB/PUSH; consumer/producer's
ZAP allowlist).  Under A3, curve_publickey ≡ role's zmq_pubkey.
The ROUTER-identity collision does not affect data plane because
data-plane sockets don't use ROUTER identity for addressing —
they use CURVE identity for authorization.

**Addressing-primitive summary (folded into main draft §5 state
model).**

> Data-plane addressing uses **CURVE pubkey** as the identity
> primitive; the ZAP allowlist on the binding side gates
> handshakes by that value.
> Control-plane addressing uses **ROUTER routing_id** as the
> identity primitive; unsolicited sends from broker to role
> address by that value.
> Both primitives MUST equal role_uid-mapped values:
> `routing_id ≡ role_uid` (per I-DEALER-IDENTITY) and
> `curve_publickey ≡ zmq_pubkey ≡ known_roles[role_uid].pubkey`
> (per I-CURVE-IS-DECLARED + I-PUBKEY-BINDING).

---

## 11. Retirements + additions implied

**New code artifacts (from B1..B5).**
- `WireEnvelope` class (utils/network_comm/wire_envelope.hpp):
  build / parse the 5-frame envelope (4 skeleton + 1 body); typed
  accessors for Frames 0/2/3 (`identity()`, `msg_type()`,
  `correlation_id()`); `body_as<BodyT>()` template returning the
  typed body class chosen by msg_type.  Single owner of frame
  layout.
- Per-msg-type typed body classes
  (utils/network_comm/wire_bodies.hpp): `RegReqBody`, `RegAckBody`,
  `ChannelAuthChangedNotifyBody`, `ChannelClosingNotifyBody`,
  `ConsumerDiedNotifyBody`, `EndpointUpdateReqBody`,
  `EndpointUpdateAckBody`, `GetChannelAuthReqBody`,
  `GetChannelAuthAckBody`, `ChannelAuthAppliedReqBody`,
  `ChannelAuthAppliedAckBody`, `HeartbeatReqBody`,
  `HeartbeatAckBody`, `DeregReqBody`, `DeregAckBody`,
  `DiscReqBody`, `DiscAckBody`, `BandJoinNotifyBody`,
  `BandLeaveNotifyBody`.  Each is the single owner of its
  msg_type's Frame 4 schema.
- Every REG-family body class also exposes `client_nonce()`,
  `client_wall_ts()`, `envelope_hash()` accessors per A9/A10.

**HubState field retirements (from D1 + G2).**
- `ConsumerEntry::zmq_identity`
- `ProducerEntry::zmq_identity`
- `BandMember::zmq_identity`

**HubState API additions (from A4 + E1).**
- `ChannelEntry::binding_side_uid() -> std::optional<std::string>`
- `HubState::is_binding_side_sender(channel, uid) -> bool`
- `HubState::nonce_seen(role_uid, nonce, wall_ts) -> bool`
  (A9 sliding-window dedup).

**Broker admission-gate additions (from A1, A2, A6, A9, A10, A11).**
- Identity-vs-body check at every REG-family handler entry:
  `env.identity() == body.role_uid()` or `INVALID_REQUEST
  error_code=IDENTITY_MISMATCH` (A1).
- Known-roles reverse-uniqueness check at broker startup:
  no pubkey appears under multiple role_uids (A2).
- Correlation-ID enforcement — envelope schema mandates it; empty
  correlation_id on REQ is rejected at envelope parse, not
  per-handler (A6).
- Anti-replay: nonce dedup + wall-clock skew reject at REG-family
  admission before any state mutation (A9); `REPLAY_OR_SKEW` error.
- Envelope↔body hash validation at `WireEnvelope::parse`;
  `ENVELOPE_TAMPERED` on mismatch (A10).
- Key-rotation gate: REG with pubkey ≠ `known_roles` current entry
  rejected with `KEY_ROTATION_REQUIRES_DEREG` (A11).

**Handler-level retirements (from B3).**
- Every `req.value("channel_name", "")` / `body.value("role_uid",
  "")` / `body.value("phase", "")` / `body.value("correlation_id",
  "")` / `body.value("role_type", "")` extraction across the
  broker + role + BRC codebase.  Replace with `env.identity()` /
  `env.correlation_id()` (skeleton fields) or
  `body.role_uid()` / `body.channel_name()` / `body.phase()` /
  `body.role_type()` (typed body accessors).  Approximate site
  count (from `req.value("channel_name"` alone): dozens across
  broker_service.cpp, role_api_base.cpp.

**BRC-side additions (from A1, A6, A9, A10, B1).**
- `set(zmq::sockopt::routing_id, cfg.role_uid)` before
  `dealer->connect()`.  Refuse-connect if cfg.role_uid empty
  (loud, parallel to broker_pubkey empty check).
- ACK matching keyed on `(msg_type, correlation_id)` from
  `WireEnvelope`; not from ad-hoc body extraction.
- Send-path builds envelope via `WireEnvelope::build`; receive-path
  parses via `WireEnvelope::parse` (validates envelope↔body hash).
- Every REQ constructor stamps `client_nonce` (16 random bytes)
  + `client_wall_ts` on the body before build (A9).

**Main draft body edits (§8.1 expansion).**  Add I-DEALER-IDENTITY,
I-PUBKEY-BINDING, I-CURVE-IS-DECLARED, I-CHANNEL-SINGLE-BINDING-SIDE,
I-INSTANCE-ID-EPHEMERAL, I-CORRELATION-STABLE, I-STATE-MUTATION-ATOMIC,
I-REPLAY-BOUND, I-ENVELOPE-BODY-BINDING, I-KEY-ROTATION-VIA-DEREG,
I-WIRE-ENVELOPE, I-WIRE-VERSION-ATOMIC, I-NOTIFY-BEST-EFFORT,
I-ROUTER-NOT-MANDATORY, I-PENDING-EPHEMERAL, I-REQ-IDEMPOTENT,
I-MSG-TYPE-TAXONOMY.  Seventeen new invariants (A7 subsumed by B1
does not add a name); existing four (I-ROUTER-SERIAL, I-OPT-ADMIT,
I-BRC-BUDGET, I-MONOTONIC-VERSION) remain.  **Total: 21.**

**Main draft new §14 — Typed wire envelope contract.**  Frame-by-
frame layout (4 skeleton + 1 body); per-msg-type typed body class
catalog; encoding rules; forbidden shapes.  Referenced by every
message shape in §2.

**Main draft §2 rewrite.**  Each message shape presented as
(a) msg_type string (Frame 2), (b) typed body class (Frame 4
schema via named accessors), (c) admission-step usage.  NO
per-message wire-frame diagrams — every message shares the
skeleton frames.

---

## 12. Sequencing to lock the draft + implement

Preferred order — invariants first, code second, tests third, per
the discipline "correct protocol compliance by design."

**Phase 1 — Draft merge (documentation only, no code).**
1. Adopt findings A1..A11 (A7 subsumed by B1), B1..B5, C1..C3,
   D1..D3, E1, F1..F4, G1..G3 into the main draft as new §8.1
   invariants + new §14 wire-envelope contract + §2 shape rewrites.
2. Re-walk §13 scenarios against expanded invariants to verify no
   scenario contradicts new invariant.
3. Cross-check HEP-CORE-0007 §12, HEP-CORE-0036 §5b/§6.5,
   HEP-CORE-0017 §4.7 for drift.  Mark sections needing merge-in.
4. Lock the expanded draft.

**Phase 2 — Code (typed envelope + broker + BRC + HubState atomic
commit).**
1. `WireEnvelope` class: define build/parse for the 5-frame envelope
   (4 skeleton + 1 body).  Skeleton accessors (`identity()`,
   `msg_type()`, `correlation_id()`) + `body_as<BodyT>()`
   template.  `parse()` validates envelope↔body hash (A10 —
   `ENVELOPE_TAMPERED` on mismatch).  Single owner of frame
   layout.  Unit-tested at L1.
2. Per-msg-type typed body classes
   (utils/network_comm/wire_bodies.hpp): one class per msg_type
   with named accessors ONLY for the fields IT carries; validate
   required fields at construction.  Every REG-family body class
   exposes `client_nonce()`, `client_wall_ts()`, `envelope_hash()`.
   No JSON key extraction anywhere else in the codebase.  Unit-
   tested at L1.
3. BRC: rewire send + recv paths to use `WireEnvelope::build` /
   `parse`.  Set DEALER routing_id from Config::role_uid;
   refuse-connect on empty (A1).  ACK matching via
   `(msg_type, correlation_id)` from envelope (A6).  Every REQ
   constructor stamps `client_nonce` (16 random bytes) +
   `client_wall_ts` on the body (A9).
4. Broker: rewire ROUTER poll-loop's frame handling to parse via
   `WireEnvelope`.  Dispatch on `env.msg_type()`.
5. Handler-by-handler sweep: replace every `req.value("channel_
   name")` / `body.value("role_uid")` / etc. with typed accessors
   on either `WireEnvelope` (skeleton) or the msg_type body class.
   Every handler signature becomes
   `handle_XXX(const WireEnvelope& env, const XxxBody& body, ...)`.
   Mass retirement of scattered JSON key extraction.
6. Broker REG handlers: identity-vs-body check at every REG entry
   (A1); nonce dedup + wall-clock skew reject (A9); key-rotation
   gate (A11).  All three run BEFORE any state mutation.
7. Broker BAND handler: identity check + retire BandMember::
   zmq_identity (A1 + D1 + G2).
8. HubState: retire zmq_identity fields on ConsumerEntry,
   ProducerEntry, BandMember (D1).
9. HubState: add ChannelEntry::binding_side_uid() +
   HubState::is_binding_side_sender() (A4 + E1) +
   HubState::nonce_seen() (A9 sliding-window dedup).
10. Broker ENDPOINT_UPDATE_REQ + GET_CHANNEL_AUTH_REQ +
    CHANNEL_AUTH_APPLIED_REQ handlers: replace bespoke checks
    with is_binding_side_sender().
11. Every `send_to_identity(x.zmq_identity, ...)` → `WireEnvelope::
    build(x.role_uid, ...)` + ROUTER send.  Retire `send_to_identity`
    helper — subsumed by envelope send.
12. Broker startup: known_roles reverse-uniqueness check (A2) —
    reject config with duplicate pubkeys before opening ROUTER.
13. Wire schema doc updates (HEP-CORE-0007 §12, HEP-CORE-0036).

**Phase 3 — Tests.**
1. L2 test: BRC's DEALER routing_id equals Config::role_uid.
2. L2 test: broker rejects REG_REQ where identity_frame_bytes ≠
   body.role_uid() with `INVALID_REQUEST error_code=
   IDENTITY_MISMATCH` (A1).
3. L2 test: `WireEnvelope::parse` rejects envelope with tampered
   body hash — `ENVELOPE_TAMPERED` (A10 splice defense).
4. L2 test: broker rejects replay of captured REG_REQ (same
   client_nonce within window) with `REPLAY_OR_SKEW` (A9 nonce
   dedup).
5. L2 test: broker rejects REG_REQ with wall_ts skewed beyond 30s
   from broker_wall_ts — `REPLAY_OR_SKEW` (A9 skew).
6. L2 test: broker rejects in-band key rotation attempt —
   `KEY_ROTATION_REQUIRES_DEREG` (A11).
7. L3 test: broker startup rejects known_roles config where two
   entries share the same pubkey (A2 reverse-uniqueness).
8. L4 test: fan-in end-to-end (currently task #95) — must pass
   without any per-role identity workaround.
9. L4 test: two roles with same role_uid (misconfig) — second one
   rejected at REG or replaces first (which behavior is
   correct-by-design — decide + pin).
10. L4 test: BAND_JOIN with 3 members — each member receives its
    own NOTIFY, not a duplicate cross-wired to another member.
11. L4 test: DEREG + known_roles pubkey update + re-REG succeeds
    (A11 operator key-rotation procedure).

**Phase 4 — Federation follow-on.**
G1 flagged; HEP-CORE-0022 addendum tracks the same-shape fix for
hub-to-hub DEALER routing_id.  Out of scope for this draft's
implementation cycle.

---

## 13. Decisions locked under CURVE-authenticated frame

pylabhub is a CURVE-encrypted, wire-authenticated, integrity-
critical system.  Under that frame every question of the form
"hard-enforce X or soft-tolerate?" has one answer: hard-enforce.
Grace periods on identity / correlation / integrity checks are
documented attack windows.  Mixed old/new deployments break the
security invariants for the duration of the mix.  Two sources of
truth for identity is a spoofing surface.

The following decisions are locked; Phase 2 code work executes
against them without further re-litigation.

1. **Identity mismatch: hard reject from day one.**  There is no
   legitimate old client to be soft on — we're transitioning FROM
   a state where identity was undefined, TO one where it's
   authoritative.  `INVALID_REQUEST error_code=IDENTITY_MISMATCH`
   at REG admission.
2. **Atomic wire cut.**  Every deployed component ships the typed
   envelope + I-DEALER-IDENTITY + retirement of cached
   `zmq_identity` + retirement of scalar `data_endpoint/data_pubkey`
   in one commit + one `broker_proto` version bump.  Old-shape
   REQ rejected with `UNSUPPORTED_PROTO`.
3. **Retire cached `zmq_identity` on ConsumerEntry / ProducerEntry
   / BandMember.**  Duplicates of `role_uid` under I-DEALER-IDENTITY;
   two sources of truth is a spoofing surface.  Every call site
   rewritten to use `role_uid`.
4. **`correlation_id` required from day one.**  Broker rejects
   empty at envelope parse; no grace-period generation.
   Anti-cross-wire + anti-replay depend on it.
5. **Unify `is_binding_side_sender`.**  One shared primitive on
   HubState used by ENDPOINT_UPDATE_REQ + GET_CHANNEL_AUTH_REQ +
   CHANNEL_AUTH_APPLIED_REQ.  Three separate implementations of a
   security-critical gate is three chances to drift.
6. **Federation in scope.**  Hub-to-hub is a trust boundary
   between hubs; identity collision means peer hubs spoof each
   other.  I-DEALER-IDENTITY extended to hub_uid for federation
   DEALERs (G1).  HEP-CORE-0022 updated in the same cycle.
7. **Keep `snapshot_version` on REG_ACK.**  Integrity witness for
   the allowlist snapshot; lets the client cross-check subsequent
   NOTIFY versions against a known baseline.  D2 verdict: KEEP.
8. **Keep `heartbeat` block on every REG_ACK.**  Broker-
   authoritative refresh discipline; closes the "broker restarted
   with new cadence" window without a separate HELLO
   renegotiation.  D3 verdict: KEEP.
9. **Typed body per msg_type.**  Different msg_types are different
   wire schemas; the code models each with a distinct C++ class
   whose named accessors ARE the schema.  Adding a new msg_type
   = one new body class + one dispatch-table entry.

**Additional security invariants surfaced under the frame** (A9
anti-replay, A10 envelope↔body binding, A11 key-rotation-via-DEREG
only) locked into §4 above.  CURVE encrypts frames; it does NOT
prevent replay of captured messages, splicing of body+envelope
from different messages, or in-band identity swap after process
compromise.  All three attack surfaces closed at wire-admission.

**Nothing further required from the user before Phase 1 merge.**
Task #96 (merge addendum) proceeds against these locked decisions.
