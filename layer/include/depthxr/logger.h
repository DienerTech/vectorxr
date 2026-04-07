#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include "depthxr/settings.h"

namespace depthxr {

class Logger {
  public:
    void Initialize(const std::filesystem::path& log_path);
    void SetLevel(LogLevel level);
    void Error(const std::string& message);
    void Info(const std::string& message);
    void Debug(const std::string& message);

  private:
    void Write(LogLevel level, const std::string& message);

    std::mutex mutex_;
    std::filesystem::path log_path_;
    LogLevel level_{LogLevel::Info};
};

} // namespace depthxr
