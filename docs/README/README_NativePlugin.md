# Native Plugin Developer Guide

This guide walks through building a native (C/C++) plugin for pyLabHub. Native
plugins are shared libraries (`.so` / `.dll`) that the role host loads at runtime
via `dlopen`. They implement the same callback contract as Python and Lua scripts
but execute as compiled machine code.

**When to choose native over Python/Lua:**

- Latency-critical inner loops (sub-microsecond per iteration)
- Wrapping existing C/C++ libraries without marshalling overhead
- Bare-metal performance with zero interpreter overhead
- Deterministic memory usage (no GC pauses)

---

## 1. Quick Start -- Minimal Producer Plugin

```c
#include <pylabhub/plugin_api.h>
#include <string.h>

/* --- Lifecycle --------------------------------------------------------- */

static const PlhPluginContext *g_ctx;

PLH_EXPORT bool plugin_init(const PlhPluginContext *ctx)
{
    g_ctx = ctx;
    plh_log(ctx->core, "info", "plugin_init: hello from native producer");
    return true;
}

PLH_EXPORT void plugin_finalize(void) {}
PLH_EXPORT void on_init(void)         {}
PLH_EXPORT void on_stop(void)         {}

/* --- ABI --------------------------------------------------------------- */

PLH_EXPORT const PlhAbiInfo *plugin_abi_info(void)
{
    static const PlhAbiInfo info = {
        .struct_size     = sizeof(PlhAbiInfo),
        .sizeof_void_ptr = sizeof(void *),
        .sizeof_size_t   = sizeof(size_t),
        .byte_order      = 1,  /* little-endian */
        .api_version     = PLH_PLUGIN_API_VERSION,
    };
    return &info;
}

/* --- Data callback ----------------------------------------------------- */

PLH_EXPORT bool on_produce(void *out_slot, size_t out_sz,
                           void *flexzone, size_t fz_sz)
{
    (void)flexzone; (void)fz_sz;
    if (!out_slot || out_sz < sizeof(float))
        return false;

    float *slot = (float *)out_slot;
    slot[0] = 42.0f;
    return true;   /* true = commit (publish), false = discard */
}
```

That is a complete, loadable producer plugin. The framework calls `plugin_init`
once, then `on_produce` every iteration, and `plugin_finalize` at shutdown.

---

## 2. Directory Structure

```
my-producer/
  producer.json                  # Role configuration
  script/
    native/
      libmy_producer.so          # Your compiled plugin (Linux)
      my_producer.dll            # (Windows equivalent)
```

The role host looks for the shared library in `<script.path>/script/native/`.
By convention `script.path` is `"."` (the role directory).

---

## 3. Configuration

`producer.json`:

```json
{
    "producer": {
        "uid": "PROD-NativeSensor-12345678",
        "name": "NativeSensor"
    },
    "out_hub_dir": "/var/pylabhub/my_hub",
    "out_channel": "sensor.accel",

    "loop_timing": "fixed_rate",
    "target_period_ms": 1.0,

    "checksum": "enforced",

    "out_transport": "shm",
    "out_shm_enabled": true,
    "out_shm_slot_count": 8,

    "out_slot_schema": {
        "fields": [
            {"name": "timestamp", "type": "float64"},
            {"name": "ax",        "type": "float32"},
            {"name": "ay",        "type": "float32"},
            {"name": "az",        "type": "float32"}
        ]
    },

    "script": {
        "type": "native",
        "path": ".",
        "library": "libmy_producer.so"
    }
}
```

The key difference from Python/Lua is `"type": "native"` and the `"library"`
field that names the shared object file.

---

## 4. Building Your Plugin

### 4.1 Compiler requirements

- C11 for pure C plugins, C++17 for the C++ convenience layer
- Same architecture and pointer width as the host binary (x86_64/aarch64)
- Same byte order as the host (virtually always little-endian)

### 4.2 Visibility flags

By default, all symbols in a shared library are exported on Linux. Best practice
is to hide everything by default and export only the plugin entry points:

```
-fvisibility=hidden
```

