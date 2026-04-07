#include "depthxr/config_path.h"

#include <cstdlib>

namespace depthxr {
namespace {

std::filesystem::path FallbackConfigPath() {
    return std::filesystem::current_path() / "config" / "depthxr.settings.json";
}

std::filesystem::path FallbackLogPath() {
    return std::filesystem::current_path() / "logs" / "depthxr-layer.log";
}

} // namespace

std::filesystem::path ResolveConfigPath() {
    if (const char* env_path = std::getenv("DEPTHXR_CONFIG_PATH"); env_path && *env_path != '\0') {
        return std::filesystem::path(env_path);
    }

#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "DepthXR" / "config" / "settings.json";
    }
#endif

    return FallbackConfigPath();
}

std::filesystem::path ResolveLogPath() {
    if (const char* env_path = std::getenv("DEPTHXR_LOG_PATH"); env_path && *env_path != '\0') {
        return std::filesystem::path(env_path);
    }

#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "DepthXR" / "logs" / "depthxr-layer.log";
    }
#endif

    return FallbackLogPath();
}

} // namespace depthxr
