/*******************************************************************************
 * @file Logger.cpp
 * @brief Implementation of the high-performance, asynchronous logger.
 ******************************************************************************/
#include "platform.hpp"
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

using namespace pylabhub::platform;
using namespace pylabhub::format_tools;


namespace pylabhub::utils
{

// Module-level flag to indicate if the logger has been initialized.
static std::atomic<bool> g_logger_initialized{false};

// Centralized check function
static void check_initialized_and_abort(const char* function_name) {
    if (!g_logger_initialized.load(std::memory_order_acquire)) {
        fmt::print(stderr,
                   "FATAL: Logger method '{}' was called before the Logger module was initialized via LifecycleManager. Aborting.\n",
                   function_name);
        std::abort();
    }
}

/**
 * @class CallbackDispatcher
 * @brief A helper to safely execute user-provided callbacks on a separate thread.
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
            return;
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
                // Exceptions in user callbacks are caught and swallowed.
            }
        }
    }

    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> shutdown_requested_;
};

// Internal Command and Sink Definitions
struct LogMessage
{
    Logger::Level level;
    std::chrono::system_clock::time_point timestamp;
    uint64_t thread_id;
    fmt::memory_buffer body;
};

class Sink
{
  public:
    virtual ~Sink() = default;
    virtual void write(const LogMessage &msg) = 0;
    virtual void flush() = 0;
    virtual std::string description() const = 0;
};

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

static std::string format_message(const LogMessage &msg)
{
    std::string time_str = formatted_time(msg.timestamp);
    return fmt::format("[{}] [{:<6}] [{:5}] {}\n", time_str, level_to_string(msg.level),
                       msg.thread_id, std::string_view(msg.body.data(), msg.body.size()));
}

// Concrete Sink Implementations
class ConsoleSink : public Sink
{
  public:
    void write(const LogMessage &msg) override { fmt::print(stderr, "{}", format_message(msg)); }
    void flush() override { fflush(stderr); }
    std::string description() const override { return "Console"; }
};

class FileSink : public Sink
{
  public:
    FileSink(const std::string &path, bool use_flock) : path_(path), use_flock_(use_flock)
    {
#ifdef PLATFORM_WIN64
        (void)use_flock;
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
class SyslogSink : public Sink
{
  public:
    SyslogSink(const char *ident, int option, int facility) { openlog(ident, option, facility); }
    ~SyslogSink() override { closelog(); }
    void write(const LogMessage &msg) override
    {
        syslog(level_to_syslog_priority(msg.level), "%.*s", (int)msg.body.size(), msg.body.data());
    }
    void flush() override {}
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
#endif

#ifdef PLATFORM_WIN64
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
        if (!handle_) return;
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
    void flush() override {}
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
#endif

// Command Definitions
struct SetSinkCommand { std::unique_ptr<Sink> new_sink; };
struct SinkCreationErrorCommand { std::string error_message; };
struct FlushCommand { std::shared_ptr<std::promise<void>> promise; };
struct SetErrorCallbackCommand { std::function<void(const std::string &)> callback; };

using Command = std::variant<LogMessage, SetSinkCommand, SinkCreationErrorCommand, FlushCommand,
                             SetErrorCallbackCommand>;

// Logger Pimpl and Implementation
struct Logger::Impl
{
    Impl();
    ~Impl();
    void start_worker();
    void worker_loop();
    void enqueue_command(Command &&cmd);
    void shutdown();

    std::thread worker_thread_;
    std::vector<Command> queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_requested_{false};

    std::atomic<Logger::Level> level_{Logger::Level::L_INFO};
    std::unique_ptr<Sink> sink_;
    std::function<void(const std::string &)> error_callback_;
    CallbackDispatcher callback_dispatcher_;
    std::atomic<bool> shutdown_completed_{false};
};

Logger::Impl::Impl() : sink_(std::make_unique<ConsoleSink>())
{
    // Worker thread is no longer started here.
}

Logger::Impl::~Impl()
{
    if (worker_thread_.joinable() && !shutdown_requested_.load()) {
        // This situation should be avoided by using the LifecycleManager.
    }
}

void Logger::Impl::start_worker()
{
    if (!worker_thread_.joinable())
    {
        worker_thread_ = std::thread(&Logger::Impl::worker_loop, this);
    }
}

void Logger::Impl::enqueue_command(Command &&cmd)
{
    if (shutdown_requested_.load(std::memory_order_relaxed))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutdown_requested_.load(std::memory_order_acquire)) return;
        queue_.emplace_back(std::move(cmd));
    }
    cv_.notify_one();
}

void Logger::Impl::worker_loop()
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
                                              make_buffer("Switching log sink to: {}", new_desc)});
                                sink_->flush();
                            }
                            sink_ = std::move(arg.new_sink);
                            if (sink_)
                            {
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(),
                                              get_native_thread_id(),
                                              make_buffer("Log sink switched from: {}", old_desc)});
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

void Logger::Impl::shutdown()
{
    if (shutdown_completed_.load() || shutdown_requested_.exchange(true))
    {
        return;
    }
    cv_.notify_one();
    if (worker_thread_.joinable())
    {
        worker_thread_.join();
    }
    callback_dispatcher_.shutdown();
    shutdown_completed_.store(true);
}

// Logger Public API Implementation
namespace
{
std::unique_ptr<Logger> g_instance;
std::mutex g_instance_mutex;
}

Logger::Logger() : pImpl(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

Logger &Logger::instance()
{
    if (!g_instance)
    {
        std::lock_guard<std::mutex> lock(g_instance_mutex);
        if (!g_instance)
        {
            struct LoggerMaker : public Logger { LoggerMaker() : Logger() {} };
            g_instance = std::make_unique<LoggerMaker>();
        }
    }
    return *g_instance;
}

bool Logger::lifecycle_initialized() noexcept {
    return g_logger_initialized.load(std::memory_order_acquire);
}

void Logger::set_console()
{
    check_initialized_and_abort("Logger::set_console");
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
    check_initialized_and_abort("Logger::set_logfile");
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
    check_initialized_and_abort("Logger::set_syslog");
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
    (void)ident; (void)option; (void)facility;
#endif
}

void Logger::set_eventlog(const wchar_t *source_name)
{
    check_initialized_and_abort("Logger::set_eventlog");
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
    (void)source_name;
#endif
}

void Logger::shutdown()
{
    // Do not abort if called before init, just do nothing.
    if (!lifecycle_initialized()) { return; }
    if (pImpl) pImpl->shutdown();
}

void Logger::flush()
{
    check_initialized_and_abort("Logger::flush");
    if (!pImpl || pImpl->shutdown_requested_.load()) return;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    pImpl->enqueue_command(FlushCommand{promise});
    future.wait();
}

void Logger::set_level(Level lvl)
{
    check_initialized_and_abort("Logger::set_level");
    if (pImpl) pImpl->level_.store(lvl, std::memory_order_relaxed);
}

Logger::Level Logger::level() const
{
    check_initialized_and_abort("Logger::level");
    return pImpl ? pImpl->level_.load(std::memory_order_relaxed) : Level::L_INFO;
}



void Logger::set_write_error_callback(std::function<void(const std::string &)> cb)
{
    check_initialized_and_abort("Logger::set_write_error_callback");
    if (pImpl) pImpl->enqueue_command(SetErrorCallbackCommand{std::move(cb)});
}

bool Logger::should_log(Level lvl) const noexcept
{

    return lifecycle_initialized() && pImpl && static_cast<int>(lvl) >= static_cast<int>(pImpl->level_.load(std::memory_order_relaxed));
}

void Logger::enqueue_log(Level lvl, fmt::memory_buffer &&body) noexcept
{
    check_initialized_and_abort("Logger::enqueue_log");
    if (pImpl)
    {
        pImpl->enqueue_command(LogMessage{lvl, std::chrono::system_clock::now(), get_native_thread_id(), std::move(body)});
    }
}

void Logger::enqueue_log(Level lvl, std::string &&body_str) noexcept
{
    check_initialized_and_abort("Logger::enqueue_log");
    if (pImpl)
    {
        pImpl->enqueue_command(LogMessage{lvl, std::chrono::system_clock::now(), get_native_thread_id(), make_buffer("{}", std::move(body_str))});
    }
}

// C-style callbacks for the ABI-safe lifecycle API.
// These functions are called by the LifecycleManager.
void do_logger_startup(const char* arg) {
    (void)arg; // Argument not used by logger startup.
    Logger::instance().pImpl->start_worker();
    g_logger_initialized.store(true, std::memory_order_release);
}
void do_logger_shutdown(const char* arg) {
    (void)arg; // Argument not used by logger shutdown.
    if (Logger::lifecycle_initialized()) {
        Logger::instance().shutdown();
    }
    g_logger_initialized.store(false, std::memory_order_release);
}

ModuleDef Logger::GetLifecycleModule()
{
    ModuleDef module("pylabhub::utils::Logger");
    // Using the no-argument overloads now.
    module.set_startup(&do_logger_startup);
    module.set_shutdown(&do_logger_shutdown, 5000 /*ms timeout*/);
    return module;
}

} // namespace pylabhub::utils
