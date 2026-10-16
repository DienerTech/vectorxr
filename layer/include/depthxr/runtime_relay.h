#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace depthxr {

constexpr std::uint32_t kRuntimeRelayProtocolVersion = 1;

struct RuntimeControlDocument {
    std::uint32_t protocol_version{0};
    std::string target_session_id;
    std::uint64_t revision{0};
    std::uint64_t expires_at_unix_milliseconds{0};
    std::optional<bool> quadviews_diagnostic_visualization;
};

struct QuadViewDimensions {
    std::uint32_t width{0}, height{0}, allocated_width{0}, allocated_height{0};
    bool operator==(const QuadViewDimensions&) const = default;
};

struct RuntimeStatusDocument {
    bool osd_available{false}, osd_visible{false}, osd_compact{false};
    std::string osd_message;
    std::string turbo_state;
    std::string turbo_reason;
    std::string framegen_state;
    std::string framegen_reason;
    // FrameGenDepthState relay name ("in-use", "not-sent", ...), empty
    // before generated frames.
    std::string framegen_depth;
    std::vector<QuadViewDimensions> quadviews_dimensions;
    std::uint64_t quadviews_dimensions_at{0};
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

// Session ids begin "<process id>-<session start, Unix ms>-". Unknown or
// access-denied processes count as alive; a process created after the session
// started is a reused process id, not the session's.
bool RuntimeRelaySessionAlive(std::uint32_t process_id, std::uint64_t started_unix_milliseconds);
using RuntimeRelaySessionAliveCheck = std::function<bool(std::uint32_t, std::uint64_t)>;
// Removes the status and control files (and leftover temporary files) of
// sessions whose process is gone, once they were last written a day ago:
// until then Export Debug includes them. Games that exit without OpenXR
// teardown leave them behind; a hard shutdown can leave them zero-filled.
// Files whose name is not a session id are left alone. Returns how many were
// removed.
std::size_t PruneRuntimeRelayFiles(const std::filesystem::path& root,
                                   const RuntimeRelaySessionAliveCheck& alive = RuntimeRelaySessionAlive);

bool ReadRuntimeControl(const std::filesystem::path& path, RuntimeControlDocument* document, std::string* error = nullptr);
bool WriteRuntimeStatus(const std::filesystem::path& path, const RuntimeStatusDocument& document, std::string* error = nullptr);

} // namespace depthxr

