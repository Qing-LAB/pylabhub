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
 *   - PythonEngine:  wraps py::scoped_interpreter (GIL held for lifetime; per-invoke acquire is no-op)
 *
 * See docs/tech_draft/script_engine_refactor.md for the full design.
 */

#include "role_host_core.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pylabhub::hub { class Messenger; class Producer; class Consumer; class InboxQueue; }
#include "utils/data_block_policy.hpp" // ChecksumPolicy enum

namespace pylabhub::scripting
{

// Maximum recursion depth for JSON ↔ script type conversion.
// Set via CMake option PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH (default: 20).
#ifndef PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH
#define PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH 20
#endif
inline constexpr int kScriptMaxRecursionDepth = PYLABHUB_SCRIPT_MAX_RECURSION_DEPTH;

// ============================================================================
// InvokeResult — return value from hot-path on_produce / on_process
// ============================================================================

enum class InvokeResult
{
    Commit,   ///< Script returned true/nil — publish the slot.
    Discard,  ///< Script returned false — discard the slot, loop continues.
    Error,    ///< Script raised an error — discard, increment error count.
};

// ============================================================================
// InvokeStatus — mechanical result of a generic invoke() / eval() call
// ============================================================================

/// Distinct from InvokeResult which is the semantic data-callback result.
enum class InvokeStatus : uint8_t
{
    Ok              = 0,   ///< Function executed successfully.
    NotFound        = 1,   ///< Function name not found in script.
    ScriptError     = 2,   ///< Script raised an exception.
    EngineShutdown  = 3,   ///< Engine is finalizing; request rejected.
};

/// Response from a generic invoke() or eval() call.
struct InvokeResponse
{
    InvokeStatus   status{InvokeStatus::Ok};
    nlohmann::json value;   ///< Populated only for eval(); empty for invoke().
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
    std::string role_tag;              ///< "prod", "cons", "proc"
    std::string uid;
    std::string name;
    std::string channel;               ///< Primary channel (or in_channel for processor)
    std::string out_channel;           ///< out_channel for processor (empty for producer/consumer)
    std::string log_level;
    std::string script_dir;
    std::string role_dir;

    hub::Messenger   *messenger{nullptr};     ///< For open_inbox, wait_for_role, broadcast, send

    hub::Producer   *producer{nullptr};      ///< Output data queue ops.
    hub::Consumer   *consumer{nullptr};      ///< Input data queue ops.
    hub::InboxQueue *inbox_queue{nullptr};   ///< Incoming peer messages (ROUTER). nullptr if no inbox.
    hub::ChecksumPolicy checksum_policy{hub::ChecksumPolicy::Enforced}; ///< Per-role checksum policy.

    /// Pointer to RoleHostCore — single source of truth for shutdown flags
    /// AND metrics (out_written, in_received, drops, script_errors, etc.).
    RoleHostCore *core{nullptr};

    bool stop_on_script_error{false};
};

// ============================================================================
// InvokeRx / InvokeTx — directional data for invoke calls
// ============================================================================

/// Read-only input direction — data received from upstream.
/// Slot is const (system-managed via SlotRWState). Flexzone is mutable
/// (user-managed coordination via SharedSpinLock / atomics per HEP-0002).
/// Passed by value (32 bytes, fits in registers on x86-64).
struct InvokeRx
{
    const void *slot{nullptr};    ///< Read-only input slot.
    size_t      slot_size{0};
    void       *fz{nullptr};      ///< Mutable flexzone (bidirectional by design).
    size_t      fz_size{0};
};

/// Writable output direction — data going downstream.
struct InvokeTx
{
    void  *slot{nullptr};         ///< Writable output slot.
    size_t slot_size{0};
    void  *fz{nullptr};           ///< Mutable flexzone.
    size_t fz_size{0};
};

/// Inbox message — one-shot peer-to-peer delivery (no flexzone, no ring buffer).
struct InvokeInbox
{
    const void *data{nullptr};    ///< Typed payload (valid until next recv_one()).
    size_t      data_size{0};
    std::string sender_uid;       ///< Sender's role UID.
    uint64_t    seq{0};           ///< Sender's monotonic sequence number.
};

// ============================================================================
// ScriptEngine — abstract interface
// ============================================================================

class ScriptEngine
{
  public:
    virtual ~ScriptEngine() = default;

