#pragma once
/**
 * @file lua_state.hpp
 * @brief LuaState — RAII wrapper for lua_State with thread-ownership tracking.
 *
 * Provides a C++-idiomatic interface to the Lua C API while preserving
 * access to the raw pointer for static closure functions (which receive
 * lua_State* from Lua callbacks and cannot use the wrapper).
 *
 * ## Design rationale
 *
 * The Lua C API uses a raw `lua_State*` with no const-correctness, no RAII,
 * and no thread-safety.  This wrapper addresses all three:
 *
 * 1. **RAII**: Constructor creates, destructor closes.  Move-only.
 * 2. **Const**: Methods that are logically read-only (balanced push/pop,
 *    sizeof queries) are marked `const`.  This works because `lua_State*`
 *    is inherently mutable — the `const` applies to the wrapper, not Lua.
 * 3. **Thread ownership**: Debug-mode assertion that all calls come from the
 *    owning thread.  Catches cross-thread access at development time.
 *
 * ## Multi-state model
 *
 * Each C++ thread that needs Lua creates its own LuaState.  This enables
 * true parallelism (no GIL, no shared state).  Threads share data through
 * C++ atomics and synchronization primitives, not through Lua.
 *
 * See HEP-CORE-0011 § ScriptHost Abstraction Framework.
 */

#include <lua.hpp>

#include <cassert>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

namespace pylabhub::scripting
{

class LuaState
{
  public:
    // ── Construction / destruction ────────────────────────────────────────

    /** Default: empty state (no lua_State created). Call create() or assign later. */
    LuaState() noexcept = default;

    /** Create a new lua_State with all standard libraries opened. */
    static LuaState create();

    /** Adopt an existing lua_State (takes ownership). */
    explicit LuaState(lua_State *L) noexcept;

    ~LuaState();

    LuaState(LuaState &&other) noexcept;
    LuaState &operator=(LuaState &&other) noexcept;

    LuaState(const LuaState &) = delete;
    LuaState &operator=(const LuaState &) = delete;

    /** True if the state was successfully created. */
    [[nodiscard]] explicit operator bool() const noexcept { return L_ != nullptr; }

    // ── Raw access ───────────────────────────────────────────────────────
    //
    // Static closure functions (lua_CFunction) receive lua_State* from Lua.
    // They MUST use the raw pointer — the wrapper is not available in that
    // context.  These accessors are for host-side code that needs to
    // interop with raw Lua C API calls.

    [[nodiscard]] lua_State *raw() const noexcept { return L_; }

    // ── Setup (call once after construction) ─────────────────────────────

    /**
     * @brief Apply security sandbox: disable io.*, os.execute/exit, dofile,
     *        loadfile, package.loadlib.
     */
    void apply_sandbox();

    /** Add a directory to package.path for require(). */
    void add_package_path(const std::string &path);

    /** Cache ffi.cast for fast slot view creation in the data loop. */
    void cache_ffi_cast();

    // ── Script loading ───────────────────────────────────────────────────

    /**
     * @brief Load and execute a Lua script file.
     * @return true on success, false on load or execution error.
     *         Error message is logged.
     */
    bool load_script(const std::filesystem::path &path, const char *tag);

    // ── FFI helpers ──────────────────────────────────────────────────────

    /** Register an FFI struct type via ffi.cdef(cdef_str). */
    bool register_ffi_type(const std::string &cdef_str, const char *tag);

    /** Query ffi.sizeof(type_name).  Returns 0 on error. */
    [[nodiscard]] size_t ffi_sizeof(const char *type_name) const;

    /**
     * @brief Push a zero-copy slot view via ffi.cast onto the Lua stack.
     * @param data Pointer to the raw memory buffer.
     * @param size Size of the buffer in bytes (used for validation).
     * @param type_name FFI type name (e.g., "SlotFrame").
     * @return true if the cdata was pushed, false on error.
     */
    bool push_slot_view(void *data, size_t size, const char *type_name);

    /** Push a read-only slot view (const void*). */
    bool push_slot_view_readonly(const void *data, size_t size, const char *type_name);

    // ── Registry reference management ────────────────────────────────────

    /** Create a reference from the value at the top of the stack. */
    [[nodiscard]] int ref() { return luaL_ref(L_, LUA_REGISTRYINDEX); }

