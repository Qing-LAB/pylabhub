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

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"

#include "role_host_helpers.hpp"
#include "zmq_poll_loop.hpp"
#include "utils/script_host_helpers.hpp" // resolve_schema, schema_spec_to_zmq_fields, compute_schema_hash

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

ConsumerRoleHost::~ConsumerRoleHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void ConsumerRoleHost::set_engine(std::unique_ptr<scripting::ScriptEngine> engine)
{
    engine_ = std::move(engine);
}

void ConsumerRoleHost::set_config(ConsumerConfig config)
{
    config_ = std::move(config);
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
        // Worker failed during setup — join immediately.
        if (worker_thread_.joinable())
            worker_thread_.join();
    }
}

// ============================================================================
// shutdown_ — signal shutdown, join worker thread
// ============================================================================

void ConsumerRoleHost::shutdown_()
{
    core_.shutdown_requested.store(true, std::memory_order_release);
    core_.notify_incoming();

    if (worker_thread_.joinable())
        worker_thread_.join();
}

// ============================================================================
// worker_main_ — the worker thread entry point
// ============================================================================

void ConsumerRoleHost::worker_main_()
{
    // Step 1: Initialize the engine.
    if (!engine_->initialize("cons"))
    {
        LOGGER_ERROR("[cons] Engine initialize() failed");
        ready_promise_.set_value(false);
        return;
    }

    // Warn if script type was not explicitly set in config.
    if (!config_.script_type_explicit)
    {
        LOGGER_WARN("[cons] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    config_.script_type, config_.script_type);
    }

    // Step 2: Load script and extract callbacks.
    const std::filesystem::path base_path =
        config_.script_path.empty() ? std::filesystem::current_path()
                                    : std::filesystem::weakly_canonical(config_.script_path);
    const std::filesystem::path script_dir = base_path / "script" / config_.script_type;
    const char *entry_point =
        (config_.script_type == "lua") ? "init.lua" : "__init__.py";

    if (!engine_->load_script(script_dir, entry_point, "on_consume"))
    {
        LOGGER_ERROR("[cons] Engine load_script() failed");
        script_load_ok_.store(false, std::memory_order_release);
        engine_->finalize();
        ready_promise_.set_value(false);
        return;
    }
    script_load_ok_.store(true, std::memory_order_release);

    // Step 3: Resolve schemas and register slot types (read-only access).
    {
        std::vector<std::string> schema_dirs;
        if (!config_.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(config_.hub_dir) / "schemas").string());

        try
        {
            slot_spec_ = scripting::resolve_schema(
                config_.slot_schema_json, false, "cons", schema_dirs);
            core_.fz_spec = scripting::resolve_schema(
                config_.flexzone_schema_json, true, "cons", schema_dirs);
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
        config_.zmq_packing.empty() ? "aligned" : config_.zmq_packing;

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
    if (core_.fz_spec.has_schema)
    {
        core_.has_fz = true;
        if (!engine_->register_slot_type(core_.fz_spec, "FlexFrame", packing))
        {
            LOGGER_ERROR("[cons] Failed to register FlexFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
        core_.schema_fz_size = engine_->type_sizeof("FlexFrame");
        // Page-align flexzone size.
        core_.schema_fz_size = (core_.schema_fz_size + 4095U) & ~size_t{4095U};
    }

    // Validate-only mode: print layout and exit.
    if (validate_only_)
    {
        // TODO: print layout via engine
        engine_->finalize();
        ready_promise_.set_value(true);
        return;
    }

    // Step 4: setup_infrastructure_
    if (!setup_infrastructure_())
    {
        teardown_infrastructure_();
        engine_->finalize();
        ready_promise_.set_value(false);
        return;
    }

    // Step 5: Build RoleContext and engine API.
    // Store script_dir string so ctx.script_dir doesn't dangle.
    const std::string script_dir_str = script_dir.string();

    scripting::RoleContext ctx;
    ctx.role_tag    = "cons";
    ctx.uid         = config_.consumer_uid.c_str();
    ctx.name        = config_.consumer_name.c_str();
    ctx.channel     = config_.channel.c_str();
    ctx.out_channel = nullptr;
    ctx.log_level   = config_.log_level.c_str();
    ctx.script_dir  = script_dir_str.c_str();
    ctx.role_dir    = config_.role_dir.c_str();
    ctx.messenger   = &in_messenger_;
    ctx.queue_writer = nullptr;
    ctx.queue_reader = queue_reader_;
    ctx.producer    = nullptr;
    ctx.consumer    = in_consumer_.has_value() ? &(*in_consumer_) : nullptr;
    ctx.core = &core_;
    ctx.stop_on_script_error = config_.stop_on_script_error;

    engine_->build_api(ctx);

    // Step 6: invoke on_init.
    engine_->invoke_on_init();

    // Step 7: Spawn ctrl_thread_ and signal ready.
    core_.running_threads.store(true, std::memory_order_release);
    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    // Step 8: Signal ready.
    ready_promise_.set_value(true);

    // Step 9: Run the data loop.
    run_data_loop_();

    // Step 10: invoke on_stop.
    engine_->invoke_on_stop();

    // Step 11: teardown infrastructure.
    teardown_infrastructure_();

    // Step 12: finalize engine.
    engine_->finalize();
}

// ============================================================================
// setup_infrastructure_ — connect to broker, create consumer, wire events
// ============================================================================

bool ConsumerRoleHost::setup_infrastructure_()
{
    // --- Consumer options ---
    hub::ConsumerOptions opts;
    opts.channel_name         = config_.channel;
    opts.shm_shared_secret    = config_.shm_enabled ? config_.shm_secret : 0u;
    opts.expected_schema_hash = scripting::compute_schema_hash(slot_spec_, core_.fz_spec);
    opts.consumer_uid         = config_.consumer_uid;
    opts.consumer_name        = config_.consumer_name;

    if (config_.target_period_ms > 0)
    {
        opts.loop_policy = hub::LoopPolicy::FixedRate;
        opts.period_ms   = std::chrono::milliseconds{static_cast<int>(config_.target_period_ms)};
    }

    // Transport declaration (Phase 7).
    opts.queue_type = (config_.queue_type == QueueType::Zmq) ? "zmq" : "shm";

    // ZMQ data loop (HEP-CORE-0021).
    if (config_.queue_type == QueueType::Zmq)
    {
        opts.zmq_schema       = scripting::schema_spec_to_zmq_fields(slot_spec_, schema_slot_size_);
        opts.zmq_buffer_depth = config_.zmq_buffer_depth;
    }

    opts.ctrl_queue_max_depth = config_.ctrl_queue_max_depth;
    opts.peer_dead_timeout_ms = config_.peer_dead_timeout_ms;

    // --- Inbox setup (optional) ---
    scripting::SchemaSpec inbox_spec;
    size_t inbox_schema_slot_size = 0;

    if (config_.has_inbox())
    {
        std::vector<std::string> schema_dirs;
        if (!config_.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(config_.hub_dir) / "schemas").string());

        try
        {
            inbox_spec = scripting::resolve_schema(
                config_.inbox_schema_json, false, "cons", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[cons] Inbox schema parse error: {}", e.what());
            return false;
        }

        const std::string inbox_packing =
            config_.zmq_packing.empty() ? "aligned" : config_.zmq_packing;

        // Register inbox type in the engine.
        if (inbox_spec.has_schema)
        {
            if (!engine_->register_slot_type(inbox_spec, "InboxFrame", inbox_packing))
            {
                LOGGER_ERROR("[cons] Failed to register InboxFrame type");
                return false;
            }
            inbox_schema_slot_size = engine_->type_sizeof("InboxFrame");
            inbox_type_name_ = "InboxFrame";
        }

        const std::string ep = config_.inbox_endpoint.empty()
            ? "tcp://127.0.0.1:0"
            : config_.inbox_endpoint;
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

        const int inbox_rcvhwm = (config_.inbox_overflow_policy == "block")
            ? 0
            : static_cast<int>(config_.inbox_buffer_depth);

        inbox_queue_ = hub::InboxQueue::bind_at(
            ep, std::move(zmq_fields), inbox_packing, inbox_rcvhwm);
        if (!inbox_queue_ || !inbox_queue_->start())
        {
            LOGGER_ERROR("[cons] Failed to start InboxQueue at '{}'", ep);
            inbox_queue_.reset();
            return false;
        }
        else
        {
            LOGGER_INFO("[cons] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());
        }
    }

    // --- Broker connect ---
    if (!config_.broker.empty())
    {
        if (!in_messenger_.connect(config_.broker, config_.broker_pubkey,
                                    config_.auth.client_pubkey, config_.auth.client_seckey))
        {
            LOGGER_ERROR("[cons] broker connect failed ({}); aborting", config_.broker);
            return false;
        }
    }

    // --- Create consumer ---
    auto maybe_consumer = hub::Consumer::connect(in_messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[cons] Failed to connect consumer to channel '{}'", config_.channel);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    if (auto *in_shm = in_consumer_->shm(); in_shm != nullptr)
        in_shm->clear_metrics();

    // --- Wire event callbacks → IncomingMessage queue ---
    in_consumer_->on_channel_closing([this]() {
        LOGGER_INFO("[cons] CHANNEL_CLOSING_NOTIFY received, queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        core_.enqueue_message(std::move(msg));
    });

    in_consumer_->on_force_shutdown([this]() {
        LOGGER_WARN("[cons] FORCE_SHUTDOWN received, forcing immediate shutdown");
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // ZMQ data routing for SHM queue_type: ZMQ frames → message queue.
    if (config_.queue_type == QueueType::Shm)
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
    in_consumer_->on_peer_dead([this]() {
        LOGGER_WARN("[cons] peer-dead: producer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        core_.stop_reason_.store(
            static_cast<int>(scripting::RoleHostCore::StopReason::PeerDead),
            std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    in_messenger_.on_hub_dead([this]() {
        LOGGER_WARN("[cons] hub-dead: broker connection lost; triggering shutdown");
        core_.stop_reason_.store(
            static_cast<int>(scripting::RoleHostCore::StopReason::HubDead),
            std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // --- Create transport QueueReader ---
    if (config_.queue_type == QueueType::Shm)
    {
        auto *in_shm = in_consumer_->shm();
        if (in_shm == nullptr)
        {
            LOGGER_ERROR("[cons] queue_type='shm' but SHM unavailable for channel '{}'",
                         config_.channel);
            return false;
        }
        shm_queue_    = hub::ShmQueue::from_consumer_ref(
            *in_shm, schema_slot_size_, core_.schema_fz_size, config_.channel);
        queue_reader_ = shm_queue_.get();
    }
    else // QueueType::Zmq
    {
        queue_reader_ = in_consumer_->queue_reader();
        if (queue_reader_ == nullptr)
        {
            LOGGER_ERROR("[cons] queue_type='zmq' but broker reported SHM transport for "
                         "channel '{}'; check that the producer uses ZMQ transport",
                         config_.channel);
            return false;
        }
    }

    if (queue_reader_)
        queue_reader_->set_verify_checksum(config_.verify_checksum, core_.has_fz);

    LOGGER_INFO("[cons] Consumer started on channel '{}' (shm={})", config_.channel,
                in_consumer_->has_shm());

    // --- Startup coordination (HEP-0023) ---
    if (!scripting::wait_for_roles(in_messenger_, config_.wait_for_roles, "[cons]"))
        return false;

    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ConsumerRoleHost::teardown_infrastructure_()
{
    core_.running_threads.store(false, std::memory_order_release);
    core_.notify_incoming();

    // Join ctrl_thread_.
    if (ctrl_thread_.joinable())
        ctrl_thread_.join();

    // Stop inbox_queue_ (if exists).
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

    // Null out the QueueReader pointer before destroying the SHM queue or consumer.
    queue_reader_ = nullptr;
    if (shm_queue_)
    {
        shm_queue_->stop();
        shm_queue_.reset();
    }

    // Deregister hub-dead callback.
    in_messenger_.on_hub_dead(nullptr);

    // Stop/close consumer.
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
    if (!queue_reader_)
    {
        LOGGER_ERROR("[cons] run_data_loop_: QueueReader not initialized — aborting");
        core_.running_threads.store(false, std::memory_order_release);
        return;
    }

    // --- Setup ---
    const double period_us =
        static_cast<double>(config_.target_period_ms) * kUsPerMs;
    const bool is_max_rate = (config_.loop_timing == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, config_.queue_io_wait_timeout_ratio);
    // read_acquire takes milliseconds; convert with rounding up to avoid 0ms.
    const auto short_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(short_timeout_us + std::chrono::microseconds{999});
    const size_t item_sz = queue_reader_->item_size();

    // Flexzone pointers — read-only for consumer.
    const char *fz_type = core_.has_fz ? "FlexFrame" : nullptr;

    // First cycle: no deadline — fire immediately.
    auto deadline = Clock::time_point::max();

    // --- Outer loop ---
    while (core_.running_threads.load(std::memory_order_acquire) &&
           !core_.shutdown_requested.load(std::memory_order_acquire) &&
           !core_.critical_error_.load(std::memory_order_relaxed))
    {
        // Check external shutdown flag.
        if (core_.g_shutdown && core_.g_shutdown->load(std::memory_order_relaxed))
            break;

        const auto cycle_start = Clock::now();

        // --- Step A: Acquire data with inner retry ---
        const void *data = nullptr;
        while (true)
        {
            data = queue_reader_->read_acquire(short_timeout);
            if (data != nullptr)
                break; // got slot

            if (is_max_rate)
                break; // MaxRate: single attempt

            // Check shutdown between retries.
            if (!core_.running_threads.load(std::memory_order_relaxed) ||
                core_.shutdown_requested.load(std::memory_order_relaxed) ||
                core_.critical_error_.load(std::memory_order_relaxed))
            {
                break;
            }
            if (core_.g_shutdown && core_.g_shutdown->load(std::memory_order_relaxed))
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
        if (!is_max_rate && data != nullptr &&
            deadline != Clock::time_point::max() && Clock::now() < deadline)
        {
            std::this_thread::sleep_until(deadline);
        }

        // Safety check after potential sleep: shutdown may have been requested.
        if (!core_.running_threads.load(std::memory_order_relaxed) ||
            core_.shutdown_requested.load(std::memory_order_relaxed) ||
            core_.critical_error_.load(std::memory_order_relaxed))
        {
            if (data != nullptr)
                queue_reader_->read_release();
            break;
        }

        // --- Step C: Drain everything right before script call ---
        auto msgs = core_.drain_messages();
        drain_inbox_sync_();

        // --- Step D: Invoke callback ---
        // Update last_seq and in_received when data arrives.
        if (data != nullptr)
        {
            last_seq_.store(queue_reader_->last_seq(), std::memory_order_relaxed);
            in_received_.fetch_add(1, std::memory_order_relaxed);
        }

        // Read-only flexzone pointer (re-read each cycle for ShmQueue).
        const void *fz_ptr = core_.has_fz ? queue_reader_->read_flexzone() : nullptr;
        const size_t fz_sz = core_.has_fz ? queue_reader_->flexzone_size() : 0;

        // Track error count before invoke to detect new errors (invoke_consume is void).
        const uint64_t errors_before = engine_->script_error_count();

        engine_->invoke_consume(data, item_sz, fz_ptr, fz_sz, fz_type, msgs);

        // --- Step E: Release slot ---
        if (data != nullptr)
            queue_reader_->read_release();

        // Check stop_on_script_error: compare error count before/after invoke.
        if (config_.stop_on_script_error &&
            engine_->script_error_count() > errors_before)
        {
            core_.shutdown_requested.store(true, std::memory_order_release);
        }

        // --- Step F: Metrics ---
        const auto now     = Clock::now();
        const auto work_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cycle_start).count());
        last_cycle_work_us_.store(work_us, std::memory_order_relaxed);
        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        // --- Step G: Compute next deadline ---
        deadline = compute_next_deadline(config_.loop_timing, deadline, cycle_start, period_us);
    }

    LOGGER_INFO("[cons] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                core_.critical_error_.load());
}

// ============================================================================
// run_ctrl_thread_ — polls consumer ZMQ sockets, sends heartbeats
// ============================================================================

void ConsumerRoleHost::run_ctrl_thread_()
{
    scripting::ZmqPollLoop loop{core_, "cons:" + config_.consumer_uid};
    loop.sockets = {
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
        {in_consumer_->data_zmq_socket_handle(),
         [&] { in_consumer_->handle_data_events_nowait(); }},
    };
    loop.get_iteration = [&] {
        return iteration_count_.load(std::memory_order_relaxed);
    };
    loop.periodic_tasks.emplace_back(
        [&] {
            in_messenger_.enqueue_heartbeat(config_.channel);
        },
        config_.heartbeat_interval_ms);
    // HEP-CORE-0019: periodic metrics report.
    loop.periodic_tasks.emplace_back(
        [&] {
            in_messenger_.enqueue_metrics_report(
                config_.channel, config_.consumer_uid,
                snapshot_metrics_json());
        },
        config_.heartbeat_interval_ms);
    loop.run();
}

// ============================================================================
// drain_inbox_sync_ — drain all inbox messages non-blocking
// ============================================================================

void ConsumerRoleHost::drain_inbox_sync_()
{
    scripting::drain_inbox_sync(inbox_queue_.get(), engine_.get(), inbox_type_name_);
}

// ============================================================================
// snapshot_metrics_json — for heartbeat/metrics reporting
// ============================================================================

nlohmann::json ConsumerRoleHost::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]        = in_received_.load(std::memory_order_relaxed);
    base["script_errors"]      = engine_ ? engine_->script_error_count() : 0;
    base["last_cycle_work_us"] = last_cycle_work_us_.load(std::memory_order_relaxed);
    base["loop_overrun_count"] = uint64_t{0}; // consumer is demand-driven, no deadline

    // Use our own iteration_count (always available, regardless of transport).
    base["iteration_count"] = iteration_count_.load(std::memory_order_relaxed);

    if (in_consumer_.has_value())
    {
        base["ctrl_queue_dropped"] = in_consumer_->ctrl_queue_dropped();
    }

    if (in_consumer_.has_value())
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — shm() is non-const but we only read
        if (const auto *shm = const_cast<hub::Consumer &>(*in_consumer_).shm();
            shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["last_iteration_us"] = m.last_iteration_us;
            base["max_iteration_us"]  = m.max_iteration_us;
            base["last_slot_work_us"] = m.last_slot_work_us;
            base["last_slot_wait_us"] = m.last_slot_wait_us;
        }
    }
    return base;
}

} // namespace pylabhub::consumer
