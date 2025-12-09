// Logger.cpp
//
// Implementation of Logger using an asynchronous worker thread.
//
// Design:
// - Logging calls from application threads are non-blocking. They format the message
//   and push it into a thread-safe queue.
// - A dedicated background worker thread pulls messages from the queue and performs
//   all I/O operations (writing to file, console, etc.).
// - This decouples application performance from I/O latency.

#include "utils/Logger.hpp"
#include <fmt/chrono.h> // For fmt::localtime

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h> // For fmt::print to FILE*

#if defined(PLATFORM_WIN64)
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pylabhub::utils
{

// A struct to hold all necessary information for a log message, passed to the
// worker thread. This is necessary for sinks like syslog that need the original
// log level and the message body separately.
struct LogMessage
{
    Logger::Level level;
    std::string message; // The fully formatted message, without newline
};
// The private implementation of the Logger.
struct Impl
{
    Impl();
    ~Impl();
    void record_write_error(int errcode, const char *msg) noexcept;
    void worker_loop();
    void close_sinks() noexcept;
    std::string get_sink_description() const;

    // Asynchronous worker components
    std::atomic<bool> done{false};
    std::thread worker_thread;
    // Mutex and CV to protect the message queue and signal the worker thread.
    std::mutex queue_mtx;
    std::condition_variable cv;
    std::vector<LogMessage> queue;

    // Flush mechanism
    std::mutex flush_mtx;
    std::condition_variable flush_cv;
    std::atomic<bool> flush_requested{false};

    // Mutex to protect configuration state (destination, file handles, etc.).
    // This is locked by public configuration methods and by the worker thread
    // before performing I/O, preventing race conditions if a sink is changed
    // while a write is in progress.
    std::mutex mtx;

    Logger::Destination dest = Logger::Destination::L_CONSOLE;

    // file sink
#if defined(PLATFORM_WIN64)
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    HANDLE evt_handle = nullptr;
#else
    int file_fd = -1;
#endif
    std::string file_path;
    bool use_flock = false;

    // configuration + diagnostics
    std::atomic<int> level{static_cast<int>(Logger::Level::L_DEBUG)};
    std::atomic<bool> fsync_per_write{false};

    std::atomic<size_t> max_log_line_length{256 * 1024};

    std::atomic<int> last_errno{0};
    std::atomic<int> write_failure_count{0};
    std::atomic<int> last_write_errcode{0};
    std::string last_write_errmsg;

    // Timestamp for the last internal warning written to stderr, used to
    // rate-limit such warnings to avoid flooding the console.
    std::chrono::steady_clock::time_point last_stderr_notice =
        std::chrono::steady_clock::now() - std::chrono::seconds(10);

    std::function<void(const std::string &)> write_error_callback;
};

Impl::Impl()
{
    // Start the background worker thread upon construction.
    worker_thread = std::thread(&Impl::worker_loop, this);

    // Reuse the flush mechanism to perform a startup synchronization.
    // This ensures the constructor waits until the worker thread is fully running
    // and ready to process messages.
    {
        std::unique_lock<std::mutex> lk(flush_mtx);
        // Set flush_requested to true regardless of queue state.
        flush_requested.store(true, std::memory_order_release);

        // Notify the worker. It will wake up immediately because the flush_requested
        // flag satisfies its wait condition.
        cv.notify_one();

        // Wait until the worker has started, run through its loop once (with an
        // empty queue), and cleared the flush_requested flag.
        flush_cv.wait(lk, [this] { return !flush_requested.load(); });
    }

#ifdef _LOGGER_DEBUG_ENABLED
    fmt::print(stdout, "Log worker thread created and ready. C++ thread id: {}\n",
               worker_thread.get_id());
    fflush(stdout);
#endif
}

Impl::~Impl()
{
#ifdef _LOGGER_DEBUG_ENABLED
    fmt::print(stdout, "Logger Impl destructor called. Shutting down worker thread.\n");
    fflush(stdout);
#endif
    // In case shutdown() was not called explicitly, ensure we still shut down gracefully.
    if (!done.load(std::memory_order_relaxed) && worker_thread.joinable())
    {
        // To avoid calling the public flush() from a destructor, we inline a simplified
        // version: just signal the worker and let it drain the queue before it exits.
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            done.store(true);
        }
        cv.notify_one();
        worker_thread.join();
    }
    else if (worker_thread.joinable())
    {
        // If done was already true, just make sure we join.
        worker_thread.join();
    }
}

