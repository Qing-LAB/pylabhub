# DRAFT — Verified peer identity: closing the gap between the CURVE handshake and the admission gates

**Status:** RATIFIED by the owner 2026-07-27. Slice 1 (the origin index)
is shipped; the gap itself is **still open** — see §10 for the per-slice
state. Nothing in the tree yet compares a claimed identity against the
connection's verified key.
**Scope:** the control planes that accept authenticated connections —
role registration, inbox messaging, admin console, federation ingress.
The data plane is already correct and is not modified.
**Governing docs this affects:** HEP-CORE-0035 §4.1–§4.5, HEP-CORE-0036
§6.1/§6.3/§6.6.1, HEP-CORE-0027 §3.6/§8, HEP-CORE-0046 §14.5,
HEP-CORE-0033 §11.

---

## 1. The problem in plain language

When a role connects to the hub, it proves — cryptographically — that it
holds the private key matching one of the public keys the operator put in
the vault. That proof is real and it works.

Then the role sends a registration message that says "I am
`daq.sensor.temp`, my public key is P_temp." The broker checks that claim
against the vault: is that role known, and is that the key recorded for
it? Both pass.

**What the broker never does is compare the claim against the proof.**

It knows which key opened the connection. ZeroMQ hands that key to the
application as message metadata after every successful handshake. No code
above the socket has ever read it.

The consequence: the handshake proves *you are one of the roles the
operator trusts*. The registration check proves *the name and key you
typed are a legitimate pair*. Neither proves *the pair you typed is
yours*. Any holder of any vault key can register, deregister, or
re-endpoint any other role. Other roles' public keys are not secret —
the hub distributes them by design so peers can authenticate each other.

### Why this is not one bad gate

Four different values in this system look like "who sent this," and each
plane picked a different one:

| Value | Trustworthy | Where it is used today |
|---|---|---|
| CURVE key verified by the handshake | **Yes** | Nowhere above the socket |
| Frame-0 routing id (set by the client at connect) | No | Registration treats it as a trust claim; inbox keys replay defence, sequence tracking, and application-visible attribution on it |
| Body fields `role_uid` / `zmq_pubkey` (written by the client) | No | Registration admission gates |
| Peer network address | Weak | Admin console, as an "anti-hijack fact" |

The two planes contradict each other in writing. `hub_inbox_queue.cpp`
states correctly that under CURVE the router admits by key, "not by this
self-asserted routing_id — the id is only the ACK-return address, no
longer a trust claim." The registration gate `gate_identity_match` binds
`role_uid` to that same routing id and treats the match as meaningful.
Same field, opposite meanings, one codebase.

Every plane reached for a weak proxy **because the strong one was never
carried above the socket.** That is a missing layer, not a missing check,
and it is why this proposal does not add a gate.

---

## 2. What the design already says (and what got built instead)

This is the part that determines whether the fix is a patch or a
completion. It is a completion.

**HEP-CORE-0035 §4.2 already specifies the mechanism**, under the heading
"Pubkey index — single source of truth":

```cpp
struct PubkeyOrigin
{
    enum class Kind { LocalRole, FederationPeer };
    Kind        kind;
    std::string subject_uid;     // role uid OR peer hub uid
    std::string subject_name;
};
std::unordered_map<std::string, PubkeyOrigin> pubkey_to_origin;
```

with the normative sentence: *"Both the ZAP handler (Layer 1) and the
federation-trust gate (Layer 2) read from this single index. There is
exactly one structure that answers 'what does this pubkey mean to this
hub.'"*

`PubkeyOrigin` and `pubkey_to_origin` **do not exist anywhere in the
source tree.** Neither does `federation_trust_mode` or any of the three
policy modes §4.3 defines. Layer 1 was built (the ZAP allowlist). Layer 2
was built differently — as body-claim verification — and the index that
was supposed to join them was never built.

HEP-CORE-0036 §6.3 then justified the substitution after the fact:

> "The verification model (body carries the claim; broker checks
> `known_roles`) accepts the same security property without adding a
> second identity-recovery path."

