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
#include "processor_fields.hpp"

#include "plh_datahub.hpp"
#include "plh_datahub_client.hpp"
#include "utils/metrics_json.hpp"

#include "role_host_helpers.hpp"
#include "zmq_poll_loop.hpp"
#include "schema_utils.hpp"

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
    core_.request_stop();
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
    if (!engine_->initialize("proc", &core_))
    {
        LOGGER_ERROR("[proc] Engine initialize() failed");
        engine_->finalize(); // Clean up any partial state (e.g. live interpreter).
        ready_promise_.set_value(false);
        return;
    }

    // Warn if script type was not explicitly set in config.
    if (!config_.script().type_explicit)
    {
        LOGGER_WARN("[proc] 'script.type' not set in config — defaulting to '{}'. "
                    "Set \"script\": {{\"type\": \"{}\"}} explicitly.",
                    config_.script().type, config_.script().type);
    }

    // Step 2: Load script and extract callbacks.
    const std::filesystem::path base_path =
        config_.script().path.empty() ? std::filesystem::current_path()
                                    : std::filesystem::weakly_canonical(config_.script().path);
    const std::filesystem::path script_dir = base_path / "script" / config_.script().type;
    const char *entry_point =
        (config_.script().type == "lua") ? "init.lua" : "__init__.py";

    if (!engine_->load_script(script_dir, entry_point, "on_process"))
    {
        LOGGER_ERROR("[proc] Engine load_script() failed");
        core_.set_script_load_ok(false);
        engine_->finalize();
        ready_promise_.set_value(false);
        return;
    }
    core_.set_script_load_ok(true);

    // Step 3: Resolve schemas and register slot types.
    scripting::SchemaSpec out_fz_local;
    scripting::SchemaSpec in_fz_local;
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
            in_slot_spec_    = scripting::resolve_schema(
                pf.in_slot_schema_json, false, "proc", schema_dirs);
            out_slot_spec_   = scripting::resolve_schema(
                pf.out_slot_schema_json, false, "proc", schema_dirs);
            in_fz_local = scripting::resolve_schema(
                pf.in_flexzone_schema_json, true, "proc", schema_dirs);
            out_fz_local = scripting::resolve_schema(
                pf.out_flexzone_schema_json, true, "proc", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[proc] Schema parse error: {}", e.what());
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
        // in_fz_spec stored in core_ after registration (below).
    }

    // Determine packing: input and output may use different ZMQ packing.
    const std::string in_packing =
        config_.in_transport().zmq_packing.empty() ? "aligned" : config_.in_transport().zmq_packing;
    const std::string out_packing =
        config_.out_transport().zmq_packing.empty() ? "aligned" : config_.out_transport().zmq_packing;

    // Compute sizes from schema (authoritative — infrastructure owns layout).
    in_schema_slot_size_  = scripting::compute_schema_size(in_slot_spec_, in_packing);
    out_schema_slot_size_ = scripting::compute_schema_size(out_slot_spec_, out_packing);
    {
        size_t out_fz_size = scripting::compute_schema_size(out_fz_local, out_packing);
        out_fz_size = (out_fz_size + 4095U) & ~size_t{4095U};
        core_.set_out_fz_spec(scripting::SchemaSpec{out_fz_local}, out_fz_size);
    }
    {
        size_t in_fz_size = scripting::compute_schema_size(in_fz_local, in_packing);
        in_fz_size = (in_fz_size + 4095U) & ~size_t{4095U};
        core_.set_in_fz_spec(scripting::SchemaSpec{in_fz_local}, in_fz_size);
    }

    // Register slot and flexzone types with engine (engine validates struct matches).
    if (in_slot_spec_.has_schema)
    {
        if (!engine_->register_slot_type(in_slot_spec_, "InSlotFrame", in_packing))
        {
            LOGGER_ERROR("[proc] Failed to register InSlotFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
    }
    if (out_slot_spec_.has_schema)
    {
        if (!engine_->register_slot_type(out_slot_spec_, "OutSlotFrame", out_packing))
        {
            LOGGER_ERROR("[proc] Failed to register OutSlotFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
    }
    if (out_fz_local.has_schema)
    {
        if (!engine_->register_slot_type(out_fz_local, "OutFlexFrame", out_packing))
        {
            LOGGER_ERROR("[proc] Failed to register OutFlexFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
    }
    if (in_fz_local.has_schema)
    {
        if (!engine_->register_slot_type(in_fz_local, "InFlexFrame", in_packing))
        {
            LOGGER_ERROR("[proc] Failed to register InFlexFrame type");
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }
    }

    // Resolve and register inbox schema (alongside slot/fz above).
    scripting::SchemaSpec inbox_spec_local;
    size_t inbox_schema_slot_size = 0;
    if (config_.inbox().has_inbox())
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

        try
        {
            inbox_spec_local = scripting::resolve_schema(
                config_.inbox().schema_json, false, "proc", schema_dirs);
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[proc] Inbox schema parse error: {}", e.what());
            engine_->finalize();
            ready_promise_.set_value(false);
            return;
        }

        if (inbox_spec_local.has_schema)
        {
            const std::string inbox_packing =
                inbox_spec_local.packing.empty()
                    ? config_.in_transport().zmq_packing
                    : inbox_spec_local.packing;
            if (!engine_->register_slot_type(inbox_spec_local, "InboxFrame",
                                              inbox_packing.empty() ? "aligned" : inbox_packing))
            {
                LOGGER_ERROR("[proc] Failed to register InboxFrame type");
                engine_->finalize();
                ready_promise_.set_value(false);
                return;
            }
            inbox_schema_slot_size = engine_->type_sizeof("InboxFrame");
        }
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
    if (!setup_infrastructure_(inbox_spec_local))
    {
        engine_->finalize();
        teardown_infrastructure_();
        ready_promise_.set_value(false);
        return;
    }

    // Validate inbox type size matches queue decode buffer size.
    if (inbox_schema_slot_size > 0 && inbox_queue_ &&
        inbox_queue_->item_size() != inbox_schema_slot_size)
    {
        const std::string inbox_packing =
            inbox_spec_local.packing.empty()
                ? config_.in_transport().zmq_packing
                : inbox_spec_local.packing;
        LOGGER_ERROR("[proc] InboxFrame size mismatch: engine type_sizeof={} "
                     "but InboxQueue item_size={} (packing='{}') — "
                     "check schema/packing consistency",
                     inbox_schema_slot_size, inbox_queue_->item_size(),
                     inbox_packing.empty() ? "aligned" : inbox_packing);
        engine_->finalize();
        teardown_infrastructure_();
        ready_promise_.set_value(false);
        return;
    }

    // Step 5: Build RoleContext and engine API.
    scripting::RoleContext ctx;
    ctx.role_tag     = "proc";
    ctx.uid          = config_.identity().uid;
    ctx.name         = config_.identity().name;
    ctx.channel      = config_.in_channel();
    ctx.out_channel  = config_.out_channel();
    ctx.log_level    = config_.identity().log_level;
    ctx.script_dir   = script_dir.string();
    ctx.role_dir     = config_.base_dir().string();
    ctx.messenger    = &out_messenger_;
    ctx.producer     = out_producer_.has_value() ? &(*out_producer_) : nullptr;
    ctx.consumer     = in_consumer_.has_value() ? &(*in_consumer_) : nullptr;
    ctx.inbox_queue  = inbox_queue_.get();
    ctx.checksum_policy = config_.checksum().policy;
    ctx.core         = &core_;
    ctx.stop_on_script_error = config_.script().stop_on_script_error;

    if (!engine_->build_api(ctx))
    {
        LOGGER_ERROR("[proc] build_api failed — aborting role start");
        engine_->finalize();
        teardown_infrastructure_();
        ready_promise_.set_value(false);
        return;
    }

    // Step 6: invoke on_init.
    engine_->invoke_on_init();

    // Sync flexzone checksum after on_init (user may have written to flexzone).
    if (out_producer_.has_value() && core_.has_out_fz())
        out_producer_->sync_flexzone_checksum();

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
// setup_infrastructure_ — dual broker connect, consumer + producer, wire events
// ============================================================================

bool ProcessorRoleHost::setup_infrastructure_(const scripting::SchemaSpec &inbox_spec)
{
    // ── Consumer side (in_channel) ──────────────────────────────────────────
    hub::ConsumerOptions in_opts;
    in_opts.channel_name         = config_.in_channel();
    in_opts.shm_shared_secret    = config_.in_shm().enabled ? config_.in_shm().secret : 0u;
    in_opts.expected_schema_hash = scripting::compute_schema_hash(
                                       in_slot_spec_, core_.in_fz_spec());
    in_opts.consumer_uid         = config_.identity().uid;
    in_opts.consumer_name        = config_.identity().name;
    in_opts.zmq_schema           = scripting::schema_spec_to_zmq_fields(in_slot_spec_);
    in_opts.zmq_packing          = config_.in_transport().zmq_packing;
    in_opts.zmq_buffer_depth     = config_.in_transport().zmq_buffer_depth;
    // Per-role checksum policy — same value on both input and output (see config_single_truth.md).
    in_opts.checksum_policy      = config_.checksum().policy;
    in_opts.flexzone_checksum    = config_.checksum().flexzone && core_.has_in_fz();
    // queue_period_us removed: loop policy set by establish_channel().
    in_opts.ctrl_queue_max_depth = config_.monitoring().ctrl_queue_max_depth;
    in_opts.peer_dead_timeout_ms = config_.monitoring().peer_dead_timeout_ms;

    // Transport declaration — broker validates mismatch.
    const bool in_is_zmq = (config_.in_transport().transport == config::Transport::Zmq);
    in_opts.queue_type = in_is_zmq ? "zmq" : "shm";

    const auto &in_ep  = config_.in_hub().broker;
    const auto &in_pub = config_.in_hub().broker_pubkey;
    if (!in_ep.empty())
    {
        if (!in_messenger_.connect(in_ep, in_pub,
                                   config_.auth().client_pubkey, config_.auth().client_seckey))
        {
            LOGGER_ERROR("[proc] in_messenger broker connect failed ({}); aborting", in_ep);
            return false;
        }
    }

    auto maybe_consumer = hub::Consumer::connect(in_messenger_, in_opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[proc] Failed to connect consumer to in_channel '{}'",
                     config_.in_channel());
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    // Graceful shutdown: queue channel_closing as a regular event message (input side).
    in_consumer_->on_channel_closing([this]() {
        LOGGER_INFO("[proc] CHANNEL_CLOSING_NOTIFY received (in_channel), queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        msg.details["channel"] = config_.in_channel();
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired (input side).
    in_consumer_->on_force_shutdown([this]() {
        LOGGER_WARN("[proc] FORCE_SHUTDOWN received (in_channel), forcing immediate shutdown");
        core_.request_stop();
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
    out_opts.channel_name  = config_.out_channel();
    out_opts.pattern       = hub::ChannelPattern::PubSub;
    out_opts.has_shm       = config_.out_shm().enabled;
    out_opts.schema_hash   = scripting::compute_schema_hash(out_slot_spec_, core_.out_fz_spec());
    out_opts.role_name     = config_.identity().name;
    out_opts.role_uid      = config_.identity().uid;
    // Per-role checksum policy — same value on both input and output (see config_single_truth.md).
    out_opts.checksum_policy    = config_.checksum().policy;
    out_opts.flexzone_checksum  = config_.checksum().flexzone && core_.has_out_fz();
    out_opts.ctrl_queue_max_depth = config_.monitoring().ctrl_queue_max_depth;
    out_opts.peer_dead_timeout_ms = config_.monitoring().peer_dead_timeout_ms;

    // SHM config (output side).
    if (config_.out_shm().enabled)
    {
        out_opts.shm_config.shared_secret        = config_.out_shm().secret;
        out_opts.shm_config.ring_buffer_capacity = config_.out_shm().slot_count;
        out_opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        out_opts.shm_config.consumer_sync_policy = config_.out_shm().sync_policy;
        out_opts.shm_config.checksum_policy      = config_.checksum().policy;
        out_opts.shm_config.flex_zone_size       = core_.out_schema_fz_size();

        out_opts.shm_config.physical_page_size = hub::system_page_size();
        out_opts.shm_config.logical_unit_size  =
            (out_schema_slot_size_ == 0) ? 1 : out_schema_slot_size_;
    }

    // HEP-CORE-0021: for ZMQ output, register as a ZMQ Virtual Channel Node.
    if (config_.out_transport().transport == config::Transport::Zmq)
    {
        out_opts.has_shm           = false;
        out_opts.data_transport    = "zmq";
        out_opts.zmq_node_endpoint = config_.out_transport().zmq_endpoint;
        out_opts.zmq_bind          = config_.out_transport().zmq_bind;
        out_opts.zmq_schema        = scripting::schema_spec_to_zmq_fields(out_slot_spec_);
        out_opts.zmq_packing       = config_.out_transport().zmq_packing;
        out_opts.zmq_buffer_depth  = config_.out_transport().zmq_buffer_depth;
        out_opts.zmq_overflow_policy =
            (config_.out_transport().zmq_overflow_policy == "block")
                ? hub::OverflowPolicy::Block
                : hub::OverflowPolicy::Drop;
    }

    // ── Inbox facility (optional) ───────────────────────────────────────────
    if (config_.inbox().has_inbox())
    {
        const std::string inbox_packing =
            inbox_spec.packing.empty()
                ? config_.in_transport().zmq_packing
                : inbox_spec.packing;

        // Endpoint validated by parse_inbox_config(); default is tcp://127.0.0.1:0.
        const std::string &ep = config_.inbox().endpoint;
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(inbox_spec);

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

        const int inbox_rcvhwm = (config_.inbox().overflow_policy == "block")
            ? 0
            : static_cast<int>(config_.inbox().buffer_depth);

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
        inbox_queue_->set_checksum_policy(config_.checksum().policy);
        LOGGER_INFO("[proc] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());

        out_opts.inbox_endpoint    = inbox_queue_->actual_endpoint();
        out_opts.inbox_schema_json = spec_json.dump();
        out_opts.inbox_packing     = inbox_packing.empty() ? "aligned" : inbox_packing;
        out_opts.inbox_checksum    = config::checksum_policy_to_string(config_.checksum().policy);
    }

    // ── Output broker connect ───────────────────────────────────────────────
    const auto &out_ep  = config_.out_hub().broker;
    const auto &out_pub = config_.out_hub().broker_pubkey;
    if (!out_ep.empty())
    {
        if (!out_messenger_.connect(out_ep, out_pub,
                                    config_.auth().client_pubkey, config_.auth().client_seckey))
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
                     config_.out_channel());
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    // Metrics reset moved to after queue creation (reset_metrics() on queue).

    // Graceful shutdown: queue channel_closing event (output side).
    out_producer_->on_channel_closing([this]() {
        IncomingMessage msg;
        msg.event = "channel_closing";
        msg.details["channel"] = config_.out_channel();
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired (output side).
    out_producer_->on_force_shutdown([this]() {
        core_.request_stop();
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

    out_messenger_.on_consumer_died(config_.out_channel(),
        [this](uint64_t pid, std::string reason) {
            LOGGER_INFO("[proc] broker_notify: consumer_died pid={} reason={}",
                        pid, reason);
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(config_.out_channel(),
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

    if (!config_.out_channel().empty())
    {
        out_messenger_.suppress_periodic_heartbeat(config_.out_channel());
        out_messenger_.enqueue_heartbeat(config_.out_channel());
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
                    config_.monitoring().peer_dead_timeout_ms);
        core_.set_stop_reason(scripting::RoleHostCore::StopReason::PeerDead);
        core_.request_stop();
    });

    out_producer_->on_peer_dead([this]() {
        LOGGER_WARN("[proc] peer-dead: downstream consumer silent for {} ms; triggering shutdown",
                    config_.monitoring().peer_dead_timeout_ms);
        core_.set_stop_reason(scripting::RoleHostCore::StopReason::PeerDead);
        core_.request_stop();
    });

    auto hub_dead_cb = [this]() {
        LOGGER_WARN("[proc] hub-dead: broker connection lost; triggering shutdown");
        core_.set_stop_reason(scripting::RoleHostCore::StopReason::HubDead);
        core_.request_stop();
    };
    in_messenger_.on_hub_dead(hub_dead_cb);
    out_messenger_.on_hub_dead(hub_dead_cb);

    // ── Create data queues ─────────────────────────────────────────────────

    // --- Start and configure data queues ---
    if (!in_consumer_->start_queue())
    {
        LOGGER_ERROR("[proc] Input start_queue() failed (in_channel='{}')",
                     config_.in_channel());
        return false;
    }
    if (!out_producer_->start_queue())
    {
        LOGGER_ERROR("[proc] Output start_queue() failed (out_channel='{}')",
                     config_.out_channel());
        return false;
    }
    // Reset metrics (checksum and period already configured via Options).
    in_consumer_->reset_queue_metrics();
    out_producer_->reset_queue_metrics();
    core_.set_configured_period(static_cast<uint64_t>(config_.timing().period_us));

    LOGGER_INFO("[proc] Processor started: '{}' -> '{}'",
                config_.in_channel(), config_.out_channel());

    // ── Startup coordination (HEP-0023) ─────────────────────────────────────
    if (!scripting::wait_for_roles(out_messenger_, config_.startup().wait_for_roles, "[proc]"))
        return false;

    return true;
}

// ============================================================================
// teardown_infrastructure_ — reverse of setup
// ============================================================================

void ProcessorRoleHost::teardown_infrastructure_()
{
    // ctrl_thread_ already joined before finalize (shutdown sequence).

    core_.clear_inbox_cache();

    // Stop inbox_queue_ (if exists).
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
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
    if (!in_consumer_.has_value() || !out_producer_.has_value())
    {
        LOGGER_ERROR("[proc] run_data_loop_: transport queues not initialized — aborting");
        core_.set_running(false);
        return;
    }

    const auto &tc  = config_.timing();
    const auto &sc  = config_.script();

    // --- Setup ---
    const double period_us = tc.period_us;
    const bool is_max_rate = (tc.loop_timing == LoopTimingPolicy::MaxRate);
    const auto short_timeout_us = compute_short_timeout(period_us, tc.queue_io_wait_timeout_ratio);
    // Acquire takes milliseconds; convert with rounding up to avoid 0ms.
    const auto short_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            short_timeout_us + std::chrono::microseconds{999});

    const bool drop_mode = (config_.out_transport().zmq_overflow_policy == "drop");

    const size_t in_sz  = in_consumer_->queue_item_size();
    const size_t out_sz = out_producer_->queue_item_size();

    // Output flexzone pointers — valid after queue start.
    void       *out_fz_ptr = core_.has_out_fz() ? out_producer_->write_flexzone() : nullptr;
    const size_t out_fz_sz = core_.has_out_fz() ? out_producer_->flexzone_size()  : 0;

    // Input flexzone — mutable per HEP-0002 TABLE 1 (user-managed coordination).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    void       *in_fz_ptr = core_.has_in_fz() ? const_cast<void *>(in_consumer_->read_flexzone()) : nullptr;
    const size_t in_fz_sz = core_.has_in_fz() ? in_consumer_->flexzone_size() : 0;

    // First cycle: no deadline — fire immediately.
    auto deadline = Clock::time_point::max();

    // Input-hold state for Block mode (§5.3 of loop_design_unified.md).
    // When Block mode output fails, held_input retains the input pointer
    // across cycles. SHM: lock held, pointer valid. ZMQ: current_read_buf_
    // unchanged, pointer valid. See §5.1 for transport semantics.
    const void *held_input = nullptr;

    // --- Outer loop ---
    while (core_.is_running() &&
           !core_.is_shutdown_requested() &&
           !core_.is_critical_error())
    {
        // Check external shutdown flag.
        if (core_.is_process_exit_requested())
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
                held_input = in_consumer_->read_acquire(short_timeout);
                if (held_input != nullptr)
                    break; // got input

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
                out_buf = out_producer_->write_acquire(std::chrono::milliseconds{0});
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
                out_buf = out_producer_->write_acquire(output_timeout);
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
        if (!core_.is_running() ||
            core_.is_shutdown_requested() ||
            core_.is_critical_error())
        {
            if (held_input != nullptr)
            {
                in_consumer_->read_release();
                held_input = nullptr;
            }
            if (out_buf != nullptr)
                out_producer_->write_discard();
            break;
        }

        // --- Step D: Drain everything right before script call ---
        auto msgs = core_.drain_messages();
        drain_inbox_sync_();

        // --- Step E: Prepare and invoke callback ---
        if (out_buf != nullptr)
            std::memset(out_buf, 0, out_sz);

        // Re-read flexzone pointers each cycle (ShmQueue may move them).
        if (core_.has_out_fz())
            out_fz_ptr = out_producer_->write_flexzone();
        if (core_.has_in_fz())
            in_fz_ptr = const_cast<void *>(in_consumer_->read_flexzone());

        InvokeResult result =
            engine_->invoke_process(
                InvokeRx{held_input, in_sz, in_fz_ptr, in_fz_sz},
                InvokeTx{out_buf, out_sz, out_fz_ptr, out_fz_sz},
                msgs);

        // --- Step F: Commit/discard OUTPUT, release/hold INPUT ---
        //
        // Output:
        if (out_buf != nullptr)
        {
            if (result == InvokeResult::Commit)
            {
                out_producer_->write_commit();
                core_.inc_out_written();
            }
            else
            {
                out_producer_->write_discard();
                core_.inc_drops();
            }
        }
        else if (held_input != nullptr)
        {
            // Had input but no output slot — count as drop.
            core_.inc_drops();
        }

        // Input: release or hold depending on output success + policy.
        if (held_input != nullptr)
        {
            if (out_buf != nullptr || drop_mode)
            {
                // Normal: data processed or dropped. Advance input.
                in_consumer_->read_release();
                held_input = nullptr;
                core_.inc_in_received();
            }
            // else: Block mode + output failed → keep held_input for next cycle.
            // SHM: lock still held, pointer valid.
            // ZMQ: current_read_buf_ unchanged, pointer valid.
        }

        if (result == InvokeResult::Error)
        {
            // script_errors already incremented by engine.
            if (sc.stop_on_script_error)
                core_.request_stop();
        }

        // --- Step G: Metrics + next deadline ---
        const auto now     = Clock::now();
        const auto work_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cycle_start).count());
        core_.set_last_cycle_work_us(work_us);
        core_.inc_iteration_count();
        if (deadline != Clock::time_point::max() && now > deadline)
            core_.inc_loop_overrun();

        deadline = compute_next_deadline(tc.loop_timing, deadline, cycle_start, period_us);
    }

    // Clean up held input on loop exit.
    if (held_input != nullptr)
    {
        in_consumer_->read_release();
        held_input = nullptr;
    }

    LOGGER_INFO("[proc] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.is_running(), core_.is_shutdown_requested(),
                core_.is_critical_error());
}

// ============================================================================
// run_ctrl_thread_ — polls both producer and consumer ZMQ sockets, sends heartbeats
// ============================================================================

void ProcessorRoleHost::run_ctrl_thread_()
{
    scripting::ThreadEngineGuard engine_guard(*engine_);

    const bool has_heartbeat_cb = engine_->has_callback("on_heartbeat");

    scripting::ZmqPollLoop loop{core_, "proc:" + config_.identity().uid};
    loop.sockets = {
        {out_producer_->peer_ctrl_socket_handle(),
         [&] { out_producer_->handle_peer_events_nowait(); }},
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
    };
    // Consumer data socket is only needed when data comes via ZMQ relay (SHM transport
    // uses the DataBlock directly, not the data socket).
    if (in_consumer_->data_transport() != "zmq")
    {
        loop.sockets.push_back(
            {in_consumer_->data_zmq_socket_handle(),
             [&] { in_consumer_->handle_data_events_nowait(); }});
    }
    loop.get_iteration = [&] {
        return core_.iteration_count();
    };
    loop.periodic_tasks.emplace_back(
        [&] {
            out_messenger_.enqueue_heartbeat(config_.out_channel(),
                                             snapshot_metrics_json());
            if (has_heartbeat_cb)
                engine_->invoke("on_heartbeat");
        },
        config_.timing().heartbeat_interval_ms);
    loop.run();
}

// ============================================================================
// drain_inbox_sync_ — drain all inbox messages non-blocking
// ============================================================================

void ProcessorRoleHost::drain_inbox_sync_()
{
    scripting::drain_inbox_sync(inbox_queue_.get(), engine_.get());
}

// ============================================================================
// snapshot_metrics_json — for heartbeat reporting
// ============================================================================

nlohmann::json ProcessorRoleHost::snapshot_metrics_json() const
{
    nlohmann::json result;

    if (in_consumer_.has_value())
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, in_consumer_->queue_metrics());
        result["in_queue"] = std::move(q);
    }
    if (out_producer_.has_value())
    {
        nlohmann::json q;
        hub::queue_metrics_to_json(q, out_producer_->queue_metrics());
        result["out_queue"] = std::move(q);
    }

    {
        nlohmann::json lm;
        hub::loop_metrics_to_json(lm, core_.loop_metrics());
        result["loop"] = std::move(lm);
    }

    {
        uint64_t in_dropped  = in_consumer_.has_value()  ? in_consumer_->ctrl_queue_dropped()  : 0;
        uint64_t out_dropped = out_producer_.has_value() ? out_producer_->ctrl_queue_dropped() : 0;
        result["role"] = {
            {"in_received",        core_.in_received()},
            {"out_written",        core_.out_written()},
            {"drops",              core_.drops()},
            {"script_errors",      engine_ ? engine_->script_error_count() : 0},
            {"ctrl_queue_dropped", {{"input", in_dropped}, {"output", out_dropped}}}
        };
    }

    if (inbox_queue_)
    {
        nlohmann::json ib;
        hub::inbox_metrics_to_json(ib, inbox_queue_->inbox_metrics());
        result["inbox"] = std::move(ib);
    }

    return result;
}

} // namespace pylabhub::processor
