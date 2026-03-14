# pylabhub-actor Design

**Status**: Implemented — 2026-02-21. Updated 2026-02-25: HEP-CORE-0010 Phase 3 complete — application-level heartbeat via `zmq_thread_`; embedded-mode API; per-role Python packages. Phase 1: unified module-based script interface, GIL-race fix, `loop_trigger`, `incoming_queue_`, `set_critical_error()`. Phase 2: 2-thread model (`zmq_thread_` + `loop_thread_`), `start_embedded()`, socket handle API, `iteration_count_`. Phase 3: `suppress_periodic_heartbeat()` + `enqueue_heartbeat()` for application-level liveness.
**Supersedes**: previous decorator-based design (decorator API removed; `actor_dispatch_table.hpp` deleted)

---

## 1. Design Goals

1. **Single actor identity** — one `actor_uid` / `actor_name` regardless of how many
   channels the actor connects to or in which direction.
2. **Role palette** — the actor JSON declares a named set of roles (`producer` or
   `consumer`), each with its own channel, broker, schema, and timing policy. The Python
   module implements `on_iteration` / `on_init` / `on_stop` by convention — no decorators.
3. **Typed zero-copy access** — slot and flexzone memory is presented to Python as a
   `ctypes.LittleEndianStructure` built from the JSON schema.  No struct packing/unpacking
   in user code.
4. **Timer-driven producer** — `interval_ms` makes the write loop a best-effort periodic
   poll.  `loop_trigger="messenger"` enables message-event-driven iteration when no SHM
   channel is needed.
5. **Timeout-aware consumer** — `timeout_ms` fires `on_iteration(slot=None, ...)` when no
   new slot arrives, enabling periodic heartbeat or fallback computation.
6. **GIL-safe ZMQ callbacks** — incoming ZMQ messages are routed to `incoming_queue_`
   (mutex + condvar) by background threads; the loop thread drains the queue before
   acquiring the GIL — single-threaded Python call path, no GIL race.
7. **Authenticated identity** — optional NaCl keypair; `actor_uid` + password-protected
   private key; CurveZMQ broker authentication; provenance chain for SHM identity binding.

---

## 2. Config File Format

The multi-role format uses an `actor` block, an optional top-level `script` fallback, and
a `roles` map where each role may carry its own `"script"` key.  `"script"` wherever it
appears must be an object `{"module": "...", "path": "..."}` — a bare string is rejected.

### 2.0 Standard actor directory layout

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

```json
{
  "hub_dir": "/opt/pylabhub/hubs/lab",

  "actor": {
    "uid":       "ACTOR-SENSOR-00000003",
    "name":      "TemperatureSensor",
    "log_level": "info",
    "auth": {
      "keyfile":  "~/.pylabhub/sensor_node_001.key",
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

> **Note**: `broker` / `broker_pubkey` are omitted above because they are
> automatically populated from `<hub_dir>/hub.json` and `<hub_dir>/hub.pubkey`
> by `ActorConfig::from_directory()` when `hub_dir` is present.
> Use the explicit `broker` / `broker_pubkey` fields only when `hub_dir` is not set
> (e.g. integration tests or direct JSON invocation via `--config`).

### 2.1 Field reference

#### Top-level `actor` block

| Field | Required | Default | Description |
|---|---|---|---|
| `uid` | no | auto-generated | Stable unique identifier. **Format**: `ACTOR-{NAME}-{8HEX}` (e.g. `ACTOR-SENSOR-9E1D4C2A`). Auto-generated from `name` if absent. Non-conforming UIDs are warned but accepted. |
| `name` | no | `""` | Human-readable name. Used as input for auto-generated UID name component. |
| `log_level` | no | `"info"` | `debug` / `info` / `warn` / `error` |
| `auth.keyfile` | no | absent | Path to NaCl keypair file. Activates CurveZMQ. |
| `auth.password` | no | absent | Passphrase. `"env:VAR"` reads `$VAR` at startup. |

#### UID format

UIDs follow the format `ACTOR-{NAME}-{SUFFIX}` where:
- `{NAME}` — up to 8 uppercase alphanumeric chars derived from the human name. Non-alnum runs → single `-`; falls back to `NODE`.
- `{SUFFIX}` — 8 uppercase hex digits from `std::random_device` (or high-res-clock+Knuth hash fallback).

```
"TemperatureSensor" → ACTOR-TEMPERAT-9E1D4C2A
"my lab node"       → ACTOR-MYLABNO-3A7F2B1C
(absent name)       → ACTOR-NODE-B3F12E9A
```

Generation utility: `pylabhub::uid::generate_actor_uid(name)` in `src/include/utils/uid_utils.hpp`
(public header, included via `plh_datahub.hpp`).

**HubConfig UID**: The hub identity follows a parallel format: `HUB-{NAME}-{8HEX}`.
Auto-generated from `hub_name` at HubConfig startup; can be overridden in `hub.json["hub"]["uid"]`.

#### Per-role fields

| Field | Applies to | Required | Default | Description |
|---|---|---|---|---|
| `kind` | both | yes | — | `"producer"` or `"consumer"` |
| `channel` | both | yes | — | Channel name to create/subscribe |
| `broker` | both | no | `tcp://127.0.0.1:5570` | Broker endpoint. Overridden by `hub_dir` resolution. |
| `broker_pubkey` | both | no | `""` (plain TCP) | Broker CurveZMQ server public key (Z85, 40 chars). Overridden by `hub_dir`. |
| `script` | both | no* | — | Per-role Python package: `{"module": "script", "path": "./roles/<role>"}`. Falls back to top-level `"script"` if absent. At least one of per-role or top-level must be present. |
| `interval_ms` | producer | no | `0` | `0`=full throughput; `N>0`=deadline-scheduled (see `loop_timing`) |
| `timeout_ms` | consumer | no | `-1` | `-1`=wait indefinitely; `N>0`=fire `on_iteration(slot=None)` on silence (see `loop_timing`) |
| `loop_timing` | both | no | `"fixed_pace"` | Overrun policy for `interval_ms`/`timeout_ms` deadline advancement. See §3.7. |
| `loop_trigger` | both | no | `"shm"` | Loop blocking strategy: `"shm"` (block on acquire_*_slot; requires `shm.enabled=true`) or `"messenger"` (block on incoming ZMQ message / poll timeout). |
| `messenger_poll_ms` | both | no | `5` | Poll wait time when `loop_trigger="messenger"`. Values ≥ 10 warn at config load. |
| `heartbeat_interval_ms` | producer | no | `0` | `0` = 10×interval_ms. Reserved for Phase 2 heartbeat. |
| `slot_schema` | both | yes* | — | ctypes field list for slot. Omit for legacy raw-bytes mode. |
| `flexzone_schema` | both | no | absent | ctypes field list for flexzone. Absent = no flexzone for this role. |
| `shm.enabled` | both | no | `false` | Whether to create/attach a DataBlock SHM segment. |
| `shm.slot_count` | producer | no | `4` | Ring buffer capacity. |
| `shm.secret` | both | no | `0` | Shared secret for SHM discovery. |
| `validation` | both | no | see below | Per-cycle checksum and error-handling policies. |

#### `validation` sub-block defaults

| Key | Default | Values |
|---|---|---|
| `slot_checksum` | `"update"` | `"none"` / `"update"` / `"enforce"` |
| `flexzone_checksum` | `"update"` | same |
| `on_checksum_fail` | `"skip"` | `"skip"` (discard slot) / `"pass"` (call `on_iteration` with `api.slot_valid()==false`) |
| `on_python_error` | `"continue"` | `"continue"` (log traceback, keep running) / `"stop"` |

