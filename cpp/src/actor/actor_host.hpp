#pragma once
/**
 * @file actor_host.hpp
 * @brief ActorHost — multi-role Python script lifecycle manager.
 *
 * One `ActorHost` hosts any number of named roles (producers and/or
 * consumers), each driven by Python callbacks registered via decorators
 * in the `pylabhub_actor` module.
 *
 * ## Script interface
 *
 * @code{.py}
 *   import pylabhub_actor as actor
 *
 *   # ── Producer role "raw_out" ───────────────────────────────────────
 *   @actor.on_init("raw_out")
 *   def raw_out_init(flexzone, api):
 *       """Called once; SHM ready; flexzone is writable."""
 *       flexzone.device_id   = 42
 *       api.update_flexzone_checksum()
 *
 *   @actor.on_write("raw_out")
 *   def write_raw(slot, flexzone, api) -> bool:
 *       """slot: writable ctypes struct — valid ONLY during this call."""
 *       slot.ts = time.time()
 *       return True   # True/None = commit, False = discard
 *
 *   @actor.on_message("raw_out")
 *   def raw_out_ctrl(sender: str, data: bytes, api): ...
 *
 *   @actor.on_stop("raw_out")
 *   def raw_out_stop(flexzone, api): ...
 *
 *   # ── Consumer role "cfg_in" ────────────────────────────────────────
 *   @actor.on_init("cfg_in")
 *   def cfg_in_init(flexzone, api):
 *       """flexzone from producer — read-only."""
 *
 *   @actor.on_read("cfg_in")
 *   def read_cfg(slot, flexzone, api, *, timed_out: bool = False):
 *       """slot: read-only ctypes struct — valid ONLY during this call."""
 *       if timed_out:
 *           api.send_ctrl(b"heartbeat")
 *           return
 *       process(slot.setpoint)
 *
 *   @actor.on_data("cfg_in")
 *   def zmq_data(data: bytes, api): ...
 *
 *   @actor.on_stop("cfg_in")
 *   def cfg_in_stop(flexzone, api): ...
 * @endcode
 *
 * ## Slot object lifetime
 *
 * Producer slot (`on_write`): writable `ctypes.Structure.from_buffer` into SHM.
 *   Valid ONLY during `on_write`. Do not store beyond the call.
 *
 * Consumer slot (`on_read`): zero-copy `from_buffer` on a read-only memoryview.
 *   Python field writes raise `TypeError`. Valid ONLY during `on_read`.
 *   Do not store beyond the call.
 *
 * `flexzone` is persistent for the role's lifetime and safe to store.
 */

#include "actor_api.hpp"
#include "actor_config.hpp"
#include "actor_schema.hpp"

#include "utils/hub_consumer.hpp"
#include "utils/hub_producer.hpp"
#include "utils/messenger.hpp"

#include <pybind11/embed.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

namespace pylabhub::actor
{

// ============================================================================
// ProducerRoleWorker — one named producer role
// ============================================================================

/**
 * @class ProducerRoleWorker
 * @brief Hosts hub::Producer and drives `on_write` callbacks for one role.
 */
class ProducerRoleWorker
{
  public:
    explicit ProducerRoleWorker(const std::string     &role_name,
                                 const RoleConfig      &role_cfg,
                                 const std::string     &actor_uid,
                                 hub::Messenger        &messenger,
                                 std::atomic<bool>     &shutdown,
                                 const py::object      &on_init_fn,
                                 const py::object      &on_write_fn,
                                 const py::object      &on_message_fn,
                                 const py::object      &on_stop_fn);
    ~ProducerRoleWorker();

    ProducerRoleWorker(const ProducerRoleWorker &) = delete;
    ProducerRoleWorker &operator=(const ProducerRoleWorker &) = delete;

    /**
     * @brief Build Python schema types and start the write loop thread.
     *        Calls `on_init(flexzone, api)` before the loop.
     */
    bool start();

