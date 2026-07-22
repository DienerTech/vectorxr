#include "depthxr/process_info.h"

#if defined(_WIN32)
#include <Windows.h>
#include <TlHelp32.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>

namespace depthxr {
namespace {

#if defined(_WIN32)
std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool ContainsAny(std::wstring_view value, std::initializer_list<std::wstring_view> fragments) {
    return std::any_of(fragments.begin(), fragments.end(), [value](std::wstring_view fragment) {
        return value.find(fragment) != std::wstring_view::npos;
    });
}

bool IsInteropModule(std::wstring_view lower_name) {
    constexpr std::array<std::wstring_view, 5> graphics_modules{
        L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"opengl32.dll", L"openvr_api.dll"};
    if (std::find(graphics_modules.begin(), graphics_modules.end(), lower_name) !=
        graphics_modules.end()) {
        return true;
    }
    if (lower_name.find(L"vectorxr") != std::wstring_view::npos) {
        return false;
    }
    return ContainsAny(lower_name,
                       {L"reshade", L"desktopxr", L"simhaptic", L"openxr", L"apilayer",
                        L"openkneeboard", L"quad_views", L"quad-views", L"xrnecksafer", L"vdxr",
                        L"pimaxxr", L"steamxr", L"oculus_openxr", L"varjo-openxr"});
}

bool IsInteropProcess(std::wstring_view lower_name) {
    return ContainsAny(lower_name,
                       {L"desktopxr", L"simhaptic", L"openkneeboard", L"xrnecksafer",
                        L"virtualdesktop", L"openxr-toolkit"});
}
#endif

} // namespace

std::filesystem::path GetCurrentExecutablePath() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
#else
    std::array<char, 4096> buffer{};
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    buffer[static_cast<size_t>(length)] = '\0';
    return std::filesystem::path(buffer.data());
#endif
}

std::string GetCurrentExecutableName() {
    return GetCurrentExecutablePath().filename().string();
}

ProcessInteropSnapshot GetProcessInteropSnapshot() {
    ProcessInteropSnapshot result;
#if defined(_WIN32)
    const HANDLE module_snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (module_snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(module_snapshot, &entry)) {
            result.module_scan_succeeded = true;
            do {
                const std::wstring lower_name = Lowercase(entry.szModule);
                if (IsInteropModule(lower_name)) {
                    result.modules.push_back({std::filesystem::path(entry.szModule).string(),
                                              std::filesystem::path(entry.szExePath)});
                }
            } while (Module32NextW(module_snapshot, &entry));
        }
        CloseHandle(module_snapshot);
    }

    const HANDLE process_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (process_snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(process_snapshot, &entry)) {
            result.process_scan_succeeded = true;
            do {
                const std::wstring lower_name = Lowercase(entry.szExeFile);
                if (IsInteropProcess(lower_name)) {
                    result.processes.push_back(
                        {std::filesystem::path(entry.szExeFile).string(), entry.th32ProcessID});
                }
            } while (Process32NextW(process_snapshot, &entry));
        }
        CloseHandle(process_snapshot);
    }

    std::sort(result.modules.begin(), result.modules.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name < rhs.name;
    });
    std::sort(result.processes.begin(), result.processes.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name < rhs.name;
    });
#endif
    return result;
}

} // namespace depthxr
