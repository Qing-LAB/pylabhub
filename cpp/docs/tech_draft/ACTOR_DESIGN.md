# pylabhub-actor Design

**Status**: Implemented — 2026-02-21 (UID format + SharedSpinLockPy added 2026-02-21; LoopTimingPolicy + RoleMetrics diagnostics added 2026-02-23)
**Supersedes**: previous single-role design (legacy flat JSON format — still accepted with warning)

---

## 1. Design Goals

1. **Single actor identity** — one `actor_uid` / `actor_name` regardless of how many
   channels the actor connects to or in which direction.
2. **Role palette** — the actor JSON declares a named set of roles (`producer` or
   `consumer`), each with its own channel, broker, schema, and timing policy. The Python
   script activates whichever subset it needs via per-role decorators.
3. **Typed zero-copy access** — slot and flexzone memory is presented to Python as a
   `ctypes.LittleEndianStructure` built from the JSON schema.  No struct packing/unpacking
   in user code.
4. **Timer-driven producer** — `interval_ms` makes the write loop a best-effort periodic
   poll.  `-1` enables event-driven write via `api.trigger_write()`.
5. **Timeout-aware consumer** — `timeout_ms` fires `on_read(slot=None, timed_out=True)`
   when no slot arrives, enabling periodic heartbeat or fallback computation.
6. **Zero per-cycle decorator cost** — decorators run once at import time and store the
   callable in a C++ `unordered_map`.  At call time: one `~50 ns` hash lookup.
7. **Authenticated identity** — optional NaCl keypair; `actor_uid` + password-protected
   private key; CurveZMQ broker authentication; provenance chain for SHM identity binding.

---

## 2. Config File Format

The new multi-role format uses an `actor` block and a `roles` map.
The legacy flat single-role format is still accepted with a deprecation warning.

```json
{
  "actor": {
    "uid":       "ACTOR-SENSOR-00000003",
    "name":      "TemperatureSensor",
    "log_level": "info",
    "auth": {
      "keyfile":  "~/.pylabhub/sensor_node_001.key",
      "password": "env:PLH_ACTOR_PASSWORD"
    }
  },
  "script": "sensor_node.py",

  "roles": {
    "raw_out": {
      "kind":        "producer",
      "channel":     "lab.sensor.temperature",
      "broker":      "tcp://127.0.0.1:5570",
      "interval_ms": 100,
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
      "broker":     "tcp://127.0.0.1:5570",
      "timeout_ms": 5000,
      "slot_schema": {
        "fields": [{"name": "setpoint", "type": "float32"}]
      }
    }
  }
}
```

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
Auto-generated from `hub_name` at HubConfig startup; can be overridden in `hub.user.json["hub"]["uid"]`.

#### Per-role fields

| Field | Applies to | Required | Default | Description |
|---|---|---|---|---|
| `kind` | both | yes | — | `"producer"` or `"consumer"` |
| `channel` | both | yes | — | Channel name to create/subscribe |
| `broker` | both | no | `tcp://127.0.0.1:5570` | Broker endpoint. Each role connects its own Messenger to this endpoint in `start()`. |
| `broker_pubkey` | both | no | `""` (plain TCP) | Broker CurveZMQ server public key (Z85, 40 chars). Required for encrypted broker connections. Empty = no CURVE. |
| `interval_ms` | producer | no | `0` | `0`=full throughput; `N>0`=deadline-scheduled (see `loop_timing`); `-1`=`trigger_write()` only |
| `timeout_ms` | consumer | no | `-1` | `-1`=wait indefinitely; `N>0`=fire `on_read(timed_out=True)` on silence (see `loop_timing`) |
| `loop_timing` | both | no | `"fixed_pace"` | Overrun policy for `interval_ms`/`timeout_ms` deadline advancement. See §3.6. |
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
| `on_checksum_fail` | `"skip"` | `"skip"` (discard slot) / `"pass"` (call on_read with `api.slot_valid()==false`) |
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

### 3.1 Decorator pattern

The embedded `pylabhub_actor` module exposes decorator factories. Scripts register
per-role handlers at import time. Handlers are stored in the C++ dispatch table.

**Key property**: `@actor.on_write("raw_out")` runs exactly **once** — at import time.
At call time, C++ does a single `~50 ns` `unordered_map::find`. Zero per-cycle overhead.

