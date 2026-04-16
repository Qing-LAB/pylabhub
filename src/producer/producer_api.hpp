#pragma once
/**
 * @file producer_api.hpp
 * @brief ProducerAPI — Python-facing proxy for producer role.
 *
 * Thin wrapper around RoleAPIBase. Provides Python type conversions
 * (py::bytes, py::dict, py::object) on top of the language-neutral C++ base.
 *
 * ## Migration status (Phase 2)
 * ProducerAPI delegates all C++ logic to RoleAPIBase. Python-specific
 * wrapping (flexzone py::object, InboxHandle, SpinLockPy, GIL release)
 * stays here. Will be replaced by direct pybind11 registration in Phase 3.
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

namespace pylabhub::producer
{

// ============================================================================
// ProducerAPI — Python wrapper around RoleAPIBase
// ============================================================================

class ProducerAPI
{
  public:
    /// Construct with a fully-wired RoleAPIBase (owned by role host).
    explicit ProducerAPI(scripting::RoleAPIBase &base)
        : base_(&base)
    {}

    // ── C++ host setters ─────────────────────────────────────────────────────

    // ── Python-accessible — identity / environment ───────────────────────────

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

    /// Return the persistent output flexzone Python object, or None.
    [[nodiscard]] py::object flexzone(std::optional<int> side = std::nullopt) const;

    // ── Python-accessible — messaging ────────────────────────────────────────

    bool update_flexzone_checksum() { return base_->update_flexzone_checksum(); }

    // ── Band pub/sub (HEP-CORE-0030) ──────────────────────────────────

    py::object band_join(const std::string &channel);
    bool band_leave(const std::string &channel) { return base_->band_leave(channel); }
    void band_broadcast(const std::string &channel, py::dict body);
    py::object band_members(const std::string &channel);

    py::object open_inbox(const std::string &target_uid);
    bool wait_for_role(const std::string &uid, int timeout_ms = 5000);
    void clear_inbox_cache();

    // ── Python-accessible — diagnostics ──────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept { return base_->script_error_count(); }
    [[nodiscard]] uint64_t out_slots_written()  const noexcept { return base_->out_slots_written(); }
    [[nodiscard]] uint64_t out_drop_count()     const noexcept { return base_->out_drop_count(); }
    [[nodiscard]] uint64_t loop_overrun_count() const noexcept { return base_->loop_overrun_count(); }
    [[nodiscard]] size_t      out_capacity()    const noexcept { return base_->out_capacity(); }
    [[nodiscard]] std::string out_policy()      const { return base_->out_policy(); }
    [[nodiscard]] uint64_t last_cycle_work_us() const noexcept { return base_->last_cycle_work_us(); }
    [[nodiscard]] py::dict metrics() const;

    // ── Python-accessible — custom metrics ───────────────────────────────────

    void report_metric(const std::string &key, double value) { base_->report_metric(key, value); }
    void report_metrics(const std::unordered_map<std::string, double> &kv) { base_->report_metrics(kv); }
    void clear_custom_metrics() { base_->clear_custom_metrics(); }

    // ── Internal — metrics snapshot ──────────────────────────────────────────

    [[nodiscard]] nlohmann::json snapshot_metrics_json() const { return base_->snapshot_metrics_json(); }

    // ── Python-accessible — shutdown diagnostics ─────────────────────────────

    [[nodiscard]] std::string stop_reason() const noexcept { return base_->stop_reason(); }
    [[nodiscard]] uint64_t ctrl_queue_dropped() const noexcept { return base_->ctrl_queue_dropped(); }

    // ── Python-accessible — spinlocks ────────────────────────────────────────

    [[nodiscard]] uint64_t slot_logical_size(std::optional<int> side = std::nullopt) const;
    [[nodiscard]] uint64_t flexzone_logical_size(std::optional<int> side = std::nullopt) const;

    py::object spinlock(std::size_t index, std::optional<int> side = std::nullopt);
    [[nodiscard]] uint32_t spinlock_count(std::optional<int> side = std::nullopt) const;

    void set_tx_flexzone(std::optional<py::object> obj) { tx_flexzone_obj_ = std::move(obj); }

  private:
    scripting::RoleAPIBase  *base_;
    std::optional<py::object> tx_flexzone_obj_;

    std::unordered_map<std::string, py::object> inbox_cache_;
public:
    py::object shared_data_{py::none()};
};

// SpinLockPy is in python_helpers.hpp (pylabhub::scripting::SpinLockPy).

} // namespace pylabhub::producer
