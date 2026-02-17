// tests/test_layer3_datahub/test_datahub_c_api_validation.cpp
//
// C API validation tests.
// Verify datablock_validate_integrity, datablock_get_metrics, and
// datablock_diagnose_slot/datablock_diagnose_all_slots return correct results
// on fresh datablocks and for non-existent names.
//
// Test class: DatahubCApiValidationTest
// Worker prefix: "c_api_validation"

#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubCApiValidationTest : public IsolatedProcessTest
{
};

TEST_F(DatahubCApiValidationTest, ValidateIntegrityOnFreshDatablock)
{
    auto proc = SpawnWorker("c_api_validation.validate_integrity_on_fresh_datablock", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiValidationTest, ValidateIntegrityNonexistentFails)
{
    auto proc = SpawnWorker("c_api_validation.validate_integrity_nonexistent_fails", {});
    // Intentionally triggers a logger ERROR (open fails on nonexistent name).
    ExpectWorkerOk(proc, {}, true);
}

TEST_F(DatahubCApiValidationTest, GetMetricsFreshHasZeroCommits)
{
    auto proc = SpawnWorker("c_api_validation.get_metrics_fresh_has_zero_commits", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiValidationTest, DiagnoseSlotFreshIsFree)
{
    auto proc = SpawnWorker("c_api_validation.diagnose_slot_fresh_is_free", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiValidationTest, DiagnoseAllSlotsReturnsCapacity)
{
    auto proc = SpawnWorker("c_api_validation.diagnose_all_slots_returns_capacity", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
