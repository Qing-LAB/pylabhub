/**
 * @file producer_role_host.cpp
 * @brief ProducerRoleHost — unified engine-agnostic producer implementation.
 *
 * This is the canonical data loop for the producer role.  It follows
 * docs/tech_draft/loop_design_unified.md §3 exactly.
 *
 * Layer 3 (infrastructure): Messenger, Producer, queue, ctrl_thread_, events.
 * Layer 2 (data loop): inner retry acquire, deadline wait, drain, invoke, commit.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_produce / invoke_on_inbox.
 */
#include "producer_role_host.hpp"
#include "producer_fields.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "engine_module_params.hpp"
#include "role_host_helpers.hpp"
#include "zmq_poll_loop.hpp"
#include "utils/schema_utils.hpp"
#include "utils/lifecycle.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::producer
{

using scripting::IncomingMessage;
using scripting::InvokeResult;
using scripting::InvokeTx;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Constructor / Destructor
// ============================================================================

ProducerRoleHost::ProducerRoleHost(config::RoleConfig config,
                                     std::unique_ptr<scripting::ScriptEngine> engine,
                                     std::atomic<bool> *shutdown_flag)
    : config_(std::move(config))
    , engine_(std::move(engine))
{
    core_.set_shutdown_flag(shutdown_flag);
}

ProducerRoleHost::~ProducerRoleHost()
{
    shutdown_();
}

// ============================================================================
// startup_ — spawn worker thread, block until ready or failure
// ============================================================================

void ProducerRoleHost::startup_()
{
    ready_promise_ = std::promise<bool>{};
    auto ready_future = ready_promise_.get_future();

    worker_thread_ = std::thread([this] { worker_main_(); });

    const bool ok = ready_future.get();
    if (!ok)
    {
        // Worker failed during setup — join immediately.
        if (worker_thread_.joinable())
            worker_thread_.join();
    }
}

// ============================================================================
// shutdown_ — signal shutdown, join worker thread
// ============================================================================

void ProducerRoleHost::shutdown_()
{
    core_.request_stop();
    core_.notify_incoming();

    if (worker_thread_.joinable())
        worker_thread_.join();
}

// ============================================================================
// worker_main_ — the worker thread entry point
// ============================================================================

void ProducerRoleHost::worker_main_()
{
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

    const std::string packing =
        tr.zmq_packing.empty() ? "aligned" : tr.zmq_packing;

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
            ready_promise_.set_value(false);
            return;
        }
    }

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
            ready_promise_.set_value(false);
            return;
        }
    }

    // ── Step 3: Create RoleAPIBase and wire infrastructure ───────────────────

    api_ = std::make_unique<scripting::RoleAPIBase>(core_);
    api_->set_role_tag("prod");
    api_->set_uid(id.uid);
    api_->set_name(id.name);
    api_->set_channel(config_.out_channel());
    api_->set_log_level(id.log_level);
    api_->set_script_dir(script_dir.string());
    api_->set_role_dir(config_.base_dir().string());
    if (!core_.is_validate_only())
    {
        api_->set_messenger(&out_messenger_);
        api_->set_producer(out_producer_.has_value() ? &(*out_producer_) : nullptr);
        api_->set_inbox_queue(inbox_queue_.get());
    }
    api_->set_checksum_policy(config_.checksum().policy);
    api_->set_stop_on_script_error(sc.stop_on_script_error);

    // ── Step 4: Load engine via lifecycle module ─────────────────────────────
    // engine_lifecycle_startup does: initialize → load_script → register_slot_type
    // (all directions + inbox) → build_api. Single call replaces manual steps.

    engine_module_name_ = fmt::format("ScriptEngine:{}:{}", sc.type, id.uid);

    scripting::EngineModuleParams params;
    params.engine             = engine_.get();
    params.api                = api_.get();
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
    if (out_producer_.has_value() && core_.has_out_fz())
        out_producer_->sync_flexzone_checksum();

    // Step 6: Spawn ctrl_thread_ and signal ready.
    core_.set_running(true);
    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    // Step 7: Signal ready.
    ready_promise_.set_value(true);

    // Step 8: Run the data loop.
    run_data_loop_();

    // Step 9: stop accepting invoke from non-owner threads.
    engine_->stop_accepting();

    // Step 10: join ctrl_thread — ensure no non-owner thread is using the engine.
    core_.set_running(false);
    core_.notify_incoming();
    if (ctrl_thread_.joinable())
        ctrl_thread_.join();

    // Step 11: last script callback (no other threads using engine).
    engine_->invoke_on_stop();

    // Step 12: finalize engine.
    engine_->finalize();

    // Step 13: teardown infrastructure.
    teardown_infrastructure_();
}

