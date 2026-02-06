#pragma once

#include "plh_base.hpp"

namespace pylabhub::utils
{

using namespace pylabhub::format_tools;

// Forward declare to avoid circular dependencies
class Logger;

// Represents a single log message event.
struct LogMessage
{
    std::chrono::system_clock::time_point timestamp;
    uint64_t process_id;
    uint64_t thread_id;
    int level; // Use int to avoid including all of logger.hpp for the enum.
    fmt::memory_buffer body;
};

// Abstract interface for a log message destination.
class Sink
{
  public:
    enum WRITE_MODE
    {
        ASYNC_WRITE,
        SYNC_WRITE
    };

    virtual ~Sink() = default;
    virtual void write(const LogMessage &msg, Sink::WRITE_MODE mode) = 0;
    virtual void flush() = 0;
    virtual std::string description() const = 0;

    static const char *level_to_string_internal(int lvl);
    static std::string format_logmsg(const LogMessage &msg, Sink::WRITE_MODE mode);
};

} // namespace pylabhub::utils
