#include "depthxr/effects.h"

#include <cmath>

namespace depthxr {
namespace {

double RestoreValue(double submitted, double native, double rendered, double epsilon) {
    const double render_delta = native - rendered;
    if (std::abs(render_delta) <= epsilon || std::abs(submitted - native) <= epsilon) {
        return submitted;
    }
    return submitted + render_delta;
}

double RestoreAngle(double submitted, double native, double rendered) {
    constexpr double kTangentEpsilon = 0.00001;
    const double submitted_tangent = std::tan(submitted);
    const double native_tangent = std::tan(native);
    const double rendered_tangent = std::tan(rendered);
    const double restored_tangent = RestoreValue(
        submitted_tangent, native_tangent, rendered_tangent, kTangentEpsilon);
    return std::isfinite(restored_tangent) ? std::atan(restored_tangent) : submitted;
}

} // namespace

void RestoreDepthSubmissionView(ViewAdjustmentData& submitted_view,
                                const ViewAdjustmentData& native_view,
                                const ViewAdjustmentData& render_view) {
    constexpr double kPositionEpsilonMeters = 0.00001;
    submitted_view.position.x = RestoreValue(
        submitted_view.position.x, native_view.position.x, render_view.position.x, kPositionEpsilonMeters);
    submitted_view.position.y = RestoreValue(
        submitted_view.position.y, native_view.position.y, render_view.position.y, kPositionEpsilonMeters);
    submitted_view.position.z = RestoreValue(
        submitted_view.position.z, native_view.position.z, render_view.position.z, kPositionEpsilonMeters);

    submitted_view.fov.angle_left = RestoreAngle(
        submitted_view.fov.angle_left, native_view.fov.angle_left, render_view.fov.angle_left);
    submitted_view.fov.angle_right = RestoreAngle(
        submitted_view.fov.angle_right, native_view.fov.angle_right, render_view.fov.angle_right);
    submitted_view.fov.angle_up = RestoreAngle(
        submitted_view.fov.angle_up, native_view.fov.angle_up, render_view.fov.angle_up);
    submitted_view.fov.angle_down = RestoreAngle(
        submitted_view.fov.angle_down, native_view.fov.angle_down, render_view.fov.angle_down);
}

} // namespace depthxr
