#pragma once

#include <string_view>

#include "depthxr/settings.h"

namespace depthxr {

bool ExeNameMatches(std::string_view lhs, std::string_view rhs);
const RegisteredApplication* FindMatchingApplication(const ConfigDocument& config, std::string_view exe_name);
const DepthXrProfile* FindMatchingDepthXrProfile(const ConfigDocument& config, std::string_view exe_name);
DepthXrResolvedSettings ResolveDepthXrSettings(const ConfigDocument& config, std::string_view exe_name);
const QuadViewsProfile* FindMatchingQuadViewsProfile(const ConfigDocument& config, std::string_view exe_name);
QuadViewsResolvedSettings ResolveQuadViewsSettings(const ConfigDocument& config, std::string_view exe_name);
ResolvedRuntimeConfig ResolveRuntimeConfig(const ConfigDocument& config, std::string_view exe_name);

} // namespace depthxr
