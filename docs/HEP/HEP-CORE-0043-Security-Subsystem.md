# HEP-CORE-0043: Security Subsystem

| Property        | Value |
|-----------------|-------|
| **HEP**         | `HEP-CORE-0043` |
| **Title**       | Security Subsystem — unified module + HEP consolidation |
| **Status**      | 🚧 **DRAFT — SEC-Fold-1 landing.**  §0-§2 (architecture + module fold design) authoritative and ready for review.  §3-§10 (per-primitive + per-protocol) cite the existing HEPs (0036, 0038, 0040, 0041) authoritatively for full detail; those HEPs are marked **SUPERSEDED-STATUS-ONLY** — their content remains authoritative until §3-§10 fully migrate content (SEC-Fold-1b, follow-up). |
| **Created**     | 2026-07-04 |
| **Area**        | Framework Architecture (security module, libsodium ownership, key management, wire auth) |
| **Depends on**  | HEP-CORE-0001 (Hybrid Lifecycle Model), HEP-CORE-0031 (ThreadManager pattern) |
| **Related**     | HEP-CORE-0035 (Hub-Role Auth + Federation Trust — NOT folded; stays independent per SEC-Fold R5), HEP-CORE-0042 (Channel Attach Coordination — NOT folded; stays independent per SEC-Fold R5) |
| **Supersedes**  | **HEP-CORE-0036** (Authenticated Connection Establishment) → §9.1 of this HEP; **HEP-CORE-0038** (Script-Accessible Vault Keystore) → §8 + §10; **HEP-CORE-0040** (Locked Key Memory) → §2 + §7; **HEP-CORE-0041** (SHM Channel Auth) → §9.2 + §9.3 |

---

## 0. Status + scope

### 0.1 Why this HEP exists

2026-07-04 triage of a CI failure (sodium_init gate missing on a
test worker path) exposed the underlying design smell — libsodium
is called from 9 files, security concerns are spread across 6 HEPs,
and no single module owns the crypto surface.  Runtime bugs
(startup ordering assumptions), design questions requiring 3-HEP
paging, and no compile-time guarantee that consumers use sodium
correctly.  All three problems close STRUCTURALLY (compile-time,
not runtime-checked) after this HEP + SEC-Fold-2's C++ refactor
lands.

Full triage narrative + design self-review R1-R8:
`docs/tech_draft/DRAFT_security_module_and_hep_consolidation_2026-07.md`.

### 0.2 What this HEP covers

- **§1 Design principles** — five load-bearing principles the
  security subsystem enforces.
- **§2 Module surface** — the C++ class shape (`SecureSubsystem`),
  lifecycle registration, cross-platform layering.  This section
  is the SEC-Fold-2 refactor spec.
- **§3-§7 Cryptographic primitives** — random, hash, KDF,
  symmetric AEAD, asymmetric box, and the `KeyStore` submodule.
- **§8 Vault at rest** — file format, salt, path discipline.
- **§9 Wire authentication protocols** — ZMQ CURVE + ZAP, SHM
  channel handshake, broker SHM observer.
- **§10 Script-facing crypto API** — the language binding
  translation layer.
- **§11 Cross-platform status** — per-OS backend matrix.
- **§13 Superseded HEPs** — mapping table.

### 0.3 What this HEP does NOT cover

- **HEP-CORE-0035 (Hub-Role Auth + Federation Trust)** — stays
  independent.  It's about multi-hub federation identity + trust
  policy, not security primitives.  §9.1 cross-references it.
- **HEP-CORE-0042 (Channel Attach Coordination Protocol)** —
  stays independent.  It's about wire ordering and instance-epoch
  guards, not security.  §9.2 cross-references it.
- **Federation crypto**, **hub-to-hub trust chains** — future
  work, will land in HEP-0035 amendments.

### 0.4 Migration status (2026-07-04)

- **§0-§2** — authoritative; new content addressing the
  scattered-design problem.  Ready for review.
