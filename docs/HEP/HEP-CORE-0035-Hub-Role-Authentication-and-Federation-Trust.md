# HEP-CORE-0035: Hub-Role Authentication and Federation Trust

| Property        | Value                                                                                              |
|-----------------|----------------------------------------------------------------------------------------------------|
| **HEP**         | `HEP-CORE-0035`                                                                                    |
| **Title**       | Hub-Role Authentication and Federation Trust                                                       |
| **Status**      | 🚧 **NOT IMPLEMENTED — TODO NEXT.** Authoritative design; supersedes the legacy `RoleIdentityPolicy` placeholder documented in HEP-CORE-0009 §2.7. |
| **Created**     | 2026-04-29                                                                                         |
| **Area**        | Framework Architecture (`BrokerService` socket layer, `HubConfig`, `BrokerService::Config`, federation) |
| **Depends on**  | HEP-CORE-0022 (Federation), HEP-CORE-0024 (Role Directory), HEP-CORE-0033 (Hub Character)          |
| **Related**     | HEP-CORE-0036 (Authenticated Connection Establishment) — adds Layer-3 data-plane peer authentication on top of HEP-0035's Layer-1+2 (see §4.1).  HEP-0036 also adds §4.6 (file-ACL discipline) and §4.7 (runtime key handling) to this HEP. |
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
                                ↓
┌─────────────────────────────────────────────────────────────────┐
│  Layer 3 — Data-plane peer authentication (HEP-CORE-0036)       │
│                                                                 │
│  Each producer attaches a per-context ZAP handler to its ZMQ     │
│  data PUSH socket (SHM transport uses the existing DataBlock     │
│  `shm_secret` mechanism per HEP-CORE-0002 — no ZAP for SHM).     │
│  The handler reads from a per-channel allowlist                  │
│  (`ChannelAccessIndex::authorized_consumer_pubkeys` in HubState  │
│  per HEP-CORE-0036 §4.1) populated by the broker via             │
│  CHANNEL_AUTH_UPDATE pushes.                                     │
│                                                                 │
│  • Consumer handshake with a pubkey on the producer's            │
│    channel allowlist → accept.                                   │
│  • Consumer handshake with any other pubkey → reject at ZAP.     │
│                                                                 │
│  Effect: layer 3 enforces "this consumer is authorized to        │
│  connect to this specific channel" at the producer's data        │
│  socket — the per-channel scope that layers 1+2 don't cover.     │
│  Per HEP-CORE-0036 I6 (T1 lock-in), neither side uses            │
│  broker-minted CURVE keys — both use their identity keypairs;    │
│  the allowlist is the gating mechanism.                          │
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

**Relation to HEP-CORE-0036 `ChannelAccessIndex`**: HEP-0036's
Layer-3 enforcement (data-plane peer ZAP on the producer side) uses
a SEPARATE per-channel structure (`HubState::channel_access_index_`,
keyed by channel name; HEP-0036 §4.1) that holds the per-channel
authorized-consumer-pubkey allowlist + the SHM secret.  That
structure CONSUMES this `PubkeyOrigin` index (the producer's identity
pubkey looked up at REG time is mirrored into the broker's per-channel
`ChannelEntry::producers[].zmq_pubkey`).  The two indices have
different scopes and are not interchangeable: `PubkeyOrigin` answers
"is this a known role at all?"; `ChannelAccessIndex` answers "is this
consumer authorized for THIS channel?"

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

The pylabhub vault is the canonical home for encrypted-at-rest
secrets — it adds password-derived encryption on top of the file-mode
discipline below.  The vault format and `vault/` directory are
designed for extension (see §4.8 for known-roles allowlist storage
inside the vault; HEP-CORE-0038 for script-managed per-role secrets).
The `0700` mode on `vault/` therefore applies to the whole vault
scope, not just current contents.

