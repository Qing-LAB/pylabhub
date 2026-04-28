/**
 * @file consumer_role_host.cpp
 * @brief ConsumerRoleHost — unified engine-agnostic consumer implementation.
 *
 * This is the canonical data loop for the consumer role.  It follows
 * docs/tech_draft/loop_design_unified.md §4 exactly.
 *
 * Layer 3 (infrastructure): BrokerRequestComm, Consumer, queue, ctrl_thread_, events.
 * Layer 2 (data loop): inner retry acquire, read, invoke, release.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_consume / invoke_on_inbox.
 *
 * Shared lifecycle scaffolding (ctor, startup_(), shutdown_(), ready-promise,
 * RoleAPIBase ownership, ThreadManager-owned worker) lives in RoleHostBase.
 * This file implements only the consumer-specific worker_main_() hook and
 * its setup_infrastructure_() / teardown_infrastructure_() helpers.
 */
#include "consumer_role_host.hpp"
#include "utils/thread_manager.hpp"
#include "service/cycle_ops.hpp"
#include "service/data_loop.hpp"
#include "utils/broker_request_comm.hpp"
#include "consumer_fields.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "utils/engine_module_params.hpp"
#include "utils/role_host_helpers.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/schema_utils.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::consumer
{

using scripting::ConsumerCycleOps;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Constructor / Destructor
// ============================================================================

ConsumerRoleHost::ConsumerRoleHost(config::RoleConfig config,
                                     std::unique_ptr<scripting::ScriptEngine> engine,
                                     std::atomic<bool> *shutdown_flag)
    : scripting::RoleHostBase("cons", std::move(config), std::move(engine),
                              shutdown_flag)
{
}

ConsumerRoleHost::~ConsumerRoleHost()
{
    // Join worker BEFORE consumer-specific members (broker_comm_,
    // inbox_queue_) begin destruction — the worker's teardown path
    // reads them. shutdown_() is idempotent; base dtor's flag check
    // therefore passes when it runs later.
    shutdown_();
}

// ============================================================================
// worker_main_ — the worker thread entry point (RoleHostBase hook)
// ============================================================================
//
// Sequence identical to the pre-migration standalone implementation; only
// the state it reads (config_, core_, engine_, api_, ready_promise_) is
// now accessed via protected RoleHostBase accessors instead of direct
// member references.

void ConsumerRoleHost::worker_main_()
{
    auto       &core_        = core();
    const auto &config_      = config();
    auto       &engine_ref   = engine();
    auto       &api_ref      = api();
    auto       &promise_ref  = ready_promise();

    const auto &id   = config_.identity();
    const auto &sc   = config_.script();
    const auto &hub  = config_.in_hub();
    const auto &tr   = config_.in_transport();

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[cons] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    sc.type, sc.type);
    }

    // ── Step 1: Resolve schemas from config ──────────────────────────────────

    const std::filesystem::path base_path =
        sc.path.empty() ? std::filesystem::current_path()
                        : std::filesystem::weakly_canonical(sc.path);
    const std::filesystem::path script_dir = base_path / "script" / sc.type;

    hub::SchemaSpec in_fz_local;
    hub::SchemaSpec inbox_spec_local;
    {
        std::vector<std::string> schema_dirs;
        if (!hub.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(hub.hub_dir) / "schemas").string());

        try
        {
            const auto &cf = config_.role_data<consumer::ConsumerFields>();
            in_slot_spec_ = hub::resolve_schema(
                cf.in_slot_schema_json, false, "cons", schema_dirs);
            in_fz_local = hub::resolve_schema(
                cf.in_flexzone_schema_json, true, "cons", schema_dirs);
            if (config_.inbox().has_inbox())
                inbox_spec_local = hub::resolve_schema(
                    config_.inbox().schema_json, false, "cons", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[cons] Schema parse error: {}", e.what());
            promise_ref.set_value(false);
            return;
        }
    }

    // Packing is schema-driven (was transport-level `zmq_packing` pre-2026-04-20).
    const std::string packing =
        in_slot_spec_.has_schema ? in_slot_spec_.packing : "aligned";

    // Compute and store sizes (infrastructure-authoritative).
    if (in_slot_spec_.has_schema)
        core_.set_in_slot_spec(hub::SchemaSpec{in_slot_spec_},
                               hub::compute_schema_size(in_slot_spec_, packing));
    {
        size_t fz_size = hub::align_to_physical_page(
            hub::compute_schema_size(in_fz_local, packing));
        core_.set_in_fz_spec(hub::SchemaSpec{in_fz_local}, fz_size);
    }

    // ── Step 2: Setup infrastructure (no engine dependency) ──────────────────
    // Skipped in validate-only mode (no broker/queue needed).

    if (!core_.is_validate_only())
    {
        if (!setup_infrastructure_(inbox_spec_local))
        {
            teardown_infrastructure_();
            promise_ref.set_value(false);
            return;
        }
    }

    // ── Step 3: Wire infrastructure on the already-created api_ ──────────────
    //
    // api_ was constructed in RoleHostBase::startup_() before the worker
    // was spawned so its ThreadManager could spawn this thread under
    // bounded join.  Here we populate the mutable post-ctor wiring:
    // name, channels, log level, script/role dirs, queue pointers,
    // engine, checksum policy, event callbacks.

    api_ref.set_name(id.name);
    api_ref.set_channel(config_.in_channel());
    api_ref.set_log_level(id.log_level);
    api_ref.set_script_dir(script_dir.string());
    api_ref.set_role_dir(config_.base_dir().string());
    if (!core_.is_validate_only())
    {
        api_ref.set_inbox_queue(inbox_queue_.get());

        // Create BrokerRequestComm (connection deferred to step 6).
        broker_comm_ = std::make_unique<hub::BrokerRequestComm>();
        api_ref.set_broker_comm(broker_comm_.get());
    }
    api_ref.set_checksum_policy(config_.checksum().policy);
    api_ref.set_stop_on_script_error(sc.stop_on_script_error);
    api_ref.set_engine(&engine_ref);

    // ── Step 4: Load engine via lifecycle startup ────────────────────────────

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, id.uid);

    scripting::EngineModuleParams params;
    params.engine             = &engine_ref;
    params.api                = &api_ref;
    params.tag                = "cons";
    params.script_dir         = script_dir;
    params.entry_point        = (sc.type == "lua") ? "init.lua" : "__init__.py";
    params.required_callback  = "on_consume";
    params.in_slot_spec       = in_slot_spec_;
    params.in_fz_spec         = in_fz_local;
    params.inbox_spec         = inbox_spec_local;
    params.in_packing         = packing;
    params.module_name        = engine_module_name_;

    try
    {
        scripting::engine_lifecycle_startup(nullptr, &params);
        core_.set_script_load_ok(true);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[cons] Engine startup failed: {}", e.what());
        core_.set_script_load_ok(false);
        engine_ref.finalize();
        if (!core_.is_validate_only())
            teardown_infrastructure_();
        promise_ref.set_value(false);
        return;
    }

    // Validate-only mode: engine loaded successfully, exit.
    if (core_.is_validate_only())
    {
        engine_ref.finalize();
        promise_ref.set_value(true);
        return;
    }

    // ── Step 5: invoke on_init ───────────────────────────────────────────────
    engine_ref.invoke_on_init();

    // ── Step 6: Connect to broker, start ctrl thread, register ────────────
    core_.set_running(true);
    {
        hub::BrokerRequestComm::Config bc_cfg;
        bc_cfg.broker_endpoint = config_.in_hub().broker;
        bc_cfg.broker_pubkey   = config_.in_hub().broker_pubkey;
        bc_cfg.client_pubkey   = config_.auth().client_pubkey;
        bc_cfg.client_seckey   = config_.auth().client_seckey;
        bc_cfg.role_uid        = id.uid;
        bc_cfg.role_name       = id.name;

        scripting::RoleAPIBase::CtrlThreadConfig ctrl_cfg;
        ctrl_cfg.heartbeat_interval_ms = config_.timing().heartbeat_interval_ms;
        ctrl_cfg.report_metrics        = true;

        // Build consumer registration payload (CONSUMER_REG_REQ) via the
        // shared helper (HEP-CORE-0034 Phase 5b).  Schema fields layered
        // on below.
        const auto &ch = config_.in_channel();
        ctrl_cfg.consumer_reg_opts = hub::build_consumer_reg_payload(
            hub::ConsumerRegInputs{ch, id.uid, id.name});

        // HEP-CORE-0034 §10.3 — citation fields (Phase 5a wire population).
        // Mode (named vs anonymous) is decided by what the config
        // produced: the JSON schema-id form yields named-mode citation,
        // an inline schema yields anonymous-mode.  All-empty (no
        // in_slot_schema in config) → no validation (legacy compat).
        // Broker enforces the mode rules; we just paste whichever
        // fields make_wire_schema_fields populated.
        const auto &cf_for_wire = config_.role_data<consumer::ConsumerFields>();
        const auto wire_schema = hub::make_wire_schema_fields(
            cf_for_wire.in_slot_schema_json, in_slot_spec_, core_.in_fz_spec());
        hub::apply_consumer_schema_fields(ctrl_cfg.consumer_reg_opts, wire_schema);

        if (inbox_cfg_.has_inbox())
            ctrl_cfg.inbox = inbox_cfg_;

        if (!api_ref.start_ctrl_thread(bc_cfg, ctrl_cfg))
        {
            LOGGER_ERROR("[cons] Broker consumer registration failed — "
                         "broker won't track this consumer for liveness");
        }
    }

    // Step 6b: Startup coordination — wait for prerequisite roles (HEP-0023).
    if (!config_.startup().wait_for_roles.empty())
    {
        if (!scripting::wait_for_roles(*broker_comm_, config_.startup().wait_for_roles, "[cons]"))
        {
            LOGGER_ERROR("[cons] Startup coordination failed — required roles not available");
            promise_ref.set_value(false);
            return;
        }
    }

    // Step 7: Signal ready.
    promise_ref.set_value(true);

    // Step 8: Run the data loop via shared frame + ConsumerCycleOps.
    if (!api_ref.has_rx_side())
    {
        LOGGER_ERROR("[cons] run_data_loop: consumer not initialized — aborting");
        core_.set_running(false);
    }
    else
    {
        const auto &tc_loop = config_.timing();
        ConsumerCycleOps ops(api_ref, engine_ref, core_,
                             sc.stop_on_script_error);
        scripting::LoopConfig lcfg;
        lcfg.period_us                   = tc_loop.period_us;
        lcfg.loop_timing                 = tc_loop.loop_timing;
        lcfg.queue_io_wait_timeout_ratio = tc_loop.queue_io_wait_timeout_ratio;
        scripting::run_data_loop(api_ref, core_, lcfg, ops);
    }

    // Step 9: stop accepting invoke from non-owner threads.
    engine_ref.stop_accepting();

    // Step 9a: Explicitly deregister from broker (ctrl thread still running).
    if (has_api())
        api_ref.deregister_from_broker();

    // Step 10: last script callback (ctrl still alive for final I/O).
    engine_ref.invoke_on_stop();

    // Step 11: finalize engine.
    engine_ref.finalize();

    // Step 12: signal ctrl to exit (non-destructive stop, no socket close).
    if (broker_comm_) broker_comm_->stop();
    core_.set_running(false);
    core_.notify_incoming();

    // Step 13: teardown infrastructure.
    teardown_infrastructure_();

    // Step 14: drain all managed threads — last.
    api_ref.thread_manager().drain();
}

