/**
 * @file test_shm_attach_orchestrator.cpp
 * @brief L2 tests for `ShmAttachOrchestrator` (HEP-CORE-0041 §9 D4
 *        steps 6-7, substep 1e).
 *
 * Pattern 1+ — binary-wide `LifecycleGuard` for `Logger` via
 * `BinaryLifecycleEnvironment`.  `ShmAttachOrchestrator::accept_and_serve_one`
 * emits `LOGGER_INFO` / `LOGGER_WARN` for the divergence-WARN table,
 * authorize / deny outcomes, and fail-closed paths; Logger must be
 * initialized.  No cross-test state — per-test sockets + per-test
 * keypairs + per-test `LogCaptureFixture::Install/Uninstall`.
 *
 * Covers the four divergence-WARN cells of the HEP-0041 §9 D4 table
 * plus the fail-closed paths:
 *   - both allowed                        → silent INFO + Sent
 *   - both denied                         → silent INFO + DeniedByBroker
 *   - broker allowed + cache denied       → WARN divergence + Sent
 *   - broker denied  + cache allowed      → WARN divergence + DeniedByBroker
 *   - broker_query returns nullopt        → WARN + DeniedTransportFail
 *   - broker reply has unexpected status  → WARN + DeniedTransportFail
 *   - AttachProtocol handshake fails      → WARN + HandshakeFailed
 *   - timeout (no consumer)               → Timeout (no logs)
 */
#include "binary_lifecycle.h"
#include "log_capture_fixture.h"
#include "utils/logger.hpp"
#include "utils/security/attach_protocol.hpp"
#include "utils/security/shm_attach_orchestrator.hpp"
#include "utils/security/shm_capability_channel.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C"
{
char *zmq_z85_encode(char *dest, const uint8_t *data, size_t size);
}

namespace fs = std::filesystem;

using pylabhub::utils::security::AttachProtocolAcceptor;
using pylabhub::utils::security::ConsumerAuthMaterial;
using pylabhub::utils::security::IShmCapabilityProducer;
using pylabhub::utils::security::SeckeyAccessor;
using pylabhub::utils::security::ShmAttachOrchestrator;
using pylabhub::utils::security::create_shm_capability_producer;
using pylabhub::utils::security::initiate_consumer_handshake;

// Pattern 1+: binary-wide LifecycleGuard for Logger (orchestrator emits
// LOGGER_INFO/WARN).
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule())

namespace
{

constexpr std::size_t kRawKeyBytes = 32;

struct TestKeypair
{
    std::array<unsigned char, kRawKeyBytes> pub_raw{};
    std::array<unsigned char, kRawKeyBytes> sec_raw{};
    std::string                             pub_z85;
};

TestKeypair
make_test_keypair()
{
    // No self-init: sodium_init is SecureMemorySubsystem's job.
    // Test fixture must construct SMS before reaching here.
    TestKeypair kp;
    ::crypto_box_keypair(kp.pub_raw.data(), kp.sec_raw.data());
    char z85[41] = {};
    if (::zmq_z85_encode(z85, kp.pub_raw.data(), kRawKeyBytes) == nullptr)
        throw std::runtime_error("make_test_keypair: zmq_z85_encode failed");
    kp.pub_z85 = std::string(z85, 40);
    return kp;
}

SeckeyAccessor
make_seckey_accessor(const TestKeypair &kp)
{
    return [&kp](auto use_sk) {
        use_sk(std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(kp.sec_raw.data()),
            kRawKeyBytes});
    };
}

class ShmAttachOrchestratorTest : public ::testing::Test,
                                  public pylabhub::tests::LogCaptureFixture
{
  protected:
    void SetUp() override { LogCaptureFixture::Install(); }
    void TearDown() override
    {
        for (const auto &p : paths_)
        {
            std::error_code ec;
            fs::remove(p, ec);
        }
        paths_.clear();
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
    }

    std::string
    unique_socket_path(const char *tag)
    {
        static std::atomic<int> ctr{0};
        fs::path                p = fs::temp_directory_path() /
                     ("plh_l2_orch_" + std::string(tag) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)) + ".sock");
        paths_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_;
};

