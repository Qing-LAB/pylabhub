/**
 * @file role_host_frame_test_shim.cpp
 * @brief Implementation of RoleHostFrameTestShim.  See header for the
 *        test-faithfulness rationale.
 */
#include "role_host_frame_test_shim.h"

#include "utils/role_host_core.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace pylabhub::tests
{

RoleHostFrameTestShim::RoleHostFrameTestShim(
    config::RoleConfig             config,
    std::atomic<bool>             *shutdown_flag,
    scripting::RoleHostFrameConfig frame_cfg,
    PresenceSeed                   presence)
    : RoleHostFrame(std::move(config), shutdown_flag, std::move(frame_cfg)),
      presence_seed_(std::move(presence))
{
}

RoleHostFrameTestShim::~RoleHostFrameTestShim()
{
    // EngineHost dtor PANICs if shutdown_() was not called.  Safe to
    // call whether or not startup_() ran (matches every other
    // RoleHostBase subclass; see test_role_host_base workers).
    shutdown_();
}

bool RoleHostFrameTestShim::test_prepare_tx_capability(
    hub::TxQueueOptions &tx_opts, const std::string &tx_channel)
{
    return prepare_tx_capability_(tx_opts, tx_channel);
}

bool RoleHostFrameTestShim::test_spawn_shm_auth_listener()
{
    return spawn_shm_auth_listener_();
}

void RoleHostFrameTestShim::test_cleanup_tx_capability()
{
    cleanup_tx_capability_();
}

const utils::security::IShmCapabilityProducer *
RoleHostFrameTestShim::test_shm_transport() const noexcept
{
    return shm_transport_.get();
}

const utils::security::AttachProtocolAcceptor *
RoleHostFrameTestShim::test_shm_acceptor() const noexcept
{
    return shm_acceptor_.get();
}

const utils::security::ShmAttachOrchestrator *
RoleHostFrameTestShim::test_shm_orchestrator() const noexcept
{
    return shm_orchestrator_.get();
}

void RoleHostFrameTestShim::signal_test_complete() noexcept
{
    test_complete_.store(true, std::memory_order_release);
}

std::vector<scripting::Presence>
RoleHostFrameTestShim::build_presences_(const config::RoleConfig &) const
{
    // Presence is move-only; reconstruct fresh from primitives each
    // call, matching production role hosts' build_presences_ shape.
    std::vector<scripting::Presence> v;
    v.emplace_back();
    auto &p     = v.back();
    p.channel   = presence_seed_.channel;
    p.role_kind = presence_seed_.role_kind;
    p.slot_spec = presence_seed_.slot_spec;
    p.fz_spec   = presence_seed_.fz_spec;
    return v;
}

void RoleHostFrameTestShim::worker_main_()
{
    // Standard ready→running→wait pattern.  Mirrors TestRoleHost in
    // tests/test_layer2_service/workers/role_host_base_workers.cpp so
    // RoleHostBase's startup_ → ready_promise → run-loop contract is
    // exercised exactly like production.
    core().set_script_load_ok(true);
    ready_promise().set_value(true);
    core().set_running(true);
    worker_in_wait_loop_.store(true, std::memory_order_release);

    // Wait for either an external shutdown request or the test's
    // explicit completion flag.  50 ms poll is the same cadence
    // TestRoleHost uses (matches `core().wait_for_incoming(50)`).
    while (core().should_continue_loop()
           && !test_complete_.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    core().set_running(false);

    // Mirror production `RoleHostFrame::teardown_infrastructure_`
    // ordering (role_host_frame.cpp:312-339):
    //   1. `api().stop_handler_threads()` drains ThreadManager —
    //      joins the `shm_accept_loop` thread spawned by
    //      `spawn_shm_auth_listener_`.
    //   2. `cleanup_tx_capability_()` LIFO-releases the SHM
    //      orchestrator → acceptor → transport bundle.
    // Without this drain the accept thread is still inside
    // `orchestrator_->accept_and_serve_one(...)` when cleanup runs
    // → orchestrator destructed while in use → UAF.  Production
    // enforces this ordering via `teardown_infrastructure_`; the
    // shim's worker_main_ does the same here so tests don't have
    // to re-implement it.
    if (has_api())
    {
        api().stop_handler_threads();
    }
    cleanup_tx_capability_();
}

} // namespace pylabhub::tests
