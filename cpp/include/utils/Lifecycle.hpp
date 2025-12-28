#pragma once

/*******************************************************************************
 * @file Lifecycle.hpp
 * @brief Manages the application lifecycle with dependency-aware modules.
 *
 * This file provides the public interface for the application lifecycle
 * management system. See docs/README_utils.md for detailed design information.
 ******************************************************************************/

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "pylabhub_utils_export.h"

namespace pylabhub::utils
{

/**
 * @brief A struct defining the shutdown properties for a module.
 */
struct ModuleShutdown
{
    std::function<void()> func;
    std::chrono::milliseconds timeout;
};

/**
 * @brief A struct containing all lifecycle information for a single module.
 */
struct Module
{
    std::string name;
    std::vector<std::string> dependencies;
    std::function<void()> startup;
    ModuleShutdown shutdown;
};

/**
 * @brief Registers a module with the lifecycle system.
 *
 * All modules must be registered *before* `InitializeApplication()` is called.
 * Attempting to register a module after initialization has started will result
 * in program termination.
 *
 * @param module The module to register.
 */
PYLABHUB_UTILS_EXPORT void RegisterModule(Module module);

/**
 * @brief Initializes the application by executing module startup functions.
 *
 * This function should be called once at the start of the application, after all
 * modules have been registered. It is idempotent.
 *
 * It performs a topological sort on the registered modules to determine the
 * correct startup order and detects any circular dependencies.
 *
 * Calling this function "locks" the registration system. Any subsequent calls
 * to `RegisterModule` will cause the program to terminate.
 */
PYLABHUB_UTILS_EXPORT void InitializeApplication();

/**
 * @brief Shuts down the application by executing module shutdown functions.
 *
 * This function should be called once at the end of the application's
 * lifecycle. It is idempotent. It executes shutdown functions in the reverse
 * order of the startup sequence.
 */
PYLABHUB_UTILS_EXPORT void FinalizeApplication();

} // namespace pylabhub::utils