// Helper: run the consumer side of the handshake in a thread.  After
// the handshake, attempts to recvmsg for the SCM_RIGHTS capability —
// matching what substep 1f's consumer-side library will eventually do
// — so the producer's send_capability has a peer to deliver to.  In
// denied scenarios the producer closes the socket without sending; the
// recv returns EOF; we close and move on.  Sets SO_RCVTIMEO so the
// recv doesn't block past the test's overall timeout.
std::thread
spawn_consumer_thread(const std::string &endpoint, const TestKeypair &cons_kp,
                      const std::string &cons_uid,
                      const std::string &prod_pubkey_z85,
                      std::exception_ptr &out_exc)
{
    return std::thread{[=, &cons_kp, &out_exc] {
        try
        {
            ConsumerAuthMaterial cons_auth{cons_uid, cons_kp.pub_z85,
                                           make_seckey_accessor(cons_kp)};
            auto fd = initiate_consumer_handshake(endpoint, cons_auth,
                                                  prod_pubkey_z85,
                                                  std::chrono::milliseconds{2000});
            if (!fd || *fd < 0)
                return;

            // Wait briefly for the producer's SCM_RIGHTS send (or for
            // the producer to close the socket on a denied path).
            // SO_RCVTIMEO bounds the wait so tests can't hang here.
            timeval tv{};
            tv.tv_sec  = 0;
            tv.tv_usec = 500000; // 500ms
            ::setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char         iov_byte = 0;
            iovec        iov{&iov_byte, 1};
            union
            {
                char     buf[CMSG_SPACE(sizeof(int))];
                cmsghdr  align;
            } u{};
            std::memset(u.buf, 0, sizeof(u.buf));
            msghdr msg{};
            msg.msg_iov        = &iov;
            msg.msg_iovlen     = 1;
            msg.msg_control    = u.buf;
            msg.msg_controllen = sizeof(u.buf);

            const ssize_t r = ::recvmsg(*fd, &msg, 0);
            (void) r; // happy with success, EOF, or timeout — all benign

            // Close any received fd so the kernel can free the SHM
            // backing the test capability.
            cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
            if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type == SCM_RIGHTS &&
                cmsg->cmsg_len == CMSG_LEN(sizeof(int)))
            {
                int received_fd = -1;
                std::memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
                if (received_fd >= 0)
                    ::close(received_fd);
            }
            ::close(*fd);
        }
        catch (...)
        {
            out_exc = std::current_exception();
        }
    }};
}

} // namespace

// ── Healthy: both sides agree (allowed) → silent INFO + Sent ──────────────

TEST_F(ShmAttachOrchestratorTest, BothAllowed_SilentAndSent)
{
    const auto prod = make_test_keypair();
    const auto cons = make_test_keypair();
    const auto path = unique_socket_path("both_allow");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    ShmAttachOrchestrator::Config cfg{
        "test.channel.both_allow",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) { return true; },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> {
            nlohmann::json r;
            r["status"] = "success";
            return r;
        }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    std::exception_ptr cons_exc;
    auto               cons_thread =
        spawn_consumer_thread(path, cons, "consumer.test.uid", prod.pub_z85,
                              cons_exc);

    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{2000});
    cons_thread.join();

    ASSERT_FALSE(cons_exc);
    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::Sent);
    // No WARN must fire — assertion in TearDown via AssertNoUnexpectedLogWarnError.
}

// ── Healthy: both sides agree (denied) → silent INFO + DeniedByBroker ─────

TEST_F(ShmAttachOrchestratorTest, BothDenied_SilentAndDeniedByBroker)
{
    const auto prod = make_test_keypair();
    const auto cons = make_test_keypair();
    const auto path = unique_socket_path("both_deny");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    ShmAttachOrchestrator::Config cfg{
        "test.channel.both_deny",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) { return false; },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> {
            nlohmann::json r;
            r["status"]        = "denied";
            r["denial_reason"] = "not_in_allowlist";
            return r;
        }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    std::exception_ptr cons_exc;
    auto cons_thread = spawn_consumer_thread(path, cons, "consumer.test.uid",
                                             prod.pub_z85, cons_exc);

    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{2000});
    cons_thread.join();

    ASSERT_FALSE(cons_exc);
    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::DeniedByBroker);
}

// ── Divergence: broker allowed + cache denied → WARN + Sent ───────────────

TEST_F(ShmAttachOrchestratorTest, BrokerAllowedCacheDenied_WarnAndSent)
{
    const auto prod = make_test_keypair();
    const auto cons = make_test_keypair();
    const auto path = unique_socket_path("div_admit");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    ShmAttachOrchestrator::Config cfg{
        "test.channel.div_admit",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) { return false; },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> {
            nlohmann::json r;
            r["status"] = "success";
            return r;
        }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    // Expect the divergence WARN explicitly.  AssertNoUnexpectedLogWarnError
    // in TearDown will fail if anything else fires.
    ExpectLogWarnMustFire("divergence broker=allowed cache=denied");

    std::exception_ptr cons_exc;
    auto cons_thread = spawn_consumer_thread(path, cons, "consumer.test.uid",
                                             prod.pub_z85, cons_exc);

    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{2000});
    cons_thread.join();

    ASSERT_FALSE(cons_exc);
    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::Sent);
}

// ── Divergence: broker denied + cache allowed → WARN + DeniedByBroker ─────

TEST_F(ShmAttachOrchestratorTest, BrokerDeniedCacheAllowed_WarnAndDenied)
{
    const auto prod = make_test_keypair();
    const auto cons = make_test_keypair();
    const auto path = unique_socket_path("div_deny");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    ShmAttachOrchestrator::Config cfg{
        "test.channel.div_deny",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) { return true; },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> {
            nlohmann::json r;
            r["status"]        = "denied";
            r["denial_reason"] = "revoked";
            return r;
        }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    ExpectLogWarnMustFire("divergence broker=denied cache=allowed");

    std::exception_ptr cons_exc;
    auto cons_thread = spawn_consumer_thread(path, cons, "consumer.test.uid",
                                             prod.pub_z85, cons_exc);

    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{2000});
    cons_thread.join();

    ASSERT_FALSE(cons_exc);
    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::DeniedByBroker);
}

