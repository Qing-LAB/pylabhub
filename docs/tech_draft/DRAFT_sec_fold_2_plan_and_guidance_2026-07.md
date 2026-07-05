# SEC-Fold-2 Plan and Guidance

**Status:** IN PROGRESS.
**Task:** Fold the Security Module (SMS) into a proper lifecycle
module.  The word "fold" IS the task.  This is not a helper
migration; it is a lifecycle-model refactor.
**Anchors:** HEP-CORE-0043 §2 (module surface), §2.3 (lifecycle
registration), HEP-CORE-0001 (lifecycle model), memory files
`feedback_parallel_production_scaffold_antipattern.md`,
`docs/README/README_testing.md` § "framework contract (absolute)".

---

## 0. Why this document exists

I have failed the plan repeatedly.  I keep drifting back to the
obsolete stack-local pattern because that pattern is still in the
production reference files (`plh_hub_main.cpp:448`,
`plh_role_main.cpp:320`, `key_store_workers.cpp` × 6, etc.), and I
copy from the reference reflexively.  I also created a substitution
alias (`SecureSubsystem = SecureMemorySubsystem`) in commit
`05a73978` explicitly to make the eventual rename mechanical, then
immediately ignored it in every subsequent piece of work and kept
typing the OLD name.

This document exists so I stop doing that.  I re-read §1 and §2
before every code change.  If I catch myself typing the OLD name
or proposing a scaffold, I stop and consult this document.

---

## 1. Naming discipline — mandatory before every new line

**In every piece of NEW code and NEW writing:**

- Write **`SecureSubsystem`**.
- Do NOT write `SecureMemorySubsystem`.

The alias `using SecureSubsystem = SecureMemorySubsystem;` was
introduced by commit `05a73978` specifically to enable substitution.
It sits in
`src/include/utils/security/secure_memory_subsystem.hpp`.  The
whole point is that every new site can use the target name; the
final class-rename phase then removes the alias and renames the
declaration — mechanical, no code semantics change.

**Legal remaining uses of `SecureMemorySubsystem` (during the fold):**
- The class declaration line itself (until Phase E rename).
- The `using SecureSubsystem = ...` alias line (until Phase E deletes it).
- Files NOT YET touched by the fold (they will be swept later).
- HEP-CORE-0040 archived docs.

**Illegal uses (of the OLD name) — this is what I keep doing wrong:**
- New code I write today mentioning `SecureMemorySubsystem`.
- New comments in code I write mentioning `SecureMemorySubsystem`.
- PANIC / log messages I write mentioning `SecureMemorySubsystem`.
- New docs I write mentioning `SecureMemorySubsystem`.

Every new occurrence I introduce with the OLD name adds work to
Phase E.  I created the alias to prevent that.  Use it.

---

## 2. Absolute rules — do not violate

**R1. No stack-local `SecureSubsystem sms;` in new production or test code.**
The obsolete pattern.  Every existing site (`plh_hub_main:448`,
`plh_role_main:320`, worker files, fixtures) is a Phase-B/C DELETE
target, not a template to copy.

**R2. SMS enters a process via `SecureSubsystem::GetLifecycleModule()`
in a `LifecycleGuard` / `run_gtest_worker` mods pack.**  Same
mechanism Logger / CryptoUtils / ZMQContext / FileLock use.  If SMS
does not have `GetLifecycleModule()` at some moment during the
fold, that's Phase A's exact deliverable — add it.

**R3. Test parent processes own no lifecycle.**  Never propose
adding SMS to `BinaryLifecycleEnvironment`, `PLH_BINARY_LIFECYCLE_
MODULES`, custom `::testing::Environment` classes, or any other
"parent brings up SMS" mechanism.  Parent's job is to dispatch
subprocesses and read their log / output (`docs/README/README_
testing.md` § "framework contract (absolute)").

**R4. No production-side test bypasses.**  No `reset_state_for_
test`, no `sms_reset_after_fork`, no exported state-mutation
helpers.  If a test problem tempts one, the fix is that the test
needs to route through the framework's `run_*` mechanism, not that
production needs a hole.

**R5. No test-framework mods.**  Do not add SMS-awareness to
`run_gtest_worker`, `run_worker_bare`, `BinaryLifecycleEnvironment`,
or any other framework helper.  These take a modules list.  Callers
decide whether to include `SecureSubsystem::GetLifecycleModule()`
in that list.  That is the entire mechanism.

**R6. Read the memory file `feedback_parallel_production_scaffold_
antipattern.md` before proposing ANY test-side accommodation.**
Every one of my recent violations traces to this file's diagnostic
smell: writing test-side code that "matches production" instead of
using the production entry points.

---

## 3. Phase order (strict)

Phases run in order.  Each phase leaves the tree buildable and
2319/2319 green (or with a documented, tracked delta).

### Phase A — SMS becomes a proper lifecycle module

**Deliverable:** `SecureSubsystem::GetLifecycleModule()` exists and
works.  SMS is brought up via LifecycleManager exactly the way
Logger is, not via ctor-side self-registration.

