/**
 * @file test_shm_capability_channel.cpp
 * @brief L2 tests for the Linux/FreeBSD `memfd_create` backend of the
 *        cross-platform SHM capability transport (HEP-CORE-0041 §6 +
 *        §13 Linux/FreeBSD).
 *
 * Pattern 1 — no LOGGER_*, no FileLock, no lifecycle module.  Uses
 * `memfd_create`, AF_UNIX sockets, and `SCM_RIGHTS` directly; these are
 * bare kernel syscalls and reach no pylabhub lifecycle module.
 *
 * Coverage:
 *   - Compile-time pins on abstract base + non-copyable surface.
 *   - Single-process round-trip: producer creates, binds, accepts;
 *     consumer connects, receives the fd via SCM_RIGHTS, mmaps; both
 *     sides see each other's writes (capability is RW for both).
 *   - SO_PEERCRED on the accepted peer reports the test process's uid.
 *   - `accept_one(timeout)` returns `nullopt` (not throws) on timeout
 *     and respects the timeout interval.
 *   - `attach_shm_capability_consumer` throws on a non-existent
 *     endpoint (connect failure).
 *   - `bind_endpoint("")` returns false at the boundary (no crash).
 *   - `create_shm_capability_producer(0)` throws (zero-size is rejected
 *     at the producer's construction boundary).
 *
 * Test isolation: each TEST_F gets a unique socket path in
 * `/tmp/plh_l2_shmcap_<pid>_<counter>.sock`; the fixture's `TearDown`
 * unlinks them.  Under `-j 2` the counter + pid suffix prevents
 * collisions across concurrent test instances.
 */
#include "utils/security/shm_capability_channel.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace fs = std::filesystem;

using pylabhub::utils::security::IShmCapabilityConsumer;
using pylabhub::utils::security::IShmCapabilityProducer;
using pylabhub::utils::security::attach_shm_capability_consumer;
using pylabhub::utils::security::create_shm_capability_producer;

// ── Compile-time interface pins ─────────────────────────────────────────
//
// These survive the backend swap from substep 1a's throw-stubs because
// they describe the abstract surface, not the implementation behaviour.

static_assert(std::is_abstract_v<IShmCapabilityProducer>,
              "IShmCapabilityProducer must remain a pure abstract base — "
              "concrete backends live behind create_shm_capability_producer "
              "per HEP-CORE-0041 §6.");
static_assert(std::is_abstract_v<IShmCapabilityConsumer>,
              "IShmCapabilityConsumer must remain a pure abstract base.");
static_assert(!std::is_copy_constructible_v<IShmCapabilityProducer>,
              "IShmCapabilityProducer must remain non-copyable.");
static_assert(!std::is_copy_constructible_v<IShmCapabilityConsumer>,
              "IShmCapabilityConsumer must remain non-copyable.");

namespace
{

class ShmCapabilityChannelTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_)
        {
            std::error_code ec;
            fs::remove(p, ec);  // best-effort; producer dtor already unlinks
        }
        paths_.clear();
    }

    std::string
    unique_socket_path(const char *tag)
    {
        static std::atomic<int> ctr{0};
        fs::path                p = fs::temp_directory_path() /
                     ("plh_l2_shmcap_" + std::string(tag) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)) + ".sock");
        paths_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_;
};

} // namespace

// ── Round-trip: writes on either side are visible on the other ──────────

