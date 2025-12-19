// src/utils/Logger.cpp

#include "utils/Logger.hpp"

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

// --- Private Callback Dispatcher for handling re-entrant calls safely ---
class CallbackDispatcher
{
  public:
    CallbackDispatcher() : shutdown_requested_(false) { worker_ = std::thread([this] { this->run(); }); }

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
                // Exceptions in user callbacks are swallowed
            }
        }
    }

    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> shutdown_requested_;
};

// --- Command and Sink Definitions ---

struct LogMessage
{
    Logger::Level level;
    std::chrono::system_clock::time_point timestamp;
    uint64_t thread_id;
    fmt::memory_buffer body;
};

// --- Sink Abstraction ---
class Sink
{
  public:
    virtual ~Sink() = default;
    virtual void write(const LogMessage &msg) = 0;
    virtual void flush() = 0;
    virtual std::string description() const = 0;
};

// --- Helper Functions ---
static const char *level_to_string(Logger::Level lvl)
{
    switch (lvl)
    {
    case Logger::Level::L_TRACE:
        return "TRACE";
    case Logger::Level::L_DEBUG:
        return "DEBUG";
    case Logger::Level::L_INFO:
        return "INFO";
    case Logger::Level::L_WARNING:
        return "WARN";
    case Logger::Level::L_ERROR:
        return "ERROR";
    case Logger::Level::L_SYSTEM:
        return "SYSTEM";
    default:
        return "UNK";
    }
}

// Helper: get a platform-native thread id
static uint64_t get_native_thread_id() noexcept
{
#if defined(PLATFORM_WIN64)
    return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(nullptr, &tid);
    return tid;
#elif defined(__linux__)
    return static_cast<uint64_t>(syscall(SYS_gettid));
#else
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

// --- Helper: formatted local time with sub-second resolution (robust) ---
// Replaces previous formatted_time(...) implementation.
// Behaviour:
//  - If the build system detected fmt chrono subseconds support (HAVE_FMT_CHRONO_SUBSECONDS),
//    use single-step fmt formatting on a microsecond-truncated time_point.
//  - Otherwise, fall back to computing the fractional microsecond part and append it
//    manually using a two-step format.
static std::string formatted_time(std::chrono::system_clock::time_point timestamp)
{
#if defined(HAVE_FMT_CHRONO_SUBSECONDS) && HAVE_FMT_CHRONO_SUBSECONDS
    auto tp_us = std::chrono::time_point_cast<std::chrono::microseconds>(timestamp);
    #if defined(FMT_CHRONO_FMT_STYLE) && (FMT_CHRONO_FMT_STYLE == 1)
        // use %f
        return fmt::format("{:%Y-%m-%d %H:%M:%S.%f}", tp_us);
    #elif defined(FMT_CHRONO_FMT_STYLE) && (FMT_CHRONO_FMT_STYLE == 2)
        // fmt prints fraction without %f
        return fmt::format("{:%Y-%m-%d %H:%M:%S}", tp_us);
    #else
        // defensive fallback to manual two-step
        auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp_us);
        int fractional_us = static_cast<int>((tp_us - secs).count());
        auto sec_part = fmt::format("{:%Y-%m-%d %H:%M:%S}", secs);
        return fmt::format("{}.{:06d}", sec_part, fractional_us);
    #endif
#else
    // no runtime support detected — fallback to manual two-step method
    auto tp_us = std::chrono::time_point_cast<std::chrono::microseconds>(timestamp);
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp_us);
    int fractional_us = static_cast<int>((tp_us - secs).count());
    auto sec_part = fmt::format("{:%Y-%m-%d %H:%M:%S}", secs);
    return fmt::format("{}.{:06d}", sec_part, fractional_us);
#endif
}


