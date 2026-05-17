# Data Exchange Hub - Master TODO

**Purpose:** This is the **master execution plan** for the DataHub project. It provides a high-level overview of what needs to be done and references to detailed TODO documents for specific areas.

**Philosophy:** Keep this document concise and high-level. Detailed tasks, completion tracking, and phase-specific work belong in subtopic TODO documents.

---

## Overview

The Data Exchange Hub (DataHub) is a cross-platform IPC framework using shared memory for high-performance data transfer. Current focus is on Layer 4 test coverage for the standalone producer/consumer/processor binaries and Named Schema Registry integration.

**Key Documents:**
- **Design Spec**: `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`
- **Implementation Guidance**: `docs/IMPLEMENTATION_GUIDANCE.md`
- **Pipeline Architecture**: `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md`
- **Producer + Consumer Binaries**: `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md`
- **Schema Registry**: `docs/HEP/HEP-CORE-0034-Schema-Registry.md` (ratified 2026-04-26; supersedes HEP-CORE-0016)
- **Policy Reference**: `docs/HEP/HEP-CORE-0009-Policy-Reference.md` (active cross-reference)

---

## Current Sprint Focus

### Ultimate goal — finish hub/broker renovation, ship dual-hub-capable plh_hub

The pylabhub hub/broker is in a multi-track renovation.  The
**end-state** is a dual-hub-capable system: a fully functional
`plh_hub` binary (HUB-side complete) PLUS role binaries that can
register presences on multiple hubs (ROLE-side renovation
complete), so a processor role can run with input on hub-A and
output on hub-B end-to-end.

Two PARALLEL renovation arcs feed this goal.  They are tracked
under different label spaces — **DO NOT MERGE THEM** in your
head; the labels were chosen at different times for different
sub-problems and the names sometimes look similar but mean
different things.  Each arc has its own canonical doc:

| Arc | Canonical doc | Label space |
|---|---|---|
| **Arc A — `plh_hub` renovation (HUB side)** | `docs/HEP/HEP-CORE-0033-Hub-Character.md` §15 | `HEP-0033 §15 Phase 1..10` |
| **Arc B — Role-host renovation (ROLE side)** | `docs/tech_draft/role_host_template_design.md` §14 | `Wave-B M0..M9` (with hyphen — see §"Label hygiene" below) |

Side-arcs (independent waves that addressed bugs / cleanups
revealed DURING the renovation) are tracked under a third label
space — **side-arc cleanup waves** — listed below the two arcs.

### Label hygiene — read this before reading any "M*" label below

The codebase has accumulated several M-series labels.  They are
**different things** despite the naming collision.  When this
file or any subtopic doc says an "M*" label, it means exactly one
of the following:

| Label prefix | Means | Examples |
|---|---|---|
| `Wave-B M0..M9` | Sequential phases of Arc B (role-host renovation; `role_host_template_design.md` §14) | `Wave-B M2`, `Wave-B M8` |
| `HEP-0033 §15 Phase N` | Sequential phases of Arc A (plh_hub renovation) | `Phase 6.2`, `Phase 7 D4.2` |
| `Wave-M2 / Wave-M2.5 / Wave-M3` | Multi-producer / controlled-access refactor side-arcs (closed) | `Wave-M2.5`, `Wave-M3` |
| `M1.2 / M1.4 / M1.5` | Sequential FSM-consolidation cleanup side-arcs (closed) | `M1.4`, `M1.5` |
| `MD1 / MD1.5` | Race-fix + ThreadManager-contract side-arcs (closed) | `MD1` |

If you see a bare `M3` or `M8` without a prefix, it's almost
certainly **Wave-B M3** or **Wave-B M8** in the renovation arc —
but check context.  In particular: **Wave-B M3** (RoleHandler
skeleton) is NOT the same as **Wave-M3** (RoleEntry controlled-
access, side-arc, closed 2026-05-11).

### Arc A — `plh_hub` renovation status (HEP-0033 §15, verified against code 2026-05-15)

| Phase | Goal | Status | Evidence |
|---|---|---|---|
| **Phase 1** | `HubConfig` + role-facing config rename | ✅ shipped 2026-04-29 |  |
| **Phase 2** | `hub_cli` | ✅ shipped 2026-04-25 (`9ba6ac1`) |  |
| **Phase 3** | `HubDirectory` + `--init` | ✅ shipped 2026-04-29 |  |
| **Phase 4** | `HubState` struct + accessors | ✅ already complete |  |
| **Phase 5** | Query engine over `HubState` | ✅ (absorbed into Phase 6.2 AdminService query methods) |  |
| **Phase 6.1a** | `HubState` ownership refactor | ✅ shipped 2026-04-30 |  |
| **Phase 6.1b** | `HubHost` lifecycle owner | ✅ shipped 2026-04-30 | `src/utils/service/hub_host.cpp` |
| **Phase 6.2** | `AdminService` structured RPC (6.2a/b/c) | ✅ shipped 2026-05-02 | `src/utils/ipc/admin_service.cpp`; 22 + 8 strengthening tests |
| **Phase 7** | `HubScriptRunner` + per-engine `build_api_(HubAPI&)` (sub-commits A/B/C, D1/1.5/D2/D3/D4) | ✅ shipped 2026-05-03/04 | `src/utils/service/hub_script_runner.{cpp,hpp}` + `src/scripting/hub_api_python.cpp` + Lua bindings |
| **Phase 8a** | `HubAPI` read accessors | ✅ shipped | `src/include/utils/hub_api.hpp:179-209` (list_channels, list_roles, list_bands, list_peers, query_metrics) |
| **Phase 8b** | `HubAPI` control delegates | ✅ shipped | `hub_api.hpp:228-242` (close_channel, broadcast_channel, request_shutdown) |
| **Phase 8c** | `HubAPI` response-augmentation hooks | ✅ shipped | `hub_api.hpp:308-325` (augment_query_metrics / list_roles / get_channel / peer_message); `ScriptEngine::invoke_returning` virtual in place |
| **Phase 9** | `plh_hub` binary + L4 tests | ✅ shipped | `src/plh_hub/plh_hub_main.cpp`; `tests/test_layer4_plh_hub/` has 7 L4 test files (init, keygen, validate, runmode, errors, role_roundtrip, fixture) |
| **Phase 10** | HEP-CORE-0019 §9 + README + deployment docs amendment | ⏳ **partial** | Many HEPs updated piecemeal; no explicit "Phase 10 ✅ closed" marker in HEP-0033.  Punch list: HEP-0019 §9 per-producer metrics tree (Wave M2.5 step 8 carryover), cross-references survey. |

**Arc A net status**: substantially complete.  Binary + L4 tests +
script bindings all in place; ONE doc-sweep item (Phase 10) is
nominally open but not blocking.

### Arc B — Role-host renovation status (Wave-B M0..M9, verified against code 2026-05-15)

