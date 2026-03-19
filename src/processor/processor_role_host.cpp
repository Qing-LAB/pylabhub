/**
 * @file processor_role_host.cpp
 * @brief ProcessorRoleHost — unified engine-agnostic processor implementation.
 *
 * This is the canonical data loop for the processor role.  It follows
 * docs/tech_draft/loop_design_unified.md §5 exactly.
 *
 * Layer 3 (infrastructure): Dual Messengers, Consumer + Producer, dual queues,
 *   ctrl_thread_, events, peer-dead/hub-dead wiring.
 * Layer 2 (data loop): dual-queue inner retry acquire, deadline wait, drain,
 *   invoke, commit/release.
 * Layer 1 (engine): delegated to ScriptEngine via invoke_process / invoke_on_inbox.
 */
#include "processor_role_host.hpp"

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

namespace pylabhub::processor
{

using scripting::IncomingMessage;
using scripting::InvokeResult;
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

void ProcessorRoleHost::set_engine(std::unique_ptr<scripting::ScriptEngine> engine)
{
    engine_ = std::move(engine);
}

void ProcessorRoleHost::set_config(ProcessorConfig config)
{
    config_ = std::move(config);
}

// ============================================================================
// startup_ — spawn worker thread, block until ready or failure
// ============================================================================

void ProcessorRoleHost::startup_()
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

void ProcessorRoleHost::shutdown_()
{
    core_.shutdown_requested.store(true, std::memory_order_release);
    core_.notify_incoming();

    if (worker_thread_.joinable())
        worker_thread_.join();
}

// ============================================================================
// worker_main_ — the worker thread entry point
// ============================================================================

void ProcessorRoleHost::worker_main_()
{
    // Step 1: Initialize the engine.
    if (!engine_->initialize("proc"))
    {
        LOGGER_ERROR("[proc] Engine initialize() failed");
        ready_promise_.set_value(false);
        return;
    }

    // Warn if script type was not explicitly set in config.
    if (!config_.script_type_explicit)
    {
        LOGGER_WARN("[proc] 'script.type' not set in config — defaulting to '{}'. "
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

    if (!engine_->load_script(script_dir, entry_point, "on_process"))
    {
        LOGGER_ERROR("[proc] Engine load_script() failed");
        script_load_ok_.store(false, std::memory_order_release);
        engine_->finalize();
        ready_promise_.set_value(false);
        return;
    }
    script_load_ok_.store(true, std::memory_order_release);

    // Step 3: Resolve schemas and register slot types.
    {
        std::vector<std::string> schema_dirs;
        auto add_schema_dir = [&schema_dirs](const std::string &hub_dir) {
            if (hub_dir.empty())
                return;
            const std::string d = (std::filesystem::path(hub_dir) / "schemas").string();
            if (std::find(schema_dirs.begin(), schema_dirs.end(), d) == schema_dirs.end())
                schema_dirs.push_back(d);
        };
        add_schema_dir(config_.in_hub_dir);
        add_schema_dir(config_.out_hub_dir);
        add_schema_dir(config_.hub_dir);

        try
        {
            in_slot_spec_  = scripting::resolve_schema(
                config_.in_slot_schema_json, false, "proc", schema_dirs);
            out_slot_spec_ = scripting::resolve_schema(
                config_.out_slot_schema_json, false, "proc", schema_dirs);
            core_.fz_spec  = scripting::resolve_schema(
                config_.flexzone_schema_json, true, "proc", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[proc] Schema parse error: {}", e.what());
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
    }

    // Determine packing: input and output may use different ZMQ packing.
    const std::string in_packing =
        config_.in_zmq_packing.empty() ? "aligned" : config_.in_zmq_packing;
    const std::string out_packing =
        config_.out_zmq_packing.empty() ? "aligned" : config_.out_zmq_packing;

    // Register input slot type.
    if (in_slot_spec_.has_schema)
    {
        if (!engine_->register_slot_type(in_slot_spec_, "InSlotFrame", in_packing))
        {
            LOGGER_ERROR("[proc] Failed to register InSlotFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
        in_schema_slot_size_ = engine_->type_sizeof("InSlotFrame");
    }

    // Register output slot type.
    if (out_slot_spec_.has_schema)
    {
        if (!engine_->register_slot_type(out_slot_spec_, "OutSlotFrame", out_packing))
        {
            LOGGER_ERROR("[proc] Failed to register OutSlotFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
        out_schema_slot_size_ = engine_->type_sizeof("OutSlotFrame");
    }

    // Register flexzone type (output side).
    if (core_.fz_spec.has_schema)
    {
        core_.has_fz = true;
        if (!engine_->register_slot_type(core_.fz_spec, "FlexFrame", out_packing))
        {
            LOGGER_ERROR("[proc] Failed to register FlexFrame type");
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
    ctx.role_tag     = "proc";
    ctx.uid          = config_.processor_uid.c_str();
    ctx.name         = config_.processor_name.c_str();
    ctx.channel      = config_.in_channel.c_str();
    ctx.out_channel  = config_.out_channel.c_str();
    ctx.log_level    = config_.log_level.c_str();
    ctx.script_dir   = script_dir_str.c_str();
    ctx.role_dir     = config_.role_dir.c_str();
    ctx.messenger    = &out_messenger_;
    ctx.queue_writer = out_q_;
    ctx.queue_reader = in_q_;
    ctx.producer     = out_producer_.has_value() ? &(*out_producer_) : nullptr;
    ctx.consumer     = in_consumer_.has_value() ? &(*in_consumer_) : nullptr;
    ctx.stop_on_script_error = config_.stop_on_script_error;

    engine_->build_api(ctx);

    // Step 6: invoke on_init.
    engine_->invoke_on_init();

    // Sync flexzone checksum after on_init (user may have written to flexzone).
    if (out_q_ && core_.has_fz)
        out_q_->sync_flexzone_checksum();

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
// setup_infrastructure_ — dual broker connect, consumer + producer, wire events
// ============================================================================

bool ProcessorRoleHost::setup_infrastructure_()
{
    // ── Consumer side (in_channel) ──────────────────────────────────────────
    hub::ConsumerOptions in_opts;
    in_opts.channel_name         = config_.in_channel;
    in_opts.shm_shared_secret    = config_.in_shm_enabled ? config_.in_shm_secret : 0u;
    in_opts.expected_schema_hash = scripting::compute_schema_hash(
                                       in_slot_spec_, scripting::SchemaSpec{});
    in_opts.consumer_uid         = config_.processor_uid;
    in_opts.consumer_name        = config_.processor_name;
    in_opts.zmq_schema           = scripting::schema_spec_to_zmq_fields(
                                       in_slot_spec_, in_schema_slot_size_);
    in_opts.zmq_packing          = config_.in_zmq_packing;
    in_opts.ctrl_queue_max_depth = config_.ctrl_queue_max_depth;
    in_opts.peer_dead_timeout_ms = config_.peer_dead_timeout_ms;

    const auto &in_ep  = config_.resolved_in_broker();
    const auto &in_pub = config_.resolved_in_broker_pubkey();
    if (!in_ep.empty())
    {
        if (!in_messenger_.connect(in_ep, in_pub,
                                   config_.auth.client_pubkey, config_.auth.client_seckey))
        {
            LOGGER_ERROR("[proc] in_messenger broker connect failed ({}); aborting", in_ep);
            return false;
        }
    }

    auto maybe_consumer = hub::Consumer::connect(in_messenger_, in_opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[proc] Failed to connect consumer to in_channel '{}'",
                     config_.in_channel);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    // Graceful shutdown: queue channel_closing as a regular event message (input side).
    in_consumer_->on_channel_closing([this]() {
        LOGGER_INFO("[proc] CHANNEL_CLOSING_NOTIFY received (in_channel), queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        msg.details["channel"] = config_.in_channel;
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired (input side).
    in_consumer_->on_force_shutdown([this]() {
        LOGGER_WARN("[proc] FORCE_SHUTDOWN received (in_channel), forcing immediate shutdown");
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Route ZMQ data messages to the incoming queue.
    in_consumer_->on_zmq_data(
        [this](std::span<const std::byte> data)
        {
            LOGGER_DEBUG("[proc] zmq_data: in_channel data_message size={}", data.size());
            IncomingMessage msg;
            msg.data.assign(data.begin(), data.end());
            core_.enqueue_message(std::move(msg));
        });

    // Wire consumer-side events.
    in_consumer_->on_producer_message(
        [this](std::string_view type, std::span<const std::byte> data)
        {
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
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = details;
            msg.details["detail"] = event;
            msg.details["source"] = "in_channel";
            core_.enqueue_message(std::move(msg));
        });

    if (!in_consumer_->start_embedded())
    {
        LOGGER_ERROR("[proc] in_consumer->start_embedded() failed");
        return false;
    }

    // ── Producer side (out_channel) ─────────────────────────────────────────
    hub::ProducerOptions out_opts;
    out_opts.channel_name = config_.out_channel;
    out_opts.pattern      = hub::ChannelPattern::PubSub;
    out_opts.has_shm      = config_.out_shm_enabled;
    out_opts.schema_hash  = scripting::compute_schema_hash(out_slot_spec_, core_.fz_spec);
    out_opts.actor_name   = config_.processor_name;
    out_opts.actor_uid    = config_.processor_uid;

    if (config_.target_period_ms > 0)
    {
        out_opts.loop_policy = hub::LoopPolicy::FixedRate;
        out_opts.period_ms   = std::chrono::milliseconds{config_.target_period_ms};
    }
    out_opts.ctrl_queue_max_depth = config_.ctrl_queue_max_depth;
    out_opts.peer_dead_timeout_ms = config_.peer_dead_timeout_ms;

    // SHM config (output side).
    if (config_.out_shm_enabled)
    {
        out_opts.shm_config.shared_secret        = config_.out_shm_secret;
        out_opts.shm_config.ring_buffer_capacity = config_.out_shm_slot_count;
        out_opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        out_opts.shm_config.consumer_sync_policy = config_.out_shm_consumer_sync_policy;
        out_opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        out_opts.shm_config.flex_zone_size       = core_.schema_fz_size;

        out_opts.shm_config.physical_page_size = hub::system_page_size();
        out_opts.shm_config.logical_unit_size  =
            (out_schema_slot_size_ == 0) ? 1 : out_schema_slot_size_;
    }

    // HEP-CORE-0021: for ZMQ output, register as a ZMQ Virtual Channel Node.
    if (config_.out_transport == Transport::Zmq)
    {
        out_opts.has_shm           = false;
        out_opts.data_transport    = "zmq";
        out_opts.zmq_node_endpoint = config_.zmq_out_endpoint;
        out_opts.zmq_bind          = config_.zmq_out_bind;
        out_opts.zmq_schema        = scripting::schema_spec_to_zmq_fields(
                                         out_slot_spec_, out_schema_slot_size_);
        out_opts.zmq_packing       = config_.out_zmq_packing;
        out_opts.zmq_buffer_depth  = config_.out_zmq_buffer_depth;
        const bool zmq_drop = config_.zmq_out_overflow_policy.empty()
            ? (config_.overflow_policy == OverflowPolicy::Drop)
            : (config_.zmq_out_overflow_policy == "drop");
        out_opts.zmq_overflow_policy = zmq_drop
                                           ? hub::OverflowPolicy::Drop
                                           : hub::OverflowPolicy::Block;
    }

    // ── Inbox facility (optional) ───────────────────────────────────────────
    scripting::SchemaSpec inbox_spec;
    size_t inbox_schema_slot_size = 0;

    if (config_.has_inbox())
    {
        std::vector<std::string> schema_dirs;
        auto add_schema_dir = [&schema_dirs](const std::string &hub_dir) {
            if (hub_dir.empty())
                return;
            const std::string d = (std::filesystem::path(hub_dir) / "schemas").string();
            if (std::find(schema_dirs.begin(), schema_dirs.end(), d) == schema_dirs.end())
                schema_dirs.push_back(d);
        };
        add_schema_dir(config_.in_hub_dir);
        add_schema_dir(config_.out_hub_dir);
        add_schema_dir(config_.hub_dir);

        try
        {
            inbox_spec = scripting::resolve_schema(
                config_.inbox_schema_json, false, "proc", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[proc] Inbox schema parse error: {}", e.what());
            return false;
        }

        const std::string inbox_packing =
            inbox_spec.packing.empty()
                ? config_.in_zmq_packing
                : inbox_spec.packing;

        // Register inbox type in the engine.
        if (inbox_spec.has_schema)
        {
            if (!engine_->register_slot_type(inbox_spec, "InboxFrame",
                                              inbox_packing.empty() ? "aligned" : inbox_packing))
            {
                LOGGER_ERROR("[proc] Failed to register InboxFrame type");
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
            ep, std::move(zmq_fields),
            inbox_packing.empty() ? "aligned" : inbox_packing,
            inbox_rcvhwm);
        if (!inbox_queue_ || !inbox_queue_->start())
        {
            LOGGER_ERROR("[proc] Failed to start InboxQueue at '{}'", ep);
            if (inbox_queue_)
                inbox_queue_.reset();
            return false;
        }
        LOGGER_INFO("[proc] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());

        out_opts.inbox_endpoint    = inbox_queue_->actual_endpoint();
        out_opts.inbox_schema_json = spec_json.dump();
        out_opts.inbox_packing     = inbox_packing.empty() ? "aligned" : inbox_packing;
    }

    // ── Output broker connect ───────────────────────────────────────────────
    const auto &out_ep  = config_.resolved_out_broker();
    const auto &out_pub = config_.resolved_out_broker_pubkey();
    if (!out_ep.empty())
    {
        if (!out_messenger_.connect(out_ep, out_pub,
                                    config_.auth.client_pubkey, config_.auth.client_seckey))
        {
            LOGGER_ERROR("[proc] out_messenger broker connect failed ({}); aborting", out_ep);
            return false;
        }
    }

    // --- Create producer ---
    auto maybe_producer = hub::Producer::create(out_messenger_, out_opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[proc] Failed to create producer for out_channel '{}'",
                     config_.out_channel);
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    if (auto *out_shm = out_producer_->shm(); out_shm != nullptr)
        out_shm->clear_metrics();

    // Graceful shutdown: queue channel_closing event (output side).
    out_producer_->on_channel_closing([this]() {
        IncomingMessage msg;
        msg.event = "channel_closing";
        msg.details["channel"] = config_.out_channel;
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired (output side).
    out_producer_->on_force_shutdown([this]() {
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    auto hex_identity = [](const std::string &raw) -> std::string {
        return format_tools::bytes_to_hex(raw);
    };

    out_producer_->on_consumer_joined(
        [this, hex_identity](const std::string &identity) {
            LOGGER_INFO("[proc] peer_event: consumer_joined identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_joined";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_left(
        [this, hex_identity](const std::string &identity) {
            LOGGER_INFO("[proc] peer_event: consumer_left identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_left";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_consumer_died(config_.out_channel,
        [this](uint64_t pid, std::string reason) {
            LOGGER_INFO("[proc] broker_notify: consumer_died pid={} reason={}",
                        pid, reason);
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(config_.out_channel,
        [this](std::string event, nlohmann::json details) {
            LOGGER_INFO("[proc] broker_notify: channel_event event='{}' details={}",
                        event, details.dump());
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = std::move(details);
            msg.details["detail"] = std::move(event);
            msg.details["source"] = "out_channel";
            core_.enqueue_message(std::move(msg));
        });

    if (!config_.out_channel.empty())
    {
        out_messenger_.suppress_periodic_heartbeat(config_.out_channel);
        out_messenger_.enqueue_heartbeat(config_.out_channel);
    }

    out_producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data) {
            LOGGER_INFO("[proc] zmq_data: consumer_message size={}", data.size());
            IncomingMessage msg;
            msg.sender = identity;
            msg.data.assign(data.begin(), data.end());
            core_.enqueue_message(std::move(msg));
        });

    if (!out_producer_->start_embedded())
    {
        LOGGER_ERROR("[proc] out_producer->start_embedded() failed");
        return false;
    }

    // ── Wire peer-dead and hub-dead monitoring ─────────────────────────────────
    in_consumer_->on_peer_dead([this]() {
        LOGGER_WARN("[proc] peer-dead: upstream producer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        core_.stop_reason_.store(
            static_cast<int>(scripting::RoleHostCore::StopReason::PeerDead),
            std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    out_producer_->on_peer_dead([this]() {
        LOGGER_WARN("[proc] peer-dead: downstream consumer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        core_.stop_reason_.store(
            static_cast<int>(scripting::RoleHostCore::StopReason::PeerDead),
            std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    auto hub_dead_cb = [this]() {
        LOGGER_WARN("[proc] hub-dead: broker connection lost; triggering shutdown");
        core_.stop_reason_.store(
            static_cast<int>(scripting::RoleHostCore::StopReason::HubDead),
            std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    };
    in_messenger_.on_hub_dead(hub_dead_cb);
    out_messenger_.on_hub_dead(hub_dead_cb);

    // ── Create data queues ─────────────────────────────────────────────────
    // Input queue.
    if (config_.in_transport == Transport::Zmq)
    {
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(
            in_slot_spec_, in_schema_slot_size_);
        in_queue_ = hub::ZmqQueue::pull_from(
            config_.zmq_in_endpoint, std::move(zmq_fields),
            config_.in_zmq_packing, config_.zmq_in_bind, config_.in_zmq_buffer_depth);
        if (!in_queue_->start())
        {
            LOGGER_ERROR("[proc] ZMQ input queue start() failed (endpoint='{}')",
                         config_.zmq_in_endpoint);
            return false;
        }
        in_q_ = in_queue_.get();
    }
    else if (auto *zmq_in = in_consumer_->queue())
    {
        if (config_.verify_checksum)
            LOGGER_WARN("[proc] verify_checksum=true has no effect on ZMQ input transport "
                        "(TCP provides integrity); consider SHM input if checksum is needed");
        in_q_ = zmq_in;
    }
    else
    {
        auto *in_shm = in_consumer_->shm();
        if (in_shm == nullptr)
        {
            LOGGER_ERROR("[proc] Input SHM unavailable (in_channel='{}')", config_.in_channel);
            return false;
        }
        in_queue_ = hub::ShmQueue::from_consumer_ref(
            *in_shm, in_schema_slot_size_, core_.schema_fz_size, config_.in_channel);
        if (!in_queue_->start())
        {
            LOGGER_ERROR("[proc] SHM input queue start() failed (channel='{}')",
                         config_.in_channel);
            return false;
        }
        if (config_.verify_checksum)
            in_queue_->set_verify_checksum(true, core_.has_fz);
        in_q_ = in_queue_.get();
    }

    // Output queue.
    if (auto *zmq_out = out_producer_->queue())
    {
        out_q_ = zmq_out;
    }
    else
    {
        auto *out_shm = out_producer_->shm();
        if (out_shm == nullptr)
        {
            LOGGER_ERROR("[proc] Output SHM unavailable (out_channel='{}')",
                         config_.out_channel);
            return false;
        }
        out_queue_ = hub::ShmQueue::from_producer_ref(
            *out_shm, out_schema_slot_size_, core_.schema_fz_size, config_.out_channel);
        out_queue_->start();
        out_q_ = out_queue_.get();
    }
    out_q_->set_checksum_options(config_.update_checksum, core_.has_fz);

    LOGGER_INFO("[proc] Processor started: '{}' -> '{}'",
                config_.in_channel, config_.out_channel);

    // ── Startup coordination (HEP-0023) ─────────────────────────────────────
    if (!scripting::wait_for_roles(out_messenger_, config_.wait_for_roles, "[proc]"))
        return false;

    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ProcessorRoleHost::teardown_infrastructure_()
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

    // Null raw pointers BEFORE destroying owned queues (prevent dangling).
    in_q_  = nullptr;
    out_q_ = nullptr;

    if (in_queue_)
    {
        in_queue_->stop();
        in_queue_.reset();
    }
    if (out_queue_)
    {
        out_queue_->stop();
        out_queue_.reset();
    }

    // Deregister hub-dead callbacks on both messengers.
    in_messenger_.on_hub_dead(nullptr);
    out_messenger_.on_hub_dead(nullptr);

    // Stop/close producer.
    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }

    // Stop/close consumer.
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }
}

// ============================================================================
// run_data_loop_ — THE UNIFIED PROCESSOR LOOP (tech draft §5)
// ============================================================================

void ProcessorRoleHost::run_data_loop_()
{
    if (!in_q_ || !out_q_)
    {
        LOGGER_ERROR("[proc] run_data_loop_: transport queues not initialized — aborting");
        core_.running_threads.store(false, std::memory_order_release);
        return;
    }

    // --- Setup ---
    const double period_us =
        static_cast<double>(config_.target_period_ms) * kUsPerMs;
    const bool is_max_rate = (config_.loop_timing == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, kDefaultQueueIoWaitRatio);
    // Acquire takes milliseconds; convert with rounding up to avoid 0ms.
    const auto short_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            short_timeout_us + std::chrono::microseconds{999});

    const bool drop_mode = (config_.overflow_policy == OverflowPolicy::Drop);

    const size_t in_sz  = in_q_->item_size();
    const size_t out_sz = out_q_->item_size();

    // Flexzone pointers — valid after queue start.
    void       *fz_ptr  = core_.has_fz ? out_q_->write_flexzone() : nullptr;
    const size_t fz_sz  = core_.has_fz ? out_q_->flexzone_size()  : 0;
    const char *fz_type = core_.has_fz ? "FlexFrame" : nullptr;

    // First cycle: no deadline — fire immediately.
    auto deadline = Clock::time_point::max();

    // Input-hold state for Block mode (§5.3 of loop_design_unified.md).
    // When Block mode output fails, held_input retains the input pointer
    // across cycles. SHM: lock held, pointer valid. ZMQ: current_read_buf_
    // unchanged, pointer valid. See §5.1 for transport semantics.
    const void *held_input = nullptr;

    // --- Outer loop ---
    while (core_.running_threads.load(std::memory_order_acquire) &&
           !core_.shutdown_requested.load(std::memory_order_acquire) &&
           !core_.critical_error_.load(std::memory_order_relaxed))
    {
        // Check external shutdown flag.
        if (core_.g_shutdown && core_.g_shutdown->load(std::memory_order_relaxed))
            break;

        const auto cycle_start = Clock::now();

        // --- Step A: Acquire INPUT (skip if held from previous cycle) ---
        //
        // If held_input is set (Block mode, previous output failed),
        // reuse the held data — don't re-acquire.
        //
        if (held_input == nullptr)
        {
            while (true)
            {
                held_input = in_q_->read_acquire(short_timeout);
                if (held_input != nullptr)
                    break; // got input

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
        }

        // --- Step B: Acquire OUTPUT (only if input available) ---
        //
        // No input → no output attempt. Script called with both nil.
        // Input available → try output with policy-dependent timeout:
        //   Drop: 0ms (non-blocking)
        //   Block: max(remaining_until_deadline, short_timeout)
        //
        void *out_buf = nullptr;
        if (held_input != nullptr)
        {
            if (drop_mode)
            {
                out_buf = out_q_->write_acquire(std::chrono::milliseconds{0});
            }
            else
            {
                // Block mode: use remaining cycle budget, floor = short_timeout.
                auto output_timeout = short_timeout;
                if (deadline != Clock::time_point::max())
                {
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - Clock::now());
                    if (remaining > short_timeout)
                        output_timeout = remaining;
                }
                out_buf = out_q_->write_acquire(output_timeout);
            }
        }

        // --- Step C: Deadline wait (FixedRate) ---
        // Skip sleep when deadline is max() (first cycle) or MaxRate.
        if (!is_max_rate &&
            deadline != Clock::time_point::max() && Clock::now() < deadline)
        {
            std::this_thread::sleep_until(deadline);
        }

        // Safety check after potential sleep: shutdown may have been requested.
        if (!core_.running_threads.load(std::memory_order_relaxed) ||
            core_.shutdown_requested.load(std::memory_order_relaxed) ||
            core_.critical_error_.load(std::memory_order_relaxed))
        {
            if (held_input != nullptr)
            {
                in_q_->read_release();
                held_input = nullptr;
            }
            if (out_buf != nullptr)
                out_q_->write_discard();
            break;
        }

        // --- Step D: Drain everything right before script call ---
        auto msgs = core_.drain_messages();
        drain_inbox_sync_();

        // --- Step E: Prepare and invoke callback ---
        if (out_buf != nullptr)
            std::memset(out_buf, 0, out_sz);

        // Re-read flexzone pointer each cycle (ShmQueue may move it).
        if (core_.has_fz)
            fz_ptr = out_q_->write_flexzone();

        InvokeResult result =
            engine_->invoke_process(held_input, in_sz, out_buf, out_sz,
                                    fz_ptr, fz_sz, fz_type, msgs);

        // --- Step F: Commit/discard OUTPUT, release/hold INPUT ---
        //
        // Output:
        if (out_buf != nullptr)
        {
            if (result == InvokeResult::Commit)
            {
                out_q_->write_commit();
                out_written_.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                out_q_->write_discard();
                out_drops_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else if (held_input != nullptr)
        {
            // Had input but no output slot — count as drop.
            out_drops_.fetch_add(1, std::memory_order_relaxed);
        }

        // Input: release or hold depending on output success + policy.
        if (held_input != nullptr)
        {
            if (out_buf != nullptr || drop_mode)
            {
                // Normal: data processed or dropped. Advance input.
                in_q_->read_release();
                held_input = nullptr;
                in_received_.fetch_add(1, std::memory_order_relaxed);
            }
            // else: Block mode + output failed → keep held_input for next cycle.
            // SHM: lock still held, pointer valid.
            // ZMQ: current_read_buf_ unchanged, pointer valid.
        }

        if (result == InvokeResult::Error)
        {
            // script_errors already incremented by engine.
            if (config_.stop_on_script_error)
                core_.shutdown_requested.store(true, std::memory_order_release);
        }

        // --- Step G: Metrics + next deadline ---
        const auto now     = Clock::now();
        const auto work_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cycle_start).count());
        last_cycle_work_us_.store(work_us, std::memory_order_relaxed);
        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        deadline = compute_next_deadline(config_.loop_timing, deadline, cycle_start, period_us);
    }

    // Clean up held input on loop exit.
    if (held_input != nullptr)
    {
        in_q_->read_release();
        held_input = nullptr;
    }

    LOGGER_INFO("[proc] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                core_.critical_error_.load());
}

// ============================================================================
// run_ctrl_thread_ — polls both producer and consumer ZMQ sockets, sends heartbeats
// ============================================================================

void ProcessorRoleHost::run_ctrl_thread_()
{
    scripting::ZmqPollLoop loop{core_, "proc:" + config_.processor_uid};
    loop.sockets = {
        {out_producer_->peer_ctrl_socket_handle(),
         [&] { out_producer_->handle_peer_events_nowait(); }},
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
    };
    // Consumer data socket is only needed when data comes via broker relay (SHM transport).
    if (config_.in_transport != Transport::Zmq && in_consumer_->data_transport() != "zmq")
    {
        loop.sockets.push_back(
            {in_consumer_->data_zmq_socket_handle(),
             [&] { in_consumer_->handle_data_events_nowait(); }});
    }
    loop.get_iteration = [&] {
        return iteration_count_.load(std::memory_order_relaxed);
    };
    loop.periodic_tasks.emplace_back(
        [&] {
            out_messenger_.enqueue_heartbeat(config_.out_channel,
                                             snapshot_metrics_json());
        },
        config_.heartbeat_interval_ms);
    loop.run();
}

// ============================================================================
// drain_inbox_sync_ — drain all inbox messages non-blocking
// ============================================================================

void ProcessorRoleHost::drain_inbox_sync_()
{
    scripting::drain_inbox_sync(inbox_queue_.get(), engine_.get(), inbox_type_name_);
}

// ============================================================================
// snapshot_metrics_json — for heartbeat reporting
// ============================================================================

nlohmann::json ProcessorRoleHost::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]        = in_received_.load(std::memory_order_relaxed);
    base["out_written"]        = out_written_.load(std::memory_order_relaxed);
    base["drops"]              = out_drops_.load(std::memory_order_relaxed);
    base["script_errors"]      = engine_ ? engine_->script_error_count() : 0;
    base["last_cycle_work_us"] = last_cycle_work_us_.load(std::memory_order_relaxed);
    base["loop_overrun_count"] = 0; // overwritten below if SHM is available

    if (out_producer_.has_value())
    {
        base["ctrl_queue_dropped"] = out_producer_->ctrl_queue_dropped();
    }

    // Use our own iteration_count (always available, regardless of transport).
    base["iteration_count"] = iteration_count_.load(std::memory_order_relaxed);

    if (out_producer_.has_value())
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — shm() is non-const but we only read
        if (const auto *shm = const_cast<hub::Producer &>(*out_producer_).shm();
            shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["loop_overrun_count"] = m.overrun_count;
            base["last_iteration_us"]  = m.last_iteration_us;
            base["max_iteration_us"]   = m.max_iteration_us;
            base["last_slot_work_us"]  = m.last_slot_work_us;
            base["last_slot_wait_us"]  = m.last_slot_wait_us;
            base["period_ms"]          = m.period_ms;
        }
    }
    return base;
}

} // namespace pylabhub::processor
