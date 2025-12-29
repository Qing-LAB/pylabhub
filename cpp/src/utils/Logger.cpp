/*******************************************************************************
 * @file Logger.cpp
 * @brief Implementation of the high-performance, asynchronous logger.
 *
 * @see include/utils/Logger.hpp
 *
 * **Implementation Details**
 *
 * This file contains the private implementation of the `Logger` class, using the
 * Pimpl idiom to hide all internal workings and provide a stable ABI.
 *
 * 1.  **Command Processing**:
 *     - The `Command` type is a `std::variant` that can hold different types of
 *       requests: a `LogMessage` to be written, a `SetSinkCommand` to change
 *       the output destination, `FlushCommand` for synchronous flushing, etc.
 *     - The public API functions (e.g., `log_fmt`, `set_logfile`) act as
 *       producers, creating command objects and pushing them onto a queue.
 *
 * 2.  **The Worker Thread (`worker_loop`)**:
 *     - This is the heart of the logger. It runs in a continuous loop, sleeping
 *       on a condition variable until the queue is not empty or shutdown is
 *       requested.
 *     - To improve performance and reduce lock contention, it "batch-processes"
 *       commands. It locks the queue, swaps the entire queue content into a
 *       local vector, and then unlocks. This minimizes the time the queue is
.
 *       locked, allowing producer threads to continue their work with minimal
 *       delay.
 *     - It then iterates through the local vector of commands, processing each
 *       one via `std::visit`.
 *
 * 3.  **Sink Management**:
 *     - Sinks are polymorphic `Sink` objects managed by `std::unique_ptr`.
 *     - When a `SetSinkCommand` is processed, the worker thread first flushes
 *       the old sink, then replaces it with the new one. Log messages are
 *       written to the sinks to announce the switch for diagnostic purposes.
 *     - Sink creation happens on the calling thread. If it fails (e.g., cannot
 *       open a file), an error command is enqueued, which can trigger the
 *       error callback.
 *
 * 4.  **Error Callback Handling (`CallbackDispatcher`)**:
 *     - A potential for deadlock exists if a user's error callback directly
 *       or indirectly calls back into the logger. For example:
 *       `ErrorCallback -> LogSomething -> Enqueue -> Deadlock` if the main
 *       queue is locked.
 *     - To prevent this, the `CallbackDispatcher` runs its own dedicated worker
 *       thread with its own queue. When the main logger's worker needs to
 *       invoke the user's callback, it simply "posts" the callback function to
 *       the dispatcher's queue and continues its work. The dispatcher's thread
 *       then executes the user callback safely, without any risk of deadlocking
 *       the main logger.
 *
 * 5.  **Lifecycle Integration (`LoggerLifecycleRegistrar`)**:
 *     - A static global object, `g_logger_registrar`, is created when the
 *       library is loaded.
 *     - Its constructor creates and registers a "Logger" module with the
 *       `LifecycleManager`. This module definition provides pointers to the
 *       `do_logger_startup` and `do_logger_shutdown` functions.
 *     - This ensures that `Logger::instance()` is called during application
 *       initialization and `Logger::instance().shutdown()` is called during
 *       finalization, automating the logger's lifecycle management.
 ******************************************************************************/

#include "utils/Logger.hpp"
#include "utils/Lifecycle.hpp"
#include "format_tools.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#ifdef PLATFORM_WIN64
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <unistd.h>
#endif

namespace pylabhub::utils
{

/**
 * @class CallbackDispatcher
 * @brief A helper to safely execute user-provided callbacks on a separate thread.
 *
 * This class runs its own internal worker thread and command queue. Its sole
 * purpose is to decouple the execution of a user's error callback from the
 * Logger's main worker thread, thereby preventing re-entrant deadlock scenarios.
 */
class CallbackDispatcher
{
  public:
    CallbackDispatcher() : shutdown_requested_(false)
    {
        worker_ = std::thread([this] { this->run(); });
    }

    ~CallbackDispatcher() { shutdown(); }

