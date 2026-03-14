/**
 * @file lifecycle_helpers.cpp
 * @brief Internal helper functions shared by lifecycle.cpp, lifecycle_topology.cpp,
 *        and lifecycle_dynamic.cpp.
 *
 * These functions are NOT part of the public API. They are declared in
 * lifecycle_impl.hpp (namespace pylabhub::utils::lifecycle_internal) and defined
 * here exactly once — eliminating the code duplication that would arise from
 * placing the bodies in the header.
 */
#include "lifecycle_impl.hpp"

#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>

namespace pylabhub::utils::lifecycle_internal
{

// ---------------------------------------------------------------------------
// validate_module_name
// ---------------------------------------------------------------------------

void validate_module_name(std::string_view name, const char *param_name)
{
    if (name.empty())
    {
        throw std::invalid_argument(std::string("Lifecycle: ") + param_name +
                                    " must not be empty.");
    }
    if (name.size() > pylabhub::utils::ModuleDef::MAX_MODULE_NAME_LEN)
    {
        throw std::length_error(std::string("Lifecycle: ") + param_name + " exceeds maximum of " +
                                std::to_string(pylabhub::utils::ModuleDef::MAX_MODULE_NAME_LEN) +
                                " characters.");
    }
}

// ---------------------------------------------------------------------------
// timedShutdown
// ---------------------------------------------------------------------------

ShutdownOutcome timedShutdown(const std::function<void()> &func,
                              std::chrono::milliseconds    timeout)
{
    if (!func)
    {
        return {true, false, {}};
    }

    // Shared state lives on the heap so a detached thread can safely write to it
    // after timedShutdown() returns. Without shared ownership, detach() + return
    // would destroy the state while the thread still holds references → UAF/UB.
    struct SharedState
    {
        std::mutex              mu;
        std::condition_variable cv;
        bool                    completed{false};
        std::exception_ptr      ex_ptr{nullptr};
    };
    auto state = std::make_shared<SharedState>();

    std::thread thread([func, state]()
                       {
                           try
                           {
                               func();
                           }
                           catch (...)
                           {
                               state->ex_ptr = std::current_exception();
                           }
                           {
                               std::lock_guard<std::mutex> lk(state->mu);
                               state->completed = true;
                           }
                           state->cv.notify_one();
                       });

    {
        std::unique_lock<std::mutex> lk(state->mu);
        if (!state->cv.wait_for(lk, timeout, [&] { return state->completed; }))
        {
            // Timed out — detach the thread; shared_ptr keeps state alive, no UAF.
            // Design note: detach() is intentional here. Using std::async or
            // joining would block the caller indefinitely if func hangs. The
            // LifecycleManager marks timed-out modules as "contaminated" to
            // prevent future use and limit the impact of a runaway thread.
            thread.detach();
            return {false, true, {}};
        }
    }

    thread.join();

    if (state->ex_ptr)
    {
        try
        {
            std::rethrow_exception(state->ex_ptr);
        }
        catch (const std::exception &e)
        {
            return {false, false, e.what()};
        }
        catch (...)
        {
            return {false, false, "unknown exception"};
        }
    }
    return {true, false, {}};
}

} // namespace pylabhub::utils::lifecycle_internal
