/**
 * @file test_cpp_exception_safety.cpp
 * @brief RAII layer exception safety tests.
 *
 * Verifies that exceptions thrown inside with_transaction lambdas and
 * ctx.slots() loops are handled correctly by the RAII layer:
 *   - Slots are auto-aborted (not published) when an exception unwinds the stack
 *   - No locks are held after the exception; producer/consumer remain usable
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubExceptionSafetyTest : public IsolatedProcessTest
{
};

TEST_F(DatahubExceptionSafetyTest, ExceptionBeforePublishAbortsWriteSlot)
{
    auto proc = SpawnWorker("exception_safety.exception_before_publish_aborts_write_slot", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubExceptionSafetyTest, ExceptionInWriteTransactionLeavesProducerUsable)
{
    auto proc =
        SpawnWorker("exception_safety.exception_in_write_transaction_leaves_producer_usable", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubExceptionSafetyTest, ExceptionInReadTransactionLeavesConsumerUsable)
{
    auto proc =
        SpawnWorker("exception_safety.exception_in_read_transaction_leaves_consumer_usable", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
