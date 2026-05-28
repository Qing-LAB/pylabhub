# API TODO

**Scope:** API / ABI / concurrency / lifecycle / RAII surface.
**Source of truth for status:** `docs/TODO_MASTER.md` Sprint Focus
(renovation arc states).  This file holds API-layer detail for open
items only; completed work lives in git history + DOC_ARCHIVE_LOG.

---

## Current Focus

### #94 — Implement HEP-CORE-0021 §16.5 ephemeral-binding production path

HEP-0021 §16.5 step 8 describes `messenger.update_endpoint()` inside
`establish_channel` for the port-0 ephemeral-binding case: ZmqQueue
binds with `tcp://*:0`, OS assigns a real port, then
`establish_channel` calls the now-sync `send_endpoint_update` to
inform the broker.  That production caller does NOT exist in the
tree today (verified 2026-05-21 — only the L3 test calls
`send_endpoint_update`).  Role configs always specify explicit
ports, so the ephemeral path was never built.

The HEP-vs-code drift was surfaced by the ENDPOINT_UPDATE sync
REQ/REP work this session (commit `8228f1ac`).  Now that the API
is sync, the missing production path can be safely wired.

Scope sketch:
- Find the existing producer create / `establish_channel` site.
- Add ephemeral-binding option (likely already supported by
  `ZmqQueue`).
- After `ZmqQueue::start()`, call
  `messenger.update_endpoint(channel, actual_endpoint())` and
  branch per HEP-0021 §16.3 (success → proceed; error/nullopt →
  return nullopt from `establish_channel` per §16.6).
- Add an L4 demo (`share/py-demo-zmq-ephemeral/` or extend an
  existing ZMQ demo) using `tcp://*:0` — exercises the production
  flow.

Effort: M.

### Demo-harness audit follow-ups (2026-05-21)

Open items left over from the multi-engine demo session that found 13
bugs (B1-B13).  B1, B2, B5, B9, B11, B12, B13 ✅ FIXED in code.  The
ones below are filed but not yet fixed; each is a tightly-scoped
single-area change.

- **B3 (#78)** — `hub.auth.keyfile=""` half-state: broker uses
  ephemeral CURVE but doesn't publish `hub.pubkey`, so roles silently
  fail handshake.  Fix: hard-error empty keyfile at config load with
  "Hub requires a vault for CURVE keypair — run `plh_hub --keygen`
  first".  See `docs/todo/MESSAGEHUB_TODO.md` (config validation path
  is broker-side).

