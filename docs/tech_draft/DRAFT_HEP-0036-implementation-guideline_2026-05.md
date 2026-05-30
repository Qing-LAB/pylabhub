# HEP-0036 Implementation Guideline (transient)

**Status:** Active.  Drives implementation work for tasks #101, #102,
#74, #94, #103, #104 (HEP-CORE-0036 auth chain).  Lives under
`docs/tech_draft/` per `DOC_STRUCTURE.md` §1.8.  When all referenced
tasks ship, merge lasting insight into the relevant HEPs +
`IMPLEMENTATION_GUIDANCE.md`, then archive per §2.1.

**Audience:** any future implementation session picking up this work.
Self-contained — no recall required.

**Authority direction:** this doc CITES permanent HEPs/READMEs.
Permanent docs MUST NOT cite this doc (per project rule
"HEPs/permanent docs DON'T cite transient docs"; transient flows into
permanent on archival, not the reverse).

---

## 1. Purpose

The HEP-CORE-0036 auth chain is a multi-task refactor, not a single
feature drop.  Each task touches production code, production tests,
and operator-facing surface (CLI, config, error messages).  Without
shared discipline, future sessions invent paths, duplicate checks,
imagine failure modes, or leave orphaned tests.  This guideline
records the shared rules + anchor docs so every session lands the
same way.

---

## 2. Always-refresh sources (open at session start)

Read these **at the moment you start work** on any task in the chain.
Do not rely on memory.

| When | Read |
|---|---|
| Always | `CLAUDE.md` (repo root) — behavioral rules + authority map |
| Always | `MEMORY.md` (`~/.claude/projects/-home-qqing-Work-pylabhub/memory/MEMORY.md`) — workflow rules index |
| Touching `HubState` / broker / channel state | `docs/HEP/HEP-CORE-0033-Hub-Character.md` §8 (HubState entry types), §15 (renovation phases) |
| Touching role/presence state | `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` §2.1 broker FSM, §2.5 hub-dead |
| Touching CURVE / ZAP / `known_roles` | `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` §4.1 layered enforcement, §4.3 federation_trust_mode, §4.6 file ACLs, §4.7 runtime hardening |
| Touching auth wiring on data plane | `docs/HEP/HEP-CORE-0036-Authenticated-Connection-Establishment.md` §3 invariants, §4 architecture, §6 wire format, §12 phases |
| Touching queue / channel / pipeline | `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` §3.3 QueueReader/Writer, §3.3 dynamic peer membership, §4.6 fan-in |
| Touching REG/CONSUMER_REG/DISC wire | `docs/HEP/HEP-CORE-0021-ZMQ-Endpoint-Registry.md` §5, §16 |
| Writing any test | `docs/README/README_testing.md` § "Choosing a test pattern" + § "Naming conventions" |
| Writing assertions | `docs/IMPLEMENTATION_GUIDANCE.md` § "Assertion Design — silent-failure prevention" |
| Placing any new `.md` | `docs/DOC_STRUCTURE.md` §1.7-1.10 |

**Don't reason from comments, docstrings, prior chat, or memory
indexes.**  Open the doc and read what it actually says today.

---

## 3. Design principle + goal

**Goal.**  Ship the production-ready single-hub authenticated data
plane.  Every CURVE handshake (BRC + data PUSH/PULL + SHM secret) is
gated by a verified pubkey ∈ `known_roles[]` (control plane, Layer 1)
plus per-channel allowlist (data plane, Layer 3).  Broker is the sole
authority for admission; producer/consumer ZAP caches reflect the
broker's decisions.  Federation deferred to task #105.

**Principles (locked in HEP-CORE-0036 §3 I1-I9):**

- **I1** Both conditions hold before any data artifact: CURVE proof
  + role ∈ `known_roles[]`.
- **I2** Broker is the single source of truth; producer-side / consumer-
  side code executes artifacts the broker handed them.