// ============================================================================
// setup_infrastructure_ — connect to broker, create consumer, wire events
// ============================================================================

bool ConsumerRoleHost::setup_infrastructure_(const hub::SchemaSpec &inbox_spec)
{
    auto       &core_   = core();
    const auto &config_ = config();
    auto       &api_ref = api();

    const auto &id    = config_.identity();
    const auto &tr    = config_.in_transport();
    const auto &shm   = config_.in_shm();
    const auto &tc    = config_.timing();
    inbox_cfg_ = config_.inbox();
    const auto &ch    = config_.in_channel();

    // --- Consumer options ---
    // channel_name / consumer_uid / consumer_name removed from opts —
    // build_rx_queue reads those directly from RoleAPIBase state
    // (set via set_channel / set_name, already called above).
    hub::RxQueueOptions opts;
    opts.shm_shared_secret    = shm.enabled ? shm.secret : 0u;
    opts.slot_spec            = in_slot_spec_;        // fields + packing
    opts.fz_spec              = core_.in_fz_spec();   // for schema-hash match

    // Queue abstraction: checksum policy.
    opts.checksum_policy    = config_.checksum().policy;
    opts.flexzone_checksum  = config_.checksum().flexzone && core_.has_rx_fz();

    // Transport declaration.
    const bool is_zmq = (tr.transport == config::Transport::Zmq);

    if (is_zmq)
    {
        opts.zmq_buffer_depth = tr.zmq_buffer_depth;
    }

    // --- Inbox setup (optional) ---
    if (inbox_cfg_.has_inbox())
    {
        auto inbox_result = scripting::setup_inbox_facility(
            inbox_spec, inbox_cfg_, config_.checksum().policy, "cons");
        if (!inbox_result)
            return false;
        inbox_queue_ = std::move(inbox_result->queue);
    }

    // --- Create consumer (RoleAPIBase owns the Rx queue) ---
    if (!api_ref.build_rx_queue(opts))
    {
        LOGGER_ERROR("[cons] Failed to connect consumer to channel '{}'", ch);
        return false;
    }

    // Broker notifications (band, hub-dead) are handled by BrokerRequestComm.

    // --- Start and configure data queue ---
    if (!api_ref.start_rx_queue())
    {
        LOGGER_ERROR("[cons] start_queue() failed for channel '{}'", ch);
        return false;
    }
    api_ref.reset_rx_queue_metrics();
    core_.set_configured_period(static_cast<uint64_t>(tc.period_us));

    LOGGER_INFO("[cons] Consumer started on channel '{}' (shm={})", ch,
                api_ref.rx_has_shm());

    // Startup coordination (HEP-0023) moved to after start_ctrl_thread().
    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ConsumerRoleHost::teardown_infrastructure_()
{
    // Broker and comm threads already joined via api_->thread_manager().drain().

    core().clear_inbox_cache();

    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    // Ctrl thread already joined by api_->thread_manager().drain().
    // Broker detects role death via heartbeat timeout — no explicit deregister needed.
    if (broker_comm_)
    {
        broker_comm_->disconnect();
        broker_comm_.reset();
    }

    if (has_api())
        api().close_queues();
}


} // namespace pylabhub::consumer
