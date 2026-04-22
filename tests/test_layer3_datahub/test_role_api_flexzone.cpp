/**
 * @file test_role_api_flexzone.cpp
 * @brief L3 role-API integration — flexzone round-trip through RoleAPIBase.
 *
 * Subject: `RoleAPIBase::build_tx_queue` / `build_rx_queue` +
 * `api.flexzone(ChannelSide)` integration.  Verifies that the Tx and Rx
 * sides of a SHM-backed channel share one physical flexzone region,
 * that both sides can read/write it (HEP-CORE-0002 TABLE 1
 * bidirectional user-managed region), and that the ZMQ-transport code
 * path returns nullptr for flexzone (no SHM backing).
 *
 * Integration-level: real `RoleAPIBase` instances on both sides, real
 * SHM, real typed-view caching through `api.flexzone()`.  Not a unit
 * test of any one method — exercises the producer-consumer contract
 * end-to-end at the role-API boundary.
 *
 * ## Lifecycle pattern
 *
 * Pattern 3: each TEST_F spawns a subprocess that owns its own
 * `LifecycleGuard` via `run_gtest_worker`.  Worker bodies live in
 * `workers/role_api_flexzone_workers.cpp`.
 */
#include "test_patterns.h"
#include "test_process_utils.h"

#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class RoleApiFlexzoneTest : public IsolatedProcessTest
{
};

TEST_F(RoleApiFlexzoneTest, ShmRoundTrip)
{
    auto w = SpawnWorker("role_api_flexzone.shm_roundtrip", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiFlexzoneTest, ZmqTxNull)
{
    auto w = SpawnWorker("role_api_flexzone.zmq_tx_null", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiFlexzoneTest, ZmqRxNull)
{
    auto w = SpawnWorker("role_api_flexzone.zmq_rx_null", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiFlexzoneTest, ShmChecksumRoundTrip)
{
    auto w = SpawnWorker("role_api_flexzone.shm_checksum_roundtrip", {});
    ExpectWorkerOk(w);
}

// ── Multi-field payload coverage ─────────────────────────────────────────────
// These tests pin the field-level round-trip contract: producer writes
// distinctive values at layout-computed offsets, consumer reads each
// field back bit-exact.  Catches offset drift, alignment bugs, and
// per-type size mistakes that simple_schema (1 field, 4 bytes) cannot.

TEST_F(RoleApiFlexzoneTest, ShmRoundTrip_PaddingSensitive)
{
    auto w = SpawnWorker("role_api_flexzone.shm_roundtrip_padding_sensitive", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiFlexzoneTest, ShmRoundTrip_AllTypes)
{
    auto w = SpawnWorker("role_api_flexzone.shm_roundtrip_all_types", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiFlexzoneTest, ShmRoundTrip_ArrayField)
{
    auto w = SpawnWorker("role_api_flexzone.shm_roundtrip_array_field", {});
    ExpectWorkerOk(w);
}

// ── Negative paths — prove the error surface is live ────────────────────────
// Positive-path tests above can't catch bugs where a validation path is a
// no-op.  These tests exercise failure modes so that, e.g., a regression
// that removed secret-verification from ShmQueue::create_reader would be
// caught here rather than shipping.

TEST_F(RoleApiFlexzoneTest, ShmConsumer_WrongSecret_Rejected)
{
    auto w = SpawnWorker(
        "role_api_flexzone.shm_consumer_wrong_secret_rejected", {});
    // find_datablock_consumer_impl emits a WARN on secret mismatch
    // (symmetrical with WriteAttach).  Pin the substring so a
    // regression that either stopped checking the secret OR removed
    // the log would fail this test.  Note: WARN-level logs don't
    // trigger ExpectWorkerOk's "unexpected ERROR" guard, so pinning
    // must be done via required_substrings (matched against stderr).
    ExpectWorkerOk(w, /*required_substrings=*/{"shared_secret mismatch"});
}

// Schema-mismatch at the ShmQueue layer alone does NOT reject (no
// broker present to run the schema-hash cross-check — that validation
// happens at channel registration, not at queue attach).  A meaningful
// schema-mismatch negative test needs a broker in scope; that belongs
// in the hub/broker test tier, not here.  This test is intentionally
// absent at the role-API / ShmQueue level.

TEST_F(RoleApiFlexzoneTest, ShmConsumer_Nonexistent_Rejected)
{
    auto w = SpawnWorker(
        "role_api_flexzone.shm_consumer_nonexistent_rejected", {});
    // Attachment failure emits an ERROR log — pin the message so a
    // regression that silently succeeded on nonexistent SHM would lose
    // the log and fail ExpectWorkerOk.
    ExpectWorkerOk(w, /*required_substrings=*/{},
                   /*expected_error_substrings=*/{"attachment failed"});
}

TEST_F(RoleApiFlexzoneTest, ShmSlotChecksum_Corrupt_Detected)
{
    auto w = SpawnWorker(
        "role_api_flexzone.shm_slot_checksum_corrupt_detected", {});
    // Corrupted slot emits "[ShmQueue] slot checksum error on slot ..."
    // — pinning the substring proves the verify path actually fired.
    // If the verify were a no-op, this log would not appear and
    // ExpectWorkerOk would reject the worker output.
    ExpectWorkerOk(w, /*required_substrings=*/{},
                   /*expected_error_substrings=*/{"slot checksum error"});
}

TEST_F(RoleApiFlexzoneTest, ShmFlexzoneChecksum_Corrupt_Detected)
{
    auto w = SpawnWorker(
        "role_api_flexzone.shm_flexzone_checksum_corrupt_detected", {});
    // Corrupted flexzone emits "[ShmQueue] flexzone checksum error on slot
    // ..." — a distinct log line from the slot-checksum path.  Pinning
    // the substring proves the flexzone verify path (not the slot path)
    // is the one that fired.  Without this test, a regression that made
    // the flexzone verify go no-op would pass shm_checksum_roundtrip.
    ExpectWorkerOk(w, /*required_substrings=*/{},
                   /*expected_error_substrings=*/{"flexzone checksum error"});
}