It does not accept the same security property, for the reason in §1. Its
stated objection — that deriving identity would need a reverse index —
is answered by §4.2 of the *other* HEP, which already specifies exactly
that index as the single source of truth.

And HEP-CORE-0036 contradicts itself internally. §6.6.1 rule 1 states as
fact:

> "the `caller_uid` in the request body is not self-claimed but derived
> from the CURVE-verified identity. (Same as any other broker REQ.)"

That is the property this proposal implements. One of these two sections
has to go regardless of what is decided about the code.

**Reading of the archaeology.** §4.1's Layer-2 box still describes
classification by origin ("If the pubkey matches a local known_roles
entry → local role; accept. If the pubkey matches a federation peer's hub
pubkey → apply the federation-trust policy"). That is origin-index
language. The body-claim sentences were edited in around it. The original
design was key-classification; the implementation drifted to claim-
checking; the docs were patched to match the implementation in one place
and left describing the design in another.

---

## 3. The layer model

```mermaid
flowchart TD
    subgraph L1["Layer 1 — transport (already correct)"]
        A["CURVE handshake"] --> B["ZAP handler verifies key<br/>against the operator's list"]
    end
    subgraph L2["Layer 2 — ingress (THE MISSING LAYER)"]
        C["Capture the verified key<br/>onto the envelope at every router"]
    end
    subgraph L3["Layer 3 — resolution (specified, never built)"]
        D["pubkey_to_origin:<br/>key → principal"]
    end
    subgraph L4["Layer 4 — policy"]
        E["Registration gates"]
        F["Inbox attribution + replay keying"]
        G["Admin session binding"]
        H["Federation delegation"]
    end
    B --> C --> D --> E & F & G & H
```

The property that makes this a layer rather than a gate: **layer 4 never
sees a raw claim again.** A handler cannot accidentally trust the wrong
field, because the trustworthy one is the only identity on the envelope.

---

## 4. The mechanism

Four steps, each in exactly one place.

**Capture.** At every router ingress, read the verified key from the
message metadata and attach it to the `WireEnvelope` at parse time.
`WireEnvelope::parse_router_recv` already receives the raw frames and
already owns its buffers, so this is one field on an existing type. The
admin service already reads `Peer-Address` off frame 0 this way, so the
access pattern is proven in this codebase.

**Resolve.** Build `pubkey_to_origin` as HEP-0035 §4.2 specifies, at
vault load, from the same source that already produces the ZAP allowlist.
One structure answers "what does this key mean to this hub."

**Consume.** Each plane asks the resolved principal, not the wire:

- Registration: the claimed `role_uid` must equal the principal's
  `subject_uid`.
- Inbox: the sender identity handed to the application, the replay-guard
  key, and the sequence-tracking key all come from the principal.
- Admin: the session binds to the connection's verified key. Note this
  plane consumes *capture* without *resolution* — the admin plane is
  deliberately not key-gated (HEP-0033 §11: "any well-formed CURVE
  client may connect"), so an operator resolves to no index entry. That
  is precisely why capture and resolution are separate layers: a plane
  may need to know *which key* without needing to know *which
  registered subject*.
- Federation: see §5.

**Demote.** Frame-0 routing id becomes what the inbox comment already
says it is — a reply address — on every plane, with no gate reading it as
a trust claim.

### Types this composes (nothing new is invented)

| Need | Existing type | Source |
|---|---|---|
| A verified key with its mechanism | `PeerIdentity{mechanism, key}` | `security/peer_admission.hpp` |
| Key → principal | `PubkeyOrigin` | HEP-0035 §4.2, specified, to be built |
| Carrier to the handlers | `WireEnvelope` | `wire_envelope.hpp` |
| Gate context | `AdmissionContext` + callbacks | `admission_gates.hpp` |
| Vault-backed source of truth | `KnownRolesStore` | `security/known_roles.hpp` |

---

## 5. Two principal classes, so federation composes

A strict "claimed identity must equal the connection's identity" rule is
wrong as a universal, and a per-gate patch would have shipped it wrong.
There are two legitimate kinds of caller, and HEP-0035 §4.2 already names
both:

- **`LocalRole`** — a role acting for itself. Strict rule: the claim must
  resolve to this connection's key.
- **`FederationPeer`** — a peer hub relaying messages authored by roles on
  *its* side (`sender_uid` / `originator_uid` in the relay handlers). The
  carried identity belongs to someone else by design and *cannot* equal
  the connection key. What governs it is delegation policy, which
  HEP-0035 §4.3 already defines as three modes (`local_only` default,
  `peer_delegated`, `peer_announced`), none of them built.

Stating the distinction at the resolution layer means the strict rule is
written once and delegation is a declared, auditable property of a link
rather than a hole. Today federation gets its exemption by accident,
because nothing checks anything.

This also gives task #69 a defined trust model instead of an open
question — one of its blockers, resolved as a by-product rather than by a
separate design pass.

---

## 5b. Integration shape — why this is a system, not threaded parameters

The obvious way to build this is also the wrong way: capture the verified
key, then pass it down through handler signatures and check it wherever a
claim appears. That yields identity policy smeared across a dozen call
sites, each free to check slightly differently or to forget. It would pass
review, because every individual check looks correct. Three rules keep the
design whole.

**Resolve at ingress, not at each consumer.** Capture and resolution are
*both* ingress concerns. If the envelope carries only a raw key, every
handler must call the index itself, handle "unknown key," and pick a
rejection code — which is policy, duplicated. The envelope therefore
carries a resolved value, not a key to look up:

```
VerifiedPeer {
    PeerIdentity            connection_key;   // always present: ZAP verified it
    std::optional<PubkeyOrigin> principal;    // absent = key known to ZAP, not a registered subject
}
```

Absence is *meaningful*, not an error: the admin plane is deliberately not
key-gated (HEP-0033 §11), so an operator legitimately resolves to no
principal. A plane that needs "which key" is served without needing "which
subject." **No handler calls `resolve()`. No function grows a
`verified_key` parameter.**

**One gate, not N checks.** The claim comparison belongs in the existing
admission pipeline (`admission_gates.hpp`, `AdmissionContext`) as a single
gate that replaces `gate_identity_match` at the same seam. Registration
handlers do not grow an `if`. The pipeline already orders gates and owns
rejection codes; identity becomes one more row there, evaluated after
resolution (HEP-0046 §14.5 step 0) and before every gate that assumes an
identity.

**Per plane this is substitution, not addition.** This is the clearest
evidence it is a mechanism rather than a patch — only one plane gains a
check at all:

| Plane | Change | Net |
|---|---|---|
| Registration | `gate_identity_match` (routing id vs config — two client-chosen values) replaced by a principal gate | one gate swapped, not added |
| Inbox | attribution / replay key / sequence key move from routing id to principal | **substitution — no new check exists** |
| Admin | session binds to the captured key instead of the `Peer-Address` proxy | one weaker fact replaced by a stronger one |
| Federation | classification becomes a property of the key | replaces an exemption currently granted by accident |

If an implementation of this design ends up adding checks to every plane,
it has drifted and should be reworked, not merged.

### Anti-goals

These would each turn the design back into a patch:

- a handler calling `resolve()` for itself;
- any function signature growing a verified-key parameter;
- a plane re-deriving identity from wire fields once the principal exists;
- new rejection codes (D3: reuse `IDENTITY_MISMATCH` / `PUBKEY_MISMATCH`);
- identity checks living outside the admission pipeline.

### An unhandled case this review surfaced

The CTRL allowlist admits **federation peer hubs** — it must, since a peer
dials this broker's ROUTER (HEP-0022). So a peer's key *does* resolve, to
`Kind::FederationPeer`. Nothing in §4 or §5 says what happens when such a
principal arrives on the **registration** plane claiming a local role.

Under §5 that is delegation, governed by the §4.3 trust modes — **which are
unbuilt**. Leaving it undefined would put the most privileged principal
class in front of an unimplemented policy, which is how holes ship.

**Interim rule, to land with slice 3:** only `Kind::LocalRole` may register.
A `FederationPeer` principal on the registration plane is rejected outright
until §4.3 modes exist, at which point the rejection is replaced by a mode
check rather than removed. The registration gate must therefore test the
principal's *kind*, not merely compare uid strings — a string comparison
would accidentally admit a peer whose `subject_uid` happened to match.

---

## 5c. The sequence, end to end — and what each step licenses

Auth is not one check; it is a chain in which each step is only sound
because the previous one held. Written out, the gate placement stops being
a matter of taste.

```mermaid
sequenceDiagram
    participant R as Role
    participant Z as ZAP handler
    participant B as Broker CTRL
    participant P as Admission pipeline

    Note over R: holds identity keypair (vault)
    R->>B: CURVE handshake
    B->>Z: ZAP request (pubkey)
    Z->>Z: pubkey ∈ allowlist?
    Z-->>B: 200 OK, user_id = Z85 pubkey
    Note over B: libzmq now stamps User-Id on EVERY<br/>message from this connection
    R->>B: REG_REQ (claims uid + pubkey)
    B->>B: CAPTURE verified key from metadata
    B->>B: RESOLVE key → principal (one index)
    B->>P: envelope carries VerifiedPeer
    P->>P: identity gate: claim vs principal
    P->>P: remaining gates (assume identity settled)
    B-->>R: REG_ACK (allowlist, roster, instance_id)
    Note over R,B: every later message repeats CAPTURE+RESOLVE
```

**What each step licenses, and what it does not.**

| Step | Establishes | Does NOT establish |
|---|---|---|
| Handshake + ZAP | this connection holds *an* allowlisted key | *which* subject it is |
| Capture | which key this specific message came from | what that key means here |
| Resolve | which principal that key denotes | whether the claim matches |
| Identity gate | the claim belongs to this connection | anything about authority |
| Later gates | authority, schema, topology | — they *assume* identity is settled |

The ordering is forced: every later gate reasons about "this role", so a
gate that runs before identity is settled is reasoning about an assumption.
That is why resolution is step 0 of the pipeline (HEP-0046 §14.5) and not a
peer of the other gates.

**There is no session to hijack.** Attribution is re-derived per message
from transport-verified metadata, not cached at registration. A registered
role does not acquire a token that later messages present. This is why the
design needs no session-fixation defence: there is no session object.

**Reconnection is the reason principal beats routing id.** A role that
drops and redials gets a *new* routing id and the *same* key. State keyed
on routing id therefore fragments across reconnects (and is attacker-chosen
besides); state keyed on principal survives them. Reconnect-correctness and
anti-impersonation want the same key here, which is a good sign the axis is
the right one.

---

## 5d. The same gap exists twice — and one structure closes both

The broker is not the only ROUTER that authenticates peers. **Every role
with an inbox owns one**, and it has exactly the same defect. This was
missed in the first pass and is the largest remaining piece of work.

`zap_router.cpp` sends the Z85 pubkey as `user_id` for **every** registered
domain, so a role's inbox ROUTER already receives transport-verified sender
keys. `hub_inbox_queue.cpp` contains no reference to that metadata. It
takes `sender_id` from the routing frame and uses it as:

- the **replay-guard key**,
- the **sequence-tracking key**, and
- the **application-visible sender identity** handed to scripts.

All three therefore rest on a value the sender chooses. An authenticated
role may present another role's uid as its routing id and have its messages
attributed to that role — and, worse, poison that role's replay and
sequence state, which is an availability defect on top of an integrity one.

**The role cannot currently fix this even if it read the metadata**, because
of a wire-format limitation: the roster it receives is a flat array of
pubkeys (`known_roles: ["KEY", ...]`, held role-side as a bare
`unordered_set<std::string>`). It carries **no uid↔pubkey binding**, so the
role can decide *"is this an authorized role"* but never *"which one."*

### Protocol change this forces

`REG_ACK` / `CONSUMER_REG_ACK` `known_roles` must carry **bindings, not bare
keys**:

```
"known_roles": [ { "uid": "prod.example.uid…", "pubkey": "<Z85>" }, … ]
```

That is the minimum shape from which a role can build the same key→subject
index the broker builds. Consequences, all of them simplifying:

- the role constructs a `PubkeyOriginIndex` from the roster — **the same
  type, unchanged**, now serving both processes. One structure answers
  "what does this key mean to me" on both sides of the wire;
- the ZAP allowlist stays a projection of that index (`as_peer_allowlist`),
  exactly as on the broker;
- inbox attribution, replay keying, and sequence keying all read the
  resolved principal, and the routing id reverts to being an address.

**This is a wire-compatibility change** and must land as one commit across
broker and role: a new broker emitting bindings to an old role, or the
reverse, must be a clean rejection rather than a silent
mis-parse. Old shape = flat strings, new shape = objects, so the two are
distinguishable by type at parse time — take that as the discriminator
rather than adding a version flag.

---

## 6. What this replaces, retires, or unifies

The proposal is mostly subtraction. That is the test of whether it is a
mechanism or a patch.

| # | Component | Disposition |
|---|---|---|
| 1 | `PubkeyOrigin` / `pubkey_to_origin` (HEP-0035 §4.2) | **Specified, never built** — build it; it is the centre of this design |
| 2 | `gate_identity_match` — routing id vs `role_uid` | Obsolete as a trust gate; both sides are client-chosen. Routing id keeps only its reply-address role |
| 3 | Body field `zmq_pubkey` on registration | **Redundant** once the key is derived from the connection — same value, better provenance. Retire from the wire or demote to a cross-check (decision D2) |
| 4 | Inbox `sender_id` taken from routing id | Replaced by the resolved principal — fixes attribution, replay keying, and sequence tracking in one change |
| 5 | Admin `Peer-Address` "anti-hijack fact" | Redundant — it was a proxy for the property we would now have properly |
| 6 | Three separate projections of the same vault data — `KnownRolesStore::as_peer_allowlist`, the broker's inline loop building `BrokerCtrlAdmission`'s list, and the linear scan in `lookup_known_role` | Collapse to one index that yields both the allowlist and the origin map. The linear scan per registration also disappears |
| 7 | HEP-0036 §6.3 "why body fields and not a User-Id recovery" | Obsolete rationale — replace with the layered model |
| 8 | HEP-0035 §4.1 Layer-2 box | Restore to origin-based language; the body-claim edits are the drift |
| 9 | `federation_trust_mode` + §4.3 modes | Specified, never built — this design is their prerequisite |
| 10 | `role_identity_policy.hpp` filename | Residue from the deleted string gate; already has an in-file rename note |

Items 2, 3, 4, 5, 6 are live code that gets simpler. Items 1 and 9 are
design debt that gets paid. Items 7 and 8 are documentation that is
currently wrong in a way that misleads security work.

---

## 7. Conflicts found in the existing HEP corpus

| Conflict | Detail | Resolution |
|---|---|---|
| HEP-0036 §6.3 vs §6.6.1 | One says body-claim verification is equivalent; the other says caller identity *is* derived from the verified handshake, "same as any other broker REQ" | §6.6.1 is correct and becomes the general rule; §6.3's rationale paragraph is deleted |
| HEP-0035 §4.1 vs §4.2 | The Layer-2 box describes body-claim checking; §4.2 describes an origin index read by Layer 2 | §4.2 is the design; §4.1's box is restored |
| HEP-0035 §4.2 vs shipped code | Index specified as "single source of truth," never built; three ad-hoc projections exist instead | Build the index; retire the projections |
| HEP-0027 vs registration plane | Inbox documents routing id as "no longer a trust claim" while using it for replay keying and attribution; registration treats it as a trust claim | One meaning: reply address. Identity comes from the principal |
| HEP-0035 §4.3 vs federation code | Three trust modes defined; none exist; relay accepts third-party identities unconditionally | Modes become implementable once origin classification exists |

No conflict was found between this design and HEP-0041/0044 (SHM attach),
HEP-0042 (channel attach), or HEP-0043 (security subsystem). The data
plane authenticates by key directly and carries no identity claims — it
is already the model this proposal brings to the control planes.

---

## 8. Decisions (all resolved by the owner, 2026-07-27)

- **D1 — resolution strategy.** Reverse index (HEP-0035 §4.2 as
  specified) versus claim-and-compare. Analysis in §9.
- **D2 — the `zmq_pubkey` wire field. ✅ DECIDED 2026-07-27: keep it.**
  The registration request stays explicit about the key it believes it
  is using, and the broker checks that belief against the key the
  connection actually authenticated with. The field is not load-bearing
  for security — the connection is — but an explicit request is easier
  to read and the mismatch it catches produces a precise diagnostic
  instead of a generic identity rejection.
- **D3 — reject-code taxonomy.** With D2 decided, two distinct failures
  are now separately detectable, and each has a different owner and a
  different fix. See §9.1.
- **D4 — scope of the first landing.** All three control planes at once,
  or registration first? A half-landing leaves the inbox trusting a
  self-asserted sender, which is hard to document honestly.

---

## 9. D1 — reverse index versus claim-and-compare

**Claim-and-compare:** on a registration, look up `known_roles[claimed_uid]`
(already done today) and additionally require the stored key to equal the
connection's verified key.

**Reverse index:** build key → principal once at vault load; every plane
resolves the connection's key to a principal and uses that.

| | Claim-and-compare | Reverse index |
|---|---|---|
| Registration plane | Works | Works |
| Inbox plane | **Cannot work** — an inbox frame carries no claimed uid, so there is nothing to compare against; the sender's identity has to be *derived* | Works — derivation is what the index does |
| Federation classification | **Cannot work** — "is this connection a peer hub or a role?" is a property of the key, not of any claim | Works — it is the `Kind` field |
| Admin plane | Partial — needs an operator identity that today does not exist as a claim | Works |
| Matches the design of record | No — HEP-0035 §4.2 specifies the index | Yes |
| Cost | One comparison per registration | One map built at vault load; one lookup per message |
| Number of mechanisms | Two (compare here, something else for the inbox) | One |

The decisive point is not cost, it is coverage: claim-and-compare cannot
serve the two planes that have no claim to compare — inbox attribution
and federation classification. Choosing it would fix registration and
leave the inbox needing a second, different mechanism, which recreates
the per-plane divergence this proposal exists to remove.

**Recommendation: reverse index**, because it is the specified design, it
is the only option that covers all four planes with one mechanism, and it
is the prerequisite for the federation trust modes that are already
written down.

---

### 9.1 D3 — two failures, two owners, two codes

Because D2 keeps the announced key on the request, three facts meet at
the gate, and comparing them pairwise separates two genuinely different
problems.

The three facts:

- **K** — the key this connection actually authenticated with. Proven.
- **U**, **P** — the role uid and key the request announces. Claims.
- **S** — the role the hub's known-roles list says key K belongs to.

Two checks, each with a distinct meaning:

| Check | Fails when | What it means | Who fixes it |
|---|---|---|---|
| `U == S` | A caller authenticated as one role and registered as another | Impersonation, or a role deployment whose identity file and uid belong to different roles | Whoever deployed the role |
| `P == K` | The request announces a key that is not the one the connection is using | The role's own belief about its identity is internally inconsistent — announcement and key file disagree | Whoever configured the role |

**No new reject codes are needed, and neither existing code changes
meaning:**

- `IDENTITY_MISMATCH` already exists and already means "you are not who
  you claim." It currently guards the routing-id comparison, which this
  design retires; the code survives its gate and takes the `U == S`
  check, which is what it always should have guarded.
- `PUBKEY_MISMATCH` keeps the `P == K` check — an announced key that
  disagrees with the key in use. This is close to its present-day
  wording and stays actionable.

Note what disappears: the current gate compares the *announced* key
against the hub's stored entry for the *announced* uid — two claims
checked against a list, with the live connection ignored. Under this
design every comparison has the proven key on one side.

## 10. Implementation sequence (after ratification)

Each slice is independently testable and leaves the tree green.

| # | Slice | State |
|---|---|---|
| 1 | **Index** — build `pubkey_to_origin`, collapse the duplicate projections | ✅ **SHIPPED** `f8ba8927`; wire-seam pin `1c805e84`. Five projections found (not three); four consolidated. Pure consolidation, no behaviour change. |
| 2 | **Capture** — carry the verified key on `WireEnvelope` at every router ingress, observe-only | ⬜ pending |
| 3 | **Registration enforcement** — resolve, compare, reject; retire `gate_identity_match` | ⬜ pending — **this is the commit that closes the gap** |
| 4 | **Inbox** — roster carries `(uid, pubkey)` bindings; role builds its own index; principal replaces routing id for attribution, replay keying, sequence tracking | ⬜ pending — **larger than first scoped**: a wire-format change landing across broker and role in one commit (§5d) |
| 5 | **Admin** — session binds to the captured key; retire the peer-address proxy | ⬜ pending |
| 6 | **Federation** — origin classification + §4.3 modes | ⬜ pending, lands with #69 |

Everything shipped so far is foundation. **The exposure described in §1 is
unchanged until slice 3 lands.** Slice 1 built the structure that answers
"what does this key mean"; no caller yet asks it that question about a
live connection.

One gap in the coverage, not in the code: no test yet drives a peer that
holds a *valid* key while claiming *another* role's identity and asserts
the rejection. That is the test that would have caught the original
defect, and it can only be written once slice 3 exists. Pinning index
contents is not the same as pinning that impersonation fails.

Slice 2 deliberately lands observation before enforcement: it turns "does
anything in this system legitimately speak for another identity?" from a
question into log evidence, before any rejection can break a real
deployment.

### Test-harness consequence

`BrokerWireClient` carries a field documented as *"the role_uid this wire
client masquerades as."* Masquerading is precisely what this change makes
impossible. Every such test needs a real provisioned key per identity —
which is exactly what task #66 already asks for. The two merge: the
security fix makes the test anti-pattern structurally unavailable rather
than merely discouraged.

---

## 11. Invariants this design establishes

Each is stated so a reviewer can find a violation by inspection, and each
names what breaks if it is dropped.

**I-IDENTITY-FROM-HANDSHAKE.** Every identity a control-plane handler acts
on derives from the connection's transport-verified key. Never from a body
field, never from a routing id.
*Violated by:* any handler that reads `zmq_pubkey`, `sender_uid` or frame 0
to decide who is speaking.

**I-RESOLVED-AT-INGRESS.** Capture and resolution happen once, where the
message enters. Handlers receive a resolved principal; no handler resolves
for itself, and no signature carries a bare verified key.
*Violated by:* a `resolve()` call outside ingress; a `verified_key`
parameter. *Dropped it becomes:* identity policy duplicated per call site.

**I-ONE-ORIGIN-INDEX-PER-PROCESS.** Exactly one structure answers "what
does this key mean here," and every consumer — ZAP allowlist, admission
gates, inbox attribution — is a *projection* of it, never a second
derivation. Both the broker and each inbox-owning role hold one; they are
separate instances of the same type, populated from the same operator
truth. (Sharpened: the earlier wording said "exactly one structure", which
read as one per system and is wrong now that roles hold one too.)
*Violated by:* a second walk over the roster building a parallel map.

**I-ROUTING-ID-IS-AN-ADDRESS.** Frame 0 is a reply address on every plane.
No gate, key, or attribution may read it as a trust claim.
*Violated by:* any map keyed on routing id — today
`sender_expected_seq_`, the inbox replay-guard key, and
`current_sender_id_` all are.

**I-STATE-KEYED-ON-PRINCIPAL.** Per-peer state that must survive a
reconnect or resist forgery — replay windows, sequence counters, session
binding — is keyed on the principal, not on a transport address.
*Why both properties want this:* a redial yields a new routing id and the
same key, so principal-keying is simultaneously the reconnect-correct and
the unforgeable choice.

**I-ROSTER-CARRIES-BINDINGS.** Any roster distributed so a peer can
authorize others carries `(uid, pubkey)` pairs, never bare keys. A bare-key
roster can answer "is this someone we know" but never "who is this," which
silently forces the recipient back onto a forgeable identifier.
*This invariant is currently violated by the wire format* (§5d).

**I-DELEGATION-IS-DECLARED.** An identity differing from the connection's
principal is accepted only across a link explicitly classified as a
federation peer, under a declared trust mode. Until those modes exist, a
federation-peer principal is refused on the registration plane rather than
silently permitted (§5b).

**I-ABSENCE-IS-MEANINGFUL.** A verified key that resolves to no principal
is a legitimate state (the admin plane is deliberately not key-gated,
HEP-0033 §11), not an error. Planes that require a principal reject
explicitly; planes that do not, proceed on the key alone.

---

## 12. Risks and obsolete residues

Detected by walking the chain in §5c against current code.

### Live risks

| Risk | Where | Disposition |
|---|---|---|
| **Inbox attribution, replay key and sequence key all derive from the routing id** | `hub_inbox_queue.cpp` `sender_id` at recv | The most serious remaining hole: an authenticated role can be attributed as another AND poison that role's replay/sequence state (integrity *and* availability). Slice 4. |
| **Roster carries bare keys** | `REG_ACK`/`CONSUMER_REG_ACK` `known_roles`; role-side `unordered_set<std::string>` | Makes the above unfixable role-side. Protocol change, §5d. |
| **Federation peer on the registration plane** | admission pipeline | Undefined today; peers resolve but §4.3 modes are unbuilt. Interim: refuse by kind (§5b). |
| **Verified key discarded at every ingress** | broker and role ROUTERs | ZAP sets `user_id` for every domain; nothing above the socket reads it. This is the enabling defect for all of the above. |

### The `zmq_pubkey` request field — declaration, with consistency required

Nothing trusts this field. The role is authorized by the connection's
verified key resolving to a principal; the body field is a declaration that
must be consistent with what is already known, and every check over it
denies:

| Condition | Outcome | Enforced |
|---|---|---|
| absent, or length ≠ 40 | deny | today, `gate_grammar` |
| `(role_uid, zmq_pubkey)` is not a registered pair | deny | today, known-role binding |
| announced key ≠ **connection's verified key** | deny (`PUBKEY_MISMATCH`) | **slice 3 — the missing leg** |
| claimed uid ≠ connection principal's subject | deny (`IDENTITY_MISMATCH`) | **slice 3** |

The two existing checks compare the claim against the operator's roster.
What is missing is the leg comparing it against the *connection* — that
absence is the defect this design closes.

Why D2 keeps the field: the request is self-describing rather than leaning
on out-of-band context, and a mismatch distinguishes config drift (a role
whose vault and config disagree) from an attack, which an operator acts on
differently.

### Obsolete residues

| Residue | Status |
|---|---|
| `gate_identity_match` — routing id vs configured uid | Compares two client-chosen values; proves nothing. Retire at slice 3. |
| Admin `Peer-Address` "anti-hijack fact" | A proxy for the property we will hold properly. Retire at slice 5. |
| `lookup_known_role` linear scan per registration | Superseded by the index; slice 3 rewrites its semantics. |
| `role_identity_policy.hpp` filename | Residue of a deleted string gate; rename when slice 3 touches it. |
| HEP-0036 §6.3 "why body fields, not User-Id recovery" | Already withdrawn — the rationale argued against the mechanism now adopted. |
| `KnownRolesStore::as_peer_allowlist` | Already deleted (slice 1); contracts handed to `test_pubkey_origin.cpp`. |

### What the HEP must carry when this is promoted

Per owner direction, the permanent text is to be written around:

- **a unified API** — one `resolve` surface, one `VerifiedPeer` type, used
  identically by broker and role;
- **a concealed data structure** — the index is pImpl'd; consumers see
  projections (`as_peer_allowlist`, `local_role_pubkeys`, `resolve`) and
  never the container. This is what keeps "one index" enforceable;
- **a formatted exchange protocol** — the roster's `(uid, pubkey)` binding
  shape stated normatively, with the old bare-key form named as rejected
  rather than merely superseded;
- **explicit contract** — the §11 invariants, each with its violation
  signature, plus the licensing table in §5c that fixes gate ordering.
