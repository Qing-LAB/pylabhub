// tests/test_pylabhub_utils/jsonconfig_workers.cpp
/**
 * @file jsonconfig_workers.cpp
 * @brief Implements the worker function for JsonConfig multi-process tests.
 */
#include "platform.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <system_error>
#include <thread>

#include "jsonconfig_workers.h"
#include "nlohmann/json.hpp"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "utils/JsonConfig.hpp"

using nlohmann::json;
using namespace pylabhub::tests::helper;
using namespace pylabhub::utils;

namespace pylabhub::tests::worker
{
namespace jsonconfig
{

int write_id(const std::string &cfgpath, const std::string &worker_id)
{
    return run_gtest_worker(
        [&]()
        {
            // Each worker repeatedly attempts to acquire a write lock and modify the file.
            // This simulates high-contention scenarios for the JsonConfig class.
            JsonConfig cfg(cfgpath);
            const int max_retries = 200;
            bool success = false;

            // Seed random number generator for sleep intervals to vary contention.
            std::srand(static_cast<unsigned int>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                std::chrono::system_clock::now().time_since_epoch().count()));

            for (int attempt = 0; attempt < max_retries; ++attempt)
            {
                std::error_code ec;
                // Attempt a non-blocking write. The lambda is only executed if the
                // file lock is acquired successfully.
                cfg.transaction().write(
                    [&](json &data)
                    {
                        int attempts = data.value("total_attempts", 0);
                        data["total_attempts"] = attempts + 1;
                        data[worker_id] = true;
                        data["last_worker_id"] = worker_id;
                    },
                    &ec);

                if (ec.value() == 0)
                {
                    success = true;
                    break;
                }

                // If the write failed (e.g., lock not acquired), sleep for a random
                // duration before retrying to reduce hot-looping.
                std::this_thread::sleep_for(std::chrono::milliseconds(10 + (std::rand() % 40)));
            }

            ASSERT_TRUE(success);
        },
        "jsonconfig::write_id", JsonConfig::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int uninitialized_behavior()
{
    // This worker function is designed to test the fatal error that occurs when
    // a JsonConfig object is constructed before its lifecycle module is initialized.
    // There is no LifecycleGuard here, so the JsonConfig module is not started.
    // The following line is expected to call PLH_PANIC and abort the process.
    JsonConfig config;

    // The lines below should be unreachable. If the process exits with 0,
    // the test will fail.
    return 0;
}

} // namespace jsonconfig
} // namespace pylabhub::tests::worker