std::string Impl::get_sink_description() const
{
    switch (dest)
    {
    case Logger::Destination::L_CONSOLE:
        return "Console";
    case Logger::Destination::L_FILE:
        return fmt::format("File: {}", file_path);
    case Logger::Destination::L_SYSLOG:
        return "Syslog";
    case Logger::Destination::L_EVENTLOG:
        return "Windows Event Log";
    default:
        return "Unknown";
    }
}

void Impl::close_sinks() noexcept
{
#if defined(PLATFORM_WIN64)
    if (this->file_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(this->file_handle);
        this->file_handle = INVALID_HANDLE_VALUE;
    }
    if (this->evt_handle)
    {
        DeregisterEventSource(this->evt_handle);
        this->evt_handle = nullptr;
    }
#else
    if (this->file_fd != -1)
    {
        ::close(this->file_fd);
        this->file_fd = -1;
        this->file_path.clear();
    }
    // closelog is safe even if not previously opened
    closelog();
#endif
    this->dest = Logger::Destination::L_CONSOLE;
}

// --- Singleton Impl management ---
// This ensures that even if the Logger object is instantiated multiple times
// (e.g., due to static linking shenanigans), they all share a single Impl instance
// with a single worker thread. This pattern is thread-safe and robust across
// most module boundaries if the logger is part of a shared library.
static std::shared_ptr<Impl> g_impl_instance;
static std::once_flag g_impl_once_flag;

static std::shared_ptr<Impl> get_impl_instance()
{
    // This function is the sole entry point for creating the shared Impl singleton.
    // `std::call_once` guarantees that the lambda is executed exactly once per
    // process, even when called concurrently from multiple threads. This is crucial
    // for ensuring that all `Logger` instances, potentially created in different
    // modules of a larger application, share a single logging backend.
    std::call_once(g_impl_once_flag, []() { g_impl_instance = std::make_shared<Impl>(); });
    return g_impl_instance;
}

// Forward declaration for the actual I/O logic, now used by the worker.
static void do_write(Impl *pImpl, LogMessage &&msg);

// Helper: get a platform-native thread id
static uint64_t get_native_thread_id() noexcept
{
#if defined(PLATFORM_WIN64)
    return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(PLATFORM_APPLE)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid ? tid : std::hash<std::thread::id>()(std::this_thread::get_id());
#elif defined(__linux__)
    // try syscall to get kernel thread id
    long tid = syscall(SYS_gettid);
    if (tid > 0)
        return static_cast<uint64_t>(tid);
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#else
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

// Helper: formatted local time with sub-second resolution
static std::string formatted_time()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    // Use {fmt} to format the time portably. {:%Y-%m-%d %H:%M:%S.%f} provides
    // microsecond precision.
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", now);
}

static const char *level_to_string(Logger::Level lvl) noexcept
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
    default:
        return "UNK";
    }
}

#if !defined(PLATFORM_WIN64)

// This function is already inside the top-level `pylabhub::utils` namespace.
// The redundant nested namespace block was removed to fix name lookup issues.
static int level_to_syslog_priority(Logger::Level lvl) noexcept
{
    switch (lvl)
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
    default:
        return LOG_INFO;
    }
}

#endif

// ------------------------ Logger implementation ------------------------

Logger::Logger() : pImpl(get_impl_instance()) {}

// The destructor must be defined in the .cpp file where Impl is a complete type.
// The default destructor in the header would not work with std::shared_ptr<Impl>
// because Impl is an incomplete type there.
Logger::~Logger()
{
    // The shared_ptr will correctly manage the lifetime of the Impl instance.
    // When the last Logger handle is destroyed, the Impl destructor will be called.
}

Logger &Logger::instance()
{
    static Logger inst;
    return inst;
}

