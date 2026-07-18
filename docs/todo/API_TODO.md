# API TODO

**Scope:** API / ABI / concurrency / lifecycle / RAII surface.
**Source of truth for status:** `docs/TODO_MASTER.md` Sprint Focus
(renovation arc states).  This file holds API-layer detail for open
items only; completed work lives in git history + DOC_ARCHIVE_LOG.

**Completed-work archive:** `docs/archive/transient-2026-06-05/todo-completions/API_TODO_completions.md`
(Task #78 closure E′-1/E′-2a/E′-2b/E′-2c; Commit C′-1 missing-field
hard error; Task #101 key-file ACL discipline; C2 + X1 code-ahead
removals from D2 / D3 drift batches).

---

## Current Focus

> **Extracted 2026-07-18 (all ✅ SHIPPED, verified against code):** Queue-owned
> topology + layer cleanup P1-P5 (2026-07-11; HEP-0036 §I9.1); Loop-ready gate +
> fan-in binding-side reader arc (2026-07-11; HEP-0011 §"Loop-ready gate");
> **#238** log-format standardization (`event=` format deployed —
> `broker_service.cpp`/`role_api_base.cpp`); **#235** Python
> `band_member_contains`/`_count` JSON-nesting fix (anchor `consumer_api.cpp:75`,
> mirrored producer/processor).  Verbatim narrative at commit `633d51c0`; index
> `docs/archive/transient-2026-07-18/todo-completions/`.  #235 residual: L3 parity
> regression tests → fold into **#232**.

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

Open items left over from the multi-engine demo session that found
13 bugs (B1–B13).  B1, B2, B5, B9, B11, B12, B13 closed inline (git
log).  B3 closed via Task #78 (see archive).  Remaining items below
are filed but not yet fixed; each is a tightly-scoped single-area
change.

- **#102** — **HEP-CORE-0035 §4.7 runtime key handling** —
  shared `src/utils/security/runtime_key_handling.{hpp,cpp}` with
  `disable_core_dumps()` (POSIX `setrlimit(RLIMIT_CORE,0)` + Linux
  `prctl(PR_SET_DUMPABLE,0)` + Windows `SetErrorMode` /
  `WerAddExcludedApplication`) and `SecureKeyBuffer` RAII wrapper
  around `sodium_malloc` / `sodium_memzero`.  Called from
  `plh_hub` and `plh_role` `main()` BEFORE the §4.6 ACL check,
  and used by all key-loading code paths.  Cross-platform via
  libsodium primitives; only the core-dump-disable helper is
  platform-conditional.  Mechanically independent of #101 and
  #74; can ship at any time.  Spec: HEP-CORE-0035 §4.7.  M.

- **#103** — **HEP-CORE-0017 §3.3 + HEP-CORE-0036 implementation:
  `RxQueueOptions::producer_peers` + `ZmqQueue` dynamic peer API.**
  Per HEP-0017 §3.3 (Dynamic peer membership) + HEP-0036 §4.1 / I9:
  replace `RxQueueOptions::zmq_node_endpoint` (singular) with
  `std::vector<ProducerPeer> producer_peers`; add
  `ZmqQueue::add_producer_peer(peer)` / `remove_producer_peer(uid)`.
  Framework drives these in response to HEP-0033 §12 channel-event
  broadcasts (producer joined/left).  Bind/connect direction is
  internal to ZmqQueue.  This unlocks fan-in pipelines (N producers
  → 1 consumer) and lets HEP-0036's `CONSUMER_REG_ACK.producers[]`
  array land coherently with §94 (DISC_REQ_ACK array migration —
  the two wire-format changes must land together per HEP-0036
  §14.1).  Spec: HEP-CORE-0017 §3.3 + HEP-CORE-0036 §6.4 + §4.1.
  M-L.  Depends-on: #94 wire-shape coordination.  **Blocks
  AUTH_TODO D4 + D5.**

