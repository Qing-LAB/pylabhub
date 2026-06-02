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

## Candidate #4 — `RoleAPIBase::set_metrics_hook`

**Status:** no code change. Setter + member + two consumer branches exist; zero callers in src/ or tests/.

**What it is:** A C++ extension point that lets a role host install a `std::function<void(nlohmann::json &)>` which fires AFTER the standard metrics fields are populated in two distinct emission paths.

**Where:**
- decl: `src/include/utils/role_api_base.hpp:226` (docstring lines 218-225)
- impl: `src/utils/service/role_api_base.cpp:443-446`
- member: `src/utils/service/role_api_base.cpp:154` (`std::function<void(nlohmann::json &)> metrics_hook;` in Impl)
- consumer #1: `src/utils/service/role_api_base.cpp:2122-2123` (inside `snapshot_metrics_for_presence`)
- consumer #2: `src/utils/service/role_api_base.cpp:2188-2189` (inside `snapshot_metrics_json`)

**Trigger sites (the critical insight):**

1. **Heartbeat hot path (PRIMARY).** `RoleAPIBase::on_heartbeat_tick_()` at lines 847-858 iterates `handler_->presences()` and calls `snapshot_metrics_for_presence(role_type)` once per presence per heartbeat period (HEP-CORE-0019 §2.3 Phase 6). The hook fires inside that call. The resulting metrics ride the heartbeat to the broker → stored → returned by admin queries.

2. **Script-on-demand.** When a script calls `api.snapshot_metrics_json()` (forwarded from ConsumerAPI/ProducerAPI/ProcessorAPI), the hook fires inside the single-result variant. Rare in practice — scripts don't usually poll their own metrics.

So this is a **hot-path telemetry-injection point** for C++ role hosts, with thoughtful multi-presence semantics (a dual-presence processor fires the hook once for consumer-presence + once for producer-presence, and hooks can disambiguate via `result["queue"]` / `result["role"]` shape).

**Tier classification:**

| Axis | Status |
|---|---|
| C++ host-side extension point | YES (designed for it) |
| Python/Lua script binding | NO (signature takes `std::function`; not script-bindable) |
| Wire-protocol RPC | NO (consumed locally; metrics ride heartbeats) |

**Relationship to existing API:**

`RoleAPIBase::report_metric(key, value)` (script-callable) emits flat scalars into the `"custom"` block at the same two emission sites. The hook is the RICHER companion — for structured/nested JSON injection that can't be expressed as a single key→double. Today, every role host uses the simpler `report_metric` path; none use the hook.

**Decision (2026-05-20):** KEEP, mark as reserved in the docstring. User: "2".

User reasoning (reconstructed from discussion): The hook is well-designed, wired into the hot path with multi-presence semantics, and costs ~10 LOC. Better to preserve it as a documented extension point than delete and re-add later. But it MUST be marked so future readers don't read it as "active production API."

**Implementation plan:**

Phase 4a — HEP-CORE-0019 §5.5 design capture (DONE 2026-05-20, working tree, uncommitted):
1. New §5.5 inserted between §5.4 and §6.  Seven sub-sections:
   §5.5.0 status preamble; §5.5.1 API surface; §5.5.2 trigger sites
   (`role_api_base.cpp:2053` heartbeat hot path + `:2128`
   on-demand); §5.5.3 sequence diagram (ctrl thread → hook → wire);
   §5.5.4 per-presence disambiguation pattern with code example;
   §5.5.5 thread-safety contract; §5.5.6 when-to-use vs
   `report_metric` table; §5.5.7 status + cross-reference back to
   inline docstring.

Phase 4b — Inline docstring annotation (in `role_api_base.hpp` lines 218-225):
1. Add a "Reserved" preamble to the existing docstring noting:
   - Status: no current callers in src/ or tests/ as of 2026-05-20.
   - Intended caller: C++ role hosts during `startup_()`, not scripts.
   - Cross-reference: HEP-CORE-0019 §5.5 for full design.
   - When using: remove the "Reserved" note in the same commit that adds the caller.
2. No body changes. The setter, member, and consumer branches stay exactly as they are.

Phase 4c — Optional follow-up tracking:
1. Consider adding an entry in `docs/todo/API_TODO.md` noting `set_metrics_hook` as a reserved extension point with documented telemetry use cases (processor cross-hub stats, consumer inbox-stall stats, producer drop counts) — so future authors who hit these needs find this API instead of re-inventing.

**Risks:**

- "Reserved" comments rot. If no caller materialises in 6-12 months, this should be re-walked: still reserved, or finally delete?
- Future authors might still miss the comment and re-invent. Mitigation: cross-reference from `report_metric` docstring ("for scalar metrics; for structured injection, see set_metrics_hook (currently reserved)") — flagged as a sub-decision in Phase 4a.

