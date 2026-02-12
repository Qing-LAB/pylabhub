/**
 * @file heartbeat_manager.hpp
 * @brief C++ wrapper for consumer heartbeat management.
 */
#pragma once

#include "pylabhub_utils_export.h"
#include <string>
#include <cstdint>

namespace pylabhub::hub
{

class DataBlockConsumer; // Forward declaration

/**
 * @class HeartbeatManager
 * @brief Manages a consumer's heartbeat registration in a DataBlock.
 *
 * This RAII-style class automatically registers a consumer's PID in the
 * DataBlock's heartbeat table upon construction and unregisters it upon
 * destruction. This allows recovery tools to identify and clean up dead
 * consumers.
 */
class PYLABHUB_UTILS_EXPORT HeartbeatManager
{
  public:
    /**
     * @brief Constructs a HeartbeatManager and registers the consumer's heartbeat.
     * @param consumer The `DataBlockConsumer` to manage the heartbeat for.
     */
    explicit HeartbeatManager(DataBlockConsumer &consumer);

    /**
     * @brief Destructor that automatically unregisters the consumer's heartbeat.
     */
    ~HeartbeatManager();

    // Movable, not copyable
    HeartbeatManager(HeartbeatManager &&) noexcept;
    HeartbeatManager &operator=(HeartbeatManager &&) noexcept;
    HeartbeatManager(const HeartbeatManager &) = delete;
    HeartbeatManager &operator=(const HeartbeatManager &) = delete;

    /**
     * @brief Updates the consumer's "last seen" timestamp in the heartbeat table.
     * This should be called periodically by the consumer to signal liveness.
     */
    void pulse();

    /**
     * @brief Checks if the consumer's heartbeat is successfully registered.
     * @return `true` if registered, `false` otherwise.
     */
    bool is_registered() const;

  private:
    DataBlockConsumer *consumer_;
    int heartbeat_slot_ = -1;
};

} // namespace pylabhub::hub
