#include "test_preamble.h" // New common preamble

#include "worker_jsonconfig.h"   // Keep this specific header
#include "shared_test_helpers.h" // Keep this specific helper header
#include "test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace
using namespace test_utils;

namespace worker
{
namespace jsonconfig
{
int write_id(const std::string &cfgpath, const std::string &worker_id)
{
    return run_gtest_worker(
        [&]() {
            Logger::instance().set_level(Logger::Level::L_DEBUG);
            JsonConfig cfg(cfgpath);
            bool success = false;
            const int max_retries = 200;
            std::srand(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) + std::chrono::system_clock::now().time_since_epoch().count()));
            for (int retry = 0; retry < max_retries; ++retry)
            {
                if (cfg.lock_for(100ms))
                {
                    int global_attempts = cfg.get_or<int>("total_attempts", 0);
                    cfg.set("total_attempts", global_attempts + 1);
                    cfg.set("last_worker_id", worker_id);
                    cfg.set(worker_id, true);
                    if (cfg.save()) { success = true; }
                    cfg.unlock();
                    if (success) { break; }
                }
                std::chrono::milliseconds random_delay(10 + (std::rand() % 41));
                std::this_thread::sleep_for(random_delay);
            }
            ASSERT_TRUE(success);
        },
        "jsonconfig::write_id");
}
} // namespace jsonconfig
} // namespace worker
