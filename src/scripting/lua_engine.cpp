/**
 * @file lua_engine.cpp
 * @brief LuaEngine — ScriptEngine implementation for LuaJIT.
 *
 * All invoke methods call lua_pcall directly.  No GIL, no locking.
 * All calls must happen on the thread that called initialize().
 *
 * Ported from the legacy LuaRoleHostBase / LuaProducerHost / LuaConsumerHost /
 * LuaProcessorHost monolithic implementations.
 */
#include "lua_engine.hpp"
#include "metrics_lua.hpp"

#include "utils/format_tools.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/messenger.hpp"
#include "utils/shared_memory_spinlock.hpp"
#include "utils/logger.hpp"
#include "utils/schema_utils.hpp"
#include "plh_platform.hpp"

#include <cassert>
#include "plh_version_registry.hpp"

#include <lua.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace pylabhub::scripting
{

// ============================================================================
// Inbox handle metatable name and userdata layout
// ============================================================================

// ============================================================================
// lua_to_json — recursive Lua stack value → nlohmann::json conversion
// ============================================================================

static nlohmann::json lua_to_json(lua_State *L, int idx, int depth = 0)
{
    if (depth > kScriptMaxRecursionDepth)
        return "<recursion limit>";

    int t = lua_type(L, idx);
    switch (t)
    {
    case LUA_TNIL:
        return nullptr;
    case LUA_TBOOLEAN:
        return static_cast<bool>(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        return lua_tonumber(L, idx);
    case LUA_TSTRING:
        return std::string(lua_tostring(L, idx));
    case LUA_TTABLE:
    {
        // Detect array vs object: if consecutive integer keys 1..N, treat as array.
        int len = static_cast<int>(lua_objlen(L, idx));
        if (len > 0)
        {
            nlohmann::json arr = nlohmann::json::array();
            for (int i = 1; i <= len; ++i)
            {
                lua_rawgeti(L, idx, i);
                arr.push_back(lua_to_json(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return arr;
        }
        // Object (string keys).
        nlohmann::json obj = nlohmann::json::object();
        lua_pushnil(L);
        while (lua_next(L, idx < 0 ? idx - 1 : idx) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                std::string key = lua_tostring(L, -2);
                obj[key] = lua_to_json(L, -1, depth + 1);
            }
            lua_pop(L, 1); // pop value, keep key
        }
        return obj;
    }
    default:
        return "<" + std::string(lua_typename(L, t)) + ">";
    }
}

// ============================================================================
// json_to_lua — recursive nlohmann::json → Lua stack push
// ============================================================================

static void json_to_lua(lua_State *L, const nlohmann::json &val, int depth = 0)
{
    if (depth > kScriptMaxRecursionDepth)
    {
        lua_pushstring(L, "<recursion limit>");
        return;
    }

    if (val.is_null())
        lua_pushnil(L);
    else if (val.is_boolean())
        lua_pushboolean(L, val.get<bool>() ? 1 : 0);
    else if (val.is_number_integer())
        lua_pushinteger(L, val.get<lua_Integer>());
    else if (val.is_number_float())
        lua_pushnumber(L, val.get<double>());
    else if (val.is_string())
        lua_pushstring(L, val.get<std::string>().c_str());
    else if (val.is_object())
    {
        lua_newtable(L);
        for (auto &[k, v] : val.items())
        {
            json_to_lua(L, v, depth + 1);
            lua_setfield(L, -2, k.c_str());
        }
    }
    else if (val.is_array())
    {
        lua_newtable(L);
        int idx = 1;
        for (auto &elem : val)
        {
            json_to_lua(L, elem, depth + 1);
            lua_rawseti(L, -2, idx++);
        }
    }
    else
    {
        lua_pushstring(L, val.dump().c_str());
    }
}

static constexpr const char *kInboxHandleMT = "pylabhub.InboxHandle";

struct LuaInboxUD
{
    LuaEngine  *engine;
    std::string target_uid;
};

// ============================================================================
// Destructor
// ============================================================================

LuaEngine::~LuaEngine()
{
    finalize();
}

// ============================================================================
// initialize — create LuaState, sandbox, luajit path, inbox metatable
// ============================================================================

bool LuaEngine::init_engine_(const std::string &log_tag, RoleHostCore *core)
{
    log_tag_ = log_tag.empty() ? "lua" : log_tag;
    // owner_thread_id_, accepting_, api_->core() set by base class initialize().

    state_ = LuaState::create();
    if (!state_)
    {
        LOGGER_ERROR("[{}] LuaEngine: luaL_newstate() failed (out of memory)", log_tag_);
        return false;
    }

    state_.apply_sandbox();
    state_.cache_ffi_cast();

    // Add LuaJIT runtime library path.
    try
    {
        const fs::path exe = platform::get_executable_name(true);
        const fs::path luajit_home =
            fs::weakly_canonical(exe.parent_path() / ".." / "opt" / "luajit");
        if (fs::exists(luajit_home))
            state_.add_package_path((luajit_home / "jit" / "?.lua").string());
    }
    catch (const std::exception &e)
    {
        LOGGER_WARN("[{}] Could not derive luajit home: {}", log_tag_, e.what());
    }

    // Register InboxHandle metatable (needed by api.open_inbox()).
    register_inbox_metatable_();

    return true;
}

// ============================================================================
// load_script — add script dir to package.path, load, extract callbacks
// ============================================================================

bool LuaEngine::load_script(const std::filesystem::path &script_dir,
                             const std::string &entry_point,
                             const std::string &required_callback)
{
    script_dir_str_ = script_dir.string();
    entry_point_ = entry_point.empty() ? "init.lua" : entry_point;
    required_callback_ = required_callback;

    // Add script dir to package.path for require().
    state_.add_package_path((script_dir / "?.lua").string());

    const fs::path script_file = script_dir / entry_point_;
    if (!fs::exists(script_file))
    {
        LOGGER_ERROR("[{}] Script not found: {}", log_tag_, script_file.string());
        return false;
    }

    if (!state_.load_script(script_file, log_tag_.c_str()))
        return false;

    LOGGER_INFO("[{}] Loaded Lua script: {}", log_tag_, script_file.string());

    // Extract all callback refs.
    ref_on_init_ = extract_callback_ref_("on_init");
    ref_on_stop_ = extract_callback_ref_("on_stop");
    ref_on_produce_ = extract_callback_ref_("on_produce");
    ref_on_consume_ = extract_callback_ref_("on_consume");
    ref_on_process_ = extract_callback_ref_("on_process");
    ref_on_inbox_ = extract_callback_ref_("on_inbox");

    // Check that the required callback is present.
    if (!required_callback_.empty() && !has_callback(required_callback_))
    {
        LOGGER_ERROR("[{}] Script has no '{}' function", log_tag_, required_callback_);
        return false;
    }

    return true;
}

// ============================================================================
// build_api — build Lua table with closures using RoleAPIBase
// ============================================================================

bool LuaEngine::build_api_(RoleAPIBase &api)
{
    stop_on_script_error_ = api.stop_on_script_error();

    // Create role-specific aliases (SlotFrame/FlexFrame) for single-direction roles.
    // Producer: SlotFrame = OutSlotFrame, FlexFrame = OutFlexFrame.
    // Consumer: SlotFrame = InSlotFrame, FlexFrame = InFlexFrame.
    // Processor: no aliases — both directions are explicit.
    // The alias is a FFI typedef so scripts can use either name.
    if (api.role_tag() == "prod")
    {
        if (ref_out_slot_writable_ != LUA_NOREF)
        {
            state_.register_ffi_type("typedef OutSlotFrame SlotFrame;", log_tag_.c_str());
            ref_slot_alias_writable_ = state_.cache_ffi_typeof("SlotFrame", /*readonly=*/false);
        }
        if (ref_out_fz_ != LUA_NOREF)
        {
            state_.register_ffi_type("typedef OutFlexFrame FlexFrame;", log_tag_.c_str());
            ref_fz_alias_ = state_.cache_ffi_typeof("FlexFrame", /*readonly=*/false);
        }
    }
    else if (api.role_tag() == "cons")
    {
        if (ref_in_slot_readonly_ != LUA_NOREF)
        {
            state_.register_ffi_type("typedef InSlotFrame SlotFrame;", log_tag_.c_str());
            ref_slot_alias_readonly_ = state_.cache_ffi_typeof("SlotFrame", /*readonly=*/true);
        }
        if (ref_in_fz_ != LUA_NOREF)
        {
            state_.register_ffi_type("typedef InFlexFrame FlexFrame;", log_tag_.c_str());
            ref_fz_alias_ = state_.cache_ffi_typeof("FlexFrame", /*readonly=*/false);
        }
    }

    lua_State *L = state_.raw();

    // Create the api table.
    lua_newtable(L);

    // Common closures.
    push_common_api_closures_(L);

    // Register metatables for userdata types.
    register_spinlock_metatable_();

    // Helper: push a C closure with `this` as upvalue(1).
    auto push_closure = [&](const char *name, lua_CFunction fn) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };

    // ── ChannelSide constants (all roles) ────────────────────────────────
    lua_pushinteger(L, static_cast<lua_Integer>(ChannelSide::Tx));
    lua_setfield(L, -2, "Tx");
    lua_pushinteger(L, static_cast<lua_Integer>(ChannelSide::Rx));
    lua_setfield(L, -2, "Rx");

    // ── Common closures (all roles) ────────────────────────────────────
    push_closure("uid", lua_api_uid);
    push_closure("name", lua_api_name);
    push_closure("channel", lua_api_channel);
    push_closure("out_slots_written", lua_api_out_slots_written);
    push_closure("in_slots_received", lua_api_in_slots_received);
    push_closure("out_drop_count", lua_api_out_drop_count);
    push_closure("metrics", lua_api_metrics);

    // ── Role-specific closures based on role_tag ────────────────────────
    if (api_->role_tag() == "prod")
    {
        if (api_->producer())
        {
            push_closure("broadcast", lua_api_broadcast);
            push_closure("send", lua_api_send);
            push_closure("consumers", lua_api_consumers);
            push_closure("update_flexzone_checksum", lua_api_update_flexzone_checksum);
        }
        push_closure("flexzone", lua_api_flexzone);
        push_closure("slot_logical_size", lua_api_slot_logical_size);
        push_closure("flexzone_logical_size", lua_api_flexzone_logical_size);
        push_closure("spinlock", lua_api_spinlock);
        push_closure("spinlock_count", lua_api_spinlock_count);
        push_closure("out_capacity", lua_api_out_capacity);
        push_closure("out_policy", lua_api_out_policy);
    }
    else if (api_->role_tag() == "cons")
    {
        if (api_->consumer())
            push_closure("set_verify_checksum", lua_api_set_verify_checksum);
        push_closure("slot_logical_size", lua_api_slot_logical_size);
        push_closure("flexzone_logical_size", lua_api_flexzone_logical_size);
        push_closure("spinlock", lua_api_spinlock);
        push_closure("spinlock_count", lua_api_spinlock_count);
        push_closure("in_capacity", lua_api_in_capacity);
        push_closure("in_policy", lua_api_in_policy);
        push_closure("last_seq", lua_api_last_seq);
    }
    else if (api_->role_tag() == "proc")
    {
        push_closure("in_channel", lua_api_in_channel);
        push_closure("out_channel", lua_api_out_channel);
        if (api_->producer())
        {
            push_closure("broadcast", lua_api_broadcast);
            push_closure("send", lua_api_send);
            push_closure("consumers", lua_api_consumers);
            push_closure("update_flexzone_checksum", lua_api_update_flexzone_checksum);
        }
        push_closure("flexzone", lua_api_flexzone);
        push_closure("slot_logical_size", lua_api_slot_logical_size);
        push_closure("flexzone_logical_size", lua_api_flexzone_logical_size);
        push_closure("spinlock", lua_api_spinlock);
        push_closure("spinlock_count", lua_api_spinlock_count);
        push_closure("out_capacity", lua_api_out_capacity);
        push_closure("out_policy", lua_api_out_policy);
        if (api_->consumer())
            push_closure("set_verify_checksum", lua_api_set_verify_checksum);
        push_closure("in_capacity", lua_api_in_capacity);
        push_closure("in_policy", lua_api_in_policy);
        push_closure("last_seq", lua_api_last_seq);
    }
    else
    {
        LOGGER_ERROR("[{}] build_api: unknown role_tag '{}' — must be 'prod', 'cons', or 'proc'",
                     log_tag_, api_->role_tag());
        lua_pop(L, 1); // pop the api table
        return false;
    }

    // Store api table in the registry AND as a global (for generic invoke).
    lua_pushvalue(L, -1);            // duplicate the api table
    lua_setglobal(L, "api");         // set as global for scripts calling api.* from free functions
    ref_api_ = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

// ============================================================================
// finalize — clear all refs, inbox cache, reset state
// ============================================================================

void LuaEngine::finalize_engine_()
{
    if (!state_)
        return;

    // accepting_ already set false by base class finalize().

    // Destroy all child thread states.
    {
        std::lock_guard lk(thread_states_mu_);
        thread_states_.clear();
    }

    // 3. Clear Lua refs. Shared resources (inbox cache, shared data) are
    //    owned by RoleHostCore — cleaned up by the role host, not the engine.
    clear_refs_();

    // 4. Destroy primary state.
    state_ = LuaState{}; // lua_close via RAII
}

// ============================================================================
// Thread-state cache
// ============================================================================

LuaEngine *LuaEngine::get_or_create_thread_state_()
{
    auto tid = std::this_thread::get_id();
    std::lock_guard lk(thread_states_mu_);
    auto it = thread_states_.find(tid);
    if (it != thread_states_.end())
        return it->second.get();

    // Create fully initialized child state.
    auto child = std::make_unique<LuaEngine>();
    child->set_owner_engine(this); // child delegates is_accepting() to parent
    if (!child->initialize(log_tag_, api_->core()))
    {
        LOGGER_ERROR("[{}] Failed to create thread-local LuaEngine", log_tag_);
        return nullptr;
    }
    if (!child->load_script(script_dir_str_, entry_point_,
                             required_callback_))
    {
        LOGGER_ERROR("[{}] Thread-local LuaEngine: load_script failed", log_tag_);
        child->finalize();
        return nullptr;
    }
    if (!child->build_api(*api_))
    {
        LOGGER_ERROR("[{}] Thread-local LuaEngine: build_api failed", log_tag_);
        child->finalize();
        return nullptr;
    }

    auto *ptr = child.get();
    thread_states_.emplace(tid, std::move(child));
    return ptr;
}

void LuaEngine::release_thread()
{
    auto tid = std::this_thread::get_id();
    if (tid == owner_thread_id_)
        return; // owner thread — don't destroy the primary state

    std::lock_guard lk(thread_states_mu_);
    auto it = thread_states_.find(tid);
    if (it != thread_states_.end())
    {
        it->second->finalize();
        thread_states_.erase(it);
    }
}

// ============================================================================
// Generic invoke / eval
// ============================================================================

bool LuaEngine::invoke(const std::string &name)
{
    if (name.empty() || !is_accepting())
        return false;

    // Non-owner thread: dispatch to thread-local state.
    if (std::this_thread::get_id() != owner_thread_id_)
    {
        auto *child = get_or_create_thread_state_();
        if (!child)
            return false;
        return child->invoke(name);
    }

    // Owner thread: direct pcall.
    executing_.store(true, std::memory_order_release);
    auto *L = state_.raw();
    lua_getglobal(L, name.c_str());
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        executing_.store(false, std::memory_order_release);
        return false;
    }
    bool ok = (lua_pcall(L, 0, 0, 0) == 0);
    if (!ok)
    {
        const char *err = lua_tostring(L, -1);
        LOGGER_ERROR("[{}] invoke('{}'): {}", log_tag_, name, err ? err : "unknown error");
        lua_pop(L, 1);
        on_pcall_error_(name);
    }
    executing_.store(false, std::memory_order_release);
    return ok;
}

bool LuaEngine::invoke(const std::string &name, const nlohmann::json &args)
{
    if (name.empty() || !is_accepting())
        return false;

    // Non-owner thread: dispatch to thread-local state.
    if (std::this_thread::get_id() != owner_thread_id_)
    {
        auto *child = get_or_create_thread_state_();
        if (!child)
            return false;
        return child->invoke(name, args);
    }

    // Owner thread: push args as Lua table, call.
    executing_.store(true, std::memory_order_release);
    auto *L = state_.raw();
    lua_getglobal(L, name.c_str());
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        executing_.store(false, std::memory_order_release);
        return false;
    }

    // Push args as a Lua table (recursive for nested objects/arrays).
    json_to_lua(L, args);

    bool ok = (lua_pcall(L, 1, 0, 0) == 0);
    if (!ok)
    {
        const char *err = lua_tostring(L, -1);
        LOGGER_ERROR("[{}] invoke('{}', args): {}", log_tag_, name, err ? err : "unknown error");
        lua_pop(L, 1);
        on_pcall_error_(name);
    }
    executing_.store(false, std::memory_order_release);
    return ok;
}

InvokeResponse LuaEngine::eval(const std::string &code)
{
    if (code.empty())
        return {InvokeStatus::NotFound, {}};
    if (!is_accepting())
        return {InvokeStatus::EngineShutdown, {}};

    if (std::this_thread::get_id() != owner_thread_id_)
    {
        auto *child = get_or_create_thread_state_();
        if (!child)
            return {InvokeStatus::ScriptError, {}};
        return child->eval(code);
    }

    executing_.store(true, std::memory_order_release);
    auto *L = state_.raw();
    if (luaL_dostring(L, code.c_str()) != 0)
    {
        on_pcall_error_("eval");
        executing_.store(false, std::memory_order_release);
        return {InvokeStatus::ScriptError, {}};
    }

    // Capture top-of-stack result as JSON (recursive for tables).
    nlohmann::json val;
    if (lua_gettop(L) > 0)
    {
        val = lua_to_json(L, lua_gettop(L));
        lua_settop(L, 0);
    }
    executing_.store(false, std::memory_order_release);
    return {InvokeStatus::Ok, std::move(val)};
}

// ============================================================================
// has_callback
// ============================================================================

bool LuaEngine::has_callback(const std::string &name) const
{
    if (name.empty())
        return false;

    if (name == "on_init")
        return state_.is_ref_callable(ref_on_init_);
    if (name == "on_stop")
        return state_.is_ref_callable(ref_on_stop_);
    if (name == "on_produce")
        return state_.is_ref_callable(ref_on_produce_);
    if (name == "on_consume")
        return state_.is_ref_callable(ref_on_consume_);
    if (name == "on_process")
        return state_.is_ref_callable(ref_on_process_);
    if (name == "on_inbox")
        return state_.is_ref_callable(ref_on_inbox_);

    // Unknown callback name — check as a global function.
    lua_State *L = state_.raw();
    lua_getglobal(L, name.c_str());
    bool is_fn = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return is_fn;
}

// ============================================================================
// build_ffi_cdef_ — shared hub::SchemaSpec → FFI typedef string builder
// ============================================================================

std::string LuaEngine::build_ffi_cdef_(const hub::SchemaSpec &spec,
                                        const std::string &type_name,
                                        const std::string &packing)
{
    std::ostringstream ss;
    const bool is_packed = (packing == "packed");

    ss << "typedef struct ";
    if (is_packed)
        ss << "__attribute__((packed)) ";
    ss << "{\n";

    for (const auto &f : spec.fields)
    {
        std::string c_type;
        if (f.type_str == "bool")         c_type = "bool";
        else if (f.type_str == "int8")    c_type = "int8_t";
        else if (f.type_str == "uint8")   c_type = "uint8_t";
        else if (f.type_str == "int16")   c_type = "int16_t";
        else if (f.type_str == "uint16")  c_type = "uint16_t";
        else if (f.type_str == "int32")   c_type = "int32_t";
        else if (f.type_str == "uint32")  c_type = "uint32_t";
        else if (f.type_str == "int64")   c_type = "int64_t";
        else if (f.type_str == "uint64")  c_type = "uint64_t";
        else if (f.type_str == "float32") c_type = "float";
        else if (f.type_str == "float64") c_type = "double";
        else if (f.type_str == "string")  c_type = "char";
        else if (f.type_str == "bytes")   c_type = "uint8_t";
        else
        {
            LOGGER_ERROR("[{}] Unsupported field type '{}' in schema", log_tag_, f.type_str);
            return {};
        }

        ss << "    " << c_type << " " << f.name;
        if (f.type_str == "string" || f.type_str == "bytes")
            ss << "[" << f.length << "]";
        else if (f.count > 1)
            ss << "[" << f.count << "]";
        ss << ";\n";
    }

    ss << "} " << type_name << ";\n";
    return ss.str();
}

// ============================================================================
// register_slot_type / type_sizeof
// ============================================================================

bool LuaEngine::register_slot_type(const hub::SchemaSpec &spec,
                                    const std::string &type_name,
                                    const std::string &packing)
{
    if (!spec.has_schema)
    {
        LOGGER_ERROR("[{}] register_slot_type('{}') called with has_schema=false",
                     log_tag_, type_name);
        return false;
    }

    // Compute expected size from schema (infrastructure-authoritative).
    auto [layout, expected_size] = hub::compute_field_layout(to_field_descs(spec.fields), packing);

    std::string cdef = build_ffi_cdef_(spec, type_name, packing);
    if (cdef.empty())
        return false;

    if (!state_.register_ffi_type(cdef, log_tag_.c_str()))
        return false;

    // Validate: FFI struct size must match schema-computed size.
    size_t actual_size = state_.ffi_sizeof(type_name.c_str());
    if (actual_size != expected_size)
    {
        LOGGER_ERROR("[{}] register_slot_type('{}') size mismatch: "
                     "ffi={}, schema={}",
                     log_tag_, type_name, actual_size, expected_size);
        return false;
    }

    // Cache ffi.typeof() for slot views — no string ops per invoke call.
    // All known type names must be cached (see script_engine.hpp §register_slot_type).
    // Helper: unref old ref before overwriting to prevent registry leak.
    auto safe_cache = [&](int &ref, const std::string &name, bool readonly) {
        if (ref != LUA_NOREF)
            state_.unref(ref);
        ref = state_.cache_ffi_typeof(name.c_str(), readonly);
    };
    if (type_name == "InSlotFrame")
    {
        safe_cache(ref_in_slot_readonly_, type_name, /*readonly=*/true);
    }
    else if (type_name == "OutSlotFrame")
    {
        safe_cache(ref_out_slot_writable_, type_name, /*readonly=*/false);
    }
    else if (type_name == "InFlexFrame")
    {
        safe_cache(ref_in_fz_, type_name, /*readonly=*/false);
    }
    else if (type_name == "OutFlexFrame")
    {
        safe_cache(ref_out_fz_, type_name, /*readonly=*/false);
    }
    else if (type_name == "InboxFrame")
    {
        safe_cache(ref_inbox_readonly_, type_name, /*readonly=*/true);
    }

    return true;
}

size_t LuaEngine::type_sizeof(const std::string &type_name) const
{
    return state_.ffi_sizeof(type_name.c_str());
}

// ============================================================================
// invoke_on_init
// ============================================================================

void LuaEngine::invoke_on_init()
{
    if (!state_.is_ref_callable(ref_on_init_))
        return;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_init_);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    if (!state_.pcall(1, 0, "on_init"))
    {
        api_->core()->inc_script_error_count();
    }
}

// ============================================================================
// invoke_on_stop
// ============================================================================

void LuaEngine::invoke_on_stop()
{
    if (!state_.is_ref_callable(ref_on_stop_))
        return;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_stop_);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    if (!state_.pcall(1, 0, "on_stop"))
    {
        api_->core()->inc_script_error_count();
    }
}

