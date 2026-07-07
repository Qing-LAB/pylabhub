/**
 * @file test_zmq_attach_protocol.cpp
 * @brief L2 test for the ZMQ AttachProtocol handshake end-to-end
 *        (Phase 4c minimal — 2026-07-07).
 *
 * Proves the transport-symmetric architecture works: the SAME
 * `run_producer_handshake` / `run_consumer_handshake` helpers that
 * `AttachProtocolAcceptor` uses on SHM also drive
 * `ZmqAttachProtocolAcceptor` on ZMQ ROUTER — with a full Frame 1/2/3
 * challenge-response, real SMS crypto, real KeyStore-seeded
 * identities.
 *
 * Pattern 1+: binary-wide LifecycleGuard for Logger + SMS.  SMS is
 * required for `secure().keys()` — the handshake resolves seckeys by
 * KeyStore entry name (HEP-CORE-0043 §6).
 *
 * Coverage:
 *   - Happy-path handshake (producer + consumer both hold matching keys).
 *   - Mutual-auth Frame 3 (opt-in per §D4.5).
 *   - Consumer proves possession of the seckey it claims via pubkey_z85.
 *   - Rejection when consumer sends a wrong pubkey (impersonator claiming
 *     a pubkey it can't decrypt for).
 */
#include "binary_lifecycle.h"
#include "log_capture_fixture.h"
#include "utils/logger.hpp"
#include "utils/security/attach_protocol.hpp"
#include "utils/security/key_store.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include "cppzmq/zmq.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>

extern "C"
{
char *zmq_z85_encode(char *dest, const uint8_t *data, size_t size);
}

// Binary-wide LifecycleGuard for Logger + SMS.
PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

using pylabhub::utils::security::AttachProtocolTimeout;
using pylabhub::utils::security::ConsumerAuthMaterial;
using pylabhub::utils::security::ObserverPubkeyAccessor;
using pylabhub::utils::security::ProducerHandshakeResult;
using pylabhub::utils::security::ZmqAttachProtocolAcceptor;
using pylabhub::utils::security::initiate_zmq_consumer_handshake;
using pylabhub::utils::security::secure;

namespace
{

constexpr std::size_t kRawKeyBytes = 32;

struct TestIdentity
{
    std::string name;      // KeyStore entry
    std::string pub_z85;   // 40-char Z85 pubkey
};

// Mint + seed a fresh keypair into the process KeyStore under `name`.
TestIdentity
make_and_seed_identity(std::string_view name)
{
    TestIdentity                            id;
    std::array<unsigned char, kRawKeyBytes> pub_raw{};
    std::array<unsigned char, kRawKeyBytes> sec_raw{};
    ::crypto_box_keypair(pub_raw.data(), sec_raw.data());
    char pub_z85[41] = {};
    char sec_z85[41] = {};
    if (::zmq_z85_encode(pub_z85, pub_raw.data(), kRawKeyBytes) == nullptr)
        throw std::runtime_error("make_and_seed_identity: pub encode failed");
    if (::zmq_z85_encode(sec_z85, sec_raw.data(), kRawKeyBytes) == nullptr)
        throw std::runtime_error("make_and_seed_identity: sec encode failed");
    id.name    = std::string(name);
    id.pub_z85 = std::string(pub_z85, 40);
    secure().keys().add_identity_from_z85(id.name, id.pub_z85,
                                           std::string(sec_z85, 40));
    return id;
}

std::string
inproc_endpoint(const char *tag)
{
    static std::atomic<int> ctr{0};
    return "inproc://zmq_attach_protocol_test_" + std::string(tag) + "_" +
           std::to_string(ctr.fetch_add(1));
}

// Read the first frame the DEALER sends to the ROUTER to capture its
// routing identity.  Drains the body too.
std::string
peek_dealer_identity(zmq::socket_t &router)
{
    zmq::message_t rid;
    (void)router.recv(rid);
    zmq::message_t body;
    (void)router.recv(body);
    return std::string(static_cast<const char *>(rid.data()), rid.size());
}

struct DealerRouterPair
{
    zmq::context_t ctx{1};
    zmq::socket_t  router{ctx, zmq::socket_type::router};
    zmq::socket_t  dealer{ctx, zmq::socket_type::dealer};
    std::string    endpoint;
    std::string    dealer_identity;