// AcceptedPeer.peer_socket_fd is caller-owned per the L1 interface
// contract — see the header docstring.  The L2 AttachProtocol that
// lands in task #250 enforces close()-after-handoff; this L1 test
// only exercises the transport mechanic, so it closes the fd
// explicitly at the end of the RoundTrip body (no producer-side
// auto-tracking).
TEST_F(ShmCapabilityChannelTest, RoundTrip_ConsumerSeesProducerWrites)
{
    constexpr size_t  kSize     = 4096;
    constexpr uint64_t kSentinel = 0xDEADBEEFCAFEBABEULL;
    constexpr uint64_t kReply    = 0x12345678ABCDEF01ULL;

    const std::string path = unique_socket_path("roundtrip");

    auto producer = create_shm_capability_producer(kSize);
    ASSERT_NE(producer, nullptr);
    ASSERT_EQ(producer->size(), kSize);
    ASSERT_TRUE(producer->bind_endpoint(path));

    auto producer_span = producer->data();
    ASSERT_EQ(producer_span.size(), kSize);
    std::memcpy(producer_span.data(), &kSentinel, sizeof(kSentinel));

    std::unique_ptr<IShmCapabilityConsumer> consumer;
    std::exception_ptr                      consumer_ex;
    std::thread                             consumer_thread{[&] {
        try
        {
            consumer = attach_shm_capability_consumer(
                path, std::chrono::milliseconds{2000});
        }
        catch (...)
        {
            consumer_ex = std::current_exception();
        }
    }};

    auto peer = producer->accept_one(std::chrono::milliseconds{2000});
    ASSERT_TRUE(peer.has_value())
        << "accept_one timed out before the consumer connected";
    EXPECT_GT(peer->peer_socket_fd, 0);
    EXPECT_EQ(peer->uid, ::getuid())
        << "SO_PEERCRED must report the test process's uid in a "
           "single-process round-trip — that's how the L2 sanity check "
           "(HEP-0041 §9 D4 step 3) reads the trust-domain signal.";

    ASSERT_TRUE(producer->send_capability(peer->peer_socket_fd));

    consumer_thread.join();
    ASSERT_FALSE(consumer_ex)
        << "consumer ctor threw — the round-trip flow is broken";
    ASSERT_NE(consumer, nullptr);
    ASSERT_EQ(consumer->size(), kSize)
        << "consumer size must match producer size (fstat must read the "
           "ftruncate'd length)";

    auto consumer_span = consumer->data();
    ASSERT_EQ(consumer_span.size(), kSize);
    uint64_t received{};
    std::memcpy(&received, consumer_span.data(), sizeof(received));
    EXPECT_EQ(received, kSentinel)
        << "consumer's mapping must show the bytes the producer wrote — "
           "the SCM_RIGHTS fd is the same kernel object on both sides";

    // Reverse direction: consumer writes, producer reads.  Pins that the
    // capability is RW for both sides (not split as producer-write /
    // consumer-read — the DataBlock ring-buffer protocol on top needs RW).
    std::memcpy(consumer_span.data() + 64, &kReply, sizeof(kReply));
    uint64_t roundtrip{};
    std::memcpy(&roundtrip, producer_span.data() + 64, sizeof(roundtrip));
    EXPECT_EQ(roundtrip, kReply)
        << "producer must see writes the consumer makes — RW capability";

    // Caller owns peer->peer_socket_fd per the AcceptedPeer ownership
    // contract; close it before the producer goes out of scope.
    ::close(peer->peer_socket_fd);
}

// ── Timeout semantics ───────────────────────────────────────────────────

TEST_F(ShmCapabilityChannelTest, AcceptOneReturnsNulloptOnTimeout)
{
    const std::string path = unique_socket_path("timeout");
    auto              producer = create_shm_capability_producer(1024);
    ASSERT_TRUE(producer->bind_endpoint(path));

    const auto start   = std::chrono::steady_clock::now();
    auto       peer    = producer->accept_one(std::chrono::milliseconds{50});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(peer.has_value())
        << "accept_one must return nullopt — not throw — when no consumer "
           "connects within the timeout window";
    EXPECT_GE(elapsed, std::chrono::milliseconds{40})
        << "accept_one must wait approximately the full timeout, not "
           "return immediately";
    EXPECT_LT(elapsed, std::chrono::milliseconds{500})
        << "accept_one must return promptly after timeout, not block "
           "indefinitely";
}

// ── Connect-failure path ────────────────────────────────────────────────

TEST_F(ShmCapabilityChannelTest, ConsumerThrowsOnNonexistentEndpoint)
{
    // A pseudo-random suffix in the path keeps this test independent of
    // any concurrent test that might use the same naming scheme.
    const std::string nonexistent =
        "/tmp/plh_no_such_socket_for_test_" + std::to_string(::getpid()) +
        "_a8f3.sock";
    EXPECT_THROW(
        {
            auto c = attach_shm_capability_consumer(
                nonexistent, std::chrono::milliseconds{100});
            (void) c;
        },
        std::runtime_error);
}

// ── Input-validation at the boundary ────────────────────────────────────

