#include "depthxr/process_info.h"

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include <array>

namespace depthxr {

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

} // namespace depthxr
