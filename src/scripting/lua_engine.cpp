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

#include "utils/format_tools.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/logger.hpp"
#include "script_host_helpers.hpp"
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

bool LuaEngine::init_engine_(const char *log_tag, RoleHostCore *core)
{
    log_tag_ = log_tag ? log_tag : "lua";
    // owner_thread_id_, accepting_, ctx_.core set by base class initialize().

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
                             const char *entry_point,
                             const char *required_callback)
{
    script_dir_str_ = script_dir.string();
    entry_point_ = entry_point ? entry_point : "init.lua";
    required_callback_ = required_callback ? required_callback : "";

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
    if (!required_callback_.empty() && !has_callback(required_callback_.c_str()))
    {
        LOGGER_ERROR("[{}] Script has no '{}' function", log_tag_, required_callback_);
        return false;
    }

    return true;
}

// ============================================================================
// build_api — save RoleContext, build Lua table with closures
// ============================================================================

void LuaEngine::build_api_(const RoleContext &ctx)
{
    // ctx_ and core preservation handled by base class build_api().
    stop_on_script_error_ = ctx.stop_on_script_error;

    lua_State *L = state_.raw();

    // Create the api table.
    lua_newtable(L);

    // Common closures.
    push_common_api_closures_(L);

    // Helper: push a C closure with `this` as upvalue(1).
    auto push_closure = [&](const char *name, lua_CFunction fn) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };

    // ── Role-specific closures based on non-null context pointers ─────────

    // ── Identity closures (always present) ──────────────────────────────
    push_closure("uid", lua_api_uid);
    push_closure("name", lua_api_name);
    push_closure("channel", lua_api_channel);

    // ── Producer-specific (ctx.producer != nullptr) ──────────────────────
    if (ctx_.producer != nullptr)
    {
        push_closure("broadcast", lua_api_broadcast);
        push_closure("send", lua_api_send);
        push_closure("consumers", lua_api_consumers);
    }

    // ── QueueWriter closures (producer / processor output) ───────────────
    if (ctx_.queue_writer != nullptr)
    {
        push_closure("update_flexzone_checksum", lua_api_update_flexzone_checksum);
    }

    // ── QueueReader closures (consumer / processor input) ────────────────
    if (ctx_.queue_reader != nullptr)
    {
        push_closure("set_verify_checksum", lua_api_set_verify_checksum);
    }

    // ── Processor-specific ───────────────────────────────────────────────
    if (!ctx_.out_channel.empty())
    {
        push_closure("in_channel", lua_api_in_channel);
        push_closure("out_channel", lua_api_out_channel);
    }

    // ── Metrics closures (pushed based on counter pointer availability) ──
    push_closure("out_written", lua_api_out_written);
    push_closure("in_received", lua_api_in_received);
    push_closure("drops", lua_api_drops);

    // Store api table in the registry AND as a global (for generic invoke).
    lua_pushvalue(L, -1);            // duplicate the api table
    lua_setglobal(L, "api");         // set as global for scripts calling api.* from free functions
    ref_api_ = luaL_ref(L, LUA_REGISTRYINDEX);
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
    if (!child->initialize(log_tag_.c_str(), ctx_.core))
    {
        LOGGER_ERROR("[{}] Failed to create thread-local LuaEngine", log_tag_);
        return nullptr;
    }
    if (!child->load_script(script_dir_str_, entry_point_.c_str(),
                             required_callback_.c_str()))
    {
        LOGGER_ERROR("[{}] Thread-local LuaEngine: load_script failed", log_tag_);
        child->finalize();
        return nullptr;
    }
    child->build_api(ctx_);

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

bool LuaEngine::invoke(const char *name)
{
    if (!name || !is_accepting())
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
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        executing_.store(false, std::memory_order_release);
        return false;
    }
    bool ok = (lua_pcall(L, 0, 0, 0) == 0);
    if (!ok)
        on_pcall_error_(name);
    executing_.store(false, std::memory_order_release);
    return ok;
}

