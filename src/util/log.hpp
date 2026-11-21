// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal category/level logger. Output goes to a replaceable sink so the
// interactive monitor can print log lines without corrupting its prompt.
#pragma once

#include <cstdarg>
#include <functional>
#include <string>

namespace tn34xx {

enum class LogLevel { Off = 0, Error, Warn, Info, Debug, Trace };

const char* log_level_name(LogLevel level);
bool parse_log_level(const std::string& text, LogLevel& out);

// Set the level for one category, or for all categories with "*" / "all".
void log_set_level(const std::string& category, LogLevel level);
LogLevel log_level(const char* category);
// Parse "cat=level,cat=level" (or a bare level for all categories). The
// item "time" prefixes every line with seconds since start-up.
bool log_configure(const std::string& spec, std::string& error);

bool log_enabled(const char* category, LogLevel level);

// gnu_printf on MinGW: see format() in util/strings.hpp.
void log_write(const char* category, LogLevel level, const char* fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 3, 4)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

void log_set_timestamps(bool on);

using LogSink = std::function<void(const std::string& line)>;
void log_set_sink(LogSink sink);      // nullptr restores stderr
bool log_open_file(const std::string& path, std::string& error);

} // namespace tn34xx

#define TN_LOG(cat, lvl, ...)                                                   \
    do {                                                                        \
        if (::tn34xx::log_enabled(cat, ::tn34xx::LogLevel::lvl))                \
            ::tn34xx::log_write(cat, ::tn34xx::LogLevel::lvl, __VA_ARGS__);     \
    } while (0)

#define LOG_ERROR(cat, ...) TN_LOG(cat, Error, __VA_ARGS__)
#define LOG_WARN(cat, ...) TN_LOG(cat, Warn, __VA_ARGS__)
#define LOG_INFO(cat, ...) TN_LOG(cat, Info, __VA_ARGS__)
#define LOG_DEBUG(cat, ...) TN_LOG(cat, Debug, __VA_ARGS__)
#define LOG_TRACE(cat, ...) TN_LOG(cat, Trace, __VA_ARGS__)
