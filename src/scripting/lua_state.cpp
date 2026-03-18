/**
 * @file lua_state.cpp
 * @brief LuaState — RAII wrapper implementation.
 */

#include "lua_state.hpp"

#include "utils/logger.hpp"

#include <cstring>
#include <string>

namespace pylabhub::scripting
{

// ============================================================================
// Construction / destruction
// ============================================================================

LuaState LuaState::create()
{
    LuaState s{luaL_newstate()};
    if (s.L_)
        luaL_openlibs(s.L_);
    return s;
}

LuaState::LuaState(lua_State *L) noexcept
    : L_(L)
    , owner_(std::this_thread::get_id())
{
}

LuaState::~LuaState()
{
    if (L_)
    {
        if (ref_ffi_cast_ != LUA_NOREF)
            luaL_unref(L_, LUA_REGISTRYINDEX, ref_ffi_cast_);
        lua_close(L_);
    }
}

LuaState::LuaState(LuaState &&other) noexcept
    : L_(other.L_)
    , ref_ffi_cast_(other.ref_ffi_cast_)
    , owner_(other.owner_)
{
    other.L_ = nullptr;
    other.ref_ffi_cast_ = LUA_NOREF;
}

LuaState &LuaState::operator=(LuaState &&other) noexcept
{
    if (this != &other)
    {
        if (L_)
        {
            if (ref_ffi_cast_ != LUA_NOREF)
                luaL_unref(L_, LUA_REGISTRYINDEX, ref_ffi_cast_);
            lua_close(L_);
        }
        L_ = other.L_;
        ref_ffi_cast_ = other.ref_ffi_cast_;
        owner_ = other.owner_;
        other.L_ = nullptr;
        other.ref_ffi_cast_ = LUA_NOREF;
    }
    return *this;
}

// ============================================================================
// Setup
// ============================================================================

void LuaState::apply_sandbox()
{
    if (!L_) return;

    // Disable dangerous globals.
    static const char *const disabled[] = {"dofile", "loadfile", "io", nullptr};
    for (int i = 0; disabled[i]; ++i)
    {
        lua_pushnil(L_);
        lua_setglobal(L_, disabled[i]);
    }

    // Disable os.execute and os.exit (keep os.clock, os.time, etc.).
    lua_getglobal(L_, "os");
    if (lua_istable(L_, -1))
    {
        lua_pushnil(L_);
        lua_setfield(L_, -2, "execute");
        lua_pushnil(L_);
        lua_setfield(L_, -2, "exit");
    }
    lua_pop(L_, 1);

    // Disable package.loadlib (prevents loading arbitrary C modules).
    lua_getglobal(L_, "package");
    if (lua_istable(L_, -1))
    {
        lua_pushnil(L_);
        lua_setfield(L_, -2, "loadlib");
    }
    lua_pop(L_, 1);
}

void LuaState::add_package_path(const std::string &path)
{
    if (!L_) return;

    lua_getglobal(L_, "package");
    lua_getfield(L_, -1, "path");
    const char *current = lua_tostring(L_, -1);
    std::string new_path = (current ? std::string(current) + ";" : std::string{}) + path;
    lua_pop(L_, 1); // pop old path
    lua_pushstring(L_, new_path.c_str());
    lua_setfield(L_, -2, "path");
    lua_pop(L_, 1); // pop package table
}

void LuaState::cache_ffi_cast()
{
    if (!L_) return;

    lua_getglobal(L_, "require");
    lua_pushstring(L_, "ffi");
    if (lua_pcall(L_, 1, 1, 0) == LUA_OK)
    {
        lua_getfield(L_, -1, "cast");
        ref_ffi_cast_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        lua_pop(L_, 1); // pop ffi table
    }
    else
    {
        LOGGER_WARN("LuaState: Failed to cache ffi.cast — slot views will use require() fallback");
        lua_pop(L_, 1); // pop error
    }
}

// ============================================================================
// Script loading
// ============================================================================

bool LuaState::load_script(const std::filesystem::path &path, const char *tag)
{
    if (!L_) return false;

    if (luaL_loadfile(L_, path.string().c_str()) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[{}] Failed to load '{}': {}", tag, path.string(),
                     err ? err : "(unknown error)");
        lua_pop(L_, 1);
        return false;
    }