    /**
     * @brief Signal stop and join the write loop thread.
     *        Calls `on_stop(flexzone, api)` after the loop exits.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

    /// Called by ActorRoleAPI::trigger_write() — wakes the write loop.
    void notify_trigger();

  private:
    std::string           role_name_;
    RoleConfig            role_cfg_;
    hub::Messenger       &messenger_;
    std::atomic<bool>    &shutdown_;

    std::optional<hub::Producer> producer_;

    // ── Schema + Python objects ───────────────────────────────────────────────
    SchemaSpec slot_spec_{};
    SchemaSpec fz_spec_{};
    py::object slot_type_{};
    py::object fz_type_{};
    py::object fz_inst_{};   ///< Persistent writable flexzone ctypes/numpy instance
    py::object fz_mv_{};     ///< Backing memoryview for fz_inst_
    size_t     schema_slot_size_{0};
    size_t     schema_fz_size_{0};
    bool       has_fz_{false};

    // ── ZMQ-only mode slot buffer ─────────────────────────────────────────────
    std::vector<std::byte> zmq_slot_buf_{};

    // ── Callbacks ─────────────────────────────────────────────────────────────
    py::object py_on_init_{};
    py::object py_on_write_{};
    py::object py_on_message_{};
    py::object py_on_stop_{};

    // ── API proxy ─────────────────────────────────────────────────────────────
    ActorRoleAPI api_{};
    py::object   api_obj_{};

    // ── Loop control ──────────────────────────────────────────────────────────
    std::atomic<bool>      running_{false};
    std::thread            loop_thread_{};
    std::mutex             trigger_mu_{};
    std::condition_variable trigger_cv_{};
    bool                   trigger_pending_{false};

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool       build_slot_types_();
    void       print_layout_() const;
    py::object make_slot_view_(void *data, size_t size) const;
    void       run_loop_shm();
    void       run_loop_zmq();
    void       call_on_init();
    void       call_on_stop();
    bool       call_on_write_(py::object &slot); ///< true = commit, false = discard
};

// ============================================================================
// ConsumerRoleWorker — one named consumer role
// ============================================================================

/**
 * @class ConsumerRoleWorker
 * @brief Hosts hub::Consumer and drives `on_read` callbacks for one role.
 */
class ConsumerRoleWorker
{
  public:
    explicit ConsumerRoleWorker(const std::string     &role_name,
                                 const RoleConfig      &role_cfg,
                                 const std::string     &actor_uid,
                                 hub::Messenger        &messenger,
                                 std::atomic<bool>     &shutdown,
                                 const py::object      &on_init_fn,
                                 const py::object      &on_read_fn,
                                 const py::object      &on_data_fn,
                                 const py::object      &on_stop_fn);
    ~ConsumerRoleWorker();

    ConsumerRoleWorker(const ConsumerRoleWorker &) = delete;
    ConsumerRoleWorker &operator=(const ConsumerRoleWorker &) = delete;

    bool start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

  private:
    std::string           role_name_;
    RoleConfig            role_cfg_;
    hub::Messenger       &messenger_;
    std::atomic<bool>    &shutdown_;

    std::optional<hub::Consumer> consumer_;

    SchemaSpec slot_spec_{};
    SchemaSpec fz_spec_{};
    py::object slot_type_{};
    py::object fz_type_{};
    py::object fz_inst_{};   ///< Persistent read-only flexzone ctypes/numpy instance
    py::object fz_mv_{};     ///< Backing read-only memoryview for fz_inst_
    size_t     schema_slot_size_{0};
    size_t     schema_fz_size_{0};
    bool       has_fz_{false};

    py::object py_on_init_{};
    py::object py_on_read_{};
    py::object py_on_data_{};
    py::object py_on_stop_{};

    ActorRoleAPI api_{};
    py::object   api_obj_{};

    std::atomic<bool> running_{false};
    std::thread       loop_thread_{};

    bool       build_slot_types_();
    void       print_layout_() const;
    py::object make_slot_view_readonly_(const void *data, size_t size) const;
    void       run_loop_shm();
    void       wire_zmq_callback();
    void       call_on_init();
    void       call_on_stop();
    void       call_on_read_timeout_();  ///< on_read(None, fz, api, timed_out=True)
};

// ============================================================================
// ActorHost — multi-role entry point
// ============================================================================

/**
 * @class ActorHost
 * @brief Owns all active roles and coordinates their lifecycle.
 *
 * Usage:
 * @code{.cpp}
 *   ActorHost host(config, messenger);
 *   if (!host.load_script()) return 1;
 *   if (!host.start())       return 1;
 *   host.wait_for_shutdown();
 *   host.stop();
 * @endcode
 */
class ActorHost
{
  public:
    explicit ActorHost(const ActorConfig &config,
                        hub::Messenger    &messenger);
    ~ActorHost();

    ActorHost(const ActorHost &) = delete;
    ActorHost &operator=(const ActorHost &) = delete;

    /**
     * @brief Import the Python script; read which roles are registered via decorators.
     *        In verbose mode prints per-role slot/flexzone layout to stdout.
     */
    bool load_script(bool verbose = false);

    /**
     * @brief Start all roles that have registered callbacks.
     *        Calls per-role on_init before starting each loop.
     */
    bool start();

    /**
     * @brief Stop all active roles (join threads, call on_stop per role).
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    /**
     * @brief Block until the global shutdown flag is set (via api.stop() or
     *        external signal_shutdown()).
     */
    void wait_for_shutdown();

    /**
     * @brief Set the shutdown flag — e.g. called from a SIGINT handler.
     */
    void signal_shutdown() noexcept { shutdown_.store(true); }

    /**
     * @brief Print a summary of configured roles and which are activated.
     *        Used by --list-roles.
     */
    void print_role_summary() const;

  private:
    ActorConfig    config_;
    hub::Messenger &messenger_;
    std::atomic<bool> shutdown_{false};

    std::unordered_map<std::string,
        std::unique_ptr<ProducerRoleWorker>> producers_;
    std::unordered_map<std::string,
        std::unique_ptr<ConsumerRoleWorker>> consumers_;

    bool script_loaded_{false};
};

} // namespace pylabhub::actor
