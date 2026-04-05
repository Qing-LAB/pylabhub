#pragma once
/**
 * @file processor_api.hpp
 * @brief ProcessorAPI — Python-facing proxy for processor role.
 *
 * Thin wrapper around RoleAPIBase. Phase 2 migration.
 */

#include "utils/role_api_base.hpp"
#include "python_helpers.hpp"

#include "utils/json_fwd.hpp"
#include <pybind11/pybind11.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace py = pybind11;

namespace pylabhub::processor
{

class ProcessorAPI
{
  public:
    explicit ProcessorAPI(scripting::RoleAPIBase &base)
        : base_(&base)
    {}

    // Identity
    [[nodiscard]] const std::string &uid()         const noexcept { return base_->uid(); }
    [[nodiscard]] const std::string &name()        const noexcept { return base_->name(); }
    [[nodiscard]] const std::string &in_channel()  const noexcept { return base_->channel(); }
    [[nodiscard]] const std::string &out_channel() const noexcept { return base_->out_channel(); }
    [[nodiscard]] const std::string &log_level()   const noexcept { return base_->log_level(); }
    [[nodiscard]] const std::string &script_dir()  const noexcept { return base_->script_dir(); }
    [[nodiscard]] const std::string &role_dir()    const noexcept { return base_->role_dir(); }
    [[nodiscard]] std::string        logs_dir()    const { return base_->logs_dir(); }
    [[nodiscard]] std::string        run_dir()     const { return base_->run_dir(); }

    void log(const std::string &level, const std::string &msg) { base_->log(level, msg); }
    void stop() { base_->stop(); }
    void set_critical_error() { base_->set_critical_error(); }
    [[nodiscard]] bool critical_error() const noexcept { return base_->critical_error(); }

    [[nodiscard]] py::object flexzone() const;

    // Messaging
    bool broadcast(py::bytes data);
    bool send(const std::string &identity, py::bytes data);
    py::list consumers();
    bool update_flexzone_checksum() { return base_->update_flexzone_checksum(); }

    // Broker
    void notify_channel(const std::string &t, const std::string &e, const std::string &d)
        { base_->notify_channel(t, e, d); }
    void broadcast_channel(const std::string &t, const std::string &m, const std::string &d)
        { base_->broadcast_channel(t, m, d); }
    py::list list_channels();
    py::object shm_info(const std::string &channel = {});

    // Inbox
    py::object open_inbox(const std::string &target_uid);
    bool wait_for_role(const std::string &uid, int timeout_ms = 5000);
    void clear_inbox_cache();

    // Diagnostics
    [[nodiscard]] uint64_t script_error_count() const noexcept { return base_->script_error_count(); }
    [[nodiscard]] uint64_t in_slots_received()  const noexcept { return base_->in_slots_received(); }
    [[nodiscard]] uint64_t out_slots_written()  const noexcept { return base_->out_slots_written(); }
    [[nodiscard]] uint64_t out_drop_count()     const noexcept { return base_->out_drop_count(); }
    [[nodiscard]] uint64_t loop_overrun_count() const noexcept { return base_->loop_overrun_count(); }
    [[nodiscard]] uint64_t last_cycle_work_us() const noexcept { return base_->last_cycle_work_us(); }
    [[nodiscard]] py::dict metrics() const;

    // Queue state
    [[nodiscard]] uint64_t last_seq()       const noexcept { return base_->last_seq(); }
    void update_last_seq(uint64_t seq) noexcept { base_->update_last_seq(seq); }
    [[nodiscard]] uint64_t in_capacity()    const noexcept { return static_cast<uint64_t>(base_->in_capacity()); }
    [[nodiscard]] std::string in_policy()   const { return base_->in_policy(); }
    [[nodiscard]] uint64_t out_capacity()   const noexcept { return static_cast<uint64_t>(base_->out_capacity()); }
    [[nodiscard]] std::string out_policy()  const { return base_->out_policy(); }
    void set_verify_checksum(bool enable) { base_->set_verify_checksum(enable); }

    // Custom metrics
    void report_metric(const std::string &key, double value) { base_->report_metric(key, value); }
    void report_metrics(const std::unordered_map<std::string, double> &kv) { base_->report_metrics(kv); }
    void clear_custom_metrics() { base_->clear_custom_metrics(); }

    // Metrics snapshot
    [[nodiscard]] nlohmann::json snapshot_metrics_json() const { return base_->snapshot_metrics_json(); }

    // Shutdown
    [[nodiscard]] std::string stop_reason() const noexcept { return base_->stop_reason(); }
    [[nodiscard]] uint64_t ctrl_queue_dropped() const noexcept { return base_->ctrl_queue_dropped(); }

    // Spinlocks
    [[nodiscard]] uint64_t slot_logical_size(std::optional<int> side = std::nullopt) const;
    [[nodiscard]] uint64_t flexzone_logical_size(std::optional<int> side = std::nullopt) const;

    py::object spinlock(std::size_t index, std::optional<int> side = std::nullopt);
    [[nodiscard]] uint32_t spinlock_count(std::optional<int> side = std::nullopt) const;

  private:
    scripting::RoleAPIBase  *base_;
    py::object              *flexzone_obj_{nullptr};
    std::unordered_map<std::string, py::object> inbox_cache_;
public:
    py::object shared_data_{py::none()};
};

// SpinLockPy is in python_helpers.hpp (pylabhub::scripting::SpinLockPy).

} // namespace pylabhub::processor
