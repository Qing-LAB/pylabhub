#pragma once
/**
 * @file python_engine.hpp
 * @brief PythonEngine — ScriptEngine implementation for CPython via pybind11.
 *
 * Key design differences from LuaEngine (see HEP-CORE-0011 and
 * docs/tech_draft/engine_thread_model.md):
 *   - Uses existing pybind11 ProducerAPI/ConsumerAPI/ProcessorAPI classes
 *     (compile-time type safety at the C++ boundary)
 *   - GIL held for engine lifetime on worker thread; per-invoke acquire is
 *     reentrant no-op. Cross-thread access uses engine request queue
 *     (see docs/tech_draft/engine_thread_model.md §6)
 *   - py::scoped_interpreter as member (no dedicated interpreter thread)
 *   - Slot views via ctypes.from_buffer(memoryview) — pre-built at init
 *   - Single-interpreter (supports_multi_state = false)
 *
 * ## Lifetime contract
 *
 * All methods are called from the working thread. The interpreter is created
 * in initialize() and destroyed in finalize(). All py::object members must
 * be released before finalize() destroys the interpreter.
 *
 * The API object (ProducerAPI/ConsumerAPI/ProcessorAPI) holds raw pointers
 * to C++ infrastructure. finalize() nulls these pointers before the role
 * host tears down infrastructure.
 */

#include "utils/script_engine.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/embed.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

// Forward declarations for role API classes.
namespace pylabhub::producer { class ProducerAPI; }
namespace pylabhub::consumer { class ConsumerAPI; }
namespace pylabhub::processor { class ProcessorAPI; }
// Forward declaration for HubAPI — full type only needed in
// python_engine.cpp (build_api_(HubAPI&) impl + py::cast).
namespace pylabhub::hub_host  { class HubAPI; }

namespace pylabhub::scripting
{

class PythonEngine : public ScriptEngine
{
  public:
    PythonEngine();
    ~PythonEngine() override;

