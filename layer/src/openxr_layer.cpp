#include "depthxr/openxr_layer.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "depthxr/config_path.h"
#include "depthxr/effects.h"
#include "depthxr/input_devices.h"
#include "depthxr/process_info.h"

namespace depthxr {
namespace {

bool NearlyEqual(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 0.0001;
}

bool NearlyZero(double value) {
    return std::abs(value) < 0.0001;
}

double Clamp(double value, double min_value, double max_value) {
    return std::max(min_value, std::min(max_value, value));
}

double DegreesToRadians(double degrees) {
    return degrees * 3.14159265358979323846 / 180.0;
}

std::string FormatDiagnosticDouble(double value) {
    std::ostringstream stream;
    if (value != 0.0 && std::abs(value) < 0.0001) {
        stream << std::scientific << std::setprecision(6) << value;
    } else {
        stream << std::fixed << std::setprecision(6) << value;
    }
    return stream.str();
}

double HorizontalProjectionCenter(const ViewFov& fov) {
    return (std::tan(fov.angle_left) + std::tan(fov.angle_right)) * 0.5;
}

double ExtractYawRadians(const ViewOrientation& orientation) {
    return std::atan2(
        2.0 * (orientation.w * orientation.y + orientation.x * orientation.z),
        1.0 - 2.0 * (orientation.y * orientation.y + orientation.x * orientation.x));
}

double ExtractPitchRadians(const ViewOrientation& orientation) {
    const double sin_pitch =
        Clamp(2.0 * (orientation.w * orientation.x - orientation.z * orientation.y), -1.0, 1.0);
    return std::asin(sin_pitch);
}

XrQuaternionf MultiplyQuaternion(const XrQuaternionf& lhs, const XrQuaternionf& rhs) {
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

XrQuaternionf ConjugateQuaternion(const XrQuaternionf& quaternion) {
    return {-quaternion.x, -quaternion.y, -quaternion.z, quaternion.w};
}

XrQuaternionf NormalizeQuaternion(const XrQuaternionf& quaternion) {
    const double magnitude = std::sqrt(static_cast<double>(quaternion.x) * quaternion.x +
                                       static_cast<double>(quaternion.y) * quaternion.y +
                                       static_cast<double>(quaternion.z) * quaternion.z +
                                       static_cast<double>(quaternion.w) * quaternion.w);
    if (magnitude < 0.000001) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    const float scale = static_cast<float>(1.0 / magnitude);
    return {quaternion.x * scale, quaternion.y * scale, quaternion.z * scale, quaternion.w * scale};
}

XrVector3f RotateVector(const XrQuaternionf& rotation, const XrVector3f& vector) {
    const XrQuaternionf pure_vector{vector.x, vector.y, vector.z, 0.0f};
    const XrQuaternionf rotated =
        MultiplyQuaternion(MultiplyQuaternion(rotation, pure_vector), ConjugateQuaternion(rotation));
    return {rotated.x, rotated.y, rotated.z};
}

XrQuaternionf YawQuaternion(float yaw_radians) {
    const float half = yaw_radians * 0.5f;
    return {0.0f, std::sin(half), 0.0f, std::cos(half)};
}

XrQuaternionf PitchQuaternion(float pitch_radians) {
    const float half = pitch_radians * 0.5f;
    return {std::sin(half), 0.0f, 0.0f, std::cos(half)};
}

XrPosef MultiplyPoses(const XrPosef& local_pose, const XrPosef& parent_pose) {
    const XrQuaternionf parent_orientation = NormalizeQuaternion(parent_pose.orientation);
    return {
        NormalizeQuaternion(MultiplyQuaternion(parent_orientation, local_pose.orientation)),
        {
            RotateVector(parent_orientation, local_pose.position).x + parent_pose.position.x,
            RotateVector(parent_orientation, local_pose.position).y + parent_pose.position.y,
            RotateVector(parent_orientation, local_pose.position).z + parent_pose.position.z,
        },
    };
}

XrPosef InvertPose(const XrPosef& pose) {
    const XrQuaternionf inverse_orientation = NormalizeQuaternion(ConjugateQuaternion(pose.orientation));
    const XrVector3f inverse_position =
        RotateVector(inverse_orientation, {-pose.position.x, -pose.position.y, -pose.position.z});
    return {inverse_orientation, inverse_position};
}

XrPosef ApplyExtraRotationToPose(const XrPosef& pose, float extra_yaw_radians, float extra_pitch_radians) {
    if (NearlyZero(extra_yaw_radians) && NearlyZero(extra_pitch_radians)) {
        return pose;
    }

    const XrQuaternionf extra_rotation = NormalizeQuaternion(
        MultiplyQuaternion(YawQuaternion(extra_yaw_radians), PitchQuaternion(extra_pitch_radians)));
    return {
        NormalizeQuaternion(MultiplyQuaternion(extra_rotation, pose.orientation)),
        pose.position,
    };
}

XrPosef IdentityPose() {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

double ComputeTimeBasedBlend(double smoothing, double delta_seconds) {
    const double clamped_smoothing = Clamp(smoothing, 0.0, 0.99);
    const double fallback_blend = Clamp(1.0 - clamped_smoothing, 0.05, 1.0);
    if (delta_seconds <= 0.0) {
        return fallback_blend;
    }

    constexpr double kReferenceFrameSeconds = 1.0 / 90.0;
    const double frame_scale = std::max(delta_seconds / kReferenceFrameSeconds, 0.0);
    return Clamp(1.0 - std::pow(clamped_smoothing, frame_scale), 0.05, 1.0);
}

double ComputePivotExtraAngleRadians(double current_angle_radians,
                                     double rotation_multiplier,
                                     double deadzone_degrees,
                                     double max_extra_degrees,
                                     double smoothing,
                                     double delta_seconds,
                                     double& smoothed_extra_angle_radians) {
    if (rotation_multiplier <= 1.0) {
        smoothed_extra_angle_radians = 0.0;
        return 0.0;
    }

    const double deadzone_radians = DegreesToRadians(std::max(0.0, deadzone_degrees));
    const double max_extra_radians = DegreesToRadians(std::max(0.0, max_extra_degrees));
    const double abs_angle = std::abs(current_angle_radians);

    double target_extra_angle = 0.0;
    if (abs_angle > deadzone_radians) {
        const double direction = current_angle_radians >= 0.0 ? 1.0 : -1.0;
        const double pivoted_angle = direction * deadzone_radians +
                                     (current_angle_radians - direction * deadzone_radians) * rotation_multiplier;
        target_extra_angle = pivoted_angle - current_angle_radians;
    }

    if (max_extra_radians > 0.0) {
        target_extra_angle = Clamp(target_extra_angle, -max_extra_radians, max_extra_radians);
    }

    const double blend = ComputeTimeBasedBlend(smoothing, delta_seconds);
    smoothed_extra_angle_radians += (target_extra_angle - smoothed_extra_angle_radians) * blend;
    if (NearlyZero(smoothed_extra_angle_radians)) {
        smoothed_extra_angle_radians = 0.0;
    }
    return smoothed_extra_angle_radians;
}

const char* ToString(XrViewConfigurationType type) {
    switch (type) {
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO:
        return "primary_mono";
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO:
        return "primary_stereo";
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET:
        return "primary_stereo_with_foveated_inset";
    default:
        return "unknown";
    }
}

ViewLayout DetermineViewLayout(XrViewConfigurationType type, uint32_t count) {
    if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET) {
        return count >= 4 ? ViewLayout::kStereoWithFoveatedInset : ViewLayout::kMono;
    }

    if (count >= 2) {
        return ViewLayout::kStereo;
    }

    return ViewLayout::kMono;
}

bool SameSettings(const ResolvedRuntimeConfig& lhs, const ResolvedRuntimeConfig& rhs) {
    return lhs.core.enabled == rhs.core.enabled &&
           lhs.core.log_level == rhs.core.log_level &&
           lhs.core.log_retention_files == rhs.core.log_retention_files &&
           lhs.depthxr.enabled == rhs.depthxr.enabled &&
           lhs.depthxr.stereo_boost_enabled == rhs.depthxr.stereo_boost_enabled &&
           lhs.depthxr.convergence_enabled == rhs.depthxr.convergence_enabled &&
           NearlyEqual(lhs.depthxr.stereo_boost, rhs.depthxr.stereo_boost) &&
           NearlyEqual(lhs.depthxr.convergence, rhs.depthxr.convergence) &&
           lhs.depthxr_bindings.toggle_enabled.type == rhs.depthxr_bindings.toggle_enabled.type &&
           lhs.depthxr_bindings.toggle_enabled.chord == rhs.depthxr_bindings.toggle_enabled.chord &&
           lhs.depthxr_bindings.toggle_enabled.device_guid == rhs.depthxr_bindings.toggle_enabled.device_guid &&
           lhs.depthxr_bindings.toggle_enabled.input_path == rhs.depthxr_bindings.toggle_enabled.input_path &&
           lhs.depthxr_bindings.toggle_enabled.product_guid == rhs.depthxr_bindings.toggle_enabled.product_guid &&
           lhs.depthxr_bindings.toggle_enabled.device_name == rhs.depthxr_bindings.toggle_enabled.device_name &&
           lhs.depthxr_bindings.toggle_enabled.input_label == rhs.depthxr_bindings.toggle_enabled.input_label &&
           lhs.pivotxr.enabled == rhs.pivotxr.enabled &&
           lhs.pivotxr.activation_mode == rhs.pivotxr.activation_mode &&
           lhs.pivotxr.activation_binding.type == rhs.pivotxr.activation_binding.type &&
           lhs.pivotxr.activation_binding.chord == rhs.pivotxr.activation_binding.chord &&
           lhs.pivotxr.activation_binding.device_guid == rhs.pivotxr.activation_binding.device_guid &&
           lhs.pivotxr.activation_binding.input_path == rhs.pivotxr.activation_binding.input_path &&
           lhs.pivotxr.activation_binding.product_guid == rhs.pivotxr.activation_binding.product_guid &&
           lhs.pivotxr.activation_binding.device_name == rhs.pivotxr.activation_binding.device_name &&
           lhs.pivotxr.activation_binding.input_label == rhs.pivotxr.activation_binding.input_label &&
           NearlyEqual(lhs.pivotxr.yaw_rotation_multiplier, rhs.pivotxr.yaw_rotation_multiplier) &&
           NearlyEqual(lhs.pivotxr.yaw_smoothing, rhs.pivotxr.yaw_smoothing) &&
           NearlyEqual(lhs.pivotxr.yaw_deadzone_degrees, rhs.pivotxr.yaw_deadzone_degrees) &&
           NearlyEqual(lhs.pivotxr.yaw_max_extra_degrees, rhs.pivotxr.yaw_max_extra_degrees) &&
           NearlyEqual(lhs.pivotxr.pitch_rotation_multiplier, rhs.pivotxr.pitch_rotation_multiplier) &&
           NearlyEqual(lhs.pivotxr.pitch_smoothing, rhs.pivotxr.pitch_smoothing) &&
           NearlyEqual(lhs.pivotxr.pitch_deadzone_degrees, rhs.pivotxr.pitch_deadzone_degrees) &&
           NearlyEqual(lhs.pivotxr.pitch_max_extra_degrees, rhs.pivotxr.pitch_max_extra_degrees);
}

bool SameInputBinding(const InputBinding& lhs, const InputBinding& rhs) {
    return lhs.type == rhs.type &&
           lhs.chord == rhs.chord &&
           lhs.device_guid == rhs.device_guid &&
           lhs.input_path == rhs.input_path &&
           lhs.product_guid == rhs.product_guid &&
           lhs.device_name == rhs.device_name &&
           lhs.input_label == rhs.input_label;
}

bool SamePivotActivationBinding(const PivotXrResolvedSettings& lhs, const PivotXrResolvedSettings& rhs) {
    return lhs.enabled == rhs.enabled && lhs.activation_mode == rhs.activation_mode &&
           SameInputBinding(lhs.activation_binding, rhs.activation_binding);
}

std::string BindingLabel(const InputBinding& binding) {
    if (binding.type == InputBindingType::Device) {
        const std::string device = binding.device_name.empty() ? binding.device_guid : binding.device_name;
        const std::string input = binding.input_label.empty() ? binding.input_path : binding.input_label;
        return device + "/" + input;
    }

    std::ostringstream stream;
    for (size_t index = 0; index < binding.chord.size(); ++index) {
        if (index > 0) {
            stream << "+";
        }
        stream << binding.chord[index];
    }
    return stream.str();
}

ConfigDocument DefaultConfig() {
    ConfigDocument document;
    document.version = 3;
    return document;
}

void AppendViewSummary(std::ostringstream& stream, std::span<const ViewAdjustmentData> views) {
    const size_t summary_count = std::min<size_t>(views.size(), 2);
    for (size_t i = 0; i < summary_count; ++i) {
        stream << " view" << i << "Pos=(" << FormatDiagnosticDouble(views[i].position.x) << ", "
               << FormatDiagnosticDouble(views[i].position.y) << ", "
               << FormatDiagnosticDouble(views[i].position.z) << ")"
               << " view" << i << "Fov=(" << FormatDiagnosticDouble(views[i].fov.angle_left) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_right) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_up) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_down) << ")"
               << " view" << i << "ProjCenter=" << FormatDiagnosticDouble(HorizontalProjectionCenter(views[i].fov));
    }
}

} // namespace

