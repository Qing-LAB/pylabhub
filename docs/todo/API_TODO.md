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

### Carried out of the Connection/Inbox/Band review (verified open 2026-08-07)

`REVIEW_Connection_Inbox_Band_2026-05-17.md` finished its verification pass:
16 of 16 findings re-checked against source, 8 resolved, 2 closed as accepted
design, and these 6 confirmed still open.  The review is closed and archivable;
these live here now so they survive it.

Four are one theme — **the multi-presence (dual-hub) model is half-built.  The
data structures carry the topology; the operations still assume presence 0.**
Harmless while every shipped role is single-presence, and a correctness bug the
day a dual-hub processor lands, so they are best done as one unit with that
milestone rather than piecemeal:

- **C5 — `Presence` has no `inbox_meta`.**  `role_presence.hpp:222-267` carries
  hub / channel / role_kind / slot_spec / fz_spec / connection /
  registration_state; inbox metadata reaches the wire via
  `append_inbox_to_reg(reg, inbox_cfg_)` from each role host
  (`consumer_role_host.cpp:343`, `processor_role_host.cpp:404,432`,
  `producer_role_host.cpp:387`) — a per-host side-field, so a dual-hub role
  cannot advertise a different inbox per presence.  **Do now regardless of the
  deferral:** the file's own header (`role_presence.hpp:6`) describes the tuple
  as `(hub, channel, role_kind, schemas, inbox)`, promising a field that does
  not exist.  Fix the comment or add the member; do not leave the doc lying.
- **B4 — every band join binds `presences.front()`.**  `role_api_base.cpp:5007`.
  Neither remedy landed: the `api.in_hub.band_join` / `api.out_hub.band_join`
  accessors do not exist (no `in_hub`/`out_hub` in `role_api_base.hpp`), and the
  deferral is recorded only in a code comment (`:4977`, `:4996-5001`), not in
  HEP-0033 §18.3.  Pick one — build the selector, or state the deferral in the
  HEP so the next reader of §18.3 is not misled.
- **X5 — `Presence::connection` pointer stability is enforced by comment only.**
  `role_handler.cpp:47-56` reserves `connections_` and declares the vector
  frozen; `role_handler.hpp:34-36` repeats it.  Nothing enforces it.  Hub
  failover or dynamic rebinding would dangle `role_presence.hpp:253` silently.
  A debug-build mutation guard is the cheap version; stable storage is the real
  fix.
- **S4 — band membership has no local state.**  `on_band_joined`
  (`role_handler.cpp:263-275`) is a bare `band_index_[name] = presence`;
  `band_index_` is `unordered_map<string, Presence *>` (`role_handler.hpp:349`).
  A dropped BAND_JOIN leaves the role unable to answer "did I join?" without
  re-querying the broker, and a dropped BAND_LEAVE leaves stale routing.

Two are independent dead-residue items:

- **X2 — `BrokerRequestComm::query_shm_info` is dead.**  Declared
  `broker_request_comm.hpp:447`, defined `:1506`, **zero callers** in `src/` or
  `tests/`.  Independently confirmed as O1b in
  `REVIEW_VALIDATION_2026-08-02.md`.  Delete method + declaration, and check
  whether its broker handler is then also unreachable.
- **X4 — phase-label comments survive.**  `engine_host.hpp:425`,
  `role_api_base.hpp:948,955` carry `M4f`/`M4c` labels.  The project rule allows
  task IDs and forbids phase labels precisely because the wave numbers mean
  nothing to a later reader.

### Comment sites naming retired symbols (found 2026-08-07)

Small, but this is the class of drift that makes a reader chase a symbol that
is not there:

- Three sites name `KnownRolesStore::as_peer_allowlist`, retired under #83 in
  favour of `PubkeyOriginIndex::zap_allowlist()` (`pubkey_origin.hpp:325`):
  `role_identity_policy.hpp:16`, `role_identity_policy.hpp:45`, and —
  ironically — `pubkey_origin.hpp:29`, the header of the type that replaced it.
- `role_identity_policy.hpp:25` holds the only `TODO(followup)` in `src/`: the
  file now defines only `KnownRole` and should be renamed (`known_role.hpp`)
  with its ~5 includers updated.  Recorded here so it is not a comment-only
  item.  Fold the comment fixes into the rename.

### Band 4 — `should_continue_loop()` / `should_exit_inner()`: adopt or delete

