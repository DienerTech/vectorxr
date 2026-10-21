#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace depthxr {

// The public status file for other programs (%LOCALAPPDATA%\VectorXR\api\
// status.json; docs/status-api.md): its own document, not the runtime relay's
// status, so the relay can change freely. Version 1 only ever gains fields; a
// breaking change would write status-v2.json beside it for a while.
constexpr const char* kExternalStatusSchema = "vectorxr.status";
constexpr std::uint32_t kExternalStatusVersion = 1;

// Auto's current step and where it steps next (FrameGenStatus).
struct ExternalAutoStep {
    std::uint32_t step{1};          // refreshes per real frame aimed for; 1 = full rate, nothing generated
    double step_up_below_fps{0.0};  // the game's rate below which it steps up; 0 with no higher step
    double step_down_above_fps{0.0}; // the game's own rate above which it tries a step down; 0 at full rate
};

struct ExternalFrameGenStatus {
    std::string state;      // active, standby, waiting, paused, unavailable or off (as the app says)
    std::string mode;       // "interpolate" or "extrapolate"
    std::string frame_rate; // "2x", "3x", "4x", "auto" or "dynamic"
    bool generating{false}; // generated frames are being shown
    std::uint32_t multiplier{0};
    double refresh_hz{0.0}; // 0 until the runtime's refresh period is known
    // The game's frame rate to aim for now (ExternalFrameGenTarget).
    double target_app_fps{0.0};
    double app_fps{0.0};
    double output_fps{0.0};
    double distinct_fps{0.0};
    // Headset refreshes per real frame, measured frame by frame (0 until
    // measured), and whether the game keeps up with target_app_fps.
    double refreshes_per_real_frame{0.0};
    bool holding{false};
    std::optional<ExternalAutoStep> automatic; // under Auto only
};

struct ExternalStatusDocument {
    std::uint32_t process_id{0};
    std::string application;
    std::string session_id;
    std::uint64_t updated_at_unix_milliseconds{0};
    // Present while the session runs with Frame Generation on.
    std::optional<ExternalFrameGenStatus> framegen;
};

// Fills target_app_fps and holding from generating, multiplier, refresh_hz
// and refreshes_per_real_frame. Generating, the game is let run at
// refresh / multiplier (a fixed rate's hold, Auto's current step, or the rate
// at which Dynamic's typical multiplier holds); otherwise its frames pass
// straight through and it may run at the refresh rate. It holds that rate
// while its real frames take at most a tenth more refreshes than aimed for.
void ExternalFrameGenTarget(ExternalFrameGenStatus* framegen);

// VECTORXR_EXTERNAL_STATUS_PATH overrides the path (development and tests).
std::filesystem::path ResolveExternalStatusPath();

std::string FormatExternalStatus(const ExternalStatusDocument& document);
// Replaces the file in one step: a reader sees the old document or the new one, never part of one.
bool WriteExternalStatus(const std::filesystem::path& path, const ExternalStatusDocument& document,
                         std::string* error = nullptr);
// At session end: removes the file only while it still names `process_id`,
// so a game shutting down never removes the next game's file. The check and
// the removal hold the file open against a writer's replace meanwhile. True
// when it was removed.
bool RemoveExternalStatusIfOwned(const std::filesystem::path& path, std::uint32_t process_id);

} // namespace depthxr
