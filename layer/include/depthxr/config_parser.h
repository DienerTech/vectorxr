#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "depthxr/settings.h"

namespace depthxr {

struct ParseResult {
    bool ok{false};
    ConfigDocument document;
    std::string error;
};

ParseResult ParseConfig(std::string_view json_text);
ParseResult LoadConfigFromFile(const std::filesystem::path& path);

} // namespace depthxr
