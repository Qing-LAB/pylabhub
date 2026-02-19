/**
 * @file test_datahub_channel.cpp
 * @brief Phase 6 — ChannelHandle integration tests.
 *
 * Tests create_channel / connect_channel for Pipeline and PubSub patterns.
 * Each test runs in an isolated child process with a real BrokerService.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubChannelTest : public IsolatedProcessTest
{
};

TEST_F(DatahubChannelTest, CreateNotConnectedReturnsNullopt)
{
    // Verify create_channel returns nullopt when Messenger has no broker connection.
    auto proc = SpawnWorker("channel.create_not_connected", {});
    ExpectWorkerOk(proc, {"Messenger"});
}

TEST_F(DatahubChannelTest, ConnectNotFoundReturnsNullopt)
{
    // Verify connect_channel returns nullopt when the channel has never been registered.
    auto proc = SpawnWorker("channel.connect_not_found", {});
    ExpectWorkerOk(proc, {"Messenger"});
}

TEST_F(DatahubChannelTest, PipelineDataExchange)
{
    // Producer create_channel(Pipeline) + consumer connect_channel + send/recv round-trip.
    auto proc = SpawnWorker("channel.pipeline_exchange", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubChannelTest, PubSubDataExchange)
{
    // Producer create_channel(PubSub) + consumer connect_channel + send/recv round-trip.
    // Producer retries until subscription propagates (no fixed sleep).
    auto proc = SpawnWorker("channel.pubsub_exchange", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubChannelTest, ChannelHandleIntrospection)
{
    // Verify channel_name(), pattern(), has_shm(), is_valid(), invalidate(), move semantics.
    auto proc = SpawnWorker("channel.channel_introspection", {});
    ExpectWorkerOk(proc);
}