    explicit DealerRouterPair(const char *tag) : endpoint(inproc_endpoint(tag))
    {
        const int mandatory = 1;
        router.set(zmq::sockopt::router_mandatory, mandatory);
        router.bind(endpoint);
        dealer.connect(endpoint);

        // DEALER sends a wake-up frame so the ROUTER learns its routing
        // identity.  The consumer handshake will then start fresh once
        // the producer sends Frame 1.
        const std::string hello = "wake";
        dealer.send(zmq::message_t(hello.data(), hello.size()),
                    zmq::send_flags::none);
        dealer_identity = peek_dealer_identity(router);
    }
};

class ZmqAttachProtocolTest : public ::testing::Test,
                              public pylabhub::tests::LogCaptureFixture
{
protected:
    void SetUp() override { LogCaptureFixture::Install(); }
    void TearDown() override
    {
        for (const auto &name : seeded_names_)
            secure().keys().remove(name);
        seeded_names_.clear();
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
    }

    TestIdentity
    MakeIdentity(const char *tag)
    {
        static std::atomic<int> ctr{0};
        std::string             name = "test.zmq_attach." + std::string(tag) +
                          "." + std::to_string(ctr.fetch_add(1));
        TestIdentity id = make_and_seed_identity(name);
        seeded_names_.push_back(name);
        return id;
    }

    std::vector<std::string> seeded_names_;
};

} // namespace

// ── happy-path handshake with mutual auth ──────────────────────────────────

TEST_F(ZmqAttachProtocolTest, EndToEndHandshakeMutualAuth)
{
    const auto producer = MakeIdentity("producer");
    const auto consumer = MakeIdentity("consumer");
    DealerRouterPair pair("mutual_auth");

    ZmqAttachProtocolAcceptor acceptor(producer.name, /*obs=*/{});

    // Producer thread — runs the acceptor.
    std::exception_ptr        prod_exc;
    ProducerHandshakeResult   prod_result;
    std::thread prod_thread{[&] {
        try
        {
            prod_result = acceptor.run_handshake(pair.router,
                                                  pair.dealer_identity,
                                                  std::chrono::seconds{5});
        }
        catch (...) { prod_exc = std::current_exception(); }
    }};

    // Consumer runs inline on the test thread.
    ConsumerAuthMaterial self;
    self.role_uid        = "consumer.role.uid";
    self.pubkey_z85      = consumer.pub_z85;
    self.own_seckey_name = consumer.name;

    ASSERT_NO_THROW(initiate_zmq_consumer_handshake(
        pair.dealer, self, producer.pub_z85,
        std::chrono::seconds{5},
        /*require_mutual_auth=*/true));

    prod_thread.join();
    if (prod_exc) std::rethrow_exception(prod_exc);

    EXPECT_EQ(prod_result.consumer_role_uid, "consumer.role.uid");
    EXPECT_EQ(prod_result.consumer_pubkey_z85, consumer.pub_z85);
    EXPECT_EQ(prod_result.role_type, "consumer");
}

// ── consumer identifies with a pubkey it doesn't hold the seckey for ───────

TEST_F(ZmqAttachProtocolTest, RejectsConsumerImpersonatingUnheldPubkey)
{
    const auto producer  = MakeIdentity("producer");
    const auto consumer  = MakeIdentity("consumer");   // real consumer
    const auto imposter  = MakeIdentity("imposter");   // pubkey stolen but not seckey
    DealerRouterPair pair("impersonator");

    ZmqAttachProtocolAcceptor acceptor(producer.name, /*obs=*/{});

    std::exception_ptr        prod_exc;
    std::thread prod_thread{[&] {
        try
        {
            (void)acceptor.run_handshake(pair.router,
                                          pair.dealer_identity,
                                          std::chrono::seconds{2});
        }
        catch (...) { prod_exc = std::current_exception(); }
    }};

    // Consumer sends its OWN seckey (via `consumer.name`) but CLAIMS
    // the imposter's pubkey in `self.pubkey_z85`.  The producer will
    // decrypt the challenge under the CLAIMED pubkey — which doesn't
    // match the consumer's actual seckey — so the MAC fails.
    ConsumerAuthMaterial self;
    self.role_uid        = "imposter.role.uid";
    self.pubkey_z85      = imposter.pub_z85;   // <-- claim
    self.own_seckey_name = consumer.name;      // <-- actual

    // Consumer sends Frame 2 fine.  Producer's decrypt fails → throws
    // → producer's thread completes.  On inproc DEALER↔ROUTER the
    // consumer's own send-then-return doesn't block on any producer
    // acknowledgement (no mutual auth requested), so the consumer
    // call itself should complete cleanly.  Priority 7/D2 tightening:
    // catch only `std::runtime_error` (the documented failure mode)
    // and FAIL on anything else to catch future regressions.
    try
    {
        initiate_zmq_consumer_handshake(
            pair.dealer, self, producer.pub_z85,
            std::chrono::seconds{2},
            /*require_mutual_auth=*/false);
    }
    catch (const std::runtime_error &)
    {
        // Consumer-side ZMQ send may EPIPE if producer already
        // closed the routing identity — acceptable for this test.
    }

    prod_thread.join();
    ASSERT_TRUE(prod_exc) << "producer should reject impersonator with a "
                            "MAC-verify failure";
    try { std::rethrow_exception(prod_exc); }
    catch (const std::runtime_error &e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("challenge-response verification failed"),
                   std::string::npos)
            << "producer error should tag MAC-verify failure; got: " << what;
    }
    catch (...) { FAIL() << "expected runtime_error, got other"; }
}

