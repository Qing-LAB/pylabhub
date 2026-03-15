# HEP-CORE-0014: Actor Framework Design

| Property      | Value                                                                |
|---------------|----------------------------------------------------------------------|
| **HEP**       | `HEP-CORE-0014`                                                      |
| **Title**     | Actor Framework Design                                               |
| **Status**    | Implemented (2026-02-28)                                             |
| **Created**   | 2026-02-28                                                           |
| **Area**      | Actor Framework (`pylabhub-actor`)                                   |
| **Depends on**| HEP-CORE-0002 (DataHub), HEP-CORE-0007 (Protocol), HEP-CORE-0008 (LoopPolicy), HEP-CORE-0009 (ConnectionPolicy), HEP-CORE-0010 (Thread Model), HEP-CORE-0011 (ScriptHost), HEP-CORE-0013 (Channel Identity) |
| **Supersedes**| `docs/tech_draft/ACTOR_DESIGN.md` (archived 2026-02-28)             |

---

## Abstract

This HEP defines the **actor framework API** for `pylabhub-actor`: the configuration format,
Python script interface, C++ class architecture, authentication model, schema validation
strategy, and failure model.

The actor framework provides a multi-role, multi-channel IPC actor in which a Python script
drives data production and consumption via typed zero-copy SHM access. Each role runs as a
pair of C++ threads (loop thread + ZMQ thread); the threading internals are specified in
**HEP-CORE-0010**. This HEP specifies everything the *developer* sees: how to configure
roles, what callbacks to implement, what API methods are available, and how failure is handled.

---

## 0. Implementation Status

All sections below are fully implemented as of 2026-02-28. Test coverage: 585/585 tests pass.

| Phase | Status |
|---|---|
| Phase 1 (2026-02-24) | Unified callback interface; `incoming_queue_` GIL-race fix; `loop_trigger`; `set_critical_error()` |
| Phase 2 (2026-02-24) | 2-thread-per-role model; embedded-mode API; per-role `Messenger` |
| Phase 3 (2026-02-24) | Application-level heartbeat via `zmq_thread_` |
| ActorVault / Security (2026-02-26) | Actor keypair (`--keygen`), `ActorAuthConfig`, CurveZMQ wiring |
| ActorScriptHost (2026-02-28) | Dedicated interpreter thread via `PythonScriptHost` (see HEP-CORE-0010 §3.7) |

For current priorities, see `docs/TODO_MASTER.md` and `docs/todo/API_TODO.md`.

---

## 1. Design Goals

1. **Single actor identity** — one `actor_uid` / `actor_name` regardless of how many
   channels the actor connects to or in which direction.
2. **Role palette** — the actor JSON declares a named set of roles (`producer` or
   `consumer`), each with its own channel, broker, schema, and timing policy. The Python
   module implements `on_iteration` / `on_init` / `on_stop` by name convention — no decorators.
3. **Typed zero-copy access** — slot and flexzone memory is presented to Python as a
   `ctypes.LittleEndianStructure` built from the JSON schema. No struct packing/unpacking
   in user code.
4. **Timer-driven producer** — `interval_ms` makes the write loop a best-effort periodic
   deadline. `loop_trigger="messenger"` enables message-event-driven iteration when no SHM
   channel is needed.
5. **Timeout-aware consumer** — `timeout_ms` fires `on_iteration(slot=None, ...)` when no
   new slot arrives, enabling periodic heartbeat or fallback computation.
6. **GIL-safe ZMQ callbacks** — incoming ZMQ messages are routed to `incoming_queue_`
   (mutex + condvar) by background threads; the loop thread drains the queue before
   acquiring the GIL — single-threaded Python call path, no GIL race.
7. **Authenticated identity** — optional NaCl keypair; `actor_uid` + password-protected
   private key; CurveZMQ broker authentication; provenance chain for SHM identity binding.

---

## 2. Actor Directory Layout and Configuration

### 2.1 Standard Directory Layout

```
<actor_dir>/
  actor.json                    ← actor identity + all role configs
  roles/
    <role_name>/                ← one subdirectory per role
      script/                   ← Python package (module name = "script")
        __init__.py             ← callbacks: on_init / on_iteration / on_stop
        helpers.py              ← optional helper submodule (from . import helpers)
  logs/                         ← rotating log files (created at startup)
  run/
    actor.pid                   ← PID of the running process
```

`pylabhub-actor --init <actor_dir>` creates this layout and writes a complete
`actor.json` + `roles/data_out/script/__init__.py` template.

### 2.2 actor.json Format

```json
{
  "hub_dir": "/opt/pylabhub/hubs/lab",

  "actor": {
    "uid":       "ACTOR-SENSOR-9E1D4C2A",
    "name":      "TemperatureSensor",
    "log_level": "info",
    "auth": {
      "keyfile":  "~/.pylabhub/sensor_node.key",
      "password": "env:PLH_ACTOR_PASSWORD"
    }
  },

  "roles": {
    "raw_out": {
      "kind":        "producer",
      "channel":     "lab.sensor.temperature",
      "interval_ms": 100,
      "script":      {"module": "script", "path": "./roles/raw_out"},
      "slot_schema": {
        "packing": "natural",
        "fields": [
          {"name": "ts",    "type": "float64"},
          {"name": "value", "type": "float32"},
          {"name": "flags", "type": "uint8"}
        ]
      },
      "flexzone_schema": {
        "fields": [
          {"name": "device_id",   "type": "uint16"},
          {"name": "sample_rate", "type": "uint32"},
          {"name": "label",       "type": "string", "length": 32}
        ]
      },
      "shm": {"enabled": true, "slot_count": 8, "secret": 0},
      "validation": {
        "slot_checksum": "update", "flexzone_checksum": "update",
        "on_checksum_fail": "skip", "on_python_error": "continue"
      }
    },

    "cfg_in": {
      "kind":       "consumer",
      "channel":    "lab.config.setpoints",
      "timeout_ms": 5000,
      "script":     {"module": "script", "path": "./roles/cfg_in"},
      "slot_schema": {
        "fields": [{"name": "setpoint", "type": "float32"}]
      }
    }
  }
}
```