// ── Broker comm failure (nullopt) → WARN + DeniedTransportFail ────────────

TEST_F(ShmAttachOrchestratorTest, BrokerNullopt_FailsClosed)
{
    const auto prod = make_test_keypair();
    const auto cons = make_test_keypair();
    const auto path = unique_socket_path("brk_nullopt");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    ShmAttachOrchestrator::Config cfg{
        "test.channel.brk_nullopt",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) { return true; },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> { return std::nullopt; }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    ExpectLogWarnMustFire("broker_query returned nullopt");

    std::exception_ptr cons_exc;
    auto cons_thread = spawn_consumer_thread(path, cons, "consumer.test.uid",
                                             prod.pub_z85, cons_exc);

    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{2000});
    cons_thread.join();

    ASSERT_FALSE(cons_exc);
    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::DeniedTransportFail);
}

// ── Broker reply with unexpected status → WARN + DeniedTransportFail ──────

TEST_F(ShmAttachOrchestratorTest, BrokerUnexpectedStatus_FailsClosed)
{
    const auto prod = make_test_keypair();
    const auto cons = make_test_keypair();
    const auto path = unique_socket_path("brk_bad_status");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    ShmAttachOrchestrator::Config cfg{
        "test.channel.brk_bad_status",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) { return true; },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> {
            nlohmann::json r;
            r["status"] = "error";  // not success/denied
            return r;
        }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    ExpectLogWarnMustFire("broker reply has unexpected status='error'");

    std::exception_ptr cons_exc;
    auto cons_thread = spawn_consumer_thread(path, cons, "consumer.test.uid",
                                             prod.pub_z85, cons_exc);

    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{2000});
    cons_thread.join();

    ASSERT_FALSE(cons_exc);
    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::DeniedTransportFail);
}

// ── AttachProtocol handshake failure (impersonator) → HandshakeFailed ─────

TEST_F(ShmAttachOrchestratorTest, HandshakeFailure_FromImpersonator)
{
    const auto prod      = make_test_keypair();
    const auto k_legit   = make_test_keypair();   // claimed pubkey
    const auto k_attacker = make_test_keypair();  // actual seckey used
    const auto path      = unique_socket_path("hs_fail");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    bool                          broker_query_invoked = false;
    ShmAttachOrchestrator::Config cfg{
        "test.channel.hs_fail",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) { return true; },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> {
            broker_query_invoked = true;
            nlohmann::json r;
            r["status"] = "success";
            return r;
        }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    ExpectLogWarnMustFire("handshake failed channel='test.channel.hs_fail'");

    // Impersonator: encrypt under k_attacker.sec but claim k_legit.pub.
    std::exception_ptr cons_exc;
    std::thread        cons_thread{[&] {
        try
        {
            ConsumerAuthMaterial bogus{"impersonator.test", k_legit.pub_z85,
                                       make_seckey_accessor(k_attacker)};
            auto fd = initiate_consumer_handshake(
                path, bogus, prod.pub_z85, std::chrono::milliseconds{2000});
            if (fd && *fd >= 0)
                ::close(*fd);
        }
        catch (...)
        {
            cons_exc = std::current_exception();
        }
    }};

    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{2000});
    cons_thread.join();

    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::HandshakeFailed);
    EXPECT_FALSE(broker_query_invoked)
        << "broker MUST NOT be queried when the L2 handshake fails";
}

// ── Timeout: no consumer connects → Timeout (no logs) ─────────────────────

TEST_F(ShmAttachOrchestratorTest, TimeoutReturnsTimeout)
{
    const auto prod = make_test_keypair();
    const auto path = unique_socket_path("timeout");

    auto transport = create_shm_capability_producer(4096);
    ASSERT_TRUE(transport->bind_endpoint(path));

    AttachProtocolAcceptor acceptor{*transport, ::getuid(),
                                    make_seckey_accessor(prod)};

    bool broker_query_invoked = false;
    bool cache_lookup_invoked = false;
    ShmAttachOrchestrator::Config cfg{
        "test.channel.timeout",
        "producer.test.uid",
        /*cache_lookup=*/[&](const std::string &) {
            cache_lookup_invoked = true;
            return true;
        },
        /*broker_query=*/
        [&](const std::string &, const std::string &)
            -> std::optional<nlohmann::json> {
            broker_query_invoked = true;
            return std::nullopt;
        }};
    ShmAttachOrchestrator orch{acceptor, *transport, std::move(cfg)};

    const auto start = std::chrono::steady_clock::now();
    const auto outcome =
        orch.accept_and_serve_one(std::chrono::milliseconds{50});
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    EXPECT_EQ(outcome, ShmAttachOrchestrator::Outcome::Timeout);
    EXPECT_GE(elapsed, 40);
    EXPECT_LT(elapsed, 500);
    EXPECT_FALSE(broker_query_invoked);
    EXPECT_FALSE(cache_lookup_invoked);
}
