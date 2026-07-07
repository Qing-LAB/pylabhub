# HEP-CORE-0043: Security Subsystem

| Property        | Value |
|-----------------|-------|
| **HEP**         | `HEP-CORE-0043` |
| **Title**       | Security Subsystem — unified module + HEP consolidation |
| **Status**      | 🚀 **IN PROGRESS — SEC-Fold-2 §2 + §7 SHIPPED 2026-07-06.**  §0-§2 (architecture, three-category facade, module fold) authoritative and matches the shipped C++ code (`SecureSubsystem` + `KeyStore` member of `Impl` + `Crypto` sub-container stub).  §7 (KeyStore API surface) authoritative.  §3-§6 + §8-§10 remain SECTION STUBS pointing to HEP-CORE-0036 / 0038 / 0040 / 0041 for full detail; those HEPs are **PARTIALLY SUPERSEDED** — their content remains authoritative for the not-yet-migrated sections until SEC-Fold-2b content migration completes. |
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

Full triage narrative + design self-review R1-R8 was drafted in
`docs/tech_draft/DRAFT_security_module_and_hep_consolidation_2026-07.md`
and archived to `docs/archive/transient-2026-07-06/` after §0-§2
of this HEP absorbed the authoritative content.  Refer to this HEP
directly for shipped design; consult the archived draft only for
historical R1-R8 reasoning trace.

### 0.2 What this HEP covers

- **§1 The contract** — three load-bearing statements plus three
  supporting principles.  §1.1-§1.3 are the headline: **Nature**
  (what the module IS), the **init flag/gate** (how sodium_init
  is enforced), and the **singularity model** (exactly one
  instance per process, five enforcement mechanisms).  §1.4-§1.6
  are supporting: use-not-export, rotation & lifetime, cross-
  platform layering.
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

### 0.4 Migration status (2026-07-07)

- **§0-§2** — authoritative; describes the shipped design.
- **§2.1 (SecureSubsystem class — two-category facade) — SHIPPED
  2026-07-07** — the prior `Crypto` sub-container was collapsed;
  encryption verbs live flat on `SecureSubsystem` alongside sodium
  primitives.
- **§2.2 (KeyStore submodule) — SHIPPED 2026-07-06** — KeyStore is a
  member of `SecureSubsystem::Impl`.  Present-tense authoritative.
- **§2.3 (Lifecycle registration) — SHIPPED 2026-07-06** — describes
  the Logger-shape static module currently registered.
- **§3 (Random + hash) — SHIPPED 2026-07-07** — Category 1a + 1b
  methods on `SecureSubsystem`.  The `pylabhub::crypto` namespace
  and its `GetLifecycleModule` are DELETED.
- **§4 (KDF pwhash_argon2id) — SHIPPED 2026-07-07** — Category 1b.
- **§5 (Symmetric secretbox) — SHIPPED 2026-07-07** — Category 1c;
  `vault_crypto.cpp` migrated.
- **§6 (Asymmetric box) — NOT SHIPPED** — future migration of
  `attach_protocol.cpp` will fold it into Category 1c methods on
  `SecureSubsystem`.  Attach protocol currently uses sodium
  directly within the security-module boundary (legal per §1.2).
- **§7 (KeyStore API surface) — SHIPPED 2026-07-06** — carries the
  full API surface preserved from HEP-CORE-0040 §5.2.  Now the
  primary reference.
- **§8–§10** — SECTION STUBS with pointers to existing HEPs
  as the authoritative source of full detail until content migration
  completes.  Old HEPs marked **PARTIALLY SUPERSEDED** — the sections
  each stub points at remain valid; sections describing lifecycle /
  singleton / registration are superseded and flagged inline in the
  old HEPs.
- **§11-§13** — supporting.

**Retired frameworks (deleted from the codebase):**
- `pylabhub::crypto` namespace + its `"CryptoUtils"` lifecycle module
  — folded into `SecureSubsystem` Category 1a/1b methods (2026-07-07).
- `pylabhub::utils::security::Crypto` scaffolding class — collapsed
  into `SecureSubsystem` Category 1c methods (2026-07-07).
- `pylabhub::utils::security::key_store()` / `key_store_ready()`
  free-function shims — deleted; access is `secure().keys()` /
  `sodium_ready()` (2026-07-06 Phase D).
- `SecureMemorySubsystem` class name + `"SecureMemory"` lifecycle
  module name — renamed to `SecureSubsystem` (2026-07-06 Phase E).
- `CurveKeyStoreFixture` test class — replaced by free function
  `seed_curve_identities(setup)` (2026-07-06).
- `g_sms` file-scope rendezvous pointer — deleted; every accessor
  routes through `SecureSubsystem::instance()` (2026-07-06 Phase E).

---

## 1. The contract

