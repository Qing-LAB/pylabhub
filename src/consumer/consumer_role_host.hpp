#pragma once
/**
 * @file consumer_role_host.hpp
 * @brief Unified consumer role host — engine-agnostic.
 *
 * Post-M9: ConsumerRoleHost inherits from `scripting::RoleHostFrame`
 * (NEW plain-class layer; see role_host_frame.hpp for the design).
 * RoleHostFrame in turn inherits from RoleHostBase, which owns the shared
 * state (role_tag, config, engine, RoleHostCore, RoleAPIBase,
 * ready-promise) and the public lifecycle surface (startup_(), shutdown_(),
 * is_running(), wait_for_wakeup(), script_load_ok()).
 *
 * This class owns only consumer-specific state:
 * - Layer 3: Infrastructure (InboxQueue, Rx queue via RoleAPIBase,
 *            ctrl threads via api.start_handler_threads).  Post-M6
 *            (Wave-B): broker connectivity is owned by the RoleHandler
 *            inside RoleAPIBase (one BRC per HubConnection); the role
 *            host no longer holds a `broker_comm_` member.
 * - Layer 2: Data loop (inner retry acquire, read, invoke, release).
 *
 * The script engine (Layer 1) is injected via RoleHostBase.
 *
 * See docs/tech_draft/loop_design_unified.md §4 for the full design.
 */

#include "pylabhub_utils_export.h"
#include "utils/config/role_config.hpp"
#include "utils/engine_host.hpp"
#include "utils/role_host_frame.hpp"
#include "plh_datahub.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace pylabhub::hub
{
class InboxQueue;
} // namespace pylabhub::hub

namespace pylabhub::consumer
{

class PYLABHUB_UTILS_EXPORT ConsumerRoleHost final : public scripting::RoleHostFrame
{
  public:
    /// Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
    /// engine is NOT passed in — it is constructed inside `worker_main_`
    /// Step 0 via `scripting::create_engine(config_.script())`.
    explicit ConsumerRoleHost(config::RoleConfig config,
                              std::atomic<bool> *shutdown_flag = nullptr);
    ~ConsumerRoleHost() override;

    // Copy/move deleted by RoleHostBase.

    /// Pure config→opts translation used by `setup_infrastructure_`.
    /// See ProducerRoleHost::make_tx_opts for rationale.  Same audit
    /// B5/B11 history (translation layer that prior L3 tests bypassed).
    [[nodiscard]] static hub::RxQueueOptions
    make_rx_opts(const config::RoleConfig &config,
                 const hub::SchemaSpec    &in_slot_spec,
                 const hub::SchemaSpec    &in_fz_spec,
                 bool                      has_rx_fz);

  private:
    // ── Worker thread entry point (RoleHostBase hook) ────────────────────────
    void worker_main_() override;

    // ── Infrastructure setup (Layer 3) ───────────────────────────────────────
    // teardown_infrastructure_ — inherited from RoleHostFrame (M9 step 2b).
    bool setup_infrastructure_(const hub::SchemaSpec &inbox_spec);

    // ── Consumer-specific members ────────────────────────────────────────────
    // Shared state — core_, config_, engine_, api_, ready_promise_ — lives in
    // RoleHostBase.  Inbox state (`inbox_queue_`, `inbox_cfg_`) lives in
    // RoleHostFrame (M9 step 2b, 2026-05-22).

    // Schema info (resolved from config during setup).
    hub::SchemaSpec                         in_slot_spec_;

    // Lifecycle module name (for UnloadModule on shutdown).
    std::string                             engine_module_name_;

    // last_seq: read directly from Consumer::last_seq() → QueueReader
    // (single source of truth).
};

} // namespace pylabhub::consumer
