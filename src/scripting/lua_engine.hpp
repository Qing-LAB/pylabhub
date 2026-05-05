#pragma once
/**
 * @file lua_engine.hpp
 * @brief LuaEngine — ScriptEngine implementation for LuaJIT.
 *
 * Wraps LuaState.  All invoke methods call lua_pcall directly — no GIL,
 * no locking.  All calls must happen on the thread that called initialize().
 *
 * supports_multi_state() returns true: create_thread_state() creates a new
 * LuaEngine with its own LuaState, loading the same script independently.
 */

#include "lua_state.hpp"
#include "utils/script_engine.hpp"

#include "utils/hub_inbox_queue.hpp"

#include <lua.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace pylabhub::hub_host { class HubAPI; }   // forward — full type only
                                                  // needed in lua_engine.cpp

namespace pylabhub::scripting
{

class LuaEngine : public ScriptEngine
{
  public:
    LuaEngine() = default;
    ~LuaEngine() override;

    LuaEngine(const LuaEngine &) = delete;
    LuaEngine &operator=(const LuaEngine &) = delete;

    // ── ScriptEngine lifecycle ───────────────────────────────────────────

    bool init_engine_(const std::string &log_tag, RoleHostCore *core) override;
    bool load_script(const std::filesystem::path &script_dir,
                     const std::string &entry_point,
                     const std::string &required_callback) override;
    bool build_api_(RoleAPIBase &api) override;
    /// Hub-side build_api override (HEP-CORE-0033 §12.3).  Creates a
    /// Lua `api` table populated with closures for the hub script
    /// surface (lifecycle log/uid/metrics + read accessors + control
    /// delegates), exposes it as a global so scripts can reference
    /// `api.log(...)` from any callback.  Hub scripts use the generic
    /// `invoke(name, args)` for event callbacks (`on_channel_opened`
    /// etc.) — those don't need pre-extraction because invoke uses
    /// lua_getglobal on each call.
    bool build_api_(::pylabhub::hub_host::HubAPI &api) override;
    void finalize_engine_() override;

    // ── Queries ──────────────────────────────────────────────────────────

    [[nodiscard]] bool has_callback(const std::string &name) const override;

    // ── Schema / type building ───────────────────────────────────────────

    bool register_slot_type(const hub::SchemaSpec &spec,
                            const std::string &type_name,
                            const std::string &packing) override;
    [[nodiscard]] size_t type_sizeof(const std::string &type_name) const override;

    // ── Callback invocation ──────────────────────────────────────────────

    void invoke_on_init() override;
    void invoke_on_stop() override;