// ============================================================================
// invoke_produce — on_produce(tx, msgs, api) -> bool
// ============================================================================

InvokeResult LuaEngine::invoke_produce(
    InvokeTx tx,
    std::vector<IncomingMessage> &msgs)
{
    if (!state_.is_ref_callable(ref_on_produce_))
        return InvokeResult::Error;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_produce_);

    // Arg 1: tx table {slot=cdata, fz=cdata|nil}.
    lua_createtable(L, 0, 2);
    if (tx.slot != nullptr && state_.push_slot_view_cached(tx.slot, ref_out_slot_writable_))
        lua_setfield(L, -2, "slot");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "slot"); }

    if (tx.fz != nullptr && tx.fz_size > 0 && ref_out_fz_ != LUA_NOREF
        && state_.push_slot_view_cached(tx.fz, ref_out_fz_))
        lua_setfield(L, -2, "fz");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "fz"); }

    // Arg 2: messages table.
    push_messages_table_(msgs);

    // Arg 3: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(3 args, 1 result).
    if (!state_.pcall(3, 1, "on_produce"))
        return on_pcall_error_("on_produce");

    // Return value contract:
    //   true  → Commit (publish the slot)
    //   false → Discard (script chose not to publish)
    //   anything else → Error (only explicit 'return true' commits)
    InvokeResult result;
    if (lua_isboolean(L, -1))
    {
        result = lua_toboolean(L, -1) ? InvokeResult::Commit : InvokeResult::Discard;
    }
    else if (lua_isnil(L, -1))
    {
        LOGGER_ERROR("[{}] on_produce returned nil — explicit 'return true' or "
                     "'return false' is required. Treating as error.",
                     log_tag_);
        result = on_pcall_error_("on_produce [missing return value]");
    }
    else
    {
        LOGGER_ERROR("[{}] on_produce returned non-boolean type '{}' (value: {}) — "
                     "expected 'return true' or 'return false'. Treating as error.",
                     log_tag_,
                     lua_typename(L, lua_type(L, -1)),
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        result = on_pcall_error_("on_produce [wrong return type]");
    }
    lua_pop(L, 1);
    return result;
}