The three load-bearing statements about `SecureSubsystem`.  Every
subsequent section — the API sketch (§2), primitives (§3-§7),
protocols (§9) — is downstream of these three.

### 1.1 Nature — what SecureSubsystem IS

**`SecureSubsystem` is the one C++ subsystem in the codebase that
owns and mediates every access to libsodium.**  It is:

- **The libsodium boundary.**  Only file that `#include <sodium.h>`
  in production code.  Every raw sodium primitive
  (`sodium_malloc`, `sodium_memzero`, `randombytes_buf`,
  `crypto_box_*`, `crypto_secretbox_*`, `crypto_pwhash`,
  `crypto_generichash`, `sodium_memcmp`, ...) is exposed through a
  typed C++ wrapper method on this module.
- **The keystore.**  Owns the `KeyStore` submodule that holds
  every long-term identity keypair and every ephemeral runtime
  key in mlocked memory (via `sodium_malloc`).  Enforces the
  use-not-export contract (§1.4).
- **The platform hardening layer.**  Disables core dumps,
  configures RLIMIT_MEMLOCK, sets PR_SET_DUMPABLE on Linux,
  excludes the binary from Windows Error Reporting.
- **The single lifecycle-registered security module.**  Registers
  itself as `"SecureSubsystem"` with the LifecycleManager,
  depends on Logger.  No other security-related lifecycle module
  exists (KeyStore is a member, not a peer).

What it is NOT:
- Not a wire protocol implementation — AttachProtocol, ZapRouter,
  vault_crypto, hub_vault continue to exist as their own layers.
  They USE `secure().*` for the crypto steps; they own their own
  framing/state/file-format logic.
- Not a script binding surface — language bindings (Python, Lua,
  Native) live in the script layer; they call `secure().*` for
  crypto but own their own name-translation sandboxing (§10).
- Not a federation trust layer — HEP-CORE-0035 owns hub-role
  federation trust, cross-references `secure().*` for crypto.

### 1.2 The init flag/gate

**libsodium requires `sodium_init()` to have completed successfully
before any other libsodium function is called.**  `SecureSubsystem`
owns this init exclusively and gates all consumer access to
libsodium behind it.

Mechanism, four layers deep:

1. **Init happens exactly once, in the constructor.**
   `SecureSubsystem::SecureSubsystem()` calls `::sodium_init()`
   as its first step.  A successful return (>= 0) sets
   `pImpl->sodium_initialized = true`.  Failure throws
   `std::runtime_error` — the process cannot proceed.
2. **The flag is exposed as a probe.**  Public methods
   `SecureSubsystem::sodium_initialized() const noexcept` and the
   free function `pylabhub::utils::security::sodium_ready()
   noexcept` both return `true` iff the singleton exists AND its
   init flag is true.  Non-throwing, safe to call from any
   context including test fixtures and unrelated startup code.
3. **Every wrapper method PANICS on gate violation.**  Every
   public method on `SecureSubsystem` that touches libsodium
   (`random_bytes`, `memcmp_ct`, `memzero`, and every future
   wrapper for hash/KDF/AEAD/box) starts with
   `if (!sodium_ready()) PLH_PANIC(...)` — same pattern FileLock
   and Logger use when their static module is called before
   init.  Reaching a `SecureSubsystem` method without having
   constructed the subsystem is a **programmer error**, not a
   recoverable exception.  The program aborts.  No per-call
   carve-outs, no "this specific primitive tolerates pre-init"
   exceptions — the module owns the boundary uniformly.
   KeyStore's public methods (`add_identity`, `add_raw`, ...)
   follow the same PANIC pattern.
4. **Compile-time enforcement (post-SEC-Fold-2).**  Once all
   consumer files migrate to the wrapper API, `<sodium.h>` is
   included ONLY in `src/utils/security/*.cpp`.  Any file
   elsewhere that adds `#include <sodium.h>` is a CI lint
   violation.  Reaching libsodium without going through
   `SecureSubsystem` becomes structurally impossible — and every
   path through the wrapper goes through the panic gate.

Consequence: the 2026-07-04 CI failure class (sodium primitives
called before `sodium_init`) cannot recur.  Even in Debug/pre-
refactor state, calling `secure().random_bytes(...)` before
constructing `SecureSubsystem` throws a clear runtime error, not
libsodium's inscrutable internal assertion.

Historical: prior to 2026-07-04 the codebase had FIVE independent
sodium_init call sites (`uuid_utils.cpp`, `crypto_utils.cpp`,
`vault_crypto.cpp`, `attach_protocol.cpp`, `SecureSubsystem`)
plus two in tests.  None was authoritative.  A test worker that
skipped all five paths hit an uninitialized libsodium and aborted
with a guard-page pointer-arithmetic assertion.  Commit `9d0a7eb4`
ripped every self-init out and centralized on the SMS constructor;
this HEP formalizes that as the permanent contract.

