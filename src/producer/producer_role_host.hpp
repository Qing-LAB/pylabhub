#pragma once
/**
 * @file producer_role_host.hpp
 * @brief Unified producer role host — engine-agnostic.
 *
 * ProducerRoleHost inherits from RoleHostBase, which owns the shared
 * state (role_tag, config, engine, RoleHostCore, RoleAPIBase,
 * ready-promise) and the public lifecycle surface (startup_(), shutdown_(),
 * is_running(), wait_for_wakeup(), script_load_ok()).
 *
 * This class owns only producer-specific state:
 * - Layer 3: Infrastructure (BrokerRequestComm, InboxQueue, producer-side
 *            queue via RoleAPIBase, ctrl_thread_ via start_ctrl_thread).
 * - Layer 2: Data loop (inner retry acquire, deadline wait, invoke, commit).
 *
 * The script engine (Layer 1) is injected via RoleHostBase.
 *
 * See docs/tech_draft/loop_design_unified.md for the full design.
 */

#include "pylabhub_utils_export.h"
#include "utils/config/role_config.hpp"
#include "utils/role_host_base.hpp"
#include "plh_datahub.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace pylabhub::hub
{
class InboxQueue;
class BrokerRequestComm;
} // namespace pylabhub::hub

namespace pylabhub::producer
{

class PYLABHUB_UTILS_EXPORT ProducerRoleHost final : public scripting::RoleHostBase
{
  public:
    explicit ProducerRoleHost(config::RoleConfig config,
                              std::unique_ptr<scripting::ScriptEngine> engine,
                              std::atomic<bool> *shutdown_flag = nullptr);
    ~ProducerRoleHost() override;

    // Copy/move deleted by RoleHostBase.

  private:
    // ── Worker thread entry point (RoleHostBase hook) ────────────────────────
    void worker_main_() override;

    // ── Infrastructure setup/teardown (Layer 3) ──────────────────────────────
    bool setup_infrastructure_(const hub::SchemaSpec &inbox_spec);
    void teardown_infrastructure_();

    // ── Producer-specific members ────────────────────────────────────────────
    // (Shared state — core_, config_, engine_, api_, ready_promise_ — lives
    //  in RoleHostBase and is reached via protected accessors.)

    // Infrastructure (created on worker thread in setup_infrastructure_).
    std::unique_ptr<hub::BrokerRequestComm> broker_comm_;
    std::unique_ptr<hub::InboxQueue>        inbox_queue_;
    config::InboxConfig                     inbox_cfg_;  ///< Resolved copy.

    // Schema info (resolved from config during setup).
    hub::SchemaSpec                         out_slot_spec_;

    // Lifecycle module name (for UnloadModule on shutdown).
    std::string                             engine_module_name_;
};

} // namespace pylabhub::producer
