#include "utils/logger_sinks/SyslogSink.hpp"
#include "utils/logger_sinks/Sink.hpp"

#if !defined(PLATFORM_WIN64)
#include <syslog.h>

namespace pylabhub::utils
{

SyslogSink::SyslogSink(const char *ident, int option, int facility)
{
    openlog(ident, option, facility);
}

SyslogSink::~SyslogSink()
{
    closelog();
}

void SyslogSink::write(const LogMessage &msg, Sink::WRITE_MODE mode)
{
    auto strmsg = format_logmsg(msg, mode);
    syslog(level_to_syslog_priority(msg.level), "%.*s", (int)strmsg.size(), strmsg.data());
}

void SyslogSink::flush()
{
    // No-op for syslog
}

std::string SyslogSink::description() const
{
    return "Syslog";
}

int SyslogSink::level_to_syslog_priority(int level)
{
    switch (level)
    {
    case 0: // TRACE
        return LOG_DEBUG;
    case 1: // DEBUG
        return LOG_DEBUG;
    case 2: // INFO
        return LOG_INFO;
    case 3: // WARNING
        return LOG_WARNING;
    case 4: // ERROR
        return LOG_ERR;
    case 5: // SYSTEM
        return LOG_CRIT;
    default:
        return LOG_INFO;
    }
}

} // namespace pylabhub::utils
#endif
