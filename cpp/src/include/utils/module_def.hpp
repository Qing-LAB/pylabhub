#pragma once
/**
 * @file module_def.hpp
 * @brief ABI-safe module definition for LifecycleManager registration.
 */
#include "pylabhub_utils_export.h"

#include <memory>

// Disable warning C4251 on MSVC. This is a common practice for exported classes
// that use std::unique_ptr to a forward-declared (incomplete) type (Pimpl).
// The user of the class cannot access the member, so it's safe.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::utils
{

// Forward-declare the implementation structs to hide them from the public API,
// which is the essence of the Pimpl idiom.
class ModuleDefImpl;
class LifecycleManager; // Forward-declaration for the friend class

/**
 * @brief A function pointer type for module startup and shutdown callbacks.
 *
 * Using a C-style function pointer is essential for ABI stability, as it has a
 * standardized calling convention, unlike `std::function`. The argument is
 * optional; `nullptr` will be passed if no argument is provided.
 */
typedef void (*LifecycleCallback)(const char *arg);

/**
 * @class ModuleDef
 * @brief An ABI-safe object for defining a lifecycle module's properties.
 *
 * This class acts as a builder for a module definition. It uses the Pimpl idiom
 * to hide its internal `std::string` and `std::vector` members, making it safe
 * to create and pass this object across shared library boundaries.
 *
 * It is movable but not copyable, enforcing a clear ownership model. Once a
 * `ModuleDef` is registered with the `LifecycleManager`, ownership is transferred.
 */
class PYLABHUB_UTILS_EXPORT ModuleDef
{
  public:
    /**
     * @brief The maximum allowed length for a callback string argument.
     *
     * This limit prevents excessive memory allocation for callback arguments.
     * An exception will be thrown if this limit is exceeded.
     */
    static constexpr size_t MAX_CALLBACK_PARAM_STRLEN = 1024;

    /**
     * @brief Constructs a module definition with a given name.
     * @param name The unique name for this module (e.g., "Logger", "Database").
     *             This name is used for dependency resolution. Must not be null.
     */
    explicit ModuleDef(const char *name);

    /// @brief Destructor. Must be defined in the .cpp file for the Pimpl idiom.
    ~ModuleDef();

    // --- Rule of Five: Movable, but not Copyable ---
    // The move constructor and assignment operators are defaulted in the .cpp file.
    // to ensure they are generated where ModuleDefImpl is a complete type.
    ModuleDef(ModuleDef &&other) noexcept;
    ModuleDef &operator=(ModuleDef &&other) noexcept;
    ModuleDef(const ModuleDef &) = delete;
    ModuleDef &operator=(const ModuleDef &) = delete;

    /**
     * @brief Adds a dependency to this module.
     * @param dependency_name The name of the module that this module depends on.
     */
    void add_dependency(const char *dependency_name);

    /**
     * @brief Sets the startup callback for this module (no argument).
     * @param startup_func The callback to be called on startup.
     */
    void set_startup(LifecycleCallback startup_func);

    /**
     * @brief Sets the startup callback with a string argument.
     * @param startup_func The callback to be called on startup.
     * @param data A pointer to the string data.
     * @param len The length of the string data.
     * @throws std::length_error if `len` > `MAX_CALLBACK_PARAM_STRLEN`.
     */
    void set_startup(LifecycleCallback startup_func, const char *data, size_t len);

    /**
     * @brief Sets the shutdown callback for this module (no argument).
     * @param shutdown_func The callback to be called on shutdown.
     * @param timeout_ms The timeout in milliseconds for the shutdown function.
     */
    void set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms);

    /**
     * @brief Sets the shutdown callback with a string argument.
     * @param shutdown_func The callback to be called on shutdown.
     * @param timeout_ms The timeout in milliseconds for the shutdown function.
     * @param data A pointer to the string data.
     * @param len The length of the string data.
     * @throws std::length_error if `len` > `MAX_CALLBACK_PARAM_STRLEN`.
     */
    void set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms, const char *data,
                      size_t len);

    /**
     * @brief Marks this module as persistent.
     * @details A persistent dynamic module, once loaded, will not be unloaded
     * automatically when its reference count drops to zero. It will only be
     * unloaded when the application is finalized. This is useful for modules that
     * are costly to initialize and should stay loaded. This flag has no effect
     * on static modules.
     * @param persistent If `true` (default), marks the module as persistent.
     */
    void set_as_persistent(bool persistent = true);

  private:
    // This friend declaration allows LifecycleManager to access the private pImpl
    // member to extract the module definition. This is a controlled way to
    // break encapsulation for the Pimpl pattern without exposing implementation
    // details publicly.
    friend class LifecycleManager;
    std::unique_ptr<ModuleDefImpl> pImpl;
};

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
