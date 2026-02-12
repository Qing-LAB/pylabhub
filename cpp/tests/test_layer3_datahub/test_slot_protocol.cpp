/**
 * @file test_slot_protocol.cpp
 * @brief Layer 3 DataHub Phase B – Slot protocol tests (in-process and cross-process).
 *
 * Test design:
 * - **In-process (single worker):** Most tests spawn one worker process that runs both
 *   producer and consumer logic in separate threads *in the same process*. That validates
 *   slot protocol, locking, and API correctness without cross-process IPC. Shared memory
 *   and SharedSpinLock are still used, but both sides share the same process.
 * - **Cross-process (two workers):** Real producer/consumer use is two separate processes.
 *   We must test that path explicitly. Currently:
 *   - CrossProcessDataExchangeWriterThenReaderVerifiesContent: writer process creates,
 *     writes, sleeps; reader process attaches and reads (one exchange).
 *   - ZombieWriterRecovery: two processes (zombie writer, then reclaimer).
 *   Additional multi-process tests (e.g. high load with producer in one process and
 *   consumer in another, writer-blocks-on-reader across processes) should be added to
 *   cover real IPC and cross-process locking; see DATAHUB_AND_MESSAGEHUB_TEST_PLAN Phase D.
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

// --- In-process tests (one worker: producer and consumer threads in same process) ---

TEST_F(SlotProtocolTest, WriteReadSucceedsInProcess)
{
    auto proc = SpawnWorker("slot_protocol.write_read", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(SlotProtocolTest, StructuredSlotDataPasses)
{
    auto proc = SpawnWorker("slot_protocol.structured_slot_data_passes", {});
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

TEST_F(SlotProtocolTest, RingBufferIterationContentVerified)
{
    auto proc = SpawnWorker("slot_protocol.ring_buffer_iteration", {});
    ExpectWorkerOk(proc, {"SlotTest:Producer", "SlotTest:Consumer", "lap1", "lap2"});
    std::string_view stderr_out = proc.get_stderr();
    EXPECT_GE(count_lines(stderr_out, "SlotTest:Producer"), 2u) << "producer lap1+lap2 logs";
    EXPECT_GE(count_lines(stderr_out, "SlotTest:Consumer"), 2u) << "consumer lap1+lap2 logs";
    EXPECT_EQ(count_lines(stderr_out, "lap1"), 2u) << "lap1 producer and consumer";
    EXPECT_EQ(count_lines(stderr_out, "lap2"), 2u) << "lap2 producer and consumer";
}

TEST_F(SlotProtocolTest, WriterBlocksOnReaderThenUnblocks)
{
    auto proc = SpawnWorker("slot_protocol.writer_blocks_on_reader_then_unblocks", {});
    ExpectWorkerOk(proc,
                   {"SlotTest:Producer", "SlotTest:Consumer", "timeout (reader holds)",
                    "ok after reader released"});
    std::string_view stderr_out = proc.get_stderr();
    EXPECT_GE(count_lines(stderr_out, "SlotTest:Producer"), 4u)
        << "producer: first write, timeout, ok after release, second write";
    EXPECT_GE(count_lines(stderr_out, "SlotTest:Consumer"), 2u) << "consumer: acquired, released";
    EXPECT_EQ(count_lines(stderr_out, "timeout (reader holds)"), 1u) << "exactly one timeout";
    EXPECT_EQ(count_lines(stderr_out, "ok after reader released"), 1u) << "exactly one unblock";
}

TEST_F(SlotProtocolTest, DiagnosticHandleOpensAndAccessesHeader)
{
    auto proc = SpawnWorker("slot_protocol.diagnostic_handle", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(SlotProtocolTest, HighContentionWrapAround)
{
    // Ring capacity 1: reader holds slot; writer blocks until reader releases
    auto proc = SpawnWorker("slot_protocol.high_contention_wrap_around", {});
    ExpectWorkerOk(proc,
                  {"SlotTest:Producer", "SlotTest:Consumer", "writer blocked", "writer unblocked"});
    std::string_view stderr_out = proc.get_stderr();
    EXPECT_GE(count_lines(stderr_out, "R1 acquired"), 1u);
    EXPECT_GE(count_lines(stderr_out, "R1 released"), 1u);
    EXPECT_GE(count_lines(stderr_out, "writer blocked"), 1u);
    EXPECT_GE(count_lines(stderr_out, "writer unblocked"), 1u);
}

TEST_F(SlotProtocolTest, ZombieWriterRecovery)
{
#if !PYLABHUB_IS_POSIX
    GTEST_SKIP() << "Zombie writer recovery tested on POSIX only";
#else
    std::string channel = make_test_channel_name("ZombieWriter");
    auto zombie = SpawnWorker("slot_protocol.zombie_writer_acquire_then_exit", {channel});
    zombie.wait_for_exit();
    EXPECT_EQ(zombie.exit_code(), 0) << "Zombie worker exits 0 (_exit)";

    auto reclaimer = SpawnWorker("slot_protocol.zombie_writer_reclaimer", {channel});
    ExpectWorkerOk(reclaimer, {"SlotTest:Producer", "zombie writer reclaimed"});
#endif
}

TEST_F(SlotProtocolTest, ConsumerSyncPolicyLatestOnly)
{
    auto proc = SpawnWorker("slot_protocol.policy_latest_only", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(SlotProtocolTest, ConsumerSyncPolicySingleReader)
{
    auto proc = SpawnWorker("slot_protocol.policy_single_reader", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(SlotProtocolTest, ConsumerSyncPolicySyncReader)
{
    auto proc = SpawnWorker("slot_protocol.policy_sync_reader", {});
    ExpectWorkerOk(proc, {});
}

TEST_F(SlotProtocolTest, HighLoadSingleReaderIntegrity)
{
    auto proc = SpawnWorker("slot_protocol.high_load_single_reader", {});
    ExpectWorkerOk(proc, {"[SlotTest:HighLoadSingleReader] ok"});
}

TEST_F(SlotProtocolTest, WriterTimeoutMetricsSplit)
{
    auto proc = SpawnWorker("slot_protocol.writer_timeout_metrics_split", {});
    // Worker intentionally triggers write_lock and reader-drain timeouts; DataBlock logs those at ERROR.
    ExpectWorkerOk(proc, {}, /*allow_expected_logger_errors=*/true);
}