OpenXrLayer& OpenXrLayer::Instance() {
    static OpenXrLayer layer;
    return layer;
}

void OpenXrLayer::SetLayerDirectory(std::filesystem::path dll_directory) {
    std::scoped_lock lock(mutex_);
    dll_directory_ = std::move(dll_directory);
}

void OpenXrLayer::SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr) {
    std::scoped_lock lock(mutex_);
    next_get_instance_proc_addr_ = next_get_instance_proc_addr;
}

XrResult OpenXrLayer::OnInstanceCreated(const XrInstanceCreateInfo* create_info, XrInstance instance) {
    std::scoped_lock lock(mutex_);

    instance_ = instance;
    ResetSessionState();
    config_path_ = ResolveConfigPath();
    log_path_ = ResolveLogPath();
    logger_.Initialize(log_path_);

    current_exe_name_ = GetCurrentExecutableName();
    logger_.Info("VectorXR attached to process: " + current_exe_name_);

    if (create_info) {
        std::ostringstream stream;
        stream << "Application=" << create_info->applicationInfo.applicationName << ", Engine="
               << create_info->applicationInfo.engineName;
        logger_.Info(stream.str());
    }

    CaptureInstanceFunctions();
    if (!next_destroy_instance_ || !next_create_session_ || !next_begin_session_ || !next_end_frame_ ||
        !next_create_reference_space_ || !next_locate_space_ || !next_locate_views_) {
        logger_.Error("Failed to resolve required OpenXR function pointers");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    logger_.Info("Active log file: " + logger_.ActiveLogPath().string());
    return XR_SUCCESS;
}

XrResult OpenXrLayer::GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!function || !name) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (std::string_view(name) == "xrGetInstanceProcAddr") {
        return next_get_instance_proc_addr_(instance, name, function);
    }

    return next_get_instance_proc_addr_(instance, name, function);
}

