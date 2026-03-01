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

All fields are nested under the `"hub"` key (except `admin`, `broker`, `paths`, `python`
which have their own top-level objects — see `README_Deployment.md §4.3` for the complete
field reference).

```json
{
  "hub": {
    "name":            "lab.physics.daq1",
    "uid":             "HUB-LABDAQ1-3A7F2B1C",
    "broker_endpoint": "tcp://0.0.0.0:5570",
    "admin_endpoint":  "tcp://127.0.0.1:5600",
    "connection_policy": "open",
    "channel_policies": [
      { "channel": "lab.daq.raw.*", "connection_policy": "verified" }
    ],
    "known_actors": [
      { "name": "lab.daq.sensor1", "uid": "ACTOR-SENSOR1-A1B2C3D4", "role": "producer" }
    ]
  },
  "paths": {
    "scripts_python": "../share/scripts/python",
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

Each actor has a **persistent identity directory**. Contains its config and per-role
Python script packages.

Created by: `pylabhub-actor --init <actor_dir>`
Started by: `pylabhub-actor <actor_dir>`

```
<actor_dir>/
  actor.json        ← actor_name, actor_uid, hub_dir, roles (each with script ref)
  roles/
    <role_name>/    ← one subdirectory per role (e.g. "data_out", "cfg_in")
      script/       ← Python package for this role
        __init__.py ← callbacks: on_init, on_iteration, on_stop
        helpers.py  ← optional submodule (from . import helpers)
  logs/             ← log files (optional; created at startup)
  run/
    actor.pid       ← PID of running process
```

Each role's script is a **Python package** — a directory named `script/` containing
`__init__.py`. The package is loaded with `importlib.util.spec_from_file_location`
using a role-unique `sys.modules` alias (`_plh_<uid_hex>_<role_name>`), ensuring
isolation between roles even when they share the same module name `"script"`.

Relative imports within the package work: `from . import helpers` in `__init__.py`
imports `roles/<role_name>/script/helpers.py`.

### actor.json schema (key fields)

```json
{
  "actor_name": "lab.daq.sensor1",
  "actor_uid":  "ACTOR-lab.daq.sensor1-A1B2C3D4",
  "hub_dir":    "/opt/pylabhub/hubs/daq1",
  "roles": [
    {
      "name":    "data_out",
      "kind":    "producer",
      "channel": "lab.daq.sensor1.raw",
      "interval_ms": 100,
      "slot_schema": { ... },
      "flexzone_schema": { ... },
      "script": {
        "module": "script",
        "path":   "./roles/data_out"
      }
    },
    {
      "name":    "cfg_in",
      "kind":    "consumer",
      "channel": "lab.daq.sensor1.config",
      "timeout_ms": 1000,
      "slot_schema": { ... },
      "script": {
        "module": "script",
        "path":   "./roles/cfg_in"
      }
    }
  ]
}
```

`hub_dir` is resolved at startup to read `hub.json` (broker endpoint) and `hub.pubkey`
(CurveZMQ public key). Role names (`"data_out"`, `"cfg_in"`) are user-defined semantic
identifiers; the role type (`"kind"`) is always separate.

See `docs/tech_draft/ACTOR_DESIGN.md §2–3` for the complete config and script
interface documentation.

`hub_dir` is the only external reference an actor needs:
- `<hub_dir>/hub.json` → `broker_endpoint`
- `<hub_dir>/hub.pubkey` → broker CurveZMQ public key for the Messenger CURVE connection

### Actor UID format

`ACTOR-{NAME}-{8HEX}` — generated by `uid_utils::generate_actor_uid(actor_name)`.
Stored in `actor.json["actor_uid"]`. Auto-generated at `--init`; reused across restarts.

---

## 4. How Instance Directories Are Created

### Hub directory — current state (directory model, complete as of 2026-02-26)

The `--init` flow and directory-based startup are fully implemented in `hubshell.cpp`:

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

# Development mode (no vault, ephemeral keypair, hub_dir optional)
pylabhub-hubshell <hub_dir> --dev
pylabhub-hubshell --dev          # built-in defaults only
```

The password is read from `PYLABHUB_MASTER_PASSWORD` env var when set; otherwise
`getpass()` prompts interactively (POSIX). The `--dev` mode skips the vault and uses
an ephemeral CurveZMQ keypair regenerated on each restart (suitable for development).

### Actor directory — current state (actor_dir model, complete)

