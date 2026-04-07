#include "depthxr/logger.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace depthxr {
namespace {

int Severity(LogLevel level) {
    switch (level) {
    case LogLevel::Off:
        return 0;
    case LogLevel::Error:
        return 1;
    case LogLevel::Info:
        return 2;
    case LogLevel::Debug:
        return 3;
    default:
        return 2;
    }
}

} // namespace

void Logger::Initialize(const std::filesystem::path& log_path) {
    std::scoped_lock lock(mutex_);
    log_path_ = log_path;

    if (!log_path_.parent_path().empty()) {
        std::filesystem::create_directories(log_path_.parent_path());
    }
}

void Logger::SetLevel(LogLevel level) {
    std::scoped_lock lock(mutex_);
    level_ = level;
}

void Logger::Error(const std::string& message) {
    Write(LogLevel::Error, message);
}

void Logger::Info(const std::string& message) {
    Write(LogLevel::Info, message);
}

void Logger::Debug(const std::string& message) {
    Write(LogLevel::Debug, message);
}

void Logger::Write(LogLevel level, const std::string& message) {
    std::scoped_lock lock(mutex_);

    if (Severity(level) > Severity(level_) || level_ == LogLevel::Off) {
        return;
    }

    if (log_path_.empty()) {
        return;
    }

    std::ofstream stream(log_path_, std::ios::app);
    if (!stream) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << ToString(level) << "] " << message << '\n';
}

} // namespace depthxr