### 2.2 Slot schema field types

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

### 3.1 Script structure — per-role Python package

Each role has its own Python **package** directory. Keeping Python files separate from other
role-specific files (config, data, etc.) is the key reason for the `script/` subdirectory:

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

The `"script"` key inside each role config points the actor at this package:

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

For simple single-role actors or actors where all roles share common logic, a top-level
`"script"` block can be used as a fallback for any role without a per-role `"script"` key:

```json
{
  "script": {"module": "sensor_node", "path": "/opt/scripts"},
  "roles": {
    "raw_out": { ... },   ← uses sensor_node (no per-role "script")
    "cfg_in":  { ... }    ← also uses sensor_node; dispatch on api.role_name()
  }
}
```

### 3.2 Importing within the script package

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
- `from .subpkg import func` — nested sub-packages within `script/`

**What does NOT work:**
- `import script` — the module is registered under a role-unique alias, not as `"script"`
- Cross-role imports — each role's module is isolated; use hub channels to share data

### 3.3 Callback reference

```python
# roles/raw_out/script/__init__.py
import pylabhub_actor as actor
import time
from . import calibration    # example relative import

# Module-level state is private to this role (isolated module object)
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

    slot:     ctypes.LittleEndianStructure (writable for producer, read-only for
              consumer), or None when triggered by Messenger timeout / no SHM slot.
    flexzone: persistent ctypes struct for this role's flexzone, or None.
    messages: list of (sender: str, data: bytes) drained from the incoming ZMQ queue
              since the last iteration. May be empty even for SHM-triggered loops.
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
        # Consumer: slot is read-only; field writes raise TypeError
        if not api.slot_valid():
            api.log('warn', "checksum failed — discarding")
            return None
        api.log('debug', f"received setpoint={slot.setpoint}")
    return None

def on_stop(api: actor.ActorRoleAPI) -> None:
    """Called once per role after the loop exits."""
    api.log('info', f"on_stop: role={api.role_name()} count={count}")
```

### 3.4 Callbacks

| Function name | Signature | When called | Required |
|---|---|---|---|
| `on_init` | `(api)` | Once per role — before loop starts | No |
| `on_iteration` | `(slot, flexzone, messages, api) -> bool` | Every loop iteration | Yes (role skipped if absent) |
| `on_stop` | `(api)` | Once per role — after loop exits | No |

All three are looked up by `py::getattr(module, name, py::none())`. Absent functions are
silently skipped. With per-role modules, each role's module has **its own** `on_iteration`
function with its own module-level state — no need to dispatch on `api.role_name()`.
(Dispatch on `api.role_name()` is still available when using the actor-level fallback where
multiple roles share a single module.)

#### Call order

1. Parse JSON → build `ActorConfig`; load/generate NaCl keypair if `auth` present
2. `load_script()`: for each role, load per-role `script/` package via
   `spec_from_file_location` (or actor-level module as fallback)
3. For each role: check `on_iteration` attribute → if absent, skip role (log warning)
4. `ActorHost::start()`: create `ProducerRoleWorker` / `ConsumerRoleWorker` for active roles
5. For each active role: call `on_init(api)`, then start loop and ZMQ threads
6. On shutdown: stop all threads → call `on_stop(api)` per role

### 3.5 Callbacks

| Function name | Signature | When called | Required |
|---|---|---|---|
| `on_init` | `(api)` | Once per role — before loop starts | No |
| `on_iteration` | `(slot, flexzone, messages, api) -> bool` | Every loop iteration | Yes (role skipped if absent) |
| `on_stop` | `(api)` | Once per role — after loop exits | No |

All three are looked up by `py::getattr(module, name, py::none())`. Absent functions are
silently skipped (no error). All roles in a module share the **same** `on_iteration` function;
use `api.role_name()` or `api.kind()` to dispatch role-specific logic.

#### Call order

1. Parse JSON → build `ActorConfig`; load/generate NaCl keypair if `auth` present
2. Import module via `importlib` — no side effects; no decorator registration
3. For each role in config: check `on_iteration` attribute → if absent, skip role (log warning)
4. `ActorHost::start()`: create `ProducerRoleWorker` / `ConsumerRoleWorker` for active roles
5. For each active role: call `on_init(api)`, then start loop thread
6. On shutdown: stop all loop threads → call `on_stop(api)` per role

### 3.3 Object lifetimes

| Object | Valid window | Notes |
|---|---|---|
| `slot` (producer) | During `on_iteration` only | Writable `from_buffer` into SHM. **Do not store.** |
| `slot` (consumer) | During `on_iteration` only | Zero-copy `from_buffer` on read-only memoryview. Writing a field raises `TypeError`. **Do not store.** |
| `slot` (no SHM / timeout) | N/A | `None` when Messenger-triggered or `timeout_ms` fires. |
| `flexzone` | Entire role lifetime | Persistent `from_buffer` into SHM. Safe to read, write (producer), or store. |
| `api` | Entire role lifetime | Stateless proxy. Safe to store. |

Producer `flexzone`: writable, backed by the SHM flexible-zone region.
Consumer `flexzone`: read-only zero-copy view of the producer's same region.

### 3.4 Incoming message queue (GIL-race fix)

ZMQ callbacks (`peer_thread` for producers, `data_thread` for consumers) no longer call
Python directly. Instead they push `IncomingMessage{sender, data}` into `incoming_queue_`
under `incoming_mu_` and notify `incoming_cv_`. The loop thread drains the queue **before**
acquiring the GIL, ensuring a single-threaded Python call path.

The queue is bounded by `kMaxIncomingQueue = 256`. If full, new messages are dropped with a
`LOGGER_WARN` log. The `messages` list passed to `on_iteration` is built from all messages
drained in one pass; it may be empty (`[]`) even for SHM-triggered loops.

### 3.5 `loop_trigger` — loop thread blocking strategy

| Config value | Loop thread blocks on | Requires SHM |
|---|---|---|
| `"shm"` (default) | `acquire_*_slot(timeout)` | Yes (`shm.enabled=true`) |
| `"messenger"` | `incoming_cv_.wait_for(messenger_poll_ms)` | No |

When `loop_trigger="shm"`, `on_iteration` fires once per SHM slot available, with slot and
messages in the same call. When `loop_trigger="messenger"`, `on_iteration` fires when an
incoming ZMQ message arrives or after `messenger_poll_ms` timeout; `slot` is always `None`.

### 3.6 `ActorRoleAPI` proxy

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

### 3.7 LoopTimingPolicy — deadline scheduling for producer and consumer loops

The write loop (producer) and timeout loop (consumer) use deadline-based scheduling
rather than a simple `sleep(interval_ms)` at the top of each iteration. The deadline
advances by one `interval_ms` (or `timeout_ms`) tick per iteration, ensuring that the
average rate converges to the configured target regardless of callback duration.

**Policy choice** (`"loop_timing"` in the role JSON):

| Policy | Formula on overrun | Behaviour |
|---|---|---|
| `"fixed_pace"` (default) | `next = now() + interval` | Resets deadline from actual wakeup time; no catch-up burst; instantaneous rate ≤ target |
| `"compensating"` | `next += interval` | Advances one tick regardless; fires immediately after an overrun until caught up; average rate converges to target |

**On-time case** (body completed before deadline): both policies advance `next += interval` —
equivalent because wakeup ≈ deadline.

**Overrun detection**: when the timing check finds `now >= next_deadline`, no sleep is taken.
The loop counts this as an **overrun** (`api.loop_overrun_count()` increments by 1) and the two
policies diverge in how `next_deadline` is advanced.

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

