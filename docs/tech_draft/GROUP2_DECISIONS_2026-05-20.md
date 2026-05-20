# Group 2 Dead-Code / Suspect-Code Decisions

**Status:** in-progress decision log. Each candidate is walked with the user; the decision is recorded here verbatim. After all candidates are decided, implementation is executed top-to-bottom from this doc — NOT from chat memory.

**Why this doc exists:** prior sessions lost accumulated decisions across context resets. Persisting decisions to a tech_draft survives summarization and lets execution be deterministic.

**Process per candidate:**
1. Locate the code (grep, read source).
2. Surface what it is, what calls it, what its intent looks like.
3. Decide: delete | keep | wire-up | investigate-further | retire-via-HEP.
4. Record the decision verbatim + implementation steps + risks.
5. Move to next candidate. Do not implement until all decisions are recorded.

---

## Candidate #1 — `RoleAPIBase::close_all_inbox_clients`

**Status:** edits applied (header decl + cpp body removed); uncommitted in working tree.

**What it is:** A one-line forwarder on the base class, body `pImpl->core->clear_inbox_cache();`. Lives in:
- decl: `src/include/utils/role_api_base.hpp:336` (now removed)
- impl: `src/utils/service/role_api_base.cpp:1797-1800` (now removed)

**What calls it:** Nothing. Zero callers in `src/` or `tests/`.

**Why it looked dead:** The underlying `clear_inbox_cache` is heavily used — but the role-specific subclasses (`ProducerAPI`, `ConsumerAPI`, `ProcessorAPI`) each expose their own `clear_inbox_cache()` method with try/catch + LOGGER_WARN. Scripts call THOSE. The base-class forwarder was a duplicate that bypassed the per-role error wrapping with no callers.

**Decision (2026-05-20):** DELETE. Approved by user.

**Implementation:** done in working tree, awaiting commit.

**Risks:** None for in-tree code. Out-of-tree code calling through a `RoleAPIBase*` pointer would break, but the function is undocumented and not part of any HEP surface.

---

## Candidate #2 — `BrokerRequestComm::query_shm_info` + `BrokerService::collect_shm_info_json`

**Status:** no code change. Both functions exist + compile + return correct shapes; both have zero callers.

**What they are:**

| Function | Side | Mechanism | Returns | Purpose |
|---|---|---|---|---|
| `BrokerRequestComm::query_shm_info(channel, timeout_ms)` | Role-process client | Sends `SHM_BLOCK_QUERY_REQ` over ZMQ to broker, awaits `SHM_BLOCK_QUERY_ACK` | `std::optional<nlohmann::json>` | Cross-process query from a role |
| `BrokerService::collect_shm_info_json(channel)` | Broker-process direct | Calls private `collect_shm_info(channel)` + `.dump()` | `std::string` | In-process query from hub-side code |

**What calls them:** Nothing in `src/` or `tests/`.

**Why they exist (intent reconstruction):**

- The role-side helper is the right shape for a role script API: `query_shm_info(channel)` is shaped exactly like `list_channels()`, `band_members(band)`, `query_role_presence(uid)` — all of which ARE wired into `ConsumerAPI` / `ProducerAPI` / `ProcessorAPI` `.def(...)` blocks and exposed to Python + Lua scripts.
- The hub-side helper returns `std::string` (JSON-dumped), which is the right shape for `HubAPI` script methods. `HubAPI` (`src/include/utils/hub_api.hpp`) is the hub-script surface per HEP-CORE-0033 §12.3.
- HEP-CORE-0033 §G2 lists `SHM_BLOCK_QUERY_REQ` as a documented Class C diagnostic endpoint.
- HEP-CORE-0019 §3.2 currently folds SHM-block info INTO the `query_metrics` response — so the underlying capability is already plumbed, just not exposed as its own focused call on either script API.

**These are NOT dead code. They are unfinished script-API scaffolding for a focused `api.shm_info(channel)` (role) + `hub.shm_info(channel)` (hub) call.**

