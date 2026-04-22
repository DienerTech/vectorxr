#include "depthxr/logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace depthxr {
namespace {

int Severity(LogLevel level) {
    switch (level) {
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

std::tm LocalTime(std::time_t time) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    return tm;
}

std::filesystem::path CreateSessionLogPath(const std::filesystem::path& base_log_path) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = LocalTime(time);

    std::ostringstream suffix;
    suffix << std::put_time(&tm, "%Y%m%d-%H%M%S");

    const std::filesystem::path directory = base_log_path.parent_path();
    const std::string stem = base_log_path.stem().string();
    const std::string extension = base_log_path.has_extension() ? base_log_path.extension().string() : ".log";

    return directory / (stem + "-" + suffix.str() + extension);
}

bool MatchesLogSeries(const std::filesystem::path& candidate, const std::filesystem::path& base_log_path) {
    if (!candidate.has_filename() || candidate.extension() != base_log_path.extension()) {
        return false;
    }

    const std::string stem = candidate.stem().string();
    const std::string prefix = base_log_path.stem().string();
    if (stem == prefix) {
        return true;
    }

    const std::string expected_prefix = prefix + "-";
    if (!stem.starts_with(expected_prefix)) {
        return false;
    }

    const std::string suffix = stem.substr(expected_prefix.size());
    if (suffix.size() != 15 || suffix[8] != '-') {
        return false;
    }

    for (size_t index = 0; index < suffix.size(); ++index) {
        if (index == 8) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(suffix[index]))) {
            return false;
        }
    }

    return true;
}

std::string FormatRepeatedDuration(std::chrono::system_clock::duration duration) {
    const auto seconds = std::max<int64_t>(
        1,
        std::chrono::duration_cast<std::chrono::seconds>(duration).count());
    return std::to_string(seconds) + "s";
}

} // namespace

Logger::~Logger() {
    std::scoped_lock lock(mutex_);
    FlushPendingDuplicateLocked();
}

void Logger::Initialize(const std::filesystem::path& log_path) {
    std::scoped_lock lock(mutex_);
    FlushPendingDuplicateLocked();
    base_log_path_ = log_path;

    if (!base_log_path_.parent_path().empty()) {
        std::filesystem::create_directories(base_log_path_.parent_path());
    }

    active_log_path_ = CreateSessionLogPath(base_log_path_);
    PruneLocked();
}

void Logger::SetRetentionFiles(int retention_files) {
    std::scoped_lock lock(mutex_);
    retention_files_ = std::max(1, retention_files);
    PruneLocked();
}

void Logger::SetLevel(LogLevel level) {
    std::scoped_lock lock(mutex_);
    FlushPendingDuplicateLocked();
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

std::filesystem::path Logger::ActiveLogPath() const {
    std::scoped_lock lock(mutex_);
    return active_log_path_;
}

void Logger::PruneLocked() {
    if (base_log_path_.empty()) {
        return;
    }

    const std::filesystem::path directory = base_log_path_.parent_path();
    if (directory.empty() || !std::filesystem::exists(directory)) {
        return;
    }

    std::vector<std::filesystem::directory_entry> log_files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (MatchesLogSeries(entry.path(), base_log_path_)) {
            log_files.push_back(entry);
        }
    }

    std::sort(log_files.begin(), log_files.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.last_write_time() > rhs.last_write_time();
    });

    for (size_t index = static_cast<size_t>(retention_files_); index < log_files.size(); ++index) {
        std::error_code error;
        std::filesystem::remove(log_files[index].path(), error);
    }
}

void Logger::FlushPendingDuplicateLocked() {
    if (pending_duplicate_count_ <= 1 || pending_duplicate_message_.empty()) {
        pending_duplicate_message_.clear();
        pending_duplicate_count_ = 0;
        pending_duplicate_first_time_ = {};
        pending_duplicate_last_time_ = {};
        return;
    }

    const auto now = pending_duplicate_last_time_;
    const auto repeated_count = pending_duplicate_count_ - 1;
    const auto repeated_duration = pending_duplicate_last_time_ - pending_duplicate_first_time_;

    std::ostringstream stream;
    stream << "Previous " << ToString(pending_duplicate_level_) << " message repeated "
           << repeated_count << " additional times over "
           << FormatRepeatedDuration(repeated_duration) << ": " << pending_duplicate_message_;
    WriteLineLocked(pending_duplicate_level_, now, stream.str());

    pending_duplicate_message_.clear();
    pending_duplicate_count_ = 0;
    pending_duplicate_first_time_ = {};
    pending_duplicate_last_time_ = {};
}

void Logger::WriteLineLocked(LogLevel level,
                             const std::chrono::system_clock::time_point& now,
                             const std::string& message) {
    if (active_log_path_.empty()) {
        return;
    }

    std::ofstream stream(active_log_path_, std::ios::app);
    if (!stream) {
        return;
    }

    const auto time = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = LocalTime(time);

    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << ToString(level) << "] " << message << '\n';
}

void Logger::Write(LogLevel level, const std::string& message) {
    std::scoped_lock lock(mutex_);

    if (Severity(level) > Severity(level_)) {
        return;
    }

    if (active_log_path_.empty()) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    if (pending_duplicate_count_ > 0 && pending_duplicate_level_ == level &&
        pending_duplicate_message_ == message) {
        ++pending_duplicate_count_;
        pending_duplicate_last_time_ = now;
        return;
    }

    FlushPendingDuplicateLocked();
    WriteLineLocked(level, now, message);

    pending_duplicate_level_ = level;
    pending_duplicate_message_ = message;
    pending_duplicate_count_ = 1;
    pending_duplicate_first_time_ = now;
    pending_duplicate_last_time_ = now;
}

} // namespace depthxr
