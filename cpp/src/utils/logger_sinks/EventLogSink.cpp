#include "utils/logger_sinks/EventLogSink.hpp"
#include "utils/logger_sinks/Sink.hpp"
#include <stdexcept>

#ifdef PYLABHUB_PLATFORM_WIN64
namespace pylabhub::utils
{

EventLogSink::EventLogSink(const wchar_t *source_name)
{
    handle_ = RegisterEventSourceW(nullptr, source_name);
    if (!handle_)
    {
        throw std::runtime_error("Failed to register event source");
    }
}

EventLogSink::~EventLogSink()
{
    if (handle_)
        DeregisterEventSource(handle_);
}

void EventLogSink::write(const LogMessage &msg, Sink::WRITE_MODE /*mode*/)
{
    if (!handle_)
        return;
    int needed = MultiByteToWideChar(CP_UTF8, 0, msg.body.data(),
                                     static_cast<int>(msg.body.size()), nullptr, 0);
    if (needed <= 0)
    {
        const wchar_t *empty_str = L"";
        ReportEventW(handle_, level_to_eventlog_type(msg.level), 0, 0, nullptr, 1, 0,
                     &empty_str, nullptr);
        return;
    }
    std::wstring wbody(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, msg.body.data(), static_cast<int>(msg.body.size()),
                        &wbody[0], needed);
    const wchar_t *strings[1] = {wbody.c_str()};
    ReportEventW(handle_, level_to_eventlog_type(msg.level), 0, 0, nullptr, 1, 0, strings,
                 nullptr);
}

void EventLogSink::flush()
{
    // No-op for event log
}

std::string EventLogSink::description() const
{
    return "Windows Event Log";
}

WORD EventLogSink::level_to_eventlog_type(int level)
{
    switch (level)
    {
    case 0: // TRACE
    case 1: // DEBUG
    case 2: // INFO
        return EVENTLOG_INFORMATION_TYPE;
    case 3: // WARNING
        return EVENTLOG_WARNING_TYPE;
    case 4: // ERROR
    case 5: // SYSTEM
        return EVENTLOG_ERROR_TYPE;
    default:
        return EVENTLOG_INFORMATION_TYPE;
    }
}

} // namespace pylabhub::utils
#endif