bool LuaEngine::invoke(const char *name, const nlohmann::json &args)
{
    if (!name || !is_accepting())
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
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        executing_.store(false, std::memory_order_release);
        return false;
    }

    // Push args as a Lua table.
    lua_newtable(L);
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        lua_pushstring(L, it.key().c_str());
        if (it.value().is_number_integer())
            lua_pushinteger(L, it.value().get<lua_Integer>());
        else if (it.value().is_number_float())
            lua_pushnumber(L, it.value().get<double>());
        else if (it.value().is_boolean())
            lua_pushboolean(L, it.value().get<bool>() ? 1 : 0);
        else if (it.value().is_string())
            lua_pushstring(L, it.value().get<std::string>().c_str());
        else
            lua_pushnil(L); // unsupported type → nil
        lua_settable(L, -3);
    }

    bool ok = (lua_pcall(L, 1, 0, 0) == 0);
    if (!ok)
        on_pcall_error_(name);
    executing_.store(false, std::memory_order_release);
    return ok;
}

nlohmann::json LuaEngine::eval(const char *code)
{
    if (!code || !is_accepting())
        return {};

    if (std::this_thread::get_id() != owner_thread_id_)
    {
        auto *child = get_or_create_thread_state_();
        if (!child)
            return {};
        return child->eval(code);
    }

    executing_.store(true, std::memory_order_release);
    auto *L = state_.raw();
    if (luaL_dostring(L, code) != 0)
    {
        on_pcall_error_("eval");
        executing_.store(false, std::memory_order_release);
        return {};
    }

    // Capture top-of-stack result as JSON.
    nlohmann::json result;
    if (lua_gettop(L) > 0)
    {
        int top = lua_gettop(L);
        if (lua_isboolean(L, top))
            result = static_cast<bool>(lua_toboolean(L, top));
        else if (lua_isnumber(L, top))
            result = lua_tonumber(L, top);
        else if (lua_isstring(L, top))
            result = lua_tostring(L, top);
        lua_settop(L, 0);
    }
    executing_.store(false, std::memory_order_release);
    return result;
}

// ============================================================================
// has_callback
// ============================================================================

bool LuaEngine::has_callback(const char *name) const
{
    if (!name)
        return false;

    if (std::strcmp(name, "on_init") == 0)
        return state_.is_ref_callable(ref_on_init_);
    if (std::strcmp(name, "on_stop") == 0)
        return state_.is_ref_callable(ref_on_stop_);
    if (std::strcmp(name, "on_produce") == 0)
        return state_.is_ref_callable(ref_on_produce_);
    if (std::strcmp(name, "on_consume") == 0)
        return state_.is_ref_callable(ref_on_consume_);
    if (std::strcmp(name, "on_process") == 0)
        return state_.is_ref_callable(ref_on_process_);
    if (std::strcmp(name, "on_inbox") == 0)
        return state_.is_ref_callable(ref_on_inbox_);

    // Unknown callback name — check as a global function.
    lua_State *L = state_.raw();
    lua_getglobal(L, name);
    bool is_fn = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return is_fn;
}

// ============================================================================
// register_slot_type / type_sizeof
// ============================================================================

