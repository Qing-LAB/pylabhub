/**
 * @file lua_role_host_base.cpp
 * @brief LuaRoleHostBase — common do_lua_work_() skeleton and helpers.
 *
 * This file contains the common Lua role host skeleton shared by all three
 * role script hosts (LuaProducerHost, LuaConsumerHost, LuaProcessorHost).
 * Role-specific behavior is dispatched via virtual hooks.
 */
#include "lua_role_host_base.hpp"

#include "utils/format_tools.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "plh_platform.hpp"
#include "plh_version_registry.hpp"

#include <lua.hpp>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace pylabhub::scripting
{

// ============================================================================
// Destructor
// ============================================================================

LuaRoleHostBase::~LuaRoleHostBase()
{
    shutdown_();
}

// ============================================================================
// Common lifecycle
// ============================================================================

void LuaRoleHostBase::startup_()
{
    worker_thread_ = std::thread([this] { do_lua_work_(); });
    // Block until the worker signals ready (script loaded, role started or failed).
    ready_future_.get();
}

void LuaRoleHostBase::shutdown_() noexcept
{
    signal_shutdown();

    if (worker_thread_.joinable())
        worker_thread_.join();

    // Lua state cleanup happens in do_lua_work_() on the worker thread
    // (which owns the lua_State).  After join(), the state is already closed.
    // Clear the raw pointer as a safety net.
    L_ = nullptr;
}

void LuaRoleHostBase::signal_ready_()
{
    ready_promise_.set_value();
}

void LuaRoleHostBase::signal_shutdown() noexcept
{
    core_.shutdown_requested.store(true, std::memory_order_release);
}

// ============================================================================
// do_lua_work_ — the common skeleton (mirrors PythonRoleHostBase::do_python_work)
// ============================================================================

void LuaRoleHostBase::do_lua_work_()
{
    // ── Early exit: signal fired before we started ───────────────────────────
    if (core_.shutdown_requested.load(std::memory_order_acquire))
    {
        core_.script_load_ok = false;
        signal_ready_();
        return;
    }

    // ── Create Lua state via RAII wrapper ────────────────────────────────────
    state_ = LuaState::create();
    if (!state_)
    {
        LOGGER_ERROR("[{}] LuaRoleHostBase: luaL_newstate() failed (out of memory)", role_tag());
        core_.script_load_ok = false;
        signal_ready_();
        return;
    }

    // Keep raw pointer for subclass data loops and static closures.
    L_ = state_.raw();

    state_.cache_ffi_cast();
    state_.apply_sandbox();

    // Setup package.path for LuaJIT runtime libs.
    {
        try
        {
            const fs::path exe         = platform::get_executable_name(true);
            const fs::path luajit_home = fs::weakly_canonical(exe.parent_path() / ".." / "opt" / "luajit");
            if (fs::exists(luajit_home))
                state_.add_package_path((luajit_home / "jit" / "?.lua").string());
        }
        catch (const std::exception &e)
        {
            LOGGER_WARN("[{}] Could not derive luajit home: {}", role_tag(), e.what());
        }
    }

    // Register InboxHandle metatable (needed by api.open_inbox()).
    register_inbox_handle_metatable_();

    // ── Load script ──────────────────────────────────────────────────────────
    const fs::path script_dir = fs::path(script_base_dir()) / "script" / "lua";
    const fs::path init_lua   = script_dir / "init.lua";

    if (!fs::exists(init_lua))
    {
        LOGGER_ERROR("[{}] Lua script not found: {}", role_tag(), init_lua.string());
        core_.script_load_ok = false;
        signal_ready_();
        return;
    }

    state_.add_package_path((script_dir / "?.lua").string());

    if (!state_.load_script(init_lua, role_tag()))
    {
        core_.script_load_ok = false;
        signal_ready_();
        return;
    }

    LOGGER_INFO("[{}] Loaded Lua script: {}", role_tag(), init_lua.string());

    // ── Extract callbacks ────────────────────────────────────────────────────
    extract_callbacks_();

    if (!has_required_callback())
    {
        LOGGER_ERROR("[{}] Script has no '{}' function — {} not started",
                     role_tag(), required_callback_name(), role_name());
        core_.script_load_ok = false;
        signal_ready_();
        return;
    }

    // Extract common callbacks: on_init, on_stop.
    lua_getglobal(L_, "on_init");
    ref_on_init_ = lua_isfunction(L_, -1) ? luaL_ref(L_, LUA_REGISTRYINDEX) : (lua_pop(L_, 1), LUA_NOREF);

    lua_getglobal(L_, "on_stop");
    ref_on_stop_ = lua_isfunction(L_, -1) ? luaL_ref(L_, LUA_REGISTRYINDEX) : (lua_pop(L_, 1), LUA_NOREF);

    core_.script_load_ok = true;

    // ── Validate mode: print layout and exit ─────────────────────────────────
    if (core_.validate_only)
    {
        if (!build_role_types())
        {
            signal_ready_();
            return;
        }
        print_validate_layout();
        signal_ready_();
        return;
    }

    // ── Build API table and store in registry ────────────────────────────────
    build_role_api_table_(L_);
    ref_api_ = luaL_ref(L_, LUA_REGISTRYINDEX);

    // ── Start role ───────────────────────────────────────────────────────────
    if (!start_role())
    {
        LOGGER_ERROR("[{}] Failed to start {}", role_tag(), role_name());
        cleanup_on_start_failure();
        signal_ready_();
        return;
    }

    core_.running = true;
    signal_ready_(); // Unblocks startup_() on the main thread.

    // ── Data loop ────────────────────────────────────────────────────────────
    // Runs on THIS thread — the worker thread that owns lua_State.
    // All Lua callbacks (on_produce, on_consume, on_process, on_inbox) happen here.
    // Replaces the Python model where loop_thread_ acquires the GIL per-callback.
    run_data_loop_();

    // If internal shutdown (api.stop()), propagate to the main thread.
    if (core_.shutdown_requested.load() && core_.g_shutdown)
        core_.g_shutdown->store(true, std::memory_order_release);

    // ── Stop role ────────────────────────────────────────────────────────────
    stop_role();
    core_.running = false;

    // ── Cleanup Lua state on the owning thread ──────────────────────────────
    clear_lua_refs_();
    state_ = LuaState{};  // lua_close via RAII, on the thread that created it
    L_ = nullptr;

    LOGGER_INFO("[{}] LuaRoleHostBase: all done", role_tag());
}

// ============================================================================
// FFI schema generation
// ============================================================================

std::string ffi_c_type_for_field(const std::string &type_str)
{
    if (type_str == "bool")    return "bool";
    if (type_str == "int8")    return "int8_t";
    if (type_str == "uint8")   return "uint8_t";
    if (type_str == "int16")   return "int16_t";
    if (type_str == "uint16")  return "uint16_t";
    if (type_str == "int32")   return "int32_t";
    if (type_str == "uint32")  return "uint32_t";
    if (type_str == "int64")   return "int64_t";
    if (type_str == "uint64")  return "uint64_t";
    if (type_str == "float32") return "float";
    if (type_str == "float64") return "double";
    if (type_str == "string")  return "char";   // with [length]
    if (type_str == "bytes")   return "uint8_t"; // with [length]
    return {};
}

std::string LuaRoleHostBase::build_ffi_cdef_(const SchemaSpec &spec, const char *struct_name,
                                              const std::string &packing)
{
    std::ostringstream ss;

    // Packed structs use #pragma pack / __attribute__((packed)).
    // For LuaJIT FFI, __attribute__((packed)) works.
    const bool is_packed = (packing == "packed");

    ss << "typedef struct ";
    if (is_packed)
        ss << "__attribute__((packed)) ";
    ss << "{\n";

    for (const auto &f : spec.fields)
    {
        const std::string c_type = ffi_c_type_for_field(f.type_str);
        if (c_type.empty())
        {
            LOGGER_ERROR("LuaRoleHostBase: unsupported field type '{}' in schema", f.type_str);
            return {};
        }

        ss << "    " << c_type << " " << f.name;

        // Array dimension: string/bytes use length, numeric use count.
        if (f.type_str == "string" || f.type_str == "bytes")
        {
            ss << "[" << f.length << "]";
        }
        else if (f.count > 1)
        {
            ss << "[" << f.count << "]";
        }

        ss << ";\n";
    }

    ss << "} " << struct_name << ";\n";
    return ss.str();
}

bool LuaRoleHostBase::register_ffi_type_(const std::string &cdef_str)
{
    if (cdef_str.empty())
        return false;
    return state_.register_ffi_type(cdef_str, role_tag());
}

size_t LuaRoleHostBase::ffi_sizeof_(const char *type_name)
{
    return state_.ffi_sizeof(type_name);
}

bool LuaRoleHostBase::push_slot_view_(void *data, size_t size, const char *type_name)
{
    return state_.push_slot_view(data, size, type_name);
}

bool LuaRoleHostBase::push_slot_view_readonly_(const void *data, size_t size, const char *type_name)
{
    return state_.push_slot_view_readonly(data, size, type_name);
}


// ============================================================================
// Message table builder
// ============================================================================

void LuaRoleHostBase::push_messages_table_(std::vector<IncomingMessage> &msgs)
{
    lua_newtable(L_);
    int idx = 1;
    for (auto &m : msgs)
    {
        if (!m.event.empty())
        {
            // Event message → Lua table: {event="...", key=val, ...}
            lua_newtable(L_);
            lua_pushstring(L_, m.event.c_str());
            lua_setfield(L_, -2, "event");
            for (auto &[key, val] : m.details.items())
            {
                if (val.is_string())
                {
                    lua_pushstring(L_, val.get<std::string>().c_str());
                }
                else if (val.is_boolean())
                {
                    lua_pushboolean(L_, val.get<bool>() ? 1 : 0);
                }
                else if (val.is_number_integer())
                {
                    lua_pushinteger(L_, val.get<lua_Integer>());
                }
                else if (val.is_number_float())
                {
                    lua_pushnumber(L_, val.get<double>());
                }
                else
                {
                    lua_pushstring(L_, val.dump().c_str());
                }
                lua_setfield(L_, -2, key.c_str());
            }
        }
        else
        {
            // Data message → Lua table: {sender="hex", data="bytes"}
            lua_newtable(L_);
            lua_pushstring(L_, format_tools::bytes_to_hex(m.sender).c_str());
            lua_setfield(L_, -2, "sender");
            lua_pushlstring(L_, reinterpret_cast<const char *>(m.data.data()), m.data.size());
            lua_setfield(L_, -2, "data");
        }
        lua_rawseti(L_, -2, idx++);
    }
}

// ============================================================================
// call_on_init_common_ / call_on_stop_common_
// ============================================================================

void LuaRoleHostBase::call_on_init_common_()
{
    if (!is_ref_callable_(ref_on_init_))
        return;

    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_init_); // function
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);     // arg: api

    if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
    {
        on_script_error();
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[{}] on_init error: {}", role_tag(), err ? err : "(unknown)");
        lua_pop(L_, 1);
    }

    update_fz_checksum_after_init();
}

