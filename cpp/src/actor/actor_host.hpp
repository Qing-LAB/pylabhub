#pragma once
/**
 * @file actor_host.hpp
 * @brief ProducerActorHost and ConsumerActorHost — Python script lifecycle managers.
 *
 * ## Script interface (new schema API)
 *
 * ### Producer
 * @code{.py}
 *   def on_init(flexzone, api):          # called once; flexzone is writable
 *   def on_write(slot, flexzone, api) -> bool:  # required; True/None = commit
 *   def on_message(sender, data, api):   # optional; consumer ctrl messages
 *   def on_stop(flexzone, api):          # optional
 * @endcode
 *
 * ### Consumer
 * @code{.py}
 *   def on_init(flexzone, api):     # called once
 *   def on_read(slot, flexzone, api):  # required (SHM mode)
 *   def on_data(data, api):         # optional; ZMQ broadcast frames
 *   def on_stop(flexzone, api):     # optional
 * @endcode
 *
 * ## Slot object lifetime
 * The `slot` ctypes struct (or numpy array) is valid ONLY for the duration
 * of the `on_write` / `on_read` call. Scripts must not store it. `flexzone`
 * persists for the actor's lifetime and may be stored safely.
 *
 * ## SHM slot_schema vs. legacy mode
 *
 * When `slot_schema` is present in the config:
 *   - C++ builds a `ctypes.LittleEndianStructure` (or `numpy.ndarray`) at
 *     `start()` and presents it to `on_write` / `on_read`.
 *
 * When absent and `shm.slot_size` is set (legacy/deprecated):
 *   - Producer: `on_write(slot, py::none(), api)` where `slot` is a bytearray.
 *   - Consumer: `on_read(slot, py::none(), api)` where `slot` is bytes.
 */

#include "actor_api.hpp"
#include "actor_config.hpp"
#include "actor_schema.hpp"

#include "utils/hub_consumer.hpp"
#include "utils/hub_producer.hpp"
#include "utils/messenger.hpp"

#include <pybind11/embed.h>

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

namespace py = pybind11;

namespace pylabhub::actor
{

// ============================================================================
// ProducerActorHost
// ============================================================================

/**
 * @class ProducerActorHost
 * @brief Hosts a hub::Producer and drives a Python script's write callbacks.
 */
class ProducerActorHost
{
  public:
    explicit ProducerActorHost(const ActorConfig &config,
                                hub::Messenger     &messenger);
    ~ProducerActorHost();

    ProducerActorHost(const ProducerActorHost &) = delete;
    ProducerActorHost &operator=(const ProducerActorHost &) = delete;

    /**
     * @brief Load the Python script and check callback signatures.
     *        In verbose mode, print slot/flexzone layout to stdout.
     * @return true if the script loaded successfully.
     */
    bool load_script(bool verbose_validation = false);

    /**
     * @brief Start the producer and write loop thread.
     *        Calls `on_init(flexzone, api)` before the loop begins.
     */
    bool start();

    /**
     * @brief Signal stop and join the write loop thread.
     *        Calls `on_stop(flexzone, api)` after the loop exits.
     */
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

  private:
    ActorConfig              config_;
    hub::Messenger           &messenger_;
    std::optional<hub::Producer> producer_;

    // ── Typed slot/flexzone views ─────────────────────────────────────────────
    SchemaSpec   slot_spec_{};        ///< Parsed slot schema
    SchemaSpec   fz_spec_{};          ///< Parsed flexzone schema

    py::object   slot_type_{};        ///< ctypes class or numpy dtype (slot)
    py::object   fz_type_{};          ///< ctypes class or numpy dtype (flexzone)
    py::object   fz_inst_{};          ///< Persistent flexzone ctypes/numpy instance
    py::object   fz_mv_{};            ///< Persistent memoryview backing fz_inst_

    size_t       schema_slot_size_{0};///< ctypes.sizeof(slot_type_) — set at start()
    size_t       schema_fz_size_{0};  ///< ctypes.sizeof(fz_type_) — set at start()
    bool         has_fz_{false};      ///< True when flexzone schema is configured

    // ── ZMQ-only mode: actor-owned slot buffer ────────────────────────────────
    std::vector<std::byte> zmq_slot_buf_{}; ///< Backing store for non-SHM slot view

    // ── Python callbacks ──────────────────────────────────────────────────────
    py::object py_on_init_{};
    py::object py_on_write_{};
    py::object py_on_message_{};
    py::object py_on_stop_{};
    bool       script_loaded_{false};

    // ── API proxy ─────────────────────────────────────────────────────────────
    ActorAPI   api_{};
    py::object api_obj_{};  ///< Python-wrapped reference to api_ (built at start())

    // ── Loop control ──────────────────────────────────────────────────────────
    std::atomic<bool> running_{false};
    std::thread       loop_thread_{};

    // ── Private helpers ───────────────────────────────────────────────────────

    /// Build ctypes/numpy types from schemas under GIL. Called from start().
    bool build_slot_types_();

    /// Print slot/flexzone layout to stdout (verbose/--validate mode). Under GIL.
    void print_layout_() const;

    /// Create a writable slot view (ctypes from_buffer / numpy.ndarray) from
    /// the given raw memory. Caller must hold the GIL.
    [[nodiscard]] py::object make_slot_view_(void *data, size_t size) const;

    void run_loop_shm();
    void run_loop_zmq();

    void call_on_init();
    void call_on_stop();
    bool call_on_write_(py::object &slot); ///< Returns true = commit, false = discard
};

// ============================================================================
// ConsumerActorHost
// ============================================================================

/**
 * @class ConsumerActorHost
 * @brief Hosts a hub::Consumer and drives a Python script's read callbacks.
 */
class ConsumerActorHost
{
  public:
    explicit ConsumerActorHost(const ActorConfig &config,
                                hub::Messenger     &messenger);
    ~ConsumerActorHost();

    ConsumerActorHost(const ConsumerActorHost &) = delete;
    ConsumerActorHost &operator=(const ConsumerActorHost &) = delete;

    bool load_script(bool verbose_validation = false);
    bool start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

  private:
    ActorConfig              config_;
    hub::Messenger           &messenger_;
    std::optional<hub::Consumer> consumer_;

    SchemaSpec   slot_spec_{};
    SchemaSpec   fz_spec_{};

    py::object   slot_type_{};
    py::object   fz_type_{};
    py::object   fz_inst_{};  ///< Persistent ctypes/numpy flexzone instance
    py::object   fz_mv_{};    ///< Backing memoryview for fz_inst_

    /// Actor-owned mutable copy of SHM flexzone (consumer cannot write to SHM
    /// directly — it is read-only mapped). Scripts may modify this local copy
    /// and call api.update_flexzone_checksum() to accept the corrected state.
    std::vector<std::byte> fz_buf_{};

    size_t       schema_slot_size_{0};
    size_t       schema_fz_size_{0};
    bool         has_fz_{false};

    py::object py_on_init_{};
    py::object py_on_data_{};
    py::object py_on_read_{};
    py::object py_on_stop_{};
    bool       script_loaded_{false};

    ActorAPI   api_{};
    py::object api_obj_{};

    std::atomic<bool> running_{false};
    std::thread       loop_thread_{};

    bool build_slot_types_();
    void print_layout_() const;

    /// Create a readonly slot view (ctypes from_buffer_copy / numpy) from
    /// the given raw memory. Caller must hold the GIL.
    [[nodiscard]] py::object make_slot_view_readonly_(const void *data, size_t size) const;

    void run_loop_shm();

    void call_on_init();
    void call_on_stop();
    void wire_zmq_callback();
};

} // namespace pylabhub::actor