**Consumer timeout policy**: the same `loop_timing` field controls how `last_slot_time` is
advanced after `on_iteration(slot=None, ...)` fires on timeout. `"fixed_pace"` resets from the
actual callback completion time; `"compensating"` advances by one `timeout_ms` tick.

### 3.8 RoleMetrics — supervised diagnostics

The Python script runs under supervision by the C++ host. The host collects diagnostic
counters **about** the script's execution and makes them available as read-only attributes
on the `api` object.

**Design principle**: the script may observe its own health but may **not** reset or write
these counters. This preserves the integrity of the metrics — the script cannot clear
evidence of its own misbehaviour. Counters are reset automatically on role restart by the
C++ host via `reset_all_role_run_metrics()`.

| Getter | Who writes | When | Semantics |
|---|---|---|---|
| `api.script_error_count()` | C++ host | Each `py::error_already_set` catch block | Python exceptions in any callback (`on_init`, `on_iteration`, `on_stop`) |
| `api.loop_overrun_count()` | C++ host | Each write-loop overrun | Cycles where `interval_ms` deadline was already past (no sleep taken). Producer only; 0 for consumers and when `interval_ms <= 0`. |
| `api.last_cycle_work_us()` | C++ host | After each successful write | Microseconds from start of acquire through commit + checksum. 0 until first write. Producer only. |

**All counters are per-role-run**: they reset to zero when `start()` is called. A role that
restarts gets a clean baseline — threshold logic in the script does not need to account for
errors from previous restart iterations.

**C++ internal write interface** (never accessible from Python):
```cpp
api_.increment_script_errors();              // called in every callback catch block
api_.increment_loop_overruns();              // called in the overrun else-branch
api_.set_last_cycle_work_us(elapsed_us);     // called after each successful write
api_.reset_all_role_run_metrics();           // called in start() before running_.store(true)
```

### 3.9 SharedSpinLockPy — cross-process spinlock for Python

`api.spinlock(idx)` returns a `SharedSpinLockPy` object backed by one of the 8
`SharedSpinLockState` slots in the SHM header. These slots are shared between all
processes that open the same channel (producer and all consumers).

All spinlock indices are pre-allocated in the SHM header — there is no reservation step.
The user's script must agree on which index is used for which purpose (convention, not
enforcement). The GIL is released while spinning; the lock is cross-process.

```python
# ── Context manager (preferred — always releases on exception) ─────────────────
with api.spinlock(0):
    flexzone.counter += 1
    api.update_flexzone_checksum()

# ── Explicit lock/unlock (for scopes that cross multiple with-blocks) ──────────
lk = api.spinlock(1)
lk.lock()
try:
    flexzone.calibration = new_value
    api.update_flexzone_checksum()
finally:
    lk.unlock()

# ── Non-blocking attempt ───────────────────────────────────────────────────────
lk = api.spinlock(2)
if lk.try_lock_for(timeout_ms=100):
    try:
        flexzone.status = "updating"
    finally:
        lk.unlock()

# ── Diagnostic ─────────────────────────────────────────────────────────────────
lk.is_locked_by_current_process()   # True while this process holds it
```

**Methods on `SharedSpinLockPy`**:

| Method | Description |
|---|---|
| `lock()` | Acquire (blocking spin) |
| `unlock()` | Release; raises `RuntimeError` if not held by this process |
| `try_lock_for(timeout_ms)` | Returns `True` on success, `False` on timeout |
| `is_locked_by_current_process()` | Diagnostic check |
| `__enter__` / `__exit__` | Context manager support |

**Lifecycle**: The `SharedSpinLockState` struct lives in SHM for the actor's lifetime.
The returned `SharedSpinLockPy` holds a copy of `SharedSpinLock` (which stores a pointer
to the SHM state + a 256-byte diagnostic name). Both the original and the copy refer to
the same SHM state — any process holding the same SHM segment sees the same lock.

**Use case — multi-role within one actor**: A producer role writing to `flexzone` and a
consumer role reading from it (in the same SHM) can use spinlock 0 to serialize access.

**Use case — cross-actor**: A producer actor and a separate consumer actor (different
processes, same SHM) can synchronize via the shared spinlock for protocol-level handshake.

---

## 4. C++ Abstraction Level

### 4.1 hub::Producer (C++ API)

The actor system builds on `hub::Producer` and `hub::Consumer` from `pylabhub-utils`.
C++ code can use these directly without the Python actor layer.

```cpp
#include "plh_datahub.hpp"   // hub::Producer, hub::Consumer, hub::Messenger

// ── Lifecycle setup ───────────────────────────────────────────────────────────
LifecycleGuard guard(MakeModDefList(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::hub::GetZMQContextModule(),
    pylabhub::hub::GetLifecycleModule()
));

// ── Producer ──────────────────────────────────────────────────────────────────
auto &messenger = pylabhub::hub::Messenger::get_instance();

hub::ProducerConfig pcfg;
pcfg.channel_name  = "lab.sensor.temperature";
pcfg.producer_uid  = "sensor_node_001";
pcfg.slot_size     = 24;          // bytes; must match consumer expectation
pcfg.slot_count    = 8;
pcfg.shm_secret    = 9876543210;
pcfg.broker_endpoint = messenger.endpoint();

hub::Producer producer;
producer.start(pcfg, messenger);

// Write a slot (typed struct)
struct SlotFrame {
    double  ts;
    float   value;
    uint8_t flags;
    uint8_t _pad[3];
};
static_assert(sizeof(SlotFrame) == 12);   // natural alignment check

auto txn = producer.begin_write();        // acquire SHM slot (blocks briefly)
if (txn.valid()) {
    auto *slot = txn.as<SlotFrame>();
    slot->ts    = current_time();
    slot->value = read_sensor();
    slot->flags = 0x01;
    txn.commit();                         // makes slot visible to consumers + ZMQ broadcast
}

// ZMQ unicast to a specific consumer
for (const auto &id : producer.connected_consumers())
    producer.send_to(id, "status", 6);

producer.stop();
```

### 4.2 hub::Consumer (C++ API)

```cpp
#include "plh_datahub.hpp"

hub::ConsumerConfig ccfg;
ccfg.channel_name    = "lab.sensor.temperature";
ccfg.consumer_uid    = "controller_001";
ccfg.timeout_ms      = 5000;
ccfg.broker_endpoint = messenger.endpoint();

hub::Consumer consumer;
consumer.connect(ccfg, messenger);  // sends HELLO; registers with producer

// Read loop
while (!shutdown) {
    auto slot_handle = consumer.acquire_consume_slot();   // wait for slot or timeout
    if (!slot_handle.valid()) {
        // timed_out or shutdown
        send_heartbeat();
        continue;
    }
    const auto *slot = slot_handle.as<SlotFrame>();
    process(slot->ts, slot->value, slot->flags);
    consumer.release_consume_slot(slot_handle);
}

consumer.close();   // sends BYE; releases SHM
```

### 4.3 ProducerRoleWorker (actor layer C++)

The Python actor system wraps `hub::Producer` in `ProducerRoleWorker`:

```cpp
namespace pylabhub::actor {

// Constructed by ActorHost::start() for each configured producer role.
// Owns hub::Producer, hub::Messenger (per-role), ctypes schema objects, and the write loop thread.
class ProducerRoleWorker {
public:
    // role_name:     matches key in JSON "roles" map
    // role_cfg:      from ActorConfig::roles[role_name] — carries broker + broker_pubkey
    // actor_uid:     from ActorConfig::actor_uid
    // auth:          from ActorConfig::auth — keyfile path for CurveZMQ
    // shutdown:      shared shutdown flag (all roles watch this)
    // script_module: imported Python module — on_iteration/on_init/on_stop looked up by attribute
    //
    // NOTE: No messenger parameter — each worker constructs its own hub::Messenger
    //       and calls messenger_.connect(role_cfg.broker, role_cfg.broker_pubkey)
    //       inside start(). Failed connect logs a warning and continues (degraded mode).
    explicit ProducerRoleWorker(
        const std::string        &role_name,
        const RoleConfig         &role_cfg,
        const std::string        &actor_uid,
        const ActorAuthConfig    &auth,
        std::atomic<bool>        &shutdown,
        const py::module_        &script_module);

    bool start();    // connect messenger; build ctypes types; call on_init; start loop_thread
    void stop();     // signal thread; join; call on_stop

private:
    hub::Messenger  messenger_;      // owned, per-role (value, not reference)
    py::object      py_on_iteration_; // looked up from script_module at construction
    py::object      py_on_init_;
    py::object      py_on_stop_;
    std::deque<IncomingMessage>  incoming_queue_;
    std::mutex                   incoming_mu_;
    std::condition_variable      incoming_cv_;
    // ...

    void run_loop_shm();       // loop_trigger=Shm  — blocks on acquire_write_slot
    void run_loop_messenger(); // loop_trigger=Messenger — blocks on incoming_cv_
    void run_zmq_thread_();    // Phase 2: polls peer ctrl socket; routes events to incoming_queue_

    std::thread            loop_thread_{};
    std::thread            zmq_thread_{};        // Phase 2: owns ZMQ socket polling
    std::atomic<uint64_t>  iteration_count_{0};  // Phase 2: incremented per iteration; read by zmq_thread_
};

// One per consumer role — symmetric design (same owned Messenger pattern).
class ConsumerRoleWorker { /* ... similar ... */ };

// Entry point — manages all roles.
// NOTE: ActorHost does NOT hold a Messenger. actor_main.cpp does NOT call
//       GetLifecycleModule() or Messenger::get_instance(). Each worker is self-contained.
class ActorHost {
public:
    explicit ActorHost(const ActorConfig &config);
    bool load_script(bool verbose = false);   // import Python module; look up on_iteration/on_init/on_stop
    bool start();                             // create workers for all configured roles
    void stop();                              // stop all workers
    void wait_for_shutdown();
    void signal_shutdown() noexcept;
};

} // namespace pylabhub::actor
```

#### Messenger ownership model

| Component | Messenger ownership | Who calls connect() |
|---|---|---|
| `HubShell` | Singleton (`Messenger::get_instance()`) | Lifecycle startup |
| `ProducerRoleWorker` | Owned value (`hub::Messenger messenger_`) | `start()` → `role_cfg_.broker` |
| `ConsumerRoleWorker` | Owned value (`hub::Messenger messenger_`) | `start()` → `role_cfg_.broker` |
| `actor_main.cpp` | None | N/A — no `GetLifecycleModule()` call |

ZMQ context remains process-wide via `GetZMQContextModule()`. Only the Messenger
lifecycle (singleton) is excluded from the actor's lifecycle list.

### 4.4 ActorScriptHost — Python Interpreter Ownership

> **Authoritative reference:** **HEP-CORE-0010 §3.7** contains the full `ActorScriptHost`
> specification: class interface, interpreter thread lifecycle diagram, GIL handoff sequence,
> `PyConfig.install_signal_handlers=0` rationale, and shutdown paths.
>
> **HEP-CORE-0011** defines the abstract `ScriptHost` / `PythonScriptHost` framework that
> `ActorScriptHost` inherits.

Summary: `ActorScriptHost` (`src/actor/actor_script_host.hpp/.cpp`) owns the CPython
interpreter on a dedicated interpreter thread. `actor_main.cpp` calls `startup_()` which
blocks until scripts are loaded and roles started, then waits on `g_shutdown`. On
`SIGINT`/`SIGTERM` or `api.stop()`, `shutdown_()` joins the interpreter thread cleanly.

### 4.5 Module-Convention Callback Resolution

There is no dispatch table. `ActorHost::load_script()` imports the configured Python module
via `importlib` with a synthetic alias, then passes the `py::module_` directly to each worker
constructor. Workers resolve callbacks at construction time via `py::getattr`:

```cpp
// In ProducerRoleWorker / ConsumerRoleWorker constructor:
py::gil_scoped_acquire g;
py_on_iteration_ = py::getattr(script_module, "on_iteration", py::none());
py_on_init_      = py::getattr(script_module, "on_init",      py::none());
py_on_stop_      = py::getattr(script_module, "on_stop",      py::none());
```

The module is imported with a unique alias to avoid sys.modules collisions when multiple
roles share the same underlying module:

```cpp
// Synthetic module name: _plh_{uid_8hex}_{module_name}
// Example: _plh_12345678_sensor_node
std::string alias = "_plh_" + uid_8hex + "_" + config_.script_module;
auto importlib   = py::module_::import("importlib");
script_module_   = importlib.attr("import_module")(config_.script_module);
py::module_::import("sys").attr("modules")[alias] = script_module_;
```

**Absence of `on_iteration`**: not an error. `ActorHost::start()` logs a warning and skips
the role. `on_init` and `on_stop` absence is silently tolerated.

**`actor_dispatch_table.hpp`**: deleted. All decorator factory functions (`make_factory()`,
`g_dispatch_table`, `on_write`, `on_read`, etc.) are removed from the embedded module.
The `pylabhub_actor` pybind11 module now only exposes `ActorRoleAPI` bindings.

### 4.5 Thread Interaction (Phase 2 — embedded mode)

Each role worker owns exactly **two threads**: `loop_thread_` and `zmq_thread_`.

#### 4.5.1 Thread responsibilities

| Thread | Responsibility |
|--------|---------------|
| `loop_thread_` | SHM acquire/release; GIL + Python `on_iteration()` calls; increments `iteration_count_` |
| `zmq_thread_` | `zmq_poll()` on peer/consumer ZMQ sockets; routes events to `incoming_queue_`; Phase 3: heartbeat |

#### 4.5.2 Initialization sequence

```
Main thread (ActorHost::start)
────────────────────────────────────────────────────────────────────────────────
messenger_.connect(broker)
Producer::create() / Consumer::connect()   ← all broker ACKs received here (blocking)
start_embedded()                           ← running_=true; NO threads launched
[build flexzone view under GIL]
running_.store(true)
zmq_thread_ starts    ← BEFORE on_init: mirrors old peer_thread/ctrl_thread timing
call_on_init()        ← ZMQ events during on_init are processed by running zmq_thread_
loop_thread_ starts
```

**Why `zmq_thread_` launches before `on_init`**: The old `peer_thread`/`ctrl_thread` inside
hub::Producer/Consumer were started by `start()` before `on_init` was called. Phase 2 preserves
this ordering so that ZMQ sends from `on_init` (e.g. `api.broadcast()`) are processed
immediately rather than queued until after `on_init` returns.

**`stop()` guard**: normal `if (!running_) return;` is replaced by
`if (!running_ && !loop_thread_.joinable() && !zmq_thread_.joinable()) return;`
This ensures `zmq_thread_` is always joined, even when `api.stop()` is called from `on_init`
(which sets `running_=false` while `zmq_thread_` is still joinable).

#### 4.5.3 Acquire timeout — policy-derived

The SHM acquire timeout is derived from the loop policy, not hardcoded:

**Producer (`run_loop_shm`):**
```
acquire_ms = (interval_ms > 0) ? interval_ms : kShmMaxRateMs (= 5 ms)
```
Rationale: after `step_write_deadline_()` returns, `next_deadline` is `interval_ms` away.
Using `interval_ms` as the acquire budget means a slot miss is treated as an overrun on
the next `step_write_deadline_()` call — no false overruns from a shorter timeout.