// --- Cross-process tests (real IPC: producer and consumer in separate processes) ---

TEST_F(SlotProtocolTest, CrossProcessDataExchangeWriterThenReaderVerifiesContent)
{
    // Verifies offset, format, and that both processes see the same data:
    // writer and reader run in separate processes; writer creates and writes, reader attaches and reads.
    // Writer sleeps so shm persists until reader attaches (producer must stay alive).
    std::string channel = make_test_channel_name("CrossProcess");
    auto workers = SpawnWorkers({
        {"slot_protocol.cross_process_writer", {channel}},
        {"slot_protocol.cross_process_reader", {channel}},
    });
    for (auto &w : workers)
        w.wait_for_exit();
    auto writer_it = workers.begin();
    auto reader_it = std::next(writer_it);
    pylabhub::tests::helper::expect_worker_ok(*writer_it,
                                             {"SlotTest:Producer", "cross-process write committed ok"});
    pylabhub::tests::helper::expect_worker_ok(*reader_it,
                                             {"SlotTest:Consumer", "cross-process read ok"});
    std::string_view writer_stderr = writer_it->get_stderr();
    std::string_view reader_stderr = reader_it->get_stderr();
    EXPECT_GE(count_lines(writer_stderr, "SlotTest:Producer"), 2u)
        << "producer: acquired and committed ok";
    EXPECT_EQ(count_lines(writer_stderr, "cross-process write committed ok"), 1u);
    EXPECT_GE(count_lines(reader_stderr, "SlotTest:Consumer"), 2u)
        << "consumer: acquired and read ok";
    EXPECT_EQ(count_lines(reader_stderr, "cross-process read ok"), 1u);

    cleanup_test_datablock(channel); // idempotent if reader already cleaned up
}
