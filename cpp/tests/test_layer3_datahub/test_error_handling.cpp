/**
 * @file test_error_handling.cpp
 * @brief DataBlock/slot error-handling tests.
 *
 * Verifies that recoverable error paths return false, nullptr, or empty
 * instead of leading to undefined behavior or segfault. Tests reflect real
 * situations: timeout waiting for slot, wrong secret, invalid/moved-from
 * handles, and bounds violations. Unsafe/unrecoverable situations (e.g.
 * use-after-free by destroying producer while a handle is still in use) are
 * documented as contract violations; these tests focus on logical handling
 * of expected failure modes.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DataBlockErrorHandlingTest : public IsolatedProcessTest
{
};

TEST_F(DataBlockErrorHandlingTest, AcquireConsumeSlotTimeoutReturnsNull)
{
    auto proc = SpawnWorker("error_handling.acquire_consume_slot_timeout_returns_null", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, FindConsumerWrongSecretReturnsNull)
{
    auto proc = SpawnWorker("error_handling.find_consumer_wrong_secret_returns_null", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, ReleaseWriteSlotInvalidHandleReturnsFalse)
{
    auto proc = SpawnWorker("error_handling.release_write_slot_invalid_handle_returns_false", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, ReleaseConsumeSlotInvalidHandleReturnsFalse)
{
    auto proc = SpawnWorker("error_handling.release_consume_slot_invalid_handle_returns_false", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, WriteBoundsReturnFalse)
{
    auto proc = SpawnWorker("error_handling.write_bounds_return_false", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, CommitBoundsReturnFalse)
{
    auto proc = SpawnWorker("error_handling.commit_bounds_return_false", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, ReadBoundsReturnFalse)
{
    auto proc = SpawnWorker("error_handling.read_bounds_return_false", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, DoubleReleaseWriteSlotIdempotent)
{
    auto proc = SpawnWorker("error_handling.double_release_write_slot_idempotent", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DataBlockErrorHandlingTest, SlotIteratorTryNextTimeoutReturnsNotOk)
{
    auto proc = SpawnWorker("error_handling.slot_iterator_try_next_timeout_returns_not_ok", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
