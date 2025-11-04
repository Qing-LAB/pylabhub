// Logger.cpp
//
// Implementation of Logger. Impl (pimpl) is defined only in this file to keep the header
// free of implementation details (mutexes, file descriptors, platform headers, etc.).
//
// Important design and locking notes (read carefully):
//  - Most state is stored in Impl and protected by Impl::mtx where appropriate.
//  - The template log_fmt() in the header calls should_log() and max_log_line_length()
//    which are small non-template functions defined below (they only read atomics).
//  - write_formatted() acquires the Impl mutex to access/serialize sink handles where needed.
//    If a write fails we **release** the write lock before invoking record_write_error()
//    to avoid a deadlock / reentrancy problem. record_write_error() acquires the mutex
//    internally as required and invokes user callbacks outside the lock.
//  - The RAII helper ImplAccessorLocal is defined in this translation unit only and is not
//    visible in the header.

#include "util/Logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <cstring>
#include <cerrno>
#include <cstdio>

#include <fmt/format.h>

#if defined(PLATFORM_WIN64)
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/file.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace pylabhub::util {

// Forward declare helpers (Impl defined below)
struct Impl {
    // Public members for this TU-local struct (we are in the .cpp).
    std::mutex mtx;

    Logger::Destination dest = Logger::Destination::CONSOLE;

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
    std::atomic<int> level{static_cast<int>(Logger::Level::DEBUG)};
    std::atomic<bool> fsync_per_write{false};

    std::atomic<size_t> max_log_line_length{256 * 1024};

    std::atomic<int> last_errno{0};
    std::atomic<int> write_failure_count{0};
    std::atomic<int> last_write_errcode{0};
    std::string last_write_errmsg;

    std::chrono::steady_clock::time_point last_stderr_notice =
        std::chrono::steady_clock::now() - std::chrono::seconds(10);

    std::function<void(const std::string &)> write_error_callback;
};

// TU-local RAII helper. Not visible outside this file.
// This helper assumes the caller will use it to capture state and call callback
// outside the critical section. It does NOT attempt to lock the Impl mutex itself
// unless needed by a member function.
struct ImplAccessorLocal {
    explicit ImplAccessorLocal(Impl *impl) : p(impl) {}

    // Atomically update counters and store the message under lock.
    void capture_error(int errcode, const std::string &msg) {
        p->write_failure_count.fetch_add(1);
        p->last_write_errcode.store(errcode);
        {
            std::lock_guard<std::mutex> g(p->mtx);
            p->last_write_errmsg = msg;
        }
    }

    // Copy the callback under lock so we can call it outside the lock.
    std::function<void(const std::string &)> copy_callback_and_update_notice() {
        std::function<void(const std::string &)> cb;
        {
            std::lock_guard<std::mutex> g(p->mtx);
            cb = p->write_error_callback;
            // update the last_stderr_notice if the interval elapsed
            // (we will print outside the lock if needed)
            auto now = std::chrono::steady_clock::now();
            if (now - p->last_stderr_notice > std::chrono::seconds(5)) {
                p->last_stderr_notice = now;
                should_warn = true;
            }
        }
        return cb;
    }

    bool should_warn_now() const noexcept { return should_warn; }

private:
    Impl *p;
    bool should_warn{false};
};

// Helper: get a platform-native thread id
static uint64_t get_native_thread_id() noexcept {
#if defined(PLATFORM_WIN64)
    return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(PLATFORM_APPLE)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid ? tid : std::hash<std::thread::id>()(std::this_thread::get_id());
#elif defined(__linux__)
    // try syscall to get kernel thread id
    long tid = syscall(SYS_gettid);
    if (tid > 0) return static_cast<uint64_t>(tid);
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#else
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

// Helper: formatted local time with millisecond resolution (UTC/local depending on system)
static std::string formatted_time() {
#if defined(PLATFORM_WIN64)
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = time_point_cast<seconds>(now);
    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm_buf;
    localtime_s(&tm_buf, &t);
    auto ms = duration_cast<milliseconds>(now - secs).count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<long long>(ms));
    return std::string(buf);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t t = tv.tv_sec;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    int ms = static_cast<int>(tv.tv_usec / 1000);
    std::ostringstream oss;
    oss << timebuf << '.' << std::setw(3) << std::setfill('0') << ms;
    return oss.str();
#endif
}

static const char *level_to_string(Logger::Level lvl) noexcept {
    switch (lvl) {
    case Logger::Level::TRACE: return "TRACE";
    case Logger::Level::DEBUG: return "DEBUG";
    case Logger::Level::INFO:  return "INFO";
    case Logger::Level::WARNING: return "WARN";
    case Logger::Level::ERROR: return "ERROR";
    default: return "UNK";
    }
}

#if !defined(PLATFORM_WIN64)
static int level_to_syslog_priority(Logger::Level lvl) noexcept {
    switch (lvl) {
    case Logger::Level::TRACE: return LOG_DEBUG;
    case Logger::Level::DEBUG: return LOG_DEBUG;
    case Logger::Level::INFO: return LOG_INFO;
    case Logger::Level::WARNING: return LOG_WARNING;
    case Logger::Level::ERROR: return LOG_ERR;
    default: return LOG_INFO;
    }
}
#endif

// ------------------------ Logger implementation ------------------------

Logger::Logger() : pImpl(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

Logger &Logger::instance() {
    static Logger inst;
    return inst;
}

// ---- small accessors used by header-only templates ----
bool Logger::should_log(Logger::Level lvl) const noexcept {
    if (!pImpl) return false;
    return static_cast<int>(lvl) >= pImpl->level.load(std::memory_order_relaxed);
}

size_t Logger::max_log_line_length() const noexcept {
    if (!pImpl) return 0;
    return pImpl->max_log_line_length.load(std::memory_order_relaxed);
}

// ---- lifecycle / configuration ----
void Logger::set_level(Logger::Level lvl) {
    if (!pImpl) return;
    pImpl->level.store(static_cast<int>(lvl), std::memory_order_relaxed);
}
Logger::Level Logger::level() const {
    if (!pImpl) return Logger::Level::INFO;
    return static_cast<Logger::Level>(pImpl->level.load(std::memory_order_relaxed));
}
void Logger::set_fsync_per_write(bool v) {
    if (!pImpl) return;
    pImpl->fsync_per_write.store(v);
}
void Logger::set_write_error_callback(std::function<void(const std::string &)> cb) {
    if (!pImpl) return;
    std::lock_guard<std::mutex> g(pImpl->mtx);
    pImpl->write_error_callback = std::move(cb);
}

int Logger::last_errno() const {
    if (!pImpl) return 0;
    return pImpl->last_errno.load();
}
int Logger::last_write_error_code() const {
    if (!pImpl) return 0;
    return pImpl->last_write_errcode.load();
}
std::string Logger::last_write_error_message() const {
    if (!pImpl) return std::string();
    std::lock_guard<std::mutex> g(pImpl->mtx);
    return pImpl->last_write_errmsg;
}
int Logger::write_failure_count() const {
    if (!pImpl) return 0;
    return pImpl->write_failure_count.load();
}

void Logger::set_max_log_line_length(size_t bytes) {
    if (!pImpl) return;
    pImpl->max_log_line_length.store(bytes ? bytes : 1);
}

// ---- sinks initialization ----
bool Logger::init_file(const std::string &utf8_path, bool use_flock, int mode) {
    if (!pImpl) return false;
    std::lock_guard<std::mutex> g(pImpl->mtx);
#if defined(PLATFORM_WIN64)
    // Convert UTF-8 path to wide and CreateFileW
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    if (needed == 0) {
        pImpl->last_errno.store(GetLastError());
        return false;
    }
    std::wstring wpath(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, &wpath[0], needed);
    // remove trailing null
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

    HANDLE h = CreateFileW(wpath.c_str(), FILE_APPEND_DATA | GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        pImpl->last_errno.store(GetLastError());
        return false;
    }
    SetFilePointer(h, 0, nullptr, FILE_END);
    if (pImpl->file_handle != INVALID_HANDLE_VALUE) CloseHandle(pImpl->file_handle);
    pImpl->file_handle = h;
    pImpl->file_path = utf8_path;
    pImpl->use_flock = use_flock;
    pImpl->dest = Logger::Destination::FILE;
    return true;
#else
    // POSIX open with O_APPEND
    if (pImpl->file_fd != -1) {
        ::close(pImpl->file_fd);
        pImpl->file_fd = -1;
    }
    int fd = ::open(utf8_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, static_cast<mode_t>(mode));
    if (fd == -1) {
        pImpl->last_errno.store(errno);
        return false;
    }
    pImpl->file_fd = fd;
    pImpl->file_path = utf8_path;
    pImpl->use_flock = use_flock;
    pImpl->dest = Logger::Destination::FILE;
    return true;
#endif
}

void Logger::init_syslog(const char *ident, int option, int facility) {
#if !defined(PLATFORM_WIN64)
    std::lock_guard<std::mutex> g(pImpl->mtx);
    openlog(ident ? ident : "app", option ? option : (LOG_PID | LOG_CONS), facility ? facility : LOG_USER);
    pImpl->dest = Logger::Destination::SYSLOG;
#else
    (void)ident; (void)option; (void)facility;
#endif
}

bool Logger::init_eventlog(const wchar_t *source_name) {
#if defined(PLATFORM_WIN64)
    std::lock_guard<std::mutex> g(pImpl->mtx);
    if (pImpl->evt_handle) { DeregisterEventSource(pImpl->evt_handle); pImpl->evt_handle = nullptr; }
    HANDLE h = RegisterEventSourceW(nullptr, source_name);
    if (!h) {
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

void Logger::set_destination(Logger::Destination dest) {
    if (!pImpl) return;
    std::lock_guard<std::mutex> g(pImpl->mtx);
    pImpl->dest = dest;
}

void Logger::shutdown() {
    if (!pImpl) return;
    std::lock_guard<std::mutex> g(pImpl->mtx);
#if defined(PLATFORM_WIN64)
    if (pImpl->file_handle != INVALID_HANDLE_VALUE) { CloseHandle(pImpl->file_handle); pImpl->file_handle = INVALID_HANDLE_VALUE; }
    if (pImpl->evt_handle) { DeregisterEventSource(pImpl->evt_handle); pImpl->evt_handle = nullptr; }
#else
    if (pImpl->file_fd != -1) { ::close(pImpl->file_fd); pImpl->file_fd = -1; pImpl->file_path.clear(); }
    // closelog is safe even if not previously opened
    closelog();
#endif
    pImpl->dest = Logger::Destination::CONSOLE;
}

// ---- write sink ----
// write_formatted centralizes all sink-specific logic and ensures:
//  - body is UTF-8 (no trailing newline expected)
//  - appropriate header (timestamp, level, tid) is prepended
//  - on write failure, record_write_error(...) is called outside of any write lock
void Logger::write_formatted(Level lvl, std::string &&body) noexcept {
    if (!pImpl) return;
    // Build header and full message (utf-8)
    std::string header = formatted_time();
    header += " [";
    header += level_to_string(lvl);
    header += "] ";
    header += "[tid=" + std::to_string(get_native_thread_id()) + "] ";

    std::string full = header + body + "\n";

    // Acquire lock to protect sink handles while writing.
    std::unique_lock<std::mutex> lk(pImpl->mtx);

    if (pImpl->dest == Logger::Destination::CONSOLE) {
        // write to stderr
        size_t wrote = fwrite(full.data(), 1, full.size(), stderr);
        if (wrote != full.size()) {
#if defined(PLATFORM_WIN64)
            int err = static_cast<int>(GetLastError());
            std::string msg = "fwrite to stderr failed on Windows";
            // release lock before recording error
            lk.unlock();
            record_write_error(err, msg.c_str());
#else
            int err = errno;
            std::string msg = std::string("fwrite to stderr failed: ") + strerror(err);
            lk.unlock();
            record_write_error(err, msg.c_str());
#endif
        } else {
            fflush(stderr);
        }
        return;
    }

    if (pImpl->dest == Logger::Destination::FILE) {
#if defined(PLATFORM_WIN64)
        if (pImpl->file_handle != INVALID_HANDLE_VALUE) {
            // prefer single WriteFile call
            DWORD written = 0;
            BOOL ok = WriteFile(pImpl->file_handle, full.data(), static_cast<DWORD>(full.size()), &written, nullptr);
            if (!ok || written != full.size()) {
                DWORD err = GetLastError();
                std::string msg = "WriteFile failed";
                lk.unlock();
                record_write_error(static_cast<int>(err), msg.c_str());
            } else {
                if (pImpl->fsync_per_write.load()) {
                    if (!FlushFileBuffers(pImpl->file_handle)) {
                        DWORD err2 = GetLastError();
                        std::string msg = "FlushFileBuffers failed";
                        lk.unlock();
                        record_write_error(static_cast<int>(err2), msg.c_str());
                    }
                }
            }
        } else {
            // fallback to OutputDebugString if no file handle
            std::wstring w = [](const std::string &s)->std::wstring {
                if (s.empty()) return {};
                int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
                if (needed <= 0) return {};
                std::wstring out(needed, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], needed);
                return out;
            }(full);
            OutputDebugStringW(w.c_str());
        }
#else
        if (pImpl->file_fd != -1) {
            // If use_flock is requested, obtain file lock first (advisory)
            if (pImpl->use_flock) flock(pImpl->file_fd, LOCK_EX);
            // Attempt single write; loop to handle EINTR / partial writes
            ssize_t total = static_cast<ssize_t>(full.size());
            ssize_t off = 0;
            const char *data = full.data();
            while (off < total) {
                ssize_t w = ::write(pImpl->file_fd, data + off, static_cast<size_t>(total - off));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    int err = errno;
                    std::string msg = std::string("write() failed: ") + strerror(err);
                    if (pImpl->use_flock) flock(pImpl->file_fd, LOCK_UN);
                    lk.unlock();
                    record_write_error(err, msg.c_str());
                    return;
                }
                off += w;
            }
            if (pImpl->fsync_per_write.load()) {
                if (::fsync(pImpl->file_fd) != 0) {
                    int err = errno;
                    std::string msg = std::string("fsync failed: ") + strerror(err);
                    if (pImpl->use_flock) flock(pImpl->file_fd, LOCK_UN);
                    lk.unlock();
                    record_write_error(err, msg.c_str());
                    return;
                }
            }
            if (pImpl->use_flock) flock(pImpl->file_fd, LOCK_UN);
        } else {
            size_t wrote = fwrite(full.data(), 1, full.size(), stderr);
            if (wrote != full.size()) {
                int err = errno;
                std::string msg = std::string("fwrite to stderr failed: ") + strerror(err);
                lk.unlock();
                record_write_error(err, msg.c_str());
            } else {
                fflush(stderr);
            }
        }
#endif
        return;
    }

    if (pImpl->dest == Logger::Destination::SYSLOG) {
#if defined(PLATFORM_WIN64)
        // On Windows default to debug output
        std::wstring w = [](const std::string &s)->std::wstring {
            if (s.empty()) return {};
            int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            if (needed <= 0) return {};
            std::wstring out(needed, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], needed);
            return out;
        }(header + body);
        OutputDebugStringW(w.c_str());
#else
        syslog(level_to_syslog_priority(lvl), "%s", (header + body).c_str());
#endif
        return;
    }

    if (pImpl->dest == Logger::Destination::EVENTLOG) {
#if defined(PLATFORM_WIN64)
        if (pImpl->evt_handle) {
            std::wstring wmsg;
            {
                // convert utf8 to wstring
                int needed = MultiByteToWideChar(CP_UTF8, 0, (header + body).c_str(), -1, nullptr, 0);
                if (needed > 0) {
                    wmsg.resize(needed);
                    MultiByteToWideChar(CP_UTF8, 0, (header + body).c_str(), -1, &wmsg[0], needed);
                    if (!wmsg.empty() && wmsg.back() == L'\0') wmsg.pop_back();
                }
            }
            LPCWSTR strings[1] = { wmsg.c_str() };
            if (!ReportEventW(pImpl->evt_handle, EVENTLOG_INFORMATION_TYPE, 0, 0, nullptr, 1, 0, strings, nullptr)) {
                DWORD err = GetLastError();
                std::string msg = "ReportEventW failed";
                lk.unlock();
                record_write_error(static_cast<int>(err), msg.c_str());
            }
        } else {
            std::wstring w;
            int needed = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, nullptr, 0);
            if (needed > 0) {
                w.resize(needed);
                MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, &w[0], needed);
                if (!w.empty() && w.back() == L'\0') w.pop_back();
            }
            OutputDebugStringW(w.c_str());
        }
#else
        // fallback to stderr
        size_t wrote = fwrite(full.data(), 1, full.size(), stderr);
        if (wrote != full.size()) {
            int err = errno;
            std::string msg = std::string("fwrite to stderr failed (eventlog fallback): ") + strerror(err);
            lk.unlock();
            record_write_error(err, msg.c_str());
        } else {
            fflush(stderr);
        }
#endif
        return;
    }

    // default fallback: console
    size_t wrote = fwrite(full.data(), 1, full.size(), stderr);
    if (wrote != full.size()) {
#if defined(PLATFORM_WIN64)
        int err = static_cast<int>(GetLastError());
        std::string msg = "fwrite to stderr failed (fallback)";
        lk.unlock();
        record_write_error(err, msg.c_str());
#else
        int err = errno;
        std::string msg = std::string("fwrite to stderr failed (fallback): ") + strerror(err);
        lk.unlock();
        record_write_error(err, msg.c_str());
#endif
    } else {
        fflush(stderr);
    }
}

