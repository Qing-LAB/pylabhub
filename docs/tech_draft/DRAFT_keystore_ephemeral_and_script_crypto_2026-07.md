# KeyStore ephemeral keys + script crypto API — tech draft

> **Status:** DRAFT, 2026-07-03.  Refreshed 2026-07-06 for the
> SEC-Fold-2 KeyStore merger.  Captures design decisions from the
> broker observer path (#317 D1).  Will promote to HEP-CORE-0043 §10
> (script crypto) once script bindings land under task #247.
> **Scope of the immediate work:** one narrow KeyStore extension for
> the broker observer use case, everything else deferred.
>
> **2026-07-06 correction to the code references below.**  Post-
> SEC-Fold-2, KeyStore is a MEMBER of `SecureSubsystem::Impl`
> (HEP-CORE-0043 §2.2 + §7).  All `key_store().X()` references below
> should be read as `secure().keys().X()` (or equivalently through
> the transitional `key_store()` inline shim).  The `generate_and_
> add_identity(name)` method described in this doc SHIPPED under
> `secure().keys().generate_and_add_identity(name)` semantics — the
> API surface didn't change, only the container.

## Background

`KeyStore` (HEP-CORE-0043 §7, formerly HEP-CORE-0040 §5) stores keypairs
and raw secrets under a `name → LockedKey` map, with the seckey
accessible only through `with_seckey(name, callback)` (use-not-export).
Entries are added via `add_identity` (raw 64-byte pack) or
`add_identity_from_z85` (Z85 pair) — both assume the caller has the
keypair already in hand from a config file or vault.

There is no on-the-fly path: framework code that needs a fresh keypair
for runtime-lifetime use (broker's observer identity, future ephemeral
mutual-auth keys) has to reach for `generate_curve_keypair()`,
manually pack the bytes, and call `add_identity` — three steps that
have to individually get the `sodium_memzero` discipline right on
every intermediate buffer.

The broker observer path (#317 C.2 D1) is the first framework consumer
that wants an ephemeral keypair.  Adding one narrow method now buys
the correctness for that use case AND sets up the shape for the
larger crypto API scripts will eventually want (task #247).

## Decisions locked (2026-07-03 conversation)

### Storage naming is caller's business, not the module's

KeyStore is a name-keyed map.  Callers pick their own names; storage
does not enforce any prefix / sandbox / naming rule.

The reason: when script bindings eventually land (task #247), the
binding layer TRANSLATES script-provided names into sandboxed storage
names before calling KeyStore.  For example:

```
Script:   api.crypto.new_keypair("mykey")
Binding:  keystore.generate_and_add_identity("script." + script_uid + "." + "mykey")
Storage:  sees "script.role_uid_1234.mykey" — no rule enforcement needed
```

Scripts never see the storage name.  Framework consumers use whatever
names they want (`"broker.observer"`, `"broker.identity"`, etc.).
Cross-contamination is impossible because the binding layer owns the
translation.

**Implication:** the new method takes a `std::string_view name` and
does nothing special with it — same shape as `add_identity`.  Rules
land only if/when they're actually needed (they're not needed today).

### Threat model for ephemeral keys

- **Broker observer key:** generated at broker startup.  Lives in
  mlocked memory (via existing LockedKey machinery).  Wiped on broker
  shutdown.  Broker publishes only the pubkey — on REG_ACK per HEP
  §D1(d).  Producer stores the pubkey to verify future observer
  handshakes.  On broker restart, new keypair; producers relearn on
  next REG_ACK.
- **Compromise scope:** if the broker's observer seckey were extracted
  from memory (kernel exploit + `/proc/<pid>/mem` + `sodium_mprotect`
  bypass), an attacker could impersonate the broker to producers as an
  observer.  Observers are read-only (header-only fd handoff); worst
  case is metadata harvesting.  Not a data-plane compromise.
- **Not the broker's identity key.**  Observer key is distinct from
  the ZMQ CURVE identity keypair.  Rotating the observer key (e.g., on
  every startup) does not disrupt existing ZMQ sessions.

### API surface — narrow now, wide later

**Immediate (this commit):**

```cpp
/// Generate a fresh CURVE keypair in-memory and register it under `name`.
/// Returns the Z85 pubkey; the seckey is accessible only via
/// `with_seckey(name, ...)`, per the HEP-CORE-0040 use-not-export
/// contract.
///
/// The keypair lives for the KeyStore's lifetime (typically the
/// process lifetime).  No disk write anywhere; libsodium primitives
/// operate on mlocked LockedKey memory.
///
/// Callers pick their own names — storage enforces no naming rules.
/// When the script crypto API lands (task #247), script bindings
/// will translate script-provided names into sandboxed storage names
/// before calling this method.
///
/// Throws `std::runtime_error` if `name` already present, if libsodium
/// / libzmq CSPRNG init fails, or if `sodium_malloc` fails.
[[nodiscard]] std::string
generate_and_add_identity(std::string_view name);
```

Internally: allocates two `SecureBuffer<32>` on the stack for raw
pub+sec, calls `zmq_curve_keypair` into those buffers, encodes the
pubkey to Z85 for the return value, packs raw 64-byte pub||sec, calls
`add_identity` (which zeros the source buffer via existing
discipline), zeroes any remaining stack copies.

The pubkey return value is Z85 40-char string — non-secret, safe to
copy, log, put on the wire.

**Deferred to task #247 (script crypto API):**

- `api.crypto.new_keypair(name)` script binding.
- `api.crypto.pubkey(name)` — returns Z85 pubkey.
- `api.crypto.box_easy(name, plaintext, nonce, peer_pubkey)` — encrypt
  using stored seckey without exposing it.
- `api.crypto.box_open_easy(name, ciphertext, nonce, peer_pubkey)` —
  decrypt.
- Vault persistence for script-generated keys (`api.vault_save(name)`).

These require:
- Language bindings (Python + Lua + Native).
- Name-translation layer per language (per-script sandboxing).
- Threading discipline audit — the existing `with_seckey` shared-lock
  contract is fine for framework consumers that hold the callback for
  microseconds; script callbacks may want a longer-hold pattern.
- Decision on whether script-generated keys can be exported to the
  wire without vault involvement.

Scope for #247 is significant (task-level) but the shape today does
not foreclose it.

## Immediate concrete work — #317 D1

1. **Add `generate_and_add_identity` to KeyStore** (this doc's core proposal).
2. **Broker startup calls it once** with name `"broker.observer"`.
3. **Broker REG_ACK builder emits** new field `broker_observer_pubkey_z85`
   (Z85 pubkey returned by step 1).
4. **Producer's `apply_producer_reg_ack` stashes** the pubkey on
   `RoleAPIBase` via `set_broker_observer_pubkey_z85` (new setter,
   symmetric with `set_shm_require_mutual_auth` from #262 wiring).
5. **Producer's `AttachProtocolAcceptor` gets** a "known observer pubkey"
   parameter (or accessor callback into `RoleAPIBase`).  When Frame 2
   arrives with `role_type="observer"`, acceptor runs `crypto_box_open_easy`
   against that stored pubkey.  This replaces the current
   "not-yet-implemented" throw at `attach_protocol.cpp:434`.
6. **Broker-side dial code** (#317 C.2.b, later slice): broker uses
   its OWN seckey (via `keystore.with_seckey("broker.observer", ...)`)
   to sign the challenge the producer sends.  Same pattern as the
   consumer/producer mutual-auth challenge-response.

Steps 1–5 form the D1 landing.  Step 6 depends on D2–D5 decisions
(pending in the conversation).

## Open decisions still ahead (D2–D5)

Filed here for continuity; not decided yet:

- **D2 — allowlist storage on producer:** where does the pubkey live?
  Constructor arg on `AttachProtocolAcceptor` (static per-channel)?
  Settable field on `RoleAPIBase` (dynamic)?  KnownRolesStore lookup?
- **D3 — broker dial timing:** immediately after REG_REQ success?  Lazy
  on first BROKER_SHM_INFO_REQ?  Per-query?
- **D4 — producer-death cleanup:** heartbeat-timeout path?  EPOLLHUP on
  observer socket?  Both?
- **D5 — producer opt-out knob** (`producer.shm_metrics_observer`):
  already agreed in scope.  Just impl.

Answered when the conversation continues.

## Promotion path

Once we have:
- Broker observer wired end-to-end (D1–D5 all landed).
- Script crypto API drafted under task #247.
- At least one production consumer of the ephemeral pattern.

Then promote this draft to `HEP-CORE-0043 KeyStore Extensions` (or
amend HEP-CORE-0040 §8.6 with a new subsection on ephemeral keys).
The promotion authoring pass should:

- Codify the naming-translation contract (script bindings vs
  framework callers).
- Formalize the "seckey never crosses the module boundary" invariant
  as a §8.6 contract line, symmetric with §5.2's use-not-export.
- Enumerate the deferred wire additions from task #247 into the wire
  protocol tables.

## Related tasks

- **#317** — parent task; C.2 D1 is the immediate consumer.
- **#247** — script crypto API; the future consumer this draft
  anticipates but does not implement.
- **#262** — mutual-auth work; wire mechanism ships uses
  config-file-loaded identity keys today.  Future ephemeral-identity
  path (rotate producer keypair on every session) would build on the
  same extension.
- **HEP-CORE-0040** — the current KeyStore HEP, where the invariants
  in §5.2 and §8.5.2 sit.

## Change log

- **2026-07-03 (initial draft):** captures conversation decisions
  during #317 D1 design.  Naming rules explicitly deferred to script
  binding layer.  Ephemeral generate + in-memory-only is the sole
  KeyStore addition in this scope.