**Consumer (`run_loop_shm`):**
```
acquire_ms = (timeout_ms > 0)  ? timeout_ms         // timed: fire on_iteration(None) on miss
           : (timeout_ms == 0) ? kShmMaxRateMs       // max-rate: 5 ms; no callback on miss
           :                     kShmBlockMs         // indefinite (-1): 5000 ms; no callback
```
When `timeout_ms > 0` and no slot is acquired within `timeout_ms`: `on_iteration(None, fz, msgs, api)`
is called, delivering any queued ZMQ messages and notifying the script of the silence interval.
This supports watchdog and heartbeat use cases.

#### 4.5.4 Application-level heartbeat (Phase 3 — complete)

**Phase 3 (complete 2026-02-25)**: `zmq_thread_` (producer only) takes over per-channel
heartbeat responsibility from the Messenger's internal worker thread.

- `messenger_.suppress_periodic_heartbeat(channel)` — disables Messenger's own periodic
  HEARTBEAT_REQ for this channel.
- `messenger_.enqueue_heartbeat(channel)` — sends one immediate heartbeat (called at
  start to keep the channel alive during `on_init`).
- `zmq_thread_` sends a heartbeat only when **`iteration_count_` has advanced** since the
  last heartbeat AND the throttle window (`hb_interval`) has elapsed.

`hb_interval` is derived from config:
```
heartbeat_interval_ms > 0  → use directly
heartbeat_interval_ms = 0, interval_ms > 0  → 10 × interval_ms
heartbeat_interval_ms = 0, interval_ms = 0  → 2000 ms (max-rate default)
```

**Why application-level heartbeat**: a stalled Python loop (GIL deadlock, SHM full, slow
script) stops heartbeats even if TCP is alive — the broker's consumer-liveness timeout
eventually fires `CHANNEL_CLOSING_NOTIFY`, giving consumers a clean notification instead
of a silent channel disappearance.

**Consumer roles** do not own their channel — heartbeat responsibility stays with the
producer's `zmq_thread_`. Consumer `zmq_thread_` tracks `iteration_count_` for future
metrics but does not send heartbeats.

#### 4.5.5 Thread interaction diagram

```mermaid
sequenceDiagram
    participant LT as Loop Thread
    participant IQ as incoming_queue_
    participant ZT as ZMQ Thread
    participant P as Producer/Consumer<br/>(embedded mode)
    participant NET as ZMQ Network

    Note over LT,ZT: start() — zmq_thread_ launches BEFORE on_init; loop_thread_ after

    loop Each SHM iteration (loop_trigger=shm)
        LT->>P: acquire_slot(timeout_ms / kShmMaxRateMs / kShmBlockMs)
        P-->>LT: slot_handle or null
        LT->>IQ: drain_incoming_queue_()
        IQ-->>LT: messages[]
        Note over LT: acquire GIL
        LT->>LT: on_iteration(slot, fz, messages, api)
        Note over LT: release GIL
        LT->>P: commit / release_slot
        LT-->>ZT: iteration_count_.fetch_add(1) [atomic relaxed]
    end

    loop ZMQ poll (messenger_poll_ms = 5 ms default)
        ZT->>P: zmq_poll(peer/data/ctrl socket, 5 ms)
        NET-->>ZT: POLLIN event
        ZT->>P: handle_*_events_nowait()
        P-->>IQ: push IncomingMessage
        IQ-->>LT: incoming_cv_.notify_one()
        ZT->>ZT: iter advanced + hb_interval elapsed?
        ZT->>NET: messenger_.enqueue_heartbeat() [producer only]
    end

    Note over LT,ZT: stop() — running_=false; incoming_cv_.notify_all()
    LT->>LT: finish iteration; call_on_stop(api)
    ZT->>ZT: zmq_poll wakes (≤5 ms timeout)
    ZT->>ZT: exit loop
    Note over LT,ZT: stop() joins loop_thread_ then zmq_thread_
    Note over LT,ZT: producer_->stop(); producer_->close() (sends BYE + DEREG)
```

**Key ordering invariant**: `zmq_thread_` starts before `loop_thread_`, so any ZMQ sends in
`on_init` (e.g. `api.broadcast()`) are dispatched immediately by the running `zmq_thread_`.

**Stop guard**: `if (!running_ && !loop_thread_.joinable() && !zmq_thread_.joinable()) return`
ensures `zmq_thread_` is joined even when `api.stop()` is called from `on_init` (which
clears `running_` before `loop_thread_` is launched).

### 4.6 ctypes schema binding (zero-copy)

At `start()`, C++ builds a `ctypes.LittleEndianStructure` subclass from the JSON schema:

```cpp
// Build a ctypes struct class for a SchemaSpec (slot or flexzone).
// Returns py::object — a ctypes.LittleEndianStructure subclass.
py::object build_ctypes_struct(const SchemaSpec &spec, const std::string &class_name);

// Producer: writable zero-copy view into SHM slot
py::object make_slot_view_write(void *data, size_t size, py::object slot_type);
// Equivalent Python: slot_type.from_buffer(memoryview(data))  — writable

// Consumer: read-only zero-copy view into SHM slot
py::object make_slot_view_readonly(const void *data, size_t size, py::object slot_type);
// Equivalent Python:
//   mv = memoryview(bytes_obj).cast('B')  — readonly
//   slot_type.from_buffer(mv)             — TypeError on field write

// Persistent flexzone (created once at start(), passed to every callback)
// Producer: writable from_buffer into SHM flexzone span
// Consumer: read-only from_buffer into same SHM flexzone span
```

---

## 5. C-API / SHM Level

### 5.1 DataBlock SHM structure

The SHM segment used by a producer has this layout (simplified):

```
SharedMemoryHeader
├── magic / version / config_hash
├── DataBlockConfig
│   ├── hub_uid                  (channel name)
│   ├── producer_uid             (actor_uid from config)
│   ├── logical_unit_size        (ctypes.sizeof(SlotFrame))
│   ├── slot_count               (ring buffer capacity)
│   └── total_size
├── ControlZone
│   ├── DataBlockMutex           (OS-backed futex/eventcount)
│   ├── write_index              (atomic, next slot to write)
│   ├── read_index               (atomic, next slot to read)
│   └── consumer_bitmap          (bit per attached consumer)
├── SlotArray[slot_count]
│   Each slot:
│   ├── SharedSpinLock           (atomic PID-based)
│   ├── sequence_number          (uint64, wraps)
│   ├── checksum                 (BLAKE2b-256, optional)
│   └── data[logical_unit_size]  ← ctypes.from_buffer() here
└── FlexZone
    ├── checksum                 (BLAKE2b over flexzone data)
    └── data[flex_zone_size]     ← persistent flexzone from_buffer()
```

### 5.2 Producer write cycle (C-API level)

```c
// 1. Acquire slot (blocks until one is free)
DataBlockSlotHandle handle;
DataBlockError err = datablock_acquire_write_slot(db, &handle);
if (err != DATABLOCK_OK) { /* retry or shutdown */ }

// 2. Access slot data pointer
void *slot_data = datablock_slot_data_ptr(db, &handle);
size_t slot_size = datablock_slot_size(db);

// 3. Write (via ctypes in Python, or direct struct write in C++)
memset(slot_data, 0, slot_size);
SlotFrame *frame = (SlotFrame *)slot_data;
frame->ts    = get_time_double();
frame->value = read_adc();
frame->flags = 0x01;

// 4. Optionally write BLAKE2b checksum
datablock_update_slot_checksum(db, &handle);

// 5. Commit (increments write_index; wakes waiting consumers)
datablock_commit_write_slot(db, &handle);

// 6. ZMQ broadcast happens automatically via hub::Producer::write_thread
//    after commit (not at C-API level — it's a Producer abstraction detail)
```

