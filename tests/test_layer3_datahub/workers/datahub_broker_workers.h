#pragma once
// tests/test_layer3_datahub/workers/datahub_broker_workers.h
// Phase C — BrokerService integration test worker declarations.

namespace pylabhub::tests::worker::broker
{

/** Register a channel via Messenger, discover it back — full REG/DISC round-trip. */
int broker_reg_disc_happy_path();

/** Re-register same channel with different schema_hash → broker replies SCHEMA_MISMATCH. */
int broker_schema_mismatch();

/** Discover a channel that was never registered → Messenger returns nullopt. */
int broker_channel_not_found();

/** Register, deregister (correct pid), discover → nullopt (CHANNEL_NOT_FOUND). */
int broker_dereg_happy_path();

/** Deregister with wrong pid → broker replies NOT_REGISTERED; channel still discoverable. */
int broker_dereg_pid_mismatch();

// ── HEP-CORE-0034 Phase 3a — schema record + citation wire-protocol paths ───

/** REG_REQ with schema_packing → broker creates schema record (path B);
 *  re-register with same fields succeeds (idempotent at registry layer). */
int broker_sch_record_path_b_created();

/** Two REG_REQs from same uid+schema_id with different hashes (both with
 *  schema_packing set) → second returns SCHEMA_HASH_MISMATCH_SELF. */
int broker_sch_record_hash_mismatch_self();

/** Producer with schema_packing + consumer with matching expected_packing →
 *  CONSUMER_REG_REQ succeeds (citation validates). */
int broker_sch_consumer_citation_match();

/** Producer with schema_packing + consumer with WRONG expected_packing →
 *  CONSUMER_REG_REQ returns SCHEMA_CITATION_REJECTED. */
int broker_sch_consumer_citation_mismatch();

/** REG_REQ without schema_packing field → no schema record created
 *  (legacy/anonymous behaviour preserved; broker still uses the older
 *  SCHEMA_MISMATCH path on hash conflict, not SCHEMA_HASH_MISMATCH_SELF). */
int broker_sch_no_packing_backward_compat();

// ── HEP-CORE-0034 Phase 3b — SCHEMA_REQ owner+id + inbox path-A ─────────────

/** SCHEMA_REQ with (owner, schema_id) returns the SchemaRecord (hash,
 *  packing, blds); legacy `channel_name` form still works alongside. */
int broker_sch_schema_req_owner_id();

/** REG_REQ with inbox metadata creates a schema record under
 *  (role_uid, "inbox"); SCHEMA_REQ for that key returns the record. */
int broker_sch_inbox_path_a();

/** Two REG_REQs from same uid with different inbox schemas →
 *  SCHEMA_HASH_MISMATCH_SELF on the second.  Pins HEP-0034 §11.4
 *  "schemas live with the role" semantics for inbox. */
int broker_sch_inbox_hash_mismatch_self();

/** Same uid, same inbox schema → idempotent (second REG_REQ succeeds). */
int broker_sch_inbox_idempotent();

/** Malformed `inbox_schema_json` (not JSON, or not an array) → broker
 *  returns INBOX_SCHEMA_INVALID before persisting any state. */
int broker_sch_inbox_invalid_json();

/** Two different roles each register their own inbox → both records
 *  exist independently (HEP-0034 §8 namespace-by-owner applied to inbox). */
int broker_sch_inbox_two_owners();

/** SCHEMA_REQ with neither (owner+schema_id) nor channel_name →
 *  INVALID_REQUEST. */
int broker_sch_schema_req_invalid();

/** REG_REQ with `inbox_packing` that is neither "aligned" nor "packed"
 *  → INVALID_INBOX_PACKING. */
int broker_sch_inbox_invalid_packing();

// ── HEP-0034 Phase 3 follow-up — Stage-2 verification + tightened gates ────

/** REG_REQ with schema_id but missing schema_packing → MISSING_PACKING. */
int broker_sch_reg_missing_packing();

/** REG_REQ with schema_id but schema_blds doesn't hash to schema_hash
 *  → FINGERPRINT_INCONSISTENT (Stage-2). */
int broker_sch_reg_fingerprint_inconsistent();

/** CONSUMER_REG_REQ with expected_schema_id but no expected_schema_hash
 *  → MISSING_HASH_FOR_NAMED_CITATION. */
int broker_sch_cons_named_missing_hash();

/** CONSUMER_REG_REQ anonymous mode (no id) with full structure +
 *  matching hash → success.  Pins the new anonymous-citation path. */
int broker_sch_cons_anonymous_happy_path();

/** CONSUMER_REG_REQ anonymous: blds present but packing missing →
 *  MISSING_PACKING_FOR_ANONYMOUS_CITATION. */
int broker_sch_cons_anonymous_missing_packing();

/** CONSUMER_REG_REQ named-mode WITH consumer-supplied structure that
 *  doesn't hash to the channel's hash → FINGERPRINT_INCONSISTENT
 *  (defense-in-depth catches consumer-local blds drift). */
int broker_sch_cons_named_with_structure_mismatch();

/** Producer with inbox metadata; on role disconnect (heartbeat
 *  timeout), the (uid, "inbox") schema record evicts atomically.
 *  Audit-found gap. */
int broker_sch_inbox_evicts_on_disconnect();

// ── HEP-0034 Phase 4b — hub-globals + path-C adoption ──────────────────────

/** Broker startup loads schemas from `cfg.schema_search_dirs` and
 *  registers each as `(hub, schema_id)` in HubState.schemas. */
int broker_sch_hub_globals_loaded_at_startup();

/** Producer REG_REQ with `schema_owner="hub"` and matching fingerprint
 *  adopts a pre-loaded hub-global; channel.schema_owner becomes "hub"
 *  (not the role uid). */
int broker_sch_path_c_adoption_succeeds();

/** Path-C with mismatching fingerprint → FINGERPRINT_INCONSISTENT. */
int broker_sch_path_c_fingerprint_mismatch();

/** Path-C citing a hub-global that was never loaded → SCHEMA_UNKNOWN. */
int broker_sch_path_c_unknown_global();

/** REG_REQ with `schema_owner` set to a third role's uid (not self,
 *  not "hub") → SCHEMA_FORBIDDEN_OWNER. */
int broker_sch_path_x_forbidden_owner();

// ── Wire-fields helpers × broker integration tests ──────────────────────────
//
// These three workers verify that JSON payloads built by the production
// wire-fields helpers (`make_wire_schema_fields` + `apply_producer_schema_fields`
// / `apply_consumer_schema_fields` in `schema_utils.hpp`) are accepted by a
// real `BrokerService` and round-trip through SCHEMA_REQ with the
// helper-emitted fingerprint matching the broker-stored fingerprint.

/** Named-citation path: producer REG_REQ + consumer CONSUMER_REG_REQ both
 *  built by the helpers; SCHEMA_REQ pins helper hash == broker hash;
 *  idempotent re-register succeeds.  Pins HEP-CORE-0034 §6.3 + §10.1/10.2
 *  + §2.4 I6 end-to-end (slot only). */
int broker_sch_wire_helpers_register_and_cite();

/** Anonymous-citation path: producer registers under named mode; consumer
 *  uses the helper with a non-string `slot_schema_json` so `WireSchemaFields::schema_id`
 *  stays empty while structure fields populate.  Pins the
 *  `is_string()` mode-selection seam in `make_wire_schema_fields` and
 *  HEP-0034 §10.3 broker anonymous-mode dispatch. */
int broker_sch_wire_helpers_anonymous_citation();

/** Slot + flexzone round-trip via helpers: producer's REG_REQ carries
 *  `flexzone_blds` / `flexzone_packing` in addition to the slot fields;
 *  helper folds both into the §6.3 canonical hash; broker recomputes
 *  matching bytes; consumer cites with full slot+fz structure and
 *  defense-in-depth recompute on the broker side includes the fz
 *  fields.  Pins the Phase 4a flexzone-in-canonical-form fix and its
 *  Phase 5a consumer-side mirror. */
int broker_sch_wire_helpers_flexzone_round_trip();

} // namespace pylabhub::tests::worker::broker