**Decision (2026-05-20):** KEEP and IMPLEMENT — wire both into their respective script APIs. User: "they may be serving this purpose, just not wired to the api."

**Implementation plan:**

Phase 2a — Role-side wiring:
1. Add `RoleAPIBase::shm_info(const std::string &channel, int timeout_ms = 5000) -> std::optional<nlohmann::json>` that forwards to the BRC `query_shm_info`.
2. Expose on `ConsumerAPI`, `ProducerAPI`, `ProcessorAPI` via:
   - C++ method that wraps `RoleAPIBase::shm_info` with try/catch + LOGGER_WARN (same pattern as `band_members`).
   - pybind11 `.def("shm_info", ...)` binding.
   - Lua `lua_api_shm_info` closure + `push_closure("shm_info", ...)`.

Phase 2b — Hub-side wiring:
1. Add `HubAPI::shm_info(const std::string &channel = "") -> nlohmann::json` that calls `BrokerService::collect_shm_info_json` and parses (or returns the raw string per HubAPI convention — check sibling `metrics()` shape first).
2. Expose in `HubAPI` script bindings (Python + Lua).

Phase 2c — Documentation:
1. Amend HEP-CORE-0030 or HEP-CORE-0019 (whichever owns role-script API surface) with `api.shm_info(channel)` entry.
2. Amend HEP-CORE-0033 §12.3 (HubAPI surface) with `hub.shm_info(channel)` entry.

Phase 2d — Tests:
1. L2: Python script + Lua script call `api.shm_info(channel)` against a stub broker.
2. L3 (hub-side): hub script calls `hub.shm_info(channel)` and validates the returned JSON shape.

**Risks:**

- The two functions cross-link to `SHM_BLOCK_QUERY_REQ` wire-protocol message — that endpoint is documented in HEPs but the wire frame should be re-verified against current broker_proto 5 grammar before exposing to scripts.
- Adding to script API is a public surface expansion; once shipped, it can't be silently removed.
- `collect_shm_info_json` returns string; `HubAPI` typical convention may be `nlohmann::json` (verify against `metrics()` and `query_metrics()`).

**Update existing tracking:**
- `docs/todo/API_TODO.md` line 77 — change from "Delete query_shm_info (zero callers)" to "Wire up SHM-info script API on role + hub sides — unfinished scaffolding, see GROUP2_DECISIONS_2026-05-20.md candidate #2."
- `docs/code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` X2/D3 row — same reframe.

---

## Candidate #3 — `ChannelSnapshot::count_by_observable`

**Status:** no code change. Method exists + compiles + correct; zero callers in src/tests.

**What it is:** Inline member function on the `struct ChannelSnapshot` value type at `src/include/utils/broker_service.hpp:77-83`. Counts how many channels in the snapshot have a given observable state (`"absent" | "registering" | "stalled" | "live"` per HEP-CORE-0023 §2.2). Body is a 4-line linear scan.

**Scope clarification:** This is for **data-streaming channels** (producer→consumer ZmqQueue/ShmQueue pipelines), NOT for bands (HEP-CORE-0030 pub/sub messaging). Verified via `e.observable` being filled from `pylabhub::hub::observe_channel(entry, hub_snap)` per HEP-0023, and the entry carrying `producer_uids` / `consumer_count` / `schema_hash` — all data-channel concepts.

**Thread-safety analysis (decision-relevant):**

`ChannelSnapshot` is built via `BrokerService::query_channel_snapshot()` → `HubState::snapshot()`. The snapshot path is correct by construction:

