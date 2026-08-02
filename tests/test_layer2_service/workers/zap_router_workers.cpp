/**
 * @file zap_router_workers.cpp
 * @brief Worker bodies for the ZapRouter L2 test suite (PeerAdmission Phase C).
 *
 * Pattern 3 — each worker runs in a fresh subprocess with its own
 * LifecycleGuard (Logger + FileLock + JsonConfig + ZMQContext per
 * the ZapRouter dynamic-module dependency chain).
 *
 * Each handshake test uses a `ZapPumpThread` RAII helper to pump the
 * inproc ZAP socket while the test thread drives CURVE handshakes
 * via a real `tcp://127.0.0.1:0` socket pair.  In production
 * binaries the pump is in the broker's / role's existing event loop
 * (Phase D wiring) — never `ZapPumpThread`.  The L2 test using the
 * RAII helper is functionally equivalent to "your main loop forgot
 * to pump" being immediately observable as a test hang.
 */
#include "utils/role_identity_policy.hpp"
#include "utils/security/pubkey_origin.hpp"
#include "utils/security/attested_key.hpp"
#include "utils/file_lock.hpp"
#include "utils/json_config.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/peer_admission.hpp"
#include "utils/security/zap_router.hpp"
#include "utils/zmq_context.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp" // recv_multipart
#include <zmq.h>

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional> // std::function — round3 admission callback
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;
using pylabhub::utils::security::PeerAdmission;
using pylabhub::utils::security::PeerAllowlist;
using pylabhub::utils::security::PeerIdentity;
using pylabhub::utils::security::ZapDomainHandle;
using pylabhub::utils::security::ZapPumpThread;
using pylabhub::utils::security::ZapRouter;

namespace
{

constexpr std::size_t kPubkeyZ85Chars = 40;

class InMemoryAdmission final : public PeerAdmission
{
  public:
    bool set_peer_allowlist(PeerAllowlist al) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        al_ = std::move(al);
        return true;
    }
    std::optional<PeerAllowlist> peer_allowlist_snapshot() const override
    {
        std::lock_guard<std::mutex> lk(mu_);
        return al_;
    }
    bool is_peer_allowed(const PeerIdentity &p) const override
    {
        std::lock_guard<std::mutex> lk(mu_);
        return al_.has_value() && al_->contains(p);
    }

  private:
    mutable std::mutex mu_;
    std::optional<PeerAllowlist> al_;
};

std::pair<std::string, std::string> make_keypair()
{
    std::array<char, kPubkeyZ85Chars + 1> pub{};
    std::array<char, kPubkeyZ85Chars + 1> sec{};
    if (::zmq_curve_keypair(pub.data(), sec.data()) != 0)
        throw std::runtime_error("zmq_curve_keypair failed");
    return {std::string(pub.data(), kPubkeyZ85Chars), std::string(sec.data(), kPubkeyZ85Chars)};
}

zmq::socket_t bind_pull_server(const std::string &server_pub, const std::string &server_sec,
                               const std::string &zap_domain)
{
    zmq::socket_t pull(pylabhub::hub::get_zmq_context(), zmq::socket_type::pull);
    pull.set(zmq::sockopt::linger, 0);
    pull.set(zmq::sockopt::curve_server, 1);
    pull.set(zmq::sockopt::curve_publickey, server_pub);
    pull.set(zmq::sockopt::curve_secretkey, server_sec);
    pull.set(zmq::sockopt::zap_domain, zap_domain);
    pull.bind("tcp://127.0.0.1:0");
    return pull;
}

