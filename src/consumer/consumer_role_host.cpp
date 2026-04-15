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
 */
#include "consumer_role_host.hpp"
#include "utils/thread_manager.hpp"
#include "utils/cycle_ops.hpp"
#include "utils/broker_request_comm.hpp"
#include "consumer_fields.hpp"

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

namespace pylabhub::consumer
{

using scripting::ConsumerCycleOps;
using scripting::IncomingMessage;
using scripting::InvokeRx;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Destructor
// ============================================================================

ConsumerRoleHost::ConsumerRoleHost(config::RoleConfig config,
                                     std::unique_ptr<scripting::ScriptEngine> engine,
                                     std::atomic<bool> *shutdown_flag)
    : config_(std::move(config))
    , engine_(std::move(engine))
{
    core_.set_shutdown_flag(shutdown_flag);
}

ConsumerRoleHost::~ConsumerRoleHost()
{
    shutdown_();
}

// ============================================================================
// startup_ — spawn worker thread, block until ready or failure
// ============================================================================

void ConsumerRoleHost::startup_()
{
    ready_promise_ = std::promise<bool>{};
    auto ready_future = ready_promise_.get_future();

    // Construct api_ here (not inside worker_main_) so the role's
    // ThreadManager exists BEFORE we spawn the worker thread — the
    // worker then lives under api_->thread_manager() alongside ctrl
    // and any future role-scope threads. role_tag + uid are compile-
    // time required by RoleAPIBase's ctor.
    api_ = std::make_unique<scripting::RoleAPIBase>(
        core_, "cons", config_.identity().uid);

    api_->thread_manager().spawn("worker", [this] { worker_main_(); });

    const bool ok = ready_future.get();
    if (!ok)
    {
        // Worker signaled setup failure. Drop api_ → its ThreadManager
        // dtor bounded-joins the worker (which should already be
        // exiting) with ERROR + detach if it got stuck.
        api_.reset();
    }
}

// ============================================================================
// shutdown_ — signal shutdown; ThreadManager dtor bounded-joins worker
// ============================================================================

void ConsumerRoleHost::shutdown_()
{
    core_.request_stop();
    core_.notify_incoming();
    // api_.reset() → RoleAPIBase dtor → ThreadManager dtor →
    // bounded join("worker") with ERROR-on-timeout + detach.
    api_.reset();
}

// ConsumerCycleOps has moved to src/include/utils/cycle_ops.hpp so the L3.β
// baseline test suite can instantiate it directly. Behavior unchanged.

// ============================================================================
// worker_main_ — the worker thread entry point
// ============================================================================

void ConsumerRoleHost::worker_main_()
{
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

    const std::string packing =
        tr.zmq_packing.empty() ? "aligned" : tr.zmq_packing;

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
            ready_promise_.set_value(false);
            return;
        }
    }

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
            ready_promise_.set_value(false);
            return;
        }
    }

    // ── Step 3: Wire infrastructure on the already-created api_ ──────────────
    //
    // api_ itself was constructed in startup_() so its ThreadManager was
    // available to spawn this worker thread. Here we populate the
    // mutable post-ctor wiring: name, channels, log level, script/role
    // dirs, queue pointers, engine, checksum policy, event callbacks.

    api_->set_name(id.name);
    api_->set_channel(config_.in_channel());
    api_->set_log_level(id.log_level);
    api_->set_script_dir(script_dir.string());
    api_->set_role_dir(config_.base_dir().string());
    if (!core_.is_validate_only())
    {
        api_->set_consumer(in_consumer_.has_value() ? &(*in_consumer_) : nullptr);
        api_->set_inbox_queue(inbox_queue_.get());

        // Create BrokerRequestComm (connection deferred to step 6).
        broker_comm_ = std::make_unique<hub::BrokerRequestComm>();
        api_->set_broker_comm(broker_comm_.get());
    }
    api_->set_checksum_policy(config_.checksum().policy);
    api_->set_stop_on_script_error(sc.stop_on_script_error);
    api_->set_engine(engine_.get());

    // ── Step 4: Load engine via lifecycle startup ────────────────────────────

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, id.uid);

    scripting::EngineModuleParams params;
    params.engine             = engine_.get();
    params.api                = api_.get();
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

        // Build consumer registration payload (CONSUMER_REG_REQ).
        const auto &ch = config_.in_channel();
        ctrl_cfg.consumer_reg_opts["channel_name"]  = ch;
        ctrl_cfg.consumer_reg_opts["consumer_uid"]  = id.uid;
        ctrl_cfg.consumer_reg_opts["consumer_name"] = id.name;
        ctrl_cfg.consumer_reg_opts["consumer_pid"]  = pylabhub::platform::get_pid();

        if (inbox_cfg_.has_inbox())
            ctrl_cfg.inbox = inbox_cfg_;

        if (!api_->start_ctrl_thread(bc_cfg, ctrl_cfg))
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
            ready_promise_.set_value(false);
            return;
        }
    }

    // Step 7: Signal ready.
    ready_promise_.set_value(true);

    // Step 8: Run the data loop via shared frame + ConsumerCycleOps.
    if (!in_consumer_.has_value())
    {
        LOGGER_ERROR("[cons] run_data_loop: consumer not initialized — aborting");
        core_.set_running(false);
    }
    else
    {
        const auto &tc_loop = config_.timing();
        ConsumerCycleOps ops(*api_, *engine_, core_,
                             sc.stop_on_script_error);
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
// setup_infrastructure_ — connect to broker, create consumer, wire events
// ============================================================================

bool ConsumerRoleHost::setup_infrastructure_(const hub::SchemaSpec &inbox_spec)
{
    const auto &id    = config_.identity();
    const auto &hub   = config_.in_hub();
    const auto &tr    = config_.in_transport();
    const auto &shm   = config_.in_shm();
    const auto &tc    = config_.timing();
    inbox_cfg_ = config_.inbox();
    const auto &mon   = config_.monitoring();
    const auto &auth  = config_.auth();
    const auto &ch    = config_.in_channel();

    // --- Consumer options ---
    hub::ConsumerOptions opts;
    opts.channel_name         = ch;
    opts.shm_shared_secret    = shm.enabled ? shm.secret : 0u;
    opts.expected_schema_hash = hub::compute_schema_hash(in_slot_spec_, core_.in_fz_spec());
    opts.consumer_uid         = id.uid;
    opts.consumer_name        = id.name;

    // Queue abstraction: checksum policy.
    opts.checksum_policy    = config_.checksum().policy;
    opts.flexzone_checksum  = config_.checksum().flexzone && core_.has_in_fz();

    // Transport declaration.
    const bool is_zmq = (tr.transport == config::Transport::Zmq);
    opts.queue_type = is_zmq ? "zmq" : "shm";

    if (is_zmq)
    {
        opts.zmq_schema       = hub::schema_spec_to_zmq_fields(in_slot_spec_);
        opts.zmq_packing      = tr.zmq_packing;
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

    // --- Create consumer ---
    auto maybe_consumer = hub::Consumer::create(opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[cons] Failed to connect consumer to channel '{}'", ch);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    // Metrics reset moved to after queue creation (reset_metrics() on queue).

    // Broker notifications (band, hub-dead) are handled by BrokerRequestComm.

    // --- Start and configure data queue ---
    if (!in_consumer_->start_queue())
    {
        LOGGER_ERROR("[cons] start_queue() failed for channel '{}'", ch);
        return false;
    }
    in_consumer_->reset_queue_metrics();
    core_.set_configured_period(static_cast<uint64_t>(tc.period_us));

    LOGGER_INFO("[cons] Consumer started on channel '{}' (shm={})", ch,
                in_consumer_->has_shm());

    // Startup coordination (HEP-0023) moved to after start_ctrl_thread().
    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ConsumerRoleHost::teardown_infrastructure_()
{
    // Broker and comm threads already joined via api_->join_all_threads().

    core_.clear_inbox_cache();

    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    // Ctrl thread already joined by api_->join_all_threads().
    // Broker detects role death via heartbeat timeout — no explicit deregister needed.
    if (broker_comm_)
    {
        broker_comm_->disconnect();
        broker_comm_.reset();
    }

    if (in_consumer_.has_value())
    {
        in_consumer_->close();
        in_consumer_.reset();
    }
}


// ============================================================================
// snapshot_metrics_json
// ============================================================================

nlohmann::json ConsumerRoleHost::snapshot_metrics_json() const
{
    nlohmann::json result;

    if (in_consumer_.has_value())
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, in_consumer_->queue_metrics());
        result["queue"] = std::move(q);
    }

    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, core_.loop_metrics());
        result["loop"] = std::move(lm);
    }

    result["role"] = {
        {"in_slots_received",  core_.in_slots_received()},
        {"script_error_count", engine_ ? engine_->script_error_count() : 0},
        {"ctrl_queue_dropped", 0}
    };

    if (inbox_queue_)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, inbox_queue_->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    return result;
}

} // namespace pylabhub::consumer