// ---- small accessors used by header-only templates ----
bool Logger::should_log(Level lvl) const noexcept
{
    if (!pImpl)
        return false;

    bool result = static_cast<int>(lvl) >= pImpl->level.load(std::memory_order_relaxed);
    return result;
}

size_t Logger::max_log_line_length() const noexcept
{
    if (!pImpl)
        return 0;
    return pImpl->max_log_line_length.load(std::memory_order_relaxed);
}

// ---- lifecycle / configuration ----
void Logger::set_level(Logger::Level lvl)
{
    if (!pImpl)
        return;
    pImpl->level.store(static_cast<int>(lvl), std::memory_order_relaxed);
}
Logger::Level Logger::level() const
{
    if (!pImpl)
        return Logger::Level::L_INFO;
    return static_cast<Logger::Level>(pImpl->level.load(std::memory_order_relaxed));
}
void Logger::set_fsync_per_write(bool v)
{
    if (!pImpl)
        return;
    pImpl->fsync_per_write.store(v);
}
void Logger::set_write_error_callback(std::function<void(const std::string &)> cb)
{
    if (!pImpl)
        return;
    std::lock_guard<std::mutex> g(pImpl->mtx);
    pImpl->write_error_callback = std::move(cb);
}

int Logger::last_errno() const
{
    if (!pImpl)
        return 0;
    return pImpl->last_errno.load();
}
int Logger::last_write_error_code() const
{
    if (!pImpl)
        return 0;
    return pImpl->last_write_errcode.load();
}
std::string Logger::last_write_error_message() const
{
    if (!pImpl)
        return std::string();
    std::lock_guard<std::mutex> g(pImpl->mtx);
    return pImpl->last_write_errmsg;
}
int Logger::write_failure_count() const
{
    if (!pImpl)
        return 0;
    return pImpl->write_failure_count.load();
}

void Logger::set_max_log_line_length(size_t bytes)
{
    if (!pImpl)
        return;
    pImpl->max_log_line_length.store(bytes ? bytes : 1);
}

// ---- sinks initialization ----
bool Logger::set_logfile(const std::string &utf8_path, bool use_flock, int mode)
{
    if (!pImpl)
        return false;

    // Log the switch to the old sink before changing.
    const std::string old_sink_desc = pImpl->get_sink_description();
    LOGGER_ERROR_RT("Switching log destination from {} to file: {}", old_sink_desc, utf8_path);
    flush();

    // Now, perform the switch under a lock.
    bool success = false;
    {
        std::lock_guard<std::mutex> g(pImpl->mtx);
        pImpl->close_sinks(); // Close any previously opened sink.

#if defined(PLATFORM_WIN64)
        // Mark parameter as unused on Windows.
        (void)mode;
        int needed = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
        if (needed == 0)
        {
            pImpl->record_write_error(GetLastError(), "MultiByteToWideChar failed in set_logfile");
        }
        else
        {
            std::wstring wpath(needed, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, &wpath[0], needed);
            if (!wpath.empty() && wpath.back() == L'\0')
                wpath.pop_back();

            HANDLE h = CreateFileW(wpath.c_str(), FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                pImpl->record_write_error(GetLastError(), "CreateFileW failed in set_logfile");
            }
            else
            {
                SetFilePointer(h, 0, nullptr, FILE_END);
                pImpl->file_handle = h;
                pImpl->file_path = utf8_path;
                pImpl->use_flock = use_flock;
                pImpl->dest = Logger::Destination::L_FILE;
                success = true;
            }
        }
#else
        int fd =
            ::open(utf8_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, static_cast<mode_t>(mode));
        if (fd == -1)
        {
            pImpl->record_write_error(errno, "open() failed in set_logfile");
        }
        else
        {
            pImpl->file_fd = fd;
            pImpl->file_path = utf8_path;
            pImpl->use_flock = use_flock;
            pImpl->dest = Logger::Destination::L_FILE;
            success = true;
        }
#endif
        if (!success)
        {
            // Revert to console logging on failure.
            pImpl->dest = Logger::Destination::L_CONSOLE;
        }
    } // Mutex is released here.

    // Log the outcome to the new sink (or console if it failed).
    if (success)
    {
        LOGGER_ERROR_RT("Logging redirected from {}", old_sink_desc);
    }
    else
    {
        LOGGER_ERROR_RT("Failed to switch logging to file: {}. Reverting to console.", utf8_path);
    }

    return success;
}

