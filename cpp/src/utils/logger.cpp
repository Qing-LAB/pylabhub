/*******************************************************************************
 * @file logger.cpp
 * @brief Implementation of the high-performance, asynchronous logger.
 ******************************************************************************/

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <stdexcept>
#include <variant>

#include "plh_base.hpp"

#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"

#include "utils/logger_sinks/sink.hpp"
#include "utils/logger_sinks/console_sink.hpp"
#include "utils/logger_sinks/file_sink.hpp"
#include "utils/logger_sinks/rotating_file_sink.hpp"
#include "utils/logger_sinks/syslog_sink.hpp"
#include "utils/logger_sinks/event_log_sink.hpp"

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#include <fmt/chrono.h>
#include <fmt/format.h>

#if defined(PYLABHUB_IS_POSIX)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace pylabhub::format_tools;

namespace pylabhub::utils
{

// Represents the lifecycle state of the logger.
enum class LoggerState
{
    Uninitialized,
    Initialized,
    ShuttingDown,
    Shutdown
};

// Global atomic to track the logger's state.
static std::atomic<LoggerState> g_logger_state{LoggerState::Uninitialized};

// Centralized check function
static bool logger_is_loggable(const char *function_name)
{
    const auto state = g_logger_state.load(std::memory_order_acquire);
    if (state == LoggerState::Uninitialized)
    {
        PLH_PANIC("Logger method '{}' was called before the Logger module was "
                  "initialized via LifecycleManager. Aborting.",
                  function_name);
    }
    return state == LoggerState::Initialized;
}

namespace
{
// Helper to synchronously check if a directory is writable.
bool check_directory_is_writable(const std::filesystem::path &dir, std::error_code &ec)
{
    ec.clear();
    try
    {
        auto temp_file_path =
            dir / fmt::format("pylabhub_write_check_{}.tmp",
                              std::chrono::high_resolution_clock::now().time_since_epoch().count());

#ifdef PYLABHUB_PLATFORM_WIN64
        std::wstring wpath = pylabhub::format_tools::win32_to_long_path(temp_file_path);
        if (wpath.empty())
        {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }

        // Create a temporary file that is deleted immediately on close.
        HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            return false;
        }
        CloseHandle(h); // The file is deleted automatically on close.
#else
        int fd = ::open(temp_file_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd == -1)
        {
            ec = std::error_code(errno, std::generic_category());
            return false;
        }
        ::close(fd);
        ::unlink(temp_file_path.c_str()); // Clean up the temporary file immediately.
#endif
        return true;
    }
    catch (...)
    {
        ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
}
} // anonymous namespace

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

// Command Definitions
struct SetSinkCommand
{
    std::unique_ptr<Sink> new_sink;
    std::shared_ptr<std::promise<bool>> promise;
};
struct SinkCreationErrorCommand
{
    std::string error_message;
    std::shared_ptr<std::promise<bool>> promise;
};
struct FlushCommand
{
    std::shared_ptr<std::promise<bool>> promise;
};
struct SetErrorCallbackCommand
{
    std::function<void(const std::string &)> callback;
    std::shared_ptr<std::promise<bool>> promise;
};

struct SetLogSinkMessagesCommand
{
    bool enabled;
    std::shared_ptr<std::promise<bool>> promise;
};

using Command = std::variant<LogMessage, SetSinkCommand, SinkCreationErrorCommand, FlushCommand,
                             SetErrorCallbackCommand, SetLogSinkMessagesCommand>;

// --- Promise Helper Functions ---
template <typename T> void promise_set_safe(const std::shared_ptr<std::promise<T>> &p, T value)
{
    if (!p)
        return;
    try
    {
        p->set_value(std::move(value));
    }
    catch (...)
    {
        // Promise already satisfied or broken - swallow to avoid std::terminate.
    }
}

template <typename T>
void promise_set_exception_safe(const std::shared_ptr<std::promise<T>> &p, std::exception_ptr ep)
{
    if (!p)
        return;
    try
    {
        p->set_exception(ep);
    }
    catch (...)
    {
        // swallow
    }
}

// Logger Pimpl and Implementation
struct Logger::Impl
{
    Impl();
    ~Impl();
    void start_worker();
    void worker_loop();
    bool enqueue_command(Command &&cmd);
    void reject_command_due_to_shutdown(Command &cmd);
    void shutdown();

