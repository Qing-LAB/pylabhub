#pragma once
/**
 * @file actor_api.hpp
 * @brief ActorRoleAPI — thin proxy passed to Python script callbacks for one role.
 *
 * One `ActorRoleAPI` is created per active role and passed (by reference) to
 * every callback of that role. All methods dispatch immediately to C++ without
 * any Python buffering.
 *
 * ## Python usage (producer role)
 * @code{.py}
 *   import pylabhub_actor as actor
 *
 *   @actor.on_write("raw_out")
 *   def write_raw(slot, flexzone, api) -> bool:
 *       api.log('info', "writing")
 *       api.broadcast(b"extra")
 *       api.update_flexzone_checksum()
 *       return True
 * @endcode
 *
 * ## Python usage (consumer role)
 * @code{.py}
 *   @actor.on_read("cfg_in")
 *   def read_cfg(slot, flexzone, api, *, timed_out: bool = False):
 *       if timed_out:
 *           api.send_ctrl(b"heartbeat")   # periodic liveness ping
 *           return
 *       if not api.slot_valid():
 *           api.log('warn', "slot checksum failed")
 *           return
 *       process(slot)
 * @endcode
 *
 * ## Object lifetime contract
 *
 * `slot` (producer) — valid ONLY during `on_write`. Writable `from_buffer` into SHM.
 * `slot` (consumer) — valid ONLY during `on_read`. Zero-copy `from_buffer` on
 *                     read-only memoryview; field writes raise TypeError.
 * `flexzone`        — persistent for the role's lifetime; safe to store and read.
 * `api`             — stateless proxy; safe to store (though rarely needed).
 */

#include "utils/hub_consumer.hpp"
#include "utils/hub_producer.hpp"
#include "utils/shared_memory_spinlock.hpp"

#include <pybind11/pybind11.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace py = pybind11;

namespace pylabhub::actor
{

/**
 * @class ActorRoleAPI
 * @brief Proxy to C++ actor services for one named role.
 *
 * Replaces the old ActorAPI. One instance per active role.
 * The role_name and actor_uid are set once at role startup and
 * remain constant for the role's lifetime.
 */
class ActorRoleAPI
{
  public:
    ActorRoleAPI() = default;

    // ── Called by C++ host — not from Python ──────────────────────────────────

    void set_producer(hub::Producer *p) noexcept { producer_ = p; }
    void set_consumer(hub::Consumer *c) noexcept { consumer_ = c; }
    void set_role_name(std::string name)         { role_name_ = std::move(name); }
    void set_actor_uid(std::string uid)          { actor_uid_ = std::move(uid); }
    void set_shutdown_flag(std::atomic<bool> *f) noexcept { shutdown_flag_ = f; }

    // ── PylabhubEnv setters (called by C++ host at role startup) ──────────────
    void set_actor_name(std::string n) { actor_name_  = std::move(n); }
    void set_channel   (std::string c) { channel_     = std::move(c); }
    void set_broker    (std::string b) { broker_      = std::move(b); }
    void set_kind_str  (std::string k) { kind_str_    = std::move(k); }
    void set_log_level (std::string l) { log_level_   = std::move(l); }
    void set_script_dir(std::string d) { script_dir_  = std::move(d); }

    /**
     * @brief Set per-call slot validity.
     *        C++ sets this before each on_read() based on checksum result.
     *        True = checksum passed (or not enforced); False = failed + "pass" policy.
     */
    void set_slot_valid(bool v) noexcept { slot_valid_ = v; }

    /**
     * @brief Set the trigger function for interval_ms == -1 producers.
     *        The write loop waits for this signal.
     */
    void set_trigger_fn(std::function<void()> fn) { trigger_fn_ = std::move(fn); }

    // ── C++ internal diagnostic write interface (never exposed to Python) ──────
    //
    // All writes to RoleMetrics go through these methods — actor_host.cpp never
    // touches metrics_ fields directly.  Adding a new metric = one field in
    // RoleMetrics + one setter here + one read-only getter in the Python section.

    /// Called under the GIL in every Python callback catch block.
    void increment_script_errors()           noexcept { ++metrics_.script_errors; }

    /// Called in the overrun branch of the producer write-loop timing block
    /// (deadline was already past — no sleep was needed).
    void increment_loop_overruns()           noexcept { ++metrics_.loop_overruns; }

    /// Called after each successful write cycle with the elapsed active-work time
    /// (acquire + on_write + commit), measured from post-sleep wakeup.
    void set_last_cycle_work_us(uint64_t us) noexcept { metrics_.last_cycle_work_us = us; }

    /// Called by the C++ host when a role restarts to clear stale per-run counters.
    /// Resets ALL RoleMetrics fields: script_errors, loop_overruns, and
    /// last_cycle_work_us. All three are per-run values — they reflect the health
    /// of the current execution instance only. Clearing them on restart ensures
    /// script threshold checks (e.g. `api.script_error_count() > N`) reflect only
    /// the current run, not accumulated errors from previous restart iterations.
    void reset_all_role_run_metrics()        noexcept { metrics_.reset(); }