bool LuaEngine::register_slot_type(const SchemaSpec &spec,
                                    const char *type_name,
                                    const std::string &packing)
{
    // Build FFI cdef string from SchemaSpec.
    // Reuse the same logic as LuaRoleHostBase::build_ffi_cdef_.
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
            return false;
        }

        ss << "    " << c_type << " " << f.name;
        if (f.type_str == "string" || f.type_str == "bytes")
            ss << "[" << f.length << "]";
        else if (f.count > 1)
            ss << "[" << f.count << "]";
        ss << ";\n";
    }

    ss << "} " << type_name << ";\n";
    std::string cdef = ss.str();

    if (cdef.empty())
        return false;

    if (!state_.register_ffi_type(cdef, log_tag_.c_str()))
        return false;

    // Cache ffi.typeof() for hot-path slot views — no string ops per cycle.
    // Convention:
    //   "SlotFrame"    → producer writable / consumer readonly
    //   "InSlotFrame"  → processor input readonly
    //   "OutSlotFrame" → processor output writable
    //   "FlexFrame"    → flexzone (cached separately via fz_type param)
    //   "InboxFrame"   → inbox (not cached — low frequency)
    // Helper: unref old ref before overwriting to prevent registry leak.
    auto safe_cache = [&](int &ref, const char *name, bool readonly) {
        if (ref != LUA_NOREF)
            state_.unref(ref);
        ref = state_.cache_ffi_typeof(name, readonly);
    };

    const std::string tn{type_name};
    if (tn == "InSlotFrame")
    {
        safe_cache(ref_in_slot_readonly_, type_name, /*readonly=*/true);
    }
    else if (tn == "OutSlotFrame")
    {
        safe_cache(ref_out_slot_writable_, type_name, /*readonly=*/false);
    }
    else if (tn == "SlotFrame")
    {
        safe_cache(ref_slot_writable_, type_name, /*readonly=*/false);
        safe_cache(ref_slot_readonly_, type_name, /*readonly=*/true);
        // Also set in/out refs for unified context (if not already set by
        // an explicit InSlotFrame/OutSlotFrame registration).
        if (ref_in_slot_readonly_ == LUA_NOREF)
            ref_in_slot_readonly_ = state_.cache_ffi_typeof(type_name, /*readonly=*/true);
        if (ref_out_slot_writable_ == LUA_NOREF)
            ref_out_slot_writable_ = state_.cache_ffi_typeof(type_name, /*readonly=*/false);
    }
    else if (tn == "FlexFrame")
    {
        safe_cache(ref_fz_writable_, type_name, /*readonly=*/false);
        safe_cache(ref_fz_readonly_, type_name, /*readonly=*/true);
    }

    return true;
}

size_t LuaEngine::type_sizeof(const char *type_name) const
{
    return state_.ffi_sizeof(type_name);
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
        ctx_.core->inc_script_errors();
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
        ctx_.core->inc_script_errors();
    }
}

// ============================================================================
// invoke_produce — on_produce(out_slot, flexzone, messages, api) -> result
// ============================================================================

InvokeResult LuaEngine::invoke_produce(
    void *out_slot, size_t out_sz,
    void *flexzone, size_t fz_sz, const char *fz_type,
    std::vector<IncomingMessage> &msgs)
{
    if (!state_.is_ref_callable(ref_on_produce_))
        return InvokeResult::Error;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_produce_);

    // Arg 1: out_slot (writable cdata or nil).
    if (out_slot != nullptr)
    {
        // Hot path: use cached ffi.typeof ref (no string ops).
        if (!state_.push_slot_view_cached(out_slot, ref_slot_writable_))
        {
            lua_pop(L, 1); // pop function
            return InvokeResult::Error;
        }
    }
    else
    {
        lua_pushnil(L);
    }

    // Arg 2: flexzone (writable cdata or nil). Uses cached ctype if available.
    if (flexzone != nullptr && fz_sz > 0)
    {
        if (ref_fz_writable_ != LUA_NOREF)
        {
            if (!state_.push_slot_view_cached(flexzone, ref_fz_writable_))
                lua_pushnil(L);
        }
        else if (fz_type != nullptr)
        {
            if (!state_.push_slot_view(flexzone, fz_sz, fz_type))
                lua_pushnil(L);
        }
        else
        {
            lua_pushnil(L);
        }
    }
    else
    {
        lua_pushnil(L);
    }

    // Arg 3: messages table.
    push_messages_table_(msgs);

    // Arg 4: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(4 args, 1 result).
    if (!state_.pcall(4, 1, "on_produce"))
        return on_pcall_error_("on_produce");

    // Strict return value contract:
    //   true  → Commit (publish the slot)
    //   false → Discard (script chose not to publish)
    //   nil   → Error (script omitted return — must be explicit)
    //   other → Error (wrong type — log, count, treat as Discard)
    InvokeResult result;
    if (lua_isboolean(L, -1))
    {
        result = lua_toboolean(L, -1) ? InvokeResult::Commit : InvokeResult::Discard;
    }
    else if (lua_isnil(L, -1))
    {
        LOGGER_WARN("[{}] on_produce returned nil — explicit 'return true' or "
                    "'return false' is required. Treating as discard.",
                    log_tag_);
        result = on_pcall_error_("on_produce [missing return value]");
    }
    else
    {
        LOGGER_ERROR("[{}] on_produce returned non-boolean type '{}' (value: {}) — "
                     "expected 'return true' or 'return false'. Treating as discard.",
                     log_tag_,
                     lua_typename(L, lua_type(L, -1)),
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        result = on_pcall_error_("on_produce [wrong return type]");
    }
    lua_pop(L, 1);
    return result;
}

