# HEP-CORE-0035: Hub-Role Authentication and Federation Trust

| Property        | Value                                                                                              |
|-----------------|----------------------------------------------------------------------------------------------------|
| **HEP**         | `HEP-CORE-0035`                                                                                    |
| **Title**       | Hub-Role Authentication and Federation Trust                                                       |
| **Status**      | 🚧 **NOT IMPLEMENTED — TODO NEXT.** Authoritative design; supersedes the legacy `ConnectionPolicy` placeholder documented in HEP-CORE-0009 §2.7. |
| **Created**     | 2026-04-29                                                                                         |
| **Area**        | Framework Architecture (`BrokerService` socket layer, `HubConfig`, `BrokerService::Config`, federation) |
| **Depends on**  | HEP-CORE-0022 (Federation), HEP-CORE-0024 (Role Directory), HEP-CORE-0033 (Hub Character)          |
| **Supersedes**  | HEP-CORE-0009 §2.7 (`ConnectionPolicy` reference) — see §6                                         |
| **Blocks**      | HEP-CORE-0033 §15 Phase 1 re-introduction of `broker.{known_roles, default_channel_policy, channel_policies}` in `HubBrokerConfig` |

---

## 1. Status banner

**This HEP is the design contract — no part of it is implemented.** The
existing code (`channel_access_policy.hpp::ConnectionPolicy`,
`BrokerServiceImpl::check_connection_policy`,
`BrokerService::Config::{connection_policy, known_roles, channel_policies}`)
is a **placeholder** that pre-dates the CURVE-required model and the
HEP-CORE-0022 federation model. It currently does string-matching on JSON
identity fields without consulting any pubkey, has no ZAP handler attached
to the broker ROUTER, and has no federation-trust path. None of that is
load-bearing for the security story this HEP describes.

When HEP-0035 lands, the legacy machinery is retired in one sweep:
the placeholder enum, the placeholder gate, and the legacy hub.json
fields all go. Until then, the legacy machinery remains live (L3 test
`test_datahub_channel_access_policy.cpp` still exercises it) but is not
the authentication story.

---

## 2. Invariants (the architectural decision being formalized)

These were ratified prior to this HEP (decision logged 2026-04-29
during Phase 1 review of `HubBrokerConfig`):

- **CURVE is required.** Every role↔hub interaction is encrypted +
  authenticated by ZMQ CURVE. NULL-mech connections to the broker
  ROUTER are not supported in any deployment except dev-mode-with-loopback.
- **Mutual pubkey knowledge is an invariant.** Every role knows its
  hub's pubkey (via `<role_dir>/<direction>_hub_dir/hub.json` →
  `network.broker_endpoint` + the hub's pubkey distributed alongside).
  Every hub knows its roles' pubkeys (via `broker.known_roles[].pubkey`
  in hub.json).
- **Identity is the pubkey, not the JSON-asserted name.** A role's
  `role_name` + `role_uid` strings in REG_REQ are *labels*, not *proofs*.
  The pubkey at the CURVE handshake is the proof.
- **Federation peers are mutually CURVE-trusted hubs.** A federation peer
  entry (`hub.json::federation.peers[]`) carries the peer hub's pubkey;
  CURVE handshake at federation socket establishment verifies the peer.
- **Federation does not transitively expose every peer's roles.** Cross-hub
  role acceptance is a per-hub policy decision (§4), not an automatic
  consequence of peering.

---

## 3. Current state — gap analysis

This section catalogs what the code actually does today so that future
implementers don't read the placeholder as authoritative design.

### 3.1 Broker ROUTER — no pubkey allowlist

`BrokerServiceImpl::run_loop` (`src/utils/ipc/broker_service.cpp:434-439`)
configures the ROUTER as a CURVE server:

```cpp
if (cfg.use_curve)
{
    router.set(zmq::sockopt::curve_server, 1);
    router.set(zmq::sockopt::curve_secretkey, server_secret_z85);
    router.set(zmq::sockopt::curve_publickey, server_public_z85);
}
```

There is **no ZAP handler** attached. ZMQ accepts any cryptographically
valid CURVE handshake — i.e., any client that knows *any* secret key
matching *any* pubkey. The broker has no opportunity to reject a
handshake based on the client's pubkey before the application layer
runs. `known_roles[].pubkey` is parsed but never consulted at the
CURVE layer.

**Consequence**: today, the CURVE-required invariant (§2) is enforced
*encryption-wise* but not *authorization-wise*. Anyone with broker
network access and the broker's pubkey can complete a handshake and
issue REG_REQ.

### 3.2 Application gate — string match, no provenance

`BrokerServiceImpl::check_connection_policy` (`broker_service.cpp:2118-2170`)
runs at the REG_REQ / CONSUMER_REG_REQ handler. The four levels:

