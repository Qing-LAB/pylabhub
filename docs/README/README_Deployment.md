# pyLabHub Deployment Guide

**Purpose:** End-to-end reference for deploying pyLabHub from a fresh build to a running
pipeline. Covers install tree, hub and role instance directories, every configuration field,
Python script authoring, connection policy, and operational patterns.

**Last Updated:** 2026-03-15 (Python runtime no longer bundled in wheel; `pylabhub prepare-runtime`)

**Related:**
- `docs/README/README_DirectoryLayout.md` — architectural directory model reference
- `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` — full producer/consumer spec
- `docs/HEP/HEP-CORE-0015-Processor-Binary.md` — full processor spec
- `docs/HEP/HEP-CORE-0007-DataHub-Protocol-and-Policy.md` — broker control plane protocol
- `docs/HEP/HEP-CORE-0025-System-Config-and-Python-Environment.md` — system config and Python env spec
- `docs/todo/SECURITY_TODO.md` — vault and CurveZMQ key management

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
3. [Build and Install Tree](#3-build-and-install-tree)
4. [Hub Setup](#4-hub-setup)
   - 4.1 [Hub directory layout](#41-hub-directory-layout)
   - 4.2 [hub.json — field reference](#42-hubjson--field-reference)
   - 4.3 [Running the hub](#43-running-the-hub)
   - 4.4 [Hub Python script (optional)](#44-hub-python-script-optional)
5. [Producer Setup](#5-producer-setup)
   - 5.1 [producer.json — field reference](#51-producerjson--field-reference)
   - 5.2 [Producer Python script](#52-producer-python-script)
6. [Consumer Setup](#6-consumer-setup)
   - 6.1 [consumer.json — field reference](#61-consumerjson--field-reference)
   - 6.2 [Consumer Python script](#62-consumer-python-script)
7. [Processor Setup](#7-processor-setup)
   - 7.1 [processor.json — field reference](#71-processorjson--field-reference)
   - 7.2 [Processor Python script](#72-processor-python-script)
8. [Python Script Reference](#8-python-script-reference)
   - 8.1 [Script package structure](#81-script-package-structure)
   - 8.2 [Slot types — ctypes field mapping](#82-slot-types--ctypes-field-mapping)
   - 8.3 [API methods — all roles](#83-api-methods--all-roles)
   - 8.4 [Flexzone — persistent shared data](#84-flexzone--persistent-shared-data)
   - 8.5 [ZMQ messaging between roles](#85-zmq-messaging-between-roles)
   - 8.6 [Inbox — typed peer-to-peer messages](#86-inbox--typed-peer-to-peer-messages)
   - 8.7 [Error handling patterns](#87-error-handling-patterns)
9. [Connection Policy — Securing the Hub](#9-connection-policy--securing-the-hub)
10. [Multi-Hub Pipelines](#10-multi-hub-pipelines)
11. [Operational Reference](#11-operational-reference)
12. [Python Environment and Virtual Environments](#12-python-environment-and-virtual-environments)

---

## 1. Overview

pyLabHub is a high-performance IPC framework for scientific data acquisition. It uses two
binaries connected by a central broker (hub):

```
plh_role           — Unified role binary. Runs as producer, consumer, or processor
                     depending on `--role <tag>`:

                     plh_role --role producer   — Data source.      Runs user on_produce().
                     plh_role --role consumer   — Data sink.        Runs user on_consume().
                     plh_role --role processor  — Data transformer. Runs user on_process(rx, tx).

plh_hub            — Broker process. Manages channel registry, SHM coordination,
                     control plane. One per independent data domain.
                     (Replaces the prior `pylabhub-hubshell` binary; binary
                     rename finalized 2026-05-21.)
```

Data flow in a typical SHM pipeline:

```
Producer ─[SHM ring]─► Processor ─[SHM ring]─► Consumer
    │                       │                       │
    └──────── Broker (control plane) ───────────────┘
```

Each role connects to the broker for registration, heartbeat, and shutdown. Data itself
flows directly through shared memory (SHM) without touching the broker.

---

## 2. Quick Start

```bash
# 1. Build
cmake -S . -B build && cmake --build build -j2

# 2. Run the bundled single-hub demo (starts all 4 processes)
bash share/py-demo-single-processor-shm/run_demo.sh

# 3. Press Ctrl-C to stop all processes
```

The demo starts a hub, producer (10 Hz, 1 k float32 samples), processor (doubles payload),
and consumer (prints throughput). Expected output:
```
[demo] hub running (pid=12345)
[demo] producer running (pid=12346)
[demo] processor running (pid=12347)
[demo] Starting consumer ... (Ctrl-C to stop)
slots/s=10  MiB/s=0.04  drops=0  errors=0
```

---

## 3. Build and Install Tree

```bash
cmake -S . -B build                              # Debug (default)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # Release
cmake --build build --target stage_all           # Stage all artifacts
```

After `stage_all`, the build output at `build/stage-<buildtype>/` contains:

```
bin/
  plh_role              ← unified role binary (dispatches on --role tag)
  plh_hub               ← hub broker (HEP-CORE-0033)
  plh_pyenv        ← Python environment manager (Unix)
  plh_pyenv.py     ← Python environment manager (core)
  plh_pyenv.ps1    ← Python environment manager (Windows)

lib/
  libpylabhub-utils.so  ← shared runtime library

opt/
  python/               ← standalone Python 3.14 (PYTHONHOME)
    bin/python3          ← interpreter
    lib/python3.14/
      site-packages/     ← base packages (numpy, zarr, pyzmq, etc.)
    venvs/               ← virtual environments (created post-install)
  luajit/               ← bundled LuaJIT runtime

NOTE: In pip-installed wheels, opt/python/ is NOT included to meet PyPI size
limits. Run 'pylabhub prepare-runtime' after pip install to download it (~130 MB).
Developer builds (cmake) stage the runtime automatically.

config/                 ← system configuration (optional)
  pylabhub.json         ← Python home override, future global settings

docs/                   ← README guides + HEP design specifications
  HEP/                  ← HEP design documents

share/                  ← demos, examples, scripts
  scripts/python/
    requirements.txt    ← base Python package list
  py-demo-*/            ← demo pipelines
  py-examples/          ← example scripts

tests/                  ← test binaries
include/                ← development headers
```

---

## 4. Hub Setup

### 4.1 Hub directory layout

Each hub instance is a self-contained directory. `--init` creates the full layout.

```
hub-dir/
  hub.json                     ← hub configuration (0644 — world-readable; no secrets)
  hub.pubkey                   ← broker CurveZMQ public key (written by --keygen; share with roles)
  vault/
    <hub_uid>.vault            ← encrypted vault: broker CurveZMQ private key + admin token (0600)
                                  Filename embeds the hub UID per HEP-CORE-0033 §6.5
                                  (revised 2026-05-31) so multiple hubs sharing a
                                  vault directory do not collide.  The operator MAY
                                  point hub.auth.keyfile at an absolute path
                                  outside hub-dir/ (recommended for production —
                                  see §4.3); the in-hub-dir relative form shown
                                  here is the --init template default for dev/CI.
  schemas/                     ← (optional) named schema registry root
    <ns>/<name>.v<N>.json
  script/                      ← (optional) hub Python script package
    python/
      __init__.py              ← hub script callbacks (on_start, on_tick, on_stop, ...)
  logs/                        ← log files written by the hub process
```

**File permissions**:
- `hub.json` — 0644 (world-readable). Contains only non-secret configuration.
- `<hub_uid>.vault` — 0600 (owner-only). Encrypted with Argon2id + XSalsa20-Poly1305. Contains the CurveZMQ private key and admin token. Never readable without the vault password.
- `hub.pubkey` — 0644. The broker's CurveZMQ public key; copy to each role's config directory or set `broker_pubkey` in their JSON.

### 4.2 hub.json — field reference

`hub.json` is **world-readable (0644)**. It must never contain secrets. The admin token and
CurveZMQ private key live exclusively in the encrypted vault file (0600).

> 🚧 **Auth/access placeholders.**  `hub.connection_policy`,
> `hub.channel_policies`, and `hub.known_roles` are retained in the
> *schema* but not parsed by today's binaries — the broker runs with
> `RoleIdentityPolicy::Open` regardless.  HEP-CORE-0035 §5 defines the
> operational replacement (`broker.federation_trust_mode` +
> `broker.known_roles[].pubkey` required) and will activate them when
> that HEP lands.

> 🆕 **2026-05-21 — canonical schema synced with `plh_hub --init`
> template.**  Pre-update versions of this section documented the old
> hub.json shape (top-level `peers`, `hub.broker_endpoint`,
> `hub.admin_endpoint`, `hub.connection_policy`, `script.tick_interval_ms`,
> `script.health_log_interval_ms`).  Those fields no longer exist;
> top-level keys moved into `admin` / `network` / `federation` /
> `logging` / `state` blocks and `hub` retained only identity + auth.
> The example below reproduces the `--init` template output verbatim.

```json
{
  "admin": {
    "enabled":        true,
    "endpoint":       "tcp://127.0.0.1:5600",
    "token_required": true
  },
  "broker": {
    "heartbeat_interval_ms":   500,
    "pending_miss_heartbeats":  10,
    "ready_miss_heartbeats":    10
  },
  "federation": {
    "enabled":            false,
    "forward_timeout_ms": 2000,
    "peers":              []
  },
  "hub": {
    "auth":      { "keyfile": "vault/hub.main.uid00000001.vault" },
    "log_level": "info",
    "name":      "main",
    "uid":       "hub.main.uid00000001"
  },
  "logging": {
    "backups":     5,
    "file_path":   "",
    "max_size_mb": 10,
    "timestamped": true
  },
  "loop_timing":          "fixed_rate",
  "network": {
    "broker_bind":     true,
    "broker_endpoint": "tcp://127.0.0.1:5570",
    "zmq_io_threads":  1
  },
  "python_venv": "",
  "script": {
    "path": ".",
    "type": "python"
  },
  "state": {
    "disconnected_grace_ms":    60000,
    "max_disconnected_entries":  1000
  },
  "stop_on_script_error": false,
  "target_period_ms":     1000
}
```

**UID format.**  Hub + role UIDs follow HEP-CORE-0033 §G2.2.0b
grammar: `<tag>.<name>.<unique-suffix>` where `<tag>` is `hub`,
`prod`, `cons`, or `proc`, and `<unique-suffix>` is `uid` + 8 hex
chars.  All lowercase, dotted.  Examples: `hub.main.uid3a7f2b1c`,
`prod.sensor.uid12345678`.  This replaces the earlier
`HUB-MAIN-3A7F2B1C` upper-dash form documented in pre-2026-05
revisions.

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `admin.enabled` | no | `true` | Bind the admin ROUTER socket.  `false` disables admin entirely |
| `admin.endpoint` | no | `"tcp://127.0.0.1:5600"` | ZMQ ROUTER bind address for the admin shell |
| `admin.token_required` | no | `true` | Require admin token (from vault) on every admin request |
| `broker.heartbeat_interval_ms` | no | `500` | Max tolerated silence between heartbeats (HEP-CORE-0023 §2.5.1) |
| `broker.pending_miss_heartbeats` | no | `10` | Pending → Disconnected demotion threshold (HEP-CORE-0023 §2.1) |
| `broker.ready_miss_heartbeats` | no | `10` | Connected → Pending demotion threshold (HEP-CORE-0023 §2.1) |
| `federation.enabled` | no | `false` | Run as a federation peer (HEP-CORE-0022) |
| `federation.forward_timeout_ms` | no | `2000` | Timeout for federation forwarding |
| `federation.peers[]` | no | `[]` | Federation peer list |
| `hub.auth.keyfile` | **YES** | `"vault/<hub_uid>.vault"` (`--init` template default; UID-keyed per HEP-CORE-0033 §6.5 revised 2026-05-31) | Required non-empty path string to the encrypted vault.  Relative paths resolve against `hub_dir`.  Empty string / missing field / missing `auth` object → config-load error (HEP-CORE-0033 §7.1).  pylabhub is a vault: no in-memory CURVE mode.  See §4.3 for the security-warning rule when the path resolves inside `hub_dir`. |
| `hub.log_level` | no | `"info"` | `debug` / `info` / `warn` / `error` |
| `hub.name` | no | `"local.hub.default"` | Human name; also used to auto-generate `hub.uid` |
| `hub.uid` | no | auto | Stable UID in `hub.<name>.uid<hex>` format; auto-generated if absent |
| `logging.backups` | no | `5` | Rotating log retention count |
| `logging.file_path` | no | `""` | Empty = derive from hub_dir/logs |
| `logging.max_size_mb` | no | `10` | Rotate when active log reaches this size |
| `logging.timestamped` | no | `true` | Append timestamp to rotated files |
| `loop_timing` | no | `"fixed_rate"` | Hub-script tick policy (`"fixed_rate"` / `"max_rate"`); irrelevant if hub has no script |
| `network.broker_bind` | yes | `true` | Broker socket binds (`true`) or connects (`false`) |
| `network.broker_endpoint` | no | `"tcp://127.0.0.1:5570"` | ZMQ ROUTER bind address |
| `network.zmq_io_threads` | no | `1` | ZMQ context I/O thread count |
| `python_venv` | no | `""` | Virtual environment for hub script; empty = base env |
| `script.path` | no | `"."` | Base directory; hub script at `<path>/script/<type>/__init__.py`; empty string = no hub script |
| `script.type` | no | `"python"` | Script engine: `"python"` or `"lua"` |
| `state.disconnected_grace_ms` | no | `60000` | Retention before pruning disconnected entries |
| `state.max_disconnected_entries` | no | `1000` | Cap on retained disconnected entries |
| `stop_on_script_error` | no | `false` | Halt hub on uncaught script exception |
| `target_period_ms` | no | `1000` | Hub-script tick interval when `loop_timing="fixed_rate"` |

**Security note**: `hub.json` is world-readable (0644).  Never put
the admin token or any secret in it.  The admin token is sealed
inside the hub vault file (0600, Argon2id+XSalsa20-Poly1305
encrypted) and unlocked at runtime by reading
`PYLABHUB_HUB_PASSWORD` from the environment.

### 4.3 Running the hub

> **Binary rename + setup flow finalized (2026-05-31).**  The binary
> is `plh_hub`.  The `--dev` flag does not exist.  pylabhub is a
> vault — there is no in-memory CURVE mode.  Operators MUST run
> `--keygen` once per hub to materialize the vault before run mode.

```bash
# First-time setup (once per hub) — creates hub.json + skeleton dirs.
# The generated hub.json sets hub.auth.keyfile = "vault/<hub_uid>.vault"
# (UID-keyed; HEP-CORE-0033 §6.5 revised 2026-05-31).
plh_hub --init <hub-dir>/ --name "my-lab-hub"

# Provision the vault + publish hub.pubkey (once per hub).
export PYLABHUB_HUB_PASSWORD="<choose a strong password>"
plh_hub --config <hub-dir>/hub.json --keygen
#   ⇒ writes <hub-dir>/vault/<hub_uid>.vault   (0600, Argon2id-encrypted)
#   ⇒ writes <hub-dir>/hub.pubkey               (0644, broker's Z85 public key)
#
#   If the vault file ALREADY exists at the resolved path, --keygen
#   refuses to overwrite and exits non-zero with an actionable
#   diagnostic ("Refusing to overwrite — rm '<path>' first").  This is
#   the no-silent-overwrite contract (HEP-CORE-0033 §7.1, added
#   2026-05-31): re-keygen would destroy the existing CURVE keypair +
#   admin token, breaking every federation peer pinning the old
#   pubkey + every admin session bound to the old token.  Operator
#   must remove the file explicitly to override.

# Run mode — unlocks vault using PYLABHUB_HUB_PASSWORD from env
export PYLABHUB_HUB_PASSWORD="<same password>"
plh_hub <hub-dir>/

# Validation only (parse config + script syntax; no broker bind; no
# vault unlock — placeholder keyfile paths are fine for --validate
# because the file is never opened).
plh_hub --validate <hub-dir>/
```

After `--keygen`, every restart of the hub uses the same vault —
no re-handshake on the role side, no rekey churn.

**Vault placement security warning (HEP-CORE-0033 §7.2).**  The
`--init` template writes the relative form
`vault/<hub_uid>.vault`, which resolves *inside* `<hub-dir>`.
This is convenient for dev / CI / smoke tests but is not the
recommended production placement: hub-side scripts share the
binary's euid and can corrupt or replace files inside `hub_dir`.
At every config load (every binary startup, every `--validate`
invocation), `plh_hub` checks whether the resolved
`hub.auth.keyfile` path lies inside `hub_dir`; if it does, the
binary prints the load-bearing `*** PYLABHUB SECURITY WARNING ***`
banner to stderr.  The warning quotes the operator's exact
`hub.auth.keyfile` value and recommends two outside-`hub_dir`
placements:

```
/etc/pylabhub/vault/<hub_uid>.vault   (system-managed service)
~/.pylabhub/vault/<hub_uid>.vault      (single-user deployment)
```

For production deployments, edit `hub.auth.keyfile` to an
absolute path outside `<hub-dir>` *before* running `--keygen` —
e.g. `/etc/pylabhub/vault/hub.main.uid3a7f2b1c.vault` for a
system-managed service, or
`/home/youruser/.pylabhub/vault/hub.main.uid3a7f2b1c.vault` for
a single-user deployment.  The warning then no longer fires.
(JSON does not shell-expand `~`; write the absolute form.)

The hub listens at `network.broker_endpoint`.  Producers,
consumers, and processors connect to this address; they read
`<hub-dir>/hub.pubkey` to pin the broker's CURVE key.

### 4.4 Hub script (optional) — Python or Lua

Hub-side scripts live at `<hub-dir>/script/python/__init__.py` (Python)
or `<hub-dir>/script/lua/init.lua` (Lua), selected by
`hub.json:script.type`.  Authoritative spec is HEP-CORE-0033 §12;
this is the operator-facing summary.

#### Callback signature conventions (engine-specific)

The two engines pass arguments differently:

- **Python** unpacks JSON arguments as **keyword arguments**.  The
  `api` object is set as a **module global** (not passed to most
  callbacks).  Script callbacks omit `api` from their signature:

  ```python
  def on_query_metrics(params, response):
      response["custom"] = compute()  # `api` resolved as module global
      return response
  ```

  Exception: `on_init(api)` and `on_stop(api)` receive `api` as a
  positional argument (matches the role-side convention).

- **Lua** passes JSON arguments as **one positional table arg**.
  The `api` table is set as a **Lua global**:

  ```lua
  function on_query_metrics(args)
      args.response.custom = compute()
      return args.response
  end
  ```

#### Available callbacks

```python
# ── Lifecycle ───────────────────────────────────────────────────────
def on_start(api):  pass        # api is positional here (per role-side convention)
def on_tick():      pass        # periodic, paced by hub.json:script.timing.period_ms
def on_stop(api):   pass

# ── Event observers (post-bookkeeping; HEP §12.2.1) ─────────────────
def on_channel_opened(channel_info):  pass
def on_channel_closed(name):          pass
def on_role_registered(role_info):    pass
def on_role_closed(role_info):        pass
def on_band_joined(band, member_uid): pass
def on_band_left(band, member_uid):   pass
def on_peer_connected(peer_uid):      pass
def on_peer_disconnected(peer_uid, reason): pass

# ── Response augmentation hooks (HEP §12.2.2) ────────────────────────
# Hub builds the default response, then calls these (if defined) on the
# worker thread; you mutate the response (or build a new one) and
# **return** it.  The engine captures the function's return value, NOT
# the post-call state of the response argument — mutating in place
# without returning is a footgun: the default response ships and your
# changes are silently lost.  See the explicit ✅/❌ examples in HEP
# §12.2.2.
def on_query_metrics(params, response):    return response
def on_list_roles(response):               return response
def on_get_channel(name, response):        return response
def on_peer_message(peer_uid, msg, response): return response  # deferred — needs HUB_TARGETED_ACK

# ── User-posted events (HEP §12.2.3) ─────────────────────────────────
# Triggered by api.post_event("foo", {...}) from any thread (script
# callback, out-of-band Flask handler, etc.).  Fires on_app_<name>.
def on_app_my_event(data): pass
```

#### `HubAPI` surface (HEP §12.3)

```
api.log(level, msg)                # "debug" / "info" / "warn" / "error"
api.uid()                          # hub instance uid
api.name()                         # hub display name
api.config()                       # full hub.json snapshot
api.metrics()                      # broker metrics snapshot
api.list_channels() / .get_channel(name)
api.list_roles() / .get_role(uid)
api.list_bands() / .get_band(name)
api.list_peers() / .get_peer(hub_uid)
api.query_metrics(categories=[])

api.close_channel(name)            # fire-and-forget through broker
api.broadcast_channel(channel, message, data="")
api.request_shutdown()

api.post_event(name, data={})      # enqueue on_app_<name> on worker
api.augment_timeout_ms()           # current cross-thread wait bound
api.set_augment_timeout(ms)        # override; -1=infinite, 0=non-blocking, >0=N ms
```

#### Threading model — important differences between Python and Lua

See HEP-CORE-0033 §12.4.1 for the full treatment.  Quick summary:

- **PythonEngine** is single-state, single-interpreter.  All callbacks
  run on the worker thread under one shared GIL; cross-thread
  `api.*` calls and out-of-band server threads (Flask, FastAPI, etc.)
  share the same module globals.  This is the supported path for
  custom HTTP endpoints (HEP §12.5).
- **LuaEngine** is dual-mode: `invoke` from a non-owner thread runs
  in a **child `lua_State`** (isolated globals — used for role-side
  parallel admin queries), but `invoke_returning` (augmentation
  hooks) always queues to the worker thread's primary state so
  `on_tick`-cached state is visible to augmentation hooks.  Lua
  does not support out-of-band servers (no thread-safe HTTP server
  ecosystem in LuaJIT).

#### Augmentation timeout

The admin/broker thread that calls into an augmentation hook blocks
for up to `api.augment_timeout_ms()` milliseconds before giving up
and shipping the default response.  Default is
`kDefaultAugmentTimeoutHeartbeats` × `kDefaultHeartbeatIntervalMs`
(currently 30s).  Override per-instance in `on_start`:

```python
def on_start(api):
    api.set_augment_timeout(-1)       # infinite — no admin-side timeout
    # or api.set_augment_timeout(5000) — 5 second cap
```

`-1` is the project-wide infinite-wait sentinel (matches
`SharedSpinLock::try_lock_for(-1)`); `0` means "non-blocking" (the
hook is effectively disabled — callbacks never get a chance to run).

#### Custom HTTP endpoints (Flask / FastAPI / aiohttp)

Hub scripts can spawn an in-process HTTP server in `on_start` to
expose custom REST endpoints that read hub state.  This is a
documented Python-only pattern (HEP §12.5 — "Out-of-band
script-managed services") built on the thread-safety guarantees of
HubAPI; no special hub support needed beyond what already ships.

The four-rule contract (HEP §12.5.2): no engine re-entry from the
HTTP handler, atomic-rebind for W → handler state hand-off,
thread-safe queue or `api.post_event` for handler → W flow,
spawn in `on_start` and shut down in `on_stop`.

Minimal recipe:

```python
import threading, queue
from flask import Flask, jsonify, request

_cache = {}                    # rebound atomically by on_tick
_cmd_q = queue.Queue()         # Flask → on_tick batch path
_app = None
_thread = None

def on_tick():
    # Drain Flask-issued commands.
    while True:
        try:    cmd = _cmd_q.get_nowait()
        except queue.Empty: break
        handle_cmd(cmd)
    # Recompute and atomically publish.
    global _cache
    _cache = build_cache(api)

def on_query_metrics(params, response):
    response["custom"] = _cache.get("custom", {})
    return response             # MUST return — see above

def _build_app():
    app = Flask(__name__)

    @app.get("/health")
    def health():
        return jsonify(_cache.get("custom", {}))

    @app.get("/roles")
    def roles():
        return jsonify(api.list_roles())   # thread-safe HubAPI call

    @app.post("/cmd")
    def cmd():
        _cmd_q.put(request.get_json())
        return "", 204

    # Or: synchronous "wake the worker now" via api.post_event
    @app.post("/process")
    def process():
        api.post_event("process_request", {"payload": request.get_json()})
        return "", 202

    return app

def on_start(api):
    global _app, _thread
    _app = _build_app()
    _thread = threading.Thread(
        target=lambda: _app.run(host="0.0.0.0", port=8080,
                                use_reloader=False, threaded=True),
        daemon=True)
    _thread.start()

def on_app_process_request(payload):
    # Runs on the worker thread with full script-state access.
    result = run_domain_logic(payload)
    # publish for /result endpoint to read, etc.
```

For production, prefer uvicorn/hypercorn (graceful shutdown via
`should_exit`) over Flask's dev server.  Lua does NOT support this
pattern — `lua_State` is single-threaded and there's no thread-safe
in-process HTTP ecosystem in LuaJIT.

---

## 5. Producer Setup

### 5.1 producer.json — field reference

A minimal producer directory:

```
producer-dir/
  producer.json
  script/
    python/
      __init__.py
```

> 🆕 **2026-05-21 — field names renamed `<field>` → `out_<field>`.**
> The canonical schema is now produced by `plh_role --init --role
> producer <producer-dir>` (the binary's own template is the source
> of truth).  Pre-update examples that used `hub_dir`/`channel`/
> `transport`/`shm.{...}` will fail to parse — all fields gained an
> `out_*` prefix (since producer writes one direction); the nested
> `shm` object was flattened to `out_shm_<field>`.  Run `plh_role
> --init` to generate a working template you can edit.

Example `producer.json` (matches current `--init` output):

```json
{
  "producer": {
    "name":      "MySensor",
    "uid":       "prod.mysensor.uid00000001",
    "log_level": "info",
    "auth":      { "keyfile": "vault/prod.mysensor.uid00000001.vault" }
  },

  "out_hub_dir":   "../hub",
  "out_channel":   "lab.sensors.temperature",
  "out_transport": "shm",
  "out_shm_enabled":    true,
  "out_shm_slot_count": 8,
  "out_shm_secret":     1111111111,

  "out_slot_schema": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },
  "out_flexzone_schema": null,

  "loop_timing":      "fixed_rate",
  "target_period_ms": 100,
  "checksum":         "enforced",
  "flexzone_checksum": true,

  "script":               { "type": "python", "path": "." },
  "stop_on_script_error": false
}
```

**SHM secret note (Audit B4):**  `--init` writes `out_shm_secret`
unset (defaults to 0).  Secret = 0 is a sentinel "SHM not configured"
— `build_tx_queue` will skip the SHM path.  Operators MUST set a
non-zero `out_shm_secret` for an SHM-transport producer.  The
consumer / processor must set a matching `in_shm_secret` on the
other end.

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `producer.name` | yes | — | Human name; used in UID (`PROD-{NAME}-{HEX}`) |
| `producer.uid` | no | generated | Override auto-generated UID |
| `producer.log_level` | no | `"info"` | `debug`/`info`/`warn`/`error` |
| `producer.auth.keyfile` | **YES** | `"vault/<role_uid>.vault"` (`--init` template default) | Required non-empty path string to the encrypted role vault.  Relative paths resolve against `<producer-dir>`.  Empty string / missing field / missing `auth` object → config-load error (HEP-CORE-0024 §3.4).  pylabhub is a vault: no in-memory CURVE mode.  See §4.3 for the symmetric hub-side warning when the resolved path is inside the role directory. |
| `hub_dir` | no† | — | Hub directory; reads `hub.json` + `hub.pubkey` |
| `broker` | no† | `"tcp://127.0.0.1:5570"` | Broker endpoint (alternative to `hub_dir`) |
| `broker_pubkey` | no | `""` | CurveZMQ broker public key (Z85, 40 chars) |
| `channel` | yes | — | Channel name to publish on |
| `loop_timing` | **yes** | — | `"max_rate"`, `"fixed_rate"`, `"fixed_rate_with_compensation"` |
| `target_period_ms` | cond. | — | Write loop period in ms; required for `fixed_rate`/`fixed_rate_with_compensation`; forbidden for `max_rate`. Mutually exclusive with `target_rate_hz`. |
| `target_rate_hz` | cond. | — | Write loop rate in Hz; alternative to `target_period_ms` |
| `checksum` | no | `"enforced"` | `"enforced"` (auto), `"manual"` (caller controls), `"none"` (skip) |
| `flexzone_checksum` | no | `true` | BLAKE2b checksum on flexzone writes (SHM only) |
| `transport` | no | `"shm"` | `"shm"` or `"zmq"` |
| `zmq_out_endpoint` | no† | — | ZMQ PUSH bind endpoint (required when `transport=zmq`) |
| `zmq_out_bind` | no | `true` | Bind (true) or connect (false) for ZMQ PUSH socket |
| `zmq_buffer_depth` | no | `64` | ZMQ send ring buffer depth (must be > 0) |
| `slot_schema` | yes‡ | — | Output slot layout.  Set `slot_schema.packing` to `"aligned"` (default) or `"packed"` — this single field drives the ctypes view in the script, the SHM slot layout, AND the ZMQ wire encoding. |
| `schema_id` | no‡ | — | Named schema from HEP-CORE-0016 (overrides inline `slot_schema`) |
| `flexzone_schema` | no | absent | Persistent flexzone layout; ignored for ZMQ transport |
| `shm.enabled` | no | `true` | Allocate SHM segment |
| `shm.slot_count` | yes§ | — | Ring buffer depth (number of slots) |
| `shm.reader_sync_policy` | no | `"sequential"` | `"sequential"` (FIFO, no data loss) or `"latest_only"` (skip to newest) |
| `shm.secret` | no | `0` | Shared secret for SHM name derivation |
| `inbox_schema` | no | absent | Inbox field list (enables inbox receive facility) |
| `inbox_endpoint` | no | auto | ZMQ ROUTER bind endpoint for inbox |
| `inbox_buffer_depth` | no | `64` | Inbox recv buffer depth (must be > 0) |
| `inbox_overflow_policy` | no | `"drop"` | `"drop"` or `"block"` |
| `script.type` | no | `"python"` | Script type |
| `script.path` | no | `"."` | Script directory; resolves `<path>/script/<type>/__init__.py` |
| `python_venv` | no | `""` | Virtual environment name; empty = base env (see §12) |

> **Packing note (producer):** set via `slot_schema.packing`, `flexzone_schema.packing`, and `inbox_schema.packing` (each defaulting to `"aligned"`).  There is no transport-level packing key; the old `zmq_packing` / `inbox_zmq_packing` were removed 2026-04-20 and the parser rejects them.

† Exactly one of `hub_dir` or `broker` is required.
‡ Exactly one of `slot_schema` or `schema_id` is required.
§ Required when `shm.enabled=true`.

### 5.2 Producer Python script

```python
# script/python/__init__.py

def on_init(api):
    api.log('info', f"Producer {api.uid()} starting on channel {api.channel()}")

def on_produce(tx, msgs, api) -> bool:
    # tx.slot   — ctypes struct, writable. Fill fields and return True to publish.
    # tx.fz     — flexzone ctypes struct (None if no flexzone configured)
    # msgs      — list of (sender: str, data: bytes) from ZMQ messaging
    # api       — ProducerAPI proxy (see §8.3)
    import time
    tx.slot.ts    = time.time()
    tx.slot.value = 42.0
    return True    # True/None = commit (publish); False = discard (loop continues)

def on_stop(api):
    api.log('info', "Producer stopping")
```

---

## 6. Consumer Setup

### 6.1 consumer.json — field reference

> 🆕 **2026-05-21 — field names renamed `<field>` → `in_<field>`.**
> Same kind of rename as producer (§5.1).  Consumer fields gained an
> `in_*` prefix; the `queue_type` field no longer exists (now
> `in_transport`); nested `shm` flattened to `in_shm_<field>`.  Use
> `plh_role --init --role consumer <consumer-dir>` for the canonical
> template.

Example `consumer.json` (matches current `--init` output):

```json
{
  "consumer": {
    "name":      "Logger",
    "uid":       "cons.logger.uid00000001",
    "log_level": "info",
    "auth":      { "keyfile": "vault/cons.logger.uid00000001.vault" }
  },

  "in_hub_dir":   "../hub",
  "in_channel":   "lab.sensors.temperature",
  "in_transport": "shm",
  "in_shm_enabled": true,
  "in_shm_secret":  1111111111,

  "in_slot_schema": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },

  "loop_timing": "max_rate",
  "checksum":    "enforced",

  "script":               { "type": "python", "path": "." },
  "stop_on_script_error": false
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `consumer.name` | yes | — | Human name; used in UID (`CONS-{NAME}-{HEX}`) |
| `consumer.uid` | no | generated | Override auto-generated UID |
| `consumer.log_level` | no | `"info"` | `debug`/`info`/`warn`/`error` |
| `consumer.auth.keyfile` | **YES** | `"vault/<role_uid>.vault"` (`--init` template default) | Required non-empty path string to the encrypted role vault.  Relative paths resolve against `<consumer-dir>`.  Empty string / missing field / missing `auth` object → config-load error (HEP-CORE-0024 §3.4). |
| `hub_dir` | no† | — | Hub directory; reads `hub.json` + `hub.pubkey` |
| `broker` | no† | `"tcp://127.0.0.1:5570"` | Broker endpoint |
| `broker_pubkey` | no | `""` | CurveZMQ broker public key (Z85, 40 chars) |
| `channel` | yes | — | Channel name to subscribe to |
| `loop_timing` | **yes** | — | `"max_rate"`, `"fixed_rate"`, `"fixed_rate_with_compensation"` |
| `target_period_ms` | cond. | — | Loop period in ms; required for fixed-rate policies; forbidden for `max_rate` |
| `target_rate_hz` | cond. | — | Loop rate in Hz; alternative to `target_period_ms` |
| `queue_type` | no | `"shm"` | `"shm"` (reads SHM ring) or `"zmq"` (ZMQ PULL from broker) |
| `slot_schema` | yes‡ | — | Expected input slot layout (must match producer schema) |
| `schema_id` | no‡ | — | Named schema from HEP-CORE-0016 |
| `checksum` | no | `"enforced"` | `"enforced"` (auto verify), `"manual"` (caller controls), `"none"` (skip) |
| `flexzone_checksum` | no | `true` | Verify flexzone checksum on read (SHM only) |
| `stop_on_script_error` | no | `false` | Halt consumer if `on_consume` raises an exception |
| `flexzone_schema` | no | absent | Input flexzone layout (SHM only; zero-copy read) |
| `zmq_buffer_depth` | no | `64` | Internal recv-ring buffer depth for ZMQ transport (must be > 0) |
| `shm.enabled` | no | `true` | Attach to producer's SHM segment (`queue_type=shm`) |
| `shm.secret` | no | `0` | Shared secret matching the producer's `shm.secret` |
| `inbox_schema` | no | absent | Inbox field list (enables inbox receive facility) |
| `inbox_endpoint` | no | auto | ZMQ ROUTER bind endpoint for inbox |
| `inbox_buffer_depth` | no | `64` | Inbox recv buffer depth (must be > 0) |
| `inbox_overflow_policy` | no | `"drop"` | `"drop"` or `"block"` |
| `script.type` | no | `"python"` | Script type |
| `script.path` | no | `"."` | Script directory |
| `python_venv` | no | `""` | Virtual environment name; empty = base env (see §12) |

> **Packing note (consumer):** same rule as producer — set via schema `.packing` fields (`slot_schema.packing` / `flexzone_schema.packing` / `inbox_schema.packing`).  No transport-level packing key.

† Exactly one of `hub_dir` or `broker` is required.
‡ Exactly one of `slot_schema` or `schema_id` is required.

### 6.2 Consumer Python script

```python
def on_init(api):
    api.log('info', f"Consumer {api.uid()} starting on channel {api.channel()}")

def on_consume(rx, msgs, api):
    # rx.slot  — ctypes struct (read-only view). None on timeout.
    # rx.fz    — flexzone ctypes struct (None if no flexzone configured or ZMQ transport)
    # msgs     — list of (sender: str, data: bytes)
    # api      — ConsumerAPI proxy
    if rx.slot is None:
        return True  # timeout
    print(f"ts={rx.slot.ts:.3f}  value={rx.slot.value}")
    return True

def on_stop(api):
    api.log('info', "Consumer stopping")
```

---

## 7. Processor Setup

### 7.1 processor.json — field reference

A processor reads from one channel and writes to another. It can span two independent
hubs (`in_hub_dir` / `out_hub_dir`) for cross-hub bridging.

> 🆕 **2026-05-21 — nested `shm.in` / `shm.out` flattened to
> top-level `in_shm_<field>` / `out_shm_<field>`.**  Use `plh_role
> --init --role processor <processor-dir>` for the canonical
> template.  Same SHM-secret note as producer / consumer — secret = 0
> is a sentinel "no SHM"; operators MUST set matching non-zero
> secrets on each side of the pipeline (producer's `out_shm_secret`
> ↔ processor's `in_shm_secret`; processor's `out_shm_secret` ↔
> consumer's `in_shm_secret`).

Example `processor.json` (matches current `--init` output):

```json
{
  "processor": {
    "name":      "Normaliser",
    "uid":       "proc.normaliser.uid00000001",
    "log_level": "info",
    "auth":      { "keyfile": "vault/proc.normaliser.uid00000001.vault" }
  },

  "in_hub_dir":  "../hub",
  "out_hub_dir": "../hub",
  "in_channel":  "lab.sensors.temperature",
  "out_channel": "lab.processed.temperature",

  "in_transport":  "shm",
  "out_transport": "shm",
  "in_shm_enabled":     true,
  "in_shm_secret":      1111111111,
  "out_shm_enabled":    true,
  "out_shm_slot_count": 8,
  "out_shm_secret":     2222222222,

  "in_slot_schema": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },
  "out_slot_schema": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",         "type": "float64"},
      {"name": "value_norm", "type": "float64"}
    ]
  },
  "out_flexzone_schema": null,

  "loop_timing":       "max_rate",
  "checksum":          "enforced",
  "flexzone_checksum": true,

  "script":               { "type": "python", "path": "." },
  "stop_on_script_error": false
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `processor.name` | yes | — | Human name; used in UID (`PROC-{NAME}-{HEX}`) |
| `processor.uid` | no | generated | Override auto-generated UID |
| `processor.log_level` | no | `"info"` | `debug`/`info`/`warn`/`error` |
| `hub_dir` | no† | — | Hub directory for both input and output directions |
| `in_hub_dir` | no† | — | Per-direction override for input broker |
| `out_hub_dir` | no† | — | Per-direction override for output broker |
| `broker` | no† | `"tcp://127.0.0.1:5570"` | Broker endpoint |
| `in_broker` | no† | — | Per-direction input broker endpoint |
| `out_broker` | no† | — | Per-direction output broker endpoint |
| `in_channel` | yes | — | Input channel name |
| `out_channel` | yes | — | Output channel name |
| `loop_timing` | **yes** | — | `"max_rate"`, `"fixed_rate"`, `"fixed_rate_with_compensation"` |
| `target_period_ms` | cond. | — | Loop period in ms; required for fixed-rate policies; forbidden for `max_rate` |
| `target_rate_hz` | cond. | — | Loop rate in Hz; alternative to `target_period_ms` |
| `in_transport` | no | `"shm"` | `"shm"` or `"zmq"` (direct ZMQ PULL; endpoint from broker) |
| `out_transport` | no | `"shm"` | `"shm"` or `"zmq"` (direct ZMQ PUSH) |
| `zmq_in_endpoint` | no† | — | ZMQ PULL endpoint (required when `in_transport=zmq` + direct mode) |
| `zmq_out_endpoint` | no† | — | ZMQ PUSH bind endpoint (required when `out_transport=zmq`) |
| `zmq_in_bind` | no | `false` | Connect (false) or bind (true) the PULL socket |
| `zmq_out_bind` | no | `true` | Bind (true) or connect (false) the PUSH socket |
| `in_zmq_buffer_depth` | no | `64` | PULL recv ring buffer depth |
| `out_zmq_buffer_depth` | no | `64` | PUSH send ring buffer depth |
| `overflow_policy` | no | `"block"` | Output overflow policy: `"block"` or `"drop"` |
| `checksum` | no | `"enforced"` | `"enforced"` (auto), `"manual"` (caller controls), `"none"` (skip). Applies to both input verification and output computation. |
| `flexzone_checksum` | no | `true` | Flexzone checksum on output writes (SHM only) |
| `stop_on_script_error` | no | `false` | Halt processor if `on_process` raises an exception |
| `in_slot_schema` | yes‡ | — | Input slot layout |
| `out_slot_schema` | yes‡ | — | Output slot layout |
| `in_schema_id` | no‡ | — | Named schema for input (HEP-CORE-0016) |
| `out_schema_id` | no‡ | — | Named schema for output (HEP-CORE-0016) |
| `flexzone_schema` | no | absent | Output flexzone layout (SHM only) |
| `shm.in.enabled` | no | `true` | Attach to producer's SHM segment |
| `shm.in.secret` | no | `0` | Shared secret matching the input producer |
| `shm.out.enabled` | no | `true` | Allocate output SHM segment |
| `shm.out.slot_count` | yes§ | — | Output ring buffer depth |
| `shm.out.reader_sync_policy` | no | `"sequential"` | `"sequential"` (FIFO, no data loss) or `"latest_only"` (skip to newest) |
| `shm.out.secret` | no | `0` | Shared secret for output SHM |
| `inbox_schema` | no | absent | Inbox field list (enables inbox receive facility) |
| `inbox_endpoint` | no | auto | ZMQ ROUTER bind endpoint for inbox |
| `inbox_buffer_depth` | no | `64` | Inbox recv buffer depth (must be > 0) |
| `inbox_overflow_policy` | no | `"drop"` | `"drop"` or `"block"` |
| `script.type` | no | `"python"` | Script type |
| `script.path` | no | `"."` | Script directory |
| `python_venv` | no | `""` | Virtual environment name; empty = base env (see §12) |

> **Packing note (processor):** each of the 4 schemas (`in_slot_schema`, `out_slot_schema`, `flexzone_schema`, `inbox_schema`) carries its own `.packing` field (each defaulting to `"aligned"`).  No transport-level packing key — the old `in_zmq_packing` / `out_zmq_packing` / `inbox_zmq_packing` were removed 2026-04-20.

† Exactly one of `hub_dir`, `in_hub_dir`/`out_hub_dir`, `broker`, or `in_broker`/`out_broker`
  combination is required per direction.
‡ Exactly one of the inline `_schema` block or `_schema_id` string is required per side.
§ Required when `shm.out.enabled=true`.

### 7.2 Processor Python script

```python
def on_init(api):
    api.log('info', f"Processor {api.uid()} ready")

def on_process(rx, tx, msgs, api) -> bool:
    # rx.slot   — ctypes struct, read-only view of input slot (None on timeout)
    # tx.slot   — ctypes struct, writable view of output slot (None on timeout)
    # tx.fz     — output flexzone ctypes struct (None if not configured or ZMQ out)
    # msgs      — list of (sender: str, data: bytes) inbox/ZMQ messages
    # api       — ProcessorAPI proxy
    if rx.slot is None:
        return False   # timeout — discard output slot
    tx.slot.ts         = rx.slot.ts
    tx.slot.value_norm = rx.slot.value / 100.0
    return True        # True/None = commit; False = discard

def on_stop(api):
    api.log('info', "Processor stopping")
```

---

## 8. Python Script Reference

### 8.1 Script package structure

All roles use the same resolution rule:

```
<script.path>/script/<script.type>/__init__.py
```

With default `"path": "."`, the script is at `./script/python/__init__.py` relative to
the role directory. The directory `./script/python/` is added to `sys.path` before the
module is imported.

### 8.2 Slot types — ctypes field mapping

Slot fields are mapped to Python `ctypes` types in `__init__.py`:

| schema type | ctypes field |
|-------------|-------------|
| `int8` / `uint8` | `c_int8` / `c_uint8` |
| `int16` / `uint16` | `c_int16` / `c_uint16` |
| `int32` / `uint32` | `c_int32` / `c_uint32` |
| `int64` / `uint64` | `c_int64` / `c_uint64` |
| `float32` | `c_float` |
| `float64` | `c_double` |
| `float32 × N` (count > 1) | `c_float * N` (array) |

Access array fields:

```python
samples = rx.slot.samples          # ctypes array
values  = list(rx.slot.samples)    # Python list (copy)
arr = api.as_numpy(rx.slot.samples)  # zero-copy numpy view (dtype inferred)
```

For the output slot, write directly:

```python
tx.slot.samples[:] = [x * 2 for x in rx.slot.samples]
# or with numpy:
api.as_numpy(tx.slot.samples)[:] = source_array
```

`api.as_numpy(field)` returns a zero-copy `numpy.ndarray` view of a ctypes array field,
with dtype inferred automatically. Available on all role APIs.

### 8.3 API methods — all roles

> 🆕 **2026-05-21 — surface aligned with current bindings.**  Pre-update
> revisions of this section claimed `api.notify_channel` /
> `api.broadcast_channel` (R3.6 removed; broker no longer handles
> `CHANNEL_NOTIFY_REQ`), `api.list_channels` / `api.shm_blocks` (not
> bound to role-side scripts today — Hub State Query Layer design
> in `docs/tech_draft/hub_state_query_layer_design.md` will land
> these as `hub.snapshot()`), `api.broadcast(data)` / `api.send` /
> `api.consumers()` (peer-messaging via the old P2C ctrl socket;
> M4f removed that socket — replacement is the HEP-CORE-0027 inbox
> messaging shown below), and `rx.fz` for consumers (the binding
> only exposes `rx.slot` — consumers should use `api.flexzone()`
> instead; Audit B6).  This revision removes those.

```python
# Identity
api.uid()            # → str: e.g. "prod.mysensor.uid12345678"
api.name()           # → str: human name from config
api.channel()        # → str: output channel (producer/processor-out)
                     #         OR input channel (consumer/processor-in)
api.log_level()      # → str: "debug"/"info"/"warn"/"error"
api.script_dir()     # → str: absolute path to the script directory
api.role_dir()       # → str: absolute path to the role directory
api.logs_dir()       # → str: role_dir + "/logs"
api.run_dir()        # → str: role_dir + "/run"
api.log(level, msg)  # write to the role's logger

# Shutdown control
api.stop()                   # request clean shutdown from inside a callback
api.set_critical_error()     # mark as failed and trigger shutdown
api.critical_error()         # → bool

# Band messaging (HEP-CORE-0030 — supersedes the retired
# `notify_channel` / `broadcast_channel` / `broadcast` peer surface)
api.band_join(band)          # join a band (broker is authoritative)
api.band_leave(band)         # leave a band
api.band_broadcast(band, payload, data=b"")
                             #   send a message to every band member
api.band_members(band)       # → list of role_uids currently in the band
api.is_in_band(band)         # → bool

# Custom metrics + diagnostics (HEP-CORE-0019)
api.report_metric(key, value)     # accumulate one scalar
api.report_metrics(dict)          # accumulate many at once
api.clear_custom_metrics()        # clear the accumulator
api.metrics()                     # → dict: full hierarchical snapshot.
                                  #   See HEP-CORE-0019 §5.4.1 for the
                                  #   canonical field list and §5.4.2
                                  #   for direct accessors that skip
                                  #   the dict-build cost.
api.last_cycle_work_us()          # → int µs (current cycle's Python work)
api.loop_overrun_count()          # → int (producer fixed-rate misses)
api.script_error_count()          # → int

# Queue counters — producer / processor-out side
api.out_slots_written()
api.out_drop_count()
api.out_capacity()
api.out_policy()

# Queue counters — consumer / processor-in side
api.in_slots_received()
api.last_seq()
api.in_capacity()
api.in_policy()
api.set_verify_checksum(enable)   # only meaningful with checksum="manual"

# Spinlocks (SHM transport only)
api.spinlock(idx)            # → context manager; GIL released during wait
api.spinlock_count()         # → int (0 for ZMQ)

# Schema introspection
api.slot_logical_size(side=None)     # → int bytes (logical C struct size)
api.flexzone_logical_size(side=None) # → int bytes (0 if no flexzone)
                                     # `side` arg required for processor
                                     # (api.Tx or api.Rx); omit for
                                     # single-direction roles
api.stop_reason()                    # → str: 'running', 'channel_closed',
                                     # 'hub_dead', 'script_requested',
                                     # 'critical_error', 'sigterm', etc.
api.clear_inbox_cache()              # drop cached InboxClient handles
                                     # (forces re-open on next open_inbox)

# Flexzone (HEP-CORE-0019 §8.4 of this doc + HEP-0011 §"Flexzone")
api.flexzone(side=None)      # → ctypes struct or None.
                             #   Single-direction roles (producer /
                             #   consumer) omit the `side` arg.
                             #   PROCESSOR REQUIRES `side` (api.Tx or
                             #   api.Rx) — it has both directions.
                             #   Audit B7.
api.update_flexzone_checksum(side=None)
                             # call after writing flexzone fields
                             # (producer / processor-Tx side only)

# Numpy zero-copy view of an array slot field
api.as_numpy(field)          # → numpy.ndarray view; dtype inferred

# Inbox — typed peer-to-peer messages (HEP-CORE-0027)
api.open_inbox(target_uid)   # → InboxHandle or None
api.wait_for_role(uid, timeout_ms=5000)  # poll broker; GIL released
```

**Cross-reference: bottleneck metrics for diagnostics.**  When
tuning throughput, the relevant subset is documented in HEP-CORE-0019
§5.4.3 as a symptom → bottleneck table — start there before
sprinkling `api.report_metric` calls.

### 8.4 Flexzone — persistent shared data

The flexzone is a persistent region of the SHM segment alongside the ring buffer.
It stores configuration or calibration data visible to all consumers. Define it in
`producer.json`:

```json
"flexzone_schema": {
  "fields": [
    {"name": "scale",  "type": "float64"},
    {"name": "offset", "type": "float64"}
  ]
}
```

In the script:

```python
def on_init(api):
    fz = api.flexzone()   # persistent ctypes struct
    fz.scale  = 1.0
    fz.offset = 0.0
    api.update_flexzone_checksum()

def on_produce(tx, msgs, api):
    tx.slot.value = raw * tx.fz.scale + tx.fz.offset
    return True
```

Consumers see the flexzone as `rx.fz` (zero-copy read-only view).

### 8.5 Role-to-role messaging (HEP-CORE-0027 inbox + HEP-CORE-0030 band)

> 🆕 **2026-05-21 — this section rewritten.**  Pre-update revisions
> described `api.broadcast(data)` / `api.send(identity, data)` /
> `api.consumers()` for "ZMQ peer messaging via the broker's P2C
> ctrl socket".  Wave-B M4f (2026-05-16) deleted that ctrl socket;
> the replacement is now two distinct mechanisms:
>
> 1. **Inbox messaging (HEP-CORE-0027)** — typed point-to-point.
>    Sender knows the receiver's role_uid.  Each role can declare
>    an inbox with a schema; peers `open_inbox(uid)` and send slot-
>    shaped messages.  Documented in §8.6 below.
>
> 2. **Band messaging (HEP-CORE-0030)** — broadcast within a
>    membership group.  Roles `band_join(name)` (name must start
>    with `!`); broker is authoritative on membership; broadcasts
>    fan out to every band member except the sender.  Use this
>    for coordination signals (e.g. "drain" in the single-hub demo).
>
> Pick inbox when sender knows receiver's identity; pick band
> when the sender just wants "everyone subscribed should hear
> this."

#### Band quick example

```python
SHUTDOWN_BAND = "!demo.shutdown"
_joined = False

def on_init(api):
    # CANNOT call api.band_join here — handler not yet up.
    # See HEP-CORE-0011 §"API availability per callback".
    pass

def on_produce(tx, msgs, api):
    global _joined
    if not _joined:
        res = api.band_join(SHUTDOWN_BAND)
        _joined = (res is not None and res.get("status") == "success")
    # ... normal slot work ...
    return True

def on_band_message(band, sender, body, api):
    if band == SHUTDOWN_BAND and body.get("cmd") == "drain":
        api.log("info", f"drain from {sender} — stopping")
        api.stop()
```

A different role broadcasts:

```python
api.band_broadcast("!demo.shutdown", {"cmd": "drain"})
```

`band_broadcast` takes a Python `dict` (serialised as JSON on the
wire); `on_band_message`'s `body` arrives as a Python dict.

### 8.6 Inbox — typed peer-to-peer messages

Any role can declare an inbox to receive structured typed messages from peers. Configure
with flat `inbox_schema` / `inbox_endpoint` fields in the JSON. A peer sends to your inbox:

```python
# In a producer/processor script, open an inbox connection to the processor
def on_init(api):
    # Wait until the target processor is registered
    api.wait_for_role("PROC-NORMALISER-A1B2C3D4", timeout_ms=10000)
    handle = api.open_inbox("PROC-NORMALISER-A1B2C3D4")
    if handle is None:
        api.log("error", "Processor inbox not available")
        api.set_critical_error()
        return
    slot = handle.acquire()   # → writable ctypes struct
    slot.cmd = 1
    rc = handle.send(timeout_ms=5000)  # 0 = OK
    handle.discard()          # or just let GC handle it
```

On the receiving side (`on_inbox` callback, optional):

```python
def on_inbox(msg, api):
    api.log('info', f"Inbox from {msg.sender_uid}: cmd={msg.data.cmd}")
```

### 8.7 Error handling patterns

```python
# Graceful shutdown on unrecoverable error
def on_produce(tx, msgs, api):
    try:
        result = do_measurement()
    except DeviceError as e:
        api.log('error', f"Device failed: {e}")
        api.set_critical_error()   # triggers shutdown
        return False

# Timeout handling
def on_consume(rx, msgs, api):
    if rx.slot is None:
        api.log('warn', "No data for 5 seconds")
        return True   # no-op on timeout
    return True

# Tracking gaps in sequence (consumer only)
_prev_seq = None
def on_consume(rx, msgs, api):
    global _prev_seq
    if rx.slot is None: return True
    seq = api.last_seq()
    if _prev_seq is not None and seq != _prev_seq + 1:
        api.log('warn', f"Gap: expected {_prev_seq+1}, got {seq}")
    _prev_seq = seq
    return True
```

---

## 9. Security and Connection Policy

### 9.1 File permission model

| File | Permissions | Contents |
|------|-------------|----------|
| `hub.json` | 0644 (world-readable) | Non-secret config only |
| `<hub_uid>.vault` | 0600 (owner only) | Encrypted: CurveZMQ private key + admin token.  Filename embeds the hub UID per HEP-CORE-0033 §6.5 (revised 2026-05-31). |
| `hub.pubkey` | 0644 | Broker CurveZMQ public key (share with roles) |
| `<role_uid>.vault` | 0600 (owner only) | Encrypted: role CurveZMQ private key |

The admin token is generated by `plh_hub --keygen` (not by `--init`), stored in the
encrypted vault file, and injected at runtime after vault unlock. It is never written
to `hub.json`.

### 9.2 Production setup

> Finalized 2026-05-31.  The flow below uses `plh_hub --init` then
> `plh_hub --keygen` — two explicit operator steps.  The prior
> `pylabhub-hubshell --init` shortcut bundled both into one step
> and is gone.

```bash
# 1. Create hub directory + hub.json (does NOT create the vault).
plh_hub --init <hub-dir>/ --name "my-lab"

# 2. Provision the vault + publish hub.pubkey.  Operator chooses the
#    vault password via PYLABHUB_HUB_PASSWORD (env) or interactive prompt.
#    --keygen refuses to overwrite an existing vault file (HEP-CORE-0033
#    §7.1); rm the file first if you really want to re-key.
export PYLABHUB_HUB_PASSWORD="<choose a strong password>"
plh_hub --config <hub-dir>/hub.json --keygen

# 3. Distribute hub.pubkey to each role directory.
cp <hub-dir>/hub.pubkey <producer-dir>/

# 4. Create each role (generates <role>.json with auth.keyfile set).
plh_role --init --role producer  <producer-dir>/  --name "MySensor"
plh_role --init --role consumer  <consumer-dir>/  --name "MyLogger"
plh_role --init --role processor <processor-dir>/ --name "MyFilter"

# 5. Provision each role's vault.
export PYLABHUB_ROLE_PASSWORD="<choose a strong password>"
plh_role --role producer  --config <producer-dir>/producer.json   --keygen
plh_role --role consumer  --config <consumer-dir>/consumer.json   --keygen
plh_role --role processor --config <processor-dir>/processor.json --keygen

# 6. Set broker_pubkey in each role's JSON, or place hub.pubkey in the role dir.
# 7. Run the hub (PYLABHUB_HUB_PASSWORD unlocks the vault).
plh_hub <hub-dir>/

# 8. Run each role.
plh_role --role producer <producer-dir>/
# ... (etc.)
```

### 9.3 No development mode

pylabhub is a vault.  There is no `--dev` flag, no "ephemeral CURVE
keys" mode, no in-memory CURVE setup that skips the vault.  This was
finalized 2026-05-31 (HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1):
empty `auth.keyfile`, missing `auth` object, and missing
`auth.keyfile` field are all config-load errors.  The same
`plh_hub --init` → `plh_hub --keygen` → `plh_hub <hub-dir>/` flow
shown in §9.2 is used for dev / CI / smoke tests, with whatever
test-friendly password the operator chooses.

### 9.4 Connection policy

> 🚧 **Placeholder mechanism — not currently configurable from `hub.json`.**
> The four modes below describe the legacy `RoleIdentityPolicy` placeholder
> (renamed 2026-05-13 from `ConnectionPolicy`).  `HubBrokerConfig` deliberately
> omits the policy fields pending HEP-CORE-0035; the broker runs with
> `RoleIdentityPolicy::Open` in every production deployment regardless of what
> `hub.connection_policy` is set to.  When HEP-0035 lands, this entire section
> is replaced by the §5 design (CURVE-required + ZAP-based pubkey allowlist
> + federation-trust gate).  See HEP-CORE-0035 §1.5 for the current-state
> explanation and §1.6 for the implementation premise.

Controls which roles the broker accepts channel registrations from (set in `hub.json`
under `hub.connection_policy`):

| Value | Behaviour |
|-------|-----------|
| `"open"` | Any role connecting with a valid CurveZMQ identity is accepted (default) |
| `"tracked"` | All roles are logged; no enforcement |
| `"required"` | Roles must present a known UID |
| `"verified"` | Roles must be in `hub.known_roles[]` with matching UID and public key |

### 9.5 ZMQ socket lifecycle policy

**All ZMQ sockets in pyLabHub set `ZMQ_LINGER = 0` at creation.**

The default ZMQ linger is -1 (infinite): `zmq_close()` blocks until all queued
sends are delivered. In pyLabHub, this would cause hangs on shutdown — particularly
in Release builds where the ZMQ I/O thread has not yet processed a peer disconnect
before the socket is closed.

pyLabHub does not rely on socket linger for delivery guarantees. Shutdown is always
coordinated through explicit protocol messages (`CHANNEL_CLOSING_NOTIFY`,
fan-out atomic with channel teardown; see HEP-CORE-0007 §12 + HEP-CORE-0023 §2.1).
By the time any socket is closed, the counterpart has already
received the shutdown notification through the control path.

LINGER=0 is set at socket **creation** (not deferred to close time) so it takes
effect before any sends are queued. See §15 in `docs/HEP/HEP-CORE-0021` for the
full policy, rationale, and list of all socket creation sites.

---

## 10. Multi-Hub Pipelines

A processor can bridge two independent hubs using ZMQ transport on one side:

```
Hub A (port 5570)                             Hub B (port 5571)
Producer ─[SHM]─► Processor-A ─[ZMQ PUSH]──►
                               tcp://localhost:5580
                  Processor-B ─[ZMQ PULL]──► [SHM] ─► Consumer
```

**processor-a/processor.json** (SHM in, ZMQ out):
```json
{
  "in_hub_dir":     "../hub-a",
  "out_hub_dir":    "../hub-a",
  "in_channel":     "lab.a.raw",
  "out_channel":    "lab.a.bridge",
  "in_transport":   "shm",
  "out_transport":  "zmq",
  "zmq_out_endpoint": "tcp://127.0.0.1:5580",
  "shm": { "in": {"enabled": true, "secret": 3333333333},
            "out": {"enabled": false} }
}
```

**processor-b/processor.json** (ZMQ in, SHM out):
```json
{
  "in_hub_dir":    "../hub-b",
  "out_hub_dir":   "../hub-b",
  "in_channel":    "lab.b.incoming",
  "out_channel":   "lab.b.processed",
  "in_transport":  "zmq",
  "out_transport": "shm",
  "zmq_in_endpoint": "tcp://127.0.0.1:5580",
  "shm": { "in": {"enabled": false},
            "out": {"enabled": true, "secret": 4444444444, "slot_count": 8} }
}
```

See `share/py-demo-dual-processor-bridge/` for a complete working example.

---

## 11. Operational Reference

### Starting order

```bash
# 1. Hub(s) first.  PYLABHUB_HUB_PASSWORD must be set in the
#    environment; the binary uses it to unlock the vault (created
#    earlier by `plh_hub --keygen`; see §9.2).
export PYLABHUB_HUB_PASSWORD="<hub vault password>"
plh_hub hub/ &

# 2. Producer next (creates SHM)
plh_role --role producer producer/ --log-file logs/producer.log &
sleep 2.0   # wait for CurveZMQ handshake + SHM creation + broker registration

# 3. Processor after producer (attaches to SHM)
plh_role --role processor processor/ --log-file logs/processor.log &
sleep 2.0

# 4. Consumer last (or any order after producer)
plh_role --role consumer consumer/ --log-file logs/consumer.log
```

### Shutdown

Send `SIGTERM` or `SIGINT` to any role. The role sends `DISC_REQ` to the broker, waits
for `CHANNEL_CLOSING_NOTIFY` to drain consumers, then exits.

### Monitoring

```python
# In any callback, inspect live metrics
def on_produce(tx, msgs, api):
    m = api.metrics()
    if m.get('loop_overrun_count', 0) > 0:
        api.log('warn', f"Loop overruns: {m['loop_overrun_count']}")
    if m.get('drops', 0) > 0:
        api.log('warn', f"Drops: {m['drops']}")
    return True
```

### Log file redirection

All three role binaries accept `--log-file <path>` to redirect log output from the
console to a file. The sink switch happens early in the lifecycle — before ZMQContext
and DataExchangeHub emit their startup messages — via the `StartupLogFileSink` lifecycle
module (see HEP-CORE-0001 §StartupLogFileSink).

```bash
plh_role --role producer  producer/  --log-file logs/producer.log
plh_role --role consumer  consumer/  --log-file logs/consumer.log
plh_role --role processor processor/ --log-file logs/processor.log
```

The hub uses a rotating log file automatically when a hub directory is provided
(`<hub_dir>/logs/hub.log`, 10 MiB per file, 3 backups).

### Log level at runtime

Set `producer.log_level` (or equivalent for other roles) to `"debug"` for verbose output.
All log output goes to stderr via the async logger (or to the log file when `--log-file`
is specified).

### Common errors

| Error | Cause | Fix |
|-------|-------|-----|
| `[producer] REG_ACK: channel already registered` | Another producer already owns the channel | Use a unique channel name or stop the existing producer |
| `[consumer] CONSUMER_REG_ACK: schema mismatch` | Consumer schema doesn't match producer | Align `slot_schema` (or use shared `schema_id`) |
| `[consumer] CONSUMER_REG_ACK: transport mismatch` | Consumer queue_type≠producer transport | Set matching `queue_type`/`transport` on both sides |
| `[proc] Failed to create hub::Processor` | Could not attach to input or output SHM | Check SHM secrets and that producer started first |
| `Segmentation fault at SHM attach` | Mismatched schema sizes | Ensure identical `slot_schema` on producer and consumer/processor sides |

---

## 12. Python Environment and Virtual Environments

pyLabHub embeds a CPython interpreter in each role binary. The interpreter's base location
is resolved at runtime via a 3-tier priority chain; virtual environments overlay the base
with additional packages.

**Full specification:** `docs/HEP/HEP-CORE-0025-System-Config-and-Python-Environment.md`

### 12.1 Python home resolution (3-tier)

When a role binary starts, it resolves the Python installation directory:

| Priority | Source | Example |
|----------|--------|---------|
| 1 | `$PYLABHUB_PYTHON_HOME` environment variable | `export PYLABHUB_PYTHON_HOME=/usr/local` |
| 2 | `config/pylabhub.json` → `"python_home"` key | `{"python_home": "/usr/local"}` |
| 3 | `<prefix>/opt/python/` (standalone default) | Bundled python-build-standalone |

`<prefix>` is the installation root (parent of `bin/`). Relative paths in tiers 1–2 are
resolved relative to `<prefix>`.

**Developer builds** (CMake) use tier 3 automatically — the build downloads and stages
a python-build-standalone distribution to `opt/python/`.

**Pip-installed wheels** do not bundle the Python runtime (PyPI size limit). After
`pip install pylabhub`, run:

```bash
pylabhub prepare-runtime                        # downloads ~130 MB from GitHub
pylabhub prepare-runtime --from archive.tar.gz  # offline/air-gapped install
pylabhub prepare-runtime --target /opt/pylabhub/python  # custom location
```

**System Python** (FreeBSD or custom builds): create `config/pylabhub.json`:

```json
{
  "python_home": "/usr/local"
}
```

### 12.2 System configuration file — `config/pylabhub.json`

Located at `<prefix>/config/pylabhub.json`. Currently defines:

| Key | Type | Description |
|-----|------|-------------|
| `python_home` | string | Path to Python installation root (lib/ and bin/ parent) |

The file is optional — standalone builds work without it. Future keys may include
log defaults, license paths, or other system-wide settings.

### 12.3 Base environment

**Pip install** — the runtime is downloaded post-install:

```bash
pip install pylabhub
pylabhub prepare-runtime                        # downloads Python 3.14 runtime
plh_pyenv install                          # install base packages
plh_pyenv verify                           # check Python + pip + installed packages
```

**Developer build** — the runtime is staged automatically:

```bash
cmake --build build --target stage_all
plh_pyenv install --requirements share/scripts/python/requirements.txt
plh_pyenv verify
```

The base environment lives in `opt/python/` and provides packages available to all roles.

### 12.4 Virtual environments

Roles can use isolated virtual environments via the `python_venv` JSON config field.
The venv overlays the base environment — base packages remain available, and venv
packages take precedence.

**Create a venv:**

```bash
plh_pyenv create-venv myenv
plh_pyenv install --venv myenv -r my-requirements.txt
plh_pyenv verify --venv myenv
```

**Activate in a role config** (producer, consumer, or processor JSON):

```json
{
  "python_venv": "myenv",
  "script": {"type": "python", "path": "."}
}
```

At startup, the C++ interpreter activation calls `site.addsitedir()` to prepend the
venv's `site-packages` to `sys.path`. `PYTHONHOME` stays pointing at the base interpreter
(stdlib always works); the venv packages overlay and take precedence.

**Venv storage:** All venvs live under `<prefix>/opt/python/venvs/<name>/`. This path
is not configurable — user customization is via `requirements.txt`, not directory layout.

### 12.5 `plh_pyenv` tool reference

The `plh_pyenv` tool manages the bundled Python environment. It runs under the
bundled interpreter itself (or a system Python specified via `$PYLABHUB_PYTHON`).

| Command | Description |
|---------|-------------|
| `plh_pyenv install [-r FILE]` | Install packages into base env |
| `plh_pyenv install --venv NAME [-r FILE]` | Install packages into a venv |
| `plh_pyenv verify [--venv NAME]` | Verify Python + pip + packages |
| `plh_pyenv info [--venv NAME]` | Show Python version, paths, packages |
| `plh_pyenv freeze [--venv NAME]` | Print `pip freeze` output |
| `plh_pyenv create-venv NAME` | Create a new virtual environment |
| `plh_pyenv list-venvs` | List all virtual environments |
| `plh_pyenv remove-venv NAME` | Delete a virtual environment |

**Platform wrappers:**
- Linux/macOS: `bin/plh_pyenv` (bash)
- Windows: `bin/plh_pyenv.ps1` (PowerShell)

### 12.6 Cross-platform support

| Platform | Python source | `python_home` |
|----------|--------------|---------------|
| Linux x86_64 / aarch64 | `pylabhub prepare-runtime` (pip) or CMake (dev) | `opt/python/` (auto) |
| macOS x86_64 / arm64 | `pylabhub prepare-runtime` (pip) or CMake (dev) | `opt/python/` (auto) |
| Windows x86_64 | `pylabhub prepare-runtime` (pip) or CMake (dev) | `opt/python/` (auto) |
| FreeBSD / other | System package manager | Set in `config/pylabhub.json` |

When using system Python, ensure the version matches the build-time pybind11 expectations
(currently Python 3.10+). The `plh_pyenv verify` command checks version compatibility.
