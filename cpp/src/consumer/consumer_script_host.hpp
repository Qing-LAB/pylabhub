#pragma once
/**
 * @file consumer_script_host.hpp
 * @brief ConsumerScriptHost — PythonRoleHostBase subclass for the consumer process.
 *
 * Inherits the common do_python_work() skeleton from PythonRoleHostBase and
 * overrides virtual hooks for consumer-specific behavior:
 *  - Demand-driven consumption loop (run_loop_shm_)
 *  - Single input channel with Consumer + Messenger
 *  - on_consume(in_slot, fz, msgs, api) → void callback
 *  - Read-only flexzone (from_buffer_copy for ctypes)
 *  - Messages list omits sender field
 *
 * See HEP-CORE-0018 for the full consumer binary specification.
 */

#include "consumer_api.hpp"
#include "consumer_config.hpp"
#include "consumer_schema.hpp"

#include "python_role_host_base.hpp"

#include "utils/hub_consumer.hpp"
#include "utils/messenger.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace py = pybind11;

namespace pylabhub::consumer
{

class ConsumerScriptHost : public scripting::PythonRoleHostBase
{
  public:
    ConsumerScriptHost() = default;
    ~ConsumerScriptHost() override;

    ConsumerScriptHost(const ConsumerScriptHost &)            = delete;
    ConsumerScriptHost &operator=(const ConsumerScriptHost &) = delete;

    void set_config(ConsumerConfig config);

    [[nodiscard]] const ConsumerConfig &config() const noexcept { return config_; }
    [[nodiscard]] const ConsumerAPI    &api()    const noexcept { return api_; }

  protected:
    // ── Virtual hooks ────────────────────────────────────────────────────────

    const char *role_tag()  const override { return "cons"; }
    const char *role_name() const override { return "consumer"; }
    std::string role_uid()  const override { return config_.consumer_uid; }
    std::string script_base_dir() const override { return config_.script_path; }
    std::string script_type_str() const override { return config_.script_type; }
    std::string required_callback_name() const override { return "on_consume"; }

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
    bool has_connection_for_stop() const override { return in_consumer_.has_value(); }

    /** Consumer: messages list contains bare bytes (no sender). */
    py::list build_messages_list_(std::vector<scripting::IncomingMessage> &msgs) override;

  private:
    ConsumerConfig config_;
    ConsumerAPI    api_;

    hub::Messenger              in_messenger_;
    std::optional<hub::Consumer> in_consumer_;

    SchemaSpec slot_spec_;
    size_t     schema_slot_size_{0};

    py::object slot_type_{py::none()};
    py::object py_on_consume_{py::none()};

    std::thread              loop_thread_;
    std::atomic<uint64_t>    iteration_count_{0};

    py::object make_in_slot_view_(const void *data, size_t size) const;

    void call_on_consume_(py::object &in_sv, py::object &fz, py::list &msgs);

    void run_loop_shm_();
    void run_zmq_thread_();
};

} // namespace pylabhub::consumer