| Created by | Path | Mode | Owner |
|---|---|---|---|
| `plh_hub --keygen` | `<hub_dir>/vault/<hub_uid>.vault` (encrypted: broker CURVE keypair + admin token + `known_roles` per §4.8; password-derived key.  Filename embeds the hub UID per HEP-CORE-0033 §6.5 revised 2026-05-31.) | `0600` | current user |
| `plh_hub --keygen` | `<hub_dir>/hub.pubkey` (plaintext broker CURVE pubkey; operator-distributable) | `0644` | current user |
| `plh_hub --init` | `<hub_dir>/vault/` (encrypted-secrets directory) | `0700` | current user |
| `plh_hub --init` | `<hub_dir>/` (hub config directory) | `0700` | current user |
| `plh_role --keygen` | `<role_dir>/vault/<role_uid>.vault` (encrypted: role CURVE keypair + per HEP-CORE-0038 a `scripts` map for script-managed secrets; password-derived key) | `0600` | current user |
| `plh_role --init` | `<role_dir>/vault/` | `0700` | current user |
| `plh_role --init` | `<role_dir>/` (role config directory) | `0700` | current user |
| `plh_role --init` | `<role_dir>/hub.pubkey` (locally-cached hub pubkey copy; plaintext) | `0644` | current user |

**No `known_roles/` directory.** Earlier drafts of this HEP listed a
plaintext `<hub_dir>/known_roles/` directory holding one `.pub` file
per authorized role.  That design was rejected because file-mode
discipline alone cannot protect the allowlist against an attacker
who obtains file-write on the hub directory.  Allowlist storage
moves INSIDE the vault per §4.8.  The vault's password+mode
protection composes; neither alone is sufficient.

**Vault directory placement is operator-controlled, NOT pinned to
`<hub_dir>` / `<role_dir>`.**  The `vault/` directory path is
derived from `auth.keyfile` at runtime — see HEP-CORE-0033 §7.1 +
HEP-CORE-0024 §3.4.  This enables the system-managed-config +
user-owned-vault deployment model: `<hub_dir>` / `<role_dir>` may
live under a root-owned global install while the vault lives in a
user-writable directory.  Mode 0700 + euid-owner match apply to
the vault directory wherever it lives.  Mode discipline is
independent of placement.

Implementation: use `open(O_CREAT | O_EXCL, 0600)` followed by an
explicit `fchmod(fd, 0600)` at write time.  Do NOT rely on the
process `umask`.

### 4.6.2 Startup verification (every `plh_hub` / `plh_role` invocation)

**The vault file path is resolved from `auth.keyfile`** per HEP-
CORE-0033 §7.1 (hub side) and HEP-CORE-0024 §3.4 (role side).  The
runtime verification has two tiers:

1. **Unconditional checks** — run on every invocation.  Cover the
   config file (config-injection prevention is needed independent of
   vault setup — a tampered config can flip endpoints or admin flags
   before the vault is ever opened).
2. **Vault-file checks** — run on every invocation as well, because
   `auth.keyfile` is required and non-empty per §4.6.3 / HEP-CORE-0024
   §3.4 / HEP-CORE-0033 §7.1 (finalized 2026-05-31; no in-memory CURVE
   mode exists).  Cover the vault file + its parent directory at the
   resolved path.  **The parent-dir check is load-bearing for the
   §4.6.4 symlink threat model** — without it the verify-then-read
   window for the vault file could be redirected by a directory-
   entry rewrite.  Any future auth surface that calls
   `verify_keyfile_acl(VaultFile)` MUST pair it with
   `verify_keyfile_acl(VaultDir)` on `path.parent_path()`; see
   §4.6.4 "Read path (verify + open) uses `stat(2)` …
   Precondition (load-bearing)" for the threat-model derivation.

`auth.keyfile` value semantics:

- Non-empty relative `auth.keyfile` → resolved against `base_dir`
  (hub_dir or role_dir).
- Non-empty absolute `auth.keyfile` → used as-is.
- Non-empty + file absent at the resolved path → hard error
  (the operator configured a vault; no silent fallback).
