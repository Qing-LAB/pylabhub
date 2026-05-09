# Engine Callbacks — Complete Taxonomy + Tier 1 Implementation

## Complete callback taxonomy (three orthogonal axes)

Every callback the engine surfaces fits into a 3-axis classification.
This is the conceptual reference for future additions — pick the right
slot when adding a new callback rather than inventing a new mechanism.

### Axis 1 — Thread of caller

- **Worker-only** — caller is the engine's owner thread (the thread
  that ran `initialize()`, holds the language runtime's lock by
  construction).  Hot-path performance via cached engine-specific
  handles.
- **Cross-thread routed** — caller may be any thread; the engine
  internally checks `std::this_thread::get_id() == owner_thread_id_`
  and either executes directly (owner) or queues + waits via the
  existing request-queue + future infrastructure.  No external
  caller-side thread checking required.
- **Any-thread query** — pure read of engine state (presence, status).
  Backed by a load-time cache or atomic primitive; no language-runtime
  touch on the read path.

### Axis 2 — Argument shape

- **Lifecycle** — `(api)` only.
- **Typed cycle** — `(tx/rx, msgs, api)` with zero-copy slot views; bound
  per role for hot-path performance.
- **Inbox** — `(msg, api)` with binary buffer.
- **Periodic** — `(api)` plus engine-side scheduling state.
- **Augment** — `(args_dict, response_dict) -> dict` (or null = no
  augment).  Generic by-name; uses JSON for marshalling.

### Axis 3 — Return discipline

- **Void** — lifecycle, periodic.
- **Bool** — cycle ops (commit / discard).
- **InvokeResult enum** — inbox (Commit / Drop / Error).
- **Dict / JSON** — augment hooks.

### Mapping every callback to the matrix

| Callback name | Axis 1 (thread) | Axis 2 (shape) | Axis 3 (return) | Engine surface |
|---|---|---|---|---|
| `on_init` | Worker-only | Lifecycle | Void | `invoke_on_init()` |
| `on_stop` | Worker-only | Lifecycle | Void | `invoke_on_stop()` |
| `on_produce` | Worker-only | Typed cycle | Bool | `invoke_produce(tx, msgs)` |
| `on_consume` | Worker-only | Typed cycle | Bool | `invoke_consume(rx, msgs)` |
| `on_process` | Worker-only | Typed cycle | Bool | `invoke_process(rx, tx, msgs)` |
| `on_inbox` | Worker-only | Inbox | InvokeResult | `invoke_on_inbox(msg)` |
| `on_heartbeat` | Cross-thread (ctrl) | Periodic | Void | `invoke("on_heartbeat")` |
| `on_query_metrics` | Cross-thread (admin) | Augment | Dict | `invoke_returning("on_query_metrics", args, timeout)` |
| `on_list_roles` | Cross-thread (admin) | Augment | Dict | `invoke_returning("on_list_roles", …)` |
| `on_get_channel` | Cross-thread (admin) | Augment | Dict | `invoke_returning("on_get_channel", …)` |
| `on_peer_message` | Cross-thread (admin) | Augment | Dict | `invoke_returning("on_peer_message", …)` |
| `has_callback` (presence query) | Any-thread | n/a | Bool | `has_callback(name)` — Tier 1 cache |

### Three rules for future callbacks

1. **Worker-only** → typed C++ method on `ScriptEngine`; engine caches
   handle at load time; method runs callback synchronously.

2. **Cross-thread routed** → reuse `invoke(name)` / `invoke_returning(name, args, timeout)` —
   the engine's existing routing handles the thread check + queue.

3. **Any-thread query** → back it with a Tier 1 cache (load-time
   populate; lock-free read).  **Never** touch `py::object` /
   `lua_State` on this path.  Annotate `THREAD-SAFETY: any thread` on
   the doc comment.

The bug fixed in this revision (a `has_callback` implementation that
silently touched pybind11) was a violation of rule (3).  Tier 1
formalises that rule with a base-class cache + subclass hook for
arbitrary-name probing on the worker.

---

# Tier 1 Implementation: Standard vs Dynamic Callback Sets

