/**
 * @file lua_script_host.cpp
 * @brief LuaScriptHost — LuaJIT concrete ScriptHost implementation.
 */
#include "lua_script_host.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace pylabhub::utils
{

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

LuaScriptHost::~LuaScriptHost()
{
    if (L_)
    {
        // Safety net: if the caller forgot base_shutdown_(), close the state.
        // do_finalize() already calls lua_close; this guard handles the abnormal path.
        lua_close(L_);
        L_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// do_initialize — called on the owning thread by base_startup_()
// ---------------------------------------------------------------------------

bool LuaScriptHost::do_initialize(const fs::path& script_path)
{
    init_thread_id_ = std::this_thread::get_id();

    const fs::path lua_script = script_path / "lua" / "main.lua";
    if (!fs::exists(lua_script))
    {
        LOGGER_ERROR("LuaScriptHost: script not found: {}", lua_script.string());
        return false;
    }

    L_ = luaL_newstate();
    if (!L_)
    {
        LOGGER_ERROR("LuaScriptHost: luaL_newstate() failed (out of memory)");
        return false;
    }

    luaL_openlibs(L_);
    apply_sandbox_();
    setup_package_path_();

    // Load and execute the script (defines global functions; does not call them).
    if (luaL_loadfile(L_, lua_script.string().c_str()) != LUA_OK)
    {
        const char* err = lua_tostring(L_, -1);
        LOGGER_ERROR("LuaScriptHost: failed to load '{}': {}", lua_script.string(),
                     err ? err : "(unknown error)");
        lua_close(L_);
        L_ = nullptr;
        return false;
    }

    if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
    {
        const char* err = lua_tostring(L_, -1);
        LOGGER_ERROR("LuaScriptHost: error executing '{}': {}", lua_script.string(),
                     err ? err : "(unknown error)");
        lua_close(L_);
        L_ = nullptr;
        return false;
    }

    LOGGER_INFO("LuaScriptHost: loaded '{}'", lua_script.string());

    // Call on_start(api) — optional.
    build_api_table_(L_);         // pushes 1 arg: api table
    call_lua_fn_("on_start", 1);  // pops the arg

    // Note: for direct mode (Lua), base_startup_() calls signal_ready_() after we return.
    // We do NOT call signal_ready_() here — the base handles it.
    return true;
}

// ---------------------------------------------------------------------------
// do_finalize — called on the owning thread by base_shutdown_()
// ---------------------------------------------------------------------------

void LuaScriptHost::do_finalize() noexcept
{
    if (!L_)
        return;

    // Call on_stop(api) — optional.
    try
    {
        build_api_table_(L_);
        call_lua_fn_("on_stop", 1);
    }
    catch (const std::exception& e)
    {
        LOGGER_WARN("LuaScriptHost: exception in on_stop: {}", e.what());
    }
    catch (...)
    {
        LOGGER_WARN("LuaScriptHost: unknown exception in on_stop");
    }

    lua_close(L_);
    L_ = nullptr;

    // Clear thread-local state (we own this thread in direct mode).
    g_script_thread_state() = {};

    LOGGER_INFO("LuaScriptHost: finalized");
}

// ---------------------------------------------------------------------------
// tick — called periodically by the owner (hub or role host)
// ---------------------------------------------------------------------------

bool LuaScriptHost::tick(const LuaTickInfo& tick_info)
{
    if (!is_ready() || !L_)
        return false;

    // Thread safety check.
    if (std::this_thread::get_id() != init_thread_id_)
    {
        LOGGER_ERROR("LuaScriptHost::tick() called from wrong thread — "
                     "must be called from the thread that called base_startup_()");
        return false;
    }

    // Check if on_tick exists before pushing args.
    lua_getglobal(L_, "on_tick");
    if (!lua_isfunction(L_, -1))
    {
        lua_pop(L_, 1);
        return true; // on_tick is optional
    }
    lua_pop(L_, 1); // pop the function check; call_lua_fn_ will re-get it

    // Push args: api, tick
    build_api_table_(L_);     // arg 1: api
    push_tick_table_(tick_info); // arg 2: tick

    return call_lua_fn_("on_tick", 2);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void LuaScriptHost::apply_sandbox_()
{
    // Disable dangerous globals.
    static const char* const disabled[] = {
        "dofile", "loadfile", "io", nullptr
    };

    for (int i = 0; disabled[i]; ++i)
    {
        lua_pushnil(L_);
        lua_setglobal(L_, disabled[i]);
    }

    // Disable os.execute and os.exit (keep the rest of os).
    lua_getglobal(L_, "os");
    if (lua_istable(L_, -1))
    {
        lua_pushnil(L_); lua_setfield(L_, -2, "execute");
        lua_pushnil(L_); lua_setfield(L_, -2, "exit");
    }
    lua_pop(L_, 1);

    // Disable package.loadlib.
    lua_getglobal(L_, "package");
    if (lua_istable(L_, -1))
    {
        lua_pushnil(L_); lua_setfield(L_, -2, "loadlib");
    }
    lua_pop(L_, 1);
}

void LuaScriptHost::setup_package_path_()
{
    // Derive <exe_dir>/../opt/luajit — symmetric with Python's PYTHONHOME.
    const fs::path luajit_home = [&]() -> fs::path {
        try
        {
            const fs::path exe   = platform::get_executable_name(true);
            return fs::weakly_canonical(exe.parent_path() / ".." / "opt" / "luajit");
        }
        catch (const std::exception& e)
        {
            LOGGER_WARN("LuaScriptHost: could not derive luajit home: {}", e.what());
            return {};
        }
    }();

    if (luajit_home.empty() || !fs::exists(luajit_home))
    {
        LOGGER_WARN("LuaScriptHost: opt/luajit not found; LuaJIT support scripts unavailable");
        return;
    }

    // Append jit/ directory to package.path so `require "jit.dump"` etc. work.
    const std::string jit_path = (luajit_home / "jit" / "?.lua").string();

    lua_getglobal(L_, "package");
    lua_getfield(L_, -1, "path");
    const char* current = lua_tostring(L_, -1);
    std::string new_path = (current ? std::string(current) + ";" : std::string{}) + jit_path;
    lua_pop(L_, 1); // pop current path
    lua_pushstring(L_, new_path.c_str());
    lua_setfield(L_, -2, "path");
    lua_pop(L_, 1); // pop package

    LOGGER_DEBUG("LuaScriptHost: package.path includes '{}'", jit_path);
}

void LuaScriptHost::push_tick_table_(const LuaTickInfo& t)
{
    lua_newtable(L_);
    lua_pushinteger(L_, static_cast<lua_Integer>(t.tick_count));
    lua_setfield(L_, -2, "tick_count");
    lua_pushinteger(L_, static_cast<lua_Integer>(t.elapsed_ms));
    lua_setfield(L_, -2, "elapsed_ms");
    lua_pushinteger(L_, static_cast<lua_Integer>(t.uptime_ms));
    lua_setfield(L_, -2, "uptime_ms");
    lua_pushinteger(L_, t.channels_ready);
    lua_setfield(L_, -2, "channels_ready");
    lua_pushinteger(L_, t.channels_pending);
    lua_setfield(L_, -2, "channels_pending");
    lua_pushinteger(L_, t.channels_closing);
    lua_setfield(L_, -2, "channels_closing");
}

bool LuaScriptHost::call_lua_fn_(const char* name, int nargs)
{
    // Get the function; if absent, discard the already-pushed args and return.
    lua_getglobal(L_, name);
    if (!lua_isfunction(L_, -1))
    {
        lua_pop(L_, 1 + nargs); // pop nil + all pushed args
        return true;            // absent callback is not an error
    }

    // Move function below its args: stack is [..., arg1, ..., argN, fn]
    // We need: [..., fn, arg1, ..., argN]
    lua_insert(L_, -(nargs + 1));

    if (lua_pcall(L_, nargs, 0, 0) != LUA_OK)
    {
        const char* err = lua_tostring(L_, -1);
        LOGGER_WARN("LuaScriptHost: error in '{}': {}", name, err ? err : "(unknown)");
        lua_pop(L_, 1); // pop error string
        return false;
    }
    return true;
}

} // namespace pylabhub::utils
