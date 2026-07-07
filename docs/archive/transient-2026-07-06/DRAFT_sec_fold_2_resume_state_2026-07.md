# SEC-Fold-2 Resume State (2026-07-06)

**Purpose:** everything a fresh session needs to continue SEC-Fold-2
without dropping context.

**Authoritative plan:** `DRAFT_sec_fold_2_plan_and_guidance_2026-07.md`
(this directory) — Consolidated 2026-07-06.

**Highest-priority rule:** `feedback_sms_five_point_pattern.md`
(memory) — the five-point SMS/KeyStore usage pattern.  ANY legacy
code that violates it is a DELETE target, not a template.  Never
accommodate.  Reload before every code change.

---

## 0. Session-start checklist

Read in order:

1. This document.
2. Memory `feedback_sms_five_point_pattern.md` — the five-point rule.
3. `docs/tech_draft/DRAFT_sec_fold_2_plan_and_guidance_2026-07.md` §0
   (the pattern rule) + §2 (absolute rules) + §3 (phase order).
4. `docs/HEP/HEP-CORE-0043-Security-Subsystem.md` §1-§2.
5. `docs/HEP/HEP-CORE-0040-Locked-Key-Memory.md` §5.2 (KeyStore API
   surface preserved through the merger) + §6 (LockedKey).
6. `docs/README/README_testing.md` § "framework contract (absolute)".
7. Other memory files:
   - `feedback_parallel_production_scaffold_antipattern.md`
   - `feedback_read_log_before_rerun.md`
   - `feedback_no_flake_explanations.md`
   - `feedback_tests_replicate_production_scenarios.md`
   - `feedback_log_format_convention.md`
   - `feedback_engineer_discipline.md`
   - `feedback_analyze_actual_code.md` + `feedback_read_code_not_binary.md`

---

## 1. Where SEC-Fold-2 stands (2026-07-06)

**Committed / stable foundation:**

| Commit | Content |
|---|---|
| `9d78b616` | SEC-Fold-1 HEP consolidation — HEP-CORE-0043 authoritative for §0-§2, stubs for §3-§10 |
| `40975ead` | HEP-0043 §1 headline items (Nature / init gate / singularity) |
| `05a73978` | Alias `using SecureSubsystem = SecureMemorySubsystem;` |
| `cbbab8db` | Wrapper methods PANIC on gate violation via `PLH_PANIC` |
| `3c40588d` | Three-state atomic lifecycle flag + CAS singleton claim |
| `c86ba5e2` | Log-line race fix in broker + hub_script_runner |
| `60de75a2` | (earlier version of the plan doc — CONSOLIDATED into current version) |
| `a258c7a5` | (earlier version of resume-state — CONSOLIDATED into this doc) |
| `fbfc8992` | Handoff snapshot of the task-list |

**In-flight uncommitted (working tree, 2026-07-06):**

Files in modified state, matching Phase 1a partial landing:
- `src/include/utils/security/secure_memory_subsystem.hpp` — Logger-shape
  statics added (`instance()`, `GetLifecycleModule()`, `lifecycle_initialized()`),
  friend thunks, private ctor is still public (deferred to Phase 4).
- `src/utils/security/secure_memory_subsystem.cpp` — trivial ctor,
  `Impl::bringup()`, `Impl::shutdown_module()`, startup/shutdown thunks,
  `instance()` static-local singleton, `GetLifecycleModule()`.  Still
  contains `g_sms` file-scope pointer + Step 6 dynamic self-registration
  (Phase 1a will delete both).
- `src/utils/security/key_store.cpp` — `add_dependency("SecureMemory")`
  → `add_dependency("SecureSubsystem")` (name-align; entire
  self-registration block will be DELETED in Phase 1b as KeyStore merges
  into SMS).
- `src/include/utils/security/key_store.hpp` — docstring reference sync.
- `docs/HEP/HEP-CORE-0043-Security-Subsystem.md` — §1.3 mechanism list +
  §2.3 rewritten for the static-lifecycle-module + Logger-shape.
- `src/plh_hub/plh_hub_main.cpp:448` — stack-local `SMS sms;` DELETED
  (KeyStore stack-local still there — Phase 1c deletes it).
- `src/plh_role/plh_role_main.cpp:320` — same treatment.
- `src/include/utils/role_main_helpers.hpp` — `SecureSubsystem::
  GetLifecycleModule()` added to `role_lifecycle_modules()`.
- ~50 test-worker files under `tests/test_layer2_service/workers/` +
  `tests/test_layer3_datahub/workers/` — `sms_module()` helper added
  next to existing `crypto_module()` helper; SMS added to mod packs.
- ~20 test files with `PLH_BINARY_LIFECYCLE_MODULES` — SMS **removed**
  from those macros (WAS erroneous — R3 was misread as blanket-banning
  parent-lifecycle SMS; RESTORED per new §2 R3 wording, needs re-add
  in current session — see §3 "Next action").
- `tests/test_framework/curve_test_setup.h` — `CurveKeyStoreFixture`
  class RETIRED (2026-07-06); replaced by free function
  `seed_curve_identities(setup)` + `add_curve_identity(name, kp)`.
- `tests/test_layer2_service/workers/zmq_queue_auth_workers.cpp` —
  `sms_` field DELETED from `ScopedKeyStore`; `ks_` still there
  (Phase 1c deletes it).