`role_host_core.hpp:516,525`. Both are defined, both have **zero** production
callers, and both have tests in `test_role_host_core.cpp`. Tested-but-uncalled
is worse than plain dead code — the tests make it read as live, so a reviewer
skimming for dead code finds coverage and moves on.

The choice is adopt-or-delete, and neither is right to make on its own:

- **Adopt** means rewriting the loop condition in three role hosts. A subtly
  different condition there is a hang or a premature exit — not a compile
  error, and not necessarily a failing test.
- **Delete** throws away the correct abstraction immediately before band 4
  (role-host unification, #292/#55) collapses those same three loops.

So it belongs **inside** band 4, where the three loops are being merged
anyway. Whatever the outcome, the tests move with the decision — do not leave
them testing an uncalled helper.

Carried here 2026-08-03 from `REVIEW_FullModule_2026-04-06.md` (finding B-1),
which was the only place it lived. That review is now archived; this is the
surviving record.

### Closed task sections removed 2026-08-07

Three sections here were titled `#92`, `#88` and `#94` and described work the
live task list records as **completed**.  Verified against code before
removal:

- **`#92` security review** — a completion note, not an open item.  Its one
  carry-forward is kept: `ShmQueue` has no `running_` flag at all;
  `is_running()` is derived from whether a DataBlock is attached, which makes
  the S-8 failure *unrepresentable* rather than merely fixed.  The three
  ZMQ-side queue classes each carry an `std::atomic<bool> running_` guarded
  by scope guards.  `ShmQueue` shows the shape that removes the question —
  a convergence target if the band-4 activation-state work goes ahead.
- **`#88` thread-spawn resource failure** — fixed.  `thread_manager.cpp:528`
  now documents and handles the one genuine OS resource failure
  (`pthread_create` returning `EAGAIN`, surfacing as `std::system_error`)
  through the non-throwing channel, distinguishing it from every other
  refusal in the function.
- **`#94` HEP-0021 §16.5 ephemeral binding** — a production
  `send_endpoint_update` caller exists at `role_api_base.cpp:2129`
  (binding-side/fan-in endpoint publish).  The section already carried a
  "STATUS CORRECTED" note saying so and was never closed.

Detail in git (`git show c91248dd:docs/todo/API_TODO.md`).

### #89 — SMS expansion + vault design (retained key, script vault, config reload)

**Filed 2026-07-29.**  One task because these are the same foundation: reloading
config out of the vault at runtime and letting a script save/load its own vault
entries both need the vault openable *after* startup without the password being
reachable.  Detail lives on task #89; the headlines that other work must not
contradict:

- **`api.vault_save` / `api.vault_load` do NOT exist.**  Confirmed from the code,
  not the HEP: `key_store.hpp:215` and `:297` both say "NOT implemented —
  deferred, #106".  What shipped is the storage half (`add_raw` / `lookup_raw`,
  HEP-CORE-0043 §7), sketched for HEP-0038 and now serving the admin-session
  seal key instead.
- **No runtime reload exists.**  `publish_peer_authority()` runs once, at broker
  construction; `plh_hub --add-known-role` writes the vault and does not talk to
  a running hub.  Adding a role means a restart.
- **Live hole:** the vault master password sits in the environment in cleartext
  for non-interactive startup, and `lua_state.cpp:70` does not disable
  `os.getenv` while the Python engine has no sandbox at all.  A user script can
  read the key that opens the hub's private identity, the admin token and the
  allowlist — breaking the isolation contract asserted at `hub_config.cpp:358`.
- **The system-vs-script vault directory separation was discussed and its
  outcome recorded as UNRESOLVED** (HEP-0038 §2.2 lists three candidate shapes
  and calls the API "shape-agnostic on purpose").  It is a requirement, not an
  option, and belongs in #106's scope.
- **Not in the HEP's own open-questions list:** KeyStore name sandboxing.  A
  retained vault key would share the store with `hub_identity`; namespacing is
  what stops a script asking for the key that opens everything.

### #85 — `plh_hub` CLI hangs at exit; shutdown diagnostics are mute in Release

**Discovered 2026-07-27** during the #84 verification sweep.  **Cause NOT
established — do not close this on a plausible story.**  Two items:
(A) a real hang of unknown cause, and (B) a diagnosability defect that is
established by code and is *why* (A) cannot be diagnosed from the logs.

**The test.**  `PlhHubCliTest.AddKnownRole_EachValidRole_AcceptedAndListed`
(`tests/test_layer4_plh_hub/test_plh_hub_known_roles.cpp:210`), whose `l` is
`plh_hub --config <cfg> --list-known-roles`.  Normal runtime 1.52–1.77 s;
on failure it is SIGTERM'd at the L4 60 s ctest timeout (exit 143).

