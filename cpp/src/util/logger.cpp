// logger.cpp
#define LOGGER_EXPORTS
#include "util/logger.hpp"

#include <mutex>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <sstream>
#include <iomanip>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/time.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>

// Implementation details hidden behind Impl
struct Logger::Impl {
    enum class Destination { NONE = 0, FILE, SYSLOG };
    Destination dest = Destination::NONE;
    int file_fd = -1;
    std::string file_path;
    bool use_flock = false;
    std::atomic<bool> fsync_per_write{false};
    std::mutex mtx;
    std::atomic<int> level{static_cast<int>(Logger::Level::DEBUG)};
    int last_errno = 0;
};

Logger::Logger() : pImpl(new Impl()) {}
Logger::~Logger() { shutdown(); delete pImpl; }

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

// Exported C symbol
extern "C" LOGGER_API Logger* get_global_logger() {
    return &Logger::instance();
}

bool Logger::init_file(const std::string& path, bool use_flock, int mode) {
    std::lock_guard<std::mutex> g(pImpl->mtx);
    if (pImpl->file_fd != -1) {
        ::close(pImpl->file_fd);
        pImpl->file_fd = -1;
    }
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, static_cast<mode_t>(mode));
    if (fd == -1) {
        pImpl->last_errno = errno;
        return false;
    }
    pImpl->file_fd = fd;
    pImpl->file_path = path;
    pImpl->use_flock = use_flock;
    pImpl->dest = Impl::Destination::FILE;
    return true;
}

void Logger::init_syslog(const char* ident, int option, int facility) {
    std::lock_guard<std::mutex> g(pImpl->mtx);
    openlog(ident, option ? option : (LOG_PID | LOG_CONS), facility ? facility : LOG_USER);
    pImpl->dest = Impl::Destination::SYSLOG;
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> g(pImpl->mtx);
    if (pImpl->file_fd != -1) {
        ::close(pImpl->file_fd);
        pImpl->file_fd = -1;
        pImpl->file_path.clear();
    }
    if (pImpl->dest == Impl::Destination::SYSLOG) {
        closelog();
    }
    pImpl->dest = Impl::Destination::NONE;
}

void Logger::set_level(Level lvl) { pImpl->level.store(static_cast<int>(lvl), std::memory_order_relaxed); }
Logger::Level Logger::level() const { return static_cast<Level>(pImpl->level.load(std::memory_order_relaxed)); }
void Logger::set_fsync_per_write(bool v) { pImpl->fsync_per_write.store(v); }
int Logger::last_errno() const { return pImpl->last_errno; }

static const char* level_to_string(Logger::Level lvl) {
    switch (lvl) {
    case Logger::Level::TRACE: return "TRACE";
    case Logger::Level::DEBUG: return "DEBUG";
    case Logger::Level::INFO: return "INFO";
    case Logger::Level::WARNING: return "WARN";
    case Logger::Level::ERROR: return "ERROR";
    default: return "UNK";
    }
}
static int level_to_syslog_priority(Logger::Level lvl) {
    switch (lvl) {
    case Logger::Level::TRACE: return LOG_DEBUG;
    case Logger::Level::DEBUG: return LOG_DEBUG;
    case Logger::Level::INFO: return LOG_INFO;
    case Logger::Level::WARNING: return LOG_WARNING;
    case Logger::Level::ERROR: return LOG_ERR;
    default: return LOG_INFO;
    }
}

static std::string build_header(Logger::Level lvl) {
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
    oss << " [" << level_to_string(lvl) << "] ";
    oss << "[tid=" << std::this_thread::get_id() << "] ";
    return oss.str();
}

void Logger::log(Level lvl, const char* fmt, ...) {
    if (static_cast<int>(lvl) < pImpl->level.load(std::memory_order_relaxed)) return;

    va_list ap; va_start(ap, fmt);
    char small[1024];
    va_list apcopy;
    va_copy(apcopy, ap);
    int needed = vsnprintf(small, sizeof(small), fmt, apcopy);
    va_end(apcopy);

    std::string body;
    if (needed < 0) {
        body = "[format error]";
    } else if (static_cast<size_t>(needed) < sizeof(small)) {
        body.assign(small, static_cast<size_t>(needed));
    } else {
        std::vector<char> buf(static_cast<size_t>(needed) + 1);
        va_list apcopy2;
        va_copy(apcopy2, ap);
        vsnprintf(buf.data(), buf.size(), fmt, apcopy2);
        va_end(apcopy2);
        body.assign(buf.data(), needed);
    }
    va_end(ap);

    std::string header = build_header(lvl);
    std::string full = header + body + "\n";

    std::lock_guard<std::mutex> g(pImpl->mtx);
    if (pImpl->dest == Impl::Destination::FILE && pImpl->file_fd != -1) {
        ssize_t total = static_cast<ssize_t>(full.size());
        if (pImpl->use_flock) {
            flock(pImpl->file_fd, LOCK_EX);
        }
        ssize_t off = 0;
        const char* data = full.data();
        while (off < total) {
            ssize_t w = ::write(pImpl->file_fd, data + off, static_cast<size_t>(total - off));
            if (w <= 0) {
                if (errno == EINTR) continue;
                break;
            }
            off += w;
        }
        if (pImpl->fsync_per_write.load()) ::fsync(pImpl->file_fd);
        if (pImpl->use_flock) flock(pImpl->file_fd, LOCK_UN);
    } else if (pImpl->dest == Impl::Destination::SYSLOG) {
        syslog(level_to_syslog_priority(lvl), "%s", (header + body).c_str());
    } else {
        fwrite(full.c_str(), 1, full.size(), stderr);
        fflush(stderr);
    }
}

// Convenience wrappers
void Logger::trace(const char* fmt, ...) {
    if (static_cast<int>(Logger::Level::TRACE) < pImpl->level.load(std::memory_order_relaxed)) return;
    va_list ap; va_start(ap, fmt); log(Logger::Level::TRACE, fmt, ap); va_end(ap);
}
void Logger::debug(const char* fmt, ...) {
    if (static_cast<int>(Logger::Level::DEBUG) < pImpl->level.load(std::memory_order_relaxed)) return;
    va_list ap; va_start(ap, fmt); log(Logger::Level::DEBUG, fmt, ap); va_end(ap);
}
void Logger::info(const char* fmt, ...) {
    if (static_cast<int>(Logger::Level::INFO) < pImpl->level.load(std::memory_order_relaxed)) return;
    va_list ap; va_start(ap, fmt); log(Logger::Level::INFO, fmt, ap); va_end(ap);
}
void Logger::warn(const char* fmt, ...) {
    if (static_cast<int>(Logger::Level::WARNING) < pImpl->level.load(std::memory_order_relaxed)) return;
    va_list ap; va_start(ap, fmt); log(Logger::Level::WARNING, fmt, ap); va_end(ap);
}
void Logger::error(const char* fmt, ...) {
    // Always allow error path to be called if compiled in (still subject to compile-time macro)
    va_list ap; va_start(ap, fmt); log(Logger::Level::ERROR, fmt, ap); va_end(ap);
}