- **§3-§10** — SECTION STUBS with pointers to existing HEPs
  (0036, 0038, 0040, 0041) as the authoritative source of full
  detail until content migration completes.  Old HEPs marked
  **SUPERSEDED-STATUS-ONLY** — their content remains valid, but
  the design contract now lives here.
- **§11-§13** — supporting.

---

## 1. Design principles

Five principles.  Every implementation contradicting them is wrong;
every design discussion re-litigating them is wasted.

### 1.1 Single-module ownership of libsodium

**One module owns every `#include <sodium.h>` in production code.**
Nothing else in the codebase includes sodium headers or calls
sodium primitives directly.  The module — `SecureSubsystem` —
exposes typed wrappers for every primitive the codebase needs
(§3-§7).

Consequences:
- **The sodium_init gate becomes structural** (compile-time,
  unreachable without going through the module) instead of runtime-
  checkable-but-easy-to-miss.
- **libsodium version knowledge concentrates** in one file.
- **Testing has one mock target**, not nine.
- **When Windows / macOS SHM backends land**, sodium interactions
  concentrate in one file.

Enforcement mechanism: the C++ refactor (SEC-Fold-2) rewrites every
existing sodium call to go through `SecureSubsystem`.  After that
refactor, `#include <sodium.h>` outside `src/utils/security/` is
grounds for CI failure (enforced by a follow-up lint rule).

### 1.2 Use-not-export for secret bytes

**Secret bytes never leave the KeyStore's mlocked memory region as
data.**  Consumers pass a KeyStore entry NAME and a use-callback;
the callback runs against a `span<const std::byte>` view whose
lifetime ends when the callback returns.  Bytes are never
materialized into a `std::string` copy, a heap buffer, or any
storage the module doesn't own.

This principle was established in HEP-CORE-0040 §5.2 as the
NORMATIVE contract for `KeyStore::with_seckey`.  It extends to
`SecureSubsystem`'s asymmetric operations (§6): `box_seal_using`
and `box_open_using` take a name, not a raw seckey.  The seckey
is dereferenced INSIDE the module, encrypts INSIDE the module,
never crosses the module boundary.

Consequence for API design: no `SecureSubsystem` method takes a
raw `std::span<const std::byte, 32>` seckey argument.  Ever.  If a
future feature needs to encrypt with a caller-owned seckey, the
seckey enters the module via `KeyStore::add_raw` first.

### 1.3 Init gate at module boundary (compile-time, not runtime)

**The `SecureSubsystem` module gates every libsodium call at its
public API boundary.**  Consumers cannot compile a call to sodium
outside the module.

This principle was VIOLATED prior to 2026-07-04 (multiple files
called `sodium_init` themselves + called sodium primitives without
any check).  Ripped out in commit `9d0a7eb4`; the module-boundary
runtime gate is a stopgap.  The SEC-Fold-2 refactor makes it
structural.

Compile-time enforcement: after SEC-Fold-2, `<sodium.h>` is
included ONLY in `src/utils/security/*.cpp`; violations are caught
by CI lint.

### 1.4 Rotation & lifetime

Two classes of keys with different lifetime semantics:

- **Long-term identity keys.**  Loaded from vault at process
  startup, live for the process's lifetime, never rotated during
  a session.  Rotation happens by re-running `plh-cli keygen` +
  redistributing vaults.  Examples: hub identity, role identity.
- **Ephemeral keys.**  Generated on-the-fly at process startup or
  during runtime, no vault persistence, wiped at process shutdown
  or removal.  Examples: broker's observer keypair (regenerated
  every broker restart), future ephemeral session keys, script-
  generated keypairs.

Both live in `KeyStore` under distinct name prefixes (see §7 for
naming convention).  The module doesn't distinguish at the storage
level; the distinction is who OWNS the naming: framework
(identity) vs runtime code (ephemeral).

Broker observer keypair is the exemplar ephemeral key: HEP-0041
§D1(d), shipped in task #317 D1 slice A (commit `d6f5d621`).