// --- Helper: turn a string into a fmt::memory_buffer (compile-time format)
template <typename... Args>
static fmt::memory_buffer make_buffer(fmt::format_string<Args...> fmt_str, Args &&...args)
{
    fmt::memory_buffer mb;
    mb.reserve(128); // small reserve to avoid many reallocs
    fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);
    return mb;
}
// --Helper: turn a string_view into a fmt::memory_buffer (runtime format)
template <typename... Args>
static fmt::memory_buffer make_buffer_rt(fmt::string_view fmt_str, Args &&...args)
{
    fmt::memory_buffer mb;
    mb.reserve(128);
    fmt::format_to(std::back_inserter(mb), fmt::runtime(fmt_str), std::forward<Args>(args)...);
    return mb;
}

// --Helper: format a LogMessage into a string
static std::string format_message(const LogMessage &msg)
{
    std::string time_str = formatted_time(msg.timestamp);
    std::string level_str = level_to_string(msg.level);
    std::string thread_str = fmt::format("{}", msg.thread_id);
    std::string body_str = std::string(msg.body.data(), msg.body.size());

    return fmt::format("[{}] [{}] [{}] {}\n", time_str, level_str, thread_str, body_str);
}

// --- Concrete Sinks ---
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
        (void)use_flock_; // Not supported
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

    void flush() override {} // Not buffered in app

    std::string description() const override { return "Syslog"; }

  private:
    static int level_to_syslog_priority(Logger::Level level)
    {
        switch (level)
        {
        case Logger::Level::L_TRACE:
            return LOG_DEBUG;
        case Logger::Level::L_DEBUG:
            return LOG_DEBUG;
        case Logger::Level::L_INFO:
            return LOG_INFO;
        case Logger::Level::L_WARNING:
            return LOG_WARNING;
        case Logger::Level::L_ERROR:
            return LOG_ERR;
        case Logger::Level::L_SYSTEM:
            return LOG_CRIT;
        default:
            return LOG_INFO;
        }
    }
};
#endif // !defined(PLATFORM_WIN64)

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
        if (!handle_)
            return;

        // Convert UTF-8 from memory_buffer to UTF-16
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
        MultiByteToWideChar(CP_UTF8, 0, msg.body.data(), static_cast<int>(msg.body.size()), &wbody[0], needed);

        const wchar_t *strings[1] = {wbody.c_str()};
        ReportEventW(handle_, level_to_eventlog_type(msg.level), 0, 0, nullptr, 1, 0, strings,
                     nullptr);
    }

    void flush() override {} // Not applicable

    std::string description() const override { return "Windows Event Log"; }

  private:
    HANDLE handle_ = nullptr;

    static WORD level_to_eventlog_type(Logger::Level level)
    {
        switch (level)
        {
        case Logger::Level::L_TRACE:
        case Logger::Level::L_DEBUG:
        case Logger::Level::L_INFO:
            return EVENTLOG_INFORMATION_TYPE;
        case Logger::Level::L_WARNING:
            return EVENTLOG_WARNING_TYPE;
        case Logger::Level::L_ERROR:
        case Logger::Level::L_SYSTEM:
            return EVENTLOG_ERROR_TYPE;
        default:
            return EVENTLOG_INFORMATION_TYPE;
        }
    }
};
#endif // PLATFORM_WIN64

// --- Control Commands for the queue ---
struct SetConsoleSinkCommand
{
};
struct SetFileSinkCommand
{
    std::string path;
    bool use_flock;
};
struct SetSyslogSinkCommand
{
    std::string ident;
    int option;
    int facility;
};
struct SetEventLogSinkCommand
{
    std::wstring source_name;
};
struct FlushCommand
{
    std::shared_ptr<std::promise<void>> promise;
};
struct SetErrorCallbackCommand
{
    std::function<void(const std::string &)> callback;
};

using Command =
    std::variant<LogMessage, SetConsoleSinkCommand, SetFileSinkCommand, SetSyslogSinkCommand,
                 SetEventLogSinkCommand, FlushCommand, SetErrorCallbackCommand>;

// --- Private Implementation (Pimpl) ---
struct Impl
{
    Impl();
    ~Impl();

    void worker_loop();
    void enqueue_command(Command &&cmd);
    void shutdown();

