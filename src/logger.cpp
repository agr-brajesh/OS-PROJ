#include "logger.hpp"

#include <chrono>
#include <iomanip>
#include <filesystem>

namespace isp {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::get_level() const {
    return level_;
}

void Logger::log(LogLevel level, const std::string& file, int line, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level < level_) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
#if defined(_WIN32)
    localtime_s(&bt, &timer);
#else
    localtime_r(&timer, &bt);
#endif

    const char* level_str = "INFO";
    const char* color_code = "\033[0m";
    switch (level) {
        case LogLevel::DEBUG: level_str = "DEBUG"; color_code = "\033[36m"; break;
        case LogLevel::INFO:  level_str = "INFO "; color_code = "\033[32m"; break;
        case LogLevel::WARN:  level_str = "WARN "; color_code = "\033[33m"; break;
        case LogLevel::ERROR: level_str = "ERROR"; color_code = "\033[31m"; break;
    }

    std::string filename = std::filesystem::path(file).filename().string();

    std::ostream& out = (level == LogLevel::ERROR) ? std::cerr : std::cout;
    out << color_code
        << "[" << std::put_time(&bt, "%Y-%m-%d %H:%M:%S") << "."
        << std::setfill('0') << std::setw(3) << ms.count() << "] "
        << "[" << level_str << "] "
        << "[" << filename << ":" << line << "] "
        << "\033[0m"
        << message << "\n";
    out.flush();
}

} // namespace isp
