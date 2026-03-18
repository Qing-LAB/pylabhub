#pragma once
// portable_atomic_shared_ptr.hpp — internal implementation detail
//
// std::atomic<std::shared_ptr<T>> is a C++20 library specialisation that requires
// explicit support from the C++ standard library. GCC libstdc++ and MSVC support it, but
// Clang libc++ (as of 17) does not fully implement it, causing build failures on macOS and
// any project using libc++.
//
// This file provides PortableAtomicSharedPtr<T>: when the standard library supports
// std::atomic<std::shared_ptr<T>> (C++20, __cpp_lib_atomic_shared_ptr), it delegates
// directly and honours the caller's memory_order.  Otherwise it falls back to a
// std::mutex wrapper where the mutex guarantees full sequential consistency (stronger
// than any std::memory_order_*).  Define PLH_FORCE_MUTEX_ATOMIC_SHARED_PTR=1 to force
// the mutex path (e.g. for testing both paths on platforms that support the native one).
//
// Usage:
//   PortableAtomicSharedPtr<Foo> m_handler;      // default-constructed (null)
//   m_handler.store(std::make_shared<Foo>(), std::memory_order_release);
//   auto h = m_handler.load(std::memory_order_acquire);

#include <atomic>
#include <memory>
#include <mutex>
#if __has_include(<version>)
#include <version>
#endif

#if defined(PLH_FORCE_MUTEX_ATOMIC_SHARED_PTR) && PLH_FORCE_MUTEX_ATOMIC_SHARED_PTR
#define PLH_HAS_STD_ATOMIC_SHARED_PTR 0
#elif defined(__cpp_lib_atomic_shared_ptr) && (__cpp_lib_atomic_shared_ptr >= 201711L)
#define PLH_HAS_STD_ATOMIC_SHARED_PTR 1
#else
#define PLH_HAS_STD_ATOMIC_SHARED_PTR 0
#endif

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
        std::memory_order order = std::memory_order_acquire) const noexcept
    {
#if PLH_HAS_STD_ATOMIC_SHARED_PTR
        return ptr_.load(order);
#else
        std::lock_guard<std::mutex> lk(mu_);
        return ptr_;
#endif
    }

    void store(std::shared_ptr<T> p,
               std::memory_order order = std::memory_order_release) noexcept
    {
#if PLH_HAS_STD_ATOMIC_SHARED_PTR
        ptr_.store(std::move(p), order);
#else
        std::lock_guard<std::mutex> lk(mu_);
        ptr_ = std::move(p);
#endif
    }

  private:
#if PLH_HAS_STD_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<T>> ptr_{nullptr};
#else
    mutable std::mutex mu_;
    std::shared_ptr<T> ptr_;
#endif
};

} // namespace pylabhub::utils::detail
