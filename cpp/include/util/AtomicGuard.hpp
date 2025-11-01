#pragma once
// AtomicGuard.hpp
// Lightweight RAII helper for incrementing/decrementing an atomic counter.
// Namespace: pylabhub::util
//
// Features:
//  - RAII increment-on-construct (default).
//  - Optional attach-without-increment constructor (do_increment=false) for try_acquire_if_zero.
//  - try_acquire_if_zero(prev) to atomically set counter 0->1 only if 0.
//  - explicit increment()/decrement() helpers for manual control.
//  - load() to read current value (non-modifying).
//  - Movable but non-copyable (transfer ownership semantics).
//
// Place at: include/util/AtomicGuard.hpp
//
// Examples:
//
//  // RAII increment
//  std::atomic<int> c{0};
//  pylabhub::util::AtomicGuard<int> g(&c); // increments to 1, decrements when g destroyed
//
//  // Move semantics
//  pylabhub::util::AtomicGuard<int> g1(&c);
//  pylabhub::util::AtomicGuard<int> g2 = std::move(g1); // g2 owns the increment, g1 inactive
//
//  // try_acquire_if_zero
//  std::atomic<int> c2{0};
//  pylabhub::util::AtomicGuard<int> g3(&c2, /*do_increment=*/false);
//  int prev;
//  if (g3.try_acquire_if_zero(prev)) { /* got exclusive owner */ }
//  else { /* prev contains observed value */ }

#include <atomic>
#include <type_traits>
#include <utility>

namespace pylabhub::util
{

template <typename T> class AtomicGuard
{
    static_assert(std::is_integral_v<T>, "AtomicGuard requires an integral type");

  public:
    /// Default constructor - inactive guard.
    AtomicGuard() noexcept : _counter(nullptr), _active(false), _order(std::memory_order_acq_rel) {}

    /// Construct and optionally increment the given atomic counter.
    /// \param counter pointer to std::atomic<T>
    /// \param do_increment if true (default) the constructor performs an increment (RAII).
    explicit AtomicGuard(std::atomic<T> *counter, bool do_increment = true,
                         std::memory_order order = std::memory_order_acq_rel) noexcept
        : _counter(counter), _active(false), _order(order)
    {
        if (_counter && do_increment)
        {
            _counter->fetch_add(static_cast<T>(1), _order);
            _active = true;
        }
    }

    /// Movable (transfer ownership). After move, source is inactive.
    AtomicGuard(AtomicGuard &&other) noexcept
        : _counter(other._counter), _active(other._active), _order(other._order)
    {
        other._counter = nullptr;
        other._active = false;
    }

    AtomicGuard &operator=(AtomicGuard &&other) noexcept
    {
        if (this != &other)
        {
            release();
            _counter = other._counter;
            _active = other._active;
            _order = other._order;
            other._counter = nullptr;
            other._active = false;
        }
        return *this;
    }

    // Non-copyable
    AtomicGuard(const AtomicGuard &) = delete;
    AtomicGuard &operator=(const AtomicGuard &) = delete;

    ~AtomicGuard() noexcept { release(); }

    /// Release early: decrement if active. Safe to call many times.
    void release() noexcept
    {
        if (_active && _counter)
        {
            _counter->fetch_sub(static_cast<T>(1), _order);
            _active = false;
            _counter = nullptr;
        }
        else
        {
            _active = false;
            _counter = nullptr;
        }
    }

    /// Manual increment (non-RAII). Increments the underlying counter and marks guard active.
    /// Caller must use with care to avoid overflow or mismatched decrements.
    void increment() noexcept
    {
        if (!_counter)
            return;
        _counter->fetch_add(static_cast<T>(1), _order);
        _active = true;
    }

    /// Manual decrement (non-RAII). Decrements underlying counter and deactivates guard.
    void decrement() noexcept
    {
        if (!_counter)
            return;
        _counter->fetch_sub(static_cast<T>(1), _order);
        _active = false;
    }

    /// Read current value of atomic counter without modifying it.
    T load(std::memory_order mo = std::memory_order_acquire) const noexcept
    {
        if (!_counter)
            return T(0);
        return _counter->load(mo);
    }

    /// Returns whether the guard currently owns an increment (will decrement on destruction).
    bool active() const noexcept { return _active && _counter != nullptr; }
    explicit operator bool() const noexcept { return active(); }

    /// try_acquire_if_zero(prev)
    /// Attempt to atomically set *counter to 1 only if its current value is 0.
    /// - On success: guard becomes active, returns true, previous set to 0.
    /// - On failure: guard remains inactive, returns false, previous receives observed value.
    bool try_acquire_if_zero(T &previous) noexcept
    {
        if (!_counter)
        {
            previous = T(0);
            return false;
        }
        if (_active)
        {
            previous = _counter->load(std::memory_order_acquire);
            return false;
        }
        T expected = T(0);
        bool exchanged = _counter->compare_exchange_strong(
            expected, static_cast<T>(1), std::memory_order_acq_rel, std::memory_order_acquire);
        if (exchanged)
        {
            _active = true;
            previous = T(0);
            return true;
        }
        else
        {
            previous = expected;
            return false;
        }
    }

    /// try_acquire_if_zero without returning previous
    bool try_acquire_if_zero() noexcept
    {
        T tmp{};
        return try_acquire_if_zero(tmp);
    }

  private:
    std::atomic<T> *_counter;
    bool _active;
    std::memory_order _order;
};

} // namespace pylabhub::util
