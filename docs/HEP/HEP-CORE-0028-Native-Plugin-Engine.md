# HEP-CORE-0028: Native Engine

**Status**: Implemented (documenting existing system)
**Created**: 2026-03-30
**Updated**: 2026-06-11 (#194 Phase C — Two-Layer Plugin Authoring Model: C ABI v6 with int+macro returns, visitor pattern, opaque metrics snapshot; expanded C++ wrapper with typed enums, concepts, handle types, std::span view; lifetime + security contract.  Supersedes v5 API.)
**Scope**: NativeEngine, native_engine_api.h, C/C++ native engine lifecycle, ABI verification, schema validation

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
  abstraction layer. A native engine can call vendor APIs directly.

The Native Plugin Engine eliminates these costs by loading compiled shared libraries
(.so/.dll/.dylib) and dispatching callbacks as direct function pointer calls. The native
engine receives raw slot pointers — the same memory the framework uses internally — with
zero copy, zero conversion, and deterministic call overhead (one indirect call per callback).

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
│  │   ├─ resolve symbols: native_init, on_produce, ...  │
│  │   ├─ verify ABI (PlhAbiInfo)                        │
│  │   ├─ verify file checksum (BLAKE2b-256)             │
│  │   ├─ verify schema (canonical string + sizeof)      │
│  │   └─ dispatch: fn_on_produce_(&tx)                   │
│  │                                                     │
│  └─ Data loop calls engine->invoke_produce() etc.      │
│     → direct function pointer call, zero marshaling    │
│                                                        │
└────────────────────────────────────────────────────────┘
         │
         │ dlopen / LoadLibrary
         ▼
┌────────────────────────────────────────────────────────┐
│  Native engine shared library (plugin.so / plugin.dll) │
│                                                        │
│  PLH_EXPORT bool native_init(const PlhNativeContext*)  │
│  PLH_EXPORT void native_finalize(void)                 │
│  PLH_EXPORT bool on_produce(const plh_tx_t *tx)        │
│  PLH_EXPORT const PlhAbiInfo* native_abi_info(void)    │
│  PLH_DECLARE_SCHEMA(SlotFrame, "ts:float64:1:0|...",   │
│                     sizeof(MySlot))                    │
└────────────────────────────────────────────────────────┘
```

NativeEngine is a concrete subclass of `ScriptEngine` (HEP-CORE-0011). It participates in
the same lifecycle as PythonEngine and LuaEngine: `initialize()` -> `load_script()` ->
`build_api()` -> `invoke_on_init()` -> data loop -> `invoke_on_stop()` -> `finalize()`.

### 2.2 Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| C linkage for all exported symbols | ABI stability across compilers (GCC, Clang, MSVC) |
| dlopen with RTLD_NOW \| RTLD_LOCAL | Eager symbol resolution; isolated namespace prevents symbol collision |
| Function pointers resolved once at load | Zero per-call overhead; null check gates optional callbacks |
| PlhNativeContext passed at init only | Native engine stores context; no per-callback context parameter overhead |
| Framework helpers via function pointers on PlhNativeContext | Native engine calls back into the framework through ctx->fn(ctx, ...) — no host symbol resolution or -rdynamic needed |
| Optional C++ convenience layer | Zero-cost wrappers (SlotRef, PLH_EXPORT_PRODUCE) for C++ native engine authors |

### 2.3 Relationship to Other Components

| Component | Relationship |
|-----------|-------------|
| ScriptEngine | NativeEngine implements the full ScriptEngine interface (HEP-CORE-0011) |
| PythonEngine / LuaEngine | Peer implementations. Role host selects engine based on `script.type` |
| RoleHostCore | Provides metrics counters, shutdown flags; passed as opaque `void* core` in PlhNativeContext |
| native_engine_api.h | The native engine contract.  Installed as a public header at `${CMAKE_INSTALL_PREFIX}/include/utils/native_engine_api.h` (plus the visitor + arg-struct typedefs in `utils/native_invoke_types.h` reached transitively).  Plugin authors include via `#include "utils/native_engine_api.h"` or `#include <utils/native_engine_api.h>`. |
| ScriptConfig | Parses `script.type`, `script.path`, `script.checksum` |

### 2.4 Two-Layer Plugin Authoring Model (#194 Phase C, 2026-06-11)

The native plugin surface is **two layers**, matching the Two-Tier API Model
established in HEP-CORE-0032 §2:

| Layer | Header section | Audience | Properties |
|---|---|---|---|
| **Tier 1 — C ABI (the floor)** | `extern "C"` portion of `native_engine_api.h` | Pure-C plugins, FFI binding targets, cross-toolchain consumers | Stable across compilers + stdlib versions; primitive returns + opaque pointers + visitor callbacks; **no malloc across the boundary**; **no exceptions across the boundary**; explicit lifetime contracts on every borrowed pointer |
| **Tier 2 — C++ wrapper (the natural choice)** | `namespace plh { ... }` portion of `native_engine_api.h` | C++ plugins (the common case) | Header-only; zero binary cost; typed `enum class`; C++20 `requires`-constrained visitor templates; lightweight handle types; `std::span` views; `std::optional` returns; RAII where ownership applies |

**Tier 1 design principles** (the C ABI is the *contract* — Tier 2 builds on it):

1. **Return codes, not exceptions.** Per IMPLEMENTATION_GUIDANCE.md §"Error
   reporting: C vs C++", the C ABI surfaces errors as primitive return values
   (e.g. `1` = success, `0` = failure, `-1` = error) or out-parameters.
   Throwing across the C ABI boundary is undefined behaviour; **every host
   wrapper must catch internally and translate to a return code.**
2. **`int` + macro instead of `const char *` for enumerated values.**  Bytes
   that name "one of N states" travel as a primitive (`int`) accompanied by
   `PLH_*_*` preprocessor macros (`PLH_MECHANISM_CURVE = 1`,
   `PLH_QUEUE_POLICY_SHM = 1`, etc.).  The plugin gets compile-time symbolic
   names without owning a string, without paying for an allocation, and
   without a lifetime contract on the returned bytes.  Macros — not enum
   names — because the C ABI is consumed from pure C, where `enum` may
   widen to `int` differently per compiler.
3. **Visitor pattern for collections.**  When the plugin needs to iterate a
   list (authorized peers, band members), the C ABI takes a callback +
   `void *userdata` pair and invokes the callback once per element with a
   typed-struct pointer.  **No `malloc()` crosses the boundary** — the host
   constructs an on-stack struct, the plugin reads it during the callback,
   the struct dies at callback return.  No caller-must-free contracts.
4. **Inquiry helpers alongside visitors.**  "Is X in the set?" and "how many
   are there?" are first-class API operations (`*_contains`, `*_count`).
   Plugin authors should not be required to hand-write a visitor callback +
   bool flag + early-exit mechanism just to ask a yes/no question.  This
   gives C plugins an ergonomic surface comparable to Lua/Python's dict
   membership.
5. **Opaque snapshot + lookup for batch reads.**  Metrics (a tree of dotted
   keys → doubles) follow a two-call shape: `metrics_snapshot()` returns
   an opaque `const void *` view of a host-owned thread-local cache;
   `metrics_get(snap, key, double *)` resolves keys against it.  Plugin
   never sees the encoding; host avoids re-building the cache on every
   query; lifetime is bounded by "the next `metrics_snapshot()` call on
   the same thread."
6. **Lifetime contract on every borrowed pointer.**  Every typed-struct
   pointer handed to a plugin (visitor argument, callback argument,
   snapshot return) carries a documented "valid for the duration of this
   call" or "valid until next snapshot on this thread" contract.  Plugins
   that stash the pointer past its lifetime have introduced their own UB —
   the framework does not paper over the violation.

**Tier 2 design principles** (the C++ wrapper *promotes* the C ABI without
sacrificing it):

1. **Header-only, zero binary cost.**  Every wrapper method is `inline` (in
   the class body or via a `template`).  The plugin includes one header
   and links nothing extra.  Building the same source against the v6 ABI
   from a different toolchain still works; the C++ wrapper only adds
   compile-time machinery.
2. **Typed `enum class` for enumerated values.**  `plh::Mechanism`,
   `plh::QueuePolicy`, `plh::StopReason` derive from the C macros with
   `: int` underlying type — `static_cast`-free interop with the C layer,
   compiler-checked switch coverage at the C++ layer.  Each enum carries
   a `constexpr std::string_view to_string(...)` for zero-alloc logging.
3. **C++20 `requires`-constrained visitor templates.**  Concepts like
   `plh::AllowedPeerVisitor`, `plh::BandMemberVisitor` constrain the
   template to callables of the right shape.  Misuse → clean compile
   error at the concept boundary instead of a 200-line template
   diagnostic inside the thunk.
4. **Lightweight handle types — zero-cost typed views.**  `ctx.allowed_peers
   ("out")` returns an `AllowedPeersHandle` (two pointers) on which the
   plugin calls `.visit(...)`, `.contains(...)`, `.count()`,
   `.collect_uids()`, `.to_uid_set()`.  The handle is
   `trivially_copyable` and passes in registers; `static_assert`s in the
   header enforce this contract.
5. **`std::span` for in-callback array views.**  Event-handler arg structs
   like `plh_allowlist_changed_args_t` are wrapped by
   `plh::AllowlistChangedView` exposing `.peers() → std::span<const
   plh_allowed_peer_t>`.  Plugin code becomes idiomatic range-for
   instead of `for (size_t i = 0; i < args->peer_count; ++i)`.
6. **Performance via *better patterns*, not lower-level access.**  C++
   plugins can call `to_uid_set()` once and do O(1) repeated membership
   queries in O(M) total — the C plugin doing the same via visitors-only
   pays O(N·M) without manual hash-set boilerplate.  The wrapper exposes
   the *idiomatic* C++ shape that happens to be faster, so writing the
   plugin in C++ is the path of least resistance and also the path of
   highest performance.
7. **No RAII for things that aren't resources.**  Wrapper objects hold
   *borrowed* pointers (handle → host state; snapshot → thread-local
   cache).  Their destructors are trivial.  Genuine RAII is reserved for
   actual scope-owned acquire/release pairs — currently only
   `plh::SpinLockGuard` qualifies.  Speculative auto-cleanup
   (`ScopedBandJoin` was considered and rejected) was avoided because
   "join and stay" — not transient participation — is the typical band
   pattern.

**What plugin authors get from each layer:**

| Need | Tier 1 (C) | Tier 2 (C++ wrapper) |
|---|---|---|
| Get queue mechanism | `int m = ctx->queue_mechanism(ctx, PLH_SIDE_TX);` + macro switch | `auto m = ctx.queue_mechanism(SideTx); switch (m) { ... }` (compiler verifies exhaustiveness) |
| Check authorized peer | `int v = ctx->allowed_peer_contains(ctx, "out", "consumer_A");` | `if (ctx.allowed_peers("out").contains("consumer_A"))` |
| Iterate band members | visitor + static fn + manual flag | range-for over `BandHandle::collect_uids()` or template lambda over `.visit_members(...)` |
| Read 10 metrics | 1× `metrics_snapshot` + 10× `metrics_get` | `auto snap = ctx.metrics_snapshot(); if (auto v = snap["queue.data_drop_count"]) { ... }` |
| Handle on_allowlist_changed | iterate `args->peers[i]` by index | `for (auto &p : plh::AllowlistChangedView{args}.peers())` |
| Acquire spinlock | `ctx->spinlock_lock(ctx, 0, side, -1);` + manual unlock on every exit path | `plh::SpinLockGuard g(ctx, 0, side);` |

The C++ wrapper does not replace the C ABI; **the C ABI is the contract that
the wrapper compiles down to.**  Every Tier 2 entry point delegates directly
to a Tier 1 function pointer with no extra runtime work.  When a plugin
author prefers raw C — for FFI generation, for cross-toolchain stability,
for a learning project — Tier 1 alone is sufficient and complete.

---

## 3. Native Engine Lifecycle

### 3.1 Load Sequence

```
1. Role host reads config: script.type == "native"
2. NativeEngine created; set_expected_checksum() if config has checksum
3. engine->initialize(log_tag, core)           — stores log_tag, sets ctx_.core
4. engine->load_script(script_dir, entry_point, required_callback):
   a. Resolve library path (resolve_native_library search order)
   b. File integrity check (BLAKE2b-256 if checksum configured)
   c. dlopen(path, RTLD_NOW | RTLD_LOCAL)
   d. ABI check (native_abi_info → PlhAbiInfo validation)
   e. Resolve required symbols: native_init, native_finalize
   f. Resolve optional callback symbols: on_produce, on_consume, on_process,
      on_init, on_stop, on_inbox, on_heartbeat
   g. Resolve metadata symbols: native_name, native_version, native_is_thread_safe
   h. Verify required_callback is present (e.g., "on_produce" for producer)
5. engine->register_slot_type(spec, type_name, packing):
   a0. Reject non-canonical type_name — must be one of the 5 canonical names
       (InSlotFrame, OutSlotFrame, InFlexFrame, OutFlexFrame, InboxFrame).
       See HEP-CORE-0011 §Canonical type names (closed set).
   a. Compute expected size from schema via compute_field_layout(spec, packing)
   b. Compute canonical schema string from config
   c. Resolve native_schema_<type_name> → compare canonical strings (hard error on mismatch)
   d. Resolve native_sizeof_<type_name> → **required** (hard error if missing)
   e. Compare native sizeof against schema-computed size (hard error on mismatch)
6. engine->build_api(ctx):
   a. Populate NativeContextStorage from RoleContext
   b. Wire string pointers into PlhNativeContext struct
   c. Call native_init(&ctx)
   d. Set accepting_ = true on success
7. engine->invoke_on_init()                    — call on_init() if exported
8. [data loop: invoke_produce / invoke_consume / invoke_process]
```

### 3.2 Shutdown Sequence

```
1. engine->stop_accepting()                    — blocks non-owner invoke()
2. engine->invoke_on_stop()                    — call on_stop() if exported
3. engine->finalize():
   a. Call native_finalize()
   b. Reset NativeContextStorage
   c. dlclose(handle)
   d. Clear all function pointers
```

### 3.3 Destructor Safety

If `finalize()` was not called (abnormal path), the destructor calls `native_finalize()`
and `dlclose()` as a safety net. This ensures resources are released even on exception
unwind.

---

## 4. Native Engine API (native_engine_api.h)

### 4.1 PlhNativeContext (API v7, #84, 2026-06-11; v6 baseline #194 Phase C, 2026-06-11)

> v7 adds 16 hub-side fn ptrs at the END of the struct (additive — see
> §4.9 "Hub-side API Surface" for the full matrix).  v6 surface below
> remains the role-side baseline.

```c
typedef struct PlhNativeContext
{
    /* ── Identity (read-only, valid until native_finalize) ──────────── */
    const char *role_tag;      /* "prod", "cons", or "proc" */
    const char *uid;           /* Role UID */
    const char *name;          /* Role name */
    const char *channel;       /* Primary channel (in_channel for processor) */
    const char *out_channel;   /* Output channel (processor only; NULL otherwise) */
    const char *log_level;     /* Configured log level string */
    const char *role_dir;      /* Role directory path */

    /* ── Framework API (function pointers filled by host) ───────────────
     * Every fn ptr in this table follows the v6 C ABI contract:
     *   - returns a primitive int / uint64_t / opaque pointer
     *   - NEVER throws a C++ exception across the boundary
     *   - NEVER allocates memory the plugin must free
     *   - typed-struct pointers handed to visitors are valid only for the
     *     duration of the visitor call (see §4.8 Lifetime + Security
     *     Contract).
     * The C++ wrapper in §5 builds on this contract — typed enums,
     * concept-constrained visitor templates, std::span views, std::optional
     * returns. */

    void (*log)(const struct PlhNativeContext *ctx, int level, const char *msg);
    void (*report_metric)(const struct PlhNativeContext *ctx,
                          const char *key, double value);
    void (*clear_custom_metrics)(const struct PlhNativeContext *ctx);
    void (*request_stop)(const struct PlhNativeContext *ctx);
    void (*set_critical_error)(const struct PlhNativeContext *ctx, const char *msg);
    int  (*is_critical_error)(const struct PlhNativeContext *ctx);

    /* stop_reason: returns one of PLH_STOP_REASON_* macros (NORMAL=0,
     * PEER_DEAD=1, HUB_DEAD=2, CRITICAL_ERROR=3, CHANNEL_CLOSED=4,
     * SCRIPT_ERROR=5).  Replaces v5 const char * return — see §4.7 +
     * §5.3. */
    int      (*stop_reason)(const struct PlhNativeContext *ctx);

    uint64_t (*out_slots_written)(const struct PlhNativeContext *ctx);
    uint64_t (*in_slots_received)(const struct PlhNativeContext *ctx);
    uint64_t (*out_drop_count)(const struct PlhNativeContext *ctx);
    uint64_t (*script_error_count)(const struct PlhNativeContext *ctx);
    uint64_t (*loop_overrun_count)(const struct PlhNativeContext *ctx);
    uint64_t (*last_cycle_work_us)(const struct PlhNativeContext *ctx);

    /* Spinlocks (HEP-CORE-0002 §2.2).  side: PLH_SIDE_TX, PLH_SIDE_RX,
     * or PLH_SIDE_AUTO. */
    int      (*spinlock_lock)(const struct PlhNativeContext *ctx,
                              int index, int side, int timeout_ms);
    void     (*spinlock_unlock)(const struct PlhNativeContext *ctx,
                                int index, int side);
    uint32_t (*spinlock_count)(const struct PlhNativeContext *ctx, int side);
    int      (*spinlock_is_locked)(const struct PlhNativeContext *ctx,
                                   int index, int side);

    /* Schema sizes (HEP-CORE-0007). */
    size_t   (*slot_logical_size)(const struct PlhNativeContext *ctx, int side);
    size_t   (*flexzone_logical_size)(const struct PlhNativeContext *ctx, int side);

    /* Role discovery (HEP-CORE-0023). */
    int      (*wait_for_role)(const struct PlhNativeContext *ctx,
                              const char *uid, int timeout_ms);

    /* ── Band pub/sub (HEP-CORE-0030) — v6 visitor + inquiry pattern ── */
    /* band_join: returns 1 on success, 0 on rejection, -1 on error.
     * (v5 returned char *malloc'd JSON; v6 drops list-from-join behaviour
     * — plugin calls band_members() separately to enumerate after join.) */
    int      (*band_join)(const struct PlhNativeContext *ctx,
                          const char *channel);
    int      (*band_leave)(const struct PlhNativeContext *ctx,
                           const char *channel);
    void     (*band_broadcast)(const struct PlhNativeContext *ctx,
                               const char *channel,
                               const char *body_json);

    /* Iterate band members.  Visitor MUST be noexcept; host catches and
     * returns -1 on caught exception (defense in depth).  Returns count
     * visited (>=0), -1 on error.  See plh_band_member_t in
     * native_invoke_types.h. */
    int      (*band_members)(const struct PlhNativeContext *ctx,
                             const char *channel,
                             plh_band_member_visitor visitor,
                             void *userdata);
    /* Inquiry helpers — same semantics as the visitor + manual flag, but
     * one C call.  contains: 1=present, 0=absent, -1=error.  count:
     * count (>=0) or -1=error. */
    int      (*band_member_contains)(const struct PlhNativeContext *ctx,
                                     const char *channel,
                                     const char *role_uid);
    int      (*band_member_count)(const struct PlhNativeContext *ctx,
                                  const char *channel);

    /* ── Channel-auth observability (HEP-CORE-0036 §I11 + §6.7) ──────── */
    /* allowed_peers: visit + contains + count triplet (same shape as
     * band_members above; symmetric so plugin authors learn one pattern). */
    int      (*allowed_peers)(const struct PlhNativeContext *ctx,
                              const char *channel,
                              plh_allowed_peer_visitor visitor,
                              void *userdata);
    int      (*allowed_peer_contains)(const struct PlhNativeContext *ctx,
                                      const char *channel,
                                      const char *role_uid);
    int      (*allowed_peer_count)(const struct PlhNativeContext *ctx,
                                   const char *channel);
    /* is_channel_ready: 1 iff the queue serving `channel` is in HEP-0036
     * §6.7 Active state.  Engine-parity with Lua/Python.  0 = !Active,
     * -1 = error. */
    int      (*is_channel_ready)(const struct PlhNativeContext *ctx,
                                 const char *channel);
    /* queue_mechanism: returns one of PLH_MECHANISM_* macros
     * (UNINITIALIZED=0, CURVE=1) for the named side.  Replaces v5
     * const char * return.  See HEP-CORE-0035 §2 for the
     * CURVE-unconditional invariant. */
    int      (*queue_mechanism)(const struct PlhNativeContext *ctx, int side);

    /* ── Queue diagnostic surface (HEP-CORE-0019 metrics + HEP-0007) ── */
    uint64_t (*out_capacity)(const struct PlhNativeContext *ctx);
    uint64_t (*in_capacity)(const struct PlhNativeContext *ctx);
    /* Queue overflow policy: returns one of PLH_QUEUE_POLICY_* macros
     * (UNKNOWN=0, SHM=1, ZMQ_DROP=2, ZMQ_BLOCK=3, ZMQ_RING=4).  Replaces
     * v5 char *malloc return. */
    int      (*out_policy)(const struct PlhNativeContext *ctx);
    int      (*in_policy)(const struct PlhNativeContext *ctx);
    /* Last decoded wire seq on rx side.  0 until first read. */
    uint64_t (*last_seq)(const struct PlhNativeContext *ctx);

    /* ── Metrics snapshot (v6 opaque-snapshot + key lookup) ─────────── */
    /* metrics_snapshot: builds a thread-local host-owned cache of the
     * current metrics tree (HEP-CORE-0019) and returns an opaque view
     * pointer.  Lifetime: valid until the next metrics_snapshot() call
     * on the SAME thread.  Plugin MUST NOT share across threads.  NULL
     * on internal error. */
    const void *(*metrics_snapshot)(const struct PlhNativeContext *ctx);
    /* metrics_get: look up a dotted-path metric key against a snapshot
     * pointer.  Returns 1 if found (writes value to *out), 0 if missing,
     * -1 on error (null snap / null out / null key). */
    int      (*metrics_get)(const void *snapshot,
                            const char *key,
                            double *out);

    /* ── Flexzone control + band-membership query ───────────────────── */
    int      (*is_in_band)(const struct PlhNativeContext *ctx,
                           const char *channel);
    int      (*update_flexzone_checksum)(const struct PlhNativeContext *ctx);
    void     (*set_verify_checksum)(const struct PlhNativeContext *ctx,
                                    int enable);

    /* ── Hub-side API surface (API v7, #84) — see §4.9 ──────────────
     * On hub-side contexts these delegate to HubAPI; on role-side
     * contexts they are noop stubs (empty `""` for JSON; -1 / 0 /
     * noop for the rest).  JSON returns share a thread-local
     * scratch buffer — valid until next hub_*_json on same thread. */
    const char *(*hub_metrics_json)(const struct PlhNativeContext *ctx);
    const char *(*hub_config_json)(const struct PlhNativeContext *ctx);
    const char *(*hub_query_metrics_json)(const struct PlhNativeContext *ctx,
                                           const char *categories_json);
    const char *(*hub_list_channels_json)(const struct PlhNativeContext *ctx);
    const char *(*hub_get_channel_json)(const struct PlhNativeContext *ctx,
                                         const char *name);
    const char *(*hub_list_roles_json)(const struct PlhNativeContext *ctx);
    const char *(*hub_get_role_json)(const struct PlhNativeContext *ctx,
                                      const char *uid);
    const char *(*hub_list_bands_json)(const struct PlhNativeContext *ctx);
    const char *(*hub_get_band_json)(const struct PlhNativeContext *ctx,
                                      const char *name);
    const char *(*hub_list_peers_json)(const struct PlhNativeContext *ctx);
    const char *(*hub_get_peer_json)(const struct PlhNativeContext *ctx,
                                      const char *hub_uid);
    int      (*hub_close_channel)(const struct PlhNativeContext *ctx,
                                   const char *name);
    int      (*hub_broadcast_channel)(const struct PlhNativeContext *ctx,
                                       const char *channel,
                                       const char *message,
                                       const char *data_json);
    int      (*hub_post_event)(const struct PlhNativeContext *ctx,
                                const char *name,
                                const char *data_json);
    int64_t  (*hub_augment_timeout_ms)(const struct PlhNativeContext *ctx);
    void     (*hub_set_augment_timeout)(const struct PlhNativeContext *ctx,
                                         int64_t ms);

    /* ── Opaque host data (do not dereference) ────────────────────── */
    void *_core;               /* Internal — RoleHostCore pointer for API implementations */
    void *_api;                /* Internal — RoleAPIBase pointer (band/auth accessors) */
    const char *_log_label;    /* Internal — log prefix e.g. "[native libfoo.so]" */

    uint32_t _magic_end;       /* Trailing sentinel; both magics validated */
} PlhNativeContext;
```

All strings are null-terminated UTF-8. String pointers in identity fields
are valid from `native_init()` until `native_finalize()`. Framework services
are accessed through the function pointers on the context struct — the
native engine passes `ctx` as the first argument to each call (e.g.,
`ctx->log(ctx, PLH_LOG_INFO, "message")`). The `_core`, `_api`, and
`_log_label` fields are internal to the host and must not be accessed
by native engine code.

**Enumeration macros (v6).** All "one-of-N" return values are surfaced as
preprocessor macros, not enums — the C ABI is consumed from pure C where
`enum` width is implementation-defined.  The C++ wrapper in §5 derives
`enum class : int` types from these macros for type-safe C++ use.

```c
#define PLH_MECHANISM_UNINITIALIZED   0
#define PLH_MECHANISM_CURVE           1

#define PLH_QUEUE_POLICY_UNKNOWN      0
#define PLH_QUEUE_POLICY_SHM          1
#define PLH_QUEUE_POLICY_ZMQ_DROP     2
#define PLH_QUEUE_POLICY_ZMQ_BLOCK    3
#define PLH_QUEUE_POLICY_ZMQ_RING     4

#define PLH_STOP_REASON_NORMAL          0
#define PLH_STOP_REASON_PEER_DEAD       1
#define PLH_STOP_REASON_HUB_DEAD        2
#define PLH_STOP_REASON_CRITICAL_ERROR  3
#define PLH_STOP_REASON_CHANNEL_CLOSED  4
#define PLH_STOP_REASON_SCRIPT_ERROR    5
```

### 4.2 Required Symbols

Every native engine must export these two symbols:

| Symbol | Signature | Description |
|--------|-----------|-------------|
| `native_init` | `bool native_init(const PlhNativeContext *ctx)` | Initialize the native engine. Return false to abort role startup. |
| `native_finalize` | `void native_finalize(void)` | Release all resources. No framework calls after this. |

### 4.3 ABI Compatibility: PlhAbiInfo

```c
typedef struct PlhAbiInfo
{
    uint32_t struct_size;       /* sizeof(PlhAbiInfo) — versioning guard */
    uint32_t sizeof_void_ptr;   /* sizeof(void*) — 4 or 8 */
    uint32_t sizeof_size_t;     /* sizeof(size_t) */
    uint32_t byte_order;        /* 1 = little-endian, 2 = big-endian */
    uint32_t api_version;       /* PLH_NATIVE_API_VERSION */
} PlhAbiInfo;

#define PLH_NATIVE_API_VERSION 7
```

**API version log:**
- v1 → v2 (2026-04): initial ABI baseline.
- v2 → v3 (audit S2, 2026-05-18): `set_critical_error` gained a `const
  char *msg` parameter (uniform with Python/Lua `api.set_critical_error(msg)`).
- v3 → v4 (#194 Phase A, 2026-06-10): `PlhNativeContext` gains
  `allowed_peers` + `is_channel_ready` + `queue_mechanism` function
  pointers (HEP-CORE-0036 §I11 + §6.7 parity with Lua/Python).  New
  `on_allowlist_changed` callback symbol + `plh_allowlist_changed_args_t`
  / `plh_allowed_peer_t` arg structs.  v3 plugins silently lacked these
  (no compile error; runtime behaviour was as if the host didn't expose
  the §I11 surface at all).  Rebuild plugins against the v4 header.
- v4 → v5 (#194 Phase B1+B2, 2026-06-10): `PlhNativeContext` gains
  `metrics_json` + `is_in_band` + `update_flexzone_checksum` +
  `set_verify_checksum` (Phase B1) plus `out_capacity` + `out_policy` +
  `in_capacity` + `in_policy` + `last_seq` (Phase B2) — diagnostic +
  flexzone-control + queue-depth parity with Lua + Python (which
  already exposed these via per-side closures / `.def`s).  v4 plugins
  silently lacked these; rebuild against the v5 header.
- v5 → v6 (#194 Phase C, 2026-06-11): **architectural cleanup — the
  Two-Layer Plugin Authoring Model (§2.4).**  Tier 1 C ABI now
  malloc-free and exception-free; Tier 2 C++ wrapper substantially
  expanded with concepts, handles, span views.  Replaced six char*-returning
  surfaces with int/macro/visitor patterns:
  - `queue_mechanism` → `int` + `PLH_MECHANISM_*` macros (was `const char *`)
  - `stop_reason` → `int` + `PLH_STOP_REASON_*` macros (was `const char *`)
  - `out_policy` / `in_policy` → `int` + `PLH_QUEUE_POLICY_*` macros (were malloc'd char*)
  - `band_join` → `int` (1/0/-1); dropped list-from-join behaviour (was malloc'd char*)
  - `metrics_json` → replaced by **opaque snapshot pattern**:
    `metrics_snapshot()` (returns thread-local view ptr) + `metrics_get(snap, key, out*)`.
  - `allowed_peers` / `band_members` → **visitor pattern**: visit + contains
    + count triplet; no allocations cross the boundary; plus inquiry
    helpers `*_contains` / `*_count` for cheap yes/no membership questions.
  All v5 plugin code that called any of the above will fail to link or
  return wrong-type — rebuild against the v6 header.  See HEP-CORE-0032
  §2 for the Two-Tier API Model this cleanup implements; see §2.4 of
  this HEP for the design considerations.
- v6 → v7 (#84 task: `NativeEngine::build_api_(HubAPI&)` — extend beyond
  MVP, 2026-06-11): hub-side surface parity with Python/Lua.
  `PlhNativeContext` gains 16 new fn ptrs at the **end** of the struct
  (additive — no reordering, no breaking change for v6 plugins running on
  the role side):
  - `hub_metrics_json` / `hub_config_json` / `hub_query_metrics_json`
  - `hub_list_channels_json` / `hub_get_channel_json` (and the 4 list/get
    pairs for roles/bands/peers)
  - `hub_close_channel` / `hub_broadcast_channel` / `hub_post_event`
  - `hub_augment_timeout_ms` / `hub_set_augment_timeout`
  `wire_hub()` now assigns **every** ctx fn ptr — role-only methods
  (band/spinlock/queue/slot/metrics_snapshot/flexzone) get noop stubs
  returning documented sentinels so pure-C hub plugins cannot segfault
  from missing null-checks.  Symmetric: role-side `wire()` populates the
  new `hub_*` ptrs with role-side noop stubs (empty `""` for JSON
  returns, `-1` for hub_close_channel / hub_broadcast_channel /
  hub_post_event, `0` for hub_augment_timeout_ms).  v6 plugins rebuilt
  against v7 see new fields zero-initialized in their own struct copy —
  but plugins never own a `PlhNativeContext`, the host populates it; so
  pure ABI compatibility (no rebuild needed) is preserved for v6
  role-side plugins.  See §4.9 for the full hub-side matrix.

The native engine exports `native_abi_info()` returning a pointer to a static `PlhAbiInfo`.
The framework validates at load time:

| Check | Failure Mode |
|-------|-------------|
| `struct_size >= sizeof(PlhAbiInfo)` | Native engine compiled with older header (missing fields) |
| `sizeof_void_ptr == sizeof(void*)` | 32/64-bit mismatch |
| `sizeof_size_t == sizeof(size_t)` | size_t width mismatch |
| `byte_order == host_byte_order` | Endianness mismatch (or 0 = don't care) |
| `api_version == PLH_NATIVE_API_VERSION` | Breaking API change between native engine and host |

Any mismatch is a hard error: the native engine is rejected, dlclose is called, and role
startup aborts. If `native_abi_info` is not exported, the check is skipped with a
warning (permissive mode for legacy native engines).

### 4.4 Role Callback Symbols

Implement the callbacks your role needs. Unimplemented callbacks are resolved as NULL
and silently skipped.

| Symbol | Signature | Roles | Description |
|--------|-----------|-------|-------------|
| `on_init` | `void on_init(void)` | All | Called once after build_api, before data loop |
| `on_stop` | `void on_stop(void)` | All | Called once after data loop exits |
| `on_produce` | `bool on_produce(const plh_tx_t *tx)` | Producer | Write data to output slot/flexzone. true=commit, false=discard. |
| `on_consume` | `bool on_consume(const plh_rx_t *rx)` | Consumer | Read data from input slot/flexzone. true=commit, false=discard. |
| `on_process` | `bool on_process(const plh_rx_t *rx, const plh_tx_t *tx)` | Processor | Read input, write output. true=commit, false=discard. |
| `on_inbox` | `bool on_inbox(const plh_inbox_msg_t *msg)` | All | Receive a typed inbox message (HEP-CORE-0027). |
| `on_heartbeat` | `void on_heartbeat(void)` | All | Called from control thread. Must be thread-safe. |
| `on_channel_closing` | `void on_channel_closing(const plh_channel_closing_args_t *args)` | All | Broker closed a channel this role is on (HEP-CORE-0011). |
| `on_consumer_died` | `void on_consumer_died(const plh_consumer_died_args_t *args)` | Producer/Processor | Registered consumer died (HEP-CORE-0011). |
| `on_hub_dead` | `void on_hub_dead(const plh_hub_dead_args_t *args)` | All | ZMTP declared a broker connection dead (HEP-CORE-0023 §2.5). |
| `on_band_member_joined` | `void on_band_member_joined(const plh_band_member_joined_args_t *args)` | All | Member joined a band this role is in (HEP-CORE-0030 §5.3). |
| `on_band_member_left` | `void on_band_member_left(const plh_band_member_left_args_t *args)` | All | Member left a band this role is in (HEP-CORE-0030 §5.3). |
| `on_band_message` | `void on_band_message(const plh_band_message_args_t *args)` | All | Band broadcast received (HEP-CORE-0030 §5.3). |
| `on_band_lost` | `void on_band_lost(const plh_band_lost_args_t *args)` | All | Synthetic — band routing invalidated (e.g. hub-dead). |
| `on_allowlist_changed` | `void on_allowlist_changed(const plh_allowlist_changed_args_t *args)` | Producer/Processor | Framework atomically applied a new authorized-consumer snapshot to the queue's ZAP cache (HEP-CORE-0036 §I11). API v4 #194. |

**Direction structs** (defined in `native_invoke_types.h`):

```c
typedef struct plh_rx_t {
    const void *slot;       /* Input slot (read-only) */
    size_t      slot_size;
    void       *fz;         /* Flexzone (mutable — per HEP-0002) */
    size_t      fz_size;
} plh_rx_t;

typedef struct plh_tx_t {
    void       *slot;       /* Output slot (writable) */
    size_t      slot_size;
    void       *fz;         /* Flexzone (mutable) */
    size_t      fz_size;
} plh_tx_t;

typedef struct plh_inbox_msg_t {
    const void *data;       /* Typed inbox payload */
    size_t      data_size;
    const char *sender_uid; /* Sender's role UID */
    uint64_t    seq;        /* Message sequence number */
} plh_inbox_msg_t;
```

**Notification + visitor arg structs** (defined in `native_invoke_types.h`):

```c
/* Authorized-peer record — used by:
 *   (a) on_allowlist_changed callback (event-driven notification), and
 *   (b) ctx->allowed_peers() visitor (polling — see §4.1).
 * Symmetric struct definition + lifetime contract across both surfaces;
 * see §4.8 Lifetime + Security Contract for the contract details. */
typedef struct plh_allowed_peer_t {
    const char *role_uid;
    const char *pubkey_z85;     /* Z85-encoded 40-char CURVE pubkey */
} plh_allowed_peer_t;

/* Visitor signature for ctx->allowed_peers() iteration.  Visitor MUST be
 * noexcept (throwing across the C ABI is undefined behaviour; host wraps
 * the iteration in a try/catch as defense-in-depth and returns -1 on a
 * caught exception, but plugin authors must not rely on this).  Pointers
 * inside `peer` valid only for the duration of this visitor call. */
typedef void (*plh_allowed_peer_visitor)(const plh_allowed_peer_t *peer,
                                         void *userdata);

/* on_allowlist_changed args (HEP-CORE-0036 §I11).
 * Fired on producer/processor after framework atomically applies a new
 * authorized-consumer snapshot to ZAP cache.  args struct + peers array
 * + all char* fields valid ONLY for the duration of the call. */
typedef struct plh_allowlist_changed_args_t {
    const char                *channel;
    const plh_allowed_peer_t  *peers;
    size_t                     peer_count;
    const char                *reason;     /* "consumer_joined",
                                              "consumer_left",
                                              "heartbeat_timeout", ... */
} plh_allowlist_changed_args_t;

/* Band-member record — used by ctx->band_members() visitor (§4.1).
 * Same lifetime contract as plh_allowed_peer_t. */
typedef struct plh_band_member_t {
    const char *role_uid;
    const char *role_name;      /* May be NULL or empty if broker did not
                                 * supply one. */
} plh_band_member_t;

typedef void (*plh_band_member_visitor)(const plh_band_member_t *member,
                                        void *userdata);
```

The band-message, hub-dead, channel-closing, consumer-died, band-member-
joined, and band-member-left arg-struct shapes follow the same lifetime
contract — see `native_invoke_types.h` for the full set.

**Return value contract (all data callbacks):**
- `true` -> `InvokeResult::Commit` (slot is published)
- `false` -> `InvokeResult::Discard` (slot is dropped, loop continues)

If the function pointer is NULL (symbol not exported), the framework returns
`InvokeResult::Discard`.

### 4.5 Optional Metadata Symbols

| Symbol | Signature | Description |
|--------|-----------|-------------|
| `native_name` | `const char *native_name(void)` | Human-readable name for logging |
| `native_version` | `const char *native_version(void)` | Version string for logging |
| `native_is_thread_safe` | `bool native_is_thread_safe(void)` | Controls multi-state behavior (see section 9) |

### 4.6 Schema Validation Macros (Required)

```c
#define PLH_DECLARE_SCHEMA(Name, Schema, Size)
```

Generates two **required** symbols:
- `native_schema_<Name>()` -> canonical schema string
- `native_sizeof_<Name>()` -> sizeof the native engine's compiled struct

The framework validates both at `register_slot_type()` time. `native_sizeof_<Name>()` is
compared against `compute_field_layout(schema, packing)` — the infrastructure-authoritative
size. Missing either export is a hard error.

**Name** must match one of the registered type names:

| Name | Role | Description |
|------|------|-------------|
| `OutSlotFrame` | Producer, Processor output | Output data slot |
| `InSlotFrame` | Consumer, Processor input | Input data slot |
| `OutFlexFrame` | Producer, Processor output | Output flexzone |
| `InFlexFrame` | Consumer, Processor input | Input flexzone |
| `InboxFrame` | All (if configured) | Inbox message payload |

Producer and Consumer scripts also get `SlotFrame`/`FlexFrame` aliases (created in
`build_api()`), but native engines should use the directional names for clarity.

**Schema** is a pipe-delimited canonical schema string with the format
`"field_name:type_str:count:length|..."`. Example:

```c
PLH_DECLARE_SCHEMA(SlotFrame,
    "timestamp:float64:1:0|temperature:float32:1:0|status:uint8:1:0",
    sizeof(MySlotStruct))
```

The framework computes the same canonical string from the JSON config's schema fields
and compares them at load time. Mismatch is a hard error.

### 4.7 Framework API (Function Pointers on PlhNativeContext)

Framework services are accessed through function pointers on the `PlhNativeContext`
struct. The host fills these pointers before calling `native_init()`. Every function
takes `const PlhNativeContext *ctx` as its first argument — the native engine simply
passes its stored context pointer through.

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
| `ctx->set_critical_error` | `void (*)(ctx, const char *msg)` | Signal a critical error (sets flag + requests stop). |
| `ctx->is_critical_error` | `int (*)(ctx)` | Check if critical error has been flagged. Returns 1 or 0. |
| `ctx->stop_reason` | `int (*)(ctx)` | Get stop reason as a `PLH_STOP_REASON_*` macro value (see §4.1). |
| `ctx->out_slots_written` | `uint64_t (*)(ctx)` | Slots written (producer/processor output). |
| `ctx->in_slots_received` | `uint64_t (*)(ctx)` | Slots received (consumer/processor input). |
| `ctx->out_drop_count` | `uint64_t (*)(ctx)` | Dropped slot count. |
| `ctx->script_error_count` | `uint64_t (*)(ctx)` | Script error count. |
| `ctx->loop_overrun_count` | `uint64_t (*)(ctx)` | Loop timing overrun count. |
| `ctx->last_cycle_work_us` | `uint64_t (*)(ctx)` | Work duration of last iteration in microseconds. |
| `ctx->queue_mechanism` | `int (*)(ctx, int side)` | Negotiated CURVE mechanism for `side` (`PLH_SIDE_TX`/`RX`) as `PLH_MECHANISM_*` macro. |
| `ctx->out_capacity` / `ctx->in_capacity` | `uint64_t (*)(ctx)` | Queue ring slot count for tx/rx side (0 when side not wired). |
| `ctx->out_policy` / `ctx->in_policy` | `int (*)(ctx)` | Queue overflow policy as `PLH_QUEUE_POLICY_*` macro. |
| `ctx->last_seq` | `uint64_t (*)(ctx)` | Last decoded wire seq on rx side. 0 until first read. |
| `ctx->is_channel_ready` | `int (*)(ctx, const char *channel)` | 1 iff queue serving `channel` is HEP-0036 §6.7 Active. 0 otherwise; -1 on error. |
| `ctx->is_in_band` | `int (*)(ctx, const char *channel)` | 1 iff role is current band member. 0 otherwise; -1 on error. |
| `ctx->update_flexzone_checksum` | `int (*)(ctx)` | Recompute + commit SHM flexzone checksum. 1=ok, 0=skipped, -1=error. |
| `ctx->set_verify_checksum` | `void (*)(ctx, int enable)` | Toggle per-slot rx checksum verification. |
| `ctx->band_join` / `band_leave` | `int (*)(ctx, const char *channel)` | Join/leave a band. 1=ok, 0=fail, -1=error. |
| `ctx->band_broadcast` | `void (*)(ctx, const char *channel, const char *body_json)` | Broadcast JSON body. Fire-and-forget (status return tracked under #192). |
| `ctx->band_members` | `int (*)(ctx, const char *channel, plh_band_member_visitor visitor, void *userdata)` | Visit each band member. Returns count (>=0), -1 on error. Visitor MUST be noexcept (§4.8). |
| `ctx->band_member_contains` | `int (*)(ctx, const char *channel, const char *role_uid)` | 1=member, 0=not, -1=error. |
| `ctx->band_member_count` | `int (*)(ctx, const char *channel)` | Member count (>=0) or -1 on error. |
| `ctx->allowed_peers` | `int (*)(ctx, const char *channel, plh_allowed_peer_visitor visitor, void *userdata)` | Visit each authorized peer (HEP-0036 §I11). Returns count (>=0), -1 on error. Visitor MUST be noexcept (§4.8). |
| `ctx->allowed_peer_contains` | `int (*)(ctx, const char *channel, const char *role_uid)` | 1=authorized, 0=not, -1=error. |
| `ctx->allowed_peer_count` | `int (*)(ctx, const char *channel)` | Authorized-peer count (>=0) or -1 on error. |
| `ctx->metrics_snapshot` | `const void *(*)(ctx)` | Build a thread-local metrics-tree cache and return an opaque view ptr. NULL on error. Lifetime: until next snapshot on same thread (§4.8). |
| `ctx->metrics_get` | `int (*)(const void *snap, const char *key, double *out)` | Dotted-path metric lookup. 1=found (writes *out), 0=missing, -1=error. |
| `ctx->spinlock_lock` / `unlock` / `count` / `is_locked` | see §4.1 | Spinlocks (HEP-CORE-0002 §2.2). |
| `ctx->slot_logical_size` / `flexzone_logical_size` | see §4.1 | Schema sizes (HEP-CORE-0007). |
| `ctx->wait_for_role` | see §4.1 | Block until role appears in broker (HEP-CORE-0023). |

All function pointers are null-safe on the host side. The C++ wrapper in §5
checks the pointer before calling and returns the documented sentinel
(`-1` for int-returning fns, `nullopt` for `MetricsSnapshot::get`,
`false` for bool-returning fns) when a fn ptr is unwired.  Plain C
plugins should defensively null-check before calling.

> The table above covers the role-side surface (v6 baseline).  The 16
> hub-side fn ptrs added in v7 (`hub_metrics_json` / `hub_config_json` /
> `hub_query_metrics_json` / 4 list+get pairs / `hub_close_channel` /
> `hub_broadcast_channel` / `hub_post_event` / `hub_augment_timeout_ms` /
> `hub_set_augment_timeout`) are documented in §4.9.  Both surfaces are
> populated on **both** sides of the wire — role-only methods route to
> noop stubs on hub-side ctx; hub-only methods route to noop stubs on
> role-side ctx — so plugin code can call any ctx fn without null
> checks and the response shape is the documented sentinel for the
> wrong-side case.

### 4.8 Lifetime + Security Contract (#194)

Every borrowed pointer the framework hands to the plugin carries a
documented lifetime.  The C ABI does not own anything on the plugin's
behalf — there is no `plh_free()` because nothing was allocated for the
plugin to free.  This section formalizes the contract so that violations
are unambiguously plugin bugs, not framework weaknesses.

**Lifetime classes:**

| Pointer surface | Valid until |
|---|---|
| `ctx->uid`, `ctx->name`, `ctx->channel`, identity strings | `native_finalize()` returns |
| `plh_allowed_peer_t::role_uid` / `::pubkey_z85` (visitor arg) | The visitor call returns (host releases the snapshot copy on visitor return) |
| `plh_band_member_t::role_uid` / `::role_name` (visitor arg) | Same as above |
| `plh_allowlist_changed_args_t` + nested `peers[]` (callback arg) | The callback returns |
| `plh_band_message_args_t` + nested fields (callback arg) | The callback returns |
| `metrics_snapshot()` return ptr | Next `metrics_snapshot()` call on the **same thread** |
| `hub_*_json()` return ptr (v7) | Next `hub_*_json()` call on the **same thread** (shared thread-local scratch buffer — same pattern as `metrics_snapshot`) |
| `plh_*_args_t` for every other on_* callback | The callback returns |

Plugin stashes a borrowed pointer past its lifetime → reads garbage at
best, segfaults at worst.  This is plugin bug territory: the host has
no way to invalidate stashed pointers because the plugin's memory is
in-process and owned by the plugin.

**"Visitor MUST be noexcept" rule.**  Visitor callbacks the plugin
hands to `ctx->allowed_peers(...)` / `ctx->band_members(...)` are
invoked synchronously from inside the host.  A C++ exception unwinding
across the C ABI boundary is undefined behaviour — the host's
iteration loop has no `catch` clause matching `std::exception`, the
host's RAII guards may not run, host invariants may break.

The host wraps every visitor invocation in a `try { ... } catch (...)
{ LOGGER_ERROR(...); return -1; }` as **defense in depth** — a buggy
plugin throwing will not corrupt the host — but plugin authors must
not treat this as a feature.  Document this rule prominently in
plugin-author docs (`README_NativePlugin.md`).

**Threat model.**  The native plugin runs in-process with the role's
full privileges.  There is no sandbox boundary.  "Compromised plugin"
is functionally equivalent to "compromised role process."  The C ABI
surface therefore is **not** designed to defend against a malicious
plugin (impossible without process-level isolation); it is designed
to:

1. **Limit accidental damage from buggy plugins** (lifetime contracts,
   visitor noexcept rule, host-side try/catch).
2. **Avoid exposing secret material**.  The C ABI surfaces only
   public-domain information: role UIDs (network-public), pubkeys
   (publishable by definition), channel names, band names, metric
   keys + values.  Private keys, ZAP shared secrets, SHM secrets are
   never reachable via any ctx->fn — they live in the
   SecureMemorySubsystem (HEP-CORE-0040) and are accessed by host code
   only.  A compromised plugin that walks the process heap can still
   find them (in-process), but the C ABI does not narrow that path.
3. **Resist plugin-internal corruption of host control flow.**  All
   pointers in `PlhNativeContext` are passed as `const
   PlhNativeContext *`; a `const_cast` write by the plugin only
   corrupts the plugin's own future calls (the host never dispatches
   through the ctx fn ptrs).  An optional future hardening — mprotect
   the `PlhNativeContext` page to `PROT_READ` after population — is
   tracked as a follow-up task; not required for the v6 release.

Out of scope at the C ABI level: side-channel attacks (Spectre, cache
timing), CPU-level mitigations, kernel-level intrusion via
`/proc/PID/mem` or `ptrace`.

### 4.9 Hub-side API Surface (API v7, #84, 2026-06-11)

The native plugin engine is loadable on either side: a role
process via `NativeEngine::build_api_(RoleAPIBase&)`, or a hub
process via `NativeEngine::build_api_(HubAPI&)`.  Until v7 the
hub-side `wire_hub()` populated only `log` + `request_stop`,
forcing pure-C hub plugins to defensively null-check every
`ctx->fn` before invoking it.  v7 fills every `ctx->fn` on both
sides — hub-applicable methods route to real `HubAPI`
implementations; role-only methods (band, allowed_peers,
spinlock, queue_*, slot_*, metrics_snapshot, flexzone-checksum)
route to **noop stubs** returning the documented sentinel.  A
pure-C hub plugin can call any `ctx->fn` without null-checking;
the behaviour for role-only methods is observationally
identical to "the queue/state were not ready yet" — the same
sentinel the role-side impl would return mid-startup.

**Hub-side methods** added in v7 (16 fn ptrs at the END of
`PlhNativeContext`, additive ABI bump):

| Function Pointer | Signature | Description |
|---|---|---|
| `ctx->hub_metrics_json` | `const char *(*)(ctx)` | Hub metrics tree (`utils::get_metrics_tree()`) serialised to JSON. |
| `ctx->hub_config_json` | `const char *(*)(ctx)` | Hub config dump. |
| `ctx->hub_query_metrics_json` | `const char *(*)(ctx, const char *categories_json)` | Filtered metrics by category list. |
| `ctx->hub_list_channels_json` | `const char *(*)(ctx)` | JSON array of channel descriptors. |
| `ctx->hub_get_channel_json` | `const char *(*)(ctx, const char *name)` | Channel info by name; `"{}"` if absent. |
| `ctx->hub_list_roles_json` | `const char *(*)(ctx)` | JSON array of role descriptors. |
| `ctx->hub_get_role_json` | `const char *(*)(ctx, const char *uid)` | Role info by uid; `"{}"` if absent. |
| `ctx->hub_list_bands_json` | `const char *(*)(ctx)` | JSON array of band descriptors. |
| `ctx->hub_get_band_json` | `const char *(*)(ctx, const char *name)` | Band info by name; `"{}"` if absent. |
| `ctx->hub_list_peers_json` | `const char *(*)(ctx)` | JSON array of federation peer descriptors. |
| `ctx->hub_get_peer_json` | `const char *(*)(ctx, const char *hub_uid)` | Peer info by hub_uid; `"{}"` if absent. |
| `ctx->hub_close_channel` | `int (*)(ctx, const char *name)` | Schedule close.  1=accepted, -1=error.  Idempotent for unknown names. |
| `ctx->hub_broadcast_channel` | `int (*)(ctx, const char *channel, const char *message, const char *data_json)` | Control-plane broadcast.  1=accepted, -1=error. |
| `ctx->hub_post_event` | `int (*)(ctx, const char *name, const char *data_json)` | Post user event; fires `on_app_<name>` on worker thread.  1=accepted, 0=invalid name, -1=error. |
| `ctx->hub_augment_timeout_ms` | `int64_t (*)(ctx)` | Current augment timeout knob.  -1=infinite, 0=non-blocking, >0=N ms. |
| `ctx->hub_set_augment_timeout` | `void (*)(ctx, int64_t ms)` | Setter for augment timeout. |

**Sentinels on role-side contexts** — when a plugin calls
`hub_*` from a role-side ctx (no `HubAPI*` attached), the
following sentinels are returned:

| Group | Sentinel |
|---|---|
| `hub_*_json` (all) | `""` (empty string) |
| `hub_close_channel` / `hub_broadcast_channel` / `hub_post_event` | `-1` |
| `hub_augment_timeout_ms` | `0` |
| `hub_set_augment_timeout` | (noop) |

**Lifetime contract** for JSON returns: every `hub_*_json` fn
returns a pointer into a **thread-local scratch buffer**.  Valid
until the next `hub_*_json` call on the same thread.  Plugin
authors who need a value across more than one call must `strdup`
or copy into their own storage.  Same shape as
`metrics_snapshot()` (§4.8) — the host owns the storage; the
plugin owns the lifetime discipline.

**C++ wrapper** — `plh::Context` gains symmetric `hub_*`
methods (§5) that return `const char *` (empty `""` on
unwired), plus a convenience `is_hub()` predicate that the
plugin can switch on instead of null-checking every call:

```cpp
plh::Context ctx{raw};
if (ctx.is_hub()) {
    auto channels_json = ctx.hub_list_channels_json();
    // ... parse, react
} else {
    // role-side path
}
```

**Cross-engine parity.**  Python `pylabhub_hub.HubAPI` and Lua
`hub_api()` script-side bindings expose the same 16 hub-callable
methods, plus the engine-agnostic `log` / `uid` / `name` /
`request_shutdown` (which on Native are not prefixed `hub_` —
they live on `PlhNativeContext` as side-agnostic fn ptrs
populated on both wires).  Where Python returns a `dict`/`list`
and Lua returns a table, Native returns a JSON string — plugin
authors who want structured access link `nlohmann/json` (or any
JSON parser) in their plugin.  The shape of the JSON is the same
as what `HubAPI::metrics()` / `list_channels()` / etc. serialise
via their C++ public surface (HEP-CORE-0033 §6).

> Naming difference: Python/Lua bindings live in a hub-only
> namespace (e.g. `api.list_channels()` on the script side), so
> no prefix is needed.  Native's fn ptrs share one
> `PlhNativeContext` table with the role-side methods (band_*,
> spinlock_*, queue_*, etc.), so the `hub_` prefix disambiguates
> at the C ABI surface.

---

## 5. C++ Convenience Layer (the natural choice — §2.4 Tier 2)

The C++ convenience layer is available when `native_engine_api.h` is included
from a C++ translation unit at C++20 or later. It is **header-only**: every
wrapper method is `inline` (in the class body or via `template`).  Plugin
authors include one header and link nothing extra.  The wrapper compiles
down to the same indirect-call surface as raw C ABI use — **zero binary
cost** — while contributing concept-checked templates, typed enums,
lightweight handle types, `std::span` views, and `std::optional` returns.

The design philosophy is set out in §2.4 (Two-Layer Plugin Authoring Model).
The Tier 2 wrapper does not replace Tier 1 — it *promotes* it.

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

### 5.2 Typed Enum Classes (#194)

Each of the three v6 enumerated returns gets an `enum class : int` whose
enumerators inherit values directly from the C ABI macros (§4.1).  No
`static_cast` is needed when converting between the C return value and
the typed enum — the underlying values are identical.

```cpp
namespace plh {

enum class Mechanism : int {
    Uninitialized = PLH_MECHANISM_UNINITIALIZED,
    Curve         = PLH_MECHANISM_CURVE,
};

enum class QueuePolicy : int {
    Unknown  = PLH_QUEUE_POLICY_UNKNOWN,
    Shm      = PLH_QUEUE_POLICY_SHM,
    ZmqDrop  = PLH_QUEUE_POLICY_ZMQ_DROP,
    ZmqBlock = PLH_QUEUE_POLICY_ZMQ_BLOCK,
    ZmqRing  = PLH_QUEUE_POLICY_ZMQ_RING,
};

enum class StopReason : int {
    Normal         = PLH_STOP_REASON_NORMAL,
    PeerDead       = PLH_STOP_REASON_PEER_DEAD,
    HubDead        = PLH_STOP_REASON_HUB_DEAD,
    CriticalError  = PLH_STOP_REASON_CRITICAL_ERROR,
    ChannelClosed  = PLH_STOP_REASON_CHANNEL_CLOSED,
    ScriptError    = PLH_STOP_REASON_SCRIPT_ERROR,
};

[[nodiscard]] constexpr std::string_view to_string(Mechanism m) noexcept;
[[nodiscard]] constexpr std::string_view to_string(QueuePolicy p) noexcept;
[[nodiscard]] constexpr std::string_view to_string(StopReason r) noexcept;

} // namespace plh
```

`to_string` returns `std::string_view` of a static literal — zero
allocation, safe for logging.  `constexpr` so the result is usable in
compile-time `switch` contexts.

### 5.3 C++20 Concepts for Visitor Type Safety (#194)

The visitor templates exposed on handle types (§5.5) are constrained
by C++20 `requires`-clauses.  Misuse → a clean compile error at the
concept boundary instead of a 200-line template diagnostic inside the
thunk.

```cpp
namespace plh {

template <typename V>
concept AllowedPeerVisitor =
    std::invocable<V &, const plh_allowed_peer_t *>;

template <typename V>
concept BandMemberVisitor =
    std::invocable<V &, const plh_band_member_t *>;

} // namespace plh
```

### 5.4 MetricsSnapshot — Opaque Handle Wrapper (#194)

`MetricsSnapshot` is a typed value-class wrapper around the opaque
`const void *` returned by `ctx->metrics_snapshot(ctx)`.  It is **not** a
RAII resource handle — the underlying thread-local cache is host-owned;
the wrapper merely binds the snapshot pointer to a typed lookup
interface.  Lifetime: valid until the next `metrics_snapshot()` call on
the same thread.

```cpp
namespace plh {

class MetricsSnapshot {
    const Context *ctx_;
    const void    *snap_;

public:
    constexpr MetricsSnapshot(const Context &ctx, const void *snap) noexcept
        : ctx_(&ctx), snap_(snap) {}

    /// Get a metric by dotted-path key.  Returns nullopt if missing,
    /// snapshot invalid, or ABI fn unwired.
    [[nodiscard]] std::optional<double> get(const char *key) const noexcept;
    [[nodiscard]] std::optional<double> get(std::string_view key) const noexcept;

    /// Ergonomic shorthand.
    [[nodiscard]] std::optional<double> operator[](const char *key) const noexcept
    { return get(key); }

    [[nodiscard]] explicit operator bool() const noexcept { return snap_ != nullptr; }
    [[nodiscard]] constexpr const void *raw() const noexcept { return snap_; }
};

} // namespace plh
```

### 5.5 Handle Types: AllowedPeersHandle, BandHandle (#194)

Handles are **lightweight typed views** — each holds exactly two
pointers (`const Context *` + `const char *channel`) and is
`trivially_copyable`.  `static_assert`s in the header pin this contract:

```cpp
static_assert(std::is_trivially_copyable_v<plh::AllowedPeersHandle>);
static_assert(std::is_trivially_copyable_v<plh::BandHandle>);
static_assert(std::is_trivially_copyable_v<plh::MetricsSnapshot>);
static_assert(sizeof(plh::AllowedPeersHandle) == 2 * sizeof(void *));
```

Returning a handle by value costs nothing — the compiler passes both
pointers in registers.

```cpp
namespace plh {

class AllowedPeersHandle {
    const Context *ctx_;
    const char    *channel_;

public:
    constexpr AllowedPeersHandle(const Context &ctx, const char *channel) noexcept
        : ctx_(&ctx), channel_(channel) {}

    /// Template visitor — concept-constrained, zero-cost thunk forwards to C ABI.
    template <AllowedPeerVisitor V>
    int visit(V &&v) const noexcept;

    /// Raw-fnptr overload — for callers that already have a plain function pointer.
    int visit(plh_allowed_peer_visitor v, void *userdata) const noexcept;

    [[nodiscard]] bool contains(const char *role_uid) const noexcept;
    [[nodiscard]] int  count() const noexcept;

    /// Collection helpers — encourage "snapshot once, query many" pattern.
    /// For repeated membership queries against a stable allowlist, calling
    /// to_uid_set() once and doing O(1) lookups against the std::unordered_set
    /// is measurably faster than repeated visit/contains calls (each of which
    /// pays a per-call vector copy under the framework lock).
    [[nodiscard]] std::vector<std::string>        collect_uids() const;
    [[nodiscard]] std::unordered_set<std::string> to_uid_set() const;

    [[nodiscard]] constexpr const char *channel() const noexcept { return channel_; }
};

class BandHandle {
    const Context *ctx_;
    const char    *channel_;

public:
    constexpr BandHandle(const Context &ctx, const char *channel) noexcept
        : ctx_(&ctx), channel_(channel) {}

    template <BandMemberVisitor V>
    int visit_members(V &&v) const noexcept;
    int visit_members(plh_band_member_visitor v, void *userdata) const noexcept;

    [[nodiscard]] bool contains(const char *role_uid) const noexcept;
    [[nodiscard]] int  member_count() const noexcept;

    /// Fire-and-forget broadcast.  Returns void at the C ABI v6 level
    /// (status return is a cross-engine concern tracked under #192).
    void broadcast(const char *body_json) const noexcept;

    [[nodiscard]] std::vector<std::string>        collect_uids() const;
    [[nodiscard]] std::unordered_set<std::string> to_uid_set() const;

    [[nodiscard]] constexpr const char *channel() const noexcept { return channel_; }
};

} // namespace plh
```

Methods on `plh::Context` that produce handles:

```cpp
[[nodiscard]] constexpr AllowedPeersHandle allowed_peers(const char *channel) const noexcept
{ return AllowedPeersHandle(*this, channel); }

[[nodiscard]] constexpr BandHandle band(const char *channel) const noexcept
{ return BandHandle(*this, channel); }
```

### 5.6 AllowlistChangedView — std::span over Event Args (#194)

The `on_allowlist_changed` callback receives a `const
plh_allowlist_changed_args_t *`.  The C++ wrapper exposes a thin view
with a `std::span` over the contiguous `peers[]` array, enabling
idiomatic range-for iteration:

```cpp
namespace plh {

class AllowlistChangedView {
    const plh_allowlist_changed_args_t *args_;
public:
    constexpr explicit AllowlistChangedView(const plh_allowlist_changed_args_t *a) noexcept
        : args_(a) {}

    [[nodiscard]] constexpr std::string_view channel() const noexcept
    { return args_->channel; }

    [[nodiscard]] constexpr std::string_view reason() const noexcept
    { return args_->reason; }

    [[nodiscard]] constexpr std::span<const plh_allowed_peer_t> peers() const noexcept
    { return {args_->peers, args_->peer_count}; }
};

} // namespace plh
```

### 5.7 Hub-side Wrappers (API v7, #84)

The `plh::Context` wrapper exposes a symmetric `hub_*` surface
matching the 16 fn ptrs added in v7 (§4.9), plus an `is_hub()`
predicate for branch-once dispatch.  All wrappers follow the same
null-safe pattern as the role-side wrappers — unwired fn ptr →
documented sentinel return — so the same `Context` value works on
either side of the wire:

```cpp
extern "C" bool native_init(PlhNativeContext *raw)
{
    plh::Context ctx{raw};
    if (!ctx.valid()) return false;

    if (ctx.is_hub())
    {
        // Hub-side: enumerate channels and post a startup event.
        auto channels = ctx.hub_list_channels_json();
        ctx.log(plh::LogLevel::Info, channels);
        auto r = ctx.hub_post_event("plugin_started", R"({"version":"1.0"})");
        if (r == plh::PostEventResult::InvalidName)
            ctx.log(plh::LogLevel::Error, "plugin bug: event name rejected");
    }
    else
    {
        // Role-side: register custom metric.
        ctx.report_metric("startup_count", 1.0);
    }
    return true;
}
```

**Wrapper method matrix** (every method is `inline`, `noexcept`,
zero binary cost — same as role-side wrappers):

| C++ method | C ABI delegate | Return on unwired / wrong side |
|---|---|---|
| `is_hub()` | role_tag comparison (`"hub"`) | `false` |
| `hub_metrics_json()` | `ctx->hub_metrics_json` | `""` |
| `hub_config_json()` | `ctx->hub_config_json` | `""` |
| `hub_query_metrics_json(categories)` | `ctx->hub_query_metrics_json` | `""` |
| `hub_list_channels_json()` etc. (4 list+get pairs) | `ctx->hub_list_*` / `ctx->hub_get_*` | `""` |
| `hub_close_channel(name)` | `ctx->hub_close_channel` | `false` |
| `hub_broadcast_channel(ch, msg, data)` | `ctx->hub_broadcast_channel` | `false` |
| `hub_post_event(name, data)` | `ctx->hub_post_event` | `PostEventResult::TransportError` |
| `hub_augment_timeout_ms()` | `ctx->hub_augment_timeout_ms` | `0` |
| `hub_set_augment_timeout(ms)` | `ctx->hub_set_augment_timeout` | (noop) |

**JSON lifetime contract.**  Every `hub_*_json()` return is a
`const char *` into a shared thread-local scratch buffer (§4.8).
Valid only until the next `hub_*_json()` call on the same thread.
Plugin authors who need the value to survive past the next call
must copy: `std::string s{ctx.hub_metrics_json()};`.

**`is_hub()` design note.**  Detection is by `role_tag == "hub"`
(the literal string the host writes during `wire_hub()`).  This
is intentional — pure-C plugins can do the same check without
linking the C++ wrapper.  The framework guarantees the role-tag
string is one of `"producer"`, `"consumer"`, `"processor"`,
`"hub"`; future role-tag additions for sub-hub variants (e.g.
federation peers) would need to extend this predicate.

### 5.8 Export Macros (existing — unchanged)

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

### 5.9 Example: C++ Producer Native Engine (v6 ergonomic surface)

```cpp
#include <utils/native_engine_api.h>

struct MySlot {
    double   timestamp;
    float    temperature;
    uint8_t  status;
};

PLH_DECLARE_SCHEMA(OutSlotFrame,
    "timestamp:float64:1:0|temperature:float32:1:0|status:uint8:1:0",
    sizeof(MySlot))

static const PlhNativeContext *g_ctx = nullptr;

extern "C" PLH_EXPORT bool native_init(const PlhNativeContext *ctx) {
    g_ctx = ctx;
    plh::Context wrap{ctx};

    // Typed enum class — switch is compiler-checked.
    switch (wrap.queue_mechanism(PLH_SIDE_TX)) {
        case plh::Mechanism::Curve:
            wrap.log(PLH_LOG_INFO, "tx mechanism = CURVE");
            break;
        case plh::Mechanism::Uninitialized:
            wrap.log(PLH_LOG_WARN, "tx mechanism not yet negotiated");
            break;
    }

    // Snapshot-once + O(1) membership check via to_uid_set().
    auto peers = wrap.allowed_peers("out");
    auto authorized = peers.to_uid_set();
    if (!authorized.contains("consumer_A")) {
        wrap.log(PLH_LOG_WARN, "consumer_A not yet authorized");
    }

    return true;
}

extern "C" PLH_EXPORT void native_finalize(void) {
    g_ctx = nullptr;
}

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void) {
    static PlhAbiInfo info = {
        sizeof(PlhAbiInfo), sizeof(void *), sizeof(size_t), 1,
        PLH_NATIVE_API_VERSION
    };
    return &info;
}

// std::span iteration over the on_allowlist_changed args.
extern "C" PLH_EXPORT
void plh_on_allowlist_changed(const plh_allowlist_changed_args_t *args)
{
    plh::AllowlistChangedView view{args};
    plh::Context wrap{g_ctx};
    for (const auto &peer : view.peers()) {
        wrap.log(PLH_LOG_INFO, peer.role_uid);
    }
}

static bool produce(plh::SlotRef<MySlot> slot) {
    if (!slot) return false;
    slot->timestamp = 0.0;      // fill from sensor API
    slot->temperature = 23.5f;
    slot->status = 1;
    return true;
}

PLH_EXPORT_PRODUCE_NOFZ(MySlot, produce)

extern "C" PLH_EXPORT void on_init(void) {}
extern "C" PLH_EXPORT void on_stop(void) {}
```

Compare to a pure-C plugin doing the same membership check via raw
visitor:

```c
/* Pure C: manual hash-set boilerplate — visit + flag + early exit */
struct check_ctx { const char *target; int found; };
static void check_visitor(const plh_allowed_peer_t *p, void *ud) {
    struct check_ctx *c = (struct check_ctx *)ud;
    if (strcmp(p->role_uid, c->target) == 0) c->found = 1;
}
struct check_ctx cc = { "consumer_A", 0 };
ctx->allowed_peers(ctx, "out", check_visitor, &cc);
if (!cc.found) { /* warn */ }

/* Or skip the visitor entirely and use the inquiry function: */
if (ctx->allowed_peer_contains(ctx, "out", "consumer_A") != 1) { /* warn */ }
```

Both layers are first-class.  The C ABI is the contract; the C++ wrapper
is the natural ergonomic surface for the common case.

---

## 6. Three-Layer Verification

NativeEngine applies three independent verification layers before the native engine's
`native_init()` is called. All checks are performed during `load_script()`.

### 6.1 Layer 1: File Integrity (BLAKE2b-256)

When `script.checksum` is present in the JSON config, the framework:

1. Reads the entire shared library file into memory
2. Computes BLAKE2b-256 over the raw bytes (via `pylabhub::crypto::compute_blake2b_array`)
3. Converts to lowercase hex string (64 characters)
4. Compares against the configured checksum

Mismatch is a hard error: native engine is not loaded, role startup aborts.

If `script.checksum` is absent or empty, this check is skipped. This allows development
workflows where the native engine is recompiled frequently. Production deployments should
always set the checksum.

### 6.2 Layer 2: ABI Compatibility (PlhAbiInfo)

After dlopen succeeds, the framework resolves `native_abi_info` and validates the
returned `PlhAbiInfo` struct (see section 4.3). This catches:

- 32/64-bit mismatches (e.g., loading a 32-bit native engine in a 64-bit host)
- Endianness mismatches (cross-compiled native engines)
- API version skew (native engine compiled against an older/newer native_engine_api.h)
- Struct layout drift (struct_size guard)

If `native_abi_info` is not exported, the check is skipped with a warning. This
provides backward compatibility for native engines compiled before ABI checks were added.

### 6.3 Layer 3: Schema Compatibility

During `register_slot_type()`, the framework validates that the native engine's compiled
struct matches the JSON config's schema. Both `native_schema_<Name>` and
`native_sizeof_<Name>` are **required exports** — missing either is a hard error.

1. **Infrastructure-authoritative size**: The framework computes the expected struct size
   from the schema fields + packing via `compute_field_layout()`. This is the same
   computation used by ShmQueue and ZmqQueue for buffer allocation.

2. **Canonical schema string**: The framework computes `"name:type:count:length|..."`
   from the config's `SchemaSpec` fields. The native engine must export
   `native_schema_<Name>()` returning the matching canonical string. Mismatch is a
   hard error.

3. **sizeof validation**: The native engine must export `native_sizeof_<Name>()` returning
   `sizeof(NativeStruct)`. The framework compares this against the schema-computed size
   from step 1. Mismatch is a hard error — it catches padding differences, field
   reordering, or type width disagreement between the native engine's compiler and the
   schema definition.

The same size cross-validation applies to all three engine types:

| Engine | Built type | Validated against |
|--------|-----------|-------------------|
| Python | ctypes.sizeof(struct) | compute_field_layout(schema, packing) |
| Lua | ffi.sizeof(struct) | compute_field_layout(schema, packing) |
| Native | native_sizeof_<T>() | compute_field_layout(schema, packing) |

If the native engine does not export `native_schema_<Name>` for a given type name, the
schema string check is skipped with a warning. But `native_sizeof_<Name>` is always
required — without it, the framework cannot guarantee memory safety.

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

Native engines follow the same directory structure as Python and Lua scripts:

```
<role_dir>/
├── config.json
└── script/
    ├── python/
    │   └── __init__.py
    ├── lua/
    │   └── init.lua
    └── native/
        └── libmy_plugin.so      ← native engine library
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

If the native engine exports `native_is_thread_safe()` returning true,
`supports_multi_state()` returns true. This signals to the framework that:

- The native engine's callbacks are fully reentrant
- Generic `invoke()` calls from any thread are permitted
- The native engine is responsible for its own internal synchronization

This is the opposite of the scripted engines: Python queues cross-thread requests to
the GIL-holding owner thread; Lua creates per-thread lua_State copies. A thread-safe
native engine needs neither mechanism.

### 9.3 release_thread()

`release_thread()` is a no-op for NativeEngine. Native engines manage their own
thread-local resources (if any). The `ThreadEngineGuard` RAII helper still works
correctly — its destructor calls `release_thread()` which simply returns.

---

## 10. Generic Invoke and Eval

### 10.1 invoke(name)

The generic `invoke(name)` dispatches in two stages:

1. Check named function pointers: `"on_heartbeat"` -> `fn_on_heartbeat_`
2. Fall back to `dlsym(handle, name)` for arbitrary exported `void(*)(void)` symbols

This allows native engines to export custom command handlers accessible via the broker's
`INVOKE_REQ` protocol.

### 10.2 invoke(name, args)

JSON arguments are not supported for native engines. The call degrades to `invoke(name)`
(args are ignored). Structured data exchange with native engines should use the inbox
mechanism (HEP-CORE-0027) or custom framework helpers.

### 10.3 eval(code)

`eval()` always returns `{InvokeStatus::NotFound, {}}`. Code evaluation is not
applicable to compiled native engines.

---

## 11. Error Handling

### 11.1 Load-Time Errors

| Error | Severity | Behavior |
|-------|----------|----------|
| Library file not found | Fatal | Role startup aborts |
| Checksum mismatch | Fatal | Native engine not loaded; role startup aborts |
| dlopen failure | Fatal | Error logged with dlerror(); role startup aborts |
| ABI check failure | Fatal | dlclose called; role startup aborts |
| Missing native_init or native_finalize | Fatal | dlclose called; role startup aborts |
| Missing required callback | Fatal | dlclose called; role startup aborts |
| Schema string mismatch | Fatal | register_slot_type returns false; role startup aborts |
| native_init returns false | Fatal | build_api returns false; role startup aborts |

### 11.2 Runtime Errors

Native engine callbacks cannot "raise exceptions" in the script engine sense. If a
callback crashes (segfault, abort), the entire process dies. The framework provides
`ctx->set_critical_error(ctx, msg)` for the native engine to signal recoverable errors that
should trigger graceful shutdown.

**Exception across the C ABI boundary is undefined behaviour.**  Per §4.8,
visitor callbacks the plugin hands to `ctx->allowed_peers(...)` /
`ctx->band_members(...)` MUST be `noexcept`.  The host wraps every
visitor invocation in a `try/catch` as defense in depth — a buggy plugin
throwing will not corrupt the host — but plugin authors must not rely
on this safety net.  Throwing across the boundary is a plugin bug.

`script_error_count()` delegates to `RoleHostCore::script_errors()`, which the native
engine can increment indirectly via `ctx->set_critical_error(ctx, msg)`.

---

## 12. Source File Reference

| Component | File | Description |
|-----------|------|-------------|
| Native Engine C API | `src/include/utils/native_engine_api.h` | C-linkage contract, PlhNativeContext, PlhAbiInfo, macros |
| C++ convenience | `src/include/utils/native_engine_api.h` | SlotRef, ConstSlotRef, PLH_EXPORT_* macros (C++ section) |
| NativeEngine header | `src/scripting/native_engine.hpp` | ScriptEngine subclass declaration |
| NativeEngine impl | `src/scripting/native_engine.cpp` | dlopen, symbol resolution, verification, dispatch |
| Framework helpers | `src/scripting/native_engine.cpp` | Implementations backing ctx->log, ctx->report_metric, etc. |
| ScriptConfig | `src/include/utils/config/script_config.hpp` | type/path/checksum parsing, resolve_native_library |
| ScriptEngine base | `src/include/utils/script_engine.hpp` | Abstract interface, RoleContext, InvokeResult |
| Crypto utils | `src/include/utils/crypto_utils.hpp` | compute_blake2b_array for file checksum |
| Producer main | `src/producer/producer_main.cpp` | Engine selection and wiring |
| Consumer main | `src/consumer/consumer_main.cpp` | Engine selection and wiring |
| Processor main | `src/processor/processor_main.cpp` | Engine selection and wiring |

---

## 13. Cross-References

- **HEP-CORE-0011**: ScriptEngine abstraction framework (NativeEngine implements this interface; cross-engine surface table for `queue_mechanism`/`stop_reason`/`is_channel_ready`/`allowed_peers`)
- **HEP-CORE-0032**: ABI Compatibility Design — Two-Tier API Model that this HEP's §2.4 implements for the native plugin surface (Tier 1 C ABI / Tier 2 C++ wrapper)
- **HEP-CORE-0008 SS6**: Iteration metrics (NativeEngine participates in the same metrics pipeline)
- **HEP-CORE-0018 SS15**: Producer/Consumer binary architecture (engine selection logic)
- **HEP-CORE-0015 SS4**: Processor binary (engine selection, dual-queue dispatch)
- **HEP-CORE-0027**: Inbox messaging (on_inbox callback, InboxFrame schema validation)
- **HEP-CORE-0034**: Schema Registry — schema field definitions used in `PLH_DECLARE_SCHEMA` (supersedes HEP-CORE-0016). Native-plugin schemas are owned by the role that hosts the plugin (same as if the role had registered a JSON schema file); citation rules in §9.1 apply unchanged.
- **HEP-CORE-0019 SS5**: Metrics plane (ctx->report_metric integration; metrics_snapshot/metrics_get opaque pattern)
- **HEP-CORE-0035 §2**: CURVE-unconditional invariant (queue_mechanism enum codomain)
- **HEP-CORE-0036 §I11 + §6.7**: Authorized-peer + channel-state observability (allowed_peers visitor + inquiry; is_channel_ready)
- **HEP-CORE-0040**: Locked Key Memory (secret material lives here, not on the C ABI surface — see §4.8 threat model)
- **IMPLEMENTATION_GUIDANCE.md §"Error reporting: C vs C++"**: foundational rule the v6 C ABI implements (return codes; no exceptions across boundary)
- **IMPLEMENTATION_GUIDANCE.md §"Explicit noexcept"**: rule for the host-side wrappers backing every ctx fn ptr
