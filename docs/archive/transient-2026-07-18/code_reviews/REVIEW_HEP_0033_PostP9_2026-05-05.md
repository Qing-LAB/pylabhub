# HEP-CORE-0033 Post-P9 Audit — Inconsistencies and Hub-Role Gaps

**Date:** 2026-05-05
**Branch:** `feature/lua-role-support` (post `0836cda` — plh_hub binary + L4 fixture)
**Scope:** Concrete code reading of the post-P9 tree against HEP-CORE-0033's
normative claims, plus a hub↔role end-to-end gap audit.

The plh_hub binary builds and the no-hub L4 test tier (17 tests) is green.
This audit pins what's *still wrong or absent* — i.e. what would make a
"plh_hub + plh_role" pipeline fail despite the binaries existing.

---

## Findings

### F1 — `HubVault::publish_public_key` is dead code; **`hub.pubkey` is never written by any production path**  ❌ BLOCKER

**Severity:** Critical — breaks role↔hub CURVE auth out of the box.

**Files:**
- `src/utils/service/hub_vault.cpp:171` — `publish_public_key` defined,
  writes `<hub_dir>/hub.pubkey` correctly.
- `src/utils/config/hub_config.cpp:203-214` — `HubConfig::create_keypair`
  calls `HubVault::create` (which writes `<hub_dir>/hub.vault`) but does
  NOT call `publish_public_key`.
- `src/utils/service/hub_host.cpp` — startup wires `bound_pubkey` from the
  ready callback but never persists it to disk.
- `src/plh_hub/plh_hub_main.cpp:do_keygen` — calls `cfg.create_keypair`,
  prints the pubkey to stdout, exits.  No `publish_public_key`.

**Consumer:**
- `src/include/utils/config/hub_ref_config.hpp:95-100` — role-side
  `HubRefConfig::parse_hub_ref_config` reads `<hub_dir>/hub.pubkey` to
  populate `broker_pubkey`.  When the file is absent, `broker_pubkey`
  is empty → role's broker connection has no pin → CURVE handshake fails
  (or silently downgrades to anonymous, depending on producer cfg).

**Effect on the documented hub-role flow:**
1. `plh_hub --init lab1 --name lab1` → creates `lab1/hub.json` etc.
2. `plh_hub --config lab1/hub.json --keygen` → writes `lab1/hub.vault`
   only.  **Operator stdout shows the pubkey, but no `lab1/hub.pubkey`
   exists for roles to read.**
3. `plh_hub lab1` → starts hub on broker_endpoint, holds pubkey in memory.
4. Producer with `out_hub_dir: lab1` → reads `hub.json` (gets endpoint
   OK), tries `lab1/hub.pubkey` (file not present), `broker_pubkey = ""`.
5. Producer connects to broker without CURVE auth.  Hub may accept or
   reject depending on its own auth state.

**Test gap:** `tests/test_layer2_service/test_hub_vault.cpp:382` directly
exercises `publish_public_key` in isolation, so the function works.  No
integration test verifies that the production flow (`--keygen` →
`--run`) actually produces `hub.pubkey`.  Classic "tested but not used."

**Fix:** Call `HubVault::publish_public_key(hub_dir)` from inside
`HubConfig::create_keypair` after `HubVault::create` returns.  One-line
add; no API change.

---

### F2 — Vault file path discrepancy: HEP §7 layout vs `HubVault` impl  ⚠️ DESIGN

**Severity:** Documentation/operator confusion — currently functional
because `--keygen` write and `--run` read use the same wrong path.

**HEP-0033 §7** (line 648-649):
```
├── vault/
│   └── hub.vault               # CURVE keypair + (optional) admin token
```

**`HubDirectory::hub_vault_file()`** (`src/include/utils/hub_directory.hpp:155`):
```cpp
std::filesystem::path hub_vault_file() const {
    return vault() / "hub.vault";   // <base>/vault/hub.vault
}
```

**`HubVault::create` / `HubVault::open`** (`src/utils/service/hub_vault.cpp:110, 129`):
```cpp
detail::vault_write(hub_dir / "hub.vault", payload.dump(), password, hub_uid);
//                  ^^^^^^^^^^^^^^^^^^^^ — flat under hub_dir, NO vault/ subdir
```

**`--init` template emits** `auth.keyfile = "vault/hub.vault"`
(`hub_directory.cpp:124`) — promises a path that the rest of the system
doesn't honour.

**Operator-visible symptom:** init creates a `vault/` subdirectory; the
vault file lands at the parent.  `vault/` is empty after `--keygen`.

