#include "workers.h"
#include "utils/FileLock.hpp"
#include "utils/JsonConfig.hpp"
#include "utils/Logger.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/ScopeGuard.hpp"
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
#include <fcntl.h>  // for open() and O_* flags
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

        bool atomic_write_int(const fs::path &target, unsigned long PID, int value)
        {
            auto tmp = target;
            tmp += ".tmp." + std::to_string(
#if defined(PLATFORM_WIN64)
                                 static_cast<unsigned long>(GetCurrentProcessId())
#else
                                 static_cast<unsigned long>(getpid())
#endif
                             );

#if defined(PLATFORM_WIN64)
            // Use native Windows I/O for a robust atomic write-flush-rename.
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            // Ensure the handle is closed and the temp file is deleted on any exit path.
            auto guard = make_scope_guard([&]() {
                CloseHandle(h);
                // In case of failure before rename, clean up the temp file.
                std::error_code ec;
                fs::remove(tmp, ec);
            });

            std::string val_str = std::to_string(value);
            DWORD bytes_written = 0;
            if (!WriteFile(h, val_str.c_str(), static_cast<DWORD>(val_str.length()),
                           &bytes_written, NULL) ||
                bytes_written != val_str.length())
            {
                return false; // Guard will cleanup.
            }

            if (!FlushFileBuffers(h))
            {
                return false; // Guard will cleanup.
            }

#else
            // Use native POSIX I/O for a robust atomic write-fsync-rename.
            int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd == -1)
            {
                return false;
            }

            // Ensure the handle is closed and the temp file is deleted on any exit path.
            auto guard = make_scope_guard([&]() {
                ::close(fd);
                // In case of failure before rename, clean up the temp file.
                std::error_code ec;
                fs::remove(tmp, ec);
            });

            std::string val_str = std::to_string(value);
            ssize_t written = ::write(fd, val_str.c_str(), val_str.length());
            if (written != static_cast<ssize_t>(val_str.length()))
            {
                return false; // Guard will cleanup.
            }

            if (::fsync(fd) != 0)
            {
                return false; // Guard will cleanup.
            }
#endif

            // If we've reached here, the temp file is correctly written and flushed.
            // We can now attempt the atomic rename.
#if !defined(PLATFORM_WIN64)
            std::error_code ec;
            fs::rename(tmp, target, ec);
            if (ec)
            {
                fmt::print(stderr, "atomic_write_int: rename failed from {} to {}: {}\n",
                           tmp.string(), target.string(), ec.message());
                fmt::print(stderr, "worker {}: intended value was: {}\n", PID, value);
                return false; // Guard will still cleanup the temp file.
            }
#else
            if (!::MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                fmt::print(stderr, "atomic_write_int: rename failed from {} to {}: {}\n",
                           tmp.string(), target.string(), GetLastError());
                fmt::print(stderr, "worker {}: intended value was: {}\n", PID, value);
                return false; // Guard will still cleanup the temp file.
            }
#endif

            // On success, the temp file is gone, so the guard's cleanup of it is a no-op.
            // We can dismiss the guard to prevent it from running.
            guard.dismiss();

#if !defined(PLATFORM_WIN64)
            // On POSIX, it's good practice to sync the directory to ensure the rename
            // operation's metadata is durable.
            int dfd = ::open(target.parent_path().c_str(), O_DIRECTORY | O_RDONLY);
            if (dfd >= 0)
            {
                fsync(dfd);
                ::close(dfd);
            }
#endif
            return true;
        }

        // Helper to perform a platform-aware native read of an integer from a file.
        bool native_read_int(const fs::path &target, int &value)
        {
#if defined(PLATFORM_WIN64)
            HANDLE h = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                value = 0; // Treat as 0 if file doesn't exist or can't be opened
                return false;
            }

            auto guard = make_scope_guard([&]() { CloseHandle(h); });

            char buffer[128]; // Assuming integer won't exceed this length
            DWORD bytes_read = 0;
            if (!ReadFile(h, buffer, sizeof(buffer) - 1, &bytes_read, NULL)) {
                value = 0;
                return false;
            }
            buffer[bytes_read] = '\0'; // Null-terminate the string

            try {
                value = std::stoi(buffer);
            } catch (const std::exception&) {
                value = 0; // Treat as 0 on parse error
                return false;
            }
            guard.dismiss();
            return true;