    // Ordered for optimal packing as suggested by clang-tidy:
    // error_callback_, worker_thread_, sink_, m_max_queue_size, m_messages_dropped,
    // m_dropping_since, queue_, cv_, queue_mutex_, m_sink_mutex, callback_dispatcher_, level_,
    // shutdown_requested_, shutdown_completed_, m_log_sink_messages_enabled_, m_was_dropping

    std::function<void(const std::string &)> error_callback_; // Size: pointer (8 bytes)
    std::thread worker_thread_; // Size: depends on implementation (e.g., 8 bytes on x64 for pointer
                                // to thread data)
    std::unique_ptr<Sink> sink_;    // Size: pointer (8 bytes)
    size_t m_max_queue_size{10000}; // Size: 8 bytes (on x64)
    std::chrono::system_clock::time_point
        m_dropping_since;        // Size: depends on implementation (e.g., 8 bytes for duration)
    std::vector<Command> queue_; // Size: 24 bytes (on x64 for pointer, size, capacity)
    std::condition_variable cv_; // Size: depends on implementation (e.g., 40 bytes)
    std::mutex queue_mutex_;     // Size: depends on implementation (e.g., 40 bytes)
    std::mutex m_sink_mutex;     // Size: depends on implementation (e.g., 40 bytes)
    CallbackDispatcher callback_dispatcher_; // Size: depends on implementation (e.g., 104 bytes in
                                             // CallbackDispatcher: std::deque (24), std::mutex
                                             // (40), std::condition_variable (40), std::thread (8),
                                             // std::atomic<bool> (1))
    std::atomic<Logger::Level> level_{Logger::Level::L_INFO}; // Size: 4 bytes (for int)
    std::atomic<bool> shutdown_requested_{false};             // Size: 1 byte
    std::atomic<bool> shutdown_completed_{false};             // Size: 1 byte
    std::atomic<bool> m_log_sink_messages_enabled_{true};     // Size: 1 byte
    std::atomic<bool> m_was_dropping{false};                  // Size: 1 byte
    std::atomic<size_t> m_messages_dropped{0};                // Batch counter for summary; exchange(0) when processed
    std::atomic<size_t> m_total_dropped_since_sink_switch{0}; // Accumulated total; reset on sink switch
};

Logger::Impl::Impl() : sink_(std::make_unique<ConsoleSink>())
{
    // Worker thread is no longer started here.
}

Logger::Impl::~Impl()
{
    if (worker_thread_.joinable() && !shutdown_requested_.load())
    {
        // This situation should be avoided by using the LifecycleManager.
        PLH_DEBUG("**HIGH ALERT: Logger Impl destructor called without prior shutdown. This should "
                  "NOT happen. Check LifeCycle management.**");
    }
}

void Logger::Impl::start_worker()
{
    if (!worker_thread_.joinable())
    {
        worker_thread_ = std::thread(&Logger::Impl::worker_loop, this);
    }
}

void Logger::Impl::reject_command_due_to_shutdown(Command &cmd)
{
    std::visit(
        [](auto &&arg)
        {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, SetSinkCommand> || std::is_same_v<T, FlushCommand> ||
                          std::is_same_v<T, SetErrorCallbackCommand> ||
                          std::is_same_v<T, SetLogSinkMessagesCommand> ||
                          std::is_same_v<T, SinkCreationErrorCommand>)
            {
                if (arg.promise)
                {
                    promise_set_safe(arg.promise, false);
                }
            }
        },
        cmd);
}