```python
import pylabhub_actor as actor
import time, os

# ── Module-level state (replaces any ctx.state concept) ───────────────────────
count    = 0
setpoint = 20.0

# ── Producer role "raw_out" ───────────────────────────────────────────────────

@actor.on_init("raw_out")
def raw_out_init(flexzone, api: actor.ActorRoleAPI):
    """
    Called once per role after SHM is created and ready.
    flexzone: writable ctypes struct backed by SHM flexible-zone region.
    Write device metadata and stamp the checksum here.
    """
    flexzone.device_id   = 42
    flexzone.sample_rate = 1000
    flexzone.label       = b"lab.sensor.temperature"
    api.update_flexzone_checksum()
    api.log('info', f"raw_out: started  uid={api.uid()}  pid={os.getpid()}")

@actor.on_write("raw_out")
def raw_out_write(slot, flexzone, api: actor.ActorRoleAPI) -> bool:
    """
    Called every interval_ms (or as fast as SHM allows if interval_ms=0).

    slot:     writable ctypes.LittleEndianStructure — zero-copy into SHM slot.
              Valid ONLY during this call.  Do not store.
    flexzone: persistent writable ctypes struct — safe to read/write and store.
    Returns True or None to commit; False to discard (no SHM commit, no ZMQ broadcast).
    """
    global count
    count += 1
    slot.ts    = time.time()
    slot.value = setpoint + 0.1   # simulated reading
    slot.flags = 0x01
    return True

@actor.on_message("raw_out")
def raw_out_message(sender: str, data: bytes, api: actor.ActorRoleAPI):
    """Called when a consumer sends a ZMQ ctrl frame to this producer role."""
    api.log('debug', f"raw_out: ctrl from {sender}: {data!r}")
    api.send(sender, b"ack")

@actor.on_stop("raw_out")
def raw_out_stop(flexzone, api: actor.ActorRoleAPI):
    api.log('info', f"raw_out: stopped after {count} writes")

# ── Consumer role "cfg_in" ────────────────────────────────────────────────────

@actor.on_init("cfg_in")
def cfg_in_init(flexzone, api: actor.ActorRoleAPI):
    """Called once before the consumer read loop starts."""
    api.log('info', f"cfg_in: connected  uid={api.uid()}")
    api.send_ctrl(b"hello")

@actor.on_read("cfg_in")
def cfg_in_read(slot, flexzone, api: actor.ActorRoleAPI, *, timed_out: bool = False):
    """
    slot:      read-only ctypes struct — zero-copy from_buffer on readonly memoryview.
               Field writes raise TypeError.  Valid ONLY during this call.
    timed_out: True when no slot arrived within timeout_ms.  slot is None then.
    api.slot_valid(): False when checksum failed and on_checksum_fail='pass'.
    """
    global setpoint
    if timed_out:
        api.send_ctrl(b"heartbeat")
        return
    if not api.slot_valid():
        api.log('warn', "cfg_in: checksum failed")
        return
    setpoint = slot.setpoint

@actor.on_data("cfg_in")
def cfg_in_data(data: bytes, api: actor.ActorRoleAPI):
    """Called for each ZMQ broadcast frame from the producer."""
    api.log('debug', f"cfg_in: zmq broadcast {len(data)} bytes")

@actor.on_stop_c("cfg_in")
def cfg_in_stop(flexzone, api: actor.ActorRoleAPI):
    api.log('info', "cfg_in: stopped")
```

### 3.2 Callback inventory

All callbacks are **per-role**. There is no global `on_init` / `on_stop`.
Each role's `on_init` / `on_stop` handles its own lifecycle.
`@actor.on_stop` is for producer roles; `@actor.on_stop_c` is for consumer roles.

#### Producer callbacks

| Decorator | Signature | When called |
|---|---|---|
| `@actor.on_init("role")` | `(flexzone, api)` | Once per role — SHM created and ready |
| `@actor.on_write("role")` | `(slot, flexzone, api) -> bool` | Every `interval_ms`; `True`/`None`=commit, `False`=discard |
| `@actor.on_message("role")` | `(sender: str, data: bytes, api)` | ZMQ ctrl frame from any consumer |
| `@actor.on_stop("role")` | `(flexzone, api)` | Once per role — after write loop exits |

#### Consumer callbacks

| Decorator | Signature | When called |
|---|---|---|
| `@actor.on_init("role")` | `(flexzone, api)` | Once per role — after SHM attach |
| `@actor.on_read("role")` | `(slot, flexzone, api, *, timed_out=False)` | Per slot; or on `timeout_ms` |
| `@actor.on_data("role")` | `(data: bytes, api)` | Per ZMQ broadcast frame |
| `@actor.on_stop_c("role")` | `(flexzone, api)` | Once per role — after read loop exits |

#### Call order

