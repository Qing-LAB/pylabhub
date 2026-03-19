/**
 * @file lua_consumer_host.cpp
 * @brief LuaConsumerHost — Lua role-specific implementation for the consumer.
 *
 * Mirrors ConsumerScriptHost but uses Lua/LuaJIT FFI callbacks instead of Python.
 * All Lua calls happen on the worker thread that owns lua_State.
 */
#include "lua_consumer_host.hpp"

#include "plh_datahub.hpp"

#include "zmq_poll_loop.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace pylabhub::consumer
{

using scripting::IncomingMessage;

// ============================================================================
// Destructor
// ============================================================================

LuaConsumerHost::~LuaConsumerHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void LuaConsumerHost::set_config(ConsumerConfig config)
{
    config_ = std::move(config);
}

// ============================================================================
// Virtual hooks — callback extraction
// ============================================================================

void LuaConsumerHost::extract_callbacks_()
{
    lua_getglobal(L_, "on_consume");
    ref_on_consume_ = lua_isfunction(L_, -1)
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

bool LuaConsumerHost::has_required_callback() const
{
    return is_ref_callable_(ref_on_consume_);
}

// ============================================================================
// Virtual hooks — schema and validation
// ============================================================================

bool LuaConsumerHost::build_role_types()
{
    using scripting::resolve_schema;

    inbox_ffi_type_ = "InboxFrame";

    if (!config_.script_type_explicit)
        LOGGER_WARN("[cons] Config 'script.type' absent — defaulting to '{}'. "
                    "Set it explicitly to suppress this warning.", config_.script_type);

    try
    {
        std::vector<std::string> schema_dirs;
        if (!config_.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(config_.hub_dir) / "schemas").string());

        slot_spec_    = resolve_schema(config_.slot_schema_json,     false, "cons", schema_dirs);
        core_.fz_spec = resolve_schema(config_.flexzone_schema_json, true,  "cons", schema_dirs);

        if (config_.has_inbox())
            inbox_spec_ = resolve_schema(config_.inbox_schema_json, false, "cons", schema_dirs);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[cons] Schema parse error: {}", e.what());
        return false;
    }

    // Register FFI types for slot, flexzone, inbox.
    const std::string packing = config_.zmq_packing.empty() ? "aligned" : config_.zmq_packing;

    if (slot_spec_.has_schema)
    {
        auto cdef = build_ffi_cdef_(slot_spec_, slot_ffi_type_.c_str(), packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[cons] Failed to register FFI type for slot schema");
            return false;
        }
        schema_slot_size_ = ffi_sizeof_(slot_ffi_type_.c_str());
    }

    if (core_.fz_spec.has_schema)
    {
        core_.has_fz = true;
        auto cdef = build_ffi_cdef_(core_.fz_spec, fz_ffi_type_.c_str(), packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[cons] Failed to register FFI type for flexzone schema");
            return false;
        }
        core_.schema_fz_size = ffi_sizeof_(fz_ffi_type_.c_str());
        core_.schema_fz_size = (core_.schema_fz_size + 4095U) & ~size_t{4095U};
    }

    if (config_.has_inbox() && inbox_spec_.has_schema)
    {
        auto cdef = build_ffi_cdef_(inbox_spec_, inbox_ffi_type_.c_str(), packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[cons] Failed to register FFI type for inbox schema");
            return false;
        }
        inbox_schema_slot_size_ = ffi_sizeof_(inbox_ffi_type_.c_str());
    }

    return true;
}

void LuaConsumerHost::print_validate_layout()
{
    std::cout << "\nConsumer (Lua): " << config_.consumer_uid << "\n";
    if (slot_spec_.has_schema)
        std::cout << "  Input slot: " << slot_ffi_type_
                  << " (" << schema_slot_size_ << " bytes)\n";
    if (core_.fz_spec.has_schema)
        std::cout << "  FlexZone: " << fz_ffi_type_
                  << " (" << core_.schema_fz_size << " bytes)\n";
}

// ============================================================================
// Virtual hooks — API table
// ============================================================================

void LuaConsumerHost::build_role_api_table_(lua_State *L)
{
    lua_newtable(L);

    // Common closures: log, stop, set_critical_error, stop_reason, script_errors,
    // and string fields (log_level, script_dir, role_dir).
    push_common_api_closures_(L);

    // Store `this` as lightuserdata upvalue for role-specific closures.
    auto push_closure = [&](const char *name, lua_CFunction fn) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };

    push_closure("uid",                  lua_api_uid);
    push_closure("name",                 lua_api_name);
    push_closure("channel",              lua_api_channel);
    push_closure("in_received",          lua_api_in_received);
    push_closure("set_verify_checksum",  lua_api_set_verify_checksum);

    // api table is now on top of the stack — caller stores in registry.
}

