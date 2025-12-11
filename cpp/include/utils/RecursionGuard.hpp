#pragma once

/*******************************************************************************
 * @file RecursionGuard.hpp
 * @brief A thread-local, RAII-based guard to detect and prevent re-entrant calls.
 ******************************************************************************/

#include <algorithm> // for std::find
#include <vector>

namespace pylabhub::utils
{

// NOTE: This class is intentionally an inline, header-only utility and is NOT
// part of the public DLL ABI. It is for internal use by other utilities
// within this library.
// Its constructor is not guaranteed to be noexcept (std::vector::push_back can
// throw std::bad_alloc), which is an unacceptable risk for a public-facing API.

// Helper function to get the thread-local stack. Defined as a function-local
// static to ensure it exists once per thread.
inline std::vector<const void *> &get_recursion_stack()
{
    static thread_local std::vector<const void *> g_stack;
    return g_stack;
}

class RecursionGuard
{
  public:
    // RAII guard. Pushes key onto thread-local stack on construction, pops on destruction.
    explicit RecursionGuard(const void *key) : key_(key)
    {
        get_recursion_stack().push_back(key_);
    }

    ~RecursionGuard()
    {
        auto &stack = get_recursion_stack();
        if (!stack.empty() && stack.back() == key_)
        {
            // Common case: The guard is destroyed in the reverse order of creation (LIFO).
            stack.pop_back();
        }
        else
        {
            // Defensive removal for out-of-order destruction.
            auto it = std::find(stack.begin(), stack.end(), key_);
            if (it != stack.end())
            {
                stack.erase(it);
            }
        }
    }

    RecursionGuard(const RecursionGuard &) = delete;
    RecursionGuard &operator=(const RecursionGuard &) = delete;

    // Checks if the given key is already present on the current thread's stack.
    static bool is_recursing(const void *key)
    {
        auto const &stack = get_recursion_stack();
        return std::find(stack.begin(), stack.end(), key) != stack.end();
    }

  private:
    const void *key_;
};

} // namespace pylabhub::utils