XrResult OpenXrLayer::DestroyInstance(XrInstance instance) {
    std::scoped_lock lock(mutex_);

    const XrResult result = next_destroy_instance_(instance);
    if (XR_SUCCEEDED(result)) {
        instance_ = XR_NULL_HANDLE;
        next_destroy_instance_ = nullptr;
        next_create_session_ = nullptr;
        next_create_reference_space_ = nullptr;
        next_end_frame_ = nullptr;
        next_locate_space_ = nullptr;
        next_locate_views_ = nullptr;
        has_loaded_config_ = false;
        locate_views_call_count_ = 0;
        pending_locate_views_diagnostics_ = 0;
        ResetPivotActivationState();
        ResetDepthToggleState();
        ResetSessionState();
    }

    return result;
}

XrResult OpenXrLayer::CreateSession(XrInstance instance,
                                    const XrSessionCreateInfo* create_info,
                                    XrSession* session) {
    const XrResult result = next_create_session_(instance, create_info, session);
    if (XR_FAILED(result) || !session) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    ResetSessionState();
    active_session_ = *session;
    return CreateInternalReferenceSpaces(*session);
}

XrResult OpenXrLayer::BeginSession(XrSession session, const XrSessionBeginInfo* begin_info) {
    const XrResult result = next_begin_session_(session, begin_info);
    if (XR_FAILED(result) || !begin_info) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    active_session_ = session;
    active_primary_view_configuration_type_ = begin_info->primaryViewConfigurationType;
    has_active_primary_view_configuration_ = true;
    has_logged_quad_view_short_count_ = false;

    logger_.Info(std::string("Session began with view configuration: ") +
                 ToString(active_primary_view_configuration_type_));
    return result;
}