Every exported function uses the `PLH_EXPORT` macro, which expands to
`__attribute__((visibility("default")))` on Linux or `__declspec(dllexport)`
on Windows. This keeps the symbol table clean.

### 4.3 CMake snippet

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_producer_plugin LANGUAGES C CXX)

# Find the pylabhub install prefix (or set manually)
set(PYLABHUB_INCLUDE_DIR "/usr/local/include" CACHE PATH
    "Path containing pylabhub/plugin_api.h")

add_library(my_producer MODULE
    my_producer.c      # or .cpp
)

target_include_directories(my_producer PRIVATE
    ${PYLABHUB_INCLUDE_DIR}
)

# Hide all symbols except PLH_EXPORT ones
set_target_properties(my_producer PROPERTIES
    C_VISIBILITY_PRESET   hidden
    CXX_VISIBILITY_PRESET hidden
    PREFIX ""              # produces "my_producer.so", not "libmy_producer.so"
)
```

If you prefer a plain Makefile or build script:

```bash
# C plugin
gcc -std=c11 -shared -fPIC -fvisibility=hidden \
    -I/usr/local/include \
    -o libmy_producer.so my_producer.c

# C++ plugin
g++ -std=c++17 -shared -fPIC -fvisibility=hidden \
    -I/usr/local/include \
    -o libmy_producer.so my_producer.cpp
```

### 4.4 Include path

The only header you need is `<pylabhub/plugin_api.h>`. It has no transitive
dependencies -- it includes only `<stddef.h>` and `<stdint.h>` (plus
`<type_traits>` in C++ mode). Point your include path at the directory
containing the `pylabhub/` subdirectory.

---

## 5. Schema Declaration

### 5.1 Why schemas matter

The framework verifies at load time that your compiled struct layout matches
the JSON config schema. A mismatch (field order, type, padding) is a hard
error -- the plugin will not load. This prevents subtle data corruption from
struct layout disagreements.

### 5.2 PLH_DECLARE_SCHEMA

```c
#include <pylabhub/plugin_api.h>

/* Your struct -- must be standard-layout (C-compatible). */
#pragma pack(push, 1)  /* Use if packing="packed" in config */
typedef struct
{
    double timestamp;   /* float64 */
    float  ax;          /* float32 */
    float  ay;          /* float32 */
    float  az;          /* float32 */
} SlotFrame;
#pragma pack(pop)

/* Declare the schema. The framework calls plugin_schema_SlotFrame()
   and plugin_sizeof_SlotFrame() at load time. */
PLH_DECLARE_SCHEMA(SlotFrame,
    "timestamp:float64:1:0|ax:float32:1:0|ay:float32:1:0|az:float32:1:0",
    sizeof(SlotFrame))
```

### 5.3 Canonical schema string format

Each field is `name:type_str:count:length`, separated by `|`:

| Component | Meaning | Examples |
|-----------|---------|----------|
| `name`    | Field name, must match JSON config | `timestamp`, `value` |
| `type_str`| Data type | `float32`, `float64`, `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`, `string`, `bytes` |
| `count`   | Array element count (1 for scalars) | `1`, `3`, `16` |
| `length`  | String/bytes max length (0 for numerics) | `0`, `256` |

Examples:

```
"value:float32:1:0"                       # single float
"samples:float64:256:0"                   # array of 256 doubles
"label:string:1:64"                       # string, max 64 bytes
"ts:float64:1:0|x:float32:1:0|y:float32:1:0"   # multi-field
```

### 5.4 Schema names

Use these names depending on your role:

| Role | Slot schema name | Flexzone schema name | Inbox schema name |
|------|-----------------|---------------------|-------------------|
| Producer | `SlotFrame` | `FlexFrame` | `InboxFrame` |
| Consumer | `SlotFrame` | `FlexFrame` | `InboxFrame` |
| Processor (input) | `InSlotFrame` | -- | `InboxFrame` |
| Processor (output) | `OutSlotFrame` | `FlexFrame` | -- |

### 5.5 Packing

If your JSON config specifies `"packing": "packed"`, use `#pragma pack(push, 1)`
around your struct definitions. If `"packing": "aligned"` (the default), use
natural C struct alignment (no pragma needed).

