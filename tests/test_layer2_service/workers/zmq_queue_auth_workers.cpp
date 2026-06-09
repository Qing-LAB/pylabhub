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
#include "utils/security/key_store.hpp"
#include "utils/security/peer_admission.hpp"
#include "utils/security/secure_memory_subsystem.hpp"
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
using pylabhub::utils::security::ZapRouter;

using pylabhub::hub::QueueReader;
using pylabhub::hub::QueueWriter;
using pylabhub::hub::SchemaFieldDesc;
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

/// Per-scenario RAII guard for the process-singleton SMS + KeyStore.
/// HEP-CORE-0040 §172 puts every CURVE keypair behind the process
/// KeyStore (locked memory); test scenarios construct one of these to
/// host their identities, then set `ZmqAuthOptions::keystore_name` to
/// a name they've added.  Pattern 3 (one subprocess per scenario)
/// guarantees the SMS+KeyStore singletons are constructed fresh each
/// time.
///
/// This class contains NO logic — it only owns the two production
/// types in stack scope.  Identity seeding is delegated to the
/// production `KeyStore::add_identity_from_z85` API directly at each
/// call site so the (pub_z85 || sec_z85) → 80-byte layout has exactly
/// one definition (in `key_store.cpp`).
class ScopedKeyStore
{
public:
    ScopedKeyStore()
        : sms_(),
          ks_("test", "test.zmq_queue_auth")
    {}
    ScopedKeyStore(const ScopedKeyStore &)            = delete;
    ScopedKeyStore &operator=(const ScopedKeyStore &) = delete;
    ScopedKeyStore(ScopedKeyStore &&)                 = delete;
    ScopedKeyStore &operator=(ScopedKeyStore &&)      = delete;

private:
    pylabhub::utils::security::SecureMemorySubsystem sms_;
    pylabhub::utils::security::KeyStore              ks_;
};

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
            ScopedKeyStore ks;
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [consumer_pub, consumer_sec] = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("producer", producer_pub, producer_sec);
            pylabhub::utils::security::key_store().add_identity_from_z85("consumer", consumer_pub, consumer_sec);

            // Producer side: bind, CURVE_SERVER, ZAP-gated.
            // HEP-CORE-0040 §8.4 (#158): allowlist is no longer a
            // factory param; seed via `set_peer_allowlist()` after
            // construction.
            PeerAllowlist initial_allow;
            initial_allow.peers.insert(PeerIdentity{"curve", consumer_pub});

            auto producer = ZmqQueue::push_to_curve(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*identity_key_name=*/"producer",
                /*zap_domain=*/"test.zmq.auth.roundtrip",
                /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            // `push_to_curve` drops the initial-allowlist factory
            // param (HEP-CORE-0040 §8.4 #158); seed via
            // `set_peer_allowlist()` AFTER start() because start()
            // is what creates + registers the ZapRouter slot that
            // set_peer_allowlist writes into.
            ASSERT_TRUE(producer->start());
            ASSERT_TRUE(producer->set_peer_allowlist(initial_allow));

            // Pump ZAP from a side thread.
            ZapPumpThread pump;

            // Consumer side: connect with CURVE_CLIENT.
            auto consumer = ZmqQueue::pull_from_curve(
                producer->actual_endpoint(),
                pylabhub::utils::security::Z85PublicKey{producer_pub},
                make_uint32_schema(), "aligned",
                /*identity_key_name=*/"consumer",
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
            ScopedKeyStore ks;
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [allowed_pub,  allowed_sec]  = make_keypair();
            const auto [denied_pub,   denied_sec]   = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("producer", producer_pub, producer_sec);
            pylabhub::utils::security::key_store().add_identity_from_z85("denied", denied_pub, denied_sec);
            // allowed_pub is intentionally not added to the KeyStore —
            // no socket on the allowed side is constructed in this
            // scenario; the allowlist holds the pubkey alone (no socket
            // wiring needed for the deny path verification).
            (void)allowed_sec;

            PeerAllowlist initial_allow;
            initial_allow.peers.insert(PeerIdentity{"curve", allowed_pub});
            // NB: denied_pub is NOT in the allowlist.

            auto producer = ZmqQueue::push_to_curve(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*identity_key_name=*/"producer",
                /*zap_domain=*/"test.zmq.auth.denied",
                /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            // `push_to_curve` drops the initial-allowlist factory
            // param (HEP-CORE-0040 §8.4 #158); seed via
            // `set_peer_allowlist()` AFTER start() because start()
            // is what creates + registers the ZapRouter slot that
            // set_peer_allowlist writes into.
            ASSERT_TRUE(producer->start());
            ASSERT_TRUE(producer->set_peer_allowlist(initial_allow));

            ZapPumpThread pump;

            auto denied_consumer = ZmqQueue::pull_from_curve(
                producer->actual_endpoint(),
                pylabhub::utils::security::Z85PublicKey{producer_pub},
                make_uint32_schema(), "aligned",
                /*identity_key_name=*/"denied",
                /*bind=*/false);
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
            ScopedKeyStore ks;
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [alice_pub, alice_sec]       = make_keypair();
            const auto [bob_pub,   bob_sec]         = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("producer", producer_pub, producer_sec);
            pylabhub::utils::security::key_store().add_identity_from_z85("bob", bob_pub, bob_sec);
            (void)alice_sec;  // alice_pub seeds the allowlist but no
                              // alice-side socket is constructed.

            // Initially allow alice only.
            PeerAllowlist initial_allow;
            initial_allow.peers.insert(PeerIdentity{"curve", alice_pub});

            auto producer = ZmqQueue::push_to_curve(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*identity_key_name=*/"producer",
                /*zap_domain=*/"test.zmq.auth.swap",
                /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            // `push_to_curve` drops the initial-allowlist factory
            // param (HEP-CORE-0040 §8.4 #158); seed via
            // `set_peer_allowlist()` AFTER start() because start()
            // is what creates + registers the ZapRouter slot that
            // set_peer_allowlist writes into.
            ASSERT_TRUE(producer->start());
            ASSERT_TRUE(producer->set_peer_allowlist(initial_allow));

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
            auto bob_consumer = ZmqQueue::pull_from_curve(
                producer->actual_endpoint(),
                pylabhub::utils::security::Z85PublicKey{producer_pub},
                make_uint32_schema(), "aligned",
                /*identity_key_name=*/"bob",
                /*bind=*/false);
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

// ── Phase C close-out (commit 2/5) — security-grade scenarios ────────────
//
// These tests REPLACE timeout-as-proxy assertions with path-level
// signals (ZapRouter::denied_count() / allowed_count()) and pin the
// directional properties that the original L2 suite missed:
//   - the OLD peer is denied after an allowlist swap (not just that
//     the NEW peer succeeds)
//   - the same peer that was just denied IS admitted after a swap
//     adds it (proves the deny was real, not a hang)
//   - pull-side PeerAdmission methods correctly return false / nullopt
//   - empty allowlist denies everyone (secure default — Phase D
//     broker bootstrap depends on this)
//   - factory rejects misconfigured ZmqAuthOptions BEFORE start()
//     instead of failing late with a stale-errno diagnostic
//   - admission_is_enforced() flips false → true → false across
//     construct → start() → stop() (interface-contract conformance)

/// **H-T1 fix.**  Same peer denied first, then admitted via swap —
/// both phases pin byte-exact data + ZapRouter counters.  This
/// converts the original "timeout-as-proxy" deny assertion into a
/// path-level pin: if the deny PATH didn't execute, the counter
/// doesn't move and the test fails immediately (not after a 500ms
/// indistinguishable-from-stall wait).
int auth_deny_then_allow_via_swap_pins_path(const char *)
{
    return run_gtest_worker(
        [&]() {
            ScopedKeyStore ks;
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("producer", producer_pub, producer_sec);
            pylabhub::utils::security::key_store().add_identity_from_z85("client", client_pub, client_sec);

            // Producer: CURVE wired, allowlist EMPTY (deny-all).
            // `push_to_curve` no longer takes an initial allowlist
            // (HEP-CORE-0040 §8.4); default is deny-all — exactly
            // what Phase 1 needs.
            auto producer = ZmqQueue::push_to_curve(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*identity_key_name=*/"producer",
                /*zap_domain=*/"test.zmq.auth.deny_then_allow",
                /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            ASSERT_TRUE(producer->start());
            EXPECT_TRUE(producer->admission_is_enforced());

            ZapPumpThread pump;

            // ── Phase 1: DENY.  Same client attempts connection — must
            //   be denied by ZAP (allowlist is empty).  The CURVE
            //   handshake fires on consumer->start(), independent of
            //   any data send — we observe the deny PATH directly via
            //   ZapRouter::denied_count().  No data is sent in this
            //   phase because any queued send would linger in the
            //   producer's internal ring and contaminate Phase 2.
            const auto deny_baseline = ZapRouter::instance().denied_count();
            {
                auto consumer = ZmqQueue::pull_from_curve(
                    producer->actual_endpoint(),
                    pylabhub::utils::security::Z85PublicKey{producer_pub},
                    make_uint32_schema(), "aligned",
                    /*identity_key_name=*/"client",
                    /*bind=*/false);
                ASSERT_NE(consumer, nullptr);
                ASSERT_TRUE(consumer->start());

                // Wait for the deny path to surface.  The CURVE
                // handshake driven by start() submits the ZAP REQ.
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(1500);
                while (ZapRouter::instance().denied_count() ==
                           deny_baseline &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));

                EXPECT_GT(ZapRouter::instance().denied_count(),
                          deny_baseline)
                    << "Phase 1: expected DENY PATH to execute "
                       "(denied_count++).  A timeout-as-proxy assertion "
                       "would pass on a hang; this counter assertion "
                       "won't.";
                consumer->stop();
            }

            // ── Phase 2: ALLOW VIA SWAP.  Add the same client to the
            //   allowlist; a fresh consumer with the SAME keys should
            //   now succeed.  Path-pin via allowed_count + byte-exact
            //   data.
            PeerAllowlist al_with_client;
            al_with_client.peers.insert(
                PeerIdentity{"curve", client_pub});
            ASSERT_TRUE(producer->set_peer_allowlist(al_with_client));

            const auto allow_baseline =
                ZapRouter::instance().allowed_count();
            {
                auto consumer = ZmqQueue::pull_from_curve(
                    producer->actual_endpoint(),
                    pylabhub::utils::security::Z85PublicKey{producer_pub},
                    make_uint32_schema(), "aligned",
                    /*identity_key_name=*/"client",
                    /*bind=*/false);
                ASSERT_NE(consumer, nullptr);
                ASSERT_TRUE(consumer->start());

                ASSERT_TRUE(try_send_uint32(producer.get(), 0xCAFEBABEu));
                EXPECT_TRUE(wait_for_one_uint32(
                    consumer.get(), 0xCAFEBABEu,
                    std::chrono::milliseconds(2000)))
                    << "Phase 2: post-swap delivery to the same peer "
                       "must succeed byte-exact.";
                EXPECT_GT(ZapRouter::instance().allowed_count(),
                          allow_baseline)
                    << "Phase 2: expected ALLOW PATH to execute "
                       "(allowed_count++).";
                consumer->stop();
            }

            producer->stop();
        },
        "zmq_queue_auth::auth_deny_then_allow_via_swap_pins_path",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// **H-T2 fix.**  Allowlist swap MUST deny the previously-admitted
/// peer.  Pins both directions: old peer succeeds before swap → old
/// peer fails after swap → new peer succeeds.  All three phases use
/// distinct byte sentinels.
int auth_swap_blocks_old_peer_pins_data(const char *)
{
    return run_gtest_worker(
        [&]() {
            ScopedKeyStore ks;
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [alice_pub, alice_sec] = make_keypair();
            const auto [bob_pub,   bob_sec]   = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("producer", producer_pub, producer_sec);
            pylabhub::utils::security::key_store().add_identity_from_z85("alice", alice_pub, alice_sec);
            pylabhub::utils::security::key_store().add_identity_from_z85("bob", bob_pub, bob_sec);

            PeerAllowlist initial_allow;
            initial_allow.peers.insert(PeerIdentity{"curve", alice_pub});

            auto producer = ZmqQueue::push_to_curve(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*identity_key_name=*/"producer",
                /*zap_domain=*/"test.zmq.auth.swap.blocks.old",
                /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            // `push_to_curve` drops the initial-allowlist factory
            // param (HEP-CORE-0040 §8.4 #158); seed via
            // `set_peer_allowlist()` AFTER start() because start()
            // is what creates + registers the ZapRouter slot that
            // set_peer_allowlist writes into.
            ASSERT_TRUE(producer->start());
            ASSERT_TRUE(producer->set_peer_allowlist(initial_allow));
            ZapPumpThread pump;

            // ── Phase A: alice admitted, receives 0xAAAA0001u.
            {
                auto alice = ZmqQueue::pull_from_curve(
                    producer->actual_endpoint(),
                    pylabhub::utils::security::Z85PublicKey{producer_pub},
                    make_uint32_schema(), "aligned",
                    /*identity_key_name=*/"alice",
                    /*bind=*/false);
                ASSERT_NE(alice, nullptr);
                ASSERT_TRUE(alice->start());
                ASSERT_TRUE(try_send_uint32(producer.get(), 0xAAAA0001u));
                EXPECT_TRUE(wait_for_one_uint32(
                    alice.get(), 0xAAAA0001u,
                    std::chrono::milliseconds(2000)))
                    << "Pre-swap baseline: alice must receive her "
                       "byte-exact sentinel.";
                alice->stop();
            }

            // ── Phase B: swap to bob-only.
            PeerAllowlist al_bob;
            al_bob.peers.insert(PeerIdentity{"curve", bob_pub});
            ASSERT_TRUE(producer->set_peer_allowlist(al_bob));

            auto snap = producer->peer_allowlist_snapshot();
            ASSERT_TRUE(snap.has_value());
            EXPECT_FALSE(snap->contains(PeerIdentity{"curve", alice_pub}));
            EXPECT_TRUE(snap->contains(PeerIdentity{"curve", bob_pub}));

            // ── Phase C: alice RECONNECTS — must be denied.  Pin via
            //   denied_count to prove the path executed.  No data
            //   send in this phase: any queued send would linger in
            //   the producer's send ring and arrive at bob in Phase D,
            //   confounding the assertion.
            const auto deny_baseline = ZapRouter::instance().denied_count();
            {
                auto alice_again = ZmqQueue::pull_from_curve(
                    producer->actual_endpoint(),
                    pylabhub::utils::security::Z85PublicKey{producer_pub},
                    make_uint32_schema(), "aligned",
                    /*identity_key_name=*/"alice",
                    /*bind=*/false);
                ASSERT_NE(alice_again, nullptr);
                ASSERT_TRUE(alice_again->start());

                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(1500);
                while (ZapRouter::instance().denied_count() ==
                           deny_baseline &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));

                EXPECT_GT(ZapRouter::instance().denied_count(),
                          deny_baseline)
                    << "After swap, alice's reconnect must trigger the "
                       "DENY path.  If denied_count did not move, the "
                       "swap is a no-op (security regression: a stale "
                       "allowlist entry would still admit alice).";
                alice_again->stop();
            }

            // ── Phase D: bob connects, receives 0xBBBB0001u.
            {
                auto bob = ZmqQueue::pull_from_curve(
                    producer->actual_endpoint(),
                    pylabhub::utils::security::Z85PublicKey{producer_pub},
                    make_uint32_schema(), "aligned",
                    /*identity_key_name=*/"bob",
                    /*bind=*/false);
                ASSERT_NE(bob, nullptr);
                ASSERT_TRUE(bob->start());
                ASSERT_TRUE(try_send_uint32(producer.get(), 0xBBBB0001u));
                EXPECT_TRUE(wait_for_one_uint32(
                    bob.get(), 0xBBBB0001u,
                    std::chrono::milliseconds(2000)));
                bob->stop();
            }

            producer->stop();
        },
        "zmq_queue_auth::auth_swap_blocks_old_peer_pins_data",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// Pins the PULL-side PeerAdmission contract.  Per the design + the
/// header docstring at hub_zmq_queue.hpp:294-307, set_peer_allowlist
/// returns false on the connect side (no allowlist concept), and
/// peer_allowlist_snapshot returns nullopt, and is_peer_allowed
/// returns false unconditionally.
int auth_set_peer_allowlist_on_pull_side_returns_false(const char *)
{
    return run_gtest_worker(
        [&]() {
            ScopedKeyStore ks;
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("client", client_pub, client_sec);
            (void)producer_sec;  // producer's pubkey is used as the
                                 // serverkey for factory validation; no
                                 // producer socket is constructed here.

            // Only a consumer here — no producer needed for this
            // interface-contract pin.  pull_from_curve still needs a
            // serverkey to pass factory validation.
            auto consumer = ZmqQueue::pull_from_curve(
                "tcp://127.0.0.1:5555",   // never started, no peer
                pylabhub::utils::security::Z85PublicKey{producer_pub},
                make_uint32_schema(), "aligned",
                /*identity_key_name=*/"client",
                /*bind=*/false);
            ASSERT_NE(consumer, nullptr);
            // Note: not started.  PULL-side contract is a property of
            //   the abstraction, not the live socket.

            PeerAllowlist any;
            any.peers.insert(PeerIdentity{"curve", client_pub});

            EXPECT_FALSE(consumer->set_peer_allowlist(any))
                << "PULL-side set_peer_allowlist must return false — "
                   "the consumer trusts the server via "
                   "curve_serverkey, not via its own allowlist.";
            EXPECT_FALSE(consumer->peer_allowlist_snapshot().has_value())
                << "PULL-side snapshot must be nullopt.";
            EXPECT_FALSE(consumer->is_peer_allowed(
                PeerIdentity{"curve", client_pub}))
                << "PULL-side is_peer_allowed must return false "
                   "unconditionally (no inbound handshakes to gate).";
        },
        "zmq_queue_auth::auth_set_peer_allowlist_on_pull_side_returns_false",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// **Empty allowlist = deny-all.**  Phase D broker bootstrap may
/// briefly install an empty allowlist before pushing the real one
/// from the KnownRolesStore; this test pins that the default is
/// secure (deny everyone) rather than permissive.
int auth_empty_allowlist_denies_all(const char *)
{
    return run_gtest_worker(
        [&]() {
            ScopedKeyStore ks;
            const auto [producer_pub, producer_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("producer", producer_pub, producer_sec);
            pylabhub::utils::security::key_store().add_identity_from_z85("client", client_pub, client_sec);

            // `push_to_curve` drops the initial_allowlist param —
            // empty (deny-all) is the new default.  Exactly what this
            // test wants to pin.
            auto producer = ZmqQueue::push_to_curve(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*identity_key_name=*/"producer",
                /*zap_domain=*/"test.zmq.auth.empty.deny",
                /*bind=*/true);
            ASSERT_NE(producer, nullptr);
            ASSERT_TRUE(producer->start());

            // Pin the in-memory contract: snapshot is present (the
            // empty allowlist WAS installed), and is_peer_allowed
            // returns false for any peer.
            auto snap = producer->peer_allowlist_snapshot();
            ASSERT_TRUE(snap.has_value());
            EXPECT_TRUE(snap->peers.empty());
            EXPECT_FALSE(producer->is_peer_allowed(
                PeerIdentity{"curve", client_pub}));

            ZapPumpThread pump;
            const auto deny_baseline = ZapRouter::instance().denied_count();

            auto consumer = ZmqQueue::pull_from_curve(
                producer->actual_endpoint(),
                pylabhub::utils::security::Z85PublicKey{producer_pub},
                make_uint32_schema(), "aligned",
                /*identity_key_name=*/"client",
                /*bind=*/false);
            ASSERT_NE(consumer, nullptr);
            ASSERT_TRUE(consumer->start());

            // The CURVE handshake fires on consumer->start() above —
            // the ZAP REQ submits independent of any data send.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(1500);
            while (ZapRouter::instance().denied_count() ==
                       deny_baseline &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20));

            EXPECT_GT(ZapRouter::instance().denied_count(),
                      deny_baseline)
                << "Empty allowlist must deny EVERY peer (Phase D "
                   "bootstrap depends on this fail-closed default).";

            consumer->stop();
            producer->stop();
        },
        "zmq_queue_auth::auth_empty_allowlist_denies_all",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// **H-Q3 fix.**  Factory must reject connect-side ZmqAuthOptions
/// that has CURVE keys but no serverkey — BEFORE constructing the
/// queue.  Pins the explicit-diagnostic path (vs. throwing
/// zmq::error_t() against stale errno from inside start()).
int auth_misconfig_connect_missing_serverkey_factory_returns_nullptr(const char *)
{
    return run_gtest_worker(
        [&]() {
            ScopedKeyStore ks;
            const auto [client_pub, client_sec] = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("client", client_pub, client_sec);
            // serverkey intentionally empty (default-ctor sentinel) —
            // factory must reject before constructing the queue.
            auto consumer = ZmqQueue::pull_from_curve(
                "tcp://127.0.0.1:5555",
                pylabhub::utils::security::Z85PublicKey{},  // empty
                make_uint32_schema(), "aligned",
                /*identity_key_name=*/"client",
                /*bind=*/false);
            EXPECT_EQ(consumer, nullptr)
                << "Factory must reject connect-side auth without "
                   "serverkey.  Returning a queue that throws inside "
                   "start() with a stale-errno 'Success' message is "
                   "the H-Q3 anti-pattern.";
        },
        "zmq_queue_auth::auth_misconfig_connect_missing_serverkey_factory_returns_nullptr",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// Factory rejects KeyStore prerequisites unmet, or connect-side
/// `server_pubkey` is missing.  Each rejection path emits a precise
/// LOGGER_ERROR — the test pin for the error wording lives in
/// `test_zmq_queue_auth.cpp`'s `ExpectWorkerOk(..., expected_errors)`
/// list.
///
/// HEP-CORE-0040 §8.4 (#158) moved wrong-length pubkey validation OUT
/// of the factory — `Z85PublicKey` now enforces the 40-char Z85
/// invariant at construction time, so any "too-short serverkey" path
/// throws upstream before reaching the factory.  That branch lives
/// in `tests/test_layer2_service/test_curve_keypair.cpp`.  Here we
/// pin the validator-side branches that survive: missing identity
/// name in KeyStore + missing serverkey on connect side + empty
/// identity name (C1 #157).
int auth_misconfig_factory_returns_nullptr(const char *)
{
    return run_gtest_worker(
        [&]() {
            ScopedKeyStore ks;
            const auto [valid_pub, valid_sec] = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("valid", valid_pub, valid_sec);
            namespace sec = pylabhub::utils::security;
            const sec::Z85PublicKey valid_serverkey{valid_pub};
            const sec::Z85PublicKey empty_serverkey;  // sentinel

            struct Case
            {
                const char *label;
                std::string identity_key_name;
                sec::Z85PublicKey server;
                bool bind;
                bool expect_null;
            };

            const std::vector<Case> cases{
                {"name not in KeyStore (bind)",
                 "missing.name", empty_serverkey, true, true},
                {"connect side: KeyStore prerequisite OK, serverkey empty",
                 "valid", empty_serverkey, false, true},
                {"connect side fully valid (control)",
                 "valid", valid_serverkey, false, false},
                // C1 (#157): empty identity_key_name is rejected on
                // both sides (HEP-CORE-0035 §2, CURVE unconditional).
                {"empty identity_key_name rejected on bind side (C1)",
                 "", empty_serverkey, true, true},
                {"empty identity_key_name rejected on connect side (C1)",
                 "", valid_serverkey, false, true},
            };

            for (const auto &c : cases)
            {
                std::unique_ptr<ZmqQueue> q;
                if (c.bind)
                    q = ZmqQueue::push_to_curve(
                        "tcp://127.0.0.1:0",
                        make_uint32_schema(), "aligned",
                        c.identity_key_name,
                        /*zap_domain=*/"",
                        /*bind=*/true);
                else
                    q = ZmqQueue::pull_from_curve(
                        "tcp://127.0.0.1:5555",
                        c.server,
                        make_uint32_schema(), "aligned",
                        c.identity_key_name,
                        /*bind=*/false);

                if (c.expect_null)
                    EXPECT_EQ(q, nullptr) << "case: " << c.label;
                else
                    EXPECT_NE(q, nullptr) << "case: " << c.label;
            }
        },
        "zmq_queue_auth::auth_misconfig_factory_returns_nullptr",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// **M-Q1 fix.**  admission_is_enforced() must reflect actual wire-
/// level enforcement: false before start(), true after a successful
/// start(), false after stop().  The pre-fix implementation returned
/// true as soon as the keys were populated — lying when start() had
/// not yet run.
int auth_admission_is_enforced_lifecycle(const char *)
{
    return run_gtest_worker(
        [&]() {
            ScopedKeyStore ks;
            const auto [pub, sec] = make_keypair();
            pylabhub::utils::security::key_store().add_identity_from_z85("identity", pub, sec);
            auto q = ZmqQueue::push_to_curve(
                "tcp://127.0.0.1:0", make_uint32_schema(), "aligned",
                /*identity_key_name=*/"identity",
                /*zap_domain=*/"test.zmq.auth.enforced.lifecycle",
                /*bind=*/true);
            ASSERT_NE(q, nullptr);

            EXPECT_FALSE(q->admission_is_enforced())
                << "Before start(): no socket exists yet, so the wire "
                   "is NOT enforced.  Returning true here would lie "
                   "to broker glue that's about to push allowlists.";

            ASSERT_TRUE(q->start());
            EXPECT_TRUE(q->admission_is_enforced())
                << "After start(): CURVE is wired on the bound socket.";

            q->stop();
            EXPECT_FALSE(q->admission_is_enforced())
                << "After stop(): socket closed, wire is no longer "
                   "enforced.";
        },
        "zmq_queue_auth::auth_admission_is_enforced_lifecycle",
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
    if (scenario == "auth_deny_then_allow_via_swap_pins_path")
        return auth_deny_then_allow_via_swap_pins_path(tmpdir);
    if (scenario == "auth_swap_blocks_old_peer_pins_data")
        return auth_swap_blocks_old_peer_pins_data(tmpdir);
    if (scenario == "auth_set_peer_allowlist_on_pull_side_returns_false")
        return auth_set_peer_allowlist_on_pull_side_returns_false(tmpdir);
    if (scenario == "auth_empty_allowlist_denies_all")
        return auth_empty_allowlist_denies_all(tmpdir);
    if (scenario == "auth_misconfig_connect_missing_serverkey_factory_returns_nullptr")
        return auth_misconfig_connect_missing_serverkey_factory_returns_nullptr(tmpdir);
    if (scenario == "auth_misconfig_factory_returns_nullptr")
        return auth_misconfig_factory_returns_nullptr(tmpdir);
    if (scenario == "auth_admission_is_enforced_lifecycle")
        return auth_admission_is_enforced_lifecycle(tmpdir);
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
