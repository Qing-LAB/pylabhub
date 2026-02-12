#pragma once

/*******************************************************************************
 * @file RecursionGuard.hpp
 * @brief A thread-local, RAII-based guard to detect and prevent re-entrant calls.
 *
 * Uses a fixed-capacity, stack-allocated (thread-local) buffer. No heap allocation.
 * If recursion depth exceeds the limit, the constructor panics (PLH_PANIC) with
 * context and stack trace instead of throwing.
 ******************************************************************************/
#include "utils/debug_info.hpp"

#include <algorithm> // for std::find
#include <array>
#include <cstddef>   // for nullptr

namespace pylabhub::basics
{

// NOTE: This class is intentionally an inline, header-only utility and is NOT
// part of the public DLL ABI. It is for internal use by other utilities
// within this library.

// Configurable at compile time via CMake option PLH_RECURSION_GUARD_MAX_DEPTH (default 64).
#ifndef PLH_RECURSION_GUARD_MAX_DEPTH
#define PLH_RECURSION_GUARD_MAX_DEPTH 64
#endif

/** Maximum recursion depth per thread. Exceeding this causes panic (PLH_PANIC). */
constexpr size_t kMaxRecursionDepth = PLH_RECURSION_GUARD_MAX_DEPTH;

struct RecursionStack
{
    std::array<const void *, kMaxRecursionDepth> keys{};
    size_t size = 0;
};

/**
 * @brief Gets the thread-local stack for tracking recursion.
 * @return A mutable reference to the thread-local stack. No allocation; storage
 *         is fixed-capacity. Does not throw.
 */
inline RecursionStack &get_recursion_stack() noexcept
{
    static thread_local RecursionStack g_recursion_stack;
    return g_recursion_stack;
}

/** Called when recursion depth would exceed kMaxRecursionDepth. Does not return. */
[[noreturn]] inline void recursion_guard_panic() noexcept
{
    PLH_PANIC("RecursionGuard: max recursion depth ({}) exceeded.", kMaxRecursionDepth);
}

/**
 * @class RecursionGuard
 * @brief An RAII guard to detect re-entrant function calls on a per-object, per-thread basis.
 *
 * This guard works by pushing a key (typically a pointer to an object instance) onto a
 * thread-local, fixed-capacity stack upon construction. It provides a static method,
 * `is_recursing`, to check if a given key is already on the stack for the current thread.
 * The key is popped from the stack upon destruction, ensuring correct state even with
 * exceptions.
 *
 * No heap allocation: storage is pre-allocated (thread-local). If recursion depth
 * exceeds kMaxRecursionDepth, the constructor panics (abort) and does not return.
 *
 * @warning The caller must ensure that the `key` pointer remains valid for the entire
 *          lifetime of the RecursionGuard instance.
 */
class RecursionGuard
{
  public:
    /**
     * @brief Constructs the guard and pushes the key onto the thread-local recursion stack.
     * @param key A pointer used as a unique identifier for the scope being guarded. Can be
     *            nullptr, in which case the guard is inert.
     * @note If recursion depth would exceed kMaxRecursionDepth, panics (abort) and does not return.
     */
    explicit RecursionGuard(const void *key) noexcept : key_(key)
    {
        if (key_)
        {
            auto &st = get_recursion_stack();
            if (st.size >= kMaxRecursionDepth)
                recursion_guard_panic();
            st.keys[st.size++] = key_;
        }
    }

    /**
     * @brief Destructor. Removes the key from the thread-local recursion stack.
     *
     * Guaranteed `noexcept`. Handles both LIFO and non-LIFO destruction order.
     * In the non-LIFO case, removes the first matching occurrence of the key.
     */
    ~RecursionGuard() noexcept
    {
        if (key_ == nullptr)
        {
            return; // Guard is inert (e.g., it was moved-from)
        }

        auto &st = get_recursion_stack();
        if (st.size > 0 && st.keys[st.size - 1] == key_)
        {
            // Common case: LIFO.
            --st.size;
        }
        else if (st.size > 0)
        {
            // Non-LIFO: find and remove first occurrence (shift tail down).
            auto *beg = st.keys.data();
            auto *end = beg + st.size;
            auto *it = std::find(beg, end, key_);
            if (it != end)
            {
                std::move(it + 1, end, it);
                --st.size;
            }
        }
    }

    // --- Rule of 5: Movable (via constructor), but not Copyable or Assignable ---
    RecursionGuard(const RecursionGuard &) = delete;
    RecursionGuard &operator=(const RecursionGuard &) = delete;
    RecursionGuard &operator=(RecursionGuard &&) = delete;

    /**
     * @brief Move constructor. Transfers ownership of the key from another guard.
     * @param other The guard to move from. Its key will be set to nullptr, making it inert.
     *        The new guard becomes responsible for popping the key from the stack.
     */
    RecursionGuard(RecursionGuard &&other) noexcept : key_(other.key_) { other.key_ = nullptr; }

    /**
     * @brief Checks if a given key is already present on the current thread's recursion stack.
     * @param key The pointer key to check for.
     * @return `true` if the key is already on the stack for this thread, `false` otherwise.
     */
    [[nodiscard]] static bool is_recursing(const void *key) noexcept
    {
        if (key == nullptr)
            return false;
        const auto &st = get_recursion_stack();
        if (st.size == 0)
            return false;
        if (st.keys[st.size - 1] == key)
            return true;
        const auto *beg = st.keys.data();
        return std::find(beg, beg + st.size, key) != beg + st.size;
    }

  private:
    const void *key_;
};

} // namespace pylabhub::basics
