#include "depthxr/settings.h"

#include <algorithm>

namespace depthxr {

const char* ToString(LogLevel level) {
    switch (level) {
    case LogLevel::Off:
        return "off";
    case LogLevel::Error:
        return "error";
    case LogLevel::Info:
        return "info";
    case LogLevel::Debug:
        return "debug";
    default:
        return "info";
    }
}

std::optional<LogLevel> ParseLogLevel(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (normalized == "off") {
        return LogLevel::Off;
    }
    if (normalized == "error") {
        return LogLevel::Error;
    }
    if (normalized == "info") {
        return LogLevel::Info;
    }
    if (normalized == "debug") {
        return LogLevel::Debug;
    }

    return std::nullopt;
}

} // namespace depthxr
