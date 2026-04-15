#include "depthxr/config_path.h"

#include <cstdlib>

namespace depthxr {
namespace {

std::filesystem::path FallbackConfigPath() {
    return std::filesystem::current_path() / "config" / "vectorxr.settings.json";
}

std::filesystem::path FallbackLogPath() {
    return std::filesystem::current_path() / "logs" / "vectorxr.log";
}

} // namespace

std::filesystem::path ResolveConfigPath() {
    if (const char* env_path = std::getenv("VECTORXR_CONFIG_PATH"); env_path && *env_path != '\0') {
        return std::filesystem::path(env_path);
    }

#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "VectorXR" / "config" / "settings.json";
    }
#endif

    return FallbackConfigPath();
}

std::filesystem::path ResolveSeenAppsPath() {
    if (const char* env_path = std::getenv("VECTORXR_SEEN_APPS_PATH"); env_path && *env_path != '\0') {
        return std::filesystem::path(env_path);
    }

#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "VectorXR" / "config" / "seen-apps.json";
    }
#endif

    return std::filesystem::current_path() / "config" / "seen-apps.json";
}

std::filesystem::path ResolveLogPath() {
    if (const char* env_path = std::getenv("VECTORXR_LOG_PATH"); env_path && *env_path != '\0') {
        return std::filesystem::path(env_path);
    }

#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "VectorXR" / "logs" / "vectorxr.log";
    }
#endif

    return FallbackLogPath();
}

} // namespace depthxr