### 1.3 Singularity — exactly one instance per process

**There is exactly one `SecureSubsystem` instance per OS process.**
No process can have zero, no process can have two.

Enforced by five mechanisms (updated 2026-07-06 post-SEC-Fold-2
Phase E — stack-local sites deleted, `g_sms` rendezvous pointer
deleted):

1. **Function-local static singleton.**
   `SecureSubsystem::instance()` returns a reference to
   `static SecureSubsystem sole;` — C++11 guarantees thread-
   safe once-only initialization.  Matches `Logger::instance()`
   (`src/utils/logging/logger.cpp:883-890`).  Ctor is `private`;
   the ONLY construction path is `instance()` (called by the
   startup thunk and the free-function accessor `secure()`).
2. **Bringup CAS on `g_state`.**  `Impl::bringup()`'s first step
   is a compare-exchange from `Uninitialized → InitCalled`.  Any
   second construction — from any thread, any code path — sees a
   non-`Uninitialized` state and PANICs via `PLH_PANIC` (matches
   FileLock / Logger discipline; not a recoverable exception).
   The CAS is the load-bearing singularity enforcer.
3. **Non-copyable, non-movable.**  Explicit `= delete` on copy
   ctor, copy assign, move ctor, move assign.  There is one
   object, no way to duplicate it.
4. **Static lifecycle module registered via mod pack.**
   `SecureSubsystem::GetLifecycleModule()` returns a
   `ModuleDef("SecureSubsystem")` with dependency on
   `"pylabhub::utils::Logger"`.  Callers add it to their
   `LifecycleGuard` mods pack (production `main()`, test workers
   via `run_gtest_worker(..., mods...)`).  LifecycleManager
   rejects a second registration of the same name.  Matches
   Logger + FileLock static-module discipline — NOT dynamic
   ctor-side self-registration.
5. **KeyStore and Crypto are members of `SecureSubsystem::Impl`.**
   Neither is a separate lifecycle module (SHIPPED 2026-07-06 per
   §2.2).  Both ctors are private + friend `SecureSubsystem::Impl`
   — no external construction site is possible at compile time.
   Their lifetime is bound to SMS's — same singleton guarantee,
   same bringup ordering.  Access via `secure().keys()` and
   `secure().crypto()`.

Standard construction site: `SecureSubsystem::GetLifecycleModule()`
in the mods pack of `plh_hub_main` / `plh_role_main`, immediately
after `Logger::GetLifecycleModule()`.  Tests do the same in their
subprocess worker's mods pack (Pattern 3) or in
`PLH_BINARY_LIFECYCLE_MODULES` (parent-lifecycle tests).  No
stack-local `SMS sms;` or `KeyStore ks(...)` sites remain anywhere
in `src/` or `tests/` (grep-enforced 2026-07-06; Phase 5 CI lint
gates this invariant).

Consequence: the codebase can rely on `secure()` always returning
the same object; state (KeyStore contents, sodium_initialized
flag) is process-global.  No coordination needed across
subsystems that all touch security.

### 1.4 Use-not-export for secret bytes

**Secret bytes never leave `KeyStore`'s mlocked memory region as
data.**  Consumers pass a `KeyStore` entry NAME and a use-callback;
the callback runs against a `std::span<const std::byte>` view whose
lifetime ends when the callback returns.  Bytes are never
materialized into a `std::string` copy, a heap buffer, or any
storage the module doesn't own.

Established as NORMATIVE in HEP-CORE-0040 §5.2 for
`KeyStore::with_seckey`.  Extends to `SecureSubsystem`'s
asymmetric operations (§6): `box_seal_using` and `box_open_using`
take a name, not a raw seckey.  The seckey is dereferenced INSIDE
the module, used INSIDE the module, never crosses the module
boundary.

API design consequence: no `SecureSubsystem` method takes a raw
`std::span<const std::byte, 32>` seckey argument.  If a future
feature needs to encrypt with a caller-owned seckey, the seckey
enters the module via `KeyStore::add_raw` first.

### 1.5 Rotation & lifetime

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

### 1.6 Cross-platform layering

Sodium primitives are available identically on Linux, FreeBSD,
macOS, and Windows.  Sodium doesn't need per-OS abstraction; the
module wraps once.

Platform-specific concerns concentrate at TWO other layers:

- **`SecureSubsystem` startup platform hardening** — core dumps,
  PR_SET_DUMPABLE, RLIMIT_MEMLOCK, SeLockMemoryPrivilege.  Per-OS
  logic in `secure_subsystem.cpp` `disable_core_dumps_or_panic`
  + `inspect_memlock_capability`.
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

