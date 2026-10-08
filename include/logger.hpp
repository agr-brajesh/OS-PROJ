#pragma once

#include <string>
#include <sstream>
#include <mutex>
#include <iostream>

namespace isp {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    LogLevel get_level() const;

    void log(LogLevel level, const std::string& file, int line, const std::string& message);

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel level_{LogLevel::INFO};
    std::mutex mutex_;
};

#define ISP_LOG_DEBUG(msg) do { \
    if (isp::Logger::instance().get_level() <= isp::LogLevel::DEBUG) { \
        std::ostringstream _oss; _oss << msg; \
        isp::Logger::instance().log(isp::LogLevel::DEBUG, __FILE__, __LINE__, _oss.str()); \
    } \
} while(0)

#define ISP_LOG_INFO(msg) do { \
    if (isp::Logger::instance().get_level() <= isp::LogLevel::INFO) { \
        std::ostringstream _oss; _oss << msg; \
        isp::Logger::instance().log(isp::LogLevel::INFO, __FILE__, __LINE__, _oss.str()); \
    } \
} while(0)

#define ISP_LOG_WARN(msg) do { \
    if (isp::Logger::instance().get_level() <= isp::LogLevel::WARN) { \
        std::ostringstream _oss; _oss << msg; \
        isp::Logger::instance().log(isp::LogLevel::WARN, __FILE__, __LINE__, _oss.str()); \
    } \
} while(0)

#define ISP_LOG_ERROR(msg) do { \
    if (isp::Logger::instance().get_level() <= isp::LogLevel::ERROR) { \
        std::ostringstream _oss; _oss << msg; \
        isp::Logger::instance().log(isp::LogLevel::ERROR, __FILE__, __LINE__, _oss.str()); \
    } \
} while(0)

} // namespace isp
