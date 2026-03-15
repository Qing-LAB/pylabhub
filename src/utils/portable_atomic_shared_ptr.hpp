#pragma once
// portable_atomic_shared_ptr.hpp — internal implementation detail
//
// std::atomic<std::shared_ptr<T>> is a C++20 library specialisation that requires
// explicit support from the C++ standard library. GCC libstdc++ and MSVC support it, but
// Clang libc++ (as of 17) does not fully implement it, causing build failures on macOS and
// any project using libc++.
//
// This file provides PortableAtomicSharedPtr<T>: a drop-in replacement using a
// std::mutex for load/store operations. The mutex guarantees full sequential consistency
// (stronger than any std::memory_order_*), so the memory_order parameters on load() and
// store() are accepted but ignored — the mutex provides the necessary synchronisation.
//
// Usage:
//   PortableAtomicSharedPtr<Foo> m_handler;      // default-constructed (null)
//   m_handler.store(std::make_shared<Foo>(), std::memory_order_release);
//   auto h = m_handler.load(std::memory_order_acquire);

#include <memory>
#include <mutex>

namespace pylabhub::utils::detail
{

template <typename T>
class PortableAtomicSharedPtr
{
  public:
    PortableAtomicSharedPtr() = default;
    explicit PortableAtomicSharedPtr(std::nullptr_t) noexcept {}

    // Non-copyable, non-movable (same contract as std::atomic).
    PortableAtomicSharedPtr(const PortableAtomicSharedPtr &) = delete;
    PortableAtomicSharedPtr &operator=(const PortableAtomicSharedPtr &) = delete;

    [[nodiscard]] std::shared_ptr<T> load(
        std::memory_order /*order*/ = std::memory_order_acquire) const noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        return ptr_;
    }

    void store(std::shared_ptr<T> p,
               std::memory_order /*order*/ = std::memory_order_release) noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        ptr_ = std::move(p);
    }

  private:
    mutable std::mutex mu_;
    std::shared_ptr<T> ptr_;
};

} // namespace pylabhub::utils::detail