- **I3** Control gates data.  When BRC dies, role's data loop exits
  because `any_presence_authorized()` returns false (presence FSM is
  the bridge — no separate `brc_is_connected()` check in the guard).
- **I4** No data artifact (endpoint, producer pubkey, SHM secret) is
  released before authorization succeeds.
- **I5** Revocation prevents NEW connections; existing CURVE sessions
  are trusted for their lifetime.
- **I6** Identity keys are reused on the data plane; broker mints no
  data-plane CURVE keys.
- **I7** Endpoint disclosure follows authorization.
- **I8** Compromised-broker defence is out of scope.
- **I9** Three-tier separation — broker emits channel-event
  broadcasts; framework calls `queue.add_producer_peer` /
  `remove_producer_peer`; queue conceals transport; script sees only
  `api.rx.acquire()`.

**Non-goals (locked in HEP-CORE-0036 §2.1):** channel pre-declaration
in hub config; force-closing CURVE sessions; per-consumer ACL inside
data path; automated pubkey distribution; mid-session identity-key
rotation; defence against compromised broker.

---

## 4. Phase plan (task graph)

```
#101 (key-file ACL utility) ─┐
                             ├─→ #74 (HEP-0035 auth, expanded ─→ #103 (ZmqQueue dyn peer) ─→ #104 (sibling code) ─→ #106 (HEP-0038 script-vault) ─→ DONE
#102 (runtime key hardening)─┘   per §5.3 below)              ↗
                                                              #94 (HEP-0021 §16.5 ephemeral binding)
```

#74 was expanded 2026-05-29 to subsume known-roles-in-vault storage
(HEP-CORE-0035 §4.8) and the `--add-known-role` / `--revoke-known-role`
/ `--list-known-roles` CLI commands.  See §5.3 for details.

#106 (NEW, filed 2026-05-29) — HEP-CORE-0038 + impl: per-role
script-accessible vault keystore (`api.vault_save` / `api.vault_load`).
Ships after #104; independent of federation.

#105 (federation, HEP-CORE-0037) — parallel + non-blocking for
single-hub shipment.

Each task's per-phase sub-plan lives in §5 below.

---

## 5. Per-task scope contracts

### 5.1 #101 — Key-file ACL discipline (HEP-CORE-0035 §4.6)

**HEP reference:** HEP-0035 §4.6 (utility + §4.6.1 modes + §4.6.2
startup verification + §4.6.3 B3/B4 interaction).  Amended
2026-05-29: §4.6.1 path table now reflects vault reality (no
plaintext `<uid>.sec` / `known_roles/` files); §4.6.4 cleaned up.

**Vault reality (verified by reading the code 2026-05-29):**

The vault is not a `.sec` stand-in.  It is the canonical
encrypted-at-rest secrets container — broader scope than a single
seckey file.  Today:

| Concept | Path |
|---|---|
| Hub seckey container | `<hub_dir>/vault/hub.vault` — encrypted JSON holding broker CURVE keypair, admin token, and (per HEP-0035 §4.8) the `known_roles` allowlist |
| Hub pubkey (operator-distributable) | `<hub_dir>/hub.pubkey` — plaintext |
| Vault parent dir | `<hub_dir>/vault/` — owner-only, designed for extension (more encrypted secrets in future) |
| Role seckey container | `<role_dir>/vault/<role_uid>.vault` — encrypted JSON holding role CURVE keypair and (per HEP-CORE-0038) script-managed `scripts` map |
| Role vault parent dir | `<role_dir>/vault/` |

Verified citations:
- `src/include/utils/hub_vault.hpp:1-26` (HubVault facility scope).
- `src/utils/service/hub_vault.cpp:114, 136, 180` (file paths emitted).
- `src/include/utils/role_vault.hpp:1-36` (RoleVault facility scope).
- `src/include/utils/role_directory.hpp:154-161` (`vault/` dir
  architecture; vault file is `<role_uid>.vault`).
