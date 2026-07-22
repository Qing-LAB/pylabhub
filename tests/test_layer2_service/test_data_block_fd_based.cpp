/**
 * @file test_data_block_fd_based.cpp
 * @brief L2 tests for HEP-CORE-0041 substep 1f (#253) — DataBlock
 *        fd-source ctors + `create_datablock_producer_from_fd_impl` /
 *        `find_datablock_consumer_from_fd_impl` factories.
 *
 * Pattern 1+ — binary-wide `LifecycleGuard` for `Logger` + `SecureSubsystem`
 * + `DataBlock` (the fd-source factories require
 * `pylabhub::hub::GetDataBlockModule()` per the same contract as the
 * name-based factories).  No cross-test state.
 *
 * Covers:
 *   - In-process round-trip via `memfd_create` + socketpair/SCM_RIGHTS:
 *     producer constructs DataBlock-from-fd, writes a sentinel byte in
 *     the flexible zone; consumer (in the same process, on a
 *     SCM_RIGHTS-duped fd) constructs DataBlock-from-fd and reads the
 *     same byte.  Pins that the underlying mmap is shared.  If the
 *     header init/validate path were wrong, the consumer's
 *     `attach_consumer_state_` would throw (magic / version / size
 *     check) — so successful construction is a positive signal on
 *     header round-trip.
 *   - End-to-end via `create_shm_capability_producer` (substep 1b) +
 *     `attach_shm_capability_consumer` → fd handoff via the producer's
 *     own `send_capability`/`recv_capability` → both sides construct
 *     DataBlock-from-fd.  This pins the 1b↔1f integration: the fd
 *     returned by `borrow_fd()` on each side IS a valid input to the
 *     fd-source factory.
 *   - Producer ctor REJECTS an under-sized fd (caller didn't ftruncate
 *     to `DataBlockLayout::total_size`).  Validates the fstat-size
 *     check in `setup_shm_from_fd_`.
 *
 * Test isolation: each TEST_F gets its own memfd / socketpair / socket
 * path; the fixture closes/unlinks on TearDown.
 */
#include "utils/security/secure_subsystem.hpp"
#include "binary_lifecycle.h"
#include "utils/data_block.hpp"
#include "utils/data_block_config.hpp"
#include "utils/data_block_policy.hpp"
#include "utils/logger.hpp"
#include "utils/security/shm_capability_channel.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

using pylabhub::hub::ChecksumType;
using pylabhub::hub::ConsumerSyncPolicy;
using pylabhub::hub::create_datablock_producer_from_fd_impl;
using pylabhub::hub::datablock_layout_total_size;
using pylabhub::hub::DataBlockConfig;
using pylabhub::hub::DataBlockPageSize;
using pylabhub::hub::DataBlockPolicy;
using pylabhub::hub::find_datablock_consumer_from_fd_impl;
using pylabhub::utils::security::attach_shm_capability_consumer;
using pylabhub::utils::security::create_shm_capability_producer;

// Register binary-wide lifecycle: Logger + SecureSubsystem + DataBlock.
// DataBlock depends on the other two (per its module def).
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                             pylabhub::hub::GetDataBlockModule())

namespace
{

DataBlockConfig make_test_config()
{
    DataBlockConfig cfg;
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.logical_unit_size = 0; // → resolves to physical_page_size (4K)
    cfg.ring_buffer_capacity = 4;
    cfg.flex_zone_size = 4096;
    cfg.checksum_type = ChecksumType::BLAKE2b;
    cfg.hub_uid = "test-hub";
    cfg.hub_name = "TestHub";
    cfg.producer_uid = "test-producer";
    cfg.producer_name = "TestProducer";
    return cfg;
}

// Send `fd` over `socket_fd` via SCM_RIGHTS.  Returns true on success.
// Mirrors the producer side of ShmCapabilityProducer::send_capability
// at a stripped-down level for in-process test plumbing.
bool send_fd(int socket_fd, int fd)
{
    char dummy = 'X';
    struct iovec iov{};
    iov.iov_base = &dummy;
    iov.iov_len = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return ::sendmsg(socket_fd, &msg, 0) == 1;
}

// Receive an fd via SCM_RIGHTS from `socket_fd`.  Returns -1 on failure.
int recv_fd(int socket_fd)
{
    char dummy = 0;
    struct iovec iov{};
    iov.iov_base = &dummy;
    iov.iov_len = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    ssize_t n = ::recvmsg(socket_fd, &msg, 0);
    if (n <= 0)
    {
        return -1;
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
    {
        return -1;
    }
    int received_fd = -1;
    std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
    return received_fd;
}

class DataBlockFdBasedTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        for (int fd : owned_fds_)
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
        }
        for (const auto &p : paths_)
        {
            std::error_code ec;
            fs::remove(p, ec);
        }
    }

    int track_fd(int fd)
    {
        owned_fds_.push_back(fd);
        return fd;
    }

    fs::path make_socket_path(const char *suffix)
    {
        const auto p = fs::path("/tmp") / (std::string("plh_l2_dbfd_") +
                                           std::to_string(::getpid()) + "_" + suffix + ".sock");
        paths_.push_back(p);
        return p;
    }

  private:
    std::vector<int> owned_fds_;
    std::vector<fs::path> paths_;
};

