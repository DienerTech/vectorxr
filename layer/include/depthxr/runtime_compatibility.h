#pragma once

#include <chrono>
#include <string_view>

namespace depthxr {

enum class EyeGazeProbeState {
    kPresent,
    kAbsent,
    kIndeterminate,
};

enum class EyeGazeRequestReason {
    kApplicationRequested,
    kRuntimeAdvertised,
    kProbeIndeterminate,
    kRuntimeWorkaround,
    kNativeQuadviews,
    kReliableNegative,
    kQuadviewsNotRequested,
};

struct EyeGazeRequestDecision {
    bool inject_extension{false};
    EyeGazeRequestReason reason{EyeGazeRequestReason::kQuadviewsNotRequested};
};

struct EyeGazeRequestPolicyInput {
    bool app_requested_layer_owned_quadviews{false};
    bool forwarding_native_quadviews{false};
    bool downstream_already_has_eye_gaze{false};
    // Pre-instance extension enumeration is advisory only. An unavailable or
    // failed probe is indeterminate, while some known runtimes can complete the
    // probe with a false negative and still accept the extension at create time.
    EyeGazeProbeState pre_instance_probe{EyeGazeProbeState::kIndeterminate};
    bool pre_instance_probe_known_unreliable{false};
};

bool IsEyeGazeExtensionProbeKnownUnreliable(std::string_view active_runtime_manifest_path);
EyeGazeProbeState ClassifyEyeGazeProbe(bool extension_scan_complete, bool extension_advertised);
EyeGazeRequestDecision DecideEyeGazeExtensionRequest(const EyeGazeRequestPolicyInput& input);
std::string_view EyeGazeProbeStateName(EyeGazeProbeState state);
std::string_view EyeGazeRequestReasonName(EyeGazeRequestReason reason);
bool ShouldRetryWithoutInjectedEyeGaze(bool injected_request, bool extension_related_failure);

struct TurboCompatibilityInput {
    std::string_view executable_name;
    std::string_view runtime_name;
    bool quadviews_active{false};
    bool native_quadviews_active{false};
    bool app_requested_quadviews_extensions{false};
};

bool ShouldBlockTurboForSession(const TurboCompatibilityInput& input);
std::chrono::milliseconds QuadViewsRecoveryStabilizationDelay(std::string_view runtime_name);

} // namespace depthxr
