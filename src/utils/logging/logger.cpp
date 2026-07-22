/*******************************************************************************
 * @file logger.cpp
 * @brief Implementation of the high-performance, asynchronous logger.
 ******************************************************************************/

#include "plh_platform.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>

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
typedef SSIZE_T ptrdiff_t;
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
enum class LoggerState : std::uint8_t
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
constexpr int kTempFileMode = 0600;

// Helper to synchronously check if a directory is writable.
bool check_directory_is_writable(const std::filesystem::path &dir, std::error_code &err_code)
{
    err_code.clear();
    try
    {
        auto temp_file_path = dir / fmt::format("pylabhub_write_check_{}.tmp",
                                                pylabhub::platform::monotonic_time_ns());

#ifdef PYLABHUB_PLATFORM_WIN64
        std::wstring wpath = pylabhub::format_tools::win32_to_long_path(temp_file_path);
        if (wpath.empty())
        {
            err_code = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }

        // Create a temporary file that is deleted immediately on close.
        HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            err_code = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            return false;
        }
        CloseHandle(h); // The file is deleted automatically on close.
#else
        int file_fd = ::open(temp_file_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, kTempFileMode);
        if (file_fd == -1)
        {
            err_code = std::error_code(errno, std::generic_category());
            return false;
        }
        ::close(file_fd);
        ::unlink(temp_file_path.c_str()); // Clean up the temporary file immediately.
#endif
        return true;
    }
    catch (...)
    {
        err_code = std::make_error_code(std::errc::invalid_argument);
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

    void post(std::function<void()> callback)
    {
        if (shutdown_requested_.load(std::memory_order_relaxed))
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock_guard(mutex_);
            queue_.push_back(std::move(callback));
        }
        cv_.notify_one();
    }

    void shutdown()
    {
        PLH_DEBUG("CallbackDispatcher::shutdown ENTER");
        if (shutdown_requested_.exchange(true))
        {
            PLH_DEBUG("CallbackDispatcher::shutdown REENTRY no-op");
            return;
        }
        PLH_DEBUG("CallbackDispatcher::shutdown notifying cv_");
        cv_.notify_one();
        if (worker_.joinable())
        {
            PLH_DEBUG("CallbackDispatcher::shutdown BEFORE worker_.join()");
            worker_.join();
            PLH_DEBUG("CallbackDispatcher::shutdown AFTER  worker_.join()");
        }
        else
        {
            PLH_DEBUG("CallbackDispatcher::shutdown worker_ not joinable");
        }
        PLH_DEBUG("CallbackDispatcher::shutdown EXIT");
    }

  private:
    void run()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return shutdown_requested_.load() || !queue_.empty(); });
                if (shutdown_requested_.load() && queue_.empty())
                {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try
            {
                task();
            }
            catch (
                ...) // NOLINT(bugprone-empty-catch) -- exceptions in user callbacks are swallowed
            {
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
template <typename T>
void promise_set_safe(const std::shared_ptr<std::promise<T>> &promise_ptr, T value)
{
    if (promise_ptr == nullptr)
    {
        return;
    }
    try
    {
        promise_ptr->set_value(std::move(value));
    }
    catch (...) // NOLINT(bugprone-empty-catch) -- Promise already satisfied or broken; swallow to
                // avoid std::terminate
    {
    }
}

template <typename T>
void promise_set_exception_safe(const std::shared_ptr<std::promise<T>> &promise_ptr,
                                std::exception_ptr exception_ptr)
{
    if (promise_ptr == nullptr)
    {
        return;
    }
    try
    {
        promise_ptr->set_exception(exception_ptr);
    }
    catch (...) // NOLINT(bugprone-empty-catch) -- swallow to avoid std::terminate
    {
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
    std::unique_ptr<Sink> sink_; // Size: pointer (8 bytes)
    static constexpr size_t kDefaultMaxQueueSize = 10000;
    std::atomic<size_t> m_max_queue_size{kDefaultMaxQueueSize};
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
    std::atomic<size_t> m_messages_dropped{
        0}; // Batch counter for summary; exchange(0) when processed
    std::atomic<size_t> m_total_dropped_since_sink_switch{
        0}; // Accumulated total; reset on sink switch
    // steady_clock::time_point::rep encoding of when Impl::shutdown()
    // was entered (0 == never).  Read by debug_dump_state_to_stderr()
    // from a signal handler / SIGTERM watchdog to report
    // "Logger has been shutting down for X ms".
    std::atomic<long long> shutdown_start_steady_{0};

  public:
    /// Async-signal-safe-ish state dump used by the SIGTERM watchdog
    /// in InteractiveSignalHandler.  Reads atomics directly, try_locks
    /// queue_mutex_, writes a single fmt::print(stderr,...) line.
    /// Safe to call when no Logger machinery is alive.
    void debug_dump_state_to_stderr(const char *trigger) noexcept;
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

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- keep as member for consistency
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
        const size_t max_queue_size_soft = m_max_queue_size.load(std::memory_order_relaxed);
        // Saturating multiply: if soft > SIZE_MAX/2 keep hard == SIZE_MAX to avoid wrap-around
        // that would make hard < soft and effectively disable dropping.
        const size_t max_queue_size_hard =
            (max_queue_size_soft <= std::numeric_limits<size_t>::max() / 2)
                ? max_queue_size_soft * 2
                : std::numeric_limits<size_t>::max();

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- command dispatch and shutdown
// handling
void Logger::Impl::worker_loop()
{
    PLH_DEBUG("Logger worker_loop ENTER tid={}",
              static_cast<unsigned long long>(pylabhub::platform::get_native_thread_id()));
    std::vector<Command> local_queue;
    size_t iter_n = 0;

    while (true)
    {
        ++iter_n;
        bool was_dropping = false;
        size_t dropped_count = 0;
        double dropping_duration_s = 0.0;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            PLH_DEBUG("Logger worker_loop iter={} cv_.wait queue_size={} "
                      "shutdown_req={}",
                      iter_n, queue_.size(), shutdown_requested_.load());
            cv_.wait(lock, [this] { return !queue_.empty() || shutdown_requested_.load(); });
            PLH_DEBUG("Logger worker_loop iter={} cv_.wait WOKE queue_size={} "
                      "shutdown_req={}",
                      iter_n, queue_.size(), shutdown_requested_.load());
            local_queue.swap(queue_);

            if (m_was_dropping.exchange(false, std::memory_order_relaxed))
            {
                was_dropping = true;
                dropped_count = m_messages_dropped.exchange(0, std::memory_order_relaxed);
                if (dropped_count > 0)
                {
                    dropping_duration_s = std::chrono::duration_cast<std::chrono::duration<double>>(
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
                                            "Overflow detected when processing the queue. Messages "
                                            "may have been dropped in the following batch.")},
                             /*sync_flag=*/false);
            }
        }

        // --- Find last SetSinkCommand ---
        ptrdiff_t last_set_sink_idx = -1;
        for (ptrdiff_t i = static_cast<ptrdiff_t>(local_queue.size()) - 1; i >= 0; --i)
        {
            if (std::holds_alternative<SetSinkCommand>(local_queue[i]))
            {
                last_set_sink_idx = i;
                break;
            }
        }

        // --- Process the dequeued batch ---
        // Per-message probes are SILENT in steady state and only spew
        // once shutdown_requested_ is set, so the normal hot path is
        // unchanged but the moment teardown begins we get
        // per-message-write granularity into where time goes.
        const bool trace_per_msg = shutdown_requested_.load(std::memory_order_relaxed);
        const ptrdiff_t batch_size = static_cast<ptrdiff_t>(local_queue.size());
        if (trace_per_msg)
        {
            PLH_DEBUG("Logger worker_loop iter={} shutdown-mode batch "
                      "size={}",
                      iter_n, batch_size);
        }
        for (ptrdiff_t i = 0; i < batch_size; ++i)
        {
            try
            {
                // Fast path: LogMessage
                if (auto *msg = std::get_if<LogMessage>(&local_queue[i]))
                {
                    if (trace_per_msg)
                    {
                        PLH_DEBUG("Logger worker_loop iter={} msg {}/{} "
                                  "BEFORE acquiring m_sink_mutex (lvl={})",
                                  iter_n, i + 1, batch_size, msg->level);
                    }
                    std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
                    if (trace_per_msg)
                    {
                        PLH_DEBUG("Logger worker_loop iter={} msg {}/{} "
                                  "acquired m_sink_mutex",
                                  iter_n, i + 1, batch_size);
                    }
                    if (sink_ &&
                        msg->level >= static_cast<int>(level_.load(std::memory_order_relaxed)))
                    {
                        if (trace_per_msg)
                        {
                            PLH_DEBUG("Logger worker_loop iter={} msg {}/{} "
                                      "BEFORE sink_->write",
                                      iter_n, i + 1, batch_size);
                        }
                        sink_->write(*msg, /*sync_flag=*/false);
                        if (trace_per_msg)
                        {
                            PLH_DEBUG("Logger worker_loop iter={} msg {}/{} "
                                      "AFTER  sink_->write",
                                      iter_n, i + 1, batch_size);
                        }
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
                            if (error_callback_ != nullptr)
                            {
                                auto error_cb = error_callback_;
                                callback_dispatcher_.post([error_cb, msg = arg.error_message]()
                                                          { error_cb(msg); });
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
                            {
                                std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
                                if (sink_)
                                {
                                    sink_->flush();
                                }
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
                if (error_callback_ != nullptr)
                {
                    auto error_cb = error_callback_;
                    auto msg = fmt::format("Logger worker error: {}", e.what());
                    callback_dispatcher_.post([error_cb, msg]() { error_cb(msg); });
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
                sink_->write(LogMessage{.timestamp = std::chrono::system_clock::now(),
                                        .process_id = pylabhub::platform::get_pid(),
                                        .thread_id = pylabhub::platform::get_native_thread_id(),
                                        .level = static_cast<int>(Logger::Level::L_WARNING),
                                        .body = make_buffer(
                                            "Summary: At this point in time, the Logger dropped {} "
                                            "messages over {:.2f}s due to full queue.",
                                            dropped_count, dropping_duration_s)},
                             /*sync_flag=*/false);
            }
        }

        if (last_set_sink_idx != -1)
        {
            auto &cmd_variant = local_queue[last_set_sink_idx];
            if (auto *sink_cmd = std::get_if<SetSinkCommand>(&cmd_variant))
            {
                PLH_DEBUG("Logger worker_loop iter={} SetSinkCommand "
                          "acquiring m_sink_mutex",
                          iter_n);
                std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
                PLH_DEBUG("Logger worker_loop iter={} SetSinkCommand "
                          "acquired m_sink_mutex",
                          iter_n);
                if (m_log_sink_messages_enabled_.load(std::memory_order_relaxed))
                {
                    std::string old_desc = sink_ ? sink_->description() : "null";
                    std::string new_desc =
                        sink_cmd->new_sink ? sink_cmd->new_sink->description() : "null";
                    PLH_DEBUG("Logger worker_loop iter={} sink switch "
                              "old='{}' new='{}'",
                              iter_n, old_desc, new_desc);
                    if (sink_)
                    {
                        PLH_DEBUG("Logger worker_loop iter={} "
                                  "BEFORE old_sink->write(switching msg)",
                                  iter_n);
                        sink_->write(
                            LogMessage{.timestamp = std::chrono::system_clock::now(),
                                       .process_id = pylabhub::platform::get_pid(),
                                       .thread_id = pylabhub::platform::get_native_thread_id(),
                                       .level = static_cast<int>(Logger::Level::L_SYSTEM),
                                       .body = make_buffer("Switching log sink to: {}", new_desc)},
                            /*sync_flag=*/false);
                        PLH_DEBUG("Logger worker_loop iter={} "
                                  "AFTER  old_sink->write(switching msg); "
                                  "BEFORE old_sink->flush()",
                                  iter_n);
                        sink_->flush();
                        PLH_DEBUG("Logger worker_loop iter={} "
                                  "AFTER  old_sink->flush()",
                                  iter_n);
                    }
                    m_total_dropped_since_sink_switch.store(0, std::memory_order_relaxed);
                    // Carve out the OLD sink so its destructor runs
                    // OUTSIDE the move and we can bracket it with
                    // probes.  Otherwise a slow flock release / fd
                    // close in the FileSink dtor would be invisible.
                    auto old_sink = std::move(sink_);
                    PLH_DEBUG("Logger worker_loop iter={} "
                              "BEFORE sink_ = move(new_sink) (carved out "
                              "old_sink for explicit destruction)",
                              iter_n);
                    sink_ = std::move(sink_cmd->new_sink);
                    PLH_DEBUG("Logger worker_loop iter={} "
                              "AFTER  sink_ = move(new_sink); "
                              "BEFORE old_sink.reset()",
                              iter_n);
                    old_sink.reset();
                    PLH_DEBUG("Logger worker_loop iter={} "
                              "AFTER  old_sink.reset()",
                              iter_n);
                    if (sink_)
                    {
                        PLH_DEBUG("Logger worker_loop iter={} "
                                  "BEFORE new_sink->write(switched-from msg)",
                                  iter_n);
                        sink_->write(
                            LogMessage{.timestamp = std::chrono::system_clock::now(),
                                       .process_id = pylabhub::platform::get_pid(),
                                       .thread_id = pylabhub::platform::get_native_thread_id(),
                                       .level = static_cast<int>(Logger::Level::L_SYSTEM),
                                       .body = make_buffer("Log sink switched from: {}", old_desc)},
                            /*sync_flag=*/false);
                        PLH_DEBUG("Logger worker_loop iter={} "
                                  "AFTER  new_sink->write(switched-from msg)",
                                  iter_n);
                    }
                }
                else
                {
                    m_total_dropped_since_sink_switch.store(0, std::memory_order_relaxed);
                    sink_ = std::move(sink_cmd->new_sink);
                }
                PLH_DEBUG("Logger worker_loop iter={} sink switch DONE; "
                          "fulfilling promise",
                          iter_n);
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

            PLH_DEBUG("Logger worker thread shutting down. iter={}", iter_n);
            PLH_DEBUG("Logger worker_loop iter={} shutdown branch "
                      "acquiring m_sink_mutex",
                      iter_n);
            std::lock_guard<std::mutex> sink_lock(m_sink_mutex);
            PLH_DEBUG("Logger worker_loop iter={} shutdown branch "
                      "acquired m_sink_mutex",
                      iter_n);
            if (sink_)
            {
                PLH_DEBUG("Logger worker_loop iter={} shutdown branch "
                          "BEFORE final sink_->write",
                          iter_n);
                sink_->write(LogMessage{.timestamp = std::chrono::system_clock::now(),
                                        .process_id = pylabhub::platform::get_pid(),
                                        .thread_id = pylabhub::platform::get_native_thread_id(),
                                        .level = static_cast<int>(Logger::Level::L_SYSTEM),
                                        .body = make_buffer("Logger is shutting down.")},
                             /*sync_flag=*/false);
                PLH_DEBUG("Logger worker_loop iter={} shutdown branch "
                          "AFTER  final sink_->write; BEFORE final flush",
                          iter_n);
                sink_->flush();
                PLH_DEBUG("Logger worker_loop iter={} shutdown branch "
                          "AFTER  final flush",
                          iter_n);
            }
            queue_.clear(); // Ensure queue is explicitly cleared before exit (final safeguard)
            g_logger_state.store(LoggerState::Shutdown, std::memory_order_release);
            PLH_DEBUG("Logger worker_loop iter={} BREAKING from while loop", iter_n);
            break;
        }
    }
    PLH_DEBUG("Logger worker_loop EXIT after iter={}", iter_n);
}

void Logger::Impl::shutdown()
{
    // Monotonic clock baseline for the shutdown sequence.  Every
    // shutdown-path PLH_DEBUG carries `[+X.XXXms]` from this point so
    // the next failure trace pinpoints which call ate the budget.
    // Also exposed via shutdown_start_steady_ so SIGTERM-time dumps
    // (interactive_signal_handler) can report wall-clock-since-shutdown.
    //
    // Guarded on PYLABHUB_ENABLE_DEBUG_MESSAGES so a Release build pays
    // ZERO cost here.  Side effect: in Release the SIGTERM watchdog
    // dump reports `shutdown_age_ms=-1.000` (the dump already returns
    // -1.0 when the atomic is 0); the rest of the dump's fields
    // (shutdown_req, worker_joinable, queue_size) still work in
    // Release.
#if defined(PYLABHUB_ENABLE_DEBUG_MESSAGES)
    const auto t0 = std::chrono::steady_clock::now();
    shutdown_start_steady_.store(t0.time_since_epoch().count(), std::memory_order_release);
    auto since_t0 = [t0]()
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - t0).count();
    };
#endif

    PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] ENTER tid={}", since_t0(),
              static_cast<unsigned long long>(pylabhub::platform::get_native_thread_id()));
    if (shutdown_completed_.load() || shutdown_requested_.exchange(true))
    {
        PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] REENTRY no-op "
                  "(completed={}, req_was_true={})",
                  since_t0(), shutdown_completed_.load(), shutdown_requested_.load());
        return;
    }
    PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] set shutdown_requested_; "
              "about to notify cv_",
              since_t0());
    cv_.notify_one();
    if (worker_thread_.joinable())
    {
        PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] before "
                  "worker_thread_.join()",
                  since_t0());
        worker_thread_.join();
        PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] after  "
                  "worker_thread_.join()",
                  since_t0());
    }
    else
    {
        PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] worker_thread_ not "
                  "joinable",
                  since_t0());
    }
    PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] before "
              "callback_dispatcher_.shutdown()",
              since_t0());
    callback_dispatcher_.shutdown();
    PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] after  "
              "callback_dispatcher_.shutdown()",
              since_t0());
    shutdown_completed_.store(true);
    PLH_DEBUG("Logger::Impl::shutdown [+{:.3f}ms] EXIT", since_t0());
}