    if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[{}] Error executing '{}': {}", tag, path.string(),
                     err ? err : "(unknown error)");
        lua_pop(L_, 1);
        return false;
    }

    return true;
}

// ============================================================================
// FFI helpers
// ============================================================================

bool LuaState::register_ffi_type(const std::string &cdef_str, const char *tag)
{
    if (!L_) return false;

    lua_getglobal(L_, "require");
    lua_pushstring(L_, "ffi");
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK)
    {
        LOGGER_ERROR("[{}] require('ffi') failed: {}", tag,
                     lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?");
        lua_pop(L_, 1);
        return false;
    }

    lua_getfield(L_, -1, "cdef");
    lua_pushstring(L_, cdef_str.c_str());
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
    {
        LOGGER_ERROR("[{}] ffi.cdef failed: {}", tag,
                     lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?");
        lua_pop(L_, 2); // pop error + ffi table
        return false;
    }

    lua_pop(L_, 1); // pop ffi table
    return true;
}

size_t LuaState::ffi_sizeof(const char *type_name) const
{
    if (!L_) return 0;

    lua_getglobal(L_, "require");
    lua_pushstring(L_, "ffi");
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK)
    {
        lua_pop(L_, 1);
        return 0;
    }

    lua_getfield(L_, -1, "sizeof");
    lua_pushstring(L_, type_name);
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK)
    {
        lua_pop(L_, 2); // error + ffi table
        return 0;
    }

    size_t sz = static_cast<size_t>(lua_tointeger(L_, -1));
    lua_pop(L_, 2); // result + ffi table
    return sz;
}

bool LuaState::push_slot_view(void *data, size_t size, const char *type_name)
{
    (void)size; // reserved for future bounds checking
    return push_ffi_cast_(data, type_name, /*readonly=*/false);
}

bool LuaState::push_slot_view_readonly(const void *data, size_t size, const char *type_name)
{
    (void)size;
    // const_cast is safe: the Lua script should treat this as read-only.
    // Enforcement is by convention (API documentation), not by the type system.
    return push_ffi_cast_(const_cast<void *>(data), type_name, /*readonly=*/true);
}

bool LuaState::push_ffi_cast_(void *data, const char *type_name, bool readonly)
{
    if (!L_) return false;

    // Fast path: use cached ffi.cast reference.
    if (ref_ffi_cast_ != LUA_NOREF)
    {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_ffi_cast_);
    }
    else
    {
        // Slow path: require('ffi').cast each time.
        lua_getglobal(L_, "require");
        lua_pushstring(L_, "ffi");
        if (lua_pcall(L_, 1, 1, 0) != LUA_OK)
        {
            lua_pop(L_, 1);
            return false;
        }
        lua_getfield(L_, -1, "cast");
        lua_remove(L_, -2); // remove ffi table, keep cast
    }

    // ffi.cast("type_name*", data)
    std::string cast_type = std::string(type_name) + (readonly ? " const*" : "*");
    lua_pushstring(L_, cast_type.c_str());
    lua_pushlightuserdata(L_, data);
    if (lua_pcall(L_, 2, 1, 0) != LUA_OK)
    {
        lua_pop(L_, 1); // pop error
        return false;
    }

    return true; // cdata on top of stack
}

// ============================================================================
// Registry reference helpers
// ============================================================================

bool LuaState::is_ref_callable(int r) const
{
    if (!L_ || r == LUA_NOREF || r == LUA_REFNIL)
        return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, r);
    bool callable = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    return callable;
}

// ============================================================================
// Protected call
// ============================================================================

bool LuaState::pcall(int nargs, int nresults, const char *tag)
{
    if (!L_) return false;

    if (lua_pcall(L_, nargs, nresults, 0) != LUA_OK)
    {
        const char *err = lua_tostring(L_, -1);
        LOGGER_ERROR("[{}] Lua error: {}", tag, err ? err : "(unknown)");
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

} // namespace pylabhub::scripting
