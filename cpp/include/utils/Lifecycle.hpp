#pragma once

/*******************************************************************************
 * @file Lifecycle.hpp
 * @brief Provides functions to manage the lifecycle of the pylabhub::utils library.
 *
 * This header defines the main entry and exit points for initializing and
 * shutting down the core utilities. To ensure proper resource management,
 * especially for asynchronous systems like the Logger, applications using this
 * library should call these functions.
 *
 * **Usage**
 *
 * In your main application entry point:
 * ```cpp
 * #include "utils/Lifecycle.hpp"
 *
 * void my_cleanup_function() {
 *     // ... clean up resources ...
 * }
 *
 * int main(int argc, char* argv[]) {
 *     // Register a finalizer to be called during shutdown.
 *     pylabhub::utils::RegisterFinalizer("MyCleanup", my_cleanup_function,
 * std::chrono::seconds(2));
 *
 *     // Initialize the utility library at the start.
 *     pylabhub::utils::Initialize();
 *
 *     // ... Application logic ...
 *     LOGGER_INFO("Application is running.");
 *
 *     // Finalize the library at the end for graceful shutdown.
 *     pylabhub::utils::Finalize();
 *
 *     return 0;
 * }
 * ```
 ******************************************************************************/

#include <chrono>
#include <functional>
#include <string>

#include "pylabhub_utils_export.h"

namespace pylabhub::utils
{

/**
 * @brief Initializes the utility library and its subsystems.
 *
 * This function should be called once at the beginning of the application's
 * lifecycle. It ensures that all necessary resources are allocated and
 * background tasks (like the Logger's worker thread) are started. It will
 * also execute any functions registered via `RegisterInitializer`.
 */
PYLABHUB_UTILS_EXPORT void Initialize();

/**
 * @brief Shuts down the utility library and its subsystems gracefully.
 *
 * This function should be called once at the end of the application's
 * lifecycle (e.g., before returning from `main`). It first calls all
 * registered finalizers and then shuts down internal subsystems.
 * Failure to call this may result in lost data.
 */
PYLABHUB_UTILS_EXPORT void Finalize();

/**
 * @brief Registers a function to be called during the `Initialize` phase.
 *
 * @param func The function to be executed.
 */
PYLABHUB_UTILS_EXPORT void RegisterInitializer(std::function<void()> func);

/**
 * @brief Registers a function to be called during the `Finalize` phase.
 *
 * Finalizers are executed in Last-In, First-Out (LIFO) order.
 *
 * @param name A descriptive name for the finalizer, used for logging.
 * @param func The function to be executed.
 * @param timeout The maximum time to wait for the function to complete. If it
 *                times out, the shutdown process will log a warning and continue.
 */
PYLABHUB_UTILS_EXPORT void RegisterFinalizer(std::string name, std::function<void()> func,
                                             std::chrono::milliseconds timeout);

} // namespace pylabhub::utils