void LuaRoleHostBase::call_on_stop_common_()
{
    if (!is_ref_callable_(ref_on_stop_) || !has_connection_for_stop())
        return;

    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_stop_); // function
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);     // arg: api

    if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
    {
        on_script_error();
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[{}] on_stop error: {}", role_tag(), err ? err : "(unknown)");
        lua_pop(L_, 1);
    }
}

// ============================================================================
// Lua helpers
// ============================================================================

void LuaRoleHostBase::clear_lua_refs_()
{
    if (!L_)
        return;
    if (ref_on_init_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_init_);
        ref_on_init_ = LUA_NOREF;
    }
    if (ref_on_stop_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_stop_);
        ref_on_stop_ = LUA_NOREF;
    }
    if (ref_api_ != LUA_NOREF)
    {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref_api_);
        ref_api_ = LUA_NOREF;
    }
    // ffi.cast cache is managed by LuaState — cleaned up in its destructor.

    // Stop all cached InboxClients before lua_close.
    for (auto &[uid, entry] : inbox_cache_)
    {
        if (entry.client)
            entry.client->stop();
    }
    inbox_cache_.clear();
}

bool LuaRoleHostBase::call_lua_fn_(const char *name, int nargs, int nresults)
{
    // Get the function by name; if absent, discard args and return.
    lua_getglobal(L_, name);
    if (!lua_isfunction(L_, -1))
    {
        lua_pop(L_, 1 + nargs);
        return true; // absent callback is not an error
    }

    // Move function below its args: stack is [..., arg1, ..., argN, fn]
    // We need: [..., fn, arg1, ..., argN]
    lua_insert(L_, -(nargs + 1));

    if (lua_pcall(L_, nargs, nresults, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_WARN("[{}] Lua error in '{}': {}", role_tag(), name, err ? err : "(unknown)");
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaRoleHostBase::is_ref_callable_(int ref) const
{
    return state_.is_ref_callable(ref);
}

// ============================================================================
// Common API closures — shared across all Lua role hosts
// ============================================================================

void LuaRoleHostBase::push_common_api_closures_(lua_State *L)
{
    // Helper: push a C closure with `this` as upvalue(1).
    auto push_closure = [&](const char *name, lua_CFunction fn) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };

    push_closure("log",                lua_api_log);
    push_closure("stop",               lua_api_stop);
    push_closure("set_critical_error", lua_api_set_critical_error);
    push_closure("stop_reason",        lua_api_stop_reason);
    push_closure("script_errors",      lua_api_script_errors);
    push_closure("version_info",       lua_api_version_info);
    push_closure("wait_for_role",      lua_api_wait_for_role);
    push_closure("open_inbox",         lua_api_open_inbox);

    // String fields as direct table entries.
    lua_pushstring(L, config_log_level().c_str());
    lua_setfield(L, -2, "log_level");

    lua_pushstring(L, config_script_path().c_str());
    lua_setfield(L, -2, "script_dir");

    lua_pushstring(L, config_role_dir().c_str());
    lua_setfield(L, -2, "role_dir");
}

int LuaRoleHostBase::lua_api_log(lua_State *L)
{
    auto *self = static_cast<LuaRoleHostBase *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *level = luaL_checkstring(L, 1);
    const char *msg   = luaL_checkstring(L, 2);

    const auto tag = fmt::format("[{}-lua]", self->role_tag());
    if (std::string_view(level) == "error")
        LOGGER_ERROR("{} {}", tag, msg);
    else if (std::string_view(level) == "warn" || std::string_view(level) == "warning")
        LOGGER_WARN("{} {}", tag, msg);
    else if (std::string_view(level) == "debug")
        LOGGER_DEBUG("{} {}", tag, msg);
    else
        LOGGER_INFO("{} {}", tag, msg);

    return 0;
}

int LuaRoleHostBase::lua_api_stop(lua_State *L)
{
    auto *self = static_cast<LuaRoleHostBase *>(lua_touserdata(L, lua_upvalueindex(1)));
    self->core_.shutdown_requested.store(true, std::memory_order_release);
    return 0;
}

int LuaRoleHostBase::lua_api_set_critical_error(lua_State *L)
{
    auto *self = static_cast<LuaRoleHostBase *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *msg = lua_tostring(L, 1);
    LOGGER_ERROR("[{}-lua] CRITICAL: {}", self->role_tag(), msg ? msg : "(no message)");
    self->core_.set_critical_error();
    return 0;
}

int LuaRoleHostBase::lua_api_stop_reason(lua_State *L)
{
    auto *self = static_cast<LuaRoleHostBase *>(lua_touserdata(L, lua_upvalueindex(1)));
    int reason = self->core_.stop_reason_.load(std::memory_order_relaxed);
    const char *name = "normal";
    switch (reason)
    {
    case 1: name = "peer_dead"; break;
    case 2: name = "hub_dead"; break;
    case 3: name = "critical_error"; break;
    default: break;
    }
    lua_pushstring(L, name);
    return 1;
}

int LuaRoleHostBase::lua_api_version_info(lua_State *L)
{
    (void)L; // no upvalue needed — version is global
    lua_pushstring(L, pylabhub::version::version_info_json().c_str());
    return 1;
}

int LuaRoleHostBase::lua_api_script_errors(lua_State *L)
{
    auto *self = static_cast<LuaRoleHostBase *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->core_.script_errors_.load(std::memory_order_relaxed)));
    return 1;
}