| Phase | Goal | Status | Code evidence |
|---|---|---|---|
| **Wave A** (Arc B's docs foundation) | A1..A8 — HEP-CORE-0011/0017/0019/0023/0027/0033 staleness scrub + §18+§19 add | ✅ all shipped | `role_host_template_design.md` §15.1 table |
| **Wave-B M0** | `send_heartbeat` wire payload gains `(uid, role_type)` | ✅ shipped | `src/include/utils/broker_request_comm.hpp:116`; commit `f353e1a` |
| **Wave-B M1** | Broker keyed lookup via `find_presence`; per-presence FSM; retire `metrics_store_` | ✅ shipped | `src/utils/ipc/broker_service.cpp:2100,2132`; commits `f2e3bd7..a41ce71` (M1.2 side-arc) + `4e902e1` (M1.4 side-arc) |
| **Wave-B M2** | Heartbeat is per-presence on the role side; broker sweep iterates BOTH `entry.producers` and `entry.consumers`; consumer heartbeat-timeout fires `CONSUMER_DIED_NOTIFY` (`reason="heartbeat_timeout"`).  `out_channel.empty() ? ...` hack deleted. | ✅ shipped 2026-05-15 | Role side: `src/utils/service/role_api_base.cpp:560-602` per-presence emission.  HubState: `src/include/utils/hub_state.hpp:1393-1419` + `src/utils/ipc/hub_state.cpp:1334-1518` (`role_type` param on `_on_heartbeat_timeout` / `_on_pending_timeout`, consumer branch added).  Broker: `src/utils/ipc/broker_service.cpp:2594-2789` (consumer pass-1 + pass-2 + notification fan-out).  Tests: 3 L2 in `test_hub_state.cpp` + 1 L3 in `test_datahub_role_state_machine.cpp` (`ConsumerHeartbeatTimeout_FiresConsumerDiedNotify`). |
| **Wave-B M3** | `Presence` / `HubConnection` / `RoleHandler` headers (build-only) | ✅ shipped 2026-05-15 | `src/include/utils/role_handler.hpp` + `src/utils/service/role_handler.cpp`; commits `4bf8dd7..a23d3d1` (skeleton + 3-test polish + forward-looking review).  Includes Wave-B M3 review follow-ups: 29 L2 tests in `test_layer2_service/test_role_handler.cpp`. |
| **Wave-B M4** | `RoleAPIBase` pImpl delegates via handler | ⏳ in progress (M4a-e shipped; M4f pending) | M4a-e: `src/utils/service/role_api_base.cpp` routes Class A through `pImpl->resolve_bc_for_channel(channel)`, Class B through `pImpl->resolve_bc_for_role()` (HEP-CORE-0033 §18.3 fall-through; pre-M5 returns first connection), Class D through `pImpl->resolve_bc_for_band(band)` with `handler->on_band_joined/on_band_left` wiring on band_join/band_leave success.  All three helpers fall back to legacy `broker_channel` view when handler is null OR the lookup is empty.  M4f: delete `broker_channel` once `set_broker_comm` + `start_ctrl_thread` retire (M5+). |
| **Wave-B M5** | `ProducerRoleHost` 1-presence list via handler | ✅ shipped 2026-05-16 | `src/producer/producer_role_host.cpp` — `broker_comm_` deleted; build presence → RoleHandler → `start_handler_threads` → `register_producer_channel` → `install_heartbeat`.  Commit `7ac9aa2` + helper additions in `fc88c47` (auto-record on register_*, install_heartbeat, extract_hub_heartbeat_max, append_inbox_to_reg). |
| **Wave-B M6** | `ConsumerRoleHost` same | ✅ shipped 2026-05-16 | Near-clone of M5: `src/consumer/consumer_role_host.cpp` — `broker_comm_` deleted; same 4-substep handler-mode sequence with Consumer presence + register_consumer.  Commit `72a4e49`. |
| **Wave-B M7** | `ProcessorRoleHost` 2 presences → 1 connection (single-hub dedup) | ✅ shipped 2026-05-16 | First non-trivial migration: 2-presence list (Consumer on in_hub + Producer on out_hub).  RoleHandler dedups by (broker_endpoint, broker_pubkey): single-hub → 1 connection, dual-hub → 2 connections.  Single-hub case is byte-identical wire trace to legacy.  Commit `4fe40d0`. |
| **Wave-B M8** | Processor dual-hub: 2 presences → 2 connections | ✅ code-validated 2026-05-16 — **PAYOFF UNLOCKED** | Dual-hub code path validated via: (a) L3 `RoleAPIBase_StartHandlerThreads_DualHub_E2E` proves BRC plumbing with real brokers + RoleHandler dedup; (b) M7 production code (`ProcessorRoleHost::worker_main_` Step 6) constructs 2 presences with potentially-different hubs and registers each REG via the per-channel routing helper; (c) the M3 dedup + M4d Class A routing already ship the per-presence wire-frame splitting.  Full L4 dual-hub processor binary test deferred to Wave-D — requires the L4 processor test infrastructure (producer + processor pipe) which doesn't exist today and is a pre-existing gap not specific to Wave-B.  See `docs/todo/MESSAGEHUB_TODO.md` "L4 processor + consumer test infrastructure". |
| **Wave-B M9** | Roles inherit `RoleHostFrame<HostT>`; `worker_main_` becomes `final` | ⏳ pending | No `RoleHostFrame` files in `src/` |
| **Wave-C** | Closure — finalise HEP cross-refs depending on Wave-B; archive transient drafts | ⏳ after M9 |  |
| **Wave-D** | Rewrite demos (`single-processor-shm`, `dual-processor-bridge`, `examples/`) | ⏳ after Wave-C |  |

**Arc B net status**: Wave A + M0..M3 + M4a-e + M5 + M6 + M7 + M8-code-validated done.  Every production role binary (producer / consumer / processor) runs on handler-mode end-to-end.  **M4f (delete legacy `broker_channel`) is the next concrete task; M9 (`RoleHostFrame<HostT>` template consolidation) follows.**

### Combined view — what blocks "dual-hub plh_hub" end-to-end

Both arcs feed the end-state.  Concretely:

| Blocker | Where it sits | Why it blocks |
|---|---|---|
| **Wave-B M3..M9** | Arc B, role side | Without `RoleHandler` + N-presence support, role binaries cannot register on more than one hub.  Even though `plh_hub` itself is ready (Arc A complete), a dual-hub deployment has no role binary to talk to it that way. |
| **HEP-CORE-0035 auth** | Hub character / security | Currently the `RoleIdentityPolicy` is a placeholder; CURVE is mandatory but key admission policy is not yet implemented.  Production-readiness gap, not a renovation blocker, but listed because it's a major remaining HUB-side item. |
| **HUB_TARGETED_ACK wire frame** | HEP-0033 §12.3.6 / §13 | Peer-to-peer message ACK frame is deferred; `on_peer_message` C++ surface is in place (Phase 8c) but the wire bit isn't wired.  Independent of dual-hub; would only affect federation use cases. |
| **HEP-0033 Phase 10 doc amendment** | Arc A docs | HEP-CORE-0019 §9 per-producer metrics tree shape and a cross-reference survey are listed as remaining.  Not blocking code; doc hygiene. |

### Next concrete task — Wave-B M4f (delete `broker_channel`)

**Scope**:
- Delete `pImpl->broker_channel` field + `set_broker_comm()` +
  `start_ctrl_thread()` (legacy single-hub ctrl thread path).
- Strip the helper fallback (`return broker_channel;` line in each
  of `resolve_bc_for_channel/role/band` — the helpers become pure
  `handler->brc_for_*()` lookups; null result becomes `nullptr`
  return, callers' existing `!bc` guards handle it).
- Remove the M4c fallback-view set/clear pair in
  `start_handler_threads`/`stop_handler_threads` (lines ~1135 +
  ~1207).
- Remove `deregister_from_broker`'s "is any broker around?" outer
  guard — handler null check + per-method guards inside the dereg
  helpers are sufficient.
- Audit + delete any remaining role-binary callers that still own
  a `broker_comm_` BRC + call `set_broker_comm` (producer /
  consumer / processor role hosts).  All role binaries must be on
  `start_handler_threads` by M4f land time.

**Blocked by**: M5..M7 — role hosts must migrate from legacy
`start_ctrl_thread` + owned-BRC pattern to `start_handler_threads`
+ handler-owned BRC before `broker_channel` can disappear.  M4f
is the final consolidation step after that migration.

### Wave-B M2 closure note (2026-05-15)

The original M2 description ("Consumer heartbeat tick removed")
was wrong.  The actual bug spanned five gaps that all hit the
same root: per-presence FSM mechanics were producer-only across
the stack even though `RolePresence` rows are role-type-keyed.
Shipped as a 3-commit sequence; full breakdown is in the M2 row
above and in `role_host_template_design.md §14.2`.

### Side-arc cleanup waves completed (parallel to / between renovation phases)

These addressed bugs and design issues revealed DURING the
renovation.  They are substrate cleanups, NOT renovation phases.
None of them belongs to Arc A or Arc B.

| Side-arc label | What it did | Commit(s) | Note |
|---|---|---|---|
| Wave-M2 MP1-MP5 | Multi-producer `ChannelEntry` refactor (`ProducerEntry` vector) | `b285628`..`416cbec` | Data-shape prereq for Wave-B's multi-hub presence model |
| Wave-M2.5 | Controlled-access API on `ChannelEntry` | `416cbec` | Closed 2026-05-11 |
| **Wave-M3** (≠ **Wave-B M3**) | `RoleEntry` / `RolePresence` controlled-access API | `0fc942f` | Closed 2026-05-11.  Naming collision warning: this is the side-arc, not the Arc-B phase. |
| M1.2 | Atomic channel teardown — retired `FORCE_SHUTDOWN`, `Closing`, channel-side FSM | `a41ce71` | Per-presence FSM became single source of truth |
| M1.4 | Retire `metrics_store_` + `METRICS_REPORT_REQ` | `4e902e1` | Metrics live on `RolePresence::latest_metrics` |
| M1.5 | `on_channel_closing` script callback | `c177c99` | Closed 2026-05-14 |
| MD1 | Role teardown UAF — ThreadManager Thread Shutdown Contract | `42092cb`..`4a5347c` | Closed; HEP-CORE-0031 §4.1 |
| MD1.5 | ThreadManager master/peer drain ordering | `eefa260` + `1f1eac4` | Post-MD1 follow-up |
| Pattern-3 migration wave | 21 test files migrated off `SetUpTestSuite`-`LifecycleGuard` antipattern | `6dfb86d`..`1ed9cc8` | Closed 2026-05-14 |

### Deferred polish (bug-/caller-triggered, not blocking renovation)

| Item | Trigger | Scope |
|---|---|---|
| Wave-M2.5 step 7 | Concrete bug surfaces from a new direct-access site to `ChannelEntry` private state | Privatize `ChannelEntry` state-bearing fields. `controlled_access_api_design.md` §7 step 7 |
| Wave-M2.5 step 2d | First caller asks for a deferred sugar method | `set_invariant_schema` / `set_invariant_transport` / `observable(roles_map)` / `is_alive(roles_map)` / span accessors |
| Wave-M2.5 step 8 / HEP-0033 Phase 10 | Land alongside HEP-0019's next edit | HEP-0019 §9 per-producer metrics tree shape + cross-reference survey |
| HEP-CORE-0035 auth implementation | Independent — production-readiness gap | 7-phase plan in HEP-0035 §8.  CURVE + ZAP pubkey allowlist + federation cross-trust delegation |
| `HUB_TARGETED_ACK` wire frame | Federation use case needs it | HEP-0033 §12.3.6 / §13.  C++ augment_peer_message surface in place; wire frame deferred |

---

**[Historical note]** The block previously here (Wave M2
multi-producer refactor with MP1-MP6 phases, open design decisions,
roadmap dependency graph, etc.) was superseded by the
"Side-arc cleanup waves completed" table earlier in this section
on 2026-05-15.  All Wave M2 / MP1-MP5 work is closed; MP6
(originally labeled "federation/dual-hub") was a mis-attribution
that confused HEP-CORE-0022 federation with Wave-B M8 dual-hub
presence — see the "Label hygiene" + "Combined view" sections
above.  Detail for the closed Wave M2 work lives in commits
`b285628`..`416cbec` and the `REVIEW_WaveM2.5_*` doc series.

---

### Snapshot — 2026-05-10 EARLIER (M1.2 Phase 4 + L3 broker test migration COMPLETE)

**Full suite: 1782/1782 green** at HEAD `4e30618`.  Branch `feature/lua-role-support`.

**Major closures this session:**

1. **M1.2 Phase 4** — readers now derive `ChannelObservable` from
   producer-presence per HEP-CORE-0023 §2.2.  `hub_state.hpp` gained
   `observe_channel(c, snap)` helper; 7 reader sites migrated; new
   `channel_to_json(c, obs)` overload emits both legacy `status` and
   protocol-defined `observable` for the M1.2 transition window.
2. **HEP-CORE-0007 protocol-doc unification** — 5 severe + 7 moderate
   + 1 minor doc/code drift findings closed.  ERROR field renamed
   `error` → `error_code`; DISC_PENDING `reason` enumerates
   `awaiting_first_heartbeat` + `heartbeat_stalled`; CHANNEL_LIST_ACK
   uses `observable`; ROLE_PRESENCE_REQ + ROLE_INFO_REQ unified to
   `role_uid`; new §12.4a Error Code Taxonomy enumerates 30 codes.
3. **`BrokerRequestComm` API harmonization (Bucket C)** — every
   request-reply method returns `optional<json>` carrying the
   broker's response body; `nullopt` means transport failure.
   `RoleAPIBase` wrappers harmonized.  Production callers + tests
   updated.
4. **L3 broker test cluster migration off mock-host pattern** —
   8 files (`broker.cpp`, `broker_admin`, `broker_consumer`,
   `broker_health`, `broker_protocol`, `broker_request_comm`,
   `broker_schema`, plus the deleted `broker_shutdown.cpp`) migrated
   from `BrokerHandle`/`LocalBrokerHandle` mock-host scaffolding to
   real `HubHost`.  Two production gaps surfaced and fixed:
   (a) `HubHost` now wires `<hub_dir>/schemas/` per HEP-CORE-0034 §12,
   (b) `HubBrokerConfig::checksum_repair_policy` field exposed per
   HEP-CORE-0007 §12.4.

**Code review findings (2026-05-10) — strand plan for next moves.**
A thorough 3-agent code review at HEAD identified 4 severe + 9
moderate + 2 minor issues.  The remediation clusters into 5 strands;
recommend pulling in this order (smallest blast radius first):

| Strand | Scope | Severity | Effort |
|---|---|---|---|
| **S1** Severe fixes — small immediate | (a) 4 false-pass `EXPECT_TRUE(brc.deregister_*())` patterns in worker tests; (b) ROLE_PRESENCE_REQ + ROLE_INFO_REQ error-envelope unification (use `make_error("MISSING_ROLE_UID", ...)`) | 2 severe | < 1 hour |
| **S2** M1.2 Phase 8 + observable coverage | `BrokerProtocolTest.ConsumerHeartbeat_DoesNotRefreshProducerPresence` (canonical regression — HEP-0019 §2.3); `DiscReq_ChannelStalled_ReturnsDiscPendingWithReason` (HEP-0023 §2.2 stalled case); atomic-teardown contract assertion (HEP-0023 §2.1) | 1 severe + 2 moderate | 0.5–1 day |
| **S3** Error-code taxonomy coverage | New `tests/test_layer3_datahub/test_datahub_error_codes.cpp` — 24 missing error_code tests per HEP-0007 §12.4a (only 6 of 30 codes have dedicated coverage today) | 1 severe (24 codes) | 1–2 days |
| **S4** M1.2 Phase 5-7 production cleanup | Phase 5: drop legacy writes in `_on_heartbeat`. Phase 6: delete `ChannelEntry.{status, last_heartbeat, state_since}`, `RoleEntry.{state, last_heartbeat, latest_metrics, metrics_collected_at}`, `ChannelStatus` enum, `_update_role_*` mutators, `observable_from_legacy_status` shim, dual channel-JSON serializer in `handle_channel_list_req`. Phase 7 (M1.3): retire FORCE_SHUTDOWN-as-grace-escalation, `closing_deadline`, `grace_heartbeats`, `effective_grace()`, `Closing` state arms. | All planned cleanup | 2–3 days |
| **S5** Coverage broadening (lower priority) | HEP-0034 §12 multi-file hub-globals; positive-path `close_channel` + `broadcast_channel` admin RPCs; processor two-presence asymmetric failure; HEP-0030 band protocol round-trip | 4 moderate | 1 day |

**Recommended sequence (original plan):** S1 → S2 → S4 (large but on
the existing M1.2 wave plan; closes phases 5-8) → S3 (error-code
coverage, can parallelise with S4) → S5.

**Actual outcome by 2026-05-12:** S1 shipped (S1a / S1b commits); S2
shipped (S2c atomic-teardown assertion landed); **S4 shipped in the
M1.2 atomic deletion sweep commit `a41ce71`** (Phase 5+6+7 collapsed
into one commit; went further than the strand plan — `FORCE_SHUTDOWN`
was REMOVED WHOLESALE rather than rewired as best-effort
notification); **M1.4 shipped 2026-05-11** (commit `4e902e1` + 4
follow-on audit/test commits); Wave M3 + Wave M2.5 shipped in
between.  S3 + S5 remain partially open as opportunistic coverage
expansion.

**Next-up milestones (re-framed 2026-05-12):**
- **MD1** — role teardown use-after-free race fix (chain reordered
  to land MD1 before M1.5 — see TODO_MASTER §"Deferred items" above).
- **M1.5** — `on_channel_closing` callback + auto-stop (re-framed
  from the original "FORCE_SHUTDOWN handler" framing; see
  `docs/tech_draft/M1.5_channel_closing_redesign_2026-05-12.md`).
- **Wave B M8 / MP6** — federation/dual-hub; verifies H43 (still open
  as of 2026-05-12 per broker_service.cpp).
- M2-M9 in `docs/tech_draft/role_host_template_design.md` §14 follow
  after Wave B M8.

**Detailed findings + suggested assertion shapes:** see
`docs/todo/TESTING_TODO.md` §"Code review findings (2026-05-10)".

---

### 2026-05-07 — Python lifecycle redesign (CLOSED 2026-05-07/08)

**Status:** complete and committed in `49b65e5` "Wave A.5 — Python
lifecycle redesign + Tier 1 callback cache + opt-in GIL release".
PythonInterpreter is a dynamic lifecycle module; engine constructed
on worker thread; Tier 1 has-callback cache landed; opt-in
GIL-release-during-wait flag added for Flask/asyncio compat.  Below
left in place for archival; the original problem statement / pending
items list is no longer load-bearing.

### 📜 2026-05-07 — ARCHIVED SNAPSHOT (was "HIGHEST PRIORITY: Python lifecycle redesign (in progress)")

> Snapshot preserved verbatim for historical record only.  The "Pending"
> list below was load-bearing on 2026-05-07; **all 7 items closed in
> commit `49b65e5` (see the CLOSED summary above)**.  Subsequent
> milestones referenced ("M1.1 / M1.2 / M1.3 / M2 / M3-M9") have also
> all advanced — M1.1 → M1.4 + Wave M2.5 + Wave M3 shipped 2026-05-10
> through 2026-05-11.  Use the active sections at the top of this file
> for current state; this snapshot is for "what did we know on
> 2026-05-07" archaeology only.

**Status**: partially landed; MUST finish before resuming any other work.

**Problem**: `PythonEngine`'s class-level `py::object{py::none()}` member-default initializers fire during `make_unique<PythonEngine>()` on `main()`, before the embedded CPython interpreter has been initialized. This calls `pybind11::handle::inc_ref()` without a held GIL on a stale `Py_None`, producing the GIL-held assertion fail visible in stderr as `pybind11::handle::inc_ref() is being called while the GIL is either not held or invalid`. Causes ~25% intermittent failure rate of `PlhHubCliTest.RoundTrip_PlhHubKeygenAndRunPlhRoleRegisters`. Confirmed pre-existing on the prior commit by git-stash + re-run (so unrelated to M1.1 per-presence work).

**Design (locked, in HEP-CORE-0011 §"Engine Construction Lifecycle")**:
- New dynamic lifecycle module `pylabhub::scripting::PythonInterpreter` owns `py::scoped_interpreter` (process-singleton). Lazily registered + loaded on first need; refcounted; unloaded on last release.
- `PythonEngine` is a plain C++ class. Constructor calls `ensure_python_interpreter_loaded()` and `PLH_PANIC`s if it returns false. Destructor calls `release_python_interpreter()`.
- Engine construction moves from `main()` to `worker_main_()` Step 0 — the worker thread becomes the GIL holder (current model preserved). `HostFactory` signature drops the `unique_ptr<ScriptEngine>` parameter.
- Lua / Native deployments: PythonInterpreter module never registered; zero Python startup cost.
- Python-home resolution kept as the existing 3-tier chain (env `PYLABHUB_PYTHON_HOME` → `<install>/config/pylabhub.json` → `<install>/opt/python` standalone default). `pylabhub.json` is dead code today (no build artifact creates it) but kept as a defensive escape hatch. **Deferred TODO**: a process-level platform-config object (e.g. `pylabhub::platform::config`) loaded once at startup, holding `python_home` plus future cross-cutting platform settings.

**Done (in this branch, uncommitted)**:
- `engine_host.cpp` shutdown race fix (drain BEFORE reset) — confirmed kills the HubLuaIntegrationTest SIGSEGV.
- HEP-CORE-0011 §"Engine Construction Lifecycle" added (Mermaid + ctor/dtor pattern + HostFactory rationale).
- HEP-CORE-0001 owner-managed-teardown counter-example for the new module.
- IMPLEMENTATION_GUIDANCE.md "Engine construction lives on the worker thread".
- `src/scripting/python_interpreter_module.hpp` and `.cpp` written (3-tier resolution ported from `python_engine.cpp:72`; `pi_startup` constructs `py::scoped_interpreter`; `pi_shutdown` finalizes; `std::call_once` + atomic refcount).
- `src/scripting/CMakeLists.txt` updated.

**Pending (must finish)**:
1. `python_engine.hpp/.cpp` — drop `interp_` member; ctor calls `ensure_python_interpreter_loaded()` + `PLH_PANIC`; dtor calls `release_python_interpreter()`; `init_engine_` drops `Py_InitializeFromConfig` block; keep venv activation (still per-engine).
2. `role_registry.hpp` — `HostFactory` signature change.
3. `plh_role_main.cpp`, `plh_hub_main.cpp` — drop engine construction.
4. `producer_role_host.{hpp,cpp}`, `consumer_role_host.{hpp,cpp}`, `processor_role_host.{hpp,cpp}` — ctors drop engine param; `worker_main_` Step 0 constructs engine.
5. `hub_host.{hpp,cpp}` + `hub_script_runner.cpp` — same shape.
6. Tests adapted (`tests/test_layer3_datahub/test_hub_lua_integration.cpp`, `test_hub_python_integration.cpp`, plh_role + plh_hub fixtures, etc.).
7. Build clean; full suite green; mutation-verify the GIL-violation fix.

**After this lands**, resume in order:
- Commit M1.1 (per-presence keying — code + test passing, uncommitted).
- M1.2 (move FSM from `ChannelEntry` to `RolePresence`; drop `ChannelEntry.status`/`last_heartbeat`; add `ConsumerHeartbeat_DoesNotRefreshProducerPresence` + `ChannelEntry_HasNoStoredFSMFields` tests).
- M1.3 (retire `FORCE_SHUTDOWN` + `grace_heartbeats` + `Closing` channel state; rewrite shutdown tests).
- M2 (retire consumer heartbeat tick).
- M3-M9 per `docs/tech_draft/role_host_template_design.md` — covers the dual-hub processor scenario fix (B3-B5 in M8).

---

### Snapshot — 2026-05-04 (Phase 7 CLOSED — Commit E rejected by design)

**Full suite: 1733/1733 green** (last full re-run at `c94f130`,
plus this commit's removals + test deletions land at the same
green count after the eval-related L2 tests are dropped).
Branch `feature/lua-role-support`.

**🎯 HEP-0033 Phase 7 — COMPLETE.**  All D-track sub-commits shipped
(D1 / D1.5 / D2 / D3 / D4 + S1/S2/S5/S7 review fixes + loop semantics
revision).  **Commit E was rejected by design 2026-05-04** — the
prior scope (`AdminService::exec_python` admin RPC) was removed
entirely rather than wired.  Security review found that arbitrary-
code-over-wire would be the first non-curated state-affecting RPC
on the hub; we deliberately don't introduce that threat class.  See
HEP-0033 §17.1 "No remote code injection" for the policy.  Operator
scripting access will be provided via the future Python SDK that
composes structured admin RPCs locally on the operator's host (see
`docs/todo/API_TODO.md` "pylabhub Python client SDK").

Removals associated with the rejection:
  - `AdminService` `exec_python` deferred-method entry.
  - `HubHost::eval_in_script` (declaration + impl).
  - `HubScriptRunner::eval` (declaration + impl).
  - L2 test_hub_host eval-forwarding tests.
  - HEP-0033 §11.2 / §11.3 / §11.5 / §12.5 references.

The engine-level `eval` virtual + `process_pending_` queue stay as
internal C++ extensibility points — no external trigger remains, but
the cross-thread invariant is preserved against future C++ callers.

D-track sub-commits since the 2026-05-03 snapshot:

  - `b5f16f7` D1.6 — HubScriptRunner R1+S1+S2 (runner now actually runs).
  - `b5f5adc` D2.1 — single config owner per hub (Option E).
  - `a321727` D2.2 — HubHost wires HubScriptRunner.
  - `b507510` D2.3 — review fixes: role-parity shutdown + const
    `subscribe_*` on HubState (handler-list mutex was already
    `mutable`, removing the prior const_cast smell).
  - `6495507` D2.4 — review fixes (eval guard, uid capture, doc
    drift).
  - `9ab6ffb` D2.5 — D2.2 test rigor (timing bounds + mutation sweep).
  - `2230d39` D3.1 — `HubScriptRunner::worker_main_` engine setup
    wiring (initialize → load_script → build_api inline; mirrors
    role-side phase ordering).
  - `cfd388d` D3.2 — `LuaEngine::build_api_(HubAPI&)` + 3 hub
    closures (`api.log` / `api.uid` / `api.metrics`); `on_pcall_error_`
    null-safe across role/hub paths.
  - `69a6133` D3.3 — L3 integration test `HubLuaIntegrationTest`
    (HubHost + LuaEngine + real `init.lua`; 2 tests).
  - `78355d7` D4.1 — `PythonEngine::build_api_(HubAPI&)` +
    `src/scripting/hub_api_python.cpp` with
    `PYBIND11_EMBEDDED_MODULE(pylabhub_hub, m)`; force-link symbol
    keeps the static-archive linker from dropping the .o.
  - `92c82c4` D4.2 — L3 integration test `HubPythonIntegrationTest`
    (single TEST_F by design — pybind11 `scoped_interpreter` re-init
    in one process is unsafe).
  - `b8f7cd6` Static review S1 + S2 — `script_error_count()` null-
    safe across role/hub paths (both engines, was returning 0 on
    hub path); L3 timing bounds tightened (5/8 s → 1.5/2.5 s based
    on ~110–140 ms steady state).
  - `ce132d1` Static review S5 — extracted `json_to_py` /
    `py_to_json` from `python_engine.cpp` to private header
    `src/scripting/json_py_helpers.hpp` (`pylabhub::scripting::detail`
    namespace); adopted by all 4 `metrics()` bindings.
  - `8f4c957` Static review S7 — Lua-hub redundant callback-ref
    re-extract in `build_api_(HubAPI&)` removed (load_script is the
    single extraction site).

Remaining static-review notes (deferred):
  - **S3** — test boilerplate duplication between
    `test_hub_lua_integration.cpp` and `test_hub_python_integration.cpp`
    (~90% identical fixture).  Refactor when adding a 3rd integration
    test (likely Phase 8 or Python-subprocess pattern).
  - **S6** — force-link symbol fragility (theoretical: a future
    refactor could remove the `plh_register_hub_api_python_module`
    call without realizing it's load-bearing).  Verified working
    via `nm`; comments at both call sites flag the rationale.
    Hardening (whole-archive link or OBJECT library) deferred unless
    the symbol ever actually goes missing.

### Snapshot — 2026-05-03 (Phase 7 in flight — D1 + D1.5 shipped) — superseded by 05-04

**Full suite: 1709/1710 green at last full run** (`2a9dd3b`; one
flake on `Roles/PlhRoleInitTest.DefaultValues/producer` — fork+exec
race under heavy parallel load, individual re-run 3/3 PASS).
Branch `feature/lua-role-support`.

**🎯 HEP-0033 Phase 7 — IN FLIGHT.**  Sub-commits split for review hygiene:

  - `3a750e3` — Commit A: `script_host_traits<ApiT>` template + sibling
                 `build_api_(HubAPI &)` virtual on ScriptEngine + symmetric
                 `hub_api_` member.  Behavior-neutral refactor; 1702/1702
                 green post-commit.
  - `239078c` — Commit B: `HubConfig::timing()` accessor (mirrors
                 `RoleConfig::timing()`); hub script tick reuses the
                 existing `LoopTimingPolicy` enum + `TimingConfig` struct.
                 Hub init template emits `loop_timing: fixed_rate` /
                 `target_period_ms: 1000` defaults.
  - `4a1fc63` — Commit C: HubAPI class declared; `script_host_traits<HubAPI>`
                 specialization; explicit `template class EngineHost<HubAPI>;`;
                 private `HubScriptRunner final : EngineHost<HubAPI>` with
                 `worker_main_()` event-and-tick loop.  Reuses RoleHostCore
                 message queue + Phase 6.2b `hub_state_json` serializers.
                 Encapsulated (no public `host.script_runner()` accessor —
                 hub-side scripts don't need outside reach-through).
  - `2a9dd3b` — Commit D1: HubAPI gains `log` / `metrics` / `uid` mirroring
                 RoleAPIBase signatures.  L2 unit tests with affirmative
                 path-discrimination on log levels.  Surfaced + recorded
                 a Class A weakness in `LogCaptureFixture::ExpectLogWarn`
                 (permissive allowlist; doesn't enforce emission).
  - **(this commit)** — Commit D1.5: `LogCaptureFixture` gains strict
                 `ExpectLogWarnMustFire` / `ExpectLogErrorMustFire` variants;
                 13 L2 tests verify the framework facility itself
                 (using `EXPECT_NONFATAL_FAILURE` to capture inner
                 assertion failures).  TODO entry filed in
                 `docs/todo/TESTING_TODO.md` — migration of existing
                 `ExpectLog*` callers to strict where appropriate is
                 deferred per-fixture review.

**Phase 7 remaining sub-commits**:

  - **D2** — HubHost wires `HubScriptRunner` into startup/shutdown
             ordering (HEP §4.1 step 10 / §4.2 step 2); adds
             `host.eval_in_script(code)` wrapper for Commit E.
             L3 lifecycle test: script-disabled / script-enabled
             startup+shutdown clean (no script callbacks tested yet —
             engine bindings come in D3/D4).
  - **D3** — `LuaEngine::build_api_(HubAPI&)` override + 3
             `lua_api_hub_*` closures (log/metrics/uid).
             Parametric L3 dispatch tests with Lua script fixture
             (on_tick fires, on_role_registered fires when broker
             registers a role, api.log/metrics/uid round-trip).
  - **D4** — `PythonEngine::build_api_(HubAPI&)` override + new
             `src/hub/hub_api_python.cpp` defining
             `PYBIND11_EMBEDDED_MODULE(pylabhub_hub, m)` with HubAPI
             bindings.  Same parametric L3 tests, Python script
             fixture.
  - **E** — AdminService `exec_python` admin RPC: wires through
             `host.eval_in_script(code)` which forwards to script
             engine's `eval()`.  Closes one of the 6 deferred §11.2
             methods.

**🎯 Test-correctness audit CLOSED — 2026-05-02.**  All 204 inventory
rows ✅ FIXED or n/a across all four bug classes (A/B/C/D).
The trust gate of `REVIEW_TestAudit_2026-05-01.md` §3 is met;
both audit docs archived to `docs/archive/transient-2026-05-02/`.

**🟡 Open follow-up — `ExpectLog*` strict-migration**: see
`docs/todo/TESTING_TODO.md` § "Open 2026-05-03".  13 fixtures using
the permissive variants — per-needle review needed to convert to
`MustFire` where the warn/error is deterministic.  Race-conditional
emissions stay permissive.

---

### Snapshot — 2026-05-02 (Phase 6.2 closed) — superseded by 05-03

**Full suite: 1702/1702 green** (last verified post-`38591dc`).
Branch `feature/lua-role-support`.

**🎯 HEP-0033 Phase 6.2 (AdminService) CLOSED — 2026-05-02.**
All 10 of 16 §11.2 methods wired; 6 deferred with explicit
upstream-HEP citations.  Three sub-commits this batch:

  - `dd5ac0d` — HEP-0033 §11.5 error code catalog (closes §16 #10)
  - `c0408a8` — Phase 6.2b: 7 query methods + shared
                `utils/hub_state_json.{hpp,cpp}` serializers
  - `38591dc` — Phase 6.2c: 3 control methods (close_channel,
                broadcast_channel, request_shutdown)

Pre-implementation review `REVIEW_AdminService_2026-05-01.md`
fully closed; archived to `docs/archive/transient-2026-05-02/`.

23 L2 AdminService tests; mutation-verified per sub-phase.  The
L3 broker test suite already covers the data-plane interactions
(BrokerService tests); Phase 6.2c commits don't add separate L3
tests because the control methods delegate to existing,
already-tested broker mutators.

**🎯 Test-correctness audit CLOSED — 2026-05-02.**  All 204 inventory
rows ✅ FIXED or n/a across all four bug classes (A/B/C/D).
The trust gate of `REVIEW_TestAudit_2026-05-01.md` §3 is met;
both audit docs archived to `docs/archive/transient-2026-05-02/`.

  Last commits (2026-05-02 audit-closure batch):
`b9f125b` (final 2 Class D rows → n/a), `54f71ad` (4 broker-client
fixtures LogCaptureFixture), `4df2e8f` (4 broker fixtures
LogCaptureFixture + BrokerService::run mutation sweep), `e559d48`
(InboxQueueTest LogCaptureFixture), `8df739d` (zmq_poll_loop
LogCaptureFixture), `82a06b3` (schema_loader LogCaptureFixture),
`600a171` (role_host_core LogCaptureFixture), `84a2e8f`
(hub_zmq_queue LogCaptureFixture), `7783334` (inventory
consolidation: -21 rows), `9340228` (plh_role: hoist LifecycleGuard
above --init; wire L4 Class D gate, 4 files / 14 sites; mutation
LOGGER_ERROR in do_init verified red→green), `7fb2c48` (framework
gate finding documented).

Earlier sprint:
`9822ce4`/`536e129` (HEP-0033 §4 doc — phase FSM ratified), `70cd6cc`
(test name cosmetic), `0d728ea` (3-phase FSM on EngineHost + HubHost),
`a0fd3a8` (HubHost Phase 6.1 fix-up), `72da2db` (HubHost Phase 6.1b),
`e59bb90` (HubHost Phase 6.1a — HubState ownership).

Earlier this sprint:
`d60ddf2` (HEP-0034 Phase 1 — fingerprint includes packing), `8e1eadc`
(HEP-0033 G2.1 HubState skeleton), `e9fc8f6` (HEP-0033 G2 "broker as
single mutator" doc ratified), `139b4ca` (HEP-0033 G1 `RoleHostBase` →
`EngineHost<ApiT>` template), `399fbfc` (HEP-0032 Phase C ABI fingerprint).

**Actively open (in priority order):**

1. **HEP-CORE-0033 Hub Character implementation** — in progress.
   - G1 (host template): ✅ `RoleHostBase` = `EngineHost<RoleAPIBase>` (`139b4ca`).
   - G2 design: ✅ ratified; `HubState` sole-mutator-through-broker model
     (`e9fc8f6`).
   - G2.1 (HubState skeleton + entry types): ✅ compile-only landed (`8e1eadc`);
     17 L2 unit tests; primitive `_set_*` mutators.
   - G2.2 (broker absorption): ✅ G2.2.0–G2.2.3 shipped earlier this
     sprint; G2.2.4 (observability) partial — `_on_message_processed`
     in place; metrics_store_ absorption deferred.
   - **Phase 6.1 — HubHost concrete class**: ✅ shipped 2026-04-30 / 2026-05-01.
     `e59bb90` (HubState ownership externalized from broker), `72da2db`
     (HubHost class — `startup()`/`shutdown()`/`run_main_loop()`/
     `request_shutdown()`/`is_running()` + const accessors), `a0fd3a8`
     (rollback on partial startup, request_shutdown contract pin,
     init/shutdown step lists pinned in HEP §4.1/§4.2), `0d728ea`
     (3-phase FSM `Constructed → Running → ShutDown` shared with
     `EngineHost` — single-use after shutdown, retryable on failed
     startup, CAS-driven, idempotent on repeated calls).
     9 L2 HubHost tests + 12 L2 RoleHostBase tests + 3 L3 integration
     tests.
   - **Next**: Phase 6.2 (`AdminService` structured RPC — HEP-0033 §11).
     Then G2.3 / G2.4 (`HubAPI` read accessors and mutation wrappers —
     HEP-0033 §12) → G2.5 (`AdminService` shell using same broker
     mutators).
   - Naming grammar in HEP-CORE-0033 Appendix §G2.2.0b.
   - Remaining open spec items: see HEP-CORE-0033 §15 Phase 9 (L4 test
     infrastructure) + §16 items 9 + 10 (`reload_config` whitelist,
     admin RPC error catalog).  The original prereqs working notes are
     archived at `docs/archive/transient-2026-04-30/HUB_CHARACTER_PREREQUISITES.md`.
2. **HEP-CORE-0032 ABI check facility** — ✅ all three phases landed
   (`c91ae84` Phase A, `34255be` Phase B, `399fbfc` Phase C).
3. **Subtopic backlogs** (see §Subtopic TODO Documents below):
   - API/ABI: Phases 2-7 of the `PYLABHUB_UTILS_TEST_EXPORT` rollout,
     `std::function`/`std::optional` ABI fixes, C API helpers.
   - Platform: CI macOS/Windows jobs + clang-tidy pass, MSVC gaps.
   - Testing: Lua V2-fixture cleanup tail, worker-helper unification,
     Script-API live-vs-frozen contract work.
   - MessageHub: HEP-0033 ancillary items (system-level L4 tests, 6
     hub-facing L3 Pattern-3 conversions folded into HEP-0033 scope).

**Closed this session (2026-04-21 / 22):**
- HEP-CORE-0024 Role Binary Unification (all 22 phases).
- HEP-CORE-0033 Hub Character design ratified.
- L3 role-api tests Pattern-3 converted + deepened.
- L2 depth review closed (tracker `21.3.5`): Pattern-3 compliance,
  vacuous-test sweep, stderr-capture fixes, assertion-quality tighten.
- 6 tech drafts archived to `docs/archive/transient-2026-04-21/`.

**Ratified 2026-04-26:**
- **HEP-CORE-0034 Schema Registry** — owner-authoritative model; supersedes
  HEP-CORE-0016. Namespace-by-owner records, owner-bound eviction (no
  refcount), cross-citation rejected even on hash match,
  fingerprint includes packing. HEP-0016 marked Superseded. HEP-0033 §7/§8/§9.4/§14
  cross-referenced. HEP-0024 §3.1/§3.5 updated for role-side `schemas/` cache.

**HEP-0034 Phase 1 landed 2026-04-27 (commit `d60ddf2`):**
- Fingerprint correction (`compute_schema_hash` and `SchemaInfo::compute_hash`
  now include packing); `parse_schema_json` mandates explicit packing;
  `PYLABHUB_SCHEMA_BEGIN_PACKED` macro added for opt-in packed C++ structs;
  share/ JSON configs + role `--init` templates emit explicit packing.
  +4 tests covering aligned-vs-packed-distinct fingerprints. 1602/1602.
- Five implementation phases remain (see §Priority 2).

### Priority 0 (in progress — Phase 6.2 shipped 2026-05-02): HEP-CORE-0033 Hub Character

📍 **Status**: G1, G2 design + G2.1 + G2.2.0–2.2.3 + Phase 6.1 (HubHost
concrete class with phase FSM) + Phase 6.2 (AdminService — 6.2a/b/c
all shipped, 23 L2 tests, mutation-verified per sub-phase) shipped.
**Next: Phase 7 — `scripting::hub_lifecycle_modules()` + HubScriptRunner
(retire `PythonInterpreter`/`HubScript`/`hub_script_api`/`pylabhub_module`).**
📋 **Spec**: `docs/HEP/HEP-CORE-0033-Hub-Character.md` (normative — single source of truth)
📋 **Detail**: `docs/todo/MESSAGEHUB_TODO.md`

Replaces the deleted `pylabhub-hubshell` (legacy `src/hub_python/` stack
removed in the post-G2 cleanup pass) with a modern `plh_hub` binary
paralleling `plh_role`: composite `HubConfig`, `hub_cli`, `HubDirectory`,
`HubHost` + `HubState`, `AdminService` structured RPC, `ScriptEngine`-based
scripting + `HubAPI`, query-driven metrics (supersedes HEP-0019 §3-4). 10
implementation phases; see HEP §14.

### Priority 0 (DONE — 2026-04-21): HEP-CORE-0024 — Role Binary Unification COMPLETE

📍 **Status**: All 22 phases ✅; `test_layer4_plh_role/` 71 tests passing; full suite **1456/1456**
📋 **Branch**: `feature/lua-role-support`

- [x] Phases 15-17: `RoleHostBase` abstract class; `RoleRuntimeInfo` + `register_runtime()`; per-role bootstrap
- [x] Phase 18: `role_cli` extended with `--role` / `--log-maxsize` / `--log-backups`; mode exclusion
- [x] Phase 19: `plh_role` unified binary + CMake target
- [x] Phase 20: `pylabhub-producer/consumer/processor` binaries + per-role `CMakeLists.txt` + `*_main.cpp` deleted
- [x] Phase 21: L4 tests unified at `tests/test_layer4_plh_role/` (parametrized by role tag; 71 tests covering --init / --validate / --keygen / CLI error paths + round-trip init↔validate)
- [x] Phase 22: README / Deployment docs updated for `plh_role`

System-level L4 tests (broker round-trip, pipeline, channel broadcast, hub-dead,
inbox) are **out of scope** for HEP-0024 — they are system-integration tests
that require a hub binary, which is HEP-CORE-0033 work. Tracked in
`docs/todo/MESSAGEHUB_TODO.md`.

### Priority 0 (DONE — 2026-04-17): HEP-0024 Phases 13-14 — Logging
📍 **Status**: Complete; **1290/1290 tests**
📋 **Branch**: `feature/lua-role-support`

- [x] `RotatingFileSink::Mode::{Numeric,Timestamped}` two-mode extension
- [x] `RotatingLogConfig::timestamped_names` flag (default false → Numeric)
- [x] Timestamped filename: `<base>-YYYY-MM-DD-HH-MM-SS.uuuuuu.log` (lex-sort = chron-sort)
- [x] `format_tools::formatted_time(tp, use_dash_spacer=true)` dash-spacer variant
- [x] `LoggingConfig` category in `RoleConfig` + strict key whitelist
- [x] `max_backup_files` semantics: `>=1` explicit, `-1` → `kKeepAllBackups` sentinel, `0` invalid
- [x] `RotatingLogConfig` default aligned to 5 (matches `LoggingConfig`)
- [x] Producer `out_shm_secret=0` boilerplate removed from init template
- [x] `init_directory` stderr messages prefixed `init_directory: error:`
- [x] 10 new L2 LoggingConfig tests; 2 L1 `formatted_time` dash-spacer tests
- [x] HEP-0024 §12 (CLI↔Config boundary), §13 phase table updated

Next (HEP-0024 Phases 15-22): `RoleHostBase` abstract class, `RoleRuntimeInfo` +
`register_runtime()`, role CLI `--role`/`--log-*` flags, `plh_role` unified
binary, per-role binary deletion, L4 test migration, docs.

### Priority 0 (DONE — 2026-04-15/16): L3.γ/ζ Role Unification + ZMQ + Flexzone + Docs
📍 **Status**: Complete; **1278/1278 tests**
📋 **Branch**: `feature/lua-role-support`

L3.γ — Role unification:
- [x] A5i: role host worker_thread_ under ThreadManager ✅
- [x] A6.1–A6.3: delete hub::Producer/Consumer; abstract-only queue ownership ✅
- [x] ZMQ: cppzmq migration + shared ZMQContext module (all consumers) ✅
- [x] BrokerService::run() migrated to shared ZMQContext ✅
- [x] ThreadManager: drain(), no-op lifecycle thunk, instance_id, HEP-0031 ✅
- [x] Deprecated shims removed; shutdown order fixed ✅

L3.ζ — Flexzone:
- [x] InvokeTx/InvokeRx stripped to slot-only; .fz removed from PyTxChannel/PyRxChannel ✅
- [x] Python api.flexzone(side) init-time cache (all 3 API classes) ✅
- [x] Lua api.flexzone(side) with side arg + correct Rx ref selection ✅
- [x] Native engine: cached context, bridge-populated plh_tx_t.fz ✅
- [x] L3 tests T2/T3 (role-level SHM flexzone round-trip) ✅
- [x] has_out_fz/has_in_fz → has_tx_fz/has_rx_fz rename ✅
- [x] Demo scripts updated to 3-arg signature ✅

Documentation cleanup (2026-04-16):
- [x] 12 tech drafts archived with verified merges into HEPs ✅
- [x] HEP-0002 §17.2 rewritten (queue abstraction + flexzone access) ✅
- [x] HEP-0008 §2.2 + §11 (timeout formula + config single-truth) ✅
- [x] HEP-0011 (unified data loop: CycleOps, 14-step lifecycle) ✅
- [x] HEP-0016 §11.0 (schema layer Mermaid diagram) ✅
- [x] HEP-0030 appendix (band design rationale) ✅
- [x] HEP-0031 created (ThreadManager — Layer 2 utility) ✅

Deferred:
- [ ] L3 test T4: processor dual-flexzone distinctness
- [ ] Extract `create_zmq_socket()` factory (7 files, 1-line pattern)
- [ ] SequenceTracker utility (20 LOC duplication)

### Priority 0 (DONE — 2026-04-04/05): RoleAPIBase Refactor + Lifecycle + API Consistency
📍 **Status**: All phases complete; **1323/1323 tests**
📋 **Branch**: `feature/lua-role-support`

- [x] RoleAPIBase: pure C++ unified role API in pylabhub-utils ✅
- [x] All 3 API classes delegate to RoleAPIBase via composition ✅
- [x] RoleContext eliminated — engine uses api_ pointer directly ✅
- [x] All 3 engines (Python/Lua/Native) migrated: ctx_ → api_-> ✅
- [x] Lifecycle integration: engine_lifecycle_startup() replaces manual init ✅
- [x] ChannelSide enum (Tx/Rx): spinlock + schema size with explicit side ✅
- [x] Schema size API: slot_logical_size, flexzone_logical_size (all engines) ✅
- [x] Inbox packing: schema.packing as sole source, shared setup_inbox_facility() ✅
- [x] align_to_physical_page() utility + assert in lifecycle callback ✅
- [x] Counter rename: C++ internals + JSON keys + Lua/Python/Native consistent ✅
- [x] Native engine API v2: spinlock, schema sizes, messaging, C++ RAII ✅
- [x] HEP-0011 complete rewrite ✅
- [x] Multi-process spinlock test through RoleAPIBase ✅
- [x] Schema size tests (4 complex schemas, aligned/packed) ✅
- [x] Native engine v2 tests (counters, schema size, spinlock count) ✅
- ProducerAPI/ConsumerAPI/ProcessorAPI kept as Python translation layer (by design)

### Priority 0 (DONE — 2026-04-02/03): DataBlock Ownership + Schema Validation + Checksum + SE-04
📍 **Status**: All ownership steps done (except RAII rewrite); **1279/1279 tests**
📋 **Branch**: `feature/lua-role-support`

Ownership refactor (all steps except template RAII):
- [x] ShmQueue owns DataBlock internally (create_writer/create_reader) ✅
- [x] Producer/Consumer: spinlock/identity delegating methods ✅
- [x] All `->shm()` external callers migrated + `shm()` public accessor removed ✅
- [x] `item_size`/`flexzone_size` removed from Options + `schema_slot_size_` from role hosts ✅
- [x] Schema size cross-validation in all 3 engines (vs compute_field_layout) ✅
- [x] Native engine: `native_sizeof_<T>` required export ✅
- [x] Checksum policy fix: Manual no-stamp + always-verify (SHM+ZMQ+Inbox unified) ✅
- [x] 8 new checksum policy tests (hub API + ZmqQueue + InboxQueue) ✅
- [x] `to_field_descs()` utility, `shm_blocks` → `shm_info` rename ✅
- [x] SE-04: Lua API parity complete (shm_info added, only as_numpy is Python-specific) ✅
- [ ] Template RAII rewrite on QueueWriter/QueueReader (Group D — separate sprint)

### Priority 0 (DONE — 2026-03-29/30): Metrics, Timing, Checksum & Config Unification
📍 **Status**: All items complete; **1181/1181 tests**
📋 **Branch**: `feature/lua-role-support`

Metrics systematization:
- [x] ContextMetrics: private fields, all-atomic, accessor API, renamed to `context_metrics.hpp` ✅
- [x] X-macros: queue (12 fields), loop (4), inbox (4) with JSON/pydict/Lua adapters ✅
- [x] ZmqQueue adopts ContextMetrics (replaces individual atomic counters) ✅
- [x] Lua `api.metrics()` — full hierarchical table ✅

Timing unification:
- [x] LoopTimingParams: single config truth, strict validation ✅
- [x] `loop_timing` required in config (no implicit default) ✅
- [x] DataBlock timing removal: `set_loop_policy` deleted, `LoopPolicy` enum deleted ✅
- [x] `configured_period_us` moved from queue to loop level ✅
- [x] `queue_period_us` removed from Options ✅

Checksum policy unification:
- [x] Per-role config (`"checksum": enforced/manual/none`) ✅
- [x] Unified queue API: `set_checksum_policy`, `update/verify_checksum`, flexzone variants ✅
- [x] InboxQueue/InboxClient checksum policy support ✅

Inbox protocol completion:
- [x] Consumer registration: CONSUMER_REG_REQ carries inbox fields ✅
- [x] ROLE_INFO_REQ: searches both producer and consumer entries ✅
- [x] RoleContext: typed pointers (void* removed), inbox_queue added, checksum_policy added ✅

Config validation:
- [x] Config key whitelist: validates all JSON keys at parse time ✅

Documentation:
- [x] HEP-0008 full rewrite ✅
- [x] HEP-0027 Inbox Messaging spec ✅

Bug fixes:
- [x] flexzone checksum returns true when no flexzone ✅
- [x] `init_metrics()` vs `reset_metrics()` separation (sequence state preserved) ✅

### Priority 0 (DONE — 2026-03-23): ScriptEngine + Config Module Redesign
📍 **Status**: ScriptEngine refactor + config module redesign COMPLETE; 1115/1115 tests
📋 **Branch**: `feature/lua-role-support`
📋 **Active review**: `docs/code_review/REVIEW_ConfigAndEngine_2026-03-21.md`

ScriptEngine refactor (DONE 2026-03-20):
- [x] `ScriptEngine` abstract interface + `LuaEngine` + `PythonEngine` ✅
- [x] Unified role hosts: `ProducerRoleHost`, `ConsumerRoleHost`, `ProcessorRoleHost` ✅
- [x] `RoleHostCore` encapsulated: all metrics/shutdown private with proper methods ✅
- [x] API classes take `RoleHostCore&` in constructor ✅
- [x] 38 Lua + 38 Python engine L2 tests ✅
- [x] Legacy host code removed (~5000 lines) ✅
- [x] Code review SE-01/02/05/06/10/11/12/13/14/15 fixed ✅ 2026-03-21

Config module redesign (DONE 2026-03-23):
- [x] Phase 1: Categorical config headers + shared parsers ✅ cb7e4b5
- [x] Phase 2: RoleConfig unified class with JsonConfig backend ✅ 36f1902
- [x] Phase 3: Migrate role hosts + mains to RoleConfig ✅ c0100d1, a445dca
- [x] Phase 4: Remove monolithic config structs ✅ 9dbfa59
- [x] Dead field cleanup: ValidationConfig merged, period_ms → configured_period_us ✅ cc4c581, f2a805e
- [x] HEP/README doc sync ✅ fcaaf33

Naming cleanup (DONE 2026-03-23):
- [x] ActorVault → RoleVault + vault integrated into RoleConfig ✅ 38172de
- [x] KnownActor → KnownRole, all "actor" terminology → "role" ✅ 154c1c3
- [x] `generate_uid(prefix, name)` unified core ✅ 154c1c3

Deferred (after code stabilizes):
- [ ] SE-03: HEP-0011 rewrite for composition model
- [ ] SE-04: Lua API parity (design decision pending)
- [ ] SE-07: --validate implementation
- [ ] SE-08: HEP-0018/0015 class name refs update
- [ ] Engine thread model: 6 phases — invoke/eval, cross-thread dispatch, shared state, NativeEngine (see `docs/tech_draft/engine_thread_model.md`)
- [ ] ScriptEngine cleanup: RoleHostCore encapsulation (CR-03), RoleContext const char*→string. (Hubshell migration superseded — legacy `src/hub_python/` deleted in post-G2 cleanup; replacement is HEP-CORE-0033 §15 Phase 7 ScriptEngine integration on the new `plh_hub` binary.)

### Code Review (CLOSED — 2026-03-17): REVIEW_FullStack_2026-03-17
📋 `docs/code_review/REVIEW_FullStack_2026-03-17.md` — 30 non-Lua findings: 17 FIXED, 8 ACCEPTED, 4 DEFERRED, 1 Lua WIP; **1184/1184 tests**
Key fixes: DataBlockMutex try_lock_for(-1) on both platforms, SharedSpinLock generation guard, StopReason/critical_error unified in RoleHostCore, SHM ownership principle (HEP §2.2), sequential_sync parser, JsonConfig retry-with-timeout

### Code Review (CLOSED — 2026-03-15): REVIEW_Codex_2026-03-15
📋 Archived to `docs/archive/transient-2026-03-15/` — ✅ CLOSED; 4 doc fixes applied, 4 code items routed to API_TODO, 2 routed to PLATFORM/TESTING_TODO, 1 false positive, 1 pre-fixed

### Code Review (CLOSED — 2026-03-14): REVIEW_CodeAndDocs_2026-03-14
📋 Archived to `docs/archive/transient-2026-03-14/` — ✅ CLOSED; 8 fixed, 4 accepted; **1166/1166 tests**

### Priority 0 (CLOSED — 2026-03-14): HEP-0025 System Config & Python Environment
📍 **Status**: ✅ CLOSED — HEP written, config/venv support implemented; **1166/1166 tests**

Completed 2026-03-14:
- [x] `tools/plh_pyenv.py` + bash/PowerShell wrappers; CMake `stage_pyenv_tools` target ✅ (renamed from `pylabhub-pyenv` on 2026-04-17)
- [x] `python_venv` config field in ProducerConfig/ConsumerConfig/ProcessorConfig + 6 tests ✅
- [x] `PythonScriptHost`: 3-tier `resolve_python_home()`, venv activation via `site.addsitedir()` ✅
- [x] `config/pylabhub.json` system config (python_home key) ✅
- [x] HEP-CORE-0025 written (full spec) ✅
- [x] README_Deployment.md §12 (Python environment guide) + `python_venv` in all config tables ✅

### Priority 0 (CLOSED — 2026-03-12): HEP-0024 Role Directory Service
📍 **Status**: ✅ CLOSED — Phases 1–3, 5, 6, 8 implemented; Phases 4, 7 deferred; **1107/1108 tests**
📋 **Details**: `docs/HEP/HEP-CORE-0024-Role-Directory-Service.md`, `docs/todo/API_TODO.md`

Completed 2026-03-12:
- [x] `RoleDirectory` class: `open/create/from_config_file`, path accessors, `resolve_hub_dir`, `hub_broker_endpoint/pubkey`, `warn_if_keyfile_in_role_dir` (0700 vault, security warning) ✅
- [x] `role_cli.hpp` header-only: `RoleArgs`, `parse_role_args`, `resolve_init_name`, `get_role_password`, `get_new_role_password` ✅
- [x] All 3 `Config::from_directory()` migrated to use `RoleDirectory`; hub triplication eliminated ✅
- [x] All 3 `main.cpp` migrated to `role_cli::parse_role_args()` + `RoleDirectory::create()` in `do_init()` ✅
- [x] `role_main_helpers.hpp` password helpers delegated to `role_cli.hpp`; duplicates removed ✅
- [x] 22 L2 tests: `RoleDirectoryTest` (18) + `RoleCliTest` (8) + security warning (4) → 1107/1108 ✅

### Priority -1 (CLOSED — 2026-03-12): Hub-Dead ZMQ Monitor + StopReason Fix
📍 **Status**: ✅ CLOSED — ZMQ socket monitor path implemented; StopReason ordering restored; **1120/1120 tests** (2026-03-12: +3 tests for MR-09/LOW-2/MR-08)

**Completed 2026-03-12:**
- [x] Restored `handle_command(ConnectCmd&, ...)` in `messenger.cpp` (accidentally deleted by prior agent) ✅
- [x] Restored `send_heartbeats()` in `messenger.cpp` (accidentally deleted by prior agent) ✅
- [x] Replaced timer-based hub-dead (`m_last_broker_recv_epoch_ms_`/`hub_last_contact_ms()`) with ZMQ socket monitor: `zmq_socket_monitor()` + PAIR socket polling `ZMQ_EVENT_DISCONNECTED`; `process_monitor_events()` / `close_monitor()` ✅
- [x] ZMTP heartbeat sockopts set BEFORE `connect()` in ConnectCmd handler (`ZMQ_HEARTBEAT_IVL=5s`, `ZMQ_HEARTBEAT_TIMEOUT=30s`) ✅
- [x] `hub_dead_timeout_ms` removed from all 3 role configs (ProducerConfig, ConsumerConfig, ProcessorConfig) ✅
- [x] `StopReason` enum corrected: `Normal=0, PeerDead=1, HubDead=2, CriticalError=3` (prior agent collapsed HubDead) ✅
- [x] `stop_reason()` switch in all 3 API classes restored (cases 1=peer_dead, 2=hub_dead, 3=critical_error) ✅
- [x] All 3 script hosts: `on_hub_dead()` wired (producer: out_messenger_; consumer: in_messenger_; processor: BOTH in+out messengers with shared lambda) ✅
- [x] All 3 script hosts: `on_hub_dead(nullptr)` deregistered in `stop_role()` ✅
- [x] Static code review: frame-size check corrected `< 2` → `< 6`; monitor setup failure LOG_WARN → LOGGER_ERROR with errno ✅

### Priority -1 (CLOSED — 2026-03-11): Peer/Hub-Dead Monitoring + MonitoredQueue
📍 **Status**: ✅ CLOSED — all items implemented and code review fixes applied; **1062/1062 tests**

**Completed 2026-03-11:**
- [x] `MonitoredQueue<T>` header at `src/utils/hub/hub_monitored_queue.hpp` (move ctor/assign, push/drain/run_check, 5 callbacks) ✅
- [x] `hub_producer.cpp` / `hub_consumer.cpp`: replace `ctrl_send_mu`+`ctrl_send_queue` with `MonitoredQueue`; add peer-dead detection (`peer_ever_seen_`, `last_peer_recv_`); `on_peer_dead()` / `ctrl_queue_dropped()` public methods ✅
- [x] `hub_producer.hpp` / `hub_consumer.hpp`: `ctrl_queue_max_depth` / `peer_dead_timeout_ms` in Options; `on_peer_dead()` / `ctrl_queue_dropped()` declared ✅
- [x] `StopReason` enum + `stop_reason_` atomic in `PythonRoleHostBase` ✅
- [x] All 3 script hosts: wire `on_peer_dead` + `on_hub_dead` callbacks; pass `ctrl_queue_max_depth`/`peer_dead_timeout_ms` in opts ✅
- [x] All 3 API classes: `stop_reason()` / `ctrl_queue_dropped()` + pybind11 bindings + `ctrl_queue_dropped` in `snapshot_metrics_json()` ✅
- [x] Config parsing: `ctrl_queue_max_depth`/`peer_dead_timeout_ms` in ProducerConfig/ConsumerConfig/ProcessorConfig ✅
- [x] Code review fixes round 1: `fire_and_forget=true` default; peer-dead detection in embedded mode (handle_peer/ctrl_events_nowait); `on_peer_dead_cb` under `callbacks_mu` (producer + consumer, 3 sites each); ctrl_queue_max_depth=0 unbounded fix; README_EmbeddedAPI.md + HEP-0017 §3.5 ✅
- [x] 9 MonitoredQueue unit tests (+2 new: FireAndForget_True_SkipsCallbacks, MoveAssignment_ResetsMonitoringState) → **1060/1060** ✅
- [x] Code review fixes round 2: `on_peer_dead_cb` not cleared in Producer::close(); Consumer::close() missing 3 callbacks; `PendingConsumerCtrlSend`→`PendingCtrlSend` rename → **1060/1060** ✅
- [x] CR-005: embedded-mode peer-dead tests → **1062/1062** ✅ 2026-03-11

### Priority -1 (CLOSED — 2026-03-10): Full-Stack Code Review Fixes + Abstraction Cleanup
📍 **Status**: ✅ CLOSED — all actionable items resolved; **1045/1045 tests**
📋 **Reviews**:
  - `docs/code_review/REVIEW_FullStack_2026-03-10.md` — ✅ CLOSED 2026-03-10
  - `docs/code_review/REVIEW_FullStack2_2026-03-10.md` — ✅ CLOSED 2026-03-10
  - `docs/code_review/REVIEW_DesignAndCode_2026-03-09.md` — ✅ CLOSED (DC-04/06 deferred)

**Completed this sprint** (2026-03-09/10):
- [x] REVIEW_DesignAndCode_2026-03-09.md: DC-01 METRICS_REQ SHM merge fixed; DC-02/03/05 verified; DC-04/06 deferred ✅
- [x] Consumer inbox_thread_ (ROUTER): ConsumerConfig inbox fields + ConsumerScriptHost::run_inbox_thread_() ✅ 2026-03-10
- [x] 9 L3 ShmQueue test scenarios → **988/988 tests** ✅ 2026-03-10
- [x] Full-stack code review (background agent): 14 findings triaged → REVIEW_FullStack_2026-03-10.md ✅ 2026-03-10
- [x] FS-01/MR-05/MR-10: false positives confirmed by code audit ✅ 2026-03-10
- [x] FS-02: inbox config validation hardened in ProducerConfig+ConsumerConfig; 8 new tests → **996/996** ✅ 2026-03-10
- [x] Code review REVIEW_DataHubInbox_2026-03-09.md: 13 items fixed, CLOSED + archived ✅ 2026-03-09
- [x] REVIEW_FullStack2_2026-03-10.md: A1 (inbox_overflow_policy parsed), A5 (zmq_buffer_depth in ConsumerConfig), A6 (+15 config tests), A11/A18 (InboxQueue per-sender seq gap), A12 (queue_type rename), A20 (HEP doc updated) → **1011/1011** ✅ 2026-03-10
- [x] `LoopDriver`/`loop_driver` → `QueueType`/`queue_type` throughout code+docs; wire key `consumer_queue_type` ✅ 2026-03-10
- [x] ProcessorAPI queue-state accessors: `last_seq`, `in_capacity`, `in_policy`, `out_capacity`, `out_policy`, `set_verify_checksum`; atomic QueueReader*/QueueWriter* members; set/clear in start/stop/cleanup ✅ 2026-03-10
- [x] API naming fix: `ProducerAPI::overrun_count()` → `loop_overrun_count()` (pybind11 + JSON keys) ✅ 2026-03-10
- [x] ConsumerAPI: `set_verify_checksum()` added; `loop_overrun_count: 0` added to `snapshot_metrics_json()` ✅ 2026-03-10
- [x] REVIEW_Processor_2026-03-10.md: all 20 items ✅ CLOSED 2026-03-10 → **1045/1045 tests**
- [x] REVIEW_DeepStack_2026-03-10.md: deep review (9 dimensions, 16 findings); DS-DS-01/02, DS-MET-01, DS-DEAD-01, DS-H18-01/02, DS-CF-01/02/03, DS-H15-01, DS-API-01/02 fixed; DS-MR-09/HR-03/HR-05 deferred (tracked in API_TODO.md) ✅ CLOSED 2026-03-10
- [x] Abstraction leak audit + fix: all 3 script hosts (producer/consumer/processor) now use QueueWriter::write_flexzone/flexzone_size/sync_flexzone_checksum and QueueReader::read_flexzone/flexzone_size; set_checksum_options moved to post-factory abstract call; update_flexzone_checksum fixed in ProducerAPI + ProcessorAPI ✅ 2026-03-10 → **1045/1045**
- [x] REVIEW_FullStack2_2026-03-10: A2/A14 PRE-FIXED confirmed (Processor inbox receive + ProcessorAPI open_inbox/wait_for_role already implemented); A13 FIXED (script_type_explicit + LOGGER_WARN); CLOSED ✅ 2026-03-10
- [x] HEP-0015: JSON example corrected (flexzone_schema, flat inbox keys, startup ⚠ warning); ProcessorConfig struct listing corrected ✅ 2026-03-10
- [x] HEP-0018: JSON examples corrected (target_period_ms, no shm.slot_count, set_critical_error(), loop_overrun_count); ProducerAPI/ConsumerAPI sections expanded ✅ 2026-03-10
- [x] README_Deployment.md: complete rewrite (stale actor content replaced with all 4 binaries, full field refs, Python API, multi-hub pipelines, operational guide) ✅ 2026-03-10

### Priority 1: Layer 4 Producer + Consumer Tests
📍 **Status**: ✅ Complete (2026-03-02) — 26 new tests; **550/550 passing**
📋 **Details**: `docs/todo/TESTING_TODO.md` § "Layer 4: pylabhub-producer/consumer Tests"

Completed:
- [x] `tests/test_layer4_producer/` — config unit tests (8) + CLI integration tests (6)
- [x] `tests/test_layer4_consumer/` — config unit tests (6) + CLI integration tests (6)
- [x] Integration test: full pipeline round-trip via live broker — `test_pipeline_roundtrip.cpp` (hubshell + producer + processor + consumer)

### Priority 2: Schema Registry — superseded model (HEP-CORE-0016 → HEP-CORE-0034)
📍 **Status**: HEP-CORE-0016 5 phases shipped 2026-03-02 → **superseded by HEP-CORE-0034 (ratified 2026-04-26)**.
📋 **New spec**: `docs/HEP/HEP-CORE-0034-Schema-Registry.md`
📋 **Historical spec**: `docs/HEP/HEP-CORE-0016-Named-Schema-Registry.md` (kept for reference)

HEP-0016 historical phases (all shipped, retained context):
- [x] Phase 1: SchemaLibrary + JSON format (2026-03-01)
- [x] Phase 2: C++ Integration — `has_schema_registry_v<T>`, `validate_named_schema<>()`,
              `ProducerOptions::schema_id`, `ConsumerOptions::expected_schema_id`; 7 tests (2026-03-02)
- [x] Phase 3: Broker protocol — `REG_REQ` schema_id/blds fields, Case A/B annotation,
              `SCHEMA_REQ/ACK`, consumer expected_schema_id validation; 7 tests (2026-03-02)
- [x] Phase 4: `SchemaStore` lifecycle singleton (**to be removed in HEP-0034 Phase 4** — file watcher + broker query fallback no longer fit hub-mutator model)
- [x] Phase 5: Script integration — named schema strings in config resolve via `SchemaLibrary`

HEP-0034 implementation phases:
- [x] Phase 1 (2026-04-27, commit `d60ddf2`): Fingerprint correction — `compute_schema_hash` and `SchemaInfo::compute_hash` include packing; `parse_schema_json` rejects missing packing; `PYLABHUB_SCHEMA_BEGIN_PACKED` macro added; share/ JSONs + role init templates updated to declare packing explicitly. +4 tests (`ParseError_MissingPacking`, `FingerprintIncludesPacking_*`). Follow-up `dc9b6ef`: `compute_inbox_schema_tag` now includes packing; `PackingMacro_DistinctHashesFromAligned` test pins macro hash distinction; HEP §14.2 doc aligned to two-macro design. 1603/1603.
- [x] Phase 2 (2026-04-27, commit `e23e33e`): HubState schema records — `SchemaRecord` (`schema_record.hpp`) + `HubState.schemas` map keyed `(owner_uid, schema_id)` + `ChannelEntry.schema_owner`; capability ops `_on_schema_registered` / `_on_schemas_evicted_for_owner` / `_validate_schema_citation`; cascade eviction wired into `_set_role_disconnected`; three new counters (`schema_{registered,evicted,citation_rejected}_total`). +15 tests in `test_hub_state.cpp` covering namespace-by-owner conflict policy, idempotent re-registration, hash/packing-mismatch rejection, cross-citation rejection on hash-equality, hub-global immunity. 1618/1618.
- [x] Phase 3 (2026-04-27, commits `92775ac` + `87390c8` + follow-up `2619b17`): Wire protocol + broker dispatch — `REG_REQ` with `schema_packing` creates path-B records; `CONSUMER_REG_REQ` with full named/anonymous citation rule per HEP-0034 §10.3 (named: id+hash with optional structure for defense-in-depth; anonymous: full structure required, hash optional with self-consistency check); `SCHEMA_REQ` accepts `(owner, schema_id)` keying alongside legacy `channel_name`; inbox metadata creates `(role_uid, "inbox")` records cascade-evicted via `_on_channel_closed`. Broker recomputes every claimed fingerprint (Stage-2 verification) — `compute_canonical_hash_from_wire` helper pinned the wire canonical form. Channel-mismatch gate moved before schema-record creation (no orphan records on failed REG_REQ). 13 NACK reasons documented in HEP-0034 §10.4. +20 Pattern-3 L3 tests across the three commits. Backward compat preserved. 1638/1638.
- [x] Phase 4a (2026-04-28, commit `4b83636`): SchemaStore deletion + bridge helper + flexzone-in-wire bug fix. Deleted `schema_registry.hpp` / `schema_registry.cpp` / `test_datahub_schema_registry.cpp`. Added `to_hub_schema_record(SchemaEntry)` helper in `schema_utils.hpp` (uses wire-form canonical hash, distinct from HEP-0002 SchemaInfo SHM-header hash). Fixed broker Stage-2 verification to include flexzone fields (was slot-only, would have NACKed any producer with a flexzone — caught while writing the helper). +2 tests (`WireForm_HashMatchesSchemaSpecHash`, `WireForm_FlexzoneIncluded`). 1630/1630.
- [x] Phase 4b (2026-04-28): hub-globals broker-startup loader — `BrokerServiceImpl::load_hub_globals_()` invoked from `BrokerService::run()` walks `cfg.schema_search_dirs` via the new free function `pylabhub::schema::load_all_from_dirs`, translates each entry via `to_hub_schema_record`, and inserts into `HubState.schemas` under `(hub, schema_id)`.  Path-C citations (producer adopts hub-global) now resolve from the very first inbound REG_REQ.  Hosted in `BrokerServiceImpl` rather than waiting for a `plh_hub` binary; ownership moves to `HubDirectory` per HEP-CORE-0034 §2.4 I9 when HubDirectory lands (HEP-0033 §7).  +5 Pattern-3 L3 tests (HubGlobalsLoadedAtStartup, PathC_AdoptionSucceeds, PathC_FingerprintMismatch, PathC_UnknownGlobal, PathX_ForbiddenOwner).
- [x] Phase 4c (2026-04-28): SchemaLibrary demotion + legacy purge.  Following an audit-driven holistic investigation, the SchemaLibrary class was deeply non-compliant with HEP-CORE-0034 §G2 (it carried `by_id_` / `by_hash_` maps + `register_schema` / `get` / `identify` / `list` / `load_all` member that constituted a parallel runtime registry).  HEP-0034 §2.4 (Module ownership and runtime invariants) added with 9 binding rules I1-I9, module-flow Mermaid diagram, forbidden + permitted patterns.  Demotion: SchemaLibrary stripped to a stateless namespace-shell holding only static parsers (`load_from_file`, `load_from_string`, `compute_layout_info`, `default_search_dirs`); copy/move ctors deleted so it can no longer be instantiated; new free function `load_all_from_dirs(dirs) → vector<pair<path, SchemaEntry>>` is the §2.4 I2 entry-point.  Files renamed: `schema_library.hpp/.cpp` → `schema_loader.hpp/.cpp`, `test_datahub_schema_library.cpp` → `test_datahub_schema_loader.cpp`.  Deleted: legacy HEP-0016 Cases A and B in `handle_reg_req` (used HEP-0002 BLDS form vs HEP-0034 wire form — different by design per §2.4 I6, also forbidden under namespace-by-owner per §2.4 I7); `BrokerServiceImpl::schema_lib_` member + `get_schema_library()` accessor; `validate_named_schema<T,F>` + `validate_named_schema_from_env<T,F>` templates (zero production callers; broker NACK is now the validator per §2.4 I4).  Wire `type` token convention pinned in HEP-0034 §6.3: JSON type name (`float32`), not BLDS token (`f32`).  RAII tech_draft §6.15 added as forward-compatibility reference for the typed addon's schema-fields helper (Phase 5d).  1631/1631.
- [ ] Phase 5: Client-side citation API + role-host refactors. Sub-phases:
  - [x] Phase 5a (2026-04-28, commit `dc8b517`): wire-fields population — `WireSchemaFields` + `make_wire_schema_fields` / `apply_producer_schema_fields` / `apply_consumer_schema_fields` helpers in `schema_utils.hpp`; producer / consumer / processor role hosts populate full HEP-0034 §10 schema fields on REG_REQ / CONSUMER_REG_REQ; broker-side consumer-flexzone fix (mirror of Phase 4a). +7 tests. 1637/1637.
  - [x] Phase 5b (2026-04-28, commit `900fe23`): payload-builder helpers — `build_producer_reg_payload(ProducerRegInputs)` / `build_consumer_reg_payload(ConsumerRegInputs)` in `role_reg_payload.hpp`.  Channel/uid/transport fields no longer duplicated.  ~50 LOC removed across the three role hosts. 1637/1637.
  - [x] Phase 5c (2026-04-28, commit `2e52319`): lifecycle helpers — `make_broker_comm_config(hub, auth, uid, name)` + `do_role_teardown(engine, api, core, broker_comm, has_api, teardown_cb)` in `role_host_lifecycle.hpp`.  ~75 LOC removed across the three role hosts. 1637/1637.
  - [ ] Phase 5c-large: full `RoleHostBase<CycleOps>` template absorbing Steps 1-8 (schema setup, infrastructure setup, ctrl-thread launch, run_data_loop).  Larger refactor — needs a CycleOps shape covering all three roles, careful state-movement for the differing slot/flexzone/inbox specs, and consistent validate-only early-exit handling.  Defer until other Phase 5/6 work stabilizes the surface.
  - [ ] Phase 5d: `ProducerOptions::{schema_owner, schema_id}` + `ConsumerOptions::expected_*` C++ API surface; `create<F,D>()` issues `SCHEMA_REQ` for path C, sends BLDS for path B (HEP-0034 §14).  Currently producers populate the wire from config JSON; this lands a typed C++ API for the same fields.
  - [ ] Phase 5e (test coverage gap from 2026-04-28 review): add an L3 integration test that uses `make_wire_schema_fields` + `apply_producer_schema_fields` directly to build a REG_REQ payload, sends it through a real `BrokerService`, and verifies the schema record is created in `HubState.schemas`.  Then a CONSUMER_REG_REQ via `apply_consumer_schema_fields` cites it.  Today the helpers are unit-tested (Phase 5a tests) and broker-side gates are integration-tested (Phase 3 tests using manually-built payloads), but the *composition* — "helper-built payload works against the broker" — isn't directly tested.  By transitivity it should work (helpers produce the same JSON keys the broker accepts), but a one-shot end-to-end test would pin the contract.
- [ ] Phase 6: Docs sweep + HEP-0016 closure — code review of HEP-0034 implementation; verify cross-references consistent. *Includes:* prune stale `Messenger` citations in HEP-0001/0002/0006/0011/0017/0019/0021/0027/0033 (the class was deleted long ago; what remains is just outdated prose still mentioning it as if active — HEP-0007 was pruned 2026-04-28 as a starting point, the rest are reference-grade text edits).
- [ ] **Wire-protocol cleanup — `schema_version` removal** (deferred from 2026-04-29 review Finding E): the REG_REQ / SCHEMA_REQ wire field `schema_version` is residual from HEP-CORE-0016; under HEP-0033 §G2.2.0b the version is encoded in `schema_id` (`$base.v<N>`).  Touches `ChannelEntry::schema_version`, `handle_reg_req` read at `broker_service.cpp:1037`, `handle_schema_req` echo at line 1461, ~7 test-worker sites, `test_hub_state.cpp:249`, HEP-0007 §REG_REQ + §DISC_ACK + §SCHEMA_REQ payload tables.  Keep separate from DataBlock SHM-header `schema_version` (HEP-0002, different concern).  Bundle with the next HEP-0007 wire sweep.
- [ ] **Hub-side admin API — schema/channel inspection** (surfaced 2026-04-29 from Phase 5e Worker A): broker has `SCHEMA_REQ(owner, schema_id)` for direct lookup but no listing/iteration RPC.  Useful surfaces:
    - `LIST_SCHEMAS` (filtered: globals only, by-owner, all) — lets admins / tests enumerate `HubState.schemas` without resolving each by id.
    - `LIST_CHANNEL_CONSUMERS(channel)` or extending `CHANNEL_LIST_REQ` to include per-channel consumer entries — lets tests directly assert "consumer X is still registered on channel Y after producer-restart" instead of inferring it from indirect signals (`broker_sch_record_path_b_created` and the new `Sch_WireHelpers_RegisterAndCite` both have to assert by indirect side-effect).
    - Operational value beyond tests: `plh_admin schemas list` + `plh_admin channel consumers <name>` are natural CLI surfaces once `plh_hub` exists.
  Belongs in HEP-0033 §11 (broker/hub admin) once the hub binary is in scope.

### Priority 2.5: User-facing demo / example refresh (deferred — after `plh_hub` ships)

📍 **Status**: 🔵 Deferred — touch only after HEP-CORE-0033 hub-binary refactoring stabilizes
📋 **Audit found 2026-04-28**:

The user-facing entry points have not been refreshed for HEP-CORE-0024 (per-role binaries deleted) or HEP-CORE-0034 (config field renames `hub_dir`→`in_/out_hub_dir`, `channel`→`in_/out_channel`, top-level `validation` block removed, `slot_schema`→`in_/out_slot_schema`, mandatory `packing` in schema JSON).  L4 tests use synthetic configs (`plh_role_fixture.h::write_minimal_config`) so CI doesn't catch the drift, but anyone running the in-tree examples will hit immediate parse errors.

Items to address as a single commit once `plh_hub` lands:
- [ ] `README.md` §"The Four Binaries" — replace with `plh_role <dir> --role <tag>` + `plh_hub <dir>` description.  All CLI examples updated.
- [ ] `share/py-demo-single-processor-shm/run_demo.sh` — REQUIRED_BINS list + every binary invocation updated to `plh_role` / `plh_hub`.
- [ ] `share/py-examples/{producer_counter,consumer_logger,sensor_node}.json` — migrate to current field names (`in_/out_*`, drop `validation` block in favour of top-level `checksum` + `stop_on_script_error`, drop top-level `broker` since it's read from hub.json, etc.).
- [ ] `share/py-demo-single-processor-shm/{producer,consumer,processor,hub}/*.json` — same migration.
- [ ] `share/py-demo-dual-processor-bridge/**/*.json` — same migration.
- [ ] In-line `_comment` blocks in those JSONs that say `"Run with: pylabhub-producer <dir>"` etc. — update.
- [ ] Verify each example actually loads via `plh_role --validate` after migration.

Scope rationale (deferred): the hub-side refactoring (HEP-0033 plh_hub binary, Phase 6+) will likely add or rename more config fields; doing the demo refresh once at the end avoids two churns of user-facing files.

### Priority 3: Processor Binary Tests + Phase 2 (HEP-CORE-0015)
📍 **Status**: ✅ Phase 2 complete (2026-03-03) — dual-broker + hub::Processor delegation; **750/750 passing**
📋 **Details**: `docs/HEP/HEP-CORE-0015-Processor-Binary.md`

Completed:
- [x] `tests/test_layer4_processor/` — config unit tests (10+5) + CLI integration tests (6)
- [x] Phase 2: dual-broker config (6 fields + 4 resolvers); ProcessorScriptHost delegates to hub::Processor
- [x] hub::Processor enhancements: timeout handler, pre-hook, zero-fill, critical error, iteration counter
- [x] ScriptHost dedup: RoleHostCore + PythonRoleHostBase extracted (~1600 lines deduped)

### Priority 4: Metrics Plane (HEP-CORE-0019)
📍 **Status**: ✅ Complete (2026-03-05) — 19 new tests; **828/828 passing**
📋 **Details**: `docs/HEP/HEP-CORE-0019-Metrics-Plane.md`, `docs/todo/MESSAGEHUB_TODO.md`

Completed:
- [x] Phases 1–5: `report_metric()` / `snapshot_metrics_json()` API; heartbeat extension;
  `METRICS_REPORT_REQ`; `METRICS_REQ`/`METRICS_ACK`; Python bindings; `pylabhub.metrics()` AdminShell
- [x] 10 MetricsPlaneTest (protocol) + 9 MetricsApiTest (API unit, pybind11-linked)
- [x] pybind11 Default Parameter Rule codified in `IMPLEMENTATION_GUIDANCE.md`
- [x] HEP-0007 §12 updated: METRICS_REPORT_REQ, METRICS_REQ/ACK, HEARTBEAT metrics extension
- [x] HEP-0017 §2: "Four Planes" → "Five Planes" (Metrics plane added)

### Priority 5 (backlog): Platform + Admin Shell Test Gaps
📍 **Status**: ✅ HP-C1/HP-C2/BN-H1 complete; platform backlog only
📋 **Details**: `docs/todo/PLATFORM_TODO.md`, `docs/todo/TESTING_TODO.md`

Completed:
- [x] HP-C1: `pylabhub.reset()` deadlock regression — `test_admin_shell.cpp` (2 tests)
- [x] HP-C2: stdout/stderr leak on exec() exception — `test_admin_shell.cpp` (2 tests)
- [x] BN-H1: Consumer ctypes round-trip — `test_pipeline_roundtrip.cpp` (field-level verification)

Remaining backlog:
- Clang-tidy full pass; Windows MSVC CI gaps

### Priority 5: HEP Document Review + Code Review + Source Polish
📍 **Status**: ✅ Complete (2026-03-03) — all 17 HEPs updated; code review passed; umbrella headers polished
📋 **Details**: Plan file `calm-herding-koala.md`

Completed:
- [x] Session 0: Housekeeping — archived HEP-0005, scrubbed 185 actor references across 12 HEPs
- [x] Batch 1-6: All 17 HEP documents updated with mermaid diagrams, source file references, status fields
- [x] Phase 2: 5-pass code review (L0-1, L2, L3, L4, cross-cutting) — 6 issues found and fixed
- [x] Phase 3: Source polish — doxygen fixes, umbrella header reorganization, orphan header inclusion
- [x] Example build fix — `0xBAD5ECRET` literal error in producer/consumer examples

### Backlog: C++ Templates + README Update
📍 **Status**: 🟡 Templates done (2026-03-03); README deferred
📋 **Details**: `docs/todo/API_TODO.md` § "C++ Pipeline Demo" and § "README Documentation Update"

Completed:
- [x] `examples/cpp_processor_template.cpp` — processor pipeline template (Producer → ShmQueue → Processor → Consumer)
- [x] `examples/CMakeLists.txt` + `PYLABHUB_BUILD_EXAMPLES` opt-in CMake flag
- [x] ZMQ wire format documentation (HEP-CORE-0002 §7.1)

Completed:
- [x] Application-oriented README update: API layers, four binaries, CLI flags, config model, C++ vs Python paths,
  getting started guide, five communication planes — root `README.md` + `share/py-demo-single-processor-shm/README.md` (2026-03-05)

---

## Active Work Areas

| Area | Status | Detail Document | Notes |
|------|--------|----------------|-------|
| Security / Identity / Provenance | ✅ Complete | `docs/archive/transient-2026-03-02/SECURITY_TODO.md` | All 6 phases complete (2026-02-28). TODO archived. |
| Actor (pylabhub-actor) | ❌ Eliminated | — | **Eliminated (2026-03-01).** `src/actor/` + `tests/test_layer4_actor/` deleted. HEP-0010/0012/0014 archived. Replaced by three standalone binaries. |
| Producer Binary (`pylabhub-producer`) | ✅ Complete | `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` | **Phase 1 done (2026-03-01); Layer 4 tests done (2026-03-02):** 8 config + 6 CLI tests. **Integration test done:** `test_pipeline_roundtrip.cpp`. |
| Consumer Binary (`pylabhub-consumer`) | ✅ Complete | `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` | **Phase 1 done (2026-03-01); Layer 4 tests done (2026-03-02):** 6 config + 6 CLI tests. **Integration test done:** `test_pipeline_roundtrip.cpp`. |
| HubShell / HubConfig (legacy) | ⚪ Retired (2026-04-29) | — | `pylabhub-hubshell` binary + `src/hub_python/` deleted in earlier passes; the legacy `pylabhub::HubConfig` lifecycle singleton (`src/utils/config/hub_config.cpp`) was retired alongside its self-test 2026-04-29. The replacement is the HEP-CORE-0033 §6.1 `pylabhub::config::HubConfig` composite (Phase 1, this pass). |
| Hub-Role Auth & Federation Trust | 🚧 **Design — TODO Next** | `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` | **NOT IMPLEMENTED.** Single source of truth for hub-role auth (CURVE+ZAP pubkey allowlist) and federation cross-trust delegation. Supersedes the placeholder `RoleIdentityPolicy` documented in HEP-CORE-0009 §2.7. Blocks re-introduction of `broker.{known_roles, federation_trust_mode}` in `HubBrokerConfig`. 7-phase implementation plan in HEP-0035 §8. |
| RAII Layer | ✅ Complete | `docs/archive/transient-2026-03-02/RAII_LAYER_TODO.md` | Phase 3 complete; all code review items resolved. TODO archived; minor backlog absorbed into TESTING_TODO. |
| API / Primitives | 🟢 Ready | `docs/todo/API_TODO.md` | WriteAttach mode + `attach_datablock_as_writer_impl` added; timeout constants; ScopedDiagnosticHandle; **header layering refactor Phase A complete (2026-02-26)**; **P2 src/ split done (2026-02-27)**: `data_block.cpp` 3969L→2894L via `data_block_internal.hpp` + 3 new split files; **HEP-CORE-0002 restructured (2026-02-27)**: §6 RAII Abstraction Layer added, §7 Control Plane Protocol stub (→HEP-CORE-0007), stale §5.3/§5.4/§5.5 removed, §6-§15→§7-§16; **P4 messenger.cpp split done (2026-02-27)**: `messenger_internal.hpp` + `messenger_protocol.cpp`; `messenger.cpp` 1707L→811L |
| Platform / Windows | 🟢 Mostly done | `docs/todo/PLATFORM_TODO.md` | Major pass done; 2 Windows CI items in backlog |
| Testing | ✅ Complete | `docs/todo/TESTING_TODO.md` | **1181/1181 passing** (2026-03-30). SHM-C2 audit: +2 draining tests, fixed stalling DrainingTimeoutRestoresCommitted. ProcessorHandlerRemoval flake fixed (sleep→poll_until barrier). Full sleep audit: 8 races eliminated in hub_processor_workers.cpp. `test_sync_utils.h` shared facility created. |
| Memory Layout | ✅ Complete | `docs/archive/transient-2026-03-02/MEMORY_LAYOUT_TODO.md` | Single structure; alignment fixed; sub-4K slots. TODO archived; minor test backlog absorbed into TESTING_TODO. |
| Schema Validation | ✅ Complete | — | BLDS schema done; dual-schema producer/consumer validation working |
| Schema Registry | 🟡 In flight (HEP-0034 ratified 2026-04-26) | `docs/HEP/HEP-CORE-0034-Schema-Registry.md` | HEP-0016 (5 phases shipped 2026-03-02) **superseded**. New owner-authoritative model: namespace-by-owner records `(owner_uid, schema_id)`, owner-bound eviction (no refcount), cross-citation rejected even on hash match, `<hub_dir>/schemas/` for hub-globals, fingerprint corrected to include packing. Six implementation phases pending; tracked in §Priority 2. |
| Processor Binary | ✅ Phase 3 complete | `docs/HEP/HEP-CORE-0015-Processor-Binary.md` | **Phase 1+2 done (2026-03-03). Phase 3 config+ScriptHost 2026-03-10:** timing policy, inbox (ROUTER), direct ZMQ PULL input, verify_checksum, zmq_packing/buffer from config. REVIEW_Processor_2026-03-10.md: all 20 items ✅ CLOSED 2026-03-10. **1078/1078 tests.** |
| Startup Coordination | ✅ Phase 1+2 (docs revised 2026-05-07) | `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` | **Phase 1 (2026-03-11):** `startup.wait_for_roles` config + per-role timeout. **Phase 2 (2026-04-14):** three-response DISC_REQ state machine + heartbeat-multiplier liveness timeouts + role-close cleanup hook + `RoleStateMetrics` counters. **2026-05-07 doc rewrite:** §2 retired the channel-side FSM (Closing state and FORCE_SHUTDOWN escalation removed); the FSM is now per-presence on `RoleEntry` (Connected/Pending/Disconnected); channel teardown is atomic on producer-presence Disconnected.  Code changes ship under Wave B M1 (`role_host_template_design.md`). **1275/1275 tests** at the Phase 2 baseline; M1 will land additional regression tests. |
| Role Directory Service | 🟢 Implemented (Phases 1-4,6) | `docs/HEP/HEP-CORE-0024-Role-Directory-Service.md` | **HEP-0024 Phases 1-4+6 DONE 2026-03-12.** `RoleDirectory` + `role_cli.hpp` public API; all 3 `from_directory()` migrated; all 3 `do_init()`/`parse_args()` migrated; 26 new L2 tests. Deferred: Phase 5 (script-host `script_entry()` migration), Phase 7 (docs), Phase 8 (L4 tests). **1104 tests.** |
| Pipeline Architecture | ✅ Design | `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` | Design complete (2026-03-01, updated 2026-03-05). Five planes (Metrics added), four standalone binaries, topology patterns. |
| Metrics Plane | ✅ Complete (Phase 6 SHIPPED 2026-05-11) | `docs/HEP/HEP-CORE-0019-Metrics-Plane.md` | **All 6 phases shipped.** Phase 1-5 (2026-03-05) — heartbeat metrics extension, METRICS_REQ/ACK, Python bindings, AdminShell.  **Phase 6 (Wave M1.4, 2026-05-11)** — per-presence keying.  METRICS_REPORT_REQ + broker `metrics_store_` retired; metrics live on `RolePresence::latest_metrics`; admin queries route through `HubState::channel_metrics_snapshot`.  Wire-protocol break: `broker_proto_major` bumped 1 → 2.  Tests: 1828/1828 including 6 new `HubStateChannelMetricsSnapshot.*`. |
| Interactive Signal Handler | ✅ Complete | `docs/HEP/HEP-CORE-0020-Interactive-Signal-Handler.md` | **Implemented (2026-03-02).** All 4 binaries integrated. Old signal handlers removed. 705/705 pass. |
| Recovery API | ✅ Complete | — | P8 recovery API done; DRAINING recovery restores COMMITTED |
| Messenger / Broker | ✅ Complete | `docs/todo/MESSAGEHUB_TODO.md` | Single-tier atomic channel shutdown (CHANNEL_CLOSING_NOTIFY fan-out + atomic ChannelEntry removal on producer-presence Disconnected — HEP-0007 §12 + HEP-0023 §2.1, corrected 2026-05-07; FORCE_SHUTDOWN escalation retired); Cat 1/Cat 2 health; event handlers; CHANNEL_NOTIFY_REQ relay |
| ZMQ Endpoint Registry | ✅ Complete | `docs/HEP/HEP-CORE-0021-ZMQ-Endpoint-Registry.md` | **HEP-0021 implemented (2026-03-06).** `data_transport`+`zmq_node_endpoint` in REG_REQ/DISC_ACK, hub::Producer/Consumer, ProcessorScriptHost. 12 L3 protocol tests (848/848 pass). Deferred: ZMQ data-plane runtime checksum+type-tag (HEP-0023). |
| Hub Federation Broadcast | ✅ Complete | `docs/HEP/HEP-CORE-0022-Hub-Federation-Broadcast.md` | **HEP-0022 fully implemented (2026-03-06).** HUB_PEER_HELLO/ACK/BYE, HUB_RELAY_MSG, dedup window, channel_to_peer_identities_ index, HubScript federation callbacks (on_hub_connected/disconnected/message, api.notify_hub). |

**Active code reviews:**
- `REVIEW_WaveM3_*_2026-05-11.md` — Six review passes from the
  Wave M3 fix-and-audit sweep.  All findings resolved or properly
  deferred with documented triggers; pass 6 explicitly calls
  Wave M3 done.  These archive to `docs/code_review/archive/transient-2026-05-11/`
  on the next archival sweep; leaving here as a quick reference
  until then.  Findings: H1-H43.  Test count: 1823/1823.
- `REVIEW_TestAudit_2026-05-01.md` — **TOP PRIORITY** full-codebase
  test-correctness audit; ground-truth tracker for three failure
  classes (outcome-only assertions, sleep-ordering, discarded
  timeout returns) + LogCaptureFixture rollout.  §11 preserves
  hub-implementation state (Phase 6.2a shipped; 6.2b/c blocked on
  audit Phase 1 acceptance).  This file is the resume bookmark
  during the audit and after.
- `REVIEW_CatchBlocks_2026-05-01.md` — production-code catch sweep
  (226 catches across `src/`); paired with TestAudit, must close
  together.
- `REVIEW_AdminService_2026-05-01.md` — Phase 6.2 pre-implementation
  audit: §11.2 method-readiness matrix (10 unblocked / 6 deferred to
  HEP-0035 + Phase 7 + small new mutator), 6.2a/b/c sub-phase split,
  3 wiring decisions (vault→HubHost, ipc/ vs service/ placement,
  hub init_directory template).  6.2a ✅ shipped (commits `5f652d2`,
  `db9f8f9`); 6.2b / 6.2c PENDING — gated by `REVIEW_TestAudit` Phase 1.

(Previously closed and archived to `docs/archive/transient-2026-03-12/`.)

Previously closed (archived):
- `REVIEW_FullSource_2026-03-06.md` — ✅ CLOSED 2026-03-12 (archived)
- `review_high_level.md` — ✅ CLOSED 2026-03-12 (all HIGH/MEDIUM/LOW resolved; archived)
- `REVIEW_DesignAndCode_2026-03-09.md` — ✅ CLOSED 2026-03-10 (archived 2026-03-12)
- `REVIEW_DataHubInbox_2026-03-09.md` — ✅ CLOSED 2026-03-09, archived `transient-2026-03-09/`
- `REVIEW_FullStack_2026-03-10.md`, `REVIEW_FullStack2_2026-03-10.md`, `REVIEW_Processor_2026-03-10.md`, `REVIEW_DeepStack_2026-03-10.md` — all ✅ CLOSED 2026-03-10 (archived 2026-03-12)
- `gemini_review.md` — triaged 2026-03-12 (5 stale/FP, 2 fixed, 1 accepted, 1 open→API_TODO; archived)

**Security fixes applied (2026-03-06):** SHM-C1 (heartbeat CAS uid/name write-before-CAS → data corruption), IPC-C3 (thread lambda `this`-capture → use-after-move), SVC-C1 (vault_crypto key not zeroed), SVC-C2/C3 (hub_vault sec+admin token buffers not zeroed), HDR-C1 (namespace outside `#ifdef __cplusplus`). See `REVIEW_codebase_2026-03-06.md` for full triage.

**Security fixes applied (2026-03-06, session 2):** IPC-H2 (BrokerService `server_secret_z85` + `cfg.server_secret_key` now zeroed in `~BrokerServiceImpl()` via `sodium_memzero`).

**Bugs fixed (2026-03-06, session 3):**
- #22 (zmq_context.cpp): Use-after-free — swapped `delete ctx` / `g_context.store(nullptr)` order. Now stores nullptr FIRST so no thread can observe a valid pointer to freed memory.
- #2 (python_interpreter.cpp): TOCTOU in `exec()` — added second `ready_` check after acquiring exec_mu AND GIL. Since `release_namespace()` needs the GIL, the check after GIL acquisition is authoritative and race-free.

**Closed non-issues (2026-03-06, session 2):** SHM-C2 (write_index burned on timeout) — analyzed and documented in `data_block.cpp`. For `Latest_only`: fully immune (reads commit_index). For `Sequential`: stale-data read is possible only with `acquire_timeout_ms==0` (non-blocking) + slow reader — NOT a supported production configuration. Documented per-policy impact in source.

**Remaining deferred items:** IPC-C2/H5 (zmq_context check-then-store — ✅ now fixed as #22); IPC-H3 (callback data race — documented design contract). Full backlog of 38 open review items in REVIEW_FullSource_2026-03-06.md.

**Legend:**  
🔴 Blocked | 🟡 In Progress | 🟢 Ready | ✅ Complete | 🔵 Deferred

---

## Subtopic TODO Documents

All detailed task tracking, completions, and phase-specific work is maintained in subtopic TODO documents.
See `docs/todo/README.md` for full list and archiving history.

### Active (have open items) — as of 2026-04-22

- **`docs/todo/API_TODO.md`** — HEP-CORE-0032 ABI check facility (design
  ready, implementation not started); `PYLABHUB_UTILS_TEST_EXPORT`
  Phases 2-7; std::function/std::optional ABI fixes; C API helpers;
  `src/`+`src/include/` restructure plan (deferred); HEP-0002 architecture
  diagrams tail.
- **`docs/todo/MESSAGEHUB_TODO.md`** — HEP-CORE-0033 Hub Character impl
  (13 prereqs G1-G13); system-level L4 tests folded into HEP-0033 scope;
  6 hub-facing L3 Pattern-3 conversions also folded into HEP-0033 scope
  (from retired 21.L5 tracker).
- **`docs/todo/TESTING_TODO.md`** — Lua V2-fixture cleanup tail
  (~65 remaining V2 tests across chunks 7+); worker-helper unification
  (deferred until Python engine converted); Script-API live-vs-frozen
  contract design; schema/packing round-trip gap; broker-protocol timing
  audit.
- **`docs/todo/PLATFORM_TODO.md`** — Linux-only CI vs documented
  platform-support claim (widen CI or narrow docs); clang-tidy pass;
  MSVC `/Zc:preprocessor` propagation audit; MSVC `/W4 /WX` CI gate.

### Archived (complete — no active items)
- `SECURITY_TODO.md` → `docs/archive/transient-2026-03-02/` (all 6 phases done 2026-02-28)
- `RAII_LAYER_TODO.md` → `docs/archive/transient-2026-03-02/` (RAII layer complete)
- `MEMORY_LAYOUT_TODO.md` → `docs/archive/transient-2026-03-02/` (memory layout complete)

### Pending / In-Progress

| Item | Status | Notes |
|------|--------|-------|
| Dual-hub bridge demo (`share/py-demo-dual-processor-bridge/`) | ✅ Complete | ProcessorConfig transport fields + ProcessorScriptHost ZMQ path + L3 ShmInZmqOut/ZmqInShmOut tests + 6-process demo configs/scripts/run_demo.sh — 2026-03-11. |
| HEP-0022 Phase 5+6: HubScript federation callbacks | ✅ Complete | `on_hub_connected`, `on_hub_disconnected`, `on_hub_message`, `api.notify_hub()` fully wired in `hub_script.cpp` + `hub_script_api.cpp` + `hubshell.cpp`. Confirmed 2026-03-06. |
| Security: IPC-H2 BrokerService key zeroing | ✅ Fixed (2026-03-06) | `~BrokerServiceImpl()` zeros `server_secret_z85` + `cfg.server_secret_key` via `sodium_memzero`. |

See `docs/HEP/HEP-CORE-0022-Hub-Federation-Broadcast.md`.

---

## How to Use This System

1. **Check this master TODO** for high-level status and current sprint focus
2. **Dive into subtopic TODOs** for detailed tasks and tracking
3. **Update subtopic TODOs** as you work (mark completions, add new tasks)
4. **Update this master TODO** when major milestones are reached or priorities shift
5. **Archive completed work** per `docs/DOC_STRUCTURE.md` guidelines

**Important**: Recent completions and detailed phase tracking belong in subtopic TODO documents, not here. Keep this document high-level and strategic.

### Maintenance Schedule

- **Weekly** (Monday): Update current focus, move completed tasks to "Recent Completions" in subtopic TODOs
- **Sprint End** (every 2 weeks): Clean up recent completions, groom backlog, update master status
- **Monthly** (1st Monday): Archive old completions (> 2 months), review all TODOs for duplicates
- **Quarterly** (every 3 months): Structural review, create/merge/archive subtopic TODOs

See **`docs/todo/README.md`** for detailed maintenance procedures and best practices.

---

## Quick Links

- Documentation Structure: `docs/DOC_STRUCTURE.md`
- TODO Maintenance Guide: `docs/todo/README.md`
- Implementation Guidance: `docs/IMPLEMENTATION_GUIDANCE.md`
- Code Review Process: `docs/CODE_REVIEW_GUIDANCE.md`
- Test Strategy: `docs/README/README_testing.md`
- Build Commands: `CLAUDE.md`
