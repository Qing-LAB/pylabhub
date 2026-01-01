#include "platform.hpp"
// Standard Library
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <system_error>
#include <thread>

// Project-specific

#include "jsonconfig_workers.h"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "utils/JsonConfig.hpp"
#include "nlohmann/json.hpp"

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
        [&]() {
            // Each worker repeatedly attempts a with_json_write (which internally saves)
            // until it succeeds or max_retries is reached.
            JsonConfig cfg(cfgpath);
            const int max_retries = 200;
            bool success = false;

            std::srand(static_cast<unsigned int>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                std::chrono::system_clock::now().time_since_epoch().count()));

            for (int attempt = 0; attempt < max_retries; ++attempt)
            {
                std::error_code ec;
                bool ok = cfg.with_json_write([&](json &data) {
                    int attempts = data.value("total_attempts", 0);
                    data["total_attempts"] = attempts + 1;
                    data[worker_id] = true;
                    data["last_worker_id"] = worker_id;
                }, std::chrono::milliseconds{0}, &ec);

                if (ok && ec.value() == 0)
                {
                    // success
                    success = true;
                    break;
                }

                // Sleep a bit before retrying to reduce hot contention
                std::this_thread::sleep_for(std::chrono::milliseconds(10 + (std::rand() % 40)));
            }

            ASSERT_TRUE(success);
        },
        "jsonconfig::write_id",
        JsonConfig::GetLifecycleModule(), // Assuming JsonConfig will have this
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

} // namespace jsonconfig
} // namespace worker
