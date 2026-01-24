#pragma once

/*******************************************************************************
 * @file RecursionGuard.hpp
 * @brief A thread-local, RAII-based guard to detect and prevent re-entrant calls.
 ******************************************************************************/
#include <algorithm> // for std::find
#include <cstddef>   // for nullptr
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
 * @throws std::bad_alloc if memory allocation fails during the initial
 *         reservation or thread-local storage setup. This typically only
 *         occurs on the first call within a thread.
 */
inline recursion_stack_t &get_recursion_stack()
{
    static thread_local recursion_stack_t g_recursion_stack;
    // On first use, reserve space to avoid allocations for typical recursion depths.
    if (g_recursion_stack.capacity() == 0)
    {
        // This may throw std::bad_alloc if the initial allocation fails.
        g_recursion_stack.reserve(16);
    }
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
 * @warning The constructor may throw `std::bad_alloc` if memory allocation
 *          for the thread-local stack fails. This can happen on the first
 *          use of a `RecursionGuard` in a thread (during initial capacity
 *          reservation) or on subsequent uses if the recursion depth exceeds
 *          the reserved capacity. Callers should be prepared to handle this
 *          exception if operating in a memory-constrained environment.
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
     * @throws std::bad_alloc if `get_recursion_stack()` fails its initial allocation or
     *         if `push_back` needs to reallocate and fails.
     */
    explicit RecursionGuard(const void *key) : key_(key)
    {
        if (key_)
        {
            get_recursion_stack().push_back(key_);
        }
    }

    /**
     * @brief Destructor. Removes the key from the thread-local recursion stack.
     *
     * This is guaranteed `noexcept`. It handles both LIFO (last-in, first-out)
     * and non-LIFO destruction order. In the non-LIFO case, it removes the
     * first matching occurrence of the key from the stack to ensure correctness.
     */
    ~RecursionGuard() noexcept
    {
        if (key_ == nullptr)
        {
            return; // Guard is inert (e.g., it was moved-from)
        }

        auto &stack = get_recursion_stack();
        if (!stack.empty() && stack.back() == key_)
        {
            // Common case: The guard is destroyed in the reverse order of creation (LIFO).
            stack.pop_back();
        }
        else if (!stack.empty())
        {
            // Non-LIFO case: search for the key and remove the first occurrence.
            auto it = std::find(stack.begin(), stack.end(), key_);
            if (it != stack.end())
            {
                stack.erase(it);
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
        {
            return false;
        }
        const auto &stack = get_recursion_stack();
        if (stack.empty())
        {
            return false;
        }

        // Fast-path: if the most-recent entry matches, return true quickly.
        if (stack.back() == key)
        {
            return true;
        }

        // Otherwise fall back to full scan.
        return std::find(stack.cbegin(), stack.cend(), key) != stack.cend();
    }

  private:
    const void *key_;
};

} // namespace pylabhub::basics
