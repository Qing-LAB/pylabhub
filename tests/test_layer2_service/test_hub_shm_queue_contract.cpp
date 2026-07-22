/**
 * @file test_hub_shm_queue_contract.cpp
 * @brief L2 contract tests for ShmQueue behaviors that survived the
 *        HEP-CORE-0041 1i-cleanup S3c retirement of the L3
 *        `test_datahub_hub_queue.cpp` suite (task #313).
 *
 * The S3c retirement claimed coverage was "redundant with
 * `test_datahub_schema_validation.cpp`" — VERIFIED FALSE during task
 * #246 review-C follow-ups (schema_validation covers schema hashes,
 * not slot_valid/flexzone/remap contracts).  Task #323 files this
 * L2 revival to close the gap; the four contracts below are the
 * concrete unpinned behaviors the reviewer flagged:
 *
 *   1. `ShmQueueNoFlexzone`               — flexzone()/flexzone_size()
 *      return {nullptr, 0} when fz_schema is empty.  Prevents a
 *      regression where an accidental default flexzone allocation
 *      exposes a mutable window scripts don't expect.
 *
 *   2. `ShmQueueFlexzoneBidirectional`    — flexzone is one shared
 *      region per channel: writer-side and reader-side pointers
 *      resolve to the same physical mmap window; writes from EITHER
 *      side are visible from the other (HEP-CORE-0002 §2.2).
 *
 *   3. `ShmQueueVerifyChecksumMismatch`   — read_acquire returns
 *      nullptr when the writer's slot has slot_valid=0 (no checksum
 *      stamped) and the reader has `set_verify_checksum(true, false)`.
 *      Pins the slot-gate contract that a corrupted/uninitialized
 *      slot cannot leak to a checksum-enforcing reader.
 *
 *   4. `DataBlockProducerRemapStubsThrow` — `request_structure_remap`
 *      and `commit_structure_remap` always throw
 *      `std::runtime_error` with a stable substring of the function
 *      name.  Pins the "explicit not-implemented" contract that a
 *      future silent-noop regression cannot mimic success.
 *
 * Pattern 1+ — binary-wide `LifecycleGuard` for Logger + SecureSubsystem
 * + DataBlock, same shape as `test_hub_shm_queue_capability.cpp`
 * which lives next door.
 *
 * The fd-source pair helper (`make_fd_backed_pair`) is sourced from
 * `test_layer3_datahub/workers/datahub_fd_test_helper.h` — a
 * header-only helper with no L3-subprocess dependency.  Its
 * production dependency chain is identical to the L2 capability
 * test's inline `make_sized_memfd` path (same fd-source factory,
 * same DataBlockConfig shape).
 */
#include "utils/security/secure_subsystem.hpp"
#include "binary_lifecycle.h"
#include "utils/data_block.hpp"
#include "utils/data_block_config.hpp"
#include "utils/data_block_policy.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/logger.hpp"
#include "utils/recovery_api.hpp"
#include "utils/schema_types.hpp"

// Cross-directory header-only helper — no L3 process infra reached.
#include "../test_layer3_datahub/workers/datahub_fd_test_helper.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

using pylabhub::hub::ChecksumPolicy;
using pylabhub::hub::ConsumerSyncPolicy;
using pylabhub::hub::datablock_layout_total_size;
using pylabhub::hub::DataBlockConfig;
using pylabhub::hub::DataBlockPageSize;
using pylabhub::hub::DataBlockPolicy;
using pylabhub::hub::SchemaFieldDesc;
using pylabhub::hub::ShmQueue;

PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                             pylabhub::hub::GetDataBlockModule())