    // Worker thread and queue
    std::thread worker_thread_;
    std::vector<Command> queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_requested_{false};

    // Logger state (worker thread only)
    std::atomic<Logger::Level> level_{Logger::Level::L_DEBUG};
    std::unique_ptr<Sink> sink_;
    std::function<void(const std::string &)> error_callback_;
    CallbackDispatcher callback_dispatcher_;
};

Impl::Impl() : sink_(std::make_unique<ConsoleSink>())
{
    worker_thread_ = std::thread(&Impl::worker_loop, this);
}

Impl::~Impl()
{
    if (!shutdown_requested_.load())
    {
        // Shutdown was not called explicitly. This is not an error, but it's
        // not recommended, as logs from other static destructors may be lost.
        // The explicit pylabhub::utils::Finalize() function is the preferred way
        // to ensure a clean shutdown.
        fmt::print(stderr,
                   "[pylabhub::Logger WARNING]: Logger was not shut down explicitly. "
                   "Call pylabhub::utils::Finalize() before main() returns to guarantee "
                   "all logs are flushed.\n");
    }
    // Always call shutdown() as a fallback.
    shutdown();
}

void Impl::enqueue_command(Command &&cmd)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutdown_requested_.load(std::memory_order_relaxed))
            return;
        queue_.emplace_back(std::move(cmd));
    }
    cv_.notify_one();
}

void Impl::worker_loop()
{
    std::vector<Command> local_queue;

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || shutdown_requested_.load(); });

            // Before exiting, process any remaining items, then do a final flush.
            if (shutdown_requested_.load() && queue_.empty())
            {
                if (sink_)
                    sink_->flush();
                break;
            }
            local_queue.swap(queue_);
        }

        for (auto &cmd : local_queue)
        {
            try
            {
                std::visit(
                    [this](auto &&arg)
                    {
                        using T = std::decay_t<decltype(arg)>;
                        if constexpr (std::is_same_v<T, LogMessage>)
                        {
                            if (sink_)
                                sink_->write(arg);
                        }
                        else if constexpr (std::is_same_v<T, SetConsoleSinkCommand>)
                        {
                            if (sink_)
                            {
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(), get_native_thread_id(),
                                              make_buffer("Switched log to Console")});
                                sink_->flush();
                            }
                            sink_ = std::make_unique<ConsoleSink>();
                            sink_->write({Logger::Level::L_SYSTEM,
                                          std::chrono::system_clock::now(), get_native_thread_id(),
                                          make_buffer("Switched log to Console")});
                        }
                        else if constexpr (std::is_same_v<T, SetFileSinkCommand>)
                        {
                            if (sink_)
                            {
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(),
                                              get_native_thread_id(),
                                              make_buffer("Switched log to file: {}", arg.path)});
                                sink_->flush();
                            }
                            sink_ = std::make_unique<FileSink>(arg.path, arg.use_flock);
                            sink_->write({Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                          get_native_thread_id(),
                                          make_buffer("Switched log to file: {}", arg.path)});
                        }
#if !defined(PLATFORM_WIN64)
                        else if constexpr (std::is_same_v<T, SetSyslogSinkCommand>)
                        {
                            if (sink_)
                            {
                                sink_->flush();
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(),
                                              get_native_thread_id(),
                                              make_buffer("Switched log to Syslog")});
                            }
                            sink_ = std::make_unique<SyslogSink>(
                                arg.ident.empty() ? nullptr : arg.ident.c_str(), arg.option,
                                arg.facility);
                            sink_->write({Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                          get_native_thread_id(),
                                          make_buffer("Switched log to Syslog")});
                        }
#endif
#ifdef PLATFORM_WIN64
                        else if constexpr (std::is_same_v<T, SetEventLogSinkCommand>)
                        {
                            if (sink_)
                            {
                                sink_->write(
                                    {Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                     get_native_thread_id(),
                                     make_buffer("Switched log to Windows Event Log")});
                                sink_->flush();
                            }
                            sink_ = std::make_unique<EventLogSink>(arg.source_name.c_str());
                            sink_->write({Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                          get_native_thread_id(),
                                          make_buffer("Switched log to Windows Event Log")});
                        }
