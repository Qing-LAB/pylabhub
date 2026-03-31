# HEP-CORE-0028: Native Plugin Engine

**Status**: Implemented (documenting existing system)
**Created**: 2026-03-30
**Scope**: NativeEngine, plugin_api.h, C/C++ plugin lifecycle, ABI verification, schema validation

---

## 1. Motivation

Python and Lua script engines provide rapid prototyping and flexible callback authoring,
but they impose inherent costs:

- **Marshaling overhead**: Every data callback requires converting raw slot bytes into
  script-native types (ctypes structs for Python, FFI casts for Lua) and interpreting
  return values back into C++ enums. For high-frequency producers (>10 kHz), this
  overhead dominates the iteration budget.
- **Non-deterministic latency**: Garbage collection pauses (Python GC, Lua incremental GC)
  introduce jitter that violates fixed-rate timing contracts. Python's GIL further
  constrains throughput in multi-threaded scenarios.
- **Existing C/C++ codebases**: Scientific instruments often ship vendor libraries with
  C APIs. Wrapping them in Python ctypes or Lua FFI adds complexity and a fragile
  abstraction layer. A native plugin can call vendor APIs directly.

The Native Plugin Engine eliminates these costs by loading compiled shared libraries
(.so/.dll/.dylib) and dispatching callbacks as direct function pointer calls. The plugin
receives raw slot pointers — the same memory the framework uses internally — with zero
copy, zero conversion, and deterministic call overhead (one indirect call per callback).

---

## 2. Architecture

### 2.1 Component Model

```
                    Role Host Process
┌────────────────────────────────────────────────────────┐
│                                                        │
│  RoleHostCore                                          │
│  ├─ NativeEngine (ScriptEngine subclass)               │
│  │   ├─ dlopen(plugin.so)                              │
│  │   ├─ resolve symbols: plugin_init, on_produce, ...  │
│  │   ├─ verify ABI (PlhAbiInfo)                        │
│  │   ├─ verify file checksum (BLAKE2b-256)             │
│  │   ├─ verify schema (canonical string + sizeof)      │
│  │   └─ dispatch: fn_on_produce_(slot, sz, fz, fz_sz)  │
│  │                                                     │
│  └─ Data loop calls engine->invoke_produce() etc.      │
│     → direct function pointer call, zero marshaling    │
│                                                        │
└────────────────────────────────────────────────────────┘
         │
         │ dlopen / LoadLibrary
         ▼
┌────────────────────────────────────────────────────────┐
│  Plugin shared library (plugin.so / plugin.dll)        │
│                                                        │
│  PLH_EXPORT bool plugin_init(const PlhPluginContext*)  │
│  PLH_EXPORT void plugin_finalize(void)                 │
│  PLH_EXPORT bool on_produce(void*, size_t, void*,      │
│                             size_t)                    │
│  PLH_EXPORT const PlhAbiInfo* plugin_abi_info(void)    │
│  PLH_DECLARE_SCHEMA(SlotFrame, "ts:float64:1:0|...",   │
│                     sizeof(MySlot))                    │
└────────────────────────────────────────────────────────┘
```

NativeEngine is a concrete subclass of `ScriptEngine` (HEP-CORE-0011). It participates
in the same lifecycle as PythonEngine and LuaEngine: `initialize()` -> `load_script()` ->
`build_api()` -> `invoke_on_init()` -> data loop -> `invoke_on_stop()` -> `finalize()`.

### 2.2 Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| C linkage for all plugin symbols | ABI stability across compilers (GCC, Clang, MSVC) |
| dlopen with RTLD_NOW \| RTLD_LOCAL | Eager symbol resolution; isolated namespace prevents symbol collision |
| Function pointers resolved once at load | Zero per-call overhead; null check gates optional callbacks |
| PlhPluginContext passed at init only | Plugin stores context; no per-callback context parameter overhead |
| Framework helpers via function pointers on PlhPluginContext | Plugin calls back into the framework through ctx->fn(ctx, ...) — no host symbol resolution or -rdynamic needed |
| Optional C++ convenience layer | Zero-cost wrappers (SlotRef, PLH_EXPORT_PRODUCE) for C++ plugin authors |

### 2.3 Relationship to Other Components