void Logger::Impl::debug_dump_state_to_stderr(const char *trigger) noexcept
{
    // Read everything via atomics + try_lock so the dump itself cannot
    // hang.  Called from the SIGTERM watchdog (signal-handler watcher
    // thread, not a real signal handler), so we tolerate some
    // allocation but absolutely no blocking on contended locks.
    const auto start_rep = shutdown_start_steady_.load(std::memory_order_acquire);
    double elapsed_ms = -1.0;
    if (start_rep != 0)
    {
        const std::chrono::steady_clock::time_point start{
            std::chrono::steady_clock::duration{start_rep}};
        elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }

    bool queue_locked = false;
    size_t queue_size = 0;
    {
        std::unique_lock<std::mutex> lk(queue_mutex_, std::try_to_lock);
        if (lk.owns_lock())
        {
            queue_locked = true;
            queue_size = queue_.size();
        }
    }

    fmt::print(stderr,
               "[LOGGER_DUMP] trigger='{}' "
               "shutdown_req={} shutdown_done={} "
               "shutdown_age_ms={:.3f} "
               "worker_joinable={} "
               "queue_mutex_acquirable={} "
               "queue_size={}\n",
               trigger ? trigger : "(null)", shutdown_requested_.load(), shutdown_completed_.load(),
               elapsed_ms, worker_thread_.joinable(), queue_locked,
               queue_locked ? std::to_string(queue_size) : std::string("?(contended)"));
    std::fflush(stderr);
}

