/**
 * @file processor_script_host.cpp
 * @brief ProcessorScriptHost — role-specific implementation.
 *
 * The common do_python_work() skeleton lives in PythonRoleHostBase.
 * This file provides the processor-specific virtual hook overrides:
 *  - Dual input/output channels with Consumer + Producer
 *  - Delegates data loop to hub::Processor
 *  - on_process callback dispatch
 */
#include "processor_script_host.hpp"

#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "zmq_poll_loop.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace py = pybind11;

namespace pylabhub::processor
{

using scripting::IncomingMessage;

namespace
{


/// Returns {commit, is_type_error}.  Wrong return type is counted as a script error.
std::pair<bool, bool> parse_on_process_return(const py::object &ret)
{
    if (ret.is_none())
        return {true, false};
    if (py::isinstance<py::bool_>(ret))
        return {ret.cast<bool>(), false};
    LOGGER_ERROR("[proc] on_process() must return bool or None — treating as discard");
    return {false, true};
}

} // anonymous namespace

// ============================================================================
// Destructor
// ============================================================================

ProcessorScriptHost::~ProcessorScriptHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void ProcessorScriptHost::set_config(ProcessorConfig config)
{
    config_ = std::move(config);
    python_venv_ = config_.python_venv;
}

// ============================================================================
// Virtual hooks — identity and script loading
// ============================================================================

void ProcessorScriptHost::wire_api_identity()
{
    api_.set_uid(config_.processor_uid);
    api_.set_name(config_.processor_name);
    api_.set_in_channel(config_.in_channel);
    api_.set_out_channel(config_.out_channel);
    api_.set_log_level(config_.log_level);
    api_.set_script_dir(config_.script_path);
    api_.set_role_dir(config_.role_dir);
    api_.set_shutdown_flag(core_.g_shutdown);
    api_.set_shutdown_requested(&core_.shutdown_requested);
    api_.set_stop_reason(&stop_reason_);
}

void ProcessorScriptHost::extract_callbacks(py::module_ &mod)
{
    py_on_process_ = py::getattr(mod, "on_process", py::none());
    py_on_init_    = py::getattr(mod, "on_init",    py::none());
    py_on_stop_    = py::getattr(mod, "on_stop",    py::none());
    // on_inbox is optional — only extract when inbox is configured
    if (config_.has_inbox() && py::hasattr(mod, "on_inbox"))
        py_on_inbox_ = mod.attr("on_inbox");
}

bool ProcessorScriptHost::has_required_callback() const
{
    return scripting::is_callable(py_on_process_);
}

// ============================================================================
// Virtual hooks — schema and validation
// ============================================================================

bool ProcessorScriptHost::build_role_types()
{
    using scripting::resolve_schema;

    // HEP-0011 §3.1: script.type should be explicit. Warn when defaulted.
    if (!config_.script_type_explicit)
        LOGGER_WARN("[proc] Config 'script.type' absent — defaulting to '{}'. "
                    "Set it explicitly to suppress this warning.", config_.script_type);

    try
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

        in_slot_spec_  = resolve_schema(config_.in_slot_schema_json,  false, "proc", schema_dirs);
        out_slot_spec_ = resolve_schema(config_.out_slot_schema_json, false, "proc", schema_dirs);
        core_.fz_spec  = resolve_schema(config_.flexzone_schema_json, true,  "proc", schema_dirs);
        if (config_.has_inbox())
            inbox_spec_ = resolve_schema(config_.inbox_schema_json, false, "proc", schema_dirs);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[proc] Schema parse error: {}", e.what());
        return false;
    }

