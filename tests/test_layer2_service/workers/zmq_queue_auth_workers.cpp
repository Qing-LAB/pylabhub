/**
 * @file zmq_queue_auth_workers.cpp
 * @brief Worker bodies — ZmqQueue CURVE+ZAP auth path (PeerAdmission Phase C).
 *
 * Drives the full integration: producer-side `push_to_with_auth` (PUSH
 * + CURVE_SERVER + zap_domain registered with ZapRouter), consumer-
 * side `pull_from_with_auth` (PULL + CURVE_CLIENT with the producer's
 * serverkey), real TCP loopback handshake, message round-trip iff
 * the allowlist admits the consumer's pubkey.
 *
 * Pattern 3 (subprocess per scenario, LifecycleGuard owned by worker).
 */
#include "utils/file_lock.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/json_config.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/schema_field_layout.hpp"
#include "utils/security/peer_admission.hpp"
#include "utils/security/zap_router.hpp"
#include "utils/zmq_context.hpp"

#include <zmq.h>

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;
using pylabhub::utils::security::PeerAllowlist;
using pylabhub::utils::security::PeerIdentity;
using pylabhub::utils::security::ZapPumpThread;

using pylabhub::hub::QueueReader;
using pylabhub::hub::QueueWriter;
using pylabhub::hub::SchemaFieldDesc;
using pylabhub::hub::ZmqAuthOptions;
using pylabhub::hub::ZmqQueue;

namespace
{

constexpr std::size_t kPubkeyZ85Chars = 40;

std::pair<std::string, std::string> make_keypair()
{
    std::array<char, kPubkeyZ85Chars + 1> pub{};
    std::array<char, kPubkeyZ85Chars + 1> sec{};
    if (::zmq_curve_keypair(pub.data(), sec.data()) != 0)
        throw std::runtime_error("zmq_curve_keypair failed");
    return {std::string(pub.data(), kPubkeyZ85Chars),
            std::string(sec.data(), kPubkeyZ85Chars)};
}

/// Simple uint32 schema used by all worker tests — sufficient to
/// exercise the message round-trip; the schema layer is not the
/// subject of this test.
std::vector<SchemaFieldDesc> make_uint32_schema()
{
    SchemaFieldDesc f;
    f.type_str = "uint32";
    f.count    = 1;
    f.length   = 0;
    return {f};
}

bool wait_for_one_uint32(ZmqQueue *consumer, uint32_t expected,
                          std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const void *buf = consumer->read_acquire(
            std::chrono::milliseconds(50));
        if (buf == nullptr) continue;
        uint32_t got{};
        std::memcpy(&got, buf, sizeof(got));
        consumer->read_release();
        return got == expected;
    }
    return false;
}

bool try_send_uint32(ZmqQueue *producer, uint32_t value)
{
    void *buf = producer->write_acquire(std::chrono::milliseconds(200));
    if (buf == nullptr) return false;
    std::memcpy(buf, &value, sizeof(value));
    producer->write_commit();
    return true;
}

// ── Scenarios ──────────────────────────────────────────────────────────────

