#pragma once

#include <filesystem>
#include <string>

namespace depthxr {

std::filesystem::path GetCurrentExecutablePath();
std::string GetCurrentExecutableName();

} // namespace depthxr
