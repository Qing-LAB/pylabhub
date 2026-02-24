# pyLabHub Directory Layout and Packaging Design

**Purpose:** Authoritative reference for all on-disk directory structures in pyLabHub.
Covers the installed binary tree, hub instance directories, and actor instance directories.

**Related:** `docs/todo/SECURITY_TODO.md` (identity and directory model design),
`docs/README/README_CMake_Design.md` (build/staging system),
`docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md` (lifecycle model)

---

## Overview: Two Kinds of Directories

pyLabHub uses **two distinct directory types** that serve different purposes:

| Type | Purpose | Owned by |
|---|---|---|
| **Install tree** | Binaries, libraries, headers, shared scripts | Build system (`cmake --install`) |
| **Hub instance directory** | One hub's identity, secrets, config, logs | `pylabhub-hubshell --init` |
| **Actor instance directory** | One actor's identity, config, script | `pylabhub-actor --init` |

The install tree is read-only at runtime and shared across all hubs running on the
same machine. Hub and actor directories are mutable, per-instance, and created at
init time.

---

## 1. Install Tree Layout

This is what `cmake --install` (or `cmake --build --target stage_all`) produces.
The root is `${CMAKE_INSTALL_PREFIX}` (install) or `${PYLABHUB_STAGING_DIR}` (staged).

```
<install-root>/
  bin/
    pylabhub-hubshell          ← hub process (broker + admin shell + lifecycle)
    pylabhub-actor             ← actor process (producer or consumer worker)
  lib/
    libpylabhub-utils-stable.so.M.m.p   ← shared library (POSIX versioned)
    libpylabhub-utils-stable.so.M       ← soname symlink
  include/
    utils/                     ← pylabhub-utils public headers
    pylabhub_utils_export.h    ← generated DLL-export macros
    pylabhub_version.h         ← version constants
    plh_platform.hpp           ← Layer 0 umbrella header
    plh_base.hpp               ← Layer 1 umbrella header
    plh_service.hpp            ← Layer 2 umbrella header
    plh_datahub.hpp            ← Layer 3 umbrella header
    fmt/                       ← fmt library headers
    nlohmann/                  ← JSON library headers
    sodium.h / sodium/         ← libsodium headers
    cppzmq/                    ← ZMQ C++ bindings
  config/
    hub.default.json           ← compiled-in hub defaults (always overwritten on stage)
    hub.user.json              ← user overrides (non-destructive: only created if absent)
  share/
    scripts/
      python/
        examples/              ← Python SDK examples
        pylabhub_sdk/          ← embedded Python module (pybind11)
        requirements.txt       ← Python package requirements
      lua/                     ← Lua scripts (future)
  opt/
    python/                    ← python-build-standalone 3.14 runtime
    IgorXOP/                   ← Igor Pro XOP bindings (optional)
    lua/                       ← LuaJIT runtime
  data/                        ← default run-data directory (writable; created at install)
  docs/                        ← deployed documentation
  tools/                       ← developer tools (clang-format, etc.)
  package/                     ← CMake package config for find_package()
  tests/
    test_layer0_*              ← test executables (only in development builds)
    test_layer1_*
    test_layer2_*
    test_layer3_*
    test_layer4_*
```

### Windows differences

On Windows, `.dll` files go to `bin/` (next to executables) and are also copied to
`tests/` so test executables find them at runtime.

### RPATH (Linux/macOS)

All executables in `bin/` are built with `RPATH=$ORIGIN/../lib` so they find the
shared library without `LD_LIBRARY_PATH`. Set via `CMAKE_BUILD_WITH_INSTALL_RPATH=ON`
in the top-level `CMakeLists.txt`.

---

## 2. Hub Instance Directory

Each hub has a **persistent identity directory**. The directory *is* the hub —
moving it clones the identity; creating a new one with `--init` gives a fresh hub.

Created by: `pylabhub-hubshell --init <hub_dir>`
Started by: `pylabhub-hubshell <hub_dir>`