XrResult OpenXrLayer::EndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
    if (!frame_end_info) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::scoped_lock lock(mutex_);
    const XrPosef pose_delta = FindPivotPoseDelta(frame_end_info->displayTime);
    const bool has_non_identity_delta =
        !NearlyZero(pose_delta.orientation.x) || !NearlyZero(pose_delta.orientation.y) ||
        !NearlyZero(pose_delta.orientation.z) || !NearlyEqual(pose_delta.orientation.w, 1.0f) ||
        !NearlyZero(pose_delta.position.x) || !NearlyZero(pose_delta.position.y) || !NearlyZero(pose_delta.position.z);

    if (!has_non_identity_delta) {
        return next_end_frame_(session, frame_end_info);
    }

    const XrPosef reverse_delta = InvertPose(pose_delta);
    std::vector<std::vector<XrCompositionLayerProjectionView>> adjusted_projection_views;
    std::vector<XrCompositionLayerProjection> adjusted_projection_layers;
    std::vector<const XrCompositionLayerBaseHeader*> adjusted_layers;
    adjusted_projection_views.reserve(frame_end_info->layerCount);
    adjusted_projection_layers.reserve(frame_end_info->layerCount);
    adjusted_layers.reserve(frame_end_info->layerCount);

    for (uint32_t i = 0; i < frame_end_info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base_header = frame_end_info->layers[i];
        if (!base_header || base_header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            adjusted_layers.push_back(base_header);
            continue;
        }

        const XrCompositionLayerProjection* projection_layer =
            reinterpret_cast<const XrCompositionLayerProjection*>(base_header);
        adjusted_projection_views.emplace_back(
            projection_layer->views, projection_layer->views + projection_layer->viewCount);
        for (XrCompositionLayerProjectionView& projection_view : adjusted_projection_views.back()) {
            projection_view.pose = MultiplyPoses(projection_view.pose, reverse_delta);
        }

        adjusted_projection_layers.push_back(*projection_layer);
        adjusted_projection_layers.back().views = adjusted_projection_views.back().data();
        adjusted_layers.push_back(
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&adjusted_projection_layers.back()));
    }

    XrFrameEndInfo adjusted_frame_end_info = *frame_end_info;
    adjusted_frame_end_info.layers = adjusted_layers.data();
    const XrResult result = next_end_frame_(session, &adjusted_frame_end_info);
    PrunePivotPoseDeltas(frame_end_info->displayTime);
    return result;
}

XrResult OpenXrLayer::CreateReferenceSpace(XrSession session,
                                           const XrReferenceSpaceCreateInfo* create_info,
                                           XrSpace* space) {
    const XrResult result = next_create_reference_space_(session, create_info, space);
    if (XR_FAILED(result) || !create_info || !space) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    switch (create_info->referenceSpaceType) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        tracked_view_spaces_.insert(*space);
        break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        tracked_local_spaces_.insert(*space);
        break;
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        tracked_stage_spaces_.insert(*space);
        break;
    default:
        break;
    }

    return result;
}

XrResult OpenXrLayer::LocateSpace(XrSpace space, XrSpace base_space, XrTime time, XrSpaceLocation* location) {
    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    const bool pivotxr_active = IsPivotXrActive(resolved_settings_.pivotxr);

    return LocateSpaceWithPivot(
        space, base_space, time, resolved_settings_.pivotxr, pivotxr_active, location, nullptr, nullptr);
}