- **#101** — **HEP-CORE-0035 §4.6 key-file ACL discipline** —
  shared `src/utils/security/key_file_acl.{hpp,cpp}` utility +
  `--keygen` / `--init` SETS modes (0600 for `*.sec`, 0700 for
  keystore dirs, 0750 for `known_roles/`); binary startup VERIFIES
  modes and refuses to start with OpenSSH-style actionable error
  (path + observed mode + required mode + exact chmod command).
  Mechanically independent of #74 ZAP plumbing; can ship before.
  Layers on B3 (#78) + B4 (#79); recommended ordering:
  #101 → #78 → #79 → #74.  Spec: HEP-CORE-0035 §4.6.  M.

- **B4 (#79)** — `plh_role --init` template emits
  `out_shm_secret/in_shm_secret = 0` (sentinel "no SHM");
  `build_tx_queue` silently skips SHM and falls through to ZMQ.
  Fix: generate a random non-zero default in
  `src/utils/config/role_config_init.cpp`.  S.

- **B6+B7 (#80)** — Consumer `rx.fz` claimed in `README_Deployment.md`
  §8.4 but `consumer_api.cpp:191` only binds `.slot`; processor needs
  `api.flexzone(api.Tx)` side arg (§8.4 example omits this).  Fix:
  EITHER bind `.fz` on RxChannel OR rewrite §8.4 to use
  `api.flexzone()`.  Pick one canonical surface.  S.

- **B10 (#82)** — `api.band_join` from `on_init` silently returns
  `None` because handler isn't up until Step 6 of `worker_main_`.
  Doc workaround already shipped in HEP-CORE-0011 §"API availability
  per callback".  Code-fix candidates: (a) defer the call (queue +
  replay after handler ready) or (b) raise `runtime_error`.  (b) is
  smaller; surface in `RoleAPIBase::band_join`.  S.

- **N2 (#84)** — `NativeEngine::build_api_(HubAPI&)` MVP shipped (B13)
  but exposes only log + uid + name + request_stop.  Missing: HubAPI
  read accessors (list_channels, get_channel, list_roles, list_bands,
  list_peers, query_metrics), control delegates (close_channel,
  broadcast_channel, post_event, augment_*), event-shape callbacks
  (on_event, on_consumer_added, on_role_registered per HEP-0033 §12).
  Each needs a `ctx_X_hub` adapter wrapping HubAPI method as a C
  function pointer returning JSON-as-C-string where applicable.  L.

- **N3+N4 (#85)** — Canonical native plugin examples
  (`good_producer_plugin.cpp:96`, `native_multifield_module.cpp:72`,
  this session's demos) declare `on_init(const char *args_json)` but
  documented ABI is `void name(void)`.  Framework's `FnVoidNoArgs`
  calls them no-args; extra parameter works by accident
  (UB-but-works).  Fix all examples + new demos to match the
  documented ABI.  N4: `NativeEngine` registers a lifecycle module
  with no-op shutdown (`native_engine.cpp:566-584`) while
  finalization is in `finalize_engine_()`; document or remove the
  misleading registration.  S.

- **N5 (#86)** — `docs/README/README_NativePlugins.md` operator-facing
  guide.  Native is currently the worst-documented engine.  Discoveries
  from this session that need to be in there: required exported
  symbols, filename convention (`plugin.so`), `PLH_DECLARE_SCHEMA`
  + schema string format (`name:type:count:dim_flag`, pipe-separated,
  trailing `0` is dim-marker NOT byte offset), on_init/on_stop
  signatures (per N3 fix), `request_stop()` vs `set_critical_error(msg)`
  distinction, on_band_message signature
  (`const plh_band_message_args_t *args`), build command (g++).
  Reference the N6 CMake helper.  M.

- **N7+N10 (#87)** — Three-engine doc parity:
  `docs/README/README_Scripting_{Python,Lua,Native}.md` with identical
  8-section structure (when-to-use → lifecycle → data structure → API
  → example → build/setup → pitfalls → cross-refs).  N10 (HEP-0011 §
  "Lua" expansion) absorbed: Lua-specific notes (`api.X(...)` not
  `api:X(...)`, FFI access patterns, no-numpy gotcha) added there.
  `README_Deployment.md` §5-8 retains deployment scope only +
  cross-references.  L (~2-3 sessions).

### Pre-existing renovation follow-ups

- **Wave-MD1 sweep (open audit sites)** — ThreadManager Thread
  Shutdown Contract (`SlotContext::with_active_loop`,
  `request_shutdown_all`, `wait_for_quiescence`) was introduced in
  MD1 (HEP-CORE-0031 §4.1).  Owners that haven't adopted yet:
  `BrokerService` ctrl/admin threads (still on monotonic-mark
  family), `AdminService` worker (review for `with_active_loop`),
  `HubHost` admin thread (review for `join_named` simplification).
  RoleAPIBase + ZmqQueue ✅ adopted.  Trigger: any new spawn site
  under ThreadManager.  Track per-module decisions inline.

- **S1 Phase B (open)** — Migrate `ZmqQueue`
  (`src/utils/hub/hub_zmq_queue.cpp`) and `InboxQueue sender`
  (`src/utils/hub/hub_inbox_queue.cpp`) to `apply_socket_policy`
  from `utils/zmq_socket_policy.hpp` + connection-state monitor +
  connected-flag gate.  Same risk profile BRC had pre-S1.  Phase A
  (BRC) shipped 2026-05-18.  Tracked as harness task **#66**.  L.

- **D2 drift — open items from Connection/Inbox/Band review
  (REVIEW_Connection_Inbox_Band_2026-05-17)**:
  - **C2** — Heartbeat tick branches on `role_tag` string
    (`role_api_base.cpp:806-823`); HEP-0033 §19.3 says iterate
    `handler_->presences()`.
  - **C4** — Heartbeat tick on master BRC only
    (`role_api_base.cpp:563-567`); HEP-0033 §19.3 says per-conn.
  - **C5** — `Presence` struct missing `inbox_meta` (HEP-0033 §19.1).
    Deferred to Wave-B M5+.
  - **I1** — ROLE_INFO_ACK wire field is `inbox_schema` (object);
    HEP-0023 §4 + HEP-0027 §4.2 say `inbox_schema_json` (string).
    Code self-consistent; HEPs need amending.
  - **I3** — Broker sets `resp["found"]` as semantic overload
    (`broker_service.cpp:3288, 3334`); rename `found` → `has_inbox`
    in wire.
  - **B4** (review item — not bug #79) — `band_index_` always uses
    `presences[0]` (`role_api_base.cpp:1267`); HEP-0033 §18.3 says
    role picks.  Dual-hub processor needs
    `api.in_hub.band_join` / `api.out_hub.band_join` accessors OR
    document the deferral.
  - **B5** (review item) — `BAND_*_NOTIFY` not catalogued in
    `NotificationId` (`role_host_core.hpp:77-82`); HEP-0011
    callback-table model side-stepped.
  - **B6** (review item) — HEP-0030 §9 retires
    `CHANNEL_BROADCAST_REQ` aggressively; broker handler + BRC
    `send_broadcast` + 3 test workers still exercise it.
    Channel-bound broadcast ≠ band-bound broadcast.  Decide: re-
    affirm both or migrate the tests.
  - **X5** — `Presence::connection` raw pointer +
    `connections_.reserve` form an implicit contract
    (`role_handler.cpp:48-57`); future hub-failover work silently
    breaks pointer stability.

- **D3 polish (batch into one PR)**:
  - **X1** — Delete `BrokerRequestComm::send_notify`
    (`broker_request_comm.cpp:691`) — zero callers.
  - **X3** — Decide on `Impl::resolve_bc_for_*` 1-line forwarders
    (`role_api_base.cpp:204-218`).
  - **X4** — Strip stale Wave-B M4d/e/f migration comments (~15
    sites in `role_api_base.cpp`).
  - **X6** — Delete `ChecksumRepairPolicy::Repair` dead enum value.
  - **I2** — Fix `hub_inbox_queue.hpp:12` docstring `fixarray[4]`
    → `fixarray[5]` (cpp is correct at `hub_inbox_queue.cpp:6`).

- **Documented-by-design — preserve against deletion attempts**:
  - **I4 / X7** — Inbox metadata stored per-presence; same
    `inbox_endpoint` string lives on `ChannelEntry.producers[*]`
    AND `ConsumerEntry.inbox_*` for dual-hub processor.  Required
    by HEP-0027 §4.1 step 7 + HEP-0033 §19.5.
  - **G2-#3 / `count_by_observable`** —
    `ChannelSnapshot::count_by_observable` is the right shape for
    hub-script API binding (`hub.count_channels_in_state(observable)`).
    Plan in `docs/tech_draft/hub_state_query_layer_design.md`.
  - **G2-#4 / `set_metrics_hook`** (RESERVED) —
    `RoleAPIBase::set_metrics_hook` (`role_api_base.hpp:226`) is
    wired into heartbeat hot path; Reserved extension point for
    C++ host-side structured-metrics injection.  See HEP-CORE-0019
    §5.5.  Future authors installing a caller MUST remove the
    "Reserved" tags.
  - **G2-#5 / `send_hub_targeted_msg`** (RESERVED federation) —
    `BrokerService::send_hub_targeted_msg` is a hub-to-hub
    federation primitive (HEP-CORE-0022 + HEP-CORE-0033 §13);
    script-side wrapper `HubAPI::send_to_peer` deferred.  Bundle
    with Task #75 (`HUB_TARGETED_ACK` reply frame).

### Wave M2 — Multi-Producer Channel Bookkeeping (open MP4 work)

Canonical plan in `docs/TODO_MASTER.md`.  MP2 + MP2.5 ✅ shipped.
Remaining MP4 broker-handler items:

- REG_REQ admission semantics — same channel + new `role_uid` ⇒
  append a new `ProducerEntry`; same `role_uid` ⇒ restart-replace.
  Reject second producer REG_REQ on `data_transport == "shm"`
  channels with `MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM`.
- DEREG_REQ — routes to `_on_producer_dropped(channel, requester_uid,
  VoluntaryDereg)`.
- Script-requested admin close — call `_on_producer_dropped` once
  per producer in `entry.producers` (atomic teardown per
  HEP-CORE-0023 §2.1).
- CHANNEL_ERROR_NOTIFY / CHANNEL_CLOSING_NOTIFY — fan-out to every
  producer + every consumer.
- ROLE_INFO_REQ / ROLE_PRESENCE_REQ — search `entry.producers` list.

### Deferred future work

- **pylabhub Python client SDK** — operator-side library binding
  composing `AdminService` RPCs (HEP-0033 §11.2).  No code over the
  wire (HEP-0033 §17).  Defer until §11.2 method list stabilizes.
- **Script-spawned worker threads** (HEP-0033 Phase 7+) —
  `api.spawn_worker(name, fn, args)` for `supports_multi_state() ==
  true` engines (Lua).  Phase 7 closed 2026-05-04 (HEP-0033 §17.1
  rejected Commit E).  Resume in Phase 8+ alongside the rich
  HubAPI surface.
- **`src/` + `src/include/` restructure** — full plan in
  `docs/tech_draft/SRC_STRUCTURE_PLAN.md`.  Phasing: A (file moves
  + `core` → `basic` rename), B (include reorg), C (umbrella +
  public/internal audit).  Execute when builds are otherwise quiet.

### ABI Compatibility (HEP-CORE-0032)

Design document: `docs/HEP/HEP-CORE-0032-ABI-Compatibility.md`.
Implementation not started; full plan + axes are in the HEP.

Open follow-ups (lower priority):

- Decide whether `PYLABHUB_UTILS_TEST_EXPORT` Phases 2-7 ship now
  or get folded into HEP-0032 Phase B (test-only symbol export
  policy).  Phase 1 (DataBlock public + test symbols) shipped
  2026-04-15.
- `std::function` / `std::optional` ABI fixes (heterogeneous-toolchain
  cross-DSO crash risk).  Action depends on whether HEP-0032 lands
  the proposed "ABI-safe wrapper" types or accepts the existing
  surface as a "build everything with the same toolchain" rule.

### Pending tests (referenced from TESTING_TODO)

- L2 native plugin coverage for `set_metrics_hook` (Reserved per
  G2-#4 above) — see `docs/todo/TESTING_TODO.md`.

---

## Notes — API Design Principles

### Error Handling Strategy
Errors are returned as `Result<T, Error>` (or equivalent variant)
for fallible operations.  `noexcept` is the default for accessors.

### Lifetime and Ownership
Owning objects use `std::unique_ptr` / pImpl; non-owning observers
use raw pointers with documented contracts.  Cross-thread sharing
goes through `shared_ptr` + a documented synchronization rule.

### Thread Safety
Documented per class.  Default: const methods are thread-safe;
mutators require external synchronization unless explicitly
marked.

### Noexcept Marking
Mark noexcept iff every line provably can't throw (or is wrapped
in try/catch).  No partial-noexcept; either mark fully or don't.

---

## Related Work

- Subtopic TODOs: `docs/todo/{MESSAGEHUB,TESTING,PLATFORM}_TODO.md`.
- Strategic status: `docs/TODO_MASTER.md`.
- ABI: `docs/HEP/HEP-CORE-0032-ABI-Compatibility.md`.
- Hub State Query Layer:
  `docs/tech_draft/hub_state_query_layer_design.md`.
- Script reload: `docs/tech_draft/SCRIPT_RELOAD_DESIGN_2026-05-20.md`.
- Audit history (2026-05-21 + earlier sweeps): see git log + tagged
  commits.
