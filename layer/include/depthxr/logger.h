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

    // Starts a session log. It prunes the folder only once a retention was
    // set (an earlier session in the process): the first session's pruning
    // waits for the configured value.
    void Initialize(const std::filesystem::path& log_path);
    // Prunes on the first call after Initialize and when the value changes,
    // not on every config reload (those run on the game's frame thread).
    void SetRetentionFiles(int retention_files);
    void SetLevel(LogLevel level);
    void Error(const std::string& message);
    void Info(const std::string& message);
    void Debug(const std::string& message);
    // Background diagnostics may skip an ordinary-log copy under contention.
    bool TryDebug(const std::string& message);
    [[nodiscard]] bool IsDebugEnabled() const;
    [[nodiscard]] std::filesystem::path ActiveLogPath() const;
    // Removes the session log unless an error was written to it, and writes
    // nothing more until the next Initialize. Returns whether it was removed.
    bool DiscardSessionLog();

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
    bool retention_configured_{false};
    bool retention_applied_{false};
    bool wrote_error_{false};
    LogLevel pending_duplicate_level_{LogLevel::Info};
    std::string pending_duplicate_message_;
    uint64_t pending_duplicate_count_{0};
    std::chrono::system_clock::time_point pending_duplicate_first_time_{};
    std::chrono::system_clock::time_point pending_duplicate_last_time_{};
};

} // namespace depthxr