// Logger Public API Implementation
Logger::Logger() : pImpl(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

void Logger::debug_dump_state_to_stderr(const char *trigger) noexcept
{
    if (pImpl)
    {
        pImpl->debug_dump_state_to_stderr(trigger);
    }
}

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
    {
        return false;
    }
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
    {
        return false;
    }
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

// NOLINTNEXTLINE(bugprone-exception-escape) -- noexcept for API; internal throws reported via
// err_code
bool Logger::set_rotating_logfile(const std::filesystem::path &base_filepath,
                                  const RotatingLogConfig &cfg, std::error_code &err_code) noexcept
{
    if (!logger_is_loggable("Logger::set_rotating_logfile"))
    {
        err_code = std::make_error_code(std::errc::not_supported);
        return false;
    }
    err_code.clear();

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
                std::filesystem::create_directories(parent_dir, err_code);
                if (err_code)
                {
                    return false;
                }
            }
            // 3. Check for writability using the new native helper.
            if (!check_directory_is_writable(parent_dir, err_code))
            {
                return false;
            }
        }
        const auto mode = cfg.timestamped_names ? RotatingFileSink::Mode::Timestamped
                                                : RotatingFileSink::Mode::Numeric;

        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        // 4. Pre-flight checks passed, enqueue the command with the normalized path.
        pImpl->enqueue_command(SetSinkCommand{
            std::make_unique<RotatingFileSink>(normalized_path, cfg.max_file_size_bytes,
                                               cfg.max_backup_files, cfg.use_flock, mode),
            promise});
        return future.get();
    }
    catch (const std::exception &e)
    {
        auto promise_err = std::make_shared<std::promise<bool>>();
        auto future_err = promise_err->get_future();
        err_code = std::make_error_code(std::errc::invalid_argument);
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
    {
        return false;
    }
#if !defined(PYLABHUB_PLATFORM_WIN64)
    try
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(SetSinkCommand{
            std::make_unique<SyslogSink>(ident != nullptr ? ident : "", option, facility),
            promise});
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

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- keep as member for API
// consistency
bool Logger::set_eventlog(const wchar_t *source_name)
{
    if (!logger_is_loggable("Logger::set_eventlog"))
    {
        return false;
    }
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
    if (pImpl != nullptr)
    {
        pImpl->shutdown();
    }
}

void Logger::flush()
{
    if (!logger_is_loggable("Logger::flush"))
    {
        return;
    }
    // This check is needed to prevent a deadlock where enqueue_command does nothing
    // because shutdown has started, and we wait on the future forever.
    if (pImpl->shutdown_requested_.load())
    {
        return;
    }
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    pImpl->enqueue_command(FlushCommand{promise});
    (void)future.get();
}

void Logger::set_level(Level lvl)
{
    if (!logger_is_loggable("Logger::set_level"))
    {
        return;
    }
    if (pImpl != nullptr)
    {
        pImpl->level_.store(lvl, std::memory_order_relaxed);
    }
}

Logger::Level Logger::level() const
{
    if (!logger_is_loggable("Logger::level"))
    {
        return Level::L_INFO;
    }
    return pImpl ? pImpl->level_.load(std::memory_order_relaxed) : Level::L_INFO;
}

void Logger::set_max_queue_size(size_t max_size)
{
    if (!logger_is_loggable("Logger::set_max_queue_size"))
    {
        return;
    }
    if (pImpl != nullptr)
    {
        pImpl->m_max_queue_size.store((max_size > 0) ? max_size : 1, std::memory_order_relaxed);
    }
}

size_t Logger::get_max_queue_size() const
{
    if (!logger_is_loggable("Logger::get_max_queue_size"))
    {
        return 0;
    }
    return pImpl ? pImpl->m_max_queue_size.load(std::memory_order_relaxed) : 0;
}

size_t Logger::get_total_dropped_since_sink_switch() const
{
    if (!logger_is_loggable("Logger::get_total_dropped_since_sink_switch"))
    {
        return 0;
    }
    return pImpl ? pImpl->m_total_dropped_since_sink_switch.load(std::memory_order_relaxed) : 0;
}

void Logger::set_write_error_callback(std::function<void(const std::string &)> callback)
{
    if (!logger_is_loggable("Logger::set_write_error_callback"))
    {
        return;
    }
    if (pImpl != nullptr)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        auto future = promise->get_future();
        pImpl->enqueue_command(SetErrorCallbackCommand{std::move(callback), promise});
        (void)future.get();
    }
}