### 1.5 Cross-platform layering

Sodium primitives are available identically on Linux, FreeBSD,
macOS, and Windows.  Sodium doesn't need per-OS abstraction; the
module wraps once.

Platform-specific concerns concentrate at TWO other layers:

- **`SecureSubsystem` startup platform hardening** — core dumps,
  PR_SET_DUMPABLE, RLIMIT_MEMLOCK, SeLockMemoryPrivilege.  Per-OS
  logic in `secure_memory_subsystem.cpp` `disable_core_dumps_or_
  throw` + `inspect_memlock_capability`.
- **Wire protocols (§9)** — SHM channel auth uses AF_UNIX +
  SCM_RIGHTS on Linux; kqueue on BSD; Windows AF_UNIX or named
  pipes.  See per-protocol subsections + HEP-0041 for Linux
  reference implementation.

The `SecureSubsystem` class itself is portable; per-OS code lives
in the two layers above.

---

## 2. Module surface

**SEC-Fold-2 refactor spec.**  This section is the authoritative
design for the C++ class shape and lifecycle registration.
Implementers of SEC-Fold-2 build to this contract.

### 2.1 The `SecureSubsystem` class

**Name.**  `SecureSubsystem` (renamed from
`SecureMemorySubsystem` per SEC-Fold R2 (c)).  Old name kept as a
type alias for the SEC-Fold-2 transition window; deleted after
all callers migrated.

Header path: `src/include/utils/security/secure_subsystem.hpp`
(new); old `secure_memory_subsystem.hpp` is preserved as a shim
until callers migrate.

**Public surface** (sketch — final API refined during SEC-Fold-2
implementation):

```cpp
namespace pylabhub::utils::security {

class PYLABHUB_UTILS_EXPORT SecureSubsystem
{
public:
    // ── Lifecycle ─────────────────────────────────────────────
    SecureSubsystem();
    ~SecureSubsystem();
    SecureSubsystem(const SecureSubsystem &)            = delete;
    SecureSubsystem &operator=(const SecureSubsystem &) = delete;
    SecureSubsystem(SecureSubsystem &&)                 = delete;
    SecureSubsystem &operator=(SecureSubsystem &&)      = delete;

    // ── Random + hash (§3) ────────────────────────────────────
    void random_bytes(std::span<std::byte> out);
    std::uint64_t random_u64();
    std::string uuid4();
    std::array<std::byte, 32> blake2b(std::span<const std::byte> data);

    // ── KDF (§4) ──────────────────────────────────────────────
    std::array<std::byte, 32> pwhash(std::string_view password,
                                     std::span<const std::byte> salt);

    // ── Symmetric AEAD (§5) ───────────────────────────────────
    void secretbox_seal(std::span<std::byte> out,
                        std::span<const std::byte> plaintext,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte, 32> key);
    bool secretbox_open(std::span<std::byte> out,
                        std::span<const std::byte> ciphertext,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte, 32> key);

    // ── Asymmetric box (§6) — use-not-export via KeyStore name.
    void box_seal_using(std::string_view seckey_name,
                        const Z85PublicKey &peer_pubkey,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte> plaintext,
                        std::span<std::byte> out);
    bool box_open_using(std::string_view seckey_name,
                        const Z85PublicKey &peer_pubkey,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte> ciphertext,
                        std::span<std::byte> out);

    // ── Constant-time helpers ─────────────────────────────────
    bool memcmp_ct(std::span<const std::byte>,
                   std::span<const std::byte>);
    void memzero(std::span<std::byte>);

    // ── KeyStore submodule (§7) ───────────────────────────────
    KeyStore &keys();
    const KeyStore &keys() const;

    // ── Readiness probe (superseded post-SEC-Fold-2) ──────────
    [[nodiscard]] bool sodium_initialized() const noexcept;

    struct Impl;
private:
    std::unique_ptr<Impl> pImpl;
};

// Global accessor — throws if not constructed.
[[nodiscard]] PYLABHUB_UTILS_EXPORT SecureSubsystem &secure();

// Non-throwing probe.  Post-SEC-Fold-2 this is not needed
// (structural gate); pre-fold callers use it for readiness.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool secure_ready() noexcept;

} // namespace pylabhub::utils::security
```