/// Complete a real CURVE handshake, receive the message, and mint the
/// attestation from the SOCKET and the MESSAGE.
///
/// This is the only way a test can obtain an `AttestedKey`, and that is the
/// design working rather than the test being awkward: `from_message` reads
/// the ZAP domain off the socket and the proven key off the message, so
/// there is no string a test could pass to fake one.  The previous factory
/// took a domain and a key as strings, which let tests — and would have let
/// production callers — assert on a value they had supplied themselves.
///
/// Returns the attestation, plus the received message kept alive so a caller
/// can re-mint against a different socket state.
std::optional<pylabhub::utils::security::AttestedKey>
attest_via_handshake(zmq::socket_t &pull, const std::string &server_pub,
                    const std::string &client_pub, const std::string &client_sec,
                    zmq::message_t *keep_msg = nullptr)
{
    zmq::socket_t push(pylabhub::hub::get_zmq_context(), zmq::socket_type::push);
    push.set(zmq::sockopt::linger, 0);
    push.set(zmq::sockopt::curve_serverkey, server_pub);
    push.set(zmq::sockopt::curve_publickey, client_pub);
    push.set(zmq::sockopt::curve_secretkey, client_sec);
    push.connect(pull.get(zmq::sockopt::last_endpoint));
    (void)push.send(zmq::str_buffer("x"), zmq::send_flags::none);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        zmq::message_t msg;
        if (pull.recv(msg, zmq::recv_flags::dontwait))
        {
            auto att = pylabhub::utils::security::AttestedKey::from_message(pull, msg);
            if (keep_msg != nullptr)
                *keep_msg = std::move(msg);
            return att;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

/// Drive one CURVE handshake by spinning up a fresh PUSH socket;
/// send one byte; observe whether the PULL side receives it within
/// @p timeout.  Returns true on delivery, false on timeout (which
/// means ZAP denied OR the network round-trip didn't complete).
bool handshake_and_deliver(zmq::socket_t &pull, const std::string &server_pub,
                           const std::string &client_pub, const std::string &client_sec,
                           std::chrono::milliseconds timeout)
{
    zmq::socket_t push(pylabhub::hub::get_zmq_context(), zmq::socket_type::push);
    push.set(zmq::sockopt::linger, 0);
    push.set(zmq::sockopt::curve_serverkey, server_pub);
    push.set(zmq::sockopt::curve_publickey, client_pub);
    push.set(zmq::sockopt::curve_secretkey, client_sec);
    const std::string endpoint = pull.get(zmq::sockopt::last_endpoint);
    push.connect(endpoint);

    zmq::message_t out(1);
    std::memcpy(out.data(), "x", 1);
    (void)push.send(out, zmq::send_flags::dontwait);

    zmq::message_t in;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        pull.set(zmq::sockopt::rcvtimeo, 50);
        try
        {
            auto rr = pull.recv(in, zmq::recv_flags::none);
            if (rr.has_value() && in.size() == 1)
                return true;
        }
        catch (const zmq::error_t &)
        {
            break;
        }
    }
    return false;
}

// ── Scenarios ──────────────────────────────────────────────────────────────

int handshake_accept_deny_cycle(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [server_pub, server_sec] = make_keypair();
            const auto [allowed_pub, allowed_sec] = make_keypair();
            const auto [denied_pub, denied_sec] = make_keypair();

            InMemoryAdmission admission;
            auto pull = bind_pull_server(server_pub, server_sec, "test.zap.handshake.suite1");
            auto handle =
                ZapRouter::instance().register_domain("test.zap.handshake.suite1", admission);
            ASSERT_TRUE(handle.is_active());
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);

            // Spin up the pump.  RAII: joins at scope exit, before
            // ASSERT_TRUE returns / before destructors of `pull` /
            // `handle` run.
            ZapPumpThread pump;

            // Phase 1: no allowlist → deny everyone (secure default).
            EXPECT_FALSE(handshake_and_deliver(pull, server_pub, allowed_pub, allowed_sec,
                                               std::chrono::milliseconds(300)));

            // Phase 2: allow `allowed_pub` → handshake succeeds.
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", allowed_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));
            EXPECT_TRUE(handshake_and_deliver(pull, server_pub, allowed_pub, allowed_sec,
                                              std::chrono::milliseconds(1000)));

            // Phase 3: different peer with valid CURVE but NOT in
            // allowlist → deny.
            EXPECT_FALSE(handshake_and_deliver(pull, server_pub, denied_pub, denied_sec,
                                               std::chrono::milliseconds(300)));

            // Phase 4: mid-flight allowlist swap → new peer
            // allowed.
            PeerAllowlist al2;
            al2.peers.insert(PeerIdentity{"curve", denied_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al2));
            EXPECT_TRUE(handshake_and_deliver(pull, server_pub, denied_pub, denied_sec,
                                              std::chrono::milliseconds(1000)));

            // Phase 5: previously-allowed peer now denied.
            EXPECT_FALSE(handshake_and_deliver(pull, server_pub, allowed_pub, allowed_sec,
                                               std::chrono::milliseconds(300)));
        },
        "zap_router::handshake_accept_deny_cycle", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int unknown_domain_denies(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [server_pub, server_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();
            auto pull = bind_pull_server(server_pub, server_sec, "test.zap.unregistered.domain");

            // Register a DIFFERENT domain so the module is loaded but
            // our pull socket's domain is unknown to the router.
            // Deliberately empty: this admission guards a DIFFERENT domain and
            // is never consulted by the pull socket under test.  Leaving it
            // deny-all makes that explicit — if it ever were consulted, the
            // test would fail rather than silently pass on a blanket admit.
            InMemoryAdmission unrelated;
            (void)unrelated.set_peer_allowlist(PeerAllowlist{});
            auto handle = ZapRouter::instance().register_domain("test.zap.other.domain", unrelated);

            ZapPumpThread pump;

            EXPECT_FALSE(handshake_and_deliver(pull, server_pub, client_pub, client_sec,
                                               std::chrono::milliseconds(300)));
        },
        "zap_router::unknown_domain_denies", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int claim_check_covers_every_verdict(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            using pylabhub::utils::security::AttestedKey;
            using pylabhub::utils::security::ClaimVerdict;
            using pylabhub::utils::security::PeerAuthority;

            const auto [server_pub, server_sec] = make_keypair();
            const std::string domain = "test.zap.claim.check";
            auto pull = bind_pull_server(server_pub, server_sec, domain);

            const auto [alice_pub, alice_sec] = make_keypair();
            const auto [bob_pub, bob_sec] = make_keypair();
            const auto [peer_pub, peer_sec] = make_keypair();
            const auto [stranger_pub, stranger_sec] = make_keypair();

            // Every client must COMPLETE the handshake — the CLAIM made after
            // it is what this test examines.  Admitting them means naming all
            // four keys; there is no blanket "let everyone in" any more, and
            // spelling them out is what makes it obvious that admission and
            // claim-checking are two separate gates.
            InMemoryAdmission admission;
            PeerAllowlist al;
            for (const auto &k : {alice_pub, bob_pub, peer_pub, stranger_pub})
                al.peers.insert(PeerIdentity{pylabhub::utils::security::kCurveMechanism, k});
            (void)admission.set_peer_allowlist(std::move(al));
            auto handle = ZapRouter::instance().register_domain(domain, admission);
            ZapPumpThread pump;

            const std::string alice_uid = "prod.alice.uid00000001";
            const std::string bob_uid = "prod.bob.uid00000002";
            const std::string peer_uid = "hub.peer.uid00000003";

            PeerAuthority::Builder builder;
            auto add_role = [&builder](const std::string &uid, const std::string &pub)
            {
                pylabhub::broker::KnownRole kr;
                kr.name = uid;
                kr.uid = uid;
                kr.role = "producer";
                kr.pubkey_z85 = pub;
                builder.add_local_role(kr);
            };
            add_role(alice_uid, alice_pub);
            add_role(bob_uid, bob_pub);
            builder.add_federation_peer(peer_uid, peer_pub);
            const PeerAuthority idx = std::move(builder).build();

            // Each of these is a genuine handshake: the key under test is the
            // one whose SECRET the client proved, read back out of the
            // message metadata ZAP stamped.
            const auto alice = attest_via_handshake(pull, server_pub, alice_pub, alice_sec);
            const auto peer = attest_via_handshake(pull, server_pub, peer_pub, peer_sec);
            const auto stranger = attest_via_handshake(pull, server_pub, stranger_pub, stranger_sec);
            ASSERT_TRUE(alice && peer && stranger) << "a handshake did not produce an attestation";
            ASSERT_EQ(alice->key().view(), alice_pub);

            // Honest registration.
            EXPECT_EQ(idx.check_registration_claim(alice, alice_uid, alice_pub),
                      ClaimVerdict::accepted);

            // THE ORIGINAL DEFECT: a peer holding a VALID key claiming ANOTHER
            // role's identity.  Alice's connection, Bob's uid.  Before this
            // work every gate accepted it.
            EXPECT_EQ(idx.check_registration_claim(alice, bob_uid, alice_pub),
                      ClaimVerdict::identity_mismatch);

            // Announcing someone else's key while connected as alice.
            EXPECT_EQ(idx.check_registration_claim(alice, alice_uid, bob_pub),
                      ClaimVerdict::pubkey_mismatch);

            // A federation peer may not register as a role.  It is refused on
            // KIND — note it claims its own uid, so a uid comparison alone
            // would have accepted this.
            EXPECT_EQ(idx.check_registration_claim(peer, peer_uid, peer_pub),
                      ClaimVerdict::kind_not_permitted);

            // Attested, but this hub has no record of the key.
            EXPECT_EQ(idx.check_registration_claim(stranger, alice_uid, stranger_pub),
                      ClaimVerdict::unknown_key);

            // No attestation at all — registration requires proof.
            EXPECT_EQ(idx.check_registration_claim(std::nullopt, alice_uid, alice_pub),
                      ClaimVerdict::no_attestation);
        },
        "zap_router::claim_check_covers_every_verdict", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int authority_answers_questions_about_attested_keys(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            using pylabhub::utils::security::ClaimVerdict;
            using pylabhub::utils::security::PeerAuthority;
            using pylabhub::utils::security::RosterEntry;

            // These questions require an ATTESTED key, so they live here
            // rather than in a plain unit test: obtaining one means a real
            // enforced ZAP domain and a real handshake.  That is the design
            // working — a caller holding a bare string cannot ask at all.
            //
            // Note what is NOT tested: there is no `resolve()` to call. The
            // authority answers questions and never hands back its internal
            // record, so a test cannot assert on one either. Asserting on
            // the answers is asserting on the contract.
            const auto [server_pub, server_sec] = make_keypair();
            const std::string domain = "test.zap.authority.questions";
            auto pull = bind_pull_server(server_pub, server_sec, domain);

            const auto [role_pub, role_sec] = make_keypair();
            const auto [peer_pub, peer_sec] = make_keypair();
            const auto [stranger_pub, stranger_sec] = make_keypair();

            // All three must reach the authority to be asked about — including
            // the stranger, whose whole point is that the authority answers
            // "no" about a key that DID authenticate.
            InMemoryAdmission admission;
            PeerAllowlist al;
            for (const auto &k : {role_pub, peer_pub, stranger_pub})
                al.peers.insert(PeerIdentity{pylabhub::utils::security::kCurveMechanism, k});
            (void)admission.set_peer_allowlist(std::move(al));
            auto handle = ZapRouter::instance().register_domain(domain, admission);
            ZapPumpThread pump;

            const auto role_att = attest_via_handshake(pull, server_pub, role_pub, role_sec);
            const auto peer_att = attest_via_handshake(pull, server_pub, peer_pub, peer_sec);
            const auto stranger_att =
                attest_via_handshake(pull, server_pub, stranger_pub, stranger_sec);
            ASSERT_TRUE(role_att && peer_att && stranger_att);

            // An empty authority knows nobody — deny-all is the bootstrap
            // state, not an invitation to admit everyone.
            {
                const PeerAuthority empty = PeerAuthority::Builder{}.build();
                EXPECT_EQ(empty.attribute_sender(role_att).verdict, ClaimVerdict::unknown_key);
                EXPECT_TRUE(empty.attribute_sender(role_att).uid.empty())
                    << "a refused attribution must carry no name to stamp";
                EXPECT_FALSE(empty.is_federation_peer(*role_att));
                EXPECT_TRUE(empty.local_role_roster().empty());
                EXPECT_TRUE(empty.zap_allowlist().is_deny_all())
                    << "an empty authority must admit nobody";
            }

            const std::string role_uid = "prod.alice.uid00000001";
            const std::string peer_uid = "hub.peer.uid00000002";

            PeerAuthority::Builder builder;
            pylabhub::broker::KnownRole kr;
            kr.name = "alice";
            kr.uid = role_uid;
            kr.role = "producer";
            kr.pubkey_z85 = role_pub;
            builder.add_local_role(kr);
            builder.add_federation_peer(peer_uid, peer_pub);
            const PeerAuthority authority = std::move(builder).build();

            // Attribution: a local role's key yields its uid — the datum a
            // broadcast is stamped with, and the one the inbox needs to name
            // a sender, key replay tracking and hold per-sender sequence
            // state (HEP-CORE-0035 §4.2.2).
            {
                const auto sender = authority.attribute_sender(role_att);
                EXPECT_EQ(sender.verdict, ClaimVerdict::accepted);
                EXPECT_EQ(sender.uid, role_uid);
            }
            EXPECT_FALSE(authority.is_federation_peer(*role_att));

            // A peer hub is NOT a local role.  Attribution refuses it rather
            // than returning its uid, because a peer relays identities other
            // than its own — treating it as a sender would attribute every
            // relayed message to the link.
            EXPECT_EQ(authority.attribute_sender(peer_att).verdict,
                      ClaimVerdict::kind_not_permitted)
                << "a federation peer must not be attributed as a local sender";
            EXPECT_TRUE(authority.is_federation_peer(*peer_att));

            // Attested but unknown.  With ZAP enforcing this is unreachable
            // on a live connection, so a caller seeing this is looking at a
            // mid-flight config change or a gate that is not doing its job.
            EXPECT_EQ(authority.attribute_sender(stranger_att).verdict, ClaimVerdict::unknown_key);
            EXPECT_FALSE(authority.is_federation_peer(*stranger_att));

            // No handshake, no name.  The verdicts are distinct because an
            // operator reading one has to tell an unarmed socket from a
            // stranger that got through.
            EXPECT_EQ(authority.attribute_sender(std::nullopt).verdict,
                      ClaimVerdict::no_attestation);

            // The roster carries the uid WITH the key.  A bare-key roster is
            // what makes the inbox unable to attribute a sender at all.
            const auto roster = authority.local_role_roster();
            ASSERT_EQ(roster.size(), 1u) << "federation peers must not appear in the role roster";
            EXPECT_EQ(roster.begin()->uid, role_uid);
            EXPECT_EQ(roster.begin()->pubkey_z85, role_pub);

            // The ZAP allowlist spans BOTH kinds — a peer hub dials this
            // broker's ROUTER too (HEP-CORE-0022) — which is why the
            // registration plane has to refuse peers on KIND rather than
            // relying on the allowlist to have kept them out.
            EXPECT_EQ(authority.zap_allowlist().peers.size(), 2u);

            // Immutability is structural, not a convention on the handle.
            // There is no mutator to reach: `add_local_role` lives on
            // Builder, and `build()` is rvalue-only, so a builder cannot
            // hand out one authority and keep editing for the next.
            static_assert(!std::is_invocable_v<decltype(&PeerAuthority::Builder::build),
                                               PeerAuthority::Builder &>,
                          "build() must consume the builder");
        },
        "zap_router::authority_answers_questions_about_attested_keys",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(), pylabhub::hub::GetZMQContextModule());
}