    void post(std::function<void()> fn)
    {
        if (shutdown_requested_.load(std::memory_order_relaxed))
            return;
        {
            std::lock_guard<std::mutex> lg(mutex_);
            queue_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    void shutdown()
    {
        if (shutdown_requested_.exchange(true))
        {
            return; // Already shutting down or shut down
        }
        cv_.notify_one();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

  private:
    void run()
    {
        for (;;)
        {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> ul(mutex_);
                cv_.wait(ul, [this] { return shutdown_requested_.load() || !queue_.empty(); });
                if (shutdown_requested_.load() && queue_.empty())
                {
                    return;
                }
                fn = std::move(queue_.front());
                queue_.pop_front();
            }
            try
            {
                fn();
            }
            catch (...)
            {
                // Exceptions in user callbacks are caught and swallowed to
                // prevent them from terminating the dispatcher thread.
            }
        }
    }

    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> shutdown_requested_;
};

// ============================================================================
// Internal Command and Sink Definitions
// ============================================================================

/** @struct LogMessage @brief Represents a single, formatted log entry. */
struct LogMessage
{
    Logger::Level level;
    std::chrono::system_clock::time_point timestamp;
    uint64_t thread_id;
    fmt::memory_buffer body;
};

/**
 * @class Sink
 * @brief The abstract base class for all log destinations.
 *
 * A Sink is responsible for the actual I/O of writing a formatted log message.
 * All methods of a Sink are guaranteed to be called only from the Logger's
 * single worker thread, so they do not need to be internally thread-safe.
 */
class Sink
{
  public:
    virtual ~Sink() = default;
    /** @brief Writes a single log message to the destination. */
    virtual void write(const LogMessage &msg) = 0;
    /** @brief Flushes any buffered output to the destination. */
    virtual void flush() = 0;
    /** @brief Returns a string description of the sink for diagnostics. */
    virtual std::string description() const = 0;
};

// --- Helper Functions ---

// Converts a log level enum to its string representation.
static const char *level_to_string(Logger::Level lvl)
{
    switch (lvl)
    {
    case Logger::Level::L_TRACE: return "TRACE";
    case Logger::Level::L_DEBUG: return "DEBUG";
    case Logger::Level::L_INFO: return "INFO";
    case Logger::Level::L_WARNING: return "WARN";
    case Logger::Level::L_ERROR: return "ERROR";
    case Logger::Level::L_SYSTEM: return "SYSTEM";
    default: return "UNK";
    }
}

// Gets a platform-native thread ID for logging.
static uint64_t get_native_thread_id() noexcept
{
#if defined(PLATFORM_WIN64)
    return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(nullptr, &tid);
    return tid;
#elif defined(__linux__)
    return static_cast<uint64t>(syscall(SYS_gettid));
#else
    // Fallback for other POSIX or unknown systems.
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

// Formats a LogMessage into a final, printable string.
static std::string format_message(const LogMessage &msg)
{
    std::string time_str = pylabhub::format_tools::formatted_time(msg.timestamp);
    return fmt::format("[{}] [{:<6}] [{:5}] {}\n", time_str, level_to_string(msg.level),
                       msg.thread_id, std::string_view(msg.body.data(), msg.body.size()));
}

// ============================================================================
// Concrete Sink Implementations
// ============================================================================

/** @brief A sink that writes log messages to the standard error console. */
class ConsoleSink : public Sink
{
  public:
    void write(const LogMessage &msg) override { fmt::print(stderr, "{}", format_message(msg)); }
    void flush() override { fflush(stderr); }
    std::string description() const override { return "Console"; }
};

/** @brief A sink that writes log messages to a file. */
class FileSink : public Sink
{
  public:
    FileSink(const std::string &path, bool use_flock) : path_(path), use_flock_(use_flock)
    {
#ifdef PLATFORM_WIN64
        (void)use_flock; // Not supported on Windows.
        // Use Win32 API for correct UTF-8 path handling.
        int needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (needed == 0)
            throw std::runtime_error("Failed to convert path to wide string");
        std::wstring wpath(needed, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], needed);

        handle_ = CreateFileW(wpath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Failed to open log file: " + path);
        }
#else
        fd_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd_ == -1)
        {
            throw std::runtime_error("Failed to open log file: " + path);
        }
#endif
    }

    ~FileSink() override
    {
#ifdef PLATFORM_WIN64
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
#else
        if (fd_ != -1)
            ::close(fd_);
#endif
    }

    void write(const LogMessage &msg) override
    {
        auto formatted_message = format_message(msg);
#ifdef PLATFORM_WIN64
        if (handle_ == INVALID_HANDLE_VALUE)
            return;
        DWORD bytes_written;
        WriteFile(handle_, formatted_message.c_str(),
                  static_cast<DWORD>(formatted_message.length()), &bytes_written, nullptr);
#else
        if (fd_ == -1)
            return;
        if (use_flock_)
            ::flock(fd_, LOCK_EX);
        ::write(fd_, formatted_message.c_str(), formatted_message.length());
        if (use_flock_)
            ::flock(fd_, LOCK_UN);
#endif
    }

    void flush() override
    {
#ifdef PLATFORM_WIN64
        if (handle_ != INVALID_HANDLE_VALUE)
            FlushFileBuffers(handle_);
#else
        if (fd_ != -1)
            ::fsync(fd_);
#endif
    }

    std::string description() const override { return "File: " + path_; }

  private:
    std::string path_;
    bool use_flock_;
#ifdef PLATFORM_WIN64
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

#if !defined(PLATFORM_WIN64)
/** @brief A sink that writes to the POSIX syslog service. */
class SyslogSink : public Sink
{
  public:
    SyslogSink(const char *ident, int option, int facility) { openlog(ident, option, facility); }

    ~SyslogSink() override { closelog(); }

    void write(const LogMessage &msg) override
    {
        syslog(level_to_syslog_priority(msg.level), "%.*s", (int)msg.body.size(), msg.body.data());
    }

    void flush() override {} // Not buffered in the application.

    std::string description() const override { return "Syslog"; }

  private:
    static int level_to_syslog_priority(Logger::Level level)
    {
        switch (level)
        {
        case Logger::Level::L_TRACE: return LOG_DEBUG;
        case Logger::Level::L_DEBUG: return LOG_DEBUG;
        case Logger::Level::L_INFO: return LOG_INFO;
        case Logger::Level::L_WARNING: return LOG_WARNING;
        case Logger::Level::L_ERROR: return LOG_ERR;
        case Logger::Level::L_SYSTEM: return LOG_CRIT;
        default: return LOG_INFO;
        }
    }
};
#endif // !defined(PLATFORM_WIN64)

#ifdef PLATFORM_WIN64
/** @brief A sink that writes to the Windows Event Log. */
class EventLogSink : public Sink
{
  public:
    EventLogSink(const wchar_t *source_name)
    {
        handle_ = RegisterEventSourceW(nullptr, source_name);
        if (!handle_)
        {
            throw std::runtime_error("Failed to register event source");
        }
    }

    ~EventLogSink() override
    {
        if (handle_)
            DeregisterEventSource(handle_);
    }

    void write(const LogMessage &msg) override
    {
        if (!handle_)
            return;

        // Convert UTF-8 message body from memory_buffer to a wide string for ReportEventW.
        int needed = MultiByteToWideChar(CP_UTF8, 0, msg.body.data(),
                                         static_cast<int>(msg.body.size()), nullptr, 0);
        if (needed <= 0)
        {
            const wchar_t *empty_str = L"";
            ReportEventW(handle_, level_to_eventlog_type(msg.level), 0, 0, nullptr, 1, 0,
                         &empty_str, nullptr);
            return;
        }
        std::wstring wbody(needed, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg.body.data(), static_cast<int>(msg.body.size()),
                            &wbody[0], needed);

        const wchar_t *strings[1] = {wbody.c_str()};
        ReportEventW(handle_, level_to_eventlog_type(msg.level), 0, 0, nullptr, 1, 0, strings,
                     nullptr);
    }

    void flush() override {} // Not applicable.

    std::string description() const override { return "Windows Event Log"; }

  private:
    HANDLE handle_ = nullptr;

    static WORD level_to_eventlog_type(Logger::Level level)
    {
        switch (level)
        {
        case Logger::Level::L_TRACE:
        case Logger::Level::L_DEBUG:
        case Logger::Level::L_INFO: return EVENTLOG_INFORMATION_TYPE;
        case Logger::Level::L_WARNING: return EVENTLOG_WARNING_TYPE;
        case Logger::Level::L_ERROR:
        case Logger::Level::L_SYSTEM: return EVENTLOG_ERROR_TYPE;
        default: return EVENTLOG_INFORMATION_TYPE;
        }
    }
};
#endif // PLATFORM_WIN64

// --- Command Definitions ---
struct SetSinkCommand { std::unique_ptr<Sink> new_sink; };
struct SinkCreationErrorCommand { std::string error_message; };
struct FlushCommand { std::shared_ptr<std::promise<void>> promise; };
struct SetErrorCallbackCommand { std::function<void(const std::string &)> callback; };

// The variant that holds any possible command for the worker queue.
using Command = std::variant<LogMessage, SetSinkCommand, SinkCreationErrorCommand, FlushCommand,
                             SetErrorCallbackCommand>;

// ============================================================================
// Logger Pimpl and Implementation
// ============================================================================

struct Impl
{
    Impl();
    ~Impl();

    void worker_loop();
    void enqueue_command(Command &&cmd);
    void shutdown();

    // Worker thread and command queue.
    std::thread worker_thread_;
    std::vector<Command> queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_requested_{false};

    // State exclusively owned and accessed by the worker thread.
    std::atomic<Logger::Level> level_{Logger::Level::L_INFO};
    std::unique_ptr<Sink> sink_;
    std::function<void(const std::string &)> error_callback_;
    CallbackDispatcher callback_dispatcher_;
    std::atomic<bool> shutdown_completed_{false};
};

Impl::Impl() : sink_(std::make_unique<ConsoleSink>())
{
    worker_thread_ = std::thread(&Impl::worker_loop, this);
}

Impl::~Impl()
{
    if (!shutdown_requested_.load())
    {
        // This destructor is called when the static Logger instance is destroyed.
        // If shutdown() was not called explicitly (e.g., via the LifecycleManager),
        // we issue a warning because logs might be lost.
        //
        // **IMPORTANT**: We DO NOT call shutdown() here as a fallback. On Windows,
        // this destructor may run while the OS loader lock is held. Attempting to
        // join a thread (which shutdown() does) in this state can cause a deadlock.
        // The explicit `pylabhub::lifecycle::FinalizeApp()` call is the only safe way.
        fmt::print(stderr,
                   "[pylabhub::Logger WARNING]: Logger was not shut down explicitly. "
                   "Call pylabhub::lifecycle::FinalizeApp() before main() returns to guarantee "
                   "all logs are flushed.\n");
    }
}

void Impl::enqueue_command(Command &&cmd)
{
    // Double-checked locking pattern. The first check is lock-free for performance.
    if (shutdown_requested_.load(std::memory_order_relaxed))
    {
        // If shutdown has started, provide a fallback for critical messages
        // by printing them directly to stderr. This prevents silent loss.
        if (std::holds_alternative<LogMessage>(cmd))
        {
            const auto &msg = std::get<LogMessage>(cmd);
            fmt::print(stderr, "[pylabhub::Logger-fallback] Log after shutdown: {}",
                       format_message(msg));
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // Final check inside the lock to handle the race condition where shutdown
        // is requested between the first check and acquiring the lock.
        if (shutdown_requested_.load(std::memory_order_acquire))
        {
            if (std::holds_alternative<LogMessage>(cmd))
            {
                const auto &msg = std::get<LogMessage>(cmd);
                fmt::print(stderr, "[pylabhub::Logger-fallback] Log after shutdown: {}",
                           format_message(msg));
            }
            return;
        }
        queue_.emplace_back(std::move(cmd));
    }
    cv_.notify_one();
}

void Impl::worker_loop()
{
    std::vector<Command> local_queue; // Batch processing queue.

    while (true)
    {
        bool do_final_flush_and_break = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || shutdown_requested_.load(); });

            if (shutdown_requested_.load() && queue_.empty())
            {
                do_final_flush_and_break = true;
            }
            // Swap the main queue with our empty local one. This is a fast operation
            // that minimizes the time the mutex is held.
            local_queue.swap(queue_);
        }

        if (do_final_flush_and_break)
        {
            if (sink_) sink_->flush();
            break; // Exit the worker loop.
        }

        for (auto &cmd : local_queue)
        {
            try
            {
                std::visit(
                    [this](auto &&arg) {
                        using T = std::decay_t<decltype(arg)>;
                        if constexpr (std::is_same_v<T, LogMessage>)
                        {
                            if (sink_ && arg.level >= level_.load(std::memory_order_relaxed))
                                sink_->write(arg);
                        }
                        else if constexpr (std::is_same_v<T, SetSinkCommand>)
                        {
                            std::string old_desc = sink_ ? sink_->description() : "null";
                            std::string new_desc = arg.new_sink ? arg.new_sink->description() : "null";

                            if (sink_)
                            {
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(),
                                              get_native_thread_id(),
                                              fmt::memory_buffer_from("Switching log sink to: " + new_desc)});
                                sink_->flush();
                            }
                            sink_ = std::move(arg.new_sink);
                            if (sink_)
                            {
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(),
                                              get_native_thread_id(),
                                              fmt::memory_buffer_from("Log sink switched from: " + old_desc)});
                            }
                        }
                        else if constexpr (std::is_same_v<T, SinkCreationErrorCommand>)
                        {
                            if (error_callback_)
                            {
                                // Post the user callback to the dispatcher to avoid deadlock.
                                auto cb = error_callback_;
                                callback_dispatcher_.post([cb, msg = arg.error_message]() { cb(msg); });
                            }
                        }
                        else if constexpr (std::is_same_v<T, FlushCommand>)
                        {
                            if (sink_) sink_->flush();
                            arg.promise->set_value(); // Unblock the waiting thread.
                        }
                        else if constexpr (std::is_same_v<T, SetErrorCallbackCommand>)
                        {
                            error_callback_ = std::move(arg.callback);
                        }
                    },
                    std::move(cmd));
            }
            catch (const std::exception &e)
            {
                // Catch exceptions from sink operations and report via callback.
                if (error_callback_)
                {
                    auto cb = error_callback_;
                    auto msg = fmt::format("Logger worker error: {}", e.what());
                    callback_dispatcher_.post([cb, msg]() { cb(msg); });
                }
            }
        }
        local_queue.clear();
    }
}