---

## 6. C++ Typed Access -- the Convenience Layer

For C++ plugins, `plugin_api.h` provides zero-cost typed wrappers and export
macros that eliminate raw pointer casting.

### 6.1 plh::SlotRef and plh::ConstSlotRef

```cpp
plh::SlotRef<T>       // writable reference (producer output, processor output)
plh::ConstSlotRef<T>  // read-only reference (consumer input, processor input)
```

Both assert `std::is_standard_layout_v<T>` at compile time. They validate
pointer and size at construction and convert to `bool`:

```cpp
plh::SlotRef<SlotFrame> slot(raw_ptr, raw_sz);
if (!slot) return false;  // null or too small
slot->timestamp = get_time();
```

### 6.2 Complete C++ producer example

```cpp
#include <pylabhub/plugin_api.h>
#include <cstdint>
#include <cmath>

/* ----- Slot type ------------------------------------------------------- */

struct SlotFrame
{
    double   timestamp;
    float    value;
    uint32_t sequence;
};

PLH_DECLARE_SCHEMA(SlotFrame,
    "timestamp:float64:1:0|value:float32:1:0|sequence:uint32:1:0",
    sizeof(SlotFrame))

/* ----- Plugin state ---------------------------------------------------- */

static const PlhPluginContext *g_ctx = nullptr;
static uint32_t g_seq = 0;

extern "C" {
PLH_EXPORT bool plugin_init(const PlhPluginContext *ctx)
{
    g_ctx = ctx;
    g_seq = 0;
    return true;
}
PLH_EXPORT void plugin_finalize(void) { g_ctx = nullptr; }

PLH_EXPORT const PlhAbiInfo *plugin_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo), sizeof(void *), sizeof(size_t),
        1, PLH_PLUGIN_API_VERSION
    };
    return &info;
}

PLH_EXPORT void on_init(void)
{
    plh_log(g_ctx->core, "info", "Native producer starting");
}

PLH_EXPORT void on_stop(void)
{
    plh_log(g_ctx->core, "info", "Native producer stopping");
}
} /* extern "C" */

/* ----- Typed produce callback ------------------------------------------ */

static bool my_produce(plh::SlotRef<SlotFrame> slot)
{
    if (!slot) return false;

    slot->timestamp = 0.0;          /* replaced by real clock in production */
    slot->value     = std::sin(static_cast<float>(g_seq) * 0.01f);
    slot->sequence  = g_seq++;

    if (g_seq % 1000 == 0)
        plh_report_metric(g_ctx->core, "iterations", static_cast<double>(g_seq));

    return true;   /* commit */
}

PLH_EXPORT_PRODUCE_NOFZ(SlotFrame, my_produce)
```

### 6.3 Complete C++ consumer example

```cpp
#include <pylabhub/plugin_api.h>
#include <cstdio>

struct SlotFrame
{
    double   timestamp;
    float    value;
    uint32_t sequence;
};

PLH_DECLARE_SCHEMA(SlotFrame,
    "timestamp:float64:1:0|value:float32:1:0|sequence:uint32:1:0",
    sizeof(SlotFrame))

static const PlhPluginContext *g_ctx = nullptr;
static uint64_t g_count = 0;

extern "C" {
PLH_EXPORT bool plugin_init(const PlhPluginContext *ctx)
{
    g_ctx = ctx;
    g_count = 0;
    return true;
}
PLH_EXPORT void plugin_finalize(void) {}

PLH_EXPORT const PlhAbiInfo *plugin_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo), sizeof(void *), sizeof(size_t),
        1, PLH_PLUGIN_API_VERSION
    };
    return &info;
}
PLH_EXPORT void on_init(void) {}
PLH_EXPORT void on_stop(void)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "Consumed %lu slots total", (unsigned long)g_count);
    plh_log(g_ctx->core, "info", buf);
}
} /* extern "C" */

static void my_consume(plh::ConstSlotRef<SlotFrame> slot)
{
    if (!slot) return;
    ++g_count;

    /* Process the data -- e.g., log every 100th sample */
    if (slot->sequence % 100 == 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "seq=%u value=%.4f",
                 slot->sequence, slot->value);
        plh_log(g_ctx->core, "debug", buf);
    }
}

PLH_EXPORT_CONSUME_NOFZ(SlotFrame, my_consume)
```

