# Security module + HEP consolidation — tech draft

> **Status:** DRAFT, 2026-07-04.  Captures the 2026-07-04
> conversation where the CI-triage sodium_init bug exposed two
> larger design problems:
> 1. The codebase has no single owner of the libsodium API surface —
>    sodium is called directly from at least 8 different files.
> 2. Security design is spread across six HEPs (0035, 0036, 0038,
>    0040, 0041, 0042) plus two live tech drafts, with cross-
>    references and shared-scope areas that only make sense
>    together.
>
> Both problems have the same root: security grew feature-by-feature
> ("add auth", "add SHM channel auth", "add locked keys", "add
> vault keystore", ...) instead of being designed as one subsystem.
> Now that the real applications need coordinated key management +
> secure allocation + wire auth + capability transport + observer
> paths, the scattered design is producing bugs like the sodium_init
> gap (2026-07-04) and design questions I keep having to re-check
> against six different HEPs.
>
> This draft has two proposals:
>   1. **Module fold** — one C++ module owns every libsodium API.
>      Nothing else `#include <sodium.h>`.
>   2. **HEP consolidation** — one unified `HEP-CORE-SECURITY` (or
>      similar) is the design contract for the module; the six
>      existing HEPs become sections or are archived.

## Part 1 — The module fold

### Current state

Files that include `<sodium.h>` or call sodium primitives directly:

| File | Sodium calls |
|---|---|
| `src/utils/security/secure_memory_subsystem.cpp` | `sodium_init` (as of 2026-07-04) |
| `src/utils/security/key_store.cpp` | `sodium_malloc`, `sodium_free`, `sodium_memzero`, `sodium_memcmp`, `add_identity`/`add_raw`/`with_seckey` primitives |
| `src/utils/security/attach_protocol.cpp` | `crypto_box_easy`, `crypto_box_open_easy`, `crypto_scalarmult_base`, `randombytes_buf`, `sodium_memcmp`, `sodium_memzero`, `crypto_box_keypair` |
| `src/utils/security/curve_keypair.cpp` | `zmq_curve_keypair` (delegates to libsodium) |
| `src/utils/service/vault_crypto.cpp` | `crypto_pwhash`, `crypto_secretbox_easy`, `crypto_secretbox_open_easy`, `randombytes_buf` |
| `src/utils/service/crypto_utils.cpp` | `crypto_generichash` (BLAKE2b), `randombytes_buf`, `sodium_memcmp` |
| `src/utils/service/hub_vault.cpp` | `randombytes_buf` |
| `src/utils/core/uuid_utils.cpp` | `randombytes_buf` |
| Tests (`test_attach_protocol.cpp`, `test_shm_attach_orchestrator.cpp`, various fixtures) | direct sodium calls |

Every one of these is a place where sodium can be used without any
gate ensuring `sodium_init()` has run.  The 2026-07-04 CI failure
was the first time we lost the race — a test worker that touched
KeyStore without going through AttachProtocol's static-init side
effect exposed the fact that no module was actually responsible for
gating sodium usage.

### Target state

**One module — `SecureSubsystem` (rename from `SecureMemorySubsystem`,
or a wrapper around it) — owns every sodium API.**  Nothing else
includes `<sodium.h>` in production code (tests may still touch
sodium for setup, gated by the module's readiness invariant).

Public surface, sketch:

```cpp
namespace pylabhub::security {

class Secure {
public:
    // ── Random ────────────────────────────────────────────────
    void random_bytes(std::span<std::byte> out);
    std::uint64_t random_u64();
    std::string   uuid4();

    // ── Constant-time ─────────────────────────────────────────
    bool  memcmp_ct(std::span<const std::byte>, std::span<const std::byte>);
    void  memzero(std::span<std::byte>);

    // ── Hash ──────────────────────────────────────────────────
    std::array<std::byte, 32> blake2b(std::span<const std::byte> data);

    // ── Password KDF (vault) ──────────────────────────────────
    std::array<std::byte, 32> pwhash(std::string_view password,
                                     std::span<const std::byte> salt);

    // ── Symmetric AEAD ────────────────────────────────────────
    void secretbox_seal(std::span<std::byte> out,
                        std::span<const std::byte> plaintext,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte, 32> key);
    bool secretbox_open(std::span<std::byte> out,
                        std::span<const std::byte> ciphertext,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte, 32> key);

    // ── Asymmetric — box ──────────────────────────────────────
    // Uses a KeyStore entry by name — seckey never leaves the module.
    void box_seal_using(std::string_view seckey_name,
                        std::span<const std::byte, 32> peer_pubkey,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte> plaintext,
                        std::span<std::byte> out);
    bool box_open_using(std::string_view seckey_name,
                        std::span<const std::byte, 32> peer_pubkey,
                        std::span<const std::byte, 24> nonce,
                        std::span<const std::byte> ciphertext,
                        std::span<std::byte> out);

    // ── Key generation ────────────────────────────────────────
    std::string generate_curve_identity(std::string_view name);
    std::string curve_pubkey_from_seckey(std::string_view seckey_name);

    // ── KeyStore submodule (owned by Secure, not separate) ────
    KeyStore &keys();
    const KeyStore &keys() const;
};

// Global accessor — throws if not constructed.
[[nodiscard]] Secure &secure();

// Non-throwing probe — replaces `sodium_ready()`.
[[nodiscard]] bool secure_ready() noexcept;

} // namespace pylabhub::security
```

Every consumer changes:

- `randombytes_buf(bytes, 16)` → `secure().random_bytes(bytes)`
- `sodium_memzero(p, n)` → `secure().memzero({p, n})`
- `crypto_generichash(...)` → `secure().blake2b(data)`
- `crypto_box_easy(...)` → `secure().box_seal_using(name, ...)`
- `key_store().add_raw(...)` → `secure().keys().add_raw(...)`
- `key_store().with_seckey(...)` → replaced by `secure().box_seal_using(...)` etc.  (Most `with_seckey` callers exist only to call `crypto_box_easy` immediately; folding both into `box_seal_using` eliminates the accessor entirely.)

### What this closes structurally (not by runtime check)

1. **The sodium_init gap.**  You cannot compile a call to sodium
   outside the module because you don't have `<sodium.h>`.
2. **The "seckey escaped the KeyStore" audit surface.**  Every
   asymmetric operation takes a KeyStore name, not a raw seckey.
   The use-not-export contract from HEP-CORE-0040 §5.2 is enforced
   by the type system, not by discipline.
3. **The scattered sodium version knowledge.**  `sodium_library_version_*`
   lives in exactly one place.
4. **Testing.**  One mock target replaces many test-side sodium
   fixture calls.
5. **Cross-platform footprint.**  When Windows / macOS SHM
   backends land, sodium interactions concentrate in one file.

### Sizing

- ~9 production files change from raw sodium → `secure().X(...)`
  wrapper calls.  Most are single-line replacements.
- KeyStore stops being a separate namespace-level module; becomes a
  member submodule of `Secure`.  Existing `key_store()` accessor
  remains as a wrapper for the transition.
- `SecureMemorySubsystem` renamed to `Secure` or wrapped as the
  public class.  Lifecycle module name changes (`"SecureMemory"` →
  `"Security"` or similar) — every registration site updates.
- L2 tests for the wrapper API.
- Documentation update in HEP (see Part 2).

Total: nontrivial refactor, ~500–800 LOC touched, ~3–5 commits.
No behavior change; pure structural cleanup.  Runs green when
2319/2319 tests still pass after the fold.

## Part 2 — HEP consolidation

### Current scatter

| HEP | Topic | Status |
|---|---|---|
| HEP-CORE-0035 | Hub-Role Authentication and Federation Trust | Partial — §4.1 shipped, §4.2, §4.7 in flight |
| HEP-CORE-0036 | Authenticated Connection Establishment (ZMQ CURVE + ZAP) | Shipped bulk; ongoing amendments |
| HEP-CORE-0038 | Script-Accessible Vault Keystore | Design stage |
| HEP-CORE-0040 | Locked Key Memory (SecureMemorySubsystem + KeyStore) | Design shipped, impl in flight |
| HEP-CORE-0041 | SHM Channel Auth (memfd + SCM_RIGHTS handshake) | Phase 1 shipping; observer path in draft |
| HEP-CORE-0042 | Channel Attach Coordination Protocol | Design shipped |

Plus live tech drafts:

- `DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md` — proposed
  KeyStore extension for on-the-fly keypairs + script crypto API
  (task #247).
- `DRAFT_broker_shm_observer_2026-07.md` — D1–D4 designer decisions
  for the broker observer path (task #317 C.2).

Cross-references between these six + two drafts run in every
direction.  Reading any one of them requires paging in three
others.  The 2026-07-04 sodium_init bug was made possible by
HEP-0040 assuming HEP-0036 discipline that HEP-0036 assumed HEP-0035
would establish — a triangle of "someone else's problem."

### Proposal

**Fold into one HEP-CORE-0043 (or renumber HEP-CORE-0040) — "Security Subsystem"** with structure:

```
0. Status + scope
1. Design principles
   1.1 Single-module ownership of libsodium
   1.2 Use-not-export for secret bytes
   1.3 Init gate at module boundary (compile-time, not runtime)
   1.4 Rotation & lifetime
   1.5 Cross-platform layering
2. Module surface (formerly HEP-0040)
   2.1 SecureSubsystem class contract
   2.2 KeyStore submodule
   2.3 Lifecycle registration
   2.4 Cross-platform stubs
3. Random + hash + KDF
4. Symmetric AEAD (vault) (formerly HEP-0038 crypto layer)
5. Asymmetric box (identity + wire) (formerly HEP-0035 + HEP-0036 crypto layer)
6. Vault at rest (formerly HEP-0038 storage layer)
7. Wire authentication protocols
   7.1 ZMQ CURVE + ZAP (formerly HEP-0036)
   7.2 Hub-Role identity & federation (formerly HEP-0035)
   7.3 SHM channel handshake (formerly HEP-0041)
   7.4 Channel Attach Coordination (formerly HEP-0042)
   7.5 Broker SHM observer (formerly the tech draft)
8. Script-facing crypto API (formerly HEP-0038 script layer + task #247)
9. Key rotation, revocation, and observability
10. Cross-platform status
11. Change log
12. Superseded HEPs — link table
```

### Rules of the consolidation

1. **One canonical section per concept.**  If two current HEPs describe
   the same rotation policy, the new HEP has one section on rotation
   and the two old HEPs cite it.
2. **The old six HEPs are marked SUPERSEDED and archived** to
   `docs/archive/superseded-YYYY-MM-DD/` per DOC_STRUCTURE.md §2.2.
   Their content moves into the new HEP; only pointers remain.
3. **Section IDs reflect the module structure** (§2.1 =
   `SecureSubsystem`, §5.3 = `box_seal_using` etc.) so a reader of
   the code can find the design in one hop.
4. **Backward compatibility** for external references: current
   HEP-CORE-0040 §5.2 becomes new HEP §1.2, with a redirect note.
   `git grep 'HEP-CORE-0040'` will find the pointers.

### Why now

The user's 2026-07-04 message hits the timing point:

> "we have real application and needs for key management and
> security management and our code has scattered design all over
> the place"

- **Real applications** — the observer path (#317), the script
  crypto API (#247), the mutual auth work (#262) are all live tasks
  needing coordinated design decisions.  Every one of them cites 3+
  HEPs.
- **Real security posture** — the scattered design is producing
  runtime bugs (sodium_init gap, ordering assumptions).  Bug class
  won't stop until the design is one thing.
- **Cost of NOT consolidating** — every future security task takes
  an extra pass through six HEPs.  We've done this pass ~10 times
  in the last two months.

## Concrete implementation plan

Two follow-on tasks, both filed today:

1. **Module fold — task N (see TODO_MASTER)** — do the C++ refactor
   as described in Part 1.  Ships in ~3–5 commits.
2. **HEP consolidation — task N+1** — write the unified HEP;
   archive old six.  Docs-only.  Ships in 1–2 commits.

Order: HEP consolidation FIRST (so the module fold is guided by the
new HEP structure), then module fold (guided by the unified
design).  The alternative — fold first, then write HEP — risks
re-discovering old constraints late.

## Change log

- **2026-07-04 initial draft** — captures the 2026-07-04 CI-triage
  conversation.  Written after the sodium_init gap was fixed with
  a module-boundary gate (commit `9d0a7eb4`).  Explicit intent:
  the module-boundary gate is a stopgap; the fold is the correct
  architecture.  Both tasks filed.

## Related

- `docs/tech_draft/DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md` — folds into new HEP §2.2 + §8.
- `docs/tech_draft/DRAFT_broker_shm_observer_2026-07.md` — folds into new HEP §7.5.
- HEP-CORE-0035 through HEP-CORE-0042 — folded per Part 2 above.
- Task #247 (script crypto API) — folds into new HEP §8.
- Task #317 (broker SHM observer) — folds into new HEP §7.5.
- Task #262 (SHM mutual auth) — folds into new HEP §7.3.

## Open questions for designer

1. Numbering: reuse `HEP-CORE-0040` (which is the closest fit
   scope-wise and would minimize external-reference churn), or
   allocate a new number (`HEP-CORE-0043` or `-0044`) and mark
   0035/0036/0038/0040/0041/0042 as SUPERSEDED?
2. Should HEP-0042 (Channel Attach Coordination) fold in?  It's
   about wire ordering, not security per se — but the coordination
   is entangled with the SHM auth path.
3. Does the script vault (HEP-0038) belong in the security HEP or
   in a separate "script APIs" HEP?  Argument for folding: it uses
   the same KeyStore.  Argument against: it's a script-facing
   concern, not a security concern.