int attestation_matches_real_handshake(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            using pylabhub::utils::security::AttestedKey;

            // What ZAP actually stamped on the message — not a value this
            // test supplied.
            //
            // The previous version of this test was TAUTOLOGICAL: it drove a
            // real handshake, then called `from_transport(domain, client_pub)`
            // with the key it already had, so it asserted that the factory
            // returns what you feed it.  It proved nothing about what ZAP
            // puts in `User-Id`, which is the entire mechanism.  Reading the
            // value off the received message is what makes the assertion
            // mean something.
            const auto [server_pub, server_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();
            const std::string domain = "test.zap.attest.roundtrip";

            auto pull = bind_pull_server(server_pub, server_sec, domain);

            InMemoryAdmission admission;
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{pylabhub::utils::security::kCurveMechanism, client_pub});
            (void)admission.set_peer_allowlist(std::move(al));
            auto handle = ZapRouter::instance().register_domain(domain, admission);

            ZapPumpThread pump;

            const auto allowed_before = ZapRouter::instance().allowed_count();
            zmq::message_t kept;
            const auto att =
                attest_via_handshake(pull, server_pub, client_pub, client_sec, &kept);
            ASSERT_GT(ZapRouter::instance().allowed_count(), allowed_before)
                << "delivery succeeded without a ZAP ALLOW actually executing";
            ASSERT_TRUE(att.has_value()) << "handshake completed but nothing was attested";

            // The attested key is the CLIENT's — the key whose secret the
            // peer just proved possession of.  Not the server's, not the
            // routing id, not anything the client typed into a payload.
            EXPECT_EQ(att->key().view(), client_pub);
            EXPECT_NE(att->key().view(), server_pub)
                << "attested the wrong side of the handshake";

            // Same message, but the domain is no longer enforced: teardown
            // has deregistered it.  The message still carries a genuine
            // User-Id, and it must STILL not be attested — enforcement is
            // the thing that earns trust, not the shape of the value.
            handle = ZapDomainHandle{};
            EXPECT_FALSE(
                pylabhub::utils::security::AttestedKey::from_message(pull, kept).has_value())
                << "attested a message on a domain nobody is enforcing any more";
        },
        "zap_router::attestation_matches_real_handshake", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int attestation_requires_enforced_domain(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            using pylabhub::utils::security::AttestedKey;

            // Enforcement is what earns trust — not the shape of the value.
            //
            // This test shrank when the factory started taking the socket.
            // It used to assert that a malformed `user_id` string, an empty
            // one, and an unregistered domain name were all refused. Those
            // cases only existed because a caller could hand the factory
            // arbitrary strings: no real handshake produces a malformed
            // `User-Id`, because our own ZAP reply is what sets it. Removing
            // the string door removed the reachable ways to be wrong, so what
            // remains is the one distinction that is real — was ZAP enforcing
            // on the socket this message arrived on, or not.
            const auto [server_pub, server_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();

            // A CURVE server with NO zap_domain: the handshake completes (no
            // ZAP is consulted at all) and libzmq sets no `User-Id`. This is
            // the case a peer can actually reach — libzmq lets a client send
            // its own ZMTP property named "User-Id", and it is only shadowed
            // where ZAP properties exist to shadow it.
            {
                zmq::socket_t pull(pylabhub::hub::get_zmq_context(), zmq::socket_type::pull);
                pull.set(zmq::sockopt::linger, 0);
                pull.set(zmq::sockopt::curve_server, 1);
                pull.set(zmq::sockopt::curve_publickey, server_pub);
                pull.set(zmq::sockopt::curve_secretkey, server_sec);
                pull.bind("tcp://127.0.0.1:0"); // deliberately no zap_domain
                EXPECT_FALSE(
                    attest_via_handshake(pull, server_pub, client_pub, client_sec).has_value())
                    << "attested on a socket with no enforced domain — an unvouched value "
                       "would have been dressed up as proof";
            }

            // Same client, same key, but now on a socket whose domain IS
            // registered and enforced.  The ONLY difference is provenance.
            {
                const std::string domain = "test.zap.attest.enforced";
                auto pull = bind_pull_server(server_pub, server_sec, domain);
                InMemoryAdmission admission;
                PeerAllowlist al;
                al.peers.insert(PeerIdentity{pylabhub::utils::security::kCurveMechanism, client_pub});
                (void)admission.set_peer_allowlist(std::move(al));
                auto handle = ZapRouter::instance().register_domain(domain, admission);
                ZapPumpThread pump;

                const auto att = attest_via_handshake(pull, server_pub, client_pub, client_sec);
                ASSERT_TRUE(att.has_value()) << "enforced domain produced no attestation";
                EXPECT_EQ(att->key().view(), client_pub);
            }
        },
        "zap_router::attestation_requires_enforced_domain", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int handle_unregisters_on_destruction(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            InMemoryAdmission a;
            {
                auto h = ZapRouter::instance().register_domain("test.zap.handle.lifetime", a);
                EXPECT_TRUE(h.is_active());
                EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);
            }
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 0u);
        },
        "zap_router::handle_unregisters_on_destruction", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int duplicate_registration_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            InMemoryAdmission a;
            InMemoryAdmission b;
            auto h = ZapRouter::instance().register_domain("test.zap.duplicate.domain", a);
            // (void) silences [[nodiscard]] on register_domain — the
            // call is expected to throw, so the would-be handle never
            // exists.  Same idiom at the two other EXPECT_THROW sites.
            EXPECT_THROW(
                (void)ZapRouter::instance().register_domain("test.zap.duplicate.domain", b),
                std::runtime_error);
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);
        },
        "zap_router::duplicate_registration_throws", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int empty_domain_throws(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            InMemoryAdmission a;
            EXPECT_THROW((void)ZapRouter::instance().register_domain("", a), std::runtime_error);
        },
        "zap_router::empty_domain_throws", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// `null_admission_throws` was DELETED — `register_domain` now takes
