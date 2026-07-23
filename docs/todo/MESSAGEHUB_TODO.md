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
| **HUB_TARGETED_ACK wire frame** (HEP-0033 §12.3.6) | ⏸ Deferred — C++ `augment_peer_message` surface in place; wire bit not landed | (task #75) — federation-only |

Wave-M2 / Wave-M2.5 / Wave-M3 side-arcs all closed.  M1.2 / M1.4 /
M1.5 / MD1 / MD1.5 all closed.

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

**Landed (islanded — pipeline not yet live):**
- `WireEnvelope` + typed body classes in `wire_envelope.hpp` +
  `wire_bodies.hpp`.  46 L1 tests in `test_wire_envelope.cpp`.
- `AdmissionGateRunner` + 7-gate pipeline in `admission_gates.hpp`.
  23 L1 tests.
- `RegAdmissionPipeline` outcome orchestration in
  `reg_admission_pipeline.hpp`.  5 L1 tests.
- `BrokerRegHandler` adapter binding pipeline to HubState +
  KnownRolesConfig in `broker_reg_handler.hpp`.  14 L2 tests.
- `HubState::nonce_seen` replay-bound primitive.  6 L2 tests.

**Phase B (LOAD-BEARING NEXT COMMIT):**
- `broker_service.cpp` dispatch rewire: parse via
  `WireEnvelope::parse_router_recv`; every REG-family handler
  switches to typed-body signature; `BrokerRegHandler` becomes a
  member of `BrokerServiceImpl` and handles REG_REQ +
  CONSUMER_REG_REQ.
- BRC (`broker_request_comm.cpp`) rewire: DEALER `ZMQ_ROUTING_ID`
  set to `role_uid`; every REG-family send method builds a typed
  body + stamps `envelope_hash` + sends via
  `WireEnvelope::build_dealer_send`; poll thread parses via
  `parse_dealer_recv`; `pending_requests` re-keyed from `msg_type`
  to `correlation_id`.
- Atomic: A + B ship together, no runtime tolerance for mixed
  old/new deployments (HEP-CORE-0046 §14.6 `I-WIRE-VERSION-ATOMIC`).

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

### Federation control-envelope bypass → fold into the federation redesign (filed 2026-07-22)

The broker↔broker federation handshake **bypasses `WireEnvelope`** and hand-rolls
a **divergent** control frame layout: `HUB_PEER_HELLO` (`broker_service.cpp:1095`)
and `HUB_PEER_BYE` (`:1338`) send `[kFrameTypeControl, msg_type, body]` (3 frames,
**no `correlation_id`**) via raw `socket.send`, vs the standard
`WireEnvelope::build_router_send` layout `['C', msg_type, correlation_id, body]`.
So federation is a second, drifting copy of the JSON control format. All *other*
control traffic uses `WireEnvelope`, and the msgpack data plane is already
centralized in `wire_detail` (`zmq_wire_helpers.hpp`) with no bypass — federation
is the lone exception (documented in HEP-CORE-0047 §3.0).

**Cleanup (do as part of the federation redesign, not standalone):** route
`HUB_PEER_HELLO`/`HUB_PEER_BYE`/`HUB_TARGETED_MSG`/`HUB_RELAY_MSG` through
`WireEnvelope::build_router_send` (+ the parse side through `receive_and_validate`)
so there is genuinely ONE control-envelope path. Federation is slated for a
broader redesign anyway (HEP-CORE-0022); this rides along.

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

### H43 — Federation propagation of role-disconnect

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