// ============================================================================
// lua_api_wait_for_role — poll broker for role presence
// ============================================================================

int LuaRoleHostBase::lua_api_wait_for_role(lua_State *L)
{
    auto *self = static_cast<LuaRoleHostBase *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *uid = luaL_checkstring(L, 1);
    int timeout_ms  = static_cast<int>(luaL_optinteger(L, 2, 5000));

    auto *messenger = self->role_messenger_();
    if (!messenger)
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        const int poll_ms = static_cast<int>(std::min<long long>(kPollMs, remaining));
        if (messenger->query_role_presence(uid, poll_ms))
        {
            lua_pushboolean(L, 1);
            return 1;
        }
    }

    lua_pushboolean(L, 0);
    return 1;
}

// ============================================================================
// lua_api_open_inbox — discover target's inbox and return InboxHandle userdata
// ============================================================================

int LuaRoleHostBase::lua_api_open_inbox(lua_State *L)
{
    auto *self = static_cast<LuaRoleHostBase *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *target_uid = luaL_checkstring(L, 1);

    // Cache hit — return existing handle.
    auto it = self->inbox_cache_.find(target_uid);
    if (it != self->inbox_cache_.end() && it->second.client && it->second.client->is_running())
    {
        auto *ud = static_cast<LuaInboxUD *>(lua_newuserdata(L, sizeof(LuaInboxUD)));
        new (ud) LuaInboxUD{self, target_uid};
        luaL_getmetatable(L, kInboxHandleMT);
        lua_setmetatable(L, -2);
        return 1;
    }

    auto *messenger = self->role_messenger_();
    if (!messenger)
    {
        lua_pushnil(L);
        return 1;
    }

    // Broker round-trip — discover target's inbox endpoint and schema.
    auto info = messenger->query_role_info(target_uid, /*timeout_ms=*/1000);
    if (!info.has_value())
    {
        lua_pushnil(L);
        return 1;
    }

    if (!info->inbox_schema.is_object() || !info->inbox_schema.contains("fields"))
    {
        lua_pushnil(L);
        return 1;
    }

    SchemaSpec spec;
    try
    {
        spec = parse_schema_json(info->inbox_schema);
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN("[{}] open_inbox('{}'): schema parse error: {}", self->role_tag(), target_uid, e.what());
        lua_pushnil(L);
        return 1;
    }

    // Build FFI type for inbox slot.
    std::string ffi_type = fmt::format("InboxSlot_{}", target_uid);
    // Sanitize: replace hyphens with underscores for valid C identifier.
    for (auto &c : ffi_type)
        if (c == '-') c = '_';

    std::string cdef = build_ffi_cdef_(spec, ffi_type.c_str(), info->inbox_packing);
    if (!self->register_ffi_type_(cdef))
    {
        LOGGER_WARN("[{}] open_inbox('{}'): FFI cdef failed", self->role_tag(), target_uid);
        lua_pushnil(L);
        return 1;
    }

    size_t item_size = self->ffi_sizeof_(ffi_type.c_str());
    if (item_size == 0)
    {
        LOGGER_WARN("[{}] open_inbox('{}'): ffi.sizeof returned 0", self->role_tag(), target_uid);
        lua_pushnil(L);
        return 1;
    }

    // Build ZMQ schema fields for the InboxClient wire format.
    auto zmq_fields = schema_spec_to_zmq_fields(spec, item_size);

    // Connect DEALER to target's ROUTER.
    auto client_ptr = hub::InboxClient::connect_to(
        info->inbox_endpoint, self->role_uid(), std::move(zmq_fields), info->inbox_packing);
    if (!client_ptr)
    {
        LOGGER_WARN("[{}] open_inbox('{}'): connect_to '{}' failed",
                    self->role_tag(), target_uid, info->inbox_endpoint);
        lua_pushnil(L);
        return 1;
    }
    if (!client_ptr->start())
    {
        LOGGER_WARN("[{}] open_inbox('{}'): start() failed", self->role_tag(), target_uid);
        lua_pushnil(L);
        return 1;
    }

    // Cache the entry.
    auto shared_client = std::shared_ptr<hub::InboxClient>(std::move(client_ptr));
    self->inbox_cache_[target_uid] = LuaInboxEntry{shared_client, ffi_type, item_size};

    // Return userdata handle.
    auto *ud = static_cast<LuaInboxUD *>(lua_newuserdata(L, sizeof(LuaInboxUD)));
    new (ud) LuaInboxUD{self, target_uid};
    luaL_getmetatable(L, kInboxHandleMT);
    lua_setmetatable(L, -2);
    return 1;
}