    // ── C++ validation helpers (internal; called from actor_host.cpp) ──────────

    /**
     * @brief True if the given SHM flexzone content matches the consumer's
     *        accepted snapshot (set via accept_flexzone_state()).
     *        Used by run_loop_shm to skip checksum verification when the
     *        flexzone has not changed since the consumer last accepted it.
     */
    [[nodiscard]] bool is_fz_accepted(std::span<const std::byte> current_fz) const noexcept;

    // ── Python-accessible — common ─────────────────────────────────────────────

    /// Log through the hub logger. level: "debug"|"info"|"warn"|"error"
    void log(const std::string &level, const std::string &msg);

    /// This role's name (as declared in the JSON "roles" map).
    [[nodiscard]] const std::string &role_name() const noexcept { return role_name_; }

    /// The actor's unique identifier (from the JSON "actor.uid" field).
    [[nodiscard]] const std::string &uid() const noexcept { return actor_uid_; }

    /// Human-readable actor name (from "actor.name" in config).
    [[nodiscard]] const std::string &actor_name() const noexcept { return actor_name_; }

    /// Channel name this role operates on.
    [[nodiscard]] const std::string &channel() const noexcept { return channel_; }

    /// Configured broker endpoint for this role (informational; may not be live).
    [[nodiscard]] const std::string &broker() const noexcept { return broker_; }

    /// Role kind: "producer" or "consumer".
    [[nodiscard]] const std::string &kind() const noexcept { return kind_str_; }

    /// Configured log level (debug/info/warn/error).
    [[nodiscard]] const std::string &log_level() const noexcept { return log_level_; }

    /// Absolute path to the directory containing this actor's Python script.
    [[nodiscard]] const std::string &script_dir() const noexcept { return script_dir_; }

    /// Request actor shutdown (all roles). Safe to call from any callback.
    void stop();

    // ── Python-accessible — producer ──────────────────────────────────────────

    /// Broadcast bytes to all connected consumers on the ZMQ data socket.
    bool broadcast(py::bytes data);

    /// Send bytes to one specific consumer (ZMQ identity string).
    bool send(const std::string &identity, py::bytes data);

    /// List of ZMQ identity strings of currently connected consumers.
    py::list consumers();

    /**
     * @brief Notify the write loop to produce one slot.
     *        Only meaningful when interval_ms == -1 (event-driven mode).
     *        In all other modes this is a no-op.
     */
    void trigger_write();

    /**
     * @brief Update the SHM flexzone BLAKE2b checksum (producer side).
     *        Should be called from on_init and after any write that modifies
     *        flexzone fields.
     * @return true on success; false if SHM unavailable or flexzone size == 0.
     */
    bool update_flexzone_checksum();

    // ── Python-accessible — consumer ──────────────────────────────────────────

    /// Send a ctrl frame to the producer.
    bool send_ctrl(py::bytes data);

    /**
     * @brief True when the current slot passed its checksum check.
     *        False when checksum failed and on_checksum_fail="pass" applies.
     *        Always true when slot checksum is not enforced.
     */
    [[nodiscard]] bool slot_valid() const noexcept { return slot_valid_; }

    /**
     * @brief Verify the SHM flexzone using the stored BLAKE2b checksum.
     * @return true if checksum matches; false on mismatch or SHM unavailable.
     */
    [[nodiscard]] bool verify_flexzone_checksum();

    /**
     * @brief Accept the current SHM flexzone content as valid (consumer side).
     *        Stores a snapshot of the flexzone bytes. Subsequent actor-level
     *        flexzone checks compare against this snapshot (content equality).
     * @return true on success; false if SHM unavailable or flexzone size == 0.
     */
    bool accept_flexzone_state();

    // ── Python-accessible — diagnostics (read-only) ───────────────────────────
    //
    // Script can observe its own health and react (e.g. call api.stop()), but
    // cannot reset or write these counters — they are collected by the C++ host
    // about the script, not data belonging to the script.

    /// Total Python exceptions caught in any callback (on_init, on_write,
    /// on_read, on_data, on_message, on_stop). Resets on role restart.
    [[nodiscard]] uint64_t script_error_count()   const noexcept { return metrics_.script_errors; }

    /// Write cycles where the deadline was already past at timing check entry
    /// (no sleep needed). Indicates the write body exceeded interval_ms.
    /// Always 0 for interval_ms == 0 or -1 modes. Resets on role restart.
    [[nodiscard]] uint64_t loop_overrun_count()   const noexcept { return metrics_.loop_overruns; }

    /// Elapsed active-work time (µs) of the most recently completed write cycle:
    /// acquire_write_slot + on_write callback + commit + checksum.
    /// 0 until the first write completes. Producer-only; always 0 for consumers.
    [[nodiscard]] uint64_t last_cycle_work_us()   const noexcept { return metrics_.last_cycle_work_us; }

    // ── Python-accessible — shared spinlocks ──────────────────────────────────

