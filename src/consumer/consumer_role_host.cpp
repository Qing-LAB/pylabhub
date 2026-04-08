/**
 * @file consumer_role_host.cpp
 * @brief ConsumerRoleHost — unified engine-agnostic consumer implementation.
 *
 * This is the canonical data loop for the consumer role.  It follows
 * docs/tech_draft/loop_design_unified.md §4 exactly.
 *
 * Layer 3 (infrastructure): Messenger, Consumer, queue, ctrl_thread_, events.
 * Layer 2 (data loop): inner retry acquire, read, invoke, release.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_consume / invoke_on_inbox.
 */
#include "consumer_role_host.hpp"
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

    worker_thread_ = std::thread([this] { worker_main_(); });

    const bool ok = ready_future.get();
    if (!ok)
    {
        if (worker_thread_.joinable())
            worker_thread_.join();
    }
}

// ============================================================================
// shutdown_ — signal shutdown, join worker thread
// ============================================================================

void ConsumerRoleHost::shutdown_()
{
    core_.request_stop();
    core_.notify_incoming();

    if (worker_thread_.joinable())
        worker_thread_.join();
}

// ============================================================================
// ConsumerCycleOps — role-specific acquire/invoke/commit for the shared frame
// ============================================================================

namespace
{

class ConsumerCycleOps final : public scripting::RoleCycleOps
{
    hub::Consumer           &consumer_;
    scripting::ScriptEngine &engine_;
    scripting::RoleHostCore &core_;
    bool                     stop_on_error_;

    size_t       item_sz_;
    const void  *data_{nullptr};

  public:
    ConsumerCycleOps(hub::Consumer &c, scripting::ScriptEngine &e,
                     scripting::RoleHostCore &core, bool stop_on_error)
        : consumer_(c), engine_(e), core_(core),
          stop_on_error_(stop_on_error),
          item_sz_(c.queue_item_size())
    {}

    bool acquire(const scripting::AcquireContext &ctx) override
    {
        data_ = scripting::retry_acquire(ctx, core_,
            [this](auto t) { return const_cast<void *>(consumer_.read_acquire(t)); });
        return data_ != nullptr;
    }

    void cleanup_on_shutdown() override
    {
        if (data_) { consumer_.read_release(); data_ = nullptr; }
    }

    bool invoke_and_commit(std::vector<scripting::IncomingMessage> &msgs) override
    {

        if (data_)
            core_.inc_in_slots_received();

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        void *fz_ptr = core_.has_in_fz()
            ? const_cast<void *>(consumer_.read_flexzone()) : nullptr;
        const size_t fz_sz = core_.has_in_fz() ? consumer_.flexzone_size() : 0;

        const uint64_t errors_before = engine_.script_error_count();

        engine_.invoke_consume(
            scripting::InvokeRx{data_, item_sz_, fz_ptr, fz_sz}, msgs);

        if (data_) { consumer_.read_release(); data_ = nullptr; }

        if (stop_on_error_ && engine_.script_error_count() > errors_before)
        {
            core_.request_stop();
            return false;
        }
        return true;
    }

    void cleanup_on_exit() override {} // nothing held across cycles
};

} // anonymous namespace

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

    // ── Step 3: Create RoleAPIBase and wire infrastructure ───────────────────

    api_ = std::make_unique<scripting::RoleAPIBase>(core_);
    api_->set_role_tag("cons");
    api_->set_uid(id.uid);
    api_->set_name(id.name);
    api_->set_channel(config_.in_channel());
    api_->set_log_level(id.log_level);
    api_->set_script_dir(script_dir.string());
    api_->set_role_dir(config_.base_dir().string());
    if (!core_.is_validate_only())
    {
        api_->set_messenger(&in_messenger_);
        api_->set_consumer(in_consumer_.has_value() ? &(*in_consumer_) : nullptr);
        api_->set_inbox_queue(inbox_queue_.get());
    }
    api_->set_checksum_policy(config_.checksum().policy);
    api_->set_stop_on_script_error(sc.stop_on_script_error);
    api_->set_engine(engine_.get());
    api_->wire_event_callbacks();

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

    // Step 6: Spawn ctrl thread via thread manager.
    core_.set_running(true);
    api_->start_ctrl_thread(
        scripting::RoleAPIBase::CtrlConfig{config_.timing().heartbeat_interval_ms, true});

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
        ConsumerCycleOps ops(*in_consumer_, *engine_, core_,
                             sc.stop_on_script_error);
        scripting::LoopConfig lcfg;
        lcfg.period_us                   = tc_loop.period_us;
        lcfg.loop_timing                 = tc_loop.loop_timing;
        lcfg.queue_io_wait_timeout_ratio = tc_loop.queue_io_wait_timeout_ratio;
        api_->run_data_loop(lcfg, ops);
    }

    // Step 9: stop accepting invoke from non-owner threads.
    engine_->stop_accepting();

    // Step 10: join all managed threads.
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
    const auto &inbox = config_.inbox();
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

    opts.ctrl_queue_max_depth = mon.ctrl_queue_max_depth;
    opts.peer_dead_timeout_ms = mon.peer_dead_timeout_ms;

    // --- Inbox setup (optional) ---
    if (inbox.has_inbox())
    {
        auto inbox_result = scripting::setup_inbox_facility(
            inbox_spec, inbox, config_.checksum().policy, "cons");
        if (!inbox_result)
            return false;
        inbox_queue_           = std::move(inbox_result->queue);
        opts.inbox_endpoint    = inbox_result->actual_endpoint;
        opts.inbox_schema_json = inbox_result->schema_json;
        opts.inbox_packing     = inbox_result->packing;
        opts.inbox_checksum    = inbox_result->checksum;
    }

    // --- Broker connect ---
    if (!hub.broker.empty())
    {
        if (!in_messenger_.connect(hub.broker, hub.broker_pubkey,
                                    auth.client_pubkey, auth.client_seckey))
        {
            LOGGER_ERROR("[cons] broker connect failed ({}); aborting", hub.broker);
            return false;
        }
    }

    // --- Create consumer ---
    auto maybe_consumer = hub::Consumer::connect(in_messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[cons] Failed to connect consumer to channel '{}'", ch);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    // Metrics reset moved to after queue creation (reset_metrics() on queue).

    if (!in_consumer_->start_embedded())
    {
        LOGGER_ERROR("[cons] in_consumer->start_embedded() failed");
        return false;
    }

    // Event callbacks (on_channel_closing, on_force_shutdown, on_peer_dead,
    // on_hub_dead, on_zmq_data, on_producer_message, on_channel_error) are
    // wired by api_->wire_event_callbacks() after RoleAPIBase construction.

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

    // --- Startup coordination (HEP-0023) ---
    if (!scripting::wait_for_roles(in_messenger_, config_.startup().wait_for_roles, "[cons]"))
        return false;

    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ConsumerRoleHost::teardown_infrastructure_()
{
    // ctrl_thread_ already joined before finalize (shutdown sequence).

    core_.clear_inbox_cache();

    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    in_messenger_.on_hub_dead(nullptr);

    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
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
        {"ctrl_queue_dropped", in_consumer_.has_value() ? in_consumer_->ctrl_queue_dropped() : 0}
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