void Impl::shutdown()
{
    // Atomically set shutdown flag and ensure it's only done once.
    if (shutdown_completed_.load() || shutdown_requested_.exchange(true))
    {
        return;
    }

    // Wake up worker thread to process the shutdown.
    cv_.notify_one();
    if (worker_thread_.joinable())
    {
        worker_thread_.join();
    }

    // Now that no more callbacks can be generated, shut down the dispatcher.
    callback_dispatcher_.shutdown();

    shutdown_completed_.store(true);
}

// --- Logger Public API Implementation ---

namespace
{
// A mutex-guarded unique_ptr is used for the singleton to allow for explicit
// destruction and re-creation in tests. This is not possible with a simple
// function-local static (`static Logger instance;`).
std::unique_ptr<Logger> g_instance;
std::mutex g_instance_mutex;
} // namespace

Logger::Logger() : pImpl(std::make_unique<Impl>()) {}

// The destructor is defaulted. The real cleanup logic is in Impl::~Impl().
Logger::~Logger() = default;

Logger &Logger::instance()
{
    // Use a fast, lock-free check first.
    if (!g_instance)
    {
        std::lock_guard<std::mutex> lock(g_instance_mutex);
        if (!g_instance)
        {
            // Logger's constructor is private, so we must use a friend or a
            // helper struct to call it.
            struct LoggerMaker : public Logger { LoggerMaker() : Logger() {} };
            g_instance = std::make_unique<LoggerMaker>();
        }
    }
    return *g_instance;
}

