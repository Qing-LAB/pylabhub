/**
 * @file processor_role_host.cpp
 * @brief ProcessorRoleHost — unified engine-agnostic processor implementation.
 *
 * This is the canonical data loop for the processor role.  It follows
 * docs/tech_draft/loop_design_unified.md §5 exactly.
 *
 * Layer 3 (infrastructure): BrokerRequestComm, Consumer + Producer, dual queues,
 *   ctrl_thread_, events.
 * Layer 2 (data loop): dual-queue inner retry acquire, deadline wait, drain,
 *   invoke, commit/release.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_process / invoke_on_inbox.
 */
#include "processor_role_host.hpp"
#include "utils/thread_manager.hpp"
#include "utils/cycle_ops.hpp"
#include "utils/broker_request_comm.hpp"
#include "processor_fields.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "utils/engine_module_params.hpp"
#include "utils/role_host_helpers.hpp"
#include "utils/zmq_poll_loop.hpp"
#include "utils/schema_utils.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::processor
{

using scripting::IncomingMessage;
using scripting::InvokeResult;
using scripting::InvokeRx;
using scripting::InvokeTx;
using scripting::ProcessorCycleOps;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Destructor
// ============================================================================

ProcessorRoleHost::~ProcessorRoleHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

ProcessorRoleHost::ProcessorRoleHost(config::RoleConfig config,
                                       std::unique_ptr<scripting::ScriptEngine> engine,
                                       std::atomic<bool> *shutdown_flag)
    : config_(std::move(config))
    , engine_(std::move(engine))
{
    core_.set_shutdown_flag(shutdown_flag);
}


// ============================================================================
// startup_ — spawn worker thread, block until ready or failure
// ============================================================================

void ProcessorRoleHost::startup_()
{
    ready_promise_ = std::promise<bool>{};
    auto ready_future = ready_promise_.get_future();

    // Construct api_ here so the role's ThreadManager is available
    // before the worker thread is spawned.
    api_ = std::make_unique<scripting::RoleAPIBase>(
        core_, "proc", config_.identity().uid);

    api_->thread_manager().spawn("worker", [this] { worker_main_(); });

    const bool ok = ready_future.get();
    if (!ok)
    {
        api_.reset();
    }
}

// ============================================================================
// shutdown_ — signal shutdown; ThreadManager dtor bounded-joins worker
// ============================================================================

void ProcessorRoleHost::shutdown_()
{
    core_.request_stop();
    core_.notify_incoming();
    api_.reset();
}

// ProcessorCycleOps has moved to src/include/utils/cycle_ops.hpp so the L3.β
// baseline test suite can instantiate it directly. Behavior unchanged.

// ============================================================================
// worker_main_ — the worker thread entry point
// ============================================================================

void ProcessorRoleHost::worker_main_()
{
    const auto &sc = config_.script();

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[proc] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    sc.type, sc.type);
    }

    // ── Step 1: Resolve schemas from config ──────────────────────────────────

    const std::filesystem::path base_path =
        sc.path.empty() ? std::filesystem::current_path()
                        : std::filesystem::weakly_canonical(sc.path);
    const std::filesystem::path script_dir = base_path / "script" / sc.type;

    // Determine packing: input and output may use different ZMQ packing.
    const std::string in_packing =
        config_.in_transport().zmq_packing.empty() ? "aligned" : config_.in_transport().zmq_packing;
    const std::string out_packing =
        config_.out_transport().zmq_packing.empty() ? "aligned" : config_.out_transport().zmq_packing;

    hub::SchemaSpec out_fz_local;
    hub::SchemaSpec in_fz_local;
    hub::SchemaSpec inbox_spec_local;
    {
        std::vector<std::string> schema_dirs;
        auto add_schema_dir = [&schema_dirs](const std::string &hub_dir) {
            if (hub_dir.empty())
                return;
            const std::string d = (std::filesystem::path(hub_dir) / "schemas").string();
            if (std::find(schema_dirs.begin(), schema_dirs.end(), d) == schema_dirs.end())
                schema_dirs.push_back(d);
        };
        add_schema_dir(config_.in_hub().hub_dir);
        add_schema_dir(config_.out_hub().hub_dir);

        const auto &pf = config_.role_data<ProcessorFields>();
        try
        {
            in_slot_spec_  = hub::resolve_schema(
                pf.in_slot_schema_json, false, "proc", schema_dirs);
            out_slot_spec_ = hub::resolve_schema(
                pf.out_slot_schema_json, false, "proc", schema_dirs);
            in_fz_local    = hub::resolve_schema(
                pf.in_flexzone_schema_json, true, "proc", schema_dirs);
            out_fz_local   = hub::resolve_schema(
                pf.out_flexzone_schema_json, true, "proc", schema_dirs);
            if (config_.inbox().has_inbox())
                inbox_spec_local = hub::resolve_schema(
                    config_.inbox().schema_json, false, "proc", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[proc] Schema parse error: {}", e.what());
            ready_promise_.set_value(false);
            return;
        }
    }

    // Compute and store sizes (infrastructure-authoritative).
    if (in_slot_spec_.has_schema)
        core_.set_in_slot_spec(hub::SchemaSpec{in_slot_spec_},
                               hub::compute_schema_size(in_slot_spec_, in_packing));
    if (out_slot_spec_.has_schema)
        core_.set_out_slot_spec(hub::SchemaSpec{out_slot_spec_},
                                hub::compute_schema_size(out_slot_spec_, out_packing));
    {
        size_t out_fz_size = hub::align_to_physical_page(
            hub::compute_schema_size(out_fz_local, out_packing));
        core_.set_out_fz_spec(hub::SchemaSpec{out_fz_local}, out_fz_size);
    }
    {
        size_t in_fz_size = hub::align_to_physical_page(
            hub::compute_schema_size(in_fz_local, in_packing));
        core_.set_in_fz_spec(hub::SchemaSpec{in_fz_local}, in_fz_size);
    }

    // ── Step 2: Setup infrastructure (no engine dependency) ──────────────────
    // Skipped in validate-only mode (no broker/queue needed).

    if (!core_.is_validate_only())
    {
        if (!setup_infrastructure_(inbox_spec_local))
        {
            teardown_infrastructure_();
            ready_promise_.set_value(false);
            return;
        }
    }

    // ── Step 3: Populate mutable wiring on the already-created api_ ──────────
    //
    // api_ was constructed in startup_() before the worker was spawned
    // so its ThreadManager could own this worker thread (bounded join on
    // teardown). Populate post-ctor state here.

    api_->set_name(config_.identity().name);
    api_->set_channel(config_.in_channel());
    api_->set_out_channel(config_.out_channel());
    api_->set_log_level(config_.identity().log_level);
    api_->set_script_dir(script_dir.string());
    api_->set_role_dir(config_.base_dir().string());
    if (!core_.is_validate_only())
    {
        api_->set_inbox_queue(inbox_queue_.get());

        // Create BrokerRequestComm (connection deferred to step 6).
        broker_comm_ = std::make_unique<hub::BrokerRequestComm>();
        api_->set_broker_comm(broker_comm_.get());
    }
    api_->set_checksum_policy(config_.checksum().policy);
    api_->set_stop_on_script_error(sc.stop_on_script_error);
    api_->set_engine(engine_.get());

    // ── Step 4: Load engine via lifecycle startup ────────────────────────────

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, config_.identity().uid);

    scripting::EngineModuleParams params;
    params.engine             = engine_.get();
    params.api                = api_.get();
    params.tag                = "proc";
    params.script_dir         = script_dir;
    params.entry_point        = (sc.type == "lua") ? "init.lua" : "__init__.py";
    params.required_callback  = "on_process";
    params.in_slot_spec       = in_slot_spec_;
    params.out_slot_spec      = out_slot_spec_;
    params.in_fz_spec         = in_fz_local;
    params.out_fz_spec        = out_fz_local;
    params.inbox_spec         = inbox_spec_local;
    params.in_packing         = in_packing;
    params.out_packing        = out_packing;
    params.module_name        = engine_module_name_;

    try
    {
        scripting::engine_lifecycle_startup(nullptr, &params);
        core_.set_script_load_ok(true);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[proc] Engine startup failed: {}", e.what());
        core_.set_script_load_ok(false);
        engine_->finalize();
        if (!core_.is_validate_only())
            teardown_infrastructure_();
        ready_promise_.set_value(false);
        return;
    }

    // Validate-only mode: engine loaded successfully, exit.
    if (core_.is_validate_only())
    {
        engine_->finalize();
        ready_promise_.set_value(true);
        return;
    }

    // ── Step 5: invoke on_init ───────────────────────────────────────────────
    engine_->invoke_on_init();

    // Sync flexzone checksum after on_init (user may have written to flexzone).
    if (api_->has_tx_side() && core_.has_out_fz())
        api_->sync_tx_flexzone_checksum();

    // ── Step 6: Connect to broker, start ctrl thread, register ────────────
    core_.set_running(true);
    {
        const auto &id  = config_.identity();
        const auto &shm = config_.out_shm();
        const auto &tr  = config_.out_transport();

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

        // Output producer registration (REG_REQ).
        ctrl_cfg.producer_reg_opts["channel_name"]      = config_.out_channel();
        ctrl_cfg.producer_reg_opts["pattern"]            = "PubSub";
        ctrl_cfg.producer_reg_opts["has_shared_memory"]  = shm.enabled;
        ctrl_cfg.producer_reg_opts["producer_pid"]       = pylabhub::platform::get_pid();
        ctrl_cfg.producer_reg_opts["shm_name"]           = config_.out_channel();
        ctrl_cfg.producer_reg_opts["role_uid"]           = id.uid;
        ctrl_cfg.producer_reg_opts["role_name"]          = id.name;
        ctrl_cfg.producer_reg_opts["role_type"]          = "processor";
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

        // Input consumer registration (CONSUMER_REG_REQ).
        ctrl_cfg.consumer_reg_opts["channel_name"]  = config_.in_channel();
        ctrl_cfg.consumer_reg_opts["consumer_uid"]  = id.uid;
        ctrl_cfg.consumer_reg_opts["consumer_name"] = id.name;
        ctrl_cfg.consumer_reg_opts["consumer_pid"]  = pylabhub::platform::get_pid();

        if (!api_->start_ctrl_thread(bc_cfg, ctrl_cfg))
        {
            LOGGER_ERROR("[proc] Broker registration failed");
        }
    }

    // Step 6b: Startup coordination — wait for prerequisite roles (HEP-0023).
    if (!config_.startup().wait_for_roles.empty())
    {
        if (!scripting::wait_for_roles(*broker_comm_, config_.startup().wait_for_roles, "[proc]"))
        {
            LOGGER_ERROR("[proc] Startup coordination failed — required roles not available");
            ready_promise_.set_value(false);
            return;
        }
    }

    // Step 7: Signal ready.
    ready_promise_.set_value(true);

    // Step 8: Run the data loop via shared frame + ProcessorCycleOps.
    if (!api_->has_rx_side() || !api_->has_tx_side())
    {
        LOGGER_ERROR("[proc] run_data_loop: transport queues not initialized — aborting");
        core_.set_running(false);
    }
    else
    {
        const auto &tc_loop = config_.timing();
        const bool drop_mode =
            (config_.out_transport().zmq_overflow_policy == "drop");
        ProcessorCycleOps ops(*api_, *engine_, core_,
                              sc.stop_on_script_error,
                              drop_mode);
        scripting::LoopConfig lcfg;
        lcfg.period_us                   = tc_loop.period_us;
        lcfg.loop_timing                 = tc_loop.loop_timing;
        lcfg.queue_io_wait_timeout_ratio = tc_loop.queue_io_wait_timeout_ratio;
        api_->run_data_loop(lcfg, ops);
    }

    // Step 9: stop accepting invoke from non-owner threads.
    engine_->stop_accepting();

    // Step 9a: Explicitly deregister from broker (ctrl thread still running).
    if (api_)
        api_->deregister_from_broker();

    // Step 10: join all managed threads (ctrl + future workers).
    core_.set_running(false);
    core_.notify_incoming();
    api_->join_all_threads();

    // Step 11: last script callback.
    engine_->invoke_on_stop();

    // Step 12: finalize engine.
    engine_->finalize();

    // Step 13: teardown infrastructure.
    teardown_infrastructure_();
}

