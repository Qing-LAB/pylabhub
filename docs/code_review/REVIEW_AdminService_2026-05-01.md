# REVIEW — AdminService design audit (HEP-CORE-0033 §11)

| Property        | Value                                                         |
|-----------------|---------------------------------------------------------------|
| **Status**      | 🟡 OPEN — working backlog for HEP-0033 Phase 6.2              |
| **Created**     | 2026-05-01                                                    |
| **Scope**       | HEP-0033 §11 (`AdminService`) design correctness + readiness  |
| **Trigger**     | Pre-implementation audit before Phase 6.2 work begins         |
| **Author**      | Quan Qing + Claude Opus 4.7 (audit) — 2026-05-01              |
| **Disposition** | Transient — archive to `docs/archive/transient-YYYY-MM-DD/` when all action items resolve and Phase 6.2 is closed |

---

## 1. Summary

The HEP §11 design is structurally sound and aligned with the rest of
the system (HubHost ownership, ThreadManager threading, HubVault
admin token). **About half of the §11.2 method surface is blocked
on upstream HEPs (HEP-0035 auth, HEP-0033 Phase 7 hub-side scripting).**
Phase 6.2 ships the unblocked subset cleanly; the rest is deferred
with explicit cross-references.

---

## 2. Audit findings

### 2.1 Design correctness

- **Subsystem pattern matches HubHost ownership** (HEP §4): owned via
  `unique_ptr<AdminService>`, spawned via the host's `ThreadManager`.
  Init step 9 + shutdown step 3 already pinned in HEP §4.1/§4.2 and
  fit cleanly under the new phase FSM (§4.3).
- **Auth model matches existing HubVault**:
  `hub_vault.cpp::generate_admin_token` already produces a 64-hex
  token; §11.3 token-required logic just consumes it.
- **JSON envelope** (`{method, token, params}` →
  `{status, result|error}`) is consistent with the structured-RPC
  pattern used elsewhere; no novel transport.
- **Threading + cross-thread dispatch** matches the broker thread
  pattern (§4.2 step 3 + ThreadManager auto-registration as a dynamic
  `LifecycleGuard` module).

### 2.2 §11.2 method readiness matrix

| §11.2 method            | Mutator/accessor available?                                | Blocked by               |
|-------------------------|------------------------------------------------------------|--------------------------|
| `list_channels`         | ✅ `BrokerService::list_channels_json_str`                 | none                     |
| `get_channel`           | 🟡 `query_channel_snapshot` exists, JSON shape needs work  | none                     |
| `query_metrics`         | ✅ broker `query_metrics_json_str`                         | none                     |
| `list_roles`            | 🟡 HubState read paths exist, JSON shape needs work        | none                     |
| `get_role`              | 🟡 same as above                                           | none                     |
| `list_bands`            | 🟡 same as above                                           | none                     |
| `list_peers`            | 🟡 same as above                                           | none                     |
| `close_channel`         | ✅ `BrokerService::request_close_channel`                  | none                     |
| `broadcast_channel`     | ✅ `BrokerService::request_broadcast_channel`              | none                     |
| `request_shutdown`      | ✅ `HubHost::request_shutdown()`                           | none                     |
| `revoke_role`           | ❌ no `_on_role_revoked` capability op                     | small new mutator needed |
| `reload_config`         | ❌ no whitelist defined                                    | HEP-0033 §16 item 9      |
| `add_known_role`        | ❌ no allowlist                                            | HEP-CORE-0035            |
| `remove_known_role`     | ❌ no allowlist                                            | HEP-CORE-0035            |
| `list_known_roles`      | ❌ no allowlist                                            | HEP-CORE-0035            |
| `exec_python` (dev)     | ❌ HubScriptRunner not yet built                           | HEP-0033 Phase 7         |

**Tally**: 10/16 methods ship in Phase 6.2; 6/16 deferred.

### 2.3 Concrete missing wiring (besides AdminService itself)

1. **Vault → HubHost.** `HubHost::HubHost(HubConfig)` takes only the
   config; the vault is unlocked by main BEFORE construction (HEP
   §4.1 step 2), but `admin_token` lives on the vault, not on
   `cfg.auth()`.  Need to decide:
     - (a) copy `admin_token` into `HubConfig` at vault unlock,
     - (b) pass `HubVault` to `HubHost` constructor,
     - (c) `AdminService` takes the vault directly.
   Recommend **(a)** — keeps `HubVault` short-lived (vault unlock
   happens once at main, then the unlocked secrets travel with the
   config), and avoids leaking `HubVault` into more subsystems.
2. **Localhost-bind enforcement on `dev_mode==true && token_required==false`.**
   HEP §11.3 mandates this — must be asserted at AdminService
   construction time (parse `tcp://...` → check host == 127.0.0.1).
3. **`admin.endpoint` field in hub.json templates.**
   `init_directory` for hub doesn't write the `admin` block — verify
   before Phase 6.2 lands and add to the template if missing.

### 2.4 Layer / module organization