> **Note**: `broker` / `broker_pubkey` are omitted above because they are automatically
> populated from `<hub_dir>/hub.json` and `<hub_dir>/hub.pubkey` by
> `ActorConfig::from_directory()` when `hub_dir` is present. Use the explicit
> `broker` / `broker_pubkey` fields only for integration tests or `--config` invocations
> that bypass hub discovery.

### 2.3 Field Reference

#### Top-level `actor` block

| Field | Required | Default | Description |
|---|---|---|---|
| `uid` | no | auto-generated | Stable unique identifier. Format: `ACTOR-{NAME}-{8HEX}`. Auto-generated from `name` if absent. Non-conforming UIDs are warned but accepted. |
| `name` | no | `""` | Human-readable name used as input for auto-generated UID name component. |
| `log_level` | no | `"info"` | `debug` / `info` / `warn` / `error` |
| `auth.keyfile` | no | absent | Path to NaCl keypair file. Activates CurveZMQ. |
| `auth.password` | no | absent | Passphrase. `"env:VAR"` reads `$VAR` at startup. |

#### UID format

UIDs follow the format `ACTOR-{NAME}-{SUFFIX}` where:
- `{NAME}` — up to 8 uppercase alphanumeric chars derived from the human name. Non-alnum runs → single `-`; falls back to `NODE`.
- `{SUFFIX}` — 8 uppercase hex digits from `std::random_device`.

```
"TemperatureSensor" → ACTOR-TEMPERAT-9E1D4C2A
"my lab node"       → ACTOR-MYLABNO-3A7F2B1C
(absent name)       → ACTOR-NODE-B3F12E9A
```

Generation utility: `pylabhub::uid::generate_actor_uid(name)` in
`src/include/utils/uid_utils.hpp` (public header, included via `plh_datahub.hpp`).

#### Per-role fields

| Field | Applies to | Required | Default | Description |
|---|---|---|---|---|
| `kind` | both | yes | — | `"producer"` or `"consumer"` |
| `channel` | both | yes | — | Channel name to create/subscribe |
| `broker` | both | no | `tcp://127.0.0.1:5570` | Broker endpoint. Overridden by `hub_dir` resolution. |
| `broker_pubkey` | both | no | `""` (plain TCP) | Broker CurveZMQ server public key (Z85, 40 chars). Overridden by `hub_dir`. |
| `script` | both | no* | — | Per-role Python package: `{"module": "script", "path": "./roles/<role>"}`. Falls back to top-level `"script"` if absent. At least one must be present. |
| `interval_ms` | producer | no | `0` | `0`=full throughput; `N>0`=deadline-scheduled (see §3.9) |
| `timeout_ms` | consumer | no | `-1` | `-1`=wait indefinitely; `N>0`=fire `on_iteration(slot=None)` on silence (see §3.9) |
| `loop_timing` | both | no | `"fixed_pace"` | Overrun policy for deadline advancement. `"fixed_pace"` or `"compensating"`. See §3.9. |
| `loop_trigger` | both | no | `"shm"` | Loop blocking strategy: `"shm"` or `"messenger"`. See §3.7. |
| `messenger_poll_ms` | both | no | `5` | Poll wait time when `loop_trigger="messenger"`. Values ≥ 10 warn at config load. |
| `heartbeat_interval_ms` | producer | no | `0` | `0` = 10×interval_ms. Controlled by `zmq_thread_`. |
| `slot_schema` | both | yes* | — | ctypes field list for slot. Omit for legacy raw-bytes mode. |
| `flexzone_schema` | both | no | absent | ctypes field list for flexzone. Absent = no flexzone. |
| `shm.enabled` | both | no | `false` | Whether to create/attach a DataBlock SHM segment. |
| `shm.slot_count` | producer | no | `4` | Ring buffer capacity. |
| `shm.secret` | both | no | `0` | Shared secret for SHM namespace isolation (not a cryptographic secret). |
| `validation` | both | no | see below | Per-cycle checksum and error-handling policies. |

#### `validation` sub-block defaults

| Key | Default | Values |
|---|---|---|
| `slot_checksum` | `"update"` | `"none"` / `"update"` / `"enforce"` |
| `flexzone_checksum` | `"update"` | same |
| `on_checksum_fail` | `"skip"` | `"skip"` (discard slot) / `"pass"` (call `on_iteration` with `api.slot_valid()==false`) |
| `on_python_error` | `"continue"` | `"continue"` (log traceback, keep running) / `"stop"` |

### 2.4 Slot Schema Field Types

