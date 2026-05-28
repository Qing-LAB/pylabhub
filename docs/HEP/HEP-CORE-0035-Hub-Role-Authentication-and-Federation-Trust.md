# HEP-CORE-0035: Hub-Role Authentication and Federation Trust

| Property        | Value                                                                                              |
|-----------------|----------------------------------------------------------------------------------------------------|
| **HEP**         | `HEP-CORE-0035`                                                                                    |
| **Title**       | Hub-Role Authentication and Federation Trust                                                       |
| **Status**      | 🚧 **NOT IMPLEMENTED — TODO NEXT.** Authoritative design; supersedes the legacy `RoleIdentityPolicy` placeholder documented in HEP-CORE-0009 §2.7. |
| **Created**     | 2026-04-29                                                                                         |
| **Area**        | Framework Architecture (`BrokerService` socket layer, `HubConfig`, `BrokerService::Config`, federation) |
| **Depends on**  | HEP-CORE-0022 (Federation), HEP-CORE-0024 (Role Directory), HEP-CORE-0033 (Hub Character)          |
| **Supersedes**  | HEP-CORE-0009 §2.7 (`RoleIdentityPolicy` reference) — see §6                                         |
| **Blocks**      | HEP-CORE-0033 §15 Phase 1 re-introduction of `broker.{known_roles, role_identity_policy, channel_policy_overrides}` in `HubBrokerConfig` |

---

## 1. Status banner

**This HEP is the design contract — no part of it is implemented.** The
existing code (`role_identity_policy.hpp::RoleIdentityPolicy`,
`BrokerServiceImpl::check_role_identity`,
`BrokerService::Config::{role_identity_policy, known_roles, channel_policy_overrides}`)
is a **placeholder** that pre-dates the CURVE-required model and the
HEP-CORE-0022 federation model. It currently does string-matching on JSON
identity fields without consulting any pubkey, has no ZAP handler attached
to the broker ROUTER, and has no federation-trust path. None of that is
load-bearing for the security story this HEP describes.

When HEP-0035 lands, the legacy machinery is retired in one sweep:
the placeholder enum, the placeholder gate, and the legacy hub.json
fields all go. Until then, the legacy machinery remains live (L3 test
`test_datahub_role_identity_policy.cpp` still exercises it) but is not
the authentication story.

---

## 1.5. What the placeholder really is (addendum, 2026-05-13)

> Added 2026-05-13 in response to "what is this *channel access*
> policy really doing?"  The placeholder code was named for its
> historical role at REG_REQ time and for the per-channel glob
> override list, but neither label captures the subject of
> verification.  This section grounds the placeholder in current
> code, so a future reader / implementer knows precisely what is
> being retired when this HEP lands.

### 1.5.1. Subject, action, and selector

The placeholder is a **role-identity verification policy applied at
broker registration time**:

- **Subject:** the role's self-asserted identity strings `role_name`
  + `role_uid` placed in the REG_REQ / CONSUMER_REG_REQ body by the
  registering role itself.
- **Action gated:** the broker's acceptance of a channel registration
  (`BrokerServiceImpl::check_role_identity` is called from
  `handle_reg_req` and the consumer-registration handler; see
  `src/utils/ipc/broker_service.cpp` `check_role_identity` definition
  and call sites).
- **Selector:** the channel name being registered — used only to look
  up which strictness mode applies (`channel_policy_overrides[]`
  list, first-glob-match wins, falling back to the hub-wide default).

The legacy `ConnectionPolicy` / "channel access policy" names
misdescribed the subject in two ways: (a) the gate has nothing to do
with the ZMQ connection layer (CURVE handles handshake; this gate
runs at the *application* protocol layer after a message arrives);
(b) "channel access" suggested the channel itself is being gated,
when the channel is the lookup key for *which role-identity rule*
applies.  Renamed to `RoleIdentityPolicy` + `ChannelPolicyOverride`
on 2026-05-13 to reflect this.

### 1.5.2. The four configuration surfaces involved

A future implementer touching this gate will navigate four surfaces.
Their relationship today (post-rename, pre-HEP-0035):