### 2.1 The `SecureSubsystem` class — two-category facade

**Name.**  `SecureSubsystem`.  Header: `src/include/utils/security/
secure_subsystem.hpp`.  Namespace-scope accessor `secure()`; the
class ctor is `private` (singleton via `instance()`).

**Design shape (SHIPPED 2026-07-06, revised 2026-07-07).**
`SecureSubsystem` is a facade over TWO service categories.  All
sodium operations — from stateless byte primitives to protocol-level
encryption verbs — live as FLAT methods on the class (Category 1).
Key management is the ONE nested sub-container (Category 2), because
`KeyStore` genuinely encapsulates state (the map, `shared_mutex`,
`LockedKey` machinery).

| Category | What it does | Access pattern |
|---|---|---|
| **1a. Byte primitives** | Stateless wrappers on single sodium functions — random, memcmp_ct, memzero, bin2hex | `secure().random_bytes(out)` |
| **1b. Hash + KDF** | BLAKE2b + verify + Argon2id | `secure().compute_blake2b(...)` |
| **1c. Encryption / decryption** | Higher-level protocol operations — secretbox (shipped), future box/aead/sealed_box | `secure().secretbox_encrypt(...)` |
| **2. Key management** | KeyStore — long-term identities + ephemeral keys under use-not-export | `secure().keys().add_identity(...)` |

Categories 1a/1b/1c are DOCUMENTATION groupings — all their methods
are flat on `SecureSubsystem`.  Encryption verbs live flat because
they have no state to encapsulate: `secretbox_encrypt(plaintext,
key, nonce)` is a stateless call.  Grouping them by concept lives in
header section markers, not class boundaries.

The prior `Crypto` nested sub-container (introduced as scaffolding
in the initial 2b design) was collapsed 2026-07-07 after realizing
its `Impl` was empty and every method was stateless — the class
provided false symmetry with `KeyStore` (which encapsulates real
state).  `KeyStore` stays nested; encryption verbs go flat.

**Public surface (shipped shape):**

```cpp
namespace pylabhub::utils::security {

inline constexpr std::size_t BLAKE2B_HASH_BYTES = 32;

class PYLABHUB_UTILS_EXPORT SecureSubsystem
{
public:
    // ── Lifecycle ─────────────────────────────────────────────
    static SecureSubsystem      &instance();
    static ModuleDef             GetLifecycleModule();
    static bool                  lifecycle_initialized() noexcept;
    ~SecureSubsystem();
    // ... deleted copy/move ctors ...

    // ── Category 2: key management (nested) ───────────────────
    KeyStore &keys();

    // ── Category 1a: byte primitives ──────────────────────────
    void          random_bytes(std::span<std::uint8_t> out);
    void          random_bytes(std::uint8_t *out, std::size_t len);
    std::uint64_t random_u64();
    std::array<std::uint8_t, 64> generate_shared_secret();
    bool          memcmp_ct(std::span<const std::uint8_t>,
                            std::span<const std::uint8_t>);
    void          memzero(std::span<std::uint8_t>);
    void          bin2hex(char *hex, std::size_t hex_max_len,
                          const std::uint8_t *bin, std::size_t bin_len);

    // ── Category 1b: hash + KDF ───────────────────────────────
    bool          compute_blake2b(std::uint8_t *out, const void *data,
                                   std::size_t len);
    std::array<std::uint8_t, 32>
                  compute_blake2b_array(const void *data, std::size_t len);
    bool          verify_blake2b(const std::uint8_t *stored,
                                  const void *data, std::size_t len);
    bool          verify_blake2b(const std::array<std::uint8_t, 32> &stored,
                                  const void *data, std::size_t len);
    bool          pwhash_argon2id(std::uint8_t *out, std::size_t out_len,
                                   const char *password, std::size_t password_len,
                                   const std::uint8_t *salt);

    // ── Category 1c: encryption / decryption ──────────────────
    // Symmetric authenticated (XSalsa20-Poly1305):
    std::size_t   secretbox_encrypt(std::uint8_t *out, std::size_t out_max_len,
                                     const std::uint8_t *plaintext, std::size_t plaintext_len,
                                     const std::uint8_t *nonce,
                                     const std::uint8_t *key);
    std::size_t   secretbox_decrypt(std::uint8_t *out, std::size_t out_max_len,
                                     const std::uint8_t *ciphertext, std::size_t ciphertext_len,
                                     const std::uint8_t *nonce,
                                     const std::uint8_t *key);
    static constexpr std::size_t kSecretboxKeyBytes   = 32;
    static constexpr std::size_t kSecretboxNonceBytes = 24;
    static constexpr std::size_t kSecretboxMacBytes   = 16;

    // Future encryption verbs (crypto_box_*, crypto_aead_*, sealed_box)
    // will land as additional flat methods on this class.

    struct Impl;                 // opaque, defined in .cpp

private:
    SecureSubsystem();           // singleton; only `instance()` calls
    std::unique_ptr<Impl> pImpl;
};

// Namespace-scope free functions:
[[nodiscard]] SecureSubsystem &secure();        // PANICs on gate
[[nodiscard]] bool             sodium_ready() noexcept;

} // namespace pylabhub::utils::security
```