### 6.4 Complete C++ processor example

```cpp
#include <pylabhub/plugin_api.h>
#include <cmath>

/* ----- Input schema ---------------------------------------------------- */

struct InSlotFrame
{
    double timestamp;
    float  raw_value;
};

PLH_DECLARE_SCHEMA(InSlotFrame,
    "timestamp:float64:1:0|raw_value:float32:1:0",
    sizeof(InSlotFrame))

/* ----- Output schema --------------------------------------------------- */

struct OutSlotFrame
{
    double timestamp;
    float  filtered_value;
    float  gain;
};

PLH_DECLARE_SCHEMA(OutSlotFrame,
    "timestamp:float64:1:0|filtered_value:float32:1:0|gain:float32:1:0",
    sizeof(OutSlotFrame))

/* ----- Plugin state ---------------------------------------------------- */

static const PlhPluginContext *g_ctx = nullptr;
static float g_gain = 1.0f;

extern "C" {
PLH_EXPORT bool plugin_init(const PlhPluginContext *ctx)
{
    g_ctx = ctx;
    return true;
}
PLH_EXPORT void plugin_finalize(void) {}
PLH_EXPORT void on_init(void) {}
PLH_EXPORT void on_stop(void) {}

PLH_EXPORT const PlhAbiInfo *plugin_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo), sizeof(void *), sizeof(size_t),
        1, PLH_PLUGIN_API_VERSION
    };
    return &info;
}
} /* extern "C" */

/* ----- Typed process callback ------------------------------------------ */

static bool my_process(plh::ConstSlotRef<InSlotFrame> in,
                       plh::SlotRef<OutSlotFrame>     out)
{
    if (!in || !out) return false;

    out->timestamp      = in->timestamp;
    out->filtered_value = in->raw_value * g_gain;
    out->gain           = g_gain;

    return true;   /* commit output */
}

PLH_EXPORT_PROCESS_NOFZ(InSlotFrame, OutSlotFrame, my_process)
```

### 6.5 Export macro reference

| Macro | Signature of your function |
|-------|---------------------------|
| `PLH_EXPORT_PRODUCE(Slot, Flex, fn)` | `bool fn(SlotRef<Slot>, SlotRef<Flex>)` |
| `PLH_EXPORT_PRODUCE_NOFZ(Slot, fn)` | `bool fn(SlotRef<Slot>)` |
| `PLH_EXPORT_CONSUME(Slot, Flex, fn)` | `void fn(ConstSlotRef<Slot>, ConstSlotRef<Flex>)` |
| `PLH_EXPORT_CONSUME_NOFZ(Slot, fn)` | `void fn(ConstSlotRef<Slot>)` |
| `PLH_EXPORT_PROCESS(In, Out, Flex, fn)` | `bool fn(ConstSlotRef<In>, SlotRef<Out>, SlotRef<Flex>)` |
| `PLH_EXPORT_PROCESS_NOFZ(In, Out, fn)` | `bool fn(ConstSlotRef<In>, SlotRef<Out>)` |

---

## 7. Framework Helpers

These functions are provided by the host process. Call them from any callback.

### 7.1 Logging

```c
plh_log(g_ctx->core, "info", "Sensor initialized");
plh_log(g_ctx->core, "warn", "Buffer nearly full");
plh_log(g_ctx->core, "error", "Hardware read failed");
plh_log(g_ctx->core, "debug", "Slot written: seq=42");
```

Log levels: `"debug"`, `"info"`, `"warn"`, `"error"`. Messages go through the
async pylabHub logger, same as Python/Lua log output.