void Logger::set_log_sink_messages_enabled(bool enabled)
{
    if (!logger_is_loggable("Logger::set_log_sink_messages_enabled"))
    {
        return;
    }
    if (pImpl != nullptr)
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
    {
        return false;
    }

    return pImpl &&
           static_cast<int>(lvl) >= static_cast<int>(pImpl->level_.load(std::memory_order_relaxed));
}

// NOLINTNEXTLINE(bugprone-exception-escape) -- noexcept for API; internal throws are rare and
// documented
bool Logger::enqueue_log(Level lvl, fmt::memory_buffer &&body) noexcept
{
    if (g_logger_state.load(std::memory_order_acquire) != LoggerState::Initialized)
    {
        return false;
    }
    if (pImpl != nullptr)
    {
        return pImpl->enqueue_command(
            LogMessage{.timestamp = std::chrono::system_clock::now(),
                       .process_id = pylabhub::platform::get_pid(),
                       .thread_id = pylabhub::platform::get_native_thread_id(),
                       .level = static_cast<int>(lvl),
                       .body = std::move(body)});
    }
    return false;
}

// NOLINTNEXTLINE(bugprone-exception-escape) -- noexcept for API; internal throws are rare and
// documented
bool Logger::enqueue_log(Level lvl, std::string &&body_str) noexcept
{
    if (g_logger_state.load(std::memory_order_acquire) != LoggerState::Initialized)
    {
        return false;
    }
    if (pImpl != nullptr)
    {
        return pImpl->enqueue_command(
            LogMessage{.timestamp = std::chrono::system_clock::now(),
                       .process_id = pylabhub::platform::get_pid(),
                       .thread_id = pylabhub::platform::get_native_thread_id(),
                       .level = static_cast<int>(lvl),
                       .body = make_buffer("{}", std::move(body_str))});
    }
    return false;
}

