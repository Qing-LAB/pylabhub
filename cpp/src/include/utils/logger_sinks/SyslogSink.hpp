#pragma once

#include "Sink.hpp"

namespace pylabhub::utils
{

#if !defined(PLATFORM_WIN64)
class SyslogSink : public Sink
{
  public:
    SyslogSink(const char *ident, int option, int facility);
    ~SyslogSink() override;
    void write(const LogMessage &msg, Sink::WRITE_MODE mode) override;
    void flush() override;
    std::string description() const override;

  private:
    static int level_to_syslog_priority(int level);
};
#endif

} // namespace pylabhub::utils
