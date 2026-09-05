#include "util/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace gw {

namespace {
std::atomic<LogSink> g_sink{nullptr};
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_rateMutex;
std::unordered_map<const void*, std::chrono::steady_clock::time_point> g_lastLog;

const char* levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "?";
}
} // namespace

void setLogSink(LogSink sink)
{
    g_sink.store(sink);
}

LogLevel logLevel()
{
    return static_cast<LogLevel>(g_level.load());
}

void setLogLevel(LogLevel level)
{
    g_level.store(static_cast<int>(level));
}

void logMessage(LogLevel level, const std::string& text)
{
    if (static_cast<int>(level) < g_level.load()) {
        return;
    }
    const LogSink sink = g_sink.load();
    if (sink != nullptr) {
        sink(level, text.c_str());
        return;
    }
    std::fprintf(level >= LogLevel::Warning ? stderr : stdout, "[%s] %s\n",
                 levelName(level), text.c_str());
}

bool logRateLimited(const void* key, const char* text, int windowMs)
{
    const auto now = std::chrono::steady_clock::now();
    {
        const std::lock_guard<std::mutex> lock(g_rateMutex);
        const auto it = g_lastLog.find(key);
        if (it != g_lastLog.end()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->second);
            if (elapsed.count() < static_cast<std::int64_t>(windowMs)) {
                return false;
            }
            it->second = now;
        } else {
            g_lastLog.emplace(key, now);
        }
    }
    logMessage(LogLevel::Warning, text);
    return true;
}

} // namespace gw
