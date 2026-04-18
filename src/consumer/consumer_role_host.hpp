#pragma once
/**
 * @file consumer_role_host.hpp
 * @brief Unified consumer role host — engine-agnostic.
 *
 * ConsumerRoleHost inherits from RoleHostBase, which owns the shared
 * state (role_tag, config, engine, RoleHostCore, RoleAPIBase,
 * ready-promise) and the public lifecycle surface (startup_(), shutdown_(),
 * is_running(), wait_for_wakeup(), script_load_ok()).
 *
 * This class owns only consumer-specific state:
 * - Layer 3: Infrastructure (BrokerRequestComm, InboxQueue, Rx queue via
 *            RoleAPIBase, ctrl_thread via start_ctrl_thread).
 * - Layer 2: Data loop (inner retry acquire, read, invoke, release).
 *
 * The script engine (Layer 1) is injected via RoleHostBase.
 *
 * See docs/tech_draft/loop_design_unified.md §4 for the full design.
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

namespace pylabhub::consumer
{

class PYLABHUB_UTILS_EXPORT ConsumerRoleHost final : public scripting::RoleHostBase
{
  public:
    explicit ConsumerRoleHost(config::RoleConfig config,
                              std::unique_ptr<scripting::ScriptEngine> engine,
                              std::atomic<bool> *shutdown_flag = nullptr);
    ~ConsumerRoleHost() override;

    // Copy/move deleted by RoleHostBase.

  private:
    // ── Worker thread entry point (RoleHostBase hook) ────────────────────────
    void worker_main_() override;

    // ── Infrastructure setup/teardown (Layer 3) ──────────────────────────────
    bool setup_infrastructure_(const hub::SchemaSpec &inbox_spec);
    void teardown_infrastructure_();

    // ── Consumer-specific members ────────────────────────────────────────────
    // (Shared state — core_, config_, engine_, api_, ready_promise_ — lives
    //  in RoleHostBase and is reached via protected accessors.)

    // Infrastructure (created on worker thread in setup_infrastructure_).
    std::unique_ptr<hub::BrokerRequestComm> broker_comm_;
    std::unique_ptr<hub::InboxQueue>        inbox_queue_;
    config::InboxConfig                     inbox_cfg_;

    // Schema info (resolved from config during setup).
    hub::SchemaSpec                         in_slot_spec_;

    // Lifecycle module name (for UnloadModule on shutdown).
    std::string                             engine_module_name_;

    // last_seq: read directly from Consumer::last_seq() → QueueReader
    // (single source of truth).
};

} // namespace pylabhub::consumer
