#pragma once
/**
 * @file lua_role_host_base.hpp
 * @brief LuaRoleHostBase — Lua-specific common layer for all role script hosts.
 *
 * Manages lua_State lifecycle, sandbox, and package path directly;
 * composes RoleHostCore (engine-agnostic infrastructure).  Provides the
 * common do_lua_work_() skeleton via virtual hooks for role-specific dispatch.
 *
 * ## What LuaRoleHostBase provides
 *
 * **From LuaScriptHost:**
 *  - lua_State lifecycle, sandbox, package.path setup
 *  - call_lua_fn_() protected helper for safe pcall
 *
 * **Own common code:**
 *  - do_lua_work_() skeleton (~100 lines shared across all roles)
 *  - FFI schema generation (SchemaSpec → ffi.cdef C struct string)
 *  - Lua slot view creation via ffi.cast (zero-copy SHM access)
 *  - call_on_init_common_ / call_on_stop_common_ wrappers
 *  - Lua message table builder
 *  - startup_() / shutdown_() / signal_shutdown() identical for all roles
 *
 * **From RoleHostCore (composed):**
 *  - Thread-safe incoming message queue
 *  - Shutdown coordination flags
 *  - State flags (validate_only, script_load_ok, running)
 *  - FlexZone schema storage
 *
 * ## Threading model
 *
 * LuaScriptHost uses direct mode (owns_dedicated_thread=false).  The role's
 * data loop thread drives Lua callbacks directly — there is no separate
 * interpreter thread.  All lua_State access happens from one thread at a time;
 * Lua has no GIL.
 *
 * ## Subclass contract (virtual hooks)
 *
 * See protected section for the full list.  Each role subclass (LuaProducerHost,
 * LuaConsumerHost, LuaProcessorHost) overrides these to provide specific behavior
 * while reusing the common skeleton.
 *
 * See HEP-CORE-0011 for the ScriptHost abstraction framework.
 */

#include "role_host_core.hpp"

#include "utils/hub_inbox_queue.hpp"
#include "utils/messenger.hpp"
#include "utils/script_host_helpers.hpp"
#include "utils/startup_wait.hpp"