// ── happy path WITHOUT mutual auth (two-frame flow) ────────────────────────
//
// Priority 4/C1a: exercises the pre-#262 two-frame path so a
// regression that requires Frame 3 unconditionally would fail here
// rather than only in mutual-auth tests.

TEST_F(ZmqAttachProtocolTest, EndToEndHandshakeNoMutualAuth)
{
    const auto producer = MakeIdentity("producer");
    const auto consumer = MakeIdentity("consumer");
    DealerRouterPair pair("no_mutual");

    ZmqAttachProtocolAcceptor acceptor(producer.name, /*obs=*/{});

    std::exception_ptr        prod_exc;
    ProducerHandshakeResult   prod_result;
    std::thread prod_thread{[&] {
        try
        {
            prod_result = acceptor.run_handshake(pair.router,
                                                  pair.dealer_identity,
                                                  std::chrono::seconds{5});
        }
        catch (...) { prod_exc = std::current_exception(); }
    }};

    ConsumerAuthMaterial self;
    self.role_uid        = "consumer.role.uid";
    self.pubkey_z85      = consumer.pub_z85;
    self.own_seckey_name = consumer.name;

    ASSERT_NO_THROW(initiate_zmq_consumer_handshake(
        pair.dealer, self, producer.pub_z85,
        std::chrono::seconds{5},
        /*require_mutual_auth=*/false));

    prod_thread.join();
    if (prod_exc) std::rethrow_exception(prod_exc);

    EXPECT_EQ(prod_result.consumer_role_uid, "consumer.role.uid");
    EXPECT_EQ(prod_result.consumer_pubkey_z85, consumer.pub_z85);
    EXPECT_EQ(prod_result.role_type, "consumer");
}

// Observer role_type path (HEP-CORE-0041 §D1(d)) E2E coverage on ZMQ:
// NOT tested here.  `run_consumer_handshake` hardcodes
// `role_type=consumer` in Frame 2, so exercising the producer's
// observer branch requires either extending `ConsumerAuthMaterial`
// with a `role_type` field or adding a dedicated `initiate_zmq_
// observer_handshake` entry.  Tracked as a follow-up under
// AUTH_TODO Phase 4c-cont.

// ── timeout: producer never sees a peer, times out cleanly ─────────────────
//
// Priority 4/C1c: consumer never sends anything.  Producer's Frame 2
// recv reaches the deadline → AttachProtocolTimeout bubbles out.

TEST_F(ZmqAttachProtocolTest, ProducerTimesOutOnSilentPeer)
{
    const auto producer = MakeIdentity("producer");
    DealerRouterPair pair("timeout");

    ZmqAttachProtocolAcceptor acceptor(producer.name, /*obs=*/{});

    // Producer runs on the test thread with a tight budget; consumer
    // never invokes the handshake, so Frame 2 recv times out.
    EXPECT_THROW(
        (void)acceptor.run_handshake(pair.router, pair.dealer_identity,
                                      std::chrono::milliseconds{200}),
        AttachProtocolTimeout);
}

// Consumer-side Frame 3 rejection (mutual-auth `attach_producer_not_
// authenticated`) is NOT tested here.  The naive scenario — consumer
// supplies the wrong `producer_pubkey_z85` — makes the PRODUCER's
// Frame 2 decrypt fail first (consumer's Frame 2 cipher was encrypted
// against the wrong producer pubkey, so it doesn't decrypt with the
// real producer's seckey), before Frame 3 is ever sent.  Exercising
// consumer-side Frame 3 rejection cleanly requires a
// producer/impersonator with mismatched public/secret keys — a
// bespoke test harness beyond Phase 4c minimal.  Tracked as a
// follow-up under AUTH_TODO Phase 4c-cont.