`KeyStore` is declared in its own header (`key_store.hpp`),
pImpl-owned, private ctor + `friend struct SecureSubsystem::Impl` —
the only construction site is Impl's member-init list.

**Gate discipline.**  Every accessor (`secure()`, `keys()`, and
every Category 1 method) routes through one helper
`panic_if_not_ready(context)` which PANICs when
`g_state != Initialized`.  The `keys()` gate is defensive against
`SecureSubsystem::instance().keys()` bypass paths.  `sodium_ready()`
and `lifecycle_initialized()` are the non-throwing probes.

**R2 (naming) resolution.**  `SecureSubsystem` chosen (not `Secure`
which was too generic, not `Security` which collides with narrower
Authentication scope).  Accessor `secure()` matches the tightness
of `thread_manager()` conventions elsewhere.

**R7 (LockedKey) resolution.**  `LockedKey` is an implementation
detail of `KeyStore`, never exposed at `SecureSubsystem`'s public
surface.  Its declaration stays in `key_store.cpp` (anonymous
namespace).

**R8 (Z85PublicKey) resolution.**  Future `box_*` methods on SMS
accept `Z85PublicKey` directly, matching the existing strong-type
discipline from task #231.  The Z85 → raw decode happens once
inside the module.

### 2.2 `KeyStore` submodule (SHIPPED 2026-07-06)

**Ownership.**  `KeyStore` is a MEMBER of `SecureSubsystem::Impl`
(field `keys_`).  Access is via `secure().keys()` exclusively —
the old `key_store()` / `key_store_ready()` inline shims were
deleted in Phase D (2026-07-06); all ~200 caller sites migrated
grep-mechanically.  There is no separate KeyStore lifecycle
module — LifecycleManager sees exactly one module
(`"SecureSubsystem"`), not two.  `KeyStore()` ctor is `private`
with `friend struct SecureSubsystem::Impl` — external construction
is a compile error.

**R1 (static vs dynamic) resolution — SHIPPED.**  `SecureSubsystem`
is STATIC (constructed once at LifecycleGuard init).  `KeyStore`
INSIDE it is logically DYNAMIC (key inserts happen at arbitrary
runtime moments) — but its DYNAMISM is now internal state on the
KeyStore map, not a separate lifecycle module.  KeyStore's ctor
is trivial (`pImpl = make_unique<Impl>()`); the map is empty at
SMS bringup and populated as consumers call `secure().keys().
add_identity(...)`.  KeyStore's dtor drains the reader-writer lock
then destructs the map (each Entry's `LockedKey` runs
`sodium_memzero` + `sodium_free`).

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

Full contract: HEP-CORE-0040 §5.2 (API surface preserved verbatim
through the merger).  Sections of HEP-0040 SUPERSEDED by the
2026-07-06 merger: §5.1 (singularity — now enforced by SMS's
singleton), §5.6 (namespace accessor — deleted; access is
`secure().keys()`), §5.4 (dynamic-module registration — deleted).
§5.3 (canonical entry names), §5.5 (thread-safety contract),
§6 (LockedKey RAII), §8.5.2 (raw-32 seckey contract) remain
authoritative.

### 2.3 Lifecycle registration

**One lifecycle module: `"SecureSubsystem"`** (renamed from
`"SecureMemory"`).  Dependency: `"pylabhub::utils::Logger"`.
STATIC module — registered via a `LifecycleGuard` mods pack
(NOT dynamic ctor-side self-registration).  Same shape as
`Logger::GetLifecycleModule()` + `FileLock::GetLifecycleModule()`.

**Startup thunk (`do_secure_subsystem_startup`)** triggers
construction of the function-local static `sole` inside
`SecureSubsystem::instance()`.  It then calls
`instance().pImpl->bringup()`, which does the singularity CAS,
calls `sodium_init()`, disables core dumps, inspects mlock
capability, and publishes `g_state = Initialized` under a
release fence.
The thunk is a friend of `SecureSubsystem` so it can drive
`pImpl` — same discipline as Logger's `do_logger_startup`
(`src/utils/logging/logger.cpp:1272-1295`).

**Shutdown thunk (`do_secure_subsystem_shutdown`)** compare-
exchanges the state gate from `Initialized → ShuttingDown`, then
calls `pImpl->shutdown_module()` which publishes the terminal
`Shutdown` state.  Sodium is stateless and core-dump disable is
irreversible (HEP-CORE-0043 §1.5) — there is nothing to unwind,
just gate-close via state transitions so late accessors PANIC.

