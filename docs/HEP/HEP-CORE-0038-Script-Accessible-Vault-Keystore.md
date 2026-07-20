# HEP-CORE-0038: Script-Accessible Vault Keystore

> **⚠ PARTIALLY SUPERSEDED by HEP-CORE-0043 (updated 2026-07-06).**
>
> - **Design contract for KeyStore storage/access (adds & retrieves
>   vault entries via `add_raw` / `lookup_raw`)** — SUPERSEDED by
>   **HEP-CORE-0043 §2.2 + §7** (SHIPPED 2026-07-06).  KeyStore is
>   a member of `SecureSubsystem::Impl`; script-vault entries land
>   in the same store via `secure().keys().add_raw(name, bytes)`.
>   Consult HEP-0043 §7 for the storage API.
> - **STILL AUTHORITATIVE (as design intent):** this HEP is a DRAFT
>   whose detailed vault-format / KDF / AEAD / script-API / name-
>   sandboxing design was never written out — it is deferred to
>   task #106.  What is recorded here is the design INTENT and scope
>   (§1 Motivation, §2 Design intent, §3 Open questions, §4
>   Implementation scope, §5 Cross-references) for role-side,
>   script-managed vault entries.  For the SHIPPED KeyStore storage
>   and access API, HEP-CORE-0043 §2.2 + §7 is authoritative; the
>   vault-file/KDF/AEAD parameters live in HEP-CORE-0035 §4.6.  There
>   are no authoritative §6-§9 subsections in this document.


| Property        | Value                                                           |
|-----------------|-----------------------------------------------------------------|
| **HEP**         | `HEP-CORE-0038`                                                 |
| **Title**       | Script-accessible Vault Keystore (per-role encrypted KV store)  |
| **Status**      | 🚧 **DRAFT** — design intent recorded 2026-05-29; detailed design + implementation pending under task #106 |
| **Created**     | 2026-05-29                                                      |
| **Area**        | Role framework / vault facility / script-callback API           |
| **Depends on**  | HEP-CORE-0035 §4.6 (vault facility), HEP-CORE-0035 §4.7 (runtime key handling), HEP-CORE-0011 (script-callback API surface) |
| **Tracks**      | task #106                                                       |
| **Relationship to HEP-CORE-0035** | HEP-0035 §4.8 stores hub-wide allowlist secrets in the hub vault, managed by operator CLI.  HEP-0038 is the parallel mechanism for ROLE-side secrets, managed by user scripts.  Same underlying vault facility; different scope and access boundary. |

---

## 1. Motivation

User scripts running inside `plh_role` binaries sometimes need to
persist secrets between runs: external API tokens, OAuth refresh
tokens, MQTT broker passwords, license keys, etc.  Today no
framework-provided mechanism exists for this.  Scripts that need it
either embed secrets in their source files (insecure, version-control
hazard) or read from environment variables / files they manage
themselves (fragmented, inconsistent protection, no audit story).

This HEP introduces a framework API giving each role its own
isolated, password-protected secret store, layered on the existing
vault facility (HEP-CORE-0035 §4.6).  Scripts get persistent secret
storage without ever touching the master password or the cipher
state; the framework owns the key-management mechanics.

---

## 2. Design intent

### 2.1 What this is

- A per-role key-value store for arbitrary script-managed secrets.
- Storage: encrypted at rest inside the role's vault (extends the
  existing role vault payload).
- API: two new script-callable methods —
  - `api.vault_save(key, value) → bool`
  - `api.vault_load(key) → optional<value>` (or sentinel "missing")
- Values are opaque from the framework's perspective; the script
  decides serialization (UTF-8 text, JSON, raw bytes).  Type
  semantics per engine (Python / Lua / Native) to be detailed in §4.

### 2.2 Isolation guarantees

- A role can ONLY read/write its own store.  Cross-role access is
  not provided by the API surface; no `vault_load(other_role_uid, key)`.
