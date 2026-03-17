/**
 * @file lua_processor_host.cpp
 * @brief LuaProcessorHost — Lua role-specific implementation for the processor.
 *
 * Mirrors ProcessorScriptHost but uses Lua/LuaJIT FFI callbacks instead of Python.
 * All Lua calls happen on the worker thread that owns lua_State.
 *
 * Key design: The data loop runs directly in run_data_loop_() rather than using
 * hub::Processor, because lua_State must be accessed from a single thread.
 * The loop reads from in_q_, writes to out_q_, and calls on_process synchronously.
 */
#include "lua_processor_host.hpp"

#include "plh_datahub.hpp"

#include "zmq_poll_loop.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace pylabhub::processor
{

using scripting::IncomingMessage;

// ============================================================================
// Destructor
// ============================================================================

LuaProcessorHost::~LuaProcessorHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void LuaProcessorHost::set_config(ProcessorConfig config)
{
    config_ = std::move(config);
}

// ============================================================================
// Virtual hooks — callback extraction
// ============================================================================

void LuaProcessorHost::extract_callbacks_()
{
    lua_getglobal(L_, "on_process");
    ref_on_process_ = lua_isfunction(L_, -1)
        ? luaL_ref(L_, LUA_REGISTRYINDEX)
        : (lua_pop(L_, 1), LUA_NOREF);

    // on_inbox is optional — only extract when inbox is configured
    if (config_.has_inbox())
    {
        lua_getglobal(L_, "on_inbox");
        ref_on_inbox_ = lua_isfunction(L_, -1)
            ? luaL_ref(L_, LUA_REGISTRYINDEX)
            : (lua_pop(L_, 1), LUA_NOREF);
    }
}

bool LuaProcessorHost::has_required_callback() const
{
    return is_ref_callable_(ref_on_process_);
}

// ============================================================================
// Virtual hooks — schema and validation
// ============================================================================

bool LuaProcessorHost::build_role_types()
{
    using scripting::resolve_schema;

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

    // Determine packing: input and output may use different ZMQ packing.
    // For FFI struct registration we use the input packing for in-slot, output for out-slot.
    const std::string in_packing  = config_.in_zmq_packing.empty()  ? "aligned" : config_.in_zmq_packing;
    const std::string out_packing = config_.out_zmq_packing.empty() ? "aligned" : config_.out_zmq_packing;

    // Register FFI types for input slot (read-only).
    if (in_slot_spec_.has_schema)
    {
        auto cdef = build_ffi_cdef_(in_slot_spec_, in_slot_ffi_type_.c_str(), in_packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[proc] Failed to register FFI type for input slot schema");
            return false;
        }
        in_schema_slot_size_ = ffi_sizeof_(in_slot_ffi_type_.c_str());
    }

    // Register FFI types for output slot (writable).
    if (out_slot_spec_.has_schema)
    {
        auto cdef = build_ffi_cdef_(out_slot_spec_, out_slot_ffi_type_.c_str(), out_packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[proc] Failed to register FFI type for output slot schema");
            return false;
        }
        out_schema_slot_size_ = ffi_sizeof_(out_slot_ffi_type_.c_str());
    }

    // FlexZone (output side).
    if (core_.fz_spec.has_schema)
    {
        core_.has_fz = true;
        auto cdef = build_ffi_cdef_(core_.fz_spec, fz_ffi_type_.c_str(), out_packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[proc] Failed to register FFI type for flexzone schema");
            return false;
        }
        core_.schema_fz_size = ffi_sizeof_(fz_ffi_type_.c_str());
        core_.schema_fz_size = (core_.schema_fz_size + 4095U) & ~size_t{4095U};
    }

    // Inbox (optional).
    if (config_.has_inbox() && inbox_spec_.has_schema)
    {
        const std::string inbox_packing = inbox_spec_.packing.empty()
            ? config_.in_zmq_packing
            : inbox_spec_.packing;
        auto cdef = build_ffi_cdef_(inbox_spec_, inbox_ffi_type_.c_str(),
                                    inbox_packing.empty() ? "aligned" : inbox_packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[proc] Failed to register FFI type for inbox schema");
            return false;
        }
        inbox_schema_slot_size_ = ffi_sizeof_(inbox_ffi_type_.c_str());
    }

    return true;
}

void LuaProcessorHost::print_validate_layout()
{
    std::cout << "\nProcessor (Lua): " << config_.processor_uid << "\n";
    if (in_slot_spec_.has_schema)
        std::cout << "  Input slot: " << in_slot_ffi_type_
                  << " (" << in_schema_slot_size_ << " bytes)\n";
    if (out_slot_spec_.has_schema)
        std::cout << "  Output slot: " << out_slot_ffi_type_
                  << " (" << out_schema_slot_size_ << " bytes)\n";
    if (core_.fz_spec.has_schema)
        std::cout << "  FlexZone: " << fz_ffi_type_
                  << " (" << core_.schema_fz_size << " bytes)\n";
}

// ============================================================================
// Virtual hooks — API table
// ============================================================================

void LuaProcessorHost::build_role_api_table_(lua_State *L)
{
    lua_newtable(L);

    // Common closures: log, stop, set_critical_error, stop_reason, script_errors
    // and string fields: log_level, script_dir, role_dir.
    push_common_api_closures_(L);

    // Role-specific closures.
    auto push_closure = [&](const char *name, lua_CFunction fn) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };

    push_closure("uid",                      lua_api_uid);
    push_closure("name",                     lua_api_name);
    push_closure("in_channel",               lua_api_in_channel);
    push_closure("out_channel",              lua_api_out_channel);
    push_closure("broadcast",                lua_api_broadcast);
    push_closure("send",                     lua_api_send);
    push_closure("consumers",                lua_api_consumers);
    push_closure("update_flexzone_checksum", lua_api_update_flexzone_checksum);
    push_closure("out_written",              lua_api_out_written);
    push_closure("in_received",              lua_api_in_received);
    push_closure("drops",                    lua_api_drops);

    // api table is now on top of the stack — caller stores in registry.
}