// ============================================================================
// setup_infrastructure_ — connect to broker, create producer, wire events
// ============================================================================

bool ProducerRoleHost::setup_infrastructure_(const hub::SchemaSpec &inbox_spec)
{
    const auto &id    = config_.identity();
    const auto &hub   = config_.out_hub();
    const auto &tr    = config_.out_transport();
    const auto &shm   = config_.out_shm();
    const auto &tc    = config_.timing();
    const auto &inbox = config_.inbox();
    const auto &mon   = config_.monitoring();
    const auto &auth  = config_.auth();
    const auto &ch    = config_.out_channel();

    // --- Producer options ---
    hub::ProducerOptions opts;
    opts.channel_name = ch;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = shm.enabled;
    opts.schema_hash  = hub::compute_schema_hash(out_slot_spec_, core_.out_fz_spec());
    opts.role_name   = id.name;
    opts.role_uid    = id.uid;


    opts.ctrl_queue_max_depth = mon.ctrl_queue_max_depth;
    opts.peer_dead_timeout_ms = mon.peer_dead_timeout_ms;

    // --- Inbox setup (optional) ---
    if (inbox.has_inbox())
    {
        auto inbox_result = scripting::setup_inbox_facility(
            inbox_spec, inbox, config_.checksum().policy, "prod");
        if (!inbox_result)
            return false;
        inbox_queue_           = std::move(inbox_result->queue);
        opts.inbox_endpoint    = inbox_result->actual_endpoint;
        opts.inbox_schema_json = inbox_result->schema_json;
        opts.inbox_packing     = inbox_result->packing;
        opts.inbox_checksum    = inbox_result->checksum;
    }

    // --- Queue abstraction: checksum policy ---
    opts.checksum_policy    = config_.checksum().policy;
    opts.flexzone_checksum  = config_.checksum().flexzone && core_.has_out_fz();
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
        opts.zmq_schema        = hub::schema_spec_to_zmq_fields(out_slot_spec_);
        opts.zmq_packing       = tr.zmq_packing;
        opts.zmq_buffer_depth  = tr.zmq_buffer_depth;
        opts.zmq_overflow_policy =
            (tr.zmq_overflow_policy == "block") ? hub::OverflowPolicy::Block
                                                : hub::OverflowPolicy::Drop;
    }

    // --- Broker connect ---
    if (!hub.broker.empty())
    {
        if (!out_messenger_.connect(hub.broker, hub.broker_pubkey,
                                    auth.client_pubkey, auth.client_seckey))
        {
            LOGGER_ERROR("[prod] broker connect failed ({}); aborting", hub.broker);
            return false;
        }
    }

    // --- Create producer ---
    auto maybe_producer = hub::Producer::create(out_messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[prod] Failed to create producer for channel '{}'", ch);
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    // Metrics reset moved to after queue creation (reset_metrics() on queue).

    if (!ch.empty())
    {
        out_messenger_.suppress_periodic_heartbeat(ch);
        out_messenger_.enqueue_heartbeat(ch);
    }

    // --- Wire event callbacks → IncomingMessage queue ---
    out_producer_->on_channel_closing([this]() {
        IncomingMessage msg;
        msg.event = "channel_closing";
        core_.enqueue_message(std::move(msg));
    });

    out_producer_->on_force_shutdown([this]() {
        core_.request_stop();
    });

    auto hex_identity = [](const std::string &raw) -> std::string {
        return format_tools::bytes_to_hex(raw);
    };

    out_producer_->on_consumer_joined(
        [this, hex_identity](const std::string &identity) {
            LOGGER_INFO("[prod] peer_event: consumer_joined identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_joined";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_left(
        [this, hex_identity](const std::string &identity) {
            LOGGER_INFO("[prod] peer_event: consumer_left identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_left";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_consumer_died(ch,
        [this](uint64_t pid, std::string reason) {
            LOGGER_INFO("[prod] broker_notify: consumer_died pid={} reason={}",
                        pid, reason);
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(ch,
        [this](std::string event, nlohmann::json details) {
            LOGGER_INFO("[prod] broker_notify: channel_event event='{}' details={}",
                        event, details.dump());
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = std::move(details);
            msg.details["detail"] = std::move(event);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data) {
            LOGGER_INFO("[prod] zmq_data: consumer_message size={}", data.size());
            IncomingMessage msg;
            msg.sender = identity;
            msg.data.assign(data.begin(), data.end());
            core_.enqueue_message(std::move(msg));
        });

    if (!out_producer_->start_embedded())
    {
        LOGGER_ERROR("[prod] out_producer->start_embedded() failed");
        return false;
    }

    // --- Wire peer-dead and hub-dead monitoring ---
    out_producer_->on_peer_dead([this]() {
        core_.set_stop_reason(scripting::RoleHostCore::StopReason::PeerDead);
        core_.request_stop();
    });

    out_messenger_.on_hub_dead([this]() {
        core_.set_stop_reason(scripting::RoleHostCore::StopReason::HubDead);
        core_.request_stop();
    });

    // --- Start and configure data queue ---
    if (!out_producer_->start_queue())
    {
        LOGGER_ERROR("[prod] start_queue() failed for channel '{}'", ch);
        return false;
    }
    out_producer_->reset_queue_metrics();
    core_.set_configured_period(static_cast<uint64_t>(tc.period_us));

    LOGGER_INFO("[prod] Producer started on channel '{}' (shm={})", ch,
                out_producer_->has_shm());

    // --- Startup coordination (HEP-0023) ---
    if (!scripting::wait_for_roles(out_messenger_, config_.startup().wait_for_roles, "[prod]"))
        return false;

    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ProducerRoleHost::teardown_infrastructure_()
{
    // ctrl_thread_ already joined before finalize (shutdown sequence).
    // set_running(false) also already called. Defensive re-set is safe.

    // Clean up shared resources (engine already finalized — no scripts running).
    core_.clear_inbox_cache();

    // Stop inbox_queue_ (if exists).
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    // Deregister hub-dead callback.
    out_messenger_.on_hub_dead(nullptr);

    // Stop/close producer.
    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
}

// ============================================================================
// run_data_loop_ — THE UNIFIED LOOP (tech draft §3)
// ============================================================================

void ProducerRoleHost::run_data_loop_()
{
    if (!out_producer_.has_value())
    {
        LOGGER_ERROR("[prod] run_data_loop_: producer not initialized — aborting");
        core_.set_running(false);
        return;
    }

    const auto &tc = config_.timing();
    const auto &sc = config_.script();

    // --- Setup ---
    const double period_us = tc.period_us;
    const bool is_max_rate = (tc.loop_timing == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, tc.queue_io_wait_timeout_ratio);
    // write_acquire takes milliseconds; convert with rounding up to avoid 0ms.
    const auto short_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(short_timeout_us + std::chrono::microseconds{999});
    const size_t buf_sz = out_producer_->queue_item_size();

    // Flexzone pointers — valid after queue start.
    void       *fz_ptr  = core_.has_out_fz() ? out_producer_->write_flexzone() : nullptr;
    const size_t fz_sz  = core_.has_out_fz() ? out_producer_->flexzone_size()  : 0;


    // First cycle: no deadline — fire immediately.
    auto deadline = Clock::time_point::max();

    // --- Outer loop ---
    while (core_.is_running() &&
           !core_.is_shutdown_requested() &&
           !core_.is_critical_error())
    {
        // Check external shutdown flag.
        if (core_.is_process_exit_requested())
            break;

        const auto cycle_start = Clock::now();

        // --- Step A: Acquire data with inner retry ---
        void *buf = nullptr;
        while (true)
        {
            buf = out_producer_->write_acquire(short_timeout);
            if (buf != nullptr)
                break; // got slot

            if (is_max_rate)
                break; // MaxRate: single attempt

            // Check shutdown between retries.
            if (!core_.is_running() ||
                core_.is_shutdown_requested() ||
                core_.is_critical_error())
            {
                break;
            }
            if (core_.is_process_exit_requested())
                break;

            // For first cycle (deadline=max), remaining is effectively infinite — always retry.
            if (deadline != Clock::time_point::max())
            {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        deadline - Clock::now());
                if (remaining <= short_timeout_us)
                    break; // not enough time to retry
            }
            // else: retry acquire
        }

        // --- Step B: Deadline wait (FixedRate with early data) ---
        // Skip sleep when deadline is max() (first cycle) or MaxRate.
        if (!is_max_rate && buf != nullptr &&
            deadline != Clock::time_point::max() && Clock::now() < deadline)
        {
            std::this_thread::sleep_until(deadline);
        }

        // Safety check after potential sleep: shutdown may have been requested.
        if (!core_.is_running() ||
            core_.is_shutdown_requested() ||
            core_.is_critical_error())
        {
            if (buf != nullptr)
                out_producer_->write_discard();
            break;
        }

        // --- Step C: Drain everything right before script call ---
        auto msgs = core_.drain_messages();
        drain_inbox_sync_();

        // --- Step D: Prepare and invoke callback ---
        if (buf != nullptr)
            std::memset(buf, 0, buf_sz);

        // Re-read flexzone pointer each cycle (ShmQueue may move it).
        if (core_.has_out_fz())
            fz_ptr = out_producer_->write_flexzone();

        InvokeResult result =
            engine_->invoke_produce(InvokeTx{buf, buf_sz, fz_ptr, fz_sz}, msgs);

        // --- Step E: Commit/discard ---
        if (buf != nullptr)
        {
            if (result == InvokeResult::Commit)
            {
                out_producer_->write_commit();
                core_.inc_out_slots_written();
            }
            else
            {
                out_producer_->write_discard();
                core_.inc_out_drop_count();
            }
        }
        else
        {
            core_.inc_out_drop_count();
        }

        if (result == InvokeResult::Error)
        {
            // script_errors already incremented by engine.
            if (sc.stop_on_script_error)
                core_.request_stop();
        }

        // --- Step F: Metrics ---
        const auto now     = Clock::now();
        const auto work_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cycle_start).count());
        core_.set_last_cycle_work_us(work_us);
        core_.inc_iteration_count();
        if (deadline != Clock::time_point::max() && now > deadline)
            core_.inc_loop_overrun();

        // --- Step G: Compute next deadline ---
        deadline = compute_next_deadline(tc.loop_timing, deadline, cycle_start, period_us);
    }

    LOGGER_INFO("[prod] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.is_running(), core_.is_shutdown_requested(),
                core_.is_critical_error());
}

// ============================================================================
// run_ctrl_thread_ — polls producer peer socket, sends heartbeats
// ============================================================================

void ProducerRoleHost::run_ctrl_thread_()
{
    scripting::ThreadEngineGuard engine_guard(*engine_);

    const auto &id = config_.identity();
    const auto &ch = config_.out_channel();
    const auto &tc = config_.timing();

    const bool has_heartbeat_cb = engine_->has_callback("on_heartbeat");

    scripting::ZmqPollLoop loop{core_, "prod:" + id.uid};
    loop.sockets = {
        {out_producer_->peer_ctrl_socket_handle(),
         [&] { out_producer_->handle_peer_events_nowait(); }},
    };
    loop.get_iteration = [&] {
        return core_.iteration_count();
    };
    loop.periodic_tasks.emplace_back(
        [&] {
            out_messenger_.enqueue_heartbeat(ch, snapshot_metrics_json());
            if (has_heartbeat_cb)
                engine_->invoke("on_heartbeat");
        },
        tc.heartbeat_interval_ms);
    loop.run();
}

// ============================================================================
// drain_inbox_sync_ — drain all inbox messages non-blocking
// ============================================================================

void ProducerRoleHost::drain_inbox_sync_()
{
    scripting::drain_inbox_sync(inbox_queue_.get(), engine_.get());
}

// ============================================================================
// snapshot_metrics_json — for heartbeat reporting
// ============================================================================

nlohmann::json ProducerRoleHost::snapshot_metrics_json() const
{
    nlohmann::json result;

    if (out_producer_.has_value())
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, out_producer_->queue_metrics());
        result["queue"] = std::move(q);
    }

    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, core_.loop_metrics());
        result["loop"] = std::move(lm);
    }

    result["role"] = {
        {"out_slots_written",  core_.out_slots_written()},
        {"out_drop_count",     core_.out_drop_count()},
        {"script_error_count", engine_ ? engine_->script_error_count() : 0},
        {"ctrl_queue_dropped", out_producer_.has_value() ? out_producer_->ctrl_queue_dropped() : 0}
    };

    if (inbox_queue_)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, inbox_queue_->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    return result;
}

} // namespace pylabhub::producer
