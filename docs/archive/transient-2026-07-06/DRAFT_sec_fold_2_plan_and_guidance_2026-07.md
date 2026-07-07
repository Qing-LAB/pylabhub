# SEC-Fold-2 Plan and Guidance (Consolidated 2026-07-06)

**Status:** IN PROGRESS — Phase 1 partially landed, needs the KeyStore
merger to reach the intended shape.
**Task:** Fold both `SecureMemorySubsystem` AND `KeyStore` into ONE
properly-registered lifecycle module (`SecureSubsystem`) that owns
libsodium + the process-wide keystore.  This is a **refactoring and
reorganization** — every legacy pattern (stack-local ctor,
dynamic self-registration, per-scope KeyStore instances) is a delete
target, not a template to preserve.
**Anchors:** HEP-CORE-0043 (§1 contract, §2 module surface), HEP-CORE-0040
(§5.2 KeyStore API surface — preserved through the merger),
HEP-CORE-0001 (lifecycle model), `docs/README/README_testing.md`
§ "framework contract (absolute)".

---

## 0. The pattern rule — HIGHEST PRIORITY

Memory file: `feedback_sms_five_point_pattern.md`.  Reload before every
code change.

**Five points, both SMS and KeyStore:**

1. **One construction per process** — via `SecureSubsystem::GetLifecycleModule()`
   in a `LifecycleGuard` mod pack.  Startup thunk calls
   `SecureSubsystem::instance()`, which constructs the `sole`
   function-local static and drives `pImpl->bringup()`.
2. **Access via `secure()`** — `secure().random_bytes(...)`,
   `secure().memcmp_ct(...)`, **`secure().keys().add_identity(...)`**,
   **`secure().keys().pubkey(...)`**.  KeyStore lives at
   `secure().keys()` post-merger; no independent KeyStore object.
3. **Never construct SMS or KeyStore directly.**  No `SecureMemorySubsystem sms;`
   on any stack.  No `KeyStore ks("hub", uid);` on any stack.  No
   `static SecureMemorySubsystem` in any Environment.  No
   `unique_ptr<KeyStore>`.  No fixture holding a KeyStore member.
4. **Downstream modules that depend on crypto declare
   `add_dependency("SecureSubsystem")`.**  Ordering via LifecycleManager
   topo sort.  Callers do NOT do "if not up construct here".
5. **Same shape in production and tests.**  Every construction of a
   `LifecycleGuard` — `role_lifecycle_modules()`, Pattern-3 worker
   `run_gtest_worker(..., mods...)`, parent-lifecycle
   `PLH_BINARY_LIFECYCLE_MODULES` — includes `SecureSubsystem::GetLifecycleModule()`.
   No test-only scaffold; no `ScopedKeyStore`; no `CurveKeyStoreFixture`
   (RETIRED 2026-07-06 → free function `seed_curve_identities(setup)`).
   Fixtures use `secure().keys()`.

**Legacy code is a delete target, never a template.**  If a test fails
after a delete, the answer is "add SMS to the mod pack" or
"convert to Pattern 3" — never "restore the stack-local".
If a caller compile-fails, the answer is "use `secure().keys()`" —
never "reinstate the `KeyStore ks(...);` line".

---

## 1. Naming discipline

**Every new mention uses `SecureSubsystem`.**  Never write
`SecureMemorySubsystem` in new code, comments, log messages, PANIC
strings, or docs.  Legal remaining uses of the old name (until Phase 4
class rename):

- The class declaration line itself.
- The `using SecureSubsystem = SecureMemorySubsystem;` alias line.
- Ctor / dtor / method DEFINITIONS in the `.cpp` — scope resolution
  and ctor/dtor NAMES must match the class name (no class of the
  aliased name exists).
- Archived HEPs.

For KeyStore: HEP-0043 §2.2 keeps the class name `KeyStore` — no
rename.

---

## 2. Absolute rules

**R1. No stack-local `SecureSubsystem sms;`.  No stack-local
`KeyStore ks(...);`.**  In prod, tests, workers, fixtures, Environments,
anywhere.  Every existing site is a delete target.

**R2. SMS + KeyStore both enter a process via
`SecureSubsystem::GetLifecycleModule()` in a mod pack.**  KeyStore is
NOT a separate lifecycle module.  KeyStore is a MEMBER of
`SecureSubsystem`, accessed via `secure().keys()`.

