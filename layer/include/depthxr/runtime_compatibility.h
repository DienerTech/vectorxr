#pragma once

#include <string_view>

namespace depthxr {

struct EyeGazeRequestPolicyInput {
    bool app_requested_layer_owned_quadviews{false};
    bool forwarding_native_quadviews{false};
    bool downstream_already_has_eye_gaze{false};
    // Pre-instance extension enumeration is advisory only. Some runtimes can
    // return an empty list while accepting XR_EXT_eye_gaze_interaction at create time.
    bool pre_instance_probe_supported{false};
    bool pre_instance_probe_known_unreliable{false};
};

bool IsEyeGazeExtensionProbeKnownUnreliable(std::string_view active_runtime_manifest_path);
bool ShouldOptimisticallyRequestEyeGaze(const EyeGazeRequestPolicyInput& input);
bool ShouldRetryWithoutOptimisticEyeGaze(bool optimistic_request, bool extension_related_failure);

struct TurboCompatibilityInput {
    std::string_view executable_name;
    std::string_view runtime_name;
    bool quadviews_active{false};
    bool native_quadviews_active{false};
    bool app_requested_quadviews_extensions{false};
};

bool ShouldBlockTurboForSession(const TurboCompatibilityInput& input);

} // namespace depthxr
