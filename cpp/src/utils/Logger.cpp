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

#include <cerrno>
#include <chrono>
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
// The private implementation of the Logger.
struct Impl
{
    Impl();
    ~Impl();
    void worker_loop();

    // Asynchronous worker components
    std::atomic<bool> done{false};
    std::thread worker_thread;
    // Mutex and CV to protect the message queue and signal the worker thread.
    std::mutex queue_mtx;
    std::condition_variable cv;
    std::vector<std::string> queue;

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
}

Impl::~Impl()
{
    // Signal the worker to shut down and wait for it to finish.
    if (worker_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(queue_mtx);
            done = true;
        }
        cv.notify_one();
        worker_thread.join();
    }
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
static void do_write(Impl *pImpl, std::string &&full_ln);

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

// Helper: formatted local time with millisecond resolution (UTC/local depending on system)
static std::string formatted_time()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    // Use {fmt} to format the time portably.
    // This combines the date/time from a time_t with the milliseconds from the time_point.
    auto as_time_t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    return fmt::format("{:%Y-%m-%d %H:%M:%S}.{:03}", fmt::localtime(as_time_t), ms.count());
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
bool Logger::should_log(Logger::Level lvl) const noexcept
{
    if (!pImpl)
        return false;
    return static_cast<int>(lvl) >= pImpl->level.load(std::memory_order_relaxed);
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
bool Logger::init_file(const std::string &utf8_path, bool use_flock, int mode)
{
    if (!pImpl)
        return false;
    std::lock_guard<std::mutex> g(pImpl->mtx);
#if defined(PLATFORM_WIN64)
    // Convert UTF-8 path to wide and CreateFileW
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    if (needed == 0)
    {
        pImpl->last_errno.store(GetLastError());
        return false;
    }
    std::wstring wpath(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, &wpath[0], needed);
    // remove trailing null
    if (!wpath.empty() && wpath.back() == L'\0')
        wpath.pop_back();

    HANDLE h = CreateFileW(wpath.c_str(), FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        pImpl->last_errno.store(GetLastError());
        return false;
    }
    SetFilePointer(h, 0, nullptr, FILE_END);
    if (pImpl->file_handle != INVALID_HANDLE_VALUE)
        CloseHandle(pImpl->file_handle);
    pImpl->file_handle = h;
    pImpl->file_path = utf8_path;
    pImpl->use_flock = use_flock;
    pImpl->dest = Logger::Destination::L_FILE;
    return true;
#else
    // POSIX open with O_APPEND
    if (pImpl->file_fd != -1)
    {
        ::close(pImpl->file_fd);
        pImpl->file_fd = -1;
    }
    int fd = ::open(utf8_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, static_cast<mode_t>(mode));
    if (fd == -1)
    {
        pImpl->last_errno.store(errno);
        return false;
    }
    pImpl->file_fd = fd;
    pImpl->file_path = utf8_path;
    pImpl->use_flock = use_flock;
    pImpl->dest = Logger::Destination::L_FILE;
    return true;
#endif
}

void Logger::init_syslog(const char *ident, int option, int facility)
{
#if !defined(PLATFORM_WIN64)
    std::lock_guard<std::mutex> g(pImpl->mtx);
    openlog(ident ? ident : "app", option ? option : (LOG_PID | LOG_CONS),
            facility ? facility : LOG_USER);
    pImpl->dest = Logger::Destination::L_SYSLOG;
#else
    (void)ident;
    (void)option;
    (void)facility;
#endif
}

bool Logger::init_eventlog(const wchar_t *source_name)
{
#if defined(PLATFORM_WIN64)
    std::lock_guard<std::mutex> g(pImpl->mtx);
    if (pImpl->evt_handle)
    {
        DeregisterEventSource(pImpl->evt_handle);
        pImpl->evt_handle = nullptr;
    }
    HANDLE h = RegisterEventSourceW(nullptr, source_name);
    if (!h)
    {
        pImpl->last_errno.store(GetLastError());
        return false;
    }
    pImpl->evt_handle = h;
    pImpl->dest = Logger::Destination::L_EVENTLOG;
    return true;
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
    if (!pImpl)
        return;
    std::lock_guard<std::mutex> g(pImpl->mtx);
#if defined(PLATFORM_WIN64)
    if (pImpl->file_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pImpl->file_handle);
        pImpl->file_handle = INVALID_HANDLE_VALUE;
    }
    if (pImpl->evt_handle)
    {
        DeregisterEventSource(pImpl->evt_handle);
        pImpl->evt_handle = nullptr;
    }
#else
    if (pImpl->file_fd != -1)
    {
        ::close(pImpl->file_fd);
        pImpl->file_fd = -1;
        pImpl->file_path.clear();
    }
    // closelog is safe even if not previously opened
    closelog();
#endif
    pImpl->dest = Logger::Destination::L_CONSOLE;
}

