#pragma once
/**
 * @file processor_script_host.hpp
 * @brief ProcessorScriptHost — PythonRoleHostBase subclass for the processor process.
 *
 * Inherits the common do_python_work() skeleton from PythonRoleHostBase and
 * overrides virtual hooks for processor-specific behavior:
 *  - Dual input/output channels with Consumer + Producer
 *  - Delegates data loop to hub::Processor (no manual loop thread)
 *  - on_process(in_slot, out_slot, fz, msgs, api) → bool callback
 *
 * See HEP-CORE-0015 for the full processor binary specification.
 */

#include "processor_api.hpp"
#include "processor_config.hpp"
#include "processor_schema.hpp"

#include "python_role_host_base.hpp"

#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_processor.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/messenger.hpp"

#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace py = pybind11;

namespace pylabhub::processor
{

class ProcessorScriptHost : public scripting::PythonRoleHostBase
{
  public:
    ProcessorScriptHost() = default;
    ~ProcessorScriptHost() override;

    ProcessorScriptHost(const ProcessorScriptHost &)            = delete;
    ProcessorScriptHost &operator=(const ProcessorScriptHost &) = delete;

    void set_config(ProcessorConfig config);

    [[nodiscard]] const ProcessorConfig &config() const noexcept { return config_; }
    [[nodiscard]] const ProcessorAPI    &api()    const noexcept { return api_; }

  protected:
    // ── Virtual hooks ────────────────────────────────────────────────────────

    const char *role_tag()  const override { return "proc"; }
    const char *role_name() const override { return "processor"; }
    std::string role_uid()  const override { return config_.processor_uid; }
    std::string script_base_dir() const override
    {
        if (config_.role_dir.empty()) return config_.script_path;
        const std::filesystem::path sp(config_.script_path);
        return sp.is_absolute() ? config_.script_path
                                : (std::filesystem::path(config_.role_dir) / sp).string();
    }
    std::string script_type_str() const override { return config_.script_type; }
    std::string required_callback_name() const override { return "on_process"; }

    void wire_api_identity() override;
    void extract_callbacks(py::module_ &mod) override;
    bool has_required_callback() const override;

    bool build_role_types() override;
    void print_validate_layout() override;

    bool start_role() override;
    void stop_role() override;
    void cleanup_on_start_failure() override;
    void clear_role_pyobjects() override;

    void on_script_error() override { api_.increment_script_errors(); }
    bool has_connection_for_stop() const override { return out_producer_.has_value(); }
    void update_fz_checksum_after_init() override;

  private:
    ProcessorConfig config_;
    ProcessorAPI    api_;

    // ── ZMQ connections ──────────────────────────────────────────────────────
    hub::Messenger                 in_messenger_;
    hub::Messenger                 out_messenger_;
    std::optional<hub::Consumer>   in_consumer_;
    std::optional<hub::Producer>   out_producer_;

    // ── Schema ───────────────────────────────────────────────────────────────
    SchemaSpec in_slot_spec_;
    SchemaSpec out_slot_spec_;
    size_t     in_schema_slot_size_{0};
    size_t     out_schema_slot_size_{0};

    // ── Python objects (role-specific) ───────────────────────────────────────
    py::object in_slot_type_{py::none()};
    py::object out_slot_type_{py::none()};
    py::object py_on_process_{py::none()};

    // ── Inbox facility (optional) ──────────────────────────────────────────
    scripting::SchemaSpec            inbox_spec_;
    size_t                           inbox_schema_slot_size_{0};
    py::object                       inbox_type_{py::none()};
    py::object                       py_on_inbox_{py::none()};
    std::unique_ptr<hub::InboxQueue> inbox_queue_;
    std::thread                      inbox_thread_;

    // ── hub::Processor delegation ────────────────────────────────────────────
    // Owned SHM queues (non-null only for SHM transport; null for ZMQ transport).
    std::unique_ptr<hub::QueueReader>  in_queue_;
    std::unique_ptr<hub::QueueWriter>  out_queue_;
    // Raw pointers used by Processor: point to either owned SHM queue or
    // borrowed ZmqQueue owned by in_consumer_ / out_producer_.
    hub::QueueReader                  *in_q_{nullptr};
    hub::QueueWriter                  *out_q_{nullptr};
    std::optional<hub::Processor>   processor_;

    // ── Internal methods ─────────────────────────────────────────────────────

    py::object make_in_slot_view_   (const void *data, size_t size) const;
    py::object make_out_slot_view_  (void       *data, size_t size) const;
    py::object make_inbox_slot_view_(const void *data, size_t size) const;

    bool call_on_process_(py::object &in_sv, py::object &out_sv,
                           py::object &fz,    py::list   &msgs);

    void run_ctrl_thread_();
    void run_inbox_thread_();
};

} // namespace pylabhub::processor