// ============================================================================
// invoke_consume — on_consume(in_slot, flexzone, messages, api)
// ============================================================================

void LuaEngine::invoke_consume(
    const void *in_slot, size_t in_sz,
    const void *flexzone, size_t fz_sz, const char *fz_type,
    std::vector<IncomingMessage> &msgs)
{
    if (!state_.is_ref_callable(ref_on_consume_))
        return;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_consume_);

    // Arg 1: in_slot (read-only cdata or nil).
    if (in_slot != nullptr)
    {
        // Hot path: use cached ffi.typeof ref (no string ops).
        // const_cast is safe: push_slot_view_cached uses the readonly ctype ref.
        if (!state_.push_slot_view_cached(const_cast<void *>(in_slot), ref_slot_readonly_))
        {
            lua_pop(L, 1); // pop function
            return;
        }
    }
    else
    {
        lua_pushnil(L);
    }

    // Arg 2: flexzone (read-only cdata or nil). Uses cached ctype if available.
    if (flexzone != nullptr && fz_sz > 0)
    {
        if (ref_fz_readonly_ != LUA_NOREF)
        {
            if (!state_.push_slot_view_cached(
                    const_cast<void *>(flexzone), ref_fz_readonly_))
                lua_pushnil(L);
        }
        else if (fz_type != nullptr)
        {
            if (!state_.push_slot_view_readonly(flexzone, fz_sz, fz_type))
                lua_pushnil(L);
        }
        else
        {
            lua_pushnil(L);
        }
    }
    else
    {
        lua_pushnil(L);
    }

    // Arg 3: messages table (bare variant for consumer).
    push_messages_table_bare_(msgs);

    // Arg 4: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(4 args, 0 results).
    if (!state_.pcall(4, 0, "on_consume"))
    {
        on_pcall_error_("on_consume");
    }
}

// ============================================================================
// invoke_process — on_process(in_slot, out_slot, flexzone, messages, api)
// ============================================================================