**R2 (naming) resolution.**  `SecureSubsystem` chosen (not `Secure`
which was too generic, not `Security` which collides with narrower
Authentication scope).  Accessor `secure()` matches the tightness
of `thread_manager()` and `key_store()` conventions elsewhere.

**R7 (LockedKey) resolution.**  `LockedKey` is an implementation
detail of `KeyStore`, never exposed at `SecureSubsystem`'s public
surface.  Its declaration stays in `key_store.cpp` (anonymous
namespace).

**R8 (Z85PublicKey) resolution.**  `box_seal_using` /
`box_open_using` accept `Z85PublicKey` directly, matching the
existing strong-type discipline from task #231.  The Z85 → raw
decode happens once inside the module.

### 2.2 `KeyStore` submodule

**Ownership.**  `KeyStore` is a member of `SecureSubsystem`
(NOT a separate lifecycle module).  Access is via
`secure().keys()`.  The old free-function `key_store()` accessor
is preserved as a shim during SEC-Fold-2 transition.

**R1 (static vs dynamic) resolution.**  `SecureSubsystem` is
STATIC (constructed at `main()` entry, holds no keys initially).
`KeyStore` INSIDE it is logically DYNAMIC (mutations tracked, key
inserts happen at arbitrary runtime moments).  The two-phase
lifecycle is preserved WITHOUT a separate lifecycle module
registration — the map is a member of `SecureSubsystem`, mutated
via `secure().keys().add_*` calls.  Lifecycle manager sees ONE
module ("SecureSubsystem"), not two.

**Public API** (surface preserved from HEP-CORE-0040 §5.2, methods
now on `KeyStore` accessed via `secure().keys()`):

- `add_identity(name, packed_pub_sec)` — 64 raw bytes.
- `add_identity_from_z85(name, pub_z85, sec_z85)` — Z85 pair.
- `add_raw(name, plaintext)` — HEP-0038 script-vault-shaped raw
  secret.
- `generate_and_add_identity(name)` — on-the-fly keypair
  (shipped `d6f5d621`).
- `remove(name)` — idempotent.
- `pubkey(name)` — Z85, non-secret.
- `with_seckey(name, callback)` — raw 32 bytes, use-not-export.
- `with_seckey_z85(name, callback)` — Z85 40 chars, use-not-export.
- `with_keypair_z85(name, callback)` — both halves.
- `lookup_raw(name)` — HEP-0038 raw span.
- `has(name)`, `size()`.

Full contract: HEP-CORE-0040 §5 until migrated to §7 of this HEP
(SEC-Fold-1b).

### 2.3 Lifecycle registration

**One lifecycle module: `"SecureSubsystem"`** (renamed from
`"SecureMemory"`).  Dependency: `"pylabhub::utils::Logger"`.
Startup thunk: no-op (real work in ctor).  Shutdown thunk: no-op
(irreversible; core dumps stay disabled to process exit).

Post-SEC-Fold-2 the old `"KeyStore"` dynamic module is DELETED —
its work is absorbed into `SecureSubsystem`.

Ordering invariant (unchanged from HEP-CORE-0040 §4.5):
`SecureSubsystem` must be constructed BEFORE any consumer of
`secure()`.  Standard call site: very early in `plh_role` /
`plh_hub` `main()`, immediately after the Logger module is up,
before vault open.

### 2.4 Cross-platform stubs

Platform hardening steps in `SecureSubsystem::SecureSubsystem()`
retain per-OS logic:

| Step | Linux | FreeBSD | macOS | Windows |
|---|---|---|---|---|
| `sodium_init` | ✅ | ✅ | ✅ | ✅ |
| Disable core dumps | `setrlimit` + `prctl(PR_SET_DUMPABLE)` | `setrlimit` | `setrlimit` | `SetErrorMode` + `WerAddExcludedApplication` |
| Inspect memlock | `getrlimit(RLIMIT_MEMLOCK)` | same | same | `SeLockMemoryPrivilege` probe (Windows follow-on) |
| KeyStore `sodium_malloc` | ✅ (mlock + guard pages) | ✅ | ✅ | ✅ (requires `SeLockMemoryPrivilege`) |

Sodium is the SAME library across all four; wrapper API is
identical.  Only the startup hardening + wire-protocol backends
(§9) differ per OS.

---

## 3. Random + hash

**Section stub.**  Full detail: HEP-CORE-0038 §2 (random for
script use) and inline usage sites documented per-caller.

Surface (§2.1):
- `random_bytes(out)` — replaces `randombytes_buf(out.data(), out.size())`.
  ~8 caller migration sites (uuid_utils, crypto_utils, vault_crypto,
  hub_vault, attach_protocol, tests).
- `random_u64()`, `uuid4()` — convenience.
- `blake2b(data)` — replaces `crypto_generichash(...)`.  Used by
  `crypto_utils::compute_blake2b`.

Full content migration: SEC-Fold-1b follow-up.

## 4. KDF (pwhash — argon2id)

**Section stub.**  Full detail: HEP-CORE-0038 §3 (vault password
derivation).

Surface (§2.1):
- `pwhash(password, salt)` — replaces `crypto_pwhash(...)`.  Only
  caller: `vault_crypto::vault_derive_key`.

Full content migration: SEC-Fold-1b follow-up.

## 5. Symmetric AEAD (secretbox — vault at rest)

**Section stub.**  Full detail: HEP-CORE-0038 §4 (vault file
encryption).

Surface (§2.1):
- `secretbox_seal` / `secretbox_open` — replace
  `crypto_secretbox_easy` / `crypto_secretbox_open_easy`.  Callers:
  `vault_crypto::vault_encrypt` / `_decrypt`.

Full content migration: SEC-Fold-1b follow-up.

## 6. Asymmetric box (crypto_box — wire + observer)

**Section stub.**  Full detail: HEP-CORE-0040 §5.5 (`SeckeyAccessor`
pattern) and HEP-CORE-0041 §5.5, §D4.5 (mutual auth), §D1(d) (observer).

Surface (§2.1):
- `box_seal_using(name, peer_pubkey, nonce, plaintext, out)` —
  replaces `KeyStore::with_seckey(name, cb)` +
  `crypto_box_easy(...)` combination.
- `box_open_using(name, peer_pubkey, nonce, ciphertext, out)` —
  replaces `KeyStore::with_seckey(name, cb)` +
  `crypto_box_open_easy(...)`.

**R3 (AttachProtocol scope) resolution.**  `AttachProtocol` is
NOT a crypto primitive; it's a 1000+ LOC protocol (framing, poll,
EINTR, Frame 3, observer verify, SCM_RIGHTS handover).
`AttachProtocol` continues to exist as its own subsystem; it USES
`secure().box_seal_using(...)` / `secure().box_open_using(...)`
for the crypto steps.  No `secure().attach()` method.

Full content migration: SEC-Fold-1b follow-up.

## 7. `KeyStore` submodule

**Section stub.**  Full detail: HEP-CORE-0040 §5, §6, §8 (KeyStore
API, LockedKey RAII, integration with vault/role/broker).

Surface: §2.2 above.

**R7 resolution.**  `LockedKey` is an implementation detail;
never exposed publicly.

**Naming convention** (from `DRAFT_keystore_ephemeral_and_script_
crypto_2026-07.md`):
- No naming rules enforced by `KeyStore` itself.
- Callers pick their own names (framework: `"hub_identity"`,
  `"role_identity"`, `"broker.observer"`; scripts: whatever the
  binding layer decides).