int auth_round_trip_allowed_peer_delivers(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [consumer_pub, consumer_sec] = make_keypair();

            // Producer side: bind, CURVE_SERVER, ZAP-gated.
            ZmqAuthOptions producer_auth;
            producer_auth.my_pubkey_z85 = producer_pub;
            producer_auth.my_seckey_z85 = producer_sec;
            producer_auth.zap_domain    = "test.zmq.auth.roundtrip";
            producer_auth.initial_allowlist.peers.insert(
                PeerIdentity{"curve", consumer_pub});

            auto producer = ZmqQueue::push_to_with_auth(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                std::move(producer_auth), /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            ASSERT_TRUE(producer->start());

            // Pump ZAP from a side thread.
            ZapPumpThread pump;

            // Consumer side: connect with CURVE_CLIENT.
            ZmqAuthOptions consumer_auth;
            consumer_auth.my_pubkey_z85 = consumer_pub;
            consumer_auth.my_seckey_z85 = consumer_sec;
            consumer_auth.serverkey_z85 = producer_pub;

            auto consumer = ZmqQueue::pull_from_with_auth(
                producer->actual_endpoint(), make_uint32_schema(),
                "aligned", std::move(consumer_auth),
                /*bind=*/false);
            ASSERT_NE(consumer, nullptr);
            ASSERT_TRUE(consumer->start());

            EXPECT_TRUE(producer->admission_is_enforced());
            EXPECT_TRUE(consumer->admission_is_enforced());

            ASSERT_TRUE(try_send_uint32(producer.get(), 0xDEADBEEFu));
            EXPECT_TRUE(wait_for_one_uint32(
                consumer.get(), 0xDEADBEEFu,
                std::chrono::milliseconds(2000)));

            consumer->stop();
            producer->stop();
        },
        "zmq_queue_auth::auth_round_trip_allowed_peer_delivers",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int auth_unallowed_peer_blocked(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [allowed_pub,  allowed_sec]  = make_keypair();
            const auto [denied_pub,   denied_sec]   = make_keypair();

            ZmqAuthOptions producer_auth;
            producer_auth.my_pubkey_z85 = producer_pub;
            producer_auth.my_seckey_z85 = producer_sec;
            producer_auth.zap_domain    = "test.zmq.auth.denied";
            producer_auth.initial_allowlist.peers.insert(
                PeerIdentity{"curve", allowed_pub});
            // NB: denied_pub is NOT in the allowlist.

            auto producer = ZmqQueue::push_to_with_auth(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                std::move(producer_auth), /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            ASSERT_TRUE(producer->start());

            ZapPumpThread pump;

            ZmqAuthOptions denied_auth;
            denied_auth.my_pubkey_z85 = denied_pub;
            denied_auth.my_seckey_z85 = denied_sec;
            denied_auth.serverkey_z85 = producer_pub;

            auto denied_consumer = ZmqQueue::pull_from_with_auth(
                producer->actual_endpoint(), make_uint32_schema(),
                "aligned", std::move(denied_auth), /*bind=*/false);
            ASSERT_NE(denied_consumer, nullptr);
            ASSERT_TRUE(denied_consumer->start());

            (void) try_send_uint32(producer.get(), 0x12345678u);
            EXPECT_FALSE(wait_for_one_uint32(
                denied_consumer.get(), 0x12345678u,
                std::chrono::milliseconds(500)));

            denied_consumer->stop();
            producer->stop();
        },
        "zmq_queue_auth::auth_unallowed_peer_blocked",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int auth_allowlist_swap_takes_effect_for_next_connection(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [alice_pub, alice_sec]       = make_keypair();
            const auto [bob_pub,   bob_sec]         = make_keypair();

            ZmqAuthOptions producer_auth;
            producer_auth.my_pubkey_z85 = producer_pub;
            producer_auth.my_seckey_z85 = producer_sec;
            producer_auth.zap_domain    = "test.zmq.auth.swap";
            // Initially allow alice only.
            producer_auth.initial_allowlist.peers.insert(
                PeerIdentity{"curve", alice_pub});

            auto producer = ZmqQueue::push_to_with_auth(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                std::move(producer_auth), /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            ASSERT_TRUE(producer->start());

            ZapPumpThread pump;

            // Initial allowlist snapshot reflects alice.
            auto snap = producer->peer_allowlist_snapshot();
            ASSERT_TRUE(snap.has_value());
            EXPECT_TRUE(snap->contains(PeerIdentity{"curve", alice_pub}));
            EXPECT_FALSE(snap->contains(PeerIdentity{"curve", bob_pub}));

            // Swap to bob-only via set_peer_allowlist.
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", bob_pub});
            ASSERT_TRUE(producer->set_peer_allowlist(al));

            snap = producer->peer_allowlist_snapshot();
            ASSERT_TRUE(snap.has_value());
            EXPECT_TRUE(snap->contains(PeerIdentity{"curve", bob_pub}));
            EXPECT_FALSE(snap->contains(PeerIdentity{"curve", alice_pub}));

            // Bob can now connect.
            ZmqAuthOptions bob_auth;
            bob_auth.my_pubkey_z85 = bob_pub;
            bob_auth.my_seckey_z85 = bob_sec;
            bob_auth.serverkey_z85 = producer_pub;

            auto bob_consumer = ZmqQueue::pull_from_with_auth(
                producer->actual_endpoint(), make_uint32_schema(),
                "aligned", std::move(bob_auth), /*bind=*/false);
            ASSERT_NE(bob_consumer, nullptr);
            ASSERT_TRUE(bob_consumer->start());

            ASSERT_TRUE(try_send_uint32(producer.get(), 0xCAFEBABEu));
            EXPECT_TRUE(wait_for_one_uint32(
                bob_consumer.get(), 0xCAFEBABEu,
                std::chrono::milliseconds(2000)));

            bob_consumer->stop();
            producer->stop();
        },
        "zmq_queue_auth::auth_allowlist_swap_takes_effect_for_next_connection",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int legacy_unauth_factories_unchanged(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]() {
            // Plain producer (no CURVE), connect to plain consumer.
            // Verifies the legacy factories are unaffected by Phase C.
            auto producer = ZmqQueue::push_to(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            ASSERT_TRUE(producer->start());

            // Downcast just to call admission_is_enforced for the
            // sanity check.  This is what Phase D will need to do
            // through a separate stash field.
            auto *producer_zq = dynamic_cast<ZmqQueue *>(producer.get());
            ASSERT_NE(producer_zq, nullptr);
            EXPECT_FALSE(producer_zq->admission_is_enforced());

            auto consumer = ZmqQueue::pull_from(
                producer_zq->actual_endpoint(),
                make_uint32_schema(), "aligned",
                /*bind=*/false);
            ASSERT_NE(consumer, nullptr);
            ASSERT_TRUE(consumer->start());

            void *buf = producer->write_acquire(
                std::chrono::milliseconds(200));
            ASSERT_NE(buf, nullptr);
            const uint32_t value = 0xABCD1234u;
            std::memcpy(buf, &value, sizeof(value));
            producer->write_commit();

            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(2000);
            bool got = false;
            while (std::chrono::steady_clock::now() < deadline)
            {
                const void *rbuf = consumer->read_acquire(
                    std::chrono::milliseconds(50));
                if (rbuf == nullptr) continue;
                uint32_t v{};
                std::memcpy(&v, rbuf, sizeof(v));
                consumer->read_release();
                got = (v == value);
                break;
            }
            EXPECT_TRUE(got);

            consumer->stop();
            producer->stop();
        },
        "zmq_queue_auth::legacy_unauth_factories_unchanged",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ── Dispatcher + registrar ─────────────────────────────────────────────────

int dispatch_zmq_queue_auth(int argc, char **argv)
{
    if (argc < 2) return -1;
    std::string mode = argv[1];
    const auto dot = mode.find('.');
    if (dot == std::string::npos) return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "zmq_queue_auth") return -1;

    if (argc < 3)
    {
        std::fprintf(stderr, "zmq_queue_auth.%s: missing <tmpdir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *tmpdir = argv[2];

    if (scenario == "auth_round_trip_allowed_peer_delivers")
        return auth_round_trip_allowed_peer_delivers(tmpdir);
    if (scenario == "auth_unallowed_peer_blocked")
        return auth_unallowed_peer_blocked(tmpdir);
    if (scenario == "auth_allowlist_swap_takes_effect_for_next_connection")
        return auth_allowlist_swap_takes_effect_for_next_connection(tmpdir);
    if (scenario == "legacy_unauth_factories_unchanged")
        return legacy_unauth_factories_unchanged(tmpdir);
    std::fprintf(stderr, "zmq_queue_auth: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct ZmqQueueAuthWorkerRegistrar
{
    ZmqQueueAuthWorkerRegistrar()
    {
        ::register_worker_dispatcher(dispatch_zmq_queue_auth);
    }
};
static ZmqQueueAuthWorkerRegistrar g_zmq_queue_auth_registrar;

} // namespace