    // Non-copyable, non-movable.
    PythonEngine(const PythonEngine &) = delete;
    PythonEngine &operator=(const PythonEngine &) = delete;
    PythonEngine(PythonEngine &&) = delete;
    PythonEngine &operator=(PythonEngine &&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// Set the Python virtual environment name before initialize().
    /// Empty = use base environment (default).
    void set_python_venv(const std::string &venv) { python_venv_ = venv; }

    /// Set the GIL-release-during-wait flag before initialize().
    /// True → the worker loop releases the GIL across each idle wait
    /// (queue read, deadline sleep, hub event wait) so cooperative
    /// sub-threads on the same Python interpreter can run.  See
    /// `ScriptEngine::release_global_lock_during_wait()` for full semantics.
    void set_release_global_lock_during_wait(bool v) noexcept
    {
        release_global_lock_during_wait_ = v;
    }

    [[nodiscard]] bool release_global_lock_during_wait() const noexcept override
    {
        return release_global_lock_during_wait_;
    }

    bool init_engine_(const std::string &log_tag, RoleHostCore *core) override;
    bool load_script(const std::filesystem::path &script_dir,
                     const std::string &entry_point,
                     const std::string &required_callback) override;
    bool build_api_(RoleAPIBase &api) override;
    /// Hub-side build_api override (HEP-CORE-0033 Phase 7 D4.1).
    /// Imports `pylabhub_hub` (registered by hub_api_python.cpp via
    /// PYBIND11_EMBEDDED_MODULE), wraps the HubAPI in a py::object,
    /// stores it in `api_obj_` for `invoke_on_init` / `invoke_on_stop`
    /// (which pass `api` as a positional arg), and ALSO sets it as
    /// `<script_module>.api` so event/tick callbacks dispatched via
    /// generic `engine.invoke(name, args)` (which doesn't pass api)
    /// can still reach it via module-global lookup.  Mirrors the Lua
    /// dual-exposure pattern (registry ref + Lua global).
    bool build_api_(::pylabhub::hub_host::HubAPI &api) override;
    void finalize_engine_() override;

    // ── Queries ────────────────────────────────────────────────────────────
    //
    // `has_callback` is inherited from `ScriptEngine` (HEP-CORE-0011
    // §"Engine Thread Affinity"; Tier 1 standard cache).  Populated by
    // `load_script()` below via
    // `ScriptEngine::set_standard_callback_present()`; read from any
    // thread thereafter via the base-class lookup.  This subclass
    // additionally provides arbitrary-name probing via the
    // `probe_uncached_callback_` hook — but ONLY when the caller is
    // on the engine's owner thread (= worker, holding the GIL); see
    // the override below for the contract.

  protected:
    [[nodiscard]] bool probe_uncached_callback_(const std::string &name)
        const noexcept override;

  public:

    // ── Schema / type building ─────────────────────────────────────────────

    bool register_slot_type(const hub::SchemaSpec &spec,
                            const std::string &type_name,
                            const std::string &packing) override;
    [[nodiscard]] size_t type_sizeof(const std::string &type_name) const override;

    // ── Callback invocation ────────────────────────────────────────────────

    void invoke_on_init() override;
    void invoke_on_stop() override;
    void invoke_on_channel_closing(const std::string &channel,
                                    const std::string &reason) override;
    void invoke_on_consumer_died(const std::string &channel,
                                  const std::string &consumer_uid,
                                  const std::string &reason) override;
    void invoke_on_hub_dead(const std::string &source_hub_uid) override;
    void invoke_on_band_member_joined(const std::string &band,
                                      const std::string &role_uid,
                                      const std::string &role_name) override;
    void invoke_on_band_member_left(const std::string &band,
                                    const std::string &role_uid,
                                    const std::string &reason) override;
    void invoke_on_band_message(const std::string &band,
                                const std::string &sender_role_uid,
                                const nlohmann::json &body) override;
    void invoke_on_band_lost(const std::string &band,
                             const std::string &reason) override;
    void invoke_on_allowlist_changed(
        const std::string &channel,
        const std::vector<AllowedPeer> &allowlist,
        const std::string &reason) override;

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

    // ── Generic invoke (thread-safe) ─────────────────────────────────────

    bool invoke(const std::string &name) override;
    bool invoke(const std::string &name, const nlohmann::json &args) override;
    InvokeResponse eval(const std::string &code) override;
    InvokeResponse invoke_returning(const std::string &name,
                                    const nlohmann::json &args,
                                    int64_t timeout_ms = -1) override;
    void process_pending() override;
    size_t pending_script_engine_request_count() const noexcept override;

    // ── Error state ────────────────────────────────────────────────────────

    // Body in python_engine.cpp — needs full HubAPI type to call
    // hub_api_->core(); this header only forward-declares HubAPI.
    [[nodiscard]] uint64_t script_error_count() const noexcept override;

    // ── Threading ──────────────────────────────────────────────────────────

    [[nodiscard]] bool supports_multi_state() const noexcept override { return false; }

  private:
    // ── Interpreter ownership (HEP-CORE-0011 §"Engine Construction
    //                            Lifecycle", Option E) ──────────────────────
    //
    // The embedded CPython interpreter is owned by the BINARY'S MAIN
    // THREAD via the `pylabhub::scripting::PythonInterpreter` dynamic
    // lifecycle module.  PythonEngine does NOT refcount the interpreter
    // and does NOT carry an interpreter-scope member.  The contract is:
    //
    //   1. main() (or test fixture's SetUpTestSuite) calls
    //      `ensure_python_interpreter_loaded()` ONCE on its own thread
    //      after config parse.  pi_startup runs Py_InitializeFromConfig
    //      on main and releases the GIL via a stored
    //      py::gil_scoped_release.
    //   2. Worker threads acquire the GIL with `py::gil_scoped_acquire`
    //      at the top of `worker_main_`, BEFORE constructing PythonEngine.
    //   3. PythonEngine ctor runs on the worker (which now holds the GIL),
    //      so the `py::object{py::none()}` member-default-initializers
    //      below execute under GIL safely.  ctor body calls
    //      `validate_python_interpreter()` as defence-in-depth and
    //      PLH_PANICs on failure.
    //   4. ~PythonEngine may run on any thread (often main, via HubHost
    //      destruction).  Cross-thread destruction is made safe by the
    //      detach pattern in `clear_pyobjects_` (run from `finalize_engine_`
    //      on the worker under GIL): each py::object is reset to
    //      py::none() AND released so its m_ptr becomes nullptr.  The
    //      eventual ~py::object is then a no-op on any thread.

    // ── Script ─────────────────────────────────────────────────────────────
    std::string log_tag_;
    std::string entry_point_;
    std::string required_callback_;
    std::string python_venv_;     ///< Optional venv name.

    /// Mirror of `script.release_global_lock_during_wait` from config.
    /// Read by the worker loop frame ONCE at worker startup (cached as a
    /// local bool) — never per iteration.  See HEP-CORE-0011 §"Engine
    /// Thread Affinity" sub-section "Optional global-lock release
    /// during idle waits."
    bool        release_global_lock_during_wait_{false};

    py::object module_{py::none()};
    py::object py_on_init_{py::none()};
    py::object py_on_stop_{py::none()};
    py::object py_on_channel_closing_{py::none()};
    py::object py_on_consumer_died_{py::none()};
    py::object py_on_hub_dead_{py::none()};
    // S4 expansion 2026-05-19 — typed band callbacks (HEP-CORE-0030 §5.3).
    py::object py_on_band_member_joined_{py::none()};
    py::object py_on_band_member_left_{py::none()};
    py::object py_on_band_message_{py::none()};
    py::object py_on_band_lost_{py::none()};
    py::object py_on_allowlist_changed_{py::none()};
    py::object py_on_produce_{py::none()};
    py::object py_on_consume_{py::none()};
    py::object py_on_process_{py::none()};
    py::object py_on_inbox_{py::none()};
    // Standard callback presence cache lives on `ScriptEngine` (HEP-CORE-0011
    // §"Engine Thread Affinity"; populated by `load_script()` via
    // `set_standard_callback_present(...)` + `freeze_standard_callback_cache()`).
    // `has_callback()` reads from there — no per-PythonEngine duplicate state.

    // ── API object (one of these is active based on role) ──────────────────
    py::object api_obj_{py::none()};


    /// Role-specific API impl. Exactly one is non-null after build_api().
    /// All 3 API classes are compiled into pylabhub-scripting (shared lib),
    /// so there is no link dependency issue.
    std::unique_ptr<producer::ProducerAPI>   producer_api_;
    std::unique_ptr<consumer::ConsumerAPI>   consumer_api_;
    std::unique_ptr<processor::ProcessorAPI> processor_api_;

    // ── Cached type objects (init-time, for hot-path slot views) ───────────
    // Directional — always set by directional type name registration.
    py::object in_slot_type_ro_{py::none()};   ///< InSlotFrame readonly (consumer, processor)
    py::object out_slot_type_{py::none()};     ///< OutSlotFrame writable (producer, processor)
    py::object in_fz_type_{py::none()};        ///< InFlexFrame (mutable, consumer/processor)
    py::object out_fz_type_{py::none()};       ///< OutFlexFrame (mutable, producer/processor)
    py::object inbox_type_ro_{py::none()};     ///< InboxFrame (read-only, from_buffer_copy)

    // Script-level aliases for producer/consumer convenience.
    // Set when only one direction is registered (not processor).
    // Producer: slot_alias_ = out_slot_type_, fz_alias_ = out_fz_type_
    // Consumer: slot_alias_ro_ = in_slot_type_ro_, fz_alias_ = in_fz_type_
    py::object slot_alias_{py::none()};        ///< "SlotFrame" writable (producer alias)
    py::object slot_alias_ro_{py::none()};     ///< "SlotFrame" readonly (consumer alias)
    py::object fz_alias_{py::none()};          ///< "FlexFrame" alias (either direction)

    hub::SchemaSpec in_slot_spec_;
    hub::SchemaSpec out_slot_spec_;
    hub::SchemaSpec in_fz_spec_;
    hub::SchemaSpec out_fz_spec_;
    hub::SchemaSpec slot_alias_spec_;               ///< Alias spec (points to whichever direction)
    hub::SchemaSpec fz_alias_spec_;                 ///< Alias spec
    hub::SchemaSpec inbox_spec_;

    // script_error_count is in api_->core()->script_error_count_ (RoleHostCore).

    // GIL stays held on the worker thread (py::scoped_interpreter holds it).
    // Each invoke_*() uses py::gil_scoped_acquire which is reentrant (no-op).

    // api_ is inherited from ScriptEngine (set by build_api).
    bool stop_on_script_error_{false};

    // ── Generic invoke queue (§4/§6 of engine_thread_model.md) ────────────
    /// Discriminator for request_queue_ entries.  Mirrors the three
    /// public entry points (`invoke`, `eval`, `invoke_returning`).  The
    /// "Returning" variant captures the script function's return value
    /// into InvokeResponse::value (HEP-CORE-0033 §12.2.2 augmentation
    /// path) — the other two ignore it.
    enum class RequestKind : uint8_t
    {
        Invoke,            ///< plain `invoke()` — bool result, value ignored
        InvokeReturning,   ///< `invoke_returning()` — return value captured
        Eval,              ///< `eval(code)` — value is `eval_direct_` result
    };
    struct PendingRequest
    {
        std::string                      name;
        nlohmann::json                   args;
        RequestKind                      kind{RequestKind::Invoke};
        std::promise<InvokeResponse>     promise;
    };
    std::deque<PendingRequest>  request_queue_;
    mutable std::mutex          queue_mu_;
    // accepting_ is inherited from ScriptEngine base class.

    InvokeResponse execute_direct_(const std::string &name);
    InvokeResponse execute_direct_(const std::string &name, const nlohmann::json &args);
    /// `invoke_returning` direct path — same call shape as
    /// `execute_direct_(name, args)` but captures the function's return
    /// value into the response.
    InvokeResponse execute_direct_returning_(const std::string &name,
                                             const nlohmann::json &args);
    InvokeResponse eval_direct_(const std::string &code);
    void process_pending_();

    // ── Internal helpers ───────────────────────────────────────────────────

    /// Build a ctypes.Structure subclass from hub::SchemaSpec.
    py::object build_ctypes_type_(const hub::SchemaSpec &spec, const std::string &name,
                                   const std::string &packing);

    /// Wrap a type as read-only (adds __setattr__ override).
    py::object wrap_readonly_(const py::object &type);

    /// Create a slot view from raw pointer using pre-built type.
    py::object make_slot_view_(const hub::SchemaSpec &spec, const py::object &type,
                                void *data, size_t size, bool readonly);

    /// Build the messages list (producer/processor format).
    py::list build_messages_list_(std::vector<IncomingMessage> &msgs);

    /// Build the messages list (consumer format — bare bytes for data).
    py::list build_messages_list_bare_(std::vector<IncomingMessage> &msgs);

    /// Parse on_produce/on_process return value.
    /// Strict contract: true=Commit, false=Discard, None/other=Error.
    InvokeResult parse_return_value_(const py::object &ret, const char *callback_name);

    /// Handle a Python exception from pcall.
    InvokeResult on_python_error_(const char *callback_name, const py::error_already_set &e);

    /// Central error-dispatch for non-exception script errors (wrong
    /// return value, config error, etc.).  Bumps script_error_count
    /// and honors stop_on_script_error_ consistently with
    /// on_python_error_.  Caller logs the ERROR diagnostic first.
    InvokeResult handle_script_error_(const char *callback_tag);

    /// Clear all py::object members (must be called with GIL held, before Py_Finalize).
    void clear_pyobjects_();
};

} // namespace pylabhub::scripting