void Logger::set_syslog(const char *ident, int option, int facility)
{
#if !defined(PLATFORM_WIN64)
    if (!pImpl)
        return;

    const std::string old_sink_desc = pImpl->get_sink_description();
    LOGGER_ERROR_RT("Switching log destination from {} to syslog", old_sink_desc);
    flush();

    {
        std::lock_guard<std::mutex> g(pImpl->mtx);
        pImpl->close_sinks();
        openlog(ident ? ident : "app", option ? option : (LOG_PID | LOG_CONS),
                facility ? facility : LOG_USER);
        pImpl->dest = Logger::Destination::L_SYSLOG;
    }

    LOGGER_ERROR_RT("Logging redirected from {}", old_sink_desc);

#else
    (void)ident;
    (void)option;
    (void)facility;
#endif
}

bool Logger::set_eventlog(const wchar_t *source_name)
{
#if defined(PLATFORM_WIN64)
    if (!pImpl)
        return false;

    const std::string old_sink_desc = pImpl->get_sink_description();
    LOGGER_ERROR_RT("Switching log destination from {} to Windows Event Log", old_sink_desc);
    flush();

    bool success = false;
    {
        std::lock_guard<std::mutex> g(pImpl->mtx);
        pImpl->close_sinks();
        HANDLE h = RegisterEventSourceW(nullptr, source_name);
        if (!h)
        {
            pImpl->record_write_error(GetLastError(), "RegisterEventSourceW failed");
            pImpl->dest = Logger::Destination::L_CONSOLE;
        }
        else
        {
            pImpl->evt_handle = h;
            pImpl->dest = Logger::Destination::L_EVENTLOG;
            success = true;
        }
    }

    if (success)
    {
        LOGGER_ERROR_RT("Logging redirected from {}", old_sink_desc);
    }
    else
    {
        LOGGER_ERROR("Failed to switch to Windows Event Log. Reverting to console.");
    }
    return success;
#else
    (void)source_name;
    return false;
#endif
}

void Logger::set_destination(Logger::Destination dest)
{
    if (!pImpl)
        return;
    std::lock_guard<std::mutex> g(pImpl->mtx);
    pImpl->dest = dest;
}

void Logger::shutdown()
{
    if (!pImpl || pImpl->done.load(std::memory_order_relaxed))
    {
        return;
    }

    // Log the shutdown event itself. This will be the last message queued.
    LOGGER_ERROR("Logger shutting down upon explicit request.");

    // It is important to flush any pending messages before signaling shutdown.
    flush();

    if (pImpl->worker_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(pImpl->queue_mtx);
            pImpl->done.store(true);
        }
        pImpl->cv.notify_one();
        pImpl->worker_thread.join();
    }
}

void Logger::flush() noexcept
{
    if (!pImpl)
        return;

    // A lock is needed to wait on the condition variable.
    std::unique_lock<std::mutex> lk(pImpl->flush_mtx);
    {
        std::lock_guard<std::mutex> qlk(pImpl->queue_mtx);
        // If the queue is already empty, no need to signal the worker and wait.
        if (pImpl->queue.empty())
        {
            return;
        }
        pImpl->flush_requested.store(true, std::memory_order_release);
    }

    // Notify the worker that a flush has been requested.
    pImpl->cv.notify_one();

    // Wait until the worker thread signals that it has finished processing
    // the queue and has reset the flush_requested flag.
    pImpl->flush_cv.wait(lk, [this] { return !pImpl->flush_requested.load(); });
}

bool Logger::dirty() const noexcept
{
    if (!pImpl)
        return false;
    std::lock_guard<std::mutex> lk(pImpl->queue_mtx);
    return !pImpl->queue.empty();
}