// ============================================================================
// invoke_consume — on_consume(rx, msgs, api) -> bool
// ============================================================================

InvokeResult LuaEngine::invoke_consume(
    InvokeRx rx,
    std::vector<IncomingMessage> &msgs)
{
    if (!state_.is_ref_callable(ref_on_consume_))
        return InvokeResult::Error;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_consume_);

    // Arg 1: rx table {slot=cdata, fz=cdata|nil}.
    lua_createtable(L, 0, 2);
    if (rx.slot != nullptr
        && state_.push_slot_view_cached(const_cast<void *>(rx.slot), ref_in_slot_readonly_))
        lua_setfield(L, -2, "slot");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "slot"); }

    if (rx.fz != nullptr && rx.fz_size > 0 && ref_in_fz_ != LUA_NOREF
        && state_.push_slot_view_cached(rx.fz, ref_in_fz_))
        lua_setfield(L, -2, "fz");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "fz"); }

    // Arg 2: messages table (bare variant for consumer).
    push_messages_table_bare_(msgs);

    // Arg 3: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(3 args, 1 result).
    if (!state_.pcall(3, 1, "on_consume"))
        return on_pcall_error_("on_consume");

    // Return value: True → Commit, False → Discard (currently ignored by loop).
    InvokeResult result;
    if (lua_isboolean(L, -1))
    {
        result = lua_toboolean(L, -1) ? InvokeResult::Commit : InvokeResult::Discard;
    }
    else if (lua_isnil(L, -1))
    {
        LOGGER_ERROR("[{}] on_consume returned nil — explicit 'return true' or "
                     "'return false' is required. Treating as error.",
                     log_tag_);
        result = on_pcall_error_("on_consume [missing return value]");
    }
    else
    {
        LOGGER_ERROR("[{}] on_consume returned non-boolean type '{}' — "
                     "expected 'return true' or 'return false'. Treating as error.",
                     log_tag_, lua_typename(L, lua_type(L, -1)));
        result = on_pcall_error_("on_consume [wrong return type]");
    }
    lua_pop(L, 1);
    return result;
}

