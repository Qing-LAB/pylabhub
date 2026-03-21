#pragma once
/**
 * @file script_engine.hpp
 * @brief ScriptEngine — abstract interface for pluggable script engines.
 *
 * A ScriptEngine is a stateful callback dispatcher that knows how to:
 *   - Load a script and extract callbacks
 *   - Build typed slot views in the engine's type system
 *   - Invoke callbacks with raw memory pointers
 *   - Interpret return values (commit/discard/error)
 *
 * The engine does NOT know about queues, messengers, or the data loop.
 * The C++ framework (RoleHost) owns infrastructure and calls engine methods
 * at well-defined points from the working thread.
 *
 * ## Threading contract
 *
 * All methods are called from a single thread (the working thread) unless
 * supports_multi_state() returns true.  The engine is created, used, and
 * destroyed on that thread.
 *
 * ## Engine implementations
 *
 *   - LuaEngine:    wraps LuaState (direct pcall, no GIL)
 *   - PythonEngine:  wraps py::scoped_interpreter (GIL acquire/release per invoke)
 *
 * See docs/tech_draft/script_engine_refactor.md for the full design.
 */

#include "role_host_core.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pylabhub::hub { class Messenger; }
namespace pylabhub::hub { class QueueWriter; }
namespace pylabhub::hub { class QueueReader; }

namespace pylabhub::scripting
{

// ============================================================================
// InvokeResult — return value from on_produce / on_process
// ============================================================================

enum class InvokeResult
{
    Commit,   ///< Script returned true/nil — publish the slot.
    Discard,  ///< Script returned false — discard the slot, loop continues.
    Error,    ///< Script raised an error — discard, increment error count.
};

// ============================================================================
// RoleContext — engine-agnostic pointers the API object needs
// ============================================================================

/**
 * @brief Pointers to role infrastructure, passed to build_api().
 *
 * The engine uses these to construct its API object/table.  The engine does NOT
 * own any of these — they are managed by the RoleHost and valid for the
 * lifetime of the role.
 */
struct RoleContext
{
    const char *role_tag{nullptr};    ///< "prod", "cons", "proc"
    const char *uid{nullptr};
    const char *name{nullptr};
    const char *channel{nullptr};     ///< Primary channel (or in_channel for processor)
    const char *out_channel{nullptr}; ///< out_channel for processor (nullptr for producer/consumer)
    const char *log_level{nullptr};
    const char *script_dir{nullptr};
    const char *role_dir{nullptr};

    hub::Messenger   *messenger{nullptr};     ///< For open_inbox, wait_for_role, broadcast, send
    hub::QueueWriter *queue_writer{nullptr};   ///< For update_flexzone_checksum (producer/processor out)
    hub::QueueReader *queue_reader{nullptr};   ///< For set_verify_checksum (consumer/processor in)

    /// Pointer to the hub::Producer (for connected_consumers, send, broadcast).
    /// Opaque to the engine — cast in engine-specific API builders.
    void *producer{nullptr};

    /// Pointer to the hub::Consumer (for consumer-specific API).
    void *consumer{nullptr};

    /// Pointer to RoleHostCore — single source of truth for shutdown flags
    /// AND metrics (out_written, in_received, drops, script_errors, etc.).
    /// All engines read metrics from core->out_written_.load() etc.
    RoleHostCore *core{nullptr};

    bool stop_on_script_error{false};
};

// ============================================================================
// ScriptEngine — abstract interface
// ============================================================================

class ScriptEngine
{
  public:
    virtual ~ScriptEngine() = default;

    // ── Lifecycle (called from working thread) ───────────────────────────

    /**
     * @brief Create the interpreter/state and apply sandbox.
     * @param log_tag Tag for log messages (e.g., "prod").
     * @param core    Pointer to RoleHostCore — provides metrics counters
     *                (script_errors, etc.) and shutdown flags. Must remain
     *                valid for the engine's lifetime. Passed at init time
     *                so error counting works from the first load_script() call.
     * @return true on success.
     */
    virtual bool initialize(const char *log_tag, RoleHostCore *core) = 0;

    /**
     * @brief Load script file and extract callbacks.
     * @param script_dir Directory containing the script (e.g., .../script/lua/).
     * @param entry_point Script entry point filename (e.g., "init.lua", "__init__.py").
     *        Lua loads this file directly. Python uses package-based import derived
     *        from script_dir and ignores entry_point (always imports __init__.py).
     * @param required_callback Name of the required callback (e.g., "on_produce").
     * @return true if script loaded and required callback exists.
     */
    virtual bool load_script(const std::filesystem::path &script_dir,
                             const char *entry_point,
                             const char *required_callback) = 0;

    /**
     * @brief Build the API object/table using role-specific context.
     *
     * Called after load_script() and before invoke_on_init().  The engine
     * creates its API representation (Lua table or Python object) using
     * the pointers in ctx.
     */
    virtual void build_api(const RoleContext &ctx) = 0;

    /**
     * @brief Release all script objects, close interpreter/state.
     *
     * Called on the working thread after the data loop exits and
     * invoke_on_stop() has returned.  After this call, the engine
     * is in a destroyed state — no further calls are valid.
     */
    virtual void finalize() = 0;

