// tests/test_layer3_datahub/test_datahub_header_structure.cpp
//
// SharedMemoryHeader structure tests.
// Verify dual-schema hash fields via DiagnosticHandle::header().
//
// Test class: DatahubHeaderStructureTest
// Worker prefix: "header_structure"

#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubHeaderStructureTest : public IsolatedProcessTest
{
};

TEST_F(DatahubHeaderStructureTest, SchemaHashesPopulatedWithTemplateApi)
{
    auto proc = SpawnWorker("header_structure.schema_hashes_populated_with_template_api", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubHeaderStructureTest, SchemaHashesZeroWithoutSchema)
{
    auto proc = SpawnWorker("header_structure.schema_hashes_zero_without_schema", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubHeaderStructureTest, DifferentTypesProduceDifferentHashes)
{
    auto proc = SpawnWorker("header_structure.different_types_produce_different_hashes", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
