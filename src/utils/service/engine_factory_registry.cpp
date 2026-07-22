/**
 * @file engine_factory_registry.cpp
 * @brief Plugin registry for ScriptEngine factories (pylabhub-utils side).
 *
 * Defines `register_engine_factory` and `create_engine` (the dispatcher).
 * The actual factory implementation lives in `pylabhub-scripting`
 * (`engine_factory.cpp`) and registers itself via `init_scripting()`
 * called from each binary's `main()`.  See
 * `utils/script_engine_factory.hpp` for the design rationale.
 */

#include "utils/script_engine_factory.hpp"
#include "utils/script_engine.hpp" // complete type required for unique_ptr<ScriptEngine> dtor

#include <atomic>

namespace pylabhub::scripting
{

namespace
{

// Single process-wide registered factory.  Acquire/release semantics so
// a thread that observes a non-null pointer reads a fully-constructed
// factory function.  A `std::atomic` of a function pointer is required
// to be lock-free on every supported platform.
std::atomic<EngineFactoryFn> g_factory{nullptr};

// GIL-pickup helpers — registered by pylabhub-scripting's
// `init_scripting()`.  Both pointers are set together; we read them
// atomically and treat (nullptr, nullptr) as "no scripting layer
// available" → workers see an empty PythonGilLease and skip GIL work.
std::atomic<PythonGilAcquireFn> g_gil_acquire{nullptr};
std::atomic<PythonGilReleaseFn> g_gil_release{nullptr};

// Idle-wait global-lock release helpers — opposite-direction mirror of
// the GIL-pickup pair.  Same registration pattern.  Treated as
// (nullptr, nullptr) when scripting is not linked.
std::atomic<EngineGlobalLockReleaseFn> g_lock_release{nullptr};
std::atomic<EngineGlobalLockReacquireFn> g_lock_reacquire{nullptr};

} // namespace

void register_engine_factory(EngineFactoryFn fp) noexcept
{
    g_factory.store(fp, std::memory_order_release);
}

std::unique_ptr<ScriptEngine> create_engine(const config::ScriptConfig &sc) noexcept
{
    auto fp = g_factory.load(std::memory_order_acquire);
    if (fp == nullptr)
        return nullptr;
    return fp(sc);
}

void register_python_gil_helpers(PythonGilAcquireFn acquire, PythonGilReleaseFn release) noexcept
{
    g_gil_acquire.store(acquire, std::memory_order_release);
    g_gil_release.store(release, std::memory_order_release);
}

PythonGilToken acquire_python_gil() noexcept
{
    auto fp = g_gil_acquire.load(std::memory_order_acquire);
    if (fp == nullptr)
        return nullptr;
    return fp();
}

void release_python_gil(PythonGilToken token) noexcept
{
    if (token == nullptr)
        return;
    auto fp = g_gil_release.load(std::memory_order_acquire);
    if (fp == nullptr)
        return; // mismatch (acquire registered, release not) — defensive
    fp(token);
}

void register_engine_global_lock_release_helpers(EngineGlobalLockReleaseFn release,
                                                 EngineGlobalLockReacquireFn reacquire) noexcept
{
    g_lock_release.store(release, std::memory_order_release);
    g_lock_reacquire.store(reacquire, std::memory_order_release);
}

EngineGlobalLockToken release_engine_global_lock() noexcept
{
    auto fp = g_lock_release.load(std::memory_order_acquire);
    if (fp == nullptr)
        return nullptr;
    return fp();
}

void reacquire_engine_global_lock(EngineGlobalLockToken token) noexcept
{
    if (token == nullptr)
        return;
    auto fp = g_lock_reacquire.load(std::memory_order_acquire);
    if (fp == nullptr)
        return; // mismatch (release registered, reacquire not) — defensive
    fp(token);
}

} // namespace pylabhub::scripting