// ============================================================================
// Lua API closures — retrieve `this` from upvalue(1)
// ============================================================================

int LuaProcessorHost::lua_api_uid(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.processor_uid.c_str());
    return 1;
}

int LuaProcessorHost::lua_api_name(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.processor_name.c_str());
    return 1;
}

int LuaProcessorHost::lua_api_in_channel(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.in_channel.c_str());
    return 1;
}

int LuaProcessorHost::lua_api_out_channel(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.out_channel.c_str());
    return 1;
}

int LuaProcessorHost::lua_api_broadcast(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);

    if (!self->out_producer_.has_value())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    self->out_producer_->send(data, len);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaProcessorHost::lua_api_send(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *identity = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);

    if (!self->out_producer_.has_value())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    self->out_producer_->send_to(identity, data, len);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaProcessorHost::lua_api_consumers(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!self->out_producer_.has_value())
    {
        lua_newtable(L);
        return 1;
    }
    auto list = self->out_producer_->connected_consumers();
    lua_newtable(L);
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
    {
        lua_pushstring(L, format_tools::bytes_to_hex(list[static_cast<size_t>(i)]).c_str());
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int LuaProcessorHost::lua_api_update_flexzone_checksum(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    if (self->out_q_)
        self->out_q_->sync_flexzone_checksum();
    lua_pushboolean(L, self->out_q_ ? 1 : 0);
    return 1;
}

int LuaProcessorHost::lua_api_out_written(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->out_slots_written_.load(std::memory_order_relaxed)));
    return 1;
}

int LuaProcessorHost::lua_api_in_received(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->in_slots_received_.load(std::memory_order_relaxed)));
    return 1;
}

int LuaProcessorHost::lua_api_drops(lua_State *L)
{
    auto *self = static_cast<LuaProcessorHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->out_drops_.load(std::memory_order_relaxed)));
    return 1;
}

// ============================================================================
// snapshot_metrics_json — for heartbeat reporting
// ============================================================================

nlohmann::json LuaProcessorHost::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]        = in_slots_received_.load(std::memory_order_relaxed);
    base["out_written"]        = out_slots_written_.load(std::memory_order_relaxed);
    base["drops"]              = out_drops_.load(std::memory_order_relaxed);
    base["script_errors"]      = script_errors_.load(std::memory_order_relaxed);
    base["last_cycle_work_us"] = last_cycle_work_us_.load(std::memory_order_relaxed);
    base["loop_overrun_count"] = 0;

    if (out_producer_.has_value())
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — shm() is non-const but we only read
        if (const auto *shm = const_cast<hub::Producer &>(*out_producer_).shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["iteration_count"]   = m.iteration_count;
            base["loop_overrun_count"] = m.overrun_count;
            base["last_iteration_us"] = m.last_iteration_us;
            base["max_iteration_us"]  = m.max_iteration_us;
            base["last_slot_work_us"] = m.last_slot_work_us;
            base["last_slot_wait_us"] = m.last_slot_wait_us;
            base["period_ms"]         = m.period_ms;
        }
    }
    return base;
}

// ============================================================================
// Virtual hooks — lifecycle
// ============================================================================