// ============================================================================
// Lua InboxHandle metatable methods
// ============================================================================

void LuaRoleHostBase::register_inbox_handle_metatable_()
{
    if (luaL_newmetatable(L_, kInboxHandleMT))
    {
        // __index points to itself for method dispatch.
        lua_pushvalue(L_, -1);
        lua_setfield(L_, -2, "__index");

        auto set_method = [&](const char *name, lua_CFunction fn) {
            lua_pushcfunction(L_, fn);
            lua_setfield(L_, -2, name);
        };

        set_method("acquire",  lua_inbox_acquire);
        set_method("send",     lua_inbox_send);
        set_method("discard",  lua_inbox_discard);
        set_method("is_ready", lua_inbox_is_ready);
        set_method("close",    lua_inbox_close);
    }
    lua_pop(L_, 1); // pop metatable
}

LuaRoleHostBase::LuaInboxEntry *LuaRoleHostBase::get_inbox_entry_(lua_State *L)
{
    auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(L, 1, kInboxHandleMT));
    auto it = ud->host->inbox_cache_.find(ud->target_uid);
    if (it == ud->host->inbox_cache_.end())
        return nullptr;
    return &it->second;
}

int LuaRoleHostBase::lua_inbox_acquire(lua_State *L)
{
    auto *entry = get_inbox_entry_(L);
    if (!entry || !entry->client || !entry->client->is_running())
    {
        lua_pushnil(L);
        return 1;
    }

    void *buf = entry->client->acquire();
    if (!buf)
    {
        lua_pushnil(L);
        return 1;
    }

    // Push writable FFI slot view.
    auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(L, 1, kInboxHandleMT));
    if (!ud->host->push_slot_view_(buf, entry->item_size, entry->ffi_type.c_str()))
    {
        lua_pushnil(L);
        return 1;
    }
    return 1;
}