---

## Candidate #5 — `BrokerService::send_hub_targeted_msg`

**Status:** no code change. Function exists, wire-frame machinery alive, one test caller in `hub_federation_workers.cpp:282`. Zero production callers in `src/`.

**What it is:** Public broker-service method whose body enqueues a `{target_hub_uid, channel, payload}` struct onto `hub_targeted_queue_` (mutex-guarded). The broker `run()` loop drains the queue every poll iteration and sends each entry as a `HUB_TARGETED_MSG` peer-wire frame via `send_to_identity` to the target hub's DEALER socket.

**Where:**
- decl: `src/include/utils/broker_service.hpp:494`
- impl: `src/utils/ipc/broker_service.cpp:4133-4139` (enqueue)
- queue drain: `src/utils/ipc/broker_service.cpp:690-718`
- wire-frame send: line 716 (`send_to_identity(router, peer->zmq_identity, "HUB_TARGETED_MSG", msg)`)
- wire dispatch (receive): `:808` (peer-DEALER inbound), `:1015` (ROUTER local inbound)
- known-message catalogue: `:141`

**Why NOT dead code:**

1. The wire frame `HUB_TARGETED_MSG` is fully alive (broker handles inbound + outbound).
2. The receive side has a working hub-script callback (`HubAPI::augment_peer_message` per `hub_api.hpp:320`).
3. Federation test (`tests/test_layer3_datahub/workers/hub_federation_workers.cpp:282`) exercises end-to-end.
4. Documented as a federation primitive in HEP-CORE-0022 (Hub Federation Broadcast) and HEP-CORE-0033 §13 (peer-DEALER inbound frame catalogue).
5. The broker `run()` loop actively drains the queue every poll iteration.

**Intent (clarified by user 2026-05-20):**

> "This is part of the unfinished design of federation which coordinates hub clusters and this serves to send messages between hubs for federation-specific tasks. We can mark it as reserved and will continue the wiring of it when we focus on the design and implementation of the federation which pends on the finishing of all hub and role host."

So:
- This function is a federation-coordination primitive — for hub-to-hub messaging used by federation-specific tasks (cluster-state sync, coordinated administrative actions, peer-aware routing).
- The script-side wrapper (`HubAPI::send_to_peer(peer_uid, channel, payload)` or similar) is INTENTIONALLY deferred — federation is a major feature that depends on hub + role-host substrate being stable first.
- Premature wiring would cause churn against an unstable substrate.

**Relationship to other federation items (CRITICAL to keep straight):**

