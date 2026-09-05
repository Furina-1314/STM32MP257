#ifndef GW_UTIL_LOG_HPP
#define GW_UTIL_LOG_HPP

#include <cstdint>
#include <string>

namespace gw {

// Minimal leveled logger. The default sink writes to stdout/stderr (journald
// captures it under systemd); install a custom sink for tests. Rate-limited
// warnings keep a missing sensor from flooding the log.
enum class LogLevel
{
    Debug,
    Info,
    Warning,
    Error,
};

using LogSink = void (*)(LogLevel level, const char* text);

void setLogSink(LogSink sink);
LogLevel logLevel();
void setLogLevel(LogLevel level);

void logMessage(LogLevel level, const std::string& text);

// True at most once per (key, windowMs); used for recurring fault logging.
bool logRateLimited(const void* key, const char* text, int windowMs);

#define GW_LOG(level, text) logMessage(level, text)

} // namespace gw

#endif // GW_UTIL_LOG_HPP