    // ── Queries ──────────────────────────────────────────────────────────

    [[nodiscard]] virtual bool has_callback(const char *name) const = 0;

    // ── Schema / type building ───────────────────────────────────────────

    /**
     * @brief Register a slot type from SchemaSpec.
     * @param spec The schema specification.
     * @param type_name Name for the type (e.g., "SlotFrame", "FlexFrame").
     * @param packing "aligned" or "packed".
     * @return true on success.
     */
    virtual bool register_slot_type(const SchemaSpec &spec,
                                    const char *type_name,
                                    const std::string &packing) = 0;

    /**
     * @brief Query the size of a registered type.
     * @return Size in bytes, or 0 on error.
     */
    [[nodiscard]] virtual size_t type_sizeof(const char *type_name) const = 0;

    // ── Callback invocation (called from working thread) ─────────────────

    virtual void invoke_on_init() = 0;
    virtual void invoke_on_stop() = 0;

    /**
     * @brief Invoke on_produce(out_slot, flexzone, messages, api).
     * @param out_slot Writable slot buffer, or nullptr if acquire failed.
     * @param out_sz   Slot size in bytes.
     * @param flexzone Writable flexzone buffer, or nullptr if no flexzone.
     * @param fz_sz    Flexzone size in bytes.
     * @param fz_type  Flexzone type name, or nullptr.
     * @param msgs     Incoming messages (drained from RoleHostCore).
     * @return Commit, Discard, or Error.
     */
    virtual InvokeResult invoke_produce(
        void *out_slot, size_t out_sz,
        void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_consume(in_slot, flexzone, messages, api).
     */
    virtual void invoke_consume(
        const void *in_slot, size_t in_sz,
        const void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_process(in_slot, out_slot, flexzone, messages, api).
     */
    virtual InvokeResult invoke_process(
        const void *in_slot, size_t in_sz,
        void *out_slot, size_t out_sz,
        void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_inbox(slot_data, sender_uid, api).
     * @param data     Raw inbox payload.
     * @param sz       Payload size in bytes.
     * @param type_name FFI/ctypes type name for the inbox slot, or nullptr for raw bytes.
     * @param sender   Sender's pylabhub UID.
     */
    virtual void invoke_on_inbox(
        const void *data, size_t sz,
        const char *type_name,
        const char *sender) = 0;

    // ── Error state ──────────────────────────────────────────────────────

    [[nodiscard]] virtual uint64_t script_error_count() const noexcept = 0;

    // ── Threading capability ─────────────────────────────────────────────

    /**
     * @brief True if the engine supports independent states on other threads.
     *
     * When true, the framework may create secondary engine states for
     * ctrl_thread_ (on_heartbeat), the main loop (admin requests), or
     * user-spawned threads (api.spawn_thread).
     *
     * When false, ALL script invocations must happen on the working thread.
     */
    [[nodiscard]] virtual bool supports_multi_state() const noexcept = 0;

    /**
     * @brief Create an independent engine state for use on another thread.
     *
     * Loads the same script, but operates on a separate interpreter/state
     * with no shared script objects.  Data sharing goes through C++.
     *
     * @return New engine instance, or nullptr if !supports_multi_state().
     */
    virtual std::unique_ptr<ScriptEngine> create_thread_state() = 0;

    // ── Script reload (future) ────────────────────────────────────────────

    /**
     * @brief Reload the script from disk without destroying engine state.
     *
     * Re-executes the script file and re-extracts callback function references.
     * The C++ infrastructure (queues, messenger, API table, FFI/ctypes types)
     * is NOT rebuilt — only the script's function bodies are updated.
     *
     * ## Semantics
     *
     * - Called on the working thread, between data loop cycles.
     * - The engine re-loads the same file path used in load_script().
     * - Old callback refs are released; new ones extracted from the re-executed script.
     * - Cached FFI/ctypes type objects remain valid (schema is immutable).
     * - The API table/object remains valid (C++ pointers unchanged).
     * - Script-side global state: preserved or reset depending on what the
     *   new script file does. This is the script author's responsibility.
     *
     * ## Error handling
     *
     * If the new script has syntax errors or is missing the required callback:
     * - Returns false.
     * - The old callbacks remain active (the engine continues with the
     *   previous version of the script).
     * - The error is logged.
     *
     * ## Trigger mechanism (not yet defined)
     *
     * How reload is triggered is a protocol-level decision:
     * - `api.reload_script()` called from within the script itself
     * - External control message via inbox or broker command
     * - File-system watcher detecting script modification
     * - Interactive signal (e.g., SIGUSR1)
     *
     * The trigger mechanism and the protocol for coordinating reload across
     * a pipeline (e.g., reload producer script without disrupting consumers)
     * are deferred to a future HEP.
     *
     * ## Implementation notes
     *
     * - Lua: `loadfile(path) + pcall()` on the existing lua_State.
     *   Overwrites global functions. No state destruction.
     * - Python: `importlib.reload(module)`. Re-executes the module.
     *   Existing py::objects for callbacks are replaced.
     *
     * @return true if reload succeeded, false on error (old script remains active).
     */
    virtual bool reload_script() { return false; } // default: not implemented
};

} // namespace pylabhub::scripting
