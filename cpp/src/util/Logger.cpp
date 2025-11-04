// Logger.cpp
#include "util/Logger.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <fmt/format.h>

#if defined(PLATFORM_WIN64)
#include <fcntl.h>
#include <io.h>
#include <stringapiset.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace pylabhub::util
{

struct Impl
{
    Logger::Destination dest = Logger::Destination::CONSOLE;
#if defined(PLATFORM_WIN64)
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    HANDLE evt_handle = nullptr;
#else
    int file_fd = -1;
#endif
    std::string file_path;
    bool use_flock = false;
    std::atomic<bool> fsync_per_write{false};
    std::mutex mtx;

    std::atomic<int> level{static_cast<int>(Logger::Level::DEBUG)};
    std::atomic<int> last_errno{0};

    std::atomic<int> write_failure_count{0};
    std::atomic<int> last_write_errcode{0};
    std::string last_write_errmsg;
    std::chrono::steady_clock::time_point last_stderr_notice =
        std::chrono::steady_clock::now() - std::chrono::seconds(10);
    std::function<void(const std::string &)> write_error_callback;

    std::atomic<size_t> max_log_line_length{256 * 1024};
};

void Logger::set_max_log_line_length(size_t bytes)
{
    pImpl->max_log_line_length.store(bytes ? bytes : 1);
}
size_t Logger::max_log_line_length() const noexcept
{
    return pImpl->max_log_line_length.load();
}

static uint64_t get_native_thread_id()
{
#if defined(PLATFORM_WIN64)
    return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(PLATFORM_APPLE)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid ? tid : std::hash<std::thread::id>()(std::this_thread::get_id());
#elif defined(__linux__)
    long tid = syscall(SYS_gettid);
    if (tid > 0)
        return static_cast<uint64_t>(tid);
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#else
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

static std::string formatted_time()
{
#if defined(PLATFORM_WIN64)
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = time_point_cast<seconds>(now);
    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm_buf;
    localtime_s(&tm_buf, &t);
    auto ms = duration_cast<milliseconds>(now - secs).count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03lld", tm_buf.tm_year + 1900,
             tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             static_cast<long long>(ms));
    return std::string(buf);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t t = tv.tv_sec;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char timebuf[64];
    int n = strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    int ms = static_cast<int>(tv.tv_usec / 1000);
    std::ostringstream oss;
    oss << std::string(timebuf, n) << '.' << std::setw(3) << std::setfill('0') << ms;
    return oss.str();
#endif
}

static const char *level_to_string(Logger::Level lvl)
{
    switch (lvl)
    {
    case Logger::Level::TRACE:
        return "TRACE";
    case Logger::Level::DEBUG:
        return "DEBUG";
    case Logger::Level::INFO:
        return "INFO";
    case Logger::Level::WARNING:
        return "WARN";
    case Logger::Level::ERROR:
        return "ERROR";
    default:
        return "UNK";
    }
}

#if !defined(PLATFORM_WIN64)
static int level_to_syslog_priority(Logger::Level lvl)
{
    switch (lvl)
    {
    case Logger::Level::TRACE:
        return LOG_DEBUG;
    case Logger::Level::DEBUG:
        return LOG_DEBUG;
    case Logger::Level::INFO:
        return LOG_INFO;
    case Logger::Level::WARNING:
        return LOG_WARNING;
    case Logger::Level::ERROR:
        return LOG_ERR;
    default:
        return LOG_INFO;
    }
}
#endif

static std::string build_header(Logger::Level lvl)
{
    std::ostringstream oss;
    oss << formatted_time();
    oss << " [" << level_to_string(lvl) << "] ";
    oss << "[tid=" << get_native_thread_id() << "] ";
    return oss.str();
}

#if defined(PLATFORM_WIN64)
static std::wstring utf8_to_wstring(const std::string &utf8)
{
    if (utf8.empty())
        return std::wstring();
    int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring out(needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], needed);
    return out;
}
#endif

struct Logger::ImplAccessorLocal
{
    Impl *p;
    explicit ImplAccessorLocal(Impl *p_) : p(p_) {}
    void record_write_error(int errcode, const std::string &msg)
    {
        p->write_failure_count.fetch_add(1);
        p->last_write_errcode.store(errcode);
        {
            std::lock_guard<std::mutex> g(p->mtx);
            p->last_write_errmsg = msg;
        }
        if (p->write_error_callback)
        {
            try
            {
                p->write_error_callback(msg);
            }
            catch (...)
            {
            }
        }
        auto now = std::chrono::steady_clock::now();
        constexpr auto NOTICE_INTERVAL = std::chrono::seconds(5);
        std::lock_guard<std::mutex> g(p->mtx);
        if (now - p->last_stderr_notice > NOTICE_INTERVAL)
        {
            p->last_stderr_notice = now;
            fprintf(stderr, "logger: write failure (count=%d): %s\n", p->write_failure_count.load(),
                    msg.c_str());
            fflush(stderr);
        }
    }
};

Logger::Logger() : pImpl(new Impl()) {}
Logger::~Logger()
{
    shutdown();
    delete pImpl;
}
Logger &Logger::instance()
{
    static Logger inst;
    return inst;
}

