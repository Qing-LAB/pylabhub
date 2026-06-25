/**
 * @file test_role_host_frame_shm.cpp
 * @brief L2 coverage for RoleHostFrame SHM auth-stack hooks
 *        (HEP-CORE-0041 task #270).
 *
 * Pattern 3 (subprocess per test) — each scenario runs in its own
 * worker subprocess via `IsolatedProcessTest`.  The worker constructs
 * a `RoleHostFrameTestShim` and drives the production SHM hooks
 * directly:
 *
 *   - `prepare_tx_capability_` (default impl, 1i-mig-M3.5 #266) — for
 *     SHM TX, creates the per-channel `IShmCapabilityProducer`, binds
 *     the Unix-socket capability endpoint, populates
 *     `tx_opts.shm_capability_fd`.  For ZMQ TX, no-op.
 *
 *   - `spawn_shm_auth_listener_` (1i-mig-2c M3 extraction) — builds
 *     the L2b `AttachProtocolAcceptor` + L2c `ShmAttachOrchestrator`
 *     on top of `shm_transport_`, then spawns the accept thread on
 *     `api().thread_manager()`.
 *
 *   - `cleanup_tx_capability_` (1i-mig-2c M3 default impl) — LIFO
 *     release of orchestrator → acceptor → transport.
 *
 * What this file does NOT cover (out of scope per HEP-0041 §10.1
 * matrix):
 *   - The accept thread actually serving a CONSUMER_ATTACH_REQ —
 *     `test_shm_attach_orchestrator.cpp` already pins the L2c
 *     orchestrator's accept_and_serve_one contract in isolation;
 *     end-to-end multi-process coverage lives at L3 Pattern 4
 *     (task #285).
 *   - Cross-platform variants — Linux/FreeBSD share the memfd
 *     backend; macOS/Windows tracked under #259/#260/#261.
 *
 * Test-faithfulness invariant (pattern4_consumer_lifecycle_workers
 * §2): the shim adds NO behavior; every test-side method is a
 * one-line forward to a protected `RoleHostFrame` method.  No mock,
 * no test-only factory.
 */
#include "shared_test_helpers.h"
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

class RoleHostFrameShmTest : public IsolatedProcessTest
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

    std::string unique_dir(const char *prefix)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_rhf_shm_" + std::string(prefix) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

// ─── prepare_tx_capability_ behaviour ────────────────────────────────────────

/// ZMQ TX (`data_transport != "shm"` OR `has_shm == false`) → method
/// must early-return true and leave `shm_transport_` null.  The
/// production no-op path for non-SHM role hosts.
TEST_F(RoleHostFrameShmTest, PrepareTxCapability_Zmq_NoOp)
{
    auto dir = unique_dir("zmq_noop");
    auto w   = SpawnWorker("role_host_frame_shm.prepare_zmq_noop", {dir});
    ExpectWorkerOk(w);
}

/// SHM TX happy path — `prepare_tx_capability_` creates an
/// `IShmCapabilityProducer`, binds the channel's default capability
/// endpoint, and populates `tx_opts.shm_capability_fd > 0`.  Pins
/// the contract that `ShmQueue`'s fd-source factory depends on
/// (HEP-CORE-0041 §6.3).
TEST_F(RoleHostFrameShmTest, PrepareTxCapability_Shm_HappyPath)
{
    auto dir = unique_dir("shm_happy");
    auto w   = SpawnWorker("role_host_frame_shm.prepare_shm_happy", {dir});
    // Pin the production INFO marker `prepare_tx_capability_` emits
    // on success (role_host_frame.cpp:427-431).  This marker is a
    // public observation surface — Pattern 4 SHM rungs (task #285)
    // will grep it; locking the format here at the L2 layer keeps
    // upstream tests from breaking silently if someone reshapes it.
    ExpectWorkerOk(w,
        /*required_substrings=*/{"event=ShmCapabilityTransportBound"});
}

// ─── spawn_shm_auth_listener_ behaviour ──────────────────────────────────────

/// Pre-condition violation: calling `spawn_shm_auth_listener_`
/// without a prior `prepare_tx_capability_` (so `shm_transport_` is
/// null) MUST return false + leave acceptor + orchestrator null.
/// Production tag: HEP-CORE-0041 1i-mig-2c §M3 docstring
/// pre-condition.
TEST_F(RoleHostFrameShmTest, SpawnShmAuthListener_FailsWithoutTransport)
{
    auto dir = unique_dir("spawn_no_xport");
    auto w   = SpawnWorker(
        "role_host_frame_shm.spawn_without_transport", {dir});
    // The contract being pinned IS the ERROR log + false return when
    // shm_transport_ is null; declare the ERROR-level marker so the
    // framework's no-unexpected-error invariant doesn't trip on it.
    ExpectWorkerOk(w,
        /*required_substrings=*/{},
        /*expected_error_substrings=*/{
            "spawn_shm_auth_listener_: shm_transport_ is null"});
}

/// Full cycle: prepare → spawn → assert all three SHM auth-stack
/// pointers non-null → cleanup_tx_capability_ → assert all three
/// reset to null.  Pins both the spawn success contract and the
/// LIFO cleanup ordering (HEP-CORE-0041 1i-mig-2c §M3 default
/// impl).
TEST_F(RoleHostFrameShmTest, SpawnAndCleanup_FullCycle)
{
    auto dir = unique_dir("full_cycle");
    auto w   = SpawnWorker("role_host_frame_shm.spawn_and_cleanup", {dir});
    // Pin both production INFO markers in the order they fire:
    //   - `event=ShmCapabilityTransportBound` from prepare_tx_capability_
    //     (role_host_frame.cpp:427-431)
    //   - `event=ShmAcceptLoopSpawned` from spawn_shm_auth_listener_
    //     (role_host_frame.cpp:643-645)
    // `required_substrings` checks presence (not order) so this is a
    // robust co-occurrence pin.  Pattern 4 SHM rungs (task #285)
    // depend on the SAME markers — locking the format at L2 means
    // any format drift breaks fast + cheap here instead of slow +
    // expensive at L3.
    ExpectWorkerOk(w,
        /*required_substrings=*/{
            "event=ShmCapabilityTransportBound",
            "event=ShmAcceptLoopSpawned"});
}