**Status:** Tech draft. Tier 1 (standard) is implemented; Tier 2 (dynamic)
is reserved by the `ScriptEngine::supports_dynamic_callbacks()` capability
flag — false on every engine today; ship implementation when a concrete
consumer materialises.

**Promote to:** HEP-CORE-0011 §"Engine Thread Affinity" + §"Callback
Tiers — Standard vs Dynamic" once Tier 2 ships and the design is
validated against a real use case.

---

## Motivation

In May 2026 a long-running L4 round-trip test (`PlhHubCliTest.RoundTrip_PlhHubKeygenAndRunPlhRoleRegisters`)
started failing deterministically with a pybind11 `inc_ref()` GIL
violation. The gdb stack trace pointed at:

```
PythonEngine::has_callback("on_heartbeat")
  ↑ called from RoleAPIBase::on_heartbeat_tick_
    ↑ which runs on the broker control thread
      (BrokerRequestComm::run_poll_loop)
```

The control thread does NOT hold the Python GIL. Inside
`PythonEngine::has_callback`, the implementation constructed a
`py::none()` sentinel and read the cached `py::object` callback
member — both pybind11 operations that require the GIL. pybind11's
debug build asserts the GIL is held and printed a violation; in the
broader process this corrupted refcounts and propagated to a `zmq_close`
abort during shutdown.

**Root cause: `has_callback` was *intended* to be any-thread-safe (its
callers run on multiple non-worker threads) but its *implementation*
silently touched language-runtime state.** The contract was implicit
and unenforced.

## Two-Tier Design

### Tier 1: Standard callbacks (this fix)

A small fixed set of names, defined by pylabhub's inter-component
protocol and rarely changing:

| Name | Used by | Shape |
|---|---|---|
| `on_init` | role host worker_main_, hub script runner | `(api) -> None` |
| `on_stop` | role host teardown, hub script runner | `(api) -> None` |
| `on_produce` | producer cycle ops | `(tx, msgs, api) -> bool` |
| `on_consume` | consumer cycle ops | `(rx, msgs, api) -> bool` |
| `on_process` | processor cycle ops | `(rx, tx, msgs, api) -> bool` |
| `on_inbox` | inbox dispatcher | `(msg, api) -> None` |
| `on_heartbeat` | broker comm periodic tick (control thread) | `(api) -> None` |
| `augment_query_*` | hub admin RPC augmentation | `(api, response) -> dict` |

**Discovery + caching:** Each engine's `load_script()` discovers each
name (`getattr(module, name, None)` for Python; the equivalent for Lua)
ONCE, on the worker thread under the runtime's lock (GIL / Lua state
mutex). The result is stored as a plain `bool` in an
engine-internal map.

**`has_callback(name)`:** reads the map. No language-runtime access.
Safe from any thread.

**Invocation:** the engine exposes typed methods (`invoke_on_init`,
`invoke_produce`, …) that retain their cached engine-specific handles
(`py_on_init_`, Lua refs) for hot-path performance. Per-frame cycle
ops do NOT pay any extra map lookup.

This tier is what fixes the L4 GIL violation and is the only change
shipped in this revision.

### Tier 2: Dynamic callbacks (reserved, capability-gated)

Some future use cases want scripts to register additional callbacks at
runtime — e.g. an admin RPC that dispatches to a custom Python handler
the script registered, or a hot-loaded extension that registers its own
event hooks. These extend the standard set with arbitrary names.

**Capability flag (in place now):**

```cpp
class ScriptEngine {
    // Default false; engines opt in by overriding.
    [[nodiscard]] virtual bool supports_dynamic_callbacks() const noexcept;
};
```

Callers that want Tier 2 features check the flag and degrade gracefully
to "feature unavailable" when it returns false.

**Engines today:** every implementation (`PythonEngine`, `LuaEngine`,
`NativeEngine`) returns the default `false`. No script-side
`register_callback` binding is exposed. Tier 2 surface is reserved,
not built.

**Bounded callback shapes (when Tier 2 ships):**

