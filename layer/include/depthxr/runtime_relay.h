#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace depthxr {

constexpr std::uint32_t kRuntimeRelayProtocolVersion = 1;

struct RuntimeControlDocument {
    std::uint32_t protocol_version{0};
    std::string target_session_id;
    std::uint64_t revision{0};
    std::uint64_t expires_at_unix_milliseconds{0};
    std::optional<bool> quadviews_diagnostic_visualization;
};

struct RuntimeStatusDocument {
    std::string session_id;
    std::uint32_t process_id{0};
    std::string application;
    std::uint64_t updated_at_unix_milliseconds{0};
    std::uint64_t acknowledged_revision{0};
    bool quadviews_diagnostic_visualization_available{false};
    bool quadviews_diagnostic_visualization_enabled{false};
};

std::filesystem::path ResolveRuntimeRelayRoot();
std::filesystem::path RuntimeControlPath(const std::filesystem::path& root, const std::string& session_id);
std::filesystem::path RuntimeStatusPath(const std::filesystem::path& root, const std::string& session_id);
std::uint64_t RuntimeRelayUnixMilliseconds();

bool ReadRuntimeControl(const std::filesystem::path& path, RuntimeControlDocument* document, std::string* error = nullptr);
bool WriteRuntimeStatus(const std::filesystem::path& path, const RuntimeStatusDocument& document, std::string* error = nullptr);

} // namespace depthxr