**Steps:**

A.1  Add `static ModuleDef SecureSubsystem::GetLifecycleModule()`
     declaration to
     `src/include/utils/security/secure_memory_subsystem.hpp`.

A.2  Implement in `secure_memory_subsystem.cpp`:
     - Returns a `ModuleDef` named `"SecureSubsystem"` (note: matches
       HEP-0043 §2.3 module name, not the obsolete `"SecureMemory"`).
     - Dependency: `"pylabhub::utils::Logger"`.
     - Startup thunk: allocates a heap-owned singleton via
       `new SecureSubsystem` (or a static-local pattern) and drives
       the ctor's platform-hardening + `sodium_init` work.
     - Shutdown thunk: destructs the singleton.
     - Timeout: `100ms` (same as current).
     - **Do NOT** call `set_owner_managed_teardown(true)` — that
       flag was the escape hatch for stack-local ownership; obsolete
       now that SMS is a proper module.

A.3  Delete from the ctor:
     - `register_dynamic_module` self-registration.
     - `LoadModule(kModuleName)` call.
     - `set_owner_managed_teardown(true)` line.
     Ctor now just holds the platform-hardening work; it is INVOKED
     by the startup thunk, not called directly by users.

A.4  Delete from the dtor:
     - `unload_module` call (matching removal).

A.5  Keep the three-state atomic + `g_sms` pointer + PANIC accessor —
     that machinery is correct.  The startup thunk publishes
     `Initialized`; the shutdown thunk publishes `Shutdown`.

A.6  Test in-process: build; nothing else changes yet (no callers
     use `GetLifecycleModule` at this point).  Existing stack-local
     construction still works during Phase A because the class
     ctor is still callable — this is intentional so Phase B/C
     can migrate incrementally.

**Discipline in Phase A code:** all new comments, log messages,
PANIC strings, and doc paragraphs use `SecureSubsystem`.  The class
declaration line stays `class ... SecureMemorySubsystem` until
Phase E.

### Phase B — Migrate production `main()` files

**Deliverable:** `plh_hub` and `plh_role` binaries bring up SMS via
`GetLifecycleModule()` in their `LifecycleGuard` mod pack.

B.1  `src/plh_hub/plh_hub_main.cpp:448` — delete the
     `sec::SecureMemorySubsystem sms;` line.  Add
     `SecureSubsystem::GetLifecycleModule()` to the mod pack passed
     to whatever `LifecycleGuard` wraps `HubHost::run()`.

B.2  `src/plh_role/plh_role_main.cpp:320` — same treatment.

B.3  Run L4 tests (`plh_hub` + `plh_role` subprocess-based) to
     confirm the binaries still start, do work, and shut down cleanly.

### Phase C — Migrate test worker files

**Deliverable:** every worker whose test body reaches SMS gets it
via its `run_gtest_worker(..., mods...)` mod pack, not via a
stack-local `SMS sms;` inside the lambda.

C.1  `tests/test_layer2_service/workers/key_store_workers.cpp` — 6
     lambdas.  Delete each `SecureMemorySubsystem sms;` line at the
     top of each lambda.  Add
     `SecureSubsystem::GetLifecycleModule()` to the mods pack of
     the corresponding `run_gtest_worker(...)` call.

C.2  All other worker files that construct `SMS sms;` — sweep with
     grep, apply the same treatment.

C.3  `tests/test_layer2_service/test_hub_zmq_queue.cpp:137` — inline
     custom Environment class has `static sec::SecureMemorySubsystem sms;`
     inside an if-block.  DELETE.  The tests in that binary either
     (a) belong at Pattern 3 subprocess (they touch lifecycle) and
     therefore their workers add SMS to their mods pack, or
     (b) are pure in-process assertion-only tests that don't need SMS.
     Check per-test.

C.4  `tests/test_layer2_service/test_role_handler.cpp:129` — same.

C.5  `tests/test_framework/curve_test_setup.h` `CurveKeyStoreFixture` —
     `sms_` member and `sms_()` initializer.  DELETE.  The fixture
     assumes SMS is already up; it just constructs `KeyStore` against
     the running SMS.  Callers who instantiate `CurveKeyStoreFixture`
     must be inside a scope where `SecureSubsystem` is up via
     LifecycleGuard — which they must be, because they're Pattern 3
     subprocess workers already.

C.6  Run full ctest.  2319/2319 must pass.

### Phase D — Consumer migrations (raw sodium → `secure().X()`)

**Sequenced by primitives needed**, each file gets one commit.

D.1  `src/utils/service/crypto_utils.cpp`.  Add `blake2b` wrapper to
     `SecureSubsystem` first (missing); migrate `compute_blake2b`,
     `verify_blake2b`, `generate_random_bytes` to
     `secure().blake2b`, `secure().memcmp_ct`, `secure().random_bytes`.
     Delete `<sodium.h>` include.  Every test binary whose workers call
     these transitively already got `SecureSubsystem::GetLifecycleModule()`
     in their mods pack from Phase C, so no test change needed.