int LuaRoleHostBase::lua_inbox_send(lua_State *L)
{
    auto *entry = get_inbox_entry_(L);
    if (!entry || !entry->client || !entry->client->is_running())
    {
        lua_pushinteger(L, 255);
        return 1;
    }

    int timeout_ms = static_cast<int>(luaL_optinteger(L, 2, 5000));
    uint8_t rc = entry->client->send(std::chrono::milliseconds{timeout_ms});
    lua_pushinteger(L, static_cast<lua_Integer>(rc));
    return 1;
}

int LuaRoleHostBase::lua_inbox_discard(lua_State *L)
{
    auto *entry = get_inbox_entry_(L);
    if (entry && entry->client)
        entry->client->abort();
    return 0;
}

int LuaRoleHostBase::lua_inbox_is_ready(lua_State *L)
{
    auto *entry = get_inbox_entry_(L);
    lua_pushboolean(L, (entry && entry->client && entry->client->is_running()) ? 1 : 0);
    return 1;
}

int LuaRoleHostBase::lua_inbox_close(lua_State *L)
{
    auto *entry = get_inbox_entry_(L);
    if (entry && entry->client)
        entry->client->stop();
    return 0;
}

// ============================================================================
// drain_inbox_sync_ — shared non-blocking inbox drain
// ============================================================================