| Item | Status | What it is |
|---|---|---|
| `HUB_TARGETED_MSG` (the send-side wire frame) | ✅ alive | This candidate's primitive — exists, tested, drained |
| `send_hub_targeted_msg` (C++ enqueue API) | ✅ alive | This candidate — reserved for federation wiring |
| `HubAPI::augment_peer_message` (receive-side script hook) | ✅ alive | Hub scripts can already RECEIVE targeted messages |
| `HUB_TARGETED_ACK` wire frame (Task #75) | ⏸ deferred | Reply frame; needs wire-protocol addition |
| `HubAPI::send_to_peer(...)` (script-side send API) | ⏸ NOT YET WRITTEN | Future federation work — depends on stable hub/role-host substrate |

`HUB_TARGETED_ACK` (Task #75) is the REPLY frame — a sibling deferral but a SEPARATE feature. `send_to_peer` script API is what completes the SEND path. Both are part of the broader federation work but tracked as distinct deferrals.

**Decision (2026-05-20):** KEEP, mark as reserved (federation-deferred). User: "we can mark it as reserved and will continue the wiring of it when we focus on the design and implementation of the federation which pends on the finishing of all hub and role host."

**Implementation plan:**

Phase 5a — Inline docstring annotation (in `broker_service.hpp` lines 483-496):
1. Add a "Reserved — federation work" preamble to the existing docstring noting:
   - Status: no production caller in `src/` as of 2026-05-20.
   - Intent: federation hub-to-hub coordination primitive (HEP-CORE-0022).
   - Why deferred: federation script-API wrapper (`HubAPI::send_to_peer`) is intentionally deferred until hub + role-host substrate is stable.
   - Related deferrals: Task #75 (`HUB_TARGETED_ACK` reply frame).
   - Test that exercises the C++ side: `hub_federation_workers.cpp:282`.
   - Do NOT delete — wire frame is live and tested.

Phase 5b — Add to deferred-features tracking:
1. Add an explicit note in `docs/todo/API_TODO.md` (or a federation-specific TODO if one exists) cross-referencing:
   - This reserved primitive.
   - The pending `HubAPI::send_to_peer` script wrapper.
   - The related Task #75 (`HUB_TARGETED_ACK`).
2. Goal: future federation work picks up a single rallying point that lists ALL deferred federation pieces, so the eventual implementation is bundled rather than fragmented across sessions.

**No HEP amendment for #5.**

Unlike #4 (which needed §5.5 design capture because the trigger-site sequence was undocumented), #5 is already documented:
- HEP-CORE-0022 covers the federation broadcast/messaging model.
- HEP-CORE-0033 §13 covers the peer-DEALER frame catalogue.
- HEP-CORE-0033 §12.3 (HubAPI surface table) already lists `on_peer_message` as "Deferred — requires HUB_TARGETED_ACK".

The doc tree already says what this is for. Adding more would duplicate. The reserved status goes into the inline docstring (Phase 5a) so a code reader doesn't misread the silence.

**Risks:**

- "Reserved" comments rot. Same risk as #4. Mitigation: every federation-related session pass should review the reserved set (#5 + Task #75 + future `send_to_peer`).
- Without explicit cross-referencing, future readers might mistake this for dead code on a future sweep. The Phase 5a docstring is the primary mitigation.

---

## Group 2 status — all candidates decided

| # | Item | Decision | Execution status |
|---|---|---|---|
| 1 | `close_all_inbox_clients` | DELETE | ✅ shipped (commit `4e9ed48`) |
| 2 | `query_shm_info` + `collect_shm_info_json` | KEEP + wire role/hub script APIs | ⏸ **SUBSUMED by Hub State Query Layer design** (`docs/tech_draft/hub_state_query_layer_design.md`).  SHM info becomes Layer 2 functions `list_shm_blocks(snap)` + `get_shm_block(snap, ch)`. |
| 3 | `ChannelSnapshot::count_by_observable` | KEEP + wire hub script API | ⏸ **SUBSUMED by Hub State Query Layer design**.  Becomes Layer 2 function `count_channels_in_state(snap, state)`.  Original method on `ChannelSnapshot` stays as C++ utility (its existence motivated the design). |
| 4 | `set_metrics_hook` | KEEP, mark reserved | ✅ shipped (commit `59be4ae`) — HEP-0019 §5.5 + inline docstring + API_TODO entry |
| 5 | `send_hub_targeted_msg` | KEEP, mark reserved (federation-deferred) | ✅ shipped (commit `59be4ae`) — inline docstring + API_TODO entry |

**Reframe note (2026-05-20):** Group 2 #2 and #3 walkthroughs led to a bigger architectural question — every `HubAPI::list_X` method snapshots hub state redundantly today, and scripts that want multi-aspect coherent views can't get them.  Rather than wire #2 and #3 as independent script-API additions, the right answer is a layered capture-then-query design that lets scripts call `hub.snapshot()` once and query everything from that coherent moment.  Design captured in `docs/tech_draft/hub_state_query_layer_design.md`; new TODO_MASTER entry under "Deferred polish".

Decisions doc is COMPLETE.  Execution phase begins now.

## Execution order

Top-to-bottom from this doc:

1. **#4 Phase 4a (HEP §5.5)** — already in working tree; bundle with #4 Phase 4b and commit.
2. **#4 Phase 4b** — inline "Reserved" docstring in `role_api_base.hpp:218-225`, cross-reference HEP-0019 §5.5.
3. **#5 Phase 5a** — inline "Reserved — federation work" docstring in `broker_service.hpp:483-496`.
4. **#4 Phase 4c + #5 Phase 5b** — `docs/todo/API_TODO.md` entries for reserved extension points + federation deferrals.
5. **Commit bundle: reserved-API annotations** — covers #4 + #5 in one logical change ("mark set_metrics_hook + send_hub_targeted_msg as reserved; add HEP-0019 §5.5 design capture").
6. **#2 wiring** — Phase 2a (role-script SHM-info API) + 2b (hub-script SHM-info API) + 2c (HEP-0030/0019/0033 amendments) + 2d (tests).
7. **#3 wiring** — Phase 3a (HubAPI method) + 3b (Python binding) + 3c (Lua binding) + 3d (HEP-0033 §12.3 amendment) + 3e (tests).
8. Each of (#6/#7) gets its own commit pair: code + HEP doc.

#2 and #3 are larger wiring tasks (each is multi-file: HubAPI changes, pybind11 binding, Lua closure, HEP doc, L2/L3 tests).  Best to do them as separate sessions with their own discussion, since each one ships a new public script-API surface.

After #4 + #5 ship (the reserved-marking commit), the next decision is whether to start #2 or #3 in the current session, or pause and queue them as separate tasks.
