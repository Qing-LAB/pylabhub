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

pyLabHub is a high-performance IPC framework for scientific data acquisition. It uses four
standalone binaries connected by a central broker (hub):

```
pylabhub-hubshell   — Broker process. Manages channel registry, SHM coordination,
                      control plane. One per independent data domain.

pylabhub-producer   — Data source. Writes slots into a named channel (SHM or ZMQ).
                      Runs user Python on_produce() callback per write cycle.

pylabhub-consumer   — Data sink. Reads slots from a named channel.
                      Runs user Python on_consume() callback per slot.

pylabhub-processor  — Data transformer. Reads from one channel, writes to another.
                      Runs user Python on_process(in_slot, out_slot) per input slot.
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
  pylabhub-hubshell     ← hub broker
  pylabhub-producer     ← producer role binary
  pylabhub-consumer     ← consumer role binary
  pylabhub-processor    ← processor role binary
  pylabhub-pyenv        ← Python environment manager (Unix)
  pylabhub-pyenv.py     ← Python environment manager (core)
  pylabhub-pyenv.ps1    ← Python environment manager (Windows)

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
  hub.json         ← hub configuration (0644 — world-readable; no secrets)
  hub.vault        ← encrypted vault: broker CurveZMQ private key + admin token (0600)
  hub.pubkey       ← broker CurveZMQ public key (written at startup; share with roles)
  schemas/         ← (optional) named schema registry root
    <ns>/<name>.v<N>.json
  script/          ← (optional) hub Python script package
    python/
      __init__.py  ← hub script callbacks (on_start, on_tick, on_stop, ...)
  logs/            ← log files written by the hub process
```

**File permissions**:
- `hub.json` — 0644 (world-readable). Contains only non-secret configuration.
- `hub.vault` — 0600 (owner-only). Encrypted with Argon2id + XSalsa20-Poly1305. Contains the CurveZMQ private key and admin token. Never readable without the vault password.
- `hub.pubkey` — 0644. The broker's CurveZMQ public key; copy to each role's config directory or set `broker_pubkey` in their JSON.

### 4.2 hub.json — field reference

`hub.json` is **world-readable (0644)**. It must never contain secrets. The admin token and
CurveZMQ private key live exclusively in `hub.vault` (0600, encrypted).

```json
{
  "hub": {
    "name":            "main",
    "uid":             "HUB-MAIN-3A7F2B1C",
    "description":     "pyLabHub instance",
    "broker_endpoint": "tcp://0.0.0.0:5570",
    "admin_endpoint":  "tcp://127.0.0.1:5600",
    "connection_policy": "open"
  },

  "broker": {
    "channel_timeout_s":          10,
    "consumer_liveness_check_s":   5,
    "channel_shutdown_grace_s":    5
  },

  "script": {
    "type":                   "python",
    "path":                   "./script",
    "tick_interval_ms":       1000,
    "health_log_interval_ms": 60000
  },

  "peers": []
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `hub.name` | no | `"local.hub.default"` | Human name; also used to auto-generate `hub.uid` |
| `hub.uid` | no | auto | Stable UID in `HUB-NAME-HEXSUFFIX` format; auto-generated from `hub.name` if absent |
| `hub.description` | no | `"pyLabHub instance"` | Human-readable description |
| `hub.broker_endpoint` | no | `"tcp://0.0.0.0:5570"` | ZMQ ROUTER bind address for the data broker |
| `hub.admin_endpoint` | no | `"tcp://127.0.0.1:5600"` | ZMQ ROUTER bind address for the admin shell |
| `hub.connection_policy` | no | `"open"` | Channel registration policy: `open`/`tracked`/`required`/`verified` |
| `broker.channel_timeout_s` | no | `10` | Seconds without heartbeat before channel is closed |
| `broker.consumer_liveness_check_s` | no | `5` | Consumer liveness check interval (0 = disabled) |
| `broker.channel_shutdown_grace_s` | no | `5` | Grace period between CHANNEL_CLOSING_NOTIFY and FORCE_SHUTDOWN |
| `script.type` | no | — | Script language; `"python"` is the only supported value |
| `script.path` | no | — | Base directory; hub script is at `<path>/<type>/__init__.py` |
| `script.tick_interval_ms` | no | `1000` | Callback interval for `on_tick()` |
| `script.health_log_interval_ms` | no | `60000` | Interval for periodic channel health log |
| `peers[]` | no | `[]` | Federation peer list (HEP-CORE-0022); see §4.5 |

**Security note**: `hub.json` must not contain `admin.token`. The admin token is stored in
`hub.vault` and injected at runtime by `pylabhub-hubshell` after vault unlock. Any
`admin.token` field found in `hub.json` is ignored with an error log.

### 4.3 Running the hub

```bash
# First-time setup — creates hub.json, hub.vault, script/python/__init__.py
pylabhub-hubshell --init <hub-dir>/ --name "my-lab-hub"

