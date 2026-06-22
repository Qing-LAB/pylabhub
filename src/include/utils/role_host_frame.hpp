#pragma once
/**
 * @file role_host_frame.hpp
 * @brief RoleHostFrame — plain base class sitting between RoleHostBase
 *        and the three concrete role hosts (Producer/Consumer/Processor).
 *
 * Per design doc `docs/archive/transient-2026-06-02/role_host_template_design.md` §11.1,
 * this is a **plain (non-templated) class**.  See that section for the
 * structural reasoning grounded in concrete code facts — short version:
 * `EngineHost<ApiT>` one layer up must be a template because its
 * `config_` member's TYPE varies by host kind (RoleConfig vs
 * std::monostate, different memory layouts); RoleHostFrame has no
 * such variant member (CycleOps lives as a stack local inside
 * worker_main_, never as a member), so virtual hooks suffice.
 *
 */

#include "pylabhub_utils_export.h"
#include "utils/config/inbox_config.hpp"
#include "utils/engine_host.hpp"
#include "utils/role_presence.hpp"   // scripting::Presence + RoleKind
#include "utils/schema_types.hpp"    // hub::SchemaSpec (inbox spec parameter)

#include <memory>
#include <string>
#include <vector>

namespace pylabhub::hub
{
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::utils::security
{
class IShmCapabilityProducer;
class AttachProtocolAcceptor;
class ShmAttachOrchestrator;
} // namespace pylabhub::utils::security

namespace pylabhub::scripting
{

/// Per-role runtime configuration carried by RoleHostFrame.
///
/// All three current roles need the same three strings (role tag, label,
/// required script callback name).  Future role kinds (tap/monitor,
/// router, sink) inherit the same shape; if they need additional
/// runtime configuration, extend this struct rather than adding new
/// template parameters.
struct RoleHostFrameConfig
{
    /// Short role tag.  Forwarded to RoleHostBase as its role_tag; used
    /// in log prefixes and as the broker uid prefix component
    /// ("prod" / "cons" / "proc" today).
    std::string role_tag;

    /// Human-readable role label for diagnostics ("producer" /
    /// "consumer" / "processor").
    std::string role_label;

    /// Required script callback name ("on_produce" / "on_consume" /
    /// "on_process").  Engine asserts this exists during init.
    std::string required_callback;
};

/// Base class for all role hosts.  Plain (non-templated) class —
/// virtual hooks vary behavior per role; no template-instantiated
/// member-type variance is needed (see §11.1 of the design doc).
///
/// Still abstract: derived classes (ProducerRoleHost, ConsumerRoleHost,
/// ProcessorRoleHost) must implement RoleHostBase::worker_main_().
/// In a future sub-step (2d) the frame will provide a final
/// worker_main_ and add pure-virtual build_presences_ / run_loop_
/// hooks; for now each role host continues to override worker_main_
/// directly.
class PYLABHUB_UTILS_EXPORT RoleHostFrame : public RoleHostBase
{
  public:
    RoleHostFrame(config::RoleConfig    config,
                  std::atomic<bool>    *shutdown_flag,
                  RoleHostFrameConfig   frame_cfg);

    ~RoleHostFrame() override;

    // Copy/move deleted by RoleHostBase.

  protected:
    /// Read-only accessor for the frame configuration.  Derived classes
    /// (and, after sub-step 2c+, the frame's own setup/teardown bodies)
    /// use this to read role_tag/label/required_callback.
    [[nodiscard]] const RoleHostFrameConfig &frame_cfg() const noexcept
    {
        return frame_cfg_;
    }

