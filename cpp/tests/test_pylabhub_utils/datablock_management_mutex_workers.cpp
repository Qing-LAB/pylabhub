#include "datablock_management_mutex_workers.h"
#include "utils/shared_memory_mutex.hpp"
#include "plh_base.hpp" // For pylabhub::platform::get_pid()
#include "fmt/core.h"
#include <thread>
#include <chrono>

namespace pylabhub::tests::worker::datablock_management_mutex
{

int acquire_and_release(const std::string &shm_name)
{
    auto pid = pylabhub::platform::get_pid();
    fmt::print("Worker {}: Attempting to acquire mutex for SHM: {}\n", pid, shm_name);
    try
    {
        // Note: For Windows, pass nullptr for base address and 0 for offset as they're ignored
        // For POSIX, this is a simplified test - in real usage, the mutex needs actual shared
        // memory
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, false);

        {
            pylabhub::hub::DataBlockLockGuard lock(mutex);
            fmt::print("Worker {}: Mutex acquired for SHM: {}\n", pid, shm_name);
            // Hold the lock for a short duration to simulate work
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } // Lock released here via RAII

        fmt::print("Worker {}: Mutex released for SHM: {}\n", pid, shm_name);
        return 0; // Success
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Worker {}: Exception: {}\n", pid, e.what());
        return 1; // Failure
    }
}

int try_acquire_non_blocking(const std::string &shm_name)
{
    auto pid = pylabhub::platform::get_pid();
    fmt::print("Worker {}: Attempting to non-blockingly acquire mutex for SHM: {}\n", pid,
               shm_name);
    try
    {
        // Note: DataBlockMutex doesn't have try_lock() method currently
        // This test scenario needs to be implemented differently or the method needs to be added
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, false);

        {
            pylabhub::hub::DataBlockLockGuard lock(mutex);
            fmt::print("Worker {}: Mutex acquired for SHM: {}\n", pid, shm_name);
            // Hold the lock for a short duration
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } // Lock released here

        fmt::print("Worker {}: Mutex released for SHM: {}\n", pid, shm_name);
        return 0; // Success
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Worker {}: Exception: {}\n", pid, e.what());
        return 1; // Failure
    }
}

} // namespace pylabhub::tests::worker::datablock_management_mutex