void LuaRoleHostBase::drain_inbox_sync_()
{
    if (!inbox_queue_ || ref_on_inbox_ == LUA_NOREF)
        return;

    static constexpr auto kNonBlocking = std::chrono::milliseconds{0};

    while (core_.running_threads.load() && !core_.shutdown_requested.load())
    {
        const auto *item = inbox_queue_->recv_one(kNonBlocking);
        if (!item) break;

        uint8_t ack_code = 0;

        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_on_inbox_);

        // Push inbox slot view.
        if (inbox_spec_.has_schema && inbox_schema_slot_size_ > 0)
        {
            if (!push_slot_view_readonly_(item->data, inbox_schema_slot_size_,
                                          inbox_ffi_type_.c_str()))
            {
                lua_pop(L_, 1); // pop function
                ack_code = 3;
                inbox_queue_->send_ack(ack_code);
                continue;
            }
        }
        else
        {
            // Schemaless or zero-size: push raw bytes using the queue's item size.
            lua_pushlstring(L_, reinterpret_cast<const char *>(item->data),
                           inbox_queue_->item_size());
        }

        lua_pushstring(L_, item->sender_id.c_str());
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_api_);

        // Call: on_inbox(slot, sender, api)
        if (lua_pcall(L_, 3, 0, 0) != LUA_OK)
        {
            const char *err = lua_tostring(L_, -1);
            LOGGER_ERROR("[{}] on_inbox error: {}", role_tag(), err ? err : "(unknown)");
            lua_pop(L_, 1);
            on_script_error();
            ack_code = 3;
            if (stop_on_script_error())
                core_.shutdown_requested.store(true, std::memory_order_release);
        }

        inbox_queue_->send_ack(ack_code);
    }
}

// ============================================================================
// wait_for_roles_ — HEP-0023 startup coordination
// ============================================================================

bool LuaRoleHostBase::wait_for_roles_(hub::Messenger &messenger,
                                       const std::vector<pylabhub::WaitForRole> &wait_list)
{
    for (const auto &wr : wait_list)
    {
        LOGGER_INFO("[{}] Startup: waiting for role '{}' (timeout {}ms)...",
                    role_tag(), wr.uid, wr.timeout_ms);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{wr.timeout_ms};
        static constexpr int kPollMs = 200;
        bool found = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            const int poll_ms = static_cast<int>(std::min<long long>(kPollMs, remaining));
            if (messenger.query_role_presence(wr.uid, poll_ms))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            LOGGER_ERROR("[{}] Startup wait failed: role '{}' not present after {}ms",
                         role_tag(), wr.uid, wr.timeout_ms);
            return false;
        }
    }
    return true;
}

} // namespace pylabhub::scripting