bool Logger::write_sync(Level lvl, fmt::memory_buffer &&body) noexcept
{
    if (g_logger_state.load(std::memory_order_acquire) != LoggerState::Initialized)
    {
        return false;
    }
    if (pImpl != nullptr)
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
                    /*sync_flag=*/true);
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
void do_logger_startup(const char *arg, void * /*userdata*/)
{
    (void)arg; // Argument not used by logger startup.
    Logger::instance().pImpl->start_worker();
    g_logger_state.store(LoggerState::Initialized, std::memory_order_release);

    // Route lifecycle internal messages through the logger now that it is running.
    pylabhub::utils::SetLifecycleLogSink(
        [](pylabhub::utils::LifecycleLogLevel level, const std::string &msg)
        {
            switch (level)
            {
            case pylabhub::utils::LifecycleLogLevel::Error:
                LOGGER_ERROR("{}", msg);
                break;
            case pylabhub::utils::LifecycleLogLevel::Warn:
                LOGGER_WARN("{}", msg);
                break;
            default:
                LOGGER_DEBUG("{}", msg);
                break;
            }
        });
}

void do_logger_shutdown(const char *arg, void * /*userdata*/)
{
    (void)arg; // Argument not used by logger shutdown.
    // Remove the lifecycle log sink before the logger tears down so no lifecycle
    // message can be dispatched to a destroyed logger queue.
    pylabhub::utils::ClearLifecycleLogSink();
    LoggerState expected = LoggerState::Initialized;
    // Atomically change state from Initialized to ShuttingDown.
    // If it wasn't Initialized, another thread is already shutting it down, so we do nothing.
    if (g_logger_state.compare_exchange_strong(expected, LoggerState::ShuttingDown,
                                               std::memory_order_acq_rel))
    {
        Logger::instance().shutdown();
        // Logger should set g_logger_state to Shutdown when the worker thread exits.
        //
        // In practice this loop should never execute even once: Impl::shutdown() calls
        // worker_thread_.join(), which blocks until the worker exits. The worker stores
        // Shutdown immediately before exiting (logger.cpp worker_loop). Therefore
        // g_logger_state is already Shutdown when shutdown() returns and the while
        // condition is false on the very first evaluation. The loop is retained as a
        // defensive safety net for any future code path where join() could return without
        // the worker having stored Shutdown.
        constexpr int kShutdownPollIterations = 50;
        constexpr int kShutdownPollIntervalMs = 100;
        int count = 0;
        while (count < kShutdownPollIterations &&
               g_logger_state.load(std::memory_order_acquire) != LoggerState::Shutdown)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPollIntervalMs));
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
    // Lifecycle shutdown timeout: must be >= kShutdownPollIterations * kShutdownPollIntervalMs
    // (50 * 100 ms = 5 000 ms) to allow do_logger_shutdown() to observe the worker exit.
    // Logger cannot depend on pylabhub::utils headers, so this constant is defined locally.
    constexpr auto kLoggerQueueDrainTimeoutMs = std::chrono::milliseconds(5000);

    ModuleDef module("pylabhub::utils::Logger");
    // Using the no-argument overloads now.
    module.set_startup(&do_logger_startup);
    module.set_shutdown(&do_logger_shutdown, kLoggerQueueDrainTimeoutMs);
    return module;
}