// ============================================================================
// invoke_process — on_process(rx, tx, msgs, api) -> bool
// ============================================================================

InvokeResult LuaEngine::invoke_process(
    InvokeRx rx, InvokeTx tx,
    std::vector<IncomingMessage> &msgs)
{
    if (!state_.is_ref_callable(ref_on_process_))
        return InvokeResult::Error;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_process_);

    // Arg 1: rx table {slot=cdata, fz=cdata|nil}.
    lua_createtable(L, 0, 2);
    if (rx.slot != nullptr
        && state_.push_slot_view_cached(const_cast<void *>(rx.slot), ref_in_slot_readonly_))
        lua_setfield(L, -2, "slot");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "slot"); }

    if (rx.fz != nullptr && rx.fz_size > 0 && ref_in_fz_ != LUA_NOREF
        && state_.push_slot_view_cached(rx.fz, ref_in_fz_))
        lua_setfield(L, -2, "fz");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "fz"); }

    // Arg 2: tx table {slot=cdata, fz=cdata|nil}.
    lua_createtable(L, 0, 2);
    if (tx.slot != nullptr
        && state_.push_slot_view_cached(tx.slot, ref_out_slot_writable_))
        lua_setfield(L, -2, "slot");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "slot"); }

    if (tx.fz != nullptr && tx.fz_size > 0 && ref_out_fz_ != LUA_NOREF
        && state_.push_slot_view_cached(tx.fz, ref_out_fz_))
        lua_setfield(L, -2, "fz");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "fz"); }

    // Arg 3: messages table.
    push_messages_table_(msgs);

    // Arg 4: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(4 args, 1 result).
    if (!state_.pcall(4, 1, "on_process"))
        return on_pcall_error_("on_process");

    // Return value contract (same as on_produce).
    InvokeResult result;
    if (lua_isboolean(L, -1))
    {
        result = lua_toboolean(L, -1) ? InvokeResult::Commit : InvokeResult::Discard;
    }
    else if (lua_isnil(L, -1))
    {
        LOGGER_ERROR("[{}] on_process returned nil — explicit 'return true' or "
                     "'return false' is required. Treating as error.",
                     log_tag_);
        result = on_pcall_error_("on_process [missing return value]");
    }
    else
    {
        LOGGER_ERROR("[{}] on_process returned non-boolean type '{}' (value: {}) — "
                     "expected 'return true' or 'return false'. Treating as error.",
                     log_tag_,
                     lua_typename(L, lua_type(L, -1)),
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        result = on_pcall_error_("on_process [wrong return type]");
    }
    lua_pop(L, 1);
    return result;
}

// ============================================================================
// invoke_on_inbox — on_inbox(msg, api) -> bool
// msg = {data=cdata, sender_uid=string, seq=number}
// ============================================================================

