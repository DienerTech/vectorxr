#pragma once

#include <filesystem>

namespace depthxr {

std::filesystem::path ResolveConfigPath();
std::filesystem::path ResolveLogPath();

} // namespace depthxr