    /**
     * @brief Return a SharedSpinLockPy proxy for the spinlock at the given index.
     *
     * Spinlocks are per-channel and cross-process: both producer and consumer
     * share the same 8 spinlock slots in the SHM header. Index must be in
     * [0, spinlock_count()). The spinlock object supports the Python context
     * manager protocol (`with api.spinlock(0): ...`) and explicit lock/unlock.
     *
     * @throws py::error_already_set (RuntimeError) if SHM is not configured.
     * @throws std::out_of_range if index >= spinlock_count().
     */
    py::object spinlock(std::size_t index);

    /// Number of available shared spinlock slots (always 8 in current layout).
    uint32_t spinlock_count() const noexcept;

    /**
     * @brief Returns a dict of all timing metrics for this role.
     *
     * Combines:
     *  - Domains 2+3: ContextMetrics from DataBlock Pimpl (acquire/release timing, loop scheduling)
     *  - Domain 4: ActorRoleAPI::RoleMetrics (script_error_count)
     *
     * Keys: context_elapsed_us, iteration_count, last_iteration_us, max_iteration_us,
     *       last_slot_wait_us, overrun_count, last_slot_work_us, period_ms,
     *       script_error_count.
     *
     * See HEP-CORE-0008 §6.1 for the full specification.
     */
    py::dict metrics() const;

  private:
    hub::Producer    *producer_{nullptr};
    hub::Consumer    *consumer_{nullptr};
    std::atomic<bool>*shutdown_flag_{nullptr};
    std::function<void()> trigger_fn_{};

    std::string role_name_;
    std::string actor_uid_;

    // ── PylabhubEnv fields (set once at role startup via F1 setters) ──────────
    std::string actor_name_;  ///< ActorConfig::actor_name
    std::string channel_;     ///< RoleConfig::channel
    std::string broker_;      ///< RoleConfig::broker (configured, may not be live)
    std::string kind_str_;    ///< "producer" or "consumer"
    std::string log_level_;   ///< ActorConfig::log_level
    std::string script_dir_;  ///< Absolute dir containing the Python script

    bool slot_valid_{true};

    /// All diagnostic counters in one place.  C++ host writes through the
    /// increment_*/set_* methods above; never access fields directly.
    struct RoleMetrics
    {
        uint64_t script_errors{0};       ///< Python exceptions in callbacks
        uint64_t loop_overruns{0};       ///< Write-loop deadline overruns
        uint64_t last_cycle_work_us{0};  ///< µs of active work, last write cycle
        void reset() noexcept { *this = RoleMetrics{}; }
    };
    RoleMetrics metrics_{};

    /// Consumer-side: accepted flexzone content snapshot (for is_fz_accepted).
    std::vector<std::byte> consumer_fz_accepted_{};
    bool                   consumer_fz_has_accepted_{false};
};

// ============================================================================
// SharedSpinLockPy — Python-facing wrapper for hub::SharedSpinLock
// ============================================================================

/**
 * @class SharedSpinLockPy
 * @brief Python context-manager and lock/unlock wrapper for a SHM spinlock.
 *
 * Returned by `ActorRoleAPI.spinlock(idx)`. Lifetime: valid as long as the
 * actor's SHM region is mapped (i.e. while the role is running).
 *
 * ## Python usage
 * @code{.py}
 *   # Context manager (preferred — always releases even on exception)
 *   with api.spinlock(0):
 *       flexzone.counter += 1
 *       api.update_flexzone_checksum()
 *
 *   # Explicit lock/unlock
 *   lk = api.spinlock(1)
 *   lk.lock()
 *   try:
 *       flexzone.calibration = new_value
 *   finally:
 *       lk.unlock()
 *
 *   # Non-blocking attempt
 *   lk = api.spinlock(2)
 *   if lk.try_lock_for(timeout_ms=100):
 *       try:
 *           ...
 *       finally:
 *           lk.unlock()
 * @endcode
 *
 * @note The underlying SharedSpinLockState lives in SHM for the actor lifetime.
 *       Calling lock()/unlock() from Python dispatches to C++ SharedSpinLock
 *       which uses PID+TID ownership semantics — safe for cross-process use.
 */
class SharedSpinLockPy
{
  public:
    /// Construct from a SharedSpinLock copy (both refer to the same SHM state).
    explicit SharedSpinLockPy(hub::SharedSpinLock lock) : lock_(std::move(lock)) {}

    /// Acquire the spinlock (blocking until acquired).
    void lock() { lock_.lock(); }

    /// Release the spinlock. Throws if not held by the current process/thread.
    void unlock() { lock_.unlock(); }

    /**
     * @brief Try to acquire within @p timeout_ms milliseconds.
     * @return True if acquired; false if timed out.
     */
    bool try_lock_for(int timeout_ms) { return lock_.try_lock_for(timeout_ms); }

    /// True if the spinlock is currently held by this process.
    bool is_locked_by_current_process() const { return lock_.is_locked_by_current_process(); }

    // Python context manager protocol: `with api.spinlock(0): ...`
    SharedSpinLockPy &enter() { lock_.lock(); return *this; }
    void exit(py::object /*exc_type*/, py::object /*exc_val*/, py::object /*exc_tb*/)
    {
        lock_.unlock();
    }

  private:
    hub::SharedSpinLock lock_;
};

} // namespace pylabhub::actor