XrResult OpenXrLayer::LocateViews(XrSession session,
                                  const XrViewLocateInfo* view_locate_info,
                                  XrViewState* view_state,
                                  uint32_t view_capacity_input,
                                  uint32_t* view_count_output,
                                  XrView* views) {
    const XrResult result =
        next_locate_views_(session, view_locate_info, view_state, view_capacity_input, view_count_output, views);

    if (XR_FAILED(result) || !views || !view_count_output) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();

    if (!resolved_settings_.core.enabled) {
        return result;
    }

    const uint32_t count = std::min(view_capacity_input, *view_count_output);
    if (count == 0) {
        return result;
    }

    XrViewConfigurationType view_configuration_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (view_locate_info) {
        view_configuration_type = view_locate_info->viewConfigurationType;
    } else if (has_active_primary_view_configuration_ && session == active_session_) {
        view_configuration_type = active_primary_view_configuration_type_;
    }

    const ViewLayout view_layout = DetermineViewLayout(view_configuration_type, count);
    if (view_configuration_type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET &&
        view_layout == ViewLayout::kMono && !has_logged_quad_view_short_count_) {
        std::ostringstream stream;
        stream << "Quad-view session reported only " << count
               << " views to xrLocateViews; skipping DepthXR adjustments for this call.";
        logger_.Info(stream.str());
        has_logged_quad_view_short_count_ = true;
        return result;
    }

    // DepthXR keeps the interception surface minimal and applies all current
    // stereo/depth experiments in xrLocateViews.
    ++locate_views_call_count_;
    std::vector<ViewAdjustmentData> original_views(count);
    std::vector<ViewAdjustmentData> adjusted_views(count);
    for (uint32_t i = 0; i < count; ++i) {
        original_views[i].position.x = views[i].pose.position.x;
        original_views[i].position.y = views[i].pose.position.y;
        original_views[i].position.z = views[i].pose.position.z;
        original_views[i].fov.angle_left = views[i].fov.angleLeft;
        original_views[i].fov.angle_right = views[i].fov.angleRight;
        original_views[i].fov.angle_up = views[i].fov.angleUp;
        original_views[i].fov.angle_down = views[i].fov.angleDown;

        adjusted_views[i].position.x = views[i].pose.position.x;
        adjusted_views[i].position.y = views[i].pose.position.y;
        adjusted_views[i].position.z = views[i].pose.position.z;
        adjusted_views[i].fov.angle_left = views[i].fov.angleLeft;
        adjusted_views[i].fov.angle_right = views[i].fov.angleRight;
        adjusted_views[i].fov.angle_up = views[i].fov.angleUp;
        adjusted_views[i].fov.angle_down = views[i].fov.angleDown;
        original_views[i].orientation.x = views[i].pose.orientation.x;
        original_views[i].orientation.y = views[i].pose.orientation.y;
        original_views[i].orientation.z = views[i].pose.orientation.z;
        original_views[i].orientation.w = views[i].pose.orientation.w;
        adjusted_views[i].orientation.x = views[i].pose.orientation.x;
        adjusted_views[i].orientation.y = views[i].pose.orientation.y;
        adjusted_views[i].orientation.z = views[i].pose.orientation.z;
        adjusted_views[i].orientation.w = views[i].pose.orientation.w;
    }

    const bool pivotxr_active = IsPivotXrActive(resolved_settings_.pivotxr);
    const bool depthxr_active = IsDepthXrActive();
    if (resolved_settings_.pivotxr.enabled && !has_logged_pivotxr_spike_mode_) {
        std::ostringstream stream;
        stream << "PivotXR spike is active in experimental view-space recomposition mode; press "
               << BindingLabel(resolved_settings_.pivotxr.activation_binding) << " to "
               << (resolved_settings_.pivotxr.activation_mode == ActivationMode::Toggle ? "toggle" : "hold")
               << " the extra pivot factor.";
        logger_.Info(stream.str());
        has_logged_pivotxr_spike_mode_ = true;
    }

    if (resolved_settings_.pivotxr.enabled && pivotxr_active && internal_view_space_ != XR_NULL_HANDLE &&
        view_locate_info && EnsureEyeOffsets(session, view_configuration_type, view_locate_info->displayTime, count)) {
        XrSpaceLocation pivot_view_location{XR_TYPE_SPACE_LOCATION};
        double applied_extra_yaw_radians = 0.0;
        double applied_extra_pitch_radians = 0.0;
        const XrResult pivot_result = LocateSpaceWithPivot(internal_view_space_,
                                                           view_locate_info->space,
                                                           view_locate_info->displayTime,
                                                           resolved_settings_.pivotxr,
                                                           pivotxr_active,
                                                           &pivot_view_location,
                                                           &applied_extra_yaw_radians,
                                                           &applied_extra_pitch_radians);
        if (XR_SUCCEEDED(pivot_result)) {
            for (uint32_t i = 0; i < count; ++i) {
                adjusted_views[i].position.x = cached_eye_offset_poses_[i].position.x;
                adjusted_views[i].position.y = cached_eye_offset_poses_[i].position.y;
                adjusted_views[i].position.z = cached_eye_offset_poses_[i].position.z;
                adjusted_views[i].orientation.x = cached_eye_offset_poses_[i].orientation.x;
                adjusted_views[i].orientation.y = cached_eye_offset_poses_[i].orientation.y;
                adjusted_views[i].orientation.z = cached_eye_offset_poses_[i].orientation.z;
                adjusted_views[i].orientation.w = cached_eye_offset_poses_[i].orientation.w;

                const XrPosef recomposed_pose = MultiplyPoses(
                    cached_eye_offset_poses_[i], pivot_view_location.pose);
                adjusted_views[i].position.x = recomposed_pose.position.x;
                adjusted_views[i].position.y = recomposed_pose.position.y;
                adjusted_views[i].position.z = recomposed_pose.position.z;
                adjusted_views[i].orientation.x = recomposed_pose.orientation.x;
                adjusted_views[i].orientation.y = recomposed_pose.orientation.y;
                adjusted_views[i].orientation.z = recomposed_pose.orientation.z;
                adjusted_views[i].orientation.w = recomposed_pose.orientation.w;
            }
            pivotxr_smoothed_extra_yaw_radians_ = applied_extra_yaw_radians;
            pivotxr_smoothed_extra_pitch_radians_ = applied_extra_pitch_radians;
        }
    } else if (!pivotxr_active) {
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
    }

    if (depthxr_active &&
        resolved_settings_.depthxr.stereo_boost_enabled &&
        !NearlyEqual(resolved_settings_.depthxr.stereo_boost, 1.0)) {
        ApplyStereoBoost(adjusted_views, resolved_settings_.depthxr.stereo_boost, view_layout);
    }
    if (depthxr_active &&
        resolved_settings_.depthxr.convergence_enabled &&
        !NearlyEqual(resolved_settings_.depthxr.convergence, 0.0)) {
        ApplyConvergence(adjusted_views, resolved_settings_.depthxr.convergence, view_layout);
    }

    for (uint32_t i = 0; i < count; ++i) {
        views[i].pose.position.x = static_cast<float>(adjusted_views[i].position.x);
        views[i].pose.position.y = static_cast<float>(adjusted_views[i].position.y);
        views[i].pose.position.z = static_cast<float>(adjusted_views[i].position.z);
        views[i].pose.orientation.x = static_cast<float>(adjusted_views[i].orientation.x);
        views[i].pose.orientation.y = static_cast<float>(adjusted_views[i].orientation.y);
        views[i].pose.orientation.z = static_cast<float>(adjusted_views[i].orientation.z);
        views[i].pose.orientation.w = static_cast<float>(adjusted_views[i].orientation.w);
        views[i].fov.angleLeft = static_cast<float>(adjusted_views[i].fov.angle_left);
        views[i].fov.angleRight = static_cast<float>(adjusted_views[i].fov.angle_right);
        views[i].fov.angleUp = static_cast<float>(adjusted_views[i].fov.angle_up);
        views[i].fov.angleDown = static_cast<float>(adjusted_views[i].fov.angle_down);
    }

    if (pending_locate_views_diagnostics_ > 0) {
        std::ostringstream stream;
        const double left_position_delta =
            adjusted_views[0].position.x - original_views[0].position.x;
        const double right_position_delta =
            count > 1 ? adjusted_views[1].position.x - original_views[1].position.x : 0.0;
        const double left_projection_center_delta =
            HorizontalProjectionCenter(adjusted_views[0].fov) - HorizontalProjectionCenter(original_views[0].fov);
        const double right_projection_center_delta = count > 1
                                                         ? HorizontalProjectionCenter(adjusted_views[1].fov) -
                                                               HorizontalProjectionCenter(original_views[1].fov)
                                                         : 0.0;
        const double left_yaw_delta =
            ExtractYawRadians(adjusted_views[0].orientation) - ExtractYawRadians(original_views[0].orientation);
        const double right_yaw_delta =
            count > 1 ? ExtractYawRadians(adjusted_views[1].orientation) -
                            ExtractYawRadians(original_views[1].orientation)
                      : 0.0;
        const double left_pitch_delta =
            ExtractPitchRadians(adjusted_views[0].orientation) - ExtractPitchRadians(original_views[0].orientation);
        const double right_pitch_delta =
            count > 1 ? ExtractPitchRadians(adjusted_views[1].orientation) -
                            ExtractPitchRadians(original_views[1].orientation)
                      : 0.0;
        stream << "LocateViews call " << locate_views_call_count_ << ": count=" << count
               << ", viewConfig=" << ToString(view_configuration_type)
               << ", pivotExtraYawRadians=" << FormatDiagnosticDouble(pivotxr_smoothed_extra_yaw_radians_)
               << ", pivotExtraPitchRadians=" << FormatDiagnosticDouble(pivotxr_smoothed_extra_pitch_radians_)
               << ", leftYawDelta=" << FormatDiagnosticDouble(left_yaw_delta)
               << ", rightYawDelta=" << FormatDiagnosticDouble(right_yaw_delta)
               << ", leftPitchDelta=" << FormatDiagnosticDouble(left_pitch_delta)
               << ", rightPitchDelta=" << FormatDiagnosticDouble(right_pitch_delta)
               << ", depthRuntimeActive=" << depthxr_active
               << ", stereoBoostEnabled=" << resolved_settings_.depthxr.stereo_boost_enabled
               << ", stereoBoost=" << FormatDiagnosticDouble(resolved_settings_.depthxr.stereo_boost)
               << ", convergenceEnabled=" << resolved_settings_.depthxr.convergence_enabled
               << ", convergence=" << FormatDiagnosticDouble(resolved_settings_.depthxr.convergence)
               << ", leftXDelta=" << FormatDiagnosticDouble(left_position_delta)
               << ", rightXDelta=" << FormatDiagnosticDouble(right_position_delta)
               << ", leftProjCenterDelta=" << FormatDiagnosticDouble(left_projection_center_delta)
               << ", rightProjCenterDelta=" << FormatDiagnosticDouble(right_projection_center_delta)
               << ", before:";
        AppendViewSummary(stream, original_views);
        stream << " after:";
        AppendViewSummary(stream, adjusted_views);
        logger_.Debug(stream.str());
        --pending_locate_views_diagnostics_;
    }

    return result;
}