#include <lua.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pylabhub::scripting
{

/// Reason why the Lua role script host stopped.
/// Same values as the Python StopReason enum for consistency.
enum class LuaStopReason : int
{
    Normal        = 0,
    PeerDead      = 1,
    HubDead       = 2,
    CriticalError = 3,
};

class LuaRoleHostBase
{
  public:
    virtual ~LuaRoleHostBase();

    LuaRoleHostBase(const LuaRoleHostBase &)            = delete;
    LuaRoleHostBase &operator=(const LuaRoleHostBase &) = delete;

    // ── Common lifecycle (identical for all roles) ───────────────────────────

    /// Initialize the Lua state, load the script, start the role, and run.
    /// Blocks until the role stops or shutdown is requested.
    void startup_();

    /// Shut down: stop role, finalize lua_State.
    void shutdown_() noexcept;

    /// Signal shutdown from an external thread.
    void signal_shutdown() noexcept;

    // ── Common configuration ─────────────────────────────────────────────────

    void set_validate_only(bool v) noexcept { core_.validate_only = v; }
    void set_shutdown_flag(std::atomic<bool> *flag) noexcept { core_.g_shutdown = flag; }

    /**
     * @brief Block until notified or timeout_ms elapses.
     *
     * Used by run_role_main_loop() instead of sleep_for() so that stop_role()
     * can wake the monitoring thread immediately via core_.notify_incoming().
     */
    void wait_for_wakeup(int timeout_ms) noexcept { core_.wait_for_incoming(timeout_ms); }

    // ── Common post-startup results ──────────────────────────────────────────

    [[nodiscard]] bool script_load_ok() const noexcept { return core_.script_load_ok; }
    [[nodiscard]] bool is_running()     const noexcept { return core_.running; }

  protected:
    LuaRoleHostBase() = default;

    // ── Composed engine-agnostic infrastructure ──────────────────────────────

    RoleHostCore core_;

    // ── Lua state ────────────────────────────────────────────────────────────

    lua_State *L_{nullptr};

    // ── Lua callback references (stored in Lua registry) ─────────────────────

    int ref_on_init_{LUA_NOREF};
    int ref_on_stop_{LUA_NOREF};
    int ref_api_{LUA_NOREF};
    int ref_ffi_cast_{LUA_NOREF}; ///< Cached ffi.cast function (avoids require per iteration)

    // ── Stop reason ──────────────────────────────────────────────────────────

    std::atomic<int> stop_reason_{0};

    // ── Shutdown flag ────────────────────────────────────────────────────────

    std::atomic<bool> stop_{false};

    // ── Shared metrics (common to all roles) ─────────────────────────────────

    std::atomic<uint64_t> script_errors_{0};
    std::atomic<bool>     critical_error_{false};

    // ── Inbox facility (common to all roles) ─────────────────────────────────

    std::unique_ptr<hub::InboxQueue> inbox_queue_;
    SchemaSpec  inbox_spec_;
    size_t      inbox_schema_slot_size_{0};
    std::string inbox_ffi_type_;
    int         ref_on_inbox_{LUA_NOREF};

    // ── Worker thread (non-blocking startup) ─────────────────────────────────

    std::thread        worker_thread_;
    std::promise<void> ready_promise_;
    std::future<void>  ready_future_{ready_promise_.get_future()};

    /** Signal that script load and role start have completed (or failed). */
    void signal_ready_();

    // ── Common implementations ───────────────────────────────────────────────

    /**
     * @brief The main work skeleton — called by startup_() on the calling thread.
     *
     * Creates lua_State, loads script, wires API, starts role, waits, stops.
     */
    void do_lua_work_();

    /** Build FFI cdef string from a SchemaSpec.  Returns e.g. "typedef struct { float x; } SlotFrame;" */
    static std::string build_ffi_cdef_(const SchemaSpec &spec, const char *struct_name,
                                       const std::string &packing);

    /** Register the FFI struct type in the Lua state (calls ffi.cdef). */
    bool register_ffi_type_(const std::string &cdef_str);

    /** Query ffi.sizeof(type_name) — returns 0 on error. */
    size_t ffi_sizeof_(const char *type_name);

    /** Create a zero-copy slot view via ffi.cast.  Pushes the cdata onto the Lua stack. */
    bool push_slot_view_(void *data, size_t size, const char *type_name);

    /** Push a read-only slot view (const cast).  Pushes cdata onto Lua stack. */
    bool push_slot_view_readonly_(const void *data, size_t size, const char *type_name);

    /** Internal: push ffi.cast result onto Lua stack using cached ffi.cast ref. */
    bool push_ffi_cast_(void *data, const char *type_name, bool readonly);

    /** Build a Lua table from incoming messages.  Pushes table onto stack. */
    virtual void push_messages_table_(std::vector<IncomingMessage> &msgs);

    /** Common on_init wrapper: calls on_init(api) if present. */
    void call_on_init_common_();

    /** Common on_stop wrapper: calls on_stop(api) if present. */
    void call_on_stop_common_();

    /** Release all Lua registry references. */
    void clear_lua_refs_();

    /** Safe Lua pcall wrapper.  name is for error messages.  nargs already on stack. */
    bool call_lua_fn_(const char *name, int nargs, int nresults = 0);

    /** Check if a Lua registry ref is a callable function. */
    bool is_ref_callable_(int ref) const;

    // ── Common API closures (shared by all roles) ───────────────────────────

    /**
     * @brief Push the common API closures (log, stop, set_critical_error,
     * stop_reason, script_errors) and string fields (log_level, script_dir,
     * role_dir) onto the Lua table at the top of the stack.
     *
     * Call this from build_role_api_table_() after lua_newtable(), before
     * adding role-specific closures.
     */
    void push_common_api_closures_(lua_State *L);

    /** Shared lua_api_log — dispatches level via role_tag(). */
    static int lua_api_log(lua_State *L);
    static int lua_api_stop(lua_State *L);
    static int lua_api_set_critical_error(lua_State *L);
    static int lua_api_stop_reason(lua_State *L);
    static int lua_api_script_errors(lua_State *L);

    // ── Inbox drain (shared by all roles) ───────────────────────────────────

    /**
     * @brief Non-blocking synchronous inbox drain.
     *
     * Called from the data loop thread (which owns lua_State).  Processes
     * all pending inbox messages by calling on_inbox(slot, sender, api).
     */
    void drain_inbox_sync_();

    // ── Wait-for-roles (HEP-0023, shared by all roles) ─────────────────────

    /**
     * @brief Block until all wait_for_roles entries are present.
     * @param messenger The Messenger to query role presence.
     * @param wait_list The list of (uid, timeout_ms) entries.
     * @return true if all found, false if any timed out.
     */
    bool wait_for_roles_(hub::Messenger &messenger,
                         const std::vector<pylabhub::WaitForRole> &wait_list);

    // ── Virtual hooks for role dispatch ──────────────────────────────────────

    // Identity
    virtual const char *role_tag()  const = 0; ///< "prod", "cons", "proc"
    virtual const char *role_name() const = 0; ///< "producer", "consumer", "processor"
    virtual std::string role_uid()  const = 0; ///< From config

    // Config accessors (for common closures and helpers)
    virtual std::string config_uid()   const = 0; ///< Role UID from config
    virtual std::string config_name()  const = 0; ///< Role name from config
    virtual std::string config_channel() const = 0; ///< Channel name from config
    virtual std::string config_log_level()   const = 0;
    virtual std::string config_script_path() const = 0;
    virtual std::string config_role_dir()    const = 0;
    virtual bool        stop_on_script_error() const = 0;

    // Script loading
    virtual std::string script_base_dir() const = 0; ///< Config script path
    virtual std::string script_type_str() const = 0; ///< "lua"
    virtual std::string required_callback_name() const = 0; ///< "on_produce" etc.

    // API wiring (Lua state available, before script callbacks)
    virtual void build_role_api_table_(lua_State *L) = 0;

    // Callback extraction (after script module loaded)
    virtual void extract_callbacks_() = 0;
    virtual bool has_required_callback() const = 0;

    // Schema and validation
    virtual bool build_role_types() = 0;
    virtual void print_validate_layout() = 0;

    // Lifecycle
    virtual bool start_role() = 0;
    virtual void stop_role() = 0;
    virtual void cleanup_on_start_failure() = 0;

    // Error handling
    virtual void on_script_error() = 0;

    // call_on_stop guard: returns true if the connection needed for on_stop exists
    virtual bool has_connection_for_stop() const = 0;

    // FlexZone checksum update after on_init (default: no-op)
    virtual void update_fz_checksum_after_init() {}

    /**
     * @brief Run the role's data loop on the worker thread (blocking).
     *
     * The subclass implements the transport-agnostic loop here.  All Lua callbacks
     * (on_produce, on_consume, on_process, on_inbox) must be called from within
     * this method — it runs on the same thread that owns lua_State.
     *
     * Replaces the Python model where loop_thread_ + inbox_thread_ acquire the GIL.
     * For Lua, there is no GIL; the data loop thread IS the Lua thread.
     */
    virtual void run_data_loop_() = 0;
};

// ============================================================================
// FFI type mapping helpers (SchemaSpec field → C type string)
// ============================================================================

/// Map a FieldDef type_str to the corresponding C type string for ffi.cdef.
/// Returns empty string if the type is not supported.
std::string ffi_c_type_for_field(const std::string &type_str);

} // namespace pylabhub::scripting