| Mode      | What it checks                                                                |
|-----------|-------------------------------------------------------------------------------|
| `Open`    | Nothing.                                                                      |
| `Tracked` | Nothing; logs identity if present.                                            |
| `Required`| `role_name` + `role_uid` are non-empty in the JSON.                           |
| `Verified`| `(role_name, role_uid)` matches an entry in `cfg.known_roles[]` (string match)|

All four operate on **self-asserted JSON fields** the role itself put in
the request body. None inspects the connecting socket's CURVE pubkey. None
consults `cfg.peers` for federation provenance. `KnownRole::pubkey_z85`
is parsed but never read.

### 3.3 Federation socket — string-based peer rejection

`BrokerService` rejects inbound HUB_PEER_HELLO messages whose `hub_uid`
is not in `cfg.peers` (`broker_service.cpp:3239-3254`). Same pattern:
self-asserted string match, not pubkey check.

### 3.4 Net effect

The mechanism the operator configures (`ConnectionPolicy` + `known_roles[].pubkey`)
*looks* like an auth design but the runtime never actually uses pubkeys
for access decisions. This HEP's job is to close that gap.

---

## 4. Design

### 4.1 Layered enforcement

Two distinct layers cooperate. They are independent and each is
mandatory in production:

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 1 — ZAP authentication (broker socket layer)             │
│                                                                 │
│  Broker ROUTER attaches a ZAP handler that consults             │
│  HubState's role + peer pubkey index (built from hub.json's     │
│  broker.known_roles[] and federation.peers[]).                  │
│                                                                 │
│  • Handshake from a known role pubkey  → accept.                │
│  • Handshake from a known peer hub pubkey → accept (this is     │
│    a federation-peer DEALER connecting in).                     │
│  • Handshake from any other pubkey → reject at ZAP. The         │
│    application dispatcher never sees the message.               │
│                                                                 │
│  Effect: layer 1 enforces "every connecting socket belongs to   │
│  someone the operator has explicitly listed."                   │
└─────────────────────────────────────────────────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────┐
│  Layer 2 — Federation-trust gate (broker registration layer)    │
│                                                                 │
│  REG_REQ / CONSUMER_REG_REQ handlers consult the registering    │
│  socket's CURVE pubkey (recoverable via `zmq_msg_gets("User-Id")│
│   from the routing frame after CURVE auth).                     │
│                                                                 │
│  • If the pubkey matches a local known_roles entry → local      │
│    role; accept.                                                │
│  • If the pubkey matches a federation peer's hub pubkey →       │
│    apply the hub's federation-trust policy (§4.3).              │
│  • Otherwise → reject (this case shouldn't be reachable if      │
│    Layer 1 is enforcing, but defense in depth).                 │
│                                                                 │
│  Layer 2 is where federation-delegated trust is decided.        │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 Pubkey index — single source of truth

`HubState` (HEP-0033 §8) gains an index built at config-load time and
maintained by capability ops:

```cpp
// HEP-CORE-0035 — pubkey provenance index.
struct PubkeyOrigin
{
    enum class Kind { LocalRole, FederationPeer };
    Kind        kind;
    std::string subject_uid;     // role uid OR peer hub uid
    std::string subject_name;    // for diagnostics
};

std::unordered_map<std::string, PubkeyOrigin>  pubkey_to_origin;
```

Both the ZAP handler (Layer 1) and the federation-trust gate (Layer 2)
read from this single index. There is exactly one structure that
answers "what does this pubkey mean to this hub."

### 4.3 Federation-trust policy modes

The hub.json field is `broker.federation_trust_mode` — *not* the legacy
`default_channel_policy`. Naming reflects what it actually controls.

