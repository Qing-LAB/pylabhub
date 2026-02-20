#pragma once
/**
 * @file actor_api.hpp
 * @brief ActorAPI — thin proxy object passed to Python script callbacks.
 *
 * Each `ProducerActorHost` / `ConsumerActorHost` owns one `ActorAPI` instance.
 * The same instance is passed by reference to every callback. C++ sets
 * per-call state (slot_valid) before each Python invocation.
 *
 * ## Python usage (producer)
 * @code{.py}
 *   def on_write(slot, flexzone, api):
 *       api.broadcast(b"extra")
 *       api.log('info', "hello")
 *       api.update_flexzone_checksum()
 *       return True
 * @endcode
 *
 * ## Python usage (consumer)
 * @code{.py}
 *   def on_read(slot, flexzone, api):
 *       if not api.slot_valid():
 *           api.log('warn', "slot checksum failed")
 *           api.update_flexzone_checksum()   # accept current flexzone state
 *           return
 * @endcode
 */

#include "utils/hub_consumer.hpp"
#include "utils/hub_producer.hpp"

#include <pybind11/pybind11.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace py = pybind11;

namespace pylabhub::actor
{

/**
 * @class ActorAPI
 * @brief Proxy to C++ producer/consumer services for Python script callbacks.
 *
 * All methods dispatch immediately to C++ without buffering or Python state.
 * The only state held here is:
 *   - Raw pointers to the current Producer/Consumer (set by host).
 *   - `slot_valid_` — set before each `on_read()` call, reset to true after.
 *   - `consumer_fz_accepted_` — accepted flexzone snapshot (consumer only).
 */
class ActorAPI
{
  public:
    ActorAPI() = default;

    // ── Called by C++ host — not from Python ──────────────────────────────────

    void set_producer(hub::Producer *p) noexcept { producer_ = p; }
    void set_consumer(hub::Consumer *c) noexcept { consumer_ = c; }

    /**
     * @brief Set the per-call slot validity flag.
     *        C++ sets this before each `on_read()` call. True = checksum passed
     *        (or not enforced). False = checksum failed and "pass" policy applies.
     */
    void set_slot_valid(bool v) noexcept { slot_valid_ = v; }

    /**
     * @brief True if the given SHM flexzone content matches the consumer's
     *        accepted snapshot (set via `update_flexzone_checksum()`).
     *        Returns false if no snapshot has been established yet.
     */
    [[nodiscard]] bool is_fz_accepted(std::span<const std::byte> current_fz) const noexcept;

    // ── Python-accessible — common ─────────────────────────────────────────────

    /// Log a message through the hub logger.
    /// level: "debug" | "info" | "warn" | "error"
    void log(const std::string &level, const std::string &msg);

    /// List of ZMQ identity strings of currently connected consumers (producer side).
    py::list consumers();

    // ── Python-accessible — producer side ─────────────────────────────────────

    /// Broadcast bytes to all connected consumers on the ZMQ data socket.
    bool broadcast(py::bytes data);

    /// Send bytes to one specific consumer (ZMQ identity string).
    bool send(const std::string &identity, py::bytes data);

    /**
     * @brief Update the SHM flexzone BLAKE2b checksum.
     *
     * **Producer**: calls `DataBlockProducer::update_checksum_flexible_zone()`.
     *
     * **Consumer**: stores the current SHM flexzone content as an accepted
     *   snapshot. Subsequent actor-level flexzone checks will compare against
     *   this snapshot (content equality) rather than the SHM-stored checksum.
     *   Useful when the SHM checksum is stale but the data is known to be correct.
     *
     * @return true on success; false if SHM not available or flexzone size is 0.
     */
    bool update_flexzone_checksum();

    // ── Python-accessible — consumer side ─────────────────────────────────────

    /// Send a ctrl frame to the producer.
    bool send_ctrl(py::bytes data);

    /**
     * @brief True when the current slot passed its checksum verification.
     *        False when checksum failed and the actor is calling the callback
     *        with `on_checksum_fail = "pass"` policy.
     *        Always true when slot checksum is not enforced.
     */
    [[nodiscard]] bool slot_valid() const noexcept { return slot_valid_; }

    /**
     * @brief Verify the SHM flexzone using the DataBlock-stored BLAKE2b checksum.
     * @return true if checksum matches; false on mismatch or SHM unavailable.
     */
    [[nodiscard]] bool verify_flexzone_checksum();

  private:
    hub::Producer *producer_{nullptr};
    hub::Consumer *consumer_{nullptr};

    /// Per-call flag — set by C++ before each on_read(); reset to true after.
    bool slot_valid_{true};

    /// Consumer-side: accepted flexzone content snapshot.
    std::vector<std::byte> consumer_fz_accepted_{};
    bool                   consumer_fz_has_accepted_{false};
};

} // namespace pylabhub::actor