TEST_F(ShmCapabilityChannelTest, BindEndpointReturnsFalseOnEmptyPath)
{
    auto producer = create_shm_capability_producer(1024);
    EXPECT_FALSE(producer->bind_endpoint(""))
        << "Empty paths are rejected at the API boundary — no crash, no "
           "throw, just a documented false return.";
}

TEST_F(ShmCapabilityChannelTest, ProducerCtorRejectsZeroSize)
{
    EXPECT_THROW(
        {
            auto p = create_shm_capability_producer(0);
            (void) p;
        },
        std::invalid_argument);
}

// ── Double-bind contract ────────────────────────────────────────────────

TEST_F(ShmCapabilityChannelTest, BindEndpointTwiceFails)
{
    const std::string path1 = unique_socket_path("doublebind_a");
    const std::string path2 = unique_socket_path("doublebind_b");

    auto producer = create_shm_capability_producer(1024);
    ASSERT_TRUE(producer->bind_endpoint(path1));
    EXPECT_FALSE(producer->bind_endpoint(path2))
        << "bind_endpoint is one-shot per instance; the second call must "
           "refuse rather than silently rebind onto a different path.";
}

// ── HEP-CORE-0041 §5.1 URI scheme: bind + dial accept `unix://...` ──────
//
// Production callers compute the endpoint via
// `default_shm_capability_endpoint`, which returns a `unix://<path>`
// URI (mirroring ZMQ's `tcp://`/`ipc://` shape; see header §5.1).  The
// L1 backend strips the scheme before the kernel syscall.  This test
// pins that contract end-to-end on both bind side and consumer-dial
// side — failure mode if the strip is missing is a literal
// `unix:///run/...` file written to the parent dir and `connect(2)`
// failing on the consumer.  Regression coverage for the 2026-06-22
// systematic-review B1 finding (substep 1g shipped the URI helper
// but not the matching stripper, and tests passed raw paths so the
// gap was invisible).

TEST_F(ShmCapabilityChannelTest, BindAndDialAcceptUnixSchemeURI)
{
    constexpr size_t   kSize = 4096;
    const std::string  path  = unique_socket_path("uri_scheme");
    const std::string  uri   = "unix://" + path;

    auto producer = create_shm_capability_producer(kSize);
    ASSERT_TRUE(producer->bind_endpoint(uri))
        << "bind_endpoint must accept the canonical `unix://` URI "
           "shape — HEP-CORE-0041 §5.1.";

    EXPECT_TRUE(fs::exists(path))
        << "bind must create the socket file at the STRIPPED path, "
           "not at the literal `unix://<path>` string.";
    EXPECT_FALSE(fs::exists("unix:" + path))
        << "No file with a `unix:` prefix component should exist — "
           "that would be the unstripped-URI regression.";

    // Consumer dial — also via URI.
    std::unique_ptr<IShmCapabilityConsumer> consumer;
    std::exception_ptr                      consumer_ex;
    std::thread                             consumer_thread{[&] {
        try
        {
            consumer = attach_shm_capability_consumer(
                uri, std::chrono::milliseconds{2000});
        }
        catch (...)
        {
            consumer_ex = std::current_exception();
        }
    }};

    auto peer = producer->accept_one(std::chrono::milliseconds{2000});
    ASSERT_TRUE(peer.has_value());
    ASSERT_TRUE(producer->send_capability(peer->peer_socket_fd));

    consumer_thread.join();
    ASSERT_FALSE(consumer_ex)
        << "attach_shm_capability_consumer must dial via the same URI "
           "shape the producer published — both ends strip identically.";
    ASSERT_NE(consumer, nullptr);
    EXPECT_EQ(consumer->size(), kSize);

    ::close(peer->peer_socket_fd);
}

// ── HEP-CORE-0041 §5.1 parent-dir mkdir contract ────────────────────────
//
// The L1 backend takes responsibility for creating the parent dir at
// mode 0700 before bind (see header line 310).  Regression coverage
// for the 2026-06-22 systematic-review B2 finding (docstring at the
// helper said "caller responsibility" but no caller did it, so the
// first production bind would hit ENOENT on a host with
// `${XDG_RUNTIME_DIR}` set but no pre-existing `pylabhub/` subdir).

