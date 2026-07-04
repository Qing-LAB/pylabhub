# SEC-Fold-2 Full Migration Plan

**Status:** IN PROGRESS (Phase A open, 1 of ~11 production files
migrated).
**Owner:** in-session assistant.
**Anchors:** HEP-CORE-0043 (design), HEP-CORE-0001 §"Owner-managed
teardown" (lifecycle contract), memory files
`feedback_persistent_test_artifacts.md`,
`feedback_log_format_convention.md`.
**Related tasks:** #74 (HEP-0035 auth impl — not in scope), #106
(HEP-0038 impl — folded into this plan).

## Purpose

Track the exhaustive plan for finishing the SEC-Fold-2 refactor:
concealing every sodium call under the `SecureSubsystem` wrapper,
folding docs into HEP-CORE-0043, and moving all test coverage onto
the new binary-lifecycle fixture.  This document persists across
sessions so context resets don't drop the plan on the floor.

## Current state (honest audit, 2026-07-04)

| Aspect | State |
|---|---|
| SEC-Fold-1 (umbrella HEP) | ✅ landed at commits `9d78b616`, `40975ead`, `cbbab8db` |
| SMS three-state atomic + PANIC gate | ✅ landed at `3c40588d` |
| Wrapper API scaffolded | ✅ `random_bytes`, `memcmp_ct`, `memzero` present in `SecureSubsystem` |
| Log-order race framework rule | ✅ documented + fixed at `c86ba5e2` |
| Persistent test log discipline | ✅ documented in memory + `feedback_persistent_test_artifacts.md` |
| **Production files migrated** | **1 of ~11** (`uuid_utils.cpp`) |
| **Test binaries migrated** | **1 of ~5** (`test_layer0_uuid_and_format`) |
| **HEP-0043 §3-§10 content** | **stubs only** (§0-§2 authoritative) |
| **Old HEPs archived** | **no** (banner added; content still authoritative there) |
| **CI lint enforcement** | **no** (`<sodium.h>` outside module still accepted) |

## Blocking issue right now

36 tests failing on this build (`HubVaultTest.*`, `RoleVaultTest.*`)
because `uuid_utils.cpp` migration made `generate_uuid4()` require
SMS, but `test_layer2_hub_vault` + `test_layer2_role_vault` binaries
still have no lifecycle fixture.  **Cannot commit uuid_utils
migration until this is fixed or reverted.**  → Phase A.

## Phase plan

### Phase A — Unblock the vault-test binaries

**Scope:** two test binaries (`test_layer2_hub_vault`,
`test_layer2_role_vault`).

**Change:** add the same `PLH_BINARY_LIFECYCLE_MODULES(...)` +
`SecureSubsystemBinaryEnvironment` scaffold that
`test_uuid_and_format.cpp` now has.  Match the shape from
`test_hub_zmq_queue.cpp:103-140`.

**Deliverable:** 2319/2319 pass with `uuid_utils.cpp` migrated.
Two commits max (one code, one commit-and-push).

**Follow-on:** these two binaries are the last remaining
lifecycle-less test binaries that transitively touch sodium.  The
fixture-boilerplate cost is what motivates Phase B.

### Phase B — Shared fixture helper

**Scope:** `tests/test_framework/binary_lifecycle.h`.

**Change:** add a new macro `PLH_BINARY_LIFECYCLE_WITH_SECURE(...)`
that:

1. Registers the static modules (Logger, ...) via the existing
   `PLH_BINARY_LIFECYCLE_MODULES` mechanism.
2. Registers a second `::testing::Environment` that constructs a
   static `SecureMemorySubsystem sms;` in its `SetUp()`.
3. Order-preserving: static modules come up first via
   LifecycleGuard; SMS constructs second.

Rewrite the three already-migrated binaries (uuid, hub_vault,
role_vault) to use the new macro.

**Deliverable:** one-line invocation for future migrations.  Any
test binary that needs SMS just adds
`PLH_BINARY_LIFECYCLE_WITH_SECURE(Logger::GetLifecycleModule())`.

**Not in scope:** a similar helper for `KeyStore` — that fixture
is different (needs owner-uid + scope-tag + identity seeding);
`CurveKeyStoreFixture` already handles it for tests that need it.