- `test_hub_vault.cpp:152-153, 407-408` (existing inline 0600/0644
  mode checks).
- `test_hub_directory.cpp:89-92` (existing inline 0700 dir check).

**Why this matters for the utility design:** the utility operates
on a path + an expected role (e.g. `VaultFile`, `PublicKeyFile`,
`VaultDir`, `ConfigDir`).  The CALLER (production code) supplies
the canonical path it actually wrote; the utility checks the right
mode for that role.  No new file scheme introduced.  Pre-existing
encryption-at-rest stays.

**Sub-phases (track as TaskCreate sub-tasks):**

| ID | Scope | Files |
|---|---|---|
| 1A | Build shared utility | NEW `src/include/utils/security/key_file_acl.hpp`, `src/utils/security/key_file_acl.cpp`, build wiring (extend `src/utils/CMakeLists.txt` parent — model on `src/utils/service/`, `src/utils/config/` siblings) |
| 1B | New L2 utility test owning the verdict matrix | NEW `tests/test_layer2_service/test_key_file_acl.cpp` (Pattern 1+, single Logger module per § 6 below), extend `tests/test_layer2_service/CMakeLists.txt` |
| 1C | Refactor existing inline mode checks to delegate to utility | MODIFY `tests/test_layer2_service/test_hub_vault.cpp` (`:152-153, 240-241, 271, 407-408`), `test_role_vault.cpp` (`:94-103, 156`), `test_hub_directory.cpp` (`:89-92`) |
| 1D | Wire production code | MODIFY hub_vault.cpp keygen path to call `set_keyfile_mode`, plh_hub + plh_role `main()` to call `verify_keyfile_acl_or_die()` before any secret read |
| 1E | Extend L4 binary fixtures | MODIFY `test_plh_hub_keygen.cpp`, `test_plh_hub_init.cpp` to assert utility-validated modes via `PlhHubCliTest`; symmetric for `test_plh_role_*` |

**Out of #101 scope:**
- Known-roles-in-vault storage + `--add-known-role` CLI commands —
  folded into #74 (see §5.3).  #101 just makes sure the vault file
  itself has correct modes; #74 changes what the vault *contains*.
- B4 non-zero SHM secret (task #79 — separate).
- Script-accessible vault (HEP-CORE-0038, task #106 — separate, lands
  after #104).

**Folded INTO #101 sub-phase 1D (decision 2026-05-30 — scope
expansion):** Task #78 (B3: hard-error empty `auth.keyfile`) +
hub-side `auth.keyfile` honoring.  Rationale below.

### 5.1.1 Sub-phase 1D — expanded scope (2026-05-30)

**Problem surfaced during 1D investigation:** `auth.keyfile` is
the source of truth for the vault path per HEP-CORE-0033 §7 and
HEP-CORE-0024 §3.4, but the implementations diverged:

- **Hub** (`src/utils/config/hub_config.cpp:170-186`): hardcodes
  `<hub_dir>/vault/hub.vault`; `auth.keyfile` content is IGNORED
  (acts as a non-empty/empty "vault auth on/off" toggle only).
  Comment admits "the path is hard-coded" without justifying it.
- **Role** (`src/utils/config/role_config.cpp:263-271`): uses
  `auth.keyfile` as-is, no fallback to a canonical default.
  When the file doesn't exist, falls through silently to ephemeral
  CURVE identity with only a stderr printf.

Neither implementation matches the HEPs' design intent (both said
`auth.keyfile` is the source of truth; both should resolve
relative paths against `base_dir` and fall back to a canonical
default).  Wiring ACL checks on top of this inconsistency would
codify the hole: the hub would ACL-check `<hub_dir>/vault/hub.vault`
but an operator who edits the config to point elsewhere would
silently bypass it.

**Unified design (locked 2026-05-30):**

