#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace depthxr {

bool RecordSeenAppObservation(const std::filesystem::path& path,
                              const std::string& exe_name,
                              std::int64_t unix_seconds,
                              std::string* error = nullptr);
bool RecordSeenApp(const std::string& exe_name, std::string* error = nullptr);

} // namespace depthxr