| Kind | Script signature | Purpose | Return |
|---|---|---|---|
| `Event` | `def cb(api, args_dict): ...` | Fire-and-forget notifications | None |
| `Query` | `def cb(api, args_dict) -> dict: ...` | Synchronous request/response | dict |

Cycle ops stay typed (separate methods, not generic) for hot-path
zero-copy performance — they don't need the dynamic surface.

**C++ invocation surface (when Tier 2 ships):**

```cpp
// any thread; queues to worker if off-worker
void invoke_event(const std::string &name, const nlohmann::json &args);

// any thread; queues to worker if off-worker; returns result
InvokeResponse invoke_query(const std::string &name,
                            const nlohmann::json &args,
                            int64_t timeout_ms);
```

The existing `invoke_returning(name, args, timeout)` is essentially
the Query primitive — when Tier 2 ships, `invoke_query` is its
preferred name.

**Script-side binding (Python first, when Tier 2 ships):**

```python
def my_event_handler(api, args):
    api.report_metric("custom_count", args["delta"])

def on_init(api):
    api.register_callback("custom_event", my_event_handler, kind="event")
```

`api.register_callback(name, fn, kind)` writes to the engine's
dynamic-callback registry. `unregister_callback(name)` removes.
`has_callback(name)` consults the registry IF the engine supports it,
otherwise checks Tier 1 only.

**Why Lua is not in Tier 2 by default:** Lua state mutex semantics
plus the absence of a current consumer make it premature scope. The
flag flip + LuaEngine implementation are mechanical when needed; they
can ship in a focused PR at that point.

## Thread-affinity contract (the broader principle)

This bug exposed a documentation gap: `ScriptEngine`'s virtual surface
mixed methods with different thread-safety guarantees without
distinguishing them. The fix going forward is to annotate every
virtual method with a `THREAD-SAFETY:` line in its doc comment:

- **`THREAD-SAFETY: any thread`** — the method may be called from any
  thread without acquiring the language runtime's lock. Implementations
  must NOT touch GIL-required (Python) or lua_State-required (Lua)
  state. Examples: `has_callback`, `script_error_count`,
  `pending_script_engine_request_count`, `supports_multi_state`,
  `supports_dynamic_callbacks`.

- **`THREAD-SAFETY: worker only (runtime lock required)`** — the
  method must be called on the engine's owner thread (the thread that
  ran `initialize()`) which holds the runtime lock. Examples:
  `load_script`, `register_slot_type`, `invoke_*` (typed cycle ops
  use the cross-thread queue but the QUEUED execution is worker-only),
  `finalize_engine_`.

- **`THREAD-SAFETY: any thread (cross-thread routing)`** — the method
  is callable from any thread; its implementation routes to the worker
  via the engine's request queue. Examples: `invoke_returning`.

`has_callback` is the canonical "any thread" method whose
implementation MUST honour the constraint. The Tier 1 cache is the
mechanism by which it does.

## Test coverage added

- `LifecycleDynamicTest.SyncShutdown_*` — for the `set_synchronous_shutdown`
  framework primitive (see HEP-CORE-0001 + HEP-CORE-0011 sync teardown).
- L4 `PlhHubCliTest.RoundTrip_PlhHubKeygenAndRunPlhRoleRegisters` —
  becomes the regression guard for Tier 1: if a future change
  reintroduces a non-thread-safe `has_callback` implementation, this
  test will fail again.

A future Tier 2 PR should add focused unit tests for the
`supports_dynamic_callbacks` flag behaviour (false case: registration
binding raises; true case: round-trip register → invoke → unregister).

## Migration / Followups

When a real Tier 2 consumer materialises:

1. Open a focused PR that:
   - Implements `register_callback` / `unregister_callback` on
     `PythonEngine` (registry + py::object handle map).
   - Adds `api.register_callback(name, fn, kind)` Python binding.
   - Implements `invoke_event(name, args)` and renames/aliases
     `invoke_returning` → `invoke_query`.
   - Flips `PythonEngine::supports_dynamic_callbacks()` to `true`.
2. Promote this tech_draft into HEP-CORE-0011 §"Callback Tiers" with
   the as-shipped semantics; archive this draft.
3. Add Tier 2 tests as outlined above.