bool Logger::Impl::enqueue_command(Command &&cmd)
{
    if (shutdown_requested_.load(std::memory_order_relaxed))
    {
        reject_command_due_to_shutdown(cmd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutdown_requested_.load(std::memory_order_acquire))
        {
            reject_command_due_to_shutdown(cmd);
            return false;
        }

        const size_t current_queue_size = queue_.size();
        const size_t max_queue_size_soft = m_max_queue_size;
        const size_t max_queue_size_hard = m_max_queue_size * 2;

        if (current_queue_size >= max_queue_size_hard)
        {
            m_messages_dropped.fetch_add(1, std::memory_order_relaxed);
            m_total_dropped_since_sink_switch.fetch_add(1, std::memory_order_relaxed);
            if (!m_was_dropping.exchange(true, std::memory_order_relaxed))
            {
                m_dropping_since = std::chrono::system_clock::now();
            }
            reject_command_due_to_shutdown(cmd);
            return false;
        }

        if (current_queue_size >= max_queue_size_soft && std::holds_alternative<LogMessage>(cmd))
        {
            m_messages_dropped.fetch_add(1, std::memory_order_relaxed);
            m_total_dropped_since_sink_switch.fetch_add(1, std::memory_order_relaxed);
            if (!m_was_dropping.exchange(true, std::memory_order_relaxed))
            {
                m_dropping_since = std::chrono::system_clock::now();
            }
            return false;
        }

        queue_.emplace_back(std::move(cmd));
    }
    cv_.notify_one();
    return true;
}

