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

// Frame Generation's live rates as numbers, for tools that read the status
// file (framegen_reason is display text): averaged over about a second.
struct RuntimeFrameGenRates {
    std::string mode;            // "interpolate" or "extrapolate"
    std::string frame_rate;      // "2x", "3x", "4x", "auto" or "dynamic"
    bool generating{false};      // generated frames are being shown
    std::uint32_t multiplier{0}; // refreshes per real frame aimed for (Auto: its choice; Dynamic: typical)
    double refresh_hz{0.0};      // 0 until the runtime's period is known
    // The most the game is let run (refresh / multiplier) while a fixed or
    // Auto rate generates; 0 when nothing holds it. A slower game runs at its
    // own rate and every refresh is still filled.
    double hold_fps{0.0};
    double app_fps{0.0};      // real frames per second
    double output_fps{0.0};   // frames submitted to the runtime per second
    double distinct_fps{0.0}; // of those, frames showing something new
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
    std::string framegen_flow;
    std::string framegen_flow_mode;
    std::string framegen_flow_reason;
    // Present while the session runs with Frame Generation on.
    std::optional<RuntimeFrameGenRates> framegen_rates;
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

// The VectorXR app's Rescan inputs, for every running game at once
// (<root>/input-rescan.json): each looks again for bound input devices that
// were not connected. A request is taken once, and only while fresh, so a
// game started later never replays an old one.
constexpr std::uint64_t kInputRescanFreshMilliseconds = 10'000;
std::filesystem::path InputRescanRequestPath(const std::filesystem::path& root);
// The request's time (Unix ms); false when there is none or it is unreadable.
bool ReadInputRescanRequest(const std::filesystem::path& path, std::uint64_t* requested_at_unix_milliseconds);
// Whether a request at `requested_at` is new since `last_taken` and fresh at
// `now` (all Unix ms).
bool InputRescanRequestDue(std::uint64_t requested_at, std::uint64_t last_taken, std::uint64_t now);

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