extern "C" Logger *get_global_logger()
{
    return &Logger::instance();
}

bool Logger::should_log(Level lvl) const noexcept {
    if (!pImpl)
        return false;
    return static_cast<int>(lvl) >= pImpl->level.load(std::memory_order_relaxed);
}

// Initialization and sinks (same as before) --------------------------------
bool Logger::init_file(const std::string &path, bool use_flock, int mode)
{
    std::lock_guard<std::mutex> g(pImpl->mtx);
#if defined(PLATFORM_WIN64)
    std::wstring wpath = utf8_to_wstring(path);
    if (wpath.empty())
    {
        pImpl->last_errno.store(GetLastError());
        return false;
    }
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
    pImpl->file_path = path;
    pImpl->use_flock = use_flock;
    pImpl->dest = Logger::Destination::FILE;
    return true;
#else
    if (pImpl->file_fd != -1)
    {
        ::close(pImpl->file_fd);
        pImpl->file_fd = -1;
    }
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, static_cast<mode_t>(mode));
    if (fd == -1)
    {
        pImpl->last_errno.store(errno);
        return false;
    }
    pImpl->file_fd = fd;
    pImpl->file_path = path;
    pImpl->use_flock = use_flock;
    pImpl->dest = Logger::Destination::FILE;
    return true;
#endif
}

void Logger::init_syslog(const char *ident, int option, int facility)
{
#if !defined(PLATFORM_WIN64)
    std::lock_guard<std::mutex> g(pImpl->mtx);
    openlog(ident ? ident : "app", option ? option : (LOG_PID | LOG_CONS),
            facility ? facility : LOG_USER);
    pImpl->dest = Logger::Destination::SYSLOG;
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
    pImpl->dest = Logger::Destination::EVENTLOG;
    return true;
#else
    (void)source_name;
    return false;
#endif
}

void Logger::set_destination(Logger::Destination dest)
{
    std::lock_guard<std::mutex> g(pImpl->mtx);
    pImpl->dest = dest;
}
void Logger::shutdown()
{
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
    closelog();
#endif
    pImpl->dest = Logger::Destination::CONSOLE;
}

void Logger::set_level(Level lvl)
{
    pImpl->level.store(static_cast<int>(lvl), std::memory_order_relaxed);
}
Logger::Level Logger::level() const
{
    return static_cast<Level>(pImpl->level.load(std::memory_order_relaxed));
}
void Logger::set_fsync_per_write(bool v)
{
    pImpl->fsync_per_write.store(v);
}
void Logger::set_write_error_callback(std::function<void(const std::string &)> cb)
{
    std::lock_guard<std::mutex> g(pImpl->mtx);
    pImpl->write_error_callback = std::move(cb);
}

int Logger::last_errno() const
{
    return pImpl->last_errno.load();
}
int Logger::last_write_error_code() const
{
    return pImpl->last_write_errcode.load();
}
std::string Logger::last_write_error_message() const
{
    std::lock_guard<std::mutex> g(pImpl->mtx);
    return pImpl->last_write_errmsg;
}
int Logger::write_failure_count() const
{
    return pImpl->write_failure_count.load();
}