InvokeResult LuaEngine::invoke_on_inbox(InvokeInbox msg)
{
    if (!state_.is_ref_callable(ref_on_inbox_))
        return InvokeResult::Error;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_inbox_);

    // Inbox type must be cached at startup via register_slot_type("InboxFrame").
    if (ref_inbox_readonly_ == LUA_NOREF)
    {
        LOGGER_ERROR("[{}] invoke_on_inbox: InboxFrame type not registered — "
                     "inbox_schema must be configured and registered before use",
                     log_tag_);
        api_->core()->inc_script_error_count();
        lua_pop(L, 1); // pop function
        return InvokeResult::Error;
    }

    // Arg 1: msg table {data=cdata, sender_uid=string, seq=number}.
    lua_createtable(L, 0, 3);

    if (msg.data != nullptr && msg.data_size > 0
        && state_.push_slot_view_cached(const_cast<void *>(msg.data), ref_inbox_readonly_))
        lua_setfield(L, -2, "data");
    else
    { lua_pushnil(L); lua_setfield(L, -2, "data"); }

    lua_pushstring(L, msg.sender_uid.c_str());
    lua_setfield(L, -2, "sender_uid");

    lua_pushinteger(L, static_cast<lua_Integer>(msg.seq));
    lua_setfield(L, -2, "seq");

    // Arg 2: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(2 args, 1 result).
    if (!state_.pcall(2, 1, "on_inbox"))
        return on_pcall_error_("on_inbox");

    // Return value: True → Commit, False → Discard (currently ignored by loop).
    InvokeResult result;
    if (lua_isboolean(L, -1))
    {
        result = lua_toboolean(L, -1) ? InvokeResult::Commit : InvokeResult::Discard;
    }
    else if (lua_isnil(L, -1))
    {
        LOGGER_ERROR("[{}] on_inbox returned nil — explicit 'return true' or "
                     "'return false' is required. Treating as error.",
                     log_tag_);
        result = on_pcall_error_("on_inbox [missing return value]");
    }
    else
    {
        LOGGER_ERROR("[{}] on_inbox returned non-boolean type '{}' — "
                     "expected 'return true' or 'return false'. Treating as error.",
                     log_tag_, lua_typename(L, lua_type(L, -1)));
        result = on_pcall_error_("on_inbox [wrong return type]");
    }
    lua_pop(L, 1);
    return result;
}

// ============================================================================
// Internal helpers
// ============================================================================

int LuaEngine::extract_callback_ref_(const char *name)
{
    lua_State *L = state_.raw();
    lua_getglobal(L, name);
    if (lua_isfunction(L, -1))
        return luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    return LUA_NOREF;
}

InvokeResult LuaEngine::on_pcall_error_(const std::string &callback_name)
{
    api_->core()->inc_script_error_count();
    // Hot-path callers (invoke_produce etc.) use state_.pcall() which logs the error.
    // Generic invoke() callers log the error themselves before calling this.
    if (stop_on_script_error_)
    {
        LOGGER_ERROR("[{}] stop_on_script_error: requesting shutdown after {} error",
                     log_tag_, callback_name);
        api_->core()->request_stop();
    }
    return InvokeResult::Error;
}

void LuaEngine::clear_refs_()
{
    state_.unref(ref_on_init_);
    ref_on_init_ = LUA_NOREF;
    state_.unref(ref_on_stop_);
    ref_on_stop_ = LUA_NOREF;
    state_.unref(ref_on_produce_);
    ref_on_produce_ = LUA_NOREF;
    state_.unref(ref_on_consume_);
    ref_on_consume_ = LUA_NOREF;
    state_.unref(ref_on_process_);
    ref_on_process_ = LUA_NOREF;
    state_.unref(ref_on_inbox_);
    ref_on_inbox_ = LUA_NOREF;
    state_.unref(ref_api_);
    ref_api_ = LUA_NOREF;

    // Release cached ffi.typeof refs (created during register_slot_type).
    state_.unref(ref_in_slot_readonly_);
    ref_in_slot_readonly_ = LUA_NOREF;
    state_.unref(ref_out_slot_writable_);
    ref_out_slot_writable_ = LUA_NOREF;
    state_.unref(ref_in_fz_);
    ref_in_fz_ = LUA_NOREF;
    state_.unref(ref_out_fz_);
    ref_out_fz_ = LUA_NOREF;
    state_.unref(ref_slot_alias_writable_);
    ref_slot_alias_writable_ = LUA_NOREF;
    state_.unref(ref_slot_alias_readonly_);
    ref_slot_alias_readonly_ = LUA_NOREF;
    state_.unref(ref_fz_alias_);
    ref_fz_alias_ = LUA_NOREF;
    state_.unref(ref_inbox_readonly_);
    ref_inbox_readonly_ = LUA_NOREF;
}

// ============================================================================
// push_messages_table_ — default: data msgs as {sender="hex", data="bytes"},
//                         event msgs as {event="...", key=val, ...}
// ============================================================================

void LuaEngine::push_messages_table_(std::vector<IncomingMessage> &msgs)
{
    lua_State *L = state_.raw();
    lua_newtable(L);
    int idx = 1;
    for (auto &m : msgs)
    {
        if (!m.event.empty())
        {
            // Event message -> Lua table: {event="...", key=val, ...}
            lua_newtable(L);
            lua_pushstring(L, m.event.c_str());
            lua_setfield(L, -2, "event");
            for (auto &[key, val] : m.details.items())
            {
                json_to_lua(L, val);
                lua_setfield(L, -2, key.c_str());
            }
        }
        else
        {
            // Data message -> Lua table: {sender="hex", data="bytes"}
            lua_newtable(L);
            lua_pushstring(L, format_tools::bytes_to_hex(m.sender).c_str());
            lua_setfield(L, -2, "sender");
            lua_pushlstring(L, reinterpret_cast<const char *>(m.data.data()), m.data.size());
            lua_setfield(L, -2, "data");
        }
        lua_rawseti(L, -2, idx++);
    }
}

// ============================================================================
// push_messages_table_bare_ — consumer variant: data msgs as plain byte strings
// ============================================================================

void LuaEngine::push_messages_table_bare_(std::vector<IncomingMessage> &msgs)
{
    lua_State *L = state_.raw();
    lua_newtable(L);
    int idx = 1;
    for (auto &m : msgs)
    {
        if (!m.event.empty())
        {
            // Event message -> Lua table: {event="...", key=val, ...}
            lua_newtable(L);
            lua_pushstring(L, m.event.c_str());
            lua_setfield(L, -2, "event");
            for (auto &[key, val] : m.details.items())
            {
                json_to_lua(L, val);
                lua_setfield(L, -2, key.c_str());
            }
        }
        else
        {
            // Data message -> bare bytes string (no sender for consumer).
            lua_pushlstring(L, reinterpret_cast<const char *>(m.data.data()), m.data.size());
        }
        lua_rawseti(L, -2, idx++);
    }
}

// ============================================================================
// push_common_api_closures_ — log, stop, critical_error, stop_reason, etc.
// ============================================================================

void LuaEngine::push_common_api_closures_(lua_State *L)
{
    auto push_closure = [&](const char *name, lua_CFunction fn) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };

    push_closure("log", lua_api_log);
    push_closure("stop", lua_api_stop);
    push_closure("set_critical_error", lua_api_set_critical_error);
    push_closure("critical_error", lua_api_critical_error);
    push_closure("stop_reason", lua_api_stop_reason);
    push_closure("script_error_count", lua_api_script_error_count);
    push_closure("version_info", lua_api_version_info);
    push_closure("wait_for_role", lua_api_wait_for_role);
    push_closure("open_inbox", lua_api_open_inbox);
    push_closure("clear_inbox_cache", lua_api_clear_inbox_cache);
    push_closure("get_shared_data", lua_api_get_shared_data);
    push_closure("set_shared_data", lua_api_set_shared_data);

    // Diagnostics — common to all roles.
    push_closure("loop_overrun_count", lua_api_loop_overrun_count);
    push_closure("last_cycle_work_us", lua_api_last_cycle_work_us);
    push_closure("ctrl_queue_dropped", lua_api_ctrl_queue_dropped);

    // Custom metrics (HEP-CORE-0019).
    push_closure("report_metric", lua_api_report_metric);
    push_closure("report_metrics", lua_api_report_metrics);
    push_closure("clear_custom_metrics", lua_api_clear_custom_metrics);

    // Channel pub/sub (HEP-CORE-0030).
    push_closure("join_channel", lua_api_join_channel);
    push_closure("leave_channel", lua_api_leave_channel);
    push_closure("send_channel_msg", lua_api_send_channel_msg);
    push_closure("channel_members", lua_api_channel_members);

    // String fields as direct table entries.
    lua_pushstring(L, api_->log_level().c_str());
    lua_setfield(L, -2, "log_level");

    lua_pushstring(L, api_->script_dir().c_str());
    lua_setfield(L, -2, "script_dir");

    lua_pushstring(L, api_->role_dir().c_str());
    lua_setfield(L, -2, "role_dir");

    // Derived directory paths.
    std::string logs = api_->role_dir().empty() ? "" : api_->role_dir() + "/logs";
    lua_pushstring(L, logs.c_str());
    lua_setfield(L, -2, "logs_dir");

    std::string run = api_->role_dir().empty() ? "" : api_->role_dir() + "/run";
    lua_pushstring(L, run.c_str());
    lua_setfield(L, -2, "run_dir");
}

