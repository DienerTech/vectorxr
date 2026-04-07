#pragma once

#include <string_view>

#include "depthxr/settings.h"

namespace depthxr {

bool ExeNameMatches(std::string_view lhs, std::string_view rhs);
const Profile* FindMatchingProfile(const ConfigDocument& config, std::string_view exe_name);
ResolvedSettings ResolveSettings(const ConfigDocument& config, std::string_view exe_name);

} // namespace depthxr
