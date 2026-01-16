#pragma once

/*******************************************************************************
 * @file RecursionGuard.hpp
 * @brief A thread-local, RAII-based guard to detect and prevent re-entrant calls.
 ******************************************************************************/

#include <algorithm> // for std::find
#include <type_traits>
#include <vector>

namespace pylabhub::basics
{

// NOTE: This class is intentionally an inline, header-only utility and is NOT
// part of the public DLL ABI. It is for internal use by other utilities
// within this library.
// Its constructor is not guaranteed to be noexcept (std::vector::push_back can
// throw std::bad_alloc), which is an unacceptable risk for a public-facing API.

// Alias for the underlying stack container so intent is clearer.
using recursion_stack_t = std::vector<const void *>;

/**
 * @brief Gets the thread-local stack for tracking recursion.
 * @return A mutable reference to the thread-local `recursion_stack_t`.
 */
inline recursion_stack_t &get_recursion_stack() noexcept
{
    static thread_local recursion_stack_t g_recursion_stack;
    return g_recursion_stack;
}

/**
 * @class RecursionGuard
 * @brief An RAII guard to detect re-entrant function calls on a per-object, per-thread basis.
 *
 * This guard works by pushing a key (typically a pointer to an object instance) onto a
 * thread-local stack upon construction. It provides a static method, `is_recursing`, to
 * check if a given key is already on the stack for the current thread. The key is popped
 * from the stack upon destruction, ensuring correct state even with exceptions.
 *
 * @warning The constructor is not `noexcept` as it may throw `std::bad_alloc`.
 */
class RecursionGuard
{
  public:
    /**
     * @brief Constructs the guard and pushes the key onto the thread-local recursion stack.
     * @param key A pointer used as a unique identifier for the scope being guarded.
     */
    explicit RecursionGuard(const void *key) : key_(key) { get_recursion_stack().push_back(key_); }
    /**
     * @brief Destructor. Removes the key from the thread-local recursion stack.
     *
     * This is guaranteed `noexcept`. It efficiently handles both LIFO (last-in, first-out)
     * and non-LIFO destruction order.
     */
    ~RecursionGuard() noexcept
    {
        auto &stack = get_recursion_stack();
        if (!stack.empty() && stack.back() == key_)
        {
            // Common case: The guard is destroyed in the reverse order of creation (LIFO).
            stack.pop_back();
        }
        else
        {
#if (__cplusplus >= 202002L)
            // C++20: std::erase removes all occurrences (no allocation, linear time).
            std::erase(stack, key_);
#else
            // Pre-C++20: find + erase first occurrence (linear time).
            auto it = std::find(stack.begin(), stack.end(), key_);
            if (it != stack.end())
            {
                stack.erase(it);
            }
#endif
        }
    }

    // Non-copyable and non-movable: moving a guard would change ownership semantics.
    RecursionGuard(const RecursionGuard &) = delete;
    RecursionGuard &operator=(const RecursionGuard &) = delete;
    RecursionGuard(RecursionGuard &&) = delete;
    RecursionGuard &operator=(RecursionGuard &&) = delete;

    /**
     * @brief Checks if a given key is already present on the current thread's recursion stack.
     * @param key The pointer key to check for.
     * @return `true` if the key is already on the stack for this thread, `false` otherwise.
     */
    [[nodiscard]] static bool is_recursing(const void *key) noexcept
    {
        const auto &stack = get_recursion_stack();
        if (stack.empty())
            return false;

        // Fast-path: if the most-recent entry matches, return true quickly.
        if (stack.back() == key)
            return true;

        // Otherwise fall back to full scan.
        return std::find(stack.cbegin(), stack.cend(), key) != stack.cend();
    }

  private:
    const void *key_;
};

} // namespace pylabhub::basics