// ── Test 1: in-process round-trip via memfd_create + socketpair ──────
//
// Producer creates a memfd, ftruncates it, hands the fd to
// `create_datablock_producer_from_fd_impl`.  We then dup the fd through
// a socketpair (mirroring what SCM_RIGHTS does cross-process) and feed
// the received dup to `find_datablock_consumer_from_fd_impl`.
//
// Successful construction on the consumer side proves header magic /
// version / size are valid (those checks are in `attach_consumer_state_`
// and throw on mismatch).  The sentinel-byte round-trip through the
// flexible zone proves the underlying mmap is shared.
TEST_F(DataBlockFdBasedTest, ProducerCreate_ConsumerAttach_FlexZoneRoundTrip)
{
    DataBlockConfig cfg = make_test_config();
    const size_t total = datablock_layout_total_size(cfg);
    ASSERT_GT(total, 0u);

    // Producer-owned memfd, sized for the layout.
    int memfd = ::memfd_create("plh_l2_dbfd_test", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0) << "memfd_create failed: errno=" << errno;
    track_fd(memfd);
    ASSERT_EQ(0, ::ftruncate(memfd, static_cast<off_t>(total)))
        << "ftruncate failed: errno=" << errno;

    auto producer = create_datablock_producer_from_fd_impl(
        "fd-test-producer", memfd, DataBlockPolicy::RingBuffer, cfg,
        /*flexzone_schema=*/nullptr, /*datablock_schema=*/nullptr);
    ASSERT_NE(producer, nullptr);

    // Pass the fd through a socketpair to mirror SCM_RIGHTS semantics:
    // the receiver gets a distinct fd (different small integer) backing
    // the same kernel object.
    int sv[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) << "socketpair failed: errno=" << errno;
    track_fd(sv[0]);
    track_fd(sv[1]);

    ASSERT_TRUE(send_fd(sv[0], memfd)) << "send_fd failed: errno=" << errno;
    int received_fd = recv_fd(sv[1]);
    ASSERT_GE(received_fd, 0) << "recv_fd failed: errno=" << errno;
    track_fd(received_fd);
    ASSERT_NE(received_fd, memfd) << "received fd should be a distinct integer from the producer's";

    auto consumer = find_datablock_consumer_from_fd_impl(
        "fd-test-consumer", received_fd,
        /*expected_config=*/&cfg,
        /*flexzone_schema=*/nullptr, /*datablock_schema=*/nullptr, "consumer-A", "ConsumerA");
    ASSERT_NE(consumer, nullptr) << "consumer factory returned nullptr — header validation in "
                                    "attach_consumer_state_ failed; mmap likely not shared.";

    // Round-trip a sentinel byte through the flexible zone.  Producer
    // and consumer have separate mmaps (different virtual addresses)
    // but they back the same kernel pages, so writes on one side are
    // visible on the other.
    auto producer_flex = producer->flexible_zone_span();
    auto consumer_flex = consumer->flexible_zone_span();
    ASSERT_FALSE(producer_flex.empty());
    ASSERT_FALSE(consumer_flex.empty());
    ASSERT_EQ(producer_flex.size(), consumer_flex.size());

    producer_flex[0] = std::byte{0xA5};
    // Memory order on x86 / arm makes this immediately visible to the
    // consumer side; no fence needed for a single-byte observation.
    EXPECT_EQ(consumer_flex[0], std::byte{0xA5})
        << "consumer's flex zone did not observe the producer's write — "
           "the two mmaps are not backed by the same kernel pages.";
}

