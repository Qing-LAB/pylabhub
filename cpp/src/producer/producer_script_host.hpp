#pragma once
/**
 * @file producer_script_host.hpp
 * @brief ProducerScriptHost — PythonRoleHostBase subclass for the producer process.
 *
 * Inherits the common do_python_work() skeleton from PythonRoleHostBase and
 * overrides virtual hooks for producer-specific behavior:
 *  - Timer-driven, transport-agnostic production loop (run_loop_)
 *  - Single output channel with Producer + Messenger
 *  - on_produce(out_slot, fz, msgs, api) → bool callback
 *
 * The data path is abstracted by hub::QueueWriter.  Depending on config_.transport:
 *  - Transport::Shm → hub::ShmQueue::from_producer_ref (wraps DataBlockProducer; checksum-aware)
 *  - Transport::Zmq → hub::ZmqQueue::push_to (PUSH socket; fire-and-forget send_thread_)
 *
 * See HEP-CORE-0018 for the full producer binary specification.
 */

#include "producer_api.hpp"
#include "producer_config.hpp"
#include "producer_schema.hpp"

#include "python_role_host_base.hpp"

#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_queue.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/messenger.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace py = pybind11;

namespace pylabhub::producer
{

class ProducerScriptHost : public scripting::PythonRoleHostBase
{
  public:
    ProducerScriptHost() = default;
    ~ProducerScriptHost() override;

    ProducerScriptHost(const ProducerScriptHost &)            = delete;
    ProducerScriptHost &operator=(const ProducerScriptHost &) = delete;

    void set_config(ProducerConfig config);

    [[nodiscard]] const ProducerConfig &config() const noexcept { return config_; }
    [[nodiscard]] const ProducerAPI    &api()    const noexcept { return api_; }

  protected:
    // ── Virtual hooks ────────────────────────────────────────────────────────

    const char *role_tag()  const override { return "prod"; }
    const char *role_name() const override { return "producer"; }
    std::string role_uid()  const override { return config_.producer_uid; }
    std::string script_base_dir() const override { return config_.script_path; }
    std::string script_type_str() const override { return config_.script_type; }
    std::string required_callback_name() const override { return "on_produce"; }

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
    ProducerConfig config_;
    ProducerAPI    api_;

    hub::Messenger               out_messenger_;
    std::optional<hub::Producer> out_producer_;

    /// Transport-agnostic output queue.  Created by start_role() based on
    /// config_.transport.  ShmQueue (write) for Shm; ZmqQueue (write) for Zmq.
    std::unique_ptr<hub::QueueWriter>  queue_;

    SchemaSpec slot_spec_;
    size_t     schema_slot_size_{0};

    py::object slot_type_{py::none()};
    py::object py_on_produce_{py::none()};

    std::thread              loop_thread_;
    std::atomic<uint64_t>    iteration_count_{0};

    // Inbox facility (optional — active only when config_.has_inbox())
    std::unique_ptr<hub::InboxQueue>  inbox_queue_;
    scripting::SchemaSpec             inbox_spec_;
    size_t                            inbox_schema_slot_size_{0};
    py::object                        inbox_type_{py::none()};
    py::object                        py_on_inbox_{py::none()};
    std::thread                       inbox_thread_;

    py::object make_out_slot_view_(void *data, size_t size) const;
    py::object make_inbox_slot_view_(const void *data, size_t size) const;

    bool call_on_produce_(py::object &out_sv, py::object &fz, py::list &msgs);

    void run_loop_();
    void run_ctrl_thread_();
    void run_inbox_thread_();
};

} // namespace pylabhub::producer