| Component | Relationship |
|-----------|-------------|
| ScriptEngine | NativeEngine implements the full ScriptEngine interface (HEP-CORE-0011) |
| PythonEngine / LuaEngine | Peer implementations. Role host selects engine based on `script.type` |
| RoleHostCore | Provides metrics counters, shutdown flags; passed as opaque `void* core` in PlhPluginContext |
| plugin_api.h | The plugin-side contract. Installed as a public header (`pylabhub/plugin_api.h`) |
| ScriptConfig | Parses `script.type`, `script.path`, `script.checksum` |

---

## 3. Plugin Lifecycle

### 3.1 Load Sequence

```
1. Role host reads config: script.type == "native"
2. NativeEngine created; set_expected_checksum() if config has checksum
3. engine->initialize(log_tag, core)           — stores log_tag, sets ctx_.core
4. engine->load_script(script_dir, entry_point, required_callback):
   a. Resolve library path (resolve_native_library search order)
   b. File integrity check (BLAKE2b-256 if checksum configured)
   c. dlopen(path, RTLD_NOW | RTLD_LOCAL)
   d. ABI check (plugin_abi_info → PlhAbiInfo validation)
   e. Resolve required symbols: plugin_init, plugin_finalize
   f. Resolve optional callback symbols: on_produce, on_consume, on_process,
      on_init, on_stop, on_inbox, on_heartbeat
   g. Resolve metadata symbols: plugin_name, plugin_version, plugin_is_thread_safe
   h. Verify required_callback is present (e.g., "on_produce" for producer)
5. engine->register_slot_type(spec, type_name, packing):
   a. Compute canonical schema string from config
   b. Resolve plugin_schema_<type_name> → compare canonical strings
   c. Resolve plugin_sizeof_<type_name> → store for type_sizeof queries
6. engine->build_api(ctx):
   a. Populate PluginContextStorage from RoleContext
   b. Wire string pointers into PlhPluginContext struct
   c. Call plugin_init(&ctx)
   d. Set accepting_ = true on success
7. engine->invoke_on_init()                    — call on_init() if exported
8. [data loop: invoke_produce / invoke_consume / invoke_process]
```

### 3.2 Shutdown Sequence

```
1. engine->stop_accepting()                    — blocks non-owner invoke()
2. engine->invoke_on_stop()                    — call on_stop() if exported
3. engine->finalize():
   a. Call plugin_finalize()
   b. Reset PluginContextStorage
   c. dlclose(handle)
   d. Clear all function pointers
```

### 3.3 Destructor Safety

If `finalize()` was not called (abnormal path), the destructor calls `plugin_finalize()`
and `dlclose()` as a safety net. This ensures resources are released even on exception
unwind.

---

## 4. Plugin API (plugin_api.h)

### 4.1 PlhPluginContext

```c
typedef struct PlhPluginContext
{
    /* ── Identity (read-only, valid until plugin_finalize) ──────────── */
    const char *role_tag;      /* "prod", "cons", or "proc" */
    const char *uid;           /* Role UID */
    const char *name;          /* Role name */
    const char *channel;       /* Primary channel (in_channel for processor) */
    const char *out_channel;   /* Output channel (processor only; NULL otherwise) */
    const char *log_level;     /* Configured log level string */
    const char *role_dir;      /* Role directory path */

    /* ── Framework API (function pointers filled by host) ──────────── */
    void (*log)(const struct PlhPluginContext *ctx, int level, const char *msg);
    void (*report_metric)(const struct PlhPluginContext *ctx,
                          const char *key, double value);
    void (*clear_custom_metrics)(const struct PlhPluginContext *ctx);
    void (*request_stop)(const struct PlhPluginContext *ctx);
    void (*set_critical_error)(const struct PlhPluginContext *ctx);
    int  (*is_critical_error)(const struct PlhPluginContext *ctx);
    const char *(*stop_reason)(const struct PlhPluginContext *ctx);
    uint64_t (*out_written)(const struct PlhPluginContext *ctx);
    uint64_t (*in_received)(const struct PlhPluginContext *ctx);
    uint64_t (*drops)(const struct PlhPluginContext *ctx);
    uint64_t (*script_errors)(const struct PlhPluginContext *ctx);
    uint64_t (*loop_overrun_count)(const struct PlhPluginContext *ctx);
    uint64_t (*last_cycle_work_us)(const struct PlhPluginContext *ctx);

    /* ── Opaque host data (do not dereference) ────────────────────── */
    void *_core;               /* Internal — RoleHostCore pointer for API implementations */
    const char *_log_label;    /* Internal — log prefix e.g. "[native libfoo.so]" */
} PlhPluginContext;
```