// ---- Non-blocking write sink ----
// This function is called by the application threads. It formats the final
// message and pushes it to the worker queue, then returns immediately.
void Logger::write_formatted(Level lvl, std::string &&body) noexcept
{
    if (!pImpl || pImpl->done.load(std::memory_order_relaxed))
        return;

    try
    {
        // Construct the full log message with timestamp, level, thread id.
        std::string prefix = fmt::format("{} [{}] [tid={}] ", formatted_time(),
                                         level_to_string(lvl), get_native_thread_id());
        std::string full_message = prefix + body;

        // Push the formatted message to the queue for the worker thread.
        {
            std::lock_guard<std::mutex> lk(pImpl->queue_mtx);
            pImpl->queue.emplace_back(LogMessage{lvl, std::move(full_message)});
        }
        pImpl->cv.notify_one(); // Wake up the worker thread.
    }
    catch (const std::exception &ex)
    {
        // This catch block is a safeguard. With the corrected formatting logic,
        // exceptions should be rare, but we must honor the noexcept contract.
        // We can't log the error using the logger itself, so we write to stderr.
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> g(pImpl->mtx);
        if (now - pImpl->last_stderr_notice > std::chrono::seconds(5))
        {
            pImpl->last_stderr_notice = now;
            fmt::print(stderr, "INTERNAL LOGGER ERROR in write_formatted: {}\n", ex.what());
        }
    }
    catch (...)
    {
        // Similar to above, for non-standard exceptions.
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> g(pImpl->mtx);
        if (now - pImpl->last_stderr_notice > std::chrono::seconds(5))
        {
            pImpl->last_stderr_notice = now;
            fmt::print(stderr, "UNKNOWN INTERNAL LOGGER ERROR in write_formatted\n");
        }
    }
}

// The main loop for the background worker thread.
void Impl::worker_loop()
{
#ifdef _LOGGER_DEBUG_ENABLED
    fmt::print(stdout, "Log worker loop started. Native thread id: {}\n", get_native_thread_id());
    fflush(stdout);
#endif
    std::vector<LogMessage> write_batch;
    while (true)
    {
        {
            std::unique_lock<std::mutex> lk(queue_mtx);
            // Wait until the queue has messages, shutdown is requested, or a flush is requested.
            cv.wait(lk, [this] { return done || !queue.empty() || flush_requested.load(); });

            if (done && queue.empty())
            {
                break;
            }
            write_batch.swap(queue);
        }

        if (!write_batch.empty())
        {
            for (auto &msg : write_batch)
            {
                do_write(this, std::move(msg));
            }
            write_batch.clear();
        }

        // If a flush was requested and we've emptied the queue, notify the flushing thread.
        if (flush_requested.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            if (queue.empty())
            {
                flush_requested.store(false, std::memory_order_release);
                // The waiting thread holds the flush_mtx, but we can notify without it.
                flush_cv.notify_all();
            }
        }
    }

    // After exiting the loop, ensure all sinks are properly closed.
    std::lock_guard<std::mutex> g(mtx);
    this->close_sinks();
#ifdef _LOGGER_DEBUG_ENABLED
    fmt::print(stdout, "Log worker loop exiting.\n");
    fflush(stdout);
#endif
}

// Internal helper for writing to console (stderr)
static void write_to_console_internal(Impl *pImpl, const std::string &full_ln,
                                      std::unique_lock<std::mutex> &lk)
{
    try
    {
        fmt::print(stderr, FMT_STRING("{}"), full_ln);
        fflush(stderr);
    }
    catch (const std::system_error &e)
    {
        lk.unlock(); // Unlock before calling back.
        pImpl->record_write_error(e.code().value(), e.what());
    }
}