// ============================================================================
// register_inbox_metatable_
// ============================================================================

void LuaEngine::register_inbox_metatable_()
{
    lua_State *L = state_.raw();
    if (luaL_newmetatable(L, kInboxHandleMT))
    {
        // __index points to itself for method dispatch.
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        // Static helper lambdas for inbox methods.
        // These do not need `this` as upvalue because they look up the engine
        // from the LuaInboxUD userdata.
        auto set_method = [&](const char *name, lua_CFunction fn) {
            lua_pushcfunction(L, fn);
            lua_setfield(L, -2, name);
        };

        set_method("acquire", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            auto entry = ud->engine->api_->core()->get_inbox_entry(ud->target_uid);
            if (!entry || !entry->client || !entry->client->is_running())
            {
                lua_pushnil(Ls);
                return 1;
            }

            void *buf = entry->client->acquire();
            if (!buf)
            {
                lua_pushnil(Ls);
                return 1;
            }

            if (!ud->engine->state_.push_slot_view(buf, entry->item_size,
                                                    entry->type_name.c_str()))
            {
                lua_pushnil(Ls);
                return 1;
            }
            return 1;
        });

        set_method("send", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            auto entry = ud->engine->api_->core()->get_inbox_entry(ud->target_uid);
            if (!entry || !entry->client || !entry->client->is_running())
            {
                lua_pushinteger(Ls, 255);
                return 1;
            }

            int timeout_ms = static_cast<int>(luaL_optinteger(Ls, 2, 5000));
            uint8_t rc = entry->client->send(std::chrono::milliseconds{timeout_ms});
            lua_pushinteger(Ls, static_cast<lua_Integer>(rc));
            return 1;
        });

        set_method("discard", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            auto entry = ud->engine->api_->core()->get_inbox_entry(ud->target_uid);
            if (entry && entry->client)
                entry->client->abort();
            return 0;
        });

        set_method("is_ready", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            auto entry = ud->engine->api_->core()->get_inbox_entry(ud->target_uid);
            lua_pushboolean(Ls,
                            (entry && entry->client && entry->client->is_running()) ? 1 : 0);
            return 1;
        });

        set_method("close", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            auto entry = ud->engine->api_->core()->get_inbox_entry(ud->target_uid);
            if (entry && entry->client)
                entry->client->stop();
            return 0;
        });

        // __gc: call destructor on LuaInboxUD to release std::string.
        set_method("__gc", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            ud->~LuaInboxUD();
            return 0;
        });
    }
    lua_pop(L, 1); // pop metatable
}

// ============================================================================
// API closure statics — common
// ============================================================================

int LuaEngine::lua_api_log(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *level = luaL_checkstring(L, 1);
    const char *msg = luaL_checkstring(L, 2);

    const auto tag = fmt::format("[{}-lua]", self->log_tag_);
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

int LuaEngine::lua_api_stop(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    LOGGER_INFO("[{}-lua] api.stop() called — requesting shutdown", self->log_tag_);
    self->api_->core()->request_stop();
    return 0;
}

int LuaEngine::lua_api_set_critical_error(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *msg = lua_tostring(L, 1);
    LOGGER_ERROR("[{}-lua] CRITICAL: {}", self->log_tag_, msg ? msg : "(no message)");
    self->api_->core()->set_critical_error();
    return 0;
}

int LuaEngine::lua_api_stop_reason(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->api_->core()->stop_reason_string().c_str());
    return 1;
}

int LuaEngine::lua_api_version_info(lua_State *L)
{
    lua_pushstring(L, pylabhub::version::version_info_json().c_str());
    return 1;
}

int LuaEngine::lua_api_script_error_count(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->api_->core()->script_error_count();
    lua_pushinteger(L, static_cast<lua_Integer>(val));
    return 1;
}

// ============================================================================
// lua_api_get_shared_data / lua_api_set_shared_data — shared script state
// ============================================================================

int LuaEngine::lua_api_get_shared_data(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *key = luaL_checkstring(L, 1);
    auto val = self->api_->core()->get_shared_data(key);
    if (!val)
    {
        lua_pushnil(L);
        return 1;
    }
    std::visit([L](const auto &v)
    {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int64_t>)
            lua_pushinteger(L, static_cast<lua_Integer>(v));
        else if constexpr (std::is_same_v<T, double>)
            lua_pushnumber(L, v);
        else if constexpr (std::is_same_v<T, bool>)
            lua_pushboolean(L, v ? 1 : 0);
        else if constexpr (std::is_same_v<T, std::string>)
            lua_pushlstring(L, v.data(), v.size());
    }, *val);
    return 1;
}

int LuaEngine::lua_api_set_shared_data(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *key = luaL_checkstring(L, 1);
    if (lua_isboolean(L, 2))
        self->api_->core()->set_shared_data(key, static_cast<bool>(lua_toboolean(L, 2)));
    else if (lua_isnumber(L, 2))
    {
        // LuaJIT: all numbers are doubles. Store as int64 if whole number.
        double num = lua_tonumber(L, 2);
        if (num == static_cast<double>(static_cast<int64_t>(num)))
            self->api_->core()->set_shared_data(key, static_cast<int64_t>(num));
        else
            self->api_->core()->set_shared_data(key, num);
    }
    else if (lua_isstring(L, 2))
        self->api_->core()->set_shared_data(key, std::string(lua_tostring(L, 2)));
    else
        self->api_->core()->remove_shared_data(key); // nil → remove key
    return 0;
}

// ============================================================================
// lua_api_wait_for_role — poll broker for role presence
// ============================================================================

int LuaEngine::lua_api_wait_for_role(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *uid = luaL_checkstring(L, 1);
    int timeout_ms = static_cast<int>(luaL_optinteger(L, 2, 5000));

    lua_pushboolean(L, self->api_->wait_for_role(uid, timeout_ms) ? 1 : 0);
    return 1;
}

// ============================================================================
// lua_api_open_inbox — discover target inbox and return InboxHandle userdata
// ============================================================================

int LuaEngine::lua_api_open_inbox(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *target_uid = luaL_checkstring(L, 1);

    // Delegate to ScriptEngine base — handles broker query, client
    // creation, and core_ cache in one place.
    auto result = self->api_->open_inbox_client(target_uid);
    if (!result)
    {
        lua_pushnil(L);
        return 1;
    }

    // Build FFI type for this inbox slot (Lua-specific).
    // On cache hit the spec is empty — type was already registered on first call.
    if (!result->spec.fields.empty())
    {
        std::string ffi_type = fmt::format("InboxSlot_{}", target_uid);
        for (auto &c : ffi_type)
            if (c == '-') c = '_';

        std::string cdef = self->build_ffi_cdef_(result->spec, ffi_type, result->packing);
        if (cdef.empty() || !self->state_.register_ffi_type(cdef, self->log_tag_.c_str()))
        {
            lua_pushnil(L);
            return 1;
        }
    }

    // Return userdata handle.
    auto *ud = static_cast<LuaInboxUD *>(lua_newuserdata(L, sizeof(LuaInboxUD)));
    new (ud) LuaInboxUD{self, target_uid};
    luaL_getmetatable(L, kInboxHandleMT);
    lua_setmetatable(L, -2);
    return 1;
}