void OpenXrLayer::ReloadConfigIfNeeded() {
    if (config_path_.empty()) {
        config_path_ = ResolveConfigPath();
    }

    if (!std::filesystem::exists(config_path_)) {
        if (!has_loaded_config_) {
            config_ = DefaultConfig();
            has_loaded_config_ = true;
            logger_.Info("No config file found. Using default settings.");
        }
        return;
    }

    const auto timestamp = std::filesystem::last_write_time(config_path_);
    if (has_loaded_config_ && has_config_timestamp_ && timestamp == last_config_write_time_) {
        return;
    }

    const ParseResult loaded = LoadConfigFromFile(config_path_);
    if (!loaded.ok) {
        const bool already_reported_failure =
            has_failed_config_timestamp_ && timestamp == last_failed_config_write_time_ &&
            loaded.error == last_failed_config_error_;
        if (!already_reported_failure) {
            logger_.Error("Failed to parse config: " + loaded.error);
        }
        last_failed_config_write_time_ = timestamp;
        has_failed_config_timestamp_ = true;
        last_failed_config_error_ = loaded.error;
        if (!has_loaded_config_) {
            config_ = DefaultConfig();
            has_loaded_config_ = true;
        }
        return;
    }

    config_ = loaded.document;
    has_loaded_config_ = true;
    has_config_timestamp_ = true;
    last_config_write_time_ = timestamp;
    has_failed_config_timestamp_ = false;
    last_failed_config_error_.clear();
    logger_.Info("Loaded config from " + config_path_.string());
}

void OpenXrLayer::RefreshResolvedSettings() {
    const ResolvedRuntimeConfig previous = resolved_settings_;
    resolved_settings_ = ResolveRuntimeConfig(config_, current_exe_name_);
    if (!SameInputBinding(previous.depthxr_bindings.toggle_enabled, resolved_settings_.depthxr_bindings.toggle_enabled) ||
        previous.depthxr.enabled != resolved_settings_.depthxr.enabled) {
        ResetDepthToggleState();
    }
    if (!SamePivotActivationBinding(previous.pivotxr, resolved_settings_.pivotxr)) {
        ResetPivotActivationState();
        has_logged_pivotxr_spike_mode_ = false;
    }
    logger_.SetLevel(resolved_settings_.core.log_level);
    logger_.SetRetentionFiles(resolved_settings_.core.log_retention_files);
    if (!last_logged_settings_ || !SameSettings(*last_logged_settings_, resolved_settings_)) {
        LogResolvedSettings(resolved_settings_);
        last_logged_settings_ = resolved_settings_;
        pending_locate_views_diagnostics_ = 5;
    }
}

void OpenXrLayer::CaptureInstanceFunctions() {
    PFN_xrVoidFunction function = nullptr;

    if (XR_SUCCEEDED(next_get_instance_proc_addr_(
            instance_, "xrDestroyInstance", &function))) {
        next_destroy_instance_ = reinterpret_cast<PFN_xrDestroyInstance>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateSession", &function))) {
        next_create_session_ = reinterpret_cast<PFN_xrCreateSession>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrBeginSession", &function))) {
        next_begin_session_ = reinterpret_cast<PFN_xrBeginSession>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEndFrame", &function))) {
        next_end_frame_ = reinterpret_cast<PFN_xrEndFrame>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateReferenceSpace", &function))) {
        next_create_reference_space_ = reinterpret_cast<PFN_xrCreateReferenceSpace>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrLocateSpace", &function))) {
        next_locate_space_ = reinterpret_cast<PFN_xrLocateSpace>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrLocateViews", &function))) {
        next_locate_views_ = reinterpret_cast<PFN_xrLocateViews>(function);
    }
}