- Empty `auth.keyfile` `""` → **HARD ERROR at config-load**
  (HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1, finalized
  2026-05-31).  pylabhub is a vault; there is no in-memory
  CURVE mode.  No silent fallback that does not actually help
  with security would be misleading.
- Field missing entirely OR `auth` object missing OR
  `auth.keyfile` non-string → config-load error before this
  section runs (closes task #78 via E′-2a).

Before any secret material is read OR any config-derived behavior
is committed, every binary MUST run the verification below.  **Tier
1 runs before Tier 2** — the config file determines `auth.keyfile`
and therefore Tier 2's resolved path; a tampered config could
redirect Tier 2 to an attacker-controlled file if the integrity of
the config itself were not verified first.

```
# ── Tier 1: unconditional checks ──────────────────────────────────
# Run on every invocation.

check hub.json / role config (the file pointed to by --config or
auto-discovered from --hub-dir / --role-dir).
  - (st_mode & 0002) != 0       → ERROR  (world-writable config = config
                                  injection — a tampered config can
                                  redirect the binary to malicious
                                  endpoints or flip admin flags before
                                  the vault is ever read).
  - (st_mode & 0040) != 0 AND file references a vault path
                                → WARN

# ── Tier 2: vault file checks ─────────────────────────────────────
# Always runs — empty auth.keyfile is rejected at config-load
# (HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1), so by the time we
# reach Tier 2 there is always a vault path to check.

check vault file at <resolved_keyfile_path>
  - (st_mode & 0077) != 0       → ERROR  "vault file <path> is
                                  group/world-accessible (mode 0NNN).
                                  Run: chmod 0600 <path>"
  - st_uid != geteuid()         → ERROR  "vault file <path> owned
                                  by uid N; expected uid M.  Check file
                                  ownership."
  - parent dir (st_mode & 0077) != 0
                                → WARN   (parent dir leak is recoverable;
                                  some operators want group-readable
                                  parents for shared host setups)

check vault directory at <resolved_keyfile_path>.parent_path()
  - (st_mode & 0077) != 0       → ERROR  "vault directory <path> is
                                  group/world-accessible.  Run:
                                  chmod 0700 <path>"
  - st_uid != geteuid()         → ERROR

# Public key files (hub.pubkey, cached role pubkey copies): NO
# permission check — public keys are intentionally distributable and
# may legitimately be group-/world-readable.
#
# No plaintext known_roles/ directory exists; the allowlist lives
# inside the vault (§4.8) and is gated by the master password.
```

Error messages MUST name the offending path, the observed mode, the
required mode, and the exact `chmod` command to fix it.  This matches
OpenSSH's failure-message style (which works well operationally).

The verification function lives in a shared utility under
`src/utils/security/` (e.g. `key_file_acl.{hpp,cpp}`) so that both
binaries call into the same code.

### 4.6.3 Interaction with `--init` / `--keygen` (closed 2026-05-31)

This requirement layers on top of work that has now landed:

- **B3 (task #78) — CLOSED 2026-05-31 via commits 42e0a873
  (C′-1), 51f76d55 (E′-2a), 2e730fa6 (E′-2b).**  Original scope:
  hard-error empty `hub.auth.keyfile`.  Final shipped scope:
  unified `auth.keyfile` semantics across hub and role per
  HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1.  Empty `""` is a
  config-load error (no in-memory CURVE mode); missing field is
  a config-load error; missing `auth` object is a config-load
  error; non-string keyfile is a config-load error.  Diagnostics
  cite HEP-CORE-0024.
- **`--keygen` symmetric contract (closed 2026-05-31).**  Both
  `plh_hub --keygen` and `plh_role --keygen` refuse to overwrite
  an existing vault file (`HubConfig::create_keypair` /
  `RoleConfig::create_keypair` check `fs::exists(vault_path)`).
  Diagnostic names the path, explains what would be destroyed
  (CURVE keypair + admin token on hub; CURVE keypair on role),
  provides the exact `rm '<path>'` command, and cites the HEP.
  No `--force` flag.
- **Symmetric `warn_if_keyfile_in_hub_dir` (closed 2026-05-31).**
  `HubDirectory::warn_if_keyfile_in_hub_dir(base, keyfile)` is
  the hub-side mirror of
  `RoleDirectory::warn_if_keyfile_in_role_dir`.  Called from
  `HubConfig::load()`; emits the standard
  `*** PYLABHUB SECURITY WARNING ***` block when the resolved
  path is inside `hub_dir`.  Closes the hub-vs-role asymmetry
  flagged in the 2026-05-31 holistic review (Finding #6).
- **Hub vault filename UID-keyed (closed 2026-05-31).**  Hub
  vault filename is now `<hub_uid>.vault` (HEP-CORE-0033 §6.5
  revised), eliminating the prior fixed-`hub.vault` collision
  hazard when multiple hubs share a vault directory.
- B4 (task #79) — `plh_role --init` non-zero SHM secret remains
  open.  Same `--init` pass SHOULD also generate the role's
  CURVE keypair (or invoke `--keygen` internally), with the
  modes from §4.6.1 applied at write time.  The operator should
  not have to remember a separate keygen step.
- §4.6 closure → **CLOSED 2026-06-01** in commit `4f3fb077`
  + #101 review chain through commit `eb6d0bbe` (task #101,
  absorbed deferred S1/S2/S3).  Implementation arrived in 4f3fb077;
  4 follow-up fresh-eye review rounds closed compounding doc/code
  drift (`40f6320c` docs+comments, `b8a51e96` test fixes,
  `50b93f23` production hardening incl. M1 atomic `publish_public_key`
  + L6 `ConfigFileReferencingVault` advisory wire, `165afdf8`
  round-2 cleanup, `eb6d0bbe` round-3 cleanup incl. H1 `AclVerdict`
  docstring contract + B2 keygen note observability).  Shared
  `key_file_acl` utility in `src/utils/security/key_file_acl.cpp`
  is wired at: vault create (`HubVault::create` + `RoleVault::create`
  enforce 0700 on the parent dir via `set_keyfile_mode`); atomic
  vault write (`vault_crypto::write_secure_file` uses POSIX
  `O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC` + `fchmod 0600`,
  closing TOCTOU + symlink-redirect attacks at the kernel layer);
  atomic pubkey publish (`HubVault::publish_public_key` uses the
  same flag set + `O_TRUNC` semantics via unlink-then-create, with
  stderr `note:` on pre-existing-file removal for tamper
  observability); and binary startup (`HubConfig::load_keypair` +
  `RoleConfig::load_keypair` call `verify_keyfile_acl` for both
  `VaultFile` and `VaultDir` BEFORE reading the secret, refusing
  to load with an OpenSSH-style actionable diagnostic on
  violation; advisory ConfigFileReferencingVault warnings on
  group-readable hub.json / role config surfaced at config-load
  via the `!v.diagnostic.empty()` gate).  Task #120 tracks the
  Windows pathway hardening follow-up (out of scope for the Linux
  cryptographic floor).

### 4.6.4 Out of scope

- File-system-level ACLs (POSIX ACLs / SELinux contexts / Linux
  capabilities) — not addressed.  Operators in hardened environments
  may layer those on top.  HEP-0035 enforces the baseline UNIX mode
  bits; richer ACL schemes are operator-discretion.
- Cipher details for the encryption-at-rest layer are out of §4.6
  scope (covered by `src/utils/service/vault_crypto.{hpp,cpp}` —
  Argon2id KDF + XSalsa20-Poly1305 secretbox); §4.6 governs only
  the on-disk file ACLs that protect the vault container.
- Symlink handling — refined scope (2026-06-01):
  - **Write path (vault create) IS symlink-gated.**
    `vault_crypto::write_secure_file` (POSIX branch) uses
    `O_NOFOLLOW` so a symlink at the final `auth.keyfile`
    component causes `open(2)` to fail with `ELOOP` and refuse
    to write — the secret never leaves the process.  This
    closes the symlink-redirect attack at vault create time.
  - **Read path (verify + open) uses `stat(2)` (symlink-
    following).**  By the time read happens the file is already
    owner-owned + mode 0600 (write path enforced).  A symlink
    planted between verify and read would point to a file the
    attacker must also own + 0600 to satisfy verify — i.e.,
    a self-redirect to attacker's own secret, which is
    no different from configuring the keyfile path there.
    **Precondition (load-bearing).**  This argument is sound ONLY
    while §4.6.2 also enforces the *parent dir* contract
    (`VaultDir` = 0700 + euid-owned).  Parent-dir 0700 + owner-
    only is what prevents an attacker from rewriting the
    `auth.keyfile` directory entry to swap the symlink in between
    verify and read.  Any future caller that uses
    `verify_keyfile_acl(VaultFile)` without the matching
    `verify_keyfile_acl(VaultDir)` on `path.parent_path()`
    inherits a strictly weaker guarantee.  The §4.6.2
    `HubConfig::load_keypair` + `RoleConfig::load_keypair` call
    sites pair the two; HEP-0036 surfaces that add new auth
    surfaces MUST do the same.
  - Non-regular files (FIFO / device) — not separately gated.
    Operators pointing `auth.keyfile` at a path under a
    directory they do not own (e.g., `/tmp`) still accept
    symlink-injection risk for read; the encryption-at-rest
    layer (vault password) remains the primary integrity
    defense in that operator-chosen-risk configuration.

---

## 4.7 Runtime key handling — swap, core dumps, plaintext zeroing

§4.6 covers secret material **at rest** on disk.  §4.7 covers the
same material **at runtime** while it's loaded into the process.

### 4.7.1 Threats addressed

| # | Leak channel | What an attacker gets without §4.7 |
|---|---|---|
| 1 | OS pages memory to swap | seckey bytes carved from the swap partition |
| 2 | Process crashes → core dump | seckey bytes in the dump file |
| 3 | Lingering plaintext in heap buffers after key has been handed to libsodium / libzmq | seckey bytes recoverable via heap forensics on a still-running or recently-stopped process |

§4.7 does NOT defend against an attacker who can read the process's
LIVE memory (debugger, malicious in-process script, ptrace).  Per
HEP-0036 I8 (trust model), that's an out-of-scope insider threat
requiring HSM/TEE — not a software-only solution.

### 4.7.2 Three measures (all three are mandatory)

1. **Lock secret pages into RAM** — prevent the OS from swapping
   them.  Apply to: in-memory copies of `<role|hub>.sec`, broker's
   in-memory copy of any cached secret bytes.
2. **Disable core dumps** on `plh_role` and `plh_hub` binaries —
   prevent crash-driven memory dumps from landing on disk.
3. **Zero plaintext buffers as soon as the key is consumed** —
   minimize the window between loading the key from disk and
   handing it to libsodium / libzmq.  Use a memory-zero primitive
   the compiler cannot optimize away.

### 4.7.3 Cross-platform mechanism table

The implementation lives in `src/utils/security/runtime_key_handling.{hpp,cpp}`
and is compiled into both `plh_role` and `plh_hub`.

| Measure | Linux | macOS / BSD (POSIX) | Windows |
|---|---|---|---|
| Lock memory pages | `mlock()` (POSIX) | `mlock()` (POSIX) | `VirtualLock()` |
| Lock memory (preferred wrapper) | `sodium_mlock()` | `sodium_mlock()` | `sodium_mlock()` |
| Hardened allocator (mlock + guard pages + canary + auto-wipe) | `sodium_malloc()` / `sodium_free()` | `sodium_malloc()` / `sodium_free()` | `sodium_malloc()` / `sodium_free()` |
| Disable core dumps (resource-limit form, portable POSIX) | `setrlimit(RLIMIT_CORE, {0,0})` | `setrlimit(RLIMIT_CORE, {0,0})` | n/a |
| Disable core dumps (defence-in-depth, Linux-only) | `prctl(PR_SET_DUMPABLE, 0)` (also blocks `ptrace`) | n/a | n/a |
| Exclude key pages from core dump (page-granular) | `madvise(addr, len, MADV_DONTDUMP)` | n/a (mlock + rlimit alone) | n/a (locked pages don't appear in minidumps) |
| Suppress Windows Error Reporting crash dialog + minidump | n/a | n/a | `SetErrorMode(SEM_NOGPFAULTERRORBOX \| SEM_FAILCRITICALERRORS)` + `WerAddExcludedApplication(L"plh_role.exe")` |
| Compiler-safe memory zero | `sodium_memzero()` | `sodium_memzero()` | `sodium_memzero()` |
| Compiler-safe memory zero (system-provided fallback) | `explicit_bzero()` (glibc 2.25+) | `explicit_bzero()` (BSD) | `SecureZeroMemory()` (Win32) |

**libsodium wraps all three measures cross-platform.**  Pylabhub
already depends on libsodium (HEP-0035 §13 q1 + HEP-0036
broker keygen), so `sodium_mlock` / `sodium_memzero` / `sodium_malloc`
are the canonical wrappers and §4.7.4's utility uses them
unconditionally.  System fallbacks are listed for reference
(when reviewing platform behavior) but the impl does not
conditionally fall back to them.

### 4.7.4 Shared utility (`src/utils/security/runtime_key_handling.{hpp,cpp}`)

The utility exposes a small surface that both binaries call:

```cpp
namespace pylabhub::security {

/// Call ONCE during `main()`, before any secret material is loaded.
/// On POSIX: setrlimit(RLIMIT_CORE, 0); on Linux also prctl PR_SET_DUMPABLE=0;
/// on Windows: SetErrorMode + WerAddExcludedApplication.
/// Returns false (and writes a diagnostic to `err`) only on systems
/// where the call unexpectedly fails (very rare).  Callers MUST treat
/// failure as a fatal startup error — refuse to proceed.
bool disable_core_dumps(std::string *err);

/// RAII wrapper around a key-sized buffer (typically 32 or 64 bytes).
/// Construction: allocates via sodium_malloc (mlock + guard pages + canary).
/// Destruction: sodium_memzero + sodium_free.
/// Copy/move: disabled.  Pass by reference.
class SecureKeyBuffer {
public:
    explicit SecureKeyBuffer(std::size_t len);
    ~SecureKeyBuffer() noexcept;
    SecureKeyBuffer(const SecureKeyBuffer&)            = delete;
    SecureKeyBuffer& operator=(const SecureKeyBuffer&) = delete;
    SecureKeyBuffer(SecureKeyBuffer&&)                 = delete;
    SecureKeyBuffer& operator=(SecureKeyBuffer&&)      = delete;

    [[nodiscard]] std::byte *data() noexcept;
    [[nodiscard]] const std::byte *data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::byte   *buf_;
    std::size_t  len_;
};

/// One-shot zero of an externally-owned buffer.  Compiler-safe via
/// sodium_memzero.  Use for buffers the caller allocated themselves
/// (e.g. a `std::vector<std::byte>` populated by file I/O before
/// the bytes are copied into a `SecureKeyBuffer`).
void zero_buffer(void *addr, std::size_t len) noexcept;

}  // namespace pylabhub::security
```

### 4.7.5 Integration points

| Site | What changes |
|---|---|
| `plh_hub` `main()` (very early) | Call `disable_core_dumps()`.  Fatal if it fails. |
| `plh_role` `main()` (very early) | Same. |
| Key-loading code (load `.sec` from disk) | Read into a stack buffer; copy into `SecureKeyBuffer`; `zero_buffer()` the stack buffer immediately.  Pass `SecureKeyBuffer::data()` to libsodium / libzmq. |
| ZMQ socket CURVE config | Already passes the bytes by value into libzmq, which keeps its own copy.  Our `SecureKeyBuffer` lifetime can shrink to "until libzmq has accepted the key"; `~SecureKeyBuffer()` then zeros our copy. |
| Broker per-channel allowlist (HEP-0036 §4.1) | Allowlist holds PUBLIC keys only; no secret material to lock.  No §4.7 work needed. |

### 4.7.6 Order with respect to §4.6

`disable_core_dumps()` MUST be called BEFORE the §4.6 file-ACL
verification, because if the file-ACL check fails and the binary
calls `std::abort()`/`exit(1)` while a core-dump policy is still
permissive, a partial dump could leak whatever the binary loaded
before the check.  Canonical order in both binaries' `main()`:

```
1. security::disable_core_dumps()        // §4.7
2. security::verify_key_file_acls(...)   // §4.6
3. security::load_keypair(...)           // returns SecureKeyBuffer
4. ... start sockets, etc. ...
```

### 4.7.7 Limitations (explicit, so operators are not misled)

- **Live-memory attackers are not defended against.**  A debugger
  attached to the running process can still read the seckey from
  RAM.  Linux `prctl(PR_SET_DUMPABLE, 0)` blocks `ptrace` from
  non-root users; root can still attach.  Windows offers similar
  but not identical protections via `SetProcessMitigationPolicy`.
  This is the same scope statement as HEP-0036 §3 I8.
- **ZeroMQ holds its own copy.**  Once we pass a key to a
  CURVE-configured socket, libzmq stores it internally for the
  socket's lifetime.  Our `SecureKeyBuffer` can be destroyed after
  the socket is configured, but libzmq's copy lives until the
  socket closes.  We do NOT have access to libzmq's internal
  storage for mlock/zeroing purposes; we accept this as a known
  limitation.
- **System swap policy still matters.**  Even with `sodium_mlock`,
  if the operator has configured an aggressive `vm.overcommit`
  policy or running under memory pressure, the OS may reject
  `mlock` (returns `ENOMEM`).  The utility logs a WARN and
  continues; this is operator-visible.  Hardened deployments
  should size RAM such that `mlock` never fails.
- **macOS sandbox / iOS-style restrictions.**  Not target
  platforms; if pylabhub ever ships there, additional entitlements
  may be required for `mlock`.

### 4.7.8 Test coverage

- L1 unit tests for `SecureKeyBuffer` lifecycle (allocate → write
  → destroy → verify post-destruction memory pattern is zeroed)
  on Linux, macOS, Windows CI runners.
- L2 platform-conditional test that verifies `disable_core_dumps`
  takes effect: trigger a deliberate `SIGSEGV` in a subprocess
  after disabling core dumps; assert no core file is produced
  (POSIX) or no minidump appears in `%LOCALAPPDATA%\CrashDumps`
  (Windows).

---

## 4.8 Known-roles allowlist storage (in vault)

The hub's `known_roles[]` allowlist — the set of CURVE public keys
authorized to register on this hub (Layer-1 ZAP gate per §4.1) — is
stored INSIDE the hub vault, NOT as plaintext `.pub` files in a
dedicated directory.

### 4.8.1 Why in the vault

File-permission discipline alone does not protect the allowlist
against an attacker who obtains file-write on the hub directory: that
attacker can drop arbitrary `.pub` files to grant themselves
admission.  Storing the allowlist inside the vault adds
encryption-at-rest — modifying the allowlist requires the master
password, even with full filesystem access.  The two protections
compose; neither alone is sufficient against a determined local
attacker.

### 4.8.2 Storage layout

The hub vault payload (`HubVault` JSON; HEP-CORE-0035 §4.6 + the file
header at `src/include/utils/hub_vault.hpp`) gains a new top-level
key `known_roles`:

```json
{
  "broker":      { "curve_secret_key": "...", "curve_public_key": "..." },
  "admin":       { "token": "..." },
  "known_roles": {
    "prod.sensor1.uid3a7f2b1c": "<Z85 40-char pubkey>",
    "cons.logger.uid8c4e1f9a":  "<Z85 40-char pubkey>"
  }
}
```

At broker startup, `BrokerService` reads `known_roles` from the
decrypted vault payload and populates its in-memory Layer-1 ZAP
allowlist (§4.1).

### 4.8.3 Operator CLI

Three new `plh_hub` CLI commands manage allowlist contents.  All
three first run the §4.6.2 file-ACL discipline check, then prompt for
the master password (or read `PYLABHUB_HUB_PASSWORD` env var), open
the vault, mutate the payload, and re-encrypt.

| Command | Action |
|---|---|
| `plh_hub --config <hub.json> --add-known-role <role.pub>` | Read CURVE pubkey from `<role.pub>` (40-char Z85).  role_uid is parsed from the `.pub` filename stem (e.g. `prod.sensor1.uid3a7f2b1c.pub` → `prod.sensor1.uid3a7f2b1c`), or supplied via `--role-uid <uid>`.  Vault gains an entry; if `role_uid` already exists, the command refuses unless `--force` is also passed (prevents accidental rotation). |
| `plh_hub --config <hub.json> --revoke-known-role <role_uid>` | Remove the entry by `role_uid`.  Refuses if uid not present (warns rather than errors). |
| `plh_hub --config <hub.json> --list-known-roles` | Print `(role_uid, pubkey_fingerprint)` table to stdout.  Fingerprint is `BLAKE2b-128(pubkey)` rendered as Z85 (24 chars), not the raw pubkey — keeps stdout from leaking sensitive material if redirected.  Exit code 0 = at least one entry; 1 = empty allowlist. |

### 4.8.4 Bootstrap

- `plh_hub --keygen` creates the vault with an empty `known_roles`
  map (`{}`).  The hub starts up with no admitted roles; every
  `REG_REQ` is rejected by Layer-1 ZAP until the operator adds at
  least one.
- Operator runs `--add-known-role` for each authorized role before
  the role binary attempts to connect.
- The role-side workflow is unchanged: each role still has its own
  vault holding its CURVE keypair; the operator distributes the
  role's `.pub` (extracted via `plh_role --keygen` output or the
  `<role_dir>/vault/<role_uid>.pub` file the role writes alongside its
  vault on first keygen) to the hub operator via the operator's
  existing channel (manual, signed messaging, etc.).

### 4.8.5 Hot reload (running hub)

A running `plh_hub` holds the allowlist in memory.  CLI commands that
mutate the vault MUST signal the running hub (admin RPC reload
command, HEP-CORE-0033 §11.2) to re-read its allowlist from the
freshly-rewritten vault.  Detection: the CLI command checks for the
hub PID file (`<hub_dir>/run/plh_hub.pid` per HEP-CORE-0033 §7) and,
if present, sends the reload RPC.  If absent, the next hub startup
picks up the new contents.

### 4.8.6 Out of scope

- Federation-delegated role propagation (HEP-CORE-0035 §4.4
  `HUB_PEER_HELLO.roles[]` augmentation) is unaffected.  The
  operator-managed `known_roles[]` is per-hub local truth; federation
  extends it at runtime with peer-attested entries that are NOT
  written to the local vault (they're held in `PeerEntry.delegated_roles`
  in memory per §4.4).
- Bulk-import of an existing OpenSSH `authorized_keys` file is not
  supported; pylabhub pubkeys are CURVE Z85, not OpenSSH format.
- Audit logging of allowlist mutations is covered separately under
  §7 audit-log scope (when that question resolves).

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
- **HEP-CORE-0036** (Authenticated Connection Establishment, locked
  2026-05-28) builds Layer-3 on top of this HEP's Layers 1+2.
  HEP-0036 is the AUTHORITATIVE source for data-plane peer
  authentication (per-producer ZAP cache + `ChannelAccessIndex` +
  `CHANNEL_AUTH_UPDATE` flow), inbox CURVE wiring (§9.3), band
  CURVE inheritance (§9.4), and the role-side `RegistrationState`
  FSM `Authorized` state (§4.3).  HEP-0035 stays scoped to Layer-1
  (broker ROUTER ZAP) + Layer-2 (federation trust modes) +
  §4.6 file-ACL discipline + §4.7 runtime key handling.

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
