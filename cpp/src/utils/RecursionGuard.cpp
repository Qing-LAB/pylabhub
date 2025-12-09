#include "utils/RecursionGuard.hpp"
#include <algorithm> // for std::find

namespace pylabhub::utils
{

// Definition of the thread-local static member.
// Each thread gets its own instance of this vector.
static thread_local std::vector<const void *> g_stack;

RecursionGuard::RecursionGuard(const void *key) : key_(key)
{
    g_stack.push_back(key_);
}

RecursionGuard::~RecursionGuard()
{
    if (!g_stack.empty() && g_stack.back() == key_)
    {
        // Common case: The guard is destroyed in the reverse order of creation (LIFO).
        g_stack.pop_back();
    }
    else
    {
        // Defensive removal. This is a safety net for cases where guards might be
        // destroyed out of order, which can happen with complex object lifetimes
        // but is not expected in normal RAII usage. A linear scan is acceptable
        // here as the stack is expected to be very small.
        auto it = std::find(g_stack.begin(), g_stack.end(), key_);
        if (it != g_stack.end())
        {
            g_stack.erase(it);
        }
    }
}

bool RecursionGuard::is_recursing(const void *key)
{
    return std::find(g_stack.begin(), g_stack.end(), key) != g_stack.end();
}

} // namespace pylabhub::utils