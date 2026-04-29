/**
 * @file test_datahub_broker.cpp
 * @brief Phase C — BrokerService integration tests.
 *
 * Tests the real BrokerService (ChannelRegistry + ROUTER loop) via
 * BrokerRequestComm (for happy paths) and raw ZMQ (for error-path verification).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubBrokerTest : public IsolatedProcessTest
{
};

TEST_F(DatahubBrokerTest, RegDiscHappyPath)
{
    // Full REG/DISC round-trip: BrokerRequestComm → real BrokerService.
    auto proc = SpawnWorker("broker.broker_reg_disc_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, SchemaMismatch)
{
    // Re-register same channel with different schema_hash → broker rejects with Cat1 error.
    // Broker logs LOGGER_ERROR "Cat1 schema mismatch". Positively verify it appeared.
    auto proc = SpawnWorker("broker.broker_schema_mismatch", {});
    ExpectWorkerOk(proc, {}, {"Cat1 schema mismatch", "CHANNEL_ERROR_NOTIFY"});
}

TEST_F(DatahubBrokerTest, ChannelNotFound)
{
    // Discover unknown channel -> BRC returns nullopt (verified inside worker).
    // Under HEP-CORE-0023 three-response DISC_REQ, broker replies with ERROR
    // payload (error_code=CHANNEL_NOT_FOUND); no ERROR log is emitted.
    auto proc = SpawnWorker("broker.broker_channel_not_found", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, DeregHappyPath)
{
    // Register -> discover (found) -> deregister -> discover -> nullopt.
    // Second discover returns CHANNEL_NOT_FOUND via wire; no ERROR log expected.
    auto proc = SpawnWorker("broker.broker_dereg_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, DeregPidMismatch)
{
    // Deregister with wrong pid → NOT_REGISTERED (raw ZMQ); broker logs LOGGER_WARN only.
    // No ERROR-level log expected; use plain ExpectWorkerOk to catch any unexpected ERRORs.
    auto proc = SpawnWorker("broker.broker_dereg_pid_mismatch", {});
    ExpectWorkerOk(proc);
}

// ── HEP-CORE-0034 Phase 3a — schema record + citation wire paths ───────────

TEST_F(DatahubBrokerTest, Sch_RegPathBCreated)
{
    // REG_REQ with `schema_packing` → broker records under
    // (role_uid, schema_id); re-registering same fields succeeds
    // (idempotent at the registry level).
    auto proc = SpawnWorker("broker.broker_sch_record_path_b_created", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_RegHashMismatchSelf)
{
    // Same uid+schema_id, different hash, both with `schema_packing` →
    // SCHEMA_HASH_MISMATCH_SELF (HEP-0034 §10.4).  Broker logs a WARN
    // ("schema record mismatch") only.
    auto proc = SpawnWorker("broker.broker_sch_record_hash_mismatch_self", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_ConsumerCitationMatch)
{
    auto proc = SpawnWorker("broker.broker_sch_consumer_citation_match", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_ConsumerCitationMismatch)
{
    // Wrong expected_packing → SCHEMA_CITATION_REJECTED.
    auto proc = SpawnWorker("broker.broker_sch_consumer_citation_mismatch", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_NoPackingBackwardCompat)
{
    // REG_REQ without schema_packing → legacy path; second REG_REQ with
    // different hash returns SCHEMA_MISMATCH (the OLD code), proving the
    // new HEP-0034 path is opt-in.  Broker emits Cat1 ERROR log on the
    // second REG_REQ — tolerate via expected substrings.
    auto proc = SpawnWorker("broker.broker_sch_no_packing_backward_compat", {});
    ExpectWorkerOk(proc, {}, {"Cat1 schema mismatch", "CHANNEL_ERROR_NOTIFY"});
}

// ── HEP-0034 Phase 3b — SCHEMA_REQ owner+id keying + inbox path-A ──────────

TEST_F(DatahubBrokerTest, Sch_SchemaReq_OwnerIdRoundTrip)
{
    // SCHEMA_REQ with (owner, schema_id) returns the SchemaRecord
    // contents; legacy channel_name form continues to work and now
    // surfaces schema_owner.
    auto proc = SpawnWorker("broker.broker_sch_schema_req_owner_id", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_InboxPathA)
{
    // REG_REQ with inbox metadata creates a SchemaRecord under
    // (role_uid, "inbox"); SCHEMA_REQ for that key returns it.
    auto proc = SpawnWorker("broker.broker_sch_inbox_path_a", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_InboxHashMismatchSelf)
{
    // Same uid, different inbox schema → SCHEMA_HASH_MISMATCH_SELF.
    // Pins HEP-0034 §11.4: inbox record is keyed by role_uid, not by
    // channel name; one inbox per role.
    auto proc = SpawnWorker("broker.broker_sch_inbox_hash_mismatch_self", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_InboxIdempotent)
{
    // Same uid + same inbox schema across two REG_REQs → registry treats
    // second as idempotent; broker accepts both.
    auto proc = SpawnWorker("broker.broker_sch_inbox_idempotent", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_InboxInvalidJson)
{
    // Malformed inbox_schema_json → INBOX_SCHEMA_INVALID before any
    // state is persisted.
    auto proc = SpawnWorker("broker.broker_sch_inbox_invalid_json", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_InboxTwoOwners)
{
    // Two roles each register their own inbox with same field shape →
    // both records exist (HEP-0034 §8 namespace-by-owner).
    auto proc = SpawnWorker("broker.broker_sch_inbox_two_owners", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_SchemaReq_Invalid)
{
    // SCHEMA_REQ with neither (owner+id) nor channel_name → INVALID_REQUEST.
    auto proc = SpawnWorker("broker.broker_sch_schema_req_invalid", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_InboxInvalidPacking)
{
    // inbox_packing must be "aligned" or "packed" — robustness gap caught
    // during Phase 3b self-review.
    auto proc = SpawnWorker("broker.broker_sch_inbox_invalid_packing", {});
    ExpectWorkerOk(proc);
}

// ── Phase 3 follow-up — Stage-2 verification + tightened gates ─────────────

TEST_F(DatahubBrokerTest, Sch_RegMissingPacking)
{
    // REG_REQ with schema_id but no schema_packing → MISSING_PACKING.
    auto proc = SpawnWorker("broker.broker_sch_reg_missing_packing", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_RegFingerprintInconsistent)
{
    // Producer claims a hash that doesn't match its BLDS+packing →
    // FINGERPRINT_INCONSISTENT (Stage-2 verification).
    auto proc = SpawnWorker("broker.broker_sch_reg_fingerprint_inconsistent", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_ConsumerNamed_MissingHash)
{
    // expected_schema_id without expected_schema_hash → NACK.
    auto proc = SpawnWorker("broker.broker_sch_cons_named_missing_hash", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_ConsumerAnonymous_HappyPath)
{
    // Consumer in anonymous mode (full structure, no id) succeeds.
    auto proc = SpawnWorker("broker.broker_sch_cons_anonymous_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_ConsumerAnonymous_MissingPacking)
{
    // Anonymous mode requires both blds and packing.
    auto proc = SpawnWorker("broker.broker_sch_cons_anonymous_missing_packing", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_ConsumerNamed_WithStructureMismatch)
{
    // Defense-in-depth: named citation + structure that doesn't hash
    // to the channel's hash → FINGERPRINT_INCONSISTENT.
    auto proc = SpawnWorker("broker.broker_sch_cons_named_with_structure_mismatch", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_InboxEvictsOnDisconnect)
{
    // Audit-found gap: producer's (uid, "inbox") record evicts on
    // channel close (DEREG_REQ → `_on_channel_closed` cascade).
    auto proc = SpawnWorker("broker.broker_sch_inbox_evicts_on_disconnect", {});
    ExpectWorkerOk(proc);
}

// ── HEP-0034 Phase 4b — hub-globals + path-C adoption ─────────────────────

TEST_F(DatahubBrokerTest, Sch_HubGlobalsLoadedAtStartup)
{
    // Broker startup walks `cfg.schema_search_dirs`, loads each `.json`,
    // and registers it under `(hub, schema_id)` in HubState.schemas.
    // SCHEMA_REQ for that key must succeed.
    auto proc = SpawnWorker("broker.broker_sch_hub_globals_loaded_at_startup", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_PathC_AdoptionSucceeds)
{
    // Producer REG_REQ with schema_owner="hub" + matching fingerprint
    // adopts the pre-loaded global; channel.schema_owner becomes "hub".
    auto proc = SpawnWorker("broker.broker_sch_path_c_adoption_succeeds", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_PathC_FingerprintMismatch)
{
    // Path-C with mismatching fingerprint → FINGERPRINT_INCONSISTENT.
    auto proc = SpawnWorker("broker.broker_sch_path_c_fingerprint_mismatch", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_PathC_UnknownGlobal)
{
    // Path-C citing a hub-global that was never loaded → SCHEMA_UNKNOWN.
    auto proc = SpawnWorker("broker.broker_sch_path_c_unknown_global", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_PathX_ForbiddenOwner)
{
    // schema_owner set to a third role's uid → SCHEMA_FORBIDDEN_OWNER.
    auto proc = SpawnWorker("broker.broker_sch_path_x_forbidden_owner", {});
    ExpectWorkerOk(proc);
}

// ── Wire-fields helpers × broker integration ───────────────────────────────

TEST_F(DatahubBrokerTest, Sch_WireHelpers_RegisterAndCite)
{
    // Named-citation path: helper-built REG_REQ + CONSUMER_REG_REQ accepted
    // by real broker; SCHEMA_REQ round-trip pins helper hash == broker
    // hash; idempotent re-register succeeds.
    auto proc = SpawnWorker("broker.broker_sch_wire_helpers_register_and_cite", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_WireHelpers_AnonymousCitation)
{
    // Helper with non-string slot_schema_json leaves WireSchemaFields::schema_id
    // empty; consumer cites in anonymous mode (HEP-0034 §10.3); broker
    // recomputes hash from full structure and matches the channel.
    auto proc = SpawnWorker("broker.broker_sch_wire_helpers_anonymous_citation", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, Sch_WireHelpers_FlexzoneRoundTrip)
{
    // Slot + flexzone via helpers: helper folds fz into §6.3 canonical form;
    // broker recomputes matching bytes on REG_REQ Stage-2 and on
    // CONSUMER_REG_REQ defense-in-depth.  Pins the Phase 4a flexzone fix.
    auto proc = SpawnWorker("broker.broker_sch_wire_helpers_flexzone_round_trip", {});
    ExpectWorkerOk(proc);
}