| JSON `"type"` | ctypes type |
|---|---|
| `"bool"` | `c_bool` |
| `"int8"` / `"uint8"` | `c_int8` / `c_uint8` |
| `"int16"` / `"uint16"` | `c_int16` / `c_uint16` |
| `"int32"` / `"uint32"` | `c_int32` / `c_uint32` |
| `"int64"` / `"uint64"` | `c_int64` / `c_uint64` |
| `"float32"` | `c_float` |
| `"float64"` | `c_double` |
| `"string"` + `"length": N` | `c_char * N` |
| `"bytes"` + `"count": N` | `c_uint8 * N` |
| any scalar + `"count": N` | `ctypes_type * N` (array) |

`"packing"`: `"natural"` (default, C struct alignment) or `"packed"` (`_pack_ = 1`).
`slot_size` is NOT specified — computed from `ctypes.sizeof(SlotFrame)` at `start()`.

---

## 3. Python Script Interface

### 3.1 Script Structure — Per-Role Python Package

Each role has its own Python package directory. The `"script"` key inside the role config
points the actor at this package:

```
roles/
  raw_out/              ← role directory (non-Python files live here)
    script/             ← Python package (module name = "script")
      __init__.py       ← entry point: on_init / on_iteration / on_stop
      calibration.py    ← optional helper submodule
      filters.py        ← another helper
  cfg_in/
    script/
      __init__.py
```

```json
"raw_out": {
  "kind":   "producer",
  "script": {"module": "script", "path": "./roles/raw_out"},
  ...
}
```

`path` is added as the **parent** of the `script/` package (i.e. `./roles/raw_out`).
The package is loaded via `importlib.util.spec_from_file_location` under the unique
alias `_plh_{uid_hex}_{role_name}` — even when two roles share the module name `"script"`,
each gets a fully isolated module object with its own state.

#### Fallback: actor-level script

For simple single-role actors, a top-level `"script"` block serves as a fallback for any
role without a per-role `"script"` key:

```json
{
  "script": {"module": "sensor_node", "path": "/opt/scripts"},
  "roles": {
    "raw_out": { ... },   ← uses sensor_node (no per-role "script")
    "cfg_in":  { ... }    ← also uses sensor_node; dispatch on api.role_name()
  }
}
```

### 3.2 Imports Within the Script Package

```python
# roles/raw_out/script/__init__.py

import pylabhub_actor as actor   # C++ embedded module — always available
import time, numpy as np         # standard/installed packages — normal imports

from . import calibration        # relative import → ./calibration.py
from .filters import ButterworthLP  # named import from sibling
```

**What works:**
- `import pylabhub_actor as actor` — always available (C++ embedded module)
- `import numpy`, `import scipy`, etc. — any installed package
- `from . import helpers` — relative imports within the `script/` package

**What does NOT work:**
- `import script` — the module is registered under a role-unique alias
- Cross-role imports — each role's module is isolated; use hub channels to share data

### 3.3 Callback Reference

```python
# roles/raw_out/script/__init__.py
import pylabhub_actor as actor
import time
from . import calibration

count = 0

def on_init(api: actor.ActorRoleAPI) -> None:
    """Called once per role before the loop starts."""
    api.log('info', f"on_init: role={api.role_name()} uid={api.uid()}")
    fz = api.flexzone()
    if fz is not None:
        fz.device_id   = 42
        fz.sample_rate = 1000
        fz.label       = b"lab.sensor.temperature"
        api.update_flexzone_checksum()

def on_iteration(slot, flexzone, messages, api: actor.ActorRoleAPI) -> bool:
    """
    Called every loop iteration.

    slot:     ctypes.LittleEndianStructure (writable for producer, read-only for consumer),
              or None when triggered by Messenger timeout or when no SHM slot.
    flexzone: persistent ctypes struct for this role's flexzone, or None.
    messages: list of (sender: str, data: bytes) drained from the incoming ZMQ queue
              since the last iteration. May be empty.
    api:      ActorRoleAPI proxy for this role.

    Producer return: True/None = commit the slot; False = discard.
    Consumer return value is ignored.
    """
    global count

    for sender, data in messages:
        api.log('debug', f"msg from {sender}: {data!r}")

    if slot is None:
        return None  # Messenger trigger or timeout; slot not available

    if api.kind() == "producer":
        count += 1
        slot.ts    = time.time()
        slot.value = calibration.apply(count)
        slot.flags = 0x01
        return True
    else:
        if not api.slot_valid():
            api.log('warn', "checksum failed — discarding")
            return None
        api.log('debug', f"received setpoint={slot.setpoint}")
    return None

def on_stop(api: actor.ActorRoleAPI) -> None:
    """Called once per role after the loop exits."""
    api.log('info', f"on_stop: role={api.role_name()} count={count}")
```

### 3.4 Callback Table

| Function name | Signature | When called | Required |
|---|---|---|---|
| `on_init` | `(api)` | Once per role — before loop starts | No |
| `on_iteration` | `(slot, flexzone, messages, api) -> bool` | Every loop iteration | Yes (role skipped if absent) |
| `on_stop` | `(api)` | Once per role — after loop exits | No |

All three are looked up by `py::getattr(module, name, py::none())`. Absent functions are
silently skipped (no error). If `on_init` raises, the role aborts init cleanly.

#### Call order

1. Parse JSON → build `ActorConfig`; load/generate NaCl keypair if `auth` present
2. `ActorHost::load_script()`: for each role, load per-role `script/` package via
   `spec_from_file_location` (or actor-level module as fallback)