InvokeResult LuaEngine::invoke_process(
    const void *in_slot, size_t in_sz,
    void *out_slot, size_t out_sz,
    void *flexzone, size_t fz_sz, const char *fz_type,
    std::vector<IncomingMessage> &msgs)
{
    if (!state_.is_ref_callable(ref_on_process_))
        return InvokeResult::Error;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_process_);

    // Arg 1: in_slot (read-only cdata or nil).
    if (in_slot != nullptr)
    {
        // Hot path: use cached ffi.typeof ref (no string ops).
        if (!state_.push_slot_view_cached(const_cast<void *>(in_slot), ref_in_slot_readonly_))
        {
            lua_pop(L, 1); // pop function
            return InvokeResult::Error;
        }
    }
    else
    {
        lua_pushnil(L);
    }

    // Arg 2: out_slot (writable cdata or nil).
    if (out_slot != nullptr)
    {
        if (!state_.push_slot_view_cached(out_slot, ref_out_slot_writable_))
        {
            lua_pop(L, 2); // pop function + in_slot
            return InvokeResult::Error;
        }
    }
    else
    {
        lua_pushnil(L);
    }

    // Arg 3: flexzone (writable cdata or nil). Uses cached ctype if available.
    if (flexzone != nullptr && fz_sz > 0)
    {
        if (ref_fz_writable_ != LUA_NOREF)
        {
            if (!state_.push_slot_view_cached(flexzone, ref_fz_writable_))
                lua_pushnil(L);
        }
        else if (fz_type != nullptr)
        {
            if (!state_.push_slot_view(flexzone, fz_sz, fz_type))
                lua_pushnil(L);
        }
        else
        {
            lua_pushnil(L);
        }
    }
    else
    {
        lua_pushnil(L);
    }

    // Arg 4: messages table.
    push_messages_table_(msgs);

    // Arg 5: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(5 args, 1 result).
    if (!state_.pcall(5, 1, "on_process"))
        return on_pcall_error_("on_process");

    // Strict return value contract (same as on_produce).
    InvokeResult result;
    if (lua_isboolean(L, -1))
    {
        result = lua_toboolean(L, -1) ? InvokeResult::Commit : InvokeResult::Discard;
    }
    else if (lua_isnil(L, -1))
    {
        LOGGER_WARN("[{}] on_process returned nil — explicit 'return true' or "
                    "'return false' is required. Treating as discard.",
                    log_tag_);
        result = on_pcall_error_("on_process [missing return value]");
    }
    else
    {
        LOGGER_ERROR("[{}] on_process returned non-boolean type '{}' (value: {}) — "
                     "expected 'return true' or 'return false'. Treating as discard.",
                     log_tag_,
                     lua_typename(L, lua_type(L, -1)),
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        result = on_pcall_error_("on_process [wrong return type]");
    }
    lua_pop(L, 1);
    return result;
}

// ============================================================================
// invoke_on_inbox — on_inbox(slot_data_or_bytes, sender, api)
// ============================================================================

void LuaEngine::invoke_on_inbox(
    const void *data, size_t sz,
    const char *type_name,
    const char *sender)
{
    if (!state_.is_ref_callable(ref_on_inbox_))
        return;

    lua_State *L = state_.raw();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_on_inbox_);

    // Arg 1: inbox slot view or raw bytes.
    if (type_name != nullptr && type_name[0] != '\0' && data != nullptr && sz > 0)
    {
        if (!state_.push_slot_view_readonly(data, sz, type_name))
        {
            // Fallback to raw bytes on ffi.cast failure.
            lua_pushlstring(L, reinterpret_cast<const char *>(data), sz);
        }
    }
    else
    {
        // No type name or no data — push raw bytes.
        if (data != nullptr && sz > 0)
            lua_pushlstring(L, reinterpret_cast<const char *>(data), sz);
        else
            lua_pushnil(L);
    }

    // Arg 2: sender UID string.
    lua_pushstring(L, sender ? sender : "");

    // Arg 3: api ref.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_api_);

    // pcall(3 args, 0 results).
    if (!state_.pcall(3, 0, "on_inbox"))
    {
        on_pcall_error_("on_inbox");
    }
}

// ============================================================================
// create_thread_state — create an independent LuaEngine for another thread
// ============================================================================