void Logger::set_console()
{
    try
    {
        pImpl->enqueue_command(SetSinkCommand{std::make_unique<ConsoleSink>()});
    }
    catch (const std::exception &e)
    {
        pImpl->enqueue_command(SinkCreationErrorCommand{fmt::format("Failed to create ConsoleSink: {}", e.what())});
    }
}

void Logger::set_logfile(const std::string &utf8_path, bool use_flock)
{
    try
    {
        pImpl->enqueue_command(SetSinkCommand{std::make_unique<FileSink>(utf8_path, use_flock)});
    }
    catch (const std::exception &e)
    {
        pImpl->enqueue_command(SinkCreationErrorCommand{fmt::format("Failed to create FileSink: {}", e.what())});
    }
}

void Logger::set_syslog(const char *ident, int option, int facility)
{
#if !defined(PLATFORM_WIN64)
    try
    {
        pImpl->enqueue_command(SetSinkCommand{std::make_unique<SyslogSink>(ident ? ident : "", option, facility)});
    }
    catch (const std::exception &e)
    {
        pImpl->enqueue_command(SinkCreationErrorCommand{fmt::format("Failed to create SyslogSink: {}", e.what())});
    }
#else
    (void)ident; (void)option; (void)facility; // Suppress unused parameter warnings.
#endif
}