// Internal helper for writing to a file on POSIX systems
#if !defined(PLATFORM_WIN64)
static void write_to_file_internal(Impl *pImpl, const std::string &full_ln,
                                   std::unique_lock<std::mutex> &lk)
{
    if (pImpl->file_fd != -1)
    {
        if (pImpl->use_flock)
            flock(pImpl->file_fd, LOCK_EX);
        ssize_t total = static_cast<ssize_t>(full_ln.size());
        ssize_t off = 0;
        const char *data = full_ln.data();
        while (off < total)
        {
            ssize_t w = ::write(pImpl->file_fd, data + off, static_cast<size_t>(total - off));
            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                int err = errno;
                if (pImpl->use_flock)
                    flock(pImpl->file_fd, LOCK_UN);
                lk.unlock();
                pImpl->record_write_error(err, "write() failed");
                return;
            }
            off += w;
        }

        if (pImpl->fsync_per_write.load())
        {
            if (::fsync(pImpl->file_fd) != 0)
            {
                int err = errno;
                if (pImpl->use_flock)
                    flock(pImpl->file_fd, LOCK_UN);
                lk.unlock();
                pImpl->record_write_error(err, "fsync failed");
                return;
            }
        }
        if (pImpl->use_flock)
            flock(pImpl->file_fd, LOCK_UN);
    }
    else
    {
        size_t wrote = fwrite(full_ln.data(), 1, full_ln.size(), stderr);
        if (wrote != full_ln.size())
        {
            lk.unlock();
            pImpl->record_write_error(errno, "fwrite to stderr failed (file fallback)");
        }
        else
        {
            fflush(stderr);
        }
    }
}
#endif

// Internal helper for writing to a file on Windows systems
#if defined(PLATFORM_WIN64)
static void write_to_file_internal(Impl *pImpl, const std::string &full_ln,
                                   std::unique_lock<std::mutex> &lk)
{
    if (pImpl->file_handle != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        BOOL ok = WriteFile(pImpl->file_handle, full_ln.data(), static_cast<DWORD>(full_ln.size()),
                            &written, nullptr);
        if (!ok || written != full_ln.size())
        {
            lk.unlock();
            pImpl->record_write_error(static_cast<int>(GetLastError()), "WriteFile failed");
        }
        else if (pImpl->fsync_per_write.load())
        {
            if (!FlushFileBuffers(pImpl->file_handle))
            {
                lk.unlock();
                pImpl->record_write_error(static_cast<int>(GetLastError()),
                                          "FlushFileBuffers failed");
            }
        }
    }
    else
    {
        // fallback to OutputDebugString if no file handle
        std::wstring w = [](const std::string &s) -> std::wstring
        {
            if (s.empty())
                return {};
            int needed =
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            if (needed <= 0)
                return {};
            std::wstring out(needed, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], needed);
            return out;
        }(full_ln);
        OutputDebugStringW(w.c_str());
    }
}
#endif

// Internal helper for writing to syslog
#if !defined(PLATFORM_WIN64)
static void write_to_syslog_internal(Impl *pImpl, LogMessage &&msg,
                                     std::unique_lock<std::mutex> &lk)
{
    syslog(level_to_syslog_priority(msg.level), "%s", msg.message.c_str());
    (void)pImpl; // Unused in this path, but consistent signature
    (void)lk;    // Unused in this path, but consistent signature
}
#endif

// Internal helper for writing to eventlog on Windows
#if defined(PLATFORM_WIN64)
static void write_to_eventlog_internal(Impl *pImpl, const std::string &full_ln, LogMessage &&msg,
                                       std::unique_lock<std::mutex> &lk)
{
    if (pImpl->evt_handle)
    {
        std::wstring wmsg;
        {
            // convert utf8 to wstring
            int needed = MultiByteToWideChar(CP_UTF8, 0, msg.message.c_str(), -1, nullptr, 0);
            if (needed > 0)
            {
                wmsg.resize(needed);
                MultiByteToWideChar(CP_UTF8, 0, msg.message.c_str(), -1, &wmsg[0], needed);
                if (!wmsg.empty() && wmsg.back() == L'\0')
                    wmsg.pop_back();
            }
        }
        LPCWSTR strings[1] = {wmsg.c_str()};
        if (!ReportEventW(pImpl->evt_handle, EVENTLOG_INFORMATION_TYPE, 0, 0, nullptr, 1, 0,
                          strings, nullptr))
        {
            lk.unlock();
            pImpl->record_write_error(static_cast<int>(GetLastError()), "ReportEventW failed");
        }
    }
    else
    {
        // Fallback to OutputDebugString if no event handle
        std::wstring w;
        int needed = MultiByteToWideChar(CP_UTF8, 0, full_ln.c_str(), -1, nullptr, 0);
        if (needed > 0)
        {
            w.resize(needed);
            MultiByteToWideChar(CP_UTF8, 0, full_ln.c_str(), -1, &w[0], needed);
            if (!w.empty() && w.back() == L'\0')
                w.pop_back();
        }
        OutputDebugStringW(w.c_str());
    }
}
#endif