// write_formatted: non-template sink (adds header and performs I/O)
// This centralizes all platform-specific write / locking behavior.
// The input 'body' is expected to be UTF-8 encoded bytes (no extra newline).
void Logger::write_formatted(Level lvl, std::string &&body) noexcept
{
    // Build full message (header + body + newline)
    std::string header = build_header(lvl);
    std::string full = header + body + "\n";

    std::lock_guard<std::mutex> g(pImpl->mtx);
    ImplAccessorLocal acc(pImpl);

    if (pImpl->dest == Logger::Destination::CONSOLE)
    {
        size_t wrote = fwrite(full.c_str(), 1, full.size(), stderr);
        if (wrote != full.size())
        {
#if defined(PLATFORM_WIN64)
            acc.record_write_error(GetLastError(), "fwrite to stderr failed on Windows");
#else
            acc.record_write_error(errno, strerror(errno));
#endif
        }
        fflush(stderr);
        return;
    }

    if (pImpl->dest == Logger::Destination::FILE)
    {
#if defined(PLATFORM_WIN64)
        if (pImpl->file_handle != INVALID_HANDLE_VALUE)
        {
            if (pImpl->use_flock)
            {
                OVERLAPPED ov = {};
                if (!LockFileEx(pImpl->file_handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 0xFFFFFFFF,
                                0xFFFFFFFF, &ov))
                {
                    acc.record_write_error(GetLastError(), "LockFileEx failed");
                }
                DWORD written = 0;
                BOOL ok = WriteFile(pImpl->file_handle, full.data(),
                                    static_cast<DWORD>(full.size()), &written, nullptr);
                if (!ok)
                    acc.record_write_error(GetLastError(), "WriteFile failed");
                else if (pImpl->fsync_per_write.load())
                    if (!FlushFileBuffers(pImpl->file_handle))
                        acc.record_write_error(GetLastError(), "FlushFileBuffers failed");
                OVERLAPPED ov2 = {};
                UnlockFileEx(pImpl->file_handle, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov2);
            }
            else
            {
                DWORD written = 0;
                BOOL ok = WriteFile(pImpl->file_handle, full.data(),
                                    static_cast<DWORD>(full.size()), &written, nullptr);
                if (!ok)
                    acc.record_write_error(GetLastError(), "WriteFile failed");
                else if (pImpl->fsync_per_write.load())
                    if (!FlushFileBuffers(pImpl->file_handle))
                        acc.record_write_error(GetLastError(), "FlushFileBuffers failed");
            }
        }
        else
        {
            std::wstring w = utf8_to_wstring(full);
            OutputDebugStringW(w.c_str());
        }
#else
        if (pImpl->file_fd != -1)
        {
            if (pImpl->use_flock)
                flock(pImpl->file_fd, LOCK_EX);
            ssize_t total = static_cast<ssize_t>(full.size());
            ssize_t off = 0;
            const char *data = full.data();
            while (off < total)
            {
                ssize_t w = ::write(pImpl->file_fd, data + off, static_cast<size_t>(total - off));
                if (w < 0)
                {
                    if (errno == EINTR)
                        continue;
                    acc.record_write_error(errno, strerror(errno));
                    break;
                }
                off += w;
            }
            if (pImpl->fsync_per_write.load())
            {
                if (::fsync(pImpl->file_fd) != 0)
                    acc.record_write_error(errno, strerror(errno));
            }
            if (pImpl->use_flock)
                flock(pImpl->file_fd, LOCK_UN);
        }
        else
        {
            size_t wrote = fwrite(full.c_str(), 1, full.size(), stderr);
            if (wrote != full.size())
                acc.record_write_error(errno, strerror(errno));
            fflush(stderr);
        }
#endif
        return;
    }

    if (pImpl->dest == Logger::Destination::SYSLOG)
    {
#if defined(PLATFORM_WIN64)
        std::wstring w = utf8_to_wstring(header + body);
        OutputDebugStringW(w.c_str());
#else
        syslog(level_to_syslog_priority(lvl), "%s", (header + body).c_str());
#endif
        return;
    }

    if (pImpl->dest == Logger::Destination::EVENTLOG)
    {
#if defined(PLATFORM_WIN64)
        if (pImpl->evt_handle)
        {
            std::wstring wmsg = utf8_to_wstring(header + body);
            LPCWSTR strings[1] = {wmsg.c_str()};
            if (!ReportEventW(pImpl->evt_handle, EVENTLOG_INFORMATION_TYPE, 0, 0, nullptr, 1, 0,
                              strings, nullptr))
            {
                acc.record_write_error(GetLastError(), "ReportEventW failed");
            }
        }
        else
        {
            std::wstring w = utf8_to_wstring(full);
            OutputDebugStringW(w.c_str());
        }
#else
        size_t wrote = fwrite(full.c_str(), 1, full.size(), stderr);
        if (wrote != full.size())
            acc.record_write_error(errno, strerror(errno));
        fflush(stderr);
#endif
        return;
    }

    // default fallback (console)
    size_t wrote = fwrite(full.c_str(), 1, full.size(), stderr);
    if (wrote != full.size())
    {
#if defined(PLATFORM_WIN64)
        acc.record_write_error(GetLastError(), "fwrite to stderr failed (fallback)");
#else
        acc.record_write_error(errno, strerror(errno));
#endif
    }
    fflush(stderr);
}

// Minimal compatibility wrapper: printf-style -> fmt
void Logger::log(const char *fmt, ...)
{
    // Use fmt to format forwarded args into a memory buffer.
    // Note: variadic C-style args cannot be directly turned into fmt args.
    // We support a simple path: if callers already use fmt-style strings, this will work.
    // For full printf->fmt migration, convert callsites to log_fmt.
    va_list ap;
    va_start(ap, fmt);
    // Create a string via vsnprintf as fallback; this keeps backward-compatibility but is less
    // efficient.
    constexpr size_t STACK_SZ = 1024;
    char stack_buf[STACK_SZ];
    va_list apcopy;
    va_copy(apcopy, ap);
    int needed = vsnprintf(nullptr, 0, fmt, apcopy);
    va_end(apcopy);

    std::string body;
    if (needed < 0)
    {
        body = "[FORMAT ERROR]";
    }
    else if (static_cast<size_t>(needed) < STACK_SZ)
    {
        va_list ap2;
        va_copy(ap2, ap);
        vsnprintf(stack_buf, STACK_SZ, fmt, ap2);
        va_end(ap2);
        body.assign(stack_buf, static_cast<size_t>(needed));
    }
    else
    {
        size_t sz = static_cast<size_t>(needed) + 1;
        std::vector<char> buf(sz);
        va_list ap3;
        va_copy(ap3, ap);
        vsnprintf(buf.data(), sz, fmt, ap3);
        va_end(ap3);
        body.assign(buf.data(), static_cast<size_t>(needed));
    }
    va_end(ap);

    write_formatted(Level::INFO, std::move(body));
}

} // namespace pylabhub::util