# Production mode — prompts for vault password, reads CurveZMQ key + admin token from vault
pylabhub-hubshell <hub-dir>/

# Development mode — ephemeral key pair, no password prompt, no vault needed
pylabhub-hubshell <hub-dir>/ --dev
```

The hub listens at `hub.broker_endpoint`. Producers, consumers, and processors connect to
this address. In dev mode, a fresh ephemeral CurveZMQ key pair is generated each run; roles
must either omit `broker_pubkey` or re-read `hub.pubkey` after each start.

### 4.4 Hub Python script (optional)

Place `<hub-dir>/script/python/__init__.py`. Callbacks:

```python
def on_start(api):              pass  # hub started
def on_channel_registered(api, channel_name, producer_uid): pass
def on_channel_deregistered(api, channel_name): pass
def on_hub_connected(api, peer_hub_name):    pass  # federation
def on_hub_disconnected(api, peer_hub_name): pass
def on_hub_message(api, peer_hub_name, topic, data): pass
def on_stop(api):               pass
```

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

Example `producer.json`:

```json
{
  "producer": {
    "name":      "MySensor",
    "log_level": "info"
  },

  "hub_dir":  "../hub",
  "channel":  "lab.sensors.temperature",

  "target_period_ms": 100,
  "transport":        "shm",

  "slot_schema": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },

  "shm": {
    "enabled":    true,
    "slot_count": 8,
    "secret":     0
  },

  "script": {"type": "python", "path": "."}
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `producer.name` | yes | — | Human name; used in UID (`PROD-{NAME}-{HEX}`) |
| `producer.uid` | no | generated | Override auto-generated UID |
| `producer.log_level` | no | `"info"` | `debug`/`info`/`warn`/`error` |
| `producer.auth.keyfile` | no | `""` | Vault file; empty = ephemeral CURVE identity |
| `hub_dir` | no† | — | Hub directory; reads `hub.json` + `hub.pubkey` |
| `broker` | no† | `"tcp://127.0.0.1:5570"` | Broker endpoint (alternative to `hub_dir`) |
| `broker_pubkey` | no | `""` | CurveZMQ broker public key (Z85, 40 chars) |
| `channel` | yes | — | Channel name to publish on |
| `target_period_ms` | no | `0` | Write loop period in ms; `0` = free-run |
| `loop_timing` | no | `"max_rate"` | `"max_rate"`, `"fixed_rate"`, `"fixed_rate_with_compensation"` |
| `slot_acquire_timeout_ms` | no | `-1` | `write_acquire()` timeout; `-1` = derive from period, `0` = non-blocking |
| `transport` | no | `"shm"` | `"shm"` or `"zmq"` |
| `zmq_out_endpoint` | no† | — | ZMQ PUSH bind endpoint (required when `transport=zmq`) |
| `zmq_out_bind` | no | `true` | Bind (true) or connect (false) for ZMQ PUSH socket |
| `zmq_buffer_depth` | no | `64` | ZMQ send ring buffer depth (must be > 0) |
| `zmq_packing` | no | `"aligned"` | `"aligned"` or `"packed"` |
| `slot_schema` | yes‡ | — | Output slot layout |
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
| `inbox_zmq_packing` | no | `"aligned"` | Packing for inbox messages |
| `script.type` | no | `"python"` | Script type |
| `script.path` | no | `"."` | Script directory; resolves `<path>/script/<type>/__init__.py` |
| `python_venv` | no | `""` | Virtual environment name; empty = base env (see §12) |

† Exactly one of `hub_dir` or `broker` is required.
‡ Exactly one of `slot_schema` or `schema_id` is required.
§ Required when `shm.enabled=true`.

