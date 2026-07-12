#include "depthxr/runtime_compatibility.h"

namespace depthxr {

bool ShouldOptimisticallyRequestEyeGaze(const EyeGazeRequestPolicyInput& input) {
    // SteamVR 2.16 with the PlayStation VR2 driver reports no gaze extension
    // during pre-instance enumeration, but accepts it in xrCreateInstance.
    // Limit the false-negative exception to runtimes known to have unreliable
    // probes so unrelated runtimes do not pay for a failed create/retry.
    return input.app_requested_layer_owned_quadviews && !input.forwarding_native_quadviews &&
           !input.downstream_already_has_eye_gaze &&
           (input.pre_instance_probe_supported || input.pre_instance_probe_known_unreliable);
}

bool ShouldRetryWithoutOptimisticEyeGaze(bool optimistic_request, bool extension_related_failure) {
    return optimistic_request && extension_related_failure;
}

bool ShouldBlockTurboForSession(const TurboCompatibilityInput& input) {
    return input.executable_name == "DCS.exe" && input.runtime_name.find("SteamVR") != std::string_view::npos &&
           input.quadviews_active && !input.native_quadviews_active && input.app_requested_quadviews_extensions;
}

} // namespace depthxr
