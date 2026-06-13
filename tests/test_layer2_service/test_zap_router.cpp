/**
 * @file test_zap_router.cpp
 * @brief Pattern 3 driver — ZapRouter L2 test suite (PeerAdmission Phase C).
 *
 * Each TEST_F spawns a fresh subprocess that owns its own LifecycleGuard
 * (Logger + FileLock + JsonConfig + ZMQContext per the ZapRouter
 * dynamic-module dependency chain).  Worker bodies in
 * `workers/zap_router_workers.cpp`.
 *
 * Subprocess isolation is required because ZapRouter is a persistent
 * dynamic module: once loaded in a process, it stays loaded until
 * LifecycleGuard finalize.  Running multiple tests serially in one
 * process would have each observe the previous one's bound inproc
 * REP socket and routing-map state.
 */
#include "test_patterns.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;

namespace
{

class ZapRouterTest : public IsolatedProcessTest
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
                     ("plh_l2_zap_" + std::string(test_name) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

TEST_F(ZapRouterTest, HandshakeAcceptDenyCycle)
{
    auto w = SpawnWorker("zap_router.handshake_accept_deny_cycle",
                         {unique_dir("handshake_accept_deny_cycle")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, UnknownDomain_Denies)
{
    auto w = SpawnWorker("zap_router.unknown_domain_denies",
                         {unique_dir("unknown_domain_denies")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, Handle_UnregistersOnDestruction)
{
    auto w = SpawnWorker("zap_router.handle_unregisters_on_destruction",
                         {unique_dir("handle_unregisters_on_destruction")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, DuplicateRegistration_Throws)
{
    auto w = SpawnWorker("zap_router.duplicate_registration_throws",
                         {unique_dir("duplicate_registration_throws")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, EmptyDomain_Throws)
{
    auto w = SpawnWorker("zap_router.empty_domain_throws",
                         {unique_dir("empty_domain_throws")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, NullAdmission_Throws)
{
    auto w = SpawnWorker("zap_router.null_admission_throws",
                         {unique_dir("null_admission_throws")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, PumpOne_WhenUnloaded_ReturnsFalse)
{
    auto w = SpawnWorker("zap_router.pump_one_when_unloaded_returns_false",
                         {unique_dir("pump_one_when_unloaded_returns_false")});
    ExpectWorkerOk(w);
}

// ── Direct frame-level / counter-pinning tests (close-out commit 1) ────────
// These tests pin the wire-protocol behaviour of pump_one against
// crafted REQ frames (bypassing libzmq's CURVE handshake), and pin
// that the security counters actually advance on the deny/allow paths.
// Without these, "blocked peer" tests can only assert absence-of-
// delivery — which a network stall, crashed pump thread, or wedged
// REP socket would mimic identically.

TEST_F(ZapRouterTest, PumpOne_MalformedShortRequest_RepliesAndRecovers)
{
    auto w = SpawnWorker(
        "zap_router.pump_one_malformed_short_request_replies_and_recovers",
        {unique_dir("pump_one_malformed_short_request_replies_and_recovers")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, PumpOne_BadVersion_Replies400)
{
    auto w = SpawnWorker(
        "zap_router.pump_one_bad_version_replies_400",
        {unique_dir("pump_one_bad_version_replies_400")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, PumpOne_NonCurveMechanism_Replies400)
{
    auto w = SpawnWorker(
        "zap_router.pump_one_non_curve_mechanism_replies_400",
        {unique_dir("pump_one_non_curve_mechanism_replies_400")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, Handshake_Deny_IncrementsDeniedCounter)
{
    auto w = SpawnWorker(
        "zap_router.handshake_deny_increments_denied_counter",
        {unique_dir("handshake_deny_increments_denied_counter")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, Handshake_Allow_IncrementsAllowedCounter)
{
    auto w = SpawnWorker(
        "zap_router.handshake_allow_increments_allowed_counter",
        {unique_dir("handshake_allow_increments_allowed_counter")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, Handle_MoveConstruct_TransfersOwnership)
{
    auto w = SpawnWorker(
        "zap_router.handle_move_construct_transfers_ownership",
        {unique_dir("handle_move_construct_transfers_ownership")});
    ExpectWorkerOk(w);
}

TEST_F(ZapRouterTest, Handle_MoveAssign_ReleasesPreviousRegistration)
{
    auto w = SpawnWorker(
        "zap_router.handle_move_assign_releases_previous_registration",
        {unique_dir("handle_move_assign_releases_previous_registration")});
    ExpectWorkerOk(w);
}

// ── Round-3 Slice A — task #215 ZapRouter UAF + reentrance fix ──────────────

// Pins ZapRouter::pump_one's shared_lock extension across the
// admission call.  Without the lock-scope extension, a parallel
// ~ZapDomainHandle returns immediately while the admission pointer is
// still in use — a UAF the moment AUTH-2 wires pump_one onto the BRC
// poll thread.
TEST_F(ZapRouterTest, Round3_UAF_DestructorBlocksUntilAdmissionReturns)
{
    auto w = SpawnWorker(
        "zap_router.round3_uaf_destructor_blocks_until_admission_returns",
        {unique_dir("round3_uaf_destructor_blocks_until_admission_returns")});
    ExpectWorkerOk(w);
}

// Pins the RecursionGuard refuse-and-log path in register_domain.
// An admission that calls back into register_domain from inside
// is_peer_allowed must get an inactive handle; the router state
// stays single-domain.
TEST_F(ZapRouterTest, Round3_Reentrant_RegisterRefused)
{
    auto w = SpawnWorker(
        "zap_router.round3_reentrant_register_refused",
        {unique_dir("round3_reentrant_register_refused")});
    // The refused register emits an ERROR log — pin the substring so a
    // regression that silently allowed re-entrance would fail
    // ExpectWorkerOk's unexpected-ERROR guard.
    ExpectWorkerOk(w, /*required_substrings=*/{},
                   /*expected_error_substrings=*/{"reentrant"});
}

// Pins the PLH_PANIC path in unregister_domain_.  Death-test shape:
// the worker subprocess must exit non-zero with the panic substring
// in stderr.
TEST_F(ZapRouterTest, Round3_Reentrant_UnregisterPanics)
{
    auto w = SpawnWorker(
        "zap_router.round3_reentrant_unregister_panics",
        {unique_dir("round3_reentrant_unregister_panics")});
    w.wait_for_exit();
    EXPECT_NE(w.exit_code(), 0)
        << "Worker did not abort — reentrant unregister_domain_ "
           "must PLH_PANIC because the router cannot recover from "
           "an erase mid-admission (dangling map entry → UAF).";
    EXPECT_THAT(w.get_stderr(),
                ::testing::HasSubstr("ZapRouter::unregister_domain_"))
        << "Expected PLH_PANIC text from unregister_domain_ in stderr.  "
           "Captured stderr:\n" << w.get_stderr();
}

// Pins the atomic counter PANIC in pump_one for concurrent pumpers.
// Two ZapPumpThread instances race into pump_one; whichever loses
// the counter race observes count > 1 and PANICs.
TEST_F(ZapRouterTest, Round3_ConcurrentPumpers_Panic)
{
    auto w = SpawnWorker(
        "zap_router.round3_concurrent_pumpers_panic",
        {unique_dir("round3_concurrent_pumpers_panic")});
    w.wait_for_exit();
    EXPECT_NE(w.exit_code(), 0)
        << "Worker did not abort — two concurrent pumpers must "
           "PLH_PANIC.  The libzmq REP socket FSM is single-threaded; "
           "silent racing two pumpers corrupts the FSM and AUTH-2's "
           "BRC pump would observe ETERM/EAGAIN unpredictably.";
    EXPECT_THAT(w.get_stderr(),
                ::testing::HasSubstr("ZapRouter::pump_one"))
        << "Expected PLH_PANIC text from pump_one in stderr.  "
           "Captured stderr:\n" << w.get_stderr();
}
