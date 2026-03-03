#pragma once
/**
 * @file producer_api.hpp
 * @brief ProducerAPI — Python-facing proxy passed to producer script callbacks.
 *
 * ## Python usage
 *
 * @code{.py}
 *   import pylabhub_producer as prod
 *
 *   def on_init(api: prod.ProducerAPI):
 *       api.log('info', f"Producer {api.uid()} starting")
 *
 *   def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
 *       # out_slot  — ctypes/numpy writable view of the output SHM slot
 *       # flexzone  — persistent output flexzone ctypes struct, or None
 *       # messages  — list of (sender: str, data: bytes) from ZMQ peers
 *       # api       — ProducerAPI proxy
 *       out_slot.value = 42.0
 *       return True  # True/None=commit; False=skip
 *
 *   def on_stop(api: prod.ProducerAPI):
 *       api.log('info', "Producer stopping")
 * @endcode
 */

#include "utils/hub_producer.hpp"
#include "utils/shared_memory_spinlock.hpp"

#include <pybind11/pybind11.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace py = pybind11;

namespace pylabhub::producer
{

// ============================================================================
// ProducerAPI
// ============================================================================

class ProducerAPI
{
  public:
    ProducerAPI() = default;

    // ── C++ host setters (never called from Python) ───────────────────────────

    void set_producer(hub::Producer *p) noexcept { producer_ = p; }
    void set_uid(std::string uid)    { uid_        = std::move(uid); }
    void set_name(std::string name)  { name_       = std::move(name); }
    void set_channel(std::string c)  { channel_    = std::move(c); }
    void set_log_level(std::string l){ log_level_  = std::move(l); }
    void set_script_dir(std::string d){ script_dir_ = std::move(d); }

    void set_shutdown_flag(std::atomic<bool> *f) noexcept { shutdown_flag_ = f; }
    void set_shutdown_requested(std::atomic<bool> *f) noexcept { shutdown_requested_ = f; }
    void set_flexzone_obj(py::object *fz) noexcept { flexzone_obj_ = fz; }

    void increment_script_errors() noexcept { ++script_errors_; }
    void increment_out_written() noexcept
        { out_slots_written_.fetch_add(1, std::memory_order_relaxed); }
    void increment_drops() noexcept
        { out_drops_.fetch_add(1, std::memory_order_relaxed); }

    // ── Python-accessible — identity / environment ────────────────────────────

    [[nodiscard]] const std::string &uid()        const noexcept { return uid_; }
    [[nodiscard]] const std::string &name()       const noexcept { return name_; }
    [[nodiscard]] const std::string &channel()    const noexcept { return channel_; }
    [[nodiscard]] const std::string &log_level()  const noexcept { return log_level_; }
    [[nodiscard]] const std::string &script_dir() const noexcept { return script_dir_; }

    void log(const std::string &level, const std::string &msg);
    void stop();
    void set_critical_error();
    [[nodiscard]] bool critical_error() const noexcept { return critical_error_.load(); }

    /// Return the persistent output flexzone Python object, or None.
    [[nodiscard]] py::object flexzone() const;

    // ── Python-accessible — producer-side ─────────────────────────────────────

    bool broadcast(py::bytes data);
    bool send(const std::string &identity, py::bytes data);
    py::list consumers();
    bool update_flexzone_checksum();

    // ── Python-accessible — diagnostics ──────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept { return script_errors_; }
    [[nodiscard]] uint64_t out_slots_written()  const noexcept
        { return out_slots_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t out_drop_count()     const noexcept
        { return out_drops_.load(std::memory_order_relaxed); }

    // ── Python-accessible — shared spinlocks ──────────────────────────────────

    py::object spinlock(std::size_t index);
    [[nodiscard]] uint32_t spinlock_count() const noexcept;

  private:
    hub::Producer    *producer_{nullptr};
    std::atomic<bool>*shutdown_flag_{nullptr};
    std::atomic<bool>*shutdown_requested_{nullptr};
    py::object       *flexzone_obj_{nullptr};

    std::atomic<bool> critical_error_{false};

    std::string uid_;
    std::string name_;
    std::string channel_;
    std::string log_level_;
    std::string script_dir_;

    uint64_t              script_errors_{0};
    std::atomic<uint64_t> out_slots_written_{0};
    std::atomic<uint64_t> out_drops_{0};
};

// ============================================================================
// ProducerSpinLockPy — Python context-manager for SHM spinlocks
// ============================================================================

class ProducerSpinLockPy
{
  public:
    explicit ProducerSpinLockPy(hub::SharedSpinLock lock) : lock_(std::move(lock)) {}

    void lock()   { lock_.lock(); }
    void unlock() { lock_.unlock(); }

    bool try_lock_for(int timeout_ms) { return lock_.try_lock_for(timeout_ms); }

    [[nodiscard]] bool is_locked_by_current_process() const
        { return lock_.is_locked_by_current_process(); }

    ProducerSpinLockPy &enter() { lock_.lock(); return *this; }
    void exit(py::object /*exc_type*/, py::object /*exc_val*/, py::object /*exc_tb*/)
        { lock_.unlock(); }

  private:
    hub::SharedSpinLock lock_;
};

} // namespace pylabhub::producer