// `PeerAdmission &`, so a null admission is rejected by the type
// system at compile time; no runtime test is needed (and none could
// be written without `reinterpret_cast<PeerAdmission &>(*nullptr)`,
// which would itself be UB).  See task #217.

int pump_one_when_unloaded_returns_false(const char * /*tmpdir*/)
{
    return run_gtest_worker(
        [&]()
        {
            // No register_domain has been called → ZapRouter dynamic
            // module is NOT yet loaded → impl_->sock is empty →
            // pump_one returns false immediately.  Pins the
            // module-not-loaded contract.
            EXPECT_FALSE(ZapRouter::instance().pump_one(std::chrono::milliseconds(10)));
        },
        "zap_router::pump_one_when_unloaded_returns_false", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ── Direct REQ-side helpers for raw frame tests ────────────────────────────
//
// These tests bypass the libzmq CURVE handshake entirely and drive the
// ZAP REP socket directly via a sibling REQ socket on the same ZMQ
// context.  This is the only way to pin our handling of frames that
// libzmq itself would never produce (truncated requests, unsupported
// versions).  In production no caller does this — but a libzmq bug, an
// internal corruption, or a future ZMQ wire-format upgrade would, and
// the server must remain coherent.

constexpr const char *kZapInproc = "inproc://zeromq.zap.01";

zmq::socket_t make_req_to_zap()
{
    zmq::socket_t req(pylabhub::hub::get_zmq_context(), zmq::socket_type::req);
    req.set(zmq::sockopt::linger, 0);
    req.set(zmq::sockopt::rcvtimeo, 1500); // generous; tests rely on
                                           // the REP side replying
    req.connect(kZapInproc);
    return req;
}

void send_multipart(zmq::socket_t &sock, const std::vector<std::string> &frames)
{
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        const auto flags =
            (i + 1 < frames.size()) ? zmq::send_flags::sndmore : zmq::send_flags::none;
        zmq::message_t m(frames[i].size());
        if (!frames[i].empty())
            std::memcpy(m.data(), frames[i].data(), frames[i].size());
        (void)sock.send(m, flags);
    }
}

/// Receive a ZAP reply from @p sock.  Returns the 6 reply frames or
/// empty vector on timeout/error.  Pins the wire contract — caller
/// checks every frame.
std::vector<std::string> recv_zap_reply(zmq::socket_t &sock)
{
    std::vector<zmq::message_t> raw;
    try
    {
        auto rr = zmq::recv_multipart(sock, std::back_inserter(raw), zmq::recv_flags::none);
        if (!rr.has_value())
            return {};
    }
    catch (const zmq::error_t &)
    {
        return {};
    }
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (auto &m : raw)
        out.push_back(m.to_string());
    return out;
}

/// Hand-craft a valid 7-frame ZAP REQ for the given domain, mechanism,
/// and 32-byte raw credentials.  RFC 27 frame order.
std::vector<std::string> make_zap_req(const std::string &version, const std::string &request_id,
                                      const std::string &domain, const std::string &mechanism,
                                      const std::string &raw_credentials)
{
    return {
        version,       request_id,      domain, std::string{}, // address (peer IP) — ignored
        std::string{},                                         // identity — ignored
        mechanism,     raw_credentials,
    };
}

std::string make_raw_pubkey_from_z85(const std::string &z85)
{
    std::array<std::uint8_t, 32> raw{};
    if (zmq_z85_decode(raw.data(), z85.c_str()) == nullptr)
        throw std::runtime_error("zmq_z85_decode failed");
    return std::string(reinterpret_cast<const char *>(raw.data()), raw.size());
}

// ── New scenarios ─────────────────────────────────────────────────────────

/// **C-Z1 regression test.**  A REQ that sends fewer than 7 frames must
/// receive a 6-frame "400 Malformed request" reply, NOT silently wedge
/// the REP socket FSM.  Then a follow-up valid REQ must still get a
/// proper reply — proves the socket recovered.
int pump_one_malformed_short_request_replies_and_recovers(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [server_pub, server_sec] = make_keypair();
            const auto [client_pub, client_sec] = make_keypair();
            const std::string domain = "test.zap.malformed.recovery";

            InMemoryAdmission admission;
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", client_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));

            auto handle = ZapRouter::instance().register_domain(domain, admission);
            ASSERT_TRUE(handle.is_active());

            ZapPumpThread pump;

            const auto baseline_denied = ZapRouter::instance().denied_count();
            const auto baseline_allowed = ZapRouter::instance().allowed_count();

            // ── 1. Send a truncated 3-frame request (version + req_id
            //       + domain only).  RFC 27 demands 7.
            {
                zmq::socket_t req = make_req_to_zap();
                send_multipart(req, {"1.0", "req-truncated-1", domain});
                auto reply = recv_zap_reply(req);

                // Pin: the REP side MUST have replied with the
                // 6-frame "400 Malformed request" envelope.
                ASSERT_EQ(reply.size(), 6u) << "REP socket wedged — no reply received for "
                                               "malformed REQ.  This is the C-Z1 bug.";
                EXPECT_EQ(reply[0], "1.0");
                EXPECT_EQ(reply[1], "req-truncated-1");
                EXPECT_EQ(reply[2], "400");
                EXPECT_EQ(reply[3], "Malformed request");
                EXPECT_EQ(reply[4], ""); // user_id empty on deny
                EXPECT_EQ(reply[5], ""); // metadata empty
            }

            // ── 2. Pump must have counted the deny.
            //       (Brief wait for the relaxed-order store to land.)
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (ZapRouter::instance().denied_count() == baseline_denied &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            EXPECT_EQ(ZapRouter::instance().denied_count(), baseline_denied + 1u)
                << "Malformed-request DENY path must increment the "
                   "denied counter.";

            // ── 3. Socket recovery probe: send a VALID 7-frame REQ
            //       with the allowed peer's pubkey.  Must receive
            //       a "200 OK" reply.
            {
                zmq::socket_t req = make_req_to_zap();
                send_multipart(req, make_zap_req("1.0", "req-recovery-1", domain, "CURVE",
                                                 make_raw_pubkey_from_z85(client_pub)));
                auto reply = recv_zap_reply(req);

                ASSERT_EQ(reply.size(), 6u) << "Follow-up valid REQ received no reply — REP "
                                               "socket is wedged.  C-Z1 fix did not recover.";
                EXPECT_EQ(reply[0], "1.0");
                EXPECT_EQ(reply[1], "req-recovery-1");
                EXPECT_EQ(reply[2], "200");
                EXPECT_EQ(reply[3], "OK");
                EXPECT_EQ(reply[4], client_pub); // user_id = pubkey
            }

            // ── 4. Allow counter must reflect the recovery probe.
            const auto deadline2 =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (ZapRouter::instance().allowed_count() == baseline_allowed &&
                   std::chrono::steady_clock::now() < deadline2)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            EXPECT_EQ(ZapRouter::instance().allowed_count(), baseline_allowed + 1u)
                << "Recovery valid-REQ ALLOW path must increment the "
                   "allowed counter.";
        },
        "zap_router::pump_one_malformed_short_request_replies_and_recovers",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(), pylabhub::hub::GetZMQContextModule());
}