// ============================================================================
// Lua API closures — retrieve `this` from upvalue(1)
// ============================================================================

int LuaConsumerHost::lua_api_uid(lua_State *L)
{
    auto *self = static_cast<LuaConsumerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.consumer_uid.c_str());
    return 1;
}

int LuaConsumerHost::lua_api_name(lua_State *L)
{
    auto *self = static_cast<LuaConsumerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.consumer_name.c_str());
    return 1;
}

int LuaConsumerHost::lua_api_channel(lua_State *L)
{
    auto *self = static_cast<LuaConsumerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.channel.c_str());
    return 1;
}

int LuaConsumerHost::lua_api_in_received(lua_State *L)
{
    auto *self = static_cast<LuaConsumerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->in_slots_received_.load(std::memory_order_relaxed)));
    return 1;
}

int LuaConsumerHost::lua_api_set_verify_checksum(lua_State *L)
{
    auto *self = static_cast<LuaConsumerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    bool enable = lua_toboolean(L, 1) != 0;
    if (self->queue_reader_)
        self->queue_reader_->set_verify_checksum(enable, false);
    return 0;
}

// ============================================================================
// push_messages_table_ — consumer: bare bytes (no sender) + event dicts
// ============================================================================

void LuaConsumerHost::push_messages_table_(std::vector<IncomingMessage> &msgs)
{
    lua_newtable(L_);
    int idx = 1;
    for (auto &m : msgs)
    {
        if (!m.event.empty())
        {
            // Event message -> Lua table: {event="...", key=val, ...}
            lua_newtable(L_);
            lua_pushstring(L_, m.event.c_str());
            lua_setfield(L_, -2, "event");
            for (auto &[key, val] : m.details.items())
            {
                if (val.is_string())
                    lua_pushstring(L_, val.get<std::string>().c_str());
                else if (val.is_boolean())
                    lua_pushboolean(L_, val.get<bool>() ? 1 : 0);
                else if (val.is_number_integer())
                    lua_pushinteger(L_, val.get<lua_Integer>());
                else if (val.is_number_float())
                    lua_pushnumber(L_, val.get<double>());
                else
                    lua_pushstring(L_, val.dump().c_str());
                lua_setfield(L_, -2, key.c_str());
            }
        }
        else
        {
            // Data message -> bare bytes string (no sender for consumer).
            lua_pushlstring(L_, reinterpret_cast<const char *>(m.data.data()), m.data.size());
        }
        lua_rawseti(L_, -2, idx++);
    }
}

// ============================================================================
// snapshot_metrics_json — for heartbeat/metrics reporting
// ============================================================================

nlohmann::json LuaConsumerHost::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["in_received"]        = in_slots_received_.load(std::memory_order_relaxed);
    base["script_errors"]      = core_.script_errors_.load(std::memory_order_relaxed);
    base["last_cycle_work_us"] = last_cycle_work_us_.load(std::memory_order_relaxed);
    base["loop_overrun_count"] = uint64_t{0}; // consumer is demand-driven, no deadline

    if (in_consumer_.has_value())
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — shm() is non-const but we only read
        if (const auto *shm = const_cast<hub::Consumer &>(*in_consumer_).shm(); shm != nullptr)
        {
            const auto &m = shm->metrics();
            base["iteration_count"]   = m.iteration_count;
            base["last_iteration_us"] = m.last_iteration_us;
            base["max_iteration_us"]  = m.max_iteration_us;
            base["last_slot_work_us"] = m.last_slot_work_us;
            base["last_slot_wait_us"] = m.last_slot_wait_us;
        }
    }
    return base;
}

// ============================================================================
// Virtual hooks — lifecycle
// ============================================================================