    /// Shared teardown body, moved from the three per-role
    /// implementations during M9 sub-step 2b.  Pre-M9 each role host
    /// had its own `teardown_infrastructure_` method with byte-
    /// equivalent bodies (modulo comments).  Now lives here once.
    ///
    /// Sequence:
    ///   1. `core().clear_inbox_cache()`
    ///   2. if `inbox_queue_` exists: stop + reset.
    ///   3. if `has_api()`: `api().stop_handler_threads()`.
    ///   4. if `has_api()`: `api().close_queues()`.
    ///
    /// Called from each role's `worker_main_` at the teardown phase
    /// (and from the lambda passed to `do_role_teardown`).  Protected
    /// (not private) so derived classes can call it from inside
    /// `worker_main_`.
    void teardown_infrastructure_();

    /// Inbox queue + config — owned by the frame so the shared
    /// setup/teardown bodies can populate/release them.  Pre-M9
    /// these lived as private members on each role host.  Made
    /// protected so derived classes' `worker_main_` continues to
    /// access them by name (e.g. `api.set_inbox_queue(inbox_queue_.get())`).
    std::unique_ptr<hub::InboxQueue> inbox_queue_;

    /// Resolved (post-load) inbox config.  Copied from
    /// `config().inbox()` during `setup_infrastructure_`.
    config::InboxConfig              inbox_cfg_;

    /// Canonical per-channel record list.  Populated by `worker_main_`
    /// calling `build_presences_()` once at startup.  Each Presence
    /// carries hub + channel + role_kind + slot_spec + fz_spec — the
    /// **canonical** per-channel state.
    ///
    /// During the transitional shadow, RoleHostCore also stores
    /// per-direction `*_slot_spec_` and `*_fz_spec_` (populated by
    /// `worker_main_` from these presences); downstream consumers
    /// (RoleAPIBase introspection, engine_module_params, test fixtures)
    /// are being migrated to read from presences_ directly.  Tracked
    /// as task #99.
    std::vector<scripting::Presence> presences_;

    /// Per-role presence-list extractor.  Each role implements once.
    /// MUST resolve slot + fz schemas inline using
    /// `hub::resolve_schema()` (the presence's `hub.hub_dir / "schemas"`
    /// is the search path).  Exceptions propagate to the caller —
    /// `worker_main_` wraps the call in try/catch and sets the ready
    /// promise to false on failure.
    [[nodiscard]] virtual std::vector<scripting::Presence>
    build_presences_(const config::RoleConfig &config) const = 0;

    /// Wire the role-API's channel name(s) from the presence list.
    /// Default implementation handles the three current shapes:
    ///   - 1 Producer presence       → api.set_channel(presence.channel)
    ///   - 1 Consumer presence       → api.set_channel(presence.channel)
    ///   - 1 Consumer + 1 Producer   → api.set_channel(consumer.channel)
    ///                                 + api.set_out_channel(producer.channel)
    /// Override only when standard presence-list-driven wiring doesn't
    /// fit (e.g. a future N-input router with 3+ presences of mixed
    /// kinds).  Per design doc §11.8.2.
    ///
    /// Pre-condition: `presences_` populated (caller in worker_main_
    /// invokes after `build_presences_()`).
    /// Post-condition: `api_->channel()` / `api_->out_channel()` set
    /// per the shape above; ready for `setup_infrastructure_`.
    virtual void wire_api_for_presences_(
        const std::vector<scripting::Presence> &presences);

    /// Shared setup body.  Reads from `presences_` (which must be
    /// populated before this call — `worker_main_` calls
    /// `build_presences_()` early).  Per design doc §11.6.2.
    [[nodiscard]] bool setup_infrastructure_(const hub::SchemaSpec &inbox_spec);

    /// HEP-CORE-0041 1i-mig-2 hook: derived role host post-processes
    /// `tx_opts` after `make_tx_opts` but BEFORE `build_tx_queue`.
    /// Default: no-op returning true.  Producer / processor override
    /// to create the per-channel `IShmCapabilityProducer` (L1) for
    /// SHM TX channels, bind the capability endpoint, and populate
    /// `opts.shm_capability_fd` from the transport's borrowed fd.
    ///
    /// Called only when a TX presence exists.  Return false to abort
    /// startup (logged + propagated by `setup_infrastructure_`).
    /// `tx_channel` is `config().out_channel()` (or fallback).
    ///
    /// **Ownership.**  The derived role host owns the L1 transport
    /// for the channel lifetime.  This hook stores the transport in
    /// a member; `cleanup_tx_capability_` releases it on teardown.
    virtual bool prepare_tx_capability_(hub::TxQueueOptions      &tx_opts,
                                          const std::string        &tx_channel)
    {
        (void)tx_opts;
        (void)tx_channel;
        return true;
    }

