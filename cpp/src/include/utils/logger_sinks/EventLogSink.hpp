#pragma once

#include "Sink.hpp"

#ifdef PYLABHUB_PLATFORM_WIN64
#include <windows.h>
#endif

namespace pylabhub::utils
{

#ifdef PYLABHUB_PLATFORM_WIN64
class EventLogSink : public Sink
{
  public:
    EventLogSink(const wchar_t *source_name);
    ~EventLogSink() override;
    void write(const LogMessage &msg, Sink::WRITE_MODE mode) override;
    void flush() override;
    std::string description() const override;

  private:
    HANDLE handle_ = nullptr;
    static WORD level_to_eventlog_type(int level);
};
#endif

} // namespace pylabhub::utils