    // ── Engine state machine ──────────────────────────────────────────────

    enum class EngineState : uint8_t
    {
        Unloaded,
        Initialized,
        ScriptLoaded,
        ApiBuilt,
        Finalized
    };

    [[nodiscard]] EngineState engine_state() const noexcept { return state_; }

    // ── Lifecycle module support ────────────────────────────────────────

    /// Magic number for lifecycle validation.
    static constexpr uint64_t kLifecycleMagic = 0x534345'4E47494E45ULL; // "SCENGINE"

    /// Lifecycle generation key — set by whoever registers this engine
    /// as a lifecycle module. Used by validate function to detect stale pointers.
    uint64_t lifecycle_key_{0};
    uint64_t lifecycle_magic_{kLifecycleMagic};

    /// Validation function for lifecycle userdata.
    static bool lifecycle_validate(void *userdata, uint64_t key)
    {
        auto *eng = static_cast<ScriptEngine *>(userdata);
        return eng && eng->lifecycle_magic_ == kLifecycleMagic
                   && eng->lifecycle_key_ == key;
    }

    // ── Lifecycle (called from working thread) ───────────────────────────

    /**
     * @brief Create the interpreter/state and apply sandbox.
     *
     * Sets owner_thread_id_ and ctx_.core. On success, sets accepting_=true.
     * On failure, accepting_ stays false. Calls init_engine_() for specifics.
     *
     * @param log_tag Tag for log messages (e.g., "prod").
     * @param core    Pointer to RoleHostCore — provides metrics counters
     *                (script_errors, etc.) and shutdown flags. Must remain
     *                valid for the engine's lifetime.
     * @return true on success.
     */
    bool initialize(const std::string &log_tag, RoleHostCore *core)
    {
        owner_thread_id_ = std::this_thread::get_id();
        ctx_.core = core;
        // accepting_ stays false until build_api() succeeds.
        if (!init_engine_(log_tag, core))
            return false;
        state_ = EngineState::Initialized;
        return true;
    }

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
                             const std::string &entry_point,
                             const std::string &required_callback) = 0;

    /**
     * @brief Build the API object/table using role-specific context.
     *
     * Called after load_script() and before invoke_on_init().  Sets ctx_
     * on the base class, calls engine-specific build_api_(), then sets
     * accepting_=true on success. Returns false if build_api_() fails
     * (e.g. unknown role_tag) — engine remains non-accepting.
     */
    [[nodiscard]] bool build_api(const RoleContext &ctx)
    {
        auto *saved_core = ctx_.core; // preserve core from initialize()
        ctx_ = ctx;
        if (ctx_.core == nullptr)
            ctx_.core = saved_core;
        if (!build_api_(ctx))
            return false;
        state_ = EngineState::ApiBuilt;
        accepting_.store(true, std::memory_order_release);
        return true;
    }

    /**
     * @brief Stop accepting new invoke() requests from non-owner threads.
     *
     * Called by the role host before invoke_on_stop(). After this call,
     * all non-owner invoke() calls return false immediately (including
     * child engines that check the owner's flag). Owner-thread hot-path
     * methods (invoke_on_stop, invoke_produce, etc.) still work.
     */
    void stop_accepting() noexcept { accepting_.store(false, std::memory_order_release); }

    /**
     * @brief Check if the engine is accepting invoke() requests.
     *
     * Child engines delegate to the owner's flag. Owner engines check their own.
     * All invoke()/eval() paths call this before executing.
     */
    [[nodiscard]] bool is_accepting() const noexcept
    {
        if (owner_engine_)
            return owner_engine_->is_accepting();
        return accepting_.load(std::memory_order_acquire);
    }

    /**
     * @brief Set the owner engine pointer (for child/thread-local states).
     * Child engines delegate is_accepting() to the owner.
     */
    void set_owner_engine(ScriptEngine *owner) noexcept { owner_engine_ = owner; }

    /**
     * @brief Release all script objects, close interpreter/state.
     *
     * Sets accepting_=false, then calls engine-specific finalize_engine_().
     * Called on the owner thread after invoke_on_stop() has returned.
     * After this call, the engine is in a destroyed state — no further
     * calls are valid. Must be called BEFORE infrastructure teardown.
     */
    void finalize()
    {
        if (state_ == EngineState::Finalized)
            return; // idempotent
        stop_accepting();
        finalize_engine_(); // Engine guards against double-finalize internally.
        state_ = EngineState::Finalized;
    }