1. Parse JSON → build `ActorConfig`; load/generate NaCl keypair if `auth` present
2. `_clear_dispatch_table()` — resets any prior registration
3. Import script (`exec_script_file`) — decorators populate dispatch table
4. For each role in config that has matching handlers: create `ProducerRoleWorker` or `ConsumerRoleWorker`
5. For each active role: `on_init(flexzone, api)`, then start loop thread
6. On shutdown: stop all loop threads → `on_stop(flexzone, api)` per role

### 3.3 Object lifetimes

| Object | Valid window | Notes |
|---|---|---|
| `slot` (producer) | During `on_write` only | Writable `from_buffer` into SHM. **Do not store.** |
| `slot` (consumer) | During `on_read` only | Zero-copy `from_buffer` on read-only memoryview. Writing a field raises `TypeError`. **Do not store.** |
| `slot` (timed_out) | N/A | `None` when `timed_out=True`. |
| `flexzone` | Entire role lifetime | Persistent `from_buffer` into SHM. Safe to read, write (producer), or store. |
| `api` | Entire role lifetime | Stateless proxy. Safe to store. |

Producer `flexzone`: writable, backed by the SHM flexible-zone region.
Consumer `flexzone`: read-only zero-copy view of the producer's same region.

### 3.4 `ActorRoleAPI` proxy

One `ActorRoleAPI` instance per active role. Passed by reference to every callback of
that role. Stateless — all methods dispatch to C++ immediately. No heap allocation per call.

```python
# ── Common (all roles) ────────────────────────────────────────────────────────
api.log(level: str, msg: str)            # log through hub logger
api.uid() -> str                         # actor uid from config
api.role_name() -> str                   # name of this role ("raw_out", "cfg_in", ...)
api.stop()                               # request actor shutdown (all roles)

# ── Producer roles ────────────────────────────────────────────────────────────
api.broadcast(data: bytes) -> bool       # ZMQ to all connected consumers
api.send(identity: str, data: bytes) -> bool  # ZMQ unicast to one consumer
api.consumers() -> list                  # ZMQ identity strings of connected consumers
api.trigger_write()                      # wake write loop (interval_ms == -1 only)
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
```

### 3.5 LoopTimingPolicy — deadline scheduling for producer and consumer loops

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
@actor.on_write("raw_out")
def write(slot, fz, api):
    total = api.loop_overrun_count() + api.script_error_count()
    if total > 0 and total % 500 == 0:
        api.log('warn', f"overruns={api.loop_overrun_count()} "
                        f"work_us={api.last_cycle_work_us()}")
    slot.ts = time.time()
    return True
```

**Consumer timeout policy**: the same `loop_timing` field controls how `last_slot_time` is
advanced after `on_read(timed_out=True)` fires. `"fixed_pace"` resets from the actual callback
completion time; `"compensating"` advances by one `timeout_ms` tick.

### 3.6 RoleMetrics — supervised diagnostics

The Python script runs under supervision by the C++ host. The host collects diagnostic
counters **about** the script's execution and makes them available as read-only attributes
on the `api` object.

**Design principle**: the script may observe its own health but may **not** reset or write
these counters. This preserves the integrity of the metrics — the script cannot clear
evidence of its own misbehaviour. Counters are reset automatically on role restart by the
C++ host via `reset_all_role_run_metrics()`.

| Getter | Who writes | When | Semantics |
|---|---|---|---|
| `api.script_error_count()` | C++ host | Each `py::error_already_set` catch block | Python exceptions in any callback (on_init, on_write, on_read, on_data, on_message, on_stop) |
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

### 3.7 SharedSpinLockPy — cross-process spinlock for Python

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
    // role_name:    matches key in JSON "roles" map
    // role_cfg:     from ActorConfig::roles[role_name] — carries broker + broker_pubkey
    // actor_uid:    from ActorConfig::actor_uid
    // shutdown:     shared shutdown flag (all roles watch this)
    // on_init_fn:   py::object from dispatch table — may be py::none()
    // on_write_fn:  py::object (required; role not activated if absent)
    // on_message_fn / on_stop_fn: optional
    //
    // NOTE: No messenger parameter — each worker constructs its own hub::Messenger
    //       and calls messenger_.connect(role_cfg.broker, role_cfg.broker_pubkey)
    //       inside start(). Failed connect logs a warning and continues (degraded mode).
    explicit ProducerRoleWorker(
        const std::string  &role_name,
        const RoleConfig   &role_cfg,
        const std::string  &actor_uid,
        std::atomic<bool>  &shutdown,
        const py::object   &on_init_fn,
        const py::object   &on_write_fn,
        const py::object   &on_message_fn,
        const py::object   &on_stop_fn);

    bool start();    // connect messenger; build ctypes types; call on_init; start write_thread
    void stop();     // signal thread; join; call on_stop

    // Called by ActorRoleAPI::trigger_write() — wakes interval_ms==-1 loop
    void notify_trigger();

private:
    hub::Messenger messenger_;  // owned, per-role (value, not reference)
    // ...
};

// One per consumer role — symmetric design (same owned Messenger pattern).
class ConsumerRoleWorker { /* ... similar ... */ };

// Entry point — manages all roles.
// NOTE: ActorHost does NOT hold a Messenger. actor_main.cpp does NOT call
//       GetLifecycleModule() or Messenger::get_instance(). Each worker is self-contained.
class ActorHost {
public:
    explicit ActorHost(const ActorConfig &config);
    bool load_script(bool verbose = false);   // import Python, read dispatch table
    bool start();                             // create workers for all registered roles
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

### 4.4 ActorDispatchTable (C++ dispatch)

The embedded `pylabhub_actor` module owns a global dispatch table:

```cpp
struct ActorDispatchTable {
    // Shared (both producer and consumer use on_init / on_stop)
    std::unordered_map<std::string, py::object> on_init;     // role → fn(flexzone, api)
    std::unordered_map<std::string, py::object> on_stop_p;   // producer: fn(flexzone, api)
    std::unordered_map<std::string, py::object> on_stop_c;   // consumer: fn(flexzone, api)

