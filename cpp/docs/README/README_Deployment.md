# pyLabHub Deployment Guide

**Purpose:** End-to-end reference for deploying pyLabHub from a fresh build to a running
hub-and-actor system. Covers the install tree, hub and actor instance directories, every
configuration field, Python script authoring, connection policy, and operational patterns.

**Related:**
- `docs/README/README_DirectoryLayout.md` — architectural directory model reference
- `docs/tech_draft/ACTOR_DESIGN.md` — full actor design spec (§2 config, §3 Python API)
- `docs/todo/SECURITY_TODO.md` — connection policy and vault design
- `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` — broker protocol

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
3. [Install Tree](#3-install-tree)
4. [Hub Setup](#4-hub-setup)
   - 4.1 [Hub directory model](#41-hub-directory-model)
   - 4.2 [Initialising a hub](#42-initialising-a-hub)
   - 4.3 [hub.json — complete field reference](#43-hubjson--complete-field-reference)
   - 4.4 [Running the hub](#44-running-the-hub)
   - 4.5 [Hub script package](#45-hub-script-package)
   - 4.6 [Connection policy](#46-connection-policy)
5. [Actor Setup](#5-actor-setup)
   - 5.1 [Actor directory model](#51-actor-directory-model)
   - 5.2 [Initialising an actor](#52-initialising-an-actor)
   - 5.3 [actor.json — complete field reference](#53-actorjson--complete-field-reference)
   - 5.4 [Running an actor](#54-running-an-actor)
6. [Writing Actor Scripts](#6-writing-actor-scripts)
   - 6.1 [Script package structure](#61-script-package-structure)
   - 6.2 [Callback reference](#62-callback-reference)
   - 6.3 [Slot types — ctypes field mapping](#63-slot-types--ctypes-field-mapping)
   - 6.4 [ActorRoleAPI methods](#64-actorroleapi-methods)
   - 6.5 [Flexzone — persistent shared data](#65-flexzone--persistent-shared-data)
   - 6.6 [ZMQ messaging between actors](#66-zmq-messaging-between-actors)
   - 6.7 [Error handling patterns](#67-error-handling-patterns)
7. [Connection Policy — Securing the Hub](#7-connection-policy--securing-the-hub)
8. [Common Topologies](#8-common-topologies)
9. [Operational Patterns](#9-operational-patterns)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Overview

pyLabHub is an IPC framework for scientific data acquisition. The deployment model has three
independent components:

```
[Install tree]          shared binaries, library, Python runtime, scripts
     |
     ├── pylabhub-hubshell ──→ [Hub instance dir]   identity, vault, config, logs
     |                         hub.json / hub.vault / hub.pubkey
     |
     └── pylabhub-actor ────→ [Actor instance dir]  identity, config, scripts, logs
                               actor.json / roles/<role>/script/__init__.py
```

**Install tree** — the binary/library/runtime output of `cmake --build`. Read-only at
runtime. Shared across all hubs and actors on the same machine.

**Hub instance directory** — one per running hub. Contains the hub's identity (UUID4),
encrypted secrets (CurveZMQ keypair), public config (broker endpoint, policy), and logs.
Created once with `--init`; reused across restarts.

**Actor instance directory** — one per running actor. Contains the actor's identity, role
configs, and Python script packages. Created once with `--init`; reused across restarts.

---

## 2. Quick Start

From a fresh build to a running hub + producer actor:

```bash
# Step 1 — Build and stage
cmake -S . -B build
cmake --build build --target stage_all

# Step 2 — Init hub (prompts for name + password)
./build/stage-debug/bin/pylabhub-hubshell --init ./my-hub
# → Hub name: lab.physics.daq1
# → Master password: ****  (saved in hub.vault; derives the broker keypair)

# Step 3 — Init actor (prompts for actor name)
./build/stage-debug/bin/pylabhub-actor --init ./my-actor
# → Actor name: lab.daq.sensor1
# → Edit actor.json: set "hub_dir" to the hub path

# Step 4 — Start hub (in one terminal)
./build/stage-debug/bin/pylabhub-hubshell ./my-hub
# → prompts for master password, starts broker on tcp://0.0.0.0:5570

# Step 5 — Start actor (in another terminal)
./build/stage-debug/bin/pylabhub-actor ./my-actor
# → connects to hub, starts writing data
```

For development without a vault (no password):

```bash
./build/stage-debug/bin/pylabhub-hubshell ./my-hub --dev
# → ephemeral keypair; hub.pubkey still written for actor use
```

Environment variable alternative to interactive password prompt:

```bash
export PYLABHUB_MASTER_PASSWORD=my-secret-password
./build/stage-debug/bin/pylabhub-hubshell ./my-hub
```

---

## 3. Install Tree

`cmake --build build --target stage_all` produces the staged tree under
`build/stage-<buildtype>/`. In production, `cmake --install build` installs to
`CMAKE_INSTALL_PREFIX` (default `/usr/local`).

```
<install-root>/
  bin/
    pylabhub-hubshell          ← hub process
    pylabhub-actor             ← actor process
  lib/
    libpylabhub-utils-stable.so.M.m.p   ← shared library (POSIX)
    libpylabhub-utils-stable.so.M       ← soname symlink
  include/
    utils/                     ← public C/C++ headers
    plh_platform.hpp / plh_base.hpp / plh_service.hpp / plh_datahub.hpp
    pylabhub_utils_export.h    ← DLL export macros
    pylabhub_version.h         ← version constants
  share/
    scripts/python/
      examples/                ← example actor scripts
      pylabhub_sdk/            ← pybind11 Python module (imported as pylabhub_actor)
      requirements.txt         ← Python package dependencies
  opt/
    python/                    ← Python 3.14 standalone runtime
  data/                        ← default data output directory
```

**`hub_dir` is required** unless `--dev` is passed. Running `pylabhub-hubshell`
without a `<hub_dir>` is an error. Use `--dev` for quick local testing without
a vault (ephemeral keypair, built-in defaults).

**RPATH:** all binaries in `bin/` embed `$ORIGIN/../lib` (Linux) or `@loader_path/../lib`
(macOS) so they find the shared library without `LD_LIBRARY_PATH`.

---

## 4. Hub Setup

### 4.1 Hub directory model

Each hub is a **directory**. The directory is the identity — its contents persist across
restarts and define the hub uniquely. Moving the directory moves the hub. `--init` on a
new directory creates a fresh hub.

```
<hub_dir>/
  hub.json                ← public config (broker endpoint, connection policy, etc.)
  hub.vault               ← encrypted secrets (binary; 0600 permissions)
  hub.pubkey              ← broker CurveZMQ public key, Z85 40-char (0644; world-readable)
  script/
    python/
      __init__.py         ← hub script package (callbacks: on_start, on_tick, on_stop)
  logs/                   ← rotating log files (created at startup if absent)
  run/
    hub.pid               ← PID of the running hub process
```

Files created by `--init`: `hub.json`, `hub.vault`, `logs/`, `run/`, `script/python/__init__.py` (template).
Files created at every startup: `hub.pubkey` (overwritten from vault on each start).

### 4.2 Initialising a hub

```bash
pylabhub-hubshell --init <hub_dir>
```

If `<hub_dir>` is omitted, the current directory is used.

The init flow:
1. Creates `<hub_dir>/logs/`, `<hub_dir>/run/`, `<hub_dir>/script/python/`
2. Prompts for **hub name** (reverse-domain format, e.g. `lab.physics.daq1`)
3. Prompts for **master password** (twice, for confirmation)
   - Or reads `PYLABHUB_MASTER_PASSWORD` env var (no prompt)
4. Generates a UUID4 for the hub identity (`hub_uid`)
5. Creates an encrypted vault: `hub.vault` (0600)
   - Generates a stable CurveZMQ broker keypair (written to vault)
   - Generates a random 64-hex admin token (written to vault)
6. Writes `hub.json` with hub_name, hub_uid, and defaults
7. Writes `script/python/__init__.py` with a starter template (callbacks: on_start, on_tick, on_stop)

Re-initialising is refused if `hub.json` already exists. To reset: remove `hub.json`
and `hub.vault` manually.

**Hub UID format:** `HUB-{NAME}-{8HEX}` (e.g. `HUB-LABDAQ1-3A7F2B1C`)
Auto-generated from `hub_name` at init time; stored in `hub.json["hub"]["uid"]`.

**Vault security:** the vault uses Argon2id key derivation
(`key = Argon2id(password, salt=BLAKE2b-128(hub_uid), OPSLIMIT_INTERACTIVE,
MEMLIMIT_INTERACTIVE)`) with XSalsa20-Poly1305 encryption. A wrong password
produces an authentication failure, not decryption of garbage.

### 4.3 hub.json — complete field reference

`hub.json` contains **no secrets** — only public config. The master password is never
stored here. Edit this file directly to change configuration.

```json
{
  "hub": {
    "name":            "lab.physics.daq1",
    "uid":             "HUB-LABDAQ1-3A7F2B1C",
    "description":     "Main data-acquisition hub for physics lab",
    "broker_endpoint": "tcp://0.0.0.0:5570",
    "admin_endpoint":  "tcp://127.0.0.1:5600",
    "connection_policy": "open",
    "known_actors": [],
    "channel_policies": []
  },
  "admin": {
    "token": ""
  },
  "broker": {
    "channel_timeout_s":         10,
    "consumer_liveness_check_s":  5
  },
  "paths": {
    "scripts_python": "../share/scripts/python",
    "scripts_lua":    "../share/scripts/lua",
    "data_dir":       "../data"
  },
  "script": {
    "type":                   "python",
    "path":                   "./script",
    "tick_interval_ms":       1000,
    "health_log_interval_ms": 60000
  },
  "python": {
    "requirements": "../share/scripts/python/requirements.txt"
  }
}
```

#### `hub` block

| Field | Default | Description |
|---|---|---|
| `name` | `"local.hub.default"` | Reverse-domain hub name. Used as base for auto-generated UID and for display. |
| `uid` | auto-generated | `HUB-{NAME}-{8HEX}` identifier. Stable across restarts. Do not change after `--init` — vault uses it as KDF salt. |
| `description` | `"pyLabHub instance"` | Free-text human description. Not parsed by any runtime code. |
| `broker_endpoint` | `"tcp://0.0.0.0:5570"` | ZMQ endpoint for the BrokerService. Actors connect here to register channels. Must be reachable from all actor machines. |
| `admin_endpoint` | `"tcp://127.0.0.1:5600"` | ZMQ REP endpoint for the AdminShell. Local only; bound to loopback. |
| `connection_policy` | `"open"` | Hub-wide channel access policy. Values: `"open"`, `"tracked"`, `"required"`, `"verified"`. See §7. |
| `known_actors` | `[]` | Allowlist for `"verified"` policy. Array of `{"name":…, "uid":…, "role":…}` entries. |
| `channel_policies` | `[]` | Per-channel policy overrides (first matching glob wins). Array of `{"channel": "glob*", "connection_policy": "…"}`. |

#### `admin` block

| Field | Default | Description |
|---|---|---|
| `token` | `""` | Admin shell authentication token (hex string). Empty = no auth. In production, set from vault via hubshell startup. |

#### `broker` block

| Field | Default | Description |
|---|---|---|
| `channel_timeout_s` | `10` | Seconds without a heartbeat before the broker marks a channel as lost. Increase for slow producers. |
| `consumer_liveness_check_s` | `5` | How often the broker checks consumer heartbeats. `0` = disabled. |

#### `paths` block

All paths may be absolute or relative to `hub.json`'s parent directory.

| Field | Default | Description |
|---|---|---|
| `scripts_python` | `"../share/scripts/python"` | Directory where Python scripts and the built-in `pylabhub_sdk/` module live. |
| `scripts_lua` | `"../share/scripts/lua"` | Lua scripts directory (future). |
| `data_dir` | `"../data"` | Default run-data output directory. |

#### `script` block (language-neutral hub script configuration)

| Field | Default | Description |
|---|---|---|
| `type` | `"python"` | Script host type. `"python"` selects the Python HubScript host (only type currently supported). |
| `path` | `"./script"` | Base directory for script packages. The actual package is at `<path>/<type>/`, e.g. `./script/python/`. Relative to `hub.json` directory. See §4.5. |
| `tick_interval_ms` | `1000` | Tick period in milliseconds. The C++ tick thread calls `on_tick(api, tick)` at this interval. |
| `health_log_interval_ms` | `60000` | How often the C++ tick runner logs a channel health summary (milliseconds). |

#### `python` block (Python-specific settings)

| Field | Default | Description |
|---|---|---|
| `requirements` | `"../share/scripts/python/requirements.txt"` | `pip install -r` requirements file used when `prepare_python_env` runs. |

### 4.4 Running the hub

```bash
# Production: vault unlocks stable broker keypair
pylabhub-hubshell <hub_dir>
# → reads hub_uid from hub.json
# → prompts for master password (or reads PYLABHUB_MASTER_PASSWORD)
# → decrypts hub.vault → writes hub.pubkey → starts broker

# Development: ephemeral keypair, no vault
pylabhub-hubshell <hub_dir> --dev

# Error: hub_dir is required without --dev
# pylabhub-hubshell  ← returns error; use --dev for ephemeral testing
```

**Shutdown:** send `SIGINT` (Ctrl-C) once to request graceful shutdown. A second
`SIGINT` while shutdown is in progress forces immediate exit (`std::_Exit(1)`).

**Environment overrides** (applied after config file):

| Variable | Overrides |
|---|---|
| `PYLABHUB_MASTER_PASSWORD` | Master password (skips interactive prompt) |
| `PYLABHUB_HUB_NAME` | `hub.name` |
| `PYLABHUB_BROKER_ENDPOINT` | `hub.broker_endpoint` |
| `PYLABHUB_ADMIN_ENDPOINT` | `hub.admin_endpoint` |
| `PYLABHUB_CONFIG_FILE` | Full path to a single config JSON (bypasses all layering) |

### 4.5 Hub script package

The hub loads a Python **package** from `hub.json["script"]["path"]/<type>/` (default
`./script/python/`). `type` selects the script host (`"python"` is the only supported value).
The package must contain `__init__.py` with up to three optional callbacks:

```
<hub_dir>/script/python/__init__.py   ← loaded by HubScript on startup
```

`--init` writes the default template automatically. The default template renders a
**live in-place channel dashboard** using `rich` (updated every tick):

```
MyHub  │  uptime 00:01:23  │  tick #83  │  2 ready  0 pending  0 closing
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 Channel               Status    Consumers  PID     Actor          Actor UID
 sensors.temperature   Ready     3          12045   lab-actor-01   ACTOR-LAB-ACTOR-01-A1B2C3D4
 events.triggers       Ready     1          12045   lab-actor-01   ACTOR-LAB-ACTOR-01-A1B2C3D4
```

`rich>=13.0` is included in the default `requirements.txt` and is installed by
`cmake --build build --target prepare_python_env`.

When running with a hub directory, the hub automatically switches from console
logging to a **rotating log file** at `<hub_dir>/logs/hub.log` (10 MB × 3 files)
as early as possible in startup.  The default script reads the tail of this file
each tick and displays it in the log panel — giving a self-contained terminal
dashboard with no interleaved console output.

#### Callback signatures

```python
def on_start(api):
    """Called once after the hub lifecycle is fully started."""

def on_tick(api, tick):
    """Called every tick_interval_ms (default: 1 s)."""

def on_stop(api):
    """Called once before the hub shuts down (after tick thread stops)."""
```

All callbacks are **optional** — missing callbacks are silently skipped.

#### Thread model

```
main thread      → startup → wait(shutdown) → teardown
broker_thread_   → BrokerService ZMQ poll loop
admin_shell_wt   → AdminShell REP socket poll
tick_thread_     → periodic tick loop: health log + on_tick callback
```

The tick thread acquires the GIL only for Python calls; the rest of each tick
(broker query, health logging) runs without the GIL and does not block the admin shell.

#### `HubScriptAPI` methods (`api` argument)

| Method | Returns | Description |
|---|---|---|
| `hub_name()` | `str` | Hub name in reverse-domain format |
| `hub_uid()` | `str` | Stable hub UID (`HUB-NAME-HEXSUFFIX`) |
| `log(level, msg)` | — | Log at `"debug"` / `"info"` / `"warn"` / `"error"` |
| `shutdown()` | — | Request graceful hub shutdown |
| `channels()` | `list[ChannelInfo]` | All channels |
| `ready_channels()` | `list[ChannelInfo]` | Ready channels only |
| `pending_channels()` | `list[ChannelInfo]` | PendingReady channels only |
| `channel(name)` | `ChannelInfo` | Look up by name (raises `RuntimeError` if not found) |

#### `ChannelInfo` attributes

| Method | Returns | Description |
|---|---|---|
| `name()` | `str` | Channel name |
| `status()` | `str` | `"Ready"` \| `"PendingReady"` \| `"Closing"` |
| `consumer_count()` | `int` | Number of registered consumers |
| `producer_pid()` | `int` | PID of the producer (0 if not set) |
| `schema_hash()` | `str` | 64-char hex hash, or `""` if no schema |
| `producer_actor_name()` | `str` | Actor name (empty if not set) |
| `producer_actor_uid()` | `str` | Actor UID (empty if not set) |
| `request_close()` | — | Mark channel for close; C++ dispatches after `on_tick` returns |

#### `HubTickInfo` attributes (`tick` argument)

| Method | Returns | Description |
|---|---|---|
| `tick_count()` | `int` | Ticks since hub start |
| `elapsed_ms()` | `int` | Actual wall-clock ms since last tick |
| `uptime_ms()` | `int` | Hub uptime in ms |
| `channels_ready()` | `int` | Count of Ready channels |
| `channels_pending()` | `int` | Count of PendingReady channels |
| `channels_closing()` | `int` | Count of Closing channels |

#### Example — stale channel cleanup

```python
def on_tick(api, tick):
    for ch in api.ready_channels():
        if ch.consumer_count() == 0 and tick.tick_count() > 60:
            api.log("warn", f"Closing idle channel '{ch.name()}' (no consumers)")
            ch.request_close()
```

### 4.6 Admin shell — hub management interface

The hub exposes a **ZMQ REP admin shell** on `admin_endpoint` (default
`tcp://127.0.0.1:5600`, loopback-only). It executes arbitrary Python code in
the hub's persistent interpreter and returns captured output. The `pylabhub`
module is always available.

#### hubshell_client.py — interactive client

`share/scripts/python/hubshell_client.py` is the standard client. It requires
`pyzmq` (`pip install pyzmq`).

**Interactive REPL** (most common usage):
```bash
python3 hubshell_client.py
# Optional flags:
#   --endpoint tcp://127.0.0.1:5600   (default)
#   --token    <hex-token>             (if hub configured with admin token)
```

At the `>>> ` prompt, type Python expressions or use built-in shortcuts:

| Shortcut | Action |
|---|---|
| `:channels` | Print formatted channel table (name, consumers, producer PID, status) |
| `:config` | Pretty-print hub config as JSON |
| `:help` | Show help |
| `:quit` / `:q` / Ctrl-D | Exit client (hub keeps running) |

Multi-line code: indent continuation lines; a blank line or un-indented line
sends the block.

**CLI one-liners** (scripting and automation):
```bash
# Print hub name
python3 hubshell_client.py --exec "print(pylabhub.config()['name'])"

# List channels with consumer counts
python3 hubshell_client.py --exec "
for ch in pylabhub.channels('all'):
    print(f\"{ch['name']:40s} consumers={ch['consumer_count']} status={ch['status']}\")
"

# Dump full config as JSON
python3 hubshell_client.py --exec "import json; print(json.dumps(pylabhub.config(), indent=2))"

# Execute a script file inside the hub
python3 hubshell_client.py --file my_admin_task.py

# Graceful hub shutdown
python3 hubshell_client.py --exec "pylabhub.shutdown()"
```

Exit code: 0 on success, 1 if the code raised an exception.

#### pylabhub module API

All functions are callable from the admin shell.

| Function | Returns | Description |
|---|---|---|
| `hub_name()` | `str` | Hub name in reverse-domain format |
| `hub_uid()` | `str` | Stable hub UID (`HUB-NAME-HEXSUFFIX`) |
| `hub_description()` | `str` | Human-readable description |
| `broker_endpoint()` | `str` | ZMQ broker endpoint (e.g. `tcp://0.0.0.0:5570`) |
| `admin_endpoint()` | `str` | Admin shell endpoint (e.g. `tcp://127.0.0.1:5600`) |
| `config()` | `dict` | Active config: `hub.{name, description, broker_endpoint, admin_endpoint, channel_timeout_s, consumer_liveness_check_s}` |
| `paths()` | `dict` | Resolved paths: `root_dir`, `config_dir`, `scripts_python`, `scripts_lua`, `data_dir`, `python_requirements`, `hub_script_dir`, `log_file` |
| `channels()` | `list[dict]` | Active channels (see below) |
| `reset()` | `None` | Clear user variables from interpreter namespace (preserves `pylabhub`) |
| `shutdown()` | `None` | Request graceful hub shutdown |
| `__version__` | `str` | Library version string |

**`channels()` dict keys** (per channel):

| Key | Type | Description |
|---|---|---|
| `name` | `str` | Channel name |
| `schema_hash` | `str` | 64-char hex schema identifier |
| `consumer_count` | `int` | Number of connected consumers |
| `producer_pid` | `int` | Producer process ID |
| `status` | `str` | `"PendingReady"` / `"Ready"` / `"Closing"` |

Channel status lifecycle: `PendingReady` (registered, awaiting first heartbeat)
→ `Ready` (operational) → `Closing` (heartbeat timeout or producer exited).

#### Wire protocol (for custom clients)

Any ZMQ REQ socket can speak to the admin shell:

**Request** (JSON):
```json
{ "token": "<hex-token-or-empty>", "code": "<python source>" }
```

**Response** (JSON):
```json
{ "success": true, "output": "<captured stdout/stderr>", "error": "" }
{ "success": false, "output": "<captured output>",       "error": "<traceback or reason>" }
```

Error cases and their `"error"` values:

| Condition | `"error"` value |
|---|---|
| Request > 1 MB | `"request too large"` |
| Invalid JSON | `"invalid JSON request"` |
| Missing `code` field | `"missing or invalid 'code' field"` |
| Wrong token | `"unauthorized"` |
| Python exception | Full traceback string |

Size check → JSON parse → token auth → code execution (in that order).

#### Authentication

Set `admin.token` in `hub.json` (written automatically by `--init`):
```json
{ "admin": { "token": "<64-char hex>" } }
```

Pass the token via `--token` flag to `hubshell_client.py`, or include it in
the `"token"` field of a raw ZMQ request. Empty string = no auth required
(any local connection is accepted; socket is loopback-only by default).

### 4.7 Connection policy

See §7 for full policy reference. Quick setup (all policy fields are nested under `"hub"` in `hub.json`):

```json
{
  "hub": {
    "connection_policy": "required",
    "channel_policies": [
      { "channel": "lab.daq.*",     "connection_policy": "verified" },
      { "channel": "lab.monitor.*", "connection_policy": "open" }
    ],
    "known_actors": [
      { "name": "lab.daq.sensor1",     "uid": "ACTOR-SENSOR1-A1B2C3D4", "role": "producer" },
      { "name": "lab.analysis.logger", "uid": "ACTOR-LOGGER-9E1D4C2A",  "role": "consumer" }
    ]
  }
}
```

To add actors to `known_actors` without manual editing:
```bash
pylabhub-actor --register-with <hub_dir> <actor_dir>
```

---

## 5. Actor Setup

### 5.1 Actor directory model

Each actor is a **directory**. The directory holds the actor's identity, role configs,
and per-role Python script packages. An actor may have multiple roles (e.g. one producer
and one consumer).

```
<actor_dir>/
  actor.json                    ← identity + all role configs
  roles/
    <role_name>/                ← one subdirectory per role (user-named: "data_out", "cfg_in", …)
      script/                   ← Python package (always module name "script")
        __init__.py             ← callbacks: on_init / on_iteration / on_stop
        helpers.py              ← optional helpers (from . import helpers)
        data/                   ← optional non-Python assets next to __init__.py
  logs/                         ← rotating log files (created at startup)
  run/
    actor.pid                   ← PID of the running process
```

**Per-role isolation:** each role's `script/` package is loaded under a unique
`sys.modules` alias (`_plh_{uid_hex}_{role_name}`). Two roles can share the module
name `"script"` without conflict — each gets its own module object with independent
module-level state.

### 5.2 Initialising an actor

```bash
pylabhub-actor --init <actor_dir>
```

If `<actor_dir>` is omitted, the current directory is used.

The init flow:
1. Creates `<actor_dir>/logs/` and `<actor_dir>/run/`
2. Prompts for **actor name** (e.g. `lab.daq.sensor1`)
3. Generates `actor_uid` in `ACTOR-{NAME}-{8HEX}` format
4. Writes `actor.json` with one example `data_out` producer role
5. Creates `roles/data_out/script/__init__.py` with template callbacks

After `--init`, edit `actor.json`:
1. Set `"hub_dir"` to the absolute path to your hub instance directory
2. Configure each role's `channel`, `slot_schema`, `flexzone_schema`, etc.
3. Write your callbacks in `roles/<role>/script/__init__.py`

**Actor UID format:** `ACTOR-{NAME}-{8HEX}` (e.g. `ACTOR-SENSOR1-A1B2C3D4`)
Auto-generated from `actor_name` at init time; stored in `actor.json["actor"]["uid"]`.

**Registering with a hub** (for `"verified"` connection policy only):
```bash
pylabhub-actor --register-with <hub_dir> <actor_dir>
# → reads actor_name + actor_uid from actor.json
# → appends to hub.json known_actors (requires write access to hub_dir)
```

### 5.3 actor.json — complete field reference

```json
{
  "hub_dir": "/opt/pylabhub/hubs/daq1",

  "actor": {
    "uid":       "ACTOR-SENSOR1-A1B2C3D4",
    "name":      "lab.daq.sensor1",
    "log_level": "info"
  },

  "roles": {
    "data_out": {
      "kind":        "producer",
      "channel":     "lab.daq.sensor1.raw",
      "interval_ms": 100,
      "loop_timing": "fixed_pace",
      "loop_trigger": "shm",
      "script":      { "module": "script", "path": "./roles/data_out" },
      "shm": {
        "enabled":    true,
        "slot_count": 8,
        "secret":     0
      },
      "slot_schema": {
        "packing": "natural",
        "fields": [
          { "name": "ts",    "type": "float64" },
          { "name": "value", "type": "float32" },
          { "name": "flags", "type": "uint8" }
        ]
      },
      "flexzone_schema": {
        "fields": [
          { "name": "device_id",   "type": "uint16" },
          { "name": "sample_rate", "type": "uint32" },
          { "name": "label",       "type": "string", "length": 32 }
        ]
      },
      "validation": {
        "slot_checksum":     "update",
        "flexzone_checksum": "update",
        "on_checksum_fail":  "skip",
        "on_python_error":   "continue"
      }
    },
    "cfg_in": {
      "kind":       "consumer",
      "channel":    "lab.daq.sensor1.config",
      "timeout_ms": 5000,
      "script":     { "module": "script", "path": "./roles/cfg_in" },
      "shm": { "enabled": true, "secret": 0 },
      "slot_schema": {
        "fields": [{ "name": "setpoint", "type": "float32" }]
      }
    }
  }
}
```

#### Top-level `actor` block

| Field | Required | Default | Description |
|---|---|---|---|
| `uid` | no | auto-generated | Stable `ACTOR-{NAME}-{8HEX}` identifier. Generated from `name` if absent. Keep stable across restarts — changing it changes the actor's identity. |
| `name` | no | `""` | Human name used as input for UID generation. Convention: reverse-domain format matching channel names. |
| `log_level` | no | `"info"` | `"debug"` / `"info"` / `"warn"` / `"error"` |
| `auth.keyfile` | no | `""` | Path to the encrypted actor vault file (created by `--keygen`). When set, the actor decrypts this vault at startup to load its CurveZMQ client keypair. Password via `PYLABHUB_ACTOR_PASSWORD` env var or interactive prompt. Empty = ephemeral keypair (dev/test only). |

**Actor vault setup (for production CurveZMQ identity):**
```bash
# 1. Set auth.keyfile in actor.json (e.g. "actor.key"), then generate the vault:
pylabhub-actor --config ./actor.json --keygen
# → prompts for vault password (or reads PYLABHUB_ACTOR_PASSWORD)
# → creates encrypted vault at the keyfile path
# → prints the 40-char Z85 public key

# 2. Run the actor — prompts for vault password at startup:
pylabhub-actor ./my-actor
# → or: PYLABHUB_ACTOR_PASSWORD=<password> pylabhub-actor ./my-actor
```

#### Top-level fields

| Field | Required | Default | Description |
|---|---|---|---|
| `hub_dir` | yes* | absent | Path to hub instance directory. `broker_endpoint` and `broker_pubkey` are resolved from `<hub_dir>/hub.json` and `<hub_dir>/hub.pubkey`. May be omitted for `--config` invocation in tests where `broker` is set explicitly. |
| `broker` | no | `tcp://127.0.0.1:5570` | Explicit broker endpoint (overridden by `hub_dir` resolution). |
| `broker_pubkey` | no | `""` (plain TCP) | CurveZMQ server public key for the broker, Z85 40 chars (overridden by `hub_dir`). |
| `script` | no | absent | Actor-level script fallback: `{"module": "…", "path": "…"}`. Used for any role that has no per-role `"script"` key. |

#### Per-role fields (common to producer and consumer)

| Field | Required | Default | Description |
|---|---|---|---|
| `kind` | yes | — | `"producer"` or `"consumer"` |
| `channel` | yes | — | Channel name to create (producer) or subscribe to (consumer). Reverse-domain format: `"lab.daq.sensor1.raw"` |
| `script` | no* | falls back to top-level | Per-role script package: `{"module": "script", "path": "./roles/<role_name>"}`. **Must be an object** — bare string throws at config load. |
| `loop_timing` | no | `"fixed_pace"` | Deadline scheduling: `"fixed_pace"` (reset deadline from wakeup; no catch-up burst) or `"compensating"` (advance tick regardless; catches up after overrun). |
| `loop_trigger` | no | `"shm"` | Loop trigger: `"shm"` (block on SHM slot; requires `shm.enabled=true`) or `"messenger"` (block on ZMQ message; slot is always `None`). |
| `messenger_poll_ms` | no | `5` | Poll timeout when `loop_trigger="messenger"`. Values ≥ 10 log a warning at config load. |
| `broker` | no | resolved from `hub_dir` | Per-role broker endpoint override. Rarely needed. |
| `broker_pubkey` | no | resolved from `hub_dir` | Per-role broker pubkey override. |
| `slot_schema` | no* | absent | ctypes field list for the slot type. Required if `shm.enabled=true`. Omit for Messenger-only roles. |
| `flexzone_schema` | no | absent | ctypes field list for the flexzone (persistent producer-to-consumer data, parallel to the slot ring). |
| `shm.enabled` | no | `false` | Whether to create (producer) or attach to (consumer) a DataBlock SHM segment. |
| `shm.secret` | no | `0` | 32-bit shared secret used during SHM segment discovery. Must match between producer and consumer. |
| `validation.slot_checksum` | no | `"update"` | `"none"` (no checksum), `"update"` (write checksum on produce, check on consume), `"enforce"` (reject slot on checksum mismatch). |
| `validation.flexzone_checksum` | no | `"update"` | Same options as `slot_checksum`, applied to the flexzone. |
| `validation.on_checksum_fail` | no | `"skip"` | `"skip"` (don't call `on_iteration`); `"pass"` (call `on_iteration` with `api.slot_valid()==False`). |
| `validation.on_python_error` | no | `"continue"` | `"continue"` (log traceback, keep running); `"stop"` (stop this role). |

#### Producer-only role fields

| Field | Required | Default | Description |
|---|---|---|---|
| `interval_ms` | no | `0` | Write interval in milliseconds. `0` = full throughput (acquire next slot immediately). `N > 0` = deadline-scheduled at `N` ms intervals. |
| `shm.slot_count` | no | `4` | SHM ring buffer capacity in slots. More slots absorb bursts at the cost of memory. Minimum 2. |
| `heartbeat_interval_ms` | no | `0` | Heartbeat interval override. `0` = `10 × interval_ms`. Rarely needs changing. |

#### Consumer-only role fields

| Field | Required | Default | Description |
|---|---|---|---|
| `timeout_ms` | no | `-1` | `-1` = block indefinitely. `N > 0` = fire `on_iteration(slot=None, …)` after `N` ms of silence (useful for watchdog or periodic fallback). |

### 5.4 Running an actor

```bash
# Standard: directory-based startup
pylabhub-actor <actor_dir>

# Explicit config file path (useful for tests and CI)
pylabhub-actor --config <path_to_actor.json>
```

**Shutdown:** send `SIGINT` or `SIGTERM`. The actor finishes the current iteration,
calls `on_stop()` on all roles, and exits cleanly.

---

## 6. Writing Actor Scripts

### 6.1 Script package structure

Each role's script is a Python **package** — a directory named `script/` containing
`__init__.py`. The package lives at `roles/<role_name>/script/`:

```
roles/
  data_out/                 ← role directory (role-name is user-defined)
    script/                 ← Python package (module name is always "script")
      __init__.py           ← main entry point (on_init, on_iteration, on_stop)
      helpers.py            ← optional helpers — use relative import: from . import helpers
      models.py             ← another submodule
      calibration/          ← optional sub-package
        __init__.py
        coefficients.py
```

The `pylabhub-actor --init` command generates a starter `__init__.py` in
`roles/data_out/script/`. Copy this structure for each additional role.

**Module isolation:** each role's package is imported under a role-unique alias in
`sys.modules`. Two roles can both name their package `"script"` without collision —
each gets its own module object with independent global state.

**Standard imports in `__init__.py`:**

```python
import pylabhub_actor as actor   # always available (C++ embedded module)
import numpy as np               # installed via requirements.txt
from . import helpers            # relative import within script/ package
from .calibration import load    # import from sub-package
```

### 6.2 Callback reference

```python
# roles/data_out/script/__init__.py
import pylabhub_actor as actor
import time

# Module-level state is private to this role
_count = 0

def on_init(api: actor.ActorRoleAPI) -> None:
    """
    Called once before the loop starts.
    Use to initialise hardware, open files, set flexzone metadata.
    api.flexzone() is valid here for producers.
    """
    api.log("info", f"[{api.role_name()}] starting on channel {api.channel()}")

    fz = api.flexzone()
    if fz is not None:
        fz.device_id   = 42
        fz.sample_rate = 1000
        fz.label       = b"lab.sensor.temperature"
        api.update_flexzone_checksum()


def on_iteration(slot, flexzone, messages, api: actor.ActorRoleAPI):
    """
    Called every loop iteration.

    Parameters
    ----------
    slot      : ctypes.LittleEndianStructure  — writable (producer) or read-only (consumer).
                None when triggered by Messenger timeout or loop_trigger="messenger".
    flexzone  : ctypes.LittleEndianStructure  — persistent; writable for producer, read-only
                for consumer. None if no flexzone_schema configured.
    messages  : list of (sender: str, data: bytes)  — ZMQ messages received since last call.
                May be empty even when slot is available.
    api       : ActorRoleAPI  — proxy for this role.

    Returns
    -------
    Producer: True/None = commit slot; False = discard slot.
    Consumer: return value is ignored.
    """
    global _count

    # Handle any ZMQ messages that arrived since the last iteration
    for sender, data in messages:
        api.log("debug", f"msg from {sender}: {data!r}")

    # slot is None when triggered by timeout or loop_trigger="messenger"
    if slot is None:
        return None

    if api.kind() == "producer":
        _count += 1
        slot.ts    = time.time()
        slot.value = float(_count) * 0.01
        slot.flags = 0x01
        return True                     # commit this slot to consumers

    else:  # consumer
        if not api.slot_valid():
            api.log("warn", "checksum failed — discarding slot")
            return None
        api.log("debug", f"received: value={slot.value:.4f}")
        return None


def on_stop(api: actor.ActorRoleAPI) -> None:
    """
    Called once after the loop exits (shutdown or api.stop()).
    Use to flush buffers, close files, release hardware.
    """
    api.log("info", f"[{api.role_name()}] stopped after {_count} iterations")
```

#### Callback summary

| Callback | Signature | When called | If absent |
|---|---|---|---|
| `on_init` | `(api)` | Once, before loop starts | Silently skipped |
| `on_iteration` | `(slot, flexzone, messages, api) → bool` | Every loop iteration | Role is disabled (warning logged) |
| `on_stop` | `(api)` | Once, after loop exits | Silently skipped |

### 6.3 Slot types — ctypes field mapping

The `slot_schema.fields` array defines the ctypes struct that maps directly to SHM memory.
No serialisation/deserialisation; reading or writing a field is a pointer dereference.

| JSON `"type"` | ctypes equivalent | Notes |
|---|---|---|
| `"bool"` | `ctypes.c_bool` | 1 byte |
| `"int8"` / `"uint8"` | `c_int8` / `c_uint8` | 1 byte |
| `"int16"` / `"uint16"` | `c_int16` / `c_uint16` | 2 bytes |
| `"int32"` / `"uint32"` | `c_int32` / `c_uint32` | 4 bytes |
| `"int64"` / `"uint64"` | `c_int64` / `c_uint64` | 8 bytes |
| `"float32"` | `c_float` | 4 bytes, IEEE 754 |
| `"float64"` | `c_double` | 8 bytes, IEEE 754 |
| `"string"` + `"length": N` | `c_char * N` | Fixed-size byte buffer; assign with `b"text"` |
| `"bytes"` + `"count": N` | `c_uint8 * N` | Raw byte array |
| any type + `"count": N` | `ctypes_type * N` | Scalar array |

**Packing:**
- `"packing": "natural"` (default) — C struct alignment rules (padding between fields)
- `"packing": "packed"` — `_pack_ = 1` (no padding; dense layout)

**String fields:** assign `bytes`, not `str`:
```python
slot.label = b"sensor-01"      # correct
slot.label = "sensor-01"       # TypeError
```

**Array fields:** index-access or slice:
```python
for i, v in enumerate(samples):
    slot.raw_data[i] = v
```

**Do not store `slot` beyond `on_iteration`:** the ctypes view is backed by SHM memory
that the runtime will reuse. Storing and accessing `slot` after return is undefined behaviour.
`flexzone` is safe to store and access across iterations.

### 6.4 ActorRoleAPI methods

One `ActorRoleAPI` instance per active role, passed to every callback.

```python
# ── Common (all roles) ──────────────────────────────────────────────────────
api.log(level: str, msg: str)           # log via hub logger; levels: debug/info/warn/error
api.uid() -> str                        # actor uid: "ACTOR-SENSOR1-A1B2C3D4"
api.role_name() -> str                  # role name: "data_out", "cfg_in", etc.
api.actor_name() -> str                 # human actor name from config
api.channel() -> str                    # channel name for this role
api.broker() -> str                     # resolved broker endpoint
api.kind() -> str                       # "producer" or "consumer"
api.log_level() -> str                  # configured log level
api.script_dir() -> str                 # path of the script package parent directory
api.stop()                              # request graceful shutdown of all roles
api.set_critical_error()                # latch error + stop; use for unrecoverable failures
api.critical_error() -> bool            # True if set_critical_error() has been called
api.flexzone() -> ctypes.Structure|None # persistent flexzone object (or None if no schema)

# ── Producer roles ──────────────────────────────────────────────────────────
api.broadcast(data: bytes) -> bool      # send ZMQ message to all connected consumers
api.send(identity: str, data: bytes)    # unicast ZMQ to one consumer (identity string)
api.consumers() -> list                 # list of ZMQ identity strings of live consumers
api.update_flexzone_checksum() -> bool  # recompute and store BLAKE2b on the SHM flexzone

# ── Consumer roles ──────────────────────────────────────────────────────────
api.send_ctrl(data: bytes) -> bool      # send ZMQ ctrl frame to the producer
api.slot_valid() -> bool                # False when checksum failed and on_checksum_fail="pass"
api.verify_flexzone_checksum() -> bool  # verify the producer's flexzone BLAKE2b
api.accept_flexzone_state() -> bool     # accept current flexzone as a valid baseline

# ── Spinlocks (requires shm.enabled=true) ───────────────────────────────────
api.spinlock(index: int)                # returns SharedSpinLockPy context manager (0–7)
api.spinlock_count() -> int             # always 8; 0 if SHM not configured

# Usage: mutual exclusion between producer and consumer on a shared flexzone field
with api.spinlock(0):
    flexzone.counter += 1

# ── Diagnostics ─────────────────────────────────────────────────────────────
api.script_error_count() -> int   # Python exceptions in callbacks since role start
api.loop_overrun_count()  -> int  # write cycles that exceeded interval_ms deadline
api.last_cycle_work_us()  -> int  # µs of active work in the last write cycle (producer)
api.metrics() -> dict             # all timing metrics (see HEP-CORE-0008 for keys)
```

### 6.5 Flexzone — persistent shared data

The **flexzone** is a fixed-size region in SHM that lives alongside the slot ring buffer.
Unlike slots (which are ring-buffered and can be overwritten), the flexzone is **written
once** (or rarely updated) and read by all consumers at any time.

**Use cases:**
- Device metadata that consumers need before processing slots (device ID, sample rate, calibration)
- Producer-to-consumer configuration channel
- Shared state that changes slowly (mode flags, scale factors)

**Schema:** define `flexzone_schema` in the role config:
```json
"flexzone_schema": {
  "fields": [
    { "name": "device_id",   "type": "uint16" },
    { "name": "sample_rate", "type": "uint32" },
    { "name": "label",       "type": "string", "length": 32 }
  ]
}
```

**Producer** — writes flexzone in `on_init` (and optionally in `on_iteration`):
```python
def on_init(api):
    fz = api.flexzone()
    fz.device_id   = 42
    fz.sample_rate = 1000
    fz.label       = b"ADC channel 0"
    api.update_flexzone_checksum()   # BLAKE2b over flexzone bytes
```

**Consumer** — reads flexzone at any time:
```python
def on_init(api):
    fz = api.flexzone()
    if api.verify_flexzone_checksum():
        print(f"device_id={fz.device_id} sample_rate={fz.sample_rate}")
        api.accept_flexzone_state()
    else:
        api.log("warn", "flexzone checksum mismatch — producer not ready yet")
```

**Spinlock protection** — if producer updates flexzone during iterations (rare), use a
spinlock to prevent consumer from reading a partially-written value:
```python
# Producer
with api.spinlock(0):
    fz.scale_factor = new_calibration
    api.update_flexzone_checksum()

# Consumer
with api.spinlock(0):
    if api.verify_flexzone_checksum():
        apply(fz.scale_factor)
```

### 6.6 ZMQ messaging between actors

In addition to SHM slots, actors can send arbitrary ZMQ messages peer-to-peer. Messages
are queued via `incoming_queue_` and passed to `on_iteration` as the `messages` list.

**Producer broadcasts to all consumers:**
```python
api.broadcast(b"sync")
api.broadcast(json.dumps({"cmd": "recalibrate"}).encode())
```

**Producer unicasts to one consumer:**
```python
consumers = api.consumers()   # list of identity strings
if consumers:
    api.send(consumers[0], b"special-data")
```

**Consumer sends back to producer:**
```python
api.send_ctrl(b"ack")
api.send_ctrl(json.dumps({"status": "ready"}).encode())
```

**Receiving messages** — in `on_iteration`, the `messages` parameter carries all ZMQ
messages received since the previous call:
```python
def on_iteration(slot, flexzone, messages, api):
    for sender, data in messages:
        cmd = json.loads(data)
        if cmd.get("cmd") == "recalibrate":
            reload_calibration()
    ...
```

The ZMQ channel is always available even when `loop_trigger="shm"` — messages and slot
data are both delivered in the same `on_iteration` call.

### 6.7 Error handling patterns

**Non-fatal recoverable error** — log and continue:
```python
def on_iteration(slot, flexzone, messages, api):
    try:
        slot.value = read_sensor()
    except IOError as e:
        api.log("warn", f"sensor read failed: {e}")
        return False    # discard this slot
    return True
```

**Fatal unrecoverable error** — call `api.set_critical_error()`:
```python
def on_init(api):
    if not hardware.connect():
        api.log("error", "hardware not found — shutting down actor")
        api.set_critical_error()   # latches error + triggers graceful stop
        return
```

**`on_python_error`** controls what happens when an unhandled exception escapes
`on_iteration`:
- `"continue"` (default): traceback is logged; `script_error_count` incremented; loop continues
- `"stop"`: role stops on the first unhandled exception

**Monitoring via diagnostics:**
```python
def on_iteration(slot, flexzone, messages, api):
    m = api.metrics()
    if api.loop_overrun_count() > 100:
        api.log("warn", f"overruns={api.loop_overrun_count()} last_work={api.last_cycle_work_us()}µs")
    ...
```

---

## 7. Connection Policy — Securing the Hub

The hub's connection policy controls who is allowed to create channels or subscribe to them.
Configured in `hub.json["hub"]["connection_policy"]` (hub-wide default) and optionally
overridden per channel via `hub.json["hub"]["channel_policies"]`.

### Policy levels

| Policy | Enforcement | Use case |
|---|---|---|
| `"open"` | No identity required. Any client connects. | Development, local lab networks. |
| `"tracked"` | Actor name + UID accepted if provided; not required. Full provenance logged without enforcement. | Audit trail; no access control. |
| `"required"` | `actor_name` + `actor_uid` must be present in every REG_REQ / CONSUMER_REG_REQ. Rejected if absent. Not checked against an allowlist. | Enforces identity without maintaining a registry. |
| `"verified"` | `actor_name` + `actor_uid` must match an entry in `known_actors`. Unknown actors are rejected. | Maximum security; requires actor pre-registration. |

### Setting up `"verified"` policy

1. Run `pylabhub-actor --init <actor_dir>` for each actor (generates stable UID).
2. Register each actor with the hub:
   ```bash
   pylabhub-actor --register-with /opt/pylabhub/hubs/daq1 /opt/pylabhub/actors/sensor1
   ```
   This appends to `hub.json["known_actors"]` — idempotent (won't add duplicates).
3. Set policy in `hub.json`:
   ```json
   { "connection_policy": "verified" }
   ```
4. Restart the hub.

### Per-channel overrides

First matching glob pattern wins. Hub-wide policy is the fallback.

```json
{
  "hub": {
    "connection_policy": "tracked",
    "channel_policies": [
      { "channel": "lab.daq.*",      "connection_policy": "verified" },
      { "channel": "lab.monitor.*",  "connection_policy": "open" },
      { "channel": "lab.control.*",  "connection_policy": "required" }
    ]
  }
}
```

Glob matching: `*` matches any sequence of characters within a channel name segment.
`lab.daq.*` matches `lab.daq.sensor1.raw`, `lab.daq.temp`, etc.

### `known_actors` format

All policy fields (`connection_policy`, `known_actors`, `channel_policies`) are nested
inside the `"hub"` block in `hub.json`:

```json
{
  "hub": {
    "known_actors": [
      { "name": "lab.daq.sensor1",     "uid": "ACTOR-SENSOR1-A1B2C3D4", "role": "producer" },
      { "name": "lab.analysis.logger", "uid": "ACTOR-LOGGER-9E1D4C2A",  "role": "consumer" },
      { "name": "lab.control.main",    "uid": "ACTOR-CONTROL-4F8E1B5A", "role": "any" }
    ]
  }
}
```

`"role"` is informational only (not enforced by the policy engine). Use `"any"` when the
same actor has both producer and consumer roles.

---

## 8. Common Topologies

### Single producer → multiple consumers

```
sensor-actor (producer: lab.daq.sensor1.raw)
    ↓ SHM slot ring
    ├── logger-actor  (consumer: lab.daq.sensor1.raw)
    └── display-actor (consumer: lab.daq.sensor1.raw)
```

Each consumer gets every committed slot in sequence. The ring buffer absorbs bursts —
consumers that fall behind read stale slots but never block the producer.

### Multi-role actor (producer + consumer)

A single actor can hold multiple roles connecting to different channels:

```json
{
  "roles": {
    "raw_out":  { "kind": "producer", "channel": "lab.daq.sensor1.raw",    ... },
    "status_in": { "kind": "consumer", "channel": "lab.control.status", ... }
  }
}
```

Both roles run concurrently. The `raw_out` role writes sensor data; `status_in` receives
commands from a control actor and can update shared state via the flexzone or `api.stop()`.

### Message-driven actor (no SHM)

When an actor only needs ZMQ messaging (no shared memory), set `loop_trigger="messenger"`
and omit `shm`:

```json
{
  "roles": {
    "alert_out": {
      "kind":            "producer",
      "channel":         "lab.alerts",
      "loop_trigger":    "messenger",
      "messenger_poll_ms": 5
    }
  }
}
```

`on_iteration` fires on each incoming ZMQ message or after `messenger_poll_ms` timeout.
`slot` is always `None`.

### Hub-to-hub bridge

An actor acts as a consumer on Hub A and a producer on Hub B, forwarding data.
Each role connects to a different `broker` endpoint:

```json
{
  "roles": {
    "bridge_in":  { "kind": "consumer", "channel": "lab.a.sensor", "broker": "tcp://192.168.1.10:5570", ... },
    "bridge_out": { "kind": "producer", "channel": "lab.b.sensor", "broker": "tcp://192.168.1.20:5570", ... }
  }
}
```

---

## 9. Operational Patterns

### Development workflow

```bash
# Terminal 1: hub in dev mode (ephemeral keypair, no vault prompt)
./build/stage-debug/bin/pylabhub-hubshell ./my-hub --dev

# Terminal 2: actor with verbose logging
./build/stage-debug/bin/pylabhub-actor ./my-actor
```

### systemd service — hub

```ini
# /etc/systemd/system/pylabhub-hub-daq1.service
[Unit]
Description=pyLabHub Hub (daq1)
After=network.target

[Service]
Type=simple
User=pylabhub
Environment=PYLABHUB_MASTER_PASSWORD=<vault-password>
ExecStart=/usr/local/bin/pylabhub-hubshell /opt/pylabhub/hubs/daq1
Restart=on-failure
RestartSec=5s
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### systemd service — actor

```ini
# /etc/systemd/system/pylabhub-actor-sensor1.service
[Unit]
Description=pyLabHub Actor (sensor1)
After=pylabhub-hub-daq1.service
Requires=pylabhub-hub-daq1.service

[Service]
Type=simple
User=pylabhub
ExecStart=/usr/local/bin/pylabhub-actor /opt/pylabhub/actors/sensor1
Restart=on-failure
RestartSec=3s

[Install]
WantedBy=multi-user.target
```

### Log files

Hub and actor rotate logs under `<instance_dir>/logs/`. Log level is set in:
- Hub: `PYLABHUB_HUB_NAME` env (no log-level env; use `hub.json` via startup script)
- Actor: `actor.json["actor"]["log_level"]` = `"debug"` / `"info"` / `"warn"` / `"error"`

### Recommended directory layout for production

```
/opt/pylabhub/
  bin/                       ← install tree (symlink to /usr/local/bin/pylabhub*)
  lib/                       ← shared library
  hubs/
    daq1/                    ← hub instance (hub.json, hub.vault, hub.pubkey, logs/)
    daq2/
  actors/
    sensor1/                 ← actor instance (actor.json, roles/, logs/)
    logger/
    display/
```

No paths are hardwired in the binaries. Pass `<hub_dir>` and `<actor_dir>` on the command
line (or as ExecStart arguments in systemd units).

---

## 10. Troubleshooting

### Actor cannot connect to hub

1. Check `hub.pubkey` exists in `<hub_dir>/`: `ls -la <hub_dir>/hub.pubkey`
   - If missing: restart the hub so it re-runs `HubVault::publish_public_key()`
2. Check `hub_dir` in `actor.json` points to the correct hub directory
3. Check `broker_endpoint` in `hub.json` is reachable from the actor machine:
   ```bash
   nc -zv <hub-ip> 5570
   ```
4. Check connection policy: if `"required"` or `"verified"`, ensure the actor has a UID
   and (for verified) is in `known_actors`. Run:
   ```bash
   pylabhub-actor --register-with <hub_dir> <actor_dir>
   ```

### `hub.vault` decrypt failure

- Wrong password: try `PYLABHUB_MASTER_PASSWORD` env var
- Corrupted vault: no recovery — re-run `--init` with a new directory (or restore from backup)
- Changed `hub_uid` in `hub.json`: the UID is used as the Argon2id salt; changing it
  makes the vault unreadable. Do not edit `hub["uid"]` after `--init`.

### Actor script import error

```
ModuleNotFoundError: No module named 'numpy'
```

Ensure the Python packages listed in `requirements.txt` are installed in the embedded
Python runtime. Run the build with `prepare_python_env` target:
```bash
cmake --build build --target prepare_python_env
```

Or install manually into the embedded Python:
```bash
./build/stage-debug/opt/python/bin/pip install numpy
```

### SHM segment not found

Consumer attaches before producer creates the segment. The consumer polls automatically
during `connect()`. If the producer never appears, check:
1. Producer and consumer use the same `channel` name (case-sensitive)
2. Producer and consumer use the same `shm.secret` value
3. Both are registered with the same broker

### Slot checksum failures

```
warn: checksum failed — discarding slot
```

Likely cause: producer writes `slot.field` and returns `True` without the C++ runtime
having any integrity issue — a genuine BLAKE2b mismatch. Check:
1. Producer and consumer use the same `slot_schema` (types, names, order, packing)
2. `validation.slot_checksum` is `"update"` on the producer side (not `"none"`)
3. No external process writing to the SHM segment

### Loop overruns (producer)

`api.loop_overrun_count()` keeps growing? The producer's `on_iteration` body takes
longer than `interval_ms`. Options:
1. Increase `interval_ms`
2. Move slow work off the iteration path (use a background thread)
3. Switch `loop_timing` to `"fixed_pace"` to prevent burst catch-up
4. Profile with `api.last_cycle_work_us()`

---

*This document covers pyLabHub as of 2026-02-27. For design rationale and protocol
details, see the HEP documents in `docs/HEP/`.*
