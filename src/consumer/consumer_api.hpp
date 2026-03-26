#pragma once
/**
 * @file consumer_api.hpp
 * @brief ConsumerAPI — Python-facing proxy passed to consumer script callbacks.
 *
 * ## Python usage
 *
 * @code{.py}
 *   import pylabhub_consumer as cons
 *
 *   def on_init(api: cons.ConsumerAPI):
 *       api.log('info', f"Consumer {api.uid()} starting")
 *
 *   def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
 *       # in_slot   — ctypes/numpy read-only view of the input SHM slot,
 *       #             or None on timeout
 *       # flexzone  — persistent read-only flexzone ctypes struct, or None
 *       # messages  — list of (sender: str, data: bytes) from ZMQ publisher
 *       # api       — ConsumerAPI proxy
 *       if in_slot is None:
 *           return
 *       api.log('info', f"value={in_slot.value}")
 *
 *   def on_stop(api: cons.ConsumerAPI):
 *       api.log('info', "Consumer stopping")
 * @endcode
 */

#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_queue.hpp"
#include "utils/in_process_spin_state.hpp"
#include "utils/messenger.hpp"
#include "script_host_helpers.hpp"
#include "utils/shared_memory_spinlock.hpp"

#include "utils/json_fwd.hpp"
#include <pybind11/pybind11.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace py = pybind11;

// RoleHostCore is needed for inline metric accessors that read core_->field.
#include "utils/role_host_core.hpp"

namespace pylabhub::scripting { class ScriptEngine; }
namespace pylabhub::consumer
{

// ============================================================================
// ConsumerAPI
// ============================================================================

class ConsumerAPI
{
  public:
    /// Construct with RoleHostCore — single source of truth for all metrics
    /// and shutdown state. Must outlive this object.
    explicit ConsumerAPI(scripting::RoleHostCore &core)
        : core_(&core)
    {}

    // ── C++ host setters (never called from Python) ───────────────────────────

    void set_consumer(hub::Consumer *c) noexcept { consumer_ = c; }
    void set_messenger(hub::Messenger *m) noexcept { messenger_ = m; }
    void set_engine(scripting::ScriptEngine *e) noexcept { engine_ = e; }
    void set_uid(std::string uid)    { uid_        = std::move(uid); }
    void set_name(std::string name)  { name_       = std::move(name); }
    void set_channel(std::string c)  { channel_    = std::move(c); }
    void set_log_level(std::string l){ log_level_  = std::move(l); }
    void set_script_dir(std::string d){ script_dir_ = std::move(d); }
    void set_role_dir(std::string d)  { role_dir_   = std::move(d); }

    void update_last_seq(uint64_t seq) noexcept
        { last_seq_snapshot_.store(seq, std::memory_order_relaxed); }

    // ── Python-accessible — identity / environment ────────────────────────────

    [[nodiscard]] const std::string &uid()        const noexcept { return uid_; }
    [[nodiscard]] const std::string &name()       const noexcept { return name_; }
    [[nodiscard]] const std::string &channel()    const noexcept { return channel_; }
    [[nodiscard]] const std::string &log_level()  const noexcept { return log_level_; }
    [[nodiscard]] const std::string &script_dir() const noexcept { return script_dir_; }
    [[nodiscard]] const std::string &role_dir()   const noexcept { return role_dir_; }
    [[nodiscard]] std::string        logs_dir()   const { return role_dir_.empty() ? "" : role_dir_ + "/logs"; }
    [[nodiscard]] std::string        run_dir()    const { return role_dir_.empty() ? "" : role_dir_ + "/run";  }

    void log(const std::string &level, const std::string &msg);
    void stop();
    void set_critical_error();
    [[nodiscard]] bool critical_error() const noexcept
        { return core_->is_critical_error(); }

    /// Send an event notification to a target channel's producer via the broker.
    void notify_channel(const std::string &target_channel, const std::string &event,
                        const std::string &data);

    /// Broadcast a control message to ALL members of a channel via the broker.
    void broadcast_channel(const std::string &target_channel, const std::string &message,
                           const std::string &data);

    /// Query the broker for the list of registered channels.
    py::list list_channels();

    /// Query the broker for SHM block topology and DataBlockMetrics.
    py::object shm_blocks(const std::string& channel = {});

    /// Open (or return cached) an InboxHandle to send typed messages to another role.
    /// Discovers inbox_endpoint + schema from the broker via ROLE_INFO_REQ.
    /// Returns py::none() if the target is not online or has no inbox.
    py::object open_inbox(const std::string &target_uid);

    /// Block until the broker confirms the role with @p uid is registered,
    /// or until @p timeout_ms elapses. GIL is released during polling.
    /// Returns true if the role is online before timeout.
    bool wait_for_role(const std::string &uid, int timeout_ms = 5000);

