/**
 * @file test_datahub_hub_inbox_queue.cpp
 * @brief Pattern 3 driver — `InboxQueue` + `InboxClient` tests
 *        (Phase 3 Inbox Facility).
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/hub_inbox_queue_workers.cpp`.
 *
 * Each scenario exchanges messages between a real `InboxQueue`
 * (ROUTER receiver) and `InboxClient` (DEALER sender) over TCP
 * loopback with port-0 ephemeral bind.  Test #6 fabricates a
 * malformed frame via raw `zmq_send` to exercise the receiver's
 * drop path.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class InboxQueueTest : public IsolatedProcessTest
{
};

TEST_F(InboxQueueTest, BindAndConnect_Basic)
{
    auto w = SpawnWorker("hub_inbox_queue.bind_and_connect_basic");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, RecvOne_Timeout_ReturnsNull)
{
    auto w = SpawnWorker("hub_inbox_queue.recv_one_timeout_returns_null");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, MultipleMessages)
{
    auto w = SpawnWorker("hub_inbox_queue.multiple_messages");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, DoubleStop_NoThrow)
{
    auto w = SpawnWorker("hub_inbox_queue.double_stop_no_throw");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, SenderUid_IsPreserved)
{
    auto w = SpawnWorker("hub_inbox_queue.sender_uid_is_preserved");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, BadMagic_Drops)
{
    auto w = SpawnWorker("hub_inbox_queue.bad_magic_drops");
    ExpectWorkerOk(w);
}

// Replay defense (HEP-CORE-0027 §3.6): a frame whose (sender, nonce) was
// already seen, and a frame with stale wall-clock skew, are both dropped
// before the handler runs.
TEST_F(InboxQueueTest, ReplayAndSkew_Dropped)
{
    auto w = SpawnWorker("hub_inbox_queue.replay_and_skew_dropped");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, AckCode3_HandlerError)
{
    auto w = SpawnWorker("hub_inbox_queue.ack_code_3_handler_error");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, NotStarted_RecvReturnsNull)
{
    auto w = SpawnWorker("hub_inbox_queue.not_started_recv_returns_null");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, EmptySchema_FactoryFails)
{
    auto w = SpawnWorker("hub_inbox_queue.empty_schema_factory_fails");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, EmptySchema_ClientFactoryFails)
{
    auto w = SpawnWorker("hub_inbox_queue.empty_schema_client_factory_fails");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, ItemSize_MatchesSchema)
{
    auto w = SpawnWorker("hub_inbox_queue.item_size_matches_schema");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, SchemaMismatch_DifferentType_DropsFrame)
{
    auto w = SpawnWorker("hub_inbox_queue.schema_mismatch_different_type_drops_frame");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, SchemaMismatch_DifferentSize_DropsFrame)
{
    auto w = SpawnWorker("hub_inbox_queue.schema_mismatch_different_size_drops_frame");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, ChecksumEnforced_Roundtrip)
{
    auto w = SpawnWorker("hub_inbox_queue.checksum_enforced_roundtrip");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, ChecksumManual_NoStamp_ReceiverRejects)
{
    auto w = SpawnWorker("hub_inbox_queue.checksum_manual_no_stamp_receiver_rejects");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, ChecksumNone_Roundtrip)
{
    auto w = SpawnWorker("hub_inbox_queue.checksum_none_roundtrip");
    ExpectWorkerOk(w);
}

// Inbox CURVE auth (HEP-CORE-0027 §3.5): the ROUTER admits only pubkeys in
// its hub-wide known_roles roster.  Authorized sender delivers; a peer with
// a valid keypair but not in the roster is denied at the ZAP gate.
TEST_F(InboxQueueTest, CurveAuthorizedDelivers)
{
    auto w = SpawnWorker("hub_inbox_queue.inbox_curve_authorized_delivers");
    ExpectWorkerOk(w);
}

TEST_F(InboxQueueTest, CurveUnknownSenderDenied)
{
    auto w = SpawnWorker("hub_inbox_queue.inbox_curve_unknown_denied");
    ExpectWorkerOk(w);
}