| Mode             | Layer-2 rule for a connecting role with peer-pubkey origin                              |
|------------------|------------------------------------------------------------------------------------------|
| `local_only`     | Reject every cross-hub role registration. Federation peers may only relay broadcasts (HEP-0022). |
| `peer_delegated` | Accept any role registration arriving via a known peer's pubkey, provided the peer's `hub.json` itself listed that role (proven via `HUB_PEER_HELLO` augmenting the peer's role list — see §4.4). |
| `peer_announced` | Accept any role registration arriving via a known peer's pubkey, no further check; treat the peer's pubkey as transitive trust. (Less strict; intended for tightly-coupled hub clusters.) |

`local_only` is the default — federations are explicit opt-in for cross-hub
role acceptance.

### 4.4 Augmenting HEP-0022 peer handshake

To support `peer_delegated`, the federation handshake (HEP-0022 §6.1
HUB_PEER_HELLO) is extended with an optional `roles[]` array carrying
the peer's `(role_uid, role_pubkey)` list. The receiving hub stores
this in `PeerEntry.delegated_roles` and consults it at Layer 2.

`peer_announced` does not need this extension — it accepts on peer-pubkey
alone.

### 4.5 Removed concepts

The following from the legacy placeholder are **dropped, not renamed**:

- `ConnectionPolicy::Required` — string-presence check; with CURVE+ZAP,
  the connecting socket's pubkey is mandatory and verified, so a
  separate "did you put a name in your JSON" gate adds nothing.
- `ConnectionPolicy::Verified` — string allowlist; redundant with ZAP
  pubkey allowlist.
- `ConnectionPolicy::Tracked` / `Open` — operator-observability concerns
  belong in audit logging, not in the gate enum.
- `ChannelPolicy` per-channel glob overrides — no use case has been
  identified that the per-role + per-peer policy can't address. Revisit
  only if a concrete scenario emerges.

---

## 5. hub.json fields when HEP-0035 lands

```json
"broker": {
  "heartbeat_timeout_ms": 15000,
  "heartbeat_multiplier": 5,
  "federation_trust_mode": "local_only",  // local_only | peer_delegated | peer_announced
  "known_roles": [
    {
      "uid":    "prod.cam.uid01234567",
      "name":   "Camera",
      "pubkey": "<Z85 40-char>"           // REQUIRED — Layer-1 allowlist key
    }
  ]
}
```

`known_roles[].pubkey` becomes **required** (empty string is not allowed).
A role without a configured pubkey cannot connect once HEP-0035 ships;
the operator must run `plh_role --keygen` (HEP-0024 §11) and paste the
resulting pubkey into the hub's `known_roles` entry.

---

## 6. Cleanup of existing HEPs

When HEP-0035 ships:

- **HEP-CORE-0009 §2.7** is fully retracted. The replacement section is
  a one-paragraph stub pointing to HEP-0035 as the authoritative source.
  Content lives here, in one place. Until HEP-0035 ships, HEP-0009 §2.7
  carries a "🚧 superseded by HEP-0035 (in design)" banner.
- **HEP-CORE-0022 §6.1** (HUB_PEER_HELLO) gains the optional `roles[]`
  field per §4.4, with a forward-reference to HEP-0035 for semantics.
- **HEP-CORE-0033 §6.4** broker sub-config description drops
  `policy/known_roles` mentions; the hub.json example in §6.2 drops
  `default_channel_policy` and shows `known_roles[].pubkey` as required;
  §15 Phase 1 marks the auth fields as deferred to HEP-0035.

---

## 7. Open questions (resolve before implementation)

1. **Pubkey rotation semantics.** When an operator rotates a role's
   keypair, hub.json is hot-reloaded (HEP-0033 §6.5 vault). What's the
   transition period? Allow both old + new pubkey for a grace window,
   or hard-cut?
2. **ZAP handler lifetime + threading.** ZAP handlers run on a separate
   inproc socket bound to `inproc://zeromq.zap.01`. Decide: hub-owned
   thread, or share with broker poll loop?
3. **`peer_delegated` consistency.** If Hub-A and Hub-B both have role
   X in their respective `known_roles` (with different pubkeys —
   misconfiguration), what does Hub-A do when Hub-B's HELLO advertises
   X? Reject the HELLO, accept Hub-B's view, or warn-and-prefer-local?
4. **Audit log requirements.** Which Layer-1/Layer-2 decisions get
   logged at what level? Per-handshake INFO is too noisy in dev; rejection
   needs WARN with enough context to diagnose.
5. **Dev-mode escape hatch.** Today `cfg.use_curve = false` disables CURVE
   entirely. With HEP-0035, dev-mode should remain "no auth at all" but
   *only* on loopback endpoints — should this be enforced at config-parse
   time (reject `tcp://0.0.0.0:*` if `use_curve=false`)?

---

## 8. Implementation phases (when work begins)

| Phase | Scope                                                                        |
|-------|------------------------------------------------------------------------------|
| 1     | ZAP handler skeleton; pubkey index in `HubState`; reject-all-by-default mode |
| 2     | Layer-1 enforcement against `known_roles[].pubkey`                           |
| 3     | Re-add `broker.known_roles[]` to `HubBrokerConfig` with `pubkey` required    |
| 4     | Layer-2 federation-trust gate; `federation_trust_mode` field                 |
| 5     | HEP-0022 HUB_PEER_HELLO `roles[]` augmentation; `peer_delegated` support     |
| 6     | Cleanup: delete `ConnectionPolicy` enum, `check_connection_policy`, `KnownRole`, `ChannelPolicy`, L3 test that exercises them |
| 7     | HEP-0009 §2.7 retraction; HEP-0022 §6.1 update; HEP-0033 §6.2/§6.4/§15 cleanup |

---

## 9. References

- HEP-CORE-0009 §2.7 — legacy `ConnectionPolicy` (superseded by this HEP).
- HEP-CORE-0022 — Hub Federation (peer handshake, federation peer config).
- HEP-CORE-0024 §11 — Role keygen + vault (source of `known_roles[].pubkey`).
- HEP-CORE-0033 §6 — Hub config (where `broker.known_roles` lives).
- HEP-CORE-0033 §8 — `HubState` (where the pubkey index lives).
- ZeroMQ ZAP RFC 27 — http://rfc.zeromq.org/spec:27/ZAP
- ZeroMQ CURVE security — http://api.zeromq.org/master:zmq-curve