void Logger::set_eventlog(const wchar_t *source_name)
{
#ifdef PLATFORM_WIN64
    try
    {
        pImpl->enqueue_command(SetSinkCommand{std::make_unique<EventLogSink>(source_name)});
    }
    catch (const std::exception &e)
    {
        pImpl->enqueue_command(SinkCreationErrorCommand{fmt::format("Failed to create EventLogSink: {}", e.what())});
    }
#else
    (void)source_name; // Suppress unused parameter warning.
#endif
}

void Logger::shutdown()
{
    if (pImpl) pImpl->shutdown();
}

void Logger::flush()
{
    if (!pImpl || pImpl->shutdown_requested_.load()) return;

    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    pImpl->enqueue_command(FlushCommand{promise});
    future.wait();
}

void Logger::set_level(Level lvl)
{
    if (pImpl) pImpl->level_.store(lvl, std::memory_order_relaxed);
}

Logger::Level Logger::level() const
{
    return pImpl ? pImpl->level_.load(std::memory_order_relaxed) : Level::L_INFO;
}

void Logger::set_write_error_callback(std::function<void(const std::string &)> cb)
{
    if (pImpl) pImpl->enqueue_command(SetErrorCallbackCommand{std::move(cb)});
}

bool Logger::should_log(Level lvl) const noexcept
{
    return pImpl && static_cast<int>(lvl) >= static_cast<int>(pImpl->level_.load(std::memory_order_relaxed));
}

