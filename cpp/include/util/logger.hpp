// logger.hpp
#pragma once
// Public header for Logger
// - Declares Logger class API
// - Exposes exported C symbol get_global_logger()
// - Defines logging convenience macros with compile-time disabling
//
// Usage:
//   // host initializes
//   get_global_logger()->init_file("/tmp/myapp.log");
//   LOG_INFO("started: pid=%d", getpid());
//   LOG_DEBUG_LOC("debug value=%d", x); // includes file:line:function
//
// Build-time control:
//   - By default in non-NDEBUG (debug) builds, TRACE..ERROR enabled
//   - In NDEBUG builds, only ERROR enabled by default
//   - Override by adding -DLOG_COMPILE_LEVEL=<n> where n maps below.
//
// Note: Consumers should NOT delete or shutdown the global logger; host owns lifecycle.
// Example:
// #include "logger.hpp"
//
// int main() {
//     Logger* lg = get_global_logger();
//     lg->init_file("/tmp/myapp.log", true);
//     lg->set_level(Logger::Level::DEBUG);
//
//     LOG_INFO("Started pid=%d", getpid());
//     LOG_DEBUG_LOC("debugging value=%d", 42);
//     LOG_TRACE("trace: this might be verbose");
//     LOG_ERROR("fatal: %s", "something broke");
//     lg->shutdown();
// }

/*
===============================================================================
IMPORTANT NOTES & GOTCHAS — READ BEFORE USING THE LOGGER
===============================================================================

This section documents important runtime and build-time caveats that every
developer working with the project must understand. Additions/changes here
should be kept minimal and synchronized with implementation comments.

1) Single instance & plugins
   - The library exposes a single exported getter:
       extern "C" LOGGER_API Logger* get_global_logger();
     The host should initialize the global logger (open file or syslog) and
     pass the pointer to plugins when they are initialized.
   - Plugins MUST NOT create their own Logger instances (i.e., DO NOT call
     Logger::instance() and take ownership). Doing so may create multiple
     in-process logger instances leading to inconsistent behavior.

2) Ordering of initialization / lifecycle rules
   - Host owns the logger lifecycle: only the host should call shutdown().
   - Plugins should store the Logger* provided by the host and reuse it.
   - If a plugin cannot obtain the pointer from the host, it may call
     get_global_logger() as a fallback — but the host must have created the
     logger and exported the symbol so the pointer is the same.

3) fork() and threads (VERY important)
   - Do NOT fork() a multithreaded process unless you know what you are doing.
     The test tool and examples fork *before* creating additional threads.
   - If your code will fork child processes that log:
       * Prefer to call fork() before spawning application threads.
       * Or ensure only the child performs safe, single-threaded logging after fork.
   - After fork, child inherits file descriptors; the logger will continue to use
     the same file descriptor (and will inherit any locks). That may be okay but
     be mindful of buffered data and resources.

4) flock() vs O_APPEND atomicity
   - O_APPEND + a single write(fd, buffer, len) tends to be atomic at the kernel
     level across processes for individual write() syscalls.
   - flock() serializes writes across processes but **NOT** across multiple
     logger instances inside the *same process* (lock ownership is per-PID).
     If you have multiple instances in the same PID (plugins loaded with local
     symbols), flock will not protect you from concurrent writes between those
     instances.
   - Recommendation: prefer a single in-process logger instance (via exported
     get_global_logger()) and ensure each log call writes a single contiguous
     buffer in one write().

5) Buffering and durability
   - By default the logger writes directly via write() to avoid stdio buffering.
     If you enable stdio buffering or call fprintf/fputs somewhere, remember to
     flush (fflush) and call fsync if durability is needed.
   - Optionally you can enable fsync-per-write (expensive, but durable).

6) File rotation & concurrency
   - If multiple processes or instances take independent responsibility for
     rotating/renaming the log file, messages can be lost or end up in the
     wrong file. Rotation should be centrally managed (the host) or coordinated
     through a sentinel lock/file.
   - If you implement rotation in this logger, make sure the operation is
     atomic and that other writers re-open the new target safely.

7) Syslog
   - Syslogd generally serializes messages received via /dev/log or the
     syslog socket. If you switch to syslog, messages from different
     instances/processes are less likely to interleave. However, ordering
     relative to file writes is not guaranteed.
   - Some platforms behave differently: test on your target OS.

8) Compile-time log disabling
   - Use LOG_COMPILE_LEVEL to reduce log code compiled into production:
       * Set LOG_COMPILE_LEVEL to LOG_COMPILE_LEVEL_INFO to remove DEBUG/TRACE,
         but keep INFO+.
       * By default NDEBUG builds compile out verbose logs (leaving ERROR).
   - Even when a log macro is compiled out, do not rely on it for side effects.

9) Recommended usage rules (for this codebase)
   - Host initializes the logger once at startup and calls shutdown at exit.
   - Plugins should only use the provided Logger* (store it during init).
   - Format entire log lines into a single buffer and write in one call.
   - Use compile-time level control in release builds to eliminate excess logging
     overhead (but keep ERROR enabled).
   - If you need to support third-party plugins that cannot accept a host
     pointer, implement a broker (unix socket or shared memory writer) to
     centralize logging.

If you are unsure about any of these, talk to the platform owner before shipping.
===============================================================================
*/