/// **H-Z1 regression test.**  A REQ with version != "1.0" must be
/// rejected with "400 Bad version", not silently processed as 1.0.
int pump_one_bad_version_replies_400(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [client_pub, client_sec] = make_keypair();
            const std::string domain = "test.zap.bad.version";

            InMemoryAdmission admission;
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", client_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));

            auto handle = ZapRouter::instance().register_domain(domain, admission);
            ZapPumpThread pump;

            const auto baseline_denied = ZapRouter::instance().denied_count();

            zmq::socket_t req = make_req_to_zap();
            // Same shape as a real CURVE REQ but version "2.0" —
            // libzmq will never produce this today but a future
            // protocol bump might.
            send_multipart(req, make_zap_req("2.0", "req-badver-1", domain, "CURVE",
                                             make_raw_pubkey_from_z85(client_pub)));
            auto reply = recv_zap_reply(req);

            ASSERT_EQ(reply.size(), 6u);
            EXPECT_EQ(reply[0], "1.0"); // server replies in 1.0
            EXPECT_EQ(reply[1], "req-badver-1");
            EXPECT_EQ(reply[2], "400");
            EXPECT_EQ(reply[3], "Bad version");
            EXPECT_EQ(reply[4], "");

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (ZapRouter::instance().denied_count() == baseline_denied &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            EXPECT_EQ(ZapRouter::instance().denied_count(), baseline_denied + 1u);

            // Follow-up valid REQ proves the socket is still alive.
            zmq::socket_t req2 = make_req_to_zap();
            send_multipart(req2, make_zap_req("1.0", "req-after-badver", domain, "CURVE",
                                              make_raw_pubkey_from_z85(client_pub)));
            auto reply2 = recv_zap_reply(req2);
            ASSERT_EQ(reply2.size(), 6u);
            EXPECT_EQ(reply2[2], "200");
        },
        "zap_router::pump_one_bad_version_replies_400", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// Pins the existing non-CURVE-mechanism deny path + counter.
int pump_one_non_curve_mechanism_replies_400(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const std::string domain = "test.zap.plain.mech";
            InMemoryAdmission admission;
            auto handle = ZapRouter::instance().register_domain(domain, admission);
            ZapPumpThread pump;

            const auto baseline_denied = ZapRouter::instance().denied_count();

            zmq::socket_t req = make_req_to_zap();
            // PLAIN mechanism: credentials are username + password
            // strings, but the size of the credentials field doesn't
            // matter — mechanism alone triggers the reject.
            send_multipart(req, make_zap_req("1.0", "req-plain-1", domain, "PLAIN",
                                             std::string("user\0pass", 9)));
            auto reply = recv_zap_reply(req);

            ASSERT_EQ(reply.size(), 6u);
            EXPECT_EQ(reply[1], "req-plain-1");
            EXPECT_EQ(reply[2], "400");
            EXPECT_EQ(reply[3], "Unsupported mechanism");

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (ZapRouter::instance().denied_count() == baseline_denied &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            EXPECT_EQ(ZapRouter::instance().denied_count(), baseline_denied + 1u);
        },
        "zap_router::pump_one_non_curve_mechanism_replies_400", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// Real CURVE handshake.  Denied peer → denied_count must increment.
/// This converts the "blocked-by-timeout" assertion of the original
/// L2 ZmqQueue test into a direct path-pinning assertion at the
/// ZapRouter layer: we observe the deny PATH executed, not just the
/// absence of a delivery.
int handshake_deny_increments_denied_counter(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [server_pub, server_sec] = make_keypair();
            const auto [denied_pub, denied_sec] = make_keypair();
            const std::string domain = "test.zap.deny.counter";

            InMemoryAdmission admission; // empty allowlist → deny-all
            auto handle = ZapRouter::instance().register_domain(domain, admission);
            auto pull = bind_pull_server(server_pub, server_sec, domain);
            ZapPumpThread pump;

            const auto baseline_denied = ZapRouter::instance().denied_count();

            // Drive a CURVE handshake that ZAP will deny.
            const bool delivered = handshake_and_deliver(pull, server_pub, denied_pub, denied_sec,
                                                         std::chrono::milliseconds(500));
            EXPECT_FALSE(delivered);

            // The handshake denial may take a moment to surface as a
            // ZAP request.  Wait for the counter to bump.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            while (ZapRouter::instance().denied_count() == baseline_denied &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));

            EXPECT_GT(ZapRouter::instance().denied_count(), baseline_denied)
                << "Denied CURVE handshake did NOT bump denied_count "
                   "— the deny PATH did not execute.  A 'no message "
                   "arrived' assertion alone cannot distinguish this "
                   "from a network stall.";
            EXPECT_EQ(ZapRouter::instance().allowed_count(), 0u);
        },
        "zap_router::handshake_deny_increments_denied_counter", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int handshake_allow_increments_allowed_counter(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [server_pub, server_sec] = make_keypair();
            const auto [allowed_pub, allowed_sec] = make_keypair();
            const std::string domain = "test.zap.allow.counter";

            InMemoryAdmission admission;
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", allowed_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));

            auto handle = ZapRouter::instance().register_domain(domain, admission);
            auto pull = bind_pull_server(server_pub, server_sec, domain);
            ZapPumpThread pump;

            const auto baseline_allowed = ZapRouter::instance().allowed_count();

            const bool delivered = handshake_and_deliver(pull, server_pub, allowed_pub, allowed_sec,
                                                         std::chrono::milliseconds(1500));
            EXPECT_TRUE(delivered);

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            while (ZapRouter::instance().allowed_count() == baseline_allowed &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));

            EXPECT_GT(ZapRouter::instance().allowed_count(), baseline_allowed)
                << "Successful CURVE handshake did NOT bump "
                   "allowed_count — delivery happened by some other "
                   "path (or libzmq never invoked ZAP).";
        },
        "zap_router::handshake_allow_increments_allowed_counter", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// Handle move-construct: ownership transfers; source becomes inactive
/// and its destructor is a no-op (no double-unregister).
int handle_move_construct_transfers_ownership(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            InMemoryAdmission a;
            const std::string domain = "test.zap.move.ctor";

            auto h1 = ZapRouter::instance().register_domain(domain, a);
            ASSERT_TRUE(h1.is_active());
            ASSERT_EQ(h1.domain(), domain);
            ASSERT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);

            ZapDomainHandle h2(std::move(h1));
            EXPECT_FALSE(h1.is_active()) << "Moved-from handle must report inactive.";
            EXPECT_TRUE(h2.is_active());
            EXPECT_EQ(h2.domain(), domain);
            // Routing-map count unchanged — same registration, new owner.
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);

            // Destroy h2 → unregister; h1's destructor is then a no-op.
            {
                ZapDomainHandle take_ownership = std::move(h2);
            }
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 0u);
        },
        "zap_router::handle_move_construct_transfers_ownership", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