// ============================================================================
// Role-specific API closures
// ============================================================================

int LuaEngine::lua_api_uid(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->api_->uid().c_str());
    return 1;
}

int LuaEngine::lua_api_name(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->api_->name().c_str());
    return 1;
}

int LuaEngine::lua_api_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->api_->channel().c_str());
    return 1;
}

int LuaEngine::lua_api_in_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->api_->channel().c_str());
    return 1;
}

int LuaEngine::lua_api_out_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->api_->out_channel().c_str());
    return 1;
}

int LuaEngine::lua_api_broadcast(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);

    auto *producer = self->api_->producer();
    if (!producer)
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    producer->send(data, len);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaEngine::lua_api_send(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *identity = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);

    auto *producer = self->api_->producer();
    if (!producer)
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    producer->send_to(identity, data, len);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaEngine::lua_api_consumers(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *producer = self->api_->producer();
    if (!producer)
    {
        lua_newtable(L);
        return 1;
    }

    auto list = producer->connected_consumers();
    lua_newtable(L);
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
    {
        lua_pushstring(L, format_tools::bytes_to_hex(list[static_cast<size_t>(i)]).c_str());
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int LuaEngine::lua_api_update_flexzone_checksum(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *p = self->api_->producer();
    if (p)
        p->sync_flexzone_checksum();
    lua_pushboolean(L, p ? 1 : 0);
    return 1;
}

int LuaEngine::lua_api_set_verify_checksum(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    bool enable = lua_toboolean(L, 1) != 0;
    auto *c = self->api_->consumer();
    if (c)
        c->set_verify_checksum(enable, false);
    return 0;
}

int LuaEngine::lua_api_out_slots_written(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->api_->core()->out_slots_written();
    lua_pushinteger(L, static_cast<lua_Integer>(val));
    return 1;
}

int LuaEngine::lua_api_in_slots_received(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->api_->core()->in_slots_received();
    lua_pushinteger(L, static_cast<lua_Integer>(val));
    return 1;
}

int LuaEngine::lua_api_out_drop_count(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->api_->core()->out_drop_count();
    lua_pushinteger(L, static_cast<lua_Integer>(val));
    return 1;
}

// ============================================================================
// Group A: diagnostics (common to all roles)
// ============================================================================

int LuaEngine::lua_api_loop_overrun_count(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->api_->core()->loop_overrun_count()));
    return 1;
}

int LuaEngine::lua_api_last_cycle_work_us(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(self->api_->core()->last_cycle_work_us()));
    return 1;
}

int LuaEngine::lua_api_critical_error(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushboolean(L, self->api_->core()->is_critical_error() ? 1 : 0);
    return 1;
}

// ============================================================================
// Group B: queue-state (role-specific)
// ============================================================================

int LuaEngine::lua_api_out_capacity(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *p = self->api_->producer();
    lua_pushinteger(L, p ? static_cast<lua_Integer>(p->queue_capacity()) : 0);
    return 1;
}

int LuaEngine::lua_api_out_policy(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *p = self->api_->producer();
    if (p)
        lua_pushstring(L, p->queue_policy_info().c_str());
    else
        lua_pushstring(L, "");
    return 1;
}

int LuaEngine::lua_api_in_capacity(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *c = self->api_->consumer();
    lua_pushinteger(L, c ? static_cast<lua_Integer>(c->queue_capacity()) : 0);
    return 1;
}

int LuaEngine::lua_api_in_policy(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *c = self->api_->consumer();
    if (c)
        lua_pushstring(L, c->queue_policy_info().c_str());
    else
        lua_pushstring(L, "");
    return 1;
}

int LuaEngine::lua_api_last_seq(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *c = self->api_->consumer();
    lua_pushinteger(L, c ? static_cast<lua_Integer>(c->last_seq()) : 0);
    return 1;
}

int LuaEngine::lua_api_ctrl_queue_dropped(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t total = 0;
    if (self->api_->producer()) total += self->api_->producer()->ctrl_queue_dropped();
    if (self->api_->consumer()) total += self->api_->consumer()->ctrl_queue_dropped();
    lua_pushinteger(L, static_cast<lua_Integer>(total));
    return 1;
}

// ============================================================================
// Group C: custom metrics (HEP-CORE-0019)
// ============================================================================

int LuaEngine::lua_api_report_metric(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *key = luaL_checkstring(L, 1);
    double value = luaL_checknumber(L, 2);
    self->api_->core()->report_metric(key, value);
    return 0;
}

int LuaEngine::lua_api_report_metrics(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnil(L); // first key
    while (lua_next(L, 1) != 0)
    {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER)
        {
            const char *key = lua_tostring(L, -2);
            double val = lua_tonumber(L, -1);
            self->api_->core()->report_metric(key, val);
        }
        lua_pop(L, 1); // pop value, keep key for next iteration
    }
    return 0;
}

int LuaEngine::lua_api_clear_custom_metrics(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    self->api_->core()->clear_custom_metrics();
    return 0;
}

// ============================================================================
// Group E: broker operations
// ============================================================================

int LuaEngine::lua_api_clear_inbox_cache(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    self->api_->core()->clear_inbox_cache();
    return 0;
}

// ── Channel pub/sub (HEP-CORE-0030) ──────────────────────────────────────────

int LuaEngine::lua_api_join_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *channel = luaL_checkstring(L, 1);
    auto result = self->api_->join_channel(channel);
    if (!result.has_value())
    {
        lua_pushnil(L);
        return 1;
    }
    json_to_lua(L, *result);
    return 1;
}

int LuaEngine::lua_api_leave_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *channel = luaL_checkstring(L, 1);
    bool ok = self->api_->leave_channel(channel);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int LuaEngine::lua_api_send_channel_msg(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *channel = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    nlohmann::json body = lua_to_json(L, 2);
    self->api_->send_channel_msg(channel, body);
    return 0;
}

int LuaEngine::lua_api_channel_members(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *channel = luaL_checkstring(L, 1);
    auto result = self->api_->channel_members(channel);
    if (!result.has_value())
    {
        lua_pushnil(L);
        return 1;
    }
    json_to_lua(L, *result);
    return 1;
}

// ============================================================================
// api.metrics() — hierarchical metrics table
// ============================================================================