**Singleton lifetime** is program-lifetime.  `sole` is a function-
local static → atexit-destructed.  The shutdown thunk closes the
gate but does NOT delete the outer object; that avoids any
`new`/`delete` of `SecureSubsystem` crossing the shared-lib
boundary.  Matches Logger.

Post-SEC-Fold-2 the old `"KeyStore"` dynamic module is DELETED —
its work is absorbed into `SecureSubsystem`.

Ordering invariant (unchanged from HEP-CORE-0040 §4.5):
`SecureSubsystem` must be constructed BEFORE any consumer of
`secure()`.  Standard call site: `SecureSubsystem::
GetLifecycleModule()` in the mods pack of `plh_hub` / `plh_role`
`main()`, immediately after `Logger::GetLifecycleModule()`.

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

## 3. Random + hash (Category 1a + 1b — SHIPPED 2026-07-07)

Category 1a byte primitives + Category 1b hash/KDF on
`SecureSubsystem` (§2.1).  All formerly `pylabhub::crypto::*` free
functions folded here 2026-07-07:

| SMS method | Replaces | Callers migrated |
|---|---|---|
| `random_bytes(out)` / `random_bytes(ptr, len)` | `randombytes_buf` + `pylabhub::crypto::generate_random_bytes` | uuid_utils, hub_vault, vault_crypto, attach_protocol, tests |
| `random_u64()` | `pylabhub::crypto::generate_random_u64` | native_engine, tests |
| `generate_shared_secret()` | `pylabhub::crypto::generate_shared_secret` | data_block startup |
| `compute_blake2b(out, data, len)` | `pylabhub::crypto::compute_blake2b` | data_block (checksums) + schema_utils + schema_blds |
| `compute_blake2b_array(data, len)` | `pylabhub::crypto::compute_blake2b_array` | schema_utils + native_engine |
| `verify_blake2b(stored, data, len)` | `pylabhub::crypto::verify_blake2b` | data_block (slot integrity) |
| `derive_pwhash_salt(out, domain)` | (new — replaces inline `crypto_generichash(salt, 16, uid, ...)` in vault_crypto) | vault_crypto (Argon2 salt) |
| `bin2hex(hex, hex_max_len, bin, bin_len)` | `sodium_bin2hex` | hub_vault (admin token) |

The `pylabhub::crypto` namespace + its `GetLifecycleModule()` are
DELETED (2026-07-07).  Consumer files are grep-verifiable: zero
`#include <sodium.h>` outside `src/utils/security/*` (HEP-CORE-0043
§1.2 mechanism 4 SHIPPED).

### 3.1 BLAKE2b output length — purpose-specific methods, not variable-length

**Design rule (2026-07-07):** the SMS BLAKE2b surface exposes
**purpose-specific methods with a FIXED output length each**, not a
single variable-length method with an `out_len` parameter.  Every
production BLAKE2b use case has a known, fixed size mandated by its
consumer.  Exposing "pick your own length" invites bugs — the
migration to SMS actually hit one (a stack smash during vault
decryption) when the caller's intended 16-byte call was silently
turned into a 32-byte write into a 16-byte stack buffer.

Two purposes, two methods:

| Purpose | Method | Size | Consumer |
|---|---|---|---|
| **Content addressing** (checksums, schema hashes, integrity verify) | `compute_blake2b(out, data, len)` | 32 (`BLAKE2B_HASH_BYTES`) | data_block, schema_utils, schema_blds, native_engine |
| **Argon2id KDF salt** | `derive_pwhash_salt(salt_out, domain)` | 16 (`kPwhashSaltBytes` = `crypto_pwhash_SALTBYTES`) | vault_crypto |

If a third purpose ever emerges (e.g. a 64-byte MAC key, or a
domain-specific short digest for a wire format), it lands as a
THIRD purpose-specific method (`derive_mac_key(...)`,
`compute_digest_short(...)`, ...) — NOT as a fourth position on an
`out_len` parameter.  Reader intent stays encoded in the method name.

### 3.2 BLAKE2b-16 is a genuine hash, not a truncation of BLAKE2b-32

**Cryptographic detail worth pinning explicitly.**  BLAKE2b's
digest_length is a **parameter of the algorithm**, not a
post-truncation length.  Per RFC 7693 §3.1, the digest_length is
placed in byte 0 of the parameter block P (a 64-byte structure).
Before ANY compression round runs, the initial hash state H₀ is
computed as:

```
H₀ = IV XOR P
```

The digest_length is therefore XOR'd into `H₀[0]`.  This means:

- `BLAKE2b("hello", outlen=16)` and `BLAKE2b("hello", outlen=32)`
  start with **different H₀ values**, run compression through
  **different states**, and produce **completely different bytes**.
- `BLAKE2b("hello", outlen=32)[0..16]` is a truncation of a
  different hash and is **NOT** equal to `BLAKE2b("hello", outlen=16)`.
- The two are cryptographically distinct primitives.

Concrete illustration:
```
BLAKE2b-16("hello") =                          e7d1acfa9dfffce02d9c8b93a01ecfa5
BLAKE2b-32("hello") = 324dcf027dd4a30a932c441f365a25e86b173defa4b8e58948253471b81b72cf
BLAKE2b-32("hello")[0..16] =                   324dcf027dd4a30a932c441f365a25e8   ← different from BLAKE2b-16
```

`derive_pwhash_salt` therefore calls `crypto_generichash(salt, 16,
domain, ...)` — invoking BLAKE2b in genuine 16-byte mode.  It does
NOT compute a 32-byte hash and truncate.  This matches the vault
format's pre-existing derivation (`crypto_generichash(salt,
kVaultSaltBytes=16, uid, ...)`); changing to any other scheme would
silently invalidate every existing vault file (different salt →
different derived key → MAC verify fails on decrypt).

## 4. KDF (pwhash — argon2id, Category 1b — SHIPPED 2026-07-07)

### 4.1 Provenance — upstream, not our implementation

Argon2id is **not implemented in pyLabHub**.  Ownership chain:

- **Argon2 reference implementation** — `github.com/P-H-C/phc-winner-argon2`,
  the winning entry of the 2015 Password Hashing Competition
  (Biryukov, Dinu, Khovratovich).
- **libsodium** vendors the reference impl and wraps it as
  `crypto_pwhash(...)` with algorithm selector
  `crypto_pwhash_ALG_ARGON2ID13` (the "13" is Argon2's own spec
  version v1.3).
- **pyLabHub SMS** wraps libsodium's `crypto_pwhash` as
  `secure().pwhash_argon2id(...)`.

We do not modify the cryptographic implementation, do not maintain
our own Argon2, and do not select any non-standard parameters
beyond the ops/mem-limit tuple (which libsodium exposes as
`INTERACTIVE` / `SENSITIVE` / `MIN` presets).

### 4.2 Salt-size ABI

`crypto_pwhash`'s salt parameter has **NO length argument**:

```c
int crypto_pwhash(unsigned char *out, unsigned long long outlen,
                  const char *passwd, unsigned long long passwdlen,
                  const unsigned char *salt,   // ← reads exactly SALTBYTES from this pointer
                  unsigned long long opslimit, size_t memlimit, int alg);
