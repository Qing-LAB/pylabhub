/**
 * @file test_message_hub.cpp
 * @brief Phase C – MessageHub unit tests (no broker).
 *
 * Covers get_instance, lifecycle, and "when not connected" behavior:
 * send_message/receive_message return nullopt, register_producer returns false,
 * discover_producer returns nullopt, disconnect is idempotent.
 * With-broker tests (register_producer, discover_producer, create/find + write/read)
 * require a running broker and are documented as future work.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubMessageHubTest : public IsolatedProcessTest
{
};

TEST_F(DatahubMessageHubTest, LifecycleInitializedFollowsState)
{
    auto proc = SpawnWorker("messagehub.lifecycle_initialized_follows_state", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessageHubTest, SendMessageWhenNotConnectedReturnsNullopt)
{
    auto proc = SpawnWorker("messagehub.send_message_when_not_connected_returns_nullopt", {});
    ExpectWorkerOk(proc, {"MessageHub"});
}

TEST_F(DatahubMessageHubTest, ReceiveMessageWhenNotConnectedReturnsNullopt)
{
    auto proc = SpawnWorker("messagehub.receive_message_when_not_connected_returns_nullopt", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessageHubTest, RegisterProducerWhenNotConnectedReturnsFalse)
{
    auto proc = SpawnWorker("messagehub.register_producer_when_not_connected_returns_false", {});
    ExpectWorkerOk(proc, {"MessageHub"});
}

TEST_F(DatahubMessageHubTest, DiscoverProducerWhenNotConnectedReturnsNullopt)
{
    auto proc = SpawnWorker("messagehub.discover_producer_when_not_connected_returns_nullopt", {});
    ExpectWorkerOk(proc, {"MessageHub"});
}

TEST_F(DatahubMessageHubTest, DisconnectWhenNotConnectedIdempotent)
{
    auto proc = SpawnWorker("messagehub.disconnect_when_not_connected_idempotent", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessageHubTest, WithBrokerHappyPath)
{
    auto proc = SpawnWorker("messagehub.with_broker_happy_path", {});
    ExpectWorkerOk(proc, {});
}