namespace
{

constexpr uint32_t kRingCapacity = 4;

// Slot schema — one int32 field, enough to write a sentinel through.
std::vector<SchemaFieldDesc> slot_schema()
{
    SchemaFieldDesc field;
    field.type_str = "int32";
    field.count = 1;
    return {field};
}

// Flexzone schema — one 512-byte scratch field so `flexzone()` returns
// a usable pointer.  ShmQueue does not care about semantic layout;
// it only sizes the region from the schema.
std::vector<SchemaFieldDesc> fz_schema_512b()
{
    SchemaFieldDesc field;
    field.type_str = "uint8";
    field.count = 512;
    return {field};
}

// Build a DataBlockConfig sized to match a schema pair.  The fd-source
// factory reads slot count + policy + page size + checksum policy
// from this config; flex_zone_size + logical_unit_size are computed
// by the caller (below) via `compute_field_layout`.
DataBlockConfig make_config(size_t item_size, size_t fz_size, ChecksumPolicy chk)
{
    DataBlockConfig cfg;
    cfg.logical_unit_size = item_size;
    cfg.flex_zone_size = fz_size;
    cfg.ring_buffer_capacity = kRingCapacity;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.checksum_policy = chk;
    return cfg;
}

// Compute the memfd size the layout requires for a given slot + flex
// schema pair.  Mirrors `ShmQueue::create_writer_standby`'s internal
// layout computation.
size_t compute_layout_size(const std::vector<SchemaFieldDesc> &slot_s,
                           const std::vector<SchemaFieldDesc> &fz_s, ChecksumPolicy chk)
{
    auto [_, item_size] = pylabhub::hub::compute_field_layout(slot_s, "aligned");
    size_t fz_size = 0;
    if (!fz_s.empty())
    {
        auto [__, fz_item] = pylabhub::hub::compute_field_layout(fz_s, "aligned");
        fz_size = fz_item;
    }
    return datablock_layout_total_size(make_config(item_size, fz_size, chk));
}

class ShmQueueContractTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        for (int fd : owned_fds_)
        {
            if (fd >= 0)
                ::close(fd);
        }
    }

    int track_fd(int fd)
    {
        owned_fds_.push_back(fd);
        return fd;
    }

    int make_sized_memfd(const char *label, const std::vector<SchemaFieldDesc> &slot_s,
                         const std::vector<SchemaFieldDesc> &fz_s,
                         ChecksumPolicy chk = ChecksumPolicy::None)
    {
        const size_t total = compute_layout_size(slot_s, fz_s, chk);
        EXPECT_GT(total, 0u);
        const int memfd = ::memfd_create(label, MFD_CLOEXEC);
        EXPECT_GE(memfd, 0) << "memfd_create failed: errno=" << errno;
        if (memfd < 0)
            return -1;
        track_fd(memfd);
        EXPECT_EQ(0, ::ftruncate(memfd, static_cast<off_t>(total)))
            << "ftruncate failed: errno=" << errno;
        return memfd;
    }

    // Dup a memfd via socketpair — in-process substitute for SCM_RIGHTS.
    // Symmetric with the same helper in the sibling
    // test_hub_shm_queue_capability.cpp ReaderStandby test.
    int scm_dup_fd(int src_fd)
    {
        int sv[2];
        EXPECT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
        track_fd(sv[0]);
        track_fd(sv[1]);
        // send
        char dummy = 'X';
        struct iovec iov{};
        iov.iov_base = &dummy;
        iov.iov_len = 1;
        char cmsgbuf_out[CMSG_SPACE(sizeof(int))] = {};
        struct msghdr out{};
        out.msg_iov = &iov;
        out.msg_iovlen = 1;
        out.msg_control = cmsgbuf_out;
        out.msg_controllen = sizeof(cmsgbuf_out);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&out);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &src_fd, sizeof(int));
        EXPECT_EQ(1, ::sendmsg(sv[0], &out, 0));
        // recv
        int dst_fd = -1;
        char in_dummy = 0;
        struct iovec in_iov{};
        in_iov.iov_base = &in_dummy;
        in_iov.iov_len = 1;
        char cmsgbuf_in[CMSG_SPACE(sizeof(int))] = {};
        struct msghdr in{};
        in.msg_iov = &in_iov;
        in.msg_iovlen = 1;
        in.msg_control = cmsgbuf_in;
        in.msg_controllen = sizeof(cmsgbuf_in);
        EXPECT_EQ(1, ::recvmsg(sv[1], &in, 0));
        struct cmsghdr *icmsg = CMSG_FIRSTHDR(&in);
        EXPECT_NE(nullptr, icmsg);
        std::memcpy(&dst_fd, CMSG_DATA(icmsg), sizeof(int));
        EXPECT_GE(dst_fd, 0);
        track_fd(dst_fd);
        return dst_fd;
    }

  private:
    std::vector<int> owned_fds_;
};

} // namespace

