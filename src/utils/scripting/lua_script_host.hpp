#pragma once
/**
 * @file lua_script_host.hpp
 * @brief LuaScriptHost — LuaJIT concrete ScriptHost implementation.
 *
 * Runs on the **calling thread** (`owns_dedicated_thread() == false`).
 * The caller drives the tick loop and calls `tick()` periodically.
 *
 * ## Script layout
 *
 * Given a `script_path`, `LuaScriptHost` loads:
 * ```
 * <script_path>/lua/main.lua
 * ```
 *
 * ## Lua script interface
 *
 * ```lua
 * -- Called once after initialization. `api` is built by build_api_table_().
 * function on_start(api) end
 *
 * -- Called every tick. `tick` is a table:
 * --   {tick_count, elapsed_ms, uptime_ms, channels_ready, channels_pending, channels_closing}
 * function on_tick(api, tick) end
 *
 * -- Called once before shutdown.
 * function on_stop(api) end
 * ```
 * All callbacks are optional; missing ones are silently skipped.
 *
 * ## Sandbox
 *
 * By default, `io.*`, `os.execute`, `os.exit`, `dofile`, `loadfile`, and
 * `package.loadlib` are disabled. See `apply_sandbox_()`.
 *
 * ## Thread safety
 *
 * `LuaScriptHost` must be initialized and used from the same thread.
 * `tick()` validates `g_script_thread_state.owner == this` and logs an error
 * if called from the wrong thread.
 *
 * ## See also
 *
 * HEP-CORE-0011: ScriptHost Abstraction Framework (docs/HEP/)
 */

#include "utils/script_host.hpp"
#include "plh_platform.hpp"

// lua.hpp is LuaJIT's official C++ wrapper — it includes lua.h, lauxlib.h,
// lualib.h, and luajit.h inside an extern "C" block, ensuring correct
// C linkage for all Lua API symbols.
#include <lua.hpp>

#include <cstdint>
#include <thread>

namespace pylabhub::utils
{

/**
 * @struct LuaTickInfo
 * @brief Snapshot passed to `on_tick` as a Lua table.
 */
struct LuaTickInfo
{
    uint64_t tick_count{};       ///< Ticks since host started.
    uint64_t elapsed_ms{};       ///< Actual ms since last tick.
    uint64_t uptime_ms{};        ///< Total ms since host started.
    int      channels_ready{};   ///< Ready channels (0 if not applicable).
    int      channels_pending{}; ///< Pending channels (0 if not applicable).
    int      channels_closing{}; ///< Closing channels (0 if not applicable).
};

/**
 * @class LuaScriptHost
 * @brief LuaJIT concrete ScriptHost. Runs on the calling thread.
 *
 * Subclass and implement `build_api_table_()` to inject the application-specific
 * API into Lua's `api` parameter. For hub scripts this is the hub API; for role
 * scripts it is the role-specific API.
 */
class LuaScriptHost : public ScriptHost
{
public:
    ~LuaScriptHost() override;

    // ScriptHost interface
    [[nodiscard]] bool owns_dedicated_thread() const noexcept override { return false; }

    /**
     * @brief Call the script's `on_tick` function with a tick snapshot.
     *
     * Thread-safe check: logs an error and returns false if called from a
     * thread other than the one that called `base_startup_()`.
     *
     * @param tick  Snapshot to pass as the `tick` table argument.
     * @return `true` if the call succeeded or `on_tick` was absent; `false` on error.
     */
    bool tick(const LuaTickInfo& tick);

protected:
    /**
     * @brief Push the application-specific `api` table onto the Lua stack.
     *
     * Called before `on_start(api)` and `on_tick(api, tick)`. The implementation
     * should leave exactly one value (the table) on the stack.
     *
     * Example:
     * ```cpp
     * void HubLuaScript::build_api_table_(lua_State* L) {
     *     lua_newtable(L);
     *     lua_pushcfunction(L, [](lua_State* l) -> int {
     *         lua_pushstring(l, HubConfig::get_instance().hub_name().c_str());
     *         return 1;
     *     });
     *     lua_setfield(L, -2, "hub_name");
     *     // ... more methods ...
     * }
     * ```
     */
    virtual void build_api_table_(lua_State* L) = 0;

    // ScriptHost hooks
    bool do_initialize(const std::filesystem::path& script_path) override;
    void do_finalize() noexcept override;

    /// Access the raw Lua state (valid between do_initialize and do_finalize).
    [[nodiscard]] lua_State* lua_state() const noexcept { return L_; }

private:
    /// Apply default sandbox: disable io.*, os.execute/exit, dofile, loadfile, package.loadlib.
    void apply_sandbox_();

    /// Set package.path to include the opt/luajit/jit directory derived from exe path.
    void setup_package_path_();

    /// Push a tick info table onto the Lua stack.
    void push_tick_table_(const LuaTickInfo& tick);

    /// Call a named Lua global function with N args already on the stack.
    /// Returns true if the function existed and was called without error.
    bool call_lua_fn_(const char* name, int nargs);

    lua_State*       L_{nullptr};
    std::thread::id  init_thread_id_;
};

} // namespace pylabhub::utils