| `auth.keyfile` value | Meaning | Runtime behavior |
|---|---|---|
| Non-empty (relative) | Path relative to `<base_dir>` (hub_dir / role_dir) | Open vault at resolved path; ACL-check file + parent dir; fail loud on mode/ownership violation |
| Non-empty (absolute) | Path as-is.  Note: JSON values are read literally — `~` is NOT shell-expanded.  Use `/home/<user>/...` for absolute home-directory paths. | Same as above; ACL-check at the exact path |
| Non-empty + file absent at resolved path | Operator-configured vault, but the file does not exist | Hard error.  No silent fallback to ephemeral mode — consistent with the explicit-opt-in semantic for the empty case. |
| Empty `""` | EXPLICIT operator opt-in to ephemeral CURVE keys (= opt-out of encryption-at-rest); dev / loopback only | No vault open; no vault-mode ACL check (Tier 1 config check still runs); warn to stderr that ephemeral mode is in use |
| Field missing entirely | Config-load error | Hard-fail at parse time (task #78) |

**No quiet fallback.**  Empty means explicit opt-out (ephemeral
mode).  An operator reading someone else's deployment sees the
vault location in the config; a missing entry is a config error.

`--init` writes the canonical default into the template:
- Hub: `"auth": { "keyfile": "vault/hub.vault" }`
- Role: `"auth": { "keyfile": "vault/<role_uid>.vault" }`

`--keygen` requires non-empty `auth.keyfile` (hard-error otherwise
— ephemeral mode is in-process only).

**Files to change in sub-phase 1D:**

| File | Change |
|---|---|
| `src/utils/config/hub_config.cpp:170-209` | Replace hardcoded path with `resolve_keyfile_path(auth.keyfile, hub_dir)`.  Hard-error on missing field; empty = ephemeral (HubVault not loaded). |
| `src/utils/config/role_config.cpp:263-275` | Already uses `auth.keyfile`.  Remove the silent ephemeral fallback when file doesn't exist (turn into hard error if `keyfile` non-empty + missing file); empty `keyfile` = ephemeral with warning, no file check. |
| `src/utils/config/hub_directory.cpp:124` | Already writes `"vault/hub.vault"`; verify role_directory.cpp `--init` writes `"vault/<role_uid>.vault"` symmetrically. |
| `src/include/utils/security/key_file_acl.hpp` or new helper | `fs::path resolve_keyfile_path(string keyfile, fs::path base_dir)` — relative → joined with base_dir, absolute → as-is.  Single source of truth for both binaries. |
| `src/plh_hub/plh_hub_main.cpp` | ACL check on the resolved keyfile path + its parent dir.  Skip when keyfile empty (ephemeral mode). |
| `src/plh_role/plh_role_main.cpp` | Same, symmetric. |

**HEP doc changes that come WITH this code (land first, see §6
"Workflow" below):**

1. **HEP-CORE-0033 §7 (Hub directory layout)** — clarify
   `auth.keyfile` IS source of truth; canonical path is what
   `--init` writes; runtime resolves relative paths against
   `hub_dir`; empty = ephemeral.
2. **HEP-CORE-0024 §3.4 (Role vault file convention)** — drop the
   "`RoleDirectory::default_keyfile()` when empty" runtime
   fallback language.  Helper stays for `--init` use (computing
   the canonical default to write into the template).  Empty
   `auth.keyfile` at runtime = ephemeral, not default fallback.
3. **HEP-CORE-0035 §4.6.1 (Required modes table)** — add note that
   the `vault/` directory may be located outside `base_dir` per
   operator's `auth.keyfile` setting (user-controlled placement
   intent — see §5.1.1 design table above).
4. **HEP-CORE-0035 §4.6.2 (Startup verification)** — clarify the
   ACL check runs on the resolved `auth.keyfile` path; runs in
   both binaries; ephemeral mode skips the check.