/// Move-assign: the destination's prior registration MUST be released
/// before adopting the source's.  Otherwise the prior domain leaks in
/// the routing map (security: an old gate-handle that operator code
/// thought was destroyed would still admit peers).
int handle_move_assign_releases_previous_registration(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            InMemoryAdmission a;
            InMemoryAdmission b;

            auto hA = ZapRouter::instance().register_domain("test.zap.move.assign.A", a);
            auto hB = ZapRouter::instance().register_domain("test.zap.move.assign.B", b);
            ASSERT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 2u);

            // hA = std::move(hB): hA's prior "A" registration must be
            // released; hA now owns "B".
            hA = std::move(hB);
            EXPECT_TRUE(hA.is_active());
            EXPECT_EQ(hA.domain(), "test.zap.move.assign.B");
            EXPECT_FALSE(hB.is_active());

            // Exactly one registration remains — "B".  If the prior
            // "A" wasn't released, count would still be 2.
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);

            // Spot-check the surviving entry is B by attempting a
            // re-register on "A" (which should now succeed — proves
            // "A" is gone) and on "B" (which should throw).
            EXPECT_NO_THROW({
                auto reA = ZapRouter::instance().register_domain("test.zap.move.assign.A", a);
                EXPECT_TRUE(reA.is_active());
            });
            EXPECT_THROW((void)ZapRouter::instance().register_domain("test.zap.move.assign.B", b),
                         std::runtime_error);
        },
        "zap_router::handle_move_assign_releases_previous_registration",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(), pylabhub::hub::GetZMQContextModule());
}

// ============================================================================
// Round-3 Slice A — UAF + reentrance + single-pumper invariants
//
// These four scenarios pin the architectural fixes from task #215
// (zap_router.cpp::pump_one + register_domain + unregister_domain_).
// Each test sets up a custom PeerAdmission that exercises one corner
// of the contract.  Mutation sweeps in the commit message confirm
// each test catches the bug it pins.
// ============================================================================

namespace round3
{

// Admission whose `is_peer_allowed` parks on a promise.  Used by the
// UAF test to deterministically interleave a pump_one mid-flight with
// a destructor on a different thread.
class BlockingAdmission final : public PeerAdmission
{
  public:
    BlockingAdmission() : release_fut_(release_promise_.get_future()) {}

    bool set_peer_allowlist(PeerAllowlist al) override
    {
        std::lock_guard<std::mutex> lk(al_mu_);
        al_ = std::move(al);
        return true;
    }
    std::optional<PeerAllowlist> peer_allowlist_snapshot() const override
    {
        std::lock_guard<std::mutex> lk(al_mu_);
        return al_;
    }
    bool is_peer_allowed(const PeerIdentity &p) const override
    {
        entered_.store(true);
        release_fut_.wait();
        exited_.store(true);
        std::lock_guard<std::mutex> lk(al_mu_);
        return al_.has_value() && al_->contains(p);
    }

    // Returns true if entered_ flips within `timeout`; false on
    // timeout.  Caller is expected to assert on the return value so a
    // never-entered admission fails with a clean test diagnostic
    // rather than an opaque subprocess hang.
    [[nodiscard]] bool
    wait_until_entered(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!entered_.load())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    void release() { release_promise_.set_value(); }
    bool was_exited() const { return exited_.load(); }

  private:
    mutable std::mutex al_mu_;
    std::optional<PeerAllowlist> al_;
    mutable std::atomic<bool> entered_{false};
    mutable std::atomic<bool> exited_{false};
    std::promise<void> release_promise_;
    mutable std::shared_future<void> release_fut_;
};

// Admission whose `is_peer_allowed` tries to register a NEW domain
// from inside the call.  Pins the RecursionGuard refuse-and-log path
// in `register_domain`.
class ReEntrantRegisterAdmission final : public PeerAdmission
{
  public:
    bool set_peer_allowlist(PeerAllowlist al) override
    {
        std::lock_guard<std::mutex> lk(al_mu_);
        al_ = std::move(al);
        return true;
    }
    std::optional<PeerAllowlist> peer_allowlist_snapshot() const override
    {
        std::lock_guard<std::mutex> lk(al_mu_);
        return al_;
    }
    bool is_peer_allowed(const PeerIdentity &p) const override
    {
        // Attempt the prohibited reentrant register.  Per the
        // RecursionGuard contract, this MUST return an inactive
        // handle and emit an ERROR log; nothing must throw, deadlock,
        // or actually register.
        nested_handle_.emplace(ZapRouter::instance().register_domain(
            "test.zap.round3.reentrant_register.nested", nested_admission_));
        nested_register_attempted_.store(true);
        nested_register_succeeded_.store(nested_handle_->is_active());
        std::lock_guard<std::mutex> lk(al_mu_);
        return al_.has_value() && al_->contains(p);
    }

    bool nested_register_attempted() const { return nested_register_attempted_.load(); }
    bool nested_register_succeeded() const { return nested_register_succeeded_.load(); }

  private:
    mutable std::mutex al_mu_;
    std::optional<PeerAllowlist> al_;
    mutable InMemoryAdmission nested_admission_;
    mutable std::optional<ZapDomainHandle> nested_handle_{};
    mutable std::atomic<bool> nested_register_attempted_{false};
    mutable std::atomic<bool> nested_register_succeeded_{false};
};

// Admission whose `is_peer_allowed` runs a test-supplied callback
// inside the call.  The callback is what the test uses to provoke
// the reentrant `~ZapDomainHandle` → `unregister_domain_` PLH_PANIC.
// A `std::function<void()>` callback (set by the test fixture)
// replaces an earlier back-pointer into the fixture's storage; the
// callback decouples the admission's reentrance behavior from any
// particular handle's storage location.
class ReEntrantUnregisterAdmission final : public PeerAdmission
{
  public:
    bool set_peer_allowlist(PeerAllowlist al) override
    {
        std::lock_guard<std::mutex> lk(al_mu_);
        al_ = std::move(al);
        return true;
    }
    std::optional<PeerAllowlist> peer_allowlist_snapshot() const override
    {
        std::lock_guard<std::mutex> lk(al_mu_);
        return al_;
    }
    bool is_peer_allowed(const PeerIdentity &p) const override
    {
        // Run the test-installed callback inside the admission
        // decision.  In the panic test the callback resets the
        // outer handle: ~ZapDomainHandle → unregister_domain_ →
        // RecursionGuard detects we are inside pump_one's admission
        // scope on this thread → PLH_PANIC.  The process abort is
        // the test signal.
        if (on_admit_)
            on_admit_();
        // Unreachable when on_admit_ provokes the panic; otherwise
        // we fall through to the allowlist check.
        std::lock_guard<std::mutex> lk(al_mu_);
        return al_.has_value() && al_->contains(p);
    }