- Script binding layer TRANSLATES script-provided names into
  sandboxed storage names before calling `add_*` (see §10).

Full content migration: SEC-Fold-1b follow-up.

## 8. Vault at rest

**Section stub.**  Full detail: HEP-CORE-0038 §5-§9 (file format,
salt derivation, path discipline) and HEP-CORE-0035 §4.6 (file
ACL floor).

Not folded into §2.1's public API — vault is a FILE format on
disk, not a crypto primitive.  `vault_crypto.cpp` continues to
exist as the vault-file layer; it USES `secure().pwhash`,
`secure().secretbox_seal/open` for its crypto steps.

Full content migration: SEC-Fold-1b follow-up.

## 9. Wire authentication protocols

Three wire protocols, each with its own security posture.

### 9.1 ZMQ CURVE + ZAP

**Full detail: HEP-CORE-0036** (5203 lines).  Marked
SUPERSEDED-STATUS-ONLY — content authoritative until migrated.

Migration touchpoints under this HEP:
- `ZapRouter` + `ZmqQueue` continue to exist as protocol
  implementations.
- Their crypto calls (currently `crypto_box_easy` for handshake
  proofs) move through `secure().box_*_using(name, ...)`.
- `KnownRolesStore` (allowlist) stays as-is; unchanged surface.

### 9.2 SHM channel handshake

**Full detail: HEP-CORE-0041** (1591 lines).  Marked
SUPERSEDED-STATUS-ONLY.

Migration touchpoints:
- `AttachProtocol` (acceptor + initiator) stays as its own
  protocol.
- Its crypto calls move through `secure().box_*_using(...)`.
- `ShmCapabilityChannel` + `memfd_create` + `SCM_RIGHTS` handover
  logic stays platform-scoped.

Cross-references: HEP-CORE-0042 (Channel Attach Coordination) —
NOT folded per R5.  §9.2 references it for wire ordering.

### 9.3 Broker SHM observer

**Full detail:**
`docs/tech_draft/DRAFT_broker_shm_observer_2026-07.md` — 4-
decision (D1-D4) + risks (7 items) + 8-slice impl plan.  Task #317.

D1 slice A (`d6f5d621`), D2 slice (`f7d3a51e`), C.2.a
(`029bbe31`), C.2.b (`ce956972`) shipped.  Remaining: C.2.c
(PeerDeathWatcher), C.2.d (broker dial + fd cache), D5 (opt-out),
C.3 (metrics_source), C.4 (L4 tests), C.5 (HEP status sync).

After SEC-Fold-2 lands, the observer's ephemeral keypair
generation moves from `key_store().generate_and_add_identity(...)`
to `secure().keys().generate_and_add_identity(...)`.  Observer
verify path uses `secure().box_open_using(...)`.

## 10. Script-facing crypto API

**Section stub.**  Full detail: HEP-CORE-0038 script layer +
`DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md` + task
#247 (script crypto API bindings).

**R6 (script sandboxing) resolution.**  Script sandboxing lives
in the LANGUAGE BINDING LAYER (Python, Lua, Native), NOT in
`SecureSubsystem`.  The bindings translate script-provided names
(`"mykey"`) into sandboxed storage names
(`"script.<role_uid>.mykey"`) before calling `secure().keys().*`.
`SecureSubsystem` sees fully-qualified names only.  This must be
documented explicitly to prevent future impls from baking
sandboxing into the module.

Sketch:

```
Script:   api.crypto.new_keypair("mykey")
Binding:  secure().keys().generate_and_add_identity(
            "script." + role_uid + "." + "mykey")
```

Full detail: SEC-Fold-1b or task #247, whichever ships first.

## 11. Cross-platform status

