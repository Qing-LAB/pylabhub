#include "workers.h"
#include "utils/FileLock.hpp"
#include "utils/JsonConfig.hpp"
#include "utils/Logger.hpp"
#include "utils/Lifecycle.hpp"
#include <filesystem>
#include <chrono>
#include <string>
#include <thread>
#include <fstream>
#include <fmt/core.h>

#include "platform.hpp"
#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h> // for write(), getpid()
#endif


using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace worker
{
    // NOTE: Each worker function below is executed in a new child process via
    // the test entrypoint. Therefore, each worker must manage its own lifecycle
    // for the pylabhub::utils library. The Initialize() and Finalize() calls
    // are not redundant; they are essential for the stability of each child process.
    // The lifecycle functions are idempotent, making them safe to call here.

    // --- FileLock Workers ---
    namespace filelock
    {
        int nonblocking_acquire(const std::string &resource_path_str)
        {
            pylabhub::utils::Initialize();
            Logger::instance().set_level(Logger::Level::L_ERROR);
            fs::path resource_path(resource_path_str);

            FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
            if (!lock.valid())
            {
                if (lock.error_code())
                {
        #if defined(PLATFORM_WIN64)
                    fmt::print(stderr, "worker: failed to acquire lock: {} - {}\n",
                               lock.error_code().value(), lock.error_code().message());
        #else
                    std::string err_msg = fmt::format("worker: failed to acquire lock: {} - {}\n",
                                                      lock.error_code().value(), lock.error_code().message());
                    ::write(STDERR_FILENO, err_msg.c_str(), err_msg.length());
        #endif
                }
                pylabhub::utils::Finalize();
                return 1;
            }
            std::this_thread::sleep_for(3s);
            pylabhub::utils::Finalize();
            return 0;
        }

        int contention_increment(const std::string &counter_path_str, int num_iterations)
        {
            pylabhub::utils::Initialize();
            Logger::instance().set_level(Logger::Level::L_ERROR);
            fs::path counter_path(counter_path_str);
            std::srand(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                                                 std::chrono::system_clock::now().time_since_epoch().count()));
            for (int i = 0; i < num_iterations; ++i)
            {
                if (std::rand() % 2 == 0) { std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500)); }
                FileLock lock(counter_path, ResourceType::File, LockMode::Blocking);
                if (!lock.valid()) { pylabhub::utils::Finalize(); return 1; }
                if (std::rand() % 10 == 0) { std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200)); }
                int current_value = 0;
                {
                    std::ifstream ifs(counter_path);
                    if (ifs.is_open()) { ifs >> current_value; }
                }
                {
                    std::ofstream ofs(counter_path);
                    ofs << (current_value + 1);
                }
            }
            pylabhub::utils::Finalize();
            return 0;
        }

        int parent_child_block(const std::string &resource_path_str)
        {
            pylabhub::utils::Initialize();
            Logger::instance().set_level(Logger::Level::L_ERROR);
            fs::path resource_path(resource_path_str);
            auto start = std::chrono::steady_clock::now();
            FileLock lock(resource_path, ResourceType::File, LockMode::Blocking);
            auto end = std::chrono::steady_clock::now();
            if (!lock.valid()) { pylabhub::utils::Finalize(); return 1; }
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            if (dur.count() < 100) { pylabhub::utils::Finalize(); return 2; }
            pylabhub::utils::Finalize();
            return 0;
        }
    } // namespace filelock

    // --- JsonConfig Workers ---
    namespace jsonconfig
    {
        int write_id(const std::string &cfgpath, const std::string &worker_id)
        {
            pylabhub::utils::Initialize();
            Logger::instance().set_level(Logger::Level::L_ERROR);
            JsonConfig cfg;
            
            bool success = false;
            int max_retries = 100;
            
            // Use a random seed for each worker process to stagger retries.
            std::srand(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                                                 std::chrono::system_clock::now().time_since_epoch().count()));

            for (int retry = 0; retry < max_retries; ++retry) {
                // Try to initialize and acquire lock
                if (cfg.init(cfgpath, false)) {
                    // If init succeeded, try to write. Also record the retry count.
                    if (cfg.with_json_write([&](json &j) { 
                        j["worker"] = worker_id;
                        j["retries"] = retry; // Record retry count
                    })) {
                        success = true;
                        break; // Success! Exit retry loop.
                    }
                }
                // If init or write failed, wait a random interval (jitter) and retry.
                std::chrono::milliseconds random_delay(10 + (std::rand() % 41)); // 10ms to 50ms
                std::this_thread::sleep_for(random_delay);
            }

            pylabhub::utils::Finalize();
            return success ? 0 : 1; // Return 0 for success, 1 for any failure
        }
    } // namespace jsonconfig

    // --- Logger Workers ---
    namespace logger
    {
        void stress_log(const std::string& log_path, int msg_count)
        {
            pylabhub::utils::Initialize();
            Logger &L = Logger::instance();
            L.set_logfile(log_path, true);
            L.set_level(Logger::Level::L_TRACE);
        #if defined(PLATFORM_WIN64)
            std::srand(static_cast<unsigned int>(GetCurrentProcessId() + std::chrono::system_clock::now().time_since_epoch().count()));
        #else
            std::srand(static_cast<unsigned int>(getpid() + std::chrono::system_clock::now().time_since_epoch().count()));
        #endif
            for (int i = 0; i < msg_count; ++i)
            {
                if (std::rand() % 10 == 0) { std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100)); }
        #if defined(PLATFORM_WIN64)
                LOGGER_INFO("child-msg pid={} idx={}", GetCurrentProcessId(), i);
        #else
                LOGGER_INFO("child-msg pid={} idx={}", getpid(), i);
        #endif
            }
            L.flush();
            pylabhub::utils::Finalize();
        }
    } // namespace logger
} // namespace worker
