#pragma once

#include <optional>
#include <string>
#include <vector>

namespace depthxr {

enum class LogLevel {
    Error,
    Info,
    Debug,
};

enum class ActivationMode {
    Toggle,
    Hold,
};

enum class InputBindingType {
    None,
    Keyboard,
    Device,
};

enum class QuadViewsTrackingMode {
    Head,
    Eye,
};

// Custom profiles override the defaults for their applications. The legacy
// Disable value is still parsed for older configs but no longer has runtime
// behavior; a profile's enabled flag controls whether it participates.
enum class ProfileMode {
    Custom,
    Disable,
};

// Optional audible feedback played when a binding's action activates or
// deactivates. Paths are absolute; an empty path means "use the bundled
// default WAV shipped next to the layer DLL". Disabled bindings never play.
struct SoundFeedback {
    bool enabled{false};
    std::string activate_sound;   // empty = bundled default
    std::string deactivate_sound; // empty = bundled default
};

struct InputBinding {
    InputBindingType type{InputBindingType::None};
    std::vector<std::string> chord;
    std::string device_guid;
    std::string input_path{"button-1"};
    std::string product_guid;
    std::string device_name;
    std::string input_label;
    SoundFeedback sound;
};

struct CoreSettings {
    bool enabled{true};
    LogLevel log_level{LogLevel::Info};
    int log_retention_files{7};
    bool track_seen_apps{true};
    // Master volume (0-100) for binding activate/deactivate sound feedback.
    int sound_volume{100};
};

struct DepthXrSettingsOverride {
    std::optional<double> stereo_boost;
    std::optional<double> convergence;
};

struct DepthXrResolvedSettings {
    bool enabled{true};
    // Neutral values (stereo_boost 1.0, convergence 0.0) mean "no effect"; there
    // is no separate per-effect enable flag.
    double stereo_boost{1.0};
    double convergence{0.0};
};

struct DepthXrBindings {
    InputBinding toggle_enabled{InputBindingType::None, {}, "", "button-1", "", "", ""};
};

struct ProfileMatch {
    std::string exe_name;
};

struct RegisteredApplication {
    std::string id;
    std::string name;
    bool enabled{true};
    ProfileMatch match;
};

struct DepthXrProfile {
    std::string name;
    std::vector<std::string> application_ids;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    DepthXrSettingsOverride settings;
};

struct DepthXrModuleConfig {
    bool enabled{false};
    DepthXrResolvedSettings defaults;
    DepthXrBindings bindings;
    std::vector<DepthXrProfile> profiles;
};

struct PivotXrSettings {
    // General (apply to both axes).
    double smoothing{0.2};
    double activation_ramp_seconds{0.35};
    // Yaw.
    double yaw_rotation_multiplier{1.5};
    double yaw_deadzone_degrees{8.0};
    double yaw_max_extra_degrees{120.0};
    // Pitch (defaults intentionally match yaw for a consistent feel).
    double pitch_rotation_multiplier{1.5};
    double pitch_deadzone_degrees{8.0};
    double pitch_max_extra_degrees{120.0};
};

struct PivotXrProfile {
    std::string name;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    std::vector<std::string> application_ids;
    ActivationMode activation_mode{ActivationMode::Toggle};
    InputBinding activation_binding;
    PivotXrSettings settings;
};

struct PivotXrModuleConfig {
    bool enabled{false};
    PivotXrSettings defaults;
    ActivationMode activation_mode{ActivationMode::Toggle};
    InputBinding activation_binding;
    std::vector<PivotXrProfile> profiles;
};

// Resolved at runtime for a specific executable — flattened from module + matched profile.
struct PivotXrResolvedSettings {
    bool enabled{false};
    ActivationMode activation_mode{ActivationMode::Toggle};
    InputBinding activation_binding;
    double smoothing{0.2};
    double activation_ramp_seconds{0.35};
    double yaw_rotation_multiplier{1.5};
    double yaw_deadzone_degrees{8.0};
    double yaw_max_extra_degrees{120.0};
    double pitch_rotation_multiplier{1.5};
    double pitch_deadzone_degrees{8.0};
    double pitch_max_extra_degrees{120.0};
};

struct QuadViewsSettings {
    QuadViewsTrackingMode tracking_mode{QuadViewsTrackingMode::Eye};
    double focus_horizontal_size_percent{40.0};
    double focus_vertical_size_percent{40.0};
    double focus_scale{1.1};
    double peripheral_scale{0.35};
    double foveate_sharpness{50.0};
    double transition_thickness_percent{25.0};
    double horizontal_offset_degrees{0.0};
    double vertical_offset_degrees{0.0};
    double gaze_smoothing{0.15};
    double gaze_deadzone_degrees{1.5};
};

struct QuadViewsProfile {
    std::string name;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    std::vector<std::string> application_ids;
    QuadViewsSettings settings;
};

struct QuadViewsModuleConfig {
    bool enabled{false};
    // Varjo-only mode switch. When true (and the runtime natively supports
    // XR_VARJO_quad_views), the layer forwards the app's native quad views to the
    // runtime instead of compositing them into a stereo layer, so the physical
    // focus panels are addressed directly. Inert on runtimes without native quad
    // views. Defaults false (emulation) so existing headsets are unaffected.
    bool varjo_native_passthrough{false};
    QuadViewsSettings defaults;
    std::vector<QuadViewsProfile> profiles;
};

struct QuadViewsResolvedSettings : QuadViewsSettings {
    bool enabled{false};
    bool varjo_native_passthrough{false};
};

struct ConfigDocument {
    int version{3};
    CoreSettings core;
    std::vector<RegisteredApplication> applications;
    DepthXrModuleConfig depthxr;
    PivotXrModuleConfig pivotxr;
    QuadViewsModuleConfig quadviews;
};

struct ResolvedRuntimeConfig {
    CoreSettings core;
    DepthXrResolvedSettings depthxr;
    DepthXrBindings depthxr_bindings;
    PivotXrResolvedSettings pivotxr;
    QuadViewsResolvedSettings quadviews;
};

const char* ToString(LogLevel level);
std::optional<LogLevel> ParseLogLevel(const std::string& value);

const char* ToString(ActivationMode mode);
std::optional<ActivationMode> ParseActivationMode(const std::string& value);
std::optional<std::string> ParseActivationKey(const std::string& value);
const char* ToString(InputBindingType type);
std::optional<InputBindingType> ParseInputBindingType(const std::string& value);
const char* ToString(QuadViewsTrackingMode mode);
std::optional<QuadViewsTrackingMode> ParseQuadViewsTrackingMode(const std::string& value);
const char* ToString(ProfileMode mode);
std::optional<ProfileMode> ParseProfileMode(const std::string& value);

} // namespace depthxr
