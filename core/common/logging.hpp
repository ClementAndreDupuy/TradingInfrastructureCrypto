#pragma once

#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace trading {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static LogLevel& min_level() {
        static LogLevel level = LogLevel::INFO;
        return level;
    }

    template<typename... Args>
    static void log(LogLevel level, const char* msg, Args&&... args) {
        if (level < min_level()) return;

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        auto timer = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&timer), "%Y-%m-%d %H:%M:%S");
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        oss << " [" << level_to_string(level) << "] " << msg;

        append_fields(oss, std::forward<Args>(args)...);
        std::cout << oss.str() << std::endl;
    }

private:
    static const char* level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }

    template<typename Key, typename Value, typename... Rest>
    static void append_fields(std::ostringstream& oss, Key&& key, Value&& value, Rest&&... rest) {
        oss << " " << key << "=" << value;
        append_fields(oss, std::forward<Rest>(rest)...);
    }

    static void append_fields(std::ostringstream&) {}
};

}  // namespace trading

#define LOG_DEBUG(msg, ...) trading::Logger::log(trading::LogLevel::DEBUG, msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...)  trading::Logger::log(trading::LogLevel::INFO, msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...)  trading::Logger::log(trading::LogLevel::WARN, msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) trading::Logger::log(trading::LogLevel::ERROR, msg, ##__VA_ARGS__)