- **#104** — **Sibling-HEP code updates per HEP-CORE-0036 §14.**
  HEP-0036 design lock-in (2026-05-28) requires synchronized code
  changes across eight sibling HEPs' implementation surfaces:
  - **§14.1 HEP-0021**: `wants_shm_secret` REG_REQ field;
    `producers[]` array on CONSUMER_REG_ACK + DISC_REQ_ACK (the
    second is task #94).
  - **§14.2 HEP-0035**: Layer-3 ZAP enforcement + PubkeyOrigin
    index consumption.  Mostly tracked under #74; this task
    captures the §4.1 / §4.2 Layer-3 wiring specifically.
  - **§14.3 HEP-0023**: `Authorized` state on role-side
    `RegistrationState` FSM (`role_presence.hpp`).
  - **§14.4 HEP-0017**: tracked under #103 above.
  - **§14.5 HEP-0027**: inbox CURVE wiring using identity keypair
    (no per-channel key); inherits data-channel allowlist.
  - **§14.6 HEP-0030**: band CURVE wiring using identity keypair;
    inherits hub-wide `known_roles[]`.
  - **§14.7 HEP-0007**: documentation only (no code; ACK schema
    already implemented via §6.4 producers[]).
  - **§14.8 HEP-0033**: `ChannelAccessEntry` row in HubState entry
    types; no new code beyond §4.1 implementation.
  Effort: L (multi-area).  Depends-on: #74 + #103 + #94.

- **#105** — **Federation protocol design + cross-hub reg/comm
  verification.**  HEP-0036 §13.1 explicitly defers full federation
  protocol to a separate design effort.  MVP inherits HEP-0022
  `HUB_RELAY_MSG` + HEP-0035 §4.3 `federation_trust_mode` + §4.4
  `HUB_PEER_HELLO.roles[]`, but the full cross-hub registration
  path (channel-name → owning-hub resolution, reverse snapshot
  push on consumer death — broker mutates the channel's
  authorized set and emits a new `CHANNEL_AUTH_UPDATE` snapshot
  per HEP-0036 §6.5 amended 2026-06-02 —
  `CONSUMER_DIED_NOTIFY` relay, multi-peer consistency model,
  retry/backoff) is NOT end-to-end verified.  Existing L4 dual-hub processor test
  (#44) covers broadcast relay + local-channel dual attachment
  only.  Output: new HEP (HEP-CORE-0037 "Federation Protocol" or
  amendment to HEP-CORE-0022) + L4 E2E test for cross-hub
  CONSUMER_REG_REQ → remote producer.  L+ (separate effort; not
  blocking single-hub auth shipment).

- **#106** — **HEP-CORE-0038 + impl: script-accessible vault
  keystore (`api.vault_save` / `api.vault_load`).**  Per-role
  isolated encrypted KV store for script-managed secrets (external
  API tokens, OAuth refresh tokens, MQTT broker passwords, license
  keys).  Filed 2026-05-29.  HEP-CORE-0038 stub at
  `docs/HEP/HEP-CORE-0038-Script-Accessible-Vault-Keystore.md`
  records design intent + open questions; detailed design happens
  when picked up.

  Scope: extend `RoleVault` payload with a `scripts` map encrypted
  alongside the CURVE keypair; add two new script-callable methods
  wired through Python + Lua + Native engines (HEP-CORE-0011
  surface); per-role isolation (a role can only read/write its own
  store).  Out of MVP scope: cross-role access, audit logging,
  quotas.

  Tests: L2 RoleVault extended payload + schema migration; L3
  cross-engine API parity; L4 demo of a producer persisting a fake
  external-API token across restarts.

  Depends-on: #104 (HEP-0036 sibling-HEP code updates) shipping
  first.  Independent of #105 (federation).  L.

  Position in chain (per
  `docs/tech_draft/DRAFT_HEP-0036-implementation-guideline_2026-05.md`
  §4): #101 + #102 → #74 → #94 + #103 → #104 → #106 → done.

> **Note on #74 (HEP-0035 auth implementation) expansion 2026-05-29.**
> #74 now subsumes HEP-CORE-0035 §4.8 (known-roles allowlist stored
> inside the hub vault) and the operator-facing CLI commands
> `plh_hub --add-known-role <role.pub>` /
> `--revoke-known-role <role_uid>` / `--list-known-roles`.  Rationale:
> file-permission discipline alone does not protect the allowlist
> against file-write attacks; storing the allowlist inside the
> encrypted vault adds password-gated integrity.  CLI commands
> compose with the existing master-password unlock flow and emit
> OpenSSH-style errors on ACL violations.

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
  Trigger: any new spawn site under ThreadManager.  Track per-module
  decisions inline.

- **S1 Phase B (open)** — Migrate `ZmqQueue`
  (`src/utils/hub/hub_zmq_queue.cpp`) and `InboxQueue sender`
  (`src/utils/hub/hub_inbox_queue.cpp`) to `apply_socket_policy`
  from `utils/zmq_socket_policy.hpp` + connection-state monitor +
  connected-flag gate.  Same risk profile BRC had pre-S1.  Phase A
  (BRC) shipped 2026-05-18.  Tracked as harness task **#66**.  L.

- **D2 drift — open items from Connection/Inbox/Band review
  (REVIEW_Connection_Inbox_Band_2026-05-17)**:
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
  - **B6** (review item) — `CHANNEL_BROADCAST_REQ` retirement
    decision tracked under `MESSAGEHUB_TODO.md` #92 audit.  Channel-
    bound broadcast ≠ band-bound broadcast; pick one or migrate
    tests.
  - **X5** — `Presence::connection` raw pointer +
    `connections_.reserve` form an implicit contract
    (`role_handler.cpp:48-57`); future hub-failover work silently
    breaks pointer stability.

- **D3 polish (batch into one PR)**:
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
    Design promoted into `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md`
    (2026-06-02; tech_draft was archived in the same batch).
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

Canonical plan in `docs/TODO_MASTER.md`.  MP2 + MP2.5 shipped.
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
  `docs/archive/transient-2026-06-02/SRC_STRUCTURE_PLAN.md`
  (archived 2026-06-02 to keep `tech_draft/` focused on active
  work; design preserved verbatim).  Phasing: A (file moves +
  `core` → `basic` rename), B (include reorg), C (umbrella +
  public/internal audit).  Execute when builds are otherwise
  quiet; re-promote the archive copy or start fresh from it as
  appropriate.

### RAII Layer Redesign (Template RAII)

Design: `docs/tech_draft/raii_layer_redesign.md`.  Phase 1 (timing
unification) shipped; Phases 2–5 pending.  Companion HEPs:
HEP-CORE-0002 §17.2 (queue abstraction), HEP-CORE-0008 (LoopPolicy
+ IterationMetrics), HEP-CORE-0009 §2.6 (policy reference),
HEP-CORE-0024 (role unification).

Open phases:
- **Phase 2** — ZMQ transport support in existing `SlotIterator` /
  `TransactionContext` path (today they take
  `DataBlockProducer*` / `DataBlockConsumer*` directly and cannot
  consume from a `ZmqQueue` reader).  Wire through the framework's
  queue abstraction (`QueueWriter` / `QueueReader` per
  HEP-CORE-0002 §17.2).
- **Phase 3** — Timing parity with `run_data_loop`: `MaxRate`,
  `FixedRate`, `FixedRateWithCompensation`, retry-acquire with
  deadline budget, short-timeout backoff, overrun detection.
  Today `SlotIterator::operator++()` implements only simple
  FixedRate.
- **Phase 4** — Typed wrappers for inbox + band: `TypedInboxClient<MsgT>`
  / `TypedBand<EventT>` mirroring the typed slot pattern.  Removes
  hand-marshalling burden for C++ users.
- **Phase 5** — `SimpleRoleHost<MySlot>` template that takes a
  per-cycle lambda + optional hooks and runs the standard 14-substep
  `worker_main_()` skeleton.  Removes the ~360 LOC `RoleHostBase`
  subclass boilerplate C++ users write today.

### ABI Compatibility (HEP-CORE-0032)

Design document: `docs/HEP/HEP-CORE-0032-ABI-Compatibility.md`.
Implementation not started; full plan + axes are in the HEP.

ABI Check Facility companion design at
`docs/tech_draft/abi_check_facility_design.md` — driven by the
2026-04-21 vtable-mismatch SIGSEGV incident.  Records each binary's
expected interface versions at compile time and verifies at startup
against linked library versions.  `ComponentVersions` + SONAME
wiring already in `plh_version_registry.hpp` + `version_registry.cpp`.

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
- RAII redesign: `docs/tech_draft/raii_layer_redesign.md`.
- ABI Check Facility: `docs/tech_draft/abi_check_facility_design.md`.
- Hub State Query Layer:
  `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md`.
- Script reload: `docs/tech_draft/SCRIPT_RELOAD_DESIGN_2026-05-20.md`.
- Engine callback tiers: `docs/tech_draft/engine_callback_tiers.md`.
- Audit history (2026-05-21 + earlier sweeps): see git log + tagged
  commits.