    /// Clear all InboxHandle Python objects and stop clients. Call with GIL held.
    void clear_inbox_cache();

    // ── Python-accessible — diagnostics ──────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count()  const noexcept
        { return core_->script_errors(); }
    [[nodiscard]] uint64_t in_slots_received()   const noexcept
        { return core_->in_received(); }
    /// Loop overrun count from the main loop deadline check. 0 when no period configured.
    [[nodiscard]] uint64_t loop_overrun_count()  const noexcept { return core_->loop_overrun_count(); }

    /// Enable/disable BLAKE2b checksum verification on input slots (SHM path only).
    /// No-op when the input path is ZMQ (TCP provides integrity).
    void set_verify_checksum(bool enable);
    /// Microseconds of active work (GIL acquire + on_consume callback + slot release) last iteration.
    [[nodiscard]] uint64_t last_cycle_work_us()  const noexcept
        { return core_->last_cycle_work_us(); }
    /// Sequence number of the last slot returned by read_acquire(). 0 until first slot.
    /// IC-04: semantics are transport-specific.
    ///   SHM  — ring-buffer slot index (0-based, wraps at capacity); NOT a global monotone counter.
    ///   ZMQ  — monotone wire sequence number (never wraps in practice).
    ///   Cross-transport comparison is meaningless; use only for detecting stalls within one session.
    [[nodiscard]] uint64_t last_seq() const noexcept
        { return last_seq_snapshot_.load(std::memory_order_relaxed); }
    /// Ring/recv buffer slot count for the input transport queue. 0 if not connected.
    [[nodiscard]] size_t      in_capacity() const noexcept;
    /// Overflow policy description for the input queue (e.g. "shm_read", "zmq_pull_ring_64").
    [[nodiscard]] std::string in_policy()   const;
    /// Combined metrics dict: DataBlock ContextMetrics + last_cycle_work_us + script_errors.
    [[nodiscard]] py::dict metrics() const;

    // ── Python-accessible — SHM spinlocks (SHM transport only) ───────────────

    py::object spinlock(std::size_t index);
    [[nodiscard]] uint32_t spinlock_count() const noexcept;

    // ── Python-accessible — custom metrics (HEP-CORE-0019) ─────────────────

    void report_metric(const std::string &key, double value);
    void report_metrics(const std::unordered_map<std::string, double> &kv);
    void clear_custom_metrics();

    // ── Internal — metrics snapshot (called from zmq thread) ────────────────

    [[nodiscard]] nlohmann::json snapshot_metrics_json() const;

    // ── Python-accessible — shutdown diagnostics ─────────────────────────────

    /// Returns reason the role stopped: "normal", "peer_dead", "hub_dead", or "critical_error".
    [[nodiscard]] std::string stop_reason() const noexcept;
    /// Number of ctrl-send messages dropped due to queue overflow.
    [[nodiscard]] uint64_t ctrl_queue_dropped() const noexcept;

  private:
    hub::Consumer               *consumer_{nullptr};
    hub::Messenger              *messenger_{nullptr};
    scripting::ScriptEngine     *engine_{nullptr};


    std::string uid_;
    std::string name_;
    std::string channel_;
    std::string log_level_;
    std::string script_dir_;
    std::string role_dir_;

    std::atomic<uint64_t> last_seq_snapshot_{0};  ///< Updated by loop_thread_ after each read_acquire()

    // RoleHostCore — single source of truth for all metrics and shutdown state.
    // Set by the constructor; always non-null.
    scripting::RoleHostCore *core_;

    mutable hub::InProcessSpinState                  metrics_spin_;
    std::unordered_map<std::string, double>          custom_metrics_;
    std::unordered_map<std::string, py::object>      inbox_cache_;
public:
    py::object shared_data_{py::none()};  ///< Shared script state dict.
};

// ============================================================================
// ConsumerSpinLockPy — Python context-manager for SHM spinlocks
// ============================================================================

class ConsumerSpinLockPy
{
  public:
    explicit ConsumerSpinLockPy(hub::SharedSpinLock lock) : lock_(std::move(lock)) {}

    void lock()   { lock_.lock(); }
    void unlock() { lock_.unlock(); }

    bool try_lock_for(int timeout_ms) { return lock_.try_lock_for(timeout_ms); }

    [[nodiscard]] bool is_locked_by_current_process() const
        { return lock_.is_locked_by_current_process(); }

    ConsumerSpinLockPy &enter() { lock_.lock(); return *this; }
    void exit(py::object /*exc_type*/, py::object /*exc_val*/, py::object /*exc_tb*/)
        { lock_.unlock(); }

  private:
    hub::SharedSpinLock lock_;
};

} // namespace pylabhub::consumer
