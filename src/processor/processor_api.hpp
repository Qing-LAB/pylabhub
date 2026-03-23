#pragma once
/**
 * @file processor_api.hpp
 * @brief ProcessorAPI — Python-facing proxy passed to processor script callbacks.
 *
 * ## Python usage
 *
 * @code{.py}
 *   import pylabhub_processor as proc
 *
 *   def on_init(api: proc.ProcessorAPI):
 *       api.log('info', f"Processor {api.uid()} starting")
 *
 *   def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
 *       # in_slot   — ctypes/numpy read-only view, or None on timeout
 *       # out_slot  — ctypes/numpy writable view, or None on timeout
 *       # flexzone  — persistent output flexzone ctypes struct, or None
 *       # messages  — list of (sender: str, data: bytes)
 *       # api       — ProcessorAPI proxy
 *       if in_slot is None:
 *           return False
 *       out_slot.value = in_slot.value * 2.0
 *       return True  # True=commit; False=discard; None=error
 *
 *   def on_stop(api: proc.ProcessorAPI):
 *       api.log('info', "Processor stopping")
 * @endcode
 */

#include "utils/hub_consumer.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/hub_producer.hpp"
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

namespace pylabhub::processor
{

// ============================================================================
// ProcessorAPI
// ============================================================================

class ProcessorAPI
{
  public:
    /// Construct with RoleHostCore — single source of truth for all metrics
    /// and shutdown state. Must outlive this object.
    explicit ProcessorAPI(scripting::RoleHostCore &core)
        : core_(&core)
    {}

    // ── C++ host setters (never called from Python) ───────────────────────────

    void set_producer(hub::Producer *p) noexcept { producer_ = p; }
    void set_consumer(hub::Consumer *c) noexcept { consumer_ = c; }
    void set_messenger(hub::Messenger *m) noexcept { messenger_ = m; }
    void set_uid(std::string uid)   { uid_   = std::move(uid); }
    void set_name(std::string name) { name_  = std::move(name); }
    void set_in_channel(std::string c)  { in_channel_  = std::move(c); }
    void set_out_channel(std::string c) { out_channel_ = std::move(c); }
    void set_log_level(std::string l)   { log_level_   = std::move(l); }
    void set_script_dir(std::string d)  { script_dir_  = std::move(d); }
    void set_role_dir(std::string d)    { role_dir_    = std::move(d); }

    /// Input/output queue pointers — set by ProcessorScriptHost after queue creation.
    /// Raw pointers (non-owning); valid between start_role() and stop_role().
    void set_in_queue(const hub::QueueReader *q) noexcept { in_queue_.store(q, std::memory_order_release); }
    void set_out_queue(hub::QueueWriter *q) noexcept { out_queue_.store(q, std::memory_order_release); }

    // ── Python-accessible — identity / environment ────────────────────────────

    /// Processor UID (PROC-{NAME}-{8HEX}).
    [[nodiscard]] const std::string &uid()         const noexcept { return uid_; }
    /// Human-readable processor name.
    [[nodiscard]] const std::string &name()        const noexcept { return name_; }
    /// Input channel name.
    [[nodiscard]] const std::string &in_channel()  const noexcept { return in_channel_; }
    /// Output channel name.
    [[nodiscard]] const std::string &out_channel() const noexcept { return out_channel_; }
    /// Configured log level.
    [[nodiscard]] const std::string &log_level()   const noexcept { return log_level_; }
    /// Absolute path to the script directory.
    [[nodiscard]] const std::string &script_dir()  const noexcept { return script_dir_; }
    [[nodiscard]] const std::string &role_dir()    const noexcept { return role_dir_; }
    [[nodiscard]] std::string        logs_dir()    const { return role_dir_.empty() ? "" : role_dir_ + "/logs"; }
    [[nodiscard]] std::string        run_dir()     const { return role_dir_.empty() ? "" : role_dir_ + "/run";  }

    /// Log through the hub logger. level: "debug"|"info"|"warn"|"error".
    void log(const std::string &level, const std::string &msg);

    /// Request processor shutdown.  Safe to call from any callback.
    void stop();

    /// Latch the critical-error flag and trigger graceful shutdown.
    void set_critical_error();

    /// True if set_critical_error() has been called.
    [[nodiscard]] bool critical_error() const noexcept
        { return core_->is_critical_error(); }

    /// Return the persistent output flexzone Python object, or None.
    [[nodiscard]] py::object flexzone() const;

    // ── Python-accessible — producer-side ─────────────────────────────────────

    /// Broadcast bytes to all connected consumers on the ZMQ data socket.
    bool broadcast(py::bytes data);

    /// Send bytes to one specific consumer (ZMQ identity).
    bool send(const std::string &identity, py::bytes data);

    /// List of ZMQ identities of currently connected consumers.
    py::list consumers();