| Feature | Linux | FreeBSD | macOS | Windows |
|---|---|---|---|---|
| `SecureSubsystem` ctor | ✅ | ✅ | ✅ | ⏸ SeLockMemoryPrivilege probe |
| `KeyStore` | ✅ | ✅ | ✅ | ✅ (with privilege) |
| ZMQ CURVE (§9.1) | ✅ | ✅ | ✅ | ✅ |
| SHM channel (§9.2) | ✅ | 🚧 #259 | 🚧 #260 | 🚧 #261 |
| Broker SHM observer (§9.3) | 🚧 #317 | ⏸ | ⏸ | ⏸ |
| Script vault (§10) | ✅ | ✅ | ✅ | ✅ |

## 12. Change log

- **2026-07-04 initial** — SEC-Fold-1 landing.  §0-§2 authoritative;
  §3-§10 stubs.  Old four HEPs (0036, 0038, 0040, 0041) marked
  SUPERSEDED-STATUS-ONLY.  Design self-review R1-R8 addressed
  (see §1-§2 for R1/R2/R3/R6/R7/R8; §7-§8 for R4 concept
  separation; §0.3 + R5 for HEP-0035 + HEP-0042 exclusion).

## 13. Superseded HEPs

Four HEPs' design contracts fold into this HEP.  Their content
remains **AUTHORITATIVE** until §3-§10 migration completes (SEC-
Fold-1b, follow-up).  Their **status** is superseded — this HEP
owns the architectural design; they own the detail.

| Old HEP | Fold target section | Status banner change |
|---|---|---|
| HEP-CORE-0036 (Authenticated Connection Establishment) | §9.1 (ZMQ CURVE + ZAP) | Add "SUPERSEDED-STATUS-ONLY by HEP-CORE-0043 §9.1; content authoritative until §9.1 detail migration" |
| HEP-CORE-0038 (Script-Accessible Vault Keystore) | §5 (AEAD), §8 (vault at rest), §10 (script API) | Same banner, pointing at those three sections |
| HEP-CORE-0040 (Locked Key Memory) | §2 (module), §7 (KeyStore submodule) | Same banner, pointing at §2 + §7 |
| HEP-CORE-0041 (SHM Channel Auth) | §9.2 (SHM channel handshake) | Same banner, pointing at §9.2 |

**Not folded** (stay independent):
- HEP-CORE-0035 (Hub-Role Auth + Federation Trust) — federation
  concern, not crypto.
- HEP-CORE-0042 (Channel Attach Coordination) — wire ordering,
  not crypto.

Both cross-referenced from §9.1 / §9.2 for the interaction
points, but their design contracts stay in place.

---

## SEC-Fold-1b — remaining work

**§3-§10 content migration** from the four superseded HEPs into
this HEP's sections.  Estimated ~2000-3000 additional lines
across the 8 sections.  Ships as a follow-up commit set:

1. §7 KeyStore detail (from HEP-0040 §5, §6, §8).  Largest.
2. §9.1 ZMQ CURVE detail (from HEP-0036).  Largest of §9.
3. §9.2 SHM channel detail (from HEP-0041 §5-§10).
4. §8 vault at rest (from HEP-0038 §5-§9).
5. §3-§6 primitives (short).
6. §10 script crypto API (short; awaits task #247 impl).
7. Delete old HEPs' authoritative content once mirrored; leave
   banner + pointer only.
8. Archive per `DOC_STRUCTURE.md §2.2`.

**SEC-Fold-2 (C++ refactor)** proceeds in parallel or after — the
`SecureSubsystem` class shape (§2.1) is the authoritative build
target regardless of §3-§10 migration state.

## Related documents

- `docs/tech_draft/DRAFT_security_module_and_hep_consolidation_2026-07.md`
  — full triage narrative + design self-review R1-R8.
- `docs/tech_draft/DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md`
  — ephemeral key + script crypto API design (folds into §7 + §10).
- `docs/tech_draft/DRAFT_broker_shm_observer_2026-07.md` —
  broker observer path (folds into §9.3).
- `docs/todo/AUTH_TODO.md` — SEC-Fold-1 / SEC-Fold-2 task tracking.
