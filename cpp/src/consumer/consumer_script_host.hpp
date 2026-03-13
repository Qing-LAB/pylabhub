#pragma once
/**
 * @file consumer_script_host.hpp
 * @brief ConsumerScriptHost — PythonRoleHostBase subclass for the consumer process.
 *
 * Inherits the common do_python_work() skeleton from PythonRoleHostBase and
 * overrides virtual hooks for consumer-specific behavior:
 *  - Demand-driven consumption loop (run_loop_)
 *  - Single input channel with Consumer + Messenger
 *  - on_consume(in_slot, fz, msgs, api) → void callback
 *  - Flexzone: zero-copy R/W live view (from_buffer); in_slot: zero-copy with write-guard
 *  - Messages list omits sender field
 *
 * See HEP-CORE-0018 for the full consumer binary specification.
 */

#include "consumer_api.hpp"
#include "consumer_config.hpp"
#include "consumer_schema.hpp"

#include "python_role_host_base.hpp"

#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_queue.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
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
    std::string script_base_dir() const override
    {
        if (config_.role_dir.empty()) return config_.script_path;
        const std::filesystem::path sp(config_.script_path);
        return sp.is_absolute() ? config_.script_path
                                : (std::filesystem::path(config_.role_dir) / sp).string();
    }
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

    /// SHM transport: owned QueueReader wrapping DataBlockConsumer.
    /// ZMQ transport: nullptr (queue_reader_ points to Consumer-owned ZmqQueue).
    std::unique_ptr<hub::QueueReader> shm_queue_;
    /// Non-owning pointer to the active QueueReader (shm_queue_.get() or Consumer::queue_reader()).
    /// Set in start_role(); cleared in stop_role().
    hub::QueueReader *queue_reader_{nullptr};

    // ── Inbox facility (optional) ──────────────────────────────────────────
    scripting::SchemaSpec             inbox_spec_;
    size_t                            inbox_schema_slot_size_{0};
    py::object                        inbox_type_{py::none()};
    py::object                        py_on_inbox_{py::none()};
    std::unique_ptr<hub::InboxQueue>  inbox_queue_;
    std::thread                       inbox_thread_;

    py::object make_in_slot_view_(const void *data, size_t size) const;
    py::object make_inbox_slot_view_(const void *data, size_t size) const;

    void call_on_consume_(py::object &in_sv, py::object &fz, py::list &msgs);

    void run_loop_();        ///< Unified transport-agnostic consumption loop
    void run_ctrl_thread_();
    void run_inbox_thread_();
};

} // namespace pylabhub::consumer
