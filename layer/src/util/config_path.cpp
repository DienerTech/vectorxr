#include "depthxr/config_path.h"

#include <cstdlib>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace depthxr {
namespace {

std::filesystem::path FallbackConfigPath() {
    return std::filesystem::current_path() / "config" / "vectorxr.settings.json";
}

std::filesystem::path FallbackLogPath() {
    return std::filesystem::current_path() / "logs" / "vectorxr.log";
}

} // namespace

std::filesystem::path LocalAppDataPath() {
#if defined(_WIN32)
    return EnvironmentPath(L"LOCALAPPDATA");
#else
    return {};
#endif
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
#if defined(_WIN32)
    if (const wchar_t* value = _wgetenv(name); value && *value != L'\0') {
        return std::filesystem::path(value);
    }
#else
    const std::wstring wide(name);
    if (const char* value = std::getenv(std::string(wide.begin(), wide.end()).c_str()); value && *value != '\0') {
        return std::filesystem::path(value);
    }
#endif
    return {};
}

std::string PathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring& wide = path.native();
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
#else
    return path.string();
#endif
}

std::filesystem::path ResolveConfigPath() {
    if (std::filesystem::path env_path = EnvironmentPath(L"VECTORXR_CONFIG_PATH"); !env_path.empty()) {
        return env_path;
    }

#if defined(_WIN32)
    if (const std::filesystem::path local_app_data = LocalAppDataPath(); !local_app_data.empty()) {
        return local_app_data / "VectorXR" / "config" / "settings.json";
    }
#endif

    return FallbackConfigPath();
}

std::filesystem::path ResolveSeenAppsPath() {
    if (std::filesystem::path env_path = EnvironmentPath(L"VECTORXR_SEEN_APPS_PATH"); !env_path.empty()) {
        return env_path;
    }

#if defined(_WIN32)
    if (const std::filesystem::path local_app_data = LocalAppDataPath(); !local_app_data.empty()) {
        return local_app_data / "VectorXR" / "config" / "seen-apps.json";
    }
#endif

    return std::filesystem::current_path() / "config" / "seen-apps.json";
}

std::filesystem::path ResolveLogPath() {
    if (std::filesystem::path env_path = EnvironmentPath(L"VECTORXR_LOG_PATH"); !env_path.empty()) {
        return env_path;
    }

#if defined(_WIN32)
    if (const std::filesystem::path local_app_data = LocalAppDataPath(); !local_app_data.empty()) {
        return local_app_data / "VectorXR" / "logs" / "vectorxr.log";
    }
#endif

    return FallbackLogPath();
}

} // namespace depthxr