#include <string>
#include <atomic>

#if defined(_WIN32) || defined(_WIN64)
  #if defined(LOGGER_EXPORTS)
    #define LOGGER_API __declspec(dllexport)
  #else
    #define LOGGER_API __declspec(dllimport)
  #endif
#else
  #if __GNUC__ >= 4
    #define LOGGER_API __attribute__((visibility("default")))
  #else
    #define LOGGER_API
  #endif
#endif

class LOGGER_API Logger {
public:
    enum class Level { TRACE = 0, DEBUG = 1, INFO = 2, WARNING = 3, ERROR = 4, NONE = 5 };

    // single-instance accessor (definition in logger.cpp)
    static Logger& instance();

    // Simple init APIs (return codes kept consistent with earlier design)
    bool init_file(const std::string& path, bool use_flock = false, int mode = 0644);
    void init_syslog(const char* ident = nullptr, int option = 0, int facility = 0);
    void shutdown();

    void set_level(Level lvl);
    Level level() const;

    void set_fsync_per_write(bool v);

    // Logging API (printf-style)
    void log(Level lvl, const char* fmt, ...);
    void trace(const char* fmt, ...); // convenience for TRACE
    void debug(const char* fmt, ...);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);

    int last_errno() const;

    // Non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger();
    ~Logger();
    struct Impl;
    Impl* pImpl;
};

// Exported C symbol for consumers (host defines and exports it in logger.cpp).
// Plugins and other modules call this to obtain the pointer.
// Keep the name stable across versions.
extern "C" LOGGER_API Logger* get_global_logger();

// -----------------------------------------------------------------------------
// Compile-time control of which macros are enabled
//
// LOG_COMPILE_LEVEL numeric mapping (lower -> more verbose):
//   0 = TRACE
//   1 = DEBUG
//   2 = INFO
//   3 = WARNING
//   4 = ERROR
//   5 = NONE
//
// Default behavior:
//   - If user defines LOG_COMPILE_LEVEL explicitly (via -D), that value is used.
//   - Else when NDEBUG is defined (release), default = 4 (ERROR only).
//   - Else (debug build) default = 0 (TRACE+).
//
// Example to force info+ in any build: -DLOG_COMPILE_LEVEL=2
#ifndef LOG_COMPILE_LEVEL
  #ifdef NDEBUG
    #define LOG_COMPILE_LEVEL 4
  #else
    #define LOG_COMPILE_LEVEL 0
  #endif
#endif

// Helper compile-level constants
#define LOG_COMPILE_LEVEL_TRACE   0
#define LOG_COMPILE_LEVEL_DEBUG   1
#define LOG_COMPILE_LEVEL_INFO    2
#define LOG_COMPILE_LEVEL_WARNING 3
#define LOG_COMPILE_LEVEL_ERROR   4
#define LOG_COMPILE_LEVEL_NONE    5

// -----------------------------------------------------------------------------
// Macro utilities
#define LOGGER_CONCAT_INTERNAL(a, b) a##b
#define LOGGER_UNIQUE_NAME(a) LOGGER_CONCAT_INTERNAL(a, __LINE__)

