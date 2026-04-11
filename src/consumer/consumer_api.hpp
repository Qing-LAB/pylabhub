#pragma once
/**
 * @file consumer_api.hpp
 * @brief ConsumerAPI — Python-facing proxy for consumer role.
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

namespace pylabhub::consumer
{

class ConsumerAPI
{
  public:
    explicit ConsumerAPI(scripting::RoleAPIBase &base)
        : base_(&base)
    {}

    // Identity / environment — delegate to base
    [[nodiscard]] const std::string &uid()        const noexcept { return base_->uid(); }
    [[nodiscard]] const std::string &name()       const noexcept { return base_->name(); }
    [[nodiscard]] const std::string &channel()    const noexcept { return base_->channel(); }
    [[nodiscard]] const std::string &log_level()  const noexcept { return base_->log_level(); }
    [[nodiscard]] const std::string &script_dir() const noexcept { return base_->script_dir(); }
    [[nodiscard]] const std::string &role_dir()   const noexcept { return base_->role_dir(); }
    [[nodiscard]] std::string        logs_dir()   const { return base_->logs_dir(); }
    [[nodiscard]] std::string        run_dir()    const { return base_->run_dir(); }

    void log(const std::string &level, const std::string &msg) { base_->log(level, msg); }
    void stop() { base_->stop(); }
    void set_critical_error() { base_->set_critical_error(); }
    [[nodiscard]] bool critical_error() const noexcept { return base_->critical_error(); }

    // Channel pub/sub (HEP-CORE-0030)
    py::object join_channel(const std::string &channel);
    bool leave_channel(const std::string &channel) { return base_->leave_channel(channel); }
    void send_channel_msg(const std::string &channel, py::dict body);
    py::object channel_members(const std::string &channel);

    // Inbox
    py::object open_inbox(const std::string &target_uid);
    bool wait_for_role(const std::string &uid, int timeout_ms = 5000);
    void clear_inbox_cache();

    // Consumer diagnostics
    [[nodiscard]] uint64_t script_error_count() const noexcept { return base_->script_error_count(); }
    [[nodiscard]] uint64_t in_slots_received()  const noexcept { return base_->in_slots_received(); }
    [[nodiscard]] uint64_t loop_overrun_count() const noexcept { return base_->loop_overrun_count(); }
    void set_verify_checksum(bool enable) { base_->set_verify_checksum(enable); }
    [[nodiscard]] uint64_t last_cycle_work_us() const noexcept { return base_->last_cycle_work_us(); }
    [[nodiscard]] uint64_t last_seq()           const noexcept { return base_->last_seq(); }
    [[nodiscard]] size_t      in_capacity()     const noexcept { return base_->in_capacity(); }
    [[nodiscard]] std::string in_policy()       const { return base_->in_policy(); }
    [[nodiscard]] py::dict metrics() const;

    // Spinlocks
    [[nodiscard]] uint64_t slot_logical_size(std::optional<int> side = std::nullopt) const;
    [[nodiscard]] uint64_t flexzone_logical_size(std::optional<int> side = std::nullopt) const;

    py::object spinlock(std::size_t index, std::optional<int> side = std::nullopt);
    [[nodiscard]] uint32_t spinlock_count(std::optional<int> side = std::nullopt) const;

    // Custom metrics
    void report_metric(const std::string &key, double value) { base_->report_metric(key, value); }
    void report_metrics(const std::unordered_map<std::string, double> &kv) { base_->report_metrics(kv); }
    void clear_custom_metrics() { base_->clear_custom_metrics(); }

    // Metrics snapshot
    [[nodiscard]] nlohmann::json snapshot_metrics_json() const { return base_->snapshot_metrics_json(); }

    // Shutdown
    [[nodiscard]] std::string stop_reason() const noexcept { return base_->stop_reason(); }
    [[nodiscard]] uint64_t ctrl_queue_dropped() const noexcept { return base_->ctrl_queue_dropped(); }

  private:
    scripting::RoleAPIBase  *base_;
    std::unordered_map<std::string, py::object> inbox_cache_;
public:
    py::object shared_data_{py::none()};
};

// SpinLockPy is in python_helpers.hpp (pylabhub::scripting::SpinLockPy).

} // namespace pylabhub::consumer