### 7.2 Metrics reporting

```c
plh_report_metric(g_ctx->core, "temperature_max", 85.3);
plh_report_metric(g_ctx->core, "dropped_samples", (double)drop_count);
```

Reported metrics are collected by the broker's metrics plane and included in
the role's metrics snapshot.

### 7.3 Requesting stop

```c
/* Graceful shutdown after current iteration */
plh_request_stop(g_ctx->core);
```

Equivalent to `api.stop()` in Python. The main loop exits after the current
iteration completes.

### 7.4 Critical error

```c
/* Signal unrecoverable error -- role exits immediately */
plh_set_critical_error(g_ctx->core);
```

Use for hardware failures, memory corruption, or any state where continuing
would produce incorrect data.

---

## 8. Verification

### 8.1 Schema verification

At load time the framework:

1. Calls `plugin_schema_SlotFrame()` (or the appropriate name for your role)
2. Parses the canonical schema string
3. Compares field names, types, counts, and lengths against the JSON config
4. Calls `plugin_sizeof_SlotFrame()` and compares against the computed size

Any mismatch is a hard error -- the role will not start. This catches:
- Struct padding differences (aligned vs packed)
- Field reordering
- Type mismatches (e.g., `int32` in config but `int64` in code)

### 8.2 ABI verification

`plugin_abi_info()` returns a `PlhAbiInfo` struct. The framework checks:

| Field | Host expects |
|-------|-------------|
| `struct_size` | `sizeof(PlhAbiInfo)` -- catches version skew |
| `sizeof_void_ptr` | Same as host (4 or 8) -- catches 32/64-bit mismatch |
| `sizeof_size_t` | Same as host |
| `byte_order` | Same as host |
| `api_version` | `PLH_PLUGIN_API_VERSION` -- catches stale headers |

Implement `plugin_abi_info()` in every plugin. Without it, the framework
skips ABI checks and you risk silent data corruption on architecture mismatch.

### 8.3 Checksum

When `"checksum": "enforced"` is set in the role config, the framework
automatically computes BLAKE2b checksums on `write_commit` and verifies on
read. Native plugins do not need to compute checksums manually -- the
framework handles it.

With `"checksum": "manual"`, your plugin is responsible for calling the
appropriate checksum API. `"none"` disables checksums entirely.

---

## 9. Threading Model

### 9.1 Default: single-threaded callbacks

By default, the framework calls data callbacks (`on_produce`, `on_consume`,
`on_process`) from a single thread. No locking is needed in your plugin
for these callbacks.

### 9.2 plugin_is_thread_safe()

```c
PLH_EXPORT bool plugin_is_thread_safe(void) { return false; }
```

`on_heartbeat()` is called from the **control thread**, not the data thread.
If `plugin_is_thread_safe()` returns `false` (or is not implemented), the
framework serializes `on_heartbeat()` with data callbacks.

If it returns `true`, `on_heartbeat()` may execute concurrently with data
callbacks. In that case, you must use appropriate synchronization (atomics,
mutexes) for any shared state accessed by both `on_heartbeat()` and
`on_produce`/`on_consume`/`on_process`.

### 9.3 on_inbox threading

`on_inbox()` is called from the inbox thread. If your plugin accesses shared
state from both `on_inbox` and data callbacks, you must synchronize access.

---

## 10. Common Patterns

### 10.1 Parameter update via inbox

A control role sends tuning parameters to a running producer:

```cpp
#include <pylabhub/plugin_api.h>
#include <atomic>
#include <cstring>

struct SlotFrame { double timestamp; float value; };
struct InboxFrame { int32_t cmd; float gain; };

PLH_DECLARE_SCHEMA(SlotFrame,
    "timestamp:float64:1:0|value:float32:1:0", sizeof(SlotFrame))
PLH_DECLARE_SCHEMA(InboxFrame,
    "cmd:int32:1:0|gain:float32:1:0", sizeof(InboxFrame))

static const PlhPluginContext *g_ctx = nullptr;
static std::atomic<float> g_gain{1.0f};

extern "C" {

PLH_EXPORT bool plugin_init(const PlhPluginContext *ctx)
{
    g_ctx = ctx;
    return true;
}
PLH_EXPORT void plugin_finalize(void) {}
PLH_EXPORT void on_init(void) {}
PLH_EXPORT void on_stop(void) {}

PLH_EXPORT const PlhAbiInfo *plugin_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo), sizeof(void *), sizeof(size_t),
        1, PLH_PLUGIN_API_VERSION
    };
    return &info;
}

PLH_EXPORT void on_inbox(const void *data, size_t sz, const char *sender_uid)
{
    plh::ConstSlotRef<InboxFrame> msg(data, sz);
    if (!msg) return;

    if (msg->cmd == 1)   /* SET_GAIN */
    {
        g_gain.store(msg->gain, std::memory_order_relaxed);

        char buf[128];
        snprintf(buf, sizeof(buf), "Gain updated to %.3f by %s",
                 msg->gain, sender_uid);
        plh_log(g_ctx->core, "info", buf);
    }
}

} /* extern "C" */

static bool my_produce(plh::SlotRef<SlotFrame> slot)
{
    if (!slot) return false;

    slot->timestamp = 0.0;
    slot->value     = 1.0f * g_gain.load(std::memory_order_relaxed);
    return true;
}

PLH_EXPORT_PRODUCE_NOFZ(SlotFrame, my_produce)
```

Key point: `on_inbox` runs on the inbox thread, `on_produce` on the data
thread. Use `std::atomic` for shared state.

### 10.2 Periodic metrics reporting

```c
PLH_EXPORT bool on_produce(void *out, size_t out_sz,
                           void *fz, size_t fz_sz)
{
    static uint64_t total = 0;
    /* ... fill slot ... */

    ++total;
    if (total % 10000 == 0)
    {
        plh_report_metric(g_ctx->core, "total_produced", (double)total);
    }
    return true;
}
```

### 10.3 Graceful shutdown on error

```c
PLH_EXPORT bool on_produce(void *out, size_t out_sz,
                           void *fz, size_t fz_sz)
{
    int err = read_hardware(out, out_sz);
    if (err == HW_FATAL)
    {
        plh_log(g_ctx->core, "error", "Hardware fatal error, requesting stop");
        plh_set_critical_error(g_ctx->core);
        return false;
    }
    if (err == HW_TRANSIENT)
    {
        plh_log(g_ctx->core, "warn", "Transient read error, discarding slot");
        return false;   /* discard this iteration, loop continues */
    }
    return true;
}
```

---

## 11. Plugin Lifecycle Summary

```
dlopen(library)
  |
  v
plugin_abi_info()       -- ABI check (pointer size, endianness, API version)
  |
  v
plugin_schema_*()       -- Schema check (struct layout vs JSON config)
plugin_sizeof_*()
  |
  v
plugin_init(ctx)        -- One-time initialization; return false to abort
  |
  v
on_init()               -- Role is registered with broker, data plane ready
  |
  v
  +-- on_produce() / on_consume() / on_process()  [data thread, repeated]
  +-- on_heartbeat()    [control thread, periodic]
  +-- on_inbox()        [inbox thread, on message]
  |
  v
on_stop()               -- Role is shutting down, last chance to flush
  |
  v
plugin_finalize()       -- Release all resources; no framework calls after this
  |
  v
dlclose(library)
```

---

## 12. Cross-References

| Topic | Document |
|-------|----------|
| Native plugin API specification | `HEP-CORE-0028` |
| Getting started with pyLabHub | `README_GettingStarted.md` |
| Full config field reference | `README_Deployment.md` |
| Embedded C++ API (linking against pylabhub-utils) | `README_EmbeddedAPI.md` |
| Schema registry | `HEP-CORE-0016-NamedSchemaRegistry.md` |
| Checksum policy | `HEP-CORE-0009-Policy-Reference.md` |
| Inbox messaging | `HEP-CORE-0027-Inbox-Messaging.md` |