- **Header layout**: `src/include/utils/` is flat — no `service/`
  subdir for headers despite `src/utils/service/` existing for
  implementations. Established asymmetry; `admin_service.hpp` joins
  the flat top level (matches HEP §11.4 path).
- **Implementation directory — `service/` vs `ipc/`**:
    - `src/utils/ipc/` holds *transport-owning* infrastructure
      (`messenger.cpp`, `broker_service.cpp`, `channel_registry.cpp`,
      `federation_manager.cpp`).
    - `src/utils/service/` holds *owned services* (`hub_host.cpp`,
      `hub_vault.cpp`, `role_host_core.cpp`, `engine_host.cpp`).
  AdminService is **hybrid** — owns a REP socket (IPC-like) but is
  owned BY HubHost (service-like).  HEP §11.4 says
  `src/utils/service/`, but **`src/utils/ipc/admin_service.cpp` would
  group it with `broker_service.cpp` (the other REP-socket-owning
  subsystem), which is more consistent**.  Recommend updating HEP
  §11.4 file path before code lands.
- **Namespace**: spec is silent.  `pylabhub::admin` for symmetry with
  `pylabhub::broker` and `pylabhub::hub_host`.
- **Dep direction**: AdminService → HubHost (read state, call broker
  mutators) → BrokerService → HubState. No back-edges.  ScriptEngine
  dependency (dev-only) is gated by `dev_mode` config.

---

## 3. Phase 6.2 implementation plan (recommended split)

| Sub-phase | Scope                                                                                            | Blockers |
|-----------|--------------------------------------------------------------------------------------------------|----------|
| **6.2a**  | AdminService skeleton: REP socket bind, JSON envelope, run loop with stop, token gate, localhost-bind assertion, ThreadManager registration, HubHost lifecycle integration. + 4–5 L2 unit tests + 2–3 L3 protocol tests. | none |
| **6.2b**  | Query methods that map onto existing accessors: `list_channels`, `get_channel`, `query_metrics`, `list_roles`, `get_role`, `list_bands`, `list_peers`. (7 methods, all ✅/🟡 in §2.2.) | none |
| **6.2c**  | Control methods that map onto existing mutators: `close_channel`, `broadcast_channel`, `request_shutdown`. (3 methods.) | none |
| ~~**6.2d**~~ | **Deferred — explicitly out of Phase 6.2 scope:** `revoke_role` (small mutator), `reload_config` (HEP §16 item 9), `add_known_role` / `remove_known_role` / `list_known_roles` (HEP-0035), `exec_python` (Phase 7). | upstream HEPs |

---

## 4. Action items

### 4.1 OPEN

- [ ] **A1** — Decide vault→HubHost wiring (option a/b/c in §2.3.1).
- [ ] **A2** — Update HEP §11.4 file path: `src/utils/ipc/admin_service.cpp`
      (or document why `service/` is preferred).
- [ ] **A3** — Verify hub `init_directory` template emits the `admin`
      block; add if missing.
- [ ] **A4** — Implement Phase 6.2a (skeleton) per §3.
- [ ] **A5** — Implement Phase 6.2b (query methods) per §3.
- [ ] **A6** — Implement Phase 6.2c (control methods) per §3.
- [ ] **A7** — Update HEP-0033 §11.2 to mark deferred methods with
      explicit upstream-HEP citations.
- [ ] **A8** — Update MESSAGEHUB_TODO with the 6.2a/b/c split + the
      §11.2 readiness matrix from §2.2.

### 4.2 RESOLVED (closed during Phase 6.2 implementation)

(populated as items land)

---

## 5. Cross-references

- **HEP-CORE-0033 §11** — AdminService normative spec (this audit).
- **HEP-CORE-0033 §4** — HubHost ownership model + phase FSM.
- **HEP-CORE-0033 §15 Phase 6.2 / Phase 7** — sequencing.
- **HEP-CORE-0033 §16 item 9** — `reload_config` whitelist (deferred).
- **HEP-CORE-0035** — Hub-Role Authentication and Federation Trust
  (blocks `add/remove/list_known_roles`).
- **HubVault** — `src/include/utils/hub_vault.hpp`,
  `src/utils/service/hub_vault.cpp` (admin token source).
- **BrokerService mutators** — `src/include/utils/broker_service.hpp`
  (`request_close_channel`, `request_broadcast_channel` — already
  shipped; `_on_role_revoked` to add).

---

## 6. Lifecycle of this document

This is a **transient** review per `docs/DOC_STRUCTURE.md §1.7`:

- Active while Phase 6.2 implementation is in flight.
- Each closed action item marked ✅ FIXED with date in §4.
- When all §4.1 items are ✅ AND HEP §11.2 readiness matrix is closed
  out (every cell is ✅ or has a deferral citation), this file is
  **archived** to `docs/archive/transient-YYYY-MM-DD/` with an entry
  in `docs/DOC_ARCHIVE_LOG.md`.  Lasting findings (the layer-placement
  recommendation, the vault wiring decision, the readiness matrix
  shape) merge into HEP-0033 §11 before archival.
