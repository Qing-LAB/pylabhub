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

// `LegacyUnauthFactories_Unchanged` test deleted in #160 (C4): the
// legacy plaintext `pull_from`/`push_to` factories no longer exist;
// every queue is CURVE-wired (HEP-CORE-0035 §2).  The new
// `pull_from`/`push_to` are the CURVE-only canonical names (renamed
// from `pull_from`/`push_to` in C4).

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

// HEP-CORE-0036 §6.7 Standby state machine (#188).  Pins the
// Standby → Configured → Active transitions on the PULL side
// (consumer):
//   - `pull_from(empty_endpoint, empty_serverkey, ...)` succeeds
//     and produces a Standby queue (is_configured=false,
//     is_running=false).
//   - `start()` refuses on Standby.
//   - `set_producer_peers(single-entry)` populates serverkey +
//     endpoint and transitions to Configured.
//   - `start()` succeeds in Configured; queue becomes Active.
//   - `stop()` is terminal (Active → not-running).
// Worker assertions inside the subprocess do the EXPECT_*; the test
// only checks the worker exited clean (no unexpected ERROR/WARN logs).
TEST_F(ZmqQueueAuthTest, Standby_StateTransitions_PullSide)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_standby_state_transitions",
        {unique_dir("auth_standby_state_transitions")});
    ExpectWorkerOk(w);
}

// PUSH-side parity: an empty bind endpoint puts the producer queue
// in Standby; start() refuses; set_producer_peers is inert.
TEST_F(ZmqQueueAuthTest, Standby_PushSide_StartRefuses_SetPeersInert)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_standby_push_side",
        {unique_dir("auth_standby_push_side")});
    ExpectWorkerOk(w);
}

// HEP-CORE-0036 §3.5.5 S3 / §6.7 Option B — `apply_master_approval`
// on the PUSH side MUST materialise REG_ACK's `initial_allowlist`
// onto the running queue.  Regression pin for the 2026-06-13 ordering
// bug where `start()` clobbered the freshly-set allowlist atomic with
// an empty default after `apply_master_approval` had seeded it.
// Mutation: revert hub_zmq_queue.cpp's `if (!allowlist_.load())` guard
// to the prior unconditional store → the worker's snap->peers.size()
// assertion fires (allowlist is empty post-start).
TEST_F(ZmqQueueAuthTest, ApplyMasterApproval_SeedsInitialAllowlist)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_apply_master_approval_seeds_initial_allowlist",
        {unique_dir("auth_apply_master_approval_seeds_initial_allowlist")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, ConnectEmptyServerkey_FactorySucceedsStandbyStartRefuses)
{
    // HEP-CORE-0036 §6.7 Standby state (#188) — RENAMED from
    // Misconfig_ConnectMissingServerkey_FactoryReturnsNullptr.  Empty
    // serverkey is no longer a factory misconfig; it's the explicit
    // Standby signal.  Factory MUST succeed.  start() MUST refuse on
    // Standby with the §6.7 diagnostic at DEBUG level (test fixture
    // does not pin DEBUG substrings — ERROR/WARN only).  Worker
    // assertions inside the subprocess verify is_configured()/
    // is_running()/start() return values.
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_misconfig_connect_missing_serverkey_factory_returns_nullptr",
        {unique_dir("auth_misconfig_connect_missing_serverkey_factory_returns_nullptr")});
    ExpectWorkerOk(w);
}

TEST_F(ZmqQueueAuthTest, Misconfig_FactoryReturnsNullptr)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_misconfig_factory_returns_nullptr",
        {unique_dir("auth_misconfig_factory_returns_nullptr")});
    // Misconfig branches the driver pins:
    //   - name-not-in-KeyStore (HEP-CORE-0040 §172)
    //   - empty keystore_name (C1 / #157 strict enforcement)
    //
    // HEP-CORE-0036 §6.7 Standby state (#188): the
    // "serverkey missing on connect side" case is NO LONGER a
    // misconfig — empty serverkey signals Standby.  Pinned by the
    // dedicated
    // `ConnectEmptyServerkey_FactorySucceedsStandbyStartRefuses`
    // worker.
    //
    // HEP-CORE-0040 §8.4 (#158) moved wrong-length pubkey validation
    // OUT of the factory — `Z85PublicKey` enforces the 40-char Z85
    // invariant at construction time.  That branch is covered by
    // `test_layer2_curve_keypair.cpp` and no longer reaches the
    // factory diagnostic path here.  Each substring below is unique
    // to one misconfig case so the framework would flag any new
    // unexpected ERROR or any silenced one.
    // The "keystore_name MUST be non-empty" entry appears twice
    // because two cases in the worker table exercise it — once on
    // the bind side and once on the connect side; each fires its
    // own LOGGER_ERROR line, and ExpectWorkerOk consumes one
    // substring per matched line.
    ExpectWorkerOk(w, {}, {
        "not present in KeyStore",
        "keystore_name MUST be non-empty",
        "keystore_name MUST be non-empty",
    });
}

// AUTH_TODO §C5 (#161) — anti-recursion test for the HEP-CORE-0035 §2
// invariant.  A raw NULL-mech client trying to connect to a
// CURVE-enforced ZmqQueue producer MUST fail the ZMTP handshake.  The
// worker monitors `ZMQ_EVENT_HANDSHAKE_FAILED_*` on the client socket
// and asserts at least one fires within a generous timeout.
//
// Why this needs a real socket-monitor test even with the L2
// `Mechanism::Curve` invariant + start() guard already pinned: the
// guard proves the PRODUCER side is CURVE-armed, but doesn't prove
// that a misconfigured client cannot still get through.  This pins
// the bidirectional contract — server CURVE-enforced + client NULL ⇒
// no data session.
TEST_F(ZmqQueueAuthTest, NullMechClient_HandshakeFails)
{
    auto w = SpawnWorker(
        "zmq_queue_auth.auth_null_mech_client_handshake_fails",
        {unique_dir("auth_null_mech_client_handshake_fails")});
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

// HEP-CORE-0040 §8.4 (#158) replaced `ZmqAuthOptions` with discrete
// factory parameters (`identity_key_name`, `Z85PublicKey
// server_pubkey`, `zap_domain`).  The struct-default mutation pin
// that used to live here is now redundant: the canonical default
// values are pinned at the type level (see
// `tests/test_layer2_service/test_curve_keypair.cpp` for
// `Z85PublicKey` sentinel semantics and at the factory signature
// (`hub_zmq_queue.hpp`) where defaults are `kRoleIdentityName` and
// `""`).  Any mutation that changes those defaults breaks compilation
// or surfaces in the curve-factory smoke tests in
// `test_hub_zmq_queue.cpp`.
