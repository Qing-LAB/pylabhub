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
#include <string_view>
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
    void set_critical_error(std::string_view msg) { base_->set_critical_error(msg); }
    [[nodiscard]] bool critical_error() const noexcept { return base_->critical_error(); }

    /// Return the persistent output flexzone Python object, or None.
    [[nodiscard]] py::object flexzone(std::optional<int> side = std::nullopt) const;

    // ── Python-accessible — messaging ────────────────────────────────────────

    bool update_flexzone_checksum() { return base_->update_flexzone_checksum(); }

    // ── Band pub/sub (HEP-CORE-0030) ──────────────────────────────────

    py::object band_join(const std::string &channel);
    /// Python-side ergonomic wrapper: returns true on success, false on
    /// error or transport failure.  Drops the protocol-level error_code
    /// for scripts that just want a boolean.  Per HEP-CORE-0007 §12.3,
    /// the underlying `RoleAPIBase::band_leave` exposes the full body.
    bool band_leave(const std::string &channel) {
        auto resp = base_->band_leave(channel);
        return resp.has_value() &&
               resp->value("status", std::string{}) == "success";
    }
    void band_broadcast(const std::string &channel, py::dict body);
    py::object band_members(const std::string &channel);

    /// HEP-CORE-0030 amendment 2026-05-19 (S4): role's cached view of
    /// own band membership.  See RoleAPIBase::is_in_band.
    bool is_in_band(const std::string &channel) const;

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

    /// HEP-CORE-0036 §I11 polling surface — snapshot of authorized
    /// peers for the named channel.  Returns a Python list of
    /// `{"role_uid": str, "pubkey": str}` dicts.  Empty list when no
    /// pull has completed for the channel (e.g. before the first
    /// CHANNEL_AUTH_CHANGED_NOTIFY arrives).
    [[nodiscard]] py::list allowed_peers(const std::string &channel) const;

    /// HEP-CORE-0036 §6.7 (#190) — Python-facing channel-state query.
    /// Forwards to RoleAPIBase::is_channel_ready.
    [[nodiscard]] bool is_channel_ready(const std::string &channel) const
    {
        return base_->is_channel_ready(channel);
    }

    /// HEP-CORE-0035 §2 (#186, #194) — direct mechanism accessor for
    /// engine parity with Lua `api.queue_mechanism(side)`.  Returns the
    /// negotiated mechanism name ("Curve" / "Plaintext" / "Uninitialized").
    /// Mirrors the snapshot-dict value at `api.metrics()["queue"]["mechanism"]`
    /// without requiring the script to dig through the metrics payload.
    [[nodiscard]] std::string queue_mechanism(int side) const
    {
        const auto cs = (side == 0) ? scripting::ChannelSide::Tx
                                    : scripting::ChannelSide::Rx;
        return std::string{pylabhub::hub::mechanism_name(
            base_->queue_mechanism(cs))};
    }

    // ── Python-accessible — custom metrics ───────────────────────────────────

    void report_metric(const std::string &key, double value) { base_->report_metric(key, value); }
    void report_metrics(const std::unordered_map<std::string, double> &kv) { base_->report_metrics(kv); }
    void clear_custom_metrics() { base_->clear_custom_metrics(); }

    // ── Internal — metrics snapshot ──────────────────────────────────────────

    [[nodiscard]] nlohmann::json snapshot_metrics_json() const { return base_->snapshot_metrics_json(); }

    // ── Python-accessible — shutdown diagnostics ─────────────────────────────

    [[nodiscard]] std::string stop_reason() const noexcept { return base_->stop_reason(); }

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
