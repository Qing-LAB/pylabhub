/**
 * @file test_hub_shm_queue_capability.cpp
 * @brief L2 tests for HEP-CORE-0041 substep 1i-mig-1 — ShmQueue
 *        capability-fd plumbing (set_shm_capability_fd + Standby-mode
 *        create_*_standby factories + start() fd-source branch).
 *
 * Pattern 1+ — binary-wide `LifecycleGuard` for `Logger` + `CryptoUtils`
 * + `DataBlock` (the fd-source factories require the DataBlock module
 * per the same contract as the name-based factories).  No cross-test
 * state.
 *
 * Covers:
 *   - WriterStandby_SetCapabilityFd_Start_Active — round-trip:
 *     memfd_create + ftruncate(layout_total_size) → create_writer_standby
 *     → set_shm_capability_fd → start() → write_acquire returns non-null.
 *   - ReaderStandby_SetCapabilityFd_Start_Active_ReadsProducerData —
 *     pair an fd-source producer with an fd-source consumer through
 *     the same memfd (dup'd via socketpair to mirror SCM_RIGHTS).
 *     Pins the producer→consumer mmap-shared round-trip via ShmQueue's
 *     write_acquire / commit + read_acquire / release.
 *   - SetCapabilityFd_RefusesFromActive — §6.7 mutator-table
 *     refuse-from-Active rule extended to the capability path.
 *   - SetCapabilityFd_RefusesWhenSecretAlreadySet — HEP-0041 D7
 *     "single unified mechanism" mutual exclusion (secret-mode wins,
 *     capability rejected).
 *   - SetShmSecret_RefusesWhenCapabilityAlreadySet — symmetric
 *     mutual exclusion (capability-mode wins, secret rejected).
 *   - SetCapabilityFd_RefusesNegativeFd — defensive guardrail on
 *     the setter input.
 */
#include "binary_lifecycle.h"
#include "utils/crypto_utils.hpp"
#include "utils/data_block.hpp"
#include "utils/data_block_config.hpp"
#include "utils/data_block_policy.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/logger.hpp"
#include "utils/schema_types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

using pylabhub::hub::ChecksumPolicy;
using pylabhub::hub::ConsumerSyncPolicy;
using pylabhub::hub::DataBlockConfig;
using pylabhub::hub::DataBlockPageSize;
using pylabhub::hub::DataBlockPolicy;
using pylabhub::hub::SchemaFieldDesc;
using pylabhub::hub::ShmQueue;
using pylabhub::hub::datablock_layout_total_size;

PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::crypto::GetLifecycleModule(),
    pylabhub::hub::GetDataBlockModule())

namespace
{

// One float32 slot field, no flexzone — keep the layout small + the
// matching_config + create_writer_standby args bit-identical so
// datablock_layout_total_size matches the ftruncate'd fd size exactly.
constexpr uint32_t kRingCapacity = 4;

std::vector<SchemaFieldDesc> make_slot_schema()
{
    SchemaFieldDesc field;
    field.type_str = "float32";
    field.count    = 1;
    return {field};
}

// Build the DataBlockConfig that ShmQueue's start() will pass to the
// fd-source factory.  The memfd must be ftruncated to
// datablock_layout_total_size(this exact config).
DataBlockConfig matching_config(size_t item_size)
{
    DataBlockConfig cfg;
    cfg.logical_unit_size    = item_size;
    cfg.flex_zone_size       = 0;   // matches empty fz_schema below
    cfg.ring_buffer_capacity = kRingCapacity;
    cfg.physical_page_size   = DataBlockPageSize::Size4K;
    cfg.policy               = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.checksum_policy      = ChecksumPolicy::None;
    return cfg;
}

class ShmQueueCapabilityTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        for (int fd : owned_fds_)
        {
            if (fd >= 0) ::close(fd);
        }
    }

    int track_fd(int fd)
    {
        owned_fds_.push_back(fd);
        return fd;
    }

    // memfd_create + ftruncate to the layout size that matches the schema
    // ShmQueue's create_writer_standby below will produce.
    int make_sized_memfd(const char *label)
    {
        const auto slot_schema = make_slot_schema();
        // Compute the item_size that the writer factory will compute.
        auto [_, item_size] = pylabhub::hub::compute_field_layout(
            slot_schema, "aligned");
        const auto total = datablock_layout_total_size(matching_config(item_size));
        EXPECT_GT(total, 0u);

        const int memfd = ::memfd_create(label, MFD_CLOEXEC);
        EXPECT_GE(memfd, 0) << "memfd_create failed: errno=" << errno;
        if (memfd < 0) return -1;
        track_fd(memfd);
        EXPECT_EQ(0, ::ftruncate(memfd, static_cast<off_t>(total)))
            << "ftruncate failed: errno=" << errno;
        return memfd;
    }

    static std::unique_ptr<ShmQueue> standby_writer(const std::string &chan)
    {
        return ShmQueue::create_writer_standby(
            chan, make_slot_schema(), "aligned",
            /*fz_schema=*/{}, /*fz_packing=*/"aligned",
            kRingCapacity, DataBlockPageSize::Size4K,
            DataBlockPolicy::RingBuffer,
            ConsumerSyncPolicy::Latest_only, ChecksumPolicy::None);
    }

    static std::unique_ptr<ShmQueue> standby_reader(const std::string &chan)
    {
        return ShmQueue::create_reader_standby(
            /*shm_name=*/chan, make_slot_schema(), "aligned",
            chan);
    }

  private:
    std::vector<int> owned_fds_;
};

} // namespace