    try
    {
        if (!build_schema_type_(in_slot_spec_, in_slot_type_, in_schema_slot_size_,
                                "InSlotFrame", /*readonly=*/true))
            return false;
        if (!build_schema_type_(out_slot_spec_, out_slot_type_, out_schema_slot_size_,
                                "OutSlotFrame"))
            return false;
        if (!build_flexzone_type_())
            return false;
        if (config_.has_inbox())
        {
            if (!build_schema_type_(inbox_spec_, inbox_type_, inbox_schema_slot_size_,
                                    "InboxFrame", /*readonly=*/true))
                return false;
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[proc] Failed to build Python schema types: {}", e.what());
        return false;
    }
    return true;
}

void ProcessorScriptHost::print_validate_layout()
{
    std::cout << "\nProcessor: " << config_.processor_uid << "\n";
    print_slot_layout_(in_slot_type_, in_slot_spec_, in_schema_slot_size_,
                       "  Input slot: InSlotFrame");
    print_slot_layout_(out_slot_type_, out_slot_spec_, out_schema_slot_size_,
                       "  Output slot: OutSlotFrame");
    print_slot_layout_(fz_type_, core_.fz_spec, core_.schema_fz_size,
                       "  FlexZone: FlexFrame");
    if (config_.has_inbox())
        print_slot_layout_(inbox_type_, inbox_spec_, inbox_schema_slot_size_,
                           "  Inbox: InboxFrame");
}

// ============================================================================
// Virtual hooks — lifecycle
// ============================================================================

bool ProcessorScriptHost::start_role()
{
    if (!build_role_types())
        return false;

    // ── Consumer side (in_channel) ──────────────────────────────────────────
    hub::ConsumerOptions in_opts;
    in_opts.channel_name         = config_.in_channel;
    in_opts.shm_shared_secret    = config_.in_shm_enabled ? config_.in_shm_secret : 0u;
    in_opts.expected_schema_hash = scripting::compute_schema_hash(
                                       in_slot_spec_, scripting::SchemaSpec{});
    in_opts.consumer_uid         = config_.processor_uid;
    in_opts.consumer_name        = config_.processor_name;
    in_opts.zmq_schema  = schema_spec_to_zmq_fields(in_slot_spec_, in_schema_slot_size_); // HEP-CORE-0021
    in_opts.zmq_packing = config_.in_zmq_packing;
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

    // Wire consumer-side events → IncomingMessage queue.
    // NOTE: The data payload may contain arbitrary binary bytes. We hex-encode it
    // to avoid UnicodeDecodeError when json_to_py() converts to py::str.
    in_consumer_->on_producer_message(
        [this](std::string_view type, std::span<const std::byte> data)
        {
            LOGGER_INFO("[proc] ctrl_msg: in_channel producer_message type='{}' size={}",
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
            LOGGER_INFO("[proc] broker_notify: in_channel channel_event event='{}' details={}",
                        event, details.dump());
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

    out_opts.ctrl_queue_max_depth = config_.ctrl_queue_max_depth;
    out_opts.peer_dead_timeout_ms = config_.peer_dead_timeout_ms;

    // HEP-CORE-0021: for ZMQ output, register as a ZMQ Virtual Channel Node.
    if (config_.out_transport == Transport::Zmq)
    {
        out_opts.has_shm           = false; // ZMQ node carries no SHM segment
        out_opts.data_transport    = "zmq";
        out_opts.zmq_node_endpoint = config_.zmq_out_endpoint;
        out_opts.zmq_bind          = config_.zmq_out_bind;
        out_opts.zmq_schema        = schema_spec_to_zmq_fields(out_slot_spec_, out_schema_slot_size_);
        out_opts.zmq_packing          = config_.out_zmq_packing;
        out_opts.zmq_buffer_depth     = config_.out_zmq_buffer_depth;
        // zmq_out_overflow_policy: explicit JSON field takes precedence;
        // falls back to general overflow_policy when absent (empty string).
        const bool zmq_drop = config_.zmq_out_overflow_policy.empty()
            ? (config_.overflow_policy == OverflowPolicy::Drop)
            : (config_.zmq_out_overflow_policy == "drop");
        out_opts.zmq_overflow_policy  = zmq_drop
                                            ? hub::OverflowPolicy::Drop
                                            : hub::OverflowPolicy::Block;
    }

    // ── Inbox facility (optional) — must be set up before Producer::create() ──
    // The actual_endpoint() must be set in out_opts so REG_REQ advertises it.
    if (config_.has_inbox())
    {
        const std::string ep = config_.inbox_endpoint.empty()
            ? "tcp://127.0.0.1:0"
            : config_.inbox_endpoint;
        // Prefer packing from the inbox schema itself; fall back to in_zmq_packing.
        const std::string packing = inbox_spec_.packing.empty()
            ? config_.in_zmq_packing
            : inbox_spec_.packing;

        auto zmq_fields = schema_spec_to_zmq_fields(inbox_spec_, inbox_schema_slot_size_);

        // Serialize inbox schema for REG_REQ advertisement.
        nlohmann::json spec_json;
        spec_json["fields"] = nlohmann::json::array();
        for (const auto &f : inbox_spec_.fields)
        {
            nlohmann::json fj = {{"name", f.name}, {"type", f.type_str}};
            if (f.count > 1)  fj["count"]  = f.count;
            if (f.length > 0) fj["length"] = f.length;
            spec_json["fields"].push_back(fj);
        }
        if (inbox_spec_.packing != "aligned") spec_json["packing"] = inbox_spec_.packing;

        const int inbox_rcvhwm = (config_.inbox_overflow_policy == "block")
            ? 0
            : static_cast<int>(config_.inbox_buffer_depth);
        inbox_queue_ = hub::InboxQueue::bind_at(ep, std::move(zmq_fields), packing, inbox_rcvhwm);
        if (!inbox_queue_ || !inbox_queue_->start())
        {
            LOGGER_ERROR("[proc] Failed to start InboxQueue at '{}'", ep);
            if (inbox_queue_) inbox_queue_.reset();
            return false; // PR-02: inbox start failure is fatal — matches producer behaviour
        }
        LOGGER_INFO("[proc] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());

        // Advertise inbox in REG_REQ so peers can discover it via ROLE_INFO_REQ.
        out_opts.inbox_endpoint    = inbox_queue_->actual_endpoint();
        out_opts.inbox_schema_json = spec_json.dump();
        out_opts.inbox_packing     = packing;
    }

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

    auto maybe_producer = hub::Producer::create(out_messenger_, out_opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[proc] Failed to create producer for out_channel '{}'",
                     config_.out_channel);
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    // Graceful shutdown: queue channel_closing event (output side).
    out_producer_->on_channel_closing([this]() {
        LOGGER_INFO("[proc] CHANNEL_CLOSING_NOTIFY received (out_channel), queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        msg.details["channel"] = config_.out_channel;
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired (output side).
    out_producer_->on_force_shutdown([this]() {
        LOGGER_WARN("[proc] FORCE_SHUTDOWN received (out_channel), forcing immediate shutdown");
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Helper: hex-encode a binary ZMQ identity so it's safe for JSON→py::str conversion.
    auto hex_identity = [](const std::string &raw) -> std::string
    {
        return format_tools::bytes_to_hex(raw);
    };

    // Wire producer-side peer events → IncomingMessage queue.
    out_producer_->on_consumer_joined(
        [this, hex_identity](const std::string &identity)
        {
            LOGGER_INFO("[proc] peer_event: out_channel consumer_joined identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_joined";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_left(
        [this, hex_identity](const std::string &identity)
        {
            LOGGER_INFO("[proc] peer_event: out_channel consumer_left identity={}",
                        hex_identity(identity));
            IncomingMessage msg;
            msg.event = "consumer_left";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    // Wire broker notifications for the output channel.
    out_messenger_.on_consumer_died(config_.out_channel,
        [this](uint64_t pid, std::string reason)
        {
            LOGGER_INFO("[proc] broker_notify: out_channel consumer_died pid={} reason={}",
                        pid, reason);
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(config_.out_channel,
        [this](std::string event, nlohmann::json details)
        {
            LOGGER_INFO("[proc] broker_notify: out_channel channel_event event='{}' details={}",
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

    // Route consumer ZMQ messages (from producer's peer side) to incoming queue.
    out_producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data)
        {
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
    // Processor has two peers: upstream producer (in_consumer_ side) and
    // downstream consumer (out_producer_ side). Either going silent is fatal.
    in_consumer_->on_peer_dead([this]() {
        LOGGER_WARN("[proc] peer-dead: upstream producer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        stop_reason_.store(static_cast<int>(scripting::StopReason::PeerDead), std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    out_producer_->on_peer_dead([this]() {
        LOGGER_WARN("[proc] peer-dead: downstream consumer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        stop_reason_.store(static_cast<int>(scripting::StopReason::PeerDead), std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Wire hub-dead for BOTH messengers — processor spans two hubs; either losing its
    // broker connection should trigger shutdown.
    auto hub_dead_cb = [this]() {
        LOGGER_WARN("[proc] hub-dead: broker connection lost; triggering shutdown");
        stop_reason_.store(static_cast<int>(scripting::StopReason::HubDead), std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    };
    in_messenger_.on_hub_dead(hub_dead_cb);
    out_messenger_.on_hub_dead(hub_dead_cb);

    // ── Create data queues ─────────────────────────────────────────────────
    // (Created before API/flexzone wiring so out_q_ is available for fz view.)
    //
    // Input: prefer direct ZMQ PULL when in_transport=="zmq" (config_.zmq_in_endpoint set),
    //        then broker-negotiated ZMQ from Consumer, otherwise owned SHM queue.
    if (config_.in_transport == Transport::Zmq)
    {
        // Direct ZMQ PULL — bypasses broker data relay; control plane (heartbeat /
        // shutdown) remains active via in_messenger_ / in_consumer_.
        auto zmq_fields = schema_spec_to_zmq_fields(in_slot_spec_, in_schema_slot_size_);
        in_queue_ = hub::ZmqQueue::pull_from(
            config_.zmq_in_endpoint, std::move(zmq_fields),
            config_.in_zmq_packing, config_.zmq_in_bind, config_.in_zmq_buffer_depth);
        in_queue_->start();
        in_q_ = in_queue_.get();
    }
    else if (auto *zmq_in = in_consumer_->queue())
    {
        // Broker-negotiated ZMQ: Consumer already owns and started the PULL socket.
        // TCP provides integrity; checksum is not applicable on the ZMQ path.
        if (config_.verify_checksum)
            LOGGER_WARN("[proc] verify_checksum=true has no effect on ZMQ input transport "
                        "(TCP provides integrity); consider SHM input if checksum is needed");
        in_q_ = zmq_in;
    }
    else
    {
        // SHM: create zero-copy owned ShmQueue from the Consumer's DataBlock.
        auto *in_shm = in_consumer_->shm();
        if (in_shm == nullptr)
        {
            LOGGER_ERROR("[proc] Input SHM unavailable (in_channel='{}')", config_.in_channel);
            return false;
        }
        in_queue_ = hub::ShmQueue::from_consumer_ref(
            *in_shm, in_schema_slot_size_, core_.schema_fz_size, config_.in_channel);
        in_queue_->start();
        if (config_.verify_checksum)
            in_queue_->set_verify_checksum(true, core_.has_fz);
        in_q_ = in_queue_.get();
    }

    // Output: use ZmqQueue owned by Producer (HEP-CORE-0021) if transport=="zmq",
    //         otherwise create an owned ShmQueue.
    if (auto *zmq_out = out_producer_->queue())
    {
        // ZMQ: Producer already created and started the PUSH socket (schema set via
        // out_opts.zmq_schema when ProducerOptions was constructed above).
        out_q_ = zmq_out;
    }
    else
    {
        // SHM: create owned ShmQueue from the Producer's DataBlock.
        auto *out_shm = out_producer_->shm();
        if (out_shm == nullptr)
        {
            LOGGER_ERROR("[proc] Output SHM unavailable (out_channel='{}')", config_.out_channel);
            return false;
        }
        auto shm_q = hub::ShmQueue::from_producer_ref(
            *out_shm, out_schema_slot_size_, core_.schema_fz_size, config_.out_channel);
        out_queue_ = std::move(shm_q);
        out_queue_->start();
        out_q_ = out_queue_.get();
    }
    out_q_->set_checksum_options(config_.update_checksum, core_.has_fz);

    // ── Wire API and output flexzone ────────────────────────────────────────
    try
    {
        // Ensure the embedded pybind11 module is imported so the ProcessorAPI type
        // is registered before py::cast.
        py::module_::import("pylabhub_processor");

        api_obj_ = py::cast(&api_, py::return_value_policy::reference);
        api_.set_producer(&*out_producer_);
        api_.set_consumer(&*in_consumer_);
        api_.set_messenger(&out_messenger_);

        // Flexzone from abstract write queue (nullptr for ZMQ transport).
        if (void *fz = out_q_->write_flexzone();
            fz != nullptr && out_q_->flexzone_size() > 0)
        {
            const size_t fz_sz = out_q_->flexzone_size();
            fz_mv_ = py::memoryview::from_memory(fz, static_cast<py::ssize_t>(fz_sz),
                                                 /*readonly=*/false);

            if (core_.fz_spec.exposure == scripting::SlotExposure::Ctypes)
            {
                fz_inst_ = fz_type_.attr("from_buffer")(fz_mv_);
            }
            else
            {
                py::module_ np = py::module_::import("numpy");
                if (!core_.fz_spec.numpy_shape.empty())
                {
                    py::list shape;
                    for (auto d : core_.fz_spec.numpy_shape) shape.append(d);
                    fz_inst_ = np.attr("ndarray")(shape, fz_type_, fz_mv_);
                }
                else
                {
                    const size_t items =
                        fz_sz / fz_type_.attr("itemsize").cast<size_t>();
                    fz_inst_ = np.attr("ndarray")(
                        py::make_tuple(static_cast<py::ssize_t>(items)), fz_type_, fz_mv_);
                }
            }
        }
        if (fz_inst_.is_none())
            fz_inst_ = py::none();

        api_.set_flexzone_obj(&fz_inst_);
    }
    catch (py::error_already_set &e)
    {
        LOGGER_ERROR("[proc] Failed to build flexzone view: {}", e.what());
        return false;
    }

    LOGGER_INFO("[proc] Processor started: '{}' → '{}'",
                config_.in_channel, config_.out_channel);

    // ── Create hub::Processor ───────────────────────────────────────────────
    hub::ProcessorOptions proc_opts;
    proc_opts.overflow_policy = (config_.overflow_policy == OverflowPolicy::Drop)
                                    ? hub::OverflowPolicy::Drop
                                    : hub::OverflowPolicy::Block;
    proc_opts.input_timeout = std::chrono::milliseconds{
        pylabhub::compute_slot_acquire_timeout(
            config_.slot_acquire_timeout_ms, config_.target_period_ms)};
    proc_opts.zero_fill_output = true;

    auto maybe_proc = hub::Processor::create(*in_q_, *out_q_, proc_opts);
    if (!maybe_proc.has_value())
    {
        LOGGER_ERROR("[proc] Failed to create hub::Processor");
        return false;
    }
    processor_ = std::move(maybe_proc);
    api_.set_in_queue(in_q_);
    api_.set_out_queue(out_q_);

    // ── Shared deadline state for both handler + timeout handler ────────────
    //
    // Both lambdas run on the same thread (hub::Processor::process_thread_),
    // so no synchronization is needed.  Deadline tracking ensures:
    //   - MaxRate:   on_process fires every iteration (no gating).
    //   - FixedRate: on_process fires at most once per period.
    //   - FixedRateWithCompensation: deadline advances by period (catch-up).
    const bool proc_is_fixed_rate =
        (config_.loop_timing != LoopTimingPolicy::MaxRate);
    const auto proc_period =
        std::chrono::milliseconds{config_.target_period_ms};

    // Shared mutable deadline — both lambdas capture by value (same pointer).
    auto next_dl = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now() + proc_period);

    // Helper: advance deadline after a successful on_process call.
    auto advance_deadline = [this, proc_is_fixed_rate, proc_period, next_dl]()
    {
        if (!proc_is_fixed_rate)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= *next_dl)
        {
            if (config_.loop_timing ==
                LoopTimingPolicy::FixedRateWithCompensation)
                *next_dl += proc_period;
            else
                *next_dl = now + proc_period;
        }
    };

    // ── Install type-erased handler (input data available) ───────────────
    //
    // Input data is ALWAYS processed — never dropped.  FixedRate pacing is
    // achieved by sleeping to the deadline AFTER processing (same pattern as
    // producer/consumer run_loop_), not by discarding input.
    processor_->set_raw_handler(
        [this, proc_is_fixed_rate, next_dl, advance_deadline](
            const void* in_data, const void* /*in_fz*/,
            void* out_data, void* /*out_fz*/) -> bool
        {
            auto msgs = core_.drain_messages();

            const size_t in_sz  = in_q_->item_size();
            const size_t out_sz = out_q_->item_size();

            bool commit = false;
            {
                py::gil_scoped_acquire g;
                try
                {
                    py::object in_sv  = make_in_slot_view_(in_data, in_sz);
                    py::object out_sv = make_out_slot_view_(out_data, out_sz);
                    py::list   mlst   = build_messages_list_(msgs);

                    commit = call_on_process_(in_sv, out_sv, fz_inst_, mlst);
                    if (commit)
                        api_.increment_out_written();
                    else
                        api_.increment_drops();

                    api_.increment_in_received();
                }
                catch (py::error_already_set &e)
                {
                    api_.increment_script_errors();
                    LOGGER_ERROR("[proc] Python error in process handler: {}", e.what());
                    if (config_.stop_on_script_error)
                        core_.shutdown_requested.store(true, std::memory_order_release);
                    api_.increment_drops();
                    api_.increment_in_received();
                }
            }
            // GIL released — sleep outside GIL for FixedRate pacing.

            if (proc_is_fixed_rate)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now < *next_dl)
                    std::this_thread::sleep_for(*next_dl - now);
                advance_deadline();
            }

            return commit;
        });

    // ── Install timeout handler (always — messages must drain during input starvation)
    //
    // When not yet due AND no messages pending, returns false immediately
    // (output slot discarded, no script call).
    processor_->set_timeout_handler(
        [this, proc_is_fixed_rate, next_dl, advance_deadline](
            void* out_data, void* /*out_fz*/) -> bool
        {
            auto msgs = core_.drain_messages();

            const auto now = std::chrono::steady_clock::now();
            const bool due = !proc_is_fixed_rate || now >= *next_dl;

            if (msgs.empty() && !due)
                return false; // not yet time — discard output slot silently

            const size_t out_sz = out_data ? out_q_->item_size() : 0;

            py::gil_scoped_acquire g;
            try
            {
                py::object none_in = py::none();
                py::object out_sv  = out_data
                    ? make_out_slot_view_(out_data, out_sz)
                    : py::none();
                py::list mlst = build_messages_list_(msgs);

                bool commit = call_on_process_(none_in, out_sv, fz_inst_, mlst);
                if (commit)
                    api_.increment_out_written();
                else if (out_data)
                    api_.increment_drops();

                advance_deadline();
                return commit;
            }
            catch (py::error_already_set &e)
            {
                api_.increment_script_errors();
                LOGGER_ERROR("[proc] Python error in timeout handler: {}", e.what());
                if (config_.stop_on_script_error)
                    core_.shutdown_requested.store(true, std::memory_order_release);
                if (out_data) api_.increment_drops();
                advance_deadline();
                return false;
            }
        });

    // Startup coordination (HEP-0023): wait for required peer roles before on_init.
    for (const auto &wr : config_.wait_for_roles)
    {
        LOGGER_INFO("[proc] Startup: waiting for role '{}' (timeout {}ms)...",
                    wr.uid, wr.timeout_ms);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{wr.timeout_ms};
        static constexpr int kPollMs = 200;
        bool found = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                break;
            const int poll_ms = static_cast<int>(std::min<long long>(kPollMs, remaining));
            py::gil_scoped_release rel;
            if (out_messenger_.query_role_presence(wr.uid, poll_ms))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            LOGGER_ERROR("[proc] Startup wait failed: role '{}' not present after {}ms",
                         wr.uid, wr.timeout_ms);
            return false;
        }
        LOGGER_INFO("[proc] Startup: role '{}' found", wr.uid);
    }

    core_.running_threads.store(true);

    // Start control thread.
    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    if (inbox_queue_)
        inbox_thread_ = std::thread([this] { run_inbox_thread_(); });

    // Call on_init with GIL held.
    call_on_init_common_();

    if (!core_.running_threads.load())
        return true; // graceful early exit from on_init

    // Start the demand-driven processing loop via hub::Processor.
    processor_->start();

    // Release GIL so Processor's handler can acquire it for Python callbacks.
    main_thread_release_.emplace();

    return true;
}

void ProcessorScriptHost::stop_role()
{
    core_.running_threads.store(false);
    core_.notify_incoming(); // unblock run_role_main_loop() immediately

    // Stop Processor and join worker threads WITHOUT holding the GIL.
    {
        py::gil_scoped_release release;
        if (processor_.has_value())
            processor_->stop();
        // Destroy Processor (joins process_thread_) while GIL is NOT held — prevents
        // a deadlock if process_thread_ was blocked waiting for the GIL.
        processor_.reset();
        // No timed join: shutdown_requested + ETERM unblocks threads within ms.
        // A timed join would require a detach fallback, which risks use-after-free.
        if (ctrl_thread_.joinable()) ctrl_thread_.join();
        // Inbox thread: join BEFORE stopping inbox_queue_.
        // The inbox loop checks core_.running_threads (already false) and exits after
        // the next recv_one() timeout (~100ms). Closing the socket while recv_one()
        // is in progress would violate ZMQ single-thread-per-socket rule.
        if (inbox_thread_.joinable()) inbox_thread_.join();
        if (inbox_queue_) { inbox_queue_->stop(); inbox_queue_.reset(); }
    }
    // GIL re-held here.

    call_on_stop_common_();

    // Stop owned queues (SHM or direct-ZMQ-PULL); broker-ZMQ queues owned by Consumer/Producer.
    if (in_queue_)  { in_queue_->stop();  in_queue_.reset(); }
    if (out_queue_) { out_queue_->stop(); out_queue_.reset(); }
    in_q_  = nullptr;
    out_q_ = nullptr;

    // Deregister hub-dead callbacks on both messengers before tearing down connections.
    in_messenger_.on_hub_dead(nullptr);
    out_messenger_.on_hub_dead(nullptr);

    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }

    // Release all Python references with GIL held.
    api_.set_in_queue(nullptr);
    api_.set_out_queue(nullptr);
    api_.set_flexzone_obj(nullptr);
    api_.set_producer(nullptr);
    api_.set_consumer(nullptr);
    api_.set_messenger(nullptr);
    clear_role_pyobjects();
    clear_common_pyobjects_();

    LOGGER_INFO("[proc] Processor stopped.");
}

void ProcessorScriptHost::cleanup_on_start_failure()
{
    // Signal all threads to exit before joining.
    core_.running_threads.store(false);

    // Join ctrl_thread_ FIRST — it accesses out_producer_ and in_consumer_.
    if (ctrl_thread_.joinable()) ctrl_thread_.join();
    if (inbox_thread_.joinable()) inbox_thread_.join();
    if (inbox_queue_) { inbox_queue_->stop(); inbox_queue_.reset(); }

    // Release processor before owned queues (it holds raw pointers into them).
    if (processor_.has_value()) processor_->stop();
    processor_.reset();

    // Stop owned queues (if queue setup succeeded before the later failure point).
    if (in_queue_)  { in_queue_->stop();  in_queue_.reset(); }
    if (out_queue_) { out_queue_->stop(); out_queue_.reset(); }
    api_.set_in_queue(nullptr);
    api_.set_out_queue(nullptr);
    in_q_  = nullptr;
    out_q_ = nullptr;

    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }
}

void ProcessorScriptHost::clear_role_pyobjects()
{
    api_.clear_inbox_cache();
    py_on_process_ = py::none();
    in_slot_type_  = py::none();
    out_slot_type_ = py::none();
    inbox_type_    = py::none();
    py_on_inbox_   = py::none();
}

void ProcessorScriptHost::update_fz_checksum_after_init()
{
    if (out_q_)
        out_q_->sync_flexzone_checksum();
}

// ============================================================================
// Slot view builders
// ============================================================================

py::object ProcessorScriptHost::make_in_slot_view_(const void *data, size_t size) const
{
    return scripting::make_slot_view(
        in_slot_spec_, in_slot_type_, data, size, /*is_read_side=*/true);
}

py::object ProcessorScriptHost::make_out_slot_view_(void *data, size_t size) const
{
    return scripting::make_slot_view(
        out_slot_spec_, out_slot_type_, data, size, /*is_read_side=*/false);
}

// ============================================================================
// Python callback wrapper
// ============================================================================

bool ProcessorScriptHost::call_on_process_(py::object &in_sv, py::object &out_sv,
                                            py::object &fz,    py::list   &msgs)
{
    py::object ret;
    try
    {
        ret = py_on_process_(in_sv, out_sv, fz, msgs, api_obj_);
    }
    catch (py::error_already_set &e)
    {
        api_.increment_script_errors();
        LOGGER_ERROR("[proc] on_process error: {}", e.what());
        if (config_.stop_on_script_error)
            core_.shutdown_requested.store(true, std::memory_order_release);
        return false;
    }
    auto [commit, is_type_error] = parse_on_process_return(ret);
    if (is_type_error)
        api_.increment_script_errors();
    return commit;
}

// ============================================================================
// make_inbox_slot_view_ — zero-copy read-only ctypes view for on_inbox callback
// ============================================================================

py::object ProcessorScriptHost::make_inbox_slot_view_(const void *data, size_t size) const
{
    // Use from_buffer_copy for ctypes to avoid storing a raw pointer into the
    // InboxItem buffer (which is valid only until the next recv_one() call).
    // This is safe and avoids a dangling-pointer risk at the cost of one memcpy.
    auto mv = py::memoryview::from_memory(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<void *>(data), static_cast<py::ssize_t>(size), /*readonly=*/true);
    if (inbox_spec_.exposure == scripting::SlotExposure::Ctypes)
        return inbox_type_.attr("from_buffer_copy")(mv);
    return mv;
}

// ============================================================================
// run_inbox_thread_ — receives inbox messages, dispatches on_inbox callback
// ============================================================================

void ProcessorScriptHost::run_inbox_thread_()
{
    static constexpr auto kPollTimeout = std::chrono::milliseconds{100};
    LOGGER_INFO("[proc] run_inbox_thread_ started");

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !api_.critical_error())
    {
        if (!inbox_queue_) break;

        const auto *item = inbox_queue_->recv_one(kPollTimeout);
        if (!item) continue;

        uint8_t ack_code = 0;
        {
            py::gil_scoped_acquire g;
            try
            {
                if (!py_on_inbox_.is_none())
                {
                    // Use item_size() — the actual decoded payload size — rather than
                    // inbox_schema_slot_size_ (ctypes sizeof). These match for "aligned"
                    // packing but may differ for "packed", causing from_buffer_copy ValueError.
                    auto sv = make_inbox_slot_view_(item->data, inbox_queue_->item_size());
                    py_on_inbox_(sv, py::str(item->sender_id), api_obj_);
                }
            }
            catch (py::error_already_set &e)
            {
                LOGGER_ERROR("[proc] on_inbox raised: {}", e.what());
                api_.increment_script_errors();
                ack_code = 3; // handler_error
                if (config_.stop_on_script_error)
                    core_.running_threads.store(false);
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR("[proc] on_inbox exception: {}", e.what());
                api_.increment_script_errors();
                ack_code = 3;
            }
        }
        inbox_queue_->send_ack(ack_code);
    }

    LOGGER_INFO("[proc] run_inbox_thread_ exiting");
}

// ============================================================================
// run_ctrl_thread_ — polls ZMQ sockets and sends heartbeats
// ============================================================================

void ProcessorScriptHost::run_ctrl_thread_()
{
    scripting::ZmqPollLoop loop{core_, "proc:" + config_.processor_uid};
    loop.sockets = {
        {out_producer_->peer_ctrl_socket_handle(),
         [&] { out_producer_->handle_peer_events_nowait(); }},
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
    };
    // Consumer data socket is only needed when data comes via broker relay (SHM transport).
    // When transport is direct ZMQ (config or broker-negotiated), data arrives through ZmqQueue.
    if (config_.in_transport != Transport::Zmq && in_consumer_->data_transport() != "zmq")
    {
        loop.sockets.push_back(
            {in_consumer_->data_zmq_socket_handle(),
             [&] { in_consumer_->handle_data_events_nowait(); }});
    }
    loop.get_iteration = [&] {
        return processor_.has_value() ? processor_->iteration_count() : 0;
    };
    loop.periodic_tasks.emplace_back(
        [&] { out_messenger_.enqueue_heartbeat(config_.out_channel,
                                                api_.snapshot_metrics_json()); },
        config_.heartbeat_interval_ms);
    loop.run();
}

} // namespace pylabhub::processor