**Task tracking:** This sub-phase BUNDLES #101's 1D wiring with
task #78 (hard-error empty keyfile) + hub `auth.keyfile`-honoring
fix.  After sub-phase 1D ships, task #78 closes; #101 advances to
sub-phase 1E (L4 binary fixture extensions).

§4.6.3 explicitly says #101 layers on top of #78/#79.

### 5.2 #102 — Runtime key handling (HEP-CORE-0035 §4.7)

Independent of #101.  Will be detailed when starting; same structure:
NEW utility, NEW L2 test for SecureKeyBuffer/disable_core_dumps, MODIFY
hub_vault.cpp / role_vault.cpp + main() entries to call it before
ACL check.

### 5.3 #74 — HEP-0035 auth implementation (expanded 2026-05-29)

HEP-0035 §8 has the 7-phase plan.  Includes:

- **Known-roles-in-vault storage** (HEP-CORE-0035 §4.8): allowlist
  lives inside `hub.vault` payload as a `known_roles` JSON key, NOT
  as plaintext `<hub_dir>/known_roles/*.pub` files.
- **New `plh_hub` CLI commands** (HEP-CORE-0035 §4.8.3):
  - `--add-known-role <role.pub>` (with optional `--role-uid` /
    `--force`)
  - `--revoke-known-role <role_uid>`
  - `--list-known-roles` (prints `(uid, pubkey_fingerprint)`; uses
    BLAKE2b-128 fingerprint, not raw pubkey)
  - All three: prompt for master password (or `PYLABHUB_HUB_PASSWORD`
    env var); first run §4.6.2 file-ACL discipline check; mutate
    vault; re-encrypt; write back.  If hub is running, signal admin
    RPC reload (HEP-CORE-0033 §11.2).
- **Layer-1 ZAP installation** on broker ROUTER (HEP-CORE-0035 §4.1):
  reads allowlist from decrypted vault payload at startup; rejects
  unknown pubkeys.
- **Layer-2 federation-trust gate** (HEP-CORE-0035 §4.3
  `federation_trust_mode`): three modes `local_only` /
  `peer_delegated` / `peer_announced`.
- **Removal** of `RoleIdentityPolicy` enum +
  `ChannelPolicyOverride` + `check_role_identity()` per
  HEP-CORE-0035 §4.5.
- **Test cleanup** triggered by removal: delete
  `tests/test_layer3_datahub/test_datahub_role_identity_policy.cpp`
  + `workers/role_identity_policy_workers.cpp` (per the obsolete-test
  flag from §7.5).  Deletion happens with the production-code removal
  commit, NOT before, NOT after.

Sub-phases TBD when task is picked up; will follow the same A/B/C/D/E
discipline (utility/test-new/test-refactor/wire-production/L4-extend)
as #101.

### 5.4 #94 + #103 — coordinated wire-shape migration

HEP-0036 §14.1 requires `CONSUMER_REG_ACK.producers[]` AND
`DISC_REQ_ACK.producers[]` to land together as one wire-format change.
Don't ship one without the other.  L3 tests for both must exist before
either binary calls the new shape.

### 5.5 #104 — Sibling-HEP code updates per §14

After #74 + #103 + #94.  Multi-area; spawn sub-tasks per sibling
(HEP-0021 §5.1/§5.2, HEP-0023 FSM Authorized state, HEP-0027 inbox
CURVE wiring, HEP-0030 band CURVE wiring) on entry.

### 5.6 #105 — Federation protocol (deferred)

Separate effort.  Required HEP doesn't exist yet (HEP-CORE-0037
working title).  See HEP-0036 §13.1 deferral note.

### 5.7 #106 — HEP-CORE-0038 + script-accessible vault keystore

Filed 2026-05-29.  HEP-CORE-0038 stub at
`docs/HEP/HEP-CORE-0038-Script-Accessible-Vault-Keystore.md` records
design intent + open questions; detailed design happens when this
task is picked up.