    InvokeResult invoke_produce(
        InvokeTx tx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_consume(
        InvokeRx rx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_process(
        InvokeRx rx, InvokeTx tx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_on_inbox(InvokeInbox msg) override;

    // ── Generic invoke (thread-safe) ───────────────────────────────────

    bool invoke(const std::string &name) override;
    bool invoke(const std::string &name, const nlohmann::json &args) override;
    InvokeResponse eval(const std::string &code) override;

    // ── Error state ──────────────────────────────────────────────────────

    // Body in lua_engine.cpp — needs full HubAPI type to call
    // hub_api_->core(); this header only forward-declares HubAPI.
    [[nodiscard]] uint64_t script_error_count() const noexcept override;

    // ── Threading ────────────────────────────────────────────────────────

    [[nodiscard]] bool supports_multi_state() const noexcept override { return true; }
    void release_thread() override;

  private:
    LuaState    state_;
    std::string log_tag_;
    std::string script_dir_str_;  ///< Saved for create_thread_state() reload
    std::string entry_point_;
    std::string required_callback_;

    // Lua registry references for callbacks
    int ref_on_init_{LUA_NOREF};
    int ref_on_stop_{LUA_NOREF};
    int ref_on_produce_{LUA_NOREF};
    int ref_on_consume_{LUA_NOREF};
    int ref_on_process_{LUA_NOREF};
    int ref_on_inbox_{LUA_NOREF};
    int ref_api_{LUA_NOREF};

    // script_errors is in api_->core()->script_errors_ (RoleHostCore).
    // Only valid after build_api() sets api_. All pcall error paths
    // guard with `if (api_ && api_->core())` — errors before build_api()
    // are reported via return values (load_script, register_slot_type),
    // not via script_errors.

    // ── Cached FFI ctype refs (set during register_slot_type) ───────────
    // Created via ffi.typeof() at init time. Used by invoke_* for zero-string-op
    // slot view creation on the hot path.
    // Directional FFI type refs.
    int ref_in_slot_readonly_{LUA_NOREF};   ///< ffi.typeof("InSlotFrame const*")
    int ref_out_slot_writable_{LUA_NOREF};  ///< ffi.typeof("OutSlotFrame*")
    int ref_in_fz_{LUA_NOREF};              ///< ffi.typeof("InFlexFrame*") (mutable per HEP-0002)
    int ref_out_fz_{LUA_NOREF};             ///< ffi.typeof("OutFlexFrame*")
    int ref_inbox_readonly_{LUA_NOREF};     ///< ffi.typeof("InboxFrame const*")

    // Script-level aliases for producer/consumer convenience.
    int ref_slot_alias_writable_{LUA_NOREF};  ///< "SlotFrame*" alias (producer)
    int ref_slot_alias_readonly_{LUA_NOREF};  ///< "SlotFrame const*" alias (consumer)
    int ref_fz_alias_{LUA_NOREF};             ///< "FlexFrame*" alias

    // api_ is inherited from ScriptEngine (set by build_api).
    bool stop_on_script_error_{false};

    // Inbox cache is shared in core_ (RoleHostCore::inbox_cache_).

    // ── Thread-state cache (multi-state: non-owner threads) ──────────────
    std::unordered_map<std::thread::id,
                       std::unique_ptr<LuaEngine>> thread_states_;
    std::mutex thread_states_mu_;
    // accepting_ is inherited from ScriptEngine base class.

    LuaEngine *get_or_create_thread_state_();

    // ── Internal helpers ─────────────────────────────────────────────────

    /// Build FFI cdef string from hub::SchemaSpec. Returns empty string on error.
    std::string build_ffi_cdef_(const hub::SchemaSpec &spec,
                                const std::string &type_name,
                                const std::string &packing);

    void push_messages_table_(std::vector<IncomingMessage> &msgs);
    void push_messages_table_bare_(std::vector<IncomingMessage> &msgs);
    void clear_refs_();

    /// Extract a global function as a registry ref, or LUA_NOREF if absent.
    int extract_callback_ref_(const char *name);

    /// Handle pcall error: log, increment counter, optionally request shutdown.
    InvokeResult on_pcall_error_(const std::string &callback_name);

    // ── API closure statics ──────────────────────────────────────────────

    void push_common_api_closures_(lua_State *L);
    void register_inbox_metatable_();

    static int lua_api_log(lua_State *L);
    static int lua_api_stop(lua_State *L);

    // ── Hub-side closures (HEP-CORE-0033 §12.3) ───────────────────────
    // Each closure captures `this` (LuaEngine*) as upvalue(1) and
    // dispatches to the bound `hub_api_` (set by build_api(HubAPI&)).
    // Lifecycle.
    static int lua_api_hub_log(lua_State *L);
    static int lua_api_hub_uid(lua_State *L);
    static int lua_api_hub_metrics(lua_State *L);
    // Read accessors (§12.3 read block).
    static int lua_api_hub_name(lua_State *L);
    static int lua_api_hub_config(lua_State *L);
    static int lua_api_hub_list_channels(lua_State *L);
    static int lua_api_hub_get_channel(lua_State *L);
    static int lua_api_hub_list_roles(lua_State *L);
    static int lua_api_hub_get_role(lua_State *L);
    static int lua_api_hub_list_bands(lua_State *L);
    static int lua_api_hub_get_band(lua_State *L);
    static int lua_api_hub_list_peers(lua_State *L);
    static int lua_api_hub_get_peer(lua_State *L);
    static int lua_api_hub_query_metrics(lua_State *L);
    // Control delegates (§12.3 control block).
    static int lua_api_hub_close_channel(lua_State *L);
    static int lua_api_hub_broadcast_channel(lua_State *L);
    static int lua_api_hub_request_shutdown(lua_State *L);
    static int lua_api_set_critical_error(lua_State *L);
    static int lua_api_stop_reason(lua_State *L);
    static int lua_api_script_error_count(lua_State *L);
    static int lua_api_version_info(lua_State *L);
    static int lua_api_wait_for_role(lua_State *L);
    static int lua_api_open_inbox(lua_State *L);
    static int lua_api_get_shared_data(lua_State *L);
    static int lua_api_set_shared_data(lua_State *L);
    static int lua_api_uid(lua_State *L);
    static int lua_api_name(lua_State *L);
    static int lua_api_channel(lua_State *L);
    static int lua_api_update_flexzone_checksum(lua_State *L);
    static int lua_api_set_verify_checksum(lua_State *L);
    static int lua_api_out_slots_written(lua_State *L);
    static int lua_api_in_slots_received(lua_State *L);
    static int lua_api_out_drop_count(lua_State *L);
    static int lua_api_metrics(lua_State *L);
    static int lua_api_in_channel(lua_State *L);
    static int lua_api_out_channel(lua_State *L);

    // ── Group A: diagnostics (common) ─────────────────────────────────
    static int lua_api_loop_overrun_count(lua_State *L);
    static int lua_api_last_cycle_work_us(lua_State *L);
    static int lua_api_critical_error(lua_State *L);

    // ── Group B: queue-state (role-specific) ──────────────────────────
    static int lua_api_out_capacity(lua_State *L);
    static int lua_api_out_policy(lua_State *L);
    static int lua_api_in_capacity(lua_State *L);
    static int lua_api_in_policy(lua_State *L);
    static int lua_api_last_seq(lua_State *L);

    // ── Group C: custom metrics ───────────────────────────────────────
    static int lua_api_report_metric(lua_State *L);
    static int lua_api_report_metrics(lua_State *L);
    static int lua_api_clear_custom_metrics(lua_State *L);

    // ── Group E: broker operations ────────────────────────────────────
    static int lua_api_clear_inbox_cache(lua_State *L);

    // ── Group G: band pub/sub (HEP-CORE-0030) ─────────────────────────
    static int lua_api_band_join(lua_State *L);
    static int lua_api_band_leave(lua_State *L);
    static int lua_api_band_broadcast(lua_State *L);
    static int lua_api_band_members(lua_State *L);

    // ── Group F: schema sizes + spinlocks (SHM-only) ──────────────────
    static int lua_api_slot_logical_size(lua_State *L);
    static int lua_api_flexzone_logical_size(lua_State *L);
    static int lua_api_spinlock(lua_State *L);
    static int lua_api_spinlock_count(lua_State *L);
    void register_spinlock_metatable_();

    static int lua_spinlock_lock(lua_State *L);
    static int lua_spinlock_unlock(lua_State *L);
    static int lua_spinlock_try_lock_for(lua_State *L);
    static int lua_spinlock_is_locked(lua_State *L);
    static int lua_spinlock_gc(lua_State *L);

    // ── Group G: flexzone getter ──────────────────────────────────────
    static int lua_api_flexzone(lua_State *L);
};

} // namespace pylabhub::scripting