| Surface                       | Owns                                                                                                                                            | Today's wiring to the gate                                                                                                                              |
|-------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Role directory + RoleConfig** (per-role JSON parsed by `pylabhub::config::RoleConfig`) | `IdentityConfig{uid, name}` for the role binary                                                                                                | Role copies `identity().uid` / `identity().name` into `BrokerRequestComm::Config` → REG_REQ body (`role_uid` / `role_name`).  This is what the gate reads. |
| **Hub directory + HubConfig** (per-hub JSON parsed by `pylabhub::config::HubConfig` → `HubBrokerConfig`)                                       | Broker endpoint, heartbeat fields, checksum policy, federation peers                                                                            | **Deliberately omits** `role_identity_policy`, `known_roles`, `channel_policy_overrides` per `hub_broker_config.hpp:13-15`.  The gate's config fields therefore cannot be set from hub.json today. |
| **`BrokerService::Config`** (C++ struct constructed by `HubHost`)                                                                              | `role_identity_policy` (default `Open`), `known_roles`, `channel_policy_overrides` — the gate's actual inputs                                   | Set to defaults by `HubHost` (because `HubConfig` doesn't carry these).  **Only callers that construct `BrokerService::Config` directly** (i.e., this HEP's L3 test) can set non-default values. |
| **`HubState`** (in-memory state owned by `HubHost`)                                                                                            | Channel registry, schema registry, producer/consumer entries with their CURVE pubkeys, federation peers                                         | Today: holds CURVE pubkeys on entries but has **no pubkey-provenance index** — the `PubkeyOrigin` index this HEP designs (§4.2) is unimplemented.  Gate does **not** consult HubState. |

Net consequence today:
1. The check function runs at every REG_REQ in production.
2. It always evaluates `policy == Open` (because nothing sets it
   otherwise from hub.json).
3. The Tracked / Required / Verified branches are unreachable from
   production callers; only the L3 test exercises them via direct
   `BrokerService::Config` construction.

### 1.5.3. Why the placeholder cannot be production-wired by simply re-adding the fields

`HubBrokerConfig`'s comment ("Auth/access fields deliberately
omitted ... See HEP-CORE-0035 for the design that must land before
they return") is precise.  Re-introducing the auth fields into the
parser *without* the rest of this HEP would re-enable the legacy
string-match enforcement at the application layer — but production
roles can claim any `role_name` + `role_uid` they like in REG_REQ
(the strings are self-asserted), so re-adding the fields alone would
provide no actual authentication.  The CURVE socket-layer guarantee
that "this connection comes from a key the operator authorized"
requires the ZAP handler designed in §4.1, not anything in the
placeholder.

---

## 1.6. Premise for this HEP's implementation (added 2026-05-13)

This HEP cannot be implemented in isolation.  Two foundation pieces
must land first, in order:

### 1.6.1. HubState completion

`PubkeyOrigin` (§4.2) is an index inside `HubState`.  Both the ZAP
handler (Layer 1) and the federation-trust gate (Layer 2) read from
that single index.  HubState's data model needs to stabilize before
the index can be added without inviting churn:

- The shape of `ChannelEntry` / `ProducerEntry` / `ConsumerEntry` /
  federation peer entries determines what pubkey-bearing records the
  index covers.
- The thread-safety contract for HubState reads/writes determines
  whether the ZAP handler (running on a separate `inproc` socket
  per RFC 27) needs lock-free read paths into the index.
- The lifecycle of pubkey-bearing entries (when do they get added /
  removed) determines the index's update protocol.

These questions belong to the broader HubState refactor work, not to
this HEP.  Adding pubkey provenance to a HubState model still in
flux would conflate concerns and likely require rework.

### 1.6.2. `plh_hub` binary completion

The hub binary (`plh_hub`) is what owns the hub config + HubHost
composite end-to-end: it loads hub.json, constructs `HubConfig` →
`HubBrokerConfig`, hands it to `HubHost::startup()`, and runs the
main loop.  Today the binary is still finding its shape (D2.3 +
ongoing work).  Auth concerns — hot-reloading the allowlist, AdminService
RPCs for `add_known_role` / `remove_known_role` (currently deferred
per `admin_service.cpp:315-326`), federation-peer hot-add/remove,
audit logging — all live in the binary.  Adding auth list management
to a binary whose shape is still moving would be premature; the
authorization machinery is mounted *on top of* the binary's
established lifecycle.

### 1.6.3. Then: auth-list propagation + management design

Once the foundations are in place, the open questions in §7 become
addressable, plus:

- **Source of truth for `known_roles`.** Static (hub.json) only?
  Hot-reloadable via SIGHUP + file watch?  AdminService RPC
  (`add_known_role` / `remove_known_role`) over an
  operator-authenticated channel?  Some hybrid?
- **Propagation across a federation.**  When Hub-A adds a role
  pubkey, does Hub-B learn about it automatically (via augmented
  HUB_PEER_HELLO per §4.4)?  What's the consistency model — eventual
  via heartbeat, immediate via push, neither?
- **Audit log model.**  Which Layer-1 + Layer-2 decisions get logged
  at what level?  Where do the logs live (per-hub stdout, central
  syslog, structured admin event stream)?
- **Operator workflow for key rotation.**  §7 open question #1
  (grace window vs hard-cut) needs an operator-facing answer.

None of these are blockers for *this HEP's design* — they are
prerequisites for *implementation* of Phase 1.  Until they're
answered, the placeholder stays in place as the only role-identity
gate; the L3 test guards it against accidental regressions during
the wait.

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

`BrokerServiceImpl::check_role_identity` (`broker_service.cpp:2118-2170`)
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

The mechanism the operator configures (`RoleIdentityPolicy` + `known_roles[].pubkey`)
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

- `RoleIdentityPolicy::Required` — string-presence check; with CURVE+ZAP,
  the connecting socket's pubkey is mandatory and verified, so a
  separate "did you put a name in your JSON" gate adds nothing.
- `RoleIdentityPolicy::Verified` — string allowlist; redundant with ZAP
  pubkey allowlist.
- `RoleIdentityPolicy::Tracked` / `Open` — operator-observability concerns
  belong in audit logging, not in the gate enum.
- `ChannelPolicyOverride` per-channel glob overrides — no use case has been
  identified that the per-role + per-peer policy can't address. Revisit
  only if a concrete scenario emerges.

---

## 4.6 Key-file storage discipline

The cryptographic strength of the layered enforcement in §4.1 rests on
**two operational trust anchors**:

1. **Secret-key file confidentiality** — `<role|hub_uid>.sec` files MUST
   not be readable by anyone other than the file owner.  A leaked
   secret key lets the holder impersonate the role / hub.
2. **Allowlist file integrity** — `known_roles/` (or whichever directory
   holds the operator-authorized pubkeys) MUST not be writable by
   anyone other than the file owner.  A tampered allowlist lets an
   attacker add arbitrary pubkeys, bypassing the I1 "role known" gate
   in HEP-0036 §3.

This is the same discipline OpenSSH enforces on `~/.ssh/id_*` and
`authorized_keys`.  HEP-0035 mandates equivalent enforcement at two
points: (a) at file creation time, the `--keygen` / `--init` tooling
SETS modes correctly without depending on the operator's `umask`;
(b) at every binary startup, before any secret is read, the binary
VERIFIES modes and refuses to start with a clear, actionable error.

### 4.6.1 Required modes (set by `--keygen` / `--init`)

| Created by | Path | Mode | Owner |
|---|---|---|---|
| `plh_hub --keygen` | `<hub_uid>.sec` | `0600` | current user |
| `plh_hub --keygen` | `<hub_uid>.pub` | `0644` (public; distributable) | current user |
| `plh_hub --init` | hub keystore directory (e.g. `hub_keys/`) | `0700` | current user |
| `plh_hub --init` | `known_roles/` directory | `0750` (owner write; optional group read) | current user |
| `plh_role --keygen` | `<role_uid>.sec` | `0600` | current user |
| `plh_role --keygen` | `<role_uid>.pub` | `0644` | current user |
| `plh_role --init` | role config directory | `0700` | current user |
| `plh_role --init` | locally-copied `<hub_uid>.pub` | `0644` | current user |

Implementation: use `open(O_CREAT | O_EXCL, 0600)` followed by an
explicit `fchmod(fd, 0600)` at write time.  Do NOT rely on the
process `umask`.

### 4.6.2 Startup verification (every `plh_hub` / `plh_role` invocation)

Before reading any secret material, every binary MUST run:

```
check <uid>.sec
  - (st_mode & 0077) != 0       → ERROR  "secret key file <path> is
                                  group/world-accessible (mode 0NNN).
                                  Run: chmod 0600 <path>"
  - st_uid != geteuid()         → ERROR  "secret key file <path> owned
                                  by uid N; expected uid M.  Check file
                                  ownership."
  - parent directory (st_mode & 0077) != 0
                                → WARN   (parent dir leak is recoverable;
                                  some operators want group-readable
                                  parents for shared host setups)

check known_roles/ (hub only)
  - (st_mode & 0002) != 0       → ERROR  "allowlist directory <path> is
                                  world-writable.  Run: chmod 0750 <path>"
  - st_uid != geteuid()         → ERROR

check hub.json / role config
  - (st_mode & 0002) != 0       → ERROR  (world-writable config = config
                                  injection)
  - (st_mode & 0040) != 0 AND file references a keyfile path
                                → WARN

# .pub files: NO permission check — public keys are intentionally
# distributable and may legitimately be group-/world-readable.
```

Error messages MUST name the offending path, the observed mode, the
required mode, and the exact `chmod` command to fix it.  This matches
OpenSSH's failure-message style (which works well operationally).

The verification function lives in a shared utility under
`src/utils/security/` (e.g. `key_file_acl.{hpp,cpp}`) so that both
binaries call into the same code.

### 4.6.3 Interaction with B3 / B4 / `--init`

This requirement layers on top of existing related work:

- B3 (task #78) — hard-error empty `hub.auth.keyfile=""`.  This HEP
  §4.6 adds the mode check on top: even if the path is non-empty,
  the file's mode is verified.
- B4 (task #79) — `plh_role --init` generates a non-zero SHM secret.
  Same `--init` pass SHOULD also generate the role's CURVE keypair
  (or invoke `--keygen` internally), with the modes from §4.6.1
  applied at write time.  The operator should not have to remember
  a separate keygen step.
- §4.6 closure → file a sub-task of HEP-0035 implementation
  (task #74) for the shared `key_file_acl` utility plus binary
  wiring at startup.

### 4.6.4 Out of scope

- File-system-level ACLs (POSIX ACLs / SELinux contexts / Linux
  capabilities) — not addressed.  Operators in hardened environments
  may layer those on top.  HEP-0035 enforces the baseline UNIX mode
  bits; richer ACL schemes are operator-discretion.
- Encrypted-at-rest secret files (e.g., passphrase-encrypted `.sec`).
  HEP-CORE-0024 §11 already has a passphrase-unlock flow; §4.6 only
  governs the on-disk file ACLs, not the cipher state of the contents.

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
| 6     | Cleanup: delete `RoleIdentityPolicy` enum, `check_role_identity`, `KnownRole`, `ChannelPolicyOverride`, L3 test that exercises them |
| 7     | HEP-0009 §2.7 retraction; HEP-0022 §6.1 update; HEP-0033 §6.2/§6.4/§15 cleanup |

---

## 9. References

- HEP-CORE-0009 §2.7 — legacy `RoleIdentityPolicy` (superseded by this HEP).
- HEP-CORE-0022 — Hub Federation (peer handshake, federation peer config).
- HEP-CORE-0024 §11 — Role keygen + vault (source of `known_roles[].pubkey`).
- HEP-CORE-0033 §6 — Hub config (where `broker.known_roles` lives).
- HEP-CORE-0033 §8 — `HubState` (where the pubkey index lives).
- ZeroMQ ZAP RFC 27 — http://rfc.zeromq.org/spec:27/ZAP
- ZeroMQ CURVE security — http://api.zeromq.org/master:zmq-curve
