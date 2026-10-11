#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "depthxr/settings.h"

namespace depthxr {

struct ParseResult {
    bool ok{false};
    ConfigDocument document;
    std::string error;
    // What a successful parse ignored, each named once: keys this layer does
    // not know, by JSON path ("Unknown field ignored: modules.framegen.presenter").
    std::vector<std::string> warnings;
};

ParseResult ParseConfig(std::string_view json_text);
ParseResult LoadConfigFromFile(const std::filesystem::path& path);

} // namespace depthxr
