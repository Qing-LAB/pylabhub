/**
 * @file test_transaction_api.cpp
 * @brief Layer 3 DataHub – Transaction API tests (RAII layer).
 *
 * Tests for with_transaction (producer/consumer), WriteTransactionGuard,
 * ReadTransactionGuard, typed write/read, and ctx.slots() non-terminating iterator.
 * Verifies exception safety: when lambdas throw, guards release slots.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubTransactionApiTest : public IsolatedProcessTest
{
};

TEST_F(DatahubTransactionApiTest, WithWriteTransactionSuccess)
{
    auto proc = SpawnWorker("transaction_api.with_write_transaction_success", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(DatahubTransactionApiTest, WithWriteTransactionTimeout)
{
    auto proc = SpawnWorker("transaction_api.with_write_transaction_timeout", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(DatahubTransactionApiTest, WriteTransactionGuardExceptionReleasesSlot)
{
    auto proc = SpawnWorker("transaction_api.WriteTransactionGuard_exception_releases_slot", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(DatahubTransactionApiTest, ReadTransactionGuardExceptionReleasesSlot)
{
    auto proc = SpawnWorker("transaction_api.ReadTransactionGuard_exception_releases_slot", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(DatahubTransactionApiTest, WithTypedWriteReadSucceeds)
{
    auto proc = SpawnWorker("transaction_api.with_typed_write_read_succeeds", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}

TEST_F(DatahubTransactionApiTest, RaiiSlotIteratorRoundtrip)
{
    auto proc = SpawnWorker("transaction_api.raii_slot_iterator_roundtrip", {});
    ExpectWorkerOk(proc, {"[transaction_api]"});
}
