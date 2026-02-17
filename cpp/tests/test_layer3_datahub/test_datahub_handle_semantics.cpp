/**
 * @file test_cpp_handle_semantics.cpp
 * @brief C++ handle move-semantics and lifecycle tests.
 *
 * Verifies that DataBlockProducer, DataBlockConsumer, SlotWriteHandle, and
 * SlotConsumeHandle implement correct move ownership transfer and that
 * moved-from objects are safely inert (no UB, no double-release).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubHandleSemanticsTest : public IsolatedProcessTest
{
};

TEST_F(DatahubHandleSemanticsTest, MoveProducerTransfersOwnership)
{
    auto proc = SpawnWorker("handle_semantics.move_producer_transfers_ownership", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubHandleSemanticsTest, MoveConsumerTransfersOwnership)
{
    auto proc = SpawnWorker("handle_semantics.move_consumer_transfers_ownership", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubHandleSemanticsTest, DefaultConstructedHandlesAreInvalid)
{
    auto proc = SpawnWorker("handle_semantics.default_constructed_handles_are_invalid", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