void Logger::Impl::worker_loop()
{
    std::vector<Command> local_queue;

    while (true)
    {
        bool was_dropping = false;
        size_t dropped_count = 0;
        double dropping_duration_s = 0.0;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || shutdown_requested_.load(); });
            local_queue.swap(queue_);

            if (m_was_dropping.exchange(false, std::memory_order_relaxed))
            {
                was_dropping = true;
                dropped_count = m_messages_dropped.exchange(0, std::memory_order_relaxed);
                if (dropped_count > 0)
                {
                    dropping_duration_s =
                        std::chrono::duration_cast<std::chrono::duration<double>>(
                            std::chrono::system_clock::now() - m_dropping_since)
                            .count();
                }
            }

            if (shutdown_requested_.load())
            {
                g_logger_state.store(LoggerState::ShuttingDown, std::memory_order_release);
            }
        }

        // Per user suggestion, log a preliminary warning immediately if dropping occurred.
        // This provides immediate context to the developer reading the logs.
        if (was_dropping && dropped_count > 0)
        {
            std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
            if (sink_)
            {
                sink_->write(LogMessage{.timestamp = std::chrono::system_clock::now(),
                                        .process_id = pylabhub::platform::get_pid(),
                                        .thread_id = pylabhub::platform::get_native_thread_id(),
                                        .level = static_cast<int>(Logger::Level::L_WARNING),
                                        .body = make_buffer(
                                            "Overflow detected when processing the queue. Messages may have been dropped in the following batch.")},
                             Sink::ASYNC_WRITE);
            }
        }

        // --- Find last SetSinkCommand ---
        ssize_t last_set_sink_idx = -1;
        for (ssize_t i = static_cast<ssize_t>(local_queue.size()) - 1; i >= 0; --i)
        {
            if (std::holds_alternative<SetSinkCommand>(local_queue[i]))
            {
                last_set_sink_idx = i;
                break;
            }
        }

        // --- Process the dequeued batch ---
        for (ssize_t i = 0; i < static_cast<ssize_t>(local_queue.size()); ++i)
        {
            try
            {
                // Fast path: LogMessage
                if (auto *msg = std::get_if<LogMessage>(&local_queue[i]))
                {
                    std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
                    if (sink_ &&
                        msg->level >= static_cast<int>(level_.load(std::memory_order_relaxed)))
                    {
                        sink_->write(*msg, Sink::ASYNC_WRITE);
                    }
                    continue; // done with this item; keep ordering
                }

                // Slow path: control commands — rare, use visit for clarity.
                std::visit(
                    [&, this, i](auto &&arg)
                    {
                        using T = std::decay_t<decltype(arg)>;

                        if constexpr (std::is_same_v<T, SetSinkCommand>)
                        {
                            if (i != last_set_sink_idx)
                            {
                                promise_set_safe(arg.promise, false);
                            }
                        }
                        else if constexpr (std::is_same_v<T, SinkCreationErrorCommand>)
                        {
                            if (error_callback_)
                            {
                                auto cb = error_callback_;
                                callback_dispatcher_.post([cb, msg = arg.error_message]()
                                                          { cb(msg); });
                            }
                            else
                            {
                                PLH_DEBUG(" ** Logger sink creation error but no error_callback "
                                          "function can be reached : {}\n",
                                          arg.error_message);
                            }
                            promise_set_safe(arg.promise, false);
                        }
                        else if constexpr (std::is_same_v<T, FlushCommand>)
                        {
                            if (sink_)
                            {
                                std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
                                sink_->flush();
                            }
                            promise_set_safe(arg.promise, true);
                        }
                        else if constexpr (std::is_same_v<T, SetErrorCallbackCommand>)
                        {
                            error_callback_ = std::move(arg.callback);
                            promise_set_safe(arg.promise, true);
                        }
                        else if constexpr (std::is_same_v<T, SetLogSinkMessagesCommand>)
                        {
                            m_log_sink_messages_enabled_.store(arg.enabled,
                                                               std::memory_order_relaxed);
                            promise_set_safe(arg.promise, true);
                        }
                    },
                    local_queue[i]);
            }
            catch (const std::exception &e)
            {
                if (error_callback_)
                {
                    auto cb = error_callback_;
                    auto msg = fmt::format("Logger worker error: {}", e.what());
                    callback_dispatcher_.post([cb, msg]() { cb(msg); });
                }
            }
        }

        // --- Post-loop actions ---

        // Log the detailed summary warning message after the main batch is processed.
        if (was_dropping && dropped_count > 0)
        {
            std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
            if (sink_)
            {
                sink_->write(
                    LogMessage{
                        .timestamp = std::chrono::system_clock::now(),
                        .process_id = pylabhub::platform::get_pid(),
                        .thread_id = pylabhub::platform::get_native_thread_id(),
                        .level = static_cast<int>(Logger::Level::L_WARNING),
                        .body = make_buffer(
                            "Summary: At this point in time, the Logger dropped {} messages over {:.2f}s due to full queue.",
                            dropped_count, dropping_duration_s)},
                    Sink::ASYNC_WRITE);
            }
        }

        if (last_set_sink_idx != -1)
        {
            auto &cmd_variant = local_queue[last_set_sink_idx];
            if (auto *sink_cmd = std::get_if<SetSinkCommand>(&cmd_variant))
            {
                std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
                if (m_log_sink_messages_enabled_.load(std::memory_order_relaxed))
                {
                    std::string old_desc = sink_ ? sink_->description() : "null";
                    std::string new_desc =
                        sink_cmd->new_sink ? sink_cmd->new_sink->description() : "null";
                    if (sink_)
                    {
                        sink_->write(
                            LogMessage{.timestamp = std::chrono::system_clock::now(),
                                       .process_id = pylabhub::platform::get_pid(),
                                       .thread_id = pylabhub::platform::get_native_thread_id(),
                                       .level = static_cast<int>(Logger::Level::L_SYSTEM),
                                       .body = make_buffer("Switching log sink to: {}", new_desc)},
                            Sink::ASYNC_WRITE);
                        sink_->flush();
                    }
                    m_total_dropped_since_sink_switch.store(0, std::memory_order_relaxed);
                    sink_ = std::move(sink_cmd->new_sink);
                    if (sink_)
                    {
                        sink_->write(
                            LogMessage{.timestamp = std::chrono::system_clock::now(),
                                       .process_id = pylabhub::platform::get_pid(),
                                       .thread_id = pylabhub::platform::get_native_thread_id(),
                                       .level = static_cast<int>(Logger::Level::L_SYSTEM),
                                       .body = make_buffer("Log sink switched from: {}", old_desc)},
                            Sink::ASYNC_WRITE);
                    }
                }
                else
                {
                    m_total_dropped_since_sink_switch.store(0, std::memory_order_relaxed);
                    sink_ = std::move(sink_cmd->new_sink);
                }
                promise_set_safe(sink_cmd->promise, true);
            }
        }

        local_queue.clear();

        if (shutdown_requested_.load() && (g_logger_state.load() == LoggerState::ShuttingDown ||
                                           g_logger_state.load() == LoggerState::Shutdown))
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (!queue_.empty())
            {
                PLH_DEBUG(
                    "Logger worker found {} messages in queue during shutdown. Reprocessing...",
                    queue_.size());
                lock.unlock(); // Release mutex before continuing loop to allow new enqueues
                continue;      // Go back to the top of the while(true) loop to process these items.
            }

            PLH_DEBUG("Logger worker thread shutting down.");
            std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
            if (sink_)
            {
                sink_->write(LogMessage{.timestamp = std::chrono::system_clock::now(),
                                        .process_id = pylabhub::platform::get_pid(),
                                        .thread_id = pylabhub::platform::get_native_thread_id(),
                                        .level = static_cast<int>(Logger::Level::L_SYSTEM),
                                        .body = make_buffer("Logger is shutting down.")},
                             Sink::ASYNC_WRITE);
                sink_->flush();
            }
            queue_.clear(); // Ensure queue is explicitly cleared before exit (final safeguard)
            g_logger_state.store(LoggerState::Shutdown, std::memory_order_release);
            break;
        }
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
Logger::Logger() : pImpl(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

Logger &Logger::instance()
{
    // C++11 and later guarantee that the initialization of function-local
    // static variables is thread-safe. This is the modern, preferred way
    // to implement a singleton.
    static Logger instance;
    return instance;
}

bool Logger::lifecycle_initialized() noexcept
{
    return g_logger_state.load(std::memory_order_acquire) != LoggerState::Uninitialized;
}

bool Logger::set_console()
{
    if (!logger_is_loggable("Logger::set_console"))
        return false;
    try
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(SetSinkCommand{std::make_unique<ConsoleSink>(), promise});
        return future.get();
    }
    catch (const std::exception &e)
    {
        auto promise_err = std::make_shared<std::promise<bool>>();
        auto future_err = promise_err->get_future();
        pImpl->enqueue_command(SinkCreationErrorCommand{
            fmt::format("Failed to create ConsoleSink: {}", e.what()), promise_err});
        (void)future_err.get(); // Wait for it to be processed, but ignore result
    }
    return false;
}