3. For each role: check `on_iteration` attribute → if absent, skip role (log warning)
4. `ActorHost::start()`: create `ProducerRoleWorker` / `ConsumerRoleWorker` for active roles
5. For each active role: call `on_init(api)`, then start loop and ZMQ threads
6. On shutdown: stop all threads → call `on_stop(api)` per role

### 3.5 Object Lifetimes

| Object | Valid window | Notes |
|---|---|---|
| `slot` (producer) | During `on_iteration` only | Writable `from_buffer` into SHM. **Do not store.** |
| `slot` (consumer) | During `on_iteration` only | Zero-copy `from_buffer` on read-only memoryview. Writing a field raises `TypeError`. **Do not store.** |
| `slot` (no SHM / timeout) | N/A | `None` when Messenger-triggered or `timeout_ms` fires. |
| `flexzone` | Entire role lifetime | Persistent `from_buffer` into SHM. Safe to read, write (producer), or store. |
| `api` | Entire role lifetime | Stateless proxy. Safe to store. |

Producer `flexzone`: writable, backed by the SHM flexible-zone region.
Consumer `flexzone`: read-only zero-copy view of the producer's same region.

### 3.6 Incoming Message Queue (GIL-Race Fix)

ZMQ callbacks (`peer_thread` for producers, `data_thread` for consumers) no longer call
Python directly. Instead they push `IncomingMessage{sender, data}` into `incoming_queue_`
under `incoming_mu_` and notify `incoming_cv_`. The loop thread drains the queue **before**
acquiring the GIL, ensuring a single-threaded Python call path.

The queue is bounded by `kMaxIncomingQueue = 256`. If full, new messages are dropped with
`LOGGER_WARN`. The `messages` list passed to `on_iteration` is built from all messages
drained in one pass; it may be empty (`[]`) even for SHM-triggered loops.

### 3.7 `loop_trigger` — Loop Thread Blocking Strategy

| Config value | Loop thread blocks on | Requires SHM |
|---|---|---|
| `"shm"` (default) | `acquire_*_slot(timeout)` | Yes (`shm.enabled=true`) |
| `"messenger"` | `incoming_cv_.wait_for(messenger_poll_ms)` | No |

When `loop_trigger="shm"`, `on_iteration` fires once per SHM slot available, with slot and
messages in the same call. When `loop_trigger="messenger"`, `on_iteration` fires when an
incoming ZMQ message arrives or after `messenger_poll_ms` timeout; `slot` is always `None`.

### 3.8 `ActorRoleAPI` Proxy

One `ActorRoleAPI` instance per active role. Passed to every lifecycle callback of that
role. All methods dispatch to C++ immediately.

```python
# ── Common (all roles) ────────────────────────────────────────────────────────
api.log(level: str, msg: str)            # log through hub logger
api.uid() -> str                         # actor uid from config
api.role_name() -> str                   # name of this role ("raw_out", "cfg_in", ...)
api.actor_name() -> str                  # human-readable actor name
api.channel() -> str                     # channel name for this role
api.broker() -> str                      # configured broker endpoint
api.kind() -> str                        # "producer" or "consumer"
api.log_level() -> str                   # configured log level
api.script_dir() -> str                  # directory containing the Python module
api.stop()                               # request actor shutdown (all roles)
api.set_critical_error()                 # latch + stop(); use for unrecoverable errors
api.critical_error() -> bool             # True if set_critical_error() was called
api.flexzone() -> ctypes.Structure|None  # persistent flexzone object, or None

# ── Producer roles ────────────────────────────────────────────────────────────
api.broadcast(data: bytes) -> bool       # ZMQ to all connected consumers
api.send(identity: str, data: bytes) -> bool  # ZMQ unicast to one consumer
api.consumers() -> list                  # ZMQ identity strings of connected consumers
api.update_flexzone_checksum() -> bool   # recompute and store BLAKE2b on SHM flexzone

# ── Consumer roles ────────────────────────────────────────────────────────────
api.send_ctrl(data: bytes) -> bool       # ZMQ ctrl frame to producer
api.slot_valid() -> bool                 # False when checksum failed + on_checksum_fail="pass"
api.verify_flexzone_checksum() -> bool   # verify SHM flexzone BLAKE2b
api.accept_flexzone_state() -> bool      # accept current flexzone as valid baseline

# ── Shared spinlocks (producer and consumer, requires shm.enabled=true) ───────
api.spinlock(index: int) -> SharedSpinLockPy   # SHM spinlock at given index (0–7)
api.spinlock_count() -> int                    # always 8; 0 if SHM not configured

# ── Diagnostics (read-only — collected by C++ host about the script) ──────────
api.script_error_count() -> int  # Python exceptions in callbacks since role start
api.loop_overrun_count()  -> int  # write cycles where interval_ms deadline was exceeded
api.last_cycle_work_us()  -> int  # µs of active work in the last write cycle (producer only)
api.metrics() -> dict              # all timing metrics (HEP-CORE-0008 domains 2–4)
```

### 3.9 LoopTimingPolicy — Deadline Scheduling

The write loop (producer) and timeout loop (consumer) use deadline-based scheduling
rather than a simple `sleep(interval_ms)` at the top of each iteration.

| Policy | Formula on overrun | Behaviour |
|---|---|---|
| `"fixed_pace"` (default) | `next = now() + interval` | Resets deadline from actual wakeup time; no catch-up burst; instantaneous rate ≤ target |
| `"compensating"` | `next += interval` | Advances one tick regardless; fires immediately after an overrun until caught up; average rate converges to target |

