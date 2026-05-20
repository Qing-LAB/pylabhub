# Discovery & Open Issues — 2026-05-20

**Purpose:** persistent capture of findings from the post-band-authority code
review + dead-code sweep, before context compaction loses them.  Each
section ends with a decision-status line (DECIDED / OPEN / DEFERRED).

## 1. Naming-collision incident (the trigger)

`src/include/utils/role_host_lifecycle.{hpp,cpp}` (Phase 5c, commit `e9a79bc`)
used the word "lifecycle" for the role-host worker_main_() epilogue — a
direct collision with the framework `Lifecycle` module
(`src/include/utils/lifecycle.hpp`, the module-init/teardown registry).
Two unrelated subsystems shared one word.  User flagged as repeat-violation
territory.

**Resolution:** commit `ac7d5c1` (2026-05-20) — split the file:
- `make_broker_comm_config()` → deleted (dead code, see §2.1).
- `do_role_teardown()` → moved into `src/utils/service/engine_host.cpp`
  with declaration in `src/include/utils/engine_host.hpp`.  Docstring
  scrubbed to remove obsolete legacy-mode references.
- `role_host_lifecycle.{hpp,cpp}` deleted.

**Memory rule added:** `feedback_name_collisions_and_dead_code.md` —
grep before naming any new helper/class/header; every code review MUST
include a dead-code pass.

**Status:** DECIDED + IMPLEMENTED.

## 2. Dead-code sweep findings

Agent-driven scan across role-host / engine / hub-broker surfaces.
Methodology: per-file public-symbol enumeration → grep across `src/` +
`tests/` + `examples/` → flag symbols with zero non-self callers.

### 2.1  Confirmed dead — already removed

- `make_broker_comm_config()` (`role_host_lifecycle.hpp`) — deleted in
  commit `ac7d5c1`.  Wave-B M5 (commit `7ac9aa2`) moved BRC config
  construction into `RoleHandler`, leaving this helper stranded.

### 2.2  Confirmed dead — DELETED 2026-05-20

- `ScriptEngine::lifecycle_validate` + `lifecycle_key_` + `lifecycle_magic_`
  + `kLifecycleMagic` (`script_engine.hpp:154-170`) — leftover from a
  pre-HEP-0011-Option-E design where `ScriptEngine` itself was a
  Lifecycle-registered module.  The 2026-05-08 redesign moved engine
  ownership to the worker thread; these members were orphaned.  Block
  also overloaded the "Lifecycle" word.  ~18 LOC removed.

### 2.3  Reserved / intentional — KEEP