All strings are null-terminated UTF-8. String pointers are valid from `plugin_init()`
until `plugin_finalize()`. Framework services are accessed through the function pointers
on the context struct — the plugin passes `ctx` as the first argument to each call
(e.g., `ctx->log(ctx, PLH_LOG_INFO, "message")`). The `_core` and `_log_label` fields
are internal to the host and must not be accessed by plugin code.

### 4.2 Required Symbols

Every plugin must export these two symbols:

| Symbol | Signature | Description |
|--------|-----------|-------------|
| `plugin_init` | `bool plugin_init(const PlhPluginContext *ctx)` | Initialize the plugin. Return false to abort role startup. |
| `plugin_finalize` | `void plugin_finalize(void)` | Release all resources. No framework calls after this. |

### 4.3 ABI Compatibility: PlhAbiInfo

```c
typedef struct PlhAbiInfo
{
    uint32_t struct_size;       /* sizeof(PlhAbiInfo) — versioning guard */
    uint32_t sizeof_void_ptr;   /* sizeof(void*) — 4 or 8 */
    uint32_t sizeof_size_t;     /* sizeof(size_t) */
    uint32_t byte_order;        /* 1 = little-endian, 2 = big-endian */
    uint32_t api_version;       /* PLH_PLUGIN_API_VERSION */
} PlhAbiInfo;

#define PLH_PLUGIN_API_VERSION 1
```

The plugin exports `plugin_abi_info()` returning a pointer to a static `PlhAbiInfo`.
The framework validates at load time:

| Check | Failure Mode |
|-------|-------------|
| `struct_size >= sizeof(PlhAbiInfo)` | Plugin compiled with older header (missing fields) |
| `sizeof_void_ptr == sizeof(void*)` | 32/64-bit mismatch |
| `sizeof_size_t == sizeof(size_t)` | size_t width mismatch |
| `byte_order == host_byte_order` | Endianness mismatch (or 0 = don't care) |
| `api_version == PLH_PLUGIN_API_VERSION` | Breaking API change between plugin and host |

Any mismatch is a hard error: the plugin is rejected, dlclose is called, and role
startup aborts. If `plugin_abi_info` is not exported, the check is skipped with a
warning (permissive mode for legacy plugins).

### 4.4 Role Callback Symbols

Implement the callbacks your role needs. Unimplemented callbacks are resolved as NULL
and silently skipped.

| Symbol | Signature | Roles | Description |
|--------|-----------|-------|-------------|
| `on_init` | `void on_init(void)` | All | Called once after build_api, before data loop |
| `on_stop` | `void on_stop(void)` | All | Called once after data loop exits |
| `on_produce` | `bool on_produce(void *out, size_t out_sz, void *fz, size_t fz_sz)` | Producer | Write data to slot. true=commit, false=discard. |
| `on_consume` | `void on_consume(const void *in, size_t in_sz, const void *fz, size_t fz_sz)` | Consumer | Read data from slot. |
| `on_process` | `bool on_process(const void *in, size_t in_sz, void *out, size_t out_sz, void *fz, size_t fz_sz)` | Processor | Read input, write output. true=commit, false=discard. |
| `on_inbox` | `void on_inbox(const void *data, size_t sz, const char *sender_uid)` | All | Receive a typed inbox message (HEP-CORE-0027). |
| `on_heartbeat` | `void on_heartbeat(void)` | All | Called from control thread. Must be thread-safe. |

**Return value contract for on_produce / on_process:**
- `true` -> `InvokeResult::Commit` (slot is published)
- `false` -> `InvokeResult::Discard` (slot is dropped, loop continues)

If the function pointer is NULL (symbol not exported), the framework returns
`InvokeResult::Discard`.

### 4.5 Optional Metadata Symbols

| Symbol | Signature | Description |
|--------|-----------|-------------|
| `plugin_name` | `const char *plugin_name(void)` | Human-readable name for logging |
| `plugin_version` | `const char *plugin_version(void)` | Version string for logging |
| `plugin_is_thread_safe` | `bool plugin_is_thread_safe(void)` | Controls multi-state behavior (see section 9) |

### 4.6 Schema Validation Macros

```c
#define PLH_DECLARE_SCHEMA(Name, Schema, Size)
```

Generates two symbols:
- `plugin_schema_<Name>()` -> canonical schema string
- `plugin_sizeof_<Name>()` -> sizeof the plugin's compiled struct

**Name** must match one of the registered type names:

| Name | Role | Description |
|------|------|-------------|
| `SlotFrame` | Producer/Consumer | Main data slot |
| `InSlotFrame` | Processor | Input slot |
| `OutSlotFrame` | Processor | Output slot |
| `FlexFrame` | All (if configured) | Flexzone data |
| `InboxFrame` | All (if configured) | Inbox message payload |

**Schema** is a pipe-delimited canonical schema string with the format
`"field_name:type_str:count:length|..."`. Example:

```c
PLH_DECLARE_SCHEMA(SlotFrame,
    "timestamp:float64:1:0|temperature:float32:1:0|status:uint8:1:0",
    sizeof(MySlotStruct))
```

The framework computes the same canonical string from the JSON config's schema fields
and compares them at load time. Mismatch is a hard error.

### 4.7 Framework API (Function Pointers on PlhPluginContext)

Framework services are accessed through function pointers on the `PlhPluginContext`
struct. The host fills these pointers before calling `plugin_init()`. Every function
takes `const PlhPluginContext *ctx` as its first argument — the plugin simply passes
its stored context pointer through.

**Calling pattern:**

```c
ctx->log(ctx, PLH_LOG_INFO, "starting acquisition");
ctx->report_metric(ctx, "temperature", 23.5);
ctx->request_stop(ctx);
```

| Function Pointer | Signature | Description |
|------------------|-----------|-------------|
| `ctx->log` | `void (*)(ctx, int level, const char *msg)` | Log through pylabHub logger. level: PLH_LOG_DEBUG/INFO/WARN/ERROR. |
| `ctx->report_metric` | `void (*)(ctx, const char *key, double value)` | Report a custom key-value metric. |
| `ctx->clear_custom_metrics` | `void (*)(ctx)` | Clear all previously reported custom metrics. |
| `ctx->request_stop` | `void (*)(ctx)` | Request graceful role shutdown. |
| `ctx->set_critical_error` | `void (*)(ctx)` | Signal a critical error (sets flag + requests stop). |
| `ctx->is_critical_error` | `int (*)(ctx)` | Check if critical error has been flagged. Returns 1 or 0. |
| `ctx->stop_reason` | `const char *(*)(ctx)` | Get stop reason: "normal", "peer_dead", "hub_dead", "critical_error". |
| `ctx->out_written` | `uint64_t (*)(ctx)` | Slots written (producer/processor output). |
| `ctx->in_received` | `uint64_t (*)(ctx)` | Slots received (consumer/processor input). |
| `ctx->drops` | `uint64_t (*)(ctx)` | Dropped slot count. |
| `ctx->script_errors` | `uint64_t (*)(ctx)` | Script error count. |
| `ctx->loop_overrun_count` | `uint64_t (*)(ctx)` | Loop timing overrun count. |
| `ctx->last_cycle_work_us` | `uint64_t (*)(ctx)` | Work duration of last iteration in microseconds. |

All function pointers are null-safe on the host side. The plugin should check for NULL
before calling if it needs to be defensive, though the host always fills all pointers.

---

## 5. C++ Convenience Layer

The C++ convenience layer is available when `plugin_api.h` is included from a C++
translation unit. It provides zero-cost abstractions over the raw C API.

### 5.1 SlotRef\<T\> and ConstSlotRef\<T\>

```cpp
namespace plh {

template <typename T>
class SlotRef {
    // T must be standard-layout (static_assert enforced).
    SlotRef(void *ptr, size_t sz);         // valid if ptr != null && sz >= sizeof(T)
    explicit operator bool() const;         // check validity
    T *operator->();                         // typed access
    T &operator*();
    T *get();
};

template <typename T>
class ConstSlotRef {
    ConstSlotRef(const void *ptr, size_t sz);
    explicit operator bool() const;
    const T *operator->() const;
    const T &operator*() const;
    const T *get() const;
};

} // namespace plh
```

These wrap raw `void*` pointers with type-safe access and a validity check. The
`static_assert` on `std::is_standard_layout_v<T>` catches non-POD types at compile time.

### 5.2 Export Macros

| Macro | Callback Signature | Description |
|-------|-------------------|-------------|
| `PLH_EXPORT_PRODUCE(Slot, Flex, fn)` | `bool fn(SlotRef<Slot>, SlotRef<Flex>)` | Typed on_produce with flexzone |
| `PLH_EXPORT_PRODUCE_NOFZ(Slot, fn)` | `bool fn(SlotRef<Slot>)` | Typed on_produce without flexzone |
| `PLH_EXPORT_CONSUME(Slot, Flex, fn)` | `void fn(ConstSlotRef<Slot>, ConstSlotRef<Flex>)` | Typed on_consume with flexzone |
| `PLH_EXPORT_CONSUME_NOFZ(Slot, fn)` | `void fn(ConstSlotRef<Slot>)` | Typed on_consume without flexzone |
| `PLH_EXPORT_PROCESS(In, Out, Flex, fn)` | `bool fn(ConstSlotRef<In>, SlotRef<Out>, SlotRef<Flex>)` | Typed on_process with flexzone |
| `PLH_EXPORT_PROCESS_NOFZ(In, Out, fn)` | `bool fn(ConstSlotRef<In>, SlotRef<Out>)` | Typed on_process without flexzone |

These macros generate the C-linkage `on_produce`/`on_consume`/`on_process` symbols and
wrap the raw pointers in typed SlotRef/ConstSlotRef before calling the user's function.

### 5.3 Example: C++ Producer Plugin

```cpp
#include <pylabhub/plugin_api.h>

struct MySlot {
    double   timestamp;
    float    temperature;
    uint8_t  status;
};

PLH_DECLARE_SCHEMA(SlotFrame,
    "timestamp:float64:1:0|temperature:float32:1:0|status:uint8:1:0",
    sizeof(MySlot))

static const PlhPluginContext *g_ctx = nullptr;

PLH_EXPORT bool plugin_init(const PlhPluginContext *ctx) {
    g_ctx = ctx;
    ctx->log(ctx, PLH_LOG_INFO, "MyProducer initialized");
    return true;
}

PLH_EXPORT void plugin_finalize(void) {
    g_ctx = nullptr;
}

PLH_EXPORT const PlhAbiInfo *plugin_abi_info(void) {
    static PlhAbiInfo info = {
        sizeof(PlhAbiInfo), sizeof(void*), sizeof(size_t), 1,
        PLH_PLUGIN_API_VERSION
    };
    return &info;
}

static bool produce(plh::SlotRef<MySlot> slot) {
    if (!slot) return false;
    slot->timestamp = 0.0;      // fill from sensor API
    slot->temperature = 23.5f;
    slot->status = 1;
    return true;
}

PLH_EXPORT_PRODUCE_NOFZ(MySlot, produce)

PLH_EXPORT void on_init(void) {}
PLH_EXPORT void on_stop(void) {}
```

---

## 6. Three-Layer Verification

NativeEngine applies three independent verification layers before the plugin's
`plugin_init()` is called. All checks are performed during `load_script()`.

### 6.1 Layer 1: File Integrity (BLAKE2b-256)

When `script.checksum` is present in the JSON config, the framework:

1. Reads the entire shared library file into memory
2. Computes BLAKE2b-256 over the raw bytes (via `pylabhub::crypto::compute_blake2b_array`)
3. Converts to lowercase hex string (64 characters)
4. Compares against the configured checksum

Mismatch is a hard error: plugin is not loaded, role startup aborts.

If `script.checksum` is absent or empty, this check is skipped. This allows development
workflows where the plugin is recompiled frequently. Production deployments should always
set the checksum.

### 6.2 Layer 2: ABI Compatibility (PlhAbiInfo)

After dlopen succeeds, the framework resolves `plugin_abi_info` and validates the
returned `PlhAbiInfo` struct (see section 4.3). This catches:

- 32/64-bit mismatches (e.g., loading a 32-bit plugin in a 64-bit host)
- Endianness mismatches (cross-compiled plugins)
- API version skew (plugin compiled against an older/newer plugin_api.h)
- Struct layout drift (struct_size guard)

If `plugin_abi_info` is not exported, the check is skipped with a warning. This
provides backward compatibility for plugins compiled before ABI checks were added.

### 6.3 Layer 3: Schema Compatibility

During `register_slot_type()`, the framework validates that the plugin's compiled
struct matches the JSON config's schema:

1. **Canonical schema string**: The framework computes `"name:type:count:length|..."`
   from the config's `SchemaSpec` fields. If the plugin exports `plugin_schema_<Name>()`,
   the strings are compared. Mismatch is a hard error.

2. **sizeof validation**: If the plugin exports `plugin_sizeof_<Name>()`, the value
   is stored for `type_sizeof()` queries. This allows the framework to detect padding
   differences between the plugin's compiler and the host.

If the plugin does not export schema symbols for a given type name, the check is skipped
with a warning. The plugin will still receive raw pointers at the correct offsets, but
the framework cannot verify struct layout agreement.

---

## 7. Configuration

### 7.1 Script Section

```json
{
    "script": {
        "type": "native",
        "path": ".",
        "checksum": "a1b2c3d4...64-char-hex..."
    }
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `script.type` | yes | `"python"` | Must be `"native"` to select NativeEngine |
| `script.path` | no | `"."` | Base directory for library resolution |
| `script.checksum` | no | — | BLAKE2b-256 hex of the .so file. Empty = skip check. |

Validation: `script.type` must be one of `"python"`, `"lua"`, or `"native"`. Any other
value throws `std::invalid_argument` at config parse time.

### 7.2 Library Path Resolution

`resolve_native_library(configured_path, filename)` searches in order:

1. **Absolute path**: If `filename` is absolute and exists, use it directly
2. **Structured path**: `<script.path>/script/native/<filename>`
3. **Structured + extension**: `<script.path>/script/native/<filename><ext>` (if no extension given)
4. **Direct relative**: `<script.path>/<filename>`
5. **Direct + extension**: `<script.path>/<filename><ext>` (if no extension given)

Platform extension is `.so` (Linux), `.dylib` (macOS), or `.dll` (Windows).

### 7.3 Engine Selection in Role Hosts

All four binaries (producer, consumer, processor, hubshell) check `script.type` and
instantiate the appropriate engine:

```cpp
if (script_type == "native")
{
    auto ne = std::make_unique<pylabhub::scripting::NativeEngine>();
    if (!config.script().checksum.empty())
        ne->set_expected_checksum(config.script().checksum);
    engine = std::move(ne);
}
```

The `set_expected_checksum()` call must happen before `load_script()`, as the checksum
is verified during the load sequence.

---

## 8. Directory Convention

Native plugins follow the same directory structure as Python and Lua scripts:

```
<role_dir>/
├── config.json
└── script/
    ├── python/
    │   └── __init__.py
    ├── lua/
    │   └── init.lua
    └── native/
        └── libmy_plugin.so      ← native plugin
```

The `script/native/` directory is the canonical location. The library filename is
passed as the entry_point argument to `load_script()`. The resolution logic
(section 7.2) also supports placing the .so directly in `script.path` or using
an absolute path for out-of-tree builds.

---

## 9. Threading Model

### 9.1 Default: Single-Threaded

By default, `supports_multi_state()` returns false. All callbacks are invoked from the
owner thread (the data loop thread). The threading contract matches Python and Lua engines:

- `on_init()`, `on_stop()`, `on_produce()`, `on_consume()`, `on_process()`, `on_inbox()`
  are called from the data loop thread
- `on_heartbeat()` is called from the control thread (must be independently thread-safe)
- Generic `invoke()` from non-owner threads returns false

### 9.2 Thread-Safe Mode

If the plugin exports `plugin_is_thread_safe()` returning true, `supports_multi_state()`
returns true. This signals to the framework that:

- The plugin's callbacks are fully reentrant
- Generic `invoke()` calls from any thread are permitted
- The plugin is responsible for its own internal synchronization

This is the opposite of the scripted engines: Python queues cross-thread requests to
the GIL-holding owner thread; Lua creates per-thread lua_State copies. A thread-safe
native plugin needs neither mechanism.

### 9.3 release_thread()

`release_thread()` is a no-op for NativeEngine. Native plugins manage their own
thread-local resources (if any). The `ThreadEngineGuard` RAII helper still works
correctly — its destructor calls `release_thread()` which simply returns.

---

## 10. Generic Invoke and Eval

### 10.1 invoke(name)

The generic `invoke(name)` dispatches in two stages:

1. Check named function pointers: `"on_heartbeat"` -> `fn_on_heartbeat_`
2. Fall back to `dlsym(handle, name)` for arbitrary exported `void(*)(void)` symbols

This allows plugins to export custom command handlers accessible via the broker's
`INVOKE_REQ` protocol.

### 10.2 invoke(name, args)

JSON arguments are not supported for native plugins. The call degrades to `invoke(name)`
(args are ignored). Structured data exchange with native plugins should use the inbox
mechanism (HEP-CORE-0027) or custom framework helpers.

### 10.3 eval(code)

`eval()` always returns `{InvokeStatus::NotFound, {}}`. Code evaluation is not
applicable to compiled plugins.

---

## 11. Error Handling

### 11.1 Load-Time Errors

| Error | Severity | Behavior |
|-------|----------|----------|
| Plugin file not found | Fatal | Role startup aborts |
| Checksum mismatch | Fatal | Plugin not loaded; role startup aborts |
| dlopen failure | Fatal | Error logged with dlerror(); role startup aborts |
| ABI check failure | Fatal | dlclose called; role startup aborts |
| Missing plugin_init or plugin_finalize | Fatal | dlclose called; role startup aborts |
| Missing required callback | Fatal | dlclose called; role startup aborts |
| Schema string mismatch | Fatal | register_slot_type returns false; role startup aborts |
| plugin_init returns false | Fatal | build_api returns false; role startup aborts |

### 11.2 Runtime Errors

Native plugin callbacks cannot "raise exceptions" in the script engine sense. If a
callback crashes (segfault, abort), the entire process dies. The framework provides
`ctx->set_critical_error(ctx)` for the plugin to signal recoverable errors that should
trigger graceful shutdown.

`script_error_count()` delegates to `RoleHostCore::script_errors()`, which the plugin
can increment indirectly via `ctx->set_critical_error(ctx)`.

---

## 12. Source File Reference

| Component | File | Description |
|-----------|------|-------------|
| Plugin C API | `src/include/utils/plugin_api.h` | C-linkage contract, PlhPluginContext, PlhAbiInfo, macros |
| C++ convenience | `src/include/utils/plugin_api.h` | SlotRef, ConstSlotRef, PLH_EXPORT_* macros (C++ section) |
| NativeEngine header | `src/scripting/native_engine.hpp` | ScriptEngine subclass declaration |
| NativeEngine impl | `src/scripting/native_engine.cpp` | dlopen, symbol resolution, verification, dispatch |
| Framework helpers | `src/scripting/native_engine.cpp` | Implementations backing ctx->log, ctx->report_metric, ctx->request_stop, etc. |
| ScriptConfig | `src/include/utils/config/script_config.hpp` | type/path/checksum parsing, resolve_native_library |
| ScriptEngine base | `src/include/utils/script_engine.hpp` | Abstract interface, RoleContext, InvokeResult |
| Crypto utils | `src/include/utils/crypto_utils.hpp` | compute_blake2b_array for file checksum |
| Producer main | `src/producer/producer_main.cpp` | Engine selection and wiring |
| Consumer main | `src/consumer/consumer_main.cpp` | Engine selection and wiring |
| Processor main | `src/processor/processor_main.cpp` | Engine selection and wiring |

---

## 13. Cross-References

- **HEP-CORE-0011**: ScriptEngine abstraction framework (NativeEngine implements this interface)
- **HEP-CORE-0008 SS6**: Iteration metrics (NativeEngine participates in the same metrics pipeline)
- **HEP-CORE-0018 SS15**: Producer/Consumer binary architecture (engine selection logic)
- **HEP-CORE-0015 SS4**: Processor binary (engine selection, dual-queue dispatch)
- **HEP-CORE-0027**: Inbox messaging (on_inbox callback, InboxFrame schema validation)
- **HEP-CORE-0016**: Named Schema Registry (schema field definitions used in PLH_DECLARE_SCHEMA)
- **HEP-CORE-0019 SS5**: Metrics plane (ctx->report_metric integration)