- Storage layout (deferred — see "Open design questions" below):
  the script keystore lives at the role's vault location per
  HEP-CORE-0024 §3.4 (operator-controlled path string in
  `auth.keyfile`).  Whether script secrets share the framework
  identity vault file (`<resolved>.vault`) or a sibling
  (`<resolved>.scripts.vault` or `<resolved>.vault/` as a
  directory) is open: the file-blob model and the
  directory-of-files model have different cost / atomicity /
  granularity trade-offs that will be settled when this HEP
  picks up implementation (task #106).  The API contract below
  is shape-agnostic on purpose.
- The framework opens the keystore with the master password at
  role startup (provided via `PYLABHUB_ROLE_PASSWORD` env var or
  interactive prompt per HEP-CORE-0035 §4.6.2).  Subsequent
  `vault_save` / `vault_load` calls operate on the decrypted
  payload + write back through the AEAD seal.  The script never
  sees the password.
- A processor role has two presences (rx + tx) but ONE role identity
  and ONE vault.  Both sides see the same `scripts` map.

### 2.3 What this is NOT

- Not a multi-role coordination mechanism — use `band` (HEP-CORE-0030)
  or `inbox` (HEP-CORE-0027) for that.
- Not a hub-wide secrets store — use the hub vault via operator CLI
  per HEP-CORE-0035 §4.8.
- Not an audit-logged surface — the master password is the only
  access gate; the framework does not log every `vault_save` /
  `vault_load` call.  Operators concerned about misuse run the
  binary under a dedicated user account whose process accounting
  they monitor independently.
- Not a quota-managed surface in MVP — unbounded growth is the
  operator's responsibility.  Future hardening MAY add quotas.

---

## 3. Open design questions (to resolve when task #106 is picked up)

These are deliberately NOT pinned here — they need discussion when
implementation work starts.  Recording them now prevents drift and
lets the next session begin from a known scope.

1. **Value encoding**: opaque bytes vs. UTF-8 string vs. JSON-typed?
   Each has different ergonomics across the three engines.  Native
   plugins want bytes; Python scripts want strings/dicts; Lua wants
   strings.  Probable answer: bytes at the framework boundary,
   per-engine convenience wrappers.
2. **Write atomicity**: write-through on every `vault_save`, or
   batched flush at role shutdown?  Trade-off: durability vs. write
   amplification on the vault file (which is the full encrypted
   payload, not an append log).  Probable answer: write-through
   with a debounce window (~500 ms) to coalesce rapid writes; final
   flush on role-host shutdown.
3. **Concurrent access**: processor role's two presences (rx + tx)
   may both call `vault_save` from worker threads.  Serialize at the
   role-host level (one mutex) or push concurrency into the vault
   crypto layer (which would require streaming AEAD, currently not
   the case)?  Probable answer: serialize at role-host level; the
   vault is a single file so concurrent writes can't atomically
   succeed anyway.
4. **Schema migration**: a vault created before HEP-0038 has no
   `scripts` key.  Lazy-add on first `vault_save`, or one-time
   migration step at role startup?  Probable answer: lazy.
5. **Quota / limits**: per-key payload size, per-role key count.
   Defaults?  Configurable?
6. **Error semantics**: `vault_load("missing-key")` vs. `vault_load`
   on a vault that hasn't been initialized (returning `nullopt`
   sentinel vs. raising vs. error string)?  Should match the
   `api.list_*` HubAPI conventions (HEP-CORE-0033 §12).
7. **Engine parity**: the three engines (Python / Lua / Native)
   must see identical semantics.  HEP-CORE-0011 §"Tier 2 dynamic
   callbacks" parity matrix applies.

---

## 4. Implementation scope (when task #106 is worked)

Production-code changes:

- Extend `RoleVault` payload schema (`src/include/utils/role_vault.hpp`
  + `src/utils/service/role_vault.cpp`) to include the `scripts` map.
  Backwards-compatible: missing key reads as empty map.
- Add `RoleVault::get_script_value(key)` / `set_script_value(key, value)`
  C++ methods (framework-internal; called from `RoleAPIBase`, not
  scripts directly).
- Add `api.vault_save` / `api.vault_load` to the script-callback API
  (HEP-CORE-0011 surface); wire through Python (`scripting/python/`),
  Lua (`scripting/lua/`), Native (`utils/native_engine.cpp`) engines.

Tests (per `docs/README/README_testing.md` patterns):

- L2 test for `RoleVault` extended payload: round-trip, missing-key
  behavior, write-after-load, payload size limit.  Pattern 3
  (`IsolatedProcessTest`) because `RoleVault` constructs hit
  lifecycle modules (Logger + libsodium).
- L2 test for the in-vault `scripts` schema migration: open a
  pre-HEP-0038 vault, call `vault_save`, verify the new key was
  added and the existing keypair is intact.
- L3 test for cross-engine API parity: each of Python / Lua /
  Native sees consistent semantics (`vault_save` returns ok,
  `vault_load` returns same value, missing-key returns the parity
  sentinel).
- L4 demo: a producer role that uses `vault_save` to persist a
  fake external-API token across restarts.  Use the existing demo
  framework (`share/demo_framework/runner.py`).

Documentation:

- Update `docs/README/README_Scripting_Python.md` /
  `README_Scripting_Lua.md` / `README_Scripting_Native.md` (per
  task #87) with the new API surface.
- Add HEP-CORE-0011 entry for `vault_save` / `vault_load` in the
  script callback / API tables.

---

## 5. Cross-references

- HEP-CORE-0035 §4.6 — vault facility + file ACL discipline
- HEP-CORE-0035 §4.7 — runtime key handling (mlock + zeroing)
- HEP-CORE-0035 §4.8 — known-roles allowlist storage (the analog
  for HUB-side secrets; this HEP is the role-side analog)
- HEP-CORE-0011 — script-callback API surface
- HEP-CORE-0027 — inbox (for multi-role messaging, NOT for shared
  secrets — the contrast)
- task #106 — implementation tracking
- `src/include/utils/role_vault.hpp`, `src/utils/service/role_vault.cpp`,
  `src/utils/service/vault_crypto.{hpp,cpp}` — production code to
  extend
