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
