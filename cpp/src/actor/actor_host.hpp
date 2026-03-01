#pragma once
/**
 * @file actor_host.hpp
 * @brief ActorHost — multi-role Python script lifecycle manager.
 *
 * One `ActorHost` hosts any number of named roles (producers and/or
 * consumers), each driven by a single Python `on_iteration` callback
 * looked up from an imported Python module.
 *
 * ## Script interface (module-convention)
 *
 * @code{.py}
 *   # my_actor/sensor_node.py
 *   import pylabhub_actor as actor
 *
 *   def on_init(api: actor.ActorRoleAPI):
 *       api.log('info', "starting")
 *
 *   def on_iteration(slot, flexzone, messages, api: actor.ActorRoleAPI) -> bool:
 *       """Called every loop iteration for ALL roles sharing this module.
 *
 *       slot      — ctypes struct into SHM slot (writable=producer,
 *                   read-only=consumer), or None (Messenger trigger / timeout).
 *       flexzone  — persistent ctypes struct for this role's flexzone, or None.
 *       messages  — list of (sender: str, data: bytes) drained from the
 *                   incoming queue since the last iteration.
 *       api       — ActorRoleAPI proxy (this role's context).
 *
 *       Producer return: True/None = commit; False = discard.
 *       Consumer return: ignored.
 *       """
 *       if slot is not None:
 *           slot.ts = ...
 *       for sender, data in messages:
 *           api.broadcast(data)
 *       return True
 *
 *   def on_stop(api: actor.ActorRoleAPI):
 *       api.log('info', "stopping")
 * @endcode
 *
 * ## Slot object lifetime
 *
 * Producer slot: writable `ctypes.Structure.from_buffer` into SHM.
 *   Valid ONLY during `on_iteration`. Do not store beyond the call.
 *
 * Consumer slot: zero-copy `from_buffer` on a read-only memoryview.
 *   Python field writes raise `TypeError`. Valid ONLY during `on_iteration`.
 *   Do not store beyond the call.
 *
 * `flexzone` is persistent for the role's lifetime and safe to store.
 *
 * ## Thread model (Phase 2)
 *
 * Each role worker runs exactly two threads:
 *   - `loop_thread_` — drives SHM acquire/release and calls Python (`on_iteration`).
 *   - `zmq_thread_`  — polls ZMQ sockets via `zmq_poll`; calls `handle_*_events_nowait()`
 *                      on `Producer` / `Consumer` (embedded mode). Callbacks push
 *                      `IncomingMessage` objects into `incoming_queue_` and notify
 *                      `incoming_cv_`. The loop thread drains the queue before each
 *                      GIL acquisition, ensuring a single-threaded Python call path.
 *
 * Producer / Consumer are started in embedded mode (`start_embedded()`): their internal
 * peer_thread / data_thread / ctrl_thread are NOT launched. The actor's `zmq_thread_`
 * owns all ZMQ socket I/O instead.
 *
 * `iteration_count_` (atomic uint64) is incremented by the loop thread after each
 * iteration. The ZMQ thread reads it to detect iteration progress (reserved for
 * heartbeat sending).
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
#include <deque>
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
 * @brief Hosts hub::Producer and drives `on_iteration` callbacks for one role.
 */
class ProducerRoleWorker
{
  public:
    /// One ZMQ message from the producer's zmq_thread_, queued for the loop thread.
    struct IncomingMessage
    {
        std::string           sender;
        std::vector<std::byte> data;
    };

    explicit ProducerRoleWorker(const std::string     &role_name,
                                 const RoleConfig      &role_cfg,
                                 const std::string     &actor_uid,
                                 const ActorAuthConfig &auth,
                                 std::atomic<bool>     &shutdown,
                                 const py::module_     &script_module);
    ~ProducerRoleWorker();

    ProducerRoleWorker(const ProducerRoleWorker &) = delete;
    ProducerRoleWorker &operator=(const ProducerRoleWorker &) = delete;

    /**
     * @brief Build Python schema types and start the write loop thread.
     *        Calls `on_init(api)` before the loop.
     */
    bool start();

    /**
     * @brief Signal stop and join the write loop thread.
     *        Calls `on_stop(api)` after the loop exits.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

    // ── PylabhubEnv context wiring (F2) ─────────────────────────────────────
    // Called by ActorHost::start() after construction, before start().
    void set_env_actor_name(const std::string &n) { api_.set_actor_name(n); }
    void set_env_log_level (const std::string &l) { api_.set_log_level(l);  }
    void set_env_script_dir(const std::string &d) { api_.set_script_dir(d); }

  private:
    static constexpr std::size_t kMaxIncomingQueue{256};

    std::string           role_name_;
    RoleConfig            role_cfg_;
    ActorAuthConfig       auth_;
    hub::Messenger        messenger_;
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

    // ── Callbacks (looked up from script_module at construction) ──────────────
    py::object py_on_iteration_{};
    py::object py_on_init_{};
    py::object py_on_stop_{};

    // ── API proxy ─────────────────────────────────────────────────────────────
    ActorRoleAPI api_{};
    py::object   api_obj_{};

    // ── Loop control ──────────────────────────────────────────────────────────
    std::atomic<bool>      running_{false};
    std::thread            loop_thread_{};

    // ── ZMQ thread (Phase 2 — embedded mode) ─────────────────────────────────
    std::thread            zmq_thread_{};
    std::atomic<uint64_t>  iteration_count_{0};

    // ── Incoming ZMQ message queue (zmq_thread → loop_thread) ────────────────
    std::deque<IncomingMessage> incoming_queue_;
    std::mutex                  incoming_mu_;
    std::condition_variable     incoming_cv_;

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool       build_slot_types_();
    void       print_layout_() const;
    py::object make_slot_view_(void *data, size_t size) const;
    void       run_loop_shm();
    void       run_loop_messenger();
    void       run_zmq_thread_();
    void       call_on_init();
    void       call_on_stop();

    /// Returns true = commit, false = discard.
    bool call_on_iteration_(py::object &slot, py::object &fz,
                             py::list &msgs);

    [[nodiscard]] std::vector<IncomingMessage> drain_incoming_queue_();
    py::list build_messages_list_(std::vector<IncomingMessage> &msgs);

    /// One step of write-loop pacing: handle deadline sleep/overrun.
    /// Mutates `next_deadline` in place. Returns false if the loop should exit.
    [[nodiscard]] bool step_write_deadline_(
        std::chrono::steady_clock::time_point &next_deadline);
};

// ============================================================================
// ConsumerRoleWorker — one named consumer role
// ============================================================================

/**
 * @class ConsumerRoleWorker
 * @brief Hosts hub::Consumer and drives `on_iteration` callbacks for one role.
 */
class ConsumerRoleWorker
{
  public:
    /// One ZMQ message from the consumer's zmq_thread_, queued for the loop thread.
    struct IncomingMessage
    {
        std::string           sender;
        std::vector<std::byte> data;
    };

