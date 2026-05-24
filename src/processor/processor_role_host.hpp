#pragma once
/**
 * @file processor_role_host.hpp
 * @brief Unified processor role host — engine-agnostic.
 *
 * Post-M9: ProcessorRoleHost inherits from `scripting::RoleHostFrame`
 * (NEW plain-class layer; see role_host_frame.hpp for the design).
 * RoleHostFrame in turn inherits from RoleHostBase, which owns the shared
 * state (role_tag, config, engine, RoleHostCore, RoleAPIBase,
 * ready-promise) and the public lifecycle surface (startup_(), shutdown_(),
 * is_running(), wait_for_wakeup(), script_load_ok()).
 *
 * This class owns only processor-specific state:
 * - Layer 3: Infrastructure (InboxQueue, dual Rx+Tx queues via
 *            RoleAPIBase, ctrl threads via api.start_handler_threads).
 *            Post-M7 (Wave-B): broker connectivity is owned by the
 *            RoleHandler inside RoleAPIBase.  The processor declares
 *            a 2-presence list (1 Consumer on in_hub + 1 Producer on
 *            out_hub); RoleHandler dedups by (broker_endpoint,
 *            broker_pubkey) → 1 connection when both presences share
 *            a hub (single-hub processor — the common case today),
 *            2 connections when the hubs differ (dual-hub processor,
 *            the M8 payoff).
 * - Layer 2: Data loop (dual-queue inner retry acquire, deadline wait,
 *            invoke, commit/release).
 *
 * The script engine (Layer 1) is injected via RoleHostBase.
 *
 * See docs/tech_draft/loop_design_unified.md §5 for the full design.
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

namespace pylabhub::processor
{

class PYLABHUB_UTILS_EXPORT ProcessorRoleHost final : public scripting::RoleHostFrame
{
  public:
    /// Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
    /// engine is NOT passed in — it is constructed inside `worker_main_`
    /// Step 0 via `scripting::create_engine(config_.script())`.
    explicit ProcessorRoleHost(config::RoleConfig config,
                               std::atomic<bool> *shutdown_flag = nullptr);
    ~ProcessorRoleHost() override;

    // Copy/move deleted by RoleHostBase.

    /// Pure config→opts translation for the processor's INPUT side.
    /// See ProducerRoleHost::make_tx_opts for rationale (audit B5+B11).
    /// Processor has BOTH directions; this is the consumer-half.
    [[nodiscard]] static hub::RxQueueOptions
    make_rx_opts(const config::RoleConfig &config,
                 const hub::SchemaSpec    &in_slot_spec,
                 const hub::SchemaSpec    &in_fz_spec,
                 bool                      has_rx_fz);

    /// Pure config→opts translation for the processor's OUTPUT side.
    [[nodiscard]] static hub::TxQueueOptions
    make_tx_opts(const config::RoleConfig &config,
                 const hub::SchemaSpec    &out_slot_spec,
                 const hub::SchemaSpec    &out_fz_spec,
                 bool                      has_tx_fz);

  private:
    // ── Worker thread entry point (RoleHostBase hook) ────────────────────────
    void worker_main_() override;

    // ── Infrastructure setup (Layer 3) — inherited from RoleHostFrame ─────
    // setup_infrastructure_  ← M9 step 2c (frame's body, uses presences_).
    // teardown_infrastructure_ ← M9 step 2b (frame's body).

    /// Build the role's presence list (M9 step 2c).  Processor returns
    /// two presences: Consumer-kind on in_hub/in_channel + Producer-kind
    /// on out_hub/out_channel.  Schemas resolved inline for each.
    [[nodiscard]] std::vector<scripting::Presence>
    build_presences_(const config::RoleConfig &config) const override;

    // ── Processor-specific members ───────────────────────────────────────────
    // Shared state — core_, config_, engine_, api_, ready_promise_ — lives in
    // RoleHostBase.  Inbox state (`inbox_queue_`, `inbox_cfg_`) lives in
    // RoleHostFrame (M9 step 2b, 2026-05-22).

    // Schema info (resolved from config during setup).
    hub::SchemaSpec                         in_slot_spec_;
    hub::SchemaSpec                         out_slot_spec_;

    // Lifecycle module name (for UnloadModule on shutdown).
    std::string                             engine_module_name_;
};

} // namespace pylabhub::processor