```
<hub_dir>/
  hub.json          ← public config: hub_name, hub_uid, broker_endpoint,
  |                     connection_policy, known_actors, channel_policies
  hub.vault         ← encrypted secrets (binary: [nonce(24)][ciphertext+MAC])
  |                     decrypted payload: broker CurveZMQ keypair + admin token
  hub.pubkey        ← broker CurveZMQ public key, 40-char Z85, mode 0644
  |                     written at every startup; safe to distribute to actors
  startup.py        ← optional Python startup script (path in hub.json)
  logs/             ← log files (rotated; created at startup)
  run/
    hub.pid         ← PID of running process
    admin.sock      ← admin ZMQ REP socket (or admin.ipc)
```

### hub.json schema (key fields)

```json
{
  "hub_name": "lab.physics.daq1",
  "hub_uid": "b3f2a1c7-1234-4xxx-yxxx-xxxxxxxxxxxx",
  "broker_endpoint": "tcp://0.0.0.0:5570",
  "admin_endpoint":  "ipc:///path/to/run/admin.sock",
  "connection_policy": "open",
  "channel_policies": {
    "lab.daq.raw.*": { "connection_policy": "verified" }
  },
  "known_actors": [
    { "name": "lab.daq.sensor1", "uid": "e7a9...", "role": "producer" }
  ],
  "paths": {
    "scripts_python": "<install-root>/share/scripts/python",
    "startup_script": "startup.py"
  },
  "broker": {
    "channel_timeout_s": 10,
    "consumer_liveness_check_s": 5
  }
}
```

### hub.vault format

Binary: `[nonce (24 bytes)][ciphertext (variable) + MAC (16 bytes)]`

Decrypted payload (JSON):
```json
{
  "broker": {
    "curve_secret_key": "<Z85 40-char>",
    "curve_public_key":  "<Z85 40-char>"
  },
  "admin": {
    "token": "<64-char hex>"
  }
}
```

Key derivation: Argon2id(password, salt=BLAKE2b-128(hub_uid),
OPSLIMIT_INTERACTIVE, MEMLIMIT_INTERACTIVE).

File permissions: `hub.vault` → 0600 (owner read/write only).
`hub.pubkey` → 0644 (world-readable; actors read this to configure CurveZMQ).
`hub.json` → 0644 (no secrets; contains only public config).

### Hub UID format

`HUB-{NAME}-{8HEX}` — generated by `uid_utils::generate_hub_uid(hub_name)`.
Stored in `hub.json["hub_uid"]`. Also used as input to the Argon2id KDF so the
vault is cryptographically bound to the hub identity.

---

## 3. Actor Instance Directory

Each actor has a **persistent identity directory**. Contains its config and Python
callback script.

Created by: `pylabhub-actor --init <actor_dir>`
Started by: `pylabhub-actor <actor_dir>`

```
<actor_dir>/
  actor.json        ← actor_name, actor_uid, role, channel, hub_dir,
  |                     slot_schema, flexzone_schema, validation policy
  script.py         ← Python callbacks (on_write / on_read / on_timeout / on_error)
  |                     path referenced from actor.json["roles"][*]["script"]
  logs/             ← log files (optional; created at startup)
  run/
    actor.pid       ← PID of running process
```

### actor.json schema (key fields)

```json
{
  "actor_name": "lab.daq.sensor1",
  "actor_uid":  "e7a9f3b2-xxxx-4xxx-yxxx-xxxxxxxxxxxx",
  "hub_dir":    "/opt/pylabhub/hubs/daq1",
  "roles": [
    {
      "name": "sensor1_producer",
      "type": "producer",
      "channel": "lab.daq.sensor1.raw",
      "broker": "<resolved from hub_dir/hub.json at startup>",
      "broker_pubkey": "<read from hub_dir/hub.pubkey at startup>",
      "interval_ms": 100,
      "slot_schema": { ... },
      "flexzone_schema": { ... },
      "script": "script.py"
    }
  ]
}
```

`hub_dir` is the only external reference an actor needs:
- `<hub_dir>/hub.json` → `broker_endpoint`
- `<hub_dir>/hub.pubkey` → broker CurveZMQ public key for the Messenger CURVE connection

### Actor UID format

`ACTOR-{NAME}-{8HEX}` — generated by `uid_utils::generate_actor_uid(actor_name)`.
Stored in `actor.json["actor_uid"]`. Auto-generated at `--init`; reused across restarts.

---

## 4. How Instance Directories Are Created