// ---- Non-blocking write sink ----
// This function is called by the application threads. It formats the final
// message and pushes it to the worker queue, then returns immediately.
void Logger::write_formatted(Level lvl, std::string &&body) noexcept
{
    if (!pImpl)
        return;
    std::string full =
        fmt::format(FMT_STRING("{} [{}] [tid={}] {}"), formatted_time(), level_to_string(lvl),
                    std::to_string(get_native_thread_id()), body);
    std::string full_ln = full + "\n";
    
    // Push the formatted message to the queue for the worker thread.
    {
        std::lock_guard<std::mutex> lk(pImpl->queue_mtx);
        pImpl->queue.push_back(std::move(full_ln));
    }
    pImpl->cv.notify_one(); // Wake up the worker thread.
}

// The main loop for the background worker thread.
void Impl::worker_loop()
{
    std::vector<std::string> write_batch;
    while (true)
    {
        {
            std::unique_lock<std::mutex> lk(queue_mtx);
            // Wait until the queue has messages or shutdown is requested.
            cv.wait(lk, [this] { return done || !queue.empty(); });

            // If shutdown is requested and the queue is empty, we're done.
            if (done && queue.empty())
            {
                break;
            }

            // Atomically swap the queue contents into our local batch.
            // This minimizes the time the queue is locked.
            write_batch.swap(queue);
        }

        // Process all messages in the batch.
        for (auto &msg : write_batch)
        {
            do_write(this, std::move(msg));
        }
        write_batch.clear();
    }

    // After exiting the loop, ensure all sinks are properly closed.
    // This is a final cleanup step.
    std::lock_guard<std::mutex> g(mtx);
#if defined(PLATFORM_WIN64)
    if (file_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file_handle);
        file_handle = INVALID_HANDLE_VALUE;
    }
    if (evt_handle)
    {
        DeregisterEventSource(evt_handle);
        evt_handle = nullptr;
    }
#else
    if (file_fd != -1)
    {
        ::close(file_fd);
        file_fd = -1;
        file_path.clear();
    }
    closelog();
#endif
}

// This function contains the original synchronous I/O logic.
// It is now called exclusively by the worker thread.
static void do_write(Impl *pImpl, std::string &&full_ln)
{
    // The worker thread is the only writer, but we still need to lock
    // to protect sink handles during reconfiguration (e.g., set_destination).
    std::unique_lock<std::mutex> lk(pImpl->mtx);

    if (pImpl->dest == Logger::Destination::L_CONSOLE)
    {
        try
        {
            // Use fmt::print for efficient, type-safe output to stderr.
            fmt::print(stderr, FMT_STRING("{}"), full_ln);
            fflush(stderr);
        }
        catch (const std::system_error &e)
        {
            // This is how {fmt} reports I/O errors.
            int err = e.code().value();
            std::string msg = e.what();
            // Unlock before calling the error handler to avoid deadlocks if the
            // callback tries to interact with the logger.
            lk.unlock();
            Logger::instance().record_write_error(err, msg.c_str());
        }
        return;
    }

    if (pImpl->dest == Logger::Destination::L_FILE)
    {
#if defined(PLATFORM_WIN64)
        if (pImpl->file_handle != INVALID_HANDLE_VALUE)
        {
            // prefer single WriteFile call
            DWORD written = 0;
            BOOL ok = WriteFile(pImpl->file_handle, full_ln.data(),
                                static_cast<DWORD>(full_ln.size()), &written, nullptr);
            if (!ok || written != full_ln.size())
            {
                DWORD err = GetLastError();
                std::string msg = "WriteFile failed";
                lk.unlock(); // Unlock before calling back.
                Logger::instance().record_write_error(static_cast<int>(err), msg.c_str());
            }
            else
            {
                if (pImpl->fsync_per_write.load())
                {
                    if (!FlushFileBuffers(pImpl->file_handle))
                    {
                        DWORD err2 = GetLastError();
                        std::string msg = "FlushFileBuffers failed";
                        lk.unlock(); // Unlock before calling back.
                        Logger::instance().record_write_error(static_cast<int>(err2), msg.c_str());
                    }
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
                int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                                 nullptr, 0);
                if (needed <= 0)
                    return {};
                std::wstring out(needed, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0],
                                    needed);
                return out;
            }(full_ln);
            OutputDebugStringW(w.c_str());
        }
#else
        if (pImpl->file_fd != -1)
        {
            // If use_flock is requested, obtain file lock first (advisory)
            if (pImpl->use_flock)
                flock(pImpl->file_fd, LOCK_EX);
            // Attempt single write; loop to handle EINTR / partial writes
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
                    std::string msg = std::string("write() failed: ") + strerror(err);
                    if (pImpl->use_flock)
                        flock(pImpl->file_fd, LOCK_UN);
                    lk.unlock(); // Unlock before calling back.
                    Logger::instance().record_write_error(err, msg.c_str());
                    return;
                }
                off += w;
            }
            if (pImpl->fsync_per_write.load())
            {
                if (::fsync(pImpl->file_fd) != 0)
                {
                    int err = errno;
                    std::string msg = std::string("fsync failed: ") + strerror(err);
                    if (pImpl->use_flock)
                        flock(pImpl->file_fd, LOCK_UN);
                    lk.unlock(); // Unlock before calling back.
                    Logger::instance().record_write_error(err, msg.c_str());
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
                int err = errno;
                std::string msg = std::string("fwrite to stderr failed: ") + strerror(err);
                lk.unlock(); // Unlock before calling back.
                Logger::instance().record_write_error(err, msg.c_str());
            }
            else
            {
                fflush(stderr);
            }
        }