// ── Test 1: writer Standby → set_shm_capability_fd → start → Active ─
TEST_F(ShmQueueCapabilityTest, WriterStandby_SetCapabilityFd_Start_Active)
{
    auto queue = standby_writer("cap-writer-test");
    ASSERT_NE(queue, nullptr);
    // Standby: write_acquire must not crash; queue is_running() false.
    ASSERT_FALSE(queue->is_running());

    const int memfd = make_sized_memfd("plh_l2_shmq_cap_w");
    ASSERT_GE(memfd, 0);

    ASSERT_TRUE(queue->set_shm_capability_fd(memfd))
        << "set_shm_capability_fd should succeed from Standby";
    ASSERT_TRUE(queue->start()) << "start() should drive Configured → Active";
    ASSERT_TRUE(queue->is_running());

    // Pin that the ring buffer is addressable on the new path.
    void *slot = queue->write_acquire(std::chrono::milliseconds(100));
    ASSERT_NE(slot, nullptr);
    float sentinel = 3.25f;
    std::memcpy(slot, &sentinel, sizeof(sentinel));
    queue->write_commit();
}

// ── Test 2: producer + consumer fd-source round-trip via ShmQueue ────
//
// Producer-side ShmQueue creates the DataBlock over a memfd; consumer-
// side ShmQueue attaches via the same fd (dup'd through a socketpair,
// mirroring SCM_RIGHTS semantics).  Pins that data written via the
// producer's write_acquire is observable via the consumer's
// read_acquire — the end-to-end pipe through the new path.
TEST_F(ShmQueueCapabilityTest,
       ReaderStandby_SetCapabilityFd_Start_Active_ReadsProducerData)
{
    const std::string chan = "cap-rw-test";

    // Producer side.
    auto writer       = standby_writer(chan);
    const int memfd_w = make_sized_memfd("plh_l2_shmq_cap_rw_w");
    ASSERT_GE(memfd_w, 0);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(writer->set_shm_capability_fd(memfd_w));
    ASSERT_TRUE(writer->start());

    // Dup the memfd across a socketpair → consumer-side fd (distinct
    // integer, same kernel object).  Test only — no auth handshake
    // wrapping (1i-mig-4 territory).
    int sv[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    track_fd(sv[0]);
    track_fd(sv[1]);
    // Use SCM_RIGHTS helpers inline — same shape as
    // test_data_block_fd_based.cpp.
    {
        char dummy = 'X';
        struct iovec iov{};
        iov.iov_base = &dummy;
        iov.iov_len  = 1;
        char           cmsgbuf[CMSG_SPACE(sizeof(int))] = {};
        struct msghdr  msg{};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level     = SOL_SOCKET;
        cmsg->cmsg_type      = SCM_RIGHTS;
        cmsg->cmsg_len       = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &memfd_w, sizeof(int));
        ASSERT_EQ(1, ::sendmsg(sv[0], &msg, 0));
    }
    int memfd_r = -1;
    {
        char dummy = 0;
        struct iovec iov{};
        iov.iov_base = &dummy;
        iov.iov_len  = 1;
        char           cmsgbuf[CMSG_SPACE(sizeof(int))] = {};
        struct msghdr  msg{};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        ASSERT_EQ(1, ::recvmsg(sv[1], &msg, 0));
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        ASSERT_NE(nullptr, cmsg);
        std::memcpy(&memfd_r, CMSG_DATA(cmsg), sizeof(int));
        ASSERT_GE(memfd_r, 0);
        track_fd(memfd_r);
    }

    auto reader = standby_reader(chan);
    ASSERT_NE(reader, nullptr);
    ASSERT_TRUE(reader->set_shm_capability_fd(memfd_r));
    ASSERT_TRUE(reader->start());
    ASSERT_TRUE(reader->is_running());

    // Write a sentinel through the writer; expect to read it on the
    // consumer side.
    void *slot = writer->write_acquire(std::chrono::milliseconds(100));
    ASSERT_NE(slot, nullptr);
    const float sentinel = 1.5f;
    std::memcpy(slot, &sentinel, sizeof(sentinel));
    writer->write_commit();

    const void *rslot = reader->read_acquire(std::chrono::milliseconds(500));
    ASSERT_NE(rslot, nullptr);
    float observed = 0.0f;
    std::memcpy(&observed, rslot, sizeof(observed));
    EXPECT_FLOAT_EQ(sentinel, observed);
    reader->read_release();
}

