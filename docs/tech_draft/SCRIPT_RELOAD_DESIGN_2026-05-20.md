# Script Reload Design — `api.request_reload`

**Status:** tech_draft, design only — no implementation in tree yet.
Captured 2026-05-20 from a discussion about whether `ScriptEngine::
reload_script()` (which exists as a virtual returning `false`) is dead
code or a real reserved feature.  Decision: real feature; needs explicit
state-machine guard + cleanup hooks + per-engine implementation.  Slot
this design when prioritised.

## 1. Motivation

Lab-day scenario: an experimenter has a running plh_role (Python or Lua)
that's accumulated state — open files, in-memory buffers, broker
registrations — and wants to tweak the script logic without losing that
state.  Restarting the role drops the broker connection, breaks the
pipeline, and forces re-handshake of every consumer.  A live reload
preserves the C++ side (queues, BRC, slot views, FFI types) and rebinds
ONLY the script's callback function objects.

Out-of-scope today (deferred to a follow-up HEP if/when needed):

- Cross-role reload coordination (e.g., reload producer + consumer in
  sync).
- Hot reload of native (`.so`) plugins.
- Reload triggered from outside the role (admin RPC, file-system
  watcher, SIGUSR1).

## 2. Core principle — when is reload safe?

Reload is safe only when:

1. The engine is in a steady serving state (`ApiBuilt`, accepting=true).
2. There is no `invoke_*` currently in flight.
3. The data loop is at a cycle boundary (between `invoke_produce` /
   `invoke_consume` / `invoke_process` calls).
4. The script author has had a chance to run their `on_stop` cleanup
   before the script body is swapped.

The cycle ops (`ProducerCycleOps::invoke_and_commit` etc.) own the cycle
boundary; the reload is performed there.

## 3. State machine extension

Current `EngineState` (in `script_engine.hpp`):

```
Unloaded → Initialized → ScriptLoaded → ApiBuilt → Finalized
```

Add one transient state:

```
Unloaded → Initialized → ScriptLoaded → ApiBuilt → Finalized
                                          │  ▲
                                          │  │
                                          ▼  │
                                       ReloadPending
```

The `ReloadPending` state is held only for the duration of the reload
(milliseconds typically — re-execute the script file + re-extract
callback refs).  During `ReloadPending`:

- `accepting_` is forced to `false` (no new `invoke_*` accepted from
  other threads).
- The script is between its old `on_stop` and new `on_init` calls.

After successful reload: transition back to `ApiBuilt`, set
`accepting_=true`.  After failed reload: transition back to `ApiBuilt`
with the OLD callback refs intact, set `accepting_=true`, surface the
error via `last_reload_status_`.

## 4. Trigger API — script-initiated only (this design)

Script-facing surface, exposed via `RoleAPIBase`:

```cpp
class RoleAPIBase
{
public:
    /// Request a script reload.  Returns immediately; the actual
    /// reload happens at the next data-loop boundary.
    ///
    /// @param new_path  If empty, reuses the path originally loaded by
    ///                  `load_script()`.  Otherwise loads a different
    ///                  file path (e.g., a hotfix script staged
    ///                  alongside the original).
    /// @return  true if request accepted (engine in ApiBuilt state,
    ///          no other reload pending).
    ///          false if rejected — engine not in a reloadable state.
    bool request_reload(const std::string &new_path = "") noexcept;

    /// Outcome of the most recent reload attempt.  Populated by the
    /// cycle ops when the pending reload completes (or fails).
    /// Reset to `Pending` when a new request_reload is issued.
    enum class ReloadStatus
    {
        None,                     // no reload ever attempted
        Pending,                  // request_reload accepted; not yet executed
        Ok,                       // last reload succeeded
        FileNotFound,             // script file missing
        ParseError,               // syntax/load error
        MissingRequiredCallback,  // post-reload, required cb (on_produce etc.) is gone
        NotAllowed,               // engine state wasn't ApiBuilt
    };
    ReloadStatus last_reload_status() const noexcept;
};
```