// ============================================================================
// setup_infrastructure_ — dual broker connect, consumer + producer, wire events
// ============================================================================

bool ProcessorRoleHost::setup_infrastructure_(const hub::SchemaSpec &inbox_spec)
{
    // ── Consumer side (in_channel) ──────────────────────────────────────────
    hub::ConsumerOptions in_opts;
    in_opts.channel_name         = config_.in_channel();
    in_opts.shm_shared_secret    = config_.in_shm().enabled ? config_.in_shm().secret : 0u;
    in_opts.expected_schema_hash = hub::compute_schema_hash(
                                       in_slot_spec_, core_.in_fz_spec());
    in_opts.consumer_uid         = config_.identity().uid;
    in_opts.consumer_name        = config_.identity().name;
    in_opts.zmq_schema           = hub::schema_spec_to_zmq_fields(in_slot_spec_);
    in_opts.zmq_packing          = config_.in_transport().zmq_packing;
    in_opts.zmq_buffer_depth     = config_.in_transport().zmq_buffer_depth;
    // Per-role checksum policy — same value on both input and output (see config_single_truth.md).
    in_opts.checksum_policy      = config_.checksum().policy;
    in_opts.flexzone_checksum    = config_.checksum().flexzone && core_.has_in_fz();
    // Transport declaration — broker validates mismatch.
    const bool in_is_zmq = (config_.in_transport().transport == config::Transport::Zmq);
    in_opts.queue_type = in_is_zmq ? "zmq" : "shm";

    if (!api_->build_rx_queue(in_opts))
    {
        LOGGER_ERROR("[proc] Failed to connect consumer to in_channel '{}'",
                     config_.in_channel());
        return false;
    }

    // ── Producer side (out_channel) ─────────────────────────────────────────
    hub::ProducerOptions out_opts;
    out_opts.channel_name  = config_.out_channel();
    out_opts.pattern       = hub::ChannelPattern::PubSub;
    out_opts.has_shm       = config_.out_shm().enabled;
    out_opts.schema_hash   = hub::compute_schema_hash(out_slot_spec_, core_.out_fz_spec());
    out_opts.role_name     = config_.identity().name;
    out_opts.role_uid      = config_.identity().uid;
    // Per-role checksum policy — same value on both input and output (see config_single_truth.md).
    out_opts.checksum_policy    = config_.checksum().policy;
    out_opts.flexzone_checksum  = config_.checksum().flexzone && core_.has_out_fz();
    // SHM config (output side).
    if (config_.out_shm().enabled)
    {
        out_opts.shm_config.shared_secret        = config_.out_shm().secret;
        out_opts.shm_config.ring_buffer_capacity = config_.out_shm().slot_count;
        out_opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        out_opts.shm_config.consumer_sync_policy = config_.out_shm().sync_policy;
        out_opts.shm_config.checksum_policy      = config_.checksum().policy;
        out_opts.shm_config.physical_page_size = hub::system_page_size();
    }

    // HEP-CORE-0021: for ZMQ output, register the peer endpoint with the broker.
    if (config_.out_transport().transport == config::Transport::Zmq)
    {
        out_opts.has_shm           = false;
        out_opts.data_transport    = "zmq";
        out_opts.zmq_node_endpoint = config_.out_transport().zmq_endpoint;
        out_opts.zmq_bind          = config_.out_transport().zmq_bind;
        out_opts.zmq_schema        = hub::schema_spec_to_zmq_fields(out_slot_spec_);
        out_opts.zmq_packing       = config_.out_transport().zmq_packing;
        out_opts.zmq_buffer_depth  = config_.out_transport().zmq_buffer_depth;
        out_opts.zmq_overflow_policy =
            (config_.out_transport().zmq_overflow_policy == "block")
                ? hub::OverflowPolicy::Block
                : hub::OverflowPolicy::Drop;
    }

    // ── Inbox facility (optional) ───────────────────────────────────────────
    inbox_cfg_ = config_.inbox();
    if (inbox_cfg_.has_inbox())
    {
        auto inbox_result = scripting::setup_inbox_facility(
            inbox_spec, inbox_cfg_, config_.checksum().policy, "proc");
        if (!inbox_result)
            return false;
        inbox_queue_ = std::move(inbox_result->queue);
    }

    // --- Create producer (RoleAPIBase owns the Tx queue) ---
    if (!api_->build_tx_queue(out_opts))
    {
        LOGGER_ERROR("[proc] Failed to create producer for out_channel '{}'",
                     config_.out_channel());
        return false;
    }

    // Broker notifications (band, hub-dead) are handled by BrokerRequestComm.

    // --- Start and configure data queues ---
    if (!api_->start_rx_queue())
    {
        LOGGER_ERROR("[proc] Input start_queue() failed (in_channel='{}')",
                     config_.in_channel());
        return false;
    }
    if (!api_->start_tx_queue())
    {
        LOGGER_ERROR("[proc] Output start_queue() failed (out_channel='{}')",
                     config_.out_channel());
        return false;
    }
    // Reset metrics (checksum and period already configured via Options).
    api_->reset_rx_queue_metrics();
    api_->reset_tx_queue_metrics();
    core_.set_configured_period(static_cast<uint64_t>(config_.timing().period_us));

    LOGGER_INFO("[proc] Processor started: '{}' -> '{}'",
                config_.in_channel(), config_.out_channel());

    // Startup coordination (HEP-0023) moved to after start_ctrl_thread().
    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ProcessorRoleHost::teardown_infrastructure_()
{
    // Broker and comm threads already joined via api_->join_all_threads().

    core_.clear_inbox_cache();

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

    // Close Tx/Rx queues (data-plane teardown happens inside RoleAPIBase).
    if (api_) api_->close_queues();
}


// ============================================================================
// snapshot_metrics_json — for heartbeat reporting
// ============================================================================

nlohmann::json ProcessorRoleHost::snapshot_metrics_json() const
{
    nlohmann::json result;

    if (api_ && api_->has_rx_side())
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, api_->queue_metrics(scripting::ChannelSide::Rx));
        result["in_queue"] = std::move(q);
    }
    if (api_ && api_->has_tx_side())
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, api_->queue_metrics(scripting::ChannelSide::Tx));
        result["out_queue"] = std::move(q);
    }

    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, core_.loop_metrics());
        result["loop"] = std::move(lm);
    }

    result["role"] = {
        {"in_slots_received",  core_.in_slots_received()},
        {"out_slots_written",  core_.out_slots_written()},
        {"out_drop_count",     core_.out_drop_count()},
        {"script_error_count", engine_ ? engine_->script_error_count() : 0},
        {"ctrl_queue_dropped", {{"input", 0}, {"output", 0}}}
    };

    if (inbox_queue_)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, inbox_queue_->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    return result;
}

} // namespace pylabhub::processor
