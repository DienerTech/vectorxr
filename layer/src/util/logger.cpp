#include "depthxr/logger.h"

#include <algorithm>
#include <chrono>
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
    // Built as a path: a name outside the ANSI code page has no narrow form.
    std::filesystem::path name = base_log_path.stem();
    name += "-" + suffix.str();
    name += base_log_path.has_extension() ? base_log_path.extension() : std::filesystem::path(".log");

    return directory / name;
}

bool MatchesLogSeries(const std::filesystem::path& candidate, const std::filesystem::path& base_log_path) {
    if (!candidate.has_filename() || candidate.extension() != base_log_path.extension()) {
        return false;
    }

    // Compared wide: any file in the log folder may be named outside the ANSI
    // code page.
    const std::wstring stem = candidate.stem().wstring();
    const std::wstring prefix = base_log_path.stem().wstring();
    if (stem == prefix) {
        return true;
    }

    const std::wstring expected_prefix = prefix + L"-";
    if (!stem.starts_with(expected_prefix)) {
        return false;
    }

    const std::wstring suffix = stem.substr(expected_prefix.size());
    if (suffix.size() != 15 || suffix[8] != L'-') {
        return false;
    }

    for (size_t index = 0; index < suffix.size(); ++index) {
        if (index == 8) {
            continue;
        }
        if (suffix[index] < L'0' || suffix[index] > L'9') {
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
    if (stream_.is_open()) {
        stream_.flush();
    }
}

void Logger::Initialize(const std::filesystem::path& log_path) {
    std::scoped_lock lock(mutex_);
    FlushPendingDuplicateLocked();
    base_log_path_ = log_path;

    if (!base_log_path_.parent_path().empty()) {
        // A folder that cannot be made leaves the logger without a file,
        // rather than throwing out of xrCreateInstance.
        std::error_code error;
        std::filesystem::create_directories(base_log_path_.parent_path(), error);
    }

    if (stream_.is_open()) {
        stream_.close();
    }
    active_log_path_ = CreateSessionLogPath(base_log_path_);
    last_stream_flush_time_ = {};
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

bool Logger::TryDebug(const std::string& message) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock) return false;
    if (Severity(LogLevel::Debug) > Severity(level_) || active_log_path_.empty()) return true;
    FlushPendingDuplicateLocked();
    WriteLineLocked(LogLevel::Debug, std::chrono::system_clock::now(), message);
    return true;
}

bool Logger::TryError(const std::string& message) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock) return false;
    if (active_log_path_.empty()) return true;
    FlushPendingDuplicateLocked();
    WriteLineLocked(LogLevel::Error, std::chrono::system_clock::now(), message);
    return true;
}

bool Logger::IsDebugEnabled() const {
    std::scoped_lock lock(mutex_);
    return Severity(LogLevel::Debug) <= Severity(level_);
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
    std::error_code error;
    if (directory.empty() || !std::filesystem::exists(directory, error)) {
        return;
    }

    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> log_files;
    for (auto iterator = std::filesystem::directory_iterator(directory, error);
         !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        std::error_code entry_error;
        if (!iterator->is_regular_file(entry_error) || !MatchesLogSeries(iterator->path(), base_log_path_)) {
            continue;
        }
        const auto write_time = iterator->last_write_time(entry_error);
        if (!entry_error) {
            log_files.emplace_back(write_time, iterator->path());
        }
    }

    std::sort(log_files.begin(), log_files.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first > rhs.first;
    });

    for (size_t index = static_cast<size_t>(retention_files_); index < log_files.size(); ++index) {
        error.clear();
        const auto& old_log = log_files[index].second;
        const bool removed = std::filesystem::remove(old_log, error);
        if (removed && !error) {
            // Recorder parts belong to this session and share its retention.
            for (unsigned part = 0; part < 2; ++part) {
                auto recorder = old_log;
                recorder.replace_extension(".flight-recorder." + std::to_string(part) + ".log");
                std::filesystem::remove(recorder, error);
                error.clear();
            }
            // Incident snapshots: the latest, two previous, and a temporary;
            // Frame Generation snapshots: the latest, one previous, and a
            // temporary; the Frame Generation trace.
            for (const char* suffix : {".flight-recorder.stall.log", ".flight-recorder.stall-1.log",
                                       ".flight-recorder.stall-2.log", ".flight-recorder.stall.log.tmp",
                                       ".flight-recorder.framegen.log", ".flight-recorder.framegen-1.log",
                                       ".flight-recorder.framegen.log.tmp", ".framegen-trace.csv"}) {
                auto snapshot = old_log;
                snapshot.replace_extension(suffix);
                std::filesystem::remove(snapshot, error);
                error.clear();
            }
        }
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

    // Reuse one open handle instead of reopening the file per line. Debug and
    // heartbeat messages can originate on the frame path, so batch ordinary
    // flushes briefly; errors remain immediately durable.
    if (!stream_.is_open()) {
        stream_.open(active_log_path_, std::ios::app);
        if (!stream_.is_open()) {
            return;
        }
    }

    const auto time = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = LocalTime(time);

    stream_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << ToString(level) << "] " << message << '\n';
    constexpr std::chrono::milliseconds kFlushInterval{250};
    const auto flush_now = std::chrono::steady_clock::now();
    if (level == LogLevel::Error || last_stream_flush_time_ == std::chrono::steady_clock::time_point{} ||
        flush_now - last_stream_flush_time_ >= kFlushInterval) {
        stream_.flush();
        last_stream_flush_time_ = flush_now;
    }
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
