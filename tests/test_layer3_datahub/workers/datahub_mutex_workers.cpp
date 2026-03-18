#include "datahub_mutex_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "utils/data_block_mutex.hpp"
#include "plh_base.hpp" // For pylabhub::platform::get_pid()
#include "fmt/core.h"
#include <thread>
#include <chrono>
#include <cstdlib>

#if PYLABHUB_IS_POSIX
#include <unistd.h>
#endif

namespace pylabhub::tests::worker::datablock_management_mutex
{

// Print to stderr so ExpectWorkerOk (checks stderr) can verify
static void log(const char *msg) { fmt::print(stderr, "{}\n", msg); }

int acquire_and_release_creator(const std::string &shm_name)
{
    try
    {
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, true);
        {
            pylabhub::hub::DataBlockLockGuard lock(mutex);
            log("Mutex acquired");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        log("Mutex released");
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
}

int acquire_and_release_creator_hold_long(const std::string &shm_name)
{
    try
    {
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, true);
        {
            pylabhub::hub::DataBlockLockGuard lock(mutex);
            log("Mutex acquired");
            pylabhub::tests::helper::signal_test_ready(); // Parent spawns attacher, which blocks
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // Hold until attacher attaches
        }
        log("Mutex released");
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
}

int acquire_and_release_attacher(const std::string &shm_name)
{
    try
    {
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, false);
        {
            pylabhub::hub::DataBlockLockGuard lock(mutex);
            log("Mutex acquired");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        log("Mutex released");
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
}

int acquire_and_release_attacher_delayed(const std::string &shm_name)
{
    try
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Let creator create first
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, false);
        {
            pylabhub::hub::DataBlockLockGuard lock(mutex);
            log("Mutex acquired");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        log("Mutex released");
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
}

int zombie_creator_acquire_then_exit(const std::string &shm_name)
{
#if !PYLABHUB_IS_POSIX
    (void)shm_name;
    fmt::print(stderr, "Zombie creator only supported on POSIX\n");
    return 1;
#else
    try
    {
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, true);
        mutex.lock();
        log("Mutex acquired");
        // Exit without unlock - no destructors run
        _exit(0);
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
#endif
}

int zombie_attacher_recovers(const std::string &shm_name)
{
#if !PYLABHUB_IS_POSIX
    (void)shm_name;
    fmt::print(stderr, "Zombie attacher only supported on POSIX\n");
    return 1;
#else
    try
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let zombie exit and OS mark mutex abandoned
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, false);
        // Use timed lock so we never hang: robust mutex can block indefinitely on some kernels.
        const int timeout_ms = 5000;
        if (!mutex.try_lock_for(timeout_ms))
        {
            fmt::print(stderr,
                       "Recoverer timed out after {} ms (robust mutex did not return EOWNERDEAD).\n",
                       timeout_ms);
            return 1;
        }
        log("Mutex acquired");
        mutex.unlock();
        log("Mutex released");
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
#endif
}


int acquire_creator_signal_then_hold(const std::string &shm_name)
{
    try
    {
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, true);
        mutex.lock();
        // Signal parent that we hold the lock; parent spawns attacher.
        pylabhub::tests::helper::signal_test_ready();
        // Hold long enough for attacher to start and call try_lock_for(-1).
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint64_t release_ts = pylabhub::platform::monotonic_time_ns();
        mutex.unlock();
        fmt::print(stderr, "RELEASE_TS={}\n", release_ts);
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
}

int try_lock_infinite_with_timestamps(const std::string &shm_name)
{
    try
    {
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, false);
        uint64_t try_ts = pylabhub::platform::monotonic_time_ns();
        fmt::print(stderr, "TRY_TS={}\n", try_ts);
        if (!mutex.try_lock_for(-1))
        {
            fmt::print(stderr, "try_lock_for(-1) returned false\n");
            return 1;
        }
        uint64_t acquired_ts = pylabhub::platform::monotonic_time_ns();
        fmt::print(stderr, "ACQUIRED_TS={}\n", acquired_ts);
        mutex.unlock();
        return 0;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "Exception: {}\n", e.what());
        return 1;
    }
}

int attach_nonexistent_fails(const std::string &shm_name)
{
    try
    {
        pylabhub::hub::DataBlockMutex mutex(shm_name, nullptr, 0, false);
        (void)mutex;
        fmt::print(stderr, "Unexpected: attach succeeded for nonexistent shm\n");
        return 1;
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "attach failed: {}\n", e.what());
        return 1; // Expected failure - caller checks exit_code != 0
    }
}

} // namespace pylabhub::tests::worker::datablock_management_mutex

namespace
{
struct DataBlockMutexWorkerRegistrar
{
    DataBlockMutexWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 3)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "datablock_mutex")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                std::string shm_name(argv[2]);
                using namespace pylabhub::tests::worker::datablock_management_mutex;
                if (scenario == "acquire_and_release_creator")
                    return acquire_and_release_creator(shm_name);
                if (scenario == "acquire_and_release_creator_hold_long")
                    return acquire_and_release_creator_hold_long(shm_name);
                if (scenario == "acquire_and_release_attacher")
                    return acquire_and_release_attacher(shm_name);
                if (scenario == "acquire_and_release_attacher_delayed")
                    return acquire_and_release_attacher_delayed(shm_name);
                if (scenario == "zombie_creator_acquire_then_exit")
                    return zombie_creator_acquire_then_exit(shm_name);
                if (scenario == "zombie_attacher_recovers")
                    return zombie_attacher_recovers(shm_name);
                if (scenario == "acquire_creator_signal_then_hold")
                    return acquire_creator_signal_then_hold(shm_name);
                if (scenario == "try_lock_infinite_with_timestamps")
                    return try_lock_infinite_with_timestamps(shm_name);
                if (scenario == "attach_nonexistent_fails")
                    return attach_nonexistent_fails(shm_name);
                fmt::print(stderr, "ERROR: Unknown datablock_mutex scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static DataBlockMutexWorkerRegistrar g_datablock_mutex_registrar;
} // namespace
