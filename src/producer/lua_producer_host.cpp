/**
 * @file lua_producer_host.cpp
 * @brief LuaProducerHost — Lua role-specific implementation for the producer.
 *
 * Mirrors ProducerScriptHost but uses Lua/LuaJIT FFI callbacks instead of Python.
 * All Lua calls happen on the worker thread that owns lua_State.
 */
#include "lua_producer_host.hpp"

#include "plh_datahub.hpp"

#include "zmq_poll_loop.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace pylabhub::producer
{

using scripting::IncomingMessage;

// ============================================================================
// Destructor
// ============================================================================

LuaProducerHost::~LuaProducerHost()
{
    shutdown_();
}

// ============================================================================
// Configuration
// ============================================================================

void LuaProducerHost::set_config(ProducerConfig config)
{
    config_ = std::move(config);
}

// ============================================================================
// Virtual hooks — callback extraction
// ============================================================================

void LuaProducerHost::extract_callbacks_()
{
    lua_getglobal(L_, "on_produce");
    ref_on_produce_ = lua_isfunction(L_, -1)
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

bool LuaProducerHost::has_required_callback() const
{
    return is_ref_callable_(ref_on_produce_);
}

// ============================================================================
// Virtual hooks — schema and validation
// ============================================================================

bool LuaProducerHost::build_role_types()
{
    using scripting::resolve_schema;

    inbox_ffi_type_ = "InboxFrame";

    if (!config_.script_type_explicit)
        LOGGER_WARN("[prod] Config 'script.type' absent — defaulting to '{}'. "
                    "Set it explicitly to suppress this warning.", config_.script_type);

    try
    {
        std::vector<std::string> schema_dirs;
        if (!config_.hub_dir.empty())
            schema_dirs.push_back(
                (std::filesystem::path(config_.hub_dir) / "schemas").string());

        slot_spec_    = resolve_schema(config_.slot_schema_json,     false, "prod", schema_dirs);
        core_.fz_spec = resolve_schema(config_.flexzone_schema_json, true,  "prod", schema_dirs);

        if (config_.has_inbox())
            inbox_spec_ = resolve_schema(config_.inbox_schema_json, false, "prod", schema_dirs);
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("[prod] Schema parse error: {}", e.what());
        return false;
    }

    // Register FFI types for slot, flexzone, inbox.
    // After registration, query ffi.sizeof() for the actual struct size (accounts for alignment).
    const std::string packing = config_.zmq_packing.empty() ? "aligned" : config_.zmq_packing;

    if (slot_spec_.has_schema)
    {
        auto cdef = build_ffi_cdef_(slot_spec_, slot_ffi_type_.c_str(), packing);
        if (cdef.empty() || !register_ffi_type_(cdef))
        {
            LOGGER_ERROR("[prod] Failed to register FFI type for slot schema");
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
            LOGGER_ERROR("[prod] Failed to register FFI type for flexzone schema");
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
            LOGGER_ERROR("[prod] Failed to register FFI type for inbox schema");
            return false;
        }
        inbox_schema_slot_size_ = ffi_sizeof_(inbox_ffi_type_.c_str());
    }

    return true;
}

void LuaProducerHost::print_validate_layout()
{
    std::cout << "\nProducer (Lua): " << config_.producer_uid << "\n";
    if (slot_spec_.has_schema)
        std::cout << "  Output slot: " << slot_ffi_type_
                  << " (" << schema_slot_size_ << " bytes)\n";
    if (core_.fz_spec.has_schema)
        std::cout << "  FlexZone: " << fz_ffi_type_
                  << " (" << core_.schema_fz_size << " bytes)\n";
}

// ============================================================================
// Virtual hooks — API table
// ============================================================================

void LuaProducerHost::build_role_api_table_(lua_State *L)
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

    push_closure("uid",                      lua_api_uid);
    push_closure("name",                     lua_api_name);
    push_closure("channel",                  lua_api_channel);
    push_closure("broadcast",                lua_api_broadcast);
    push_closure("send",                     lua_api_send);
    push_closure("consumers",                lua_api_consumers);
    push_closure("update_flexzone_checksum", lua_api_update_flexzone_checksum);
    push_closure("out_written",              lua_api_out_written);
    push_closure("drops",                    lua_api_drops);

    // api table is now on top of the stack — caller stores in registry.
}

// ============================================================================
// Lua API closures — retrieve `this` from upvalue(1)
// ============================================================================

int LuaProducerHost::lua_api_uid(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.producer_uid.c_str());
    return 1;
}

int LuaProducerHost::lua_api_name(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.producer_name.c_str());
    return 1;
}

int LuaProducerHost::lua_api_channel(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->config_.channel.c_str());
    return 1;
}