#else
            int fd = ::open(target.c_str(), O_RDONLY);
            if (fd == -1) {
                value = 0; // Treat as 0 if file doesn't exist or can't be opened
                return false;
            }

            auto guard = make_scope_guard([&]() { ::close(fd); });

            char buffer[128];
            ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer) - 1);
            if (bytes_read <= 0) {
                value = 0;
                return false;
            }
            buffer[bytes_read] = '\0';

            try {
                value = std::stoi(buffer);
            } catch (const std::exception&) {
                value = 0; // Treat as 0 on parse error
                return false;
            }
            guard.dismiss();
            return true;
#endif
        }

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
            
#if defined(PLATFORM_WIN64)
            auto PID = GetCurrentProcessId();
#else
            auto PID = getpid();
#endif

            std::srand(static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) +
                                                 std::chrono::system_clock::now().time_since_epoch().count()));
            for (int i = 0; i < num_iterations; ++i)
            {
                //fmt::print(stderr, "[TIME {}] worker {}: iteration {}.\n", formatted_time(std::chrono::system_clock::now()), PID, i);
                
                if (std::rand() % 2 == 0) { 
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 500)); 
                }
                do{
                    fmt::print(stderr, "[TIME {}] worker {}: attempting to acquire lock.\n", formatted_time(std::chrono::system_clock::now()), PID);
                    FileLock filelock(resource_path, ResourceType::File, LockMode::Blocking);
                    if (!filelock.valid()) { 
                        pylabhub::utils::Finalize(); 
                        return 1; 
                    }
                    
                    // Per the advisory lock protocol, we must now get the path to the resource
                    // that the lock is protecting.
                    auto locked_path_opt = filelock.get_locked_resource_path();
                    if (!locked_path_opt) {
                        // This should not happen if the lock is valid.
                        fmt::print(stderr, "[TIME {}] worker {}: failed to get locked resource path.\n", 
                                formatted_time(std::chrono::system_clock::now()), PID);
                        pylabhub::utils::Finalize();
                        return 1;
                    }
                    
                    fmt::print(stderr, "[TIME {}] worker {}: acquired lock.\n", formatted_time(std::chrono::system_clock::now()), PID);
                    int current_value = 0;
                    try {
                        if (std::rand() % 10 == 0) { 
                            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200)); 
                        }
                        
                        // Use the new native_read_int for robust, platform-aware reading.
                        if (!native_read_int(*locked_path_opt, current_value)) {
                            // If reading fails (e.g., file doesn't exist or is empty),
                            // current_value will be 0, which is the desired behavior for an initial counter.
                            // Log a warning or debug message if needed, but don't fail the worker.
                            fmt::print(stderr, "[TIME {}] worker {}: native_read_int failed, assuming 0.\n", 
                                    formatted_time(std::chrono::system_clock::now()), PID);
                        }
                    }
                    catch (...) {
                        fmt::print(stderr, "[TIME {}] worker {}: failed to read counter (exception).\n", 
                                formatted_time(std::chrono::system_clock::now()), PID);
                        return 1;
                    }
                    try {
                        if(!atomic_write_int(*locked_path_opt, static_cast<unsigned int>(PID), current_value + 1))
                        {
                            // This is for debugging the test itself.
                            fmt::print(stderr, "worker {}: atomic_write_int failed. intended value was: {}\n", PID, current_value + 1);
                        }
                    }
                    catch (...) {
                        fmt::print(stderr, "[TIME {}] worker {}: failed to write counter.\n", 
                                formatted_time(std::chrono::system_clock::now()), PID);
                        return 1;
                    }
                } while(0); // scope of lock ends
                fmt::print(stderr, "[TIME {}] worker {}: outside FileLock scope, lock should be released.\n", 
                    formatted_time(std::chrono::system_clock::now()), PID);
            }
            fmt::print(stderr, "[TIME {}] worker {}: finished.\n", formatted_time(std::chrono::system_clock::now()), PID);
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
