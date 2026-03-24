#pragma once
/**
 * @file python_engine.hpp
 * @brief PythonEngine — ScriptEngine implementation for CPython via pybind11.
 *
 * Key design differences from LuaEngine (see script_engine_refactor.md §5.4):
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
#include <vector>

namespace py = pybind11;

// Forward declarations for role API classes.
namespace pylabhub::producer { class ProducerAPI; }
namespace pylabhub::consumer { class ConsumerAPI; }
namespace pylabhub::processor { class ProcessorAPI; }

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

    bool initialize(const char *log_tag, RoleHostCore *core) override;
    bool load_script(const std::filesystem::path &script_dir,
                     const char *entry_point,
                     const char *required_callback) override;
    void build_api(const RoleContext &ctx) override;
    void finalize() override;

    // ── Queries ────────────────────────────────────────────────────────────

    [[nodiscard]] bool has_callback(const char *name) const override;

    // ── Schema / type building ─────────────────────────────────────────────

    bool register_slot_type(const SchemaSpec &spec,
                            const char *type_name,
                            const std::string &packing) override;
    [[nodiscard]] size_t type_sizeof(const char *type_name) const override;

    // ── Callback invocation ────────────────────────────────────────────────

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

    // ── Generic invoke (thread-safe) ─────────────────────────────────────

    bool invoke(const char *name) override;
    bool invoke(const char *name, const nlohmann::json &args) override;
    nlohmann::json eval(const char *code) override;

    // ── Error state ────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept override
    {
        return ctx_.core->script_errors();
    }

    // ── Threading ──────────────────────────────────────────────────────────

    [[nodiscard]] bool supports_multi_state() const noexcept override { return false; }
    std::unique_ptr<ScriptEngine> create_thread_state() override { return nullptr; }

  private:
    // ── Interpreter ────────────────────────────────────────────────────────
    std::optional<py::scoped_interpreter> interp_;

    // ── Script ─────────────────────────────────────────────────────────────
    std::string log_tag_;
    std::string script_dir_str_;
    std::string entry_point_;
    std::string required_callback_;
    std::string python_venv_;     ///< Optional venv name.

    py::object module_{py::none()};
    py::object py_on_init_{py::none()};
    py::object py_on_stop_{py::none()};
    py::object py_on_produce_{py::none()};
    py::object py_on_consume_{py::none()};
    py::object py_on_process_{py::none()};
    py::object py_on_inbox_{py::none()};

    // ── API object (one of these is active based on role) ──────────────────
    py::object api_obj_{py::none()};

    /// Role-specific API impl. Exactly one is non-null after build_api().
    /// All 3 API classes are compiled into pylabhub-scripting (shared lib),
    /// so there is no link dependency issue.
    std::unique_ptr<producer::ProducerAPI>   producer_api_;
    std::unique_ptr<consumer::ConsumerAPI>   consumer_api_;
    std::unique_ptr<processor::ProcessorAPI> processor_api_;

    // ── Cached type objects (init-time, for hot-path slot views) ───────────
    py::object slot_type_{py::none()};       ///< ctypes.Structure subclass for primary slot
    py::object slot_type_ro_{py::none()};    ///< Read-only wrapper for consumer input
    py::object in_slot_type_ro_{py::none()}; ///< Processor input (read-only)
    py::object out_slot_type_{py::none()};   ///< Processor output (writable)
    py::object fz_type_{py::none()};         ///< Flexzone type (writable)
    py::object fz_type_ro_{py::none()};      ///< Flexzone type (read-only, consumer)

    SchemaSpec slot_spec_;                    ///< For slot view creation mode
    SchemaSpec in_slot_spec_;
    SchemaSpec out_slot_spec_;
    SchemaSpec fz_spec_;

    // script_errors is in ctx_.core->script_errors_ (RoleHostCore).

    // GIL stays held on the worker thread (py::scoped_interpreter holds it).
    // Each invoke_*() uses py::gil_scoped_acquire which is reentrant (no-op).

    // ctx_ is inherited from ScriptEngine (set by build_api).
    bool stop_on_script_error_{false};

    // ── Generic invoke queue (§4/§6 of engine_thread_model.md) ────────────
    struct PendingRequest
    {
        std::string                      name;
        nlohmann::json                   args;
        bool                             is_eval{false};
        std::promise<InvokeResponse>     promise;
    };
    std::deque<PendingRequest>  request_queue_;
    std::mutex                  queue_mu_;
    // accepting_ is inherited from ScriptEngine base class.
    std::atomic<bool>           executing_{false};

    InvokeResponse execute_direct_(const char *name);
    InvokeResponse execute_direct_(const char *name, const nlohmann::json &args);
    nlohmann::json eval_direct_(const char *code);
    void process_pending_();

    // ── Internal helpers ───────────────────────────────────────────────────

    /// Build a ctypes.Structure subclass from SchemaSpec.
    py::object build_ctypes_type_(const SchemaSpec &spec, const char *name,
                                   const std::string &packing);

    /// Wrap a type as read-only (adds __setattr__ override).
    py::object wrap_readonly_(const py::object &type);

    /// Create a slot view from raw pointer using pre-built type.
    py::object make_slot_view_(const SchemaSpec &spec, const py::object &type,
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

    /// Clear all py::object members (must be called with GIL held, before Py_Finalize).
    void clear_pyobjects_();
};

} // namespace pylabhub::scripting
