#pragma once

namespace pylabhub::basics
{

/**
 * @class ScopeGuard
 * @brief An RAII-style guard that executes a callable object upon scope exit.
 *
 * The `ScopeGuard` is a general-purpose utility to ensure that a cleanup action
 * is performed when the current scope is exited, whether by normal execution
 * or by an exception. It is movable but not copyable.
 *
 * @tparam F The type of the callable object (e.g., a lambda).
 */
template <typename F> class ScopeGuard
{
  public:
    using FnT = std::decay_t<F>;

    /**
     * @brief Constructs a `ScopeGuard` with a given callable.
     * @param f The callable object to execute on scope exit.
     */
    explicit ScopeGuard(F &&f) noexcept(std::is_nothrow_move_constructible_v<FnT>)
        : m_func(std::forward<F>(f)), m_active(true)
    {
    }

    /**
     * @brief Move constructor. Transfers ownership of the callable from another guard.
     * @param rhs The source guard to move from. The source guard is dismissed.
     */
    ScopeGuard(ScopeGuard &&rhs) noexcept(std::is_nothrow_move_constructible_v<FnT>)
        : m_func(std::move(rhs.m_func)), m_active(rhs.m_active)
    {
        rhs.dismiss();
    }

    /**
     * @brief Destructor. Executes the callable if the guard is active.
     * Any exceptions thrown by the callable are caught and swallowed.
     */
    ~ScopeGuard() noexcept
    {
        if (m_active)
        {
            try
            {
                m_func();
            }
            catch (...)
            {
                // swallow exceptions in destructor
            }
        }
    }

    // Prevent copies
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;

    // Optional: leave move assignment deleted for simplicity (as you had)
    ScopeGuard &operator=(ScopeGuard &&) = delete;

    /**
     * @brief Deactivates the guard, preventing the callable from being executed on scope exit.
     */
    void dismiss() noexcept { m_active = false; }

    /**
     * @brief Executes the callable immediately and then dismisses the guard.
     * Any exceptions thrown by the callable are caught and swallowed.
     */
    void invoke() noexcept
    {
        if (m_active)
        {
            try
            {
                m_func();
            }
            catch (...)
            {
                // swallow or log
            }
            m_active = false;
        }
    }

  private:
    FnT m_func;
    bool m_active;
};

/**
 * @brief A factory function to create a `ScopeGuard`.
 * @tparam F The type of the callable object.
 * @param f The callable object to be managed by the guard.
 * @return A `ScopeGuard` instance for the given callable.
 */
template <typename F> ScopeGuard<F> make_scope_guard(F &&f)
{
    return ScopeGuard<F>(std::forward<F>(f));
}

} // namespace pylabhub::basics

