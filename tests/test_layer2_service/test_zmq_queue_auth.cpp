/**
 * @file test_zmq_queue_auth.cpp
 * @brief Pattern 3 driver — ZmqQueue CURVE+ZAP auth (PeerAdmission Phase C).
 *
 * Each TEST_F spawns a subprocess.  Workers live in
 * `workers/zmq_queue_auth_workers.cpp`.
 */
#include "test_patterns.h"
#include "utils/broker_request_comm.hpp"
#include "utils/hub_zmq_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class ZmqQueueAuthTest : public IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::string unique_dir(const char *test_name)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_zmq_auth_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

TEST_F(ZmqQueueAuthTest, AllowedPeer_DeliversRoundTrip)
{
    auto w = SpawnWorker("zmq_queue_auth.auth_round_trip_allowed_peer_delivers",
                         {unique_dir("auth_round_trip_allowed_peer_delivers")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, UnallowedPeer_BlockedFromDelivery)
{
    auto w = SpawnWorker("zmq_queue_auth.auth_unallowed_peer_blocked",
                         {unique_dir("auth_unallowed_peer_blocked")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, AllowlistSwap_TakesEffectForNextConnection)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_allowlist_swap_takes_effect_for_next_connection",
        {unique_dir("auth_allowlist_swap_takes_effect_for_next_connection")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, LegacyUnauthFactories_Unchanged)
{
    auto w = SpawnWorker("zmq_queue_auth.legacy_unauth_factories_unchanged",
                         {unique_dir("legacy_unauth_factories_unchanged")});
    ExpectWorkerOk(w);
}

// ── Close-out commit 2 — security-grade tests (path-pinning) ───────────────
// See the worker file's narrative for the rationale.

TEST_F(ZmqQueueAuthTest, Deny_ThenAllowViaSwap_PinsPath)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_deny_then_allow_via_swap_pins_path",
        {unique_dir("auth_deny_then_allow_via_swap_pins_path")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, Swap_BlocksOldPeer_PinsData)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_swap_blocks_old_peer_pins_data",
        {unique_dir("auth_swap_blocks_old_peer_pins_data")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, SetPeerAllowlist_OnPullSide_ReturnsFalse)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_set_peer_allowlist_on_pull_side_returns_false",
        {unique_dir("auth_set_peer_allowlist_on_pull_side_returns_false")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, EmptyAllowlist_DeniesAll)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_empty_allowlist_denies_all",
        {unique_dir("auth_empty_allowlist_denies_all")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, Misconfig_ConnectMissingServerkey_FactoryReturnsNullptr)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_misconfig_connect_missing_serverkey_factory_returns_nullptr",
        {unique_dir("auth_misconfig_connect_missing_serverkey_factory_returns_nullptr")});
    // The factory's LOGGER_ERROR is the expected diagnostic path —
    // declare a unique substring of the single emitted ERROR line.
    ExpectWorkerOk(w, {}, {
        "connect-side ZmqAuthOptions requires serverkey_z85",
    });
}

TEST_F(ZmqQueueAuthTest, Misconfig_PubkeyWrongLength_FactoryReturnsNullptr)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_misconfig_pubkey_wrong_length_factory_returns_nullptr",
        {unique_dir("auth_misconfig_pubkey_wrong_length_factory_returns_nullptr")});
    // Misconfig branches the driver pins:
    //   - name-not-in-KeyStore (HEP-CORE-0040 §172)
    //   - serverkey missing on connect side (HEP-CORE-0035 §2)
    //   - serverkey wrong length (Z85 contract)
    //   - empty keystore_name (C1 / #157 strict enforcement)
    //
    // HEP-CORE-0040 §172 collapsed the keypair-byte fields into a
    // single `keystore_name` lookup; wrong-length pubkey/seckey is
    // now enforced inside `KeyStore::add_identity_from_z85` (covered
    // by L2 KeyStore tests).  Each substring below is unique to one
    // misconfig case so the framework would flag any new unexpected
    // ERROR or any silenced one.
    // The "keystore_name MUST be non-empty" entry appears twice
    // because two cases in the worker table exercise it — once on
    // the bind side and once on the connect side; each fires its
    // own LOGGER_ERROR line, and ExpectWorkerOk consumes one
    // substring per matched line.
    ExpectWorkerOk(w, {}, {
        "not present in KeyStore",
        "connect-side ZmqAuthOptions requires serverkey_z85",
        "serverkey_z85 must be exactly 40 chars",
        "keystore_name MUST be non-empty",
        "keystore_name MUST be non-empty",
    });
}

