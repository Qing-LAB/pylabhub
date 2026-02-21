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

    /**
     * @brief True if the given SHM flexzone content matches the consumer's
     *        accepted snapshot (set via update_flexzone_checksum()).
     */
    [[nodiscard]] bool is_fz_accepted(std::span<const std::byte> current_fz) const noexcept;

    // ── Python-accessible — common ─────────────────────────────────────────────

    /// Log through the hub logger. level: "debug"|"info"|"warn"|"error"
    void log(const std::string &level, const std::string &msg);

    /// This role's name (as declared in the JSON "roles" map).
    [[nodiscard]] const std::string &role_name() const noexcept { return role_name_; }

    /// The actor's unique identifier (from the JSON "actor.uid" field).
    [[nodiscard]] const std::string &uid() const noexcept { return actor_uid_; }

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

  private:
    hub::Producer    *producer_{nullptr};
    hub::Consumer    *consumer_{nullptr};
    std::atomic<bool>*shutdown_flag_{nullptr};
    std::function<void()> trigger_fn_{};

    std::string role_name_;
    std::string actor_uid_;

    bool slot_valid_{true};

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
