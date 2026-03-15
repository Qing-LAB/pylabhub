# pyLabHub Directory Layout and Packaging Design

**Purpose:** Authoritative reference for all on-disk directory structures in pyLabHub.
Covers the installed binary tree, hub instance directories, and role instance directories
(producer, consumer, processor).

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
| **Role instance directory** | One role's config, vault, script | `pylabhub-producer --init` / `pylabhub-consumer --init` / `pylabhub-processor --init` |

The install tree is read-only at runtime and shared across all hubs running on the
same machine. Hub and role directories are mutable, per-instance, and created at
init time.

---

## 1. Install Tree Layout

This is what `cmake --install` (or `cmake --build --target stage_all`) produces.
The root is `${CMAKE_INSTALL_PREFIX}` (install) or `${PYLABHUB_STAGING_DIR}` (staged).

```
<install-root>/
  bin/
    pylabhub-hubshell          ← hub process (broker + admin shell + lifecycle)
    pylabhub-producer          ← producer role binary
    pylabhub-consumer          ← consumer role binary
    pylabhub-processor         ← processor role binary
    pylabhub-pyenv             ← Python environment manager (bash wrapper)
    pylabhub-pyenv.ps1         ← Python environment manager (PowerShell, Windows)
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
    pylabhub.json              ← system config (python_home, etc.) — optional
  share/
    scripts/
      python/
        hubshell_client.py     ← AdminShell interactive client
        requirements.txt       ← Python package requirements
    py-examples/               ← standalone Python script examples
    py-demo-single-processor-shm/   ← SHM pipeline demo
    py-demo-dual-processor-bridge/  ← cross-hub bridge demo
  opt/
    python/                    ← python-build-standalone runtime
      venvs/                   ← user virtual environments
    IgorXOP/                   ← Igor Pro XOP bindings (optional)
    luajit/                    ← LuaJIT runtime
  docs/                        ← deployed documentation
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
      { "name": "lab.daq.sensor1", "uid": "PROD-SENSOR1-A1B2C3D4", "role": "producer" },
      { "name": "lab.daq.logger",  "uid": "CONS-LOGGER-B5C6D7E8",  "role": "consumer" }
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

## 3. Role Instance Directories

Each role (producer, consumer, or processor) has a **persistent instance directory**.
The directory holds the role's config, vault (for credentials), script, logs, and
runtime state. The directory *is* the role — moving it preserves all state; creating a
new one with `--init` gives a fresh identity.

### Producer instance directory

Created by: `pylabhub-producer --init <producer_dir>`
Started by: `pylabhub-producer <producer_dir>`

```
<producer_dir>/
  producer.json     ← role config: uid, name, broker, channel, transport, schema, script
  script/
    python/
      __init__.py   ← callbacks: on_init, on_produce, on_stop
  logs/             ← log files (created at startup)
  run/
    producer.pid    ← PID of running process
```

### Consumer instance directory

Created by: `pylabhub-consumer --init <consumer_dir>`
Started by: `pylabhub-consumer <consumer_dir>`

```
<consumer_dir>/
  consumer.json     ← role config: uid, name, broker, channel, transport, schema, script
  script/
    python/
      __init__.py   ← callbacks: on_init, on_consume, on_stop
  logs/             ← log files (created at startup)
  run/
    consumer.pid    ← PID of running process
```

### Processor instance directory

Created by: `pylabhub-processor --init <processor_dir>`
Started by: `pylabhub-processor <processor_dir>`

```
<processor_dir>/
  processor.json    ← role config: uid, name, broker, channels, transport, schema, script
  script/
    python/
      __init__.py   ← callbacks: on_init, on_process, on_stop
  logs/             ← log files (created at startup)
  run/
    processor.pid   ← PID of running process
```

### Role config key fields

All three role configs share the same structural pattern. Example `producer.json`:

```json
{
  "producer": {
    "uid":       "PROD-SENSOR1-3A7F2B1C",
    "name":      "Sensor1",
    "log_level": "info"
  },
  "broker":          "tcp://127.0.0.1:5570",
  "broker_pubkey":   "<Z85 40-char>",
  "channel":         "lab.daq.sensor1.raw",
  "target_period_ms": 100,
  "shm": {
    "enabled":    true,
    "secret":     1234567890,
    "slot_count": 8
  },
  "slot_schema": [
    { "name": "ts",    "type": "float64", "count": 1 },
    { "name": "value", "type": "float32", "count": 1 }
  ],
  "script": { "type": "python", "path": "." }
}
```

The `script.path` field is resolved relative to the role directory. `"path": "."` means
`<role_dir>/script/python/__init__.py`. The hub's CurveZMQ public key is read from
`broker_pubkey` in the config (or from `<hub_dir>/hub.pubkey` if a hub directory
reference is used).

### Role UID formats

| Role | Format | Example |
|------|--------|---------|
| Producer | `PROD-{NAME}-{8HEX}` | `PROD-SENSOR1-3A7F2B1C` |
| Consumer | `CONS-{NAME}-{8HEX}` | `CONS-LOGGER-9E1D4C2A` |
| Processor | `PROC-{NAME}-{8HEX}` | `PROC-SCALER-B3F12E9D` |

UIDs are auto-generated at `--init` and reused across restarts. They are what the
broker records in its channel registry and what `known_actors` in `hub.json` must
reference for `verified` connection policy enforcement.

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

### Role directories — current state (fully implemented)

Each role binary has a symmetric `--init` flow and directory-based startup:

```bash
# First-time setup (producer example; same pattern for consumer/processor)
pylabhub-producer --init <producer_dir>
  # → prompts: role name (e.g. "Sensor1")
  # → prompts: master password (twice, confirm) — written to producer.vault
  # → generates producer_uid (PROD-{NAME}-{8HEX})
  # → writes producer.json with uid, name, defaults
  # → creates script/python/__init__.py with template callbacks
  # → creates logs/, run/ subdirectories