#endif
        return;
    }

    if (pImpl->dest == Logger::Destination::L_SYSLOG)
    {
#if defined(PLATFORM_WIN64)
        // On Windows default to debug output
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
        }(full);
        OutputDebugStringW(w.c_str());
#else
        syslog(level_to_syslog_priority(lvl), "%s", full.c_str());
#endif
        return;
    }

    if (pImpl->dest == Logger::Destination::L_EVENTLOG)
    {
#if defined(PLATFORM_WIN64)
        if (pImpl->evt_handle)
        {
            std::wstring wmsg;
            {
                // convert utf8 to wstring
                int needed = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, nullptr, 0);
                if (needed > 0)
                {
                    wmsg.resize(needed);
                    MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, &wmsg[0], needed);
                    if (!wmsg.empty() && wmsg.back() == L'\0')
                        wmsg.pop_back();
                }
            }
            LPCWSTR strings[1] = {wmsg.c_str()};
            if (!ReportEventW(pImpl->evt_handle, EVENTLOG_INFORMATION_TYPE, 0, 0, nullptr, 1, 0,
                              strings, nullptr))
            {
                DWORD err = GetLastError();
                std::string msg = "ReportEventW failed";
                lk.unlock(); // Unlock before calling back.
                Logger::instance().record_write_error(static_cast<int>(err), msg.c_str());
            }
        }
        else
        {
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
#else
        // fallback to stderr
        size_t wrote = fwrite(full_ln.data(), 1, full_ln.size(), stderr);
        if (wrote != full_ln.size())
        {
            int err = errno;
            std::string msg =
                std::string("fwrite to stderr failed (eventlog fallback): ") + strerror(err);
            lk.unlock(); // Unlock before calling back.
            Logger::instance().record_write_error(err, msg.c_str());
        }
        else
        {
            fflush(stderr);
        }
#endif
        return;
    }

    // default fallback: console
    try
    {
        fmt::print(stderr, FMT_STRING("{}"), full_ln);
        fflush(stderr);
    }
    catch (const std::system_error &e)
    {
        int err = e.code().value();
        std::string msg = std::string("fwrite to stderr failed (fallback): ") + e.what();
        lk.unlock(); // Unlock before calling back.
        Logger::instance().record_write_error(err, msg.c_str());
    }
}

// record_write_error implementation:
// - Atomically updates counters and stores last error message under lock
// - Copies the user callback under lock
// - Calls the user callback outside the lock and performs optional rate-limited stderr notice
void Logger::record_write_error(int errcode, const char *msg) noexcept
{
    if (!pImpl)
        return;
    std::string saved_msg = msg ? msg : std::string();

    // Acquire lock once to update state and copy the callback.
    std::function<void(const std::string &)> cb;
    bool should_warn = false;
    {
        std::lock_guard<std::mutex> g(pImpl->mtx);
        pImpl->write_failure_count.fetch_add(1);
        pImpl->last_write_errcode.store(errcode);
        pImpl->last_write_errmsg = saved_msg;
        cb = pImpl->write_error_callback;
        auto now = std::chrono::steady_clock::now();
        if (now - pImpl->last_stderr_notice > std::chrono::seconds(5)) // Rate-limit warnings
        {
            pImpl->last_stderr_notice = now;
            should_warn = true;
        }
    }

    // Invoke callback outside the lock (safe)
    if (cb)
    {
        try
        {
            cb(saved_msg);
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
        fmt::print(stderr, "logger: write failure (count={}): {}\n",
                   pImpl->write_failure_count.load(), saved_msg);
    }
}

// Minimal printf-style compatibility wrapper using vsnprintf fallback
void Logger::log_printf(const char *fmt, ...) noexcept
{
    if (!fmt)
        return;
    // vsnprintf to compute size then format
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(nullptr, 0, fmt, ap_copy);
    va_end(ap_copy);

    std::string body;
    if (needed < 0)
    {
        body = "[FORMAT ERROR]";
    }
    else
    {
        size_t sz = static_cast<size_t>(needed) + 1;
        std::vector<char> buf(sz);
        va_list ap2;
        va_copy(ap2, ap);
        vsnprintf(buf.data(), sz, fmt, ap2);
        va_end(ap2);
        body.assign(buf.data(), static_cast<size_t>(needed));
    }
    va_end(ap);

    log_fmt(Logger::Level::L_INFO, "{}", body);
}

} // namespace pylabhub::utils