    // Test fixture installs the reentrance trigger here.  Empty by
    // default — no callback runs.
    std::function<void()> on_admit_;

  private:
    mutable std::mutex al_mu_;
    std::optional<PeerAllowlist> al_;
};

// Trivial admission for the concurrent-pumpers test — admission body
// never runs (the second pumper PANICs before reaching the call).
class NoopAdmission final : public PeerAdmission
{
  public:
    bool set_peer_allowlist(PeerAllowlist) override { return true; }
    std::optional<PeerAllowlist> peer_allowlist_snapshot() const override { return std::nullopt; }
    bool is_peer_allowed(const PeerIdentity &) const override { return true; }
};

} // namespace round3

// ── Scenario 1 — UAF destructor blocks until admission returns ──────────────
//
// Pins ZapRouter::pump_one's shared_lock extension across the
// `admission->is_peer_allowed(...)` call.  Mechanism:
//   - Thread A (pump): receives a valid ZAP request, calls
//     admission.is_peer_allowed → parks on `release_fut_`.
//   - Thread B (destroyer): moves the handle into a local scope, so
//     its destructor calls unregister_domain_'s unique_lock.
//   - Invariant: unique_lock blocks while pump_one's shared_lock is
//     held.  ~ZapDomainHandle MUST NOT return until admission returns.
//   - Test releases the promise → admission returns → shared_lock
//     released → unique_lock proceeds → destructor returns.
//
// Without the fix, pump_one releases shared_lock BEFORE the admission
// call, so the destructor on Thread B completes immediately while
// admission is still mid-flight — the assertion
// `EXPECT_FALSE(destroyed.load())` fails.
int round3_uaf_destructor_blocks_until_admission_returns(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [client_pub, client_sec] = make_keypair();
            const std::string domain = "test.zap.round3.uaf.blocking";

            round3::BlockingAdmission admission;
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", client_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));

            ZapDomainHandle handle = ZapRouter::instance().register_domain(domain, admission);
            ASSERT_TRUE(handle.is_active());

            ZapPumpThread pump;

            // Drive a valid ZAP request from a side thread; pump_one
            // picks it up and parks inside admission.  The reply
            // round-trip completes only after we release the promise.
            std::thread req_sender(
                [&]
                {
                    zmq::socket_t req = make_req_to_zap();
                    send_multipart(req, make_zap_req("1.0", "round3-uaf-1", domain, "CURVE",
                                                     make_raw_pubkey_from_z85(client_pub)));
                    auto reply = recv_zap_reply(req);
                    if (reply.size() == 6u)
                        EXPECT_EQ(reply[2], "200");
                    else
                        ADD_FAILURE()
                            << "req_sender got reply.size()=" << reply.size() << " (expected 6).";
                });

            ASSERT_TRUE(admission.wait_until_entered())
                << "Pump never delivered the request to admission "
                   "within 2s — synthetic ZAP request lost or pump "
                   "stalled.  Test cannot proceed to the UAF check.";

            // Start destruction on a separate thread.  The destructor
            // (`~local` at the inner scope's closing brace) MUST block
            // waiting for pump_one's shared_lock to release.  We
            // signal `destroyer_entered_destructor` immediately BEFORE
            // ~local runs (so the test thread can synchronize on
            // "destructor has been entered") and set `destroyed` AFTER
            // ~local returns (so the flag flips only when the
            // destructor actually completes, not when the lambda body
            // merely started).  This gives us two positive
            // synchronization points instead of a blind sleep.
            std::atomic<bool> destroyer_entered_destructor{false};
            std::atomic<bool> destroyed{false};
            std::thread destroyer(
                [&]
                {
                    {
                        ZapDomainHandle local = std::move(handle);
                        destroyer_entered_destructor.store(true);
                    } // ← ~local executes here.  With the fix:
                      // unregister_domain_'s unique_lock blocks here
                      // until pump_one's shared_lock releases (which
                      // requires admission to return first).
                    destroyed.store(true);
                });

            // Wait for the destroyer to actually enter the destructor
            // (positive signal — no blind sleep).  2s timeout matches
            // wait_until_entered's default.
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!destroyer_entered_destructor.load() &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ASSERT_TRUE(destroyer_entered_destructor.load())
                    << "Destroyer thread did not enter ~ZapDomainHandle "
                       "within 2s — thread spawn or move stalled.";
            }
            // Give the destructor a brief window to MAYBE return (it
            // shouldn't — the unique_lock should block).  This is the
            // only timing-bound bit of the test; 50ms is enough to
            // distinguish "blocked indefinitely" from "returned
            // immediately" on any sane scheduler.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // INVARIANT: destructor still blocked.  If this fires, the
            // UAF window is open: pump_one released its shared_lock
            // before calling is_peer_allowed, so unregister_domain_'s
            // unique_lock proceeded while the admission pointer was
            // still in use.  Post-AUTH-2 this is a live UAF on every
            // handshake-vs-teardown race.
            EXPECT_FALSE(destroyed.load()) << "~ZapDomainHandle returned BEFORE in-flight "
                                              "is_peer_allowed completed — UAF window OPEN.  "
                                              "pump_one must hold registered_mu shared across "
                                              "the admission call.";
            EXPECT_FALSE(admission.was_exited())
                << "Admission body somehow exited before release — "
                   "test logic broken, re-check.";

            // Release: admission returns → shared_lock released →
            // unique_lock proceeds → destructor completes.
            admission.release();

            destroyer.join();
            req_sender.join();

            EXPECT_TRUE(destroyed.load());
            EXPECT_TRUE(admission.was_exited());
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 0u);
        },
        "zap_router::round3_uaf_destructor_blocks_until_admission_returns",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(), pylabhub::hub::GetZMQContextModule());
}

// ── Scenario 2 — Reentrant register_domain refused ──────────────────────────
//
// Pins the RecursionGuard refuse-and-log path in register_domain.
// An admission that calls ZapRouter::register_domain from inside its
// own is_peer_allowed MUST get an inactive handle back; the router
// MUST NOT actually register the nested domain.
//
// Without the fix, register_domain would attempt to acquire its
// unique_lock while the same thread holds pump_one's shared_lock,
// which is undefined behavior under std::shared_mutex (the thread
// already holds shared; attempting to acquire shared OR unique
// without release is UB) — observable as silent deadlock or worse.
int round3_reentrant_register_refused(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [client_pub, client_sec] = make_keypair();
            const std::string domain = "test.zap.round3.reentrant_register.outer";

            round3::ReEntrantRegisterAdmission admission;
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", client_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));

            auto handle = ZapRouter::instance().register_domain(domain, admission);
            ASSERT_TRUE(handle.is_active());
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);

            ZapPumpThread pump;

            // Drive a valid ZAP request — pump_one will reach
            // admission, which attempts the nested register.
            zmq::socket_t req = make_req_to_zap();
            send_multipart(req, make_zap_req("1.0", "round3-reentrant-1", domain, "CURVE",
                                             make_raw_pubkey_from_z85(client_pub)));
            auto reply = recv_zap_reply(req);
            ASSERT_EQ(reply.size(), 6u);
            EXPECT_EQ(reply[2], "200") << "Outer admission must still admit the client; the "
                                          "nested register attempt should not block the "
                                          "outer decision.";

            // The reentrant register MUST have been refused.  If it
            // succeeded, the RecursionGuard check is missing from
            // register_domain.
            EXPECT_TRUE(admission.nested_register_attempted())
                << "Admission body did not attempt the nested register "
                   "— test logic broken.";
            EXPECT_FALSE(admission.nested_register_succeeded())
                << "register_domain accepted a call from inside "
                   "is_peer_allowed.  RecursionGuard check is missing "
                   "or wrong key.";

            // Router state: only the outer domain is registered.
            EXPECT_EQ(ZapRouter::instance().registered_domain_count_for_test(), 1u);
        },
        "zap_router::round3_reentrant_register_refused", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ── Scenario 3 — Reentrant unregister_domain_ panics ────────────────────────