bool LuaProcessorHost::start_role()
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
    in_opts.zmq_schema  = scripting::schema_spec_to_zmq_fields(in_slot_spec_, in_schema_slot_size_);
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
        out_opts.has_shm           = false;
        out_opts.data_transport    = "zmq";
        out_opts.zmq_node_endpoint = config_.zmq_out_endpoint;
        out_opts.zmq_bind          = config_.zmq_out_bind;
        out_opts.zmq_schema        = scripting::schema_spec_to_zmq_fields(out_slot_spec_, out_schema_slot_size_);
        out_opts.zmq_packing          = config_.out_zmq_packing;
        out_opts.zmq_buffer_depth     = config_.out_zmq_buffer_depth;
        const bool zmq_drop = config_.zmq_out_overflow_policy.empty()
            ? (config_.overflow_policy == OverflowPolicy::Drop)
            : (config_.zmq_out_overflow_policy == "drop");
        out_opts.zmq_overflow_policy  = zmq_drop
                                            ? hub::OverflowPolicy::Drop
                                            : hub::OverflowPolicy::Block;
    }

    // ── Inbox facility (optional) ───────────────────────────────────────────
    if (config_.has_inbox())
    {
        const std::string ep = config_.inbox_endpoint.empty()
            ? "tcp://127.0.0.1:0"
            : config_.inbox_endpoint;
        const std::string packing = inbox_spec_.packing.empty()
            ? config_.in_zmq_packing
            : inbox_spec_.packing;

        auto zmq_fields = scripting::schema_spec_to_zmq_fields(inbox_spec_, inbox_schema_slot_size_);

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
            return false;
        }
        LOGGER_INFO("[proc] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());

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
            IncomingMessage msg;
            msg.event = "consumer_joined";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_left(
        [this, hex_identity](const std::string &identity) {
            IncomingMessage msg;
            msg.event = "consumer_left";
            msg.details["identity"] = hex_identity(identity);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_consumer_died(config_.out_channel,
        [this](uint64_t pid, std::string reason) {
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(config_.out_channel,
        [this](std::string event, nlohmann::json details) {
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
        stop_reason_.store(static_cast<int>(scripting::LuaStopReason::PeerDead),
                           std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    out_producer_->on_peer_dead([this]() {
        LOGGER_WARN("[proc] peer-dead: downstream consumer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        stop_reason_.store(static_cast<int>(scripting::LuaStopReason::PeerDead),
                           std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    auto hub_dead_cb = [this]() {
        LOGGER_WARN("[proc] hub-dead: broker connection lost; triggering shutdown");
        stop_reason_.store(static_cast<int>(scripting::LuaStopReason::HubDead),
                           std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    };
    in_messenger_.on_hub_dead(hub_dead_cb);
    out_messenger_.on_hub_dead(hub_dead_cb);

    // ── Create data queues ─────────────────────────────────────────────────
    // Input queue.
    if (config_.in_transport == Transport::Zmq)
    {
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(in_slot_spec_, in_schema_slot_size_);
        in_queue_ = hub::ZmqQueue::pull_from(
            config_.zmq_in_endpoint, std::move(zmq_fields),
            config_.in_zmq_packing, config_.zmq_in_bind, config_.in_zmq_buffer_depth);
        in_queue_->start();
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
        in_queue_->start();
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

    LOGGER_INFO("[proc] Lua processor started: '{}' -> '{}'",
                config_.in_channel, config_.out_channel);

    // ── Startup coordination (HEP-0023) ─────────────────────────────────────
    if (!wait_for_roles_(out_messenger_, config_.wait_for_roles))
        return false;

    core_.running_threads.store(true);
    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    // on_init is called on the worker thread (which owns lua_State) — safe.
    call_on_init_common_();

    return true;
}

void LuaProcessorHost::stop_role()
{
    core_.running_threads.store(false);
    core_.notify_incoming();

    if (ctrl_thread_.joinable()) ctrl_thread_.join();

    // Inbox: stop after worker thread exits (no separate inbox thread for Lua).
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }

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

    // Release Lua callback refs.
    if (ref_on_process_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_process_);
        ref_on_process_ = LUA_NOREF;
    }
    if (ref_on_inbox_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_inbox_);
        ref_on_inbox_ = LUA_NOREF;
    }

    LOGGER_INFO("[proc] Lua processor stopped.");
}

void LuaProcessorHost::cleanup_on_start_failure()
{
    core_.running_threads.store(false);

    if (ctrl_thread_.joinable()) ctrl_thread_.join();
    if (inbox_queue_) { inbox_queue_->stop(); inbox_queue_.reset(); }

    if (in_queue_)  { in_queue_->stop();  in_queue_.reset(); }
    if (out_queue_) { out_queue_->stop(); out_queue_.reset(); }
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

void LuaProcessorHost::update_fz_checksum_after_init()
{
    if (out_q_)
        out_q_->sync_flexzone_checksum();
}

// ============================================================================
// run_data_loop_ — processor data loop (Lua single-thread model)
//
// Unlike the Python ProcessorScriptHost which delegates to hub::Processor,
// we run the loop directly here because lua_State must only be accessed
// from one thread.
//
// Flow per iteration:
//   1. read_acquire from in_q (read-only input)
//   2. write_acquire from out_q (writable output)
//   3. call on_process(in_slot, out_slot, fz, msgs, api)
//   4. based on return: write_commit or write_discard on out_q
//   5. read_release on in_q
//   6. drain inbox synchronously
//   7. timing (same as producer)
// ============================================================================

void LuaProcessorHost::run_data_loop_()
{
    if (!in_q_ || !out_q_)
    {
        LOGGER_ERROR("[proc] run_data_loop_: transport queues not initialized — aborting");
        core_.running_threads.store(false);
        return;
    }

    const auto input_timeout = std::chrono::milliseconds{
        pylabhub::compute_slot_acquire_timeout(
            config_.slot_acquire_timeout_ms, config_.target_period_ms)};

    // Output acquire timeout: for Block policy, same as input; for Drop, non-blocking.
    const auto output_timeout = (config_.overflow_policy == OverflowPolicy::Drop)
        ? std::chrono::milliseconds{0}
        : input_timeout;

    auto next_deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds{config_.target_period_ms};

    const bool is_fixed_rate = (config_.loop_timing != LoopTimingPolicy::MaxRate);
    const auto period        = std::chrono::milliseconds{config_.target_period_ms};

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !critical_error_.load(std::memory_order_relaxed))
    {
        const auto iter_start = std::chrono::steady_clock::now();

        // Step 1: read_acquire from input queue.
        const void *in_buf = in_q_->read_acquire(input_timeout);
        auto msgs = core_.drain_messages();

        if (!in_buf)
        {
            // No input available — call on_process with nil input if messages or timer due.
            const bool due = !is_fixed_rate || iter_start >= next_deadline;

            if (!msgs.empty() || due)
            {
                // Try to acquire output for timeout handler.
                void *out_buf = out_q_->write_acquire(output_timeout);
                if (out_buf)
                {
                    const size_t out_sz = out_q_->item_size();
                    std::memset(out_buf, 0, out_sz);

                    bool commit = call_on_process_no_input_(out_buf, out_sz, msgs);
                    if (commit)
                    {
                        out_q_->write_commit();
                        out_slots_written_.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        out_q_->write_discard();
                        out_drops_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                else
                {
                    // No output slot either — call with nil for both.
                    (void)call_on_process_no_input_(nullptr, 0, msgs);
                    out_drops_.fetch_add(1, std::memory_order_relaxed);
                }

                if (is_fixed_rate)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_deadline)
                    {
                        if (config_.loop_timing == LoopTimingPolicy::FixedRateWithCompensation)
                            next_deadline += period;
                        else
                            next_deadline = now + period;
                    }
                }
            }

            iteration_count_.fetch_add(1, std::memory_order_relaxed);
            drain_inbox_sync_();
            continue;
        }

        // Step 2: write_acquire from output queue.
        void *out_buf = out_q_->write_acquire(output_timeout);

        if (!out_buf)
        {
            // Output acquire failed — drop this input.
            in_q_->read_release();
            in_slots_received_.fetch_add(1, std::memory_order_relaxed);
            out_drops_.fetch_add(1, std::memory_order_relaxed);
            iteration_count_.fetch_add(1, std::memory_order_relaxed);
            drain_inbox_sync_();
            continue;
        }

        const size_t in_sz  = in_q_->item_size();
        const size_t out_sz = out_q_->item_size();
        std::memset(out_buf, 0, out_sz);

        // Step 3: call on_process.
        bool commit = call_on_process_(in_buf, in_sz, out_buf, out_sz, msgs);

        // Step 4: commit or discard output.
        if (commit)
        {
            out_q_->write_commit();
            out_slots_written_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            out_q_->write_discard();
            out_drops_.fetch_add(1, std::memory_order_relaxed);
        }

        // Step 5: release input.
        in_q_->read_release();
        in_slots_received_.fetch_add(1, std::memory_order_relaxed);

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        // Step 6: synchronous inbox drain.
        drain_inbox_sync_();

        // Step 7: timing.
        {
            const auto now     = std::chrono::steady_clock::now();
            const auto work_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - iter_start).count());
            last_cycle_work_us_.store(work_us, std::memory_order_relaxed);

            if (is_fixed_rate)
            {
                if (now < next_deadline)
                {
                    std::this_thread::sleep_for(next_deadline - now);
                    next_deadline += period;
                }
                else
                {
                    if (config_.loop_timing == LoopTimingPolicy::FixedRateWithCompensation)
                        next_deadline += period;
                    else
                        next_deadline = now + period;
                }
            }
        }
    }

    LOGGER_INFO("[proc] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                critical_error_.load());
}

// ============================================================================
// push_flexzone_view_ — push flexzone cdata or nil onto Lua stack
// ============================================================================

void LuaProcessorHost::push_flexzone_view_()
{
    if (core_.has_fz && out_q_)
    {
        void *fz = out_q_->write_flexzone();
        size_t fz_sz = out_q_->flexzone_size();
        if (fz && fz_sz > 0)
        {
            if (!push_slot_view_(fz, fz_sz, fz_ffi_type_.c_str()))
                lua_pushnil(L_); // fallback to nil on error
        }
        else
        {
            lua_pushnil(L_);
        }
    }
    else
    {
        lua_pushnil(L_);
    }
}

// ============================================================================
// call_on_process_ — with both input and output slots
// ============================================================================

bool LuaProcessorHost::call_on_process_(const void *in_buf, size_t in_sz,
                                          void *out_buf, size_t out_sz,
                                          std::vector<IncomingMessage> &msgs)
{
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_process_);

    // arg 1: in_slot (read-only)
    if (!push_slot_view_readonly_(in_buf, in_sz, in_slot_ffi_type_.c_str()))
    {
        lua_pop(L_, 1); // pop function
        return false;
    }

    // arg 2: out_slot (writable)
    if (!push_slot_view_(out_buf, out_sz, out_slot_ffi_type_.c_str()))
    {
        lua_pop(L_, 2); // pop function + in_slot
        return false;
    }

    // arg 3: flexzone
    push_flexzone_view_();

    // arg 4: messages
    push_messages_table_(msgs);

    // arg 5: api
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);

    // Call: on_process(in_slot, out_slot, fz, msgs, api) -> 1 result
    if (lua_pcall(L_, 5, 1, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[proc] on_process error: {}", err ? err : "(unknown)");
        lua_pop(L_, 1);
        on_script_error();
        if (config_.stop_on_script_error)
            core_.running_threads.store(false);
        return false;
    }

    // Parse return: nil/true -> commit, false -> discard.
    bool commit = lua_isnil(L_, -1) || lua_toboolean(L_, -1);
    lua_pop(L_, 1);
    return commit;
}

// ============================================================================
// call_on_process_no_input_ — without input slot (acquire timeout)
// ============================================================================

bool LuaProcessorHost::call_on_process_no_input_(void *out_buf, size_t out_sz,
                                                   std::vector<IncomingMessage> &msgs)
{
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_process_);

    // arg 1: in_slot = nil (no input available)
    lua_pushnil(L_);

    // arg 2: out_slot (writable, or nil if not available)
    if (out_buf && out_sz > 0)
    {
        if (!push_slot_view_(out_buf, out_sz, out_slot_ffi_type_.c_str()))
        {
            lua_pop(L_, 2); // pop function + nil
            return false;
        }
    }
    else
    {
        lua_pushnil(L_);
    }

    // arg 3: flexzone
    push_flexzone_view_();

    // arg 4: messages
    push_messages_table_(msgs);

    // arg 5: api
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);

    if (lua_pcall(L_, 5, 1, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[proc] on_process (no input) error: {}", err ? err : "(unknown)");
        lua_pop(L_, 1);
        on_script_error();
        if (config_.stop_on_script_error)
            core_.running_threads.store(false);
        return false;
    }

    // Parse return.
    bool commit = lua_isnil(L_, -1) || lua_toboolean(L_, -1);
    lua_pop(L_, 1);
    return commit;
}

// ============================================================================
// run_ctrl_thread_ — polls both producer and consumer ZMQ sockets, sends heartbeats
// ============================================================================

void LuaProcessorHost::run_ctrl_thread_()
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
    loop.get_iteration = [&] { return iteration_count_.load(std::memory_order_relaxed); };
    loop.periodic_tasks.emplace_back(
        [&] { out_messenger_.enqueue_heartbeat(config_.out_channel,
                                                snapshot_metrics_json()); },
        config_.heartbeat_interval_ms);
    loop.run();
}

} // namespace pylabhub::processor