int LuaProducerHost::lua_api_broadcast(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
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

int LuaProducerHost::lua_api_send(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
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

int LuaProducerHost::lua_api_consumers(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
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

int LuaProducerHost::lua_api_update_flexzone_checksum(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    if (self->queue_)
        self->queue_->sync_flexzone_checksum();
    lua_pushboolean(L, self->queue_ ? 1 : 0);
    return 1;
}

int LuaProducerHost::lua_api_out_written(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->out_slots_written_.load(std::memory_order_relaxed)));
    return 1;
}

int LuaProducerHost::lua_api_drops(lua_State *L)
{
    auto *self = static_cast<LuaProducerHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->out_drops_.load(std::memory_order_relaxed)));
    return 1;
}

// ============================================================================
// snapshot_metrics_json — for heartbeat reporting
// ============================================================================

nlohmann::json LuaProducerHost::snapshot_metrics_json() const
{
    nlohmann::json base;
    base["out_written"]        = out_slots_written_.load(std::memory_order_relaxed);
    base["drops"]              = out_drops_.load(std::memory_order_relaxed);
    base["script_errors"]      = script_errors_.load(std::memory_order_relaxed);
    base["last_cycle_work_us"] = last_cycle_work_us_.load(std::memory_order_relaxed);
    base["loop_overrun_count"] = 0; // No ProducerAPI; TODO: wire SHM metrics

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

bool LuaProducerHost::start_role()
{
    if (!build_role_types())
        return false;

    hub::ProducerOptions opts;
    opts.channel_name = config_.channel;
    opts.pattern      = hub::ChannelPattern::PubSub;
    opts.has_shm      = config_.shm_enabled;
    opts.schema_hash  = scripting::compute_schema_hash(slot_spec_, core_.fz_spec);
    opts.actor_name   = config_.producer_name;
    opts.actor_uid    = config_.producer_uid;

    if (config_.target_period_ms > 0)
    {
        opts.loop_policy = hub::LoopPolicy::FixedRate;
        opts.period_ms   = std::chrono::milliseconds{config_.target_period_ms};
    }
    opts.ctrl_queue_max_depth = config_.ctrl_queue_max_depth;
    opts.peer_dead_timeout_ms = config_.peer_dead_timeout_ms;

    // Inbox setup (optional)
    if (config_.has_inbox())
    {
        const std::string ep = config_.inbox_endpoint.empty()
            ? "tcp://127.0.0.1:0"
            : config_.inbox_endpoint;
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
        const std::string packing = config_.zmq_packing.empty() ? "aligned" : config_.zmq_packing;
        const int inbox_rcvhwm = (config_.inbox_overflow_policy == "block")
            ? 0
            : static_cast<int>(config_.inbox_buffer_depth);
        inbox_queue_ = hub::InboxQueue::bind_at(ep, std::move(zmq_fields),
                                                  packing, inbox_rcvhwm);
        if (!inbox_queue_ || !inbox_queue_->start())
        {
            LOGGER_ERROR("[prod] Failed to start InboxQueue for channel '{}'", config_.channel);
            if (inbox_queue_) inbox_queue_.reset();
            return false;
        }
        opts.inbox_endpoint    = inbox_queue_->actual_endpoint();
        opts.inbox_schema_json = spec_json.dump();
        opts.inbox_packing     = packing;
    }

    if (config_.shm_enabled)
    {
        opts.shm_config.shared_secret        = config_.shm_secret;
        opts.shm_config.ring_buffer_capacity = config_.shm_slot_count;
        opts.shm_config.policy               = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = config_.shm_consumer_sync_policy;
        opts.shm_config.checksum_policy      = hub::ChecksumPolicy::Manual;
        opts.shm_config.flex_zone_size       = core_.schema_fz_size;

        opts.shm_config.physical_page_size = hub::system_page_size();
        opts.shm_config.logical_unit_size  =
            (schema_slot_size_ == 0) ? 1 : schema_slot_size_;
    }

    if (!config_.broker.empty())
    {
        if (!out_messenger_.connect(config_.broker, config_.broker_pubkey,
                                    config_.auth.client_pubkey, config_.auth.client_seckey))
        {
            LOGGER_ERROR("[prod] broker connect failed ({}); aborting", config_.broker);
            return false;
        }
    }

    auto maybe_producer = hub::Producer::create(out_messenger_, opts);
    if (!maybe_producer.has_value())
    {
        LOGGER_ERROR("[prod] Failed to create producer for channel '{}'", config_.channel);
        return false;
    }
    out_producer_ = std::move(maybe_producer);

    if (auto *out_shm = out_producer_->shm(); out_shm != nullptr)
        out_shm->clear_metrics();

    if (!config_.channel.empty())
    {
        out_messenger_.suppress_periodic_heartbeat(config_.channel);
        out_messenger_.enqueue_heartbeat(config_.channel);
    }

    // Wire event callbacks → IncomingMessage queue.
    out_producer_->on_channel_closing([this]() {
        IncomingMessage msg;
        msg.event = "channel_closing";
        core_.enqueue_message(std::move(msg));
    });

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

    out_messenger_.on_consumer_died(config_.channel,
        [this](uint64_t pid, std::string reason) {
            IncomingMessage msg;
            msg.event = "consumer_died";
            msg.details["pid"] = pid;
            msg.details["reason"] = std::move(reason);
            core_.enqueue_message(std::move(msg));
        });

    out_messenger_.on_channel_error(config_.channel,
        [this](std::string event, nlohmann::json details) {
            IncomingMessage msg;
            msg.event = "channel_event";
            msg.details = std::move(details);
            msg.details["detail"] = std::move(event);
            core_.enqueue_message(std::move(msg));
        });

    out_producer_->on_consumer_message(
        [this](const std::string &identity, std::span<const std::byte> data) {
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

    // Wire peer-dead and hub-dead monitoring.
    out_producer_->on_peer_dead([this]() {
        stop_reason_.store(static_cast<int>(scripting::LuaStopReason::PeerDead),
                           std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    out_messenger_.on_hub_dead([this]() {
        stop_reason_.store(static_cast<int>(scripting::LuaStopReason::HubDead),
                           std::memory_order_relaxed);
        core_.shutdown_requested.store(true, std::memory_order_release);
    });

    // Create transport queue.
    if (config_.transport == Transport::Shm)
    {
        auto *out_shm = out_producer_->shm();
        if (out_shm == nullptr)
        {
            LOGGER_ERROR("[prod] transport=shm but SHM unavailable for channel '{}'",
                         config_.channel);
            return false;
        }
        queue_ = hub::ShmQueue::from_producer_ref(
            *out_shm, schema_slot_size_, core_.schema_fz_size, config_.channel);
    }
    else
    {
        auto zmq_fields = scripting::schema_spec_to_zmq_fields(slot_spec_, schema_slot_size_);
        const auto zmq_policy = (config_.zmq_overflow_policy == "block")
                                     ? hub::OverflowPolicy::Block
                                     : hub::OverflowPolicy::Drop;
        queue_ = hub::ZmqQueue::push_to(
            config_.zmq_out_endpoint, std::move(zmq_fields), config_.zmq_packing,
            config_.zmq_out_bind, std::nullopt, 0, config_.zmq_buffer_depth, zmq_policy);
        if (!queue_)
        {
            LOGGER_ERROR("[prod] Failed to create ZmqQueue for endpoint '{}'",
                         config_.zmq_out_endpoint);
            return false;
        }
    }

    if (!queue_->start())
    {
        LOGGER_ERROR("[prod] Queue::start() failed for channel '{}'", config_.channel);
        queue_.reset();
        return false;
    }
    queue_->set_checksum_options(config_.update_checksum, core_.has_fz);

    LOGGER_INFO("[prod] Lua producer started on channel '{}' (shm={})", config_.channel,
                out_producer_->has_shm());

    // Startup coordination (HEP-0023).
    if (!wait_for_roles_(out_messenger_, config_.wait_for_roles))
        return false;

    core_.running_threads.store(true);
    ctrl_thread_ = std::thread([this] { run_ctrl_thread_(); });

    // on_init is called on the worker thread (which owns lua_State) — safe.
    call_on_init_common_();

    return true;
}

void LuaProducerHost::stop_role()
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

    if (queue_)
    {
        queue_->stop();
        queue_.reset();
    }

    out_messenger_.on_hub_dead(nullptr);

    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }

    // Release Lua callback refs.
    if (ref_on_produce_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_produce_);
        ref_on_produce_ = LUA_NOREF;
    }
    if (ref_on_inbox_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_inbox_);
        ref_on_inbox_ = LUA_NOREF;
    }

    LOGGER_INFO("[prod] Lua producer stopped.");
}

void LuaProducerHost::cleanup_on_start_failure()
{
    if (inbox_queue_)
    {
        inbox_queue_->stop();
        inbox_queue_.reset();
    }
    if (queue_)
    {
        queue_->stop();
        queue_.reset();
    }
    if (out_producer_.has_value())
    {
        out_producer_->stop();
        out_producer_->close();
        out_producer_.reset();
    }
}

void LuaProducerHost::update_fz_checksum_after_init()
{
    if (queue_)
        queue_->sync_flexzone_checksum();
}

// ============================================================================
// run_data_loop_ — transport-agnostic timer-driven production loop (Lua)
//
// All Lua callbacks (on_produce, on_inbox) run on THIS thread — the worker
// thread that owns lua_State. No GIL, no separate loop/inbox threads.
// ============================================================================

void LuaProducerHost::run_data_loop_()
{
    if (!queue_)
    {
        LOGGER_ERROR("[prod] run_data_loop_: transport queue not initialized — aborting");
        core_.running_threads.store(false);
        return;
    }

    const auto acquire_timeout = std::chrono::milliseconds{
        pylabhub::compute_slot_acquire_timeout(
            config_.slot_acquire_timeout_ms, config_.target_period_ms)};

    auto next_deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds{config_.target_period_ms};

    const bool is_fixed_rate = (config_.loop_timing != LoopTimingPolicy::MaxRate);
    const auto period        = std::chrono::milliseconds{config_.target_period_ms};

    while (core_.running_threads.load() && !core_.shutdown_requested.load() &&
           !critical_error_.load(std::memory_order_relaxed))
    {
        const auto iter_start = std::chrono::steady_clock::now();

        void *buf = queue_->write_acquire(acquire_timeout);
        auto msgs = core_.drain_messages();

        if (!buf)
        {
            const bool due = !is_fixed_rate || iter_start >= next_deadline;

            if (!msgs.empty() || due)
            {
                (void)call_on_produce_no_slot_(msgs);

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
            out_drops_.fetch_add(1, std::memory_order_relaxed);
            iteration_count_.fetch_add(1, std::memory_order_relaxed);

            // Synchronous inbox drain.
            drain_inbox_sync_();
            continue;
        }

        const size_t buf_sz = queue_->item_size();
        std::memset(buf, 0, buf_sz);

        bool commit = call_on_produce_(buf, buf_sz, msgs);

        if (commit)
        {
            queue_->write_commit();
            out_slots_written_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            queue_->write_discard();
            out_drops_.fetch_add(1, std::memory_order_relaxed);
        }

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

    LOGGER_INFO("[prod] run_data_loop_ exiting: running_threads={} shutdown_requested={} "
                "critical_error={}",
                core_.running_threads.load(), core_.shutdown_requested.load(),
                critical_error_.load());
}

// ============================================================================
// push_flexzone_view_ — push flexzone cdata or nil onto Lua stack
// ============================================================================

void LuaProducerHost::push_flexzone_view_()
{
    if (core_.has_fz)
    {
        void *fz = queue_->write_flexzone();
        size_t fz_sz = queue_->flexzone_size();
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
// call_on_produce_ — with slot
// ============================================================================

bool LuaProducerHost::call_on_produce_(void *buf, size_t buf_sz,
                                        std::vector<IncomingMessage> &msgs)
{
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_produce_);

    if (!push_slot_view_(buf, buf_sz, slot_ffi_type_.c_str()))
    {
        lua_pop(L_, 1); // pop function
        return false;
    }

    push_flexzone_view_();
    push_messages_table_(msgs);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);

    // Call: on_produce(out_slot, fz, msgs, api) → 1 result
    if (lua_pcall(L_, 4, 1, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[prod] on_produce error: {}", err ? err : "(unknown)");
        lua_pop(L_, 1);
        on_script_error();
        if (config_.stop_on_script_error)
            core_.running_threads.store(false);
        return false;
    }

    // Parse return: nil/true → commit, false → discard.
    bool commit = lua_isnil(L_, -1) || lua_toboolean(L_, -1);
    lua_pop(L_, 1);
    return commit;
}

// ============================================================================
// call_on_produce_no_slot_ — without slot (acquire failed)
// ============================================================================

bool LuaProducerHost::call_on_produce_no_slot_(std::vector<IncomingMessage> &msgs)
{
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_produce_);
    lua_pushnil(L_); // out_slot = nil

    push_flexzone_view_();
    push_messages_table_(msgs);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);

    if (lua_pcall(L_, 4, 1, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[prod] on_produce (no slot) error: {}", err ? err : "(unknown)");
        lua_pop(L_, 1);
        on_script_error();
        if (config_.stop_on_script_error)
            core_.running_threads.store(false);
        return false;
    }

    lua_pop(L_, 1); // discard return value (no slot to commit)
    return false;
}

// ============================================================================
// run_ctrl_thread_ — polls producer peer socket, sends heartbeats
// ============================================================================

void LuaProducerHost::run_ctrl_thread_()
{
    scripting::ZmqPollLoop loop{core_, "prod:" + config_.producer_uid};
    loop.sockets = {
        {out_producer_->peer_ctrl_socket_handle(),
         [&] { out_producer_->handle_peer_events_nowait(); }},
    };
    loop.get_iteration = [&] { return iteration_count_.load(std::memory_order_relaxed); };
    loop.periodic_tasks.emplace_back(
        [&] { out_messenger_.enqueue_heartbeat(config_.channel,
                                                    snapshot_metrics_json()); },
        config_.heartbeat_interval_ms);
    loop.run();
}

} // namespace pylabhub::producer
