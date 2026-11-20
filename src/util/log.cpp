#include "util/log.hpp"

#include "util/strings.hpp"

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>

namespace tn34xx {

namespace {

struct LogState {
    std::mutex mutex;
    LogLevel default_level = LogLevel::Info;
    std::map<std::string, LogLevel> levels;
    LogSink sink;
    FILE* file = nullptr;
    bool timestamps = false;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

LogState& state() {
    static LogState s;
    return s;
}

} // namespace

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Off: return "off";
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
    case LogLevel::Trace: return "trace";
    }
    return "?";
}

bool parse_log_level(const std::string& text, LogLevel& out) {
    const std::string t = to_lower(text);
    if (t == "off" || t == "none") out = LogLevel::Off;
    else if (t == "error") out = LogLevel::Error;
    else if (t == "warn" || t == "warning") out = LogLevel::Warn;
    else if (t == "info") out = LogLevel::Info;
    else if (t == "debug") out = LogLevel::Debug;
    else if (t == "trace") out = LogLevel::Trace;
    else return false;
    return true;
}

void log_set_level(const std::string& category, LogLevel level) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (category == "*" || category == "all") {
        s.default_level = level;
        s.levels.clear();
    } else {
        s.levels[category] = level;
    }
}

LogLevel log_level(const char* category) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.levels.find(category);
    return it != s.levels.end() ? it->second : s.default_level;
}

bool log_configure(const std::string& spec, std::string& error) {
    for (const std::string& item : split(spec, ',')) {
        const std::string part = trim(item);
        if (part.empty()) continue;
        if (to_lower(part) == "time") {
            log_set_timestamps(true);
            continue;
        }
        const auto eq = part.find('=');
        LogLevel level;
        if (eq == std::string::npos) {
            if (!parse_log_level(part, level)) { error = "unknown log level '" + part + "'"; return false; }
            log_set_level("*", level);
        } else {
            const std::string cat = trim(part.substr(0, eq));
            if (!parse_log_level(trim(part.substr(eq + 1)), level)) {
                error = "unknown log level in '" + part + "'";
                return false;
            }
            log_set_level(cat, level);
        }
    }
    return true;
}

bool log_enabled(const char* category, LogLevel level) {
    return level != LogLevel::Off && level <= log_level(category);
}

void log_write(const char* category, LogLevel level, const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    LogState& s = state();
    std::string line;
    if (s.timestamps) {
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
        line = format("%10.6f ", t);
    }
    line += std::string("[") + category + "] ";
    if (level <= LogLevel::Warn) line += std::string(log_level_name(level)) + ": ";
    line += body;

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        std::fprintf(s.file, "%s\n", line.c_str());
        std::fflush(s.file);
    }
    if (s.sink) {
        s.sink(line);
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

void log_set_timestamps(bool on) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.timestamps = on;
}

void log_set_sink(LogSink sink) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.sink = std::move(sink);
}

bool log_open_file(const std::string& path, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { error = "cannot open log file '" + path + "'"; return false; }
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) std::fclose(s.file);
    s.file = f;
    return true;
}

} // namespace tn34xx