### 5.3 Consumer read cycle (C-API level)

```c
// 1. Wait for a new slot (or timeout)
DataBlockSlotHandle handle;
int timeout_ms = 5000;
DataBlockError err = datablock_acquire_consume_slot(db, &handle, timeout_ms);

if (err == DATABLOCK_TIMEOUT) {
    // No data in 5 s — heartbeat or fallback
    send_heartbeat_via_zmq();
    return;
}
if (err != DATABLOCK_OK) { /* error handling */ }

// 2. Optionally verify BLAKE2b checksum
bool valid = datablock_verify_slot_checksum(db, &handle);
if (!valid && policy == SKIP) {
    datablock_release_consume_slot(db, &handle);
    return;
}

// 3. Read slot data (read-only — slot is locked against producer overwrite)
const void *slot_data = datablock_slot_data_ptr(db, &handle);
const SlotFrame *frame = (const SlotFrame *)slot_data;
process(frame->ts, frame->value, frame->flags);

// 4. Release (decrements consumer_bitmap; frees slot for reuse)
datablock_release_consume_slot(db, &handle);
```

### 5.4 FlexZone C-API

```c
// Producer: write flexzone data
void *fz = datablock_flexible_zone_ptr(db);
size_t fz_size = datablock_flexible_zone_size(db);
FlexFrame *fframe = (FlexFrame *)fz;
fframe->device_id   = 42;
fframe->sample_rate = 1000;
memcpy(fframe->label, "temperature", 11);
datablock_update_flexzone_checksum(db);

// Consumer: read flexzone (after attaching to the same SHM segment)
bool ok = datablock_verify_flexzone_checksum(db);
if (ok) {
    const FlexFrame *fframe = (const FlexFrame *)datablock_flexible_zone_ptr(db);
    printf("device_id=%u rate=%u\n", fframe->device_id, fframe->sample_rate);
}
```

---

## 6. Complete Example: sensor_node

See `share/scripts/python/examples/sensor_node.json` and `sensor_node.py` for a complete
multi-role actor with one producer (`raw_out`) and one consumer (`cfg_in`).

Key points demonstrated:
- One module (`sensor_node.py`) implements `on_init`, `on_iteration`, `on_stop` for both roles
- Producer `raw_out` writes a typed slot (ts, value, flags, samples[8]) at 10 Hz; returns `True`
- Producer flexzone carries device metadata (device_id, sample_rate, label)
- Consumer `cfg_in` receives setpoints from a separate controller channel
- Consumer loop fires with `slot=None` on timeout — used for periodic heartbeat
- Both roles share `current_setpoint` as a module-level variable (GIL serialises access)
- `messages` list carries ZMQ-routed data from the `incoming_queue_` (arrived since last iteration)

```
Usage:
    pylabhub-actor --config sensor_node.json
    pylabhub-actor --config sensor_node.json --validate    # print layout
    pylabhub-actor --config sensor_node.json --list-roles  # show role activation
    pylabhub-actor --config sensor_node.json --keygen      # generate keypair
```

---

## 7. Validation and Startup Diagnostics

### 7.1 `--validate` output (example)

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
    Total: 48 bytes  (ctypes.sizeof=48)

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

Role: cfg_in   (consumer)
  Channel:  lab.config.setpoints
  timeout_ms: 5000
  Slot: setpoint(float32)
  Python handler: on_iteration ✓   on_init ✓   on_stop ✓
