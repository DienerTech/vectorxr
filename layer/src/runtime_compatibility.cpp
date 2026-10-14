#include "depthxr/runtime_compatibility.h"

#include <algorithm>
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

bool IsRuntimeExtensionProbeStructurallyUnreliable(
    bool extension_scan_complete,
    std::span<const std::string_view> advertised_extensions,
    std::span<const std::string_view> forwarded_extensions) {
    if (!extension_scan_complete) {
        // A failed/incomplete scan is already classified as indeterminate.
        return false;
    }
    if (advertised_extensions.empty()) {
        // A usable desktop runtime necessarily exposes platform/graphics
        // extensions. An empty lower-chain result is not credible evidence that
        // any particular optional extension is unsupported.
        return true;
    }

    return std::any_of(forwarded_extensions.begin(), forwarded_extensions.end(),
                       [&](std::string_view forwarded_extension) {
                           if (forwarded_extension.empty()) {
                               return false;
                           }
                           return std::find(advertised_extensions.begin(),
                                            advertised_extensions.end(),
                                            forwarded_extension) == advertised_extensions.end();
                       });
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
    if (input.pre_instance_probe_structurally_unreliable) {
        // A completed list that is empty or omits extensions already being
        // forwarded cannot make a trustworthy negative claim about eye gaze.
        return {true, EyeGazeRequestReason::kProbeInconsistent};
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
    case EyeGazeRequestReason::kProbeInconsistent:
        return "probe-inconsistent";
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

std::vector<std::vector<const char*>> InstanceExtensionRetries(const std::vector<const char*>& first,
                                                               std::span<const char* const> additions) {
    const auto same = [](const char* lhs, const char* rhs) {
        return lhs && rhs && std::string_view(lhs) == std::string_view(rhs);
    };
    const auto without = [&](const std::vector<const char*>& names, const char* removed) {
        std::vector<const char*> kept;
        kept.reserve(names.size());
        for (const char* name : names) {
            if (!same(name, removed)) {
                kept.push_back(name);
            }
        }
        return kept;
    };
    std::vector<std::vector<const char*>> retries;
    std::vector<const char*> all_removed = first;
    for (const char* addition : additions) {
        if (std::none_of(first.begin(), first.end(), [&](const char* name) { return same(name, addition); })) {
            continue;
        }
        retries.push_back(without(first, addition));
        all_removed = without(all_removed, addition);
    }
    if (retries.size() > 1) {
        retries.push_back(std::move(all_removed));
    }
    return retries;
}


std::chrono::milliseconds QuadViewsRecoveryStabilizationDelay(std::string_view runtime_name) {
    std::string normalized_name;
    normalized_name.reserve(runtime_name.size());
    for (const char character : runtime_name) {
        normalized_name.push_back(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    }

    if (normalized_name.find("virtualdesktop") != std::string::npos) {
        return std::chrono::milliseconds(250);
    }
    return std::chrono::milliseconds(0);
}

bool ResolveQuadViewsSessionActive(bool configured_active, std::optional<bool> session_latched_active) {
    return session_latched_active.value_or(configured_active);
}

std::string_view QuadViewsInactiveReason(bool application_can_select_quad_views, bool quad_views_offered) {
    if (!application_can_select_quad_views) {
        return "The application did not enable XR_VARJO_quad_views (in DCS: Options > VR > Use Quad View), so it "
               "cannot select quad views; restarting alone will not change this.";
    }
    if (!quad_views_offered) {
        return "The foveated-inset configuration was not offered when the application listed view configurations; "
               "if quadviews was enabled after the app started, restart the app.";
    }
    return "The application could select quad views and was offered the foveated-inset configuration, but chose "
           "stereo; check its own quad-view setting.";
}

} // namespace depthxr