bool Logger::set_logfile(const std::string &utf8_path)
{
    return set_logfile(utf8_path, true);
}

bool Logger::set_logfile(const std::string &utf8_path, bool use_flock)
{
    if (!logger_is_loggable("Logger::set_logfile"))
        return false;
    try
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(
            SetSinkCommand{std::make_unique<FileSink>(utf8_path, use_flock), promise});
        return future.get();
    }
    catch (const std::exception &e)
    {
        auto promise_err = std::make_shared<std::promise<bool>>();
        auto future_err = promise_err->get_future();
        pImpl->enqueue_command(SinkCreationErrorCommand{
            fmt::format("Failed to create FileSink: {}", e.what()), promise_err});
        (void)future_err.get();
    }
    return false;
}

bool Logger::set_rotating_logfile(const std::filesystem::path &base_filepath,
                                  size_t max_file_size_bytes, size_t max_backup_files,
                                  std::error_code &ec) noexcept
{
    // Call the full overload with flocking enabled by default.
    return set_rotating_logfile(base_filepath, max_file_size_bytes, max_backup_files, true, ec);
}

bool Logger::set_rotating_logfile(const std::filesystem::path &base_filepath,
                                  size_t max_file_size_bytes, size_t max_backup_files,
                                  bool use_flock, std::error_code &ec) noexcept
{
    if (!logger_is_loggable("Logger::set_rotating_logfile"))
    {
        ec = std::make_error_code(std::errc::not_supported);
        return false;
    }
    ec.clear();

    try
    {
        // 1. Normalize the path first. This provides a clean, absolute path.
        auto normalized_path = std::filesystem::absolute(base_filepath).lexically_normal();
        auto parent_dir = normalized_path.parent_path();

        // 2. Synchronous pre-flight checks on the directory.
        if (!parent_dir.empty())
        {
            if (!std::filesystem::exists(parent_dir))
            {
                std::filesystem::create_directories(parent_dir, ec);
                if (ec)
                {
                    return false;
                }
            }
            // 3. Check for writability using the new native helper.
            if (!check_directory_is_writable(parent_dir, ec))
            {
                return false;
            }
        }
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        // 4. Pre-flight checks passed, enqueue the command with the normalized path.
        pImpl->enqueue_command(
            SetSinkCommand{std::make_unique<RotatingFileSink>(normalized_path, max_file_size_bytes,
                                                              max_backup_files, use_flock),
                           promise});
        return future.get();
    }
    catch (const std::exception &e)
    {
        auto promise_err = std::make_shared<std::promise<bool>>();
        auto future_err = promise_err->get_future();
        ec = std::make_error_code(std::errc::invalid_argument);
        // Also enqueue an error message so it appears in the previous log sink if possible.
        pImpl->enqueue_command(SinkCreationErrorCommand{
            fmt::format("Failed to set rotating log file: {}", e.what()), promise_err});
        (void)future_err.get();
        return false;
    }
}