### 5.2 Producer Python script

```python
# script/python/__init__.py

def on_init(api):
    api.log('info', f"Producer {api.uid()} starting on channel {api.channel()}")

def on_produce(out_slot, fz, msgs, api) -> bool:
    # out_slot  — ctypes struct, writable. Fill fields and return True to publish.
    # fz        — flexzone ctypes struct (None if no flexzone configured)
    # msgs      — list of (sender: str, data: bytes) from ZMQ messaging
    # api       — ProducerAPI proxy (see §8.3)
    import time
    out_slot.ts    = time.time()
    out_slot.value = 42.0
    return True    # True/None = commit (publish); False = discard (loop continues)

def on_stop(api):
    api.log('info', "Producer stopping")
```

---

## 6. Consumer Setup

### 6.1 consumer.json — field reference

Example `consumer.json`:

```json
{
  "consumer": {
    "name":      "Logger",
    "log_level": "info"
  },

  "hub_dir":     "../hub",
  "channel":     "lab.sensors.temperature",

  "queue_type":  "shm",
  "slot_acquire_timeout_ms":  -1,

  "slot_schema": {
    "packing": "aligned",
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },

  "shm": {
    "enabled": true,
    "secret":  0
  },

  "script": {"type": "python", "path": "."}
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `consumer.name` | yes | — | Human name; used in UID (`CONS-{NAME}-{HEX}`) |
| `consumer.uid` | no | generated | Override auto-generated UID |
| `consumer.log_level` | no | `"info"` | `debug`/`info`/`warn`/`error` |
| `consumer.auth.keyfile` | no | `""` | Vault file; empty = ephemeral CURVE identity |
| `hub_dir` | no† | — | Hub directory; reads `hub.json` + `hub.pubkey` |
| `broker` | no† | `"tcp://127.0.0.1:5570"` | Broker endpoint |
| `broker_pubkey` | no | `""` | CurveZMQ broker public key (Z85, 40 chars) |
| `channel` | yes | — | Channel name to subscribe to |
| `queue_type` | no | `"shm"` | `"shm"` (reads SHM ring) or `"zmq"` (ZMQ PULL from broker) |
| `slot_acquire_timeout_ms` | no | `-1` | Slot acquire timeout; `-1` = derive from period, `0` = non-blocking, `>0` = ms |
| `slot_schema` | yes‡ | — | Expected input slot layout (must match producer schema) |
| `schema_id` | no‡ | — | Named schema from HEP-CORE-0016 |
| `validation.verify_checksum` | no | `false` | Enable BLAKE2b slot verification on `read_acquire()` (SHM only) |
| `validation.stop_on_script_error` | no | `false` | Halt consumer if `on_consume` raises an exception |
| `flexzone_schema` | no | absent | Input flexzone layout (SHM only; zero-copy read) |
| `zmq_buffer_depth` | no | `64` | Internal recv-ring buffer depth for ZMQ transport (must be > 0) |
| `zmq_packing` | no | `"aligned"` | ZMQ frame packing: `"aligned"` or `"packed"` |
| `shm.enabled` | no | `true` | Attach to producer's SHM segment (`queue_type=shm`) |
| `shm.secret` | no | `0` | Shared secret matching the producer's `shm.secret` |
| `inbox_schema` | no | absent | Inbox field list (enables inbox receive facility) |
| `inbox_endpoint` | no | auto | ZMQ ROUTER bind endpoint for inbox |
| `inbox_buffer_depth` | no | `64` | Inbox recv buffer depth (must be > 0) |
| `inbox_overflow_policy` | no | `"drop"` | `"drop"` or `"block"` |
| `inbox_zmq_packing` | no | `"aligned"` | Packing for inbox messages |
| `script.type` | no | `"python"` | Script type |
| `script.path` | no | `"."` | Script directory |
| `python_venv` | no | `""` | Virtual environment name; empty = base env (see §12) |

† Exactly one of `hub_dir` or `broker` is required.
‡ Exactly one of `slot_schema` or `schema_id` is required.

### 6.2 Consumer Python script

```python
def on_init(api):
    api.log('info', f"Consumer {api.uid()} starting on channel {api.channel()}")