void Logger::enqueue_log(Level lvl, fmt::memory_buffer &&body) noexcept
{
    if (pImpl)
    {
        pImpl->enqueue_command(LogMessage{lvl, std::chrono::system_clock::now(), get_native_thread_id(), std::move(body)});
    }
}

void Logger::enqueue_log(Level lvl, std::string &&body_str) noexcept
{
    if (pImpl)
    {
        pImpl->enqueue_command(LogMessage{lvl, std::chrono::system_clock::now(), get_native_thread_id(), fmt::memory_buffer_from(std::move(body_str))});
    }
}

namespace
{
// C-style callbacks for the ABI-safe lifecycle API.
void do_logger_startup() { Logger::instance(); }
void do_logger_shutdown() { Logger::instance().shutdown(); }

/**
 * @struct LoggerLifecycleRegistrar
 * @brief A static object that automatically registers the Logger with the LifecycleManager.
 *
 * When this static object is constructed (at program startup), it defines and
 * registers the "pylabhub::utils::Logger" module. This ensures the logger is
* started up and shut down correctly by the main application lifecycle.
 */
struct LoggerLifecycleRegistrar
{
    LoggerLifecycleRegistrar()
    {
        ModuleDef module("pylabhub::utils::Logger");
        module.set_startup(&do_logger_startup);
        module.set_shutdown(&do_logger_shutdown, 5000 /*ms timeout*/);
        // The logger has no dependencies, so it will be one of the first modules to start.
        LifecycleManager::instance().register_module(std::move(module));
    }
};

// The global instance that triggers the registration.
static LoggerLifecycleRegistrar g_logger_registrar;

} // namespace

} // namespace pylabhub::utils