**Currently-working path:** since both `create_keypair` and `load_keypair`
go through `HubVault::{create,open}(hub_dir, ...)` with `hub_dir =
HubConfig::base_dir()`, both write and read the SAME wrong path
(`<base>/hub.vault`).  So the system functions despite the discrepancy;
the bug is in observability and HEP credibility.

**Fix options:**
- (a) Move vault into `<base>/vault/hub.vault` to match HEP §7.
  Touches `HubVault::{create,open}` + `HubConfig::{create,load}_keypair`
  + `tests/test_layer2_service/test_hub_vault.cpp` (4 expectations on
  `<hub_dir>/hub.vault` need to become `<hub_dir>/vault/hub.vault`).
- (b) Update HEP §7 + init template + `auth.keyfile` default to
  `hub.vault` (drop the `vault/` subdir).  Simpler but loses parity
  with role-side directory layout where everything goes in subdirs.

Recommend (a) — maintains HEP-CORE-0024 directory-layout discipline.

---

### F3 — HEP §5 + §6.5 still reference the rejected `--dev` mode  ❌ DOC STALE

**Severity:** Documentation only — the mode is correctly absent from the
implementation.

**References still in HEP-0033:**
- Line 446: `plh_hub --dev                         # Dev/test mode; ephemeral keys, no vault`
- Line 625: "**Run mode** (without `--dev`) reads the vault, …"
- Line 633: "**`plh_hub --dev`** generates an ephemeral CURVE keypair at startup, …"

**Source of truth** — HEP §17.1:
> the `dev_mode` admin config flag was removed entirely on 2026-05-04 …
> See HEP-0033 §15 Phase 7 Commit-E rejection note.

**`hub_cli.hpp`:** no `--dev` parsing; the `HubArgs` struct has no `dev`
field.

**Fix:** Strip the three §5/§6.5 references; update the §6.5 narrative
to reflect that the only modes are `--init` / `--keygen` / `--validate`
/ run.

---

### F4 — Init-template `broker_endpoint = tcp://0.0.0.0:5570` becomes a
**connect** string for roles  ⚠️ USABILITY

**Severity:** Operator footgun — most operators discover this on the
first cross-host attempt.

**Code:** `src/utils/config/hub_directory.cpp:144` writes
`network.broker_endpoint = "tcp://0.0.0.0:5570"` into the generated
`hub.json`.  The hub uses this string as its **bind** address
(`HubHost::startup_` → `BrokerService::Config::endpoint`).

**Role side:** `src/include/utils/config/hub_ref_config.hpp:77` reads
the same string and uses it as the **connect** string when the role
opens its broker socket.  A role on a different host (or even same-host
in some libzmq versions) trying `connect("tcp://0.0.0.0:5570")` will
fail or behave unpredictably.

**Fix options:**
- (a) Keep `0.0.0.0` for bind (correct), make HubHost rewrite the
  published `broker_endpoint` to the actual reachable address.
  Requires "what IP do roles see?" inference — non-trivial cross-host.
- (b) Default the init template to `tcp://127.0.0.1:5570` (loopback)
  with a comment saying operators must override for cross-host
  deployments.  Single-machine demos work out of the box.
- (c) Split the field: `broker_bind_endpoint` (hub-only) vs
  `broker_publish_endpoint` (what roles connect to).  Cleanest but a
  schema change; operators must set both.

Recommend (b) for now — matches the demo use case; operators editing
for cross-host already understand the rebind.

---

### F5 — No L4 / L3 integration coverage exercises hub-role end-to-end  ⚠️ TEST

**Severity:** Test gap — symptom of the F1/F2/F4 bugs going undetected
until manual integration time.

**MESSAGEHUB_TODO.md** "System-level L4 tests" lists these explicitly as
deferred until plh_hub binary lands.  Now that plh_hub exists, the four
deferred tests become writable:

  - plh_role run-mode lifecycle (no hub).
  - plh_role + broker round-trip (producer → hub → consumer).
  - plh_role channel broadcast (hub broadcast/notify control plane).
  - plh_role processor pipeline (producer → processor → consumer).

The first one is no-hub and can land immediately.  The other three would
have caught F1 (without a `hub.pubkey`, the pipeline fails at producer
registration when CURVE is enabled).

---

### F6 — `broker.request_broadcast_channel` synthetic
`sender_uid = self_hub_uid` (cleanup from §17 review) — wire-compatibility check

**Severity:** Behavioural change — verified compatible.

