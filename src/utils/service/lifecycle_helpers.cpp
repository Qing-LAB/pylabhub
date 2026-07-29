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
#include <cstring> // For std::memcpy (LifecycleTrace append)
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
// LifecycleTrace — shared narration pool for the shutdown workers
// ---------------------------------------------------------------------------

namespace
{
/// Guards the pool for the duration of a memcpy.
///
/// A `std::mutex` would be the reflexive choice, and is deliberately NOT
/// used: its destructor is non-trivial on some standard libraries, which
/// would drag the pool into static-destruction order — and the pool exists
/// precisely so that DETACHED shutdown workers can still narrate after
/// that point.  Destroying a lock underneath such a thread would make the
/// diagnostic aid a use-after-free on the unhappy path.  An `atomic_flag`
/// is trivially destructible, so the pool is constant-initialised and
/// never torn down.
///
/// The spin yields rather than burning the core: contention here shows up
/// under exactly the parallel load where teardown problems appear, and a
/// writer can be descheduled mid-critical-section.
class SpinGuard
{
  public:
    explicit SpinGuard(std::atomic_flag &f) noexcept : flag_(&f)
    {
        while (flag_->test_and_set(std::memory_order_acquire))
            std::this_thread::yield();
    }

    /// Bounded acquisition for the read path.  A reader must never be able
    /// to wedge: if a writer died or stalled holding the flag, an
    /// unbounded spin would hang the very dump that is supposed to explain
    /// the failure.  `held()` reports whether the pool was acquired.
    SpinGuard(std::atomic_flag &f, std::size_t max_attempts) noexcept : flag_(&f)
    {
        for (std::size_t i = 0; i < max_attempts; ++i)
        {
            if (!flag_->test_and_set(std::memory_order_acquire))
                return;
            std::this_thread::yield();
        }
        flag_ = nullptr; // gave up — do NOT clear a flag we never took
    }

    [[nodiscard]] bool held() const noexcept { return flag_ != nullptr; }

    ~SpinGuard()
    {
        if (flag_ != nullptr)
            flag_->clear(std::memory_order_release);
    }
    SpinGuard(const SpinGuard &) = delete;
    SpinGuard &operator=(const SpinGuard &) = delete;

