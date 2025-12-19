#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>
#include <vector>

namespace
{
// A global flag to ensure initialization and finalization are paired correctly.
std::atomic<bool> g_is_initialized = {false};

// --- Callback Registries ---
struct Finalizer
{
    std::string name;
    std::function<void()> func;
    std::chrono::milliseconds timeout;
};

std::mutex g_registry_mutex;
std::vector<std::function<void()>> g_initializers;
std::vector<Finalizer> g_finalizers;

} // namespace

namespace pylabhub::utils
{

void RegisterInitializer(std::function<void()> func)
{
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_initializers.push_back(std::move(func));
}

void RegisterFinalizer(std::string name,
                       std::function<void()> func,
                       std::chrono::milliseconds timeout)
{
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_finalizers.push_back({std::move(name), std::move(func), timeout});
}

void Initialize()
{
    if (g_is_initialized.exchange(true))
    {
        // Already initialized, do nothing.
        return;
    }

    // 1. Eagerly create the logger instance to start its worker thread.
    Logger::instance();
    LOGGER_TRACE("pylabub::utils::Initialize - Core subsystems started.");

    // 2. Run all registered user initializers.
    decltype(g_initializers) initializers_copy;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        initializers_copy = g_initializers;
    }

    for (const auto &initializer : initializers_copy)
    {
        try
        {
            initializer();
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("A registered initializer threw an exception: {}", e.what());
        }
        catch (...)
        {
            LOGGER_ERROR("A registered initializer threw an unknown exception.");
        }
    }
    LOGGER_TRACE("pylabub::utils::Initialize - All initializers executed.");
}

void Finalize()
{
    if (!g_is_initialized.exchange(false))
    {
        // Not initialized or already finalized, do nothing.
        return;
    }

    LOGGER_TRACE("pylabub::utils::Finalize - Shutdown process started.");

    // 1. Run all registered user finalizers in LIFO order.
    decltype(g_finalizers) finalizers_copy;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        finalizers_copy = g_finalizers;
        std::reverse(finalizers_copy.begin(), finalizers_copy.end());
    }

    for (const auto &finalizer : finalizers_copy)
    {
        try
        {
            LOGGER_TRACE("Executing finalizer: '{}'", finalizer.name);
            std::future<void> future = std::async(std::launch::async, finalizer.func);
            auto status = future.wait_for(finalizer.timeout);

            if (status == std::future_status::timeout)
            {
                LOGGER_WARN("Finalizer '{}' timed out after {}ms.",
                            finalizer.name,
                            finalizer.timeout.count());
            }
            else
            {
                // future.get() will re-throw any exception caught by the async task.
                future.get();
                LOGGER_TRACE("Finalizer '{}' completed successfully.", finalizer.name);
            }
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("Finalizer '{}' threw an exception: {}", finalizer.name, e.what());
        }
        catch (...)
        {
            LOGGER_ERROR("Finalizer '{}' threw an unknown exception.", finalizer.name);
        }
    }

    // 2. Gracefully shut down the logger, flushing all messages.
    LOGGER_TRACE("pylabub::utils::Finalize - Shutting down core subsystems.");
    Logger::instance().shutdown();
}

} // namespace pylabhub::utils
