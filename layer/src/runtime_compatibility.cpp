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

bool ShouldOptimisticallyRequestEyeGaze(const EyeGazeRequestPolicyInput& input) {
    // SteamVR with some eye-tracking drivers and PimaxXR can report no gaze
    // extension during pre-instance enumeration, but accept it in xrCreateInstance.
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