    // ── Queries ──────────────────────────────────────────────────────────

    [[nodiscard]] virtual bool has_callback(const std::string &name) const = 0;

    // ── Schema / type building ───────────────────────────────────────────

    /**
     * @brief Register a slot type from SchemaSpec.
     *
     * Directional type names (registered by all roles):
     *   "InSlotFrame"  — input slot (readonly)
     *   "OutSlotFrame" — output slot (writable)
     *   "InFlexFrame"  — input flexzone (mutable, HEP-0002)
     *   "OutFlexFrame" — output flexzone (mutable)
     *   "InboxFrame"   — inbox (readonly; uses from_buffer_copy for Python)
     *
     * Engines create "SlotFrame"/"FlexFrame" aliases for single-direction roles
     * (producer/consumer). Processor has both directions, no aliases.
     *
     * All types MUST be cached by the engine for hot-path use.
     * invoke_on_inbox() requires the cached type — null cache is an error.
     *
     * @param spec The schema specification.
     * @param type_name Name for the type.
     * @param packing "aligned" or "packed".
     * @return true on success.
     */
    virtual bool register_slot_type(const SchemaSpec &spec,
                                    const std::string &type_name,
                                    const std::string &packing) = 0;

    /**
     * @brief Query the size of a registered type.
     * @return Size in bytes, or 0 on error.
     */
    [[nodiscard]] virtual size_t type_sizeof(const std::string &type_name) const = 0;

    // ── Generic invoke (thread-safe — engine handles internal locking) ────

    /**
     * @brief Invoke a named script function with no arguments.
     *
     * Safe to call from any thread. The engine handles all internal locking,
     * queue management, and thread dispatch. Owner thread gets priority.
     * @return false if function not found, engine shutting down, or error.
     */
    virtual bool invoke(const std::string &name) = 0;

    /**
     * @brief Invoke a named script function with JSON arguments.
     *
     * The engine unpacks args into the script's native format (Python dict,
     * Lua table) before calling. Same thread-safety contract as invoke(name).
     */
    virtual bool invoke(const std::string &name, const nlohmann::json &args) = 0;

    /**
     * @brief Evaluate a script code string. For admin/debug use.
     *
     * Same thread-safety contract as invoke(). Returns InvokeResponse with
     * status (Ok/ScriptError/EngineShutdown) and value (JSON result or empty).
     */
    virtual InvokeResponse eval(const std::string &code) = 0;

    // ── Hot-path invocation (owner thread — data loop) ──────────────────

    virtual void invoke_on_init() = 0;
    virtual void invoke_on_stop() = 0;