// Internal macro generator for level-enabled cases.
// Parameters:
//  LVL_ENUM: Logger::Level::DEBUG etc
//  METHOD: method name on Logger pointer (debug/info/)
//  FMT_AND_ARGS: format and variadic args
#define LOG_AT_LEVEL_RUNTIME(LVL_ENUM, METHOD, fmt, ...)                     \
    do {                                                                     \
        Logger* LOGGER_UNIQUE_NAME(_lg_) = get_global_logger();              \
        if (LOGGER_UNIQUE_NAME(_lg_) != nullptr) {                           \
            if (static_cast<int>(LOGGER_UNIQUE_NAME(_lg_)->level()) <=      \
                static_cast<int>(LVL_ENUM)) {                                \
                LOGGER_UNIQUE_NAME(_lg_)->METHOD(fmt, ##__VA_ARGS__);        \
            }                                                                \
        }                                                                    \
    } while (0)

// Variant with location prefix: the macro will prepend "[file:line:function] " to the message.
#define LOG_AT_LEVEL_RUNTIME_LOC(LVL_ENUM, METHOD, fmt, ...)                 \
    do {                                                                     \
        Logger* LOGGER_UNIQUE_NAME(_lg_) = get_global_logger();              \
        if (LOGGER_UNIQUE_NAME(_lg_) != nullptr) {                           \
            if (static_cast<int>(LOGGER_UNIQUE_NAME(_lg_)->level()) <=      \
                static_cast<int>(LVL_ENUM)) {                                \
                LOGGER_UNIQUE_NAME(_lg_)->METHOD("[%s:%d:%s] " fmt,          \
                   __FILE__, __LINE__, __func__, ##__VA_ARGS__);             \
            }                                                                \
        }                                                                    \
    } while (0)

// -----------------------------------------------------------------------------
// Public macros: compile-time enable/disable based on LOG_COMPILE_LEVEL.
// If a macro is disabled at compile-time, expand to (void)0 so it's compiled out.

#if LOG_COMPILE_LEVEL <= LOG_COMPILE_LEVEL_TRACE
  #define LOG_TRACE(fmt, ...) LOG_AT_LEVEL_RUNTIME(Logger::Level::TRACE, trace, fmt, ##__VA_ARGS__)
#else
  #define LOG_TRACE(fmt, ...) (void)0
#endif

#if LOG_COMPILE_LEVEL <= LOG_COMPILE_LEVEL_DEBUG
  #define LOG_DEBUG(fmt, ...) LOG_AT_LEVEL_RUNTIME(Logger::Level::DEBUG, debug, fmt, ##__VA_ARGS__)
  #define LOG_DEBUG_LOC(fmt, ...) LOG_AT_LEVEL_RUNTIME_LOC(Logger::Level::DEBUG, debug, fmt, ##__VA_ARGS__)
#else
  #define LOG_DEBUG(fmt, ...) (void)0
  #define LOG_DEBUG_LOC(fmt, ...) (void)0
#endif

#if LOG_COMPILE_LEVEL <= LOG_COMPILE_LEVEL_INFO
  #define LOG_INFO(fmt, ...) LOG_AT_LEVEL_RUNTIME(Logger::Level::INFO, info, fmt, ##__VA_ARGS__)
  #define LOG_INFO_LOC(fmt, ...) LOG_AT_LEVEL_RUNTIME_LOC(Logger::Level::INFO, info, fmt, ##__VA_ARGS__)
#else
  #define LOG_INFO(fmt, ...) (void)0
  #define LOG_INFO_LOC(fmt, ...) (void)0
#endif

#if LOG_COMPILE_LEVEL <= LOG_COMPILE_LEVEL_WARNING
  #define LOG_WARN(fmt, ...) LOG_AT_LEVEL_RUNTIME(Logger::Level::WARNING, warn, fmt, ##__VA_ARGS__)
#else
  #define LOG_WARN(fmt, ...) (void)0
#endif

// Errors are always available (do not compile out) even in release builds by default,
// but they still respect runtime null-check for get_global_logger.
#if LOG_COMPILE_LEVEL <= LOG_COMPILE_LEVEL_ERROR
  #define LOG_ERROR(fmt, ...) LOG_AT_LEVEL_RUNTIME(Logger::Level::ERROR, error, fmt, ##__VA_ARGS__)
#else
  // If LOG_COMPILE_LEVEL > ERROR then even ERROR macros are disabled at compile time:
  #define LOG_ERROR(fmt, ...) (void)0
#endif

// Conditional/log-if macros (compile-time aware)
#define LOG_DEBUG_IF(cond, fmt, ...) do { if (cond) LOG_DEBUG(fmt, ##__VA_ARGS__); } while (0)
#define LOG_INFO_IF(cond, fmt, ...)  do { if (cond) LOG_INFO(fmt, ##__VA_ARGS__); } while (0)

