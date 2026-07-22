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
#include <string_view>
#include <unordered_map>

namespace py = pybind11;

namespace pylabhub::consumer
{

class ConsumerAPI
{
  public:
    explicit ConsumerAPI(scripting::RoleAPIBase &base) : base_(&base) {}

    // Identity / environment — delegate to base
    [[nodiscard]] const std::string &uid() const noexcept { return base_->uid(); }
    [[nodiscard]] const std::string &name() const noexcept { return base_->name(); }
    [[nodiscard]] const std::string &channel() const noexcept { return base_->channel(); }
    [[nodiscard]] const std::string &log_level() const noexcept { return base_->log_level(); }
    [[nodiscard]] const std::string &script_dir() const noexcept { return base_->script_dir(); }
    [[nodiscard]] const std::string &role_dir() const noexcept { return base_->role_dir(); }
    [[nodiscard]] std::string logs_dir() const { return base_->logs_dir(); }
    [[nodiscard]] std::string run_dir() const { return base_->run_dir(); }

    void log(const std::string &level, const std::string &msg) { base_->log(level, msg); }
    void stop() { base_->stop(); }
    void set_critical_error(std::string_view msg) { base_->set_critical_error(msg); }
    [[nodiscard]] bool critical_error() const noexcept { return base_->critical_error(); }

    // Band pub/sub (HEP-CORE-0030)
    py::object band_join(const std::string &channel);
    /// Python-side ergonomic wrapper — see ProducerAPI::band_leave for rationale.
    bool band_leave(const std::string &channel)
    {
        auto resp = base_->band_leave(channel);
        return resp.has_value() && resp->value("status", std::string{}) == "success";
    }
    void band_broadcast(const std::string &channel, py::dict body);
    py::object band_members(const std::string &channel);
    bool is_in_band(const std::string &channel) const;

    /// Inquiry helpers — engine-parity with Native + Lua.  Bool / int
    /// returns rather than C-API int tristate.  See ProducerAPI for
    /// rationale.
    bool band_member_contains(const std::string &channel, const std::string &role_uid);
    int band_member_count(const std::string &channel);
    bool allowed_peer_contains(const std::string &channel, const std::string &role_uid) const;
    int allowed_peer_count(const std::string &channel) const;

    /// HEP-CORE-0028 §6a + HEP-CORE-0007 §CHANNEL_AUTH_CHANGED_NOTIFY
    /// (lines 1834-1838) — binding-side live-peer count.
    [[nodiscard]] std::size_t consumer_count(const std::string &channel) const
    {
        return base_->consumer_count(channel);
    }
    [[nodiscard]] std::size_t producer_count(const std::string &channel) const
    {
        return base_->producer_count(channel);
    }

    // Inbox
    py::object open_inbox(const std::string &target_uid);
    bool wait_for_role(const std::string &uid, int timeout_ms = 5000);
    void clear_inbox_cache();

    // Flexzone
    [[nodiscard]] py::object flexzone(std::optional<int> side = std::nullopt) const;

    // Consumer diagnostics
    [[nodiscard]] uint64_t script_error_count() const noexcept
    {
        return base_->script_error_count();
    }
    [[nodiscard]] uint64_t in_slots_received() const noexcept { return base_->in_slots_received(); }
    [[nodiscard]] uint64_t loop_overrun_count() const noexcept
    {
        return base_->loop_overrun_count();
    }
    void set_verify_checksum(bool enable) { base_->set_verify_checksum(enable); }
    [[nodiscard]] uint64_t last_cycle_work_us() const noexcept
    {
        return base_->last_cycle_work_us();
    }
    [[nodiscard]] uint64_t last_seq() const noexcept { return base_->last_seq(); }
    [[nodiscard]] size_t in_capacity() const noexcept { return base_->in_capacity(); }
    [[nodiscard]] std::string in_policy() const { return base_->in_policy(); }
    [[nodiscard]] py::dict metrics() const;

    /// HEP-CORE-0036 §I11 polling surface — engine-parity stub on the
    /// consumer side.  Consumer-side handler does not currently
    /// populate the cache (broker only sends CHANNEL_AUTH_CHANGED_NOTIFY
    /// to producers), so this returns an empty list.  API kept for
    /// uniformity so scripts written against ProducerAPI's polling
    /// shape work unmodified if reused for consumer roles.
    [[nodiscard]] py::list allowed_peers(const std::string &channel) const;

    /// HEP-CORE-0036 §I11 + §6.4 consumer-side polling surface
    /// (mirror of producer-side `allowed_peers`).  Returns the most
    /// recent CONSUMER_REG_ACK.producers[] snapshot for `channel` as a
    /// list of `{role_uid, pubkey}` dicts.  Empty when the channel was
    /// never registered, the broker delivered an empty list, or the
    /// transport is SHM (no producers[] field per §5.6).  Read-only.
    [[nodiscard]] py::list producers(const std::string &channel) const;

    /// HEP-CORE-0028 §6a — LIVE consumer role_uid list.  Symmetric
    /// with `producers()`.  Empty on consumer-role side.
    [[nodiscard]] py::list consumers(const std::string &channel) const;

    /// HEP-CORE-0036 §6.7 (#190) — see ProducerAPI::is_channel_ready.
    [[nodiscard]] bool is_channel_ready(const std::string &channel) const
    {
        return base_->is_channel_ready(channel);
    }

    /// HEP-CORE-0035 §2 (#186, #194) — see ProducerAPI::queue_mechanism.
    [[nodiscard]] std::string queue_mechanism(int side) const
    {
        const auto cs = (side == 0) ? scripting::ChannelSide::Tx : scripting::ChannelSide::Rx;
        return std::string{pylabhub::hub::mechanism_name(base_->queue_mechanism(cs))};
    }

    // Spinlocks
    [[nodiscard]] uint64_t slot_logical_size(std::optional<int> side = std::nullopt) const;
    [[nodiscard]] uint64_t flexzone_logical_size(std::optional<int> side = std::nullopt) const;

    py::object spinlock(std::size_t index, std::optional<int> side = std::nullopt);
    [[nodiscard]] uint32_t spinlock_count(std::optional<int> side = std::nullopt) const;

    // Custom metrics
    void report_metric(const std::string &key, double value) { base_->report_metric(key, value); }
    void report_metrics(const std::unordered_map<std::string, double> &kv)
    {
        base_->report_metrics(kv);
    }
    void clear_custom_metrics() { base_->clear_custom_metrics(); }

    // Metrics snapshot
    [[nodiscard]] nlohmann::json snapshot_metrics_json() const
    {
        return base_->snapshot_metrics_json();
    }

    // Shutdown
    [[nodiscard]] std::string stop_reason() const noexcept { return base_->stop_reason(); }

    void set_rx_flexzone(std::optional<py::object> obj) { rx_flexzone_obj_ = std::move(obj); }

  private:
    scripting::RoleAPIBase *base_;
    std::optional<py::object> rx_flexzone_obj_;
    std::unordered_map<std::string, py::object> inbox_cache_;

  public:
    py::object shared_data_{py::none()};
};

// SpinLockPy is in python_helpers.hpp (pylabhub::scripting::SpinLockPy).

} // namespace pylabhub::consumer
