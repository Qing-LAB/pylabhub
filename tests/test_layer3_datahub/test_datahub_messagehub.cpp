/**
 * @file test_datahub_messagehub.cpp
 * @brief Phase C – Messenger unit tests (no broker and with in-process broker).
 *
 * Covers get_instance, lifecycle, and "when not connected" behavior:
 * discover_producer returns nullopt, register_producer is fire-and-forget (no-throw),
 * disconnect is idempotent.
 * With-broker tests (register_producer, discover_producer, create/find + write/read)
 * run against an in-process test broker.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubMessengerTest : public IsolatedProcessTest
{
};

TEST_F(DatahubMessengerTest, LifecycleInitializedFollowsState)
{
    auto proc = SpawnWorker("messagehub.lifecycle_initialized_follows_state", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessengerTest, SendMessageWhenNotConnectedReturnsNullopt)
{
    // Tests that discover_producer returns nullopt when not connected (replaces send_message).
    auto proc = SpawnWorker("messagehub.send_message_when_not_connected_returns_nullopt", {});
    ExpectWorkerOk(proc, {"Messenger"});
}

TEST_F(DatahubMessengerTest, ReceiveMessageWhenNotConnectedReturnsNullopt)
{
    // Tests that discover_producer returns nullopt when not connected (replaces receive_message).
    auto proc = SpawnWorker("messagehub.receive_message_when_not_connected_returns_nullopt", {});
    ExpectWorkerOk(proc, {"Messenger"});
}

TEST_F(DatahubMessengerTest, RegisterProducerWhenNotConnectedReturnsFalse)
{
    // Tests that register_producer (void/fire-and-forget) does not throw when not connected.
    auto proc = SpawnWorker("messagehub.register_producer_when_not_connected_returns_false", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessengerTest, DiscoverProducerWhenNotConnectedReturnsNullopt)
{
    auto proc = SpawnWorker("messagehub.discover_producer_when_not_connected_returns_nullopt", {});
    ExpectWorkerOk(proc, {"Messenger"});
}

TEST_F(DatahubMessengerTest, DisconnectWhenNotConnectedIdempotent)
{
    auto proc = SpawnWorker("messagehub.disconnect_when_not_connected_idempotent", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessengerTest, WithBrokerHappyPath)
{
    auto proc = SpawnWorker("messagehub.with_broker_happy_path", {});
    ExpectWorkerOk(proc, {});
}

// ── B2: Not-connected guard tests ────────────────────────────────────────────

TEST_F(DatahubMessengerTest, QueryChannelSchemaNotConnectedReturnsNullopt)
{
    auto proc = SpawnWorker("messagehub.query_channel_schema_not_connected", {});
    ExpectWorkerOk(proc, {}, {"Messenger"});
}

TEST_F(DatahubMessengerTest, CreateChannelNotConnectedReturnsNullopt)
{
    auto proc = SpawnWorker("messagehub.create_channel_not_connected", {});
    ExpectWorkerOk(proc, {}, {"Messenger"});
}

TEST_F(DatahubMessengerTest, ConnectChannelNotConnectedReturnsNullopt)
{
    auto proc = SpawnWorker("messagehub.connect_channel_not_connected", {});
    ExpectWorkerOk(proc, {}, {"Messenger"});
}

TEST_F(DatahubMessengerTest, HeartbeatNoopWhenNotRunning)
{
    auto proc = SpawnWorker("messagehub.heartbeat_noop_not_running", {});
    ExpectWorkerOk(proc, {});
}

// ── B4: Callback registration tests ─────────────────────────────────────────

TEST_F(DatahubMessengerTest, OnChannelClosingGlobalRegister)
{
    auto proc = SpawnWorker("messagehub.on_channel_closing_global_register", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessengerTest, OnChannelClosingRegisterDeregister)
{
    auto proc = SpawnWorker("messagehub.on_channel_closing_register_deregister", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(DatahubMessengerTest, OnConsumerDiedRegister)
{
    auto proc = SpawnWorker("messagehub.on_consumer_died_register", {});
    ExpectWorkerOk(proc, {});
}
