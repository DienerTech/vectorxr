#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include "depthxr/settings.h"

namespace depthxr {

class Logger {
  public:
    void Initialize(const std::filesystem::path& log_path);
    void SetRetentionFiles(int retention_files);
    void SetLevel(LogLevel level);
    void Error(const std::string& message);
    void Info(const std::string& message);
    void Debug(const std::string& message);
    [[nodiscard]] std::filesystem::path ActiveLogPath() const;

  private:
    void PruneLocked();
    void Write(LogLevel level, const std::string& message);

    mutable std::mutex mutex_;
    std::filesystem::path base_log_path_;
    std::filesystem::path active_log_path_;
    LogLevel level_{LogLevel::Info};
    int retention_files_{7};
};

} // namespace depthxr
