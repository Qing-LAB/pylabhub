# pylabhub-actor Design

**Status**: Implemented — 2026-02-21
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
    "uid":       "sensor_node_001",
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
| `uid` | no | `""` | Stable unique identifier (UUID or custom string). |
| `name` | no | `""` | Human-readable name. |
| `log_level` | no | `"info"` | `debug` / `info` / `warn` / `error` |
| `auth.keyfile` | no | absent | Path to NaCl keypair file. Activates CurveZMQ. |
| `auth.password` | no | absent | Passphrase. `"env:VAR"` reads `$VAR` at startup. |

#### Per-role fields

| Field | Applies to | Required | Default | Description |
|---|---|---|---|---|
| `kind` | both | yes | — | `"producer"` or `"consumer"` |
| `channel` | both | yes | — | Channel name to create/subscribe |
| `broker` | both | no | `tcp://127.0.0.1:5570` | Broker endpoint |
| `interval_ms` | producer | no | `0` | `0`=as fast as SHM; `N>0`=sleep N ms; `-1`=trigger_write() only |
| `timeout_ms` | consumer | no | `-1` | `-1`=wait indefinitely; `N>0`=fire `on_read(timed_out=True)` |
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
```

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
// Owns hub::Producer, ctypes schema objects, and the write loop thread.
class ProducerRoleWorker {
public:
    // role_name:    matches key in JSON "roles" map
    // role_cfg:     from ActorConfig::roles[role_name]
    // actor_uid:    from ActorConfig::actor_uid
    // messenger:    shared Messenger (one per broker endpoint)
    // shutdown:     shared shutdown flag (all roles watch this)
    // on_init_fn:   py::object from dispatch table — may be py::none()
    // on_write_fn:  py::object (required; role not activated if absent)
    // on_message_fn / on_stop_fn: optional
    explicit ProducerRoleWorker(
        const std::string  &role_name,
        const RoleConfig   &role_cfg,
        const std::string  &actor_uid,
        hub::Messenger     &messenger,
        std::atomic<bool>  &shutdown,
        const py::object   &on_init_fn,
        const py::object   &on_write_fn,
        const py::object   &on_message_fn,
        const py::object   &on_stop_fn);

    bool start();    // build ctypes types; call on_init; start write_thread
    void stop();     // signal thread; join; call on_stop

    // Called by ActorRoleAPI::trigger_write() — wakes interval_ms==-1 loop
    void notify_trigger();
};

// One per consumer role — symmetric design.
class ConsumerRoleWorker { /* ... similar ... */ };

// Entry point — manages all roles.
class ActorHost {
public:
    explicit ActorHost(const ActorConfig &config, hub::Messenger &messenger);
    bool load_script(bool verbose = false);   // import Python, read dispatch table
    bool start();                             // create workers for all registered roles
    void stop();                              // stop all workers
    void wait_for_shutdown();
    void signal_shutdown() noexcept;
};

} // namespace pylabhub::actor
```

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
| `interval_ms` is best-effort (no drift correction) | Cumulative drift | Use `interval_ms=0` (SHM pacing) for real-time; or `-1` + `trigger_write()` |
| GIL contention with many concurrent roles | Latency spikes | Keep callbacks short; limit ~4 active roles per actor |
| Script cannot be hot-reloaded | Restart on script change | By design; reload would require dispatch table reset |
| Typo in role name in decorator | Role silently not activated | Startup warns; `--validate` shows activation summary |
| Multiple brokers deferred | All roles share singleton Messenger | Each role's `broker` field is stored but current Messenger uses HubConfig endpoint |
| `--keygen` not yet implemented | Placeholder message printed | See SECURITY_TODO Phase 5 |