**On-time case**: both policies advance `next += interval` — equivalent because wakeup ≈ deadline.

**Overrun detection**: when `now >= next_deadline`, no sleep is taken. The loop counts
this as an **overrun** (`api.loop_overrun_count()` increments by 1).

```python
# Example: 100 Hz producer — detect if more than 5% of cycles are overrunning
def on_iteration(slot, fz, messages, api):
    total = api.loop_overrun_count() + api.script_error_count()
    if total > 0 and total % 500 == 0:
        api.log('warn', f"overruns={api.loop_overrun_count()} "
                        f"work_us={api.last_cycle_work_us()}")
    slot.ts = time.time()
    return True
```

**Consumer timeout policy**: the same `loop_timing` field controls how the timeout deadline
is advanced. `"fixed_pace"` resets from actual callback completion; `"compensating"` advances
by one `timeout_ms` tick.

### 3.10 RoleMetrics — Supervised Diagnostics

The Python script runs under supervision by the C++ host. The host collects diagnostic
counters **about** the script's execution and makes them available as read-only attributes.

**Design principle**: the script may observe its own health but may **not** reset or write
these counters. This preserves metric integrity — the script cannot clear evidence of its
own misbehaviour. Counters reset automatically on role restart via `reset_all_role_run_metrics()`.

| Getter | Who writes | When | Semantics |
|---|---|---|---|
| `api.script_error_count()` | C++ host | Each `py::error_already_set` catch block | Python exceptions in any callback |
| `api.loop_overrun_count()` | C++ host | Each write-loop overrun | Cycles where `interval_ms` deadline was past. Producer only; 0 for consumers when `interval_ms <= 0`. |
| `api.last_cycle_work_us()` | C++ host | After each successful write | µs from start of acquire through commit + checksum. 0 until first write. Producer only. |

**C++ internal write interface** (never accessible from Python):
```cpp
api_.increment_script_errors();
api_.increment_loop_overruns();
api_.set_last_cycle_work_us(elapsed_us);
api_.reset_all_role_run_metrics();   // called in start() before running_.store(true)
```

### 3.11 SharedSpinLockPy — Cross-Process Spinlock

`api.spinlock(idx)` returns a `SharedSpinLockPy` object backed by one of the 8
`SharedSpinLockState` slots in the SHM header. These slots are shared between all
processes that open the same channel (producer and all consumers).

```python
# ── Context manager (preferred — always releases on exception) ─────────────────
with api.spinlock(0):
    flexzone.counter += 1
    api.update_flexzone_checksum()

# ── Non-blocking attempt ───────────────────────────────────────────────────────
lk = api.spinlock(2)
if lk.try_lock_for(timeout_ms=100):
    try:
        flexzone.status = "updating"
    finally:
        lk.unlock()
```

| Method | Description |
|---|---|
| `lock()` | Acquire (blocking spin); GIL released while spinning |
| `unlock()` | Release; raises `RuntimeError` if not held by this process |
| `try_lock_for(timeout_ms)` | Returns `True` on success, `False` on timeout |
| `is_locked_by_current_process()` | Diagnostic check |
| `__enter__` / `__exit__` | Context manager support |

The `SharedSpinLockState` struct lives in SHM for the actor's lifetime. All spinlock
indices are pre-allocated — no reservation step. Convention (not enforcement) determines
which index is used for which purpose.

---

## 4. C++ Class Architecture

### 4.1 hub::Producer C++ API

The actor system builds on `hub::Producer` from `pylabhub-utils`. C++ code can use this
directly without the Python actor layer.

```cpp
#include "plh_datahub.hpp"   // hub::Producer, hub::Consumer, hub::Messenger

LifecycleGuard guard(MakeModDefList(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::hub::GetZMQContextModule(),
    pylabhub::hub::GetLifecycleModule()
));

auto &messenger = pylabhub::hub::Messenger::get_instance();

hub::ProducerConfig pcfg;
pcfg.channel_name    = "lab.sensor.temperature";
pcfg.producer_uid    = "sensor_node_001";
pcfg.slot_size       = 24;
pcfg.slot_count      = 8;
pcfg.shm_secret      = 9876543210;
pcfg.broker_endpoint = messenger.endpoint();

hub::Producer producer;
producer.start(pcfg, messenger);

auto txn = producer.begin_write();
if (txn.valid()) {
    auto *slot = txn.as<SlotFrame>();
    slot->ts    = current_time();
    slot->value = read_sensor();
    slot->flags = 0x01;
    txn.commit();
}

producer.stop();
```

### 4.2 hub::Consumer C++ API

```cpp
hub::ConsumerConfig ccfg;
ccfg.channel_name    = "lab.sensor.temperature";
ccfg.consumer_uid    = "controller_001";
ccfg.timeout_ms      = 5000;
ccfg.broker_endpoint = messenger.endpoint();

hub::Consumer consumer;
consumer.connect(ccfg, messenger);

while (!shutdown) {
    auto slot_handle = consumer.acquire_consume_slot();
    if (!slot_handle.valid()) {
        send_heartbeat();
        continue;
    }
    const auto *slot = slot_handle.as<SlotFrame>();
    process(slot->ts, slot->value, slot->flags);
    consumer.release_consume_slot(slot_handle);
}

consumer.close();
```

### 4.3 ProducerRoleWorker and ConsumerRoleWorker