```

### 7.2 Startup validation

| Check | When | On failure |
|---|---|---|
| `ctypes.sizeof(SlotFrame) == SHM logical_unit_size` | At `start()` per role | Fail fast, clear error |
| `on_iteration` attribute found in module | At `ActorHost::start()` | Warning logged; role not activated |
| `loop_trigger=shm` and `shm.enabled=false` | At `ActorConfig::from_json_file()` | `std::runtime_error`; actor exits |
| `"script"` field is an object (not a string) | At `ActorConfig::from_json_file()` | `std::runtime_error`; actor exits |
| Schema type is a known BLDS type | At `from_json_file()` per field | `std::runtime_error`; actor exits |

---

## 8. Actor Authentication

### 8.1 Threat model

Without authentication, any process can register a channel under an existing `actor_uid`.
The goal is to make identity cryptographically verifiable at startup — no per-cycle overhead.

### 8.2 Keypair lifecycle

1. `--keygen`: generate NaCl keypair → write to `auth.keyfile` (Argon2id-protected)
2. On `start()` with `auth.keyfile` present: load and decrypt keypair using `auth.password`
3. Password sourced from: literal JSON string (dev), `"env:VAR"` (production), or
   interactive prompt fallback

### 8.3 CurveZMQ integration

| Side | Component | Change |
|---|---|---|
| Broker | `BrokerService` | Add known-clients list; accept actor public key in REG_REQ |
| Actor | `Messenger` DEALER socket | Set `ZMQ_CURVE_SECRETKEY`, `ZMQ_CURVE_PUBLICKEY`, `ZMQ_CURVE_SERVERKEY` |
| BLDS metadata | Channel record | Include producer public key for consumer-to-producer encryption |

### 8.4 Runtime cost

- Key load/decrypt: ~5 ms at startup (Argon2id KDF, once only)
- CURVE ZMQ handshake: once per broker connection
- Per-cycle overhead: **zero** (ZMQ CURVE is at transport level)

---

## 9. Runtime Cost Analysis

| Item | Cost | Notes |
|---|---|---|
| Dispatch table lookup per callback | ~50 ns | `unordered_map::find`, cold-ish cache |
| GIL acquisition | 0–5 μs | Zero contention when roles run at different frequencies |
| `ctypes.from_buffer` (writable slot) | ~200 ns | Zero-copy into SHM slot |
| `ctypes.from_buffer` (readonly slot) | ~200 ns | Zero-copy; read-only memoryview |
| Python function call via pybind11 | ~500 ns | Includes args marshal + return unmarshal |
| `interval_ms` sleep | configured ms ± OS jitter | `std::this_thread::sleep_for` |
| **Total per 100 Hz write cycle** | **~3 μs** | 0.03% CPU at 100 Hz |

GIL contention with N active roles: N threads acquire GIL in FIFO order. Keep
callbacks short; do not sleep inside callbacks.

---

## 10. Known Limitations

| Limitation | Impact | Mitigation |
|---|---|---|
| Storing `slot` beyond its callback | Silent stale data (no crash; SHM mapped) | Python read-only binding adds write protection; `from_buffer` contract is documented |
| `interval_ms` overrun on slow callbacks | Body exceeds deadline → immediate next fire | `loop_timing="fixed_pace"` (default) resets deadline from now, capping rate; `"compensating"` catches up. Monitor `api.loop_overrun_count()` and `api.last_cycle_work_us()`. |
| GIL contention with many concurrent roles | Latency spikes | Keep callbacks short; limit ~4 active roles per actor |
| Script cannot be hot-reloaded | Restart on script change | By design; reload would require dispatch table reset |
| Typo in role name in decorator | Role silently not activated | Startup warns; `--validate` shows activation summary |
| CurveZMQ actor ↔ broker (client keypair) | Broker pubkey wired via `broker_pubkey`; actor private key loading deferred to Phase 2 | `--keygen` generates keypair; Phase 2 wires actor secret key into `Messenger::connect()` |
| `--keygen` not yet implemented | ~~Placeholder message printed~~ | **Implemented 2026-02-21** — `zmq_curve_keypair()` + JSON keypair file |

---

## 11. Gap Analysis — Status After 2026-02-21 Session

This section tracks what is **working**, what was **fixed in this session**, and what
remains open.  Items marked ✅ Fixed are verified by the 426/426 test suite.

### 11.1 What is ready

| Component | Status | Notes |
|---|---|---|
| `pylabhub-broker` executable | ✅ Ready | CurveZMQ keypair; REG/DISC/DEREG; consumer tracking |
| `pylabhub-actor` executable | ✅ Ready | Multi-role config; ctypes zero-copy schema; decorator dispatch |
| `hub::Producer` / `hub::Consumer` C++ API | ✅ Ready | Full lifecycle; SHM ring buffer; ZMQ broadcast; HELLO/BYE |
| Actor validation policy | ✅ Ready | slot/flexzone checksum; on_checksum_fail; on_python_error |
| SharedSpinLockPy | ✅ Ready | Cross-process spinlock for Python; context manager |
| UID format enforcement | ✅ Ready | HUB- / ACTOR- prefix; auto-generation; validation warning |
| `--validate` / `--list-roles` mode | ✅ Ready | Prints ctypes layout + handler activation; exit 0 |
| Example scripts | ✅ Ready | producer_counter.py/.json + consumer_logger.py/.json |
| `HubShell` + `AdminShell` | ✅ Ready | Admin REPL; `channels()` JSON; startup script |
| `HubConfig` single-file JSON | ✅ Ready | hub.json (compiled-in defaults + file); env var overrides |
| Demo launch scripts | ✅ Fixed 2026-02-21 | `demo.sh` (bash) + `demo.ps1` (PowerShell); not chmod+x by design |
| `consumer_logger.py` console output | ✅ Fixed 2026-02-21 | `print()` in on_init/on_iteration/on_stop |
| SHM cleanup on crash | ✅ Already working | `shm_unlink()` before create in data_block.cpp |
| Schema declaration hash (Layer 2) | ✅ Fixed 2026-02-21 | `compute_schema_hash()` wired into Producer/Consumer opts |
| `--keygen` implementation | ✅ Fixed 2026-02-21 | `zmq_curve_keypair()` + JSON keypair file; auto-creates parent dir |
| Timeout constants unified | ✅ Fixed 2026-02-21 | `timeout_constants.hpp`; cmake-overridable; magic numbers replaced |

### 11.2 Remaining open gaps

| Gap | Priority | Status | Description |
|---|---|---|---|
| **Actor lifecycle signals** | 🔴 High | Open | No way to cleanly stop an actor from HubShell/external code other than OS signal. Need AdminShell ZMQ endpoint registration at actor startup. |
| **CurveZMQ actor ↔ broker (server side)** | 🟢 Done | ✅ Fixed 2026-02-22 | `broker_pubkey` (Z85 40-char) added to `RoleConfig`; parsed from JSON; passed to `messenger_.connect(role_cfg_.broker, role_cfg_.broker_pubkey)` in each worker's `start()`. |
| **CurveZMQ actor ↔ broker (client keypair)** | 🟡 Medium | Open | `auth.keyfile` / `--keygen` generates actor CURVE25519 keypair; but the actor's own secret key is not yet passed to `Messenger::connect()`. Deferred to Security Phase 2. |
| **Multiple broker endpoints** | 🟢 Done | ✅ Fixed 2026-02-22 | Each role worker owns its own `hub::Messenger` and connects to `role.broker` in `start()`. Roles can point to different brokers. |
| **FlexZone startup race** | 🟡 Medium | Open | Consumer may call `on_init(flexzone)` before producer writes it. Mitigation: validate checksum in `on_init`; if invalid, treat as not-ready. |
| **Broker port conflict** | 🟡 Medium | Open | Port 5570 hardcoded in examples; no discovery fallback. |
| **Dynamic role activation** | 🔵 Low | Open | All roles activated at start(). No runtime enable/disable. |
| **Lua scripting** | 🔵 Low | Open | HEP-CORE-0005 proposed LuaJIT; not implemented. Python-only. |
| **Slot throughput metrics** | 🟢 Done | ✅ Fixed 2026-02-23 | `api.script_error_count()`, `api.loop_overrun_count()`, `api.last_cycle_work_us()` — supervised read-only diagnostics via `RoleMetrics` struct. C++ host writes; Python reads. Resets on role restart. |
| **Broker/producer crash recovery** | — | **By design: not attempted** | See §14. |

---

## 12. Actor Security Model — Public vs. Secret Boundary

This section defines what belongs in the **public actor config** (the `.json` file safe
to version-control) vs. the **private keyfile** (mode 600, never committed).

### 12.1 Design principle

The rule mirrors standard SSH/TLS practice:

> **Public config** = anything needed to *establish identity or connect*, but that reveals
> nothing that would allow impersonation.
> **Private keyfile** = the secret that *proves* the identity claim.

### 12.2 Public actor config (`actor.json`)

These fields belong in the main JSON file and may be committed to a version control system:

| Field | Reason |
|---|---|
| `actor.uid`, `actor.name` | Identity labels — public by nature |
| `actor.log_level` | Operational setting — no security impact |
| `actor.auth.keyfile` | Path to the keyfile — reveals nothing about the key content |
| `actor.auth.password` | Must be `"env:PLH_ACTOR_PASSWORD"` — the env var *name* is public; the *value* is secret and never in the file |
| `roles.*.kind`, `.channel`, `.broker` | Topology — which channels to connect to |
| `roles.*.broker_pubkey` | Broker's CURVE25519 public key (Z85, 40 chars) — identifies the server to connect to; NOT secret (equivalent to SSH host key) |
| `roles.*.slot_schema`, `.flexzone_schema` | Field layouts — public protocol description |
| `roles.*.shm.*` | SHM configuration including `shm.secret` — see note below |
| `roles.*.interval_ms`, `.timeout_ms` | Timing policy — no security impact |
| `roles.*.validation` | Checksum policy — no security impact |

**Note on `shm.secret`**: The SHM shared secret is a 64-bit integer used to prevent
accidental cross-producer attachment; it is *not* a cryptographic secret.  It provides
partition/namespace isolation (preventing a consumer from accidentally attaching to the
wrong producer when multiple producers use the same channel name).  It does not provide
confidentiality.  It may remain in the public config.  For high-security namespacing,
move it to the keyfile (see §12.3 extension note).

### 12.3 Private keyfile (`actor.key`)

The keyfile is generated by `pylabhub-actor --keygen` and contains:

```json
{
  "actor_uid":  "ACTOR-Sensor-DEADBEEF",
  "public_key": "Z85-encoded CURVE25519 public key (40 chars)",
  "secret_key": "Z85-encoded CURVE25519 private key (40 chars)",
  "_note":      "Keep secret_key private."
}
```

| Field | Why here |
|---|---|
| `public_key` | Included for reference (can be distributed to broker admin); the keyfile is the authoritative source |
| `secret_key` | The CURVE25519 private key — must NEVER appear in the public config |

**File security**: the keyfile should be `chmod 600`, owned by the user running the actor.
On systems with a secrets manager (Vault, k8s Secrets), the keyfile content should be
injected at runtime rather than stored on disk.

**What the broker needs from the actor**: only the `public_key` (to whitelist the actor).
The actor's `secret_key` never leaves the actor process.

**What the actor needs from the broker**: the broker's server public key.  This is stored
in `roles.*.broker_pubkey` (Z85, 40 chars) and is now wired to `Messenger::connect()`.
The broker public key is NOT secret — it is the equivalent of an SSH host key that every
client must know to verify the server.  Empty = plain TCP (no CURVE).

### 12.4 Future: Argon2id password wrapping

The current keyfile stores the secret key in plaintext Z85.  A future Phase 5 (SECURITY_TODO)
will wrap the secret key with Argon2id password derivation, so the keyfile at rest is safe
even if stolen.  The `actor.auth.password` / `"env:..."` field is already reserved for this.

---

## 13. Schema Validation Architecture — Three Independent Layers

pylabhub uses three independent, complementary schema validation mechanisms.
Each guards a different point in the data lifecycle.

### 13.1 Overview table

| Layer | Name | Where computed | When enforced | Protects against |
|---|---|---|---|---|
| **1 — Slot data checksum** | Per-write BLAKE2b | C++ after `on_iteration()` commits | Per slot, before consumer reads | Bit-flip / memory corruption during IPC |
| **2 — Schema declaration hash** | BLAKE2b of field list | C++ at `start()` from JSON | At `Consumer::connect()` | Mismatched field layout between independent actor configs |
| **3 — BLDS channel registry** | `SchemaBLDS::compute_hash()` | Broker at REG_REQ | Broker-enforced at registration | Channel schema changes while active consumers exist |

### 13.2 Layer 1 — Slot data checksum

**Mechanism**: after `on_iteration()` returns `True` and commits a slot, C++ calls `update_checksum_slot()` which
writes a BLAKE2b-256 digest of the slot bytes into the SHM control zone.  On the consumer
side, `verify_checksum_slot()` re-computes and compares before delivering the slot to Python.

**Config**: `validation.slot_checksum` = `"none"` / `"update"` (default) / `"enforce"`.
`validation.on_checksum_fail` = `"skip"` / `"pass"` (consumer policy).

**What it does NOT protect**: field layout mismatches.  If both sides have valid checksums
but different field widths, the data is checksum-correct but semantically wrong.  That is
Layer 2's job.

### 13.3 Layer 2 — Schema declaration hash

**Mechanism**: `compute_schema_hash()` in `actor_host.cpp` (anonymous namespace) builds a
canonical string from the parsed `SchemaSpec` field list (name, type, count, length) and
hashes it with BLAKE2b-256.  The result is set on:
- `ProducerOptions::schema_hash` — sent to broker in REG_REQ
- `ConsumerOptions::expected_schema_hash` — validated in `Messenger::connect_channel()`
  (lines 1116-1134 of messenger.cpp)

**What triggers a mismatch**: any difference in field name, type, count, or length between
the producer's `slot_schema` / `flexzone_schema` and the consumer's.

**What it does NOT protect**: wire corruption after connect; that is Layer 1's job.

**Canonical format** (for reference / debugging):
```
slot:name:type:count:len|name:type:count:len|...[|fz:name:type:count:len|...]
```
For numpy_array mode: `slot:numpy_array:dtype[:dim1:dim2:...]`

### 13.4 Layer 3 — BLDS channel registry

**Mechanism**: `SchemaBLDS::compute_hash()` hashes the full broker-level data schema
descriptor.  Stored by the broker in its channel registry at producer registration.
Returned to consumers in DISC_ACK.  Prevents a new producer from reusing a channel name
with an incompatible schema while existing consumers are active.

**What it does NOT protect**: cross-actor field layout agreement (Layer 2).

### 13.5 Why all three are needed

```
Producer writes slot
  │
  ├─ Layer 2: computed once at start(); ensures consumer was configured with
  │           the same field layout BEFORE the first slot is ever read.
  │           (Config-time check — catches developer mistakes.)
  │
  ├─ Layer 1: per-write checksum; guards in-flight SHM from bit flips or
  │           partial writes (hardware/OS fault protection).
  │           (Runtime check — catches infrastructure failures.)
  │
  └─ Layer 3: broker registry; prevents schema drift when the producer
              binary is replaced and channels are reused.
              (Operational check — catches deployment mistakes.)