// ─── Test 1: NoFlexzone ──────────────────────────────────────────────
//
// flex_zone_size=0 (empty fz_schema) → both writer and reader report
// flexzone_size()==0 and flexzone()==nullptr.  Pins §HEP-CORE-0002 §2.2
// "no flexzone unless the schema says so."
TEST_F(ShmQueueContractTest, ShmQueueNoFlexzone)
{
    const std::string chan = "shm-queue-no-flexzone";
    const int memfd_w = make_sized_memfd("plh_l2_noflex_w", slot_schema(), /*fz_schema=*/{});
    ASSERT_GE(memfd_w, 0);

    auto q_write = ShmQueue::create_writer_standby(
        chan, slot_schema(), "aligned",
        /*fz_schema=*/{}, /*fz_packing=*/"", kRingCapacity, DataBlockPageSize::Size4K,
        DataBlockPolicy::RingBuffer, ConsumerSyncPolicy::Latest_only, ChecksumPolicy::None);
    ASSERT_NE(q_write, nullptr);
    ASSERT_TRUE(q_write->set_shm_capability_fd(memfd_w));
    ASSERT_TRUE(q_write->start());

    const int memfd_r = scm_dup_fd(memfd_w);
    ASSERT_GE(memfd_r, 0);

    auto q_read = ShmQueue::create_reader_standby(
        /*shm_name=*/chan, slot_schema(), "aligned", chan);
    ASSERT_NE(q_read, nullptr);
    ASSERT_TRUE(q_read->set_shm_capability_fd(memfd_r));
    ASSERT_TRUE(q_read->start());

    EXPECT_EQ(q_write->flexzone_size(), 0u);
    EXPECT_EQ(q_read->flexzone_size(), 0u);
    EXPECT_EQ(q_write->flexzone(), nullptr);
    EXPECT_EQ(q_read->flexzone(), nullptr);
}

// ─── Test 2: FlexzoneBidirectional ───────────────────────────────────
//
// One shared flexzone region per channel — writer→reader AND
// reader→writer visibility.  HEP-CORE-0002 §2.2.  Pre-S3c this was
// pinned by `ShmQueueFlexzoneBidirectional`.
TEST_F(ShmQueueContractTest, ShmQueueFlexzoneBidirectional)
{
    const std::string chan = "shm-queue-flex-bidi";
    const auto fz = fz_schema_512b();

    const int memfd_w = make_sized_memfd("plh_l2_flexbi_w", slot_schema(), fz);
    ASSERT_GE(memfd_w, 0);

    auto q_write = ShmQueue::create_writer_standby(
        chan, slot_schema(), "aligned", fz, "aligned", kRingCapacity, DataBlockPageSize::Size4K,
        DataBlockPolicy::RingBuffer, ConsumerSyncPolicy::Latest_only, ChecksumPolicy::None);
    ASSERT_NE(q_write, nullptr);
    ASSERT_TRUE(q_write->set_shm_capability_fd(memfd_w));
    ASSERT_TRUE(q_write->start());

    const int memfd_r = scm_dup_fd(memfd_w);
    ASSERT_GE(memfd_r, 0);

    auto q_read = ShmQueue::create_reader_standby(
        /*shm_name=*/chan, slot_schema(), "aligned", chan);
    ASSERT_NE(q_read, nullptr);
    ASSERT_TRUE(q_read->set_shm_capability_fd(memfd_r));
    ASSERT_TRUE(q_read->start());

    void *wfz = q_write->flexzone();
    void *rfz = q_read->flexzone();
    ASSERT_NE(wfz, nullptr);
    ASSERT_NE(rfz, nullptr);
    ASSERT_GT(q_write->flexzone_size(), 0u);
    ASSERT_EQ(q_write->flexzone_size(), q_read->flexzone_size());

    // (b) writer→reader visibility.
    static const char kForward[] = "writer_to_reader";
    std::memcpy(wfz, kForward, sizeof(kForward));
    EXPECT_EQ(std::memcmp(rfz, kForward, sizeof(kForward)), 0);

    // (c) reader→writer visibility.  Offset far enough to not clobber
    // the forward-direction sentinel above.
    static const char kReply[] = "reader_to_writer";
    std::memcpy(static_cast<char *>(rfz) + 64, kReply, sizeof(kReply));
    EXPECT_EQ(std::memcmp(static_cast<char *>(wfz) + 64, kReply, sizeof(kReply)), 0);
}

