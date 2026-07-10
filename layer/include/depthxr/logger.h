#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "depthxr/settings.h"

namespace depthxr {

class Logger {
  public:
    ~Logger();

    void Initialize(const std::filesystem::path& log_path);
    void SetRetentionFiles(int retention_files);
    void SetLevel(LogLevel level);
    void Error(const std::string& message);
    void Info(const std::string& message);
    void Debug(const std::string& message);
    [[nodiscard]] bool IsDebugEnabled() const;
    [[nodiscard]] std::filesystem::path ActiveLogPath() const;

  private:
    void PruneLocked();
    void FlushPendingDuplicateLocked();
    void WriteLineLocked(LogLevel level,
                         const std::chrono::system_clock::time_point& now,
                         const std::string& message);
    void Write(LogLevel level, const std::string& message);

    mutable std::mutex mutex_;
    std::filesystem::path base_log_path_;
    std::filesystem::path active_log_path_;
    std::ofstream stream_;
    std::chrono::steady_clock::time_point last_stream_flush_time_{};
    LogLevel level_{LogLevel::Info};
    int retention_files_{7};
    LogLevel pending_duplicate_level_{LogLevel::Info};
    std::string pending_duplicate_message_;
    uint64_t pending_duplicate_count_{0};
    std::chrono::system_clock::time_point pending_duplicate_first_time_{};
    std::chrono::system_clock::time_point pending_duplicate_last_time_{};
};

} // namespace depthxr