The script invokes `api.request_reload()` (e.g., from a custom band
callback, a periodic check, or after receiving a sentinel message),
then schedules whatever it wants to happen next.

## 5. Cleanup contract — what the script owns vs what the framework owns

| Concern | Framework | Script |
|---|---|---|
| Re-execute the script file | ✓ | — |
| Re-extract callback function refs | ✓ | — |
| Re-cache `set_standard_callback_present` | ✓ | — |
| Verify required callback still present | ✓ | — |
| Preserve C++ side (queues, slot views, API table, FFI types) | ✓ | — |
| Call old script's `on_stop()` before reload | ✓ | — |
| Call new script's `on_init()` after reload | ✓ | — |
| Flush persistent state (counters, accumulated data, log files) | — | ✓ via `on_stop` |
| Re-create per-script state | — | ✓ via `on_init` |
| Manage Python module instances of reloaded classes | — | ✓ (known Python reload pitfall) |

Script-author note: writing your role's `on_stop` and `on_init` to be
**reload-safe** is the same discipline as writing them to be
**restart-safe**.  If your role survives a process restart cleanly, it
survives a reload cleanly.

## 6. Cycle-ops integration

```cpp
// Pseudocode added to ProducerCycleOps::invoke_and_commit (similar in
// ConsumerCycleOps / ProcessorCycleOps).
bool invoke_and_commit(std::vector<IncomingMessage> &msgs)
{
    // Step 1: cycle boundary — check for pending reload request.
    if (engine_.has_pending_reload())
    {
        engine_.perform_pending_reload();   // see §7
        // No `invoke_produce` call this cycle — reload happened in this
        // slot.  Resume normal serving next cycle.
        return true;   // commit nothing
    }

    // Step 2: normal dispatch + invoke (unchanged).
    dispatch_notifications(engine_, msgs, StopRequestor{core_});
    // ...
}
```

`engine_.perform_pending_reload()` does (in order):

1. Set `state_ = ReloadPending` + `accepting_ = false`.
2. Call `invoke_on_stop()` on the OLD script.
3. Per-engine: re-execute file + re-bind callback refs (`reload_script_(path)`).
4. If reload returned false: revert (state back to `ApiBuilt`, `accepting_=true`),
   set `last_reload_status_` to the failure code, LOGGER_ERROR, return.
5. Re-`set_standard_callback_present` cache for every standard callback name.
6. Check required callback presence; if missing, treat as failure (revert).
7. Call `invoke_on_init()` on the NEW script.
8. Set `state_ = ApiBuilt`, `accepting_ = true`, `last_reload_status_ = Ok`.

## 7. Per-engine implementation

### PythonEngine

```cpp
bool PythonEngine::reload_script_(const std::string &new_path) override
{
    py::gil_scoped_acquire g;
    try
    {
        // If new_path differs, swap which path the next reload uses.
        if (!new_path.empty()) script_path_ = new_path;

        // Detach OLD callback refs so dec_ref doesn't race the reload.
        // (Same pattern as `clear_pyobjects_`.)
        // ... release_to_none(py_on_init_); etc. ...

        // Re-execute the module.
        module_ = py::module_::import(script_module_name_.c_str());
        module_.reload();   // py::module_::reload() wraps importlib.reload

        // Re-extract all callback refs (mirrors `load_script` block).
        py_on_init_ = py::getattr(module_, "on_init", py::none());
        // ... etc for every py_on_*_ ...

        return true;
    }
    catch (py::error_already_set &e)
    {
        on_python_error_("reload_script", e);
        return false;
    }
}
```

### LuaEngine

