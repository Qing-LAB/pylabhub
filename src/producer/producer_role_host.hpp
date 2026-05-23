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
 * - Layer 3: Infrastructure (InboxQueue, producer-side queue via
 *            RoleAPIBase, ctrl threads via api.start_handler_threads).
 *            Post-M5: broker connectivity is owned by the RoleHandler
 *            inside RoleAPIBase (one BRC per HubConnection); the
 *            role host no longer holds a `broker_comm_` member.
 * - Layer 2: Data loop (inner retry acquire, deadline wait, invoke, commit).
 *
 * The script engine (Layer 1) is injected via RoleHostBase.
 *
 * See docs/tech_draft/loop_design_unified.md for the full design.
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

namespace pylabhub::producer
{

class PYLABHUB_UTILS_EXPORT ProducerRoleHost final : public scripting::RoleHostFrame
{
  public:
    /// Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
    /// the host takes config + shutdown flag.  The script engine is
    /// constructed on the host's worker thread inside `worker_main_`
    /// Step 0 (via `scripting::create_engine(config_.script())`),
    /// NOT pre-constructed by main().  This guarantees the
    /// PythonInterpreter dynamic lifecycle module — and thus
    /// `py::scoped_interpreter` — initialises on the worker, making
    /// the worker the GIL holder.
    explicit ProducerRoleHost(config::RoleConfig config,
                              std::atomic<bool> *shutdown_flag = nullptr);
    ~ProducerRoleHost() override;

    // Copy/move deleted by RoleHostBase.

    /// Pure config→opts translation used by `setup_infrastructure_`.
    /// Extracted so an L2/L3 test can exercise the production deployment
    /// path (file → `RoleConfig::load_from_directory` → this) without
    /// requiring a live broker or queue.  Closes the systemic test
    /// gap that produced bugs B5 (shm_name not copied, 2026-05-20) and
    /// B11 (zmq fields not copied, 2026-05-21) — both lived in the
    /// translation layer that prior L3 tests bypassed by hand-
    /// constructing the opts struct directly.
    ///
    /// Inputs are everything the translation actually consumes; output
    /// is a fully-populated `TxQueueOptions`.  Pure — no side effects,
    /// no broker, no queue construction.  Safe to call from tests.
    [[nodiscard]] static hub::TxQueueOptions
    make_tx_opts(const config::RoleConfig &config,
                 const hub::SchemaSpec    &out_slot_spec,
                 const hub::SchemaSpec    &out_fz_spec,
                 bool                      has_tx_fz);

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
    // Wave-B M5: `broker_comm_` deleted — broker connectivity is owned
    // by the RoleHandler inside RoleAPIBase (created lazily in worker_main_
    // Step 6 and consumed by api.start_handler_threads).  The role host
    // no longer holds a `unique_ptr<BrokerRequestComm>`.
    std::unique_ptr<hub::InboxQueue>        inbox_queue_;
    config::InboxConfig                     inbox_cfg_;  ///< Resolved copy.

    // Schema info (resolved from config during setup).
    hub::SchemaSpec                         out_slot_spec_;

    // Lifecycle module name (for UnloadModule on shutdown).
    std::string                             engine_module_name_;
};

} // namespace pylabhub::producer