The Python actor system wraps `hub::Producer` / `hub::Consumer` in role worker classes:

```cpp
namespace pylabhub::actor {

class ProducerRoleWorker {
public:
    explicit ProducerRoleWorker(
        const std::string        &role_name,
        const RoleConfig         &role_cfg,
        const std::string        &actor_uid,
        const ActorAuthConfig    &auth,
        std::atomic<bool>        &shutdown,
        const py::module_        &script_module);

    bool start();    // connect messenger; build ctypes types; call on_init; start threads
    void stop();     // signal threads; join; call on_stop

private:
    hub::Messenger  messenger_;       // owned, per-role (value, not reference)
    py::object      py_on_iteration_;
    py::object      py_on_init_;
    py::object      py_on_stop_;
    std::deque<IncomingMessage>  incoming_queue_;
    std::mutex                   incoming_mu_;
    std::condition_variable      incoming_cv_;

    void run_loop_shm();        // loop_trigger=Shm
    void run_loop_messenger();  // loop_trigger=Messenger

    std::thread            loop_thread_{};
    std::thread            zmq_thread_{};
    std::atomic<uint64_t>  iteration_count_{0};
};

class ConsumerRoleWorker { /* symmetric design */ };

} // namespace pylabhub::actor
```

See **HEP-CORE-0010 §3** for the thread lifecycle (2-thread model, GIL handoff,
application-level heartbeat, initialization sequence).

### 4.4 ActorHost

```cpp
class ActorHost {
public:
    explicit ActorHost(const ActorConfig &config);
    bool load_script(bool verbose = false);  // import module; look up callbacks
    bool start();                            // create workers for configured roles
    void stop();                             // stop all workers
    void wait_for_shutdown();
    void signal_shutdown() noexcept;
};
```

`ActorHost` does **not** hold a singleton `Messenger` — each role worker owns its own
`hub::Messenger` and connects to `role_cfg_.broker` inside `start()`.

### 4.5 Messenger Ownership Model

| Component | Messenger ownership | Who calls connect() |
|---|---|---|
| `HubShell` | Singleton (`Messenger::get_instance()`) | Lifecycle startup |
| `ProducerRoleWorker` | Owned value (`hub::Messenger messenger_`) | `start()` → `role_cfg_.broker` |
| `ConsumerRoleWorker` | Owned value (`hub::Messenger messenger_`) | `start()` → `role_cfg_.broker` |
| `actor_main.cpp` | None | N/A — no `GetLifecycleModule()` call |

ZMQ context remains process-wide via `GetZMQContextModule()`.

### 4.6 Module-Convention Callback Resolution

There is no dispatch table. `ActorHost::load_script()` imports the configured Python module
via `importlib` with a synthetic alias, then passes the `py::module_` directly to each worker
constructor. Workers resolve callbacks at construction time via `py::getattr`:

```cpp
py::gil_scoped_acquire g;
py_on_iteration_ = py::getattr(script_module, "on_iteration", py::none());
py_on_init_      = py::getattr(script_module, "on_init",      py::none());
py_on_stop_      = py::getattr(script_module, "on_stop",      py::none());
```

The module is imported with a unique alias to avoid `sys.modules` collisions:

```cpp
// Synthetic name: _plh_{uid_8hex}_{module_name}
std::string alias = "_plh_" + uid_8hex + "_" + config_.script_module;
py::module_::import("sys").attr("modules")[alias] = script_module_;
```

Absence of `on_iteration` is not an error — `ActorHost::start()` logs a warning and skips
the role. Absence of `on_init` / `on_stop` is silently tolerated.

### 4.7 ctypes Schema Binding (Zero-Copy)

At `start()`, C++ builds a `ctypes.LittleEndianStructure` subclass from the JSON schema:

```cpp
// Build a ctypes struct class from SchemaSpec.
py::object build_ctypes_struct(const SchemaSpec &spec, const std::string &class_name);

// Producer: writable zero-copy view into SHM slot
py::object make_slot_view_write(void *data, size_t size, py::object slot_type);
// Equivalent: slot_type.from_buffer(memoryview(data))

// Consumer: read-only zero-copy view
py::object make_slot_view_readonly(const void *data, size_t size, py::object slot_type);
// Equivalent: slot_type.from_buffer(readonly_memoryview) — TypeError on field write
```

Flexzone is created once at `start()` and passed by reference to every `on_iteration` call.

### 4.8 ActorScriptHost

`ActorScriptHost` (`src/actor/actor_script_host.hpp/.cpp`) owns the CPython interpreter
on a dedicated interpreter thread. See **HEP-CORE-0010 §3.7** for the full specification:
class interface, interpreter thread lifecycle diagram, GIL handoff sequence,
`PyConfig.install_signal_handlers=0` rationale, and shutdown paths.

---

## 5. Authentication and Security

### 5.1 Threat Model

Without authentication, any process can register a channel under an existing `actor_uid`.
The goal is to make identity cryptographically verifiable at startup — no per-cycle overhead.

### 5.2 Keypair Lifecycle

1. `--keygen`: generate NaCl CURVE25519 keypair → write to `auth.keyfile` (Argon2id-protected)
2. On `start()` with `auth.keyfile` present: load and decrypt keypair using `auth.password`
3. Password sourced from: literal JSON string (dev only), `"env:VAR"` (production), or
   interactive prompt fallback

