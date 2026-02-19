/**
 * @file test_datahub_e2e.cpp
 * @brief End-to-end multi-process integration test.
 *
 * Spawns a real producer process and a real consumer process that exchange live data
 * through a real in-thread BrokerService. This is the final validation of the full
 * producer → broker → consumer pipeline.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubE2ETest : public IsolatedProcessTest
{
};

TEST_F(DatahubE2ETest, ProducerToConsumerViaRealBroker)
{
    // The orchestrator worker starts the broker in-thread, then spawns a producer
    // and consumer subprocess. Producer writes 5 slots; consumer discovers the channel,
    // attaches to the DataBlock, reads the latest slot, and verifies the data.
    auto proc = SpawnWorker("e2e.orchestrator", {});
    ExpectWorkerOk(proc);
}