bool Logger::set_syslog(const char *ident, int option, int facility)
{
    if (!logger_is_loggable("Logger::set_syslog"))
        return false;
#if !defined(PLATFORM_WIN64)
    try
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(SetSinkCommand{
            std::make_unique<SyslogSink>(ident ? ident : "", option, facility), promise});
        return future.get();
    }
    catch (const std::exception &e)
    {
        auto promise_err = std::make_shared<std::promise<bool>>();
        auto future_err = promise_err->get_future();
        pImpl->enqueue_command(SinkCreationErrorCommand{
            fmt::format("Failed to create SyslogSink: {}", e.what()), promise_err});
        (void)future_err.get();
    }
#else
    (void)ident;
    (void)option;
    (void)facility;
#endif
    return false;
}

bool Logger::set_eventlog(const wchar_t *source_name)
{
    if (!logger_is_loggable("Logger::set_eventlog"))
        return false;
#ifdef PYLABHUB_PLATFORM_WIN64
    try
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(
            SetSinkCommand{std::make_unique<EventLogSink>(source_name), promise});
        return future.get();
    }
    catch (const std::exception &e)
    {
        auto promise_err = std::make_shared<std::promise<bool>>();
        auto future_err = promise_err->get_future();
        pImpl->enqueue_command(SinkCreationErrorCommand{
            fmt::format("Failed to create EventLogSink: {}", e.what()), promise_err});
        (void)future_err.get();
    }
#else
    (void)source_name;
#endif
    return false;
}

void Logger::shutdown()
{
    // Do not abort if called before init, just do nothing.
    if (!lifecycle_initialized())
    {
        return;
    }
    if (pImpl)
        pImpl->shutdown();
}

void Logger::flush()
{
    if (!logger_is_loggable("Logger::flush"))
        return;
    // This check is needed to prevent a deadlock where enqueue_command does nothing
    // because shutdown has started, and we wait on the future forever.
    if (pImpl->shutdown_requested_.load())
        return;
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    pImpl->enqueue_command(FlushCommand{promise});
    (void)future.get();
}

void Logger::set_level(Level lvl)
{
    if (!logger_is_loggable("Logger::set_level"))
        return;
    if (pImpl)
        pImpl->level_.store(lvl, std::memory_order_relaxed);
}

Logger::Level Logger::level() const
{
    if (!logger_is_loggable("Logger::level"))
        return Level::L_INFO;
    return pImpl ? pImpl->level_.load(std::memory_order_relaxed) : Level::L_INFO;
}

void Logger::set_max_queue_size(size_t max_size)
{
    if (!logger_is_loggable("Logger::set_max_queue_size"))
        return;
    if (pImpl)
        pImpl->m_max_queue_size = (max_size > 0) ? max_size : 1;
}

size_t Logger::get_max_queue_size() const
{
    if (!logger_is_loggable("Logger::get_max_queue_size"))
        return 0;
    return pImpl ? pImpl->m_max_queue_size : 0;
}

size_t Logger::get_total_dropped_since_sink_switch() const
{
    if (!logger_is_loggable("Logger::get_total_dropped_since_sink_switch"))
        return 0;
    return pImpl ? pImpl->m_total_dropped_since_sink_switch.load(std::memory_order_relaxed) : 0;
}

void Logger::set_write_error_callback(std::function<void(const std::string &)> cb)
{
    if (!logger_is_loggable("Logger::set_write_error_callback"))
        return;
    if (pImpl)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(SetErrorCallbackCommand{std::move(cb), promise});
        (void)future.get();
    }
}