| Step | Cost | Notes |
|---|---|---|
| Key load/decrypt | ~5 ms at startup | Argon2id KDF; once only |
| CurveZMQ handshake | once per broker connection | |
| Per-cycle overhead | **zero** | ZMQ CURVE is at transport layer |

### 5.3 CurveZMQ Integration

| Side | Component | Details |
|---|---|---|
| Broker | `BrokerService` | Maintains `KnownActors` table; validates actor public key in `REG_REQ` |
| Actor | `Messenger` DEALER socket | Sets `ZMQ_CURVE_SECRETKEY`, `ZMQ_CURVE_PUBLICKEY`, `ZMQ_CURVE_SERVERKEY` |
| BLDS metadata | Channel record | Includes producer public key for consumer connection policy |

The actor's `secret_key` never leaves the actor process.

### 5.4 Public vs Secret Boundary

The rule mirrors standard SSH/TLS practice:

> **Public config** (`actor.json`, safe to version-control): actor UID, name, log level,
> `auth.keyfile` path, `auth.password` field name (`"env:VAR"`), role topology, `broker_pubkey`,
> slot/flexzone schemas, `shm.secret` (namespace isolation only, not cryptographic).
>
> **Private keyfile** (`actor.key`, `chmod 600`, never committed): the CURVE25519 private key.

The keyfile contains:
```json
{
  "actor_uid":  "ACTOR-Sensor-DEADBEEF",
  "public_key": "<Z85-encoded CURVE25519 public key, 40 chars>",
  "secret_key": "<Z85-encoded CURVE25519 private key, 40 chars>",
  "_note":      "Keep secret_key private."
}
```

What the broker needs from the actor: only `public_key` (to whitelist the actor).
What the actor needs from the broker: `roles.*.broker_pubkey` in `actor.json` (Z85, 40 chars;
equivalent to SSH host key — every client must know it, not secret).

Connection policy enforcement is defined in **HEP-CORE-0009** (Open / Tracked / Required /
Verified). Channel identity provenance is defined in **HEP-CORE-0013**.

---

## 6. Schema Validation Architecture

Three independent, complementary validation mechanisms guard different points in the data lifecycle.

### 6.1 Overview

| Layer | Name | Where computed | When enforced | Protects against |
|---|---|---|---|---|
| **1** | Per-write BLAKE2b | C++ after `on_iteration()` commits | Per slot, before consumer reads | Bit-flip / memory corruption during IPC |
| **2** | Schema declaration hash | C++ at `start()` from JSON | At `Consumer::connect()` | Mismatched field layout between independent actor configs |
| **3** | BLDS channel registry | Broker at `REG_REQ` | At broker registration | Channel schema changes while active consumers exist |

### 6.2 Layer 1 — Slot Data Checksum

After `on_iteration()` returns `True` and commits a slot, C++ calls `update_checksum_slot()`
which writes a BLAKE2b-256 digest of the slot bytes into the SHM control zone. On the
consumer side, `verify_checksum_slot()` re-computes and compares before delivering to Python.

Config: `validation.slot_checksum` = `"none"` / `"update"` (default) / `"enforce"`.
Consumer policy: `validation.on_checksum_fail` = `"skip"` / `"pass"`.

### 6.3 Layer 2 — Schema Declaration Hash

`compute_schema_hash()` builds a canonical string from the parsed `SchemaSpec` field list
(name, type, count, length) and hashes it with BLAKE2b-256. The result is set on:
- `ProducerOptions::schema_hash` — sent to broker in `REG_REQ`
- `ConsumerOptions::expected_schema_hash` — validated in `Messenger::connect_channel()`

Canonical format:
```
slot:name:type:count:len|name:type:count:len|...[|fz:name:type:count:len|...]
```

### 6.4 Layer 3 — BLDS Channel Registry

`SchemaBLDS::compute_hash()` hashes the full broker-level data schema descriptor. Stored
by the broker in its channel registry at producer registration; returned to consumers in
`DISC_ACK`. Prevents a new producer from reusing a channel name with an incompatible schema
while existing consumers are active.

### 6.5 Why All Three Are Needed

```
Producer writes slot
  │
  ├─ Layer 2: computed once at start(); ensures consumer was configured with
  │           the same field layout BEFORE the first slot is read.
  │           Config-time check — catches developer mistakes.
  │
  ├─ Layer 1: per-write checksum; guards in-flight SHM from bit flips or
  │           partial writes.
  │           Runtime check — catches infrastructure failures.
  │
  └─ Layer 3: broker registry; prevents schema drift when the producer
              binary is replaced and channels are reused.
              Operational check — catches deployment mistakes.
```

---

## 7. Validation and Startup Diagnostics

### 7.1 `--validate` Output (Example)

```
Role: raw_out  (producer)
  Channel:  lab.sensor.temperature
  Broker:   tcp://127.0.0.1:5570
  interval_ms: 100

  Slot layout: SlotFrame (ctypes.LittleEndianStructure)
    ts        float64   offset=0    size=8
    value     float32   offset=8    size=4
    flags     uint8     offset=12   size=1
    [3 bytes padding — natural alignment]
    samples   float32*8 offset=16   array=32 bytes
    Total: 48 bytes

  FlexZone layout: FlexFrame (ctypes.LittleEndianStructure)
    device_id   uint16    offset=0    size=2
    [2 bytes padding]
    sample_rate uint32    offset=4    size=4
    label       string*32 offset=8    size=32
    Total: 40 bytes

  Validation:
    slot_checksum=update  flexzone_checksum=update
    on_checksum_fail=skip  on_python_error=continue

  Python handler: on_iteration ✓   on_init ✓   on_stop ✓
```

