# DRAFT — REG Wire Alignment Cleanup

**Status:** DRAFT, review pending. Sole source of truth for the REG-wire
alignment work that follows.
**Purpose:** Consolidate what the authoritative HEP documents actually
say about REG-family wire, list every place my current in-tree code
(committed + uncommitted) contradicts them, and record the concrete
edit plan so the implementation reads directly from a reviewed spec
rather than from moment-to-moment inference.

Every claim below cites the HEP section that establishes it. Every
proposed edit names the file it lives in. Do not begin the edits until
this draft is reviewed and confirmed.

---

## 0. Why this document exists

The REG-family wire module was rewired against my own reading of
HEP-CORE-0046 §14 in isolation. That reading turned out to conflict
with HEP-CORE-0007, HEP-CORE-0032, HEP-CORE-0034, HEP-CORE-0035,
HEP-CORE-0036 — each of which is older, more specific, and still in
force for its subsystem. The current in-tree code enforces a wire
contract that no other HEP describes, and that no production sender
satisfies. Tests fail with `BODY_SCHEMA_VIOLATION` on every REG_REQ.

The right fix is to correct the code in place against what the HEPs
actually say — not to revert, not to loosen the design. This document
records both the design ground truth and the edit plan.

---

## 1. Design authority stack (as the HEPs themselves state it)

HEP-CORE-0046 §14 is nominally authoritative for REG-family wire per
delegation banners in HEP-CORE-0007 §12, HEP-CORE-0017 top banner,
HEP-CORE-0018 §5, HEP-CORE-0023 §2, HEP-CORE-0033 §12, HEP-CORE-0036 §0,
and HEP-CORE-0042 §2.

However, HEP-CORE-0046 §14.3's field list contradicts every one of the
subsystem HEPs on specific fields. Since the subsystem HEPs are older,
more specific, and never withdrawn, this document treats them as
authoritative for their respective concerns and treats HEP-CORE-0046
§14.3 as needing errata. See §7 for the errata list.

The subsystem authorities used below:

| Concern | HEP | Section |
|---|---|---|
| Producer REG_REQ schema fields | HEP-CORE-0034 | §10.1 |
| Consumer REG_REQ schema fields | HEP-CORE-0034 | §10.2 |
| Schema fingerprint canonical form | HEP-CORE-0034 | §6.3 |
| Wire-version declaration | HEP-CORE-0032 | §8 |
| `known_roles` authoritative source | HEP-CORE-0035 | §4.8.2 |
| Layer-2 identity gate at REG admission | HEP-CORE-0036 | §6.1, §6.3 |
| REG_REQ / CONSUMER_REG_REQ required fields | HEP-CORE-0036 | §5b.4, §5b.6 |
| Wire-authority delegation | HEP-CORE-0007 | §12 banner |
| Topology matrix | HEP-CORE-0017 | §3.3.0 |
| `channel_topology` default | HEP-CORE-0018 | §5 amendment 2026-07-08 |
| Broker handler structural contract | HEP-CORE-0033 | §9.7 |
| Per-msg-type role-tag gate policy | HEP-CORE-0033 | §G2.2.0b.8 |
| Broker counter taxonomy | HEP-CORE-0033 | §9.4 |
| Field-name canonical table | HEP-CORE-0023 | §2.5.4 |
| `role_uid` requirement on DEREG | HEP-CORE-0023 | §2.1.1 |
| Unified admission ledger + INVARIANT-BIND-CONFIRM-1..3 | HEP-CORE-0042 | §5.5.2, §5.5.2.1 |
| Retired wire messages | HEP-CORE-0007 | §12.3, §12.4, §12.5 |
| Envelope, envelope_hash, correlation_id semantics | HEP-CORE-0046 | §14 |
| Security triple (client_nonce, client_wall_ts) | HEP-CORE-0046 | §I-REPLAY-BOUND |
| DEALER routing_id = role_uid | HEP-CORE-0046 | §I-DEALER-IDENTITY |

---

## 2. REG_REQ field truth table (per authoritative HEPs)

### 2.1 Producer REG_REQ (msg_type = `REG_REQ`)

Required fields per HEP-CORE-0036 §5b.4 + HEP-CORE-0034 §10.1 + HEP-CORE-0007 §12.3:

| Field | Type | Requiredness | Authority |
|---|---|---|---|
| `channel_name` | string | required | HEP-0036 §5b.4 |
| `role_uid` | string | required | HEP-0036 §5b.4 + HEP-0023 §2.5.4 |
| `role_name` | string | required | HEP-0036 §5b.4 |
| `role_type` | string | required (`"producer"` \| `"processor"`) | HEP-0036 §5b.4 |
| `zmq_pubkey` | Z85 40-char | required | HEP-0036 §5b.4, §6.1 |
| `data_transport` | string (`"shm"` \| `"zmq"`) | required | HEP-0036 §5b.4, HEP-0017 §3.3.0 |
| `zmq_node_endpoint` | string | required if `data_transport = "zmq"` AND producer is binding side | HEP-0036 §5b.4, HEP-0017 §3.3.0 |
| `shm_capability_endpoint` | string | required if `data_transport = "shm"` | HEP-0036 §5b.4, HEP-CORE-0041 §5.1 |
| `producer_pid` | uint64 | required | HEP-0007 §12.3 |
| `channel_topology` | string | OPTIONAL, default `"one-to-one"` | HEP-0018 §5 amendment 2026-07-08 |
| `abi_fingerprint` | object (15-field ComponentVersions) | required | HEP-CORE-0032 §8.2 |
| `build_id` | string | optional (sibling to abi_fingerprint) | HEP-CORE-0032 §8.2 |
| `schema_id` | string | optional; empty = anonymous | HEP-0034 §10.1 |
| `schema_hash` | hex(32) | required if `schema_id` non-empty | HEP-0034 §10.1 |
| `schema_blds` | string (canonical wire form) | required if `schema_id` non-empty | HEP-0034 §10.1 |
| `schema_packing` | string (`"aligned"` \| `"packed"`) | required if `schema_id` non-empty | HEP-0034 §10.1 |
| `schema_owner` | string (`""` \| `"hub"`) | optional (empty = self/path B) | HEP-0034 §10.1 |
| `flexzone_blds` | string | optional (present when schema uses a flexzone) | HEP-0034 §10.1 |
| `flexzone_packing` | string | optional (present when schema uses a flexzone) | HEP-0034 §10.1 |
| `inbox_endpoint` | string | optional | HEP-CORE-0027 §4.1 |
| `inbox_schema_json` | json array | optional | HEP-CORE-0027 §4.1 |
| `inbox_packing` | string | optional | HEP-CORE-0027 §4.1 |
| `inbox_checksum` | string (`"enforced"` \| `"manual"` \| `"none"`) | optional | HEP-CORE-0027 §4.1 |
| `metadata` | object | optional | HEP-0007 §12.3 |
| `correlation_id` | string | required by HEP-0046 §I-CORRELATION-STABLE; optional per HEP-0036 §5b.4 — HEP-0046 wins per delegation | HEP-0046 |
| `client_nonce` | string (hex, 16 bytes) | required per HEP-0046 §I-REPLAY-BOUND | HEP-0046 |
| `client_wall_ts` | uint64 ms | required per HEP-0046 §I-REPLAY-BOUND | HEP-0046 |
| `envelope_hash` | hex(32) | required per HEP-0046 §I-ENVELOPE-BODY-BINDING | HEP-0046 |

**Fields that are NOT part of the design and MUST NOT be treated as required:**
- `schema_version` — no HEP defines this field. The fingerprint (`schema_hash`) IS the version discriminator per HEP-0034 §6.3.
- `broker_proto` (u32 scalar) — legacy pre-§8 field per HEP-0032. Not stamped by production senders. `abi_fingerprint.broker_proto_major/minor` is the canonical carrier per HEP-0032 §8.

### 2.2 Consumer CONSUMER_REG_REQ (msg_type = `CONSUMER_REG_REQ`)

Required fields per HEP-CORE-0036 §5b.6 + HEP-CORE-0034 §10.2 + HEP-CORE-0007 §12.3:

| Field | Type | Requiredness | Authority |
|---|---|---|---|
| `channel_name` | string | required | HEP-0036 §5b.6 |
| `role_uid` | string | required | HEP-0036 §5b.6 + HEP-0023 §2.5.4 |
| `role_name` | string | required | HEP-0036 §5b.6 |
| `role_type` | string | required (`"consumer"` \| `"processor"`) | HEP-0036 §5b.6 |
| `zmq_pubkey` | Z85 40-char | required (consumer's own identity pubkey) | HEP-0036 §5b.6, §6.3 |
| `data_transport` | string | required (must match channel's stored transport) | HEP-0036 §5b.6, HEP-0017 §3.3.0 |
| `consumer_pid` | uint64 | required | HEP-0007 §12.3 |
| `consumer_hostname` | string | required | HEP-0007 §12.3 |
| `channel_topology` | string | OPTIONAL, default `"one-to-one"` | HEP-0018 §5 amendment |
| `abi_fingerprint` | object | required | HEP-CORE-0032 §8.2 |
| `build_id` | string | optional | HEP-CORE-0032 §8.2 |
| `expected_schema_id` | string | required in named-citation mode | HEP-0034 §10.2 |
| `expected_schema_hash` | hex(32) | required in named-citation mode; optional in anonymous | HEP-0034 §10.2 |
| `expected_schema_blds` | string | required in anonymous-citation mode; optional in named | HEP-0034 §10.2 |
| `expected_schema_packing` | string | required in anonymous-citation mode; optional in named | HEP-0034 §10.2 |
| `expected_flexzone_blds` | string | optional | HEP-0034 §10.2 |
| `expected_flexzone_packing` | string | optional | HEP-0034 §10.2 |
| `inbox_endpoint` / `inbox_schema_json` / `inbox_packing` / `inbox_checksum` | — | optional (companion to producer inbox) | HEP-0007 §12.4 (2026-03-30) |
| `correlation_id` | string | required per HEP-0046 §I-CORRELATION-STABLE | HEP-0046 |
| `client_nonce` | string | required per HEP-0046 §I-REPLAY-BOUND | HEP-0046 |
| `client_wall_ts` | uint64 ms | required per HEP-0046 §I-REPLAY-BOUND | HEP-0046 |
| `envelope_hash` | hex(32) | required per HEP-0046 §I-ENVELOPE-BODY-BINDING | HEP-0046 |

**Explicit design commitment (HEP-0034 §10.2, last paragraph):**
> "The form `expected_blds` / `expected_packing` (no `schema_` infix)
> was used in pre-Phase-4d code and is no longer accepted."

So consumer schema fields use the `expected_` prefix. Not the same
field names as producer. This IS the design intent — semantically the
consumer DECLARES an expectation, the producer DECLARES an assertion.

---

## 3. Other REG-family + control message field truth

### 3.1 DEREG_REQ / CONSUMER_DEREG_REQ

Per HEP-CORE-0023 §2.1.1 (broker_proto 2→3 closure, 2026-05-15) and
HEP-CORE-0007 §12.3:

| Field | Requiredness | Authority |
|---|---|---|
| `channel_name` | required | HEP-0007 §12.3 |
| `role_uid` | **required** (was optional pre-2026-05-15) | HEP-0023 §2.1.1 |
| `producer_pid` (DEREG_REQ) / `consumer_pid` (CONSUMER_DEREG_REQ) | required | HEP-0007 §12.3 |
| security triple + correlation_id + envelope_hash | required (REG-family per HEP-0046) | HEP-0046 |

Broker resolves target by `(pid, role_uid)` tuple — mismatch on either
returns `NOT_REGISTERED` per HEP-0007 §12.3 lines 1257 / 1342.

### 3.2 ENDPOINT_UPDATE_REQ

Per HEP-CORE-0021 §16 (deferred by HEP-CORE-0007 §12.4) and HEP-CORE-0046 §14.3:

| Field | Requiredness | Authority |
|---|---|---|
| `channel_name` | required | HEP-0046 §14.3 |
| `endpoint_type` | required (`"zmq_node"` — inbox variant retires per HEP-0046 §2.3) | HEP-0046 §14.3 |
| `endpoint` | required | HEP-0046 §14.3 |
| security triple + correlation_id + envelope_hash | required | HEP-0046 |

Body does NOT carry `role_uid` — identity comes from envelope Frame 0
(DEALER routing_id) per HEP-0046 §I-DEALER-IDENTITY. Broker's
sender-validation uses `env.identity() == binding_side_uid(channel)`
per HEP-0046 §2.3.

### 3.3 CHANNEL_AUTH_APPLIED_REQ

Per HEP-CORE-0042 §5.5.2 + HEP-CORE-0046 §14.3:

| Field | Requiredness | Authority |
|---|---|---|
| `channel_name` | required | HEP-0042 §5.5.2 |
| `role_uid` | required (was `producer_role_uid` pre-2026-07-11; back-compat allowed when `role_type` absent/`"producer"`) | HEP-0042 §5.5.2 |
| `role_type` | required (`"producer"` \| `"consumer"`); absent defaults to `"producer"` back-compat | HEP-0042 §5.5.2 |
| `instance_id` | required if `role_type = "producer"`; ignored if `"consumer"` | HEP-0042 §5.5.2 |
| `applied_version` | required | HEP-0042 §5.5.2 |
| security triple + correlation_id + envelope_hash | required | HEP-0046 |

### 3.4 CHECK_PEER_READY_REQ

HEP-0042 §5.5.2 and HEP-CORE-0036 §6.6.3 mention it semantically but
neither defines its wire fields. Authority is HEP-CORE-0046. Per
INVARIANT-BIND-CONFIRM-3 the broker's response resolves via
`HubState::is_pubkey_visible_to` (already implemented in the committed
ledger work).

### 3.5 GET_CHANNEL_AUTH_REQ

Per HEP-CORE-0036 §6.5:

| Field | Requiredness | Authority |
|---|---|---|
| `channel_name` | required | HEP-0036 §6.5 |
| `role_uid` | required (caller's uid; broker validates caller is registered on channel) | HEP-0036 §6.5 |
| `broker_proto` | required integer (this is one of the messages that DOES carry the scalar per §6.5) | HEP-0036 §6.5 |
| `corr_id` (note: not `correlation_id`) | required | HEP-0036 §6.5 |
| envelope_hash | required | HEP-0046 |

Note the field name discrepancy: HEP-0036 §6.5 uses `corr_id`;
HEP-0046 §14 uses `correlation_id`. This is a real cross-HEP naming
conflict — flagged in §7 below.

### 3.6 HEARTBEAT_REQ

Per HEP-CORE-0023 §2.5.2 + §2.5.4:

| Field | Requiredness | Authority |
|---|---|---|
| `channel_name` | required | HEP-0023 §2.5.2 |
| `role_uid` | required (was `uid` pre-broker_proto 4→5) | HEP-0023 §2.5.4 |
| `role_type` | required (`"producer"` \| `"consumer"`) | HEP-0023 §2.5.2 |
| `producer_pid` | required (retained from Phase 1 wire — even for consumer heartbeats) | HEP-0007 §12.4 |

Fire-and-forget; no ACK. Envelope_hash required per HEP-0046.
Correlation_id optional (fire-and-forget).

### 3.7 Retired wire messages that MUST NOT appear in any dispatch table

Per HEP-CORE-0007 §12.3, §12.4, §12.5, and HEP-CORE-0046 §3:

- `CHANNEL_NOTIFY_REQ` — REMOVED, superseded by HEP-CORE-0030
- `CHANNEL_BROADCAST_REQ` — REMOVED, superseded by BAND_BROADCAST_REQ
- `METRICS_REPORT_REQ` — RETIRED Wave M1.4 (broker_proto 1→2)
- `FORCE_SHUTDOWN` — REMOVED 2026-05-07
- `CHANNEL_EVENT_NOTIFY` — REMOVED
- `CHANNEL_BROADCAST_NOTIFY` — REMOVED
- `GET_CHANNEL_PRODUCERS_REQ` / `_ACK` — RETIRED 2026-07-08
- `CHANNEL_PRODUCERS_CHANGED_NOTIFY` — RETIRED 2026-07-08
- `CONSUMER_ATTACH_REQ_ZMQ` / `_ACK` — RETIRED per HEP-CORE-0046 §3

---

## 4. Where my in-tree code violates the design

Each item cites the exact code location and the HEP text it violates.

### 4.1 `src/include/utils/wire_bodies.hpp` (committed on branch)

| # | Violation | HEP citation |
|---|---|---|
| A1 | `RegReqBody::schema_version()` accessor exists (line 176-179). Not a real field. | HEP-0034 §10.1 does not list; HEP-0007 §12.3 does not list. |
| A2 | `RegReqBody` used for both producer and consumer per HEP-0046 §14.3 header. Consumer wire uses `expected_schema_*` prefix. | HEP-0034 §10.2. |
| A3 | `RegReqBody::broker_proto()` required (line 168-171). Scalar `broker_proto` is legacy per HEP-0032 §8; abi_fingerprint canonical. Not stamped by production senders. | HEP-0032 §8, HEP-0036 §5b.4 (not listed on REG_REQ). |
| A4 | `RegReqBody` missing accessors for `schema_packing`, `flexzone_blds`, `flexzone_packing`. | HEP-0034 §10.1. |
| A5 | `RegReqBody` missing accessors for `producer_pid`, `has_shared_memory`, `zmq_node_endpoint`, `shm_capability_endpoint`, `metadata`, and inbox companion fields. | HEP-0007 §12.3 + HEP-CORE-0027 §4.1. If not exposed as typed accessors, handlers use `body.value()` scatter — violates HEP-0046 §14 "no JSON key extraction." |

### 4.2 `src/include/utils/admission_gates.hpp` + `.cpp` (committed)

| # | Violation | HEP citation |
|---|---|---|
| B1 | `gate_grammar` does not enforce per-msg-type role-tag policy. | HEP-0033 §G2.2.0b.8 lines 3577-3606. |
| B2 | `gate_supported_proto` compares body scalar `broker_proto` — should be removed or wired to `abi_fingerprint.broker_proto_major` verification instead. | HEP-0032 §8. |
| B3 | `BrokerAdmissionConfig::broker_proto{7U}` (broker_reg_handler.hpp:70) duplicates `kBrokerProtoMajor = 7` (plh_version_registry.hpp:227) with no static_assert linking them. | HEP-0032 §8.9 drift-guard requirement. |

### 4.3 `src/include/utils/wire_dispatch.hpp` + `.cpp` (uncommitted, my new file)

| # | Violation | HEP citation |
|---|---|---|
| C1 | `ValidatedConsumerRegReq` variant uses `RegReqBody` (should use `ConsumerRegReqBody` after split). | HEP-0034 §10.2. |
| C2 | Dispatch table row for `CHANNEL_BROADCAST_REQ` — retired. | HEP-0007 §12.4 lines 1485-1488. |
| C3 | Counter name `sys.admission_rejected` in `dispatch_received` visitor — not in HEP-0033 §9.4 taxonomy. | HEP-0033 §9.4. |
| C4 | `AdmissionBinder::context.broker_proto = 0` treated as "any" — invented semantic. | HEP-0032 §8 says use abi_fingerprint; no such "any" fallback documented. |

### 4.4 `src/utils/network_comm/broker_request_comm.cpp` (uncommitted swap + committed layer)

| # | Violation | HEP citation |
|---|---|---|
| D1 | `send_broadcast()` uses retired `CHANNEL_BROADCAST_REQ`. | HEP-0007 §12.4 lines 1485-1488. |
| D2 | `consumer_attach_zmq()` uses retired `CONSUMER_ATTACH_REQ_ZMQ`. | HEP-0007 §12 lines 693-706, HEP-0046 §3. |
| D3 | `pending_requests` map keyed on `correlation_id` alone. | HEP-0046 §I-CORRELATION-STABLE explicit: keys on `(msg_type, correlation_id)`. |
| D4 | BRC never sets scalar `broker_proto` in REG_REQ payload. | This is CORRECT per HEP-0032 §8; the current typed-body check demanding it (A3) is what's wrong. |

### 4.5 `tests/test_framework/broker_wire_client.h` + `.cpp` (uncommitted)

| # | Violation | HEP citation |
|---|---|---|
| E1 | `Config::client_role_uid` defaults to `"pattern4-wire-client"`. Enables identity collision when multiple wire clients share a broker. | HEP-0046 §I-DEALER-IDENTITY: routing_id MUST equal role_uid. Defaults violate per-role uniqueness. |

### 4.6 `tests/test_layer3_datahub/workers/datahub_*_workers.cpp` (uncommitted)

| # | Violation | HEP citation |
|---|---|---|
| F1 | `raw_req` in datahub_broker_workers.cpp falls back to `"raw-req-anon"` when `role_identity_name` empty. | HEP-0046 §I-DEALER-IDENTITY. |
| F2 | `raw_req` in datahub_broker_protocol_workers.cpp falls back to `"raw-req-anon-protocol"`. | Same. |

### 4.7 `src/utils/ipc/broker_service.cpp` (uncommitted swap)

| # | Violation | HEP citation |
|---|---|---|
| G1 | `AdmissionBinder::lookup_known_role` callback iterates `cfg.known_roles` (a config vector). Authoritative source is `KnownRolesStore` loaded from the vault. Need to verify these are the same population; if not, wire the callback against the store directly. | HEP-0035 §4.8.2. |
| G2 | `dispatch_received` visitor + `AdmissionBinder` counter name (C3 above). | HEP-0033 §9.4. |
| G3 | `hub_state_` accessed with writer_lock / shared_lock semantics in comments — HubState uses a single mutex per HEP-0033 §8. | HEP-0033 §8 line 1183. |

### 4.8 What IS aligned (do not touch)

The following committed work IS aligned with the HEPs and should not
be modified as part of this cleanup:

- `src/include/utils/versioned_admission_ledger.hpp` — matches HEP-CORE-0042 §5.5.2.1 INVARIANT-BIND-CONFIRM-1..3.
- `_on_role_confirmed` / `is_pubkey_visible_to` / `is_role_registered_on_channel` on HubState — match the §5.5.2 amendment.
- HEP-CORE-0042 §5.5.2 amendment text — good.
- `tests/test_layer1_base/test_versioned_admission_ledger.cpp` — L1 tests pin invariants directly.
- `src/utils/network_comm/wire_envelope.cpp` — envelope build/parse matches HEP-0046 §14.
- `src/utils/network_comm/wire_adapter.cpp::kRegFamilyMsgTypes` — 6 msg types per HEP-0046 §I-REPLAY-BOUND.

---

## 5. Concrete edit plan (order of execution)

Grouped so each group builds + tests cleanly on its own.

### Group 1 — wire_bodies split + accessor fixes

Files: `src/include/utils/wire_bodies.hpp`, `src/utils/network_comm/wire_bodies.cpp`, `tests/test_layer1_base/test_role_reg_payload.cpp` (any tests that construct RegReqBody directly).

- Delete `RegReqBody::schema_version()` accessor + delete the `require(body_, "schema_version", U32)` line in the constructor.
- Split `RegReqBody` (rename to `ProducerRegReqBody` with producer field set) and add new `ConsumerRegReqBody` with `expected_schema_*` accessor set.
- Remove `broker_proto()` accessor from producer body OR make it optional (returns 0 if absent). Recommended: remove; scalar broker_proto per HEP-0032 §8 is legacy and canonical carrier is `abi_fingerprint`.
- Add producer body accessors for `schema_packing`, `flexzone_blds`, `flexzone_packing`, `producer_pid`, `has_shared_memory`, `zmq_node_endpoint`, `shm_capability_endpoint`, `metadata`, `inbox_endpoint`, `inbox_schema_json`, `inbox_packing`, `inbox_checksum`.
- Add consumer body accessors: same universal fields + `expected_schema_id`, `expected_schema_hash`, `expected_schema_blds`, `expected_schema_packing`, `expected_flexzone_blds`, `expected_flexzone_packing`, `consumer_pid`, `consumer_hostname`.

### Group 2 — admission_gates per-msg-type role-tag policy

Files: `src/include/utils/admission_gates.hpp`, `src/utils/ipc/admission_gates.cpp`.

- Add per-msg-type role-tag lookup table per HEP-CORE-0033 §G2.2.0b.8 table.
- Extend `gate_grammar` (or add new `gate_role_tag`) to reject with `INVALID_ROLE_TAG` when the tag in `role_uid` does not match the msg-type's allowed set.
- Wire the new gate into `run_reg_family_gates` and the authenticated / control gate runners.

### Group 3 — delete or repair `gate_supported_proto`

Files: `src/utils/ipc/admission_gates.cpp`, `src/utils/ipc/broker_reg_handler.cpp`, `src/include/utils/broker_reg_handler.hpp`.

- Preferred: delete `gate_supported_proto` and its call from `run_reg_family_gates`. Rely on the existing `log_peer_abi_fingerprint()` path in broker_service.cpp (already wired) to enforce ABI compatibility per HEP-CORE-0032 §8.
- Delete `BrokerAdmissionConfig::broker_proto` field.
- Delete `AdmissionContext::broker_proto` field OR retain it only for HEP-0036 §6.5 GET_CHANNEL_AUTH_REQ (which does carry the scalar per that HEP).

### Group 4 — wire_dispatch corrections

Files: `src/include/utils/wire_dispatch.hpp`, `src/utils/network_comm/wire_dispatch.cpp`.

- Change `ValidatedConsumerRegReq` to hold `ConsumerRegReqBody`.
- Update `validate_reg_req` and dispatch table `ConsumerRegReq` tier to construct `ConsumerRegReqBody`.
- Delete dispatch table row for `CHANNEL_BROADCAST_REQ`.
- Remove counter-bump `sys.admission_rejected` from `AdmissionBinder`/dispatch path; keep the WARN log.
- Remove the `broker_proto = 0 means any` semantic from `AdmissionBinder` (aligns with Group 3).

### Group 5 — BRC cleanup

File: `src/utils/network_comm/broker_request_comm.cpp` + `src/include/utils/broker_request_comm.hpp`.

- Delete `send_broadcast()` method. Update callers to use `band_broadcast()` (HEP-CORE-0030). Grep for callers first.
- Delete `consumer_attach_zmq()` method. Callers should use the R6 pending path (HEP-CORE-0042 §5.5 amendment) — no client-side wire.
- Change `pending_requests` map key from `std::string correlation_id` to `std::pair<std::string, std::string> (msg_type, correlation_id)`.

### Group 6 — Test-side wire helpers

Files: `tests/test_framework/broker_wire_client.h/.cpp`, `tests/test_layer3_datahub/workers/datahub_*_workers.cpp`, `tests/test_layer3_pattern4/*.cpp`.

- Remove `client_role_uid` default from `BrokerWireClient::Config`. Throw at construction if empty.
- Remove `"raw-req-anon"` / `"raw-req-anon-protocol"` fallbacks from raw_req helpers.
- Update all test callsites to pass explicit role_uid.

### Group 7 — Broker AdmissionBinder wiring against KnownRolesStore

File: `src/utils/ipc/broker_service.cpp`.

- Verify `cfg.known_roles` is populated from the vault-loaded `KnownRolesStore` (HEP-CORE-0035 §4.8.2). If not, wire the `lookup_known_role` callback against `KnownRolesStore::find()` directly. This is a research step — read the load path first.

### Group 8 — Locking-comment cleanup

Files: any comment referencing writer_lock / shared_lock on HubState.

- Correct comments to say "under HubState's single mutex" per HEP-CORE-0033 §8. Not a code fix, just doc alignment.

---

## 6. Verification plan

After each group:
1. `cmake --build /home/qqing/Work/pylabhub/build -j 2 --target stage_all`.
2. Run the affected L1/L2 test filters via `tools/ctest_evidence.sh`.
3. After Group 4-7 complete, run full L1+L2+L3 sweep and read any surviving failures for design-vs-code disagreement, not for "how to make them pass."

Test outcome grading:
- Test that PASSES: the corresponding code + test alignment holds.
- Test that FAILS with a symptom traceable to a documented design invariant it violates: the test needs updating to pin design, not observed behavior. Do not loosen the design.
- Test that FAILS with a symptom NOT traceable to a documented design invariant: my code has a real bug independent of this cleanup. Fix in place.

---

## 7. Design-doc-level items to flag to the design side

These are not code fixes. They are inconsistencies between HEP
documents that any implementer will trip over. Recommend filing as
errata on HEP-CORE-0046 §14.3:

1. **HEP-0046 §14.3's `RegReqBody` field list conflicts with HEP-0034 §10.2** on consumer schema field naming (`expected_` prefix). §14.3 says both REG_REQ and CONSUMER_REG_REQ use `RegReqBody`; §10.2 explicit that consumer uses `expected_schema_*`. Recommendation: §14.3 splits into `ProducerRegReqBody` + `ConsumerRegReqBody`.

2. **HEP-0046 §14.3 includes `schema_version`** — no other HEP defines this field. HEP-0034 §6.3 says fingerprint IS the version discriminator. Recommendation: remove `schema_version` from §14.3.

3. **HEP-0046 §14.3 lists `broker_proto` on RegReqBody** — HEP-0032 §8 says `abi_fingerprint` is canonical. Production senders don't stamp scalar `broker_proto` on REG_REQ. Recommendation: remove `broker_proto` from §14.3 RegReqBody; keep it on the §6.5 auth-family messages per HEP-0036.

4. **HEP-0046 §14 `correlation_id` vs HEP-0036 §6.5 `corr_id`** — the same field is spelled differently in two HEPs. Recommendation: unify on `correlation_id`.

5. **HEP-0032 §8.9 drift-guard** — `BrokerAdmissionConfig::broker_proto` and `kBrokerProtoMajor` are duplicated constants. Recommendation: static_assert linkage, or delete the former if `gate_supported_proto` is removed.

---

## 8. What this document does NOT cover

- Wiring the admission pipeline (BrokerRegHandler) into `broker_service.cpp` handlers is a downstream step, tracked separately. The current work focuses on making the wire itself correct — the admission pipeline currently sits as dead code (has always been so on this branch).
- Splitting `RegReqBody` into producer/consumer requires all-callsite updates in test workers and role_reg_payload.hpp — mechanical follow-on to Group 1.
- Full L3/L4 test alignment: some L3/L4 tests were written against pre-migration wire behavior. Whether each surviving failure is a test-side or code-side fix is decided per HEP citation, not per make-tests-pass reflex.

---

## 9. Review checklist

Before I begin implementation, confirm:

- [ ] §2 field truth tables match your reading of the HEPs.
- [ ] §3 field truth for the other REG-family messages matches.
- [ ] §4 inconsistency list is complete — nothing missed.
- [ ] §5 edit-plan ordering is what you want.
- [ ] §7 errata list is worth filing against HEP-0046 §14.3.
- [ ] Group 3's "delete `gate_supported_proto` rather than repair it" is the direction you want (vs. wiring it to `kBrokerProtoMajor`).
- [ ] Group 5's "delete `send_broadcast()` and `consumer_attach_zmq()`" is fine — no live caller depends on them (I will grep before deleting).

---

## 10. Design conflict resolutions (running log)

Decisions confirmed per user review of §7. Each resolution records the
HEPs that need updating and the concrete edit to make in each.

### C1 — RESOLVED: split `RegReqBody`

**Decision:** Split into `ProducerRegReqBody` + `ConsumerRegReqBody`.
HEP-CORE-0034 §10.2 stays authoritative for consumer-side `expected_`
prefix. HEP-CORE-0046 §14.3's unified-body claim is retired.

**Reasoning captured:**
- Producer schema field = declaration ("I publish schema X").
- Consumer schema field = citation/expectation ("I EXPECT schema X").
- Wire-level self-description via `expected_` prefix aids observability
  (tcpdump readers can tell which side each message is from without
  parsing `role_type`).
- HEP-0034 §10.2 explicitly rejected the unprefixed form as of Phase 4d;
  reversing that would un-do a settled design decision.
- Production code (schema_utils.hpp's `apply_producer_schema_fields` +
  `apply_consumer_schema_fields`) already emits the two different wire
  shapes. No emitter-side change needed.

**HEPs needing update:**
- **HEP-CORE-0046 §14.3** — the RegReqBody catalog entry. Replace with
  two entries: `ProducerRegReqBody` (fields per HEP-CORE-0034 §10.1)
  and `ConsumerRegReqBody` (fields per HEP-CORE-0034 §10.2 including
  the `expected_` prefix). Cross-reference HEP-0034 as authority for
  the schema-related field set on each.

**Code fixes unblocked:**
- Group 1 edit A1-A2 in §5: split `RegReqBody` in `wire_bodies.hpp`.
- Group 4 edit C1 in §5: change `ValidatedConsumerRegReq` variant to
  hold `ConsumerRegReqBody`.

Status: HEP-0046 §14.3 annotated with erratum note (pending confirmed
edit to §14.3 catalog once all C1-C12 resolutions are locked in and
HEP amendments happen as a batch).

---

### C2 — RESOLVED: retire wire `schema_version`; version rides inside `schema_id`

**Decision:** `schema_version` is not a separate wire field. The
version is part of the `schema_id` string per HEP-CORE-0033
§G2.2.0b naming grammar (`$base.v<N>` form). All schema-id
construction, parsing, and validation MUST go through the unified
API in `naming.hpp`.

**Correction to earlier draft claim:** my first pass called
`schema_version` "invented." That was wrong. `schema_version` IS a
real design concept — it identifies the contract, separately from
the content fingerprint. But it does NOT ride on the wire as a
distinct field; it lives inside `schema_id`. Two contracts with the
same fingerprint but different versions produce different
`schema_id` strings and are correctly rejected as different
contracts by a single string-equality check.

**Design principle recorded:** contract identity is
`(owner_uid, schema_id)` — where `schema_id` includes the version.
The fingerprint (`schema_hash`) is a separate CONTENT check. Both
must match on citation. Wire form single-field: `schema_id`
carries name AND version; wire code uses ONE string comparison.

**Unified schema-name API (mandatory, no ad-hoc string manipulation):**
- **Parse**: `pylabhub::hub::parse_schema_id(std::string_view id)` →
  `std::optional<SchemaIdParts>` (naming.hpp:157).
- **Validate**: `pylabhub::hub::is_valid_identifier(id, IdentifierKind::Schema)`
  (naming.hpp:91).
- **Assert-valid**: `pylabhub::hub::require_valid_identifier(id, kind, ctx)`
  (naming.hpp:105).
- **Construct (new helper to add)**:
  `pylabhub::hub::make_schema_id(std::string_view base, std::uint32_t version)`
  returning `"$" + base + ".v" + std::to_string(version)`. Debug-mode
  self-validates via `is_valid_identifier` to catch base-string
  malformations early. Single source of truth for canonical form;
  no inline `"$" + base + ".v" + ...` at any other site.

**Cross-HEP conflict flagged:** HEP-CORE-0034 §5.1 uses `{name}@{version}`
example form (with `@`). HEP-CORE-0033 §G2.2.0b uses `$name.v<N>`
(with `.v`). Code (schema_loader.cpp:246, `parse_schema_id`) follows
HEP-0033. HEP-0034 §5.1 is out of sync.

**HEPs needing update:**
- **HEP-CORE-0034 §5.1** — replace `{name}@{version}` example form
  with canonical `$name.v<version>` per HEP-0033 §G2.2.0b. Add
  contract note: "all schema-id construction/parsing/validation
  MUST use `pylabhub::hub::make_schema_id` /
  `parse_schema_id` / `is_valid_identifier(..., Schema)`; no
  ad-hoc string manipulation."
- **HEP-CORE-0034 §6.3** — add note distinguishing content
  discriminator (fingerprint) from contract identity discriminator
  (`schema_id` including version). Different contracts CAN share
  fingerprints; same contract MUST have same fingerprint.
- **HEP-CORE-0034 §10.1** — add note: `schema_id` includes the
  version per HEP-0033; broker validates identity via `schema_id`
  string equality (which subsumes version) then content via
  `schema_hash`.
- **HEP-CORE-0034 §10.2** — same note on consumer side
  (`expected_schema_id` includes version).
- **HEP-CORE-0033 §G2.2.0b** — add cross-reference: "canonical
  constructor for schema ids: `pylabhub::hub::make_schema_id(base,
  version)`; canonical parser: `parse_schema_id()`."
- **HEP-CORE-0007 §12.3 DISC_ACK** — remove `schema_version` field
  from payload table; subsumed by `schema_id`.
- **HEP-CORE-0046 §14.3** — correct my erratum block. `schema_version`
  is not "spurious" (that was my mischaracterization); it lives
  inside `schema_id` per HEP-0033. Remove `schema_version` from both
  amended body-class field lists.

**Code fixes:**
- `naming.hpp` — add `make_schema_id(base, version)` helper.
- `schema_loader.cpp:246` — use `make_schema_id`.
- `wire_bodies.hpp` — drop `schema_version()` accessor and
  `require(..., "schema_version", U32)` line.
- `hub_state.hpp:361` — remove `ChannelSchemaInvariants::schema_version`
  field.
- `broker_service.cpp:2608, 2686, 3038` — remove separate
  `schema_version` handling (read/echo/invariant-field-list).
- `hub_state.cpp:1292` — remove separate `schema_version` reject;
  `schema_id` string equality subsumes it.
- `hub_state_json.cpp:51` — remove `schema_version` serialization.
- Tests — remove every `reg_opts["schema_version"] = N;` line; ensure
  test `schema_id` strings are in canonical `$name.v<N>` form built
  via `make_schema_id`.

**Code fixes unblocked:**
- Group 1 in §5 modified: no `schema_version` accessor on either
  body class; both ProducerRegReqBody and ConsumerRegReqBody carry
  `schema_id` / `expected_schema_id` in canonical form.

Status: HEP-0046 §14.3 erratum block updated to correct my prior
mischaracterization. Other HEP amendments (HEP-0034, HEP-0033,
HEP-0007) pending batch after all C1-C12 confirmations.

---

### C3 — RESOLVED: remove scalar `broker_proto` from REG_REQ / CONSUMER_REG_REQ; `abi_fingerprint` is the canonical wire-version carrier

**Decision:** REG-family REQs carry `abi_fingerprint` only for ABI /
wire-version enforcement.  Scalar `broker_proto` field is retired
from `RegReqBody` (and any successor split into
`ProducerRegReqBody` + `ConsumerRegReqBody`).  Scalar `broker_proto`
IS retained on HEP-CORE-0036 §6.5 auth-family messages
(`CHANNEL_AUTH_CHANGED_NOTIFY`, `GET_CHANNEL_AUTH_REQ`,
`GET_CHANNEL_AUTH_ACK`) — different concern per that HEP's field
tables.

**Design principle recorded:** wire-version and ABI-compatibility
verification lives at ONE place — the `abi_fingerprint` object
per HEP-CORE-0032 §8.  It carries all 7 axes
(`library`, `shm`, `broker_proto`, `zmq_frame`, `script_api`,
`script_engine`, `config`), each as major/minor.  Verification
runs through `pylabhub::version::verify_peer_versions()` with the
policy taxonomy from HEP-CORE-0032 §8.5
(`Ok` / `BuildOnly` / `MinorMismatch` / `MajorMismatchAccepted` /
`MajorMismatchRejected` / `Absent` / `InvalidEnvelope`).  Strict
mode is opt-in via `broker.strict_abi_mismatch` /
`role.strict_abi_mismatch`.

Additional load-bearing property: HEP-CORE-0026 §2.6 originally
scheduled *"Broker handshake | Exchange `wire_major.wire_minor`
in REG_REQ"* as a future item — that future was materialized as
`abi_fingerprint` in HEP-CORE-0032 §8, not as a separate scalar
field.  HEP-0026 §2.6 needs an update pointing at HEP-0032 §8 as
the fulfilment.

**Additional requirement from user (2026-07-13):** every HEP that
touches REG_REQ / CONSUMER_REG_REQ or references
`broker_proto` / ABI matching MUST explicitly explain the
matching rules and cite HEP-CORE-0032 §8 as the authority.  A
reader landing on any REG-family HEP should be able to see, at
that section, how wire-version + ABI verification actually work
without hunting across the doc tree.

**HEPs needing update:**
- **HEP-CORE-0046 §14.3** — my erratum block already flags removal
  of `broker_proto`.  Confirm the removal in the C3 pending errata
  batch.  Add a sentence at the top of §14.3 (or §14 preamble)
  saying: *"Wire-version + ABI compatibility is verified via the
  body's `abi_fingerprint` object per HEP-CORE-0032 §8 (7-axis
  matrix, major/minor per axis, `verify_peer_versions()` policy
  taxonomy per §8.5).  No separate wire-version scalar rides on
  REG-family REQs."*
- **HEP-CORE-0032 §8** — already authoritative; add an explicit
  paragraph making clear that the scalar `broker_proto` field
  that appears in the current code and in HEP-0046 §14.3 is
  retired for REG_REQ / CONSUMER_REG_REQ.  Retained only on
  §6.5 auth-family messages.
- **HEP-CORE-0032 §8.9** — the drift-guard between
  `BrokerAdmissionConfig::broker_proto` and `kBrokerProtoMajor`
  becomes moot when both are deleted.  Note the removal and cite
  the axis-inside-fingerprint as the only remaining source.
- **HEP-CORE-0026 §2.6** — update the "Broker handshake | Exchange
  `wire_major.wire_minor` in REG_REQ | Future (HEP-0023)" row to
  point at HEP-CORE-0032 §8 (`abi_fingerprint`) as the fulfilment.
- **HEP-CORE-0036 §5b.4 + §5b.6** — REG_REQ / CONSUMER_REG_REQ
  field tables already do NOT list scalar `broker_proto`; add an
  explicit note at the head of each table: *"ABI + wire-version
  is carried by `abi_fingerprint` per HEP-CORE-0032 §8; no
  separate scalar version field on REG_REQ."*
- **HEP-CORE-0036 §6.5** — auth-family messages
  (`CHANNEL_AUTH_CHANGED_NOTIFY`, `GET_CHANNEL_AUTH_REQ`,
  `GET_CHANNEL_AUTH_ACK`) retain scalar `broker_proto`.  Add an
  explicit note explaining why: this is a per-message
  auth-protocol version, distinct from the initial-REG ABI
  verification which uses `abi_fingerprint`.  Callers on these
  messages check the scalar against the broker's stated
  auth-family protocol version.
- **HEP-CORE-0007 §12 preamble** — add a wire-version-verification
  cross-reference at the top of the msg_type catalog: *"For any
  REG-family REQ, wire-version + ABI compatibility is verified
  via the body's `abi_fingerprint` object per HEP-CORE-0032 §8;
  no separate scalar wire-version field is used on REG_REQ /
  CONSUMER_REG_REQ."*
- **HEP-CORE-0033 §9.7** (broker handler structural contract) —
  add note that step 1 (broker-level field validation) includes
  the ABI-fingerprint verification per HEP-CORE-0032 §8; on
  strict-mode major mismatch this fails at that step and no
  HubState op runs.
- **HEP-CORE-0023 §2.5** — no direct mention today of ABI /
  wire-version; add a paragraph or footnote clarifying that the
  role's ABI is declared on REG_REQ once (via `abi_fingerprint`)
  and not re-declared on subsequent HEARTBEAT_REQ.

**Code fixes:**
- `wire_bodies.hpp` — drop `RegReqBody::broker_proto()` accessor
  and its `require(..., "broker_proto", U32)` line.  (When the
  body class splits per C1, neither `ProducerRegReqBody` nor
  `ConsumerRegReqBody` gains the accessor.)
- `admission_gates.hpp` + `admission_gates.cpp` — delete
  `gate_supported_proto` and its call from
  `run_reg_family_gates`.  `RegFamilyBodyView` loses the
  `broker_proto` field.
- `broker_reg_handler.hpp` — delete
  `BrokerAdmissionConfig::broker_proto` field.
- `broker_reg_handler.cpp` — delete `ctx.broker_proto = ...`
  line.
- `admission_gates.hpp` — `AdmissionContext::broker_proto` field
  removed.
- `wire_dispatch.hpp` + `.cpp` — `AdmissionBinder`
  initialization: delete
  `admission_binder_.context.broker_proto = 0` and any
  related "0 = any" comment.  The invented
  "broker_proto = 0 means any" semantic is gone.
- `broker_service.cpp` — delete
  `impl->admission_binder_.context.broker_proto = 0` line from
  BrokerService::BrokerService.
- Tests — remove every `body["broker_proto"] = 7U;` and every
  `req["broker_proto"] = ...` line I set for wire_dispatch
  validation and any pre-existing tests that set the scalar.

**Code paths that DO NOT change:**
- `apply_common_reg_envelope` in `role_reg_payload.hpp` continues
  to stamp `abi_fingerprint` + optional `build_id`.
- `log_peer_abi_fingerprint` in `broker_service.cpp:2081`
  continues to invoke `verify_peer_versions()` per HEP-0032 §8.
- `AbiFingerprintOutcome::reject` → `abi_major_mismatch` ERROR
  reply path continues to run under strict mode.
- Role-side `log_broker_abi_fingerprint` on REG_ACK continues to
  enforce the same taxonomy in the reverse direction.
- HEP-0036 §6.5 auth-family messages retain scalar `broker_proto`.

**Code fixes unblocked:**
- Group 3 in §5: delete `gate_supported_proto` (confirmed
  direction — recommendation from §5 already matched Option A).
- Group 4 in §5 edits `AdmissionBinder`: drop `broker_proto`
  context init.

Status: HEP-CORE-0046 §14.3 erratum block updated to point at
`abi_fingerprint` as canonical carrier and reference HEP-0032 §8
policy.  Other HEP amendments (HEP-0032, HEP-0026, HEP-0036,
HEP-0007, HEP-0033, HEP-0023) pending batch after all C1-C12
confirmations.

---

### C4 — RESOLVED: unify field-name spelling on `correlation_id`

**Decision:** the canonical wire spelling of the correlation
identifier is `correlation_id`.  The shorthand `corr_id` (used
only in HEP-CORE-0036 §6.5 field tables) is retired from the
docs; no code change needed.

**Evidence:**
- Zero code sites use `"corr_id"` as a wire key.
- Every code site (BRC, broker `make_error`, wire_dispatch
  envelope, wire_bodies typed accessors) uses `correlation_id`.
- HEP-CORE-0007 §12.3 uses `correlation_id` throughout REG-family
  ACK payload tables (lines 987, 1078, 1094, 1110, 1185, 1265, ...).
- HEP-CORE-0036 §6.5 is the only outlier — 2 field-table cells at
  lines 3979 (`GET_CHANNEL_AUTH_REQ`) and 3987
  (`GET_CHANNEL_AUTH_ACK`) spell it `corr_id`.

**HEPs needing update:**
- **HEP-CORE-0036 §6.5** — change `corr_id` → `correlation_id` in
  the 2 field-table cells.
- **HEP-CORE-0046 §14** OR **HEP-CORE-0007 §12.2.1** — add a
  one-sentence canonical-spelling declaration to prevent future
  drift.  Preferred: HEP-CORE-0046 §14 preamble, since HEP-0046
  is now the wire authority (delegation banners in HEP-0007
  and others).

**Code changes:** none.  Code is already unified on
`correlation_id`.

Status: HEP-0046 §14 preamble + HEP-0036 §6.5 pending batch after
all C1-C12 confirmations.

---

### C5 — RESOLVED: `correlation_id` REQUIRED on every non-NOTIFY REQ

**Decision:** every REQ msg_type that expects an ACK MUST carry a
non-empty `correlation_id`.  HEP-CORE-0046 §I-CORRELATION-STABLE
wins over HEP-CORE-0036 §5b's "OPTIONAL" markers.  The wire
envelope's Frame 3 is a mandatory frame; empty value on a
non-NOTIFY is a wire violation rejected at
`WireEnvelope::parse_router_recv` with `ParseError::correlation_missing`.

**Evidence code already enforces REQUIRED:**
- BRC `do_request_multi` always generates a fresh 32-hex
  `correlation_id` per REQ (if caller didn't set one).
- `wire::adapter::encode_dealer_send` refuses empty
  correlation_id on non-NOTIFY msg_types.
- `WireEnvelope::parse_router_recv` rejects empty Frame 3 on
  non-NOTIFY.

**Design principle recorded:** under stable DEALER identity
(I-DEALER-IDENTITY), any number of concurrent REQs of the same
msg_type can be in-flight from one role.  Correlation_id is the
sole reliable way to match the incoming ACK to the specific REQ
that produced it.  Without it, silent cross-wiring of replies
becomes a real hazard.  `pending_requests` map is keyed on
`(msg_type, correlation_id)` composite per §I-CORRELATION-STABLE.

**HEPs needing update:**
- **HEP-CORE-0036 §5b.4** — REG_REQ field table: mark
  `correlation_id` REQUIRED (was OPTIONAL).  Add cross-reference
  to HEP-0046 §I-CORRELATION-STABLE.
- **HEP-CORE-0036 §5b.6** — CONSUMER_REG_REQ field table: same.
- **HEP-CORE-0036 §6.5** — `GET_CHANNEL_AUTH_REQ` and
  `CHANNEL_AUTH_APPLIED_REQ` field tables: mark
  `correlation_id` REQUIRED (spelling per C4 fix).
- **HEP-CORE-0007 §12.3** — every ACK row currently says
  *"correlation_id | (opt) Echo of request correlation_id if
  provided"*.  Reword to *"correlation_id | required; echoes
  REQ.correlation_id per HEP-CORE-0046 §I-CORRELATION-STABLE."*
- **HEP-CORE-0007 §12.2.1** — the shape-conformance section:
  add explicit statement that every `_REQ` msg_type expecting an
  ACK MUST carry non-empty `correlation_id`; every `_ACK` MUST
  echo it; empty on either is a wire violation.  Note that
  fire-and-forget REQs (see C13) are exempt.

**Code changes:** none.  Code already enforces REQUIRED.

Status: HEP-0036 §5b.4 + §5b.6 + §6.5, HEP-0007 §12.3 +
§12.2.1 pending batch after all C1-C12 confirmations.

---

### C13 — CONFIRMED: rename `HEARTBEAT_REQ` → `HEARTBEAT_NOTIFY` for taxonomy consistency

**Decision:** rename the wire literal.  User instruction
2026-07-14: "this is a large change, need careful review what
to do" — so the rename direction is set, but the sequencing
and scope of the code cutover is subject to a careful review
before touching code.

**Reason for the rename:** HEP-CORE-0046 §I-MSG-TYPE-TAXONOMY
defines `_NOTIFY` suffix as fire-and-forget (no ACK,
best-effort), `_REQ` suffix as request-reply (requires `_ACK` or
`ERROR`).  HEARTBEAT_REQ is fire-and-forget per HEP-CORE-0007
§12.4 and per every implementation site (no ACK path).  Its
`_REQ` suffix violates the taxonomy.  Renaming to
`HEARTBEAT_NOTIFY` aligns the suffix with the semantic.

**Scope of the change (impact analysis pending):** the rename
touches wire literal, handler dispatch, BRC send method name(s),
CMake / dispatch tables, tests (log-substring assertions across
L2/L3/L4), documentation, and possibly the `broker_proto`
version bump if the rename is a breaking change (it is — old
brokers won't recognize `HEARTBEAT_NOTIFY`, old roles won't
recognize a broker that stopped expecting `HEARTBEAT_REQ`).

**Two-phase migration option:**
1.  Broker accepts BOTH literals (`HEARTBEAT_REQ` and
    `HEARTBEAT_NOTIFY`) for one broker_proto version;
    `HEARTBEAT_REQ` deprecation-logged.  Role senders switched
    to `HEARTBEAT_NOTIFY`.
2.  Next `broker_proto` MAJOR bump: broker drops
    `HEARTBEAT_REQ` acceptance; roles send `HEARTBEAT_NOTIFY`
    only.

**Single-cut option:**
- Bump `broker_proto_major` in the same commit that renames.
  Everyone upgrades together.  Simpler.

Impact analysis + sequencing decision recorded in a separate
sub-section after review below.

**Sequencing decision (confirmed 2026-07-14):** bundle the rename
into the C1-C12 atomic commit.  Single `broker_proto_major`
bump `7 → 8` covers every wire-shape change (C1 body-class
split, C2 schema_version wire retirement, C3 broker_proto
scalar retirement, C13 HEARTBEAT rename) atomically per
HEP-CORE-0046 §I-WIRE-VERSION-ATOMIC.  No two-phase
acceptance window.

**Impact scope (impact analysis 2026-07-14):**
- 59 grep hits across 23 code files (12 production, 11 test).
- 11 HEPs reference `HEARTBEAT_REQ`.
- Wire literal changes at: BRC send site
  (`broker_request_comm.cpp:949`), broker known-msg-type list
  (`broker_service.cpp:177`), broker dispatch
  (`broker_service.cpp:1622`, `:1845`), wire_dispatch table.
- Handler function `handle_heartbeat_req` → recommend
  `handle_heartbeat_notify` (internal, cosmetic).
- BRC public method `send_heartbeat()` UNCHANGED (caller-facing
  helper; wire literal changes inside).
- 11 HEPs need msg_type reference updates:
  HEP-CORE-0002, -0007, -0017, -0018, -0019, -0021, -0023,
  -0030, -0033, -0036, -0046.
- Test-side log-substring assertions rename.
- `native_engine_api.h` comment (script-visible protocol
  version history) update.

**broker_proto version bump:** `kBrokerProtoMajor` in
`plh_version_registry.hpp:227` changes `7 → 8`.  Bump commentary
in the same header records this cleanup as the trigger.

**HEPs needing update (bundled with C1-C12 batch):**
- **HEP-CORE-0007 §12.4** — `HEARTBEAT_REQ` → `HEARTBEAT_NOTIFY`.
  This section is the primary authority for the msg_type
  catalog under §12; update the fire-and-forget entry.
- **HEP-CORE-0023 §2.5** — heartbeat spec; primary owner.
  Rename references throughout.
- **HEP-CORE-0019 §2.3 + §4.1** — metrics piggyback on
  heartbeats; rename references.
- **HEP-CORE-0033 §G2.2.0b.8** — per-msg-type role-tag policy
  table currently lists `HEARTBEAT_REQ`; rename.
- **HEP-CORE-0036 §5b** — REG_REQ / CONSUMER_REG_REQ tables
  reference heartbeat cadence via REG_ACK; rename.
- **HEP-CORE-0046 §I-MSG-TYPE-TAXONOMY** — the invariant that
  triggered this rename.  Update to note that HEARTBEAT was
  historically named `_REQ` despite being fire-and-forget, and
  the rename resolves it.  Add explicit rule: any new
  fire-and-forget msg_type MUST use the `_NOTIFY` suffix; any
  new request-reply msg_type MUST use the `_REQ` suffix with
  matching `_ACK`.
- **HEP-CORE-0046 §14.3** — `HeartbeatReqBody` typed body class
  → rename to `HeartbeatNotifyBody`; move from REQ section to
  NOTIFY section.  Envelope-hash only (already correct — no
  security triple).
- HEP-CORE-0002, -0017, -0018, -0021, -0030 — prose references
  only; batch rename.

**Code fixes (bundled in atomic commit):**
- `broker_request_comm.cpp:949` — wire literal.
- `broker_service.cpp:177, 1622, 1845, 5160+` — known-msg-type
  list, dispatch case, handler function name.
- `wire_dispatch.cpp` + `wire_dispatch.hpp` — dispatch table
  entry, ValidatedHeartbeat variant rename.
- `wire_bodies.hpp` — `HeartbeatReqBody` → `HeartbeatNotifyBody`.
- `admission_gates.hpp` — role-tag policy table comment.
- `plh_version_registry.hpp:227` — `kBrokerProtoMajor: 7 → 8`;
  add commentary block explaining this cleanup as the trigger.
- Test-side: rename literal + log-substring assertions across
  8 test files.

Status: rename direction confirmed; scope + sequencing recorded;
bundled with C1-C12 atomic commit + `broker_proto_major: 7 → 8`.

---

### C6 — RESOLVED: `role_uid` REQUIRED on REG_REQ; HEP-0007 §12.3 `(opt)` marker retired

**Decision:** `role_uid` is REQUIRED on REG_REQ.  HEP-CORE-0023
§2.5.4 canonical field-name table + HEP-CORE-0036 §5b.4 field
table win over HEP-CORE-0007 §12.3's stale `(opt)` marker.

**Evidence code already enforces REQUIRED:**
- Producer + consumer role hosts always populate `role_uid` from
  `KeyStore` + role identity setup.  No caller sends empty.
- Broker `handle_reg_req` (broker_service.cpp:1974) invokes
  `validate_identity_fields(...)` which rejects
  `INVALID_REQUEST` on empty/malformed.
- HEP-CORE-0036 §6.1 Layer-2 identity check requires non-empty
  `role_uid` for `known_roles` lookup — empty cannot pass.
- HEP-CORE-0033 §G2.2.0b.8 unconditional `is_valid_identifier(...,
  RoleUid)` at handler entry rejects empty.

**Design reasoning:** every downstream operation keyed on role
identity (nonce dedup, admission, allowlist mutation, DEREG
resolution, heartbeat routing) requires non-empty `role_uid`.
Once HEP-CORE-0023 §2.1.1 made `(pid, role_uid)` the target-
resolution tuple, "OPTIONAL" ceased to be implementable.  The
HEP-0007 §12.3 `(opt)` marker was written pre-§2.1.1 and never
updated.

**HEPs needing update:**
- **HEP-CORE-0007 §12.3 REG_REQ payload table** — remove `(opt)`
  from `role_uid`.  Add cross-reference note: *"See HEP-CORE-0023
  §2.5.4 canonical field-name table for the authoritative
  role_uid + role_name requiredness across REG-family messages."*

**Code changes:** none.

Status: HEP-0007 §12.3 pending batch after all C1-C12 confirmations.

---

### C7 — RESOLVED: CONSUMER_REG_REQ + CONSUMER_DIED_NOTIFY rename `consumer_uid`/`consumer_name` → `role_uid`/`role_name`

**Decision:** consumer-side wire field names on both
`CONSUMER_REG_REQ` and `CONSUMER_DIED_NOTIFY` are `role_uid` +
`role_name`, both REQUIRED.  HEP-CORE-0007 §12.3 + §12.5 stale
`consumer_uid` / `consumer_name` names are retired per the
broker_proto 4→5 unification recorded in HEP-CORE-0023 §2.5.4.

**Evidence code already unified:**
- Zero grep hits for `"consumer_uid"` or `"consumer_name"` as
  wire keys in `src/` or `tests/`.
- Every consumer sender + broker handler + test worker uses
  `role_uid` / `role_name`.
- Rename executed in broker_proto 4→5 (2026-05-19, audit R3.5b)
  per HEP-CORE-0023 §2.5.4.

**HEPs needing update:**
- **HEP-CORE-0007 §12.3 CONSUMER_REG_REQ payload table** —
  rename `consumer_uid`/`consumer_name` → `role_uid`/`role_name`,
  mark both REQUIRED, cross-reference HEP-CORE-0023 §2.5.4
  canonical field-name table.
- **HEP-CORE-0007 §12.5 CONSUMER_DIED_NOTIFY payload table** —
  rename `consumer_uid` → `role_uid` (retain REQUIRED marker).
  Cross-reference HEP-CORE-0023 §2.5.4.  Same stale-name pattern
  from broker_proto 4→5.
- **HEP-CORE-0019** — any leftover `consumer_uid` prose
  references to the CONSUMER_DIED_NOTIFY wire field (grep-and-
  amend during batch).

**Code changes:** none.

Status: HEP-0007 §12.3 + §12.5 pending batch after all C1-C12
confirmations.

---

### C8 — RESOLVED: REG_REQ transport discriminator is `data_transport`; retire HEP-0007 §12.3 `shm_name` + `has_shared_memory`

**Decision:** REG_REQ carries a single `data_transport` string
discriminator (`"shm"` \| `"zmq"`) per HEP-CORE-0036 §5b.4.  The
older HEP-CORE-0007 §12.3 fields `shm_name` and `has_shared_memory`
are retired.

**Evidence code already unified:**
- Zero grep hits for `"shm_name"` or `"has_shared_memory"` as
  wire keys in `src/` or `tests/`.
- Every producer sender + broker handler + test worker uses
  `data_transport`.
- Broker's `handle_reg_req` at broker_service.cpp:2190 requires
  `data_transport` non-empty string; empty → `INVALID_REQUEST`.

**Design principle recorded:** single-field transport
discriminator eliminates the ambiguity that came from having
both `shm_name` (implies SHM) and `has_shared_memory` (may
disagree with shm_name) — those two could contradict and the
broker had to pick one.  `data_transport` string is
unambiguous.  SHM + fan-in combination is refused per
HEP-CORE-0017 §3.3.0 gate 1 with
`TOPOLOGY_NOT_SUPPORTED_FOR_TRANSPORT`.

**HEPs needing update:**
- **HEP-CORE-0007 §12.3 REG_REQ payload table** — remove
  `shm_name` row and `has_shared_memory` row.  Add
  `data_transport` row: REQUIRED, `"shm"` \| `"zmq"`.
  Cross-reference HEP-CORE-0036 §5b.4 as authority; note that
  SHM + fan-in is refused per HEP-CORE-0017 §3.3.0 gate 1.
- **HEP-CORE-0007 §12.3 DISC_ACK payload table** — verify
  `has_shared_memory` isn't listed as a legacy sibling; if it
  is, remove.

**Code changes:** none.

Status: HEP-0007 §12.3 pending batch after all C1-C12
confirmations.

---

### C9 — RESOLVED: CONSUMER_REG_REQ carries `data_transport` REQUIRED; retire legacy `consumer_queue_type`

**Decision:** CONSUMER_REG_REQ MUST carry `data_transport` string
(`"shm"` \| `"zmq"`) per HEP-CORE-0036 §5b.6.  Broker rejects
`INVALID_REQUEST` on missing/empty and `TRANSPORT_MISMATCH` if
the value disagrees with channel's stored `data_transport`.
Legacy wire key `consumer_queue_type` is retired.

**Real correctness gap surfaced:** code today does NOT emit
`data_transport` on CONSUMER_REG_REQ.  Broker reads
`consumer_queue_type` (legacy field name) and treats empty as
"skip transport check."  `TRANSPORT_MISMATCH` never fires from
the consumer side — silent transport confusion possible under
fan-out ZMQ vs SHM misconfiguration.

**Code changes (bundled with C1-C12 atomic commit):**
- `role_reg_payload.hpp:99` (`ConsumerRegInputs`): add
  `std::string data_transport;` field.
- `role_reg_payload.hpp:234` (`build_consumer_reg_payload`):
  emit `reg["data_transport"] = in.data_transport`.
- `consumer_role_host.cpp:317`: populate
  `cons_reg_in.data_transport` from role config transport.
- `processor_role_host.cpp:413`: same for processor's consumer
  side.
- `broker_service.cpp:3410` (`handle_consumer_reg_req`
  transport gate): rename read key
  `consumer_queue_type` → `data_transport`; make REQUIRED;
  reject `INVALID_REQUEST` on empty; preserve
  `TRANSPORT_MISMATCH` on channel-transport disagreement.
- `ConsumerRegReqBody` (once C1 split lands): typed accessor
  `data_transport()`; required-field validation in constructor.
- Tests: rename any `consumer_queue_type` wire-key sets to
  `data_transport`.  Grep across L3 datahub workers.

**HEPs needing update:**
- **HEP-CORE-0007 §12.3 CONSUMER_REG_REQ payload table** — add
  `data_transport | string | REQUIRED (\"shm\" | \"zmq\") —
  must equal channel's stored data_transport; mismatch →
  TRANSPORT_MISMATCH`.  Cross-reference HEP-CORE-0036 §5b.6.
- **HEP-CORE-0036 §5b.6** — already correct.
- **HEP-CORE-0017 §3.3.0** — add note that CONSUMER_REG_REQ
  carries `data_transport` too (not only producer REG_REQ).
- **HEP-CORE-0046 §14.3** — my erratum block already lists
  `data_transport` REQUIRED on the amended `ConsumerRegReqBody`;
  stays.

Status: pending batch after all C1-C12 confirmations.  Code
work bundled with C1-C12 atomic commit + broker_proto bump.

---

### C10 — RESOLVED: `producer_pid` / `consumer_pid` / `consumer_hostname` are OPTIONAL diagnostic fields; HEP-CORE-0007 §12.3 stale

**Correction to earlier audit recommendation.**  My initial
recommendation in the C10 audit item was "mark REQUIRED to align
with HEP-0007 §12.3 implicit-required."  Reading the broker code
showed that reading was wrong — these fields are diagnostic /
informational since Phase 6.  HEP-CORE-0036 §5b.4 / §5b.6's
"Optional" markers are accurate for current code.

**Decision:** these fields are OPTIONAL wire fields.  Broker MUST
accept REG_REQ / CONSUMER_REG_REQ / DEREG_REQ / CONSUMER_DEREG_REQ
/ HEARTBEAT_REQ (soon HEARTBEAT_NOTIFY per C13) with these fields
missing or zero — protocol correctness does not depend on them.
Their role is:
- `producer_pid` / `consumer_pid` — early process-death detection
  via `platform::is_process_alive(pid)`; echoed in
  `CONSUMER_DIED_NOTIFY` for operator diagnostics.  NOT used by
  the presence FSM (Phase 6 `(channel, uid, role_type)` tuple is
  authoritative).
- `consumer_hostname` — informational only; echoed in
  `CONSUMER_DIED_NOTIFY`.

**Evidence code already treats OPTIONAL:**
- `broker_service.cpp:5272-5281` HEARTBEAT_REQ handler explicit
  comment: *"`producer_pid` is retained on the wire from Phase 1
  for diagnostic / audit purposes only; the broker no longer
  uses it for presence resolution ... Missing or zero pid is
  logged for diagnostics but does not reject the heartbeat."*
- `broker_service.cpp:2091`, `:3072` REG_REQ / CONSUMER_REG_REQ
  handlers read pid with `.value("...", uint64_t{0})` — missing
  → 0, stored, no rejection.
- DEREG resolution uses `role_uid` alone post-broker_proto 2→3
  (HEP-CORE-0023 §2.1.1 rename).

**HEPs needing update:**
- **HEP-CORE-0007 §12.3 REG_REQ payload table** — add `(opt)`
  marker to `producer_pid`.  Add note: *"diagnostic /
  early-death-detection field; not used for target resolution.
  See HEP-CORE-0023 §2.1.1 amendment (broker_proto 2→3) where
  `role_uid` became the authoritative target-resolution key."*
- **HEP-CORE-0007 §12.3 CONSUMER_REG_REQ payload table** — add
  `(opt)` markers to `consumer_pid` and `consumer_hostname`.
  Same note.
- **HEP-CORE-0007 §12.3 DEREG_REQ / CONSUMER_DEREG_REQ tables**
  — verify pid fields are already marked opt or add markers.
  Note that resolution is by `role_uid` post-broker_proto 2→3.
- **HEP-CORE-0007 §12.4 HEARTBEAT_REQ payload table** — mark
  `producer_pid` as opt with the same note.
- **HEP-CORE-0007 §12.5 CONSUMER_DIED_NOTIFY payload table** —
  note that `consumer_pid` and `consumer_hostname` may be
  zero/empty if the original REG didn't provide them (they
  echo whatever the REG provided).
- **HEP-CORE-0023 §2.1.1** — amend to clarify that after the
  broker_proto 2→3 rename, `role_uid` alone is sufficient for
  target resolution; `(pid, role_uid)` tuple is retained only
  for legacy compatibility and diagnostic value.
- **HEP-CORE-0036 §5b.4 + §5b.6** — already correct; no change.

**Code changes:** none.

**Correction to §7 audit item C10 in this draft:** the earlier
"mark REQUIRED" recommendation is superseded by the OPTIONAL
resolution here.  §7 item 6 (which reflected the pre-code-read
recommendation) is retracted.  Batch amendments to HEP-0007 add
`(opt)` markers where currently missing, aligned with code and
HEP-0036.

Status: HEP-0007 §12.3-§12.5, HEP-0023 §2.1.1 pending batch
after all C1-C12 confirmations.

---

### C11 — RESOLVED: retire `CHANNEL_BROADCAST_REQ` end-to-end (code + docs)

> **ERRATUM 2026-07-14: C11 REVERSED.**  This item is wrong on the
> facts.  `CHANNEL_BROADCAST_REQ` is **NOT** retired.  HEP-CORE-0030
> §9.1 (audit T3 correction, 2026-05-17) explicitly places
> `CHANNEL_BROADCAST_REQ` / `CHANNEL_BROADCAST_NOTIFY` in the
> **coexisting (NOT superseded)** category.  The channel-bound
> broadcast family serves a different membership axis from the
> band-bound family:
>
> - **Channel-bound** (`CHANNEL_BROADCAST_REQ`): registry-derived
>   membership — every REG'd producer + every CONSUMER_REG'd
>   consumer of a data channel receives the fan-out.  No opt-in.
> - **Band-bound** (`BAND_BROADCAST_REQ`, HEP-CORE-0030): opt-in
>   membership via `band_join`.  Different coordination need.
>
> **Live production callers of `CHANNEL_BROADCAST_REQ`:**
> - `src/utils/service/hub_api.cpp:370` — public hub API
>   `broadcast_channel`
> - `src/utils/service/native_engine.cpp:257,265` — native C ABI
>   export `ctx_hub_broadcast_channel`
> - `src/utils/ipc/admin_service.cpp:582` — admin CLI broadcast
> - Federation relay in broker's poll loop (synthetic
>   `handle_channel_broadcast_req` delegation at
>   `broker_service.cpp:1350-1365`)
>
> The BRC method `send_broadcast()` has no role-side callers today
> because roles reach channel-broadcast via the hub-side path
> (`hub.broadcast_channel` → `request_broadcast_channel` →
> internal queue → `handle_channel_broadcast_req`), not through the
> BRC wire path.  The wire msg_type stays live (see HEP-0030 §9.1);
> L3 tests exercising it via BRC validate the broker-side handler
> and are legitimate.
>
> **Actions superseded:** all "Code changes" and "HEPs needing
> update" below are cancelled.  No `CHANNEL_BROADCAST_REQ` removal
> occurs.  A doc-fix task remains: add a HEP-CORE-0007 §12.4
> cross-reference pointing at HEP-CORE-0030 §9.1 so readers landing
> on either HEP find the coexistence table.
>
> **Consumer_attach_zmq retirement** (Group 5 sub-item) is likewise
> not mechanical: the method has a production caller at
> `src/utils/service/role_api_base.cpp:1178` and its removal
> requires the R6 pending-path migration (HEP-CORE-0042 §5.5) —
> tracked as a separate future task, not part of this cleanup.
>
> **pending_requests composite-key change** (Group 5 sub-item) is
> defensive only: correlation_ids are `make_random_hex16()` per
> REQ, no collision has ever been observed.  Skipped.
>
> Net: Group 5 collapses to zero code changes; the doc cross-ref
> is folded into the HEP-batch phase.

**~~Decision:~~** ~~`CHANNEL_BROADCAST_REQ` is retired completely.
HEP-CORE-0007 §12.4 already documented it as REMOVED (superseded
by HEP-CORE-0030's `BAND_BROADCAST_REQ`); the code cleanup was
never executed and now runs.  Additionally the HEP-0007 §12.2.1
fire-and-forget audit's stale row for the retired wire is
removed to eliminate the self-inconsistency.~~

**Evidence dead code, zero production callers:**
- `BrokerRequestComm::send_broadcast()` — no call sites in
  `src/consumer/`, `src/producer/`, `src/processor/`,
  `src/utils/service/`.  Consumers migrated to
  `send_band_broadcast()` per HEP-CORE-0030.
- Handler `handle_channel_broadcast_req` — reachable only via
  dead BRC method.

**Code changes (bundled with C1-C12 atomic commit):**
- Delete `BrokerRequestComm::send_broadcast()` method +
  declaration (`broker_request_comm.hpp`,
  `broker_request_comm.cpp:977`).
- Delete `BrokerServiceImpl::handle_channel_broadcast_req`
  handler function.
- Remove `"CHANNEL_BROADCAST_REQ"` from `is_known_msg_type`
  list (broker_service.cpp:183).
- Remove `else if (msg_type == "CHANNEL_BROADCAST_REQ")`
  dispatch branch (broker_service.cpp:1874-1877).
- Delete dispatch table row in `wire_dispatch.cpp:385`.
- Grep tests + cleanup residual literal usages.

**HEPs needing update:**
- **HEP-CORE-0007 §12.2.1** — remove `CHANNEL_BROADCAST_REQ`
  row from fire-and-forget audit.  Eliminates the
  self-inconsistency vs. §12.4's REMOVED note.
- **HEP-CORE-0007 §12.4** — REMOVED note stays as
  archaeological reference.  Optionally move to a "Retired
  wires" appendix for cleanliness across the doc.
- **HEP-CORE-0030 §5.2** — already correct.

Status: bundled with C1-C12 atomic commit + broker_proto bump.

---

### C12 — RESOLVED: two admission-gate counters (aggregate + per-code); keep `sys.invalid_identifier_rejected` separate; enrich HEP-CORE-0033 §9.4 counter comments across the board

**Decision:** add TWO new wire-layer admission counters,
retain the HubState-level defense-in-depth counter unchanged,
enrich every §9.4 counter row with (when/how-to-interpret/
what-to-check) prose.

**New wire-layer counters (both bumped on every admission-gate
reject):**
- `sys.admission_rejected_total` — aggregate volume trend.
- `sys.admission_rejected_by_code[<reject_code>]` — per-code
  bucketed.  `<reject_code>` drawn from the closed enum in
  `admission_gates.hpp::RejectCode` (identity_mismatch,
  replay_or_skew, unknown_role, pubkey_mismatch,
  key_rotation_required, envelope_tampered,
  body_schema_violation, invalid_request, uid_conflict,
  broker_internal_error).  Cardinality bounded by the enum
  (no cardinality-attack risk per HEP-CORE-0033 §9.3 R1).

**`sys.invalid_identifier_rejected` stays separate.**  Different
concern: HubState-level defense-in-depth backstop.  Bumps only
when the wire layer's own validation MISSED a malformed
identifier — a wire-layer bug signal, not a client-behavior
signal.  In healthy production this counter is near-zero;
non-zero means the broker's wire handler has a validation gap.

Two counters answer different operator questions:
- `sys.admission_rejected_*` moves → *"my clients are
  misbehaving OR under attack"* (normal-under-load).
- `sys.invalid_identifier_rejected` moves → *"my wire layer
  is buggy — malformed input slipped through"* (bug signal).

**HEP-CORE-0033 §9.4 comment enrichment (across the board):**
every counter row gets a paragraph explaining:
- **When**: which code path / conditions trigger the bump.
- **How to interpret**: is it normal traffic, an attack
  signal, a code bug, a config error?
- **What to check**: operator triage steps when the counter
  moves — which log to open, which role_uid to look at, which
  config to verify.

Enrichment example (for `sys.malformed_frame`):
> *"When: envelope parse failed at frame count / control byte
> / oversized payload (S1 stage).  How to interpret: a message
> that never passed the 5-frame skeleton check.  In healthy
> production this is near-zero.  Non-zero means a client is
> producing malformed wire (protocol version drift, bug in a
> custom client, or a middlebox corrupting frames).  What to
> check: is a specific role_uid the source (search recent WARN
> logs for the corresponding envelope-parse rejection message);
> do the messages predate a recent broker_proto bump?"*

**HEPs needing update:**
- **HEP-CORE-0033 §9.4 Operational metrics table** — enrich
  EVERY row with (when/how-to-interpret/what-to-check) prose.
  Add new rows for `sys.admission_rejected_total` +
  `sys.admission_rejected_by_code[<reject_code>]`.
- **HEP-CORE-0033 §9.3** dispatch state machine — add an
  admission-gate stage between S3 and S4 (S3.5 or annotate S3)
  with its own counter-bump on failure.
- **HEP-CORE-0033 §9.4** intro paragraph — distinguish
  wire-layer admission counters (normal-under-load) from
  HubState-level `sys.invalid_identifier_rejected` (wire-layer
  buggy — near-zero in healthy production).

**Code changes (bundled with C1-C12 atomic commit):**
- `hub_state.cpp:1065-1073` (bump_invalid_identifier template)
  — enrich the docstring with the "defense-in-depth backstop;
  near-zero in healthy production; non-zero means wire-layer
  bug" note.  No behavior change.
- `wire_dispatch.cpp` `dispatch_received::RejectedMessage`
  branch — replace single `sys.admission_rejected` bump with:
  ```
  hub_state_->_bump_counter("sys.admission_rejected_total");
  hub_state_->_bump_counter(
      std::string("sys.admission_rejected_by_code.") +
      std::string(admission::to_wire_string(v.code)));
  ```
  (verify HubState `_bump_counter` accepts keyed pattern
  like `msg_type_counts.<msg_type>`; if not, use the
  right typed helper.)
- `hub_state_json.cpp` — verify serialization for the new
  keyed counter pattern.  If not present, add.

Status: bundled with C1-C12 atomic commit + broker_proto bump.
HEP-CORE-0033 §9.4 comment enrichment is a doc-only change;
batch with other HEP updates.

---