**(A) Observed facts only.**  The subprocess finishes its work and reaches
teardown in ~83 ms, then never exits; the last line in both captured
failures is `ZMQContext: ZeroMQ context destroyed.`  Seen once inside a
full Release sweep (`-j2`) and once in 5 standalone runs immediately after
that sweep; then 15/15 and 12/12 clean on an idle machine.  Frequency
correlates with recent heavy activity, but that correlation is weak
evidence and is not a diagnosis.

**Two earlier claims here were wrong and are retracted.**  (1) The context
being destroyed on a non-main thread is *expected*: ZMQContext registers
`set_shutdown(fn, timeout)` and the timed path runs the callback on a
worker thread by design (`lifecycle_helpers.cpp:65`).  (2) "Load-dependent
timeout leaving a detached runaway thread" is a hypothesis, not a finding.
The framework is *designed* to warn on shutdown timeouts, so a silent hang
is not explained by a timeout — unless the warning cannot be emitted,
which is exactly item (B).  Settle (B) before believing any timeout story.

**(B) ✅ RESOLVED 2026-07-29 (#86).**  As originally established: `finalize()`
accumulated every breadcrumb into a local `debug_info` string and emitted it
**once** after Phase 3, so a hang mid-finalize discarded the whole record — and
that single emit was `PLH_DEBUG`, compiled out of Release.  A detached runaway
shutdown thread was an invisible event in a release build.

Now: every step is written **as it happens** to the debug module's last-resort
buffer (HEP-CORE-0048) — fixed storage, no queue, nothing to flush — and
`finalize()` calls `trace_print()` unconditionally at its end.  A module that
overruns its deadline, throws, or cannot spawn its worker marks the report
dirty, and a dirty report prints in **every** build.  Logger's own shutdown
brackets its two blocking steps (`worker_thread_.join()`,
`callback_dispatcher_.shutdown()`) directly into that buffer, since it cannot
use `LOGGER_*` while joining the thread that would drain the queue.  The
SIGTERM watcher prints it too, which covers the signature every observed
instance of (A) has actually arrived as.

**(A) is still open and its cause is still unknown.**  Do not close it on a
plausible story.  What changed is only that the next recurrence should be
readable: a marker with no matching exit names the step that hung.

**Follow-up.**  (1) ✅ done — see (B) above.  (2) Wait for a recurrence with real
breadcrumbs, or attach `gdb -p <pid> -batch -ex "thread apply all bt full"`
to a caught instance.  (3) Only then name a cause.

Operator impact: `--list-known-roles` is operator-facing, so a scripted
provisioning step can wedge indefinitely — worth fixing regardless of rate.
Evidence preserved at `build-release/Testing/logs/ctest-20260727-143417-*.log`
(sweep) and `ctest-20260727-143953-*.log` (standalone).

### Demo-harness audit follow-ups (2026-05-21)

> **⚠ The `#10x` IDs in this section are from a retired numbering scheme and
> collide with live task IDs.** Legacy `#102`/`#103`/`#104`/`#105`/`#106` here
> mean runtime key handling / HEP-0017 §3.3 / sibling-HEP sync / federation
> design / script-vault — the live tasks with those numbers are entirely
> different items. Read the description, not the number.
>
> Only the first entry was verified on 2026-08-07 (and turned out to be
> superseded, see below). **The rest of this section is unverified** — treat
> each as a claim until someone reads the code. Known live successors:
> legacy `#105` federation → **#69**; legacy `#106` script vault → **#89**.

Open items left over from the multi-engine demo session that found
13 bugs (B1–B13).  B1, B2, B5, B9, B11, B12, B13 closed inline (git
log).  B3 closed via Task #78 (see archive).  Remaining items below
are filed but not yet fixed; each is a tightly-scoped single-area
change.

- **~~Runtime key handling (HEP-CORE-0035 §4.7)~~ — SUPERSEDED, DO NOT BUILD.**
  This item proposed a shared `src/utils/security/runtime_key_handling.{hpp,cpp}`
  with `disable_core_dumps()` and a `SecureKeyBuffer` RAII wrapper around
  `sodium_malloc`/`sodium_memzero`.  **It was superseded on 2026-06-05 by the
  HEP-CORE-0040 chain, which all shipped — but the supersession was recorded in
  `AUTH_TODO.md` and never here, so it sat at the head of this list as buildable
  work for two months.**

  Verified 2026-08-07: the functionality exists inside the security module, not
  as a separate file.  `secure_subsystem.hpp:127-128` documents the hardening
  (`setrlimit(RLIMIT_CORE, 0)` + Linux `prctl(PR_SET_DUMPABLE, 0)`),
  implemented in `secure_subsystem.cpp`; `key_store.cpp:70` relies on the
  process-wide `PR_SET_DUMPABLE=0` it sets.  `SecureBuffer` covers the RAII
  wrapper.

  **Building the proposed file today would add a second security surface beside
  `SecureMemorySubsystem`** — exactly what the "one module owns libsodium"
  direction (#121) is consolidating away from.  If a §4.7 gap is found, it is
  closed *inside* SMS, never in a sibling file.

**Verified 2026-08-07 — the rest of this chain has closed.** Every remaining
`#10x` item below was checked against the tree; the dependency chain they
formed (`#101 + #102 → #74 → #94 + #103 → #104 → #106 → done`) is spent.

- **Legacy `#103` — dynamic peer membership — ✅ SHIPPED.**
  `ZmqQueue::set_producer_peers` / `add_producer_peer` / `remove_producer_peer`
  all exist (`hub_zmq_queue.hpp:461`, `:466`, `:472`), and fan-in is proven
  end-to-end at L4 (`ZmqE2E_MultiProducer_TwoAuthorized`). The singular
  `RxQueueOptions::zmq_node_endpoint` survives on purpose — it is the queue's
  *own* bind endpoint (may be `tcp://host:0` for ephemeral bind, resolved and
  published via `ENDPOINT_UPDATE_REQ`), a different thing from the peer set.
  The "replace singular with vector" framing made them sound like one field.

- **Legacy `#104` — HEP-0036 §14 sibling-HEP code updates — ✅ CLOSED**, part
  shipped and part superseded:
  §14.1's `wants_shm_secret` REG_REQ field **does not exist and must not be
  built** — it belonged to the AUTH-4 `shm_secret` design that HEP-0041 retired;
  §14.2 PubkeyOrigin index consumption shipped (#83); §14.3 `Authorized` on the
  role-side FSM is in `role_presence.hpp:120`; §14.4 is legacy `#103` above;
  §14.5 inbox CURVE on the identity keypair shipped (#61); §14.6 band CURVE is
  covered — bands ride the broker CTRL ROUTER, which arms
  `arm_curve_server` at `broker_service.cpp:1134`; §14.7 was documentation
  only; §14.8 `ChannelAccessEntry` shipped.

- **Legacy `#105` federation** → **#69** (parked, design-first).

- **Legacy `#106` script-accessible vault** → **#89**, and **its gate is now
  open.** The item recorded "Depends-on: #104 shipping first"; #104 is closed,
  so nothing blocks it. Still genuinely unbuilt: `api.vault_save` /
  `api.vault_load` appear nowhere in `src/`, `key_store.hpp:297` records the
  binding as deferred, and HEP-CORE-0038 is still 🚧 DRAFT. Both of those
  sites cite the dead ID `#106` — re-anchor them to **#89** when it is picked
  up.

*(The note that used to sit here about `#74` subsuming HEP-0035 §4.8 — the
known-roles allowlist inside the hub vault, plus the `--add-known-role` /
`--revoke-known-role` / `--list-known-roles` CLI — is closed: the vault
allowlist shipped 2026-07-19 as CURVE-review item (a), a hard cutover where
the hub refuses to start while a plaintext `known_roles.json` exists.)*

### Pre-existing renovation follow-ups

- **Wave-MD1 sweep — now task #132, and it is worse than "not adopted yet".**
  Verified 2026-08-07: `with_active_loop` (`thread_manager.hpp:92`,
  HEP-CORE-0031 §4.1) has **zero production callers** — every hit in `src/`
  is its own definition or docstring — while carrying a full L2 test binary
  (`test_thread_manager_active_loop.cpp`). None of the three named owners
  (`BrokerService` ctrl/admin threads, the `AdminService` worker, the
  `HubHost` admin thread) mentions it.

  **Same shape as the band-4 `should_continue_loop()` item above: defined,
  tested, uncalled.** The coverage makes it read as live, so a reviewer
  skimming for dead code finds tests and moves on. Two independent instances
  is a pattern, not a coincidence — this codebase lands abstractions ahead of
  their adopters. Adopt or delete, and move the tests with the decision.
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
