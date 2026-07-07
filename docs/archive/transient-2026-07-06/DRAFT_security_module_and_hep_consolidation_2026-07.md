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

## Design self-review (2026-07-04)

Reading the sketch back against HEP-CORE-0040 as it exists today
and against the two live tech drafts.  Eight real issues found —
each needs a designer decision before SEC-Fold-1 promotion.

**R1 — Static vs dynamic lifecycle module split.**
HEP-0040 §4 declares `SecureMemorySubsystem` STATIC (constructed
at `main()` entry, holds no keys) and §5 declares `KeyStore`
DYNAMIC (registered after SMS, key inserts happen at arbitrary
runtime moments — vault decrypt, script vault_save, broker's
ephemeral observer keypair via `generate_and_add_identity`).  The
fold sketch talks about "Secure owns KeyStore" but doesn't say
whether the merged module is static or dynamic.  If STATIC, we
can't support the runtime keypair generation (broker observer
regen on restart, script-generated keys).  If DYNAMIC, we lose
the "guaranteed constructed at main entry" invariant.
**Resolution needed:** the fold must preserve the two-phase
lifecycle.  Suggest: keep `SecureSubsystem` static (owns sodium
init + platform hardening + KeyStore's storage backend); expose
`secure().keys()` returning a `KeyStore &` that's still logically
dynamic (mutations tracked, but no separate lifecycle module
registration — the map is a member of `SecureSubsystem`).

**R2 — Naming: `Secure` vs `Security` vs `SecureMemorySubsystem`.**
The sketch uses `Secure` / `secure()`.  Current code uses
`SecureMemorySubsystem` / `secure_memory_subsystem()`.  The fold
covers hashes, random, box, KDF, AEAD — not just memory.  Three
options:
- **(a)** Rename to `Security` / `security()`.  Semantically
  correct but breaks every existing reference (~60 sites) and
  reintroduces the ambiguity with "Authentication" (which is a
  narrower slice).
- **(b)** Keep `SecureMemorySubsystem` / `secure_memory_subsystem()`.
  Semantically misleading (it's not just memory) but preserves the
  existing surface.
- **(c)** Introduce `SecureSubsystem` / `secure()` as a new class,
  fold `SecureMemorySubsystem` into it as an implementation detail.
  Cleanest rename; ~60 sites still need updating.
**Suggested: (c).**  Big rename, but done once.

**R3 — AttachProtocol is not a crypto primitive.**
Part 1's sketch has `secure().attach()` — but AttachProtocol is
1000+ LOC of framing + poll + EINTR handling + mutual-auth Frame
3 + observer verify + SCM_RIGHTS handover.  Only a small subset
(the `crypto_box_easy`/`crypto_box_open_easy` calls) is crypto.
Wrapping the whole protocol in `secure()` is a category error.
**Resolution needed:** AttachProtocol continues to exist as its own
subsystem; it USES `secure().box_seal_using(...)` /
`secure().box_open_using(...)` for the crypto steps.  No
`secure().attach()` in the fold.  Same for vault_crypto — the
crypto primitives it uses (pwhash, secretbox) go into `secure()`;
the vault file format + salt derivation stays in `vault_crypto`.

**R4 — Section 4 vs 5 concept conflation in the fold structure.**
Part 2 §4 is labeled "Symmetric AEAD (vault)" but `crypto_pwhash`
(argon2id KDF used by vault) isn't AEAD — it's password hashing.
And §5 is labeled "Asymmetric box (identity + wire)" mixing
identity storage (a KeyStore concern) with wire operations (a
crypto concern).  **Suggested restructure:**
```
3. Random + hash
4. KDF (pwhash — argon2id)
5. Symmetric AEAD (secretbox — vault at rest)
6. Asymmetric box (crypto_box — wire + observer)
7. KeyStore submodule
8. Vault at rest (file format, salt, path discipline)
9. Wire authentication protocols
10. Script-facing crypto API
```
This makes each section one concept.

**R5 — HEP-0042 folding decision.**
Part 2 §7.4 folds HEP-0042 into the unified HEP.  Reading
HEP-0042 back: it's about `instance_id` echo, `applied_req` timing,
`snapshot_version` — pure protocol coordination, no crypto.  The
only tie to security is that channel attach + auth need to happen
in a coordinated order.  **Decision: LEAVE HEP-0042 out of the
fold.**  Cross-reference from the security HEP §7 (wire auth) to
HEP-0042 for ordering, but keep HEP-0042 as its own doc.  Same
for HEP-0035 — it's about federation trust (multi-hub identity
verification), not security primitives; keep it, cross-reference.

**Revised superseded list:** HEP-0036, HEP-0038, HEP-0040, HEP-0041.
Four HEPs fold in, not six.

**R6 — Script crypto API vs KeyStore API coupling.**
`DRAFT_keystore_ephemeral_and_script_crypto_2026-07.md` proposes
that script bindings TRANSLATE script-provided names (`"mykey"`)
into sandboxed storage names (`"script.<uid>.mykey"`).  The
consolidation sketch's §10 (script-facing crypto API) doesn't
mention this translation layer.  **Resolution:** the script API
lives OUTSIDE `Secure` — in the language binding layer.
`Secure::keys()` sees fully-qualified names only.  The binding
layer translates.  Document this explicitly in the fold — otherwise
the next impl attempt will bake script sandboxing into `Secure` and
we'll fight it later.

**R7 — LockedKey isn't in the sketch.**
Part 1's wrapper API talks about `KeyStore` methods but doesn't
mention `LockedKey`.  `LockedKey` is the RAII wrapper around
`sodium_malloc` — HEP-0040 §6.  Under the fold, LockedKey is
still needed (it's the only way to hold a seckey in mlocked
memory).  It stays as an implementation detail of `KeyStore`;
never exposed at the fold's public surface.  Note this explicitly
so no one tries to expose it.

**R8 — `Z85PublicKey` strong type doesn't get mentioned.**
HEP-0040 §8.4.1 introduced `Z85PublicKey` as a strong type for
pubkeys on the wire (task #231, shipped).  The fold sketch uses
raw `std::span<const std::byte, 32>` for pubkey arguments in
`box_seal_using`.  That inconsistency will bite — the codebase
elsewhere uses `Z85PublicKey` for identity, then unwraps at the
crypto call site.  **Resolution:** `secure().box_seal_using`
accepts `Z85PublicKey` (or its raw form) directly.  The unwrap
happens once inside `Secure`, not at every caller.

**Meta-observation:** all eight issues are the kind that would
have surfaced in first design review of the unified HEP.  Doing
the review NOW before promotion means the promoted HEP already
addresses them.  Doing them AFTER promotion means each becomes
a §12 amendment.

## Resume checklist (for future sessions picking up SEC-Fold)

**State as of 2026-07-04:**

- SEC-Fold is **NOT STARTED** as code.  Only tech-draft (this
  file) + task filing in `docs/todo/AUTH_TODO.md`.
- CI-triage stopgap `9d0a7eb4` is committed + pushed.  Do NOT
  revert — that's the working state until SEC-Fold-2 replaces it.
- Two drafts feed into the fold: `DRAFT_keystore_ephemeral_and_
  script_crypto_2026-07.md`, `DRAFT_broker_shm_observer_2026-07.md`.
  Both stay valid; will be superseded by the unified HEP at
  SEC-Fold-1 promotion.
- Design self-review addendum (above, R1-R8) MUST be addressed in
  SEC-Fold-1's HEP draft — don't promote without them.

**Order to resume:**

1. **Confirm CI status of `9d0a7eb4`.**  If sodium test still
   fails, read the new debug log lines (`[SMS] event=SodiumInit`,
   `[KeyStore] event=AddRaw`, `[LockedKey] event=SodiumMalloc`) and
   decide from evidence.  If those show `sodium_ready=true` +
   `buf_null=false` + still assertion, we have a deeper libsodium
   issue and SEC-Fold isn't the fix — file separately.
2. **SEC-Fold-1 — HEP consolidation.**  Author the unified HEP,
   addressing R1-R8.  Section structure per R4's revised list.
   Old four (0036, 0038, 0040, 0041) marked SUPERSEDED.  1-2
   commits.  See `docs/DOC_STRUCTURE.md §2.2` for archive path.
3. **SEC-Fold-2 — module fold.**  C++ refactor.  Start with:
   (a) rename `SecureMemorySubsystem` → `SecureSubsystem` (R2 (c)).
   (b) add wrapper methods for random / hash / KDF / AEAD / box.
   (c) update consumers file by file: `uuid_utils.cpp`,
       `crypto_utils.cpp`, `vault_crypto.cpp`, `hub_vault.cpp`,
       `attach_protocol.cpp`.
   (d) delete raw `<sodium.h>` includes from those files.
   (e) rebuild + 2319/2319 ctest after each file.
   ~3-5 commits.  See sizing in Part 1.
4. **Return to #317 C.2.c-C.5** on top of the new architecture.
5. **Retire the tech drafts** (this one, keystore, observer) —
   archive per DOC_STRUCTURE.md §2.2 with a note "content merged
   into unified HEP §X.Y".

**In-flight work not blocked by SEC-Fold:**
- CI monitoring on `9d0a7eb4` — passive.
- #262 mutual auth L4 test — can complete before or after SEC-Fold-2.
- Anything on the #317 C.2 chain — can pause here indefinitely.

**Uncommitted state:** none.  All work as of 2026-07-04 push
`a5d81e35` is committed and pushed.

## Change log

- **2026-07-04 initial draft** — captures the 2026-07-04 CI-triage
  conversation.  Written after the sodium_init gap was fixed with
  a module-boundary gate (commit `9d0a7eb4`).  Explicit intent:
  the module-boundary gate is a stopgap; the fold is the correct
  architecture.  Both tasks filed.
- **2026-07-04 self-review addendum** — R1-R8 design issues found
  during first re-read.  Resume checklist added.  Must be addressed
  in SEC-Fold-1's HEP draft.

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
