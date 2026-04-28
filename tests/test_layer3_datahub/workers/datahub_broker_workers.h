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

} // namespace pylabhub::tests::worker::broker
