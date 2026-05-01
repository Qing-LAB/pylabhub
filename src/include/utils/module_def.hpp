#pragma once
/**
 * @file module_def.hpp
 * @brief ABI-safe module definition for LifecycleManager registration.
 */
#include "pylabhub_utils_export.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

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
 * Using a C-style function pointer is intentional and essential for ABI stability
 * across shared library boundaries — it has a standardised calling convention,
 * unlike `std::function`. The `arg` pointer is never null when a startup/shutdown
 * argument was supplied; it is `nullptr` when none was given.
 *
 * **Do not change this to `std::string_view` or any other C++ type**: function
 * pointers cross `.so` / DLL boundaries and must use C-compatible signatures.
 */
/**
 * @brief Validation function for user-data pointers.
 * Called before every callback invocation when userdata is configured.
 * @param userdata  The pointer stored at module construction.
 * @param key       The generation key assigned at construction.
 * @return true if the pointer is still valid.
 */
using UserDataValidateFn = bool (*)(void *userdata, uint64_t key);

/**
 * @brief Function pointer type for module startup and shutdown callbacks.
 *
 * @param arg       String argument (nullptr when none was given).
 * @param userdata  User data pointer (nullptr when not configured).
 */
using LifecycleCallback = void (*)(const char *arg, void *userdata);

/**
 * @class ModuleDef
 * @brief An ABI-safe builder for a lifecycle module definition.
 *
 * `ModuleDef` uses the Pimpl idiom to hide its internal `std::string` and
 * `std::vector` members, making it safe to create and pass across shared-library
 * boundaries without exposing STL internals in the ABI.
 *
 * It is movable but not copyable, enforcing a clear ownership model.  Once a
 * `ModuleDef` is registered with the `LifecycleManager`, ownership is transferred.
 *
 * **Name length limit:** all module and dependency names must not exceed
 * `MAX_MODULE_NAME_LEN` characters.  Violations are rejected at the point of
 * call with a `std::length_error` so there is no silent truncation.
 */
class PYLABHUB_UTILS_EXPORT ModuleDef
{
  public:
    /**
     * @brief Maximum number of characters allowed in a module or dependency name.
     *
     * Enforced in `ModuleDef(std::string_view)` and `add_dependency()`.
     * Kept as a `size_t` so it compares directly against `std::string_view::size()`.
     */
    static constexpr size_t MAX_MODULE_NAME_LEN = 256;

    /**
     * @brief Maximum number of characters allowed in a callback string argument.
     *
     * Enforced in `set_startup(cb, arg)` and `set_shutdown(cb, timeout, arg)`.
     */
    static constexpr size_t MAX_CALLBACK_PARAM_STRLEN = 1024;

    /**
     * @brief Constructs a module definition with a given name.
     *
     * @param name Unique name for this module (e.g. `"Logger"`, `"Database"`).
     *             Must be non-empty and at most `MAX_MODULE_NAME_LEN` characters.
     * @throws std::invalid_argument if `name` is empty.
     * @throws std::length_error     if `name.size() > MAX_MODULE_NAME_LEN`.
     */
    explicit ModuleDef(std::string_view name);

    /**
     * @brief Constructs a module definition with user data.
     *
     * The userdata pointer is passed to every callback as the second argument.
     * The validate function is called before each callback invocation; if it
     * returns false, the callback is skipped (prevents dangling pointer use).
     * A generation key is assigned automatically via LifecycleManager::next_unique_key().
     *
     * @param name      Unique module name.
     * @param userdata  Pointer passed to all callbacks. nullptr = no userdata.
     * @param validate  Validation function. nullptr = skip validation.
     */
    ModuleDef(std::string_view name, void *userdata, UserDataValidateFn validate);

    /// @brief Returns the generation key assigned at construction (0 if no userdata).
    [[nodiscard]] uint64_t userdata_key() const noexcept;

    /// @brief Destructor. Defined in the .cpp file to satisfy the Pimpl idiom.
    ~ModuleDef();

    // --- Rule of Five: Movable, but not Copyable ---
    // Move constructor/assignment are defined in the .cpp where ModuleDefImpl is complete.
    ModuleDef(ModuleDef &&other) noexcept;
    ModuleDef &operator=(ModuleDef &&other) noexcept;
    ModuleDef(const ModuleDef &) = delete;
    ModuleDef &operator=(const ModuleDef &) = delete;