### Phase C — Production consumer migration

**Sequenced by primitives needed** (simpler ones first so the
wrapper API grows in dependency order):

#### C.1 — `crypto_utils.cpp`

- Primitives: `randombytes_buf` (already wrapped), `blake2b`.
- Add wrapper: `secure().blake2b(std::span<const std::byte>) →
  std::array<std::byte, 32>`.
- Migrate `crypto_utils::compute_blake2b` + any other callers to
  `secure().blake2b(...)`.
- Delete `<sodium.h>` include.
- Verify: 2319/2319 pass; no test-binary fixture change needed
  (crypto_utils is used from L2+ binaries that already have SMS).

#### C.2 — `vault_crypto.cpp`

- Primitives: `crypto_pwhash` (Argon2id), `crypto_secretbox_easy`,
  `crypto_secretbox_open_easy`, `sodium_memzero` (already wrapped),
  `randombytes_buf` (already wrapped), `crypto_generichash` (via
  blake2b wrapper from C.1).
- Add wrappers:
  - `secure().pwhash(std::string_view password,
    std::span<const std::byte> salt) → std::array<std::byte, 32>`.
  - `secure().secretbox_seal(out, plaintext, nonce, key)` +
    `secure().secretbox_open(out, ciphertext, nonce, key) →
    bool`.
- Migrate `vault_crypto::vault_derive_key`,
  `vault_encrypt`, `vault_decrypt`.
- Delete `<sodium.h>` include.
- Verify: HubVaultTest + RoleVaultTest still pass (they use vault
  APIs indirectly).

#### C.3 — `hub_vault.cpp` + `role_vault.cpp`

- Primitives: `sodium_memzero` (already wrapped).
- No new wrappers.
- Migrate the `sodium_memzero` calls to
  `secure().memzero(std::span{...})`.
- Delete `<sodium.h>` includes.
- These files also depend on C.2 wrappers being live.

#### C.4 — `curve_keypair.cpp`

- Primitives: likely `crypto_box_keypair`.
- Investigate first: is this file's function `KeyStore::generate_
  and_add_identity` in disguise, or a separate utility?  Likely a
  transitional helper that KeyStore's ephemeral-key path already
  supersedes.  If so, delete the file entirely.
- If kept: add `secure().generate_box_keypair() → BoxKeypair`.
- Migrate callers.
- Delete `<sodium.h>` include.

#### C.5 — `attach_protocol.cpp`

- Primitives: `crypto_box_easy`, `crypto_box_open_easy`,
  `sodium_memcmp` (already wrapped).
- Add wrappers (use-not-export per HEP-0043 §1.4):
  - `secure().box_seal_using(std::string_view seckey_name,
    const Z85PublicKey &peer_pubkey, std::span<const std::byte, 24>
    nonce, std::span<const std::byte> plaintext, std::span<std::
    byte> out)`.
  - `secure().box_open_using(...)` mirror.
- Migrate:
  - `AttachProtocolInitiator`'s `crypto_box_easy` call →
    `secure().box_seal_using(seckey_name, peer_pubkey, ...)`.
  - `AttachProtocolAcceptor`'s `crypto_box_open_easy` call →
    `secure().box_open_using(...)`.
  - Observer verify path's `sodium_memcmp` → `secure().memcmp_ct`
    (already wrapped).
- Delete `<sodium.h>` include.
- Verify: L3 SHM auth tests + L4 SHM e2e tests still pass.

#### C.6 — `broker_service.cpp`

- Primitives: small usage — grep to confirm scope.
- Migrate the specific calls found.
- Delete `<sodium.h>` include.

#### C.7 — `key_store.cpp`

- Stays INSIDE the module by design (HEP-0043 §2.2).
- No migration needed — but check that `#include <sodium.h>` is the
  only sodium touch; if `LockedKey`'s internals leak sodium types to
  callers, hide them behind pImpl.

#### C.8 — `secure_buffer.hpp` (public header)

- **Problem:** public header currently includes `<sodium.h>` — that
  pulls sodium into every consumer's translation unit.
- Options:
  1. Move `SecureBuffer` to `.cpp` behind pImpl.
  2. Delete `SecureBuffer` if unused (audit callers first).
  3. Keep header but remove the sodium include, forward-declare
     what's needed.