void OpenXrLayer::ResetSessionState() {
    active_session_ = XR_NULL_HANDLE;
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_last_smoothing_wall_time_.reset();
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
    internal_local_space_ = XR_NULL_HANDLE;
    internal_view_space_ = XR_NULL_HANDLE;
    internal_stage_space_ = XR_NULL_HANDLE;
    active_primary_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    cached_eye_offsets_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    has_active_primary_view_configuration_ = false;
    has_logged_quad_view_short_count_ = false;
    has_logged_pivotxr_spike_mode_ = false;
    tracked_view_spaces_.clear();
    tracked_local_spaces_.clear();
    tracked_stage_spaces_.clear();
    cached_eye_offset_poses_.clear();
    cached_pivot_pose_deltas_.clear();
}

XrResult OpenXrLayer::CreateInternalReferenceSpaces(XrSession session) {
    if (!next_create_reference_space_) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    create_info.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    create_info.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_SUCCEEDED(next_create_reference_space_(session, &create_info, &internal_local_space_))) {
        tracked_local_spaces_.insert(internal_local_space_);
    }

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    const XrResult view_result = next_create_reference_space_(session, &create_info, &internal_view_space_);
    if (XR_FAILED(view_result)) {
        internal_view_space_ = XR_NULL_HANDLE;
        logger_.Error("Failed to create internal VIEW reference space for PivotXR.");
        return view_result;
    }
    tracked_view_spaces_.insert(internal_view_space_);

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_SUCCEEDED(next_create_reference_space_(session, &create_info, &internal_stage_space_))) {
        tracked_stage_spaces_.insert(internal_stage_space_);
    } else {
        internal_stage_space_ = XR_NULL_HANDLE;
    }

    return XR_SUCCESS;
}

bool OpenXrLayer::EnsureEyeOffsets(XrSession session,
                                   XrViewConfigurationType view_configuration_type,
                                   XrTime display_time,
                                   uint32_t view_count) {
    if (internal_view_space_ == XR_NULL_HANDLE || !next_locate_views_ || view_count == 0) {
        return false;
    }

    if (cached_eye_offset_poses_.size() == view_count &&
        cached_eye_offsets_view_configuration_type_ == view_configuration_type) {
        return true;
    }

    std::vector<XrView> eye_views(view_count);
    for (XrView& eye_view : eye_views) {
        eye_view = {XR_TYPE_VIEW};
    }
    XrViewState eye_view_state{XR_TYPE_VIEW_STATE};
    uint32_t eye_view_count = 0;
    XrViewLocateInfo eye_view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    eye_view_locate_info.viewConfigurationType = view_configuration_type;
    eye_view_locate_info.displayTime = display_time;
    eye_view_locate_info.space = internal_view_space_;

    const XrResult result =
        next_locate_views_(session, &eye_view_locate_info, &eye_view_state, view_count, &eye_view_count, eye_views.data());
    if (XR_FAILED(result) || eye_view_count < view_count) {
        logger_.Error("Failed to capture internal eye offsets for PivotXR view-space recomposition.");
        return false;
    }

    cached_eye_offset_poses_.resize(view_count);
    for (uint32_t i = 0; i < view_count; ++i) {
        cached_eye_offset_poses_[i] = eye_views[i].pose;
    }
    cached_eye_offsets_view_configuration_type_ = view_configuration_type;
    return true;
}

void OpenXrLayer::CachePivotPoseDelta(XrTime time, const XrPosef& pose_delta) {
    cached_pivot_pose_deltas_[time] = pose_delta;
}

XrPosef OpenXrLayer::FindPivotPoseDelta(XrTime time) const {
    if (cached_pivot_pose_deltas_.empty()) {
        return IdentityPose();
    }

    auto exact = cached_pivot_pose_deltas_.find(time);
    if (exact != cached_pivot_pose_deltas_.end()) {
        return exact->second;
    }

    auto upper = cached_pivot_pose_deltas_.lower_bound(time);
    if (upper == cached_pivot_pose_deltas_.begin()) {
        return upper->second;
    }
    if (upper == cached_pivot_pose_deltas_.end()) {
        return std::prev(upper)->second;
    }

    auto lower = std::prev(upper);
    return (time - lower->first <= upper->first - time) ? lower->second : upper->second;
}

void OpenXrLayer::PrunePivotPoseDeltas(XrTime time) {
    auto keep_from = cached_pivot_pose_deltas_.lower_bound(time);
    if (keep_from == cached_pivot_pose_deltas_.begin()) {
        return;
    }

    cached_pivot_pose_deltas_.erase(cached_pivot_pose_deltas_.begin(), keep_from);
}

bool OpenXrLayer::IsTrackedViewSpace(XrSpace space) const {
    return space != XR_NULL_HANDLE && tracked_view_spaces_.contains(space);
}