    /// HEP-CORE-0041 1i-mig-2 cleanup hook: releases shm_orchestrator_
    /// → shm_acceptor_ → shm_transport_ in LIFO destruction order
    /// AFTER `teardown_infrastructure_` has reset `tx_queue` (the
    /// ShmQueue held a borrowed fd from the L1 transport).
    ///
    /// Default impl handles all three pointers correctly — derived
    /// role hosts should NOT override unless they have side-effects
    /// to add (no current role host does).
    virtual void cleanup_tx_capability_();

    /// HEP-CORE-0041 1i-mig-2c (M3 extraction): build the L2b acceptor
    /// + L2c orchestrator on top of `shm_transport_` and spawn the
    /// accept thread on `api().thread_manager()`.  Shared across every
    /// role host with a SHM TX presence (Producer today, Processor
    /// after 1i-mig-3).
    ///
    /// Pre-conditions:
    ///   - `shm_transport_` is non-null (caller checked
    ///     `prepare_tx_capability_` succeeded for SHM).
    ///   - `apply_producer_reg_ack` has been called and seeded the
    ///     allowlist cache (the orchestrator's CacheLookup callback
    ///     reads it).
    ///
    /// Effect on success:
    ///   - `shm_acceptor_` + `shm_orchestrator_` populated.
    ///   - Accept thread spawned on the role host's ThreadManager
    ///     (peer slot, named "shm_accept_loop", HEP-CORE-0031 §2
    ///     categorization).
    ///   - LOGGER_INFO "event=ShmAcceptLoopSpawned" marker emitted.
    ///
    /// Returns false on any failure (ctor throw, spawn refusal);
    /// caller sets the worker promise to false + returns.  Errors
    /// are logged at ERROR level with the role's `frame_cfg_.role_tag`
    /// prefix.
    [[nodiscard]] bool spawn_shm_auth_listener_();

    /// HEP-CORE-0041 1i-mig-2 members: per-channel SHM auth stack.
    ///
    /// Declaration order = construction order = REVERSE destruction:
    ///   - `shm_transport_` (L1)    — constructed first by
    ///     `prepare_tx_capability_`; destroyed LAST.
    ///   - `shm_acceptor_` (L2b)    — constructed by
    ///     `spawn_shm_auth_listener_` AFTER apply_reg_ack;
    ///     references the transport.
    ///   - `shm_orchestrator_` (L2c)— constructed last; references
    ///     both acceptor and transport.
    /// Destruction order: orchestrator → acceptor → transport (LIFO).
    ///
    /// The accept thread spawned on `api().thread_manager()` is
    /// joined by the TM Shutdown Contract (HEP-CORE-0031 §4.1) via
    /// `api().stop_handler_threads()` inside
    /// `teardown_infrastructure_`, BEFORE `cleanup_tx_capability_`
    /// resets these members.  So no thread is still inside the
    /// orchestrator when the reset fires.
    ///
    /// All three are nullptr for ZMQ TX channels and for role hosts
    /// without a TX presence (Consumer).
    std::unique_ptr<utils::security::IShmCapabilityProducer> shm_transport_;
    std::unique_ptr<utils::security::AttachProtocolAcceptor> shm_acceptor_;
    std::unique_ptr<utils::security::ShmAttachOrchestrator>  shm_orchestrator_;

  private:
    RoleHostFrameConfig frame_cfg_;
};

} // namespace pylabhub::scripting