**R3. Test parent processes own their lifecycle via a mod pack — same
mechanism as production.**  R3 is NOT a ban on adding SMS to
`PLH_BINARY_LIFECYCLE_MODULES` or a `BinaryLifecycleEnvironment`.
It IS a ban on parent-side scaffolding that constructs SMS/KeyStore
outside the mod-pack mechanism (`static SecureMemorySubsystem` in a
custom `::testing::Environment::SetUp()`, hand-crafted "if
key_store_ready then don't rebringup" checks, etc.).

**R4. No production-side test bypasses.**  No `reset_state_for_test`,
no `sms_reset_after_fork`.

**R5. No test-framework mods.**  Do NOT add SMS-awareness to
`run_gtest_worker`, `run_worker_bare`, `BinaryLifecycleEnvironment`,
etc.  Callers decide what's in the mods list.

**R6. Read `feedback_sms_five_point_pattern.md` and this doc's §0
before proposing ANY test-side accommodation.**

---

## 3. Phase order (strict, consolidated)

The task is one landing.  Multiple phases only for commit hygiene —
each is a discrete diff, not a checkpoint you can stop at.  Each
phase leaves the tree buildable + green (or with a documented,
tracked delta).

### Phase 1 — SMS proper static lifecycle module + KeyStore merger (SHIPPED 2026-07-06)

**Deliverable:** SMS is a Logger-shape static lifecycle module.
Ctor is trivial.  Bringup runs from the startup thunk via
`instance()` → `pImpl->bringup()`.  No dynamic self-registration.
No file-scope `g_sms` rendezvous pointer.

Current state (as of 2026-07-06):
- Header: `SecureMemorySubsystem::instance()` + `GetLifecycleModule()` +
  `lifecycle_initialized()` present.  Friend startup/shutdown thunks
  declared.  Ctor still public (Phase 4 makes it private after all
  callers migrate).
- `.cpp`: trivial ctor + dtor.  `Impl::bringup()` +
  `Impl::shutdown_module()` on Impl.  Startup thunk = `instance().pImpl->bringup()`.
  `g_sms` file-scope pointer STILL EXISTS as a transitional accessor
  path — DELETE in Phase 1c.  Bringup Step 6 dynamic self-registration
  STILL EXISTS as a transitional scaffold — DELETE in Phase 1c.

**Phase 1a — cleanup of transitional scaffolds — SHIPPED**:
1. ✓ `g_sms` file-scope pointer deleted from `secure_memory_subsystem.cpp`.
2. ✓ Step 6 dynamic self-registration deleted from `Impl::bringup()`.
3. ✓ Accessors `secure_memory_subsystem()` / `secure()` return
   `SecureSubsystem::instance()` after state gate.
4. Not done: `Impl::sodium_init_rc` field kept (diagnostic value).

### Phase 1b — KeyStore merged INTO SecureSubsystem — SHIPPED

**Deliverable:** KeyStore is a MEMBER of `SecureSubsystem`, accessed
via `secure().keys()`.  No separate `KeyStore` lifecycle module.
No `KeyStore` ctor call anywhere in the codebase.

**Steps:**

1b.1  Move `KeyStore` state onto `SecureSubsystem::Impl`.  Add
      `KeyStore keys_;` as a member (or `std::optional<KeyStore>` if
      construction order requires it).  Initialize inside
      `Impl::bringup()` AFTER `sodium_init` succeeds — KeyStore's
      allocations need sodium.

1b.2  Add `KeyStore &SecureSubsystem::keys()` accessor.  Returns
      `pImpl->keys_`.  Panic-gated same as other wrappers.

1b.3  Delete from `key_store.cpp`:
      - `g_keystore` file-scope pointer.
      - `g_keystore_mu` mutex.
      - "already constructed" throw in ctor.
      - Entire lifecycle-module block: `kModuleName`,
        `ks_impl_validate`, `ks_startup`, `ks_shutdown`,
        `register_dynamic_module`, `LoadModule`, `unload_module` in
        dtor, `set_owner_managed_teardown(true)`.

1b.4  KeyStore ctor becomes no-args (or `explicit KeyStore()` with a
      friend of `SecureSubsystem::Impl`).  Diagnostic labels
      (`scope_tag`, `owner_id`) become optional via
      `KeyStore::set_diagnostic_labels(scope, uid)` set by production
      main after config load.  Test binaries use default labels.

1b.5  Keep `pylabhub::utils::security::key_store()` as an INLINE
      SHIM in the header — `inline KeyStore &key_store() { return
      secure().keys(); }`.  Zero call-site churn on the ~50 sites
      that use `key_store()`.  Post-Phase-4 the shim is deleted and
      callers migrate to `secure().keys()` grep-mechanically.

1b.6  HEP-0043 §2.2 wording sync: KeyStore-as-member is realized;
      §7 (currently stub) gets a "see §2.2" pointer or is filled with
      the KeyStore API surface from HEP-0040 §5.2.

### Phase 1c — Delete every SMS + KeyStore stack-local site — SHIPPED

**Deliverable:** grep across `src/` + `tests/` for
`SecureMemorySubsystem\|KeyStore\s+\w+\s*(` returns ONLY:
- The class declaration lines (`class SecureMemorySubsystem`, `class KeyStore`).
- The `using SecureSubsystem` alias line.
- The single function-local `static SecureMemorySubsystem sole;` inside
  `instance()`.
- The `KeyStore keys_;` member inside `SecureSubsystem::Impl`.

**Sites to delete** (grep-verified — update on execution):

**Production:**
- `src/plh_hub/plh_hub_main.cpp:448` — was `sec::SecureMemorySubsystem sms;`
  (DONE).  Also `KeyStore ks("hub", cfg.identity().uid);` — DELETE.
  Replace with `secure().keys().set_diagnostic_labels("hub", cfg.identity().uid);`
  after mod pack drives bringup.
- `src/plh_role/plh_role_main.cpp:320` — same treatment (SMS DONE;
  KeyStore delete pending).

**Test framework:**
- `tests/test_framework/curve_test_setup.h`  `seed_curve_identities()` —
  DELETE the `sms_` member (DONE).  DELETE the `ks_` member.  Fixture
  no longer owns anything — it just seeds identities into
  `secure().keys()`.  Rename to `CurveIdentitySeeder` if the
  "fixture" naming becomes misleading.

**Test workers (Pattern 3 — worker mods pack owns SMS bringup):**
- `tests/test_layer2_service/workers/key_store_workers.cpp` — 17
  `SecureMemorySubsystem sms;` DONE.  Any `KeyStore ks(...)` inside
  lambdas — DELETE, replace with `secure().keys()`.
- `tests/test_layer2_service/workers/zmq_queue_auth_workers.cpp`
  `ScopedKeyStore` class — DELETE the whole class.  Tests call
  `secure().keys()` directly.
- Sweep other worker files for `KeyStore ks(...)` — delete each,
  route through `secure().keys()`.

**Parent-lifecycle tests (per R3, mod pack is the mechanism):**
- `tests/test_layer2_service/test_hub_zmq_queue.cpp`
  `ZmqQueueTestEnvironment::SetUp()` — the `static KeyStore ks;` is
  DELETED (KeyStore is now inside SMS; identity seeding uses
  `secure().keys().add_identity_from_z85(...)` directly).
- `tests/test_layer2_service/test_role_handler.cpp`
  `RoleHandlerTestEnvironment::SetUp()` — same.
- `tests/test_layer2_service/test_hub_state.cpp:938` `TEST(...)` body
  — the `sec::KeyStore ks;` is DELETED (already done SMS-side); test
  body uses `secure().keys()`.

### Phase 1d — Add SMS to every mod pack that transitively touches crypto — SHIPPED

**Deliverable:** every `LifecycleGuard` in production + tests that
touches KeyStore, sodium, CURVE, or SHM crypto has
`SecureSubsystem::GetLifecycleModule()` in its mod list.

Add to:
- `role_lifecycle_modules()` in `role_main_helpers.hpp` — DONE.
- `PLH_BINARY_LIFECYCLE_MODULES(...)` in the 7-8 L2 test files —
  ADD (was removed erroneously; R3 restated correctly in §2).
- Every Pattern-3 worker's `run_gtest_worker(fn, name, mods...)` mod
  pack that includes `crypto::GetLifecycleModule()` or seeds
  identities — sweep + add.
- Every helper file's `#define PLH_X_MODS ...` macro that lists
  crypto — add SMS entry.

### Phase 1e — Verify

- Build: `cmake --build build -j2 --target stage_all`.
- Test the affected binaries (not the full ctest suite).
- Full ctest only when 1a–1d appear complete.
- Grep verifications (rules in §0 as checks):
  - `grep -rn "SecureMemorySubsystem\s\+\w\+;" src/ tests/` — returns
    only class-declaration + `static ... sole;` + `KeyStore keys_;`.
  - `grep -rn "KeyStore\s\+\w\+\s*(" src/ tests/` — returns only the
    class declaration.  No ctor sites.
  - `grep -rln "register_dynamic_module.*KeyStore\|register_dynamic_module.*SecureMemory"
    src/` — empty.  No dynamic self-registration.

### Phase 2 — CryptoUtils full merge into SMS

**Deliverable:** `pylabhub::crypto` namespace DELETED.  All three sodium
primitives it wraps (BLAKE2b, `sodium_memcmp`, `randombytes_buf`) live
on `SecureSubsystem`.  All ~11 real prod callers use `secure().X(...)`.
~43 mod-pack entries for `pylabhub::crypto::GetLifecycleModule()`
removed.

**Type discipline:** `uint8_t` throughout the SMS wrapper API (not
`std::byte`).  Matches sodium's `unsigned char *` convention + every
existing caller's storage type.  Zero call-site cast.

**Steps:**

2.1  Add to `SecureSubsystem` (public methods, `uint8_t`-typed):
     - `std::array<uint8_t, 32> blake2b(std::span<const uint8_t> data);`
     - `std::uint64_t random_u64();`
     - `bool verify_blake2b(std::span<const uint8_t, 32> stored,
                            std::span<const uint8_t> data);`
     Convert the existing three stubs from `std::byte` → `uint8_t`
     (zero callers today — free rewrite):
     - `random_bytes(std::span<uint8_t>)`
     - `memcmp_ct(std::span<const uint8_t>, std::span<const uint8_t>)`
     - `memzero(std::span<uint8_t>)`
     HEP-0043 §2.1 sketch updated in-commit.

2.2  Migrate 11 prod callers of `pylabhub::crypto::*` to
     `secure().X(...)`.  Sites: `data_block.cpp:1008/1056/1116/1177`,
     `hub_inbox_queue.cpp:167/379/581`, `hub_zmq_queue.cpp:326/400`,
     `broker_service.cpp:2385`, `native_engine.cpp:2101`,
     `data_block_schema.cpp:130/150`, `schema_blds.hpp:229`,
     `schema_utils.hpp:259/295`.

2.3  Delete `crypto::GetLifecycleModule()` mod-pack entries
     everywhere (~43 sites, grep-mechanical).

2.4  Delete files: `src/include/utils/crypto_utils.hpp`,
     `src/utils/service/crypto_utils.cpp`.  Remove from utils
     `CMakeLists.txt`.

2.5  Verify: `grep -rn "pylabhub::crypto" src/ tests/` returns empty.

### Phase 3 — Remaining sodium consumer migrations

Each file gets one commit:
- 3.1  `src/utils/core/uuid_utils.cpp` — `randombytes_buf` →
       `secure().random_bytes`.  Delete `<sodium.h>`.
- 3.2  `src/utils/service/vault_crypto.cpp` — add `pwhash`,
       `secretbox_seal`, `secretbox_open` wrappers on SMS; migrate.
- 3.3  `src/utils/service/hub_vault.cpp` + `role_vault.cpp`.
- 3.4  `src/utils/security/curve_keypair.cpp`.
- 3.5  `src/utils/security/attach_protocol.cpp` — add
       `box_seal_using`, `box_open_using` wrappers (use-not-export
       via `secure().keys()`); migrate.
- 3.6  `src/utils/ipc/broker_service.cpp` — small usage; migrate.
- 3.7  `src/utils/security/key_store.cpp` — audit `<sodium.h>` uses
       (KeyStore is inside SMS now; sodium touches stay in SMS's
       impl area).
- 3.8  `src/include/utils/security/secure_buffer.hpp` — public header
       currently includes `<sodium.h>`; move behind pImpl, delete if
       unused, or forward-declare.

### Phase 4 — Class rename

**Deliverable:** the alias is gone.  `SecureMemorySubsystem` no longer
appears anywhere in `src/` or `tests/` except archived HEPs.

4.1  Mechanical grep-replace: `SecureMemorySubsystem` → `SecureSubsystem`
     throughout `src/` + `tests/`.

4.2  Rename source files: `secure_memory_subsystem.hpp` →
     `secure_subsystem.hpp`; `.cpp` same.  Update includes.

4.3  Delete `using SecureSubsystem = SecureMemorySubsystem;` alias.

4.4  Make the ctor `private` (Logger discipline; all callers migrated).

4.5  Delete `key_store()` inline shim (from Phase 1b.5).  Callers
     migrate grep-mechanically to `secure().keys()`.

4.6  HEP-0043 §2.1 update — drop transition-window alias language.

### Phase 5 — CI enforcement lint

5.1  CI check: `grep -rl '#include *<sodium.h>' src/ | grep -v
     '^src/utils/security/'` must return empty.

5.2  CI check: `grep -rl 'SecureMemorySubsystem' src/ tests/` must
     return empty (post Phase 4).

5.3  CI check: `grep -rE 'SecureSubsystem\s+\w+\s*[;(]|KeyStore\s+\w+\s*\(' src/ tests/`
     must return empty except the class declarations + the
     `sole`/`keys_` singleton sites.  Enforces the pattern rule.

### Phase 6 — HEP content sync

Populate HEP-CORE-0043 §3-§10 stubs with detail from the superseded
HEPs (0036 §9.1, 0038 §8/§10, 0040 §5+§7+§8, 0041 §9.2/§9.3).  Add
Mermaid diagrams, API reference tables, worked examples.  Note that
§2.2 (KeyStore submodule) + §7 (KeyStore detail) are DONE via Phase 1b's
merger — those sections just describe the shipped state.

### Phase 7 — Old HEP archival + close-out

Archive HEP-CORE-0036, 0038, 0040, 0041 per `docs/DOC_STRUCTURE.md`
§2.2.  Update `DOC_ARCHIVE_LOG.md`.  Fresh-eye review of the whole
SEC-Fold-2 landing.  Update `docs/TODO_MASTER.md`.  Final close-out
commit.

---

## 4. Discipline enforcement — checklist before every code change

Before touching a single line:

- [ ] Have I reloaded the `feedback_sms_five_point_pattern.md` memory
      + this doc's §0?
- [ ] Does what I'm about to write use `SecureSubsystem` for all new
      mentions?
- [ ] Am I about to write `SecureMemorySubsystem sms;` or
      `KeyStore ks(...)` on a stack (or as a member of anything but
      `SecureSubsystem::Impl`)?  → STOP, that's the delete target.
- [ ] Am I about to add a "if SMS not ready then bring it up here"
      scaffold?  → STOP, add SMS to the mod pack instead.
- [ ] Am I about to add a test bypass to production code?  → STOP,
      re-read §2 R4.
- [ ] Which Phase am I in (§3)?  Am I doing that phase's work or
      skipping ahead / behind?

If any answer is wrong: stop, correct the plan, re-read the relevant §.

---

## 5. Recovery from drift

If I catch myself proposing an accommodation-of-legacy solution (or
the user catches me):

1. Stop.  Do not defend.
2. Articulate the offending section of §0 or §2 out loud in the
   response — demonstrate re-anchoring.
3. State the pattern-conforming approach.
4. Wait for confirmation before touching code.

---

## 6. Cross-references

- **Memory: `feedback_sms_five_point_pattern.md`** — highest-priority
  rule.  Reload before every code change.
- **HEP-CORE-0043 §1 (contract), §2.1 (SMS class), §2.2 (KeyStore as
  member), §2.3 (lifecycle registration)** — authoritative design.
- **HEP-CORE-0040 §5.2 (KeyStore API), §6 (LockedKey), §8.5.2 (seckey
  raw-32)** — API surface preserved through the merger.
- **HEP-CORE-0001 "Owner-managed teardown"** — OLD escape hatch;
  deleted by this fold.
- **`docs/README/README_testing.md` § "framework contract (absolute)"** —
  test lifecycle discipline.
- **Reference implementations for `GetLifecycleModule()`:**
  - `Logger::GetLifecycleModule()` — `src/utils/logging/logger.cpp:1336`
    (function-local-static singleton, friend startup/shutdown thunks,
    trivial ctor — CANONICAL template for SMS Phase 1).
  - `FileLock::GetLifecycleModule()` — `src/utils/service/file_lock.cpp:750`
    (non-singleton; `lifecycle_initialized()` gate — canonical for the
    `GetLifecycleModule()` shape).