// This function contains the original synchronous I/O logic, now dispatching to helpers.
// It is called exclusively by the worker thread.
static void do_write(Impl *pImpl, LogMessage &&msg)
{
    // The worker thread is the only writer, but we still need to lock
    // to protect sink handles during reconfiguration (e.g., set_destination).
    std::unique_lock<std::mutex> lk(pImpl->mtx);

    const std::string full_ln =
        msg.message + "\n"; // Only needed for console/file/eventlog fallback

    switch (pImpl->dest)
    {
    case Logger::Destination::L_CONSOLE:
        write_to_console_internal(pImpl, full_ln, lk);
        break;
    case Logger::Destination::L_FILE:
#if defined(PLATFORM_WIN64)
        write_to_file_internal(pImpl, full_ln, lk);
#else
        write_to_file_internal(pImpl, full_ln, lk);
#endif
        break;
    case Logger::Destination::L_SYSLOG:
#if !defined(PLATFORM_WIN64)
        write_to_syslog_internal(pImpl, std::move(msg), lk);
#else
    {
        // Fallback to debug output on Windows
        std::wstring w = [](const std::string &s) -> std::wstring
        {
            if (s.empty())
                return {};
            int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            if (needed <= 0)
                return {};
            std::wstring out(needed, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], needed);
            return out;
        }(msg.message);
        OutputDebugStringW(w.c_str());
    }
#endif
        break;
    case Logger::Destination::L_EVENTLOG:
#if defined(PLATFORM_WIN64)
        write_to_eventlog_internal(pImpl, full_ln, std::move(msg), lk);
#else
        // Fallback to stderr on POSIX
        write_to_console_internal(pImpl, full_ln, lk);
#endif
        break;
    default:
        // Default fallback to console logging
        write_to_console_internal(pImpl, full_ln, lk);
        break;
    }
}

// record_write_error implementation:
// - Atomically updates counters and stores last error message under lock
// - Copies the user callback under lock
// - Calls the user callback outside the lock and performs optional rate-limited stderr notice
void Impl::record_write_error(int errcode, const char *msg) noexcept
{
    std::string saved_msg = msg ? msg : std::string();

    // Acquire lock once to update state and copy the callback.
    std::function<void(const std::string &)> cb;
    bool should_warn = false;
    {
        std::lock_guard<std::mutex> g(this->mtx);
        this->write_failure_count.fetch_add(1);
        this->last_errno.store(errcode); // Store the OS-specific error code
        this->last_write_errcode.store(errcode);
        this->last_write_errmsg = saved_msg;
        cb = this->write_error_callback;
        auto now = std::chrono::steady_clock::now();
        if (now - this->last_stderr_notice > std::chrono::seconds(5)) // Rate-limit warnings
        {
            this->last_stderr_notice = now;
            should_warn = true;
        }
    }

    // Invoke callback outside the lock (safe)
#if defined(_LOGGER_DEBUG_ENABLED)
    fmt::print(stdout, "record_write_error: cb is {}.\n", cb ? "NOT NULL" : "NULL");
    fflush(stdout);
#endif
    if (cb)
    {
        try
        {
            // Construct a more informative message for the callback
            std::string full_err_msg =
                fmt::format("Logger write error: {} (code: {})", saved_msg, errcode);
            cb(full_err_msg);
        }
        catch (...)
        {
            // swallow user exceptions - logging should never throw
        }
    }

    // Rate-limited warning printed outside the lock
    if (should_warn)
    {
        // Use fmt::print for consistency and type safety.
        fmt::print(stderr, "logger: write failure (count={}): {} (code: {}.\n",
                   this->write_failure_count.load(), saved_msg, errcode);
    }
}

} // namespace pylabhub::utils