    // Producer
    std::unordered_map<std::string, py::object> on_write;    // fn(slot, fz, api) -> bool
    std::unordered_map<std::string, py::object> on_message;  // fn(sender, data, api)

    // Consumer
    std::unordered_map<std::string, py::object> on_read;     // fn(slot, fz, api, *, timed_out=False)
    std::unordered_map<std::string, py::object> on_data;     // fn(data, api)

    void clear();
};

// Accessor — declared in actor_dispatch_table.hpp; defined in actor_module.cpp
ActorDispatchTable &get_dispatch_table();
```

### 4.5 ctypes schema binding (zero-copy)

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
- Producer `raw_out` writes a typed slot (ts, value, flags, samples[8]) at 10 Hz
- Producer flexzone carries device metadata (device_id, sample_rate, label)
- Consumer `cfg_in` receives setpoints from a separate controller channel
- Consumer uses `timed_out=True` path to send heartbeats
- Both roles share `current_setpoint` module-level variable (GIL serialises access)
- `@actor.on_stop` (producer) and `@actor.on_stop_c` (consumer) for distinct lifecycle hooks

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

  Python handler: on_write ✓   on_init ✓   on_message ✓   on_stop ✓

Role: cfg_in   (consumer)
  Channel:  lab.config.setpoints
  timeout_ms: 5000
  Slot: setpoint(float32)
  Python handler: on_read ✓   on_init ✓   on_stop_c ✓
```

### 7.2 Startup validation

| Check | When | On failure |
|---|---|---|
| `ctypes.sizeof(SlotFrame) == SHM logical_unit_size` | At `start()` per role | Fail fast, clear error |
| Role name in decorator matches config `roles` key | At `load_script()` | Warning logged; role not activated |
| Required handler present (`on_write` for producer, `on_read` or `on_data` for consumer) | At `start()` | Role not activated (warning) |
| Duplicate decorator for same event+role | At import time | `RuntimeError` from Python |

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
| `HubConfig` layered JSON | ✅ Ready | hub.default.json + hub.user.json; env var overrides |
| Demo launch scripts | ✅ Fixed 2026-02-21 | `demo.sh` (bash) + `demo.ps1` (PowerShell); not chmod+x by design |
| `consumer_logger.py` console output | ✅ Fixed 2026-02-21 | `print()` in on_init/on_read/on_stop_c |
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
| **1 — Slot data checksum** | Per-write BLAKE2b | C++ after `on_write()` | Per slot, before consumer reads | Bit-flip / memory corruption during IPC |
| **2 — Schema declaration hash** | BLAKE2b of field list | C++ at `start()` from JSON | At `Consumer::connect()` | Mismatched field layout between independent actor configs |
| **3 — BLDS channel registry** | `SchemaBLDS::compute_hash()` | Broker at REG_REQ | Broker-enforced at registration | Channel schema changes while active consumers exist |

### 13.2 Layer 1 — Slot data checksum

**Mechanism**: after `on_write()` commits a slot, C++ calls `update_checksum_slot()` which
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
`on_write`. pylabhub itself does not attempt to solve this.