### 7.2 Startup Validation Rules

| Check | When | On failure |
|---|---|---|
| `ctypes.sizeof(SlotFrame) == SHM logical_unit_size` | At `start()` per role | Fail fast, clear error |
| `on_iteration` attribute found in module | At `ActorHost::start()` | Warning logged; role not activated |
| `loop_trigger=shm` and `shm.enabled=false` | At `ActorConfig::from_json_file()` | `std::runtime_error`; actor exits |
| `"script"` field is an object (not a string) | At `ActorConfig::from_json_file()` | `std::runtime_error`; actor exits |
| Schema type is a known BLDS type | At `from_json_file()` per field | `std::runtime_error`; actor exits |

### 7.3 CLI Flags

```bash
pylabhub-actor --config actor.json             # normal run
pylabhub-actor --config actor.json --validate  # print layout; exit 0
pylabhub-actor --config actor.json --list-roles # show role activation; exit 0
pylabhub-actor --config actor.json --keygen    # generate keypair; exit 0
pylabhub-actor --init <actor_dir>              # create directory layout + templates; exit 0
```

---

## 8. Failure Model

### 8.1 Guiding Principle

Recovery logic is only justified when the post-crash state is **coherent enough to continue
correctly**. When it is not, the right response is a fast, clean exit with a clear error message.

### 8.2 Catastrophic — Exit Cleanly, Do Not Attempt Recovery

| Failure | Why unrecoverable | Correct response |
|---|---|---|
| **Broker crash** | All channel registration records, consumer lists, schema hashes are lost; reconstructing from surviving data is impossible | Heartbeat timeout fires `CHANNEL_CLOSING_NOTIFY`; actor performs clean exit. Operators restart the pipeline. |
| **Producer crash** (seen by consumer) | SHM segment is unlinked on producer exit; consumer holds a mapping to a segment that may be overwritten | Consumer detects stale SHM (sequence stall or `CHANNEL_CLOSING_NOTIFY`); clean exit. |
| **ZMQ context destroyed under active sockets** | Undefined behaviour | Lifecycle guard enforces shutdown ordering (`Messenger` → `ZMQContext`). Actor has no singleton Messenger so this cannot occur. |

### 8.3 Manageable — Handle Gracefully

| Failure | Correct response |
|---|---|
| **Broker not reachable at startup** | `messenger_.connect()` failure logs warning and continues in degraded mode (SHM write/read still works; no broker registration). |
| **Slot checksum failure** | `on_checksum_fail` policy: `"skip"` (discard slot) or `"pass"` (deliver with `api.slot_valid()==false`). |
| **Python callback throws** | `on_python_error` policy: `"continue"` (log traceback, keep running) or `"stop"` (clean exit). |
| **Consumer crash** (seen by producer) | Broker detects dead PID via `check_consumer_health()` and sends `CONSUMER_DIED_NOTIFY`. Producer logs and continues. |
| **SHM segment name collision** | Fail fast with clear error at start (`shm_open()` failure). |

### 8.4 Operational Contract

The pylabhub IPC stack is a **same-lifetime** system: broker, producers, and consumers are
expected to start together and stop together. It is not a durable message queue. Data in
flight at the time of any crash is lost — a deliberate design choice that keeps the system
simple, fast, and correct.

For deployments requiring durability across crashes, add it **above** pylabhub — e.g., a
supervisor process that restarts the pipeline in the right order, or an external buffer
(file, database) that the Python script writes to inside `on_iteration`.

---

## 9. Runtime Cost Analysis

| Item | Cost | Notes |
|---|---|---|
| GIL acquisition | 0–5 μs | Zero contention when roles run at different frequencies |
| `ctypes.from_buffer` (writable slot) | ~200 ns | Zero-copy into SHM slot |
| `ctypes.from_buffer` (readonly slot) | ~200 ns | Zero-copy; `TypeError` on field write |
| Python function call via pybind11 | ~500 ns | Includes args marshal + return unmarshal |
| `interval_ms` sleep | configured ms ± OS jitter | `std::this_thread::sleep_for` |
| **Total per 100 Hz write cycle** | **~3 μs** | 0.03% CPU at 100 Hz |

GIL contention with N active roles: N loop threads acquire GIL in FIFO order. Keep callbacks
short; do not sleep inside callbacks. Recommended limit: ~4 active roles per actor process.

---

## 10. Known Limitations

| Limitation | Impact | Mitigation |
|---|---|---|
| Storing `slot` beyond its callback | Silent stale data (SHM mapped; no crash) | Read-only binding adds write protection; `from_buffer` contract is documented |
| `interval_ms` overrun on slow callbacks | Body exceeds deadline → immediate next fire | `"fixed_pace"` (default) resets from now, capping rate. Monitor `api.loop_overrun_count()` and `api.last_cycle_work_us()`. |
| GIL contention with many concurrent roles | Latency spikes | Keep callbacks short; limit ~4 active roles per actor |
| Script cannot be hot-reloaded | Restart on script change | By design; reload would require `sys.modules` reset |
| `on_iteration` typo silently skips role | Role not activated | Startup warns; `--validate` shows activation summary |

---

## Copyright

This document is placed in the public domain or under CC0-1.0-Universal.