```

libsodium reads exactly `crypto_pwhash_SALTBYTES = 16` bytes from
that pointer.  This is an ABI, not a configurable parameter.
Passing a longer buffer wastes bytes silently; a shorter buffer
reads past the buffer (undefined behaviour).

The `derive_pwhash_salt` method above produces the correct 16-byte
input.  The static_assert in `secure_subsystem.cpp` verifies
`SecureSubsystem::kPwhashSaltBytes == crypto_pwhash_SALTBYTES` at
build time — if libsodium ever changes the constant (they haven't
in 10 years), the build fails loud with a clear message.

### 4.3 SMS surface

- `pwhash_argon2id(out, out_len, password, password_len, salt)` —
  wrapper around `crypto_pwhash(...)` with algorithm hardcoded to
  `crypto_pwhash_ALG_ARGON2ID13`.  `salt` MUST be exactly
  `kPwhashSaltBytes` (16) bytes; typically produced by
  `derive_pwhash_salt(salt, domain)` (§3.1).  Uses INTERACTIVE
  ops/mem-limit constants — appropriate for vault-file unlock; NOT
  for password-storage KDF (the SENSITIVE preset would be needed
  there, and we don't currently expose it — vault decryption is our
  only Argon2 use case).
- `derive_pwhash_salt(salt_out, domain)` — produces the 16-byte
  Argon2 salt from a domain string.  See §3.1.
- `kPwhashSaltBytes` — public constexpr, value 16.

Only caller today: `vault_crypto::vault_derive_key`.

## 5. Symmetric encryption (secretbox — Category 1c — SHIPPED 2026-07-07)

Surface (§2.1):
- `secretbox_encrypt(out, out_max_len, plaintext, plaintext_len, nonce, key)`
  — replaces `crypto_secretbox_easy`.  Returns bytes written on
  success, 0 on failure.  Ciphertext includes MAC as 16-byte prefix.
- `secretbox_decrypt(out, out_max_len, ciphertext, ciphertext_len, nonce, key)`
  — replaces `crypto_secretbox_open_easy`.  Returns bytes written on
  success, 0 on MAC failure or bad input.  Callers MUST check
  return value.
- Constants: `kSecretboxKeyBytes` (32), `kSecretboxNonceBytes` (24),
  `kSecretboxMacBytes` (16).
- Callers: `vault_crypto::vault_write` / `vault_read_secure`.

**Use-case boundary.**  Symmetric encryption is appropriate when
the SAME party (or same process instance) is on both sides — e.g.
file-at-rest with a password-derived key.  For two-party mutual
auth (broker ↔ role, hub ↔ hub), use `box_*` methods (§6).

## 6. Asymmetric box (crypto_box — wire + observer, Category 1c — NOT YET SHIPPED)

**Status.**  Design for future migration; not yet shipped.  Current
callers still touch sodium directly inside the security module
(`attach_protocol.cpp`), which is legal per §1.2 mechanism 4
(sodium.h boundary confined to `src/utils/security/*`).

Expected surface (Category 1c on `SecureSubsystem`):
- `box_encrypt_using(seckey_name, peer_pubkey, nonce, plaintext, out)` —
  wrapper that reads the seckey via `KeyStore::with_seckey` and
  calls `crypto_box_easy` internally.  Seckey never leaves the
  LockedKey region (use-not-export, §1.4).
- `box_decrypt_using(seckey_name, peer_pubkey, nonce, ciphertext, out)` —
  same pattern for `crypto_box_open_easy`.

**Use-case boundary.**  Asymmetric box is appropriate for two-party
mutual authentication where both parties have their own long-term
identity keypair and know the other's PUBKEY through the KeyStore.
Broker ↔ role authentication (`AttachProtocol`), hub ↔ hub
federation (HEP-CORE-0033) are the canonical use cases.

**R3 (AttachProtocol scope) resolution.**  `AttachProtocol` is
NOT a crypto primitive; it's a 1000+ LOC protocol (framing, poll,
EINTR, Frame 3, observer verify, SCM_RIGHTS handover).
`AttachProtocol` continues to exist as its own subsystem; when Cat
1c box methods land, `AttachProtocol` USES them for the crypto
steps.  No `secure().attach()` method.

## 7. `KeyStore` submodule (SHIPPED 2026-07-06)

**Shipped state.**  KeyStore is a MEMBER of `SecureSubsystem::Impl`
per §2.2.  Access via `secure().keys()` throughout production +
tests.  No separate lifecycle module.  No stack-local ctor sites
anywhere in the codebase (grep-enforced).

**API surface (SHIPPED unchanged from HEP-CORE-0040 §5.2).**  All
methods accessible via `secure().keys()`:

| Method | Semantics |
|---|---|
| `add_identity(name, packed_pub_sec)` | Insert 64-byte identity (pub_raw[32] ‖ sec_raw[32]).  Source span zeroed on return.  Throws on duplicate name. |
| `add_identity_from_z85(name, pub_z85, sec_z85)` | Convenience: Z85 pair → raw 64 bytes via `SecureBuffer<64>` (zero-on-destruct) → `add_identity`.  Single site for Z85→raw decode at the module boundary. |
| `generate_and_add_identity(name) → std::string` | Generate fresh CURVE keypair in-memory; return Z85 pubkey; seckey accessible only via `with_seckey`. |
| `add_raw(name, plaintext)` | HEP-CORE-0038 vault_save: opaque bytes.  Source span zeroed on return. |
| `remove(name)` | Delete a stored secret; blocks until in-flight `with_seckey` callbacks return. |
| `pubkey(name) → string_view` | Z85 pubkey (40 chars).  Non-secret; view lifetime = KeyStore lifetime. |
| `with_seckey(name, callback)` | Raw 32-byte seckey via callback; view valid only inside callback (use-not-export). |
| `with_seckey_z85(name, callback)` | Z85 seckey (40 ASCII) via callback; encoded on-the-fly, buffer sodium_memzero'd on return. |
| `with_keypair_z85(name, callback)` | Both halves Z85 via callback. |
| `lookup_raw(name) → span<const byte>` | HEP-CORE-0038 vault_load: raw bytes span. |
| `has(name)` / `size()` | Existence + count probes. |

**LockedKey (HEP-CORE-0040 §6).**  Each entry owns a
`sodium_malloc`'d region: mlock'd + guard-paged + canaried +
`sodium_memzero` on dtor.  `LockedKey` is an implementation detail
of KeyStore's `.cpp`; never exposed publicly (R7 resolution).

**Concurrency (HEP-CORE-0040 §5.5).**  `pubkey` / `with_seckey` /
`lookup_raw` / `has` / `size` take a shared lock (parallel reads
OK); `add_identity` / `add_raw` / `remove` take exclusive.
Callbacks must be prompt (µs) — no blocking I/O.

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