```

---

## 14. Failure Model — What Is Recoverable and What Is Not

### 14.1 Guiding principle

Recovery logic is only justified when the post-crash state is **coherent enough to continue
correctly**. When it is not — when the attempt would operate on stale, partially-valid
information and could produce worse outcomes than a clean restart — recovery is the wrong
abstraction. The right response is a fast, clean exit with a clear error message.

This section identifies which failures fall into each category and what the correct system
response is for each.

### 14.2 Catastrophic — exit cleanly, do not attempt recovery

| Failure | Why unrecoverable | Correct response |
|---|---|---|
| **Broker crash** | All channel registration records, consumer lists, schema hashes, heartbeat timers are lost. The broker's in-memory state cannot be reconstructed from any surviving data. Re-registering with a restarted broker would create a new, empty channel — any consumers still attached to the old SHM would never receive DISC_ACK for it. | Heartbeat timeout fires `CHANNEL_CLOSING_NOTIFY`; `on_channel_closing` callback triggers actor shutdown. Exit cleanly. Operators restart the pipeline. |
| **Producer crash** (seen by consumer) | The SHM segment is unlinked on producer exit. The consumer holds a mapping to a segment that no longer exists or is being overwritten. There is no way to re-attach to a new producer without full re-discovery via the broker. | Consumer detects stale SHM (sequence number stall or CHANNEL_CLOSING_NOTIFY from broker). Clean exit. |
| **Consumer crash** (seen by producer) | Already handled. Broker detects dead PID via `check_consumer_health()` (Cat 2 liveness check) and sends `CONSUMER_DIED_NOTIFY`. Producer logs and continues — it does not block on dead consumers. | Already implemented. |
| **ZMQ context destroyed under active sockets** | Undefined behaviour. The shutdown ordering (`Messenger` → `ZMQContext`) in the lifecycle guard prevents this when lifecycle is managed. Unmanaged code paths must not destroy the context while Messenger is alive. | Lifecycle guard enforces ordering. No workaround needed in actor (no singleton Messenger). |

### 14.3 Manageable — handle gracefully

| Failure | Why manageable | Correct response |
|---|---|---|
| **Broker not reachable at startup** | No prior state to lose. Messenger connect fails → worker logs warning → actor runs in degraded mode (SHM write/read still works; no broker registration, no schema validation). | Already implemented: `messenger_.connect()` failure logs warning and continues. |
| **Broker temporarily unreachable (transient network blip) before first REG_ACK** | Channel not yet live; no consumers. Worst case: `create_channel()` returns `nullopt`. | Producer start fails; actor logs error and skips role. Retry is at operator level (restart). |
| **Slot checksum failure** | In-flight data corruption (hardware, partial write). Stateless per-slot check; SHM structure intact. | `on_checksum_fail` policy: `"skip"` (discard slot) or `"pass"` (deliver with `slot_valid()=false`). Already implemented. |
| **Python callback throws** | Script bug; worker continues or stops per `on_python_error` policy. | `"continue"` (log traceback, keep running) or `"stop"` (clean exit). Already implemented. |
| **SHM segment name collision** | Caught at `shm_open()` — producer detects and fails fast. Existing segment is inspected, not silently overwritten. | Fail fast with clear error at start. Already implemented in `data_block.cpp`. |

### 14.4 Operational contract

The pylabhub IPC stack is a **same-lifetime** system: broker, producers, and consumers are
expected to start together and stop together. It is not a durable message queue or a
persistent store. Data in flight at the time of any crash is lost. This is a deliberate
design choice that keeps the system simple, fast, and correct.

For deployments that require durability across crashes, the correct layer to add it is
**above** pylabhub — e.g., a supervisor process that restarts the pipeline in the right
order, or an external buffer (file, database) that the Python script writes to inside
`on_iteration`. pylabhub itself does not attempt to solve this.