    /**
     * @brief Update the SHM flexzone BLAKE2b checksum (output side).
     *        Call after writing flexzone fields in on_init or on_process.
     */
    bool update_flexzone_checksum();

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
    [[nodiscard]] uint64_t out_slots_written()   const noexcept
        { return core_->out_written(); }
    [[nodiscard]] uint64_t out_drop_count()      const noexcept
        { return core_->drops(); }
    /// Processor is queue-driven (no deadline); always returns 0. Present for API symmetry.
    [[nodiscard]] uint64_t loop_overrun_count()  const noexcept { return 0; }
    /// Microseconds of active work (GIL acquire + on_process callback) in the last iteration.
    [[nodiscard]] uint64_t last_cycle_work_us()  const noexcept
        { return core_->last_cycle_work_us(); }
    /// Combined metrics dict: in/out DataBlock ContextMetrics + D4 binary-level counters.
    [[nodiscard]] py::dict metrics() const;

    // ── Python-accessible — queue state ───────────────────────────────────────

    /// Sequence number of the last consumed input slot. 0 until first slot.
    /// IC-04: semantics are transport-specific.
    ///   SHM  — ring-buffer slot index (0-based, wraps at capacity); NOT a global monotone counter.
    ///   ZMQ  — monotone wire sequence number (never wraps in practice).
    ///   Cross-transport comparison is meaningless; use only for detecting stalls within one session.
    [[nodiscard]] uint64_t last_seq() const noexcept;
    /// Input ring-buffer capacity (slot count), or 0 if not connected.
    [[nodiscard]] uint64_t in_capacity() const noexcept;
    /// Input queue policy string, or empty string if not connected.
    [[nodiscard]] std::string in_policy() const;
    /// Output ring-buffer capacity (slot count), or 0 if not connected.
    [[nodiscard]] uint64_t out_capacity() const noexcept;
    /// Output queue policy string, or empty string if not connected.
    [[nodiscard]] std::string out_policy() const;

    /// Enable/disable BLAKE2b checksum verification on input slots (SHM path only).
    /// No-op when the input path is ZMQ (TCP provides integrity).
    void set_verify_checksum(bool enable);

    // ── Python-accessible — custom metrics (HEP-CORE-0019) ─────────────────

    void report_metric(const std::string &key, double value);
    void report_metrics(const std::unordered_map<std::string, double> &kv);
    void clear_custom_metrics();

    // ── Internal — metrics snapshot (called from zmq thread) ────────────────

    [[nodiscard]] nlohmann::json snapshot_metrics_json() const;

    // ── Python-accessible — shutdown diagnostics ─────────────────────────────

    /// Returns reason the role stopped: "normal", "peer_dead", "hub_dead", or "critical_error".
    [[nodiscard]] std::string stop_reason() const noexcept;
    /// Number of ctrl-send messages dropped due to queue overflow (sum of in + out queues).
    [[nodiscard]] uint64_t ctrl_queue_dropped() const noexcept;

    // ── Python-accessible — shared spinlocks ──────────────────────────────────

    /**
     * @brief Return a ProcessorSpinLockPy proxy for spinlock at the given index.
     * @throws py::value_error if SHM is not configured.
     * @throws std::out_of_range if index >= spinlock_count().
     */
    py::object spinlock(std::size_t index);

    [[nodiscard]] uint32_t spinlock_count() const noexcept;

  private:
    hub::Producer    *producer_{nullptr};
    hub::Consumer    *consumer_{nullptr};
    hub::Messenger   *messenger_{nullptr};
    py::object       *flexzone_obj_{nullptr};


    std::atomic<const hub::QueueReader*> in_queue_{nullptr}; ///< Non-owning read side; set by ProcessorScriptHost
    std::atomic<hub::QueueWriter*> out_queue_{nullptr};

    std::string uid_;
    std::string name_;
    std::string in_channel_;
    std::string out_channel_;
    std::string log_level_;
    std::string script_dir_;
    std::string role_dir_;

    // RoleHostCore — single source of truth for all metrics and shutdown state.
    // Set by the constructor; always non-null.
    scripting::RoleHostCore *core_;

    mutable hub::InProcessSpinState                  metrics_spin_;
    std::unordered_map<std::string, double>          custom_metrics_;
    std::unordered_map<std::string, py::object>      inbox_cache_;
};

// ============================================================================
// ProcessorSpinLockPy — Python context-manager for SHM spinlocks
// ============================================================================

class ProcessorSpinLockPy
{
  public:
    explicit ProcessorSpinLockPy(hub::SharedSpinLock lock) : lock_(std::move(lock)) {}

    void lock()   { lock_.lock(); }
    void unlock() { lock_.unlock(); }

    bool try_lock_for(int timeout_ms) { return lock_.try_lock_for(timeout_ms); }

    [[nodiscard]] bool is_locked_by_current_process() const
        { return lock_.is_locked_by_current_process(); }

    // Python context manager protocol
    ProcessorSpinLockPy &enter() { lock_.lock(); return *this; }
    void exit(py::object /*exc_type*/, py::object /*exc_val*/, py::object /*exc_tb*/)
        { lock_.unlock(); }

  private:
    hub::SharedSpinLock lock_;
};

} // namespace pylabhub::processor