TEST_F(ZmqQueueAuthTest, AdmissionIsEnforced_Lifecycle)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_admission_is_enforced_lifecycle",
        {unique_dir("auth_admission_is_enforced_lifecycle")});
    ExpectWorkerOk(w);
}

// ───────────────────────────────────────────────────────────────────────
// #161 Phase 1 — magic-string + default-value mutation pins
// ───────────────────────────────────────────────────────────────────────
//
// These TEST() blocks run inline (no subprocess) because they pin
// constant-field default values, not runtime behaviour.  They close
// the mutation-test gaps where the wire path works against any
// non-empty name, so a refactor that changed the literal string
// would slip through every existing end-to-end test.

// HEP-CORE-0040 §172 contract: BRC::Config defaults `keystore_name`
// to the literal string "role_identity" — the same name
// `RoleConfig::load_keypair` seeds the KeyStore under.  If a
// refactor changes the default to "" or "role" or anything else,
// production role↔broker connections would fail at connect() time
// with "KeyStore entry '<new_default>' absent" because the seeded
// entry would no longer match.
//
// Mutations this pins:
//   - Changing the default to "" (would silently revert to the
//     no-CURVE path BRC::connect rejects).
//   - Changing the default to "role" / "role_id" / "identity"
//     (would force every test fixture + production caller to set
//     it explicitly, surfacing as a load-bearing refactor).
TEST(BrcConfigDefault, KeystoreNameIsRoleIdentityLiteral)
{
    pylabhub::hub::BrokerRequestComm::Config cfg;
    EXPECT_EQ(cfg.keystore_name, "role_identity")
        << "BRC::Config::keystore_name MUST default to the literal "
           "\"role_identity\" (HEP-CORE-0040 §172) — the name "
           "RoleConfig::load_keypair seeds.  A change here breaks "
           "every production role↔broker connection without any "
           "wire test catching it.";
}

// ZmqAuthOptions defaults all fields to empty.  After C1 (#157),
// passing an empty `keystore_name` to a `*_with_auth` factory is
// rejected at validate time (CURVE unconditional per HEP-CORE-0035
// §2) — callers MUST set it explicitly.  Plaintext sockets are only
// reachable through the legacy `push_to` / `pull_from` factories
// that don't take a `ZmqAuthOptions`.
//
// Mutations this pins:
//   - Defaulting `keystore_name` to a literal like "role_identity"
//     would silently force CURVE-with-that-identity on every queue
//     whose caller forgot to set it explicitly — exactly the kind
//     of accidental cross-binding the strict validator prevents.
//   - Defaulting `serverkey_z85` or `zap_domain` to non-empty would
//     cross-contaminate scenarios that intentionally leave them
//     blank for negative-path tests.
TEST(ZmqAuthOptionsDefault, KeystoreNameDefaultsToEmpty)
{
    pylabhub::hub::ZmqAuthOptions opts;
    EXPECT_TRUE(opts.keystore_name.empty())
        << "ZmqAuthOptions::keystore_name MUST default to empty "
           "(HEP-CORE-0040 §172 — the legacy-plaintext branch is "
           "selected by the empty default).  Got: "
        << opts.keystore_name;
    EXPECT_TRUE(opts.serverkey_z85.empty());
    EXPECT_TRUE(opts.zap_domain.empty());
}