#endif
                        else if constexpr (std::is_same_v<T, FlushCommand>)
                        {
                            if (sink_)
                                sink_->flush();
                            arg.promise->set_value();
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
                if (error_callback_)
                {
                    auto cb = error_callback_;
                    auto msg = fmt::format("Logger error: {}", e.what());
                    callback_dispatcher_.post([cb, msg]() { cb(msg); });
                }
                sink_.reset(); // Stop logging to a faulty sink
            }
        }
        local_queue.clear();
    }
}

void Impl::shutdown()
{
    // Prevent multiple shutdowns and stop accepting new commands
    if (shutdown_requested_.exchange(true))
    {
        return;
    }

    // Wake up worker thread to process the shutdown
    cv_.notify_one();
    if (worker_thread_.joinable())
    {
        worker_thread_.join();
    }

    // Now that no more callbacks can be generated, shut down the dispatcher.
    // This will process any remaining callbacks in its queue.
    callback_dispatcher_.shutdown();
}


// --- Logger Public API Implementation ---

Logger::Logger() : pImpl(std::make_unique<Impl>()) {}
Logger::~Logger()
{
    if (pImpl)
    {
        // Fallback shutdown for cases where Finalize() is not used
        pImpl->shutdown();
    }
}

Logger &Logger::instance()
{
    static Logger instance;
    return instance;
}

void Logger::set_console()
{
    pImpl->enqueue_command(SetConsoleSinkCommand{});
}

void Logger::set_logfile(const std::string &utf8_path, bool use_flock)
{
    pImpl->enqueue_command(SetFileSinkCommand{utf8_path, use_flock});
}

void Logger::set_syslog(const char *ident, int option, int facility)
{
#if !defined(PLATFORM_WIN64)
    pImpl->enqueue_command(
        SetSyslogSinkCommand{ident ? std::string(ident) : std::string(), option, facility});
#else
    (void)ident;
    (void)option;
    (void)facility;
#endif
}

void Logger::set_eventlog(const wchar_t *source_name)
{
#ifdef PLATFORM_WIN64
    pImpl->enqueue_command(SetEventLogSinkCommand{source_name});
#else
    (void)source_name;
#endif
}

void Logger::shutdown()
{
    if (pImpl)
    {
        pImpl->shutdown();
    }
}

void Logger::flush()
{
    if (!pImpl || pImpl->shutdown_requested_.load())
        return;
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    pImpl->enqueue_command(FlushCommand{promise});
    future.wait();
}

void Logger::set_level(Level lvl)
{
    if (!pImpl)
        return;
    pImpl->level_.store(lvl, std::memory_order_relaxed);
}

Logger::Level Logger::level() const
{
    if (!pImpl)
        return Level::L_INFO;
    return pImpl->level_.load(std::memory_order_relaxed);
}

void Logger::set_write_error_callback(std::function<void(const std::string &)> cb)
{
    if (!pImpl)
        return;
    pImpl->enqueue_command(SetErrorCallbackCommand{std::move(cb)});
}

bool Logger::should_log(Level lvl) const noexcept
{
    if (!pImpl)
        return false;
    return static_cast<int>(lvl) >= static_cast<int>(pImpl->level_.load(std::memory_order_relaxed));
}

void Logger::enqueue_log(Level lvl, fmt::memory_buffer &&body) noexcept
{
    if (!pImpl)
        return;
    pImpl->enqueue_command(
        LogMessage{lvl, std::chrono::system_clock::now(), get_native_thread_id(), std::move(body)});
}

void Logger::enqueue_log(Level lvl, std::string &&body_str) noexcept
{
    if (!pImpl)
        return;

    fmt::memory_buffer mb;
    mb.append(body_str);
    pImpl->enqueue_command(
        LogMessage{lvl, std::chrono::system_clock::now(), get_native_thread_id(), std::move(mb)});
}

} // namespace pylabhub::utils