TEST_F(ShmCapabilityChannelTest, BindCreatesMissingParentDirectory)
{
    constexpr size_t kSize = 1024;
    // Generate a nested parent dir that does not exist yet.
    const fs::path nested_parent =
        fs::temp_directory_path() /
        ("plh_l2_shmcap_mkdir_" + std::to_string(::getpid()) + "_" +
         std::to_string(::getpid()));
    paths_.push_back(nested_parent);  // best-effort cleanup
    const std::string sock = (nested_parent / "the.sock").string();
    paths_.push_back(sock);

    ASSERT_FALSE(fs::exists(nested_parent))
        << "test pre-condition: parent dir must not exist before bind";

    auto producer = create_shm_capability_producer(kSize);
    ASSERT_TRUE(producer->bind_endpoint(sock))
        << "bind_endpoint must mkdir -p the parent dir before bind(2) "
           "— HEP-CORE-0041 §5.1.";
    EXPECT_TRUE(fs::exists(nested_parent));
    EXPECT_TRUE(fs::exists(sock));

    // Parent dir must be mode 0700 (owner-only); HEP-0041 §5.1
    // hardening contract.
    const fs::file_status st = fs::status(nested_parent);
    const fs::perms       owner_all =
        fs::perms::owner_read | fs::perms::owner_write |
        fs::perms::owner_exec;
    const fs::perms group_others_mask =
        fs::perms::group_all | fs::perms::others_all;
    EXPECT_EQ(st.permissions() & owner_all, owner_all)
        << "owner-all bits must be set on the parent dir.";
    EXPECT_EQ(st.permissions() & group_others_mask, fs::perms::none)
        << "group/others bits must be cleared — defence in depth.";

    // Best-effort cleanup; remove socket then dir.
    std::error_code ec;
    fs::remove(sock, ec);
    fs::remove(nested_parent, ec);
}

// ── HEP-CORE-0041 1i-prod-hardening H3d: probe-then-unlink ─────────────
//
// Pre-H3d `bind_endpoint` unconditionally `unlink`'d the path before
// `bind`.  Two role hosts that picked the same channel name would
// silently steal the socket from each other (same-uid race;
// SO_PEERCRED can't protect within one uid).  H3d adds a connect()
// probe: if a live peer is on the path, refuse to clobber.

TEST_F(ShmCapabilityChannelTest, BindRefusesWhenLivePeerHoldsThePath)
{
    const std::string path = unique_socket_path("h3d_live");

    // First producer takes the path successfully.
    auto first  = create_shm_capability_producer(1024);
    ASSERT_TRUE(first->bind_endpoint(path));

    // Second producer attempts the same path — the H3d probe must
    // connect() successfully (first is listening) and refuse to
    // unlink + rebind.  Pre-H3d this would silently succeed and
    // orphan first's listen_fd_.
    auto second = create_shm_capability_producer(1024);
    EXPECT_FALSE(second->bind_endpoint(path))
        << "bind_endpoint must refuse to clobber a live peer's socket "
           "(HEP-CORE-0041 1i-prod-hardening H3d probe-then-unlink).";
}

TEST_F(ShmCapabilityChannelTest, BindCleansUpStaleSocketFile)
{
    const std::string path = unique_socket_path("h3d_stale");

    // Manually create a stale AF_UNIX socket file at the path with NO
    // listener bound.  This simulates the "previous process crashed
    // without unlink" scenario H3d still needs to handle gracefully.
    {
        const int sk = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_NE(sk, -1);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        ASSERT_LT(path.size(), sizeof(addr.sun_path));
        std::memcpy(addr.sun_path, path.c_str(), path.size());
        ASSERT_EQ(::bind(sk, reinterpret_cast<const sockaddr *>(&addr),
                          sizeof(addr)),
                  0);
        // CLOSE without listen() — connect() to this path returns
        // ECONNREFUSED (file exists, no listener accepts).
        ::close(sk);
    }

    // bind_endpoint should detect ECONNREFUSED, recognize the stale
    // file, unlink it, and rebind successfully.
    auto producer = create_shm_capability_producer(1024);
    EXPECT_TRUE(producer->bind_endpoint(path))
        << "bind_endpoint must unlink + rebind when the path exists "
           "but no listener is there (stale socket from prior crash; "
           "HEP-CORE-0041 1i-prod-hardening H3d ECONNREFUSED branch).";
}