- **`ScriptEngine::reload_script()` (#2)** — legitimate reserved feature.
  Full design in §4 below.  Promoted to its own tech_draft
  (`SCRIPT_RELOAD_DESIGN_2026-05-20.md`).
- **`ScriptEngine::supports_dynamic_callbacks()` (#3)** — Tier-2
  capability marker.  Backing design lives in
  `docs/tech_draft/engine_callback_tiers.md`.  Per 2026-05-20 decision:
  default flipped to throw "not implemented" (no longer silent `false`),
  PythonEngine gets minimal impl, LuaEngine + NativeEngine keep the
  stub.  See §5 below.

### 2.4  Open — awaiting user decision

- **`RoleAPIBase::close_all_inbox_clients()`** (`role_api_base.hpp:336` +
  `role_api_base.cpp:1797`).  Zero callers anywhere.  `~Impl` and
  `RoleHostCore::clear_inbox_cache` handle teardown.  Need to verify
  whether scripts call `api.close_all_inbox_clients()` via reflection.
  Status: pending discussion.
- **`BrokerRequestComm::query_shm_info()`** + **`BrokerService::collect_shm_info_json()`**
  (`broker_request_comm.cpp:925` + `broker_service.cpp:3932`).  Paired —
  client + server.  Zero callers.  Need to check whether `SHM_INFO_REQ`
  wire frame is still listed in the broker's dispatch table.  If the
  wire frame is also dead, both can go.  Status: pending discussion.
- **`ChannelSnapshot::count_by_observable()`** (`broker_service.hpp:77-83`).
  Filter helper.  Zero callers.  Verify exposure to admin queries.
  Status: pending discussion.

## 3. Code-review findings still open (from review reports of 2026-05-19)

### 3.1  D1 must-fix

- **A1 — `ctx_band_leave` semantic bug.**
  `src/utils/service/native_engine.cpp:280`:
  ```cpp
  int ctx_band_leave(const PlhNativeContext *ctx, const char *channel)
  {
      if (!ctx || !ctx->_api || !channel) return 0;
      return static_cast<RoleAPIBase *>(ctx->_api)->band_leave(channel) ? 1 : 0;
  }
  ```
  Returns `1` if `band_leave` returns ANY value, including the broker's
  typed `{status:error, NOT_A_MEMBER}` response.  Native plugins see a
  rejected leave as success.  Fix: gate on `result->value("status","") == "success"`.
  Status: OPEN, single-line fix.

### 3.2  D2 drift

- **B1 — empty `correlation_id` in BAND_JOIN/LEAVE validator errors.**
  `broker_service.cpp:4488,4584`.  `handle_band_join_req` and
  `handle_band_leave_req` pass `corr_id=""` to the validator even
  though the request payload carries `correlation_id`.  Other handlers
  (e.g., DEREG_REQ at line 1805) thread the real value through.  Fix:
  read `req.value("correlation_id","")` at handler entry.  Status: OPEN.
- **Stale comments in `role_api_base.{hpp,cpp}`** mentioning the now-
  deleted `set_broker_comm` / `start_ctrl_thread` / `pImpl->broker_channel`.
  Identified sites: `role_api_base.hpp:462-468`, `role_api_base.cpp:75-76,
  556, 870-877, 1264-1269, 1290-1292`.  Plus:
  - `hub_script_runner.cpp:131,219` references `start_ctrl_thread`.
  - 3 role hosts: lines 261/320/473, 249/424/445, 279/538 — references
    to "legacy start_ctrl_thread."
  - 3 role hosts: file-header at line 8 lists "Layer 3 = BrokerRequestComm…"
    but BRC is now owned by `RoleHandler`.
  - `engine_host.hpp:268` inconsistency between `RoleHostBase::shutdown_`
    and `EngineHost::shutdown_` in the same docstring.
  - `role_host_helpers.hpp:53-55` says "(legacy)" branch that's gone.
  - `role_host_core.hpp:369` "Teardown timeline (role_host_lifecycle.cpp + …)"
    — file is gone.
  Status: OPEN, ~15-site mechanical scrub.
- **Obsolete includes**: `#include "utils/broker_request_comm.hpp"` in
  three role hosts (line ~22) — BRC now reaches them transitively via
  `role_handler.hpp`.  Status: OPEN, mechanical.

### 3.3  Test-coverage gaps (priority order)

1. **Gate-mutation tests** for the `expected_tags` argument in every
   gate-validator call site EXCEPT REG_REQ and CONSUMER_REG_REQ (which
   already have `Gate_RegReq_*` and `Gate_ConsumerRegReq_*`).  Missing:
   - DEREG_REQ — expects `{prod, proc}` tag set.
   - CONSUMER_DEREG_REQ — expects `{cons, proc}`.
   - HEARTBEAT_REQ — tag derived from `role_type` field; mismatch path
     not pinned.
   - ROLE_INFO_REQ — expects `{prod, cons, proc}`.
   - ROLE_PRESENCE_REQ — same.
   - BAND_JOIN_REQ — expects `{prod, cons, proc}`.
   - BAND_LEAVE_REQ — same.
   - BAND_BROADCAST_REQ — same.
   A typo in any `expected_tags` initializer compiles silently today.
2. **S4 broker tests** missing:
   - `BAND_BROADCAST_REQ` sender-not-member drop (`broker_service.cpp:
     4685-4697`).
   - `BAND_LEAVE_REQ` `NOT_A_MEMBER` typed-error path (`broker_service.cpp:
     4605-4622`).
   - `BAND_BROADCAST` band-doesn't-exist drop (`broker_service.cpp:
     4677-4684`).
3. **S4 role-side bookkeeping tests** missing:
   - `band_join` on `{status:error}` → erase index entry
     (`role_api_base.cpp:1551-1560`).
   - `band_leave` on `{status:error}` → erase index entry
     (`role_api_base.cpp:1597-1605`).
   - `mark_connection_disconnected` band_index_ sweep returning
     `bands_lost` (`role_handler.cpp:337-349`).
4. **L3 hub-dead → on_band_lost cascade test** — verifies the
   `role_api_base.cpp:1166-1176` enqueue from hub-dead path actually
   fires `on_band_lost` callbacks end-to-end.  Explicitly out of scope
   per commit `40701da` but tracked here.
5. **Real-engine parity tests** for the 4 band callbacks (Python/Lua/
   Native).  D1+D2 had `Dispatcher_RealLuaEngine_RecordsArgs` for
   `on_channel_closing`; nothing analogous exists for bands or for
   `on_hub_dead`.
6. **`api.is_in_band()`** — untested at any layer.

Status: ALL OPEN.  Suggest one focused test-add commit covering items
1–3 (the broker-protocol surfaces); items 4–6 a separate commit each.

## 4. `reload_script` — design extract

(Full standalone tech_draft at
`docs/tech_draft/SCRIPT_RELOAD_DESIGN_2026-05-20.md`.)

**Core principle:** reload is safe only between data-loop cycles, when no
`invoke_*` is in flight.  Triggered by `api.request_reload(path)` —
script-initiated only for now (no admin RPC or file watcher yet).
Engine state machine extended with `ReloadPending` transient state.

**Cleanup contract:** the framework calls old script's `on_stop()`, then
re-loads the file, then new script's `on_init()`.  Script author owns
per-script state across reload.

**Failure mode:** parse error / missing required callback → reject
reload, keep old script.  LOGGER_ERROR + `last_reload_status_`.

**Per-engine implementation feasibility:**
- PythonEngine: `importlib.reload(module_)` under GIL + re-`getattr` all `py_on_*_`.
- LuaEngine: `luaL_loadfile + lua_pcall` + re-`extract_callback_ref_`.
- NativeEngine: not feasible (would require `dlclose+dlopen`); override returns false + WARN.

**Status:** DECIDED — tech_draft only; no implementation now.  Task #76.

## 5. `supports_dynamic_callbacks` — Tier 2 — execution plan

**Decision (2026-05-20):**

- Default in `ScriptEngine` flips from silent `return false` to
  **throwing an exception** with "not implemented" — makes accidental
  reliance on a non-implemented Tier-2 surface loud, not silent.
- **PythonEngine: full minimal implementation** if simple.  Registry
  (`std::map<std::string, py::object>`) + `register_callback(name,fn)`
  pybind binding + `invoke_event` / `invoke_query` bridges (under GIL).
- **LuaEngine: stub override that throws "not implemented".**  Lua's
  per-thread state model means a callback registered in one thread's
  state isn't visible from others; a proper implementation requires
  either replication or a dedicated registry thread — deferred.
  Documented constraint in the override.
- **NativeEngine: stub override that throws "not implemented".**  Plugin
  ABIs typically manage their own dynamic dispatch; not a framework
  concern.  Documented.

**HEP work:** promote `docs/tech_draft/engine_callback_tiers.md` content
into HEP-CORE-0011 §"Callback Tiers — Standard vs Dynamic" (ratified).
Archive the tech_draft per `docs/DOC_STRUCTURE.md §2.2`.

**Status:** PARTIAL.  Stubs + Python impl pending under task #77.

## 6. Other observations from the band-authority work

- The `RoleState::Disconnected` enum value still has defensive readers
  at `hub_state.hpp:1003` and `broker_service.cpp:1698`, even though
  H18 (Wave-M3 step 5h) made tombstones get erased rather than marked
  Disconnected.  Defensive checks are inexpensive and survive future
  regressions; KEEP but consider adding `assert(!"unreachable")` plus
  a comment in debug builds.
- `RoleAPIBase::stop_ctrl_for_teardown()` has exactly one caller
  (`engine_host.cpp:385`) but the standalone method documents the
  "non-destructive signal" intent.  KEEP.

## 7. Doc bookkeeping debt

- `docs/HEP/HEP-CORE-0019-*` Phase 6 amendment (per-producer metrics
  tree shape) is the Wave-M2.5 step-8 carryover — Task #73 ("HEP-0033
  Phase 10 doc closure").  Still open.
- `docs/code_review/REVIEW_*WaveM3*.md` (4 files from 2026-05-11) are
  archive candidates — their open items were verified resolved during
  the 2026-05-19 review-triage pass but the docs haven't been moved
  to `docs/archive/transient-2026-05-11/`.

Status: OPEN, low priority, can ride with the next bookkeeping commit.