    /**
     * @brief Invoke on_produce(tx, msgs, api).
     * @param tx   Output direction (writable slot + flexzone).
     * @param msgs Incoming messages (drained from RoleHostCore).
     * @return Commit, Discard, or Error.
     */
    virtual InvokeResult invoke_produce(
        InvokeTx tx,
        std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_consume(rx, msgs, api).
     * @param rx   Input direction (read-only slot + flexzone).
     * @param msgs Incoming messages (drained from RoleHostCore).
     * @return Commit, Discard, or Error. Currently ignored by the consumer
     *         data loop — reserved for future flow control.
     */
    virtual InvokeResult invoke_consume(
        InvokeRx rx,
        std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_process(rx, tx, msgs, api).
     *
     * Each direction pairs a slot with its flexzone. Either flexzone may
     * be nullptr if not configured.
     *
     * @param rx   Input direction (read-only slot + flexzone from upstream).
     * @param tx   Output direction (writable slot + flexzone to downstream).
     * @param msgs Incoming messages (drained from RoleHostCore).
     * @return Commit, Discard, or Error.
     */
    virtual InvokeResult invoke_process(
        InvokeRx rx, InvokeTx tx,
        std::vector<IncomingMessage> &msgs) = 0;

    /**
     * @brief Invoke on_inbox(msg, api).
     *
     * @param msg  Inbox message with typed payload, sender UID, and sequence.
     * @return Commit, Discard, or Error. Currently ignored by the inbox loop.
     *
     * ## Invariants
     *
     * - Inbox only exists when inbox_schema is configured (no schemaless inbox).
     * - "InboxFrame" type is registered via register_slot_type() at startup.
     * - Data lifetime: valid only until the next InboxQueue::recv_one() call.
     *   Python must use from_buffer_copy (not from_buffer). Lua ffi.cast is
     *   safe because the callback completes before the next recv_one().
     */
    virtual InvokeResult invoke_on_inbox(InvokeInbox msg) = 0;

    // ── Error state ──────────────────────────────────────────────────────

    [[nodiscard]] virtual uint64_t script_error_count() const noexcept = 0;

    // ── Threading capability ─────────────────────────────────────────────

    /**
     * @brief True if the engine handles multi-threaded invoke() internally.
     *
     * When true (Lua): non-owner threads get independent thread-local states.
     * When false (Python): non-owner requests are queued to the owner thread.
     * The engine manages threading internally — callers just call invoke().
     */
    [[nodiscard]] virtual bool supports_multi_state() const noexcept = 0;

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

    // ── Thread lifecycle ─────────────────────────────────────────────────

    /**
     * @brief Release any thread-local resources for the calling thread.
     *
     * Called when a non-owner thread is done using this engine. The engine
     * cleans up thread-specific state:
     *   - Lua: destroys the thread-local LuaState from the cache
     *   - Python: no-op (single-state, no per-thread resources)
     *   - Native: no-op (native engine manages its own threads)
     *
     * Safe to call from any thread. No-op if the thread has no state.
     */
    virtual void release_thread() {}

    // ── Inbox client management (shared via RoleHostCore) ────────────────

    /// Result from open_inbox_client() — everything an engine needs to build
    /// its script-native inbox handle.
    struct InboxOpenResult
    {
        std::shared_ptr<hub::InboxClient> client;
        SchemaSpec spec;        ///< Parsed schema (for FFI cdef / ctypes struct building).
        std::string packing;    ///< "aligned" or "packed".
        size_t item_size{0};    ///< Size of one inbox slot in bytes.
    };

    /**
     * @brief Open (or reuse) an InboxClient connection to the given target.
     *
     * Handles broker discovery, schema parsing, InboxClient creation, and
     * core_ cache management. Returns everything the engine needs to build
     * its script-native handle (FFI type for Lua, ctypes struct for Python).
     *
     * @param target_uid  The target role's UID.
     * @return InboxOpenResult, or nullopt on failure.
     */
    std::optional<InboxOpenResult> open_inbox_client(const std::string &target_uid);

  protected:
    // ── Engine-specific overrides (Template Method pattern) ──────────────

    /// Create the interpreter/state. Called by initialize().
    virtual bool init_engine_(const std::string &log_tag, RoleHostCore *core) = 0;

    /// Build engine-specific API (Lua table / Python pybind11). Called by build_api().
    /// Returns true on success. If false, build_api() does NOT set accepting_=true.
    virtual bool build_api_(const RoleContext &ctx) = 0;

    /// Release all script objects, close interpreter. Called by finalize().
    virtual void finalize_engine_() = 0;

    /// Thread that called initialize(). Engines use this to detect whether
    /// invoke() is called from the owner (hot path) or another thread.
    std::thread::id owner_thread_id_;

    /// Context captured at build_api() — provides core_, messenger, identity.
    /// Used by open_inbox_client() for broker queries and cache access.
    RoleContext ctx_{};

  private:
    /// Accepting flag — true only between successful build_api() and stop_accepting().
    /// Child engines delegate to owner's flag via owner_engine_ pointer.
    std::atomic<bool> accepting_{false};

    /// Non-null for child engines (thread-local states). Points to the owner
    /// engine that owns the lifecycle. is_accepting() checks owner's flag.
    ScriptEngine *owner_engine_{nullptr};

    EngineState state_{EngineState::Unloaded};
};

/**
 * @brief RAII guard for non-owner thread engine lifecycle.
 *
 * Construct at thread start, destructor calls engine.release_thread()
 * to clean up thread-local resources (Lua thread-state, etc.).
 */
class ThreadEngineGuard
{
  public:
    explicit ThreadEngineGuard(ScriptEngine &engine) : engine_(engine) {}
    ~ThreadEngineGuard() { engine_.release_thread(); }

    ThreadEngineGuard(const ThreadEngineGuard &) = delete;
    ThreadEngineGuard &operator=(const ThreadEngineGuard &) = delete;

  private:
    ScriptEngine &engine_;
};

} // namespace pylabhub::scripting
