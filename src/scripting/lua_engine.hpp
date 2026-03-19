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
#include "script_engine.hpp"

#include "utils/hub_inbox_queue.hpp"

#include <lua.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

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

    bool initialize(const char *log_tag) override;
    bool load_script(const std::filesystem::path &script_dir,
                     const char *entry_point,
                     const char *required_callback) override;
    void build_api(const RoleContext &ctx) override;
    void finalize() override;

    // ── Queries ──────────────────────────────────────────────────────────

    [[nodiscard]] bool has_callback(const char *name) const override;

    // ── Schema / type building ───────────────────────────────────────────

    bool register_slot_type(const SchemaSpec &spec,
                            const char *type_name,
                            const std::string &packing) override;
    [[nodiscard]] size_t type_sizeof(const char *type_name) const override;

    // ── Callback invocation ──────────────────────────────────────────────

    void invoke_on_init() override;
    void invoke_on_stop() override;

    InvokeResult invoke_produce(
        void *out_slot, size_t out_sz,
        void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) override;

    void invoke_consume(
        const void *in_slot, size_t in_sz,
        const void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_process(
        const void *in_slot, size_t in_sz,
        void *out_slot, size_t out_sz,
        void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) override;

    void invoke_on_inbox(
        const void *data, size_t sz,
        const char *type_name,
        const char *sender) override;

    // ── Error state ──────────────────────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept override
    {
        return script_errors_.load(std::memory_order_relaxed);
    }

    // ── Threading ────────────────────────────────────────────────────────

    [[nodiscard]] bool supports_multi_state() const noexcept override { return true; }
    std::unique_ptr<ScriptEngine> create_thread_state() override;

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

    std::atomic<uint64_t> script_errors_{0};

    // ── Cached FFI ctype refs (set during register_slot_type) ───────────
    // Created via ffi.typeof() at init time. Used by invoke_* for zero-string-op
    // slot view creation on the hot path.
    int ref_slot_writable_{LUA_NOREF};   ///< ffi.typeof("SlotFrame*")
    int ref_slot_readonly_{LUA_NOREF};   ///< ffi.typeof("SlotFrame const*")
    int ref_in_slot_readonly_{LUA_NOREF};  ///< ffi.typeof("InSlotFrame const*") (processor)
    int ref_out_slot_writable_{LUA_NOREF}; ///< ffi.typeof("OutSlotFrame*") (processor)

    // ── RoleContext captured at build_api time ────────────────────────────
    RoleContext ctx_{};
    bool stop_on_script_error_{false};

    // ── Inbox handle cache ───────────────────────────────────────────────
    struct InboxEntry
    {
        std::shared_ptr<hub::InboxClient> client;
        std::string ffi_type;
        size_t item_size{0};
    };
    std::unordered_map<std::string, InboxEntry> inbox_cache_;

    // ── Internal helpers ─────────────────────────────────────────────────

    void push_messages_table_(std::vector<IncomingMessage> &msgs);
    void push_messages_table_bare_(std::vector<IncomingMessage> &msgs);
    void clear_refs_();

    /// Extract a global function as a registry ref, or LUA_NOREF if absent.
    int extract_callback_ref_(const char *name);

    /// Handle pcall error: log, increment counter, optionally request shutdown.
    InvokeResult on_pcall_error_(const char *callback_name);

    // ── API closure statics ──────────────────────────────────────────────

    void push_common_api_closures_(lua_State *L);
    void register_inbox_metatable_();

    static int lua_api_log(lua_State *L);
    static int lua_api_stop(lua_State *L);
    static int lua_api_set_critical_error(lua_State *L);
    static int lua_api_stop_reason(lua_State *L);
    static int lua_api_script_errors(lua_State *L);
    static int lua_api_version_info(lua_State *L);
    static int lua_api_wait_for_role(lua_State *L);
    static int lua_api_open_inbox(lua_State *L);
    static int lua_api_uid(lua_State *L);
    static int lua_api_name(lua_State *L);
    static int lua_api_channel(lua_State *L);
    static int lua_api_broadcast(lua_State *L);
    static int lua_api_send(lua_State *L);
    static int lua_api_consumers(lua_State *L);
    static int lua_api_update_flexzone_checksum(lua_State *L);
    static int lua_api_set_verify_checksum(lua_State *L);
    static int lua_api_out_written(lua_State *L);
    static int lua_api_in_received(lua_State *L);
    static int lua_api_drops(lua_State *L);
    static int lua_api_in_channel(lua_State *L);
    static int lua_api_out_channel(lua_State *L);
};

} // namespace pylabhub::scripting
