#ifndef PYLHUB_SCOPE_GUARD_HPP
#define PYLHUB_SCOPE_GUARD_HPP

#include <utility>
#include <type_traits>
#include <optional>

namespace pylabhub::basics
{

template <typename F>
class ScopeGuard
{
  public:
    using FnT = std::decay_t<F>;

    explicit ScopeGuard(F &&f) noexcept(std::is_nothrow_move_constructible_v<FnT>)
        : m_func(std::forward<F>(f)), m_active(true) {}

    ScopeGuard(ScopeGuard &&rhs) noexcept(std::is_nothrow_move_constructible_v<FnT>)
        : m_func(std::move(rhs.m_func)), m_active(rhs.m_active)
    {
        rhs.dismiss();
    }

    ~ScopeGuard() noexcept
    {
        if (m_active)
        {
            try {
                m_func();
            } catch (...) {
                // swallow exceptions in destructor
            }
        }
    }

    // Prevent copies
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;

    // Optional: leave move assignment deleted for simplicity (as you had)
    ScopeGuard &operator=(ScopeGuard &&) = delete;

    // Dismiss prevents the callable from running on destruction
    void dismiss() noexcept { m_active = false; }

    // Run the callable immediately (if active) and then dismiss
    void invoke() noexcept
    {
        if (m_active)
        {
            try {
                m_func();
            } catch (...) {
                // swallow or log
            }
            m_active = false;
        }
    }

  private:
    FnT m_func;
    bool m_active;
};

template <typename F>
ScopeGuard<F> make_scope_guard(F &&f)
{
    return ScopeGuard<F>(std::forward<F>(f));
}

} // namespace pylabhub::utils

#endif // PYLHUB_SCOPE_GUARD_HPP