// ─── Test 3: VerifyChecksumMismatch ──────────────────────────────────
//
// Writer commits a slot without stamping a checksum (slot_valid=0);
// reader with `set_verify_checksum(true, false)` observes nullptr on
// read_acquire.  Then the writer enables `set_checksum_options`,
// writes a fresh slot, and read_acquire returns non-null.
//
// Pre-S3c this was `ShmQueueVerifyChecksumMismatch`.  The slot-gate
// contract prevents a corrupted or uninitialized slot from leaking
// to a checksum-enforcing reader.
TEST_F(ShmQueueContractTest, ShmQueueVerifyChecksumMismatch)
{
    const std::string chan = "shm-queue-verify-chksum";

    const int memfd_w = make_sized_memfd("plh_l2_chksum_w", slot_schema(), /*fz_schema=*/{});
    ASSERT_GE(memfd_w, 0);

    auto q_write = ShmQueue::create_writer_standby(
        chan, slot_schema(), "aligned",
        /*fz_schema=*/{}, /*fz_packing=*/"", kRingCapacity, DataBlockPageSize::Size4K,
        DataBlockPolicy::RingBuffer, ConsumerSyncPolicy::Latest_only, ChecksumPolicy::None);
    ASSERT_NE(q_write, nullptr);
    ASSERT_TRUE(q_write->set_shm_capability_fd(memfd_w));
    ASSERT_TRUE(q_write->start());

    const int memfd_r = scm_dup_fd(memfd_w);
    ASSERT_GE(memfd_r, 0);

    auto q_read = ShmQueue::create_reader_standby(
        /*shm_name=*/chan, slot_schema(), "aligned", chan);
    ASSERT_NE(q_read, nullptr);
    ASSERT_TRUE(q_read->set_shm_capability_fd(memfd_r));
    ASSERT_TRUE(q_read->start());

    // Reader enforces slot checksum (fz off).
    q_read->set_verify_checksum(/*slot=*/true, /*fz=*/false);

    // Writer commits WITHOUT enabling checksum stamping — slot_valid=0.
    void *slot = q_write->write_acquire(std::chrono::milliseconds{100});
    ASSERT_NE(slot, nullptr);
    int32_t sentinel = 0x7a1a1a1a;
    std::memcpy(slot, &sentinel, sizeof(sentinel));
    q_write->write_commit();

    // Reader refuses the invalid slot.
    const void *bad = q_read->read_acquire(std::chrono::milliseconds{50});
    EXPECT_EQ(bad, nullptr) << "read_acquire must return nullptr on slot_valid=0 with "
                            << "verify_checksum enabled (HEP-CORE-0002 slot-gate contract)";

    // Writer enables checksum stamping; next slot passes verification.
    q_write->set_checksum_options(/*slot=*/true, /*fz=*/false);
    slot = q_write->write_acquire(std::chrono::milliseconds{100});
    ASSERT_NE(slot, nullptr);
    sentinel = 0x0badf00d;
    std::memcpy(slot, &sentinel, sizeof(sentinel));
    q_write->write_commit();

    const void *good = q_read->read_acquire(std::chrono::milliseconds{100});
    ASSERT_NE(good, nullptr) << "read_acquire must succeed once writer stamps checksum";
    int32_t observed = 0;
    std::memcpy(&observed, good, sizeof(observed));
    EXPECT_EQ(observed, 0x0badf00d);
    q_read->read_release();
}