XrResult OpenXrLayer::LocateSpaceWithPivot(XrSpace space,
                                           XrSpace base_space,
                                           XrTime time,
                                           const PivotXrResolvedSettings& settings,
                                           bool pivotxr_active,
                                           XrSpaceLocation* location,
                                           double* applied_extra_yaw_radians,
                                           double* applied_extra_pitch_radians) {
    if (!location) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    const XrResult result = next_locate_space_(space, base_space, time, location);
    if (XR_FAILED(result) || !settings.enabled) {
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
        return result;
    }

    if (!pivotxr_active) {
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
        return result;
    }

    const bool space_is_view = IsTrackedViewSpace(space);
    const bool base_space_is_view = IsTrackedViewSpace(base_space);
    if (space_is_view == base_space_is_view) {
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
        return result;
    }

    const XrPosef original_pose = location->pose;
    const ViewOrientation orientation{
        location->pose.orientation.x,
        location->pose.orientation.y,
        location->pose.orientation.z,
        location->pose.orientation.w,
    };
    const auto now = std::chrono::steady_clock::now();
    double delta_seconds = 0.0;
    if (pivotxr_last_smoothing_wall_time_.has_value()) {
        delta_seconds =
            std::chrono::duration<double>(now - *pivotxr_last_smoothing_wall_time_).count();
    }
    pivotxr_last_smoothing_wall_time_ = now;

    const double current_yaw_radians = ExtractYawRadians(orientation);
    const double current_pitch_radians = ExtractPitchRadians(orientation);
    const double extra_yaw_radians = ComputePivotExtraAngleRadians(current_yaw_radians,
                                                                   settings.yaw_rotation_multiplier,
                                                                   settings.yaw_deadzone_degrees,
                                                                   settings.yaw_max_extra_degrees,
                                                                   settings.yaw_smoothing,
                                                                   delta_seconds,
                                                                   pivotxr_smoothed_extra_yaw_radians_);
    const double extra_pitch_radians = ComputePivotExtraAngleRadians(current_pitch_radians,
                                                                     settings.pitch_rotation_multiplier,
                                                                     settings.pitch_deadzone_degrees,
                                                                     settings.pitch_max_extra_degrees,
                                                                     settings.pitch_smoothing,
                                                                     delta_seconds,
                                                                     pivotxr_smoothed_extra_pitch_radians_);
    if (applied_extra_yaw_radians) {
        *applied_extra_yaw_radians = extra_yaw_radians;
    }
    if (applied_extra_pitch_radians) {
        *applied_extra_pitch_radians = extra_pitch_radians;
    }
    if (NearlyZero(extra_yaw_radians) && NearlyZero(extra_pitch_radians)) {
        return result;
    }

    const XrPosef manipulated_pose =
        ApplyExtraRotationToPose(location->pose,
                                 static_cast<float>(extra_yaw_radians),
                                 static_cast<float>(extra_pitch_radians));
    location->pose = space_is_view ? manipulated_pose : InvertPose(manipulated_pose);
    CachePivotPoseDelta(time, MultiplyPoses(InvertPose(original_pose), location->pose));
    return result;
}

void OpenXrLayer::LogResolvedSettings(const ResolvedRuntimeConfig& settings) {
    std::ostringstream stream;
    stream << "Resolved settings for " << current_exe_name_ << ": "
           << "coreEnabled=" << settings.core.enabled
           << ", logLevel=" << ToString(settings.core.log_level)
           << ", depthxrEnabled=" << settings.depthxr.enabled
           << ", depthToggleBinding=" << BindingLabel(settings.depthxr_bindings.toggle_enabled)
           << ", stereoBoostEnabled=" << settings.depthxr.stereo_boost_enabled
           << ", stereoBoost=" << settings.depthxr.stereo_boost
           << ", convergenceEnabled=" << settings.depthxr.convergence_enabled
           << ", convergence=" << settings.depthxr.convergence
           << ", pivotxrEnabled=" << settings.pivotxr.enabled
           << ", pivotActivation=" << ToString(settings.pivotxr.activation_mode)
           << ", pivotActivationBinding=" << BindingLabel(settings.pivotxr.activation_binding)
           << ", pivotYawMultiplier=" << settings.pivotxr.yaw_rotation_multiplier
           << ", pivotYawSmoothing=" << settings.pivotxr.yaw_smoothing
           << ", pivotYawDeadzone=" << settings.pivotxr.yaw_deadzone_degrees
           << ", pivotYawMaxExtra=" << settings.pivotxr.yaw_max_extra_degrees
           << ", pivotPitchMultiplier=" << settings.pivotxr.pitch_rotation_multiplier
           << ", pivotPitchSmoothing=" << settings.pivotxr.pitch_smoothing
           << ", pivotPitchDeadzone=" << settings.pivotxr.pitch_deadzone_degrees
           << ", pivotPitchMaxExtra=" << settings.pivotxr.pitch_max_extra_degrees;
    logger_.Debug(stream.str());
}

void OpenXrLayer::ResetPivotActivationState() {
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_last_smoothing_wall_time_.reset();
    pivotxr_toggle_enabled_ = false;
    pivotxr_activation_key_was_down_ = false;
}

void OpenXrLayer::ResetDepthToggleState() {
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
}

bool OpenXrLayer::IsDepthXrActive() {
    if (!resolved_settings_.depthxr.enabled) {
        ResetDepthToggleState();
        return false;
    }

#if defined(_WIN32)
    const bool binding_down = IsInputBindingDown(resolved_settings_.depthxr_bindings.toggle_enabled);
    const bool was_pressed_this_call = binding_down && !depthxr_toggle_binding_was_down_;
    depthxr_toggle_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        depthxr_toggle_enabled_ = !depthxr_toggle_enabled_;
        pending_locate_views_diagnostics_ = 5;
        logger_.Info(std::string("Depth effects ") +
                     (depthxr_toggle_enabled_ ? "enabled" : "disabled") + " via " +
                     BindingLabel(resolved_settings_.depthxr_bindings.toggle_enabled) + ".");
    }

    return depthxr_toggle_enabled_;
#else
    return resolved_settings_.depthxr.enabled;
#endif
}

bool OpenXrLayer::IsPivotXrActive(const PivotXrResolvedSettings& settings) {
    if (!settings.enabled) {
        ResetPivotActivationState();
        return false;
    }

#if defined(_WIN32)
    const bool binding_down = IsInputBindingDown(settings.activation_binding);
    const bool was_pressed_this_call = binding_down && !pivotxr_activation_key_was_down_;
    pivotxr_activation_key_was_down_ = binding_down;

    if (settings.activation_mode == ActivationMode::Toggle) {
        if (was_pressed_this_call) {
            pivotxr_toggle_enabled_ = !pivotxr_toggle_enabled_;
            pending_locate_views_diagnostics_ = 5;
            logger_.Info(std::string("PivotXR extra pivot factor ") +
                         (pivotxr_toggle_enabled_ ? "enabled" : "disabled") + " via " +
                         BindingLabel(settings.activation_binding) + ".");
        }
        return pivotxr_toggle_enabled_;
    }

    return binding_down;
#else
    return settings.enabled;
#endif
}

} // namespace depthxr
