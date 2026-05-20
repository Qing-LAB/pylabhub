# Hub State Query Layer — Layered Snapshot + Query Design

**Status:** tech_draft, design only — no implementation in tree yet.
**Captured:** 2026-05-20 during Group 2 dead-code review (candidate #3
walkthrough surfaced the bigger architectural question).
**Tracking:** add to `docs/TODO_MASTER.md` § "Deferred polish" /
"Audit follow-ups".  Will subsume Group 2 candidates #2
(`query_shm_info` / `collect_shm_info_json`) and #3
(`count_by_observable`) wiring.

---

## 1. Motivation

Every HubAPI read accessor today takes its own internal snapshot of
hub state.  Concrete evidence in `src/utils/service/hub_api.cpp`:

| Method | Snapshot site |
|---|---|
| `list_channels()` | line 243: `impl_->host->state().snapshot()` |
| `get_channel(name)` | line 263: `impl_->host->state().snapshot()` |
| `list_roles()` | line 276: `impl_->host->state().snapshot()` |
| `list_bands()` | line 296: `impl_->host->state().snapshot()` |
| `list_peers()` | line 316: `impl_->host->state().snapshot()` |
| `query_metrics(filter)` | delegates to broker; 10+ snapshot sites in `broker_service.cpp` |

For the *single-aspect, one-shot* query pattern this is fine — each
call gets a coherent view of its own concern.

For the *multi-aspect coherent-view* pattern this is broken:

- "Is everything live yet?" health check needs channels + roles +
  peers to all agree → 3 snapshots → 3 different moments → views may
  disagree (a role registered between snapshots #1 and #2 appears in
  snapshot #2 but is missing as a channel-producer in snapshot #1).
- "Distribution of channel states" → 4 calls to a count helper → 4
  snapshots — counts don't sum coherently to the total.
- Periodic telemetry / forensics dumps → multi-snapshot churn for
  what should be a single frozen capture.
- Reconfiguration triggers ("if half consumers stalled, do X") → 2+
  snapshots for what should be one decision.

The fix: **decouple capture from query.**  Capture is the expensive,
locked, deep-copy operation.  Query is the cheap, pure, lock-free
operation over a captured value.

## 2. Three-layer design

### Layer 1 — Capture

One operation, one shared_lock, one deep-copy, one self-describing
result.

```cpp
namespace pylabhub::hub {
    struct HubStateSnapshot {
        // ── Data (existing containers) ──────────────────────────────
        std::unordered_map<std::string, ChannelEntry>  channels;
        std::unordered_map<std::string, RoleEntry>     roles;
        std::unordered_map<std::string, BandEntry>     bands;
        std::unordered_map<std::string, PeerEntry>     peers;
        std::unordered_map<std::string, ShmBlockRef>   shm_blocks;
        std::unordered_map<std::string, SchemaEntry>   schemas;
        BrokerCounters                                 counters;

        // ── Metadata ────────────────────────────────────────────────
        std::chrono::system_clock::time_point captured_at;    // EXISTS — wall-clock
        std::chrono::steady_clock::time_point captured_mono;  // NEW — monotonic for age math
        std::string                            hub_uid;        // NEW — which hub produced it
        uint64_t                               snapshot_seq{0};// NEW — monotonic per hub
    };

    class HubState {
        HubStateSnapshot snapshot() const;  // shared_lock + deep copy + metadata
    };
}
```

**Metadata field justifications:**

| Field | Why |
|---|---|
| `captured_at` | Wall-clock for human-readable logs / forensic timestamps |
| `captured_mono` | Steady-clock so `age_seconds()` is immune to NTP jumps and monotonic |
| `hub_uid` | Disambiguates dual-hub-processor snapshots; logs are unambiguous |
| `snapshot_seq` | Detect missed snapshots; debugging; "did I lose a frame?" |

The existing `HubStateSnapshot` already has `captured_at` (populated
at `hub_state.cpp:143`).  The other three fields are additions.

### Layer 2 — Query (pure functions, no locks, no re-snapshotting)

A new header (`hub_state_queries.hpp`) with free functions in
`pylabhub::hub` that all take `const HubStateSnapshot &` as input
and return either `nlohmann::json`, primitive, or `std::vector<...>`.

```cpp
namespace pylabhub::hub {
    // ── Channel queries ────────────────────────────────────────────
    nlohmann::json list_channels(const HubStateSnapshot &);
    nlohmann::json get_channel  (const HubStateSnapshot &, const std::string &name);
    int            count_channels_in_state(const HubStateSnapshot &, const std::string &state);
    std::vector<std::string> channels_in_state(const HubStateSnapshot &, const std::string &state);

    // ── Role queries ───────────────────────────────────────────────
    nlohmann::json list_roles(const HubStateSnapshot &);
    nlohmann::json get_role  (const HubStateSnapshot &, const std::string &uid);

    // ── Band queries ───────────────────────────────────────────────
    nlohmann::json list_bands(const HubStateSnapshot &);
    nlohmann::json get_band  (const HubStateSnapshot &, const std::string &name);

    // ── Peer queries ───────────────────────────────────────────────
    nlohmann::json list_peers(const HubStateSnapshot &);
    nlohmann::json get_peer  (const HubStateSnapshot &, const std::string &hub_uid);

    // ── SHM block queries (absorbs Group 2 #2) ────────────────────
    nlohmann::json list_shm_blocks(const HubStateSnapshot &);
    nlohmann::json get_shm_block  (const HubStateSnapshot &, const std::string &channel);

    // ── Cross-aspect / aggregate convenience ──────────────────────
    nlohmann::json channel_state_distribution(const HubStateSnapshot &);
    // → {absent: N, registering: N, stalled: N, live: N}
    nlohmann::json health_summary(const HubStateSnapshot &);
    // → {channels: {total, live, stalled, ...}, roles: {total, registered, ...}, peers: {total, connected, ...}}

    // ── Metrics filter (folds existing query_metrics) ─────────────
    nlohmann::json query_metrics_from_snapshot(const HubStateSnapshot &, const MetricsFilter &);
}
```

**Existing function `observe_channel(entry, snap)` fits this layer
naturally** and is preserved as-is.

Each query is:
- Lock-free (snapshot is private to caller).
- Allocation-bounded to what the query computes.
- Pure — same input, same output.
- Easy to test in isolation.

### Layer 3 — Script API

Hub script gets a snapshot object that holds the C++ snapshot by
shared ownership and exposes Layer 2 queries as methods.

**Python (pybind11):**

```cpp
py::class_<HubSnapshot>(m, "HubSnapshot")
    .def("list_channels",          &HubSnapshot::list_channels)
    .def("get_channel",            &HubSnapshot::get_channel, py::arg("name"))
    .def("count_in_state",         &HubSnapshot::count_channels_in_state, py::arg("state"))
    .def("channels_in_state",      &HubSnapshot::channels_in_state, py::arg("state"))
    .def("list_roles",             &HubSnapshot::list_roles)
    .def("get_role",               &HubSnapshot::get_role, py::arg("uid"))
    .def("list_bands",             &HubSnapshot::list_bands)
    .def("get_band",               &HubSnapshot::get_band, py::arg("name"))
    .def("list_peers",             &HubSnapshot::list_peers)
    .def("get_peer",               &HubSnapshot::get_peer, py::arg("hub_uid"))
    .def("list_shm_blocks",        &HubSnapshot::list_shm_blocks)
    .def("get_shm_block",          &HubSnapshot::get_shm_block, py::arg("channel"))
    .def("channel_state_distribution", &HubSnapshot::channel_state_distribution)
    .def("health_summary",         &HubSnapshot::health_summary)
    .def("age_seconds",            &HubSnapshot::age_seconds)
    .def_property_readonly("captured_at",   &HubSnapshot::captured_at_iso)
    .def_property_readonly("captured_epoch",&HubSnapshot::captured_epoch)
    .def_property_readonly("hub_uid",       &HubSnapshot::hub_uid)
    .def_property_readonly("seq",           &HubSnapshot::seq);

// And on HubAPI:
.def("snapshot", &HubAPI::snapshot)
```

Usage:

```python
def on_periodic_check(api):
    snap = api.snapshot()
    if snap.health_summary()["channels"]["live"] < expected_live:
        api.log("warn", f"only {snap.count_in_state('live')} live "
                        f"(snapshot age {snap.age_seconds():.1f}s)")
```

**Lua:**

Lua needs userdata + metatable for the snapshot type, with methods
attached.  Slightly more boilerplate than Python but the same shape.

```lua
local snap = api:snapshot()
local live = snap:count_in_state("live")
local dist = snap:channel_state_distribution()
local age  = snap:age_seconds()
```

**C++ host-side use:**

```cpp
auto snap = hub.snapshot();
auto live = pylabhub::hub::count_channels_in_state(snap, "live");
auto health = pylabhub::hub::health_summary(snap);
```

### What happens to existing HubAPI list_X methods

They stay, refactored as one-line wrappers:

```cpp
nlohmann::json HubAPI::list_channels() const {
    if (!impl_->host) return nlohmann::json::array();
    return pylabhub::hub::list_channels(impl_->host->state().snapshot());
}
```

This preserves backwards compatibility — scripts that use the
single-aspect pattern see no change.  Internally they do one snapshot
each, same as today.  The *new* primitive is `hub.snapshot()`.

## 3. Files / modules / functions / data structures affected

### 3.1 New files

| Path | Purpose |
|---|---|
| `src/include/utils/hub_state_queries.hpp` | Layer 2 free-function declarations |
| `src/utils/ipc/hub_state_queries.cpp` | Layer 2 free-function implementations |
| `src/include/utils/hub_snapshot.hpp` | `HubSnapshot` wrapper class (shared_ptr-based, script-bindable) |
| `src/utils/service/hub_snapshot.cpp` | Wrapper class methods (forwards to Layer 2) |
| `tests/test_layer2_hub_state_queries/` | L2 unit tests for every Layer 2 function (~14 test files) |
| `tests/test_layer2_hub_snapshot_python/` | Script-binding L2 tests (Python) |
| `tests/test_layer2_hub_snapshot_lua/` | Script-binding L2 tests (Lua) |

### 3.2 Modified files

| Path | Change |
|---|---|
| `src/include/utils/hub_state.hpp` | Add `captured_mono`, `hub_uid`, `snapshot_seq` to `HubStateSnapshot` struct |
| `src/utils/ipc/hub_state.cpp` | Populate new metadata fields in `HubState::snapshot()`; add per-hub `snapshot_seq_` counter (atomic) |
| `src/include/utils/hub_api.hpp` | Add `[[nodiscard]] std::shared_ptr<HubSnapshot> snapshot() const;` method |
| `src/utils/service/hub_api.cpp` | Implement `HubAPI::snapshot()`; refactor `list_channels` / `get_channel` / `list_roles` / `list_bands` / `list_peers` / `get_role` / `get_band` / `get_peer` to one-line wrappers over Layer 2 |
| `src/scripting/hub_api_python.cpp` | Bind `HubSnapshot` class with all methods + properties; add `.def("snapshot", ...)` on HubAPI |
| `src/scripting/lua_engine.cpp` | Add `HubSnapshot` userdata + metatable + 14 method closures + `lua_api_hub_snapshot` factory |
| `src/utils/ipc/broker_service.cpp` | Mark `BrokerService::query_channel_snapshot()` as deprecated (Layer 2 covers this) OR keep as legacy convenience; decide |
| `src/include/utils/broker_service.hpp` | Mark `collect_shm_info_json()` as deprecated in favour of `pylabhub::hub::list_shm_blocks(snap)` |

### 3.3 Data structures changed

| Type | Change |
|---|---|
| `pylabhub::hub::HubStateSnapshot` | Add `captured_mono`, `hub_uid`, `snapshot_seq` fields |
| `pylabhub::hub::HubState::Impl` | Add `std::atomic<uint64_t> snapshot_seq_counter_{0}` |
| `pylabhub::hub_host::HubSnapshot` | NEW — script-binding wrapper class holding `std::shared_ptr<const HubStateSnapshot>` |

### 3.4 Functions changed / added

**New Layer 2 functions** (declared in `hub_state_queries.hpp`):

- `list_channels`, `get_channel`, `count_channels_in_state`, `channels_in_state`
- `list_roles`, `get_role`
- `list_bands`, `get_band`
- `list_peers`, `get_peer`
- `list_shm_blocks`, `get_shm_block`
- `channel_state_distribution`, `health_summary`
- `query_metrics_from_snapshot` (folds existing broker-side logic)

**Refactored functions** (existing impl becomes one-line delegate):

- `HubAPI::list_channels()`
- `HubAPI::get_channel(name)`
- `HubAPI::list_roles()`
- `HubAPI::get_role(uid)`
- `HubAPI::list_bands()`
- `HubAPI::get_band(name)`
- `HubAPI::list_peers()`
- `HubAPI::get_peer(hub_uid)`

**Extended functions:**

- `HubState::snapshot()` — now populates 3 additional metadata fields

**New top-level functions:**

- `HubAPI::snapshot()` — returns the script-bindable wrapper

**Possibly deprecated:**

- `BrokerService::query_channel_snapshot()` — Layer 2 covers it
- `BrokerService::collect_shm_info_json()` — Layer 2 covers it (Group 2 #2 absorbs)
- `ChannelSnapshot` struct — Layer 2 returns `nlohmann::json` directly; the intermediate struct may become redundant
- `ChannelSnapshot::count_by_observable` — moved to Layer 2 as `count_channels_in_state(snap, state)` (Group 2 #3 absorbs)

### 3.5 HEP doc impacts

| HEP | Change |
|---|---|
| HEP-CORE-0033 §12.3 | Rewrite "HubAPI surface" — document the layered API: `snapshot()` is the primary primitive; `list_X` are one-shot conveniences |
| HEP-CORE-0033 §12.3 (read block table) | Add `snapshot()` row at the top of the read block; list all `HubSnapshot` methods |
| HEP-CORE-0019 §4.2 | Cross-reference: `query_metrics_from_snapshot` is the new entry point; existing `METRICS_REQ` flow unchanged |
| HEP-CORE-0023 §2.2 | Cross-reference: `observe_channel(entry, snap)` is a Layer 2 primitive; scripts now reach it via `snap.list_channels()[i]["observable"]` |
| HEP-CORE-0019 §5.5 (the new section we just added) | No change — `set_metrics_hook` still fires at the same call sites; the snapshot layer is orthogonal |

### 3.6 Test impacts

**Existing tests:** all should pass unchanged.  The HubAPI list_X
methods refactor preserves behaviour.

**New L2 tests** (~14 test files, one per Layer 2 function family):

- `test_layer2_hub_state_queries_channels.cpp` — list/get/count/channels_in_state
- `test_layer2_hub_state_queries_roles.cpp` — list/get
- `test_layer2_hub_state_queries_bands.cpp` — list/get
- `test_layer2_hub_state_queries_peers.cpp` — list/get
- `test_layer2_hub_state_queries_shm.cpp` — list/get_shm_block
- `test_layer2_hub_state_queries_aggregates.cpp` — distribution + health_summary
- `test_layer2_hub_state_queries_metrics.cpp` — query_metrics_from_snapshot vs old path
- `test_layer2_hub_state_queries_metadata.cpp` — captured_at / mono / hub_uid / seq populated correctly
- `test_layer2_hub_snapshot_python.cpp` — Python binding smoke + coherence test
- `test_layer2_hub_snapshot_lua.cpp` — Lua binding smoke + coherence test

**Critical coherence test** (the whole point of the refactor):

```cpp
TEST(HubSnapshot, CoherentMultiAspectView) {
    // Capture once.
    auto snap = hub.snapshot();
    // Mutate hub state between queries.
    register_a_new_role(...);
    // Query multiple aspects from the SAME snapshot.
    auto channels = pylabhub::hub::list_channels(snap);
    auto roles    = pylabhub::hub::list_roles(snap);
    // Assert that the new role is NOT visible — snapshot was captured before mutation.
    EXPECT_FALSE(role_appears_in(roles, new_role_uid));
    // Even after mutation, the snapshot stays coherent.
}
```

## 4. Migration plan

**Phase A — Layer 2 only (parallel to existing path)**

1. Add `hub_state_queries.hpp` + `.cpp` with all Layer 2 functions.
2. Add the 3 metadata fields to `HubStateSnapshot`.
3. Populate them in `HubState::snapshot()`.
4. New L2 unit tests for every Layer 2 function.
5. No HubAPI changes yet.  Existing code unaffected.
6. Validate: full test suite passes.

**Phase B — Refactor HubAPI to delegate**

7. Refactor `HubAPI::list_channels` / `get_channel` / `list_roles` / `get_role` / `list_bands` / `get_band` / `list_peers` / `get_peer` to one-line wrappers over Layer 2.
8. Validate: every existing test still passes.  Behaviour identical.

**Phase C — Add HubAPI::snapshot() + script binding**

9. Add `HubSnapshot` wrapper class.
10. Add `HubAPI::snapshot()`.
11. Python binding for `HubSnapshot`.
12. Lua binding for `HubSnapshot`.
13. New L2 binding tests.
14. Validate: new script API works; coherence test passes.

**Phase D — Documentation**

15. HEP-CORE-0033 §12.3 rewrite.
16. HEP-CORE-0019 cross-reference.
17. Group 2 #2 + #3 cleanup: their decisions in
    `GROUP2_DECISIONS_2026-05-20.md` get marked "subsumed by hub
    state query layer design"; the SHM helpers stay as
    `pylabhub::hub::list_shm_blocks` Layer 2 functions; the
    `count_by_observable` becomes `count_channels_in_state` Layer 2
    function.  Original BRC + broker-side helpers can either be
    deprecated (with HEP-amendment cycle) or kept as compatibility
    surface — decide during Phase D.

**Phase E — Cleanup (optional, separate sprint)**

18. Deprecate `BrokerService::query_channel_snapshot()` if no
    in-tree caller remains after Phase B.
19. Deprecate `ChannelSnapshot` struct if Layer 2 returning JSON
    suffices.
20. Deprecate `BrokerRequestComm::query_shm_info()` (the BRC client
    wrapper from Group 2 #2) — Layer 2 + new HubAPI surface replaces
    it.

## 5. Open design questions

These need resolution before Phase A:

1. **HubSnapshot ownership model.**  Options:
   - `std::shared_ptr<const HubStateSnapshot>` — multiple scripts can hold the same snapshot; cheap copies.
   - Move-only wrapper holding a `unique_ptr` — stricter; one owner.
   - Value type (just `HubStateSnapshot` by value) — simplest but expensive to copy; might thrash pybind11.

   Recommend shared_ptr because Python/Lua bindings duplicate handles freely.

2. **`captured_at` Layer 3 representation.**  ISO 8601 string, Unix epoch float, or both?  Recommend both — `captured_at` for human-readable, `captured_epoch` for math.

3. **Should `query_metrics_from_snapshot` replace `BrokerService::query_metrics`?**  The broker-side `query_metrics` has internal snapshotting and federation-aware logic that may not fit Layer 2 cleanly.  Decision: keep broker-side `query_metrics` for the admin RPC path; add Layer 2 `query_metrics_from_snapshot` for the script path that wants a coherent view.

4. **Lua snapshot binding mechanism.**  `lua_newuserdata` + metatable is the standard.  But our snapshot wrapper holds a shared_ptr — needs proper `__gc` to release the ref.  Decide whether to roll our own or use sol2/LuaBridge if already a dep.

5. **What does `snapshot()` return for hub-uninitialised state?**  Recommend: returns a snapshot with empty containers + `captured_at = now()` + `hub_uid = ""`.  Script callers test `if not snap.hub_uid` to detect.  Matches existing "empty array on no-host" pattern.

6. **Snapshot caching / TTL?**  Should `HubAPI::snapshot()` cache the last result and return the same snapshot if called twice within X ms?  Recommend: NO.  Caching adds policy complexity; scripts that want to reuse can hold a snapshot themselves.  Keep `snapshot()` as "always fresh."

## 6. Scope estimate

| Phase | Effort |
|---|---|
| A — Layer 2 functions + metadata + L2 tests | ~1.5 days |
| B — Refactor HubAPI list_X methods | ~0.5 day |
| C — HubSnapshot wrapper + Python binding + Lua binding | ~1 day |
| D — Documentation + Group 2 #2/#3 absorption | ~0.5 day |
| E — Cleanup deprecations (optional) | ~0.5 day |
| **Total** | **~3-4 days focused** |

## 7. Relationship to other in-flight work

- **Group 2 #2** (`query_shm_info` / `collect_shm_info_json` wiring) — **subsumed**.  SHM info becomes Layer 2's `list_shm_blocks(snap)` + `get_shm_block(snap, channel)`.  Original BRC + broker helpers stay as legacy surface until Phase E.
- **Group 2 #3** (`count_by_observable` wiring) — **subsumed**.  Becomes Layer 2's `count_channels_in_state(snap, state)`.  The original `ChannelSnapshot::count_by_observable` method stays as a C++ utility on the public struct (its existence motivated this design and was correct in the small).
- **Task #44** (L4 plh_role test infra) — unaffected.  Independent dual-hub validation work.
- **Task #66** (S1 Phase B socket policy) — unaffected.
- **Task #72** (Wave-B M9 RoleHostFrame template) — unaffected.
- **Task #73** (HEP-0033 Phase 10 doc closure) — partially affected.  §12.3 rewrite under Phase D contributes to closure.
- **Task #75** (HUB_TARGETED_ACK wire frame) — unaffected.
- **Task #76** (Script reload) — unaffected.
- **Task #77** (Tier 2 dynamic callbacks) — unaffected.

## 8. Decision log

- 2026-05-20: tech_draft created.  Triggered by Group 2 candidate #3
  walkthrough — user spotted that wiring `count_by_observable` to
  hub script API would force a full snapshot per call, and that
  every existing `HubAPI::list_X` already snapshots redundantly.
  Group 2 #2 and #3 decisions paused pending this design's
  implementation.  Implementation queued as a new TODO_MASTER entry.

- 2026-05-20: Snapshot metadata extended with `captured_mono`,
  `hub_uid`, `snapshot_seq` per user direction: "the snapshot would
  also carry for example a time stamp so this information is not
  lost when it was taken."
