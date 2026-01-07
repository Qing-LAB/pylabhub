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

// Helper function to get the thread-local stack. The stack is defined as a
// function-local static to ensure it is initialized only once per thread
// in a thread-safe manner (guaranteed by C++11 and later).
inline recursion_stack_t &get_recursion_stack() noexcept
{
    static thread_local recursion_stack_t g_recursion_stack;
    return g_recursion_stack;
}

/**
 * @brief RAII guard that records a pointer key on a thread-local stack.
 *
 * Typical usage:
 *   RecursionGuard g(&some_object);
 *   if (RecursionGuard::is_recursing(&some_object)) { ... }
 *
 * Notes:
 *  - Construction may throw (std::vector::push_back may allocate).
 *  - Destruction never throws (marked noexcept) and will remove the key from
 *    the stack; if destruction is out-of-order the key is removed by search.
 */
class RecursionGuard
{
  public:
    // RAII guard. Pushes key onto thread-local stack on construction, pops on destruction.
    explicit RecursionGuard(const void *key) : key_(key) { get_recursion_stack().push_back(key_); }
    // Destructor must not throw. Vector operations on pointers are noexcept on
    // all mainstream implementations, but wrap defensively to guarantee noexcept.
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

    // Checks if the given key is already present on the current thread's stack.
    /** @return true if 'key' is present in the current thread's recursion stack. */
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