  private:
    std::atomic_flag *flag_;
};

/// Monotonic microseconds for trace stamps.  steady_clock, not wall-clock:
/// these order events within one teardown and must not jump if the system
/// clock is stepped mid-shutdown.
unsigned long long trace_now_us() noexcept
{
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// Attempts before the read path gives up rather than spinning forever.
constexpr std::size_t kTraceReadAttempts = 10000;

/// The pool itself.  Constant-initialised (every member is trivially
/// constructible) and never destroyed, by design — a detached shutdown
/// worker may narrate at any point, including after static destruction
/// has begun.
LifecycleTrace g_lifecycle_trace;
} // namespace

LifecycleTrace &lifecycle_trace() noexcept
{
    return g_lifecycle_trace;
}

LifecycleTrace &LifecycleTrace::operator+=(std::string_view text) noexcept
{
    SpinGuard guard(lock_);
    const std::size_t room = kCapacity - len_;
    const std::size_t n = text.size() < room ? text.size() : room;
    if (n > 0)
    {
        std::memcpy(buf_.data() + len_, text.data(), n);
        len_ += n;
    }
    // Overflow is counted rather than silently swallowed, so a truncated
    // narrative announces itself instead of just looking short.
    dropped_ += text.size() - n;
    return *this;
}

std::size_t LifecycleTrace::copy_out(char *dst, std::size_t cap) const noexcept
{
    if (dst == nullptr || cap == 0)
        return 0;
    SpinGuard guard(lock_, kTraceReadAttempts);
    if (!guard.held())
        return 0;
    const std::size_t n = len_ < cap ? len_ : cap;
    std::memcpy(dst, buf_.data(), n);
    return n;
}

std::optional<std::size_t> LifecycleTrace::dropped() const noexcept
{
    SpinGuard guard(lock_, kTraceReadAttempts);
    if (!guard.held())
        return std::nullopt;
    return dropped_;
}

void LifecycleTrace::clear() noexcept
{
    SpinGuard guard(lock_);
    len_ = 0;
    dropped_ = 0;
    anomaly_ = false;
}

void LifecycleTrace::mark_anomaly() noexcept
{
    SpinGuard guard(lock_);
    anomaly_ = true;
}

bool LifecycleTrace::had_anomaly() const noexcept
{
    SpinGuard guard(lock_, kTraceReadAttempts);
    // A pool we could not acquire is itself suspicious; treat it as an
    // anomaly so the dump is emitted rather than silently skipped.
    return guard.held() ? anomaly_ : true;
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

    // The worker narrates its own ENTER/EXIT into the shared pool.  This
    // has to happen HERE rather than in the caller: on timeout the caller
    // detaches this thread and returns, so the caller can only ever record
    // that the module overran — never whether it eventually finished.  An
    // ENTER with no matching EXIT in the dump names the stuck module.
    // Formatting goes through format_to_n into a stack buffer so narrating
    // never allocates on a teardown path.
    const std::string label_copy(label);
    // Named rather than passed inline, because it runs on EITHER of two
    // paths: normally on a worker thread with a deadline, and — when the
    // thread cannot be created — directly on this one.  Single-sourcing it
    // means the inline fallback produces byte-identical narration and
    // identical exception capture, so a reader of the trace does not have
    // to know which path ran to interpret it.
    auto narrated_run = [func, state, label_copy]()
    {
        {
            char line[256];
            const auto res = fmt::format_to_n(
                line, sizeof(line), "[trace|t{}|{}us] event=ShutdownEnter module='{}'\n",
                pylabhub::platform::get_native_thread_id(), trace_now_us(), label_copy);
            lifecycle_trace() +=
                std::string_view(line, res.size < sizeof(line) ? res.size : sizeof(line));
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
            char line[256];
            const auto res = fmt::format_to_n(
                line, sizeof(line), "[trace|t{}|{}us] event=ShutdownExit module='{}' outcome={}\n",
                pylabhub::platform::get_native_thread_id(), trace_now_us(), label_copy,
                threw ? "threw" : "ok");
            lifecycle_trace() +=
                std::string_view(line, res.size < sizeof(line) ? res.size : sizeof(line));
            if (threw)
                lifecycle_trace().mark_anomaly();
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
        thread = std::thread(narrated_run);
    }
    catch (const std::exception &e)
    {
        {
            char line[256];
            const auto res = fmt::format_to_n(
                line, sizeof(line),
                "[trace|t{}|{}us] event=ShutdownWorkerSpawnFailed module='{}' "
                "action=ran_inline_without_deadline reason='{}'\n",
                pylabhub::platform::get_native_thread_id(), trace_now_us(), label, e.what());
            lifecycle_trace() +=
                std::string_view(line, res.size < sizeof(line) ? res.size : sizeof(line));
        }
        lifecycle_trace().mark_anomaly();
        narrated_run();
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
                char line[256];
                const auto res = fmt::format_to_n(
                    line, sizeof(line),
                    "[trace|t{}|{}us] event=ShutdownDeadlineExceeded module='{}' deadline_ms={} "
                    "action=detached\n",
                    pylabhub::platform::get_native_thread_id(), trace_now_us(), label,
                    timeout.count());
                lifecycle_trace() += std::string_view(
                    line, res.size < sizeof(line) ? res.size : sizeof(line));
            }
            lifecycle_trace().mark_anomaly();
            thread.detach();
            return {false, true, {}};
        }
    }

    thread.join();

    return outcome_from_state();
}

} // namespace pylabhub::utils::lifecycle_internal