// record_write_error implementation:
// - Atomically updates counters and stores last error message under lock
// - Copies the user callback under lock
// - Calls the user callback outside the lock and performs optional rate-limited stderr notice
void Logger::record_write_error(int errcode, const char *msg) noexcept {
    if (!pImpl) return;
    std::string saved_msg = msg ? msg : std::string();

    // update counters and store message under lock
    {
        std::lock_guard<std::mutex> g(pImpl->mtx);
        pImpl->write_failure_count.fetch_add(1);
        pImpl->last_write_errcode.store(errcode);
        pImpl->last_write_errmsg = saved_msg;
    }

    // copy callback under lock
    std::function<void(const std::string &)> cb;
    bool should_warn = false;
    {
        std::lock_guard<std::mutex> g(pImpl->mtx);
        cb = pImpl->write_error_callback;
        auto now = std::chrono::steady_clock::now();
        if (now - pImpl->last_stderr_notice > std::chrono::seconds(5)) {
            pImpl->last_stderr_notice = now;
            should_warn = true;
        }
    }

    // Invoke callback outside the lock (safe)
    if (cb) {
        try {
            cb(saved_msg);
        } catch (...) {
            // swallow user exceptions - logging should never throw
        }
    }

    // Rate-limited warning printed outside the lock
    if (should_warn) {
        fprintf(stderr, "logger: write failure (count=%d): %s\n", pImpl->write_failure_count.load(), saved_msg.c_str());
        fflush(stderr);
    }
}

// Minimal printf-style compatibility wrapper using vsnprintf fallback
void Logger::log_printf(const char *fmt, ...) noexcept {
    if (!fmt) return;
    // vsnprintf to compute size then format
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(nullptr, 0, fmt, ap_copy);
    va_end(ap_copy);

    std::string body;
    if (needed < 0) {
        body = "[FORMAT ERROR]";
    } else {
        size_t sz = static_cast<size_t>(needed) + 1;
        std::vector<char> buf(sz);
        va_list ap2;
        va_copy(ap2, ap);
        vsnprintf(buf.data(), sz, fmt, ap2);
        va_end(ap2);
        body.assign(buf.data(), static_cast<size_t>(needed));
    }
    va_end(ap);

    write_formatted(Logger::Level::INFO, std::move(body));
}

} // namespace pylabhub::util