void Logger::set_log_sink_messages_enabled(bool enabled)
{
    if (!logger_is_loggable("Logger::set_log_sink_messages_enabled"))
        return;
    if (pImpl)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(SetLogSinkMessagesCommand{enabled, promise});
        (void)future.get();
    }
}

bool Logger::should_log(Level lvl) const noexcept
{
    const auto state = g_logger_state.load(std::memory_order_acquire);
    if (state != LoggerState::Initialized)
        return false;

    return pImpl &&
           static_cast<int>(lvl) >= static_cast<int>(pImpl->level_.load(std::memory_order_relaxed));
}

bool Logger::enqueue_log(Level lvl, fmt::memory_buffer &&body) noexcept
{
    if (g_logger_state.load(std::memory_order_acquire) != LoggerState::Initialized)
        return false;
    if (pImpl)
    {
        return pImpl->enqueue_command(LogMessage{.timestamp = std::chrono::system_clock::now(),
                                                 .process_id = pylabhub::platform::get_pid(),
                                                 .thread_id =
                                                     pylabhub::platform::get_native_thread_id(),
                                                 .level = static_cast<int>(lvl),
                                                 .body = std::move(body)});
    }
    return false;
}

bool Logger::enqueue_log(Level lvl, std::string &&body_str) noexcept
{
    if (g_logger_state.load(std::memory_order_acquire) != LoggerState::Initialized)
        return false;
    if (pImpl)
    {
        return pImpl->enqueue_command(LogMessage{.timestamp = std::chrono::system_clock::now(),
                                                 .process_id = pylabhub::platform::get_pid(),
                                                 .thread_id =
                                                     pylabhub::platform::get_native_thread_id(),
                                                 .level = static_cast<int>(lvl),
                                                 .body = make_buffer("{}", std::move(body_str))});
    }
    return false;
}

bool Logger::write_sync(Level lvl, fmt::memory_buffer &&body) noexcept
{
    if (g_logger_state.load(std::memory_order_acquire) != LoggerState::Initialized)
        return false;
    if (pImpl)
    {
        std::lock_guard<std::mutex> sink_lock(pImpl->m_sink_mutex);
        if (pImpl->sink_ && static_cast<int>(lvl) >=
                                static_cast<int>(pImpl->level_.load(std::memory_order_relaxed)))
        {
            try
            {
                pImpl->sink_->write(
                    LogMessage{.timestamp = std::chrono::system_clock::now(),
                               .process_id = pylabhub::platform::get_pid(),
                               .thread_id = pylabhub::platform::get_native_thread_id(),
                               .level = static_cast<int>(lvl),
                               .body = std::move(body)},
                    Sink::SYNC_WRITE);
                return true;
            }
            catch (...)
            {
                // Cannot log here, as it could lead to infinite recursion.
                // Simply return false to indicate failure.
                return false;
            }
        }
    }
    return false;
}

// C-style callbacks for the ABI-safe lifecycle API.
// These functions are called by the LifecycleManager.
void do_logger_startup(const char *arg)
{
    (void)arg; // Argument not used by logger startup.
    Logger::instance().pImpl->start_worker();
    g_logger_state.store(LoggerState::Initialized, std::memory_order_release);
}

void do_logger_shutdown(const char *arg)
{
    (void)arg; // Argument not used by logger shutdown.
    LoggerState expected = LoggerState::Initialized;
    // Atomically change state from Initialized to ShuttingDown.
    // If it wasn't Initialized, another thread is already shutting it down, so we do nothing.
    if (g_logger_state.compare_exchange_strong(expected, LoggerState::ShuttingDown,
                                               std::memory_order_acq_rel))
    {
        Logger::instance().shutdown();
        // Logger should set g_logger_state to Shutdown when the worker thread exits.

        int count = 0;
        // Wait up to 5 seconds for shutdown to complete.
        while (count < 50 &&
               g_logger_state.load(std::memory_order_acquire) != LoggerState::Shutdown)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            count++;
        }
        if (g_logger_state.load(std::memory_order_acquire) != LoggerState::Shutdown)
        {
            PLH_DEBUG("Logger shutdown timed out. Forcing shutdown state.");
        }
        g_logger_state.store(LoggerState::Shutdown, std::memory_order_release);
    }
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