def on_consume(in_slot, fz, msgs, api):
    # in_slot  — ctypes struct (read-only view). None on timeout.
    # fz       — flexzone ctypes struct (None if no flexzone configured or ZMQ transport)
    # msgs     — list of (sender: str, data: bytes)
    # api      — ConsumerAPI proxy
    if in_slot is None:
        return  # timeout
    print(f"ts={in_slot.ts:.3f}  value={in_slot.value}")

def on_stop(api):
    api.log('info', "Consumer stopping")
```

---

## 7. Processor Setup

### 7.1 processor.json — field reference

A processor reads from one channel and writes to another. It can span two independent
hubs (`in_hub_dir` / `out_hub_dir`) for cross-hub bridging.

Example `processor.json`:

```json
{
  "processor": {
    "name":      "Normaliser",
    "log_level": "info"
  },

  "hub_dir":     "../hub",
  "in_channel":  "lab.sensors.temperature",
  "out_channel": "lab.processed.temperature",

  "in_transport":  "shm",
  "out_transport": "shm",
  "slot_acquire_timeout_ms":    -1,

  "in_slot_schema": {
    "fields": [
      {"name": "ts",    "type": "float64"},
      {"name": "value", "type": "float32"}
    ]
  },
  "out_slot_schema": {
    "fields": [
      {"name": "ts",         "type": "float64"},
      {"name": "value_norm", "type": "float64"}
    ]
  },

  "shm": {
    "in":  {"enabled": true,  "secret": 0},
    "out": {"enabled": true,  "slot_count": 8, "secret": 0}
  },

  "script": {"type": "python", "path": "."}
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
| `in_transport` | no | `"shm"` | `"shm"` or `"zmq"` (direct ZMQ PULL; endpoint from broker) |
| `out_transport` | no | `"shm"` | `"shm"` or `"zmq"` (direct ZMQ PUSH) |
| `zmq_in_endpoint` | no† | — | ZMQ PULL endpoint (required when `in_transport=zmq` + direct mode) |
| `zmq_out_endpoint` | no† | — | ZMQ PUSH bind endpoint (required when `out_transport=zmq`) |
| `zmq_in_bind` | no | `false` | Connect (false) or bind (true) the PULL socket |
| `zmq_out_bind` | no | `true` | Bind (true) or connect (false) the PUSH socket |
| `in_zmq_buffer_depth` | no | `64` | PULL recv ring buffer depth |
| `out_zmq_buffer_depth` | no | `64` | PUSH send ring buffer depth |
| `in_zmq_packing` | no | `"aligned"` | `"aligned"` or `"packed"` |
| `out_zmq_packing` | no | `"aligned"` | `"aligned"` or `"packed"` |
| `slot_acquire_timeout_ms` | no | `-1` | Input acquire timeout; `-1` = derive from period, `0` = non-blocking, `>0` = ms |
| `overflow_policy` | no | `"block"` | Output overflow policy: `"block"` or `"drop"` |
| `validation.verify_checksum` | no | `false` | Enable BLAKE2b verification on SHM input |
| `validation.update_checksum` | no | `true` | Write BLAKE2b checksum on SHM output |
| `validation.stop_on_script_error` | no | `false` | Halt processor if `on_process` raises an exception |
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
| `inbox_zmq_packing` | no | `"aligned"` | Packing for inbox messages |
| `script.type` | no | `"python"` | Script type |
| `script.path` | no | `"."` | Script directory |
| `python_venv` | no | `""` | Virtual environment name; empty = base env (see §12) |

† Exactly one of `hub_dir`, `in_hub_dir`/`out_hub_dir`, `broker`, or `in_broker`/`out_broker`
  combination is required per direction.
‡ Exactly one of the inline `_schema` block or `_schema_id` string is required per side.
§ Required when `shm.out.enabled=true`.

### 7.2 Processor Python script

```python
def on_init(api):
    api.log('info', f"Processor {api.uid()} ready")

def on_process(in_slot, out_slot, fz, msgs, api) -> bool:
    # in_slot   — ctypes struct, read-only view of input slot (None on timeout)
    # out_slot  — ctypes struct, writable view of output slot (None on timeout)
    # fz        — output flexzone ctypes struct (None if not configured or ZMQ out)
    # msgs      — list of (sender: str, data: bytes) inbox/ZMQ messages
    # api       — ProcessorAPI proxy
    if in_slot is None:
        return False   # timeout — discard output slot
    out_slot.ts         = in_slot.ts
    out_slot.value_norm = in_slot.value / 100.0
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
samples = in_slot.samples          # ctypes array
values  = list(in_slot.samples)    # Python list (copy)
import ctypes, numpy as np
arr = np.frombuffer((ctypes.c_float * 1024).from_buffer_copy(in_slot.samples),
                    dtype=np.float32)  # zero-copy numpy view of copy
```

For the output slot, write directly:

```python
out_slot.samples[:] = [x * 2 for x in in_slot.samples]
# or with numpy:
np.frombuffer(out_slot.samples, dtype=np.float32)[:] = source_array
```

### 8.3 API methods — all roles

```python
# Identity
api.uid()            # → str: e.g. "PROD-MYSENSOR-A1B2C3D4"
api.name()           # → str: human name from config
api.log_level()      # → str: "debug"/"info"/"warn"/"error"
api.script_dir()     # → str: absolute path to the script directory
api.role_dir()       # → str: absolute path to the role directory (empty if launched via --config)
api.logs_dir()       # → str: role_dir + "/logs" (empty if role_dir is empty)
api.run_dir()        # → str: role_dir + "/run"  (empty if role_dir is empty)
api.log(level, msg)  # write to hub logger

# Shutdown control
api.stop()                   # request clean shutdown from inside callback
api.set_critical_error()     # mark as failed and trigger shutdown (no argument)
api.critical_error()         # → bool: True if set_critical_error() was called

# Broker messaging
api.notify_channel(target_channel, event, data="")  # send event to channel producer
api.broadcast_channel(target_channel, message, data="")  # broadcast to all channel members
api.list_channels()          # → list of dicts [{name, status, schema_id, ...}]
api.shm_blocks(channel="")   # → dict with SHM block topology from broker

# Counters (all roles)
api.script_error_count()     # → int: number of Python exceptions in callbacks
api.loop_overrun_count()     # → int: producer: cycles past deadline; consumer/processor: always 0
api.last_cycle_work_us()     # → int: µs of active work in last callback invocation
api.metrics()                # → dict: full metrics snapshot (DataBlock + D4 counters + custom)

# Custom metrics (HEP-CORE-0019)
api.report_metric(key, value)     # report single {key: number}
api.report_metrics(dict)          # batch report {key: number} pairs
api.clear_custom_metrics()        # clear all custom metrics

# Queue metadata — producer
api.out_slots_written()      # → int
api.out_drop_count()         # → int
api.out_capacity()           # → int: ring buffer slot count (SHM) or send buffer depth (ZMQ)
api.out_policy()             # → str: overflow policy description

# Queue metadata — consumer
api.in_slots_received()      # → int
api.last_seq()               # → int: SHM=ring-buffer slot index (wraps); ZMQ=monotone wire seq
api.in_capacity()            # → int: ring buffer slot count (SHM) or recv buffer depth (ZMQ)
api.in_policy()              # → str: overflow policy info
api.set_verify_checksum(enable)  # toggle BLAKE2b slot verification at runtime (SHM only)

# Queue metadata — processor (both input and output sides)
api.in_slots_received()      # → int
api.out_slots_written()      # → int
api.out_drop_count()         # → int
api.last_seq()               # → int: SHM=ring-buffer slot index (wraps); ZMQ=monotone wire seq
api.in_capacity()  / api.in_policy()
api.out_capacity() / api.out_policy()
api.set_verify_checksum(enable)

# Spinlocks (SHM transport only)
api.spinlock(idx)            # → context manager; GIL released during lock wait
api.spinlock_count()         # → int: 0 for ZMQ transport

# Inbox — receive typed messages from another role
api.open_inbox(target_uid)   # → InboxHandle or None if target not online (cached)
api.wait_for_role(uid, timeout_ms=5000)  # → bool; polls broker with GIL released

# Flexzone (producer and processor only)
api.flexzone()               # → ctypes struct or None
api.update_flexzone_checksum()  # update BLAKE2b checksum after writing flexzone fields

# ZMQ peer messaging (producer and processor only)
api.broadcast(data)          # send bytes to all connected consumers
api.send(identity, data)     # send bytes to specific consumer ZMQ identity
api.consumers()              # → list of connected consumer identities
```

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

def on_produce(out_slot, fz, msgs, api):
    out_slot.value = raw * fz.scale + fz.offset
    return True
```

Consumers see the flexzone as the `fz` callback argument (zero-copy read-only view).

### 8.5 ZMQ messaging between roles

Producers and processors can send arbitrary bytes to connected consumers via the
broker's P2C ZMQ ctrl socket (control plane relay):

```python
def on_produce(out_slot, fz, msgs, api):
    # Broadcast to all connected consumers
    api.broadcast(b"heartbeat")

    # Send to one specific consumer
    for cid in api.consumers():
        api.send(cid, b"hello")

    # Receive messages sent by consumers
    for sender, data in msgs:
        print(f"from {sender}: {data}")
    return True
```

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
def on_inbox(inbox_slot, sender, api):
    api.log('info', f"Inbox from {sender}: cmd={inbox_slot.cmd}")
```

### 8.7 Error handling patterns

```python
# Graceful shutdown on unrecoverable error
def on_produce(out_slot, fz, msgs, api):
    try:
        result = do_measurement()
    except DeviceError as e:
        api.log('error', f"Device failed: {e}")
        api.set_critical_error()   # triggers shutdown
        return False

# Timeout handling
def on_consume(in_slot, fz, msgs, api):
    if in_slot is None:
        api.log('warn', "No data for 5 seconds")
        return   # no-op on timeout

# Tracking gaps in sequence (consumer only)
_prev_seq = None
def on_consume(in_slot, fz, msgs, api):
    global _prev_seq
    if in_slot is None: return
    seq = api.last_seq()
    if _prev_seq is not None and seq != _prev_seq + 1:
        api.log('warn', f"Gap: expected {_prev_seq+1}, got {seq}")
    _prev_seq = seq
```

---

## 9. Security and Connection Policy

### 9.1 File permission model

| File | Permissions | Contents |
|------|-------------|----------|
| `hub.json` | 0644 (world-readable) | Non-secret config only |
| `hub.vault` | 0600 (owner only) | Encrypted: CurveZMQ private key + admin token |
| `hub.pubkey` | 0644 | Broker CurveZMQ public key (share with roles) |
| `<role>.vault` | 0600 (owner only) | Encrypted: role CurveZMQ private key |

The admin token is generated by `--init`, stored in `hub.vault`, and injected at runtime
after vault unlock. It is never written to `hub.json`.

### 9.2 Production setup

```bash
# 1. Create hub (generates hub.json + hub.vault)
pylabhub-hubshell --init <hub-dir>/ --name "my-lab"

# 2. Copy hub.pubkey to each role directory (or reference it in broker_pubkey)
cp <hub-dir>/hub.pubkey <producer-dir>/

# 3. Create each role (generates role.json + role.vault)
pylabhub-producer  --init <producer-dir>/  --name "MySensor"
pylabhub-consumer  --init <consumer-dir>/  --name "MyLogger"
pylabhub-processor --init <processor-dir>/ --name "MyFilter"

# 4. Set broker_pubkey in each role's JSON, or place hub.pubkey in the role dir
# 5. Run the hub (prompts for vault password)
pylabhub-hubshell <hub-dir>/
```

### 9.3 Development mode

In dev mode all roles use ephemeral CurveZMQ key pairs; no vault or password is required:

```bash
pylabhub-hubshell <hub-dir>/ --dev
```

Roles without a vault still connect if `broker_pubkey` is set (ephemeral key, broker
authenticated). For mutual authentication (both sides verified), set `auth.keyfile` on
both the hub and each role.

### 9.4 Connection policy

Controls which roles the broker accepts channel registrations from (set in `hub.json`
under `hub.connection_policy`):

| Value | Behaviour |
|-------|-----------|
| `"open"` | Any role connecting with a valid CurveZMQ identity is accepted (default) |
| `"tracked"` | All roles are logged; no enforcement |
| `"required"` | Roles must present a known UID |
| `"verified"` | Roles must be in `hub.known_actors[]` with matching UID and public key |

### 9.5 ZMQ socket lifecycle policy

**All ZMQ sockets in pyLabHub set `ZMQ_LINGER = 0` at creation.**

The default ZMQ linger is -1 (infinite): `zmq_close()` blocks until all queued
sends are delivered. In pyLabHub, this would cause hangs on shutdown — particularly
in Release builds where the ZMQ I/O thread has not yet processed a peer disconnect
before the socket is closed.

pyLabHub does not rely on socket linger for delivery guarantees. Shutdown is always
coordinated through explicit protocol messages (`CHANNEL_CLOSING_NOTIFY`,
`FORCE_SHUTDOWN`). By the time any socket is closed, the counterpart has already
received a shutdown notification through the control path.

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
# 1. Hub(s) first
pylabhub-hubshell hub/ --dev &

# 2. Producer next (creates SHM)
pylabhub-producer producer/ --log-file logs/producer.log &
sleep 2.0   # wait for CurveZMQ handshake + SHM creation + broker registration

# 3. Processor after producer (attaches to SHM)
pylabhub-processor processor/ --log-file logs/processor.log &
sleep 2.0

# 4. Consumer last (or any order after producer)
pylabhub-consumer consumer/ --log-file logs/consumer.log
```

### Shutdown

Send `SIGTERM` or `SIGINT` to any role. The role sends `DISC_REQ` to the broker, waits
for `CHANNEL_CLOSING_NOTIFY` to drain consumers, then exits.

### Monitoring

```python
# In any callback, inspect live metrics
def on_produce(out_slot, fz, msgs, api):
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
pylabhub-producer  producer/  --log-file logs/producer.log
pylabhub-consumer  consumer/  --log-file logs/consumer.log
pylabhub-processor processor/ --log-file logs/processor.log
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
pylabhub-pyenv install                          # install base packages
pylabhub-pyenv verify                           # check Python + pip + installed packages
```

**Developer build** — the runtime is staged automatically:

```bash
cmake --build build --target stage_all
pylabhub-pyenv install --requirements share/scripts/python/requirements.txt
pylabhub-pyenv verify
```

The base environment lives in `opt/python/` and provides packages available to all roles.

### 12.4 Virtual environments

Roles can use isolated virtual environments via the `python_venv` JSON config field.
The venv overlays the base environment — base packages remain available, and venv
packages take precedence.

**Create a venv:**

```bash
pylabhub-pyenv create-venv myenv
pylabhub-pyenv install --venv myenv -r my-requirements.txt
pylabhub-pyenv verify --venv myenv
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

### 12.5 `pylabhub-pyenv` tool reference

The `pylabhub-pyenv` tool manages the bundled Python environment. It runs under the
bundled interpreter itself (or a system Python specified via `$PYLABHUB_PYTHON`).

| Command | Description |
|---------|-------------|
| `pylabhub-pyenv install [-r FILE]` | Install packages into base env |
| `pylabhub-pyenv install --venv NAME [-r FILE]` | Install packages into a venv |
| `pylabhub-pyenv verify [--venv NAME]` | Verify Python + pip + packages |
| `pylabhub-pyenv info [--venv NAME]` | Show Python version, paths, packages |
| `pylabhub-pyenv freeze [--venv NAME]` | Print `pip freeze` output |
| `pylabhub-pyenv create-venv NAME` | Create a new virtual environment |
| `pylabhub-pyenv list-venvs` | List all virtual environments |
| `pylabhub-pyenv remove-venv NAME` | Delete a virtual environment |

**Platform wrappers:**
- Linux/macOS: `bin/pylabhub-pyenv` (bash)
- Windows: `bin/pylabhub-pyenv.ps1` (PowerShell)

### 12.6 Cross-platform support

| Platform | Python source | `python_home` |
|----------|--------------|---------------|
| Linux x86_64 / aarch64 | `pylabhub prepare-runtime` (pip) or CMake (dev) | `opt/python/` (auto) |
| macOS x86_64 / arm64 | `pylabhub prepare-runtime` (pip) or CMake (dev) | `opt/python/` (auto) |
| Windows x86_64 | `pylabhub prepare-runtime` (pip) or CMake (dev) | `opt/python/` (auto) |
| FreeBSD / other | System package manager | Set in `config/pylabhub.json` |

When using system Python, ensure the version matches the build-time pybind11 expectations
(currently Python 3.10+). The `pylabhub-pyenv verify` command checks version compatibility.
