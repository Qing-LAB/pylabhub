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

#include "role_host_helpers.hpp"
#include "zmq_poll_loop.hpp"
#include "script_host_helpers.hpp" // resolve_schema, schema_spec_to_zmq_fields, compute_schema_hash

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace pylabhub::producer
{

using scripting::IncomingMessage;
using scripting::InvokeResult;
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
    const auto &tc   = config_.timing();
    const auto &hub  = config_.out_hub();
    const auto &tr   = config_.out_transport();
    const auto &shm  = config_.out_shm();
    const auto &inbox = config_.inbox();
    const auto &pf   = config_.role_data<ProducerFields>();

    // Step 1: Initialize the engine.
    if (!engine_->initialize("prod", &core_))
    {
        LOGGER_ERROR("[prod] Engine initialize() failed");
        engine_->finalize(); // Clean up any partial state (e.g. live interpreter).
        ready_promise_.set_value(false);
        return;
    }

    // Warn if script type was not explicitly set in config.
    if (!sc.type_explicit)
    {
        LOGGER_WARN("[prod] 'script.type' not set in config — defaulting to '{}'. "
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

    if (!engine_->load_script(script_dir, entry_point, "on_produce"))
    {
        LOGGER_ERROR("[prod] Engine load_script() failed");
        core_.set_script_load_ok(false);
        engine_->finalize();
        ready_promise_.set_value(false);
        return;
    }
    core_.set_script_load_ok(true);

    // Step 3: Resolve schemas and register slot types.
    scripting::SchemaSpec fz_spec_local;
    {
        std::vector<std::string> schema_dirs;
        if (!hub.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(hub.hub_dir) / "schemas").string());

        try
        {
            slot_spec_ = scripting::resolve_schema(
                pf.out_slot_schema_json, false, "prod", schema_dirs);
            fz_spec_local = scripting::resolve_schema(
                pf.out_flexzone_schema_json, true, "prod", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[prod] Schema parse error: {}", e.what());
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
    }

    const std::string packing =
        tr.zmq_packing.empty() ? "aligned" : tr.zmq_packing;

    // Register slot type.
    if (slot_spec_.has_schema)
    {
        if (!engine_->register_slot_type(slot_spec_, "SlotFrame", packing))
        {
            LOGGER_ERROR("[prod] Failed to register SlotFrame type");
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
            LOGGER_ERROR("[prod] Failed to register FlexFrame type");
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
    ctx.role_tag    = "prod";
    ctx.uid         = id.uid;
    ctx.name        = id.name;
    ctx.channel     = config_.out_channel();
    ctx.log_level   = id.log_level;
    ctx.script_dir  = script_dir.string();
    ctx.role_dir    = config_.base_dir().string();
    ctx.messenger   = &out_messenger_;
    ctx.queue_writer = queue_.get();
    ctx.queue_reader = nullptr;
    ctx.producer    = out_producer_.has_value() ? &(*out_producer_) : nullptr;
    ctx.consumer    = nullptr;
    ctx.core         = &core_;
    ctx.stop_on_script_error = sc.stop_on_script_error;

    if (!engine_->build_api(ctx))
    {
        LOGGER_ERROR("[prod] build_api failed — aborting role start");
        engine_->finalize();
        teardown_infrastructure_();
        ready_promise_.set_value(false);
        return;
    }

    // Step 6: invoke on_init.
    engine_->invoke_on_init();

    // Sync flexzone checksum after on_init (user may have written to flexzone).
    if (queue_ && core_.has_fz())
        queue_->sync_flexzone_checksum();

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

    // Step 12: last script callback (no other threads using engine).
    engine_->invoke_on_stop();

    // Step 13: finalize engine — destroy child states, close interpreter.
    engine_->finalize();

    // Step 14: teardown infrastructure — retract resources.
    teardown_infrastructure_();
}

// ============================================================================
// setup_infrastructure_ — connect to broker, create producer, wire events
// ============================================================================

bool ProducerRoleHost::setup_infrastructure_()
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
    opts.schema_hash  = scripting::compute_schema_hash(slot_spec_, core_.fz_spec());
    opts.role_name   = id.name;
    opts.role_uid    = id.uid;

    if (tc.period_us > 0.0)
    {
        opts.loop_policy          = hub::LoopPolicy::FixedRate;
        opts.configured_period_us = std::chrono::microseconds{static_cast<int64_t>(tc.period_us)};
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
                inbox.schema_json, false, "prod", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[prod] Inbox schema parse error: {}", e.what());
            return false;
        }

        const std::string inbox_packing =
            tr.zmq_packing.empty() ? "aligned" : tr.zmq_packing;

        // Register inbox type in the engine.
        if (inbox_spec.has_schema)
        {
            if (!engine_->register_slot_type(inbox_spec, "InboxFrame", inbox_packing))
            {
                LOGGER_ERROR("[prod] Failed to register InboxFrame type");
                return false;
            }
            inbox_schema_slot_size = engine_->type_sizeof("InboxFrame");
        }

        const std::string ep = inbox.endpoint.empty()
            ? "tcp://127.0.0.1:0"
            : inbox.endpoint;
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(inbox_spec, inbox_schema_slot_size);

        // Serialize full SchemaSpec JSON for ROLE_INFO_REQ discovery.
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
            LOGGER_ERROR("[prod] Failed to start InboxQueue for channel '{}'", ch);
            if (inbox_queue_)
                inbox_queue_.reset();
            return false;
        }
        opts.inbox_endpoint    = inbox_queue_->actual_endpoint();
        opts.inbox_schema_json = spec_json.dump();
        opts.inbox_packing     = inbox_packing;
    }

    // --- SHM config ---
    if (shm.enabled)
    {
        opts.shm_config.shared_secret        = shm.secret;
        opts.shm_config.ring_buffer_capacity = shm.slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = shm.sync_policy;
        opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        opts.shm_config.flex_zone_size       = core_.schema_fz_size();

        opts.shm_config.physical_page_size = hub::system_page_size();
        opts.shm_config.logical_unit_size  =
            (schema_slot_size_ == 0) ? 1 : schema_slot_size_;
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

    // --- Create transport queue ---
    if (tr.transport == config::Transport::Shm)
    {
        auto *out_shm_ptr = out_producer_->shm();
        if (out_shm_ptr == nullptr)
        {
            LOGGER_ERROR("[prod] transport=shm but SHM unavailable for channel '{}'", ch);
            return false;
        }
        queue_ = hub::ShmQueue::from_producer_ref(
            *out_shm_ptr, schema_slot_size_, core_.schema_fz_size(), ch);
    }
    else
    {
        auto zmq_fields =
            scripting::schema_spec_to_zmq_fields(slot_spec_, schema_slot_size_);
        const auto zmq_policy = (tr.zmq_overflow_policy == "block")
                                     ? hub::OverflowPolicy::Block
                                     : hub::OverflowPolicy::Drop;
        queue_ = hub::ZmqQueue::push_to(
            tr.zmq_endpoint, std::move(zmq_fields), tr.zmq_packing,
            tr.zmq_bind, std::nullopt, 0, tr.zmq_buffer_depth, zmq_policy);
        if (!queue_)
        {
            LOGGER_ERROR("[prod] Failed to create ZmqQueue for endpoint '{}'",
                         tr.zmq_endpoint);
            return false;
        }
    }

    if (!queue_->start())
    {
        LOGGER_ERROR("[prod] Queue::start() failed for channel '{}'", ch);
        queue_.reset();
        return false;
    }
    queue_->set_checksum_options(shm.update_checksum, core_.has_fz());
    queue_->reset_metrics();
    if (tc.period_us > 0.0)
        queue_->set_configured_period(static_cast<uint64_t>(tc.period_us));

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

    // Stop queue.
    if (queue_)
    {
        queue_->stop();
        queue_.reset();
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
    if (!queue_)
    {
        LOGGER_ERROR("[prod] run_data_loop_: transport queue not initialized — aborting");
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
    const size_t buf_sz = queue_->item_size();

    // Flexzone pointers — valid after queue start.
    void       *fz_ptr  = core_.has_fz() ? queue_->write_flexzone() : nullptr;
    const size_t fz_sz  = core_.has_fz() ? queue_->flexzone_size()  : 0;
    const char *fz_type = core_.has_fz() ? "FlexFrame" : nullptr;

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
            buf = queue_->write_acquire(short_timeout);
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
                queue_->write_discard();
            break;
        }

        // --- Step C: Drain everything right before script call ---
        auto msgs = core_.drain_messages();
        drain_inbox_sync_();

        // --- Step D: Prepare and invoke callback ---
        if (buf != nullptr)
            std::memset(buf, 0, buf_sz);

        // Re-read flexzone pointer each cycle (ShmQueue may move it).
        if (core_.has_fz())
            fz_ptr = queue_->write_flexzone();

        InvokeResult result =
            engine_->invoke_produce(buf, buf_sz, fz_ptr, fz_sz, fz_type, msgs);

        // --- Step E: Commit/discard ---
        if (buf != nullptr)
        {
            if (result == InvokeResult::Commit)
            {
                queue_->write_commit();
                core_.inc_out_written();
            }
            else
            {
                queue_->write_discard();
                core_.inc_drops();
            }
        }
        else
        {
            core_.inc_drops();
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
    const auto &id = config_.identity();
    const auto &ch = config_.out_channel();
    const auto &tc = config_.timing();

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
    nlohmann::json base;
    base["out_written"]        = core_.out_written();
    base["drops"]              = core_.drops();
    base["script_errors"]      = engine_ ? engine_->script_error_count() : 0;
    base["last_cycle_work_us"] = core_.last_cycle_work_us();
    base["loop_overrun_count"] = 0; // overwritten below if SHM is available

    if (out_producer_.has_value())
    {
        base["ctrl_queue_dropped"] = out_producer_->ctrl_queue_dropped();
    }

    // Use our own iteration_count (always available, regardless of transport).
    base["iteration_count"] = core_.iteration_count();

    if (queue_)
    {
        const auto m = queue_->metrics();
        base["loop_overrun_count"]  = m.overrun_count;
        base["last_iteration_us"]   = m.last_iteration_us;
        base["max_iteration_us"]    = m.max_iteration_us;
        base["last_slot_work_us"]   = m.last_slot_work_us;
        base["last_slot_wait_us"]   = m.last_slot_wait_us;
        base["configured_period_us"] = m.configured_period_us;
    }
    return base;
}

} // namespace pylabhub::producer