int LuaEngine::lua_api_metrics(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto *core = self->api_->core();

    // Top-level metrics table.
    lua_newtable(L);

    // "queue" (or "in_queue"/"out_queue" for processor)
    auto *producer = self->api_->producer();
    auto *consumer = self->api_->consumer();

    if (self->api_->role_tag() == "proc")
    {
        if (consumer)
        {
            lua_newtable(L);
            queue_metrics_to_lua(L, consumer->queue_metrics());
            lua_setfield(L, -2, "in_queue");
        }
        if (producer)
        {
            lua_newtable(L);
            queue_metrics_to_lua(L, producer->queue_metrics());
            lua_setfield(L, -2, "out_queue");
        }
    }
    else if (self->api_->role_tag() == "prod" && producer)
    {
        lua_newtable(L);
        queue_metrics_to_lua(L, producer->queue_metrics());
        lua_setfield(L, -2, "queue");
    }
    else if (self->api_->role_tag() == "cons" && consumer)
    {
        lua_newtable(L);
        queue_metrics_to_lua(L, consumer->queue_metrics());
        lua_setfield(L, -2, "queue");
    }

    // "loop"
    {
        lua_newtable(L);
        loop_metrics_to_lua(L, core->loop_metrics());
        lua_setfield(L, -2, "loop");
    }

    // "role" (role-specific, built inline)
    {
        lua_newtable(L);
        lua_pushinteger(L, static_cast<lua_Integer>(core->out_slots_written()));
        lua_setfield(L, -2, "out_slots_written");
        lua_pushinteger(L, static_cast<lua_Integer>(core->in_slots_received()));
        lua_setfield(L, -2, "in_slots_received");
        lua_pushinteger(L, static_cast<lua_Integer>(core->out_drop_count()));
        lua_setfield(L, -2, "out_drop_count");
        lua_pushinteger(L, static_cast<lua_Integer>(core->script_error_count()));
        lua_setfield(L, -2, "script_error_count");

        // ctrl_queue_dropped — processor gets {input, output} sub-table
        if (self->api_->role_tag() == "proc")
        {
            lua_newtable(L);
            lua_pushinteger(L, static_cast<lua_Integer>(
                producer ? producer->ctrl_queue_dropped() : 0));
            lua_setfield(L, -2, "output");
            lua_pushinteger(L, static_cast<lua_Integer>(
                consumer ? consumer->ctrl_queue_dropped() : 0));
            lua_setfield(L, -2, "input");
            lua_setfield(L, -2, "ctrl_queue_dropped");
        }
        else if (producer)
        {
            lua_pushinteger(L, static_cast<lua_Integer>(producer->ctrl_queue_dropped()));
            lua_setfield(L, -2, "ctrl_queue_dropped");
        }
        else if (consumer)
        {
            lua_pushinteger(L, static_cast<lua_Integer>(consumer->ctrl_queue_dropped()));
            lua_setfield(L, -2, "ctrl_queue_dropped");
        }

        lua_setfield(L, -2, "role");
    }

    // "inbox" (optional — only if inbox configured)
    if (self->api_->inbox_queue())
    {
        lua_newtable(L);
        inbox_metrics_to_lua(L, self->api_->inbox_queue()->inbox_metrics());
        lua_setfield(L, -2, "inbox");
    }

    // "custom" (optional — only if custom metrics reported)
    {
        auto cm = core->custom_metrics_snapshot();
        if (!cm.empty())
        {
            lua_newtable(L);
            for (auto &[k, v] : cm)
            {
                lua_pushnumber(L, v);
                lua_setfield(L, -2, k.c_str());
            }
            lua_setfield(L, -2, "custom");
        }
    }

    return 1; // one table on stack
}

// ============================================================================
// Group F: Spinlocks (SHM-only)
// ============================================================================

// ============================================================================
// ChannelSide helpers + schema size + spinlocks
// ============================================================================

static const char *kSpinlockMT = "pylabhub.SharedSpinLock";

// Parse optional ChannelSide from Lua argument (integer or nil).
static std::optional<ChannelSide> lua_opt_channel_side(lua_State *L, int arg)
{
    if (lua_isnoneornil(L, arg))
        return std::nullopt;
    int v = static_cast<int>(luaL_checkinteger(L, arg));
    if (v != 0 && v != 1)
        luaL_error(L, "side must be api.Tx (0) or api.Rx (1), got %d", v);
    return static_cast<ChannelSide>(v);
}

int LuaEngine::lua_api_slot_logical_size(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto side = lua_opt_channel_side(L, 1);
    try
    {
        lua_pushinteger(L, static_cast<lua_Integer>(self->api_->slot_logical_size(side)));
    }
    catch (const std::exception &e)
    {
        return luaL_error(L, "%s", e.what());
    }
    return 1;
}

int LuaEngine::lua_api_flexzone_logical_size(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto side = lua_opt_channel_side(L, 1);
    try
    {
        lua_pushinteger(L, static_cast<lua_Integer>(self->api_->flexzone_logical_size(side)));
    }
    catch (const std::exception &e)
    {
        return luaL_error(L, "%s", e.what());
    }
    return 1;
}

int LuaEngine::lua_api_spinlock_count(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    auto side = lua_opt_channel_side(L, 1);
    try
    {
        lua_pushinteger(L, static_cast<lua_Integer>(self->api_->spinlock_count(side)));
    }
    catch (const std::exception &e)
    {
        return luaL_error(L, "%s", e.what());
    }
    return 1;
}

int LuaEngine::lua_api_spinlock(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    int idx = static_cast<int>(luaL_checkinteger(L, 1));
    auto side = lua_opt_channel_side(L, 2);

    try
    {
        void *ud = lua_newuserdata(L, sizeof(hub::SharedSpinLock));
        new (ud) hub::SharedSpinLock(
            self->api_->get_spinlock(static_cast<size_t>(idx), side));
        luaL_setmetatable(L, kSpinlockMT);
    }
    catch (const std::exception &e)
    {
        return luaL_error(L, "%s", e.what());
    }
    return 1;
}

int LuaEngine::lua_spinlock_lock(lua_State *L)
{
    auto *lock = static_cast<hub::SharedSpinLock *>(
        luaL_checkudata(L, 1, kSpinlockMT));
    lock->lock();
    return 0;
}

int LuaEngine::lua_spinlock_unlock(lua_State *L)
{
    auto *lock = static_cast<hub::SharedSpinLock *>(
        luaL_checkudata(L, 1, kSpinlockMT));
    lock->unlock();
    return 0;
}

int LuaEngine::lua_spinlock_try_lock_for(lua_State *L)
{
    auto *lock = static_cast<hub::SharedSpinLock *>(
        luaL_checkudata(L, 1, kSpinlockMT));
    int timeout_ms = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, lock->try_lock_for(timeout_ms) ? 1 : 0);
    return 1;
}

int LuaEngine::lua_spinlock_is_locked(lua_State *L)
{
    auto *lock = static_cast<hub::SharedSpinLock *>(
        luaL_checkudata(L, 1, kSpinlockMT));
    lua_pushboolean(L, lock->is_locked_by_current_process() ? 1 : 0);
    return 1;
}

int LuaEngine::lua_spinlock_gc(lua_State *L)
{
    auto *lock = static_cast<hub::SharedSpinLock *>(
        luaL_checkudata(L, 1, kSpinlockMT));
    // If still locked by us, unlock to prevent deadlock on GC.
    if (lock->is_locked_by_current_process())
        lock->unlock();
    lock->~SharedSpinLock();
    return 0;
}

void LuaEngine::register_spinlock_metatable_()
{
    lua_State *L = state_.raw();
    if (luaL_newmetatable(L, kSpinlockMT))
    {
        // __index points to itself for method dispatch.
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        auto reg = [&](const char *name, lua_CFunction fn) {
            lua_pushcfunction(L, fn);
            lua_setfield(L, -2, name);
        };

        reg("lock", lua_spinlock_lock);
        reg("unlock", lua_spinlock_unlock);
        reg("try_lock_for", lua_spinlock_try_lock_for);
        reg("is_locked", lua_spinlock_is_locked);

        lua_pushcfunction(L, lua_spinlock_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1); // pop metatable
}

// ============================================================================
// Group G: Flexzone getter
// ============================================================================

int LuaEngine::lua_api_flexzone(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));

    // Only producer/processor have writable flexzone.
    auto *producer = self->api_->producer();
    if (!producer)
    {
        lua_pushnil(L);
        return 1;
    }

    void *fz_ptr = producer->write_flexzone();
    size_t fz_sz = producer->flexzone_size();
    if (!fz_ptr || fz_sz == 0)
    {
        lua_pushnil(L);
        return 1;
    }

    // Use cached FlexFrame ctype ref for type-safe FFI cast.
    if (self->ref_out_fz_ != LUA_NOREF)
    {
        if (self->state_.push_slot_view_cached(fz_ptr, self->ref_out_fz_))
            return 1;
    }

    // Fallback: push as raw lightuserdata (no type safety).
    lua_pushlightuserdata(L, fz_ptr);
    return 1;
}

} // namespace pylabhub::scripting