    explicit ConsumerRoleWorker(const std::string     &role_name,
                                 const RoleConfig      &role_cfg,
                                 const std::string     &actor_uid,
                                 const ActorAuthConfig &auth,
                                 std::atomic<bool>     &shutdown,
                                 const py::module_     &script_module);
    ~ConsumerRoleWorker();

    ConsumerRoleWorker(const ConsumerRoleWorker &) = delete;
    ConsumerRoleWorker &operator=(const ConsumerRoleWorker &) = delete;

    bool start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

    // ── PylabhubEnv context wiring (F2) ─────────────────────────────────────
    void set_env_actor_name(const std::string &n) { api_.set_actor_name(n); }
    void set_env_log_level (const std::string &l) { api_.set_log_level(l);  }
    void set_env_script_dir(const std::string &d) { api_.set_script_dir(d); }

  private:
    static constexpr std::size_t kMaxIncomingQueue{256};

    std::string           role_name_;
    RoleConfig            role_cfg_;
    ActorAuthConfig       auth_;
    hub::Messenger        messenger_;
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

    py::object py_on_iteration_{};
    py::object py_on_init_{};
    py::object py_on_stop_{};

    ActorRoleAPI api_{};
    py::object   api_obj_{};

    std::atomic<bool> running_{false};
    std::thread       loop_thread_{};

    // ── ZMQ thread (Phase 2 — embedded mode) ─────────────────────────────────
    std::thread            zmq_thread_{};
    std::atomic<uint64_t>  iteration_count_{0};

    // ── Incoming ZMQ message queue (zmq_thread → loop_thread) ────────────────
    std::deque<IncomingMessage> incoming_queue_;
    std::mutex                  incoming_mu_;
    std::condition_variable     incoming_cv_;

    bool       build_slot_types_();
    void       print_layout_() const;
    py::object make_slot_view_readonly_(const void *data, size_t size) const;
    void       run_loop_shm();
    void       run_loop_messenger();
    void       run_zmq_thread_();
    void       call_on_init();
    void       call_on_stop();

    /// Consumer return value is ignored (always passes).
    void call_on_iteration_(py::object &slot, py::object &fz, py::list &msgs);

    [[nodiscard]] std::vector<IncomingMessage> drain_incoming_queue_();
    py::list build_messages_list_(std::vector<IncomingMessage> &msgs);
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
 *   ActorHost host(config);
 *   if (!host.load_script()) return 1;
 *   if (!host.start())       return 1;
 *   host.wait_for_shutdown();
 *   host.stop();
 * @endcode
 */
class ActorHost
{
  public:
    explicit ActorHost(const ActorConfig &config);
    ~ActorHost();

    ActorHost(const ActorHost &) = delete;
    ActorHost &operator=(const ActorHost &) = delete;

    /**
     * @brief Import the Python module from `config_.script_module` /
     *        `config_.script_base_dir`.  Prints slot/flexzone layout when verbose.
     */
    bool load_script(bool verbose = false);

    /**
     * @brief Start all roles that have an `on_iteration` function in the module.
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
     * @brief True if a shutdown was requested (via signal_shutdown() or api.stop()).
     *
     * Used by ActorScriptHost::do_python_work() to detect internal shutdown and
     * propagate it to the main thread's wait loop.
     */
    [[nodiscard]] bool is_shutdown_requested() const noexcept
    {
        return shutdown_.load(std::memory_order_acquire);
    }

    /**
     * @brief Print a summary of configured roles and which are activated.
     *        Used by --list-roles.
     */
    void print_role_summary() const;

  private:
    ActorConfig    config_;
    std::atomic<bool> shutdown_{false};

    /// Per-role loaded Python module: role_name → imported package.
    /// Populated by load_script(); consumed by start() to construct role workers.
    std::unordered_map<std::string, py::module_> role_modules_{};
    bool           script_loaded_{false};

    std::unordered_map<std::string,
        std::unique_ptr<ProducerRoleWorker>> producers_;
    std::unordered_map<std::string,
        std::unique_ptr<ConsumerRoleWorker>> consumers_;

    // Holds the GIL release injected by start() so the main wait loop and
    // worker loop_threads can acquire the GIL freely.  Destroyed (= GIL
    // re-acquired) at the start of stop().
    std::optional<py::gil_scoped_release> main_thread_release_;
};

} // namespace pylabhub::actor