Scope summary:
- Extend `RoleVault` payload with a `scripts` map (encrypted at rest
  alongside the CURVE keypair).
- New script API: `api.vault_save(key, value)` /
  `api.vault_load(key)`.  Wire through Python + Lua + Native engines.
- Per-role isolation: a role can only read/write its own store.
- Tests per HEP-CORE-0038 §4: L2 RoleVault payload round-trip, L3
  cross-engine API parity, L4 demo (producer persisting a token
  across restarts).

Out of MVP scope per HEP-CORE-0038 §2.3: cross-role access, audit
logging, quotas.

Depends on: #104 (HEP-0036 sibling-HEP code updates) shipping first,
so the auth chain is solid before adding new vault surface.  Does
NOT depend on #105 (federation).

---

## 6. Test design discipline (binding for all tasks)

### 6.1 Pattern decision

Per `README_testing.md` § "Choosing a test pattern" decision
checklist.  Don't invent.  For each new test ask in order:

1. Does the body call any `LOGGER_*`?
2. Does it construct a class listing `GetLifecycleModule()` in its
   public API, or take FileLock/JsonConfig in ctor?
3. Does it call `RoleConfig::load_from_directory`, `JsonConfig::load`,
   `FileLock(...)`, or anything in `src/scripting/`?
4. Does it construct Messenger / BrokerService / hub::Producer /
   hub::Consumer / hub::Processor / ShmQueue / ZmqQueue /
   ThreadManager / RoleHostBase / script engine?