D.2  `src/utils/core/uuid_utils.cpp`.  Migrate `randombytes_buf` →
     `secure().random_bytes`.  Delete `<sodium.h>`.

D.3  `src/utils/service/vault_crypto.cpp` — add `pwhash`,
     `secretbox_seal`, `secretbox_open` wrappers first; migrate.

D.4  `src/utils/service/hub_vault.cpp` + `src/utils/service/role_vault.cpp`.

D.5  `src/utils/security/curve_keypair.cpp`.

D.6  `src/utils/security/attach_protocol.cpp` — add `box_seal_using`,
     `box_open_using` wrappers first (use-not-export per HEP-0043
     §1.4); migrate.

D.7  `src/utils/ipc/broker_service.cpp` — small usage; migrate.

D.8  `src/utils/security/key_store.cpp` — inside the module; audit
     that `<sodium.h>` is the only sodium touch and hide any leaked
     types behind `pImpl`.

D.9  `src/include/utils/security/secure_buffer.hpp` — public header
     currently includes `<sodium.h>`.  Options: (a) move to `.cpp`
     behind pImpl, (b) delete if unused, (c) forward-declare.
     Decide per caller audit.

### Phase E — Class rename

**Deliverable:** the alias is gone.  `SecureMemorySubsystem` no
longer appears anywhere in `src/` or `tests/` except archived HEPs
that reference the historical name.

E.1  Mechanical grep-replace: `SecureMemorySubsystem` →
     `SecureSubsystem` throughout `src/` and `tests/`.  Every file.

E.2  Rename source files:
     - `secure_memory_subsystem.hpp` → `secure_subsystem.hpp`.
     - `secure_memory_subsystem.cpp` → `secure_subsystem.cpp`.
     Update includes throughout.

E.3  Delete `using SecureSubsystem = SecureMemorySubsystem;` alias
     (its declaration site — the header file — becomes the actual
     class after the rename).

E.4  Update HEP-CORE-0043 §2.1 to drop the transition-window language
     about the alias.

### Phase F — CI enforcement lint

F.1  CI check: `grep -rl '#include *<sodium.h>' src/ | grep -v
     '^src/utils/security/'` must return empty.  Fail if not.

F.2  CI check: `grep -rl 'SecureMemorySubsystem' src/ tests/` must
     return empty (post Phase E).

### Phase G — HEP-CORE-0043 §3-§10 content migration

Populate §3-§10 stubs with detail from the superseded HEPs
(0036, 0038, 0040, 0041).  Add Mermaid diagrams, API reference
tables, worked examples.

### Phase H — Old HEP archival

Archive HEP-CORE-0036, 0038, 0040, 0041 per DOC_STRUCTURE.md §2.2.

### Phase I — SEC-Fold-2 close-out

Fresh-eye review, grep verifications, TODO updates, final commit.

---

## 4. Discipline enforcement — checklist before every code change

Before I touch a single line:

- [ ] Have I re-read §1 (naming) and §2 (rules) of this document?
- [ ] Does what I'm about to write use `SecureSubsystem` for all new
      mentions?
- [ ] Am I about to propose or add SMS to a test-side scaffold
      (Environment, worker helper)?  If yes → STOP, re-read §2 R3/R5.
- [ ] Am I about to write `SecureMemorySubsystem sms;` as a stack
      local anywhere?  If yes → STOP, that's the pattern being
      deleted by Phase B/C.
- [ ] Am I about to add a test bypass to production code?  If yes →
      STOP, re-read §2 R4.
- [ ] Have I checked the current Phase against §3?  Am I doing that
      phase's work, or am I skipping ahead / behind?

If any answer is wrong, I stop, correct my plan, and re-read the
relevant §.

---

## 5. Recovery from drift

If I catch myself proposing an obsolete-pattern solution (or the
user catches me), the recovery protocol is:

1. Stop.  Do not defend the proposal.
2. Read the offending section of this document out loud (or
   articulate it in my response) to demonstrate re-anchoring.
3. State the correct approach per this document.
4. Wait for confirmation before touching code.

---

## 6. Cross-references

- **HEP-CORE-0043 §2.3** — module surface, one lifecycle module
  named `"SecureSubsystem"`.
- **HEP-CORE-0001 §"Owner-managed teardown"** — the OLD design's
  opt-in escape hatch; deleted by this fold.
- **`docs/README/README_testing.md` § "framework contract (absolute)"** —
  parent tests own no lifecycle; subprocess workers own their own
  `LifecycleGuard`.
- **Memory files:**
  - `feedback_parallel_production_scaffold_antipattern.md`
  - `feedback_no_mocks_via_observability.md`
  - `feedback_test_bypass_explicit.md`
  - `feedback_tests_replicate_production_scenarios.md`
- **Reference implementations for `GetLifecycleModule()`:**
  - `Logger::GetLifecycleModule()` — `src/utils/logging/logger.cpp`
  - `CryptoUtils::GetLifecycleModule()` — `src/utils/service/crypto_utils.cpp`
  - `FileLock::GetLifecycleModule()` — `src/utils/service/file_lock.cpp`