- Decide based on caller audit.

### Phase D — CI lint enforcement

**Scope:** CI check.

**Change:** add a CMake test / GitHub Action step:

```bash
if grep -rl '#include *<sodium.h>' src/ | grep -v '^src/utils/security/'; then
    echo "ERROR: sodium.h included outside src/utils/security/"
    exit 1
fi
```

**Deliverable:** structural guarantee.  Cannot land after Phase C
without breaking the build.  Any future PR that tries to
`#include <sodium.h>` outside the module fails CI.

**Bonus:** after Phase C-8, also lint that `secure_buffer.hpp`
doesn't re-export sodium types.

### Phase E — Class rename

**Scope:** `SecureMemorySubsystem` → `SecureSubsystem` in all files.

**Change:** mechanical rename via `git grep -l SecureMemorySubsystem`
+ per-file edit.  Then delete the type alias `using SecureSubsystem =
SecureMemorySubsystem;`.  Rename the header + .cpp too.

**Deliverable:** class name matches HEP-0043 §2.1.

**Order:** after Phase D lint lands, so no stray sodium.h includes
survive the rename.

### Phase F — KeyStore submodule fold

**Scope:** merge `KeyStore` into `SecureSubsystem` as a member.

**Change:**

1. Move `KeyStore` from top-level module to `SecureSubsystem::keys()`
   accessor.
