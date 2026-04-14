#pragma once
/**
 * @file zmq_context.hpp
 * @brief Shared ZeroMQ context as a named static lifecycle module.
 *
 * Provides a single `zmq::context_t` instance owned by the lifecycle manager.
 * All ZMQ sockets in the process must be created from this context.
 *
 * Usage:
 *   Include "ZMQContext" in your LifecycleGuard dependencies (via GetZMQContextModule()).
 *   Then call get_zmq_context() to obtain the shared context.
 */
#include "pylabhub_utils_export.h"

#include "cppzmq/zmq.hpp"
#include "utils/module_def.hpp"

namespace pylabhub::hub
{

/**
 * @brief Returns the global ZeroMQ context.
 * @pre Must be called only after GetZMQContextModule() has been initialized via LifecycleGuard.
 */
[[nodiscard]] PYLABHUB_UTILS_EXPORT zmq::context_t &get_zmq_context();

/**
 * @brief Initializes the global ZeroMQ context.
 * Called by the DataExchangeHub lifecycle module startup (and by GetZMQContextModule startup).
 * Safe to call only once per process lifetime.
 */
PYLABHUB_UTILS_EXPORT void zmq_context_startup();

/**
 * @brief Destroys the global ZeroMQ context.
 * Called by the DataExchangeHub lifecycle module shutdown (and by GetZMQContextModule shutdown).
 */
PYLABHUB_UTILS_EXPORT void zmq_context_shutdown();

/**
 * @brief Lifecycle module for the ZMQ library (zmq::context_t).
 * Must be included in LifecycleGuard for any code that creates ZMQ sockets.
 * Depends on: Logger.
 */
PYLABHUB_UTILS_EXPORT pylabhub::utils::ModuleDef GetZMQContextModule();

} // namespace pylabhub::hub
