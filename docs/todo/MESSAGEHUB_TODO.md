# Messenger / Broker TODO

**Scope:** MessageHub / broker protocol / federation / hub-side
connection lifecycle.
**Canonical strategic status:** `docs/TODO_MASTER.md` § "Current Sprint
Focus".  This file holds broker-specific detail for open items only;
completed work lives in git history + DOC_ARCHIVE_LOG.

**Completed-work archive:** `docs/archive/transient-2026-06-05/todo-completions/MESSAGEHUB_TODO_completions.md`
(A1 `ctx_band_leave` semantic bug — was listed as "D1 must-fix" but
the fix is in production code at `native_engine.cpp:289-305`).

---

## Current Status (broker-specific summary)

| Track | Where it stands | Active item here |
|---|---|---|
| **Arc A — `plh_hub` renovation** (HEP-0033 §15 Phase 1..10) | Phases 1-9 shipped; Phase 10 doc-amendment ⏳ partial. | HEP-0033 Phase 10 (task #73) |
| **Arc B — role-host renovation** (Wave-B M0..M9) | M0..M9 shipped (M9 closed 2026-05-26). | — |
| **HEP-CORE-0035 auth** | 🚧 partial — Phase B + #101 + D1 + D2 + D3 shipped (per `AUTH_TODO.md`).  D4–D7 open.  Critical-path for production readiness. | task #74; detail in `docs/todo/AUTH_TODO.md` |
| **HUB_TARGETED_ACK wire frame** (HEP-0033 §12.3.6) | ⏸ Deferred — folded into the consolidated federation design task #69 (2026-07-24); never lands standalone | #69 (was #75) — federation-only |

Wave-M2 / Wave-M2.5 / Wave-M3 side-arcs all closed.  M1.2 / M1.4 /
M1.5 / MD1 / MD1.5 all closed.

### Schema/metrics query integration (task #95 resolved: KEEP + INTEGRATE, 2026-07-26) 🚧 DESIGN DRAFTED

The 2026-07-26 wire-inventory audit found `SCHEMA_REQ`/`SCHEMA_ACK` and
`METRICS_REQ`/`METRICS_ACK` are complete broker-side mechanisms with NO
client half (no BRC method, no role-API accessor, no engine binding) —
the only two HEP-0033 Class-C queries whose client plumbing was never
built.  User ruling: these are built mechanisms for schema communication
and metrics reporting — integrate them for a schema-driven role
ecosystem rather than delete.

Design draft (scenario-driven, gap register G1–G6, 4 slices, 3 ⚖
decisions pending user ruling):
`docs/tech_draft/DRAFT_schema_metrics_query_integration_2026-07-26.md`

- [x] ⚖ Decisions resolved 2026-07-26: G1 = schema rides the REG/ACK
      (user); G7 = Option B, owners always declare (user); G3
      member-gating + slice order 1→2→3→4 proceeding on
      recommendation; segment stays fingerprints-only.
- [x] Slice 1a — open-row validation SHIPPED 2026-07-26 (full ctest
      2676/2676): SCHEMA_REQUIRED on material-free fan-in open (SI-1
      broker half), owner-citation self-consistency (G8), anonymous
      producer structure⇒hash rule (G8b); 5 L3 pins; HEP-0007
      taxonomy rows (SCHEMA_REQUIRED new; FINGERPRINT_INCONSISTENT /
      MISSING_HASH widened); test helpers
      `apply_owner_citation`/`apply_matching_producer_schema` +
      `register_fanin_owner`/`register_fanin_producer`.
- [x] Slice 1b — query plumbing + gating SHIPPED 2026-07-26: BRC
      `get_schema`/`get_channel_schema`/`get_channel_metrics` +
      RoleAPI pass-throughs (Class-C routing); both messages moved to
      the `Control_EnvelopeWithRoleUid` tier (caller `role_uid`
      identity-bound — superseded the "identity-aware signatures"
      approach, no handler-signature change needed); channel-form
      member gating (NOT_A_ROLE_OF_CHANNEL) + `(owner,id)` form
      known-role-open; METRICS all-channels wire branch retired
      (channel_name required); 10 worker call-sites + L1 tier pins
      migrated; new pins SchemaReq_ChannelForm_MemberGated +
      MetricsReq_MemberGatedPull; HEP-0007 §12.2.1 caller note
      flipped, NOT_A_ROLE row widened (0036 §6.6).  Typed
      SchemaReqBody/MetricsReqBody stay on the HEP-0046
      EnvelopeOnly→typed follow-on list with the notify bodies.
- [x] ✅ Slice 1c SHIPPED 2026-07-26 (G9+G10 → SI-9): the open row
      validates the owner axis it installs.  `expected_schema_owner`
      ∈ {"", "hub"} (else SCHEMA_FORBIDDEN_OWNER); named fan-in open ⇒
      owner="hub" REQUIRED (new code SCHEMA_OWNER_REQUIRED) + registry
      resolution via `_validate_schema_citation`
      (`check_registry_record`; SCHEMA_UNKNOWN /
      FINGERPRINT_INCONSISTENT) + record structure MATERIALIZED into
      the channel invariants when the citation carried none; joiner
      consumer owner claims exact-matched (`sin.cited_owner`);
      owner-without-schema_id → INVALID_REQUEST both sides.  Broker
      worker gained the `hub_globals` profile (production
      `load_hub_globals_` walker from `<temp_dir>/schemas`).  Pins:
      `FanInOwnerOpen_OwnerAxis_Rejections`,
      `FanInOwnerOpen_HubGlobal_ResolvedAndServed` (incl. the G10
      materialized channel-form read),
      `ConsumerJoin_OwnerClaim_ExactMatch`,
      `Reg_OwnerWithoutId_Rejected`.  Full sweep 2682/2682 green.
      HEP-0007 rows (SCHEMA_OWNER_REQUIRED new; FORBIDDEN_OWNER /
      UNKNOWN / CITATION_REJECTED widened); HEP-0034 §10.2 owner-axis
      note.  (Packing thread RESOLVED same day — no storage/delivery,
      the fingerprint binds it; HEP-0034 §6.4 + I10 document the full
      chain.)
- [x] ✅ Slice 2 SHIPPED 2026-07-26: 3-engine `api.get_schema(owner,id)`
      / `api.get_channel_schema(ch)` / `api.get_channel_metrics(ch)`,
      executed per the committed map.  Return contract everywhere: the
      FULL broker reply as data (Lua table / Python dict / native JSON
      string via thread-local scratch); nil/None/NULL only on transport
      failure.  Lua: 3 closures + `push_common_api_closures_` rows.
      Python: 3 methods × producer/consumer/processor APIs + pybind
      `.def` rows (triplication accepted until #292).  Native:
      `get_schema_json` / `get_channel_schema_json` /
      `get_channel_metrics_json` appended before the opaque tail,
      `PLH_NATIVE_API_VERSION` 12→13 + version-log entry, CLEAN rebuild
      of utils+scripting done; hub-side ctx routes them to NULL stubs.
      Docs: HEP-0028 §4.7 table rows; README_topology_channels §5.
      Pins: `Api_SchemaMetricsQueries_WithoutBroker_ReturnNil` (Lua —
      nil + empty-arg raise), `Api_SchemaMetricsQueries_Graceful_NoBroker`
      (Python — None, no raise); broker-side reply shapes + gating
      already L3-pinned (slice 1b/1c).  DEFERRED from the map: the L4
      metrics-adaptive-script keystone — tracked below.
- [x] ✅ Slice-2 review pass 2026-07-26 (3-engine parity + language
      conventions): all three engines verified faithful to their local
      conventions (Lua dot-closures + raise-on-empty; Python
      kwargs + GIL-release + pass-through; native `*_json` scratch +
      appended-ABI discipline).  Fixes shipped: native L2 no-broker pin
      (`Api_SchemaQueries_NoBroker_GracefulReturn` — closed the
      test-parity break), README_Deployment api-catalog entries,
      native NULL-sentinel precision in README_topology_channels §5,
      `discover_channel` ERROR→WARN harmonization, and the
      query-surface graceful-degrade contract made EXPLICIT in
      IMPLEMENTATION_GUIDANCE § "Role-side query surface" (WARN-only
      on transport failure, typed rejections as data, registration
      keeps ERROR; enforced by the three per-engine L2 pins).
- [x] ✅ Slice-2 follow-up SHIPPED 2026-07-26 — both halves as L4
      scenarios in `test_plh_hub_role_zmq_e2e.cpp`:
      `ZmqE2E_MetricsAdaptiveConsumer` (keystone — consumer script
      pulls `get_channel_metrics` MID-RUN and adapts; pins the full
      metrics loop: producer `report_metric` custom + built-in
      `out_slots_written` → heartbeat piggyback → HubState → member-
      gated pull → script dict → decision; adapt marker carries the
      pulled values, parent regex-pins uid/threshold/custom-roundtrip/
      self-row + adapt→complete order) and
      `ZmqE2E_NativeProducer_LiveSchemaQueries` (native smoke — real
      `plh_role` loads `test_l4_native_producer_plugin` from the
      production `script/native/plugin.so` layout; first produce tick
      queries all three v13 `get_*_json` against the live broker:
      hub-global `$l4.native.frame.v1` from `<hub_dir>/schemas/`,
      channel form, metrics pull — single all-1s marker pinned).
      3× repeat green; full ZmqE2E suite 11/11.
- [x] ✅ Slice 3a SHIPPED 2026-07-26 (`756028e1`) — schema rides
      CONSUMER_REG_ACK (HEP-0034 §10.3a new): five optional fields
      filled from the channel record by the unified success-ACK
      builder, empty axes elided, NO packing on the wire;
      ConsumerRegAckBody accessors + validate_if_present rows.  Pin:
      `ConsumerRegAck_CarriesEstablishedSchema` (content + elision +
      no-packing).
- [x] ✅ Slice 3b SHIPPED 2026-07-26 (`b782b6a1`) — schema-pending
      queue: reader factory empty-schema → pending Standby build;
      `QueueReader::configure_slot_schema` (Standby-only, single
      establishment, factory-grade validation via shared
      `validate_schema_fields`); apply/is_configured/start all refuse
      while pending (SI-6).  Pins: 3 new + the empty-schema factory
      pin updated to the split contract (writer rejects, reader
      pends).
- [x] ✅ Slice 3c SHIPPED 2026-07-26 — runtime resolution end-to-end:
      `parse_canonical_fields_str` (lossless BLDS inverse) +
      `recover_zone_packing` (§6.4 two-candidate recovery = SI-6 pin
      verification) in schema_utils; role-host
      `resolve_runtime_slot_schema` in `apply_consumer_reg_ack`
      (empty-format abort, both-zone fingerprint verification,
      absent-flexzone consistency, queue install + core in-spec,
      `event=RuntimeSchemaResolved`); `"from-channel"` sentinel
      (`SchemaSpec.runtime_resolved`, resolve_schema-only) + SI-7
      gates (writer / fan-in-owner / SHM-v1 / missing-schema-stays-
      an-error) + citation-free wire join.  Pins: 4 unit (parser +
      recovery), 2 sentinel (resolve + wire builder),
      `FromChannel_Si7Gates` (5-gate worker, ERROR content pinned),
      L4 keystone `ZmqE2E_FromChannelConsumer_ResolvesAndReceives`
      (QueueSchemaPending → RuntimeSchemaResolved →
      QueueSchemaConfigured → consumption; `rx.slot is None` pinned
      as the slice-3 script contract — slice 4 flips exactly one
      assertion).
- [ ] Slice-3 residuals: SHM runtime-resolved consumer (ACK delivery
      + segment-header cross-check before mapping — v1 refused with a
      named config error); optional config PIN field for from-channel
      consumers (SI-6 "config pin when present" hook — the delivered-
      fingerprint verification already runs; a config pin would bind
      it to operator expectation).
- [x] ✅ Slice 4 SHIPPED 2026-07-27 (G4 — runtime slot proxies): after
      the SI-6 chain closes, `apply_consumer_reg_ack` registers the
      RESOLVED spec with the engine (`InSlotFrame`) + the step-5 size
      cross-check, BEFORE queue activation — same worker thread as the
      startup registrations, so no engine synchronization (contract
      documented in script_engine.hpp::register_slot_type + HEP-0034
      §10.3a).  Native registration doubles as the adoption gate
      (compiled exports must match the resolved format, else
      activation refused).  Resolved flexzone: verified, deliberately
      NOT registered (ZMQ rx has no fz data plane; SHM path owns it).
      3-engine direct verification (parity rule): Python — L4 keystone
      flipped to the generic-consumer contract (`slot_is_none=False`,
      valid==N content pin, resolve→register→consume order pin); Lua —
      `LateSlotRegistration_G4` (nil before / typed decode after on
      the same engine); Native — `LateSlotRegistration_G4_Gate`
      (mismatch refused with pinned ERROR, match registers).
- [ ] G5 doc folds (HEP-0034 §10.3 lifetime rules incl. fan-in
      dual-lifetime "consumers pull by channel"; HEP-0019 freshness;
      HEP-0007 §12.3 SCHEMA-vs-DISC complementarity note).
- [ ] G2 → #292: observer (control-plane-only) role kind named as a
      unification requirement.

### Envelope-framework fresh-eyes review — 3 ratifications + doc corrections (2026-07-24) ✅

Second full review of the typed-envelope framework (2 independent
verifiers + targeted pass) after Phase B closure.  Core verdict: wire
behavior sound (frame layout, gate order, §14.7 conformance of all nine
handlers, adapter triple list all verified consistent).  Three
ambiguities RATIFIED + drift tail fixed:

- **role_name = display-only, deliberately unvalidated (ratified).**
  Two HEPs claimed boundary grammar enforcement that existed nowhere
  (the gate skips it; the "commit callback" it deferred to was the
  retired skeleton).  No enforcement added — the validated name lives
  inside `role_uid` by construction; consumers treat role_name as
  untrusted display text.  HEP-0023 §2.5.4 (now also documents the
  two-layer grammar model: gate sanity check + HubState full
  `is_valid_identifier` re-check), HEP-0046 §14.5 step 3 + §14.3 note,
  gate + ctor comments all reconciled.
- **STALE_INSTANCE ERROR reply ratified (was "silent drop").**  The
  doc's no-reply text assumed stale ⇒ dead sender; the live re-REG
  race (role_api_base handles it by name) needs the error, and
  correlation keying + unroutable-drop make the reply harmless to dead
  senders.  HEP-0042 §5.4 (steps reordered to shipped
  guard→advance→drain→reply, + ratification note), §5.5.2, §12
  diagram; the broker comment's phantom "Phase 2.4" apology deleted.
- **ChannelAuthAppliedAckBody aligned to the emitted shape** —
  `{status, channel_name, applied_version}`; the draft-era
  `confirmed_version` (never emitted, yet validated by the BRC's live
  inbound check) retired; reply-value semantics (post-clamp confirmed
  version) documented in §5.5.2 + §14.3; L1 re-pinned.
- Drift tail: §7.1 reject-code rows corrected (ctor→BODY_SCHEMA_VIOLATION;
  gate grammar/pubkey-length→INVALID_REQUEST — fixing a misattribution
  introduced in the earlier §7.1 edit); §14.2 illustrative API renamed to
  the shipped build_*_send/parse_*_recv (no `body_as` — typed
  construction is the dispatch layer's job, stated); §14.4 example
  rewritten to receive_and_validate + std::visit; §14.3 catalog
  (producer_hostname/metadata added, spurious ConsumerRegAck
  correlation_id body field removed, channel_topology marked optional);
  §14.5 step 5 now names UNKNOWN_ROLE; HEP-0036 §5b.4/§5b.6 retire the
  separate `inbox_packing` rows (Forbidden tables) + gain
  abi_fingerprint/build_id/channel_topology rows; §5b.7 gains
  broker_abi_fingerprint/broker_build_id/known_roles; HEP-0042 §12
  consumer-attach msg_type fixed (_ZMQ) + §5.5.2 documents the
  registration/anti-poisoning guard.

### Wire-field reconciliation — APPLIED_REQ strict contract + consumer transport (2026-07-24) ✅

- **CHANNEL_AUTH_APPLIED_REQ strict wire** (HEP-0042 §5.5.2 amendment
  2026-07-24, migration window closed): `role_type` REQUIRED
  ("producer"|"consumer" — ctor `require`s it; the dead absent→"producer"
  handler default deleted), `instance_id` always present (producer echoes the
  §5.5.3 shift number; consumer sends 0, broker ignores), `producer_role_uid`
  alias retired (BRC dual-write deleted; broker never read it).  §5.5.3 gained
  the normative `instance_id` definition (fencing token — NOT identity, NOT an
  index) + hop-by-hop integration table + crash-restart race diagram.  L1 pins:
  `ChannelAuthAppliedReqBodyRejectsMissingRoleType` (+ fixture now carries
  role_type / consumer-shape instance_id=0).
- **Consumer transport arbitration moved onto `data_transport`** (HEP-0036
  §5b.6): the handler now value-checks (`∈{shm,zmq}` else INVALID_REQUEST) and
  arbitrates the REQUIRED `data_transport` (was: DELETE-scheduled
  `consumer_queue_type`, which production never sent — the §5b.6 reject was
  unenforced in production).  Fan-in open path stores the declared transport
  (silent `"zmq"` default removed).  `consumer_queue_type()` accessor deleted.
  §5b.6 gained the explicit two-path arbitration table.  L3 re-pins:
  `TransportMismatch_ShmProducer_ZmqConsumer_Fails` +
  `TransportMatch_ShmConsumer_ShmProducer_Succeeds` now drive
  `data_transport`; `TransportMatch_NoDriverField_AlwaysSucceeds` (pinned the
  abolished no-field-skips-arbitration behavior) replaced by
  `TransportValue_Bogus_RejectedInvalidRequest`.
- **HEP-0036 §5b.4/§5b.6 `role_name` rows** corrected YES→OPTIONAL (rationale
  owned by HEP-0046 §14.3); HEP-0046 §14.3 catalog entries for
  `ChannelAuthAppliedReqBody` (adds role_type) + `HeartbeatNotifyBody` (adds
  role_type / producer_pid / metrics) reconciled with the shipped ctors.
- Verified clean in the same field audit (no action): `instance_id` (live
  HEP-0042 fencing token), `applied_version`, `snapshot_version`,
  `broker_observer_pubkey_z85`, `producer_hostname`, `metadata`,
  `consumer_pid`/`consumer_hostname`, `channel_topology`, `flexzone_*`,
  `inbox_*`.

### Phase-B drift cleanup — stale refs, pairing-rule completion, optional-absent pins (2026-07-24) ✅

- **Stale-doc/comment sweep**: `wire_dispatch.hpp` header reconciled (typed
  pathway COMPLETE; correct gate list — no key-rotation gate, role-tag added;
  RegFamily vs authenticated tier split documented); retired-symbol citations
  (`validate_identity_fields`, `verify_known_role_binding`) replaced with the
  live `gate_grammar` / `gate_known_role_binding` references across
  role_uid.hpp, hub_state.hpp, hub_state.cpp, admission_gates.cpp,
  broker_service.cpp; dangling "zmq_pubkey enforcement above" comments
  re-pointed at gate_grammar; all "(B.1x)"/"Phase B" phase labels stripped
  from broker_service.cpp comments (no-phase-labels rule).
- **HEP-0046 internal consistency**: §14.7 no longer claims the envelope
  carries broker_proto; §12 step 6 + §14.7 now cite §14.5 steps 1-6/7-8
  correctly; §14.5 step 5 names `gate_known_role_binding`; §7.1 rejection
  table split into BODY_SCHEMA_VIOLATION (missing/wrong-typed/grammar, at
  parse) vs INVALID_REQUEST (semantic value checks, in handler) — matching
  the shipped wire + L3 pins.
- **§14.3 pairing rule fully applied**: every optional field on every wire
  body now has `validate_if_present` in its ctor (53 call sites) — wrong-typed
  optionals reject as BODY_SCHEMA_VIOLATION at parse instead of INTERNAL_ERROR
  mid-handler; wrong-typed `metadata` is now rejected rather than silently
  ignored.
- **Optional-absent test pins**: L1 `ProducerRegReqBodyOptionalFieldsAbsentDefaults`
  + `RejectsWrongTypedOptional` + first-ever `ConsumerRegReqBody` L1
  construction (`ValidatesRequiredFields` + `OptionalFieldsAbsentDefaults`) +
  `HeartbeatNotifyBodyOptionalFieldsAbsentDefaults`; L3
  `RegReq_WithoutRoleName_Succeeds` (end-to-end regression pin for the
  2026-07-24 role_name accessor crash).
- Still open (deliberately): direct `receive_and_validate` unit test (B.4
  residual drift-guard), id_frame/ABI-probe dedup cosmetics, stale 3-frame
  comments in `broker_wire_client.h`.

### PID is debug/record only — never a validation input (2026-07-24) ✅

- **Design ratified**: a PID is machine-local and meaningless to a hub on
  another host, and `role_uid` is already the authoritative unique key (same-uid
  REG is a restart-replace, so a channel never holds two presences under one
  `role_uid`).  So `producer_pid` / `consumer_pid` may be transferred, stored,
  and logged for debug/record, but **no broker decision may read a PID**.
- **DEREG + CONSUMER_DEREG resolve by `role_uid` ALONE** (was the residual
  `(pid, role_uid)` tuple).  The tuple had only ever added `role_uid` to fix
  pid-alone raciness; the pid half was redundant.  broker_service.cpp `handle_dereg_req`
  + `handle_consumer_dereg_req`.
- **Heartbeat**: removed the `LOGGER_ERROR("missing or zero producer_pid")` — a
  debug field's absence is not an error.  This is the **#308 source-side fix**, so
  the `datahub_metrics_workers` ERROR allow-list entry retired with it.
- **Docs**: HEP-CORE-0023 §"role_uid is the sole key" + "A PID is debug/record
  only"; HEP-CORE-0007 DEREG/CONSUMER_DEREG effects + NOT_REGISTERED row reconciled.
- **Test**: `broker_dereg_pid_mismatch` → `broker_dereg_ignores_pid`
  (`DatahubBrokerTest.DeregIgnoresPid_ResolvesByRoleUid`) — now pins that a wrong
  pid + correct role_uid SUCCEEDS, with a repeat-DEREG side-effect check.
- **NOT retired (debug/record, intentionally kept)**: pid on the wire (REG/DEREG/
  HEARTBEAT), `ProducerEntry.producer_pid` storage, admin/list/snapshot pid fields,
  and the SHM data-plane crash-detection pid (a separate, co-located mechanism).

### Schema registry — two-zone unification + inbox-record removal (2026-07-22) ✅

- **Two-zone `SchemaRecord`**: one record carries datablock + flexzone un-merged;
  single 64-byte `datablock_half ‖ flexzone_half` fingerprint (each half
  `BLAKE2b(zone_blds||"|pack:"||packing)`, absent zone = zero half, never
  all-zero). Unified API in `schema_utils.hpp`: `compute_zone_hash`,
  `compute_fingerprint_from_wire` (was `compute_canonical_hash_from_wire`),
  `make_schema_record` (THE single builder), `schema_records_equivalent`,
  `verify_request_fingerprint`. Wire `schema_hash`/`expected_schema_hash` now
  128 hex; `SCHEMA_ACK`/DISC_ACK/snapshot return both zones. Data-plane
  `schema_tag` (`compute_schema_hash`) left folded — separate Job-1 mechanism.
  Fixes the SCHEMA_REQ flexzone-loss bug (REVIEW_FullSystem finding).
- **Inbox removed from the registry**: the broker no longer files a
  `(uid,"inbox")` `SchemaRecord`; it only fail-fast validates `inbox_schema_json`
  / `inbox_packing` (`INBOX_SCHEMA_INVALID` / `INVALID_INBOX_PACKING`). Inbox
  schema is discovered as JSON via `ROLE_INFO_REQ` (HEP-0027 §4.0). Registry now
  holds channel schemas only; `make_schema_record` is the sole creation path.
- **Docs**: HEP-0034 (§2.2/2.4/3/4.1/4.3/6.3/9/10/11.4 + Mermaid + worked
  examples), HEP-0027 §4.0 "Inbox initiation & execution", HEP-0033 §19.5. Two
  design drafts archived (`docs/archive/transient-2026-07-22/`); DOC_ARCHIVE_LOG
  updated. Green: 2639/2639.
- **Note — flexzone wiring is already implemented (RETRACTED false "gap" list).**
  An earlier draft of this entry listed a "wiring arc" of flexzone gaps sourced
  from an unverified subagent map; on direct code review those were wrong.
  Flexzone is carried and verified: SHM flexzone identity lives in the data-block
  header (`data_block.hpp:237 flexzone_schema_hash[32]`) and the reader verifies
  it (`hub::RxOptions::{fz_schema,fz_packing,verify_fz}`, `hub_queue_factory.hpp`);
  mismatches are rejected (tests `FlexzoneMismatchRejected` /
  `BothSchemasMismatchRejected`, `test_datahub_schema_validation.cpp`). Flexzone
  is SHM-only by design (ZMQ folds `fz_spec` into the drift `schema_tag` only),
  and flexzone-only SHM channels are supported. The only genuinely-deferred item
  is the runtime role-side `SCHEMA_REQ` *sender* (owner decision (ii): handler
  correct now, sender later) — and even that is optional, since roles resolve
  named schemas from the local cache and the broker validates on REG_REQ.

### REG/REG_ACK Protocol Redesign — HEP-CORE-0046 promoted (2026-07-12)

**Design authority:** `docs/HEP/HEP-CORE-0046-REG-Protocol-Redesign.md`
(promoted from `DRAFT_reg_ack_protocol_redesign.md`, DESIGN LOCKED
with 21 invariants + typed wire envelope + admission-gate
pipeline).

Wire discipline binding rule:
`docs/IMPLEMENTATION_GUIDANCE.md § "REG Protocol Wire Discipline
(HEP-CORE-0046)"`.

**Landed:**
- `WireEnvelope` + typed body classes in `wire_envelope.hpp` +
  `wire_bodies.hpp`.  46 L1 tests in `test_wire_envelope.cpp`.
- Shared gate runners (`run_reg_family_gates` /
  `run_authenticated_reg_family_gates` / `run_control_gates`) in
  `admission_gates.hpp` — LIVE on every message via
  `receive_and_validate`.  L1 `test_admission_gates` (`AdmissionGate_*`).
- `HubState::nonce_seen` replay-bound primitive.  L2
  `test_hub_state_nonce_dedup`.
- ~~`RegAdmissionPipeline` / `BrokerRegHandler`~~ **RETIRED 2026-07-24**
  (parallel test-only skeleton that duplicated the live gates; never
  wired to production — see the retirement note below).

**Phase B (LOAD-BEARING NEXT — task #57).  Sequenced plan (2026-07-23,
code-verified).**  Coverage confirmed: every REG-family typed body already
exists (Phase A; `CONSUMER_DEREG_REQ` reuses `DeregReqBody`) and the recv path
already parses each message to its `Validated*` form — so there is NO body-class
work; the handler rewire is wire-neutral and stageable, with a single atomic
BRC/ACK flip.  Design authority + verified approach: **HEP-CORE-0046 §12
Phase B**.

- **B.1 — broker recv-handler rewire (internal, staged, reviewable).**  Convert
  the 9 hand-parsed handlers to consume the typed `Validated*` directly,
  retiring `to_legacy` / `dispatch_legacy` (`broker_service.cpp:1422-1518`) per
  handler.  Wire-neutral: the reply already ships as a typed `WireEnvelope` via
  `send_reply` — only the handler INPUT flips (JSON-key extraction → typed
  accessors on the uniform `handle_XXX(const WireEnvelope&, const XxxBody&)`
  signature).  **Two
  kinds of work (verified against code 2026-07-23):**
  - **Simple swaps — do FIRST.**  Seven handlers, all already gated in
    `receive_and_validate`: the four authenticated REG-family (`handle_dereg_req`,
    `handle_consumer_dereg_req`, `handle_endpoint_update_req`,
    `handle_channel_auth_applied_req`) via `run_authenticated_reg_family_gates`;
    the three control-family (`handle_disc_req`, `handle_get_channel_auth_req`,
    `handle_heartbeat_req`) via `run_control_gates`.  Each just swaps
    `req.value(...)` for the typed body accessor and builds a typed ack —
    mechanical, behavior-preserving.
    - ✅ **B.1a `handle_disc_req`** (2026-07-23) — pattern-setter (read-only, smallest).
    - ✅ **B.1b `handle_get_channel_auth_req`** (2026-07-23).
    - ✅ **B.1c `handle_heartbeat_req`** (2026-07-24) — fire-and-forget; removed the
      redundant grammar/tag re-checks (gated by `run_control_gates`) and documented
      the surviving blank-field check as observability-only (`_on_heartbeat` is the
      authoritative sink).  Design lesson written up in HEP-0046 §14.7.1 (two
      authoritative guards → zero handler validation); pinned by L2
      `HubStateHeartbeat.BlankOrInvalidFieldsAreNoop`.
    - ✅ **B.1d `handle_dereg_req`** (2026-07-24) — typed `(env, DeregReqBody, socket)`;
      resolves by `role_uid` alone (no pid accessor — see the PID entry above).
    - ✅ **B.1e `handle_consumer_dereg_req`** (2026-07-24) — typed; shares `DeregReqBody`
      with B.1d; `role_uid`-only resolution.
    - ✅ **B.1f `handle_endpoint_update_req`** (2026-07-24) — typed `(env, EndpointUpdateReqBody)`;
      `sender_id` now from `env.identity()` (authoritative I-DEALER-IDENTITY frame).
    - ✅ **B.1g `handle_channel_auth_applied_req`** (2026-07-24) — typed
      `(env, ChannelAuthAppliedReqBody, socket)`; added a `role_type()` accessor
      (load-bearing — drives the registration guard + producer/consumer branch);
      dropped the dead `producer_role_uid` broker-side fallback (current wire always
      carries `role_uid`).
    - **✅ ALL 7 simple swaps landed.** Next: REG_REQ / CONSUMER_REG_REQ (below).
    - ⏳ **Residue follow-on (sender-side):** the BRC `channel_auth_applied` still
      writes a duplicate `producer_role_uid = role_uid` "for pre-amendment brokers"
      (`broker_request_comm.cpp:1219`).  No reader remains (this broker ignores it);
      retire the write in a focused wire-cleanup (it is a wire-shape change → own step).
  - **✅ REG_REQ / CONSUMER_REG_REQ — DONE (B.1h / B.1i, 2026-07-24).** The SAME
    typed-input swap, only larger.
    ⚠ **Framing corrected 2026-07-24:** this is NOT a "relocation into a pipeline
    commit callback."  The HEP-0046 framework types + validates the wire (§14.4/
    §14.7); it does not restructure handler logic.  `handle_reg_req` /
    `handle_consumer_reg_req` convert exactly like the seven above — signature →
    `(const WireEnvelope&, const ProducerRegReqBody&/ConsumerRegReqBody&, …)`,
    every `req.value(...)` → typed accessor, **logic kept in place**, duplicated
    in-handler gate checks deleted.  They are larger only because they read more
    fields, so the swap first adds four still-missing accessors (§14.3):
    `producer_hostname()`, `metadata()` on `ProducerRegReqBody`;
    `consumer_queue_type()`, `expected_schema_owner()` on `ConsumerRegReqBody`.
    The old `BrokerRegHandler` / `reg_admission_pipeline` skeleton (test-only,
    parallel re-implementation of the already-live gates) is **retired**, not a
    target — see the retirement note below.
    - ✅ **B.1h `handle_reg_req`** + **B.1i `handle_consumer_reg_req`** landed
      2026-07-24; the four accessors added; a latent `role_name` accessor bug
      fixed (optional field → `read_string_or_empty`, HEP-0046 §14.3).  **All nine
      REG-family handlers are now typed.**  With the last two arms swapped, the
      `to_legacy` / `dispatch_legacy` / `LegacyDispatchInputs` bridge is dead and
      **REMOVED** — the B.4 legacy-surface retirement, done naturally.
  Each conversion behavior-preserving; existing L2/L3/L4 REG round-trip tests stay
  green.

- **✅ `BrokerRegHandler` / `reg_admission_pipeline` skeleton RETIRED (2026-07-24).**
  A prior arc built a parallel typed REG-admission pipeline (`RegRequest` +
  `RegCommitFn` commit callback) that duplicated the live gates and was invoked
  ONLY by tests — production always ran the handcrafted handlers behind the live
  `receive_and_validate` gate path.  It drifted from HEP-0046's actual purpose
  (type + validate the wire; do not restructure handler logic), so the four files
  (`{include/utils,utils/ipc}/{broker_reg_handler,reg_admission_pipeline}.*`) +
  their L1/L2 tests (`test_reg_admission_pipeline`, `test_broker_reg_handler`) are
  deleted.  Gate coverage is unaffected — it lives on the live path: L1
  `test_admission_gates` (`AdmissionGate_*`), L2 `test_hub_state_nonce_dedup`, L3
  `test_datahub_broker` (`Gate_RegReq_*`/`Gate_ConsumerReg_*`).  HEP-0046 §12 +
  IMPLEMENTATION_GUIDANCE "REG Wire Discipline" rule 2 corrected to match.
- **✅ B.2 — `inbox_schema_json` → typed `SchemaSpec` sub-structure (2026-07-24).**
  The doubly-encoded field is parsed ONCE at body construction: both REG ctors
  run `hub::parse_schema_json(json::parse(s))` (the canonical parser discovery
  already used) and expose `has_inbox_schema()` / `inbox_schema()`; malformed
  content (non-JSON, bare array, missing/invalid in-object packing) →
  BODY_SCHEMA_VIOLATION at the boundary.  The broker's hand-parse block
  (`INBOX_SCHEMA_INVALID` / `INVALID_INBOX_PACKING` handler rejects) is deleted;
  stored `ProducerEntry`/`ConsumerEntry.inbox_packing` derives from the parsed
  spec (ROLE_INFO_ACK shape unchanged — engine parity untouched).  **Packing is
  carried once, in-object** (HEP-0034 §6.2): the separate `inbox_packing` REG
  wire field is retired (sender line dropped, accessor deleted).  Docs:
  HEP-0027 §3/§4.1/§11.4 + HEP-0046 §14.3 catalog.  Pins: L1
  `*RegReqBodyParsesInboxSchemaOnce` + `ProducerRegReqBodyRejectsMalformedInboxSchema`
  (incl. the array-vs-object incident shape); L3 workers re-pinned to
  BODY_SCHEMA_VIOLATION + in-object packing.
- **✅ B.3 — RECONCILED AS ALREADY LANDED (2026-07-24 audit).**  Every item in
  the 2026-07-12 plan text shipped during the envelope/adapter arcs that ran
  between drafting and Phase B: BRC DEALER sets `ZMQ_ROUTING_ID = role_uid`
  (broker_request_comm.cpp `start()`, hard-error if empty); sends go through
  `wire::adapter::encode_dealer_send` (envelope_hash + security triple per
  msg_type); the poll thread parses via `WireEnvelope::parse_dealer_recv`;
  `pending_requests` is keyed on `correlation_id`; broker replies build the
  typed envelope via `send_reply → build_router_send`.  The §14.6
  I-WIRE-VERSION-ATOMIC cut was the envelope migration itself (a 3-frame
  client fails `WireEnvelope::parse`; version compatibility rides
  `abi_fingerprint` — no scalar bump exists).  The plan's residual "ack build
  → typed bodies" is NOT a design requirement: §14.4/§14.7 define handler
  output as wire response body construction through the typed envelope
  (`send_reply`), which is satisfied; ACK body classes exist for the parse
  side.  No atomic flip remained to perform.
- **✅ B.4 — drift guard landed; legacy surface already retired (2026-07-24).**
  The embedded-JSON drift-guard test landed at its correct anchor — the single
  ingress: L1 `ReceiveAndValidate.*` (test_wire_dispatch_table.cpp) drives real
  5-frame envelopes through `receive_and_validate` and pins the happy-path
  typed variant, BODY_SCHEMA_VIOLATION for missing required fields AND for
  malformed doubly-encoded `inbox_schema_json` content, identity-mismatch,
  nonce replay, and unknown-msg_type → `ValidatedRawControl` (previously this
  function had zero direct unit coverage).  The `to_legacy` /
  `dispatch_legacy` / JSON-handler surface was deleted with B.1i.

**➡ HEP-0046 Phase B is COMPLETE** (B.1a–B.1i, B.2, B.3-reconciled, B.4).
Remaining REG-adjacent work lives in its own tracks: EnvelopeOnly-tier body
classes (per-msg_type follow-ons, independent commits), #72 reconciliation
(expected_schema_owner, band role_name), federation ingress bypass (#69).

**Phase B design requirement — guard embedded-JSON shape drift:**
The typed-envelope work MUST cover *doubly-encoded* fields (a wire
field that is a `std::string` whose content is itself JSON), not just
top-level envelope fields.  These are the fields most prone to
scatter, because each consumer re-parses the inner blob ad hoc and
the parses silently diverge.
- **Concrete in-scope case (fixed as a site-patch 2026-07-17):**
  `inbox_schema_json` on REG_REQ.  The role serializes it as the
  object `{"fields":[...], ...}` (HEP-CORE-0027 §6, canonical form
  that `parse_schema_json` / ROLE_INFO discovery consume), but the
  broker's REG validator hand-parsed it expecting a bare **array** and
  rejected every inbox-configured producer (`INBOX_SCHEMA_INVALID`).
  Both divergent readers live on broker↔role comm, so Phase B's reach
  covers this — BUT only if the field is modeled as a typed
  sub-structure (parsed once at the envelope boundary), not passed
  through as a string.  Design target: broker + discovery both consume
  the same parsed `SchemaSpec`, so a second divergent hand-parse is
  structurally impossible.
- **Critical-path nodes for `inbox_schema_json` (trace before
  refactoring — this is the full serialize→wire→store→re-emit→parse
  chain the typed field must collapse):**
  1. Config in: `config::parse_inbox_config` (`inbox_config.hpp:67`) →
     `InboxConfig::schema_json` (canonical object).
  2. Role serialize: `serialize_inbox_spec_json`
     (`role_host_helpers.hpp:105`) — emits the object `{"fields":[...],
     "packing"?}`; assigned to `inbox_cfg.schema_fields_json` in
     `setup_inbox_facility` (`role_host_helpers.hpp:186`).
  3. Role → broker send: `RoleAPIBase` REG_REQ build,
     `role_api_base.cpp:2916` (`opts["inbox_schema_json"] =
     inbox_cfg.schema_fields_json`).
  4. Wire accessors: `RegReqBody::inbox_schema_json` /
     `ConsumerRegReqBody::inbox_schema_json`
     (`wire_bodies.hpp:241`, `:361`).
  5. Broker ingest + validate/fingerprint (producer):
     `broker_service.cpp:2115` (store on `ProducerEntry`), `:2482`
     (**the fixed hand-parse site**), `:2512` (`rec.blds`).
     Consumer path store: `broker_service.cpp:3534`.
  6. Broker re-emit on ROLE_INFO_ACK: `broker_service.cpp:6461–6465`
     (producer) / `:6507–6511` (consumer) — `resp["inbox_schema"] =
     json::parse(inbox_schema_json)` (string → object transcode).
  7. Discovery consumer parse: `RoleAPIBase::open_inbox_client`
     (`role_api_base.cpp:4244–4265`) → `hub::parse_schema_json`
     (**the canonical parser the broker at node 5 bypassed**).
  The unification: nodes 5 and 7 must consume one shared parsed
  `SchemaSpec`; today only node 7 uses `parse_schema_json`.
- **Second divergence in the SAME chain (fixed at source 2026-07-17):**
  `serialize_inbox_spec_json` (node 2, `role_host_helpers.hpp:120`)
  OMITTED `packing` when it was `"aligned"`, producing a non-canonical
  schema object.  Node 7's `parse_schema_json` REQUIRES `packing`
  (HEP-CORE-0034 §6.2, no silent default) → the sender's `open_inbox`
  looped forever on a parse error and never sent.  Fixed by always
  emitting `packing` at node 2.  Note the redundancy the typed field
  must resolve: packing travels BOTH inside the schema object AND as a
  separate top-level `inbox_packing` wire field (broker fingerprint at
  `broker_service.cpp:2503` reads the separate one; discovery reads the
  in-object one) — two sources of the same value, the classic scatter
  smell.  Design target: one packing, carried once.
- **Coverage boundary to record:** a broker↔role envelope framework
  cannot be the general format-safety net for the *inbox data plane*
  (role↔role ROUTER/DEALER slot messages never pass through the
  broker).  It only guards the inbox's schema *negotiation*
  (REG_REQ / ROLE_INFO_ACK).  The inbox wire itself needs its own
  schema-fingerprint check — which is why the fingerprint is
  negotiated through the broker (the only point both roles touch).

**Phase C completion (post-Phase-B):**
- Add `HubState::binding_side_uid` / `is_binding_side_sender`
  (replaces the roster-walk pattern in
  `broker_service.cpp:3845-3869, 4504`).
- Add `known_roles` reverse-uniqueness startup check.
- Wire R6 pending queue for fan-in producer admission
  (currently `broker_reg_handler.cpp:283-335` returns
  `broker_internal_error` for the fan-in-producer branch).

**Phase D retirements:** `zmq_identity` fields; per-producer
`data_endpoint` / `data_pubkey` scalars; `CONSUMER_ATTACH_REQ_ZMQ`;
`zmq_bind`; symmetric R6 gate.  Ship atomically with the
`broker_proto` bump.

**Phase E:** integration tests (broker ROUTER poll through the
envelope; envelope-body binding across BRC→broker round-trip;
consumer path through the pipeline; R6-gate pending-queue).

**Phase F:** federation follow-on (`I-DEALER-IDENTITY` extended
to hub-to-hub DEALERs).

**Interim fix ✅ SHIPPED 2026-07-15 (commit `327b3abb`).**
Wire dispatch now runs `run_control_gates` on role_uid-bearing
envelope-only tiers — restores role_uid grammar + identity checks
that a Task #46 regression silently bypassed (envelope hash was
validated but role_uid grammar was not, for 6 msg_types).
`wire_dispatch.cpp` splits `Tier::EnvelopeOnly` into three:
`Control_EnvelopeWithRoleUid` (CHECK_PEER_READY_REQ, BAND_JOIN_REQ,
BAND_LEAVE_REQ, BAND_BROADCAST_SEND_NOTIFY — full identity_match),
`Control_EnvelopeWithQueryRoleUid` (ROLE_PRESENCE_REQ,
ROLE_INFO_REQ — grammar + tag policy only; body role_uid is the
queried subject, not the caller), and plain `EnvelopeOnly`.
Fixes a dangling `string_view` in `ControlBodyView` population
(role_uid/channel_name now copied to locals that outlive
`run_control_gates`).  Enriched `identity_mismatch` reject
diagnostic to name both `env.identity()` and `body.role_uid`
(mismatch is silent-fatal — only diagnosable by seeing both).
L1 pin `test_wire_dispatch_table.cpp` updated.

> **Extracted 2026-07-18 (✅ SHIPPED 2026-07-11, verified against code):**
> Queue-owned topology + layer cleanup P1-P3 and the Loop-ready gate + fan-in
> binding-side reader arc.  Lasting record: HEP-0036 §I9.1 + HEP-0011
> §"Loop-ready gate" + HEP-0042 §5.5.2.  Verbatim at commit `633d51c0`; index
> `docs/archive/transient-2026-07-18/todo-completions/`.

---

## Open broker-specific items

### ✅ CLOSED (no wire change) — band notifies' `role_name` (2026-07-24)

Re-evaluated against the clarified framework goals and closed without a
code change.  The load-bearing half was already fixed the same day: all
four `role_name()` accessors (REG ×2 + band ×2) are lenient
(`read_string_or_empty`), so the throwing-accessor crash class is dead.
The residual ctor-side `require` on the two band bodies is deliberately
KEPT: the broker is the sole sender and always populates the label, so
the `require` pins the broker's actual output shape at the role-side
parse; nothing branches on the value, and loosening it would be
symmetry-only churn on a non-load-bearing field (per the
minimal-honest-fix rule).  Rationale recorded at HEP-CORE-0046 §14.3
band-body entry.  Revisit only under a dedicated band-family wire
design pass.

### #72 reconciliation — `expected_schema_owner` is an uncanonical wire name (filed 2026-07-24)

`ConsumerRegReqBody::expected_schema_owner()` (wire_bodies.hpp) is read by
`handle_consumer_reg_req` (broker_service.cpp, consumer-opens-channel path:
`s.schema_owner = body.expected_schema_owner()`), but the field appears in NO
canonical schema: HEP-CORE-0036 §5b.6's CONSUMER_REG_REQ catalog has no owner
field, and HEP-CORE-0034's citation flow names the citer's field `schema_owner`
(no `expected_` prefix).  Production consumers (`build_consumer_reg_payload`)
send no citation fields at all — only test helpers exercise it.  **Decision
needed in the #72 pass:** either add an owner-citation field to the §5b.6/§10.2
canonical tables under a single agreed name, or delete the accessor + broker
read.  Do not extend its use meanwhile (warning comment sits on the accessor).

### Federation — CONSOLIDATED, design-first (task #69) (ratified 2026-07-24)

**Ruling: federation gets a full top-down design (HEP-level: hub↔hub
trust model, peer lifecycle/discovery, wire, security) BEFORE any
further protocol work — no piecemeal patches to federation paths.**
Task #69 is the single umbrella; it consolidates: (1) the SECURITY
ingress bypass — peer-DEALER traffic skips the `receive_and_validate`
gate chain, the last unvalidated broker ingress; (2) the
control-envelope bypass — `HUB_PEER_HELLO` (`broker_service.cpp:1095`)
/ `HUB_PEER_BYE` (`:1338`) hand-roll a divergent 3-frame layout (no
correlation_id) via raw `socket.send`, and `HUB_TARGETED_MSG` /
`HUB_RELAY_MSG` are likewise off-envelope (today's lone exception,
HEP-0047 §3.0) — the redesign routes all of them through
`WireEnvelope` + the gate chain so there is ONE control path; (3) H43
role-disconnect propagation (below — design question, not a patch);
(4) #75 `HUB_TARGETED_ACK` (deferred wire bit — folds into the
designed wire, never lands standalone); (5) #105 / HEP-0037 post-MVP
scope + the skipped `BrokerFederationTest.*` suite (the design defines
the test strategy); (6) the federation input to #95's SCHEMA_REQ /
METRICS_REQ keep-vs-delete survey.  Design anchors: HEP-0022, HEP-0037,
HEP-0035 federation-trust, HEP-0033 §12.3, HEP-0047 §3.0.

### Native engine inbox API parity gap (filed 2026-07-17; RE-SCOPED 2026-07-18)

**CORRECTION (2026-07-18, verified against code):** the RECEIVE side is
already fully wired for Native — the original note was wrong.  `NativeEngine`
DOES override `invoke_on_inbox` (`native_engine.hpp:112`, impl
`native_engine.cpp:1906`), resolves the `on_inbox` symbol
(`native_engine.cpp:1384`), reports `has_callback("on_inbox")` (`:1600`), the
fixture plugin `good_producer_plugin.cpp:347` exports `on_inbox`, and the L2
test `native_engine.invoke_on_inbox_typed_data` passes.  A native plugin's
`on_inbox` DOES fire.

**Genuine remaining gap — SEND only.**  There is no `open_inbox` /
inbox-send host callback in the native ABI (`PlhNativeContext`,
`native_engine_api.h`), so a native plugin can RECEIVE inbox messages but
cannot SEND them.  Lua/Python expose `api.open_inbox(target_uid)` →
`InboxHandle:{acquire,send,discard,close}` (base:
`RoleAPIBase::open_inbox_client`, `role_api_base.cpp:4282`).

**✅ CLOSED 2026-07-18.**  Added the SEND host callbacks to
`PlhNativeContext` — `open_inbox` (→ opaque `InboxClient*` handle, cached
per-uid by RoleHostCore for role lifetime), `inbox_acquire`/`inbox_send`/
`inbox_discard`/`inbox_close` — with `ctx_*` role-side impls delegating to
`RoleAPIBase::open_inbox_client` + `InboxClient::{acquire,send,abort}`,
`hub_stub_*` on the hub context, wired in both `wire()` branches
(`native_engine.cpp`).  Native plugin ABI bumped v9→v10 (additive; appended
before the opaque `_core`/`_api` tail; ComponentVersions registry unchanged).
`good_producer_plugin` probes the surface; L2 test
`NativeEngineTest.Api_InboxSend_NoBroker_GracefulReturn` pins the wiring
(all 5 ptrs non-null) + graceful null on an unreachable target.

**Coverage note:** the native SEND delegation is thin over the SHARED
`InboxClient`, whose end-to-end delivery is proven by the L3 CURVE inbox
tests + the L4 Python delivery test.  A native-*sender* L4 delivery test
would need native-L4-role harness infra (the L4 harness is Python-only
today) — deferred as disproportionate; the transport itself is already
covered.  See `feedback_multi_engine_parity_audit`.

### Notify/broadcast message doc-consistency (2026-07-17)

**Authoritative specs cleaned + consistent** (HEP-0007 + HEP-0030):
- `CHANNEL_ERROR_NOTIFY` (Cat 1) vs `CHANNEL_EVENT_NOTIFY` (Cat 2) unified in
  HEP-0007 §"Broker Notifications" (table + Mermaid + worked example).
- Channel-bound broadcast documented as **renamed, not removed**:
  `CHANNEL_BROADCAST_REQ`→`CHANNEL_BROADCAST_SEND_NOTIFY`,
  `CHANNEL_BROADCAST_NOTIFY`→`CHANNEL_BROADCAST_DELIVER_NOTIFY` (HEP-0046
  §I-MSG-TYPE-TAXONOMY).  Live handler `handle_channel_broadcast_req`
  (`broker_service.cpp:6197`, dispatched `:1826`).
- HEP-0030 §9.1 coexistence table + `CHANNEL_NOTIFY_REQ` paragraph corrected
  (handler deleted audit R3.6; federation relay now via `HUB_RELAY_MSG` →
  `handle_hub_relay_msg:7377`, outbound `relay_notify_to_peers`).

**Residual `CHANNEL_BROADCAST_*` / `CHANNEL_NOTIFY_REQ` sweep — DONE 2026-07-17:**
- `HEP-0022` (federation): added a message-naming note; swept all ~8 sites
  (motivation, design principles, both Mermaid diagrams, relay frame, §6.2
  heading, §8.2 example, impl table).  `CHANNEL_NOTIFY_REQ`→retired,
  `CHANNEL_BROADCAST_REQ`→`CHANNEL_BROADCAST_SEND_NOTIFY`.
- `HEP-0033` (Hub Character): F&F list + catalog rows swept
  (`CHANNEL_NOTIFY_REQ` retired, `CHANNEL_BROADCAST_REQ`→`_SEND_NOTIFY`,
  `CHANNEL_BROADCAST_NOTIFY`→`_DELIVER_NOTIFY`; also HEARTBEAT/BAND names in
  the same tables).
- `HEP-0015` line 524 (`_NOTIFY`→`_DELIVER_NOTIFY`); `HEP-0023` line 713
  (`_REQ`→`_SEND_NOTIFY`).

**New findings during the sweep (NOT yet fixed):**
1. **Phantom script APIs in HEP-0022 §8.2** — `api.notify_channel` (real API is
   `api.broadcast_channel`, fixed inline) and `api.notify_hub` (no script API
   sends `HUB_TARGETED_MSG` — only a broker augment hook; left flagged in the
   doc pending a decision on whether to add the script surface).
2. **Wider rename residuals — SWEPT 2026-07-17.**
   `HEARTBEAT_REQ`→`HEARTBEAT_NOTIFY` across 0002, 0007, 0017, 0018, 0019,
   0021, 0023, 0030, 0033, 0036, 0046 + `IMPLEMENTATION_GUIDANCE.md`
   (routing-class table + REG wire-discipline list).
   `BAND_BROADCAST_REQ`→`BAND_BROADCAST_SEND_NOTIFY` /
   `BAND_BROADCAST_NOTIFY`→`BAND_BROADCAST_DELIVER_NOTIFY` in 0007, 0023,
   0033 + `IMPLEMENTATION_GUIDANCE.md`.  Protected historical quotes/notes
   left intact (0007:1500/2057, 0030:23/180, 0047 ledger, 0033:3096 "was").
   HEP-0046 §2.7/§14.3: dropped the phantom `HEARTBEAT_ACK` and renamed
   `HeartbeatReqBody`→`HeartbeatNotifyBody` per shipped code (C13);
   HEP-0021:398 phantom `HEARTBEAT_ACK` reply removed from the diagram.
   Verified against code: heartbeat is fire-and-forget `HEARTBEAT_NOTIFY`,
   no ack (send returns void; broker handler emits nothing).

**Code-cruft follow-ups — DONE 2026-07-17 (verified, build clean, 98/98 tests pass):**
- Deleted dead `HeartbeatAckBody` (declaration in `wire_bodies.hpp`, ctor in
  `wire_bodies.cpp`, isolated L1 test in `test_wire_envelope.cpp`).  The full
  `pylabhub-utils` lib recompiled cleanly — nothing referenced it.
- Renamed the misleading `"HEARTBEAT_ACK"` placeholder → `"NON_NOTIFY_MSG"`
  (10 sites) in `test_dispatch_notifications.cpp`.

### HEP-CORE-0047 — Messaging & Communication Master Reference

**DRAFT LANDED 2026-07-17** — `docs/HEP/HEP-CORE-0047-Messaging-Master-Reference.md`.
Scope: index + canonical wire-message registry (cross-cutting rules referenced,
not moved).  Sections: plane map, registry (§3, code-verified names/dir/anchor),
glossary, do-not-confuse pairs, rename ledger, drift-guard spec.  DOC_STRUCTURE
index updated with an -0047 pointer + staleness note.

**Doc gap fixed 2026-07-17:** added `CONSUMER_ATTACH_REQ_SHM`/`_ACK_SHM`
(producer-initiated pre-attach gate, HEP-0041 §9 D4) and
`CONSUMER_ATTACH_REQ_ZMQ`/`_ACK_ZMQ` (consumer-initiated, HEP-0042 §6.2) to
HEP-0047 §3.2 — real broker-dispatched JSON control messages (proto 6→7) that
the registry had omitted; corrected §3.9 to distinguish them from the binary
AttachProtocol frames.  This omission is exactly what the drift test would catch.

**Open follow-ups:**
1. **Drift test — DEFERRED; rides with HEP-0046 Phase B (task #57), which is
   itself deferred behind the AUTH critical path.**  The `21-vs-29` split
   (`kDispatchTable` gated subset vs `process_message` full dispatch) is the
   fingerprint of the half-migrated typed-envelope framework, NOT a bug to
   patch: `kDispatchTable` + typed body classes = the new HEP-0046 pipeline
   (Phase A shipped, Phase C islanded); the 29-branch `process_message` if/else
   = the old hand-parsed JSON dispatch.  Phase B rewires `process_message` onto
   the typed pipeline → old chain disappears, `kDispatchTable` becomes the
   single source of truth, drift closes structurally.  So the reconciliation
   test belongs WITH Phase B as its guard — building it standalone now = parallel
   plumbing Phase B reworks.  Correct anchor when written: `process_message`
   (the complete 29), NOT `kDispatchTable` alone.  See HEP-0047 §7.
2. **Resolve owning-HEP ambiguity (⟳ rows in §3).**  Two catalogs exist
   (HEP-0007 §12 + HEP-0033 message table); decide authoritative owner per
   message.  Designer decision.
3. **Enumerate the Inbox (HEP-0027) wire family** into §3 when #191/#103 land.
4. **Sweep residual old-names** listed above (0022/0033/0015/0023).

### Doc-vs-code message audit — RESOLVED 2026-07-17

Diffed every message token in the docs against the actual `src` wire literals.
Four doc-only names were reconciled against code:
- `ROLE_REGISTERED_NOTIFY` / `ROLE_DEREGISTERED_NOTIFY` — **not implemented**
  (no `src` literal); kept as **planned** (federation role-presence
  propagation).  HEP-0007 spec sections + HEP-0007/0015 event tables +
  HEP-0047 registry now marked "planned, not implemented."
- `BROKER_SHM_INFO_REQ` (HEP-0045) — same message as shipped
  `SHM_BLOCK_QUERY_REQ` (`query_shm_info` / `handle_shm_block_query`); HEP-0045
  renamed to the real wire name + §0 note that the observer extends its response.
- `CREATE_CHANNEL_REQ` (HEP-0018) — phantom; channel creation is the first
  `REG_REQ` / (fan-in) `CONSUMER_REG_REQ`, flagged by `admission.channel_opened`.
  Fixed HEP-0018 §15.3 + added the definition to HEP-0047 §3.1.
- `KNOWN_ROLES_REQ` (HEP-0040) — never a real message; `known_roles` is
  file-provisioned (`KnownRolesStore::load_from_file`), and wire pubkeys arrive
  via `CONSUMER_REG_ACK`/`REG_ACK.initial_allowlist`/`GET_CHANNEL_AUTH_ACK.allowlist`.
  Fixed HEP-0040:800.

Verified-legit (design-future / never-shipped / historical, left as-is):
`CHANNEL_KEY_ROTATION_NOTIFY` (0041 Phase 2), `CHANNEL_WARNING_NOTIFY` (0019
hypothetical), `METRICS_COLLECT_REQ` (0019 "never shipped"),
`CHANNEL_AUTH_UPDATE_ACK` (0036 retired-design history).

### #92 (HIGH-leverage) — Audit all `_REQ` frames against HEP-0007 §12.2.1

The REQ shape contract (Sync vs Fire-and-forget) was codified in
HEP-CORE-0007 §12.2.1 + HEP-CORE-0021 §16.3 on 2026-05-21 after
finding ENDPOINT_UPDATE_REQ was in the prohibited half-mix shape
(broker emitted `_ACK`, client dropped it).  The fix shipped as
commits `5ccae1b2` (HEP) + `8228f1ac` (code) + `66e71894` (error-
path tests).

Now scan every other `_REQ` frame in `BrokerRequestComm` (header) +
`BrokerService` (broker side) and classify each.  Known suspects
where the client method is fire-and-forget `void` and we should
check whether the broker also sends an ACK that's being dropped:

- `send_broadcast` (`broker_request_comm.cpp`)
- `send_checksum_error`
- `send_heartbeat`

For each half-mix found:
1. Decide intended shape per HEP-0007 §12.2.1 (does caller's next
   decision depend on broker acceptance?).
2. Align BRC + broker: either make BRC sync (`do_request`) OR
   remove broker's `_ACK`.
3. Update the relevant HEP to declare the shape (don't leave it
   implicit).

Trigger: any half-mix is a latent flake source like the
ENDPOINT_UPDATE one we just fixed.  Estimated effort M.

### H43 — Federation propagation of role-disconnect (folded into #69, 2026-07-24)

Verified open 2026-05-12: `broker_service.cpp` does not call
`subscribe_role_disconnected`; only `hub_script_runner.cpp:281`
subscribes (for script-side `on_role_disconnected` callback).
Peer hubs therefore do NOT learn about non-channel-close role
disconnects.  Whether they NEED to is HEP-CORE-0022 federation
scope, not Wave-B M8 (dual-hub presence is role-side, not peer-hub
state replication).  Trigger to address: a concrete federation
scenario where peer-hub bookkeeping diverges.  Bands are not
federated per HEP-CORE-0030 §3 so impact is bounded.

### Wave-M2 / Wave-M3 deferred sub-items (broker-internal)

All trigger-gated — not actively blocking.  Re-open when the
trigger fires.

- **Wave-M3 Step 5** — strict `add_role` admission with global-uid
  uniqueness.  Trigger: spoofing-attempt observation OR security-
  design pass requirement.
- **Wave-M3 Step 7** — privatize `RolePresence` state-bearing
  fields.  Trigger: concrete misuse bug OR audit observation.
- **H15** — `_on_heartbeat` direct metrics-field mutation
  (`src/utils/ipc/hub_state.cpp:1294-1306`).  Trigger:
  `RoleEntry::set_presence_metrics(...)` API addition for a
  concrete reason.
- **H40** — `active_router_` concurrency hardening (atomic + DEBUG
  assertion).  Trigger: any new HubState mutator caller from a
  non-broker-IO thread.

### Wave M2 — Multi-Producer Channel Bookkeeping

API-layer MP detail in `docs/todo/API_TODO.md` § "Wave M2 — Multi-
Producer Channel Bookkeeping (open MP4 work)".  Broker-side
remaining items mirror those — same handler list, broker-internal
mutation only.

### Open 2026-05-03: `IncomingMessage` `sender` field semantics

`HubScriptRunner::worker_main_()` (Phase 7 Commit C) reuses
`scripting::IncomingMessage` from `role_host_core.hpp` as the
cross-thread queue payload between broker subscriptions and the
script-thread runner.  Hub-side semantics differ from role-side:
the role-side `sender` is the broker peer; hub-side it should be
the originating role's uid (per HEP-0033 §12).  Audit field-use
sites + document the semantic split, OR introduce a sibling type.

### Hub State Query Layer (HEP-CORE-0039)

Full design: `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md`
(promoted from tech_draft 2026-06-02; tasks #140-#150 shipped Phase A
+ P8 sweep migration).  Layer-1 metadata + Layer-2 free-function
query API + `hub.snapshot()` script binding.  Absorbs `query_shm_info`
/ `count_by_observable` wiring (Group 2 #2/#3 from 2026-05-20
decision log).  Phases B+ remain open.

---

## Naming hygiene (mirror TODO_MASTER, for quick disambiguation)

| Looks like | Actually means | Status |
|---|---|---|
| Wave-B MN | Arc B Wave (`docs/archive/transient-2026-06-02/role_host_template_design.md` §14) | M0..M9 shipped |
| HEP-0033 §15 Phase N | Arc A Wave | Phases 1-9 shipped; Phase 10 partial |
| Wave-M2 / Wave-M2.5 / Wave-M3 | Side-arc waves (multi-producer / controlled-access) | closed |
| M1.2 / M1.4 / M1.5 / MD1 / MD1.5 | Side-arc FSM-consolidation + race-fix cleanups | closed |

If a sentence says "M3" without a prefix, it almost certainly means
**Wave-B M3** — but verify against context.

---

## Open items from 2026-05-20 post-band-authority discovery (migrated 2026-06-02)

Carried over from the archived
`docs/archive/transient-2026-06-02/DISCOVERY_2026-05-20.md`.  Each
should be verified-fixed-or-still-open before next sprint.

### D2 drift

- **B1 — empty `correlation_id` in BAND_JOIN/LEAVE validator errors.**
  ✅ FIXED — verified 2026-06-27 against current code.  Both handlers extract
  `corr_id = req.value("correlation_id", "")` at entry (`broker_service.cpp:5540`
  and `:5651`) and pass it through to `validate_role_uid_only(...)` (`:5577-5580`
  and `:5678-5681`).  Audit B1 anchors at `:5573-5576` and `:5676-5677` record
  the fix.  Earlier line references (`:4488,4584`) are stale post-refactor.
- **Stale-comment scrub (~15 sites).**  References to deleted
  `set_broker_comm` / `start_ctrl_thread` / `pImpl->broker_channel`
  in `role_api_base.{hpp,cpp}`, `hub_script_runner.cpp`, the 3 role
  hosts, `engine_host.hpp`, `role_host_helpers.hpp`, `role_host_core.hpp`.
- **Obsolete includes:** `#include "utils/broker_request_comm.hpp"`
  in three role hosts (~line 22) — BRC reaches them via
  `role_handler.hpp` now.  Mechanical.

### Pending discussion (dead-code candidates)

- `RoleAPIBase::close_all_inbox_clients()` — zero callers; verify
  no script reflection path before removing.
- `BrokerRequestComm::query_shm_info()` + `BrokerService::collect_shm_info_json()`
  + `SHM_INFO_REQ` wire frame — paired client+server; remove together
  if wire frame is also dead.
- `ChannelSnapshot::count_by_observable()` — verify exposure to
  admin queries before removing.

### Doc bookkeeping debt

- 4 `docs/code_review/REVIEW_*WaveM3*.md` files from 2026-05-11 are
  archive candidates (open items verified resolved during the
  2026-05-19 review-triage pass).

---

## Notes

### Error taxonomy (Cat 1 / Cat 2)

Broker / producer / consumer follow the two-category error
taxonomy in `docs/IMPLEMENTATION_GUIDANCE.md` § "Error Taxonomy".
- **Cat 1** — recoverable; log + continue (e.g. transient frame
  decode error).
- **Cat 2** — fatal; emit notification, fail closed (e.g. schema
  mismatch on registration).

### Key design decisions (single source of truth)

- Per-presence FSM on `RoleEntry` (Connected / Pending /
  Disconnected) replaced the Pending / Ready / Closing channel
  FSM in HEP-CORE-0023 §2 rewrite (2026-05-07).  Channel teardown
  is **atomic** on producer-presence Disconnected — no separate
  channel-grace window, no `FORCE_SHUTDOWN` escalation.
- Wire-field unification (broker_proto 5, 2026-05-19): every role-
  context message uses `role_uid`/`role_name`; federation peer-
  context uses `sender_uid`; inbox uses `sender_uid` (authoring
  producer).  See HEP-CORE-0023 §2.5.4 + HEP-CORE-0033 §G2.2.0b.
- Side-aware role-tag policy at every REG/DEREG/HEARTBEAT gate:
  REG/DEREG accept `{prod, proc}`; CONSUMER_REG/DEREG accept
  `{cons, proc}` (processor dual-role).  `HEARTBEAT_REQ` cross-
  checks `role_type` against tag.

---

## Related Work

- `docs/todo/API_TODO.md` — API-layer view of the same renovation.
- `docs/todo/TESTING_TODO.md` — broker-protocol test gaps.
- `docs/HEP/HEP-CORE-0033-Hub-Character.md` — Arc A canonical spec.
- `docs/HEP/HEP-CORE-0023-Startup-Coordination.md` — per-presence
  FSM canonical spec.
- `docs/HEP/HEP-CORE-0035-Hub-Role-Authentication-and-Federation-Trust.md` — auth design.
- `docs/HEP/HEP-CORE-0039-Hub-State-Query-Layer.md` — hub state
  query layer design (promoted from tech_draft 2026-06-02).