```cpp
bool LuaEngine::reload_script_(const std::string &new_path) override
{
    if (!new_path.empty()) script_path_ = new_path;

    // Release OLD callback refs from the registry.
    clear_refs_();   // already exists; releases every ref_on_*_

    // Re-load + execute the file.
    if (luaL_loadfile(state_.raw(), script_path_.c_str()) != LUA_OK ||
        lua_pcall(state_.raw(), 0, 0, 0) != LUA_OK)
    {
        const char *msg = lua_tostring(state_.raw(), -1);
        LOGGER_ERROR("[{}] reload_script: load/exec failed: {}", log_tag_, msg);
        lua_pop(state_.raw(), 1);
        return false;
    }

    // Re-extract all callback refs (mirrors `load_script` block).
    ref_on_init_ = extract_callback_ref_("on_init");
    // ... etc ...

    return true;
}
```

### NativeEngine

Not feasible — native plugins are `.so` files.  Hot reload would require
`dlclose` + `dlopen` AND the plugin would need a careful state-export
+ -import protocol.  Out of scope.

```cpp
bool NativeEngine::reload_script_(const std::string & /*new_path*/) override
{
    LOGGER_WARN("[{}] reload_script: native engine does not support "
                "live reload (would require dlclose+dlopen with plugin "
                "state migration; restart the role instead)",
                log_tag_);
    return false;
}
```

## 8. Test plan

L2 (mock-engine + cycle ops):
- `RequestReload_AcceptedInApiBuilt_FlagSet`
- `RequestReload_RejectedInUnloaded_Returns_false`
- `RequestReload_RejectedInReloadPending_OneAtATime`
- `CycleBoundary_DetectsPendingReload_PerformsReload`
- `Reload_Failure_RevertsToOldCallbacks`
- `Reload_MissingRequiredCallback_TreatedAsFailure`

L3 per engine (real Python + real Lua scripts on disk):
- `PythonEngine_ReloadScript_NewCallbackBodyInvoked`
- `LuaEngine_ReloadScript_NewCallbackBodyInvoked`
- `NativeEngine_ReloadScript_ReturnsFalse_WarningLogged`

L4 (live role + script file modified on disk):
- `Producer_ReloadScript_PreservesQueueState`

## 9. Scope estimate

| Piece | Time |
|---|---|
| State-machine plumbing (ReloadPending + flags) | ~2h |
| `RoleAPIBase::request_reload` + status enum + binding | ~1.5h |
| Cycle-ops integration (3 sites) | ~1.5h |
| PythonEngine impl | ~3h |
| LuaEngine impl | ~3h |
| NativeEngine warn-only stub | ~30min |
| L2 tests | ~2h |
| L3 per-engine tests | ~3h |
| HEP-CORE-0011 §"Script Reload" amendment | ~1h |
| **Total** | **~1.5 days** |

## 10. Risks / unknowns

- **Python `importlib.reload` pitfalls.** Module-scope class identity
  changes; instances created BEFORE reload retain the OLD class.
  Scripts that hoard instances cross-reload will silently misbehave.
  Mitigation: docstring warning + a test that calls reload + verifies
  a new instance of a reloaded class is of the new type.
- **Lua upvalue capture.** Closures from the old script are no longer
  reachable after reload (their global function names get rebound, but
  any C++-held `ref_on_*_` pointed to the OLD closure object).  Our
  pattern of holding registry refs to GLOBAL function names + re-
  extracting on reload sidesteps this.  Document.
- **Script-side `request_reload` calling itself in `on_init`.** Infinite
  loop risk.  Detect: reject `request_reload` if currently inside
  `on_init` or `on_stop` (i.e., engine state `ReloadPending`).
- **Reload during `on_consumer_died` / `on_band_*` typed-callback
  dispatch.** Typed callbacks fire via `dispatch_notifications` at
  cycle start — before the reload-check.  Solution: move the reload
  check BEFORE `dispatch_notifications` so a pending reload is honoured
  before the typed-callback batch fires.  (OR move it after — but
  before-is-simpler.)

## 11. Decision log

- 2026-05-20: Tech_draft created.  Implementation deferred — captured
  here so the design isn't lost.  Task #76 tracks promotion.
