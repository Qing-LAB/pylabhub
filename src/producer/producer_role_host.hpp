#pragma once
/**
 * @file producer_role_host.hpp
 * @brief Unified producer role host — engine-agnostic.
 *
 * Post-M9 (2026-05-22): ProducerRoleHost inherits from
 * `scripting::RoleHostFrame` (NEW plain-class layer slotted between
 * `RoleHostBase` and the three role hosts).  `RoleHostFrame` carries the
 * shared role-host body + the per-role configuration struct
 * (role_tag/role_label/required_callback).  `RoleHostBase`
 * (`= EngineHost<RoleAPIBase>`) still owns the shared state (config,
 * engine, RoleHostCore, RoleAPIBase, ready-promise) and the public
 * lifecycle surface (startup_(), shutdown_(), is_running(),
 * wait_for_wakeup(), script_load_ok()).  See
 * `docs/archive/transient-2026-06-02/role_host_template_design.md` §10.6 for the full
 * inheritance map + architecture diagrams.
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

    // ── Infrastructure setup (Layer 3) — inherited from RoleHostFrame ─────
    // setup_infrastructure_ / teardown_infrastructure_ live on the frame
    // (shared across all role hosts).

    /// Build the role's presence list.  Producer returns a single
    /// Producer-kind presence on out_hub/out_channel with both
    /// slot_spec and fz_spec resolved inline.
    [[nodiscard]] std::vector<scripting::Presence>
    build_presences_(const config::RoleConfig &config) const override;

    // HEP-CORE-0041 1i-mig: prepare_tx_capability_ + cleanup_tx_capability_
    // both inherited from RoleHostFrame defaults — prepare_ was identical
    // in producer + processor; promoted to the frame in 1i-mig-M3.5
    // (commit set tracked under #266).

    // ── Producer-specific members ────────────────────────────────────────────
    // Shared state — core_, config_, engine_, api_, ready_promise_ — lives in
    // RoleHostBase.  Inbox state (`inbox_queue_`, `inbox_cfg_`) lives in
    // RoleHostFrame.  SHM auth listener state (`shm_transport_`,
    // `shm_acceptor_`, `shm_orchestrator_`) also lives in RoleHostFrame
    // post-1i-mig-2c M3.

    // Local cache of the resolved slot SchemaSpec.  Read by wire-emission
    // code (REG payload composition) later in `worker_main_`; also fed into
    // `core_.set_out_slot_spec()` and `params.out_slot_spec`.  Canonical
    // home is `presences_[0].slot_spec` (see RoleHostFrame); kept here as
    // the member that `worker_main_` initializes.
    hub::SchemaSpec                         out_slot_spec_;

    // Lifecycle module name (for UnloadModule on shutdown).
    std::string                             engine_module_name_;
};

} // namespace pylabhub::producer