ANY yes → Pattern 3 (or Pattern 1+ after passing the 4-vector exam in
README_testing.md § "Decision checklist — when to use binary-wide vs.
subprocess").  ALL no → Pattern 1.

### 6.2 Pattern 1+ exam (the four vectors)

When the subject emits LOGGER_* but the test logic is pure:

- Static state in subject? → If yes, Pattern 3.
- Init-once invariant violations? → If yes, Pattern 3.
- libzmq / luajit / libsodium global state alive past `RUN_ALL_TESTS`?
  → If yes, Pattern 3.
- Crash isolation needs (panic / abort / `finalize()` deliberately)?
  → If yes, Pattern 3.

All clean → Pattern 1+.  Reference: `test_zmq_poll_loop.cpp`
(single-module Logger guard) or `test_admin_service.cpp` (five-module
guard).

### 6.3 Layer placement

- L1 (`tests/test_layer1_<area>/`): pure functions, no LOGGER, no
  lifecycle module.
- L2 (`tests/test_layer2_service/`): service-layer utilities; Pattern
  1+ for lifecycle-touching pure-logic, Pattern 3 for stateful or
  multi-process.
- L3 (`tests/test_layer3_datahub/`): DataHub protocol + integration.
- L4 (`tests/test_layer4_plh_hub/` / `test_layer4_plh_role/`): actual
  binary CLI surface; uses existing `plh_hub_fixture.h` /
  `plh_role_fixture.h`.

**Do not invent new layer dirs.**

### 6.4 Naming (MANDATORY — `README_testing.md` § "Naming conventions")

- File: `test_<subject>.cpp` (subject = class / subsystem).
- Fixture class: `<Subject>Test` (CamelCase).
- TEST_F name: `Subject_Qualifier_Behavior` (CamelCase parts +
  underscores for grep boundaries).
- NO transient labels (chunk / sprint / phase / ticket ID) in
  fixture or test names.

### 6.5 Refactor-not-add discipline

**Survey before writing.**  Every new test must be screened against
existing tests in the same area.  For #101 the survey is in §7 below.
For future tasks, perform the survey as the first sub-phase.

The screening produces three outcomes per existing test:

1. **REFACTOR** — existing test does the same job ad-hoc; replace
   inline check with utility call.  Test continues to exist with its
   primary subject (e.g. vault encryption); mode-bit assertion
   delegates.  Logged as MODIFY.
2. **KEEP** — existing test covers a different surface; no change.
3. **OBSOLETE** — existing test covers production code being deleted
   in the same task chain (e.g. `RoleIdentityPolicy` removal under
   HEP-0035 §4.5).  **Do not delete in the current task.**  File a
   line in `TESTING_TODO.md` under "Obsolete after <task>" with the
   exact file path; deletion happens with the production-code removal
   commit.

### 6.6 Assertion discipline (per `IMPLEMENTATION_GUIDANCE.md`)

For every assertion that gates a contract:

- **Path discrimination** — pin exception type + message substring,
  not just `EXPECT_THROW(..., std::exception)`.
- **Timing bound** — when "fast" is in the contract, assert wall-clock.
- **Structural payload** — for `{status, result}` envelopes, assert
  result fields, not just envelope.
- **Mutation sweep** — before committing, flip the production code's
  decision bit and confirm the test fails.  Restore.  Document in the
  file-level comment block which mutations were swept.
- **Log capture** — every test that calls LOGGER_* installs
  `LogCaptureFixture`; TearDown asserts zero unexpected WARN/ERROR.
  Error-path tests register expected matchers up-front.

### 6.7 Test bypass discipline (per memory rule)

L2 tests that bypass production setup MUST declare the bypass in a
file-level comment block stating: **purpose / bypass / why / canonical
storage it populates / re-examine when**.  No silent helpers.

---

## 7. Refactor map — #101 (verified against actual code 2026-05-29)

### 7.1 Existing tests that REFACTOR (delegate inline mode checks to utility)

| File | Lines | Today | After 1C |
|---|---|---|---|
| `test_hub_vault.cpp` | `:152-153, 240-241, 271` | inline `fs::permissions` check for `hub.vault` 0600 | `EXPECT_TRUE(verify_keyfile_acl(path).ok())` |
| `test_hub_vault.cpp` | `:407-408` | inline check for `*.pub` 0644 | same |
| `test_role_vault.cpp` | `:94-103, 156` | inline check for `role.vault` 0600 (with Windows-skip) | same; Windows-skip moves into utility |
| `test_hub_directory.cpp` | `:89-92` | inline `st_mode & 0777 == 0700` for `vault/` dir | same |

### 7.2 Existing tests that KEEP unchanged

- `test_hub_vault.cpp` outside the lines above — vault encrypt /
  decrypt / password handling.
- `test_role_vault.cpp` outside line ranges above — role vault open /
  close.
- `test_hub_directory.cpp` directory structure tests not touching mode.
- `test_plh_hub_keygen.cpp`, `test_plh_role_keygen.cpp` — CLI flow
  (vault creation, pubkey emission, error on unset keyfile).
- `test_plh_hub_init.cpp` directory structure existence tests.

### 7.3 NEW test (sub-phase 1B)

`tests/test_layer2_service/test_key_file_acl.cpp` — Pattern 1+
(single Logger module).  Fixture `KeyFileAclTest`.  Owns the full
verdict matrix:

- 0600 sec file → Ok
- 0640 sec file → Error("group-readable", chmod 0600 hint)
- 0644 sec file → Error("group/world-readable")
- 0666 sec file → Error
- wrong owner → Error("expected uid M, got N")
- missing file → Error("path does not exist")
- 0644 pub file → Ok
- 0600 pub file → Ok (more restrictive is fine; check this is
  intended)
- 0700 vault dir → Ok
- 0750 known_roles dir → Ok
- 0755 known_roles dir → Error
- 0777 known_roles dir → Error
- world-writable config → Error

Plus mutation-sweep verification: deliberately break each gate bit in
the utility, run the matrix, confirm each verdict flips for the
expected mode.  Restore.  Document in file-level comment.

### 7.4 NEW L4 tests (sub-phase 1E, extend existing fixtures)

`test_plh_hub_keygen.cpp` — add tests asserting utility-validated
modes after `--keygen` runs.

`test_plh_hub_init.cpp` — extend `CreatesDirectoryStructure` to call
utility on each created dir; add startup-refusal tests (run binary
against a tmp dir with intentionally-loose mode and assert non-zero
exit + OpenSSH-style error in stderr).

Symmetric for `test_plh_role_keygen.cpp` and (whichever role error/init
file exists; verify via `ls tests/test_layer4_plh_role/` at start of
1E).

### 7.5 Tests flagged OBSOLETE (do NOT delete in #101; logged for #74)

`tests/test_layer3_datahub/test_datahub_role_identity_policy.cpp` +
`tests/test_layer3_datahub/workers/role_identity_policy_workers.cpp`
— the file's own header comment says "placeholder mechanism per
HEP-CORE-0035 §1 (pending HEP-0035 Phase 6 retirement)."  HEP-0035
§4.5 drops `RoleIdentityPolicy` enum + `ChannelPolicyOverride`.  When
the production code (`src/include/utils/role_identity_policy.hpp`,
`src/utils/ipc/broker_service.cpp` `check_role_identity`) is removed
in task #74, these tests + worker file go with them.

**Action in #101:** add a line to `docs/todo/TESTING_TODO.md` under
"Obsolete after HEP-0035 §4.5 cleanup (task #74)" with the two
explicit paths so they aren't forgotten.

---

## 8. Anti-patterns observed this session (don't repeat)

- **Inventing test directory names** (`tests/L1_pure/`) without reading
  `tests/` actual layout.  Always `ls tests/` first.
- **Inventing wire fields** (`endpoint_hint_range`) and tracking them
  as "open questions."  Grep src + docs first.
- **Imagining failure modes** (ZAP thread dies independently of BRC)
  contradicted by the design doc (`HEP-0036 §7.1`).  Read the relevant
  section before raising the concern.
- **Recommending non-goals** for policy decisions the model already
  precludes (per-producer ACL under queue model).  Re-read the
  invariants / data structures first.
- **Skipping the discussion step** before editing ("go ahead" without
  presenting what's being decided).  Findings → discuss → approval →
  code.  Always.
- **Adding instead of refactoring.**  Survey existing tests in the
  area before writing a new one.  Inline checks that the new utility
  subsumes get refactored, not duplicated.
- **Deletion without discussion.**  Even when code is "obviously" dead,
  flag for review.  Per memory rule: "before you delete any code, we
  need to discuss."

---

## 9. Session-start checklist

Every time a new session opens to work on this chain:

1. `git status` + `git log --oneline -20` — what shipped since last
   session; commits don't lie, but check the actual files.
2. Open this guideline.  Note which task you're on.
3. Open the HEPs/READMEs from §2 above for the surface you're touching.
4. Open `docs/TODO_MASTER.md` "Production-readiness gap" + "P3 —
   HEP-0036 auth implementation chain" sections.
5. Check TaskList for this task's open sub-tasks.
6. If touching tests: open `README_testing.md` § "Choosing a test
   pattern" + § "Naming conventions" + `IMPLEMENTATION_GUIDANCE.md`
   § "Assertion Design — silent-failure prevention".
7. Survey existing tests in the area (§6.5 refactor-not-add discipline).
8. Present plan + survey results to user.  Wait for approval.
9. Mark sub-task in-progress.  Code.  Mutation-sweep.  Commit.
10. Mark sub-task completed.  Update this guideline if scope changed.

---

## 10. Archival trigger

When tasks #101, #102, #74, #94, #103, #104 are all ✅ shipped:

1. Merge lasting insight from §3 (principles), §6 (test discipline),
   §8 (anti-patterns) into `IMPLEMENTATION_GUIDANCE.md` and the
   relevant HEPs.
2. Move this file to `docs/archive/transient-YYYY-MM-DD/`.
3. Record in `docs/DOC_ARCHIVE_LOG.md`.

Until then this is the authoritative implementation reference for the
chain.
