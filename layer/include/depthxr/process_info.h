#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace depthxr {

struct LoadedInteropModule {
    std::string name;
    std::filesystem::path path;
};

struct RunningInteropProcess {
    std::string name;
    std::uint32_t process_id{0};
};

struct ProcessInteropSnapshot {
    bool module_scan_succeeded{false};
    bool process_scan_succeeded{false};
    std::vector<LoadedInteropModule> modules;
    std::vector<RunningInteropProcess> processes;
};

std::filesystem::path GetCurrentExecutablePath();
std::string GetCurrentExecutableName();
// Returns a deliberately bounded view of the graphics/OpenXR/input stack:
// known in-process hooks/API layers and known companion VR or input applications.
// It never emits an unrestricted module or process list into a support log.
ProcessInteropSnapshot GetProcessInteropSnapshot();

} // namespace depthxr