# Subsequent runs
pylabhub-producer <producer_dir>
  # → reads producer.json (broker endpoint, channel, schema, transport)
  # → decrypts producer.vault (password prompt or PYLABHUB_MASTER_PASSWORD env var)
  # → connects to broker with CurveZMQ using hub.pubkey from broker_pubkey field
  # → runs producer loop: on_init → on_produce (per period) → on_stop
```

```bash
# Same pattern for consumer:
pylabhub-consumer --init <consumer_dir>
pylabhub-consumer <consumer_dir>

# Same pattern for processor:
pylabhub-processor --init <processor_dir>
pylabhub-processor <processor_dir>
```

The broker's CurveZMQ public key is embedded in the role config as `broker_pubkey`
(a Z85 40-char string). This is copied from `<hub_dir>/hub.pubkey` at setup time.
There is no runtime `hub_dir` reference — the role config is self-contained.

---

## 5. Current State vs. Target State

### Current state (all phases complete)

The hub directory model is fully implemented:

- `HubConfig` reads a single `<hub_dir>/hub.json`; no flat-config files in the install tree
- `pylabhub-hubshell <hub_dir>` is required (error without `<hub_dir>` unless `--dev`)
- `pylabhub-hubshell --dev` uses built-in compiled-in defaults and an ephemeral keypair
- `pylabhub-hubshell --init <hub_dir>` creates the hub instance directory

**Complete (2026-02-27 – 2026-03-10):** `uuid_utils.hpp/cpp` (UUID4 via libsodium CSRNG),
`uid_utils.hpp` (pylabhub-format UIDs: `HUB-{NAME}-{8HEX}`, `PROD-{NAME}-{8HEX}`,
`CONS-{NAME}-{8HEX}`, `PROC-{NAME}-{8HEX}`),
`hub_vault.hpp/cpp` (`HubVault::create()` / `HubVault::open()` / `publish_public_key()`),
`pylabhub-hubshell --init <hub_dir>` CLI, `pylabhub-hubshell <hub_dir>` directory startup,
`channel_access_policy.hpp` (ConnectionPolicy: Open/Tracked/Required/Verified enforcement),
role identity fields wired into REG_REQ/CONSUMER_REG_REQ broker protocol,
single-file HubConfig (compiled-in defaults + hub.json; layered flat-config removed),
producer/consumer/processor `--init` flows with correct config templates.

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
| Producer config path (`<producer_dir>/producer.json`) | ✅ complete (2026-03-08) |
| Consumer config path (`<consumer_dir>/consumer.json`) | ✅ complete (2026-03-10) |
| Processor config path (`<processor_dir>/processor.json`) | ✅ complete (2026-03-10) |
| Hub pubkey distribution (`hub.pubkey`) | ✅ complete (2026-02-26) |
| Connection policy enforcement | ✅ complete (2026-02-26) |

### Hub and role directories are NOT in the install tree

Hub and role instance directories live **outside** the install tree — they are
created by `--init` at a user-chosen path, not by the build system. The install
tree provides the binaries and shared scripts; the instance directories hold
per-hub/per-role config and runtime state.

This is the standard Unix pattern: binaries in `/usr/local/bin/`, instance data
in `/opt/pylabhub/hubs/daq1/` or `~/.pylabhub/producers/sensor1/`.

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
| Producer instance | `/opt/pylabhub/producers/<name>/` or `~/pylabhub/producers/<name>/` |
| Consumer instance | `/opt/pylabhub/consumers/<name>/` or `~/pylabhub/consumers/<name>/` |
| Processor instance | `/opt/pylabhub/processors/<name>/` or `~/pylabhub/processors/<name>/` |
| Development (staged) | `build/stage-debug/` (install tree); `./hubs/`, `./roles/` (instances) |

No default instance paths are hardwired in any binary. The `<role_dir>` is always
passed as a CLI argument. Running `pylabhub-hubshell` without a `<hub_dir>` is an
error unless `--dev` is passed.

---

## Related Documents

| Document | Topic |
|---|---|
| `docs/README/README_CMake_Design.md` | Build system architecture, staging functions reference |
| `docs/README/README_Deployment.md` | User-facing deployment guide (hub setup, actor setup, scripts) |
| `docs/todo/SECURITY_TODO.md` | Hub vault, directory model, connection policy design and phases |
| `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` | Protocol flows, slot state machine |
| `docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md` | Lifecycle model, module startup order |