std::unique_ptr<ScriptEngine> LuaEngine::create_thread_state()
{
    auto engine = std::make_unique<LuaEngine>();
    if (!engine->initialize(log_tag_.c_str(), ctx_.core))
        return nullptr;

    if (script_dir_str_.empty())
        return nullptr;

    fs::path script_dir(script_dir_str_);
    if (!engine->load_script(script_dir, entry_point_.c_str(), required_callback_.c_str()))
        return nullptr;

    // build_api must be called separately by the caller with the appropriate context.
    return engine;
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

InvokeResult LuaEngine::on_pcall_error_(const char *callback_name)
{
    ctx_.core->inc_script_errors();
    // Error message is already logged by state_.pcall().
    if (stop_on_script_error_)
    {
        LOGGER_ERROR("[{}] stop_on_script_error: requesting shutdown after {} error",
                     log_tag_, callback_name);
        ctx_.core->request_stop();
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
    state_.unref(ref_slot_writable_);
    ref_slot_writable_ = LUA_NOREF;
    state_.unref(ref_slot_readonly_);
    ref_slot_readonly_ = LUA_NOREF;
    state_.unref(ref_in_slot_readonly_);
    ref_in_slot_readonly_ = LUA_NOREF;
    state_.unref(ref_out_slot_writable_);
    ref_out_slot_writable_ = LUA_NOREF;
    state_.unref(ref_fz_writable_);
    ref_fz_writable_ = LUA_NOREF;
    state_.unref(ref_fz_readonly_);
    ref_fz_readonly_ = LUA_NOREF;
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
                if (val.is_string())
                    lua_pushstring(L, val.get<std::string>().c_str());
                else if (val.is_boolean())
                    lua_pushboolean(L, val.get<bool>() ? 1 : 0);
                else if (val.is_number_integer())
                    lua_pushinteger(L, val.get<lua_Integer>());
                else if (val.is_number_float())
                    lua_pushnumber(L, val.get<double>());
                else
                    lua_pushstring(L, val.dump().c_str());
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
                if (val.is_string())
                    lua_pushstring(L, val.get<std::string>().c_str());
                else if (val.is_boolean())
                    lua_pushboolean(L, val.get<bool>() ? 1 : 0);
                else if (val.is_number_integer())
                    lua_pushinteger(L, val.get<lua_Integer>());
                else if (val.is_number_float())
                    lua_pushnumber(L, val.get<double>());
                else
                    lua_pushstring(L, val.dump().c_str());
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
    push_closure("stop_reason", lua_api_stop_reason);
    push_closure("script_errors", lua_api_script_errors);
    push_closure("version_info", lua_api_version_info);
    push_closure("wait_for_role", lua_api_wait_for_role);
    push_closure("open_inbox", lua_api_open_inbox);
    push_closure("get_shared_data", lua_api_get_shared_data);
    push_closure("set_shared_data", lua_api_set_shared_data);

    // String fields as direct table entries.
    lua_pushstring(L, ctx_.log_level.c_str());
    lua_setfield(L, -2, "log_level");

    lua_pushstring(L, ctx_.script_dir.c_str());
    lua_setfield(L, -2, "script_dir");

    lua_pushstring(L, ctx_.role_dir.c_str());
    lua_setfield(L, -2, "role_dir");
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
            auto entry = ud->engine->ctx_.core->get_inbox_entry(ud->target_uid);
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
            auto entry = ud->engine->ctx_.core->get_inbox_entry(ud->target_uid);
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
            auto entry = ud->engine->ctx_.core->get_inbox_entry(ud->target_uid);
            if (entry && entry->client)
                entry->client->abort();
            return 0;
        });

        set_method("is_ready", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            auto entry = ud->engine->ctx_.core->get_inbox_entry(ud->target_uid);
            lua_pushboolean(Ls,
                            (entry && entry->client && entry->client->is_running()) ? 1 : 0);
            return 1;
        });

        set_method("close", [](lua_State *Ls) -> int {
            auto *ud = static_cast<LuaInboxUD *>(luaL_checkudata(Ls, 1, kInboxHandleMT));
            auto entry = ud->engine->ctx_.core->get_inbox_entry(ud->target_uid);
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
    self->ctx_.core->request_stop();
    return 0;
}

int LuaEngine::lua_api_set_critical_error(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *msg = lua_tostring(L, 1);
    LOGGER_ERROR("[{}-lua] CRITICAL: {}", self->log_tag_, msg ? msg : "(no message)");
    self->ctx_.core->set_critical_error();
    return 0;
}

int LuaEngine::lua_api_stop_reason(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->ctx_.core->stop_reason_string().c_str());
    return 1;
}

int LuaEngine::lua_api_version_info(lua_State *L)
{
    lua_pushstring(L, pylabhub::version::version_info_json().c_str());
    return 1;
}