2. Delete the separate `"KeyStore"` LM dynamic module registration
   (KeyStore construction now happens as a member of SMS's ctor).
3. `key_store()` free function stays as a shim (`secure().keys()`)
   during transition; deleted after all callers migrated.
4. Update HEP-0043 §2.2 to reflect this final shape (matches doc
   already).
5. Update HEP-CORE-0001 dependency list (was `SecureMemory` +
   `KeyStore`; becomes `SecureSubsystem`).

**Deliverable:** one lifecycle module, `"SecureSubsystem"`, per
HEP-0043 §2.3.

### Phase G — HEP content migration (SEC-Fold-1b)

**Scope:** fill in HEP-0043 §3-§10 content from old HEPs.

**Sections in migration order (smallest first):**

1. **§3 random + hash** (~50 lines from HEP-0038 §2).
2. **§4 KDF (pwhash)** (~100 lines from HEP-0038 §3).
3. **§5 AEAD (secretbox)** (~150 lines from HEP-0038 §4).
4. **§6 asymmetric box** (~200 lines from HEP-0040 §5.5 +
   HEP-0041 §D4.5).
5. **§7 KeyStore full API** (~600 lines from HEP-0040 §5-§8 —
   biggest single migration).
6. **§8 vault at rest** (~400 lines from HEP-0038 §5-§9).
7. **§9.3 broker observer** (~200 lines from tech_draft — new
   NORMATIVE content).
8. **§9.2 SHM channel** (~800 lines from HEP-0041 §D — big).
9. **§9.1 ZMQ CURVE + ZAP** (~1200 lines from HEP-0036 — biggest;
   ships as separate commits given size).
10. **§10 script crypto API** (~200 lines, awaits task #247).

**Cross-cutting additions in HEP-0043:**

- Mermaid diagrams:
  - Module dependency graph (Logger → SecureSubsystem → consumers).
  - Sodium call graph (which wrapper each consumer uses).
  - Wrapper API surface (class diagram).
  - Ctor / dtor state machine (Uninit → InitCalled → Initialized →
    ShuttingDown → Shutdown).
- Worked examples:
  - Keypair generation via `KeyStore::generate_and_add_identity`.
  - `box_seal_using` / `box_open_using` full example.
  - Vault open flow (pwhash → secretbox_open → KeyStore populate).
  - Test binary bringup (canonical PLH_BINARY_LIFECYCLE_WITH_SECURE).
- Full API reference table (every public method of `SecureSubsystem`
  + `KeyStore`, with 1-line description + `#include`).
- Dependency list (Logger, LifecycleManager).

**Deliverable:** HEP-0043 stands alone.  Nobody needs to read
HEP-0036, 0038, 0040, or 0041 to understand the security module.

### Phase H — Old HEP archival

**Scope:** HEP-CORE-0036, 0038, 0040, 0041.

**Change:** for each:

1. Delete authoritative content sections (they now live in
   HEP-0043).
2. Leave banner + one-line pointer to the HEP-0043 section.
3. Archive per `DOC_STRUCTURE.md §2.2` → `docs/archive/hep-superseded-2026-07/`.
4. Record in `docs/DOC_ARCHIVE_LOG.md`.
5. Update any cross-references in other HEPs.

**Deliverable:** HEP-0043 is the only place with detail.

### Phase I — SEC-Fold review + close-out

**Scope:** end-to-end verification.

**Change:**

1. Fresh-eye code review of the full fold via `code-review` workflow.
2. Grep verifications:
   - Zero `#include <sodium.h>` outside `src/utils/security/*.cpp`.
   - Zero direct `randombytes_buf` / `crypto_*` / `sodium_*` calls
     outside `src/utils/security/`.
   - All test binaries using `PLH_BINARY_LIFECYCLE_WITH_SECURE` or
     equivalent.
3. Update `docs/todo/AUTH_TODO.md` — mark SEC-Fold-1, SEC-Fold-2,
   SEC-Fold-1b all complete.
4. Update `docs/TODO_MASTER.md` — remove SEC-Fold resume section.
5. Push final commit.

**Deliverable:** SEC-Fold complete.

## Rough scale

| Phase | Commits | LOC change | Difficulty |
|---|---|---|---|
| A | 1-2 | ~50 | trivial |
| B | 1 | ~100 | small |
| C.1-C.8 | 8 | ~50-200 each, ~1000 total | medium (each) |
| D | 1 | ~30 | trivial |
| E | 1-2 | ~500 (rename all) | trivial-but-mechanical |
| F | 1-2 | ~300 | medium |
| G | 6-10 (per section) | ~4000 doc lines total | high (doc rigor) |
| H | 1 | ~100 (mostly deletion) | small |
| I | 1 | ~50 (task updates + review notes) | small |
| **Total** | **~20-25 commits** | **~6000 lines touched** | — |

## Order + parallelism

- A → B are strictly sequential (A unblocks tests; B is code we've
  already discovered we need for B onwards).
- C.1-C.8 can be interleaved with G (docs) once the module surface
  stabilizes.  Practically: finish C first for hard focus.
- D locks in after C-7 (moment when no consumer imports sodium).
- E → F can follow C or run in parallel with G.
- G is the biggest chunk by wall-time; can be broken into per-section
  commits.
- H requires G done; I requires everything.

## When to check in with the user

- After Phase A + B (fixture stabilized).
- After Phase C.5 (halfway through migration — verify the
  use-not-export box wrappers work as intended for `AttachProtocol`).
- After Phase D (CI lint locked in; guarantees preserved).
- After Phase G section 5 (KeyStore doc — biggest content dump;
  worth a review pass).
- Final: after Phase I (SEC-Fold complete).

## Discipline reminders (from memory files)

- `feedback_no_flake_explanations.md` — every test failure is
  investigated, never labeled flake.
- `feedback_read_log_before_rerun.md` — capture failure output
  before any rerun.
- `feedback_persistent_test_artifacts.md` — L4 hub/role logs
  survive under `build/stage-*/test_artifacts/`; check first.
- `feedback_log_format_convention.md` — new log lines use
  `[component] event=EventName` convention 1.
- `feedback_tests_replicate_production_scenarios.md` — tests bring
  up SMS the same way production does (`LifecycleGuard`), not
  per-test stack-local hacks.
- `feedback_engineer_discipline.md` — findings → discuss → approval
  → code.
- `feedback_analyze_actual_code.md` — read source, not comments.
- `feedback_lib_change_full_sweep.md` — after any lib change, run
  full ctest unfiltered.

## Next concrete step

Fix the 36 vault-test failures (Phase A) — add
`PLH_BINARY_LIFECYCLE_MODULES` + `SecureSubsystemBinaryEnvironment`
to `test_hub_vault.cpp` and `test_role_vault.cpp`.  Same shape as
`test_uuid_and_format.cpp`.  Then commit uuid_utils migration + vault
binary fixtures together.  Then start Phase B (shared helper).