### Hub directory — current state (flat-config model, no init)

There is no `--init` flow yet. The hub process (`pylabhub-hubshell`) looks for its
configuration in a `config/` directory relative to the binary:

```bash
# Current invocation — no hub_dir argument
pylabhub-hubshell          # reads config/ next to the binary
```

The config directory is created by CMake staging (`stage_hub_config` target). The user
edits `config/hub.user.json` to override defaults. The vault files (`hub.vault`,
`hub.pubkey`) do not exist yet.

### Hub directory — target state (Phase 1 remainder, not yet implemented)

```bash
# First-time setup
pylabhub-hubshell --init <hub_dir>
  # → prompts: hub_name (e.g. "lab.physics.daq1")
  # → prompts: master password (twice, confirm)
  # → generates hub_uid (UUID4 via generate_uuid4())
  # → writes hub.json with hub_uid, hub_name, defaults
  # → generates broker CurveZMQ keypair + admin token (via HubVault::create())
  # → writes hub.vault (Argon2id encrypted)
  # → creates logs/ and run/ subdirectories

# Subsequent runs
pylabhub-hubshell <hub_dir>
  # → reads hub_uid from hub.json
  # → prompts: master password (or reads PYLABHUB_MASTER_PASSWORD env var)
  # → decrypts hub.vault (HubVault::open())
  # → writes hub.pubkey (HubVault::publish_public_key())
  # → starts broker with stable keypair from vault

# Development mode (no vault, ephemeral keypair)
pylabhub-hubshell <hub_dir> --dev
```

The crypto primitives (`HubVault::create()`, `HubVault::open()`, `generate_uuid4()`)
are already implemented. What remains is wiring them in `hubshell.cpp` and adding
`--init` CLI argument handling. See `docs/todo/SECURITY_TODO.md §Phase 1`.

### Actor directory — current state (flat JSON file)

The actor process reads a JSON file at any path:

```bash
pylabhub-actor <path_to_actor.json>   # current: any path, no --init
```

The JSON file contains inline config for all roles. There are no standard `actor_dir`
subdirectories yet (`logs/`, `run/` do not exist).

### Actor directory — target state (Phase 2, not yet implemented)

```bash
# First-time setup
pylabhub-actor --init <actor_dir>
  # → prompts: actor_name, hub_dir, role, channel, schema
  # → generates actor_uid (UUID4 via generate_uuid4())
  # → writes actor.json
  # → creates logs/ and run/ subdirectories

# Run
pylabhub-actor <actor_dir>
  # → reads actor.json → resolves broker_endpoint from hub_dir/hub.json
  # → reads hub.pubkey from hub_dir/hub.pubkey → connects with CurveZMQ
  # → runs role workers

# Register with hub (for 'verified' policy)
pylabhub-actor --register-with <hub_dir> <actor_dir>
  # → appends actor_name + actor_uid to hub.json known_actors
```

### Producer and Consumer directories

pyLabHub does not have separate producer/consumer directories. Each actor is either
a producer or a consumer (or has multiple roles). The actor directory holds one
actor's config and script for all its roles.

The `hub_dir` reference in `actor.json` is the only link from actor to hub — the
actor reads `hub.pubkey` and `hub.json["broker_endpoint"]` from there.

---

## 5. Current State vs. Target State

### Current state (Phase 1 partially complete)

HubConfig uses the **legacy flat-config model**: it reads two JSON files from a
`config/` subdirectory relative to the binary:

```
<binary>/../config/hub.default.json   ← canonical defaults, always staged
<binary>/../config/hub.user.json      ← user overrides, non-destructive
```

This model works for development but does not support multiple hub instances,
hub vault encryption, or stable CurveZMQ keypairs (regenerated on each restart).

**Phase 1 items complete:** `hub_identity.hpp/cpp` (UUID4 generation + validation)
and `hub_vault.hpp/cpp` (`HubVault::create()` / `HubVault::open()` / `publish_public_key()`).

**Phase 1 items remaining:** `pylabhub-hubshell --init` CLI flow and `pylabhub-hubshell <hub_dir>`
directory-based startup. These wire the vault into `hubshell.cpp`.
See `docs/todo/SECURITY_TODO.md §Phase 1`.

