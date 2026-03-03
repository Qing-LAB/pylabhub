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
 *       #             or None on timeout (if timeout_ms > 0)
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

#include <pybind11/pybind11.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace py = pybind11;

namespace pylabhub::consumer
{

// ============================================================================
// ConsumerAPI
// ============================================================================

class ConsumerAPI
{
  public:
    ConsumerAPI() = default;

    // ── C++ host setters (never called from Python) ───────────────────────────

    void set_consumer(hub::Consumer *c) noexcept { consumer_ = c; }
    void set_uid(std::string uid)    { uid_        = std::move(uid); }
    void set_name(std::string name)  { name_       = std::move(name); }
    void set_channel(std::string c)  { channel_    = std::move(c); }
    void set_log_level(std::string l){ log_level_  = std::move(l); }
    void set_script_dir(std::string d){ script_dir_ = std::move(d); }

    void set_shutdown_flag(std::atomic<bool> *f) noexcept { shutdown_flag_ = f; }
    void set_shutdown_requested(std::atomic<bool> *f) noexcept { shutdown_requested_ = f; }

    void increment_script_errors() noexcept { ++script_errors_; }
    void increment_in_received() noexcept
        { in_slots_received_.fetch_add(1, std::memory_order_relaxed); }

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

    // ── Python-accessible — diagnostics ──────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept { return script_errors_; }
    [[nodiscard]] uint64_t in_slots_received()  const noexcept
        { return in_slots_received_.load(std::memory_order_relaxed); }

  private:
    hub::Consumer    *consumer_{nullptr};
    std::atomic<bool>*shutdown_flag_{nullptr};
    std::atomic<bool>*shutdown_requested_{nullptr};

    std::atomic<bool> critical_error_{false};

    std::string uid_;
    std::string name_;
    std::string channel_;
    std::string log_level_;
    std::string script_dir_;

    uint64_t              script_errors_{0};
    std::atomic<uint64_t> in_slots_received_{0};
};

} // namespace pylabhub::consumer
