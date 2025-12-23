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
#include "format_tools.hpp"
#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h> // for write(), getpid()
#endif


using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using pylabhub::basic::tools::formatted_time;

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
            fs::path resource_path(counter_path_str);
            
            std::srand(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                                                 std::chrono::system_clock::now().time_since_epoch().count()));
            for (int i = 0; i < num_iterations; ++i)
            {
                if (std::rand() % 2 == 0) { std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500)); }
                
                FileLock lock(resource_path, ResourceType::File, LockMode::Blocking);
                if (!lock.valid()) { 
                    pylabhub::utils::Finalize(); 
                    return 1; 
                }

                // Per the advisory lock protocol, we must now get the path to the resource
                // that the lock is protecting.
                auto locked_path_opt = lock.get_locked_resource_path();
                if (!locked_path_opt) {
                    // This should not happen if the lock is valid.
#if defined(PLATFORM_WIN64)
                        fmt::print(stderr, "[TIME {}] worker {}: failed to acquire lock.\n", formatted_time(std::chrono::system_clock::now()), GetCurrentProcessId());
#else
                    fmt::print(stderr, "[TIME {}] worker {}: failed to acquire lock.\n", formatted_time(std::chrono::system_clock::now()), getpid());
#endif
                    pylabhub::utils::Finalize();
                    return 1;
                }
                
                if (std::rand() % 10 == 0) { std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200)); }
                
                int current_value = 0;
                {
                    std::ifstream ifs(*locked_path_opt);
                    if (ifs.is_open()) { ifs >> current_value; }
                }
                {
                    std::ofstream ofs(*locked_path_opt);
                    ofs << (current_value + 1);
                    ofs.flush();
                    ofs.close();
#if defined(PLATFORM_WIN64)
                        fmt::print(stderr, "[TIME {}] worker {}: incremented counter to {}\n", formatted_time(std::chrono::system_clock::now()), GetCurrentProcessId(), current_value + 1);
#else
                    fmt::print(stderr, "[TIME {}] worker {}: incremented counter to {}\n", formatted_time(std::chrono::system_clock::now()), getpid(), current_value + 1);
#endif
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
            // Temporarily use DEBUG level for this worker for detailed test logging
            Logger::instance().set_level(Logger::Level::L_DEBUG);
            JsonConfig cfg(cfgpath); // init with path

            bool success = false;
            const int max_retries = 200;

            std::srand(
                static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                                          std::chrono::system_clock::now().time_since_epoch().count()));

            for (int retry = 0; retry < max_retries; ++retry)
            {
                LOGGER_DEBUG("Worker {} attempting to lock, try #{}", worker_id, retry);
                // Try to acquire the lock with a timeout.
                if (cfg.lock_for(100ms))
                {
                    LOGGER_DEBUG("Worker {} ACQUIRED lock, try #{}", worker_id, retry);
                    int attempts_for_this_worker = retry + 1;

                    json before_data = cfg.as_json();

                    // Perform the read-modify-write operation.
                    int global_attempts = cfg.get_or<int>("total_attempts", 0);
                    cfg.set("total_attempts", global_attempts + attempts_for_this_worker);
                    cfg.set("last_worker_id", worker_id);
                    cfg.set(worker_id, true); // Mark that this worker has successfully written.

                    json after_data = cfg.as_json();

                    LOGGER_DEBUG("Worker {} read data: {}. writing data: {}", worker_id,
                                 before_data.dump(), after_data.dump());

                    if (cfg.save())
                    {
                        LOGGER_DEBUG("Worker {} SAVE SUCCEEDED", worker_id);
                        success = true;
                    }
                    else
                    {
                        LOGGER_ERROR("Worker {} SAVE FAILED even after acquiring lock.", worker_id);
                    }

                    cfg.unlock(); // IMPORTANT: always unlock.
                    if (success)
                    {
                        break; // Success! Exit retry loop.
                    }
                }
                else
                {
                    LOGGER_DEBUG("Worker {} FAILED to acquire lock, try #{}", worker_id, retry);
                }

                // If lock failed, wait a random interval (jitter) and retry.
                std::chrono::milliseconds random_delay(10 + (std::rand() % 41));
                std::this_thread::sleep_for(random_delay);
            }

            pylabhub::utils::Finalize();
            return success ? 0 : 1;
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
