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
#include "plh_platform.hpp"

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

ShutdownOutcome timedShutdown(const std::function<void()> &func, std::chrono::milliseconds timeout,
                              std::string_view label)
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
        std::mutex mu;
        std::condition_variable cv;
        bool completed{false};
        std::exception_ptr ex_ptr{nullptr};
    };
    auto state = std::make_shared<SharedState>();

    // The worker reports its own ENTER/EXIT into the critical-report
    // buffer.  This
    // has to happen HERE rather than in the caller: on timeout the caller
    // detaches this thread and returns, so the caller can only ever record
    // that the module overran — never whether it eventually finished.  An
    // ENTER with no matching EXIT in the dump names the stuck module.
    // Formatting goes through format_to_n into a stack buffer so reporting
    // never allocates on a teardown path.
    const std::string label_copy(label);
    // Named rather than passed inline, because it runs on EITHER of two
    // paths: normally on a worker thread with a deadline, and — when the
    // thread cannot be created — directly on this one.  Single-sourcing it
    // means the inline fallback produces byte-identical reports and
    // identical exception capture, so a reader of the trace does not have
    // to know which path ran to interpret it.
    auto reported_run = [func, state, label_copy]()
    {
        {
            char line[kCriticalReportLineBytes];
            const auto res = fmt::format_to_n(line, sizeof(line),
                                              "event=ShutdownEnter module='{}'", label_copy);
            critical_report(line, res.size);
        }
        bool threw = false;
        try
        {
            func();
        }
        catch (...)
        {
            threw = true;
            state->ex_ptr = std::current_exception();
        }
        {
            char line[kCriticalReportLineBytes];
            const auto res =
                fmt::format_to_n(line, sizeof(line), "event=ShutdownExit module='{}' outcome={}",
                                 label_copy, threw ? "threw" : "ok");
            critical_report(line, res.size);
            if (threw)
                pylabhub::debug::trace_mark_dirty();
        }
        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->completed = true;
        }
        state->cv.notify_one();
    };

    // Tail shared by every path that actually RAN the callback: translate a
    // captured exception into the outcome's message field.
    auto outcome_from_state = [&state]() -> ShutdownOutcome
    {
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
    };

    // Creating the thread is the one step here that can fail for a reason
    // outside this process's control — `pthread_create` returns EAGAIN at the
    // thread or memory limit, surfacing as `std::system_error`.  Letting that
    // escape is fatal rather than merely unfortunate: this function is
    // reached from `finalize()` under `~LifecycleGuard()`, which is
    // `noexcept`, so an escaping exception is `std::terminate` — no trace
    // dump, and every module after this one never tears down.  And the
    // triggering condition is resource exhaustion, i.e. exactly the shutdown
    // that most needs to finish and report.
    //
    // So the deadline is what we give up, not the teardown.  Running the
    // callback inline is the same bargain modules that opt into
    // `set_synchronous_shutdown(true)` already accept: no deadline, no
    // detach, and a hang here wedges the finalize thread.  That is strictly
    // better than terminating, and the trace says which one happened.
    std::thread thread;
    try
    {
        thread = std::thread(reported_run);
    }
    catch (const std::exception &e)
    {
        {
            char line[kCriticalReportLineBytes];
            const auto res = fmt::format_to_n(line, sizeof(line),
                                              "event=ShutdownWorkerSpawnFailed module='{}' "
                                              "action=ran_inline_without_deadline reason='{}'",
                                              label, e.what());
            critical_report(line, res.size);
        }
        pylabhub::debug::trace_mark_dirty();
        reported_run();
        return outcome_from_state();
    }

    {
        std::unique_lock<std::mutex> lk(state->mu);
        if (!state->cv.wait_for(lk, timeout, [&] { return state->completed; }))
        {
            // Timed out — detach the thread; shared_ptr keeps state alive, no UAF.
            // Design note: detach() is intentional here. Using std::async or
            // joining would block the caller indefinitely if func hangs. The
            // LifecycleManager marks timed-out modules as "contaminated" to
            // prevent future use and limit the impact of a runaway thread.
            // Record the detach from the CALLER: the worker is by
            // definition not going to report this itself, and a runaway
            // shutdown thread outliving the subsystem it was tearing down
            // is the single most important thing this pool can surface.
            {
                char line[kCriticalReportLineBytes];
                const auto res =
                    fmt::format_to_n(line, sizeof(line),
                                     "event=ShutdownDeadlineExceeded module='{}' deadline_ms={} "
                                     "action=detached",
                                     label, timeout.count());
                critical_report(line, res.size);
            }
            pylabhub::debug::trace_mark_dirty();
            thread.detach();
            return {false, true, {}};
        }
    }

    thread.join();

    return outcome_from_state();
}

} // namespace pylabhub::utils::lifecycle_internal
