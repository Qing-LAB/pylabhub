#pragma once

/*******************************************************************************
 * @file Lifecycle.hpp
 * @brief Manages the application lifecycle with dependency-aware modules.
 *
 * This file provides the public, ABI-safe C++ interface for the application
 * lifecycle management system.
 *
 * It uses the Pimpl (Pointer to Implementation) idiom to hide all STL container
 * and `std::function` members from the public API. This ensures that the
 * library's ABI remains stable across different compiler versions and standard
 * library implementations, which is critical for a shared library.
 *
 * The `pylabhub_module` class provides an idiomatic C++ interface for defining
 * a module, which can then be registered with the central manager.
 ******************************************************************************/

#include <memory> // For std::unique_ptr

#include "pylabhub_utils_export.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251) // unique_ptr needs to have dll-interface
#endif

// Forward-declare the implementation struct to hide it from the public API.
struct pylabhub_module_impl;

/**
 * @brief A function pointer type for module startup and shutdown callbacks.
 */
typedef void (*pylabhub_lifecycle_callback)();

/**
 * @class pylabhub_module
 * @brief A C++ wrapper for defining a lifecycle module in an ABI-safe way.
 *
 * This class uses the Pimpl idiom to hide its implementation details,
 * particularly STL containers, which makes it safe to use across shared
 * library boundaries.
 *
 * Usage:
 * @code
 *   // In a source file:
 *   void my_startup() { ... }
 *   void my_shutdown() { ... }
 *
 *   pylabhub_module module("MyModule");
 *   module.add_dependency("Logger");
 *   module.set_startup(&my_startup);
 *   module.set_shutdown(&my_shutdown, 2000);
 *   pylabhub_register_module(std::move(module));
 * @endcode
 */
class PYLABHUB_UTILS_EXPORT pylabhub_module
{
  public:
    /**
     * @brief Constructs a module definition with a given name.
     * @param name The unique name for this module.
     */
    explicit pylabhub_module(const char *name);

    /// @brief Destructor.
    ~pylabhub_module();

    // --- Rule of Five: Movable, but not Copyable ---
    pylabhub_module(pylabhub_module &&other) noexcept;
    pylabhub_module &operator=(pylabhub_module &&other) noexcept;
    pylabhub_module(const pylabhub_module &) = delete;
    pylabhub_module &operator=(const pylabhub_module &) = delete;

    /**
     * @brief Adds a dependency to this module.
     * @param dependency_name The name of the module that this module depends on.
     */
    void add_dependency(const char *dependency_name);

    /**
     * @brief Sets the startup callback for this module.
     * @param startup_func A C-style function pointer to be called on startup.
     */
    void set_startup(pylabhub_lifecycle_callback startup_func);

    /**
     * @brief Sets the shutdown callback for this module.
     * @param shutdown_func A C-style function pointer to be called on shutdown.
     * @param timeout_ms The time in milliseconds to wait for shutdown to complete.
     */
    void set_shutdown(pylabhub_lifecycle_callback shutdown_func, unsigned int timeout_ms);

  private:
    // The friend declaration allows the implementation of pylabhub_register_module
    // to access the private pImpl member in order to extract the module
    // definition without exposing it publicly.
    friend void pylabhub_register_module(pylabhub_module &&module);

    std::unique_ptr<pylabhub_module_impl> pImpl;
};


/**
 * @brief Registers a module with the lifecycle system.
 *
 * After this call, the lifecycle system takes ownership of the module
 * definition. All modules must be registered *before*
 * `pylabhub_initialize_application()` is called.
 *
 * @param handle The fully configured module object, passed by rvalue-reference
 *               to indicate a transfer of ownership.
 */
PYLABHUB_UTILS_EXPORT void pylabhub_register_module(pylabhub_module &&module);

/**
 * @brief Initializes the application by executing module startup functions.
 *
 * This function should be called once at the start of the application. It is
 * idempotent. It performs a topological sort on registered modules and detects
 * circular dependencies.
 */
PYLABHUB_UTILS_EXPORT void pylabhub_initialize_application();

/**
 * @brief Shuts down the application by executing module shutdown functions.
 *
 * This function should be called once at the end of the application's
 * lifecycle. It is idempotent.
 */
PYLABHUB_UTILS_EXPORT void pylabhub_finalize_application();

#if defined(_MSC_VER)
#pragma warning(pop)
#endif