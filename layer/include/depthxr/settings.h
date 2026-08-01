#pragma once

#include <optional>
#include <string>
#include <utility>
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
    // Engaged automatically whenever no other profile is engaged; the
    // activation binding (optional) suspends/resumes it.
    AlwaysOn,
};

enum class PivotResponseMode {
    // Extra rotation grows continuously with head angle (multiplier).
    Continuous,
    // Extra rotation is added in discrete steps as angle thresholds are
    // crossed, with hysteresis so the view does not oscillate at a threshold.
    Stepped,
};

enum class PivotStepGlideMode {
    Instant,
    Glide,
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
    std::optional<bool> depth_anchor;
};

struct DepthXrResolvedSettings {
    bool enabled{true};
    // Neutral values (stereo_boost 1.0, convergence 0.0) mean "no effect"; there
    // is no separate per-effect enable flag.
    double stereo_boost{1.0};
    double convergence{0.0};
    // Render with the adjusted stereo geometry, then restore the runtime's
    // native eye poses/FOV when the projection layer is submitted.
    bool depth_anchor{false};
};

struct DepthXrBindings {
    InputBinding toggle_enabled{InputBindingType::None, {}, "", "button-1", "", "", ""};
    InputBinding toggle_anchor{InputBindingType::None, {}, "", "button-1", "", "", ""};
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

// Per-direction tuning used when advanced axes are enabled. "Left" and "up"
// are the positive yaw/pitch directions in OpenXR's right-handed, y-up
// convention. Defaults match the symmetric yaw/pitch defaults.
struct PivotAxisTuning {
    double rotation_multiplier{1.5};
    double deadzone_degrees{8.0};
    double max_extra_degrees{120.0};
};

struct PivotStepTuning {
    double deadzone_degrees{8.0};
    double trigger_degrees{10.0};
    double amount_degrees{10.0};
    double hysteresis_degrees{4.0};
    double max_extra_degrees{120.0};
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
    PivotResponseMode response_mode{PivotResponseMode::Continuous};
    PivotStepGlideMode step_glide_mode{PivotStepGlideMode::Glide};
    double step_glide_seconds{0.12};
    PivotStepTuning yaw_step;
    PivotStepTuning pitch_step;
    // Advanced axes applies independently to Continuous and Stepped.
    bool advanced_axes{false};
    PivotAxisTuning yaw_left;
    PivotAxisTuning yaw_right;
    PivotAxisTuning pitch_up;
    PivotAxisTuning pitch_down;
    PivotStepTuning yaw_left_step;
    PivotStepTuning yaw_right_step;
    PivotStepTuning pitch_up_step;
    PivotStepTuning pitch_down_step;
};

enum class PivotActivationBehavior {
    Toggle,
    Hold,
};

struct PivotActivationBinding {
    PivotActivationBehavior behavior{PivotActivationBehavior::Toggle};
    InputBinding binding;
};

enum class PivotQuickViewTurnDirection {
    Left,
    Right,
};

struct PivotNudgeSettings {
    double yaw_step_degrees{30.0};
    double pitch_step_degrees{20.0};
    double transition_seconds{0.12};
    std::vector<InputBinding> yaw_left_bindings;
    std::vector<InputBinding> yaw_right_bindings;
    std::vector<InputBinding> pitch_up_bindings;
    std::vector<InputBinding> pitch_down_bindings;
    std::vector<InputBinding> center_bindings;
};

struct PivotQuickView {
    std::string id;
    std::string name;
    double yaw_degrees{0.0};
    double pitch_degrees{0.0};
    double position_right_cm{0.0};
    double position_up_cm{0.0};
    double position_forward_cm{0.0};
    double transition_seconds{0.18};
    PivotQuickViewTurnDirection turn_direction{PivotQuickViewTurnDirection::Right};
    std::vector<PivotActivationBinding> activation_bindings;
};

struct PivotViewControls {
    PivotNudgeSettings nudges;
    std::vector<PivotQuickView> quick_views;
};

struct PivotXrProfile {
    // Stable identifier assigned by the UI; optional in hand-written configs.
    std::string id;
    std::string name;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    std::vector<std::string> application_ids;
    bool always_active{false};
    std::vector<PivotActivationBinding> activation_bindings;
    // Optional origin bindings. Set-origin captures the current head yaw/pitch
    // as pivot's neutral forward (bind it to the same button as the game's
    // recenter so both origins stay 1:1). Release-origin restores the default
    // HMD/reference-space origin.
    std::vector<InputBinding> set_origin_bindings;
    std::vector<InputBinding> release_origin_bindings;
    PivotXrSettings settings;
    PivotViewControls view_controls;
};

struct PivotXrModuleConfig {
    bool enabled{false};
    PivotXrSettings defaults;
    bool always_active{false};
    std::vector<PivotActivationBinding> activation_bindings;
    std::vector<InputBinding> set_origin_bindings;
    std::vector<InputBinding> release_origin_bindings;
    PivotViewControls view_controls;
    std::vector<PivotXrProfile> profiles;
};

// One activatable pivot behavior for the current executable, flattened from a
// matched profile (or the module defaults when nothing matches).
struct PivotXrResolvedProfile {
    std::string name;
    bool always_active{false};
    std::vector<PivotActivationBinding> activation_bindings;
    std::vector<InputBinding> set_origin_bindings;
    std::vector<InputBinding> release_origin_bindings;
    PivotViewControls view_controls;
    double smoothing{0.2};
    double activation_ramp_seconds{0.35};
    double yaw_rotation_multiplier{1.5};
    double yaw_deadzone_degrees{8.0};
    double yaw_max_extra_degrees{120.0};
    double pitch_rotation_multiplier{1.5};
    double pitch_deadzone_degrees{8.0};
    double pitch_max_extra_degrees{120.0};
    PivotResponseMode response_mode{PivotResponseMode::Continuous};
    PivotStepGlideMode step_glide_mode{PivotStepGlideMode::Glide};
    double step_glide_seconds{0.12};
    // Direction-resolved tunings. Positive = yaw left / pitch up.
    PivotAxisTuning yaw_positive;
    PivotAxisTuning yaw_negative;
    PivotAxisTuning pitch_positive;
    PivotAxisTuning pitch_negative;
    PivotStepTuning yaw_step_positive;
    PivotStepTuning yaw_step_negative;
    PivotStepTuning pitch_step_positive;
    PivotStepTuning pitch_step_negative;
};

// Resolved at runtime for a specific executable. Several profiles may target
// the same application: array order is priority order, and the activation
// binding pressed selects which profile engages. Later profiles whose binding
// duplicates an earlier candidate's are pruned at resolve time (the earlier
// profile owns that binding).
struct PivotXrResolvedSettings {
    bool enabled{false};
    std::vector<PivotXrResolvedProfile> profiles;
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
    // Optional in-session toggle for the synthesized-compositor diagnostic view.
    // The visualization itself always starts hidden for each OpenXR session.
    InputBinding diagnostic_visualization_binding;
    QuadViewsSettings defaults;
    std::vector<QuadViewsProfile> profiles;
};

struct QuadViewsResolvedSettings : QuadViewsSettings {
    bool enabled{false};
    InputBinding diagnostic_visualization_binding;
};

// Turbo mode: overrides runtime frame pacing (one frame of pipelining).
// Binary per application — profiles carry no settings.
struct TurboProfile {
    std::string id;
    std::string name;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    std::vector<std::string> application_ids;
};

// How the real xrWaitFrame is sequenced against the frame submit.
// kAsync: background-thread wait launched after submit, drained at the next
//   EndFrame — best overlap, but stalls on runtimes that interlock the wait
//   with submission (Oculus, Varjo, PiOpenXR).
// kSequenced: wait joined inside EndFrame right after the submit that
//   unblocks it, then the next frame is pre-begun — safe on interlocking
//   runtimes at the cost of blocking EndFrame for the pacing wait.
// kUnsupported: verdict-only value — even sequenced pacing stalled, so turbo
//   auto-suspends on this runtime until re-armed.
enum class TurboPacingMode {
    kAsync,
    kSequenced,
    kUnsupported,
};

// User intent for pacing. kAuto discovers per runtime (seed table, then
// probe) and records verdicts to the runtime-pacing sidecar; forced modes
// disable discovery, pins, and verdict writes entirely.
enum class TurboPacingSetting {
    kAuto,
    kAsync,
    kSequenced,
};

// When turbo's frame-pacing metrics are captured. kAlways records whenever a
// session with turbo available is rendering; kBinding records only while the
// capture binding is armed, so the user can cut loading screens and menus out
// of the data; kOff disables capture entirely.
enum class TurboMetricsMode {
    kOff,
    kAlways,
    kBinding,
};

struct TurboModuleConfig {
    bool enabled{false};
    // Optional in-session A/B toggle, mirroring the Depth toggle binding.
    InputBinding toggle_binding;
    TurboPacingSetting pacing_mode{TurboPacingSetting::kAuto};
    // Per-runtime user overrides, keyed by exact xrGetInstanceProperties
    // runtimeName. Only consulted when pacing_mode is kAuto.
    std::vector<std::pair<std::string, TurboPacingMode>> runtime_pins;
    TurboMetricsMode metrics_mode{TurboMetricsMode::kAlways};
    // Begin/end metric capture while in-game (kBinding mode only).
    InputBinding metrics_binding;
    std::vector<TurboProfile> profiles;
};

struct TurboResolvedSettings {
    bool enabled{false};
    InputBinding toggle_binding;
    TurboPacingSetting pacing_mode{TurboPacingSetting::kAuto};
    std::vector<std::pair<std::string, TurboPacingMode>> runtime_pins;
    TurboMetricsMode metrics_mode{TurboMetricsMode::kAlways};
    InputBinding metrics_binding;
};

struct ConfigDocument {
    int version{3};
    CoreSettings core;
    std::vector<RegisteredApplication> applications;
    DepthXrModuleConfig depthxr;
    PivotXrModuleConfig pivotxr;
    QuadViewsModuleConfig quadviews;
    TurboModuleConfig turbo;
};

struct ResolvedRuntimeConfig {
    CoreSettings core;
    DepthXrResolvedSettings depthxr;
    DepthXrBindings depthxr_bindings;
    PivotXrResolvedSettings pivotxr;
    QuadViewsResolvedSettings quadviews;
    TurboResolvedSettings turbo;
};

const char* ToString(LogLevel level);
std::optional<LogLevel> ParseLogLevel(const std::string& value);

const char* ToString(TurboPacingMode mode);
std::optional<TurboPacingMode> ParseTurboPacingMode(const std::string& value);
const char* ToString(TurboPacingSetting setting);
std::optional<TurboPacingSetting> ParseTurboPacingSetting(const std::string& value);
const char* ToString(TurboMetricsMode mode);
std::optional<TurboMetricsMode> ParseTurboMetricsMode(const std::string& value);

const char* ToString(ActivationMode mode);
std::optional<ActivationMode> ParseActivationMode(const std::string& value);
std::optional<std::string> ParseActivationKey(const std::string& value);
const char* ToString(InputBindingType type);
std::optional<InputBindingType> ParseInputBindingType(const std::string& value);
const char* ToString(QuadViewsTrackingMode mode);
std::optional<QuadViewsTrackingMode> ParseQuadViewsTrackingMode(const std::string& value);
const char* ToString(ProfileMode mode);
std::optional<ProfileMode> ParseProfileMode(const std::string& value);
const char* ToString(PivotResponseMode mode);
std::optional<PivotResponseMode> ParsePivotResponseMode(const std::string& value);
const char* ToString(PivotStepGlideMode mode);
std::optional<PivotStepGlideMode> ParsePivotStepGlideMode(const std::string& value);

} // namespace depthxr