### Target state (Phase 5)

`HubConfig::load_(hub_dir)` reads `<hub_dir>/hub.json` (with compiled-in defaults
as fallback). `hubshell.cpp` opens the vault and passes the stable keypair to
`BrokerService::Config`. The `config/hub.*.json` files in the install tree are
removed or kept only as a first-run bootstrap template.

See `docs/todo/SECURITY_TODO.md §Phase 5` for the transition steps.

---

## 6. CMake Staging Integration

### What is CMake-managed (not hardwired)

| Item | CMake variable / target |
|---|---|
| Staging root | `PYLABHUB_STAGING_DIR` = `${CMAKE_BINARY_DIR}/stage-${BUILD_TYPE}` |
| Binary directory | `${PYLABHUB_STAGING_DIR}/bin` |
| Library directory | `${PYLABHUB_STAGING_DIR}/lib` |
| Config directory | `${PYLABHUB_STAGING_DIR}/config` |
| Share directory | `${PYLABHUB_STAGING_DIR}/share/scripts/python` |
| Data directory | `${PYLABHUB_STAGING_DIR}/data` |
| Hub config files | `stage_hub_config` target (top-level `CMakeLists.txt`) |
| Python runtime | `prepare_python_env` target (`src/hub_python/CMakeLists.txt`) |
| Startup script path | `hub.default.json.in["python"]["startup_script"]` template variable |
| Script paths | `hub.default.json.in["paths"]` template variables |

All paths in `hub.default.json.in` use relative references (`../share/scripts/python`,
`../data`) so the install tree is relocatable.

### What needs attention (gap between current and target)

| Item | Current | Required change |
|---|---|---|
| Hub config path discovery | `<binary>/../config/` (hardwired relative) | `hub_dir` argument (Phase 5) |
| Hub vault creation | Not yet called from hubshell | `--init` flow (Phase 2) |
| CurveZMQ keypair stability | Regenerated on every restart | Read from vault (Phase 5) |
| Admin token source | `hub.user.json["admin"]["token"]` (plaintext) | Read from vault (Phase 5) |
| Actor config path | `actor.json` at any path | `<actor_dir>/actor.json` (Phase 2) |
| Hub pubkey distribution | Not yet written to disk | `HubVault::publish_public_key()` (Phase 2) |

### Hub and actor directories are NOT in the install tree

Hub and actor instance directories live **outside** the install tree — they are
created by `--init` at a user-chosen path, not by the build system. The install
tree provides the binaries and shared scripts; the instance directories hold
per-hub/per-actor identity and runtime state.

This is the standard Unix pattern: binaries in `/usr/local/bin/`, instance data
in `/opt/pylabhub/hubs/daq1/` or `~/.pylabhub/hubs/daq1/`.

### Directory creation at startup

Directories that don't need to exist before first run (logs/, run/) are created
by the process itself at startup, not by CMake. This matches the install tree —
the `data/` directory is created by `create_staging_dirs` as a placeholder but
is otherwise not managed by CMake.

---

## 7. Recommended Default Paths

| Context | Recommended location |
|---|---|
| System install (multi-user) | `/usr/local/lib/pylabhub/` (install tree) |
| Hub instance | `/opt/pylabhub/hubs/<hub_name>/` or `~/pylabhub/hubs/<hub_name>/` |
| Actor instance | `/opt/pylabhub/actors/<actor_name>/` or `~/pylabhub/actors/<actor_name>/` |
| Development (staged) | `cpp/build/stage-debug/` (install tree); `./hubs/`, `./actors/` (instances) |

No default instance paths are hardwired in the binary. The `<hub_dir>` and
`<actor_dir>` are always passed as CLI arguments or default to the current
directory (git-convention style: `pylabhub-hubshell` with no args → `./`).

---

## Related Documents

| Document | Topic |
|---|---|
| `docs/README/README_CMake_Design.md` | Build system architecture, staging functions reference |
| `docs/todo/SECURITY_TODO.md` | Hub vault, directory model, connection policy design and phases |
| `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` | Protocol flows, slot state machine |
| `docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md` | Lifecycle model, module startup order |
| `config/hub.default.json.in` | Template for staged hub defaults (current flat-config model) |