    /**
     * @brief Declares a dependency on another module.
     *
     * The `LifecycleManager` ensures the named module is started before this one
     * and shut down after it.  An empty `dependency_name` is silently ignored.
     *
     * @param dependency_name Name of the module this module depends on.
     *                        At most `MAX_MODULE_NAME_LEN` characters.
     * @throws std::length_error if `dependency_name.size() > MAX_MODULE_NAME_LEN`.
     */
    void add_dependency(std::string_view dependency_name);

    /**
     * @brief Sets the startup callback (no argument variant).
     * @param startup_func Called on module startup; must not be null.
     */
    void set_startup(LifecycleCallback startup_func);

    /**
     * @brief Sets the startup callback with a string argument.
     *
     * @param startup_func Called on module startup; must not be null.
     * @param arg          Argument forwarded to `startup_func` as a null-terminated
     *                     C-string.  At most `MAX_CALLBACK_PARAM_STRLEN` characters.
     * @throws std::length_error if `arg.size() > MAX_CALLBACK_PARAM_STRLEN`.
     */
    void set_startup(LifecycleCallback startup_func, std::string_view arg);

    /**
     * @brief Sets the shutdown callback (no argument variant).
     *
     * @param shutdown_func Called on module shutdown; must not be null.
     * @param timeout       Maximum time allowed for the callback to complete.
     *                      Use `std::chrono::milliseconds(0)` for no timeout (runs
     *                      until completion, no thread detach).
     */
    void set_shutdown(LifecycleCallback shutdown_func, std::chrono::milliseconds timeout);

    /**
     * @brief Sets the shutdown callback with a string argument.
     *
     * @param shutdown_func Called on module shutdown; must not be null.
     * @param timeout       Maximum time allowed for the callback to complete.
     * @param arg           Argument forwarded to `shutdown_func` as a null-terminated
     *                      C-string.  At most `MAX_CALLBACK_PARAM_STRLEN` characters.
     * @throws std::length_error if `arg.size() > MAX_CALLBACK_PARAM_STRLEN`.
     */
    void set_shutdown(LifecycleCallback shutdown_func, std::chrono::milliseconds timeout,
                      std::string_view arg);

    /**
     * @brief Marks this module as persistent (dynamic modules only).
     *
     * A persistent dynamic module will not be unloaded when its reference count
     * drops to zero — it stays loaded until `finalize()` is called.  Useful for
     * expensive-to-initialise services that should remain active for the entire
     * application lifetime.  Has no effect on static modules.
     *
     * @param persistent `true` (default) to mark as persistent.
     */
    void set_as_persistent(bool persistent = true);

    /**
     * @brief Opt in to owner-managed teardown semantics.
     *
     * When `true`, the module declares that its C++ owner performs the real
     * teardown synchronously (e.g. in the owner's destructor) and clears the
     * userdata pointer / flips the validator-fail-signal flag as a deliberate
     * indicator of "I'm gone, skip my callback".
     *
     * Effect on the unload path (`processOneUnloadInThread`):
     *   - **Without this flag (default)**: a validator-fail at unload time is
     *     treated as an anomaly per HEP-CORE-0001 — `FAILED_SHUTDOWN` status,
     *     module marked contaminated, name retained in the graph for
     *     diagnostics, WARN logged.
     *   - **With this flag (`true`)**: a validator-fail at unload time is
     *     treated as a success-without-callback — the registered shutdown
     *     callback (which is contractually a no-op for owner-managed modules)
     *     is skipped, and full graph cleanup runs as if the callback had
     *     succeeded.  The module name is freed for re-registration; no WARN
     *     fires.
     *
     * Use ONLY if the registered shutdown callback is genuinely a no-op
     * (cannot mutate state) AND the C++ owner reliably performs the real
     * teardown synchronously.  See `src/utils/service/thread_manager.cpp` for
     * the canonical example.
     *
     * @param enabled `true` (default) to enable owner-managed teardown.
     */
    void set_owner_managed_teardown(bool enabled = true);

  private:
    // LifecycleManager is the sole consumer of the pImpl internals.
    friend class LifecycleManager;
    std::unique_ptr<ModuleDefImpl> pImpl;
};

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
