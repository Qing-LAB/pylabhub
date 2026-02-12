/**
 * @file test_slot_protocol.cpp
 * @brief Layer 3 DataHub Phase B – Slot protocol in one process.
 *
 * Tests acquire write, write+commit, acquire consume, read; checksum update/verify;
 * optional diagnostic handle.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class SlotProtocolTest : public IsolatedProcessTest
{
};

TEST_F(SlotProtocolTest, WriteReadSucceedsInProcess)
{
    auto proc = SpawnWorker("slot_protocol.write_read", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(SlotProtocolTest, ChecksumUpdateVerifySucceeds)
{
    auto proc = SpawnWorker("slot_protocol.checksum", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(SlotProtocolTest, LayoutWithChecksumAndFlexibleZoneSucceeds)
{
    auto proc = SpawnWorker("slot_protocol.layout_smoke", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(SlotProtocolTest, DiagnosticHandleOpensAndAccessesHeader)
{
    auto proc = SpawnWorker("slot_protocol.diagnostic_handle", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