// ── Test 3: refuse from Active (§6.7 mutator-table extension) ────────
TEST_F(ShmQueueCapabilityTest, SetCapabilityFd_RefusesFromActive)
{
    auto queue = standby_writer("cap-refuse-active");
    ASSERT_NE(queue, nullptr);
    const int memfd = make_sized_memfd("plh_l2_shmq_cap_active");
    ASSERT_GE(memfd, 0);
    ASSERT_TRUE(queue->set_shm_capability_fd(memfd));
    ASSERT_TRUE(queue->start());
    ASSERT_TRUE(queue->is_running());

    // Already Active.  Second call must refuse per §6.7 (fd is
    // per-channel-lifetime; teardown needed before swapping).
    const int memfd2 = make_sized_memfd("plh_l2_shmq_cap_active2");
    ASSERT_GE(memfd2, 0);
    EXPECT_FALSE(queue->set_shm_capability_fd(memfd2))
        << "set_shm_capability_fd must refuse from Active";
}

// ── Test 4: capability path refuses when secret path already used ────
TEST_F(ShmQueueCapabilityTest, SetCapabilityFd_RefusesWhenSecretAlreadySet)
{
    auto queue = standby_writer("cap-vs-secret-1");
    ASSERT_NE(queue, nullptr);
    // Apply legacy secret first.  Does NOT start() — leaves queue in
    // Configured(secret-mode).
    ASSERT_TRUE(queue->set_shm_secret(0x1234));

    const int memfd = make_sized_memfd("plh_l2_shmq_cap_excl1");
    ASSERT_GE(memfd, 0);
    EXPECT_FALSE(queue->set_shm_capability_fd(memfd))
        << "HEP-CORE-0041 D7 mutual exclusion violated";
}

// ── Test 5: secret path refuses when capability path already used ────
TEST_F(ShmQueueCapabilityTest, SetShmSecret_RefusesWhenCapabilityAlreadySet)
{
    auto queue = standby_writer("cap-vs-secret-2");
    ASSERT_NE(queue, nullptr);
    const int memfd = make_sized_memfd("plh_l2_shmq_cap_excl2");
    ASSERT_GE(memfd, 0);
    ASSERT_TRUE(queue->set_shm_capability_fd(memfd));

    EXPECT_FALSE(queue->set_shm_secret(0xABCD))
        << "HEP-CORE-0041 D7 mutual exclusion violated";
}

// ── Test 6: defensive guardrail on negative fd ───────────────────────
TEST_F(ShmQueueCapabilityTest, SetCapabilityFd_RefusesNegativeFd)
{
    auto queue = standby_writer("cap-neg-fd");
    ASSERT_NE(queue, nullptr);
    EXPECT_FALSE(queue->set_shm_capability_fd(-1));
    EXPECT_FALSE(queue->set_shm_capability_fd(-42));
}
