/**
 * @file test_datahub_write_attach.cpp
 * @brief Tests for DataBlockOpenMode::WriteAttach — broker-owned shared memory model.
 *
 * Verifies that a source process can attach R/W to a hub-created DataBlock segment,
 * write data, and that security/schema validation on the WriteAttach path rejects
 * mismatches.  Also verifies that WriteAttach does NOT unlink the segment on destruction
 * (only the Creator/hub owner unlinks).
 *
 * See plan: docs/tech_draft/ and CLAUDE.md §"DataBlock Three-Mode Constructor".
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubWriteAttachTest : public IsolatedProcessTest
{
};

// ─── Basic write-attach roundtrip ─────────────────────────────────────────────

TEST_F(DatahubWriteAttachTest, CreatorThenWriterAttachBasic)
{
    auto proc = SpawnWorker("write_attach.creator_then_writer_attach_basic", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── Validation: wrong shared_secret ─────────────────────────────────────────

TEST_F(DatahubWriteAttachTest, WriterAttachValidatesSecret)
{
    auto proc = SpawnWorker("write_attach.writer_attach_validates_secret", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── Validation: schema hash mismatch ────────────────────────────────────────

TEST_F(DatahubWriteAttachTest, WriterAttachValidatesSchema)
{
    auto proc = SpawnWorker("write_attach.writer_attach_validates_schema", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── Segment lifetime: persists after writer detaches ─────────────────────────

TEST_F(DatahubWriteAttachTest, SegmentPersistsAfterWriterDetach)
{
    auto proc = SpawnWorker("write_attach.segment_persists_after_writer_detach", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