// ── Test 2: end-to-end via ShmCapabilityProducer + fd-source DataBlock ─
//
// This is the integration pin between 1b (ShmCapability transport) and
// 1f (DataBlock-from-fd).  The producer creates its ShmCapability over
// a configured layout-size; we read the fd back via borrow_fd() and
// feed it to the DataBlock fd-source factory; same on the consumer side
// after attach_shm_capability_consumer + recv_capability.
TEST_F(DataBlockFdBasedTest, EndToEnd_ViaShmCapability_RoundTrip)
{
    DataBlockConfig cfg = make_test_config();
    const size_t total = datablock_layout_total_size(cfg);

    auto producer_cap = create_shm_capability_producer(total);
    ASSERT_NE(producer_cap, nullptr);

    const auto sock_path = make_socket_path("e2e");
    ASSERT_TRUE(producer_cap->bind_endpoint(sock_path.string()));

    std::atomic<bool> consumer_ready{false};
    std::atomic<int> consumer_observed_byte{-1};
    std::thread consumer_thread(
        [&]
        {
            try
            {
                // attach_shm_capability_consumer connects + recvs + mmaps in a
                // single step.  No separate recv_capability call.
                auto consumer_cap =
                    attach_shm_capability_consumer(sock_path.string(), std::chrono::seconds{5});
                if (consumer_cap == nullptr)
                {
                    return;
                }
                int cfd = consumer_cap->borrow_fd();
                if (cfd < 0)
                {
                    return;
                }
                auto consumer_db = find_datablock_consumer_from_fd_impl(
                    "fd-e2e-consumer", cfd,
                    /*expected_config=*/&cfg,
                    /*flexzone_schema=*/nullptr, /*datablock_schema=*/nullptr, "consumer-E2E",
                    "ConsumerE2E");
                if (consumer_db == nullptr)
                {
                    return;
                }
                auto flex = consumer_db->flexible_zone_span();
                if (flex.empty())
                {
                    return;
                }
                consumer_observed_byte.store(
                    static_cast<int>(std::to_integer<unsigned int>(flex[0])),
                    std::memory_order_release);
                consumer_ready.store(true, std::memory_order_release);
            }
            catch (...)
            {
                // Test failure surfaces via consumer_ready remaining false.
            }
        });

    // Producer side: accept the connection, then write the sentinel
    // BEFORE sending the cap so the consumer is guaranteed to see it
    // on first read.
    auto peer = producer_cap->accept_one(std::chrono::seconds{5});
    ASSERT_TRUE(peer.has_value());

    int pfd = producer_cap->borrow_fd();
    ASSERT_GE(pfd, 0);
    auto producer_db = create_datablock_producer_from_fd_impl(
        "fd-e2e-producer", pfd, DataBlockPolicy::RingBuffer, cfg,
        /*flexzone_schema=*/nullptr, /*datablock_schema=*/nullptr);
    ASSERT_NE(producer_db, nullptr);

    auto producer_flex = producer_db->flexible_zone_span();
    ASSERT_FALSE(producer_flex.empty());
    producer_flex[0] = std::byte{0xA5};

    ASSERT_TRUE(producer_cap->send_capability(peer->peer_socket_fd));
    ::close(peer->peer_socket_fd);

    consumer_thread.join();
    ASSERT_TRUE(consumer_ready.load(std::memory_order_acquire));
    EXPECT_EQ(0xA5, consumer_observed_byte.load(std::memory_order_acquire));
}

// ── Test 3: under-sized fd → ctor throws ──────────────────────────────
//
// Validates that `setup_shm_from_fd_`'s fstat-size check fires.
// Caller must ftruncate the fd to `DataBlockLayout::total_size` BEFORE
// constructing.  An fd one page short → throw with a recognizable
// message.
TEST_F(DataBlockFdBasedTest, CreateFromFd_SizeMismatch_Throws)
{
    DataBlockConfig cfg = make_test_config();
    const size_t total = datablock_layout_total_size(cfg);
    ASSERT_GT(total, 4096u);

    int memfd = ::memfd_create("plh_l2_dbfd_undersize", MFD_CLOEXEC);
    ASSERT_GE(memfd, 0);
    track_fd(memfd);
    ASSERT_EQ(0, ::ftruncate(memfd, static_cast<off_t>(total - 4096))) << "ftruncate failed";

    EXPECT_THROW(
        {
            auto producer = create_datablock_producer_from_fd_impl(
                "fd-undersize-test", memfd, DataBlockPolicy::RingBuffer, cfg,
                /*flexzone_schema=*/nullptr, /*datablock_schema=*/nullptr);
            (void)producer;
        },
        std::runtime_error);
}

} // namespace