- `HubState::snapshot()` takes a shared (reader) lock on `pImpl->mu`, then deep-copies seven containers (`channels`, `roles`, `bands`, `peers`, `shm_blocks`, `schemas`, `counters`) and releases the lock.
- All entry types (ConsumerEntry, ProducerEntry, ChannelEntry, BandEntry, RoleEntry, PeerEntry, ShmBlockRef) hold ONLY value types: `std::string`, integers, `enum`s, `std::vector<EntryType>`, `std::chrono::time_point`, `nlohmann::json`. Verified by grep — zero `std::atomic`, zero `std::mutex`, zero `std::shared_ptr` / `weak_ptr` / `unique_ptr`, zero `std::function`, zero raw pointers to live state.
- The only `std::function` / `std::unique_ptr` in `hub_state.hpp` live in `HubState::Impl` itself (subscription handlers + pImpl) and are NOT in the snapshot.
- Result: after `snapshot()` returns, the snapshot has **zero aliasing** with live hub state. Reads are allocation-free pointer-chases through the caller's private memory.
- `count_by_observable` is therefore contention-free: pure function over private value tree.

The only perf characteristic worth noting: snapshot creation does heap allocations for every string/vector/json blob — that's a `snapshot()` cost, not a `count_by_observable` cost.

**Tier classification:**

| Axis | Status |
|---|---|
| C++ public method on public struct | YES (header-only, default-public `struct`) |
| Python/Lua script binding | NO (not `.def(...)` or pushed via closure) |
| Wire-protocol RPC | NO (purely local) |

**Decision (2026-05-20):** KEEP, AND wire up to the hub script engine API. User: "keep this, and wire it up for hub script engine api."

**Implementation plan:**

Phase 3a — Hub-script API method:
1. Add `HubAPI::count_channels_in_state(const std::string &state) const -> int` (name tentative — final TBD; could also be `count_channels_by_observable` to match the underlying method, or `channel_count(state)` for script ergonomics).
2. Implementation calls `BrokerService::query_channel_snapshot().count_by_observable(state)` and returns the int.
3. The method should validate the input string against the four canonical observable states (`absent`/`registering`/`stalled`/`live`) and either: (a) return 0 for unknown states + log a WARN, or (b) throw a script-visible error. Decision pending in 3a sub-walk.

Phase 3b — Python binding:
1. Locate the `HubAPI` pybind11 binding site (search for `.def("metrics", &HubAPI::metrics)` etc. in the appropriate `.cpp`).
2. Add `.def("count_channels_in_state", &HubAPI::count_channels_in_state, py::arg("state"))`.

Phase 3c — Lua binding:
1. Locate the Lua closure pusher for HubAPI in `lua_engine.cpp` (or wherever hub-side Lua bindings live).
2. Add a `lua_api_count_channels_in_state` closure that pulls the arg, calls the HubAPI method, pushes the int return.

Phase 3d — Documentation:
1. HEP-CORE-0033 §12.3 (HubAPI surface table) — add entry for `count_channels_in_state`.
2. Optional: a paragraph in HEP-CORE-0023 cross-referencing that the observable values are scriptable.

Phase 3e — Tests:
1. L2 hub-script test (Python + Lua): script calls `hub.count_channels_in_state("live")` against a stub broker that returns a known snapshot, verifies the int returned matches expectation.
2. Mutation test: script calls with an invalid state string, verifies the error/warn path per 3a decision.

**Risks:**

- Naming bikeshed — `count_channels_in_state` vs `channel_count(state)` vs `count_channels_by_observable`. Match HubAPI's existing naming conventions (sibling methods: `metrics()`, `query_metrics(categories)` — verb-first, snake_case). Resolve in 3a sub-decision before binding.
- Input validation policy — silent return-0 vs error-throwing. Affects whether script authors can typo-debug easily.
- Each call to `count_channels_in_state` snapshots the hub. If a script calls it in a tight loop, that's repeated allocation. Document as "use sparingly / cache results / take one snapshot for multiple queries" — OR expose the full snapshot as a separate scriptable type (out of scope for this candidate; flag as future work).

---

## Further candidates

Original list of 4 candidates from prior dead-code sweep now decided. Next step: fresh dead-code sweep across role-host / engine / hub-broker subsystems to surface any candidates missed.