After the 2026-05-05 cleanup the synthetic CHANNEL_BROADCAST_REQ tags
its sender as the hub's own uid (or `"hub"` fallback) instead of the
legacy literal `"admin_shell"`.  Recipients of this frame —
`handle_channel_broadcast_req` (broker-internal dispatch) — only use
sender_uid for log lines and the broadcast envelope's `originator_uid`
field.  No path discriminates on the literal value.

Producer/consumer side: the broadcast NOTIFY frame echoes the field;
script callbacks see it as the broadcaster's identity.  The hub uid
is a more accurate identity than `"admin_shell"` ever was.

No regression risk identified.

---

## Hub↔Role end-to-end gap summary

What it takes for `plh_role <role_dir>` to successfully connect to
`plh_hub <hub_dir>` and start a data-flow pipeline:

| Step | Code path | Status |
|---|---|---|
| 1. plh_hub starts, broker binds | `HubHost::startup_` → `BrokerService::run` | ✅ |
| 2. plh_hub writes `<hub_dir>/hub.json` (during init) | `HubDirectory::init_directory` | ✅ |
| 3. plh_hub `--keygen` writes vault + pubkey | `HubConfig::create_keypair` | **F1** writes vault only |
| 4. Role reads `<hub_dir>/hub.json` for endpoint | `HubRefConfig::parse_hub_ref_config:74-80` | ✅ |
| 5. Role reads `<hub_dir>/hub.pubkey` for CURVE pin | `HubRefConfig::parse_hub_ref_config:95-100` | **F1** pubkey absent → empty pin |
| 6. Role connects to broker_endpoint | role-side broker_request_comm | ⚠️ F4 if endpoint is `0.0.0.0` |
| 7. Role sends `REG_REQ` / `CONSUMER_REG_REQ` | broker_request_comm + BrokerService handler | ✅ |
| 8. Broker registers role into `HubState` | `_on_channel_registered` / `_on_consumer_joined` | ✅ |
| 9. Role enters data loop | role_host_base run loop | ✅ |
| 10. Slot R/W via SHM channel | DataBlock / ShmQueue | ✅ |
| 11. Hub script `on_channel_opened` fires | HubScriptRunner event drain | ✅ |
| 12. Admin RPC `query_metrics` reaches role state | AdminService → HubState | ✅ |

**Steps 3 + 5 are the breaking pair.**  Without F1's fix the pipeline
either:
- Runs without CURVE auth (if both sides allow it) — bypass.
- Fails at the broker handshake (if `auth.client_pubkey` non-empty
  on either side) — diagnostic message about missing pubkey.

**Step 6 is the operator footgun.**  Same-host operators dodge it; the
moment someone tries cross-host they hit it.

Everything else in the chain works as designed.

---

## Recommended fixes — priority order

| # | Finding | Fix | Effort | Lands when |
|---|---|---|---|---|
| 1 | F1 | Add `vault.publish_public_key(hub_dir)` to `HubConfig::create_keypair` | 1 line | This sweep |
| 2 | F3 | Strip `--dev` from HEP §5 + §6.5 | doc edit | This sweep |
| 3 | F4 | Init template default to `tcp://127.0.0.1:5570` + comment | 2-line config + HEP §6.2 example update | This sweep |
| 4 | F2 | Move vault to `<base>/vault/hub.vault` (HubVault internal change + 4 test updates) | small | Separate slice |
| 5 | F5 | Land deferred MESSAGEHUB_TODO L4 tests one at a time | per-test slices | Subsequent commits |

This sweep targets #1-3.  #4 and #5 are tracked in `MESSAGEHUB_TODO.md`
for follow-up.

---

## Status

| Item | Status |
|---|---|
| F1 — hub.pubkey publishing | ✅ FIXED 2026-05-05 (commit `1439ef4`) |
| F2 — vault subdir layout | ✅ FIXED 2026-05-05 (commit `efb604c`) |
| F3 — `--dev` doc stale | ✅ FIXED 2026-05-05 (commit `1439ef4`) |
| F4 — broker_endpoint default | ✅ FIXED 2026-05-05 (commits `1439ef4` template + `efb604c` struct default) |
| F5 — L4 hub-pipeline tests | ❌ OPEN — tracked in MESSAGEHUB_TODO |
| F6 — sender_uid renaming | ✅ verified compatible (no fix needed) |

All audit findings except F5 (deferred integration tests) are resolved.
F5 is independent ongoing work tracked in
`docs/todo/MESSAGEHUB_TODO.md` "System-level L4 tests"; per-test slices
land separately.