    /** Release a registry reference. */
    void unref(int r)
    {
        if (r != LUA_NOREF && L_)
            luaL_unref(L_, LUA_REGISTRYINDEX, r);
    }

    /** Push a registry reference onto the stack. */
    void push_ref(int r) const { lua_rawgeti(L_, LUA_REGISTRYINDEX, r); }

    /** Check if a registry reference is a callable function. */
    [[nodiscard]] bool is_ref_callable(int r) const;

    // ── Protected call ───────────────────────────────────────────────────

    /**
     * @brief Safe lua_pcall wrapper.
     * @param nargs Number of arguments already on the stack (below the function).
     * @param nresults Expected number of return values (or LUA_MULTRET).
     * @param tag Label for error messages (e.g., "on_produce").
     * @return true on success, false on error (error message is logged and popped).
     */
    bool pcall(int nargs, int nresults, const char *tag);

    // ── Stack operations (thin forwarding) ───────────────────────────────

    void push_string(const char *s) { lua_pushstring(L_, s); }
    void push_lstring(const char *s, size_t len) { lua_pushlstring(L_, s, len); }
    void push_integer(lua_Integer n) { lua_pushinteger(L_, n); }
    void push_number(lua_Number n) { lua_pushnumber(L_, n); }
    void push_boolean(bool b) { lua_pushboolean(L_, b ? 1 : 0); }
    void push_nil() { lua_pushnil(L_); }
    void push_lightuserdata(void *p) { lua_pushlightuserdata(L_, p); }
    void push_cclosure(lua_CFunction fn, int nupvalues) { lua_pushcclosure(L_, fn, nupvalues); }
    void push_cfunction(lua_CFunction fn) { lua_pushcfunction(L_, fn); }
    void push_value(int idx) { lua_pushvalue(L_, idx); }

    void new_table() { lua_newtable(L_); }
    void set_field(int idx, const char *name) { lua_setfield(L_, idx, name); }
    void get_field(int idx, const char *name) { lua_getfield(L_, idx, name); }
    void set_global(const char *name) { lua_setglobal(L_, name); }
    void get_global(const char *name) { lua_getglobal(L_, name); }
    void raw_seti(int idx, int n) { lua_rawseti(L_, idx, n); }

    void pop(int n = 1) { lua_pop(L_, n); }

    [[nodiscard]] bool is_function(int idx) const { return lua_isfunction(L_, idx); }
    [[nodiscard]] bool is_table(int idx) const { return lua_istable(L_, idx); }
    [[nodiscard]] bool is_nil(int idx) const { return lua_isnil(L_, idx); }
    [[nodiscard]] const char *to_string(int idx) const { return lua_tostring(L_, idx); }
    [[nodiscard]] lua_Integer to_integer(int idx) const { return lua_tointeger(L_, idx); }
    [[nodiscard]] int to_boolean(int idx) const { return lua_toboolean(L_, idx); }

    /** Create a full userdata of the given size and push it onto the stack. */
    void *new_userdata(size_t sz) { return lua_newuserdata(L_, sz); }

    /** Set the metatable for the value at the given index. */
    void set_metatable(int idx) { lua_setmetatable(L_, idx); }

    /** Create or fetch a named metatable. Returns true if newly created. */
    bool new_metatable(const char *name) { return luaL_newmetatable(L_, name) != 0; }
    void get_metatable(const char *name) { luaL_getmetatable(L_, name); }

    // ── Thread ownership ─────────────────────────────────────────────────

    /** The thread that created (or adopted) this state. */
    [[nodiscard]] std::thread::id owner_thread() const noexcept { return owner_; }

    /** Assert that the current thread is the owner (debug builds only). */
    void assert_owner() const
    {
        assert(std::this_thread::get_id() == owner_ &&
               "LuaState accessed from non-owning thread");
    }

    /** Transfer ownership to the current thread (call after moving to a new thread). */
    void set_owner_thread() noexcept { owner_ = std::this_thread::get_id(); }

  private:
    lua_State *L_{nullptr};
    int ref_ffi_cast_{LUA_NOREF}; ///< Cached ffi.cast (per-state)
    std::thread::id owner_;

    /** Internal: push ffi.cast result using cached ref. */
    bool push_ffi_cast_(void *data, const char *type_name, bool readonly);
};

} // namespace pylabhub::scripting
