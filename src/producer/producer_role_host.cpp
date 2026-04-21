/**
 * @file producer_role_host.cpp
 * @brief ProducerRoleHost — unified engine-agnostic producer implementation.
 *
 * This is the canonical data loop for the producer role.  It follows
 * docs/tech_draft/loop_design_unified.md §3 exactly.
 *
 * Layer 3 (infrastructure): Producer, queue, BrokerRequestComm, events.
 * Layer 2 (data loop): inner retry acquire, deadline wait, drain, invoke, commit.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_produce / invoke_on_inbox.
 *
 * Shared lifecycle scaffolding (ctor, startup_(), shutdown_(), ready-promise,
 * RoleAPIBase ownership, ThreadManager-owned worker) lives in RoleHostBase.
 * This file implements only the producer-specific worker_main_() hook and
 * its setup_infrastructure_() / teardown_infrastructure_() helpers.
 */
#include "producer_role_host.hpp"
#include "utils/thread_manager.hpp"
#include "producer_fields.hpp"
#include "utils/broker_request_comm.hpp"
#include "service/cycle_ops.hpp"
#include "service/data_loop.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "utils/engine_module_params.hpp"
#include "utils/role_host_helpers.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/schema_utils.hpp"
#include "utils/lifecycle.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::producer
{

using scripting::ProducerCycleOps;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Constructor / Destructor
// ============================================================================

ProducerRoleHost::ProducerRoleHost(config::RoleConfig config,
                                     std::unique_ptr<scripting::ScriptEngine> engine,
                                     std::atomic<bool> *shutdown_flag)
    : scripting::RoleHostBase("prod", std::move(config), std::move(engine),
                              shutdown_flag)
{
}

ProducerRoleHost::~ProducerRoleHost()
{
    // Join worker BEFORE producer-specific members (broker_comm_, inbox_queue_)
    // begin destruction — the worker's teardown path reads them. shutdown_()
    // is idempotent, so the base destructor calling it again is a no-op.
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

void ProducerRoleHost::worker_main_()
{
    auto       &core_        = core();
    const auto &config_      = config();
    auto       &engine_ref   = engine();
    auto       &api_ref      = api();
    auto       &promise_ref  = ready_promise();

    const auto &id   = config_.identity();
    const auto &sc   = config_.script();
    const auto &hub  = config_.out_hub();
    const auto &tr   = config_.out_transport();
    const auto &pf   = config_.role_data<ProducerFields>();

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[prod] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    sc.type, sc.type);
    }

    // ── Step 1: Resolve schemas from config ──────────────────────────────────

    const std::filesystem::path base_path =
        sc.path.empty() ? std::filesystem::current_path()
                        : std::filesystem::weakly_canonical(sc.path);
    const std::filesystem::path script_dir = base_path / "script" / sc.type;

    hub::SchemaSpec out_fz_local;
    hub::SchemaSpec inbox_spec_local;
    {
        std::vector<std::string> schema_dirs;
        if (!hub.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(hub.hub_dir) / "schemas").string());

        try
        {
            out_slot_spec_ = hub::resolve_schema(
                pf.out_slot_schema_json, false, "prod", schema_dirs);
            out_fz_local = hub::resolve_schema(
                pf.out_flexzone_schema_json, true, "prod", schema_dirs);
            if (config_.inbox().has_inbox())
                inbox_spec_local = hub::resolve_schema(
                    config_.inbox().schema_json, false, "prod", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[prod] Schema parse error: {}", e.what());
            promise_ref.set_value(false);
            return;
        }
    }

    // Packing is schema-driven (was transport-level `zmq_packing` pre-2026-04-20).
    // Slot and flexzone each carry their own packing; script/wire/SHM all honor
    // the schema's packing for that side.  "aligned" fallback for flexzone-only
    // roles where out_slot_spec_ is unset.
    const std::string packing =
        out_slot_spec_.has_schema ? out_slot_spec_.packing : "aligned";

    // Compute and store sizes (infrastructure-authoritative).
    if (out_slot_spec_.has_schema)
        core_.set_out_slot_spec(hub::SchemaSpec{out_slot_spec_},
                                hub::compute_schema_size(out_slot_spec_, packing));
    {
        size_t fz_size = hub::align_to_physical_page(
            hub::compute_schema_size(out_fz_local, packing));
        core_.set_out_fz_spec(hub::SchemaSpec{out_fz_local}, fz_size);
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
    // bounded join.  Here we populate mutable post-ctor wiring state.
    api_ref.set_name(id.name);
    api_ref.set_channel(config_.out_channel());
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

    // ── Step 4: Load engine via lifecycle module ─────────────────────────────
    // engine_lifecycle_startup does: initialize → load_script → register_slot_type
    // (all directions + inbox) → build_api. Single call replaces manual steps.

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, id.uid);

    scripting::EngineModuleParams params;
    params.engine             = &engine_ref;
    params.api                = &api_ref;
    params.tag                = "prod";
    params.script_dir         = script_dir;
    params.entry_point        = (sc.type == "lua") ? "init.lua" : "__init__.py";
    params.required_callback  = "on_produce";
    params.out_slot_spec      = out_slot_spec_;
    params.out_fz_spec        = out_fz_local;
    params.inbox_spec         = inbox_spec_local;
    params.out_packing        = packing;
    params.module_name        = engine_module_name_;

    try
    {
        scripting::engine_lifecycle_startup(nullptr, &params);
        core_.set_script_load_ok(true);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[prod] Engine startup failed: {}", e.what());
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

    // Sync flexzone checksum after on_init (user may have written to flexzone).
    if (api_ref.has_tx_side() && core_.has_tx_fz())
        api_ref.sync_tx_flexzone_checksum();

    // ── Step 6: Connect to broker, start ctrl thread, register ────────────
    core_.set_running(true);
    {
        hub::BrokerRequestComm::Config bc_cfg;
        bc_cfg.broker_endpoint = config_.out_hub().broker;
        bc_cfg.broker_pubkey   = config_.out_hub().broker_pubkey;
        bc_cfg.client_pubkey   = config_.auth().client_pubkey;
        bc_cfg.client_seckey   = config_.auth().client_seckey;
        bc_cfg.role_uid        = id.uid;
        bc_cfg.role_name       = id.name;

        scripting::RoleAPIBase::CtrlThreadConfig ctrl_cfg;
        ctrl_cfg.heartbeat_interval_ms = config_.timing().heartbeat_interval_ms;
        ctrl_cfg.report_metrics        = false;

        // Build producer registration payload (REG_REQ).
        const auto &ch  = config_.out_channel();
        const auto &shm = config_.out_shm();

        ctrl_cfg.producer_reg_opts["channel_name"]      = ch;
        ctrl_cfg.producer_reg_opts["pattern"]            = "PubSub";
        ctrl_cfg.producer_reg_opts["has_shared_memory"]  = shm.enabled;
        ctrl_cfg.producer_reg_opts["producer_pid"]       = pylabhub::platform::get_pid();
        ctrl_cfg.producer_reg_opts["shm_name"]           = ch;
        ctrl_cfg.producer_reg_opts["role_uid"]           = id.uid;
        ctrl_cfg.producer_reg_opts["role_name"]          = id.name;
        ctrl_cfg.producer_reg_opts["role_type"]          = "producer";
        ctrl_cfg.producer_reg_opts["zmq_ctrl_endpoint"]  = "tcp://127.0.0.1:0";
        ctrl_cfg.producer_reg_opts["zmq_data_endpoint"]  = "tcp://127.0.0.1:0";
        ctrl_cfg.producer_reg_opts["zmq_pubkey"]         = "";

        if (tr.transport == config::Transport::Zmq)
        {
            ctrl_cfg.producer_reg_opts["data_transport"]    = "zmq";
            ctrl_cfg.producer_reg_opts["zmq_node_endpoint"] = tr.zmq_endpoint;
        }
        if (inbox_cfg_.has_inbox())
            ctrl_cfg.inbox = inbox_cfg_;

        if (!api_ref.start_ctrl_thread(bc_cfg, ctrl_cfg))
        {
            LOGGER_ERROR("[prod] Broker registration failed — consumers won't discover this producer");
            // Non-fatal: producer can still operate locally without broker discovery.
        }
    }

    // Step 6b: Startup coordination — wait for prerequisite roles (HEP-0023).
    if (!config_.startup().wait_for_roles.empty())
    {
        if (!scripting::wait_for_roles(*broker_comm_, config_.startup().wait_for_roles, "[prod]"))
        {
            LOGGER_ERROR("[prod] Startup coordination failed — required roles not available");
            promise_ref.set_value(false);
            return;
        }
    }

    // Step 7: Signal ready.
    promise_ref.set_value(true);

    // Step 8: Run the data loop via shared frame + ProducerCycleOps.
    if (!api_ref.has_tx_side())
    {
        LOGGER_ERROR("[prod] run_data_loop: producer not initialized — aborting");
        core_.set_running(false);
    }
    else
    {
        const auto &tc_loop = config_.timing();
        ProducerCycleOps ops(api_ref, engine_ref, core_,
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

    // Step 10: last script callback — ctrl thread is still alive so the
    // script can perform final I/O (flush metrics, send summary, etc.).
    engine_ref.invoke_on_stop();

    // Step 11: finalize engine (free script resources).
    engine_ref.finalize();

    // Step 12: signal ctrl thread's poll loop to exit (non-destructive —
    // sets stop flag + wakes poll, does NOT close sockets).
    if (broker_comm_) broker_comm_->stop();
    core_.set_running(false);
    core_.notify_incoming();

    // Step 13: teardown infrastructure (disconnect broker, close inbox/queues).
    teardown_infrastructure_();

    // Step 14: drain all managed threads — last. Ctrl thread has already
    // exited its poll loop (signaled in step 12), so join is immediate.
    api_ref.thread_manager().drain();
}

// ============================================================================
// setup_infrastructure_ — connect to broker, create producer, wire events
// ============================================================================

bool ProducerRoleHost::setup_infrastructure_(const hub::SchemaSpec &inbox_spec)
{
    auto       &core_   = core();
    const auto &config_ = config();
    auto       &api_ref = api();

    const auto &id    = config_.identity();
    const auto &tr    = config_.out_transport();
    const auto &shm   = config_.out_shm();
    const auto &tc    = config_.timing();
    inbox_cfg_ = config_.inbox(); // mutable copy for resolved fields
    const auto &ch    = config_.out_channel();

    // --- Producer options ---
    // channel_name / role_name / role_uid removed from opts — build_tx_queue
    // reads those directly from RoleAPIBase state (set via set_channel /
    // set_name, already called above).
    hub::TxQueueOptions opts;
    opts.has_shm      = shm.enabled;
    // Single source of truth for schema+packing; hash auto-computed
    // inside build_tx_queue from these.
    opts.slot_spec    = out_slot_spec_;
    opts.fz_spec      = core_.out_fz_spec();


    // --- Inbox setup (optional) ---
    if (inbox_cfg_.has_inbox())
    {
        auto inbox_result = scripting::setup_inbox_facility(
            inbox_spec, inbox_cfg_, config_.checksum().policy, "prod");
        if (!inbox_result)
            return false;
        inbox_queue_ = std::move(inbox_result->queue);
    }

    // --- Queue abstraction: checksum policy ---
    opts.checksum_policy    = config_.checksum().policy;
    opts.flexzone_checksum  = config_.checksum().flexzone && core_.has_tx_fz();
    // Timing is a role-level concern — core_.set_configured_period() handles it.

    // --- SHM config ---
    if (shm.enabled)
    {
        opts.shm_config.shared_secret        = shm.secret;
        opts.shm_config.ring_buffer_capacity = shm.slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = shm.sync_policy;
        opts.shm_config.checksum_policy      = config_.checksum().policy;
        opts.shm_config.physical_page_size = hub::system_page_size();
    }

    // --- ZMQ transport (HEP-CORE-0021) ---
    if (tr.transport == config::Transport::Zmq)
    {
        opts.has_shm           = false;
        opts.data_transport    = "zmq";
        opts.zmq_node_endpoint = tr.zmq_endpoint;
        opts.zmq_bind          = tr.zmq_bind;
        opts.zmq_buffer_depth  = tr.zmq_buffer_depth;
        opts.zmq_overflow_policy =
            (tr.zmq_overflow_policy == "block") ? hub::OverflowPolicy::Block
                                                : hub::OverflowPolicy::Drop;
    }

    // --- Create producer (RoleAPIBase owns the Tx queue) ---
    if (!api_ref.build_tx_queue(opts))
    {
        LOGGER_ERROR("[prod] Failed to create producer for channel '{}'", ch);
        return false;
    }

    // Broker notifications (band, hub-dead) are handled by BrokerRequestComm.

    // --- Start and configure data queue ---
    if (!api_ref.start_tx_queue())
    {
        LOGGER_ERROR("[prod] start_queue() failed for channel '{}'", ch);
        return false;
    }
    api_ref.reset_tx_queue_metrics();
    core_.set_configured_period(static_cast<uint64_t>(tc.period_us));

    LOGGER_INFO("[prod] Producer started on channel '{}' (shm={})", ch,
                api_ref.tx_has_shm());

    // Startup coordination (HEP-0023) moved to after start_ctrl_thread()
    // where the BRC is connected and poll loop running.

    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ProducerRoleHost::teardown_infrastructure_()
{
    // Broker and comm threads already joined via api_->thread_manager().drain().
    // set_running(false) also already called. Defensive re-set is safe.

    // Clean up shared resources (engine already finalized — no scripts running).
    core().clear_inbox_cache();

    // Stop inbox_queue_ (if exists).
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    // Ctrl thread already joined. Deregistration done in step 9a.
    if (broker_comm_)
    {
        broker_comm_->disconnect();
        broker_comm_.reset();
    }

    // Close producer (data-plane queue teardown happens inside RoleAPIBase).
    if (has_api())
        api().close_queues();
}


} // namespace pylabhub::producer