int LuaEngine::lua_api_script_errors(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->ctx_.core->script_errors();
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
    auto val = self->ctx_.core->get_shared_data(key);
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
        self->ctx_.core->set_shared_data(key, static_cast<bool>(lua_toboolean(L, 2)));
    else if (lua_isnumber(L, 2))
    {
        // LuaJIT: all numbers are doubles. Store as int64 if whole number.
        double num = lua_tonumber(L, 2);
        if (num == static_cast<double>(static_cast<int64_t>(num)))
            self->ctx_.core->set_shared_data(key, static_cast<int64_t>(num));
        else
            self->ctx_.core->set_shared_data(key, num);
    }
    else if (lua_isstring(L, 2))
        self->ctx_.core->set_shared_data(key, std::string(lua_tostring(L, 2)));
    else
        self->ctx_.core->remove_shared_data(key); // nil → remove key
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

    auto *messenger = self->ctx_.messenger;
    if (!messenger)
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    static constexpr int kPollMs = 200;

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            break;
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
// lua_api_open_inbox — discover target inbox and return InboxHandle userdata
// ============================================================================

int LuaEngine::lua_api_open_inbox(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *target_uid = luaL_checkstring(L, 1);

    // Delegate to ScriptEngine base — handles broker query, client
    // creation, and core_ cache in one place.
    auto result = self->open_inbox_client(target_uid);
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

        std::ostringstream ss;
        const bool is_packed = (result->packing == "packed");
        ss << "typedef struct ";
        if (is_packed) ss << "__attribute__((packed)) ";
        ss << "{\n";

        bool cdef_ok = true;
        for (const auto &f : result->spec.fields)
        {
            std::string c_type;
            if      (f.type_str == "bool")    c_type = "bool";
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
            else { cdef_ok = false; break; }

            ss << "    " << c_type << " " << f.name;
            if (f.type_str == "string" || f.type_str == "bytes")
                ss << "[" << f.length << "]";
            else if (f.count > 1)
                ss << "[" << f.count << "]";
            ss << ";\n";
        }
        ss << "} " << ffi_type << ";\n";

        if (!cdef_ok || !self->state_.register_ffi_type(ss.str(), self->log_tag_.c_str()))
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
    lua_pushstring(L, self->ctx_.uid.c_str());
    return 1;
}

int LuaEngine::lua_api_name(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->ctx_.name.c_str());
    return 1;
}

int LuaEngine::lua_api_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->ctx_.channel.c_str());
    return 1;
}

int LuaEngine::lua_api_in_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->ctx_.channel.c_str());
    return 1;
}

int LuaEngine::lua_api_out_channel(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushstring(L, self->ctx_.out_channel.c_str());
    return 1;
}

int LuaEngine::lua_api_broadcast(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);

    auto *producer = static_cast<hub::Producer *>(self->ctx_.producer);
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

    auto *producer = static_cast<hub::Producer *>(self->ctx_.producer);
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
    auto *producer = static_cast<hub::Producer *>(self->ctx_.producer);
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
    auto *qw = self->ctx_.queue_writer;
    if (qw)
        qw->sync_flexzone_checksum();
    lua_pushboolean(L, qw ? 1 : 0);
    return 1;
}

int LuaEngine::lua_api_set_verify_checksum(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    bool enable = lua_toboolean(L, 1) != 0;
    auto *qr = self->ctx_.queue_reader;
    if (qr)
        qr->set_verify_checksum(enable, false);
    return 0;
}

int LuaEngine::lua_api_out_written(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->ctx_.core->out_written();
    lua_pushinteger(L, static_cast<lua_Integer>(val));
    return 1;
}

int LuaEngine::lua_api_in_received(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->ctx_.core->in_received();
    lua_pushinteger(L, static_cast<lua_Integer>(val));
    return 1;
}

int LuaEngine::lua_api_drops(lua_State *L)
{
    auto *self = static_cast<LuaEngine *>(lua_touserdata(L, lua_upvalueindex(1)));
    uint64_t val = self->ctx_.core->drops();
    lua_pushinteger(L, static_cast<lua_Integer>(val));
    return 1;
}

} // namespace pylabhub::scripting
