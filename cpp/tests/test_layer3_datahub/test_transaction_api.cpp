/**
 * @file test_transaction_api.cpp
 * @brief Layer 3 DataHub – Transaction API tests (Phase 2.3).
 *
 * Tests for with_write_transaction, with_read_transaction, WriteTransactionGuard,
 * ReadTransactionGuard, with_typed_write/with_typed_read, and with_next_slot.
 * Verifies exception safety: when lambdas throw, guards release slots.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class TransactionAPITest : public IsolatedProcessTest
{
};

TEST_F(TransactionAPITest, WithWriteTransactionSuccess)
{
    auto proc = SpawnWorker("transaction_api.with_write_transaction_success", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(TransactionAPITest, WithWriteTransactionTimeout)
{
    auto proc = SpawnWorker("transaction_api.with_write_transaction_timeout", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(TransactionAPITest, WriteTransactionGuardExceptionReleasesSlot)
{
    auto proc = SpawnWorker("transaction_api.WriteTransactionGuard_exception_releases_slot", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(TransactionAPITest, ReadTransactionGuardExceptionReleasesSlot)
{
    auto proc = SpawnWorker("transaction_api.ReadTransactionGuard_exception_releases_slot", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(TransactionAPITest, WithTypedWriteReadSucceeds)
{
    auto proc = SpawnWorker("transaction_api.with_typed_write_read_succeeds", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(TransactionAPITest, WithNextSlotIterator)
{
    auto proc = SpawnWorker("transaction_api.with_next_slot_iterator", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}
