#ifndef PYLHUB_SCOPE_GUARD_HPP
#define PYLHUB_SCOPE_GUARD_HPP

#include <utility>

namespace pylabhub::utils
{

/**
 * @brief A generic RAII scope guard.
 *
 * This class template ensures that a given function or lambda is executed when
 * the ScopeGuard object goes out of scope, unless it has been explicitly dismissed.
 * It's a powerful tool for ensuring cleanup actions (like releasing resources,
 * closing handles, etc.) are always performed, even in the presence of exceptions
 * or multiple return paths.
 *
 * @tparam F The type of the callable object (e.g., lambda, function pointer).
 */
template <typename F>
class ScopeGuard
{
  public:
    /**
     * @brief Constructs a ScopeGuard with the given callable.
     * @param f A callable object (e.g., a lambda) to be executed on destruction.
     */
    explicit ScopeGuard(F &&f) : m_func(std::move(f)), m_active(true) {}

    /**
     * @brief Move constructor. Transfers ownership of the callable from another ScopeGuard.
     * @param rhs The ScopeGuard to move from.
     */
    ScopeGuard(ScopeGuard &&rhs) noexcept : m_func(std::move(rhs.m_func)), m_active(rhs.m_active)
    {
        rhs.dismiss();
    }

    /**
     * @brief Destructor. Executes the callable if the guard is active.
     */
    ~ScopeGuard()
    {
        if (m_active)
        {
            m_func();
        }
    }

    /**
     * @brief Dismisses the guard, preventing the callable from being executed.
     */
    void dismiss() { m_active = false; }

    // This class is non-copyable to prevent accidental duplication of the cleanup action.
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
    ScopeGuard &operator=(ScopeGuard &&) = delete;

  private:
    F m_func;
    bool m_active;
};

/**
 * @brief Factory function to create a ScopeGuard.
 *
 * This is a helper function to deduce the type of the lambda automatically,
 * making the creation of a ScopeGuard more concise.
 *
 * @tparam F The type of the callable object.
 * @param f The callable object to be executed by the guard.
 * @return A ScopeGuard object that will execute `f` on destruction.
 */
template <typename F>
ScopeGuard<F> make_scope_guard(F &&f)
{
    return ScopeGuard<F>(std::move(f));
}

} // namespace pylabhub::utils

#endif // PYLHUB_SCOPE_GUARD_HPP
