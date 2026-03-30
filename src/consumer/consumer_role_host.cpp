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

#include "role_host_helpers.hpp"
#include "zmq_poll_loop.hpp"
#include "script_host_helpers.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::consumer
{

using scripting::IncomingMessage;
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
// worker_main_ — the worker thread entry point
// ============================================================================

void ConsumerRoleHost::worker_main_()
{
    const auto &id   = config_.identity();
    const auto &sc   = config_.script();
    const auto &hub  = config_.in_hub();
    const auto &tr   = config_.in_transport();

    // Step 1: Initialize the engine.
    if (!engine_->initialize("cons", &core_))
    {
        LOGGER_ERROR("[cons] Engine initialize() failed");
        engine_->finalize();
        ready_promise_.set_value(false);
        return;
    }

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[cons] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    sc.type, sc.type);
    }

    // Step 2: Load script and extract callbacks.
    const std::filesystem::path base_path =
        sc.path.empty() ? std::filesystem::current_path()
                        : std::filesystem::weakly_canonical(sc.path);
    const std::filesystem::path script_dir = base_path / "script" / sc.type;
    const char *entry_point =
        (sc.type == "lua") ? "init.lua" : "__init__.py";

    if (!engine_->load_script(script_dir, entry_point, "on_consume"))
    {
        LOGGER_ERROR("[cons] Engine load_script() failed");
        core_.set_script_load_ok(false);
        engine_->finalize();
        ready_promise_.set_value(false);
        return;
    }
    core_.set_script_load_ok(true);

    // Step 3: Resolve schemas and register slot types (read-only access).
    scripting::SchemaSpec fz_spec_local;
    {
        std::vector<std::string> schema_dirs;
        if (!hub.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(hub.hub_dir) / "schemas").string());

        try
        {
            const auto &cf = config_.role_data<consumer::ConsumerFields>();
            slot_spec_ = scripting::resolve_schema(
                cf.in_slot_schema_json, false, "cons", schema_dirs);
            fz_spec_local = scripting::resolve_schema(
                cf.in_flexzone_schema_json, true, "cons", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[cons] Schema parse error: {}", e.what());
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
    }

    const std::string packing =
        tr.zmq_packing.empty() ? "aligned" : tr.zmq_packing;

    // Register slot type (read-only).
    if (slot_spec_.has_schema)
    {
        if (!engine_->register_slot_type(slot_spec_, "SlotFrame", packing))
        {
            LOGGER_ERROR("[cons] Failed to register SlotFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
        schema_slot_size_ = engine_->type_sizeof("SlotFrame");
    }

    // Register flexzone type.
    if (fz_spec_local.has_schema)
    {
        if (!engine_->register_slot_type(fz_spec_local, "FlexFrame", packing))
        {
            LOGGER_ERROR("[cons] Failed to register FlexFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
        size_t fz_size = engine_->type_sizeof("FlexFrame");
        fz_size = (fz_size + 4095U) & ~size_t{4095U};
        core_.set_fz_spec(std::move(fz_spec_local), fz_size);
    }
    else
    {
        core_.set_fz_spec(std::move(fz_spec_local), 0);
    }

    // Validate-only mode: print layout and exit.
    if (core_.is_validate_only())
    {
        // TODO: print layout via engine
        engine_->finalize();
        ready_promise_.set_value(true);
        return;
    }

    // Step 4: setup_infrastructure_
    if (!setup_infrastructure_())
    {
        engine_->finalize();
        teardown_infrastructure_();
        ready_promise_.set_value(false);
        return;
    }

    // Step 5: Build RoleContext and engine API.
    scripting::RoleContext ctx;
    ctx.role_tag    = "cons";
    ctx.uid         = id.uid;
    ctx.name        = id.name;
    ctx.channel     = config_.in_channel();
    ctx.log_level   = id.log_level;
    ctx.script_dir  = script_dir.string();
    ctx.role_dir    = config_.base_dir().string();
    ctx.messenger   = &in_messenger_;
    ctx.producer    = nullptr;
    ctx.consumer    = in_consumer_.has_value() ? &(*in_consumer_) : nullptr;
    ctx.inbox_queue = inbox_queue_.get();
    ctx.core         = &core_;
    ctx.stop_on_script_error = sc.stop_on_script_error;

    if (!engine_->build_api(ctx))
    {
        LOGGER_ERROR("[cons] build_api failed — aborting role start");
        engine_->finalize();
        teardown_infrastructure_();
        ready_promise_.set_value(false);
        return;
    }

    // Step 6: invoke on_init.
    engine_->invoke_on_init();

    // Step 7: Spawn ctrl_thread_ and signal ready.
    core_.set_running(true);
    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    // Step 8: Signal ready.
    ready_promise_.set_value(true);

    // Step 9: Run the data loop.
    run_data_loop_();

    // Step 10: stop accepting invoke from non-owner threads.
    engine_->stop_accepting();

    // Step 11: join ctrl_thread — ensure no non-owner thread is using the engine.
    core_.set_running(false);
    core_.notify_incoming();
    if (ctrl_thread_.joinable())
        ctrl_thread_.join();

    // Step 12: last script callback.
    engine_->invoke_on_stop();

    // Step 13: finalize engine.
    engine_->finalize();

    // Step 14: teardown infrastructure.
    teardown_infrastructure_();
}

// ============================================================================
// setup_infrastructure_ — connect to broker, create consumer, wire events
// ============================================================================

bool ConsumerRoleHost::setup_infrastructure_()
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
    opts.expected_schema_hash = scripting::compute_schema_hash(slot_spec_, core_.fz_spec());
    opts.consumer_uid         = id.uid;
    opts.consumer_name        = id.name;

    opts.timing = tc.timing_params();

    // Queue abstraction: sizes + checksum for internal queue creation.
    opts.item_size          = schema_slot_size_;
    opts.flexzone_size      = core_.schema_fz_size();
    opts.verify_checksum    = shm.verify_checksum;
    opts.verify_checksum_fz = core_.has_fz() && shm.verify_checksum;

    // Transport declaration.
    const bool is_zmq = (tr.transport == config::Transport::Zmq);
    opts.queue_type = is_zmq ? "zmq" : "shm";

    if (is_zmq)
    {
        opts.zmq_schema       = scripting::schema_spec_to_zmq_fields(slot_spec_, schema_slot_size_);
        opts.zmq_packing      = tr.zmq_packing;
        opts.zmq_buffer_depth = tr.zmq_buffer_depth;
    }

    opts.ctrl_queue_max_depth = mon.ctrl_queue_max_depth;
    opts.peer_dead_timeout_ms = mon.peer_dead_timeout_ms;

    // --- Inbox setup (optional) ---
    scripting::SchemaSpec inbox_spec;
    size_t inbox_schema_slot_size = 0;

    if (inbox.has_inbox())
    {
        std::vector<std::string> schema_dirs;
        if (!hub.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(hub.hub_dir) / "schemas").string());

        try
        {
            inbox_spec = scripting::resolve_schema(
                inbox.schema_json, false, "cons", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[cons] Inbox schema parse error: {}", e.what());
            return false;
        }

        const std::string inbox_packing =
            tr.zmq_packing.empty() ? "aligned" : tr.zmq_packing;

        if (inbox_spec.has_schema)
        {
            if (!engine_->register_slot_type(inbox_spec, "InboxFrame", inbox_packing))
            {
                LOGGER_ERROR("[cons] Failed to register InboxFrame type");
                return false;
            }
            inbox_schema_slot_size = engine_->type_sizeof("InboxFrame");
        }

        // Endpoint validated by parse_inbox_config(); default is tcp://127.0.0.1:0.
        const std::string &ep = inbox.endpoint;
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(inbox_spec, inbox_schema_slot_size);

        nlohmann::json spec_json;
        spec_json["fields"] = nlohmann::json::array();
        for (const auto &f : inbox_spec.fields)
        {
            nlohmann::json fj = {{"name", f.name}, {"type", f.type_str}};
            if (f.count > 1)  fj["count"]  = f.count;
            if (f.length > 0) fj["length"] = f.length;
            spec_json["fields"].push_back(fj);
        }
        if (inbox_spec.packing != "aligned")
            spec_json["packing"] = inbox_spec.packing;

        const int inbox_rcvhwm = (inbox.overflow_policy == "block")
            ? 0
            : static_cast<int>(inbox.buffer_depth);

        inbox_queue_ = hub::InboxQueue::bind_at(
            ep, std::move(zmq_fields), inbox_packing, inbox_rcvhwm);
        if (!inbox_queue_ || !inbox_queue_->start())
        {
            LOGGER_ERROR("[cons] Failed to start InboxQueue at '{}'", ep);
            inbox_queue_.reset();
            return false;
        }
        // Validate: engine type size must match queue decode buffer size.
        if (inbox_schema_slot_size > 0 &&
            inbox_queue_->item_size() != inbox_schema_slot_size)
        {
            LOGGER_ERROR("[cons] InboxFrame size mismatch: engine type_sizeof={} "
                         "but InboxQueue item_size={} (packing='{}') — "
                         "check schema/packing consistency",
                         inbox_schema_slot_size, inbox_queue_->item_size(),
                         inbox_packing);
            inbox_queue_.reset();
            return false;
        }
        LOGGER_INFO("[cons] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());
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

    // --- Wire event callbacks → IncomingMessage queue ---
    in_consumer_->on_channel_closing([this]() {
        LOGGER_INFO("[cons] CHANNEL_CLOSING_NOTIFY received, queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        core_.enqueue_message(std::move(msg));
    });

    in_consumer_->on_force_shutdown([this]() {
        LOGGER_WARN("[cons] FORCE_SHUTDOWN received, forcing immediate shutdown");
        core_.request_stop();
    });

    // ZMQ data routing for SHM transport: ZMQ frames → message queue.
    if (!is_zmq)
    {
        in_consumer_->on_zmq_data(
            [this](std::span<const std::byte> data)
            {
                LOGGER_DEBUG("[cons] zmq_data: data_message size={}", data.size());
                IncomingMessage msg;
                msg.data.assign(data.begin(), data.end());
                core_.enqueue_message(std::move(msg));
            });
    }

    in_consumer_->on_producer_message(
        [this](std::string_view type, std::span<const std::byte> data)
        {
            LOGGER_INFO("[cons] ctrl_msg: producer_message type='{}' size={}",
                        type, data.size());
            IncomingMessage msg;
            msg.event = "producer_message";
            msg.details["type"] = std::string(type);
            msg.details["data"] = format_tools::bytes_to_hex(
                {reinterpret_cast<const char *>(data.data()), data.size()});
            core_.enqueue_message(std::move(msg));
        });

    in_consumer_->on_channel_error(
        [this](const std::string &event, const nlohmann::json &details)
        {
            LOGGER_INFO("[cons] broker_notify: channel_event event='{}' details={}",
                        event, details.dump());
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = details;
            msg.details["detail"] = event;
            core_.enqueue_message(std::move(msg));
        });

    if (!in_consumer_->start_embedded())
    {
        LOGGER_ERROR("[cons] in_consumer->start_embedded() failed");
        return false;
    }

    // --- Wire peer-dead and hub-dead monitoring ---
    in_consumer_->on_peer_dead([this, &mon]() {
        LOGGER_WARN("[cons] peer-dead: producer silent for {} ms; triggering shutdown",
                    mon.peer_dead_timeout_ms);
        core_.set_stop_reason(scripting::RoleHostCore::StopReason::PeerDead);
        core_.request_stop();
    });

    in_messenger_.on_hub_dead([this]() {
        LOGGER_WARN("[cons] hub-dead: broker connection lost; triggering shutdown");
        core_.set_stop_reason(scripting::RoleHostCore::StopReason::HubDead);
        core_.request_stop();
    });

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
// run_data_loop_ — THE UNIFIED LOOP (tech draft §4)
// ============================================================================

void ConsumerRoleHost::run_data_loop_()
{
    if (!in_consumer_.has_value())
    {
        LOGGER_ERROR("[cons] run_data_loop_: QueueReader not initialized — aborting");
        core_.set_running(false);
        return;
    }

    const auto &tc  = config_.timing();
    const auto &sc  = config_.script();

    const double period_us = tc.period_us;
    const bool is_max_rate = (tc.loop_timing == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, tc.queue_io_wait_timeout_ratio);
    const auto short_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(short_timeout_us + std::chrono::microseconds{999});
    const size_t item_sz = in_consumer_->queue_item_size();

    const char *fz_type = core_.has_fz() ? "FlexFrame" : nullptr;

    auto deadline = Clock::time_point::max();

    while (core_.is_running() &&
           !core_.is_shutdown_requested() &&
           !core_.is_critical_error())
    {
        if (core_.is_process_exit_requested())
            break;

        const auto cycle_start = Clock::now();

        // --- Step A: Acquire data with inner retry ---
        const void *data = nullptr;
        while (true)
        {
            data = in_consumer_->read_acquire(short_timeout);
            if (data != nullptr)
                break;

            if (is_max_rate)
                break;

            if (!core_.is_running() ||
                core_.is_shutdown_requested() ||
                core_.is_critical_error())
                break;
            if (core_.is_process_exit_requested())
                break;

            if (deadline != Clock::time_point::max())
            {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        deadline - Clock::now());
                if (remaining <= short_timeout_us)
                    break;
            }
        }

        // --- Step B: Deadline wait ---
        if (!is_max_rate && data != nullptr &&
            deadline != Clock::time_point::max() && Clock::now() < deadline)
        {
            std::this_thread::sleep_until(deadline);
        }

        if (!core_.is_running() ||
            core_.is_shutdown_requested() ||
            core_.is_critical_error())
        {
            if (data != nullptr)
                in_consumer_->read_release();
            break;
        }

        // --- Step C: Drain ---
        auto msgs = core_.drain_messages();
        drain_inbox_sync_();

        // --- Step D: Invoke callback ---
        if (data != nullptr)
        {
            last_seq_.store(in_consumer_->last_seq(), std::memory_order_relaxed);
            core_.inc_in_received();
        }

        const void *fz_ptr = core_.has_fz() ? in_consumer_->read_flexzone() : nullptr;
        const size_t fz_sz = core_.has_fz() ? in_consumer_->flexzone_size() : 0;

        const uint64_t errors_before = engine_->script_error_count();

        engine_->invoke_consume(data, item_sz, fz_ptr, fz_sz, fz_type, msgs);

        // --- Step E: Release slot ---
        if (data != nullptr)
            in_consumer_->read_release();

        if (sc.stop_on_script_error &&
            engine_->script_error_count() > errors_before)
        {
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

    LOGGER_INFO("[cons] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.is_running(), core_.is_shutdown_requested(),
                core_.is_critical_error());
}

// ============================================================================
// run_ctrl_thread_
// ============================================================================

void ConsumerRoleHost::run_ctrl_thread_()
{
    const auto &id = config_.identity();
    const auto &ch = config_.in_channel();
    const auto &tc = config_.timing();

    scripting::ZmqPollLoop loop{core_, "cons:" + id.uid};
    loop.sockets = {
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
        {in_consumer_->data_zmq_socket_handle(),
         [&] { in_consumer_->handle_data_events_nowait(); }},
    };
    loop.get_iteration = [&] {
        return core_.iteration_count();
    };
    loop.periodic_tasks.emplace_back(
        [&] {
            in_messenger_.enqueue_heartbeat(ch);
        },
        tc.heartbeat_interval_ms);
    loop.periodic_tasks.emplace_back(
        [&] {
            in_messenger_.enqueue_metrics_report(
                ch, id.uid, snapshot_metrics_json());
        },
        tc.heartbeat_interval_ms);
    loop.run();
}

// ============================================================================
// drain_inbox_sync_
// ============================================================================

void ConsumerRoleHost::drain_inbox_sync_()
{
    scripting::drain_inbox_sync(inbox_queue_.get(), engine_.get());
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
        {"in_received",        core_.in_received()},
        {"script_errors",      engine_ ? engine_->script_error_count() : 0},
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