// ─── Test 4: DataBlockProducerRemapStubsThrow ────────────────────────
//
// `request_structure_remap` and `commit_structure_remap` are documented
// stubs (data_block.hpp:1054 "NOT IMPLEMENTED — always throws
// std::runtime_error").  Pin that they DO throw, and pin the function-
// name substring on `what()` so a regression that redirects one stub's
// throw through the wrong path fails visibly.
//
// Uses `make_fd_backed_producer` from the sibling header helper (no L3
// subprocess machinery — same fd-source factory as the ShmQueue tests
// above, just at the DataBlockProducer level directly).  DataBlock
// stubs live below ShmQueue in the layer stack; testing at
// DataBlockProducer is the correct level for this contract.
TEST_F(ShmQueueContractTest, DataBlockProducerRemapStubsThrow)
{
    // Compute a DataBlockConfig identical to what ShmQueue would compute
    // for slot_schema() with no flexzone + ChecksumPolicy::None.
    auto [_, item_size] = pylabhub::hub::compute_field_layout(slot_schema(), "aligned");
    const DataBlockConfig cfg = make_config(item_size, /*fz_size=*/0, ChecksumPolicy::None);

    auto p = pylabhub::tests::helper::make_fd_backed_producer("shm-queue-remap-stubs",
                                                              DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(p.producer, nullptr);
    auto &producer = p.producer;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    // request_structure_remap — stub always throws with the
    // function-name substring in what().  The `[[deprecated]]`
    // attribute on the API is intentional — this test EXISTS to pin
    // the throw contract on that stub; suppressing the warning here
    // matches the shape of the retired L3 test.
    {
        bool threw = false;
        std::string msg;
        try
        {
            (void)producer->request_structure_remap(std::nullopt, std::nullopt);
        }
        catch (const std::runtime_error &e)
        {
            threw = true;
            msg = e.what();
        }
        EXPECT_TRUE(threw);
        EXPECT_NE(msg.find("DataBlockProducer::request_structure_remap"), std::string::npos)
            << "wrong runtime_error path; what(): " << msg;
    }

    // commit_structure_remap — symmetric stub.
    {
        bool threw = false;
        std::string msg;
        try
        {
            producer->commit_structure_remap(0, std::nullopt, std::nullopt);
        }
        catch (const std::runtime_error &e)
        {
            threw = true;
            msg = e.what();
        }
        EXPECT_TRUE(threw);
        EXPECT_NE(msg.find("DataBlockProducer::commit_structure_remap"), std::string::npos)
            << "wrong runtime_error path; what(): " << msg;
    }
#pragma GCC diagnostic pop
}

// ─── Test 5: datablock_get_metrics_from_fd (#317 Phase A) ────────────
//
// HEP-CORE-0041 §10.5 (task #317) — memfd-source metrics reader.
// The name-based `datablock_get_metrics` fails with ENOENT for
// memfd-backed segments (no /dev/shm name).  This fd-source variant
// lets an observer (broker, admin plane) read live metrics from a
// memfd received via SCM_RIGHTS.
//
// Test scope:
// - Success path: producer writes N slots; `..._from_fd` returns 0,
//   metrics show total_slots_written == N.
// - Error paths: invalid fd (-1), null out pointer, non-DataBlock fd
//   (mmap'd memfd with garbage header) — all return -1 without
//   crashing.
TEST_F(ShmQueueContractTest, DataBlockGetMetricsFromFd)
{
    auto [_, item_size] = pylabhub::hub::compute_field_layout(slot_schema(), "aligned");
    const DataBlockConfig cfg = make_config(item_size, /*fz_size=*/0, ChecksumPolicy::None);

    auto p = pylabhub::tests::helper::make_fd_backed_producer("shm-queue-metrics-fd",
                                                              DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(p.producer, nullptr);

    // Produce two slots so total_slots_written reflects a live counter,
    // not a fresh-init zero.
    for (int i = 0; i < 2; ++i)
    {
        auto handle = p.producer->acquire_write_slot(/*timeout_ms=*/100);
        ASSERT_NE(handle, nullptr);
        auto buf = handle->buffer_span();
        ASSERT_GE(buf.size(), sizeof(int32_t));
        int32_t sentinel = i;
        std::memcpy(buf.data(), &sentinel, sizeof(sentinel));
        ASSERT_TRUE(handle->commit(sizeof(sentinel)));
        ASSERT_TRUE(p.producer->release_write_slot(*handle));
    }

    // ── Success: read metrics via the observation fd ─────────────────
    DataBlockMetrics m{};
    const int rc = ::datablock_get_metrics_from_fd(p.transport->borrow_fd(), &m);
    EXPECT_EQ(rc, 0) << "datablock_get_metrics_from_fd should succeed on a "
                     << "live memfd-backed DataBlock";
    EXPECT_GE(m.total_slots_written, 2u)
        << "metrics should reflect at least the 2 slots we committed";
    EXPECT_GT(m.slot_count, 0u) << "slot_count must be non-zero";

    // ── Error: invalid fd (-1) ──────────────────────────────────────
    DataBlockMetrics m_bad{};
    EXPECT_EQ(::datablock_get_metrics_from_fd(-1, &m_bad), -1);

    // ── Error: null out pointer ─────────────────────────────────────
    EXPECT_EQ(::datablock_get_metrics_from_fd(p.transport->borrow_fd(), nullptr), -1);

    // ── Error: non-DataBlock fd (memfd with no valid header) ────────
    const int garbage_fd = ::memfd_create("plh_l2_garbage", MFD_CLOEXEC);
    ASSERT_GE(garbage_fd, 0);
    track_fd(garbage_fd);
    // Truncate to a plausible size but leave contents zero — header
    // magic won't match; the from-fd open should refuse.
    ASSERT_EQ(0, ::ftruncate(garbage_fd, static_cast<off_t>(4096)));
    EXPECT_EQ(::datablock_get_metrics_from_fd(garbage_fd, &m_bad), -1);
}

// ─── Test 6: DataBlockObserverHandle — header-only + ABI check ──────
//
// HEP-CORE-0041 §10.5 (task #317 Phase B): the observer factory maps
// ONLY sizeof(SharedMemoryHeader) bytes with PROT_READ, and runs
// `validate_header_layout_hash` for ABI compatibility.
//
// Test surface:
//   (a) Success: producer writes slots; observer sees a non-null
//       header; magic_number reads non-zero.
//   (b) Isolation: DataBlockConfig-computed total size is strictly
//       greater than sizeof(SharedMemoryHeader) — pins the design
//       invariant that we're NOT mapping the whole DataBlock.
//   (c) ABI: garbage memfd is refused (magic OR layout hash fails).
TEST_F(ShmQueueContractTest, DataBlockObserverHandle_HeaderOnlyIsolation)
{
    auto [_, item_size] = pylabhub::hub::compute_field_layout(slot_schema(), "aligned");
    const DataBlockConfig cfg = make_config(item_size, /*fz_size=*/0, ChecksumPolicy::None);

    auto p = pylabhub::tests::helper::make_fd_backed_producer("shm-queue-observer",
                                                              DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(p.producer, nullptr);

    // Do a write so the header has non-init state.
    {
        auto handle = p.producer->acquire_write_slot(/*timeout_ms=*/100);
        ASSERT_NE(handle, nullptr);
        auto buf = handle->buffer_span();
        ASSERT_GE(buf.size(), sizeof(int32_t));
        int32_t v = 0xC0FFEE;
        std::memcpy(buf.data(), &v, sizeof(v));
        ASSERT_TRUE(handle->commit(sizeof(v)));
        ASSERT_TRUE(p.producer->release_write_slot(*handle));
    }

    // (a) Success — observer handle exposes the atomic header.
    auto obs = pylabhub::hub::open_datablock_for_observer_from_fd(p.transport->borrow_fd());
    ASSERT_NE(obs, nullptr);
    ASSERT_NE(obs->header(), nullptr);

    const uint32_t magic = obs->header()->magic_number.load(std::memory_order_acquire);
    EXPECT_NE(magic, 0u) << "Observer must read the atomic magic_number field";

    // (b) Isolation — total DataBlock region is strictly larger than
    // the header we mapped.  Design invariant: observer's mmap is
    // HEADER-ONLY.
    const size_t total = pylabhub::hub::datablock_layout_total_size(cfg);
    EXPECT_GT(total, sizeof(pylabhub::hub::SharedMemoryHeader))
        << "Test premise: total region exceeds header, so observer's "
        << "PROT_READ mmap of sizeof(header) does NOT cover slots/fz.";

    // (c) ABI / magic gate — garbage memfd refused.  A zero-header
    // memfd fails is_header_magic_valid before validate_header_layout_hash
    // can run; both gates are protective.  This subtest pins the
    // combined refusal, not a specific gate.
    const int garbage_fd = ::memfd_create("plh_l2_obs_garbage", MFD_CLOEXEC);
    ASSERT_GE(garbage_fd, 0);
    track_fd(garbage_fd);
    ASSERT_EQ(0, ::ftruncate(garbage_fd,
                             static_cast<off_t>(sizeof(pylabhub::hub::SharedMemoryHeader) + 4096)));
    auto bad = pylabhub::hub::open_datablock_for_observer_from_fd(garbage_fd);
    EXPECT_EQ(bad, nullptr) << "Observer factory must reject a memfd whose header is invalid";
}