The actor process accepts either a JSON file path (`--config`) or an actor directory
positional argument:

```bash
# Directory-based startup (standard)
pylabhub-actor <actor_dir>
  # → reads actor.json from the directory
  # → resolves broker_endpoint from hub_dir/hub.json
  # → reads hub.pubkey from hub_dir/hub.pubkey → connects with CurveZMQ
  # → loads each role's script package from roles/<role_name>/script/__init__.py
  # → runs role workers (loop_thread_ + zmq_thread_ per role)

# Init flow (creates a new actor directory)
pylabhub-actor --init <actor_dir>
  # → prompts: actor_name
  # → generates actor_uid (ACTOR-{name}-{8HEX})
  # → writes actor.json (with example data_out role)
  # → creates logs/, run/ subdirectories
  # → creates roles/data_out/script/__init__.py with template callbacks

```

```bash
# Register with hub (for 'verified' connection policy)
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

### Current state (all phases complete)

The hub directory model is fully implemented:

- `HubConfig` reads a single `<hub_dir>/hub.json`; no flat-config files in the install tree
- `pylabhub-hubshell <hub_dir>` is required (error without `<hub_dir>` unless `--dev`)
- `pylabhub-hubshell --dev` uses built-in compiled-in defaults and an ephemeral keypair
- `pylabhub-hubshell --init <hub_dir>` creates the hub instance directory

**Complete (2026-02-27):** `uuid_utils.hpp/cpp` (UUID4 via libsodium CSRNG),
`uid_utils.hpp` (pylabhub-format UIDs: `HUB-{NAME}-{8HEX}`, `ACTOR-{NAME}-{8HEX}`),
`hub_vault.hpp/cpp` (`HubVault::create()` / `HubVault::open()` / `publish_public_key()`),
`pylabhub-hubshell --init <hub_dir>` CLI, `pylabhub-hubshell <hub_dir>` directory startup,
`channel_access_policy.hpp` (ConnectionPolicy: Open/Tracked/Required/Verified enforcement),
actor identity fields wired into REG_REQ/CONSUMER_REG_REQ broker protocol,
single-file HubConfig (compiled-in defaults + hub.json; layered flat-config removed).

---

## 6. CMake Staging Integration

### What is CMake-managed (not hardwired)

| Item | CMake variable / target |
|---|---|
| Staging root | `PYLABHUB_STAGING_DIR` = `${CMAKE_BINARY_DIR}/stage-${BUILD_TYPE}` |
| Binary directory | `${PYLABHUB_STAGING_DIR}/bin` |
| Library directory | `${PYLABHUB_STAGING_DIR}/lib` |
| Share directory | `${PYLABHUB_STAGING_DIR}/share/scripts/python` |
| Data directory | `${PYLABHUB_STAGING_DIR}/data` |
| Python runtime | `prepare_python_env` target (`src/hub_python/CMakeLists.txt`) |

Hub config files (`hub.json`) are not staged by CMake — they are created by
`pylabhub-hubshell --init <hub_dir>` at a user-chosen instance directory.

### Completed implementation items

| Item | Status |
|---|---|
| Hub config path — `<hub_dir>` argument | ✅ complete (2026-02-26) |
| Hub vault creation (`--init`) | ✅ complete (2026-02-26) |
| CurveZMQ stable keypair from vault | ✅ complete (2026-02-26) |
| Admin token from vault | ✅ complete (2026-02-26) |
| HubConfig single-file load (`hub.json`) | ✅ complete (2026-02-27) |
| Actor config path (`<actor_dir>/actor.json`) | ✅ complete (2026-02-25) |
| Hub pubkey distribution (`hub.pubkey`) | ✅ complete (2026-02-26) |
| Actor registration (`--register-with`) | ✅ complete (2026-02-26) |
| Connection policy enforcement | ✅ complete (2026-02-26) |

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
`<actor_dir>` are always passed as CLI arguments. Running `pylabhub-hubshell`
without a `<hub_dir>` is an error unless `--dev` is passed.

---

## Related Documents

| Document | Topic |
|---|---|
| `docs/README/README_CMake_Design.md` | Build system architecture, staging functions reference |
| `docs/README/README_Deployment.md` | User-facing deployment guide (hub setup, actor setup, scripts) |
| `docs/todo/SECURITY_TODO.md` | Hub vault, directory model, connection policy design and phases |
| `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` | Protocol flows, slot state machine |
| `docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md` | Lifecycle model, module startup order |
