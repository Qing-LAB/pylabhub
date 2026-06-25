#pragma once
/**
 * @file role_host_frame_test_shim.h
 * @brief Test-side `RoleHostFrame` subclass that exposes the protected
 *        SHM auth hooks publicly so they can be driven directly from
 *        tests (HEP-CORE-0041 task #270 L2 coverage; reusable from
 *        Pattern 4 SHM rungs under task #285).
 *
 * Test-faithfulness note (see
 * `pattern4_consumer_lifecycle_workers.cpp:5-47` for the principle):
 *
 *   This shim adds NO behavior.  Every method is a one-line forward to
 *   the protected production method on `RoleHostFrame`.  The shim's
 *   purpose is solely to make the protected surface reachable from a
 *   test — exactly the layer-on-top-of-public-structure pattern.  No
 *   mock, no behavior tweak, no design change.
 *
 *   Subclassing `RoleHostFrame` and overriding `worker_main_` to a
 *   ready-then-wait body lets the test drive the protected SHM hooks
 *   from the parent thread while the production lifecycle (api_
 *   construction, ThreadManager + KeyStore access, dtor invariants)
 *   runs unchanged.
 *
 * Lifecycle invariants (inherited from `EngineHost` /
 * `RoleHostBase`):
 *
 *   - The shim's destructor calls `shutdown_()` (as do all
 *     `RoleHostBase` subclasses).  Calling `shutdown_()` without a
 *     prior `startup_()` is safe (used by the prepare-only tests).
 *   - Tests that exercise `spawn_shm_auth_listener_` MUST call
 *     `startup_()` (because the production body uses
 *     `api().thread_manager()`).  Such tests must also call
 *     `signal_test_complete()` BEFORE invoking `shutdown_()` so the
 *     overridden `worker_main_` exits its wait loop and the
 *     shutdown can join cleanly.
 */
#include "utils/role_host_frame.hpp"
#include "utils/role_presence.hpp"
#include "utils/schema_types.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace pylabhub::utils::security
{
class IShmCapabilityProducer;
class AttachProtocolAcceptor;
class ShmAttachOrchestrator;
} // namespace pylabhub::utils::security

namespace pylabhub::tests
{

/// Subclass of `scripting::RoleHostFrame` that:
///   - returns a test-supplied presences vector from `build_presences_`;
///   - runs a ready→running→wait-on-flag→stop body in `worker_main_`;
///   - publicly forwards `prepare_tx_capability_`,
///     `spawn_shm_auth_listener_`, and `cleanup_tx_capability_`;
///   - publicly exposes the three SHM auth-stack pointers via const
///     accessors.
///
/// Used by `test_role_host_frame_shm.cpp` (task #270) and reused by
/// the Pattern 4 SHM rungs (task #285).
class RoleHostFrameTestShim final : public scripting::RoleHostFrame
{
  public:
    /// Constructor primitives — Presence is move-only, so the shim
    /// can't store a vector<Presence> and clone it across multiple
    /// `build_presences_` calls.  Storing the primitive fields and
    /// reconstructing a fresh Presence inside `build_presences_`
    /// matches what every production role host does.
    struct PresenceSeed
    {
        std::string         channel;
        scripting::RoleKind role_kind;
        hub::SchemaSpec     slot_spec;
        hub::SchemaSpec     fz_spec;
    };

    RoleHostFrameTestShim(config::RoleConfig             config,
                          std::atomic<bool>             *shutdown_flag,
                          scripting::RoleHostFrameConfig frame_cfg,
                          PresenceSeed                   presence);

    ~RoleHostFrameTestShim() override;

    // ── Public wrappers around protected production methods ──
    //
    // Each is a one-line forward.  Test code drives the production
    // sequence by calling these directly; the contract being pinned is
    // exactly the production method, no test-side reimplementation.

    bool test_prepare_tx_capability(hub::TxQueueOptions &tx_opts,
                                    const std::string   &tx_channel);
    bool test_spawn_shm_auth_listener();
    void test_cleanup_tx_capability();

    // ── Public read-only accessors for protected members ──
    //
    // Const-pointer-to-const surface — tests can null-check + inspect
    // identity without mutating ownership.

    [[nodiscard]] const utils::security::IShmCapabilityProducer *
    test_shm_transport() const noexcept;

    [[nodiscard]] const utils::security::AttachProtocolAcceptor *
    test_shm_acceptor() const noexcept;

    [[nodiscard]] const utils::security::ShmAttachOrchestrator *
    test_shm_orchestrator() const noexcept;

    // ── Worker-loop coordination ──
    //
    // Tests that invoke `startup_()` MUST set the complete flag (via
    // `signal_test_complete`) BEFORE calling `shutdown_()` so the
    // overridden `worker_main_` exits its wait loop and the join in
    // shutdown completes.

    void signal_test_complete() noexcept;

    /// True once the worker thread has entered its wait loop after
    /// firing the ready promise.  Polled by tests to know when the
    /// production lifecycle is ready for `test_spawn_shm_auth_listener`.
    [[nodiscard]] bool worker_in_wait_loop() const noexcept
    {
        return worker_in_wait_loop_.load(std::memory_order_acquire);
    }

  protected:
    [[nodiscard]] std::vector<scripting::Presence>
    build_presences_(const config::RoleConfig &) const override;

    void worker_main_() override;

  private:
    PresenceSeed      presence_seed_;
    std::atomic<bool> worker_in_wait_loop_{false};
    std::atomic<bool> test_complete_{false};
};

} // namespace pylabhub::tests
