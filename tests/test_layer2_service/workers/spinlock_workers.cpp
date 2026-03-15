/**
 * @file spinlock_workers.cpp
 * @brief Worker functions for SharedSpinLock multi-process tests.
 */
#include "spinlock_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_platform.hpp"
#include "plh_service.hpp"
#include "utils/shared_memory_spinlock.hpp"
#include "gtest/gtest.h"
#include "fmt/core.h"
#include <chrono>
#include <thread>

using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker
{
namespace spinlock
{

int multiprocess_acquire_release(const std::string &shm_name)
{
    return run_gtest_worker(
        [shm_name]()
        {
            pylabhub::platform::ShmHandle h = pylabhub::platform::shm_attach(shm_name.c_str());
            ASSERT_NE(h.base, nullptr) << "Worker: shm_attach failed for " << shm_name;
            ASSERT_GE(h.size, sizeof(pylabhub::hub::SharedSpinLockState));
            auto *state = static_cast<pylabhub::hub::SharedSpinLockState *>(h.base);
            pylabhub::hub::SharedSpinLock lock(state, "worker_acquire_release");
            ASSERT_TRUE(lock.try_lock_for(2000)) << "Worker: try_lock_for failed";
            std::this_thread::sleep_for(20ms);
            lock.unlock();
            pylabhub::platform::shm_close(&h);
        },
        "spinlock::multiprocess_acquire_release", pylabhub::utils::Logger::GetLifecycleModule());
}

int zombie_hold_lock(const std::string &shm_name)
{
    return run_gtest_worker(
        [shm_name]()
        {
            pylabhub::platform::ShmHandle h = pylabhub::platform::shm_attach(shm_name.c_str());
            ASSERT_NE(h.base, nullptr) << "Worker: shm_attach failed for " << shm_name;
            auto *state = static_cast<pylabhub::hub::SharedSpinLockState *>(h.base);
            pylabhub::hub::SharedSpinLock lock(state, "worker_zombie");
            ASSERT_TRUE(lock.try_lock_for(2000)) << "Worker: try_lock_for failed";
            // Exit without unlocking: lock remains "held" by this PID (which will be dead).
            pylabhub::platform::shm_close(&h);
            // Do not unlock - process exits here; parent will reclaim.
        },
        "spinlock::zombie_hold_lock", pylabhub::utils::Logger::GetLifecycleModule());
}

} // namespace spinlock
} // namespace pylabhub::tests::worker

namespace
{
struct SpinlockWorkerRegistrar
{
    SpinlockWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "spinlock")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::spinlock;
                if (scenario == "multiprocess_acquire_release" && argc > 2)
                    return multiprocess_acquire_release(argv[2]);
                if (scenario == "zombie_hold_lock" && argc > 2)
                    return zombie_hold_lock(argv[2]);
                fmt::print(stderr, "ERROR: Unknown spinlock scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static SpinlockWorkerRegistrar g_spinlock_registrar;
} // namespace