//
// Pins the PLH_PANIC path in unregister_domain_.  An admission whose
// is_peer_allowed triggers `~ZapDomainHandle` from inside the call
// would leave the router with a dangling map entry — unrecoverable.
// The runtime MUST panic instead of silently returning.
//
// Death test: SpawnWorker expects a non-zero exit code + a stderr
// substring matching the PLH_PANIC message.
int round3_reentrant_unregister_panics(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            const auto [client_pub, client_sec] = make_keypair();
            const std::string domain = "test.zap.round3.reentrant_unregister";

            round3::ReEntrantUnregisterAdmission admission;
            PeerAllowlist al;
            al.peers.insert(PeerIdentity{"curve", client_pub});
            ASSERT_TRUE(admission.set_peer_allowlist(al));

            std::optional<ZapDomainHandle> handle =
                ZapRouter::instance().register_domain(domain, admission);
            ASSERT_TRUE(handle->is_active());
            admission.on_admit_ = [&handle] { handle.reset(); };

            ZapPumpThread pump;

            // Drive a valid ZAP request.  pump_one reaches admission,
            // admission resets the held handle, ~ZapDomainHandle
            // calls unregister_domain_, RecursionGuard fires, process
            // panics.  No reply ever returns.
            zmq::socket_t req = make_req_to_zap();
            send_multipart(req, make_zap_req("1.0", "round3-reentrant-unreg-1", domain, "CURVE",
                                             make_raw_pubkey_from_z85(client_pub)));
            // We must NEVER reach the assertion below — the pump
            // thread should have aborted the process.  Wait briefly
            // so the abort surfaces before we exit cleanly.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            FAIL() << "Worker still alive after admission triggered "
                      "reentrant unregister — PLH_PANIC did not fire.  "
                      "RecursionGuard check is missing from "
                      "unregister_domain_.";
        },
        "zap_router::round3_reentrant_unregister_panics", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ── Scenario 4 — Concurrent pumpers panic ───────────────────────────────────
//
// Pins the single-pumper invariant via the atomic counter PANIC in
// pump_one.  Two threads enter pump_one concurrently; whichever loses
// the race observes the counter > 1 and panics.
//
// Death test: non-zero exit + PLH_PANIC substring.
int round3_concurrent_pumpers_panic(const char *)
{
    return run_gtest_worker(
        [&]()
        {
            round3::NoopAdmission admission;
            auto handle =
                ZapRouter::instance().register_domain("test.zap.round3.two_pumpers", admission);
            ASSERT_TRUE(handle.is_active());

            // Spawn two pumpers.  pump_one's atomic counter MUST
            // detect the simultaneous entry and panic.  The order
            // of panic vs the slower pumper's entry is racy but
            // either way, at least one PANIC fires.
            ZapPumpThread pump1(std::chrono::milliseconds(50));
            ZapPumpThread pump2(std::chrono::milliseconds(50));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            FAIL() << "Worker still alive with two concurrent "
                      "ZapPumpThread instances — pump_one's "
                      "single-pumper PANIC is missing.";
        },
        "zap_router::round3_concurrent_pumpers_panic", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ── Dispatcher + registrar ─────────────────────────────────────────────────

int dispatch_zap_router(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    std::string mode = argv[1];
    const auto dot = mode.find('.');
    if (dot == std::string::npos)
        return -1;
    const std::string module = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "zap_router")
        return -1;

    if (argc < 3)
    {
        std::fprintf(stderr, "zap_router.%s: missing <tmpdir> arg\n", scenario.c_str());
        return 1;
    }
    const char *tmpdir = argv[2];

    if (scenario == "handshake_accept_deny_cycle")
        return handshake_accept_deny_cycle(tmpdir);
    if (scenario == "claim_check_covers_every_verdict")
        return claim_check_covers_every_verdict(tmpdir);
    if (scenario == "authority_answers_questions_about_attested_keys")
        return authority_answers_questions_about_attested_keys(tmpdir);
    if (scenario == "attestation_matches_real_handshake")
        return attestation_matches_real_handshake(tmpdir);
    if (scenario == "attestation_requires_enforced_domain")
        return attestation_requires_enforced_domain(tmpdir);
    if (scenario == "unknown_domain_denies")
        return unknown_domain_denies(tmpdir);
    if (scenario == "handle_unregisters_on_destruction")
        return handle_unregisters_on_destruction(tmpdir);
    if (scenario == "duplicate_registration_throws")
        return duplicate_registration_throws(tmpdir);
    if (scenario == "empty_domain_throws")
        return empty_domain_throws(tmpdir);
    if (scenario == "pump_one_when_unloaded_returns_false")
        return pump_one_when_unloaded_returns_false(tmpdir);
    if (scenario == "pump_one_malformed_short_request_replies_and_recovers")
        return pump_one_malformed_short_request_replies_and_recovers(tmpdir);
    if (scenario == "pump_one_bad_version_replies_400")
        return pump_one_bad_version_replies_400(tmpdir);
    if (scenario == "pump_one_non_curve_mechanism_replies_400")
        return pump_one_non_curve_mechanism_replies_400(tmpdir);
    if (scenario == "handshake_deny_increments_denied_counter")
        return handshake_deny_increments_denied_counter(tmpdir);
    if (scenario == "handshake_allow_increments_allowed_counter")
        return handshake_allow_increments_allowed_counter(tmpdir);
    if (scenario == "handle_move_construct_transfers_ownership")
        return handle_move_construct_transfers_ownership(tmpdir);
    if (scenario == "handle_move_assign_releases_previous_registration")
        return handle_move_assign_releases_previous_registration(tmpdir);
    if (scenario == "round3_uaf_destructor_blocks_until_admission_returns")
        return round3_uaf_destructor_blocks_until_admission_returns(tmpdir);
    if (scenario == "round3_reentrant_register_refused")
        return round3_reentrant_register_refused(tmpdir);
    if (scenario == "round3_reentrant_unregister_panics")
        return round3_reentrant_unregister_panics(tmpdir);
    if (scenario == "round3_concurrent_pumpers_panic")
        return round3_concurrent_pumpers_panic(tmpdir);
    std::fprintf(stderr, "zap_router: unknown scenario '%s'\n", scenario.c_str());
    return 1;
}

struct ZapRouterWorkerRegistrar
{
    ZapRouterWorkerRegistrar() { ::register_worker_dispatcher(dispatch_zap_router); }
};
static ZapRouterWorkerRegistrar g_zap_router_registrar;

} // namespace