ModuleDef Logger::GetStartupLogFileSinkModule(const std::string &log_file_path,
                                              std::optional<RotatingLogConfig> rotating)
{
    ModuleDef sink_mod("StartupLogFileSink");
    sink_mod.add_dependency("pylabhub::utils::Logger");

    if (rotating)
    {
        // Encode rotation parameters into the callback arg string as
        // "path|max_size|max_backups|timestamped" since LifecycleCallback
        // only accepts a single const char* argument.
        const std::string encoded_arg = log_file_path + '|' +
                                        std::to_string(rotating->max_file_size_bytes) + '|' +
                                        std::to_string(rotating->max_backup_files) + '|' +
                                        (rotating->timestamped_names ? "t" : "n");
        sink_mod.set_startup(
            [](const char *arg, void * /*userdata*/)
            {
                if (arg == nullptr || arg[0] == '\0')
                {
                    return;
                }

                // Parse "path|max_size|max_backups|timestamped" — the
                // 4-field form is the only producer (GetStartupLogFileSinkModule
                // itself). Any other shape is a bug in the producer.
                const std::string encoded(arg);
                const auto sep1 = encoded.find('|');
                const auto sep2 =
                    (sep1 == std::string::npos) ? std::string::npos : encoded.find('|', sep1 + 1);
                const auto sep3 =
                    (sep2 == std::string::npos) ? std::string::npos : encoded.find('|', sep2 + 1);
                if (sep1 == std::string::npos || sep2 == std::string::npos ||
                    sep3 == std::string::npos)
                {
                    std::fprintf(stderr,
                                 "WARNING: StartupLogFileSink: malformed rotating "
                                 "logfile arg '%s'\n",
                                 arg);
                    return;
                }

                const std::string path = encoded.substr(0, sep1);
                Logger::RotatingLogConfig cfg;
                cfg.max_file_size_bytes = std::stoull(encoded.substr(sep1 + 1, sep2 - sep1 - 1));
                cfg.max_backup_files = std::stoull(encoded.substr(sep2 + 1, sep3 - sep2 - 1));
                cfg.timestamped_names = (encoded.substr(sep3 + 1) == "t");

                std::error_code ec;
                if (!Logger::instance().set_rotating_logfile(path, cfg, ec))
                {
                    std::fprintf(stderr,
                                 "WARNING: failed to open rotating log file '%s': %s, "
                                 "falling back to console\n",
                                 path.c_str(), ec.message().c_str());
                }
            },
            encoded_arg);
    }
    else
    {
        // Plain append-mode log file.
        sink_mod.set_startup(
            [](const char *path, void * /*userdata*/)
            {
                if (path != nullptr && path[0] != '\0')
                {
                    if (!Logger::instance().set_logfile(path))
                    {
                        std::fprintf(stderr,
                                     "WARNING: failed to open log file '%s', "
                                     "falling back to console\n",
                                     path);
                    }
                }
            },
            log_file_path);
    }

    return sink_mod;
}

} // namespace pylabhub::utils
