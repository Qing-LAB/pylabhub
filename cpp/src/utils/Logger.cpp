// src/utils/Logger.cpp

#include "utils/Logger.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/ostream.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <unistd.h>
#endif

// Helper: get a platform-native thread id
static uint64_t get_native_thread_id() noexcept
{
#if defined(_WIN32)
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

namespace pylabhub::utils
{

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

// Helper: formatted local time with sub-second resolution
static std::string formatted_time(std::chrono::system_clock::time_point timestamp)
{
    // Use {fmt} to format the time portably. {:%Y-%m-%d %H:%M:%S.%f} provides
    // microsecond precision.
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", timestamp);
}

static std::string format_message(const LogMessage &msg)
{
    return fmt::format("{} [{}] [tid={}] {}\n", formatted_time(msg.timestamp),
                       level_to_string(msg.level), msg.thread_id,
                       std::string_view(msg.body.data(), msg.body.size()));
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
#ifdef _WIN32
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
#ifdef _WIN32
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
#ifdef _WIN32
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
#ifdef _WIN32
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
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

#if !defined(_WIN32)
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
#endif // !defined(_WIN32)

#ifdef _WIN32
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
#endif // _WIN32

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
struct ShutdownCommand
{
};
struct SetErrorCallbackCommand
{
    std::function<void(const std::string &)> callback;
};

using Command =
    std::variant<LogMessage, SetConsoleSinkCommand, SetFileSinkCommand, SetSyslogSinkCommand,
                 SetEventLogSinkCommand, FlushCommand, ShutdownCommand, SetErrorCallbackCommand>;

// --- Private Implementation (Pimpl) ---
struct Impl
{
    Impl();
    ~Impl();

    void worker_loop();
    void enqueue_command(Command &&cmd);

    // Worker thread and queue
    std::thread worker_thread_;
    std::vector<Command> queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> done_{false};

    // Logger state (worker thread only)
    std::atomic<Logger::Level> level_{Logger::Level::L_DEBUG};
    std::unique_ptr<Sink> sink_;
    std::function<void(const std::string &)> error_callback_;
};

Impl::Impl() : sink_(std::make_unique<ConsoleSink>())
{
    worker_thread_ = std::thread(&Impl::worker_loop, this);
}

Impl::~Impl()
{
    if (worker_thread_.joinable())
    {
        if (!done_.load(std::memory_order_relaxed))
        {
            enqueue_command(ShutdownCommand{});
        }
        worker_thread_.join();
    }
}

void Impl::enqueue_command(Command &&cmd)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
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
            cv_.wait(lock, [this] { return !queue_.empty() || done_.load(); });

            if (done_.load() && queue_.empty())
                break;
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
                                              fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to Console")});
                                sink_->flush();
                            }
                            sink_ = std::make_unique<ConsoleSink>();
                            sink_->write({Logger::Level::L_SYSTEM,
                                          std::chrono::system_clock::now(), get_native_thread_id(),
                                          fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to Console")});
                        }
                        else if constexpr (std::is_same_v<T, SetFileSinkCommand>)
                        {
                            if (sink_)
                            {
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(),
                                              get_native_thread_id(),
                                              fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to file: {}", arg.path)});
                                sink_->flush();
                            }
                            sink_ = std::make_unique<FileSink>(arg.path, arg.use_flock);
                            sink_->write({Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                          get_native_thread_id(),
                                          fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to file: {}", arg.path)});
                        }
#if !defined(_WIN32)
                        else if constexpr (std::is_same_v<T, SetSyslogSinkCommand>)
                        {
                            if (sink_)
                            {
                                sink_->flush();
                                sink_->write({Logger::Level::L_SYSTEM,
                                              std::chrono::system_clock::now(),
                                              get_native_thread_id(),
                                              fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to Syslog")});
                            }
                            sink_ = std::make_unique<SyslogSink>(
                                arg.ident.empty() ? nullptr : arg.ident.c_str(), arg.option,
                                arg.facility);
                            sink_->write({Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                          get_native_thread_id(),
                                          fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to Syslog")});
                        }
#endif
#ifdef _WIN32
                        else if constexpr (std::is_same_v<T, SetEventLogSinkCommand>)
                        {
                            if (sink_)
                            {
                                sink_->write(
                                    {Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                     get_native_thread_id(),
                                     fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to Windows Event Log")});
                                sink_->flush();
                            }
                            sink_ = std::make_unique<EventLogSink>(arg.source_name.c_str());
                            sink_->write({Logger::Level::L_SYSTEM, std::chrono::system_clock::now(),
                                          get_native_thread_id(),
                                          fmt::format_to_buffer(fmt::memory_buffer(), "Switched log to Windows Event Log")});
                        }
#endif
                        else if constexpr (std::is_same_v<T, FlushCommand>)
                        {
                            if (sink_)
                                sink_->flush();
                            arg.promise.set_value();
                        }
                        else if constexpr (std::is_same_v<T, ShutdownCommand>)
                        {
                            if (sink_)
                                sink_->flush();
                            done_.store(true);
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
                    error_callback_(fmt::format("Logger error: {}", e.what()));
                sink_.reset(); // Stop logging to a faulty sink
            }
        }
        local_queue.clear();

        if (done_.load() && queue_.empty())
            break;
    }
}

// --- Logger Public API Implementation ---

Logger::Logger() : pImpl(std::make_unique<Impl>()) {}
Logger::~Logger()
{
    if (pImpl)
        shutdown();
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
#if !defined(_WIN32)
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
#ifdef _WIN32
    pImpl->enqueue_command(SetEventLogSinkCommand{source_name});
#else
    (void)source_name;
#endif
}

void Logger::shutdown()
{
    if (!pImpl || pImpl->done_.load())
        return;
    pImpl->enqueue_command(ShutdownCommand{});
    if (pImpl->worker_thread_.joinable())
    {
        pImpl->worker_thread_.join();
    }
}

void Logger::flush()
{
    if (!pImpl)
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
    if (!pImpl || pImpl->done_.load(std::memory_order_relaxed))
        return;

    LogMessage msg{lvl, std::chrono::system_clock::now(), get_native_thread_id(), std::move(body)};
    pImpl->enqueue_command(std::move(msg));
}

void Logger::enqueue_log(Level lvl, std::string &&body_str) noexcept
{
    if (!pImpl || pImpl->done_.load(std::memory_order_relaxed))
        return;

    fmt::memory_buffer mb;
    mb.append(body_str);
    LogMessage msg{lvl, std::chrono::system_clock::now(), get_native_thread_id(), std::move(mb)};
    pImpl->enqueue_command(std::move(msg));
}

} // namespace pylabhub::utils