- `tests/test_layer2_service/workers/key_store_workers.cpp` — 17
  stack-local `SecureMemorySubsystem sms;` DELETED.  Mod pack updated
  with `Logger + SecureSubsystem` on 19 of 20 lambdas (one lambda
  deliberately excluded to test SMS-not-up behavior).
- `tests/test_layer2_service/workers/crypto_workers.cpp`,
  `datahub_schema_blds_workers.cpp` — Logger added to mod packs (was
  missing; my earlier work exposed the pre-existing latent bug).

**Not started:**
- Phase 1a cleanup of transitional scaffolds (`g_sms`, dynamic
  self-registration in bringup).
- **Phase 1b — KeyStore merger into SMS** (major work — see plan §3).
- Phase 1c — delete every KeyStore stack-local + Environment ownership
  site.
- Phase 1d — add SMS to PLH_BINARY_LIFECYCLE_MODULES sites (was
  removed erroneously).
- Phase 1e — verify (grep + affected-binary tests only, no full ctest).
- Phase 2 — CryptoUtils merger.
- Phase 3 through Phase 7.

---

## 2. Attempts and reverts

**Attempt 1 (reverted `bf6ae2c6`)** — Phase B fold SMS bringup into
`BinaryLifecycleEnvironment`.  Reason: test-side scaffold pattern
(created hand-crafted lifecycle mirror inside a parent-test helper).
**Attempt 2 (reverted `9e19fc89`)** — Phase C.0 uuid_utils migration +
inline `SecureSubsystemBinaryEnvironment` in three test files.  Same
reason.

Both reverts are unchanged by the current consolidated plan — they
were wrong for a reason that's still in force (test-side scaffolds
mirroring production).

---

## 3. Concrete next action

**Phase 1 SHIPPED 2026-07-06** — SMS lifecycle-module + KeyStore
merger complete.  See plan doc §3 Phase 1a/1b/1c/1d for the diff.

**Next: Phase 2 — CryptoUtils full merge into SMS.**  Steps per
plan §3 Phase 2:

1. Add `blake2b`, `random_u64`, `verify_blake2b` methods to SMS
   (with `uint8_t` typing per the 2026-07-05 type-discipline decision).
2. Convert existing `random_bytes` / `memcmp_ct` / `memzero` stubs
   from `std::byte` → `uint8_t`.
3. Migrate 11 prod callers of `pylabhub::crypto::*` to `secure().X()`.
4. Delete ~43 `crypto::GetLifecycleModule()` mod-pack entries.
5. Delete `crypto_utils.hpp` + `crypto_utils.cpp` + CMake entry.
6. Update HEP-0043 §2.1 sketch.

Deferred pattern-cleanup (opportunistic during Phase 2 sweeps):
- `CurveKeyStoreFixture` class RETIRED 2026-07-06 — replaced by free
  function `seed_curve_identities(setup)` in `curve_test_setup.h`.
  73 call sites migrated across 22 files; static
  `CurveKeyStoreFixture::add_identity` → free function `add_curve_identity`.
  `BrokerTestEnv::ks_fixture` unique_ptr field deleted (nothing to own).
- ~25 test worker files declare `sms_module()` next to `crypto_module()`
  helper — when Phase 2 deletes `crypto_module`, the paired
  `sms_module` becomes cruft.  Fold both into inline
  `SecureSubsystem::GetLifecycleModule()` in the mod pack directly.
- `secure_memory_subsystem_ready()` + `secure_memory_subsystem()`
  free functions — redundant now.  Small header cleanup.

---

## 4. In-flight downstream tasks paused for SEC-Fold-2

- **#317 broker SHM observer** — C.2.c through C.5 pending.  Resume after
  Phase 4 (class rename) completes.
- **#262 SHM mutual auth L4 squatter test** — orthogonal, can complete
  anytime.
- Post-Phase 6 (HEP content sync) unblocks tasks #106 (HEP-0038 impl)
  and #247 (script crypto API).

---

## 5. What NOT to touch

- HEP-0043 §3-§10 stubs — Phase 6 populates them.
- Old HEPs (0036 / 0038 / 0040 / 0041) — Phase 7 archives them; content
  remains authoritative in the interim.
- #317 broker observer code — paused.
- Unrelated in-flight tasks.
- **Do NOT propose `restore stack-local`, `keep dynamic
  self-registration`, `keep g_sms pointer`, or any other legacy-
  accommodation option.**  The pattern rule (§0) forbids it.  Legacy
  is a delete target.

---

## 6. Unpushed local commits

Same set as prior resume-state (`bf6ae2c6`, `9e19fc89`, `60de75a2`,
`a258c7a5`, `fbfc8992` — all local, pending DNS return + explicit
go-ahead).  This session's work adds more to that pile; nothing pushed
without go-ahead.

---

## 7. Success criteria (Phase 7 close-out)

- `grep -rn "SecureMemorySubsystem\|SecureSubsystem\b" src/ tests/` returns
  only the class declaration + `sole` singleton + `keys_` member.
- `grep -rn "KeyStore\s\+\w\+\s*(" src/ tests/` returns only the class
  declaration.  Zero ctor sites.
- `grep -rl "#include *<sodium.h>" src/ | grep -v ^src/utils/security/`
  returns empty.
- Every consumer routes through `secure().X()` / `secure().keys().X()`.
- CI lint enforces the invariants (Phase 5).
- HEP-CORE-0043 §3-§10 content migrated (Phase 6).
- Old HEPs archived (Phase 7).
- Fresh-eye code review confirms.

---

**End of resume state.  Reload the pattern-rule memory + read plan
§0 + §2 before starting.**
