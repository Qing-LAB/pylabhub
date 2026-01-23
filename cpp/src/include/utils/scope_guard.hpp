#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace pylabhub::basics
{

/**
 * @class ScopeGuard
 * @brief An RAII-style guard that robustly executes a callable on scope exit.
 *
 * This implementation ensures that a cleanup action is performed when the
 * current scope is exited, whether by normal execution (falling off the end of
 * a block) or by an exception.
 *
 * It is movable but not copyable, enforcing unique ownership of the cleanup
 * action.
 *
 * @tparam Callable The type of the callable object. It is constrained to be
 *         invocable with no arguments. The template parameter is always a
 *         decayed, non-reference type.
 *
 * ### Usage Example
 *
 * ScopeGuard is invaluable for managing resources that don't have their own
 * RAII wrappers, such as raw pointers, C-style file handles, or transactional
 * application state.
 *
 * @code
 *  // Example: Managing a raw resource pointer
 *  void process_resource() {
 *      ResourceType* res = create_resource();
 *
 *      // The guard ensures the resource is destroyed even if an error occurs.
 *      auto guard = pylabhub::basics::make_scope_guard([&res]() {
 *          if (res) {
 *              destroy_resource(res);
 *              res = nullptr;
 *          }
 *      });
 *
 *      // If any of these operations throw an exception, the guard guarantees
 *      // that destroy_resource() is still called during stack unwinding.
 *      res->do_something();
 *      res->do_another_thing();
 *
 *      // If processing is successful, we can dismiss the guard if we are
 *      // intentionally transferring ownership of the resource.
 *      // guard.dismiss();
 *  } // `guard` executes here if not dismissed, cleaning up the resource.
 * @endcode
 *
 * ### Error Handling and Exceptions
 *
 * The callable provided to ScopeGuard should ideally be `noexcept`. Exceptions
 * thrown from the callable during stack unwinding (in the destructor) are
 * problematic:
 *
 * 1.  **Destructor Swallows Exceptions**: To prevent `std::terminate` (which
 *     occurs if a destructor emits an exception while another exception is
 *     already active), the `ScopeGuard` destructor is `noexcept`. It will catch
 *     and swallow any exception thrown by its callable.
 * 2.  **Masking Problems**: This swallowing behavior is safe for program stability
 *     but can hide the underlying cause of a problem. A failing cleanup
 *     operation will go unnoticed at the call site.
 *
 * **Best Practice**: Ensure cleanup logic is simple and non-throwing. If failure
 * is possible, it should be handled inside the callable (e.g., by logging).
 *
 * ### Common Pitfalls and Bad Practices
 *
 * - **Dangling References**: Be cautious with lambda captures. Capturing a local
 *   variable by reference (`[&]`) that goes out of scope before the guard
 *   executes will result in undefined behavior.
 *   @code
 *   auto make_bad_guard() {
 *       int local_var = 1;
 *       // BAD: `local_var` will be destroyed before the returned guard runs.
 *       return pylabhub::basics::make_scope_guard([&]() {
 *           // Accessing `local_var` here is a use-after-free error.
 *           std::cout << local_var;
 *       });
 *   } // UB when the returned guard is destructed!
 *   @endcode
 *
 * - **Premature Dismissal**: Calling `dismiss()` too early will prevent cleanup.
 *   Only dismiss a guard when you are intentionally canceling the cleanup action,
 *   for example, after successfully transferring ownership of a resource.
 *
 * - **Misunderstanding Move Semantics**: When a `ScopeGuard` is moved, it becomes
 *   inactive. The moved-to guard takes over responsibility. The destructor of a
 *   moved-from guard has no effect.
 *
 * ### Thread Safety
 *
 * This class is **not** thread-safe. Accessing a single `ScopeGuard` instance
 * from multiple threads without external synchronization (e.g., a `std::mutex`)
 * will result in data races and undefined behavior.
 */
// The `std::invocable<Callable&>` constraint is intentionally used instead of
// `std::invocable<Callable>`. This is because the guard stores the callable
// as a member `m_func` and invokes it as an lvalue. This constraint correctly
// rejects callables that are only invocable as rvalues (e.g., with an
// `operator() &&`), providing a clearer error message at the call site of
// `make_scope_guard` rather than a confusing one deep within the guard's
// implementation.
template <typename Callable>
requires std::invocable<Callable &>
class ScopeGuard
{
  public:
    // A ScopeGuard cannot be instantiated with a reference type. This is
    // because it stores the callable by value. Use `make_scope_guard` to ensure
    // the type is correctly decayed.
    static_assert(!std::is_reference_v<Callable>, "ScopeGuard cannot hold a reference to a callable.");

    // The callable must be copyable or movable.
    static_assert(std::is_move_constructible_v<Callable> || std::is_copy_constructible_v<Callable>,
                  "ScopeGuard's callable must be move- or copy-constructible.");

    /**
     * @brief Checks if the guard is active and will execute on scope exit.
     * @return `true` if active, `false` if dismissed or moved-from.
     */
    [[nodiscard]] explicit operator bool() const noexcept { return m_active; }

    /**
     * @brief Constructs a ScopeGuard with a given callable.
     * @param fn The callable object to execute on scope exit. It is copied or
     *           moved into the guard's internal storage.
     */
    explicit ScopeGuard(Callable fn) noexcept(std::is_nothrow_move_constructible_v<Callable>)
        : m_func(std::move(fn))
    {
    }

    /**
     * @brief Move constructor. Transfers ownership of the cleanup action.
     * @param other The source guard to move from. The source guard is dismissed
     *              and will no longer execute.
     */
    ScopeGuard(ScopeGuard &&other) noexcept(std::is_nothrow_move_constructible_v<Callable>)
        : m_func(std::move(other.m_func)), m_active(other.m_active)
    {
        other.dismiss();
    }

    /**
     * @brief Destructor. Executes the callable if the guard is active.
     *
     * Any exceptions thrown by the callable are caught and swallowed to preserve
     * the `noexcept` guarantee of the destructor and avoid `std::terminate`.
     */
    ~ScopeGuard() noexcept
    {
        if (m_active)
        {
            try
            {
                std::invoke(m_func);
            }
            catch (...)
            {
                // Deliberately swallow exceptions in destructor. See class docs.
            }
        }
    }

    // Non-copyable to enforce unique ownership.
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;

    // Move assignment is intentionally deleted to avoid the complexity of
    // handling the currently-held active callable.
    ScopeGuard &operator=(ScopeGuard &&) = delete;

    /**
     * @brief Deactivates the guard, preventing the callable from being executed.
     */
    constexpr void dismiss() noexcept { m_active = false; }

    /**
     * @brief Deactivates the guard. Alias for `dismiss()`.
     */
    constexpr void release() noexcept { dismiss(); }

    /**
     * @brief Executes the callable immediately if active, and then dismisses the
     * guard.
     *
     * Any exceptions thrown by the callable are caught and swallowed.
     */
    void invoke() noexcept
    {
        if (m_active)
        {
            m_active = false; // Must dismiss before invoke to prevent double execution.
            try
            {
                std::invoke(m_func);
            }
            catch (...)
            {
                // Swallow exceptions. See class docs.
            }
        }
    }

    /**
     * @brief Executes the callable immediately if active, and then dismisses the
     * guard.
     *
     * This version does NOT swallow exceptions. If the callable throws, the
     * exception propagates. The guard is still dismissed before execution.
     */
    void invoke_and_rethrow()
    {
        if (m_active)
        {
            m_active = false; // Must dismiss before invoke to prevent double execution.
            std::invoke(m_func);
        }
    }

  private:
    /// @brief The callable function to be executed.
    Callable m_func;
    /// @brief Flag indicating if the guard is active. If false, the destructor will do nothing.
    bool m_active{true};
};

/**
 * @brief A factory function to create a ScopeGuard.
 *
 * This function is the preferred way to create a ScopeGuard. It uses type
 * deduction and ensures that the ScopeGuard is not instantiated with a
 * reference type, which simplifies its behavior and avoids potential issues.
 *
 * @note The callable `f` is always stored by value (either copied or moved)
 *       into the `ScopeGuard`. Ensure that any references captured by `f`
 *       remain valid until the guard executes.
 *
 * @tparam F The type of the callable object.
 * @param f The callable object to be managed by the guard.
 * @return A ScopeGuard instance for the given callable.
 */
template <typename F> auto make_scope_guard(F &&f) -> ScopeGuard<std::decay_t<F>>
{
    return ScopeGuard<std::decay_t<F>>(std::forward<F>(f));
}

} // namespace pylabhub::basics