bool LuaConsumerHost::start_role()
{
    if (!build_role_types())
        return false;

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

    if (!config_.broker.empty())
    {
        if (!in_messenger_.connect(config_.broker, config_.broker_pubkey,
                                   config_.auth.client_pubkey, config_.auth.client_seckey))
        {
            LOGGER_ERROR("[cons] broker connect failed ({}); aborting", config_.broker);
            return false;
        }
    }

    auto maybe_consumer = hub::Consumer::connect(in_messenger_, opts);
    if (!maybe_consumer.has_value())
    {
        LOGGER_ERROR("[cons] Failed to connect consumer to channel '{}'", config_.channel);
        return false;
    }
    in_consumer_ = std::move(maybe_consumer);

    if (auto *in_shm = in_consumer_->shm(); in_shm != nullptr)
        in_shm->clear_metrics();

    // Graceful shutdown: queue channel_closing as a regular event message.
    in_consumer_->on_channel_closing([this]() {
        LOGGER_INFO("[cons] CHANNEL_CLOSING_NOTIFY received, queuing event");
        IncomingMessage msg;
        msg.event = "channel_closing";
        core_.enqueue_message(std::move(msg));
    });

    // Forced shutdown: broker grace period expired — immediate stop.
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

    // Wire peer-dead and hub-dead monitoring.
    in_consumer_->on_peer_dead([this]() {
        LOGGER_WARN("[cons] peer-dead: producer silent for {} ms; triggering shutdown",
                    config_.peer_dead_timeout_ms);
        core_.stop_reason_.store(static_cast<int>(scripting::LuaStopReason::PeerDead),
                           std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    in_messenger_.on_hub_dead([this]() {
        LOGGER_WARN("[cons] hub-dead: broker connection lost; triggering shutdown");
        core_.stop_reason_.store(static_cast<int>(scripting::LuaStopReason::HubDead),
                           std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Create the transport QueueReader.
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

    // Inbox facility (optional).
    if (config_.has_inbox())
    {
        const std::string ep = config_.inbox_endpoint.empty()
            ? "tcp://127.0.0.1:0"
            : config_.inbox_endpoint;
        const std::string packing = inbox_spec_.packing.empty()
            ? config_.zmq_packing
            : inbox_spec_.packing;

        auto zmq_fields = scripting::schema_spec_to_zmq_fields(inbox_spec_,
                                                                inbox_schema_slot_size_);

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
        inbox_queue_ = hub::InboxQueue::bind_at(ep, std::move(zmq_fields),
                                                packing, inbox_rcvhwm);
        if (!inbox_queue_ || !inbox_queue_->start())
        {
            LOGGER_ERROR("[cons] Failed to start InboxQueue at '{}'", ep);
            if (inbox_queue_) inbox_queue_.reset();
        }
        else
        {
            LOGGER_INFO("[cons] InboxQueue bound at '{}'", inbox_queue_->actual_endpoint());
        }
    }

    LOGGER_INFO("[cons] Lua consumer started on channel '{}'", config_.channel);

    // Startup coordination (HEP-0023).
    if (!wait_for_roles_(in_messenger_, config_.wait_for_roles))
        return false;

    core_.running_threads.store(true);
    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    // on_init is called on the worker thread (which owns lua_State) — safe.
    call_on_init_common_();

    return true;
}

void LuaConsumerHost::stop_role()
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

    // Null out the QueueReader pointer before destroying the SHM queue or consumer.
    queue_reader_ = nullptr;
    if (shm_queue_)
    {
        shm_queue_.reset();
    }

    // Deregister hub-dead callback before closing the messenger.
    in_messenger_.on_hub_dead(nullptr);

    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }

    // Release Lua callback refs.
    if (ref_on_consume_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_consume_);
        ref_on_consume_ = LUA_NOREF;
    }
    if (ref_on_inbox_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_inbox_);
        ref_on_inbox_ = LUA_NOREF;
    }

    LOGGER_INFO("[cons] Lua consumer stopped.");
}

void LuaConsumerHost::cleanup_on_start_failure()
{
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }
    queue_reader_ = nullptr;
    shm_queue_.reset();
    if (in_consumer_.has_value())
    {
        in_consumer_->stop();
        in_consumer_->close();
        in_consumer_.reset();
    }
}

// ============================================================================
// run_data_loop_ — transport-agnostic demand-driven consumption loop (Lua)
//
// All Lua callbacks (on_consume, on_inbox) run on THIS thread — the worker
// thread that owns lua_State. No GIL, no separate loop/inbox threads.
// ============================================================================

void LuaConsumerHost::run_data_loop_()
{
    if (!queue_reader_)
    {
        LOGGER_ERROR("[cons] run_data_loop_: QueueReader not initialized — aborting");
        core_.running_threads.store(false);
        return;
    }

    const auto timeout = std::chrono::milliseconds{
        pylabhub::compute_slot_acquire_timeout(
            config_.slot_acquire_timeout_ms, config_.target_period_ms)};

    const bool is_fixed_rate = (config_.loop_timing != LoopTimingPolicy::MaxRate);
    const auto period        = std::chrono::milliseconds{static_cast<int>(config_.target_period_ms)};

    auto next_deadline = std::chrono::steady_clock::now() + period;

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !core_.critical_error_.load(std::memory_order_relaxed))
    {
        const auto iter_start = std::chrono::steady_clock::now();

        // 1. Block until a slot arrives (or timeout).
        const void *data = queue_reader_->read_acquire(timeout);

        // 2. Drain any queued ZMQ ctrl messages.
        auto msgs = core_.drain_messages();

        if (!data)
        {
            const bool due = !is_fixed_rate || iter_start >= next_deadline;

            if (!msgs.empty() || due)
            {
                call_on_consume_no_slot_(msgs);

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

            // Synchronous inbox drain.
            drain_inbox_sync_();
            continue;
        }

        in_slots_received_.fetch_add(1, std::memory_order_relaxed);

        if (!core_.running_threads.load() || core_.critical_error_.load(std::memory_order_relaxed))
        {
            queue_reader_->read_release();
            break;
        }

        const size_t item_sz = queue_reader_->item_size();
        call_on_consume_(data, item_sz, msgs);

        queue_reader_->read_release();

        iteration_count_.fetch_add(1, std::memory_order_relaxed);

        // Synchronous inbox drain.
        drain_inbox_sync_();

        // Timing.
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

    LOGGER_INFO("[cons] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                core_.critical_error_.load());
}

// ============================================================================
// push_flexzone_view_readonly_ — push read-only flexzone cdata or nil
// ============================================================================

void LuaConsumerHost::push_flexzone_view_readonly_()
{
    if (core_.has_fz && queue_reader_)
    {
        const void *fz = queue_reader_->read_flexzone();
        size_t fz_sz = queue_reader_->flexzone_size();
        if (fz && fz_sz > 0)
        {
            if (!push_slot_view_readonly_(fz, fz_sz, fz_ffi_type_.c_str()))
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
// call_on_consume_ — with slot (read-only)
// ============================================================================

void LuaConsumerHost::call_on_consume_(const void *buf, size_t buf_sz,
                                        std::vector<IncomingMessage> &msgs)
{
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_consume_);

    if (!push_slot_view_readonly_(buf, buf_sz, slot_ffi_type_.c_str()))
    {
        lua_pop(L_, 1); // pop function
        return;
    }

    push_flexzone_view_readonly_();
    push_messages_table_(msgs);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);

    // Call: on_consume(in_slot, fz, msgs, api) -> void (return value ignored)
    if (lua_pcall(L_, 4, 0, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[cons] on_consume error: {}", err ? err : "(unknown)");
        lua_pop(L_, 1);
        on_script_error();
        if (config_.stop_on_script_error)
            core_.shutdown_requested.store(true, std::memory_order_release);
    }
}

// ============================================================================
// call_on_consume_no_slot_ — without slot (acquire timed out)
// ============================================================================

void LuaConsumerHost::call_on_consume_no_slot_(std::vector<IncomingMessage> &msgs)
{
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_consume_);
    lua_pushnil(L_); // in_slot = nil

    push_flexzone_view_readonly_();
    push_messages_table_(msgs);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);

    // Call: on_consume(nil, fz, msgs, api) -> void
    if (lua_pcall(L_, 4, 0, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[cons] on_consume (no slot) error: {}", err ? err : "(unknown)");
        lua_pop(L_, 1);
        on_script_error();
        if (config_.stop_on_script_error)
            core_.shutdown_requested.store(true, std::memory_order_release);
    }
}

// ============================================================================
// run_ctrl_thread_ — polls consumer ZMQ sockets, sends heartbeats
// ============================================================================

void LuaConsumerHost::run_ctrl_thread_()
{
    scripting::ZmqPollLoop loop{core_, "cons:" + config_.consumer_uid};
    loop.sockets = {
        {in_consumer_->ctrl_zmq_socket_handle(),
         [&] { in_consumer_->handle_ctrl_events_nowait(); }},
        {in_consumer_->data_zmq_socket_handle(),
         [&] { in_consumer_->handle_data_events_nowait(); }},
    };
    loop.get_iteration = [&] { return iteration_count_.load(std::memory_order_relaxed); };
    loop.periodic_tasks.emplace_back(
        [&] { in_messenger_.enqueue_heartbeat(config_.channel); },
        config_.heartbeat_interval_ms);
    // HEP-CORE-0019: periodic metrics report.
    loop.periodic_tasks.emplace_back(
        [&] { in_messenger_.enqueue_metrics_report(
                  config_.channel, config_.consumer_uid,
                  snapshot_metrics_json()); },
        config_.heartbeat_interval_ms);
    loop.run();
}

} // namespace pylabhub::consumer
