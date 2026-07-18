#include "depthxr/runtime_compatibility.h"

#include <string>

namespace depthxr {

bool IsEyeGazeExtensionProbeKnownUnreliable(std::string_view active_runtime_manifest_path) {
    std::string normalized_path;
    normalized_path.reserve(active_runtime_manifest_path.size());
    for (const char character : active_runtime_manifest_path) {
        normalized_path.push_back(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    }

    return normalized_path.find("steamvr") != std::string::npos ||
           normalized_path.find("steamxr") != std::string::npos ||
           normalized_path.find("pimax-openxr") != std::string::npos ||
           normalized_path.find("pimaxxr") != std::string::npos;
}

EyeGazeProbeState ClassifyEyeGazeProbe(bool extension_scan_complete, bool extension_advertised) {
    if (!extension_scan_complete) {
        return EyeGazeProbeState::kIndeterminate;
    }
    if (extension_advertised) {
        return EyeGazeProbeState::kPresent;
    }
    return EyeGazeProbeState::kAbsent;
}

EyeGazeRequestDecision DecideEyeGazeExtensionRequest(const EyeGazeRequestPolicyInput& input) {
    if (input.downstream_already_has_eye_gaze) {
        return {false, EyeGazeRequestReason::kApplicationRequested};
    }
    if (!input.app_requested_layer_owned_quadviews) {
        return {false, EyeGazeRequestReason::kQuadviewsNotRequested};
    }
    if (input.forwarding_native_quadviews) {
        return {false, EyeGazeRequestReason::kNativeQuadviews};
    }
    if (input.pre_instance_probe == EyeGazeProbeState::kPresent) {
        return {true, EyeGazeRequestReason::kRuntimeAdvertised};
    }
    if (input.pre_instance_probe == EyeGazeProbeState::kIndeterminate) {
        // If enumeration could not be completed, absence was never established.
        // Try the extension and use the extension-specific create retry if needed.
        return {true, EyeGazeRequestReason::kProbeIndeterminate};
    }
    if (input.pre_instance_probe_known_unreliable) {
        // SteamVR with some eye-tracking drivers and PimaxXR can complete
        // enumeration without gaze, then accept it in xrCreateInstance.
        return {true, EyeGazeRequestReason::kRuntimeWorkaround};
    }
    return {false, EyeGazeRequestReason::kReliableNegative};
}

std::string_view EyeGazeProbeStateName(EyeGazeProbeState state) {
    switch (state) {
    case EyeGazeProbeState::kPresent:
        return "present";
    case EyeGazeProbeState::kAbsent:
        return "absent";
    case EyeGazeProbeState::kIndeterminate:
        return "indeterminate";
    }
    return "unknown";
}

std::string_view EyeGazeRequestReasonName(EyeGazeRequestReason reason) {
    switch (reason) {
    case EyeGazeRequestReason::kApplicationRequested:
        return "application-requested";
    case EyeGazeRequestReason::kRuntimeAdvertised:
        return "runtime-advertised";
    case EyeGazeRequestReason::kProbeIndeterminate:
        return "probe-indeterminate";
    case EyeGazeRequestReason::kRuntimeWorkaround:
        return "runtime-workaround";
    case EyeGazeRequestReason::kNativeQuadviews:
        return "native-quadviews";
    case EyeGazeRequestReason::kReliableNegative:
        return "reliable-negative";
    case EyeGazeRequestReason::kQuadviewsNotRequested:
        return "quadviews-not-requested";
    }
    return "unknown";
}

bool ShouldRetryWithoutInjectedEyeGaze(bool injected_request, bool extension_related_failure) {
    return injected_request && extension_related_failure;
}

bool ShouldBlockTurboForSession(const TurboCompatibilityInput& input) {
    return input.executable_name == "DCS.exe" && input.runtime_name.find("SteamVR") != std::string_view::npos &&
           input.quadviews_active && !input.native_quadviews_active && input.app_requested_quadviews_extensions;
}

} // namespace depthxr
