// tests/test_layer3_datahub/test_datahub_config_validation.cpp
//
// DataBlockConfig validation tests.
// Verify that create_datablock_producer_impl throws std::invalid_argument when mandatory
// config fields are unset or out of range, and succeeds with a complete valid config.
//
// Test class: DatahubConfigValidationTest
// Worker prefix: "config_validation"

#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubConfigValidationTest : public IsolatedProcessTest
{
};

TEST_F(DatahubConfigValidationTest, PolicyUnsetThrows)
{
    auto proc = SpawnWorker("config_validation.policy_unset_throws", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubConfigValidationTest, ConsumerSyncPolicyUnsetThrows)
{
    auto proc = SpawnWorker("config_validation.consumer_sync_policy_unset_throws", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubConfigValidationTest, PhysicalPageSizeUnsetThrows)
{
    auto proc = SpawnWorker("config_validation.physical_page_size_unset_throws", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubConfigValidationTest, RingBufferCapacityZeroThrows)
{
    auto proc = SpawnWorker("config_validation.ring_buffer_capacity_zero_throws", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubConfigValidationTest, ValidConfigCreatesSuccessfully)
{
    auto proc = SpawnWorker("config_validation.valid_config_creates_successfully", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
