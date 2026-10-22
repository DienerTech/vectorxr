#include "depthxr/framegen_plan.h"

#include "depthxr/config_path.h"
#include "depthxr/process_info.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace depthxr {
namespace {

using Matrix3 = std::array<std::array<float, 3>, 3>;

Matrix3 RotationMatrix(const std::array<float, 4>& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {{
        {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w), 2.0f * (x * z + y * w)},
        {2.0f * (x * y + z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w)},
        {2.0f * (x * z - y * w), 2.0f * (y * z + x * w), 1.0f - 2.0f * (x * x + y * y)},
    }};
}

// `newest`'s position in `previous`'s view space, meters.
std::array<float, 3> RelativeTranslation(const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous) {
    const auto& q = previous.orientation;
    const std::array<float, 3> delta{newest.position[0] - previous.position[0], newest.position[1] - previous.position[1],
                                     newest.position[2] - previous.position[2]};
    // Rotate by the inverse previous orientation: v + 2 w (u x v) + 2 u x (u x v), u = -q.xyz.
    const std::array<float, 3> u{-q[0], -q[1], -q[2]};
    const float w = q[3];
    const std::array<float, 3> uv{u[1] * delta[2] - u[2] * delta[1], u[2] * delta[0] - u[0] * delta[2],
                                  u[0] * delta[1] - u[1] * delta[0]};
    const std::array<float, 3> uuv{u[1] * uv[2] - u[2] * uv[1], u[2] * uv[0] - u[0] * uv[2], u[0] * uv[1] - u[1] * uv[0]};
    std::array<float, 3> translation{};
    for (int i = 0; i < 3; ++i) {
        translation[i] = delta[i] + 2.0f * (w * uv[i] + uuv[i]);
    }
    return translation;
}

std::array<float, 4> Tangents(const FrameGenViewGeometry& view) {
    return {std::tan(view.angle_left), std::tan(view.angle_right), std::tan(view.angle_up), std::tan(view.angle_down)};
}

std::uint32_t Half(std::uint32_t value) {
    return std::max<std::uint32_t>(1u, (value + 1u) / 2u);
}

FrameGenDispatch Dispatch(FrameGenPass pass, FrameGenExtent destination, FrameGenExtent source,
                          const FrameGenConstants& base) {
    FrameGenDispatch dispatch;
    dispatch.pass = pass;
    dispatch.width = destination.width;
    dispatch.height = destination.height;
    dispatch.constants = base;
    dispatch.constants.size = {destination.width, destination.height, source.width, source.height};
    return dispatch;
}

FrameGenTexture Texture(FrameGenTextureKind kind, std::uint8_t level = 0) {
    return {kind, level};
}

// The bits of an IEEE half float (f16tof32 in the shaders), rounded to
// nearest; for times between 0 and a few, so no infinities or subnormals.
std::uint32_t FrameGenHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        return sign;
    }
    if (exponent >= 31) {
        return sign | 0x7bffu;
    }
    return (sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13)) + ((mantissa >> 12) & 1u);
}

// The constants every dispatch of a view starts from: both views' tangents,
// the relative pose (with depth, the newest view's position in the previous
// view's space), the flags and the tuning scales.
FrameGenConstants BaseConstants(const FrameGenViewRequest& request, const FrameGenTuning& tuning,
                                FrameGenSynthesisMode mode) {
    const float suppression = static_cast<float>(std::clamp(tuning.artifact_suppression, 0, 100)) / 100.0f;
    const float limit = static_cast<float>(std::clamp(tuning.motion_limit_percent, 2, 25)) / 100.0f;
    FrameGenConstants base;
    base.fov_current = Tangents(request.newest);
    base.fov_previous = Tangents(request.previous);
    const Matrix3 rotation = FrameGenRelativeRotation(request.newest.orientation, request.previous.orientation);
    // The w components carry the newest view's position in the previous
    // view's space (meters), used with depth: a point at newest-view depth z
    // along direction d lies at R d + t / z, up to scale, in the previous view.
    const std::array<float, 3> translation =
        request.depth ? RelativeTranslation(request.newest, request.previous) : std::array<float, 3>{0.0f, 0.0f, 0.0f};
    base.rot0 = {rotation[0][0], rotation[0][1], rotation[0][2], translation[0]};
    base.rot1 = {rotation[1][0], rotation[1][1], rotation[1][2], translation[1]};
    base.rot2 = {rotation[2][0], rotation[2][1], rotation[2][2], translation[2]};
    std::uint32_t flags = 0;
    if (request.output == FrameGenOutputKind::kSrgb8) {
        flags |= kFrameGenFlagSrgbEncode;
    }
    if (mode == FrameGenSynthesisMode::kExtrapolate) {
        flags |= kFrameGenFlagExtrapolate;
    }
    if (request.temporal) {
        flags |= kFrameGenFlagTemporal;
    }
    if (request.depth) {
        flags |= kFrameGenFlagDepth;
    }
    if (tuning.occlusion && mode == FrameGenSynthesisMode::kInterpolate) {
        flags |= kFrameGenFlagOcclusion;
    }
    if (tuning.cross_fade && mode == FrameGenSynthesisMode::kInterpolate) {
        flags |= kFrameGenFlagCrossFade;
    }
    if (tuning.depth_match && request.depth) {
        flags |= kFrameGenFlagDepthMatch;
    }
    base.options = {kFrameGenSearchRadius, flags, static_cast<std::uint32_t>(tuning.diagnostic), 0};
    base.tuning = {std::clamp(tuning.fractions[0], 0.0f, 1.0f), limit, 4.0f + 26.0f * suppression,
                   3.0f + 21.0f * suppression};
    return base;
}

// The eye-shift map for a generated frame seen from `eye` (meters, in the
// newest view's space), built into the one map texture.
void AppendShiftMap(FrameGenViewPlan& plan, const FrameGenConstants& base, const std::array<float, 3>& eye,
                    FrameGenTexture depth) {
    FrameGenConstants mapping = base;
    mapping.fov_previous = mapping.fov_current;
    mapping.rot0 = {1.0f, 0.0f, 0.0f, -eye[0]};
    mapping.rot1 = {0.0f, 1.0f, 0.0f, -eye[1]};
    mapping.rot2 = {0.0f, 0.0f, 1.0f, -eye[2]};
    mapping.options = {0, kFrameGenFlagDepth, 0, 0};
    auto pass = [&](FrameGenPass kind, std::array<FrameGenTexture, 6> inputs, FrameGenTexture output,
                    FrameGenTexture second = {}) {
        FrameGenDispatch dispatch = Dispatch(kind, plan.levels[1], plan.levels[1], mapping);
        dispatch.inputs = inputs;
        dispatch.outputs = {output, second};
        plan.dispatches.push_back(dispatch);
    };
    const FrameGenTexture keys = Texture(FrameGenTextureKind::kShiftKeys);
    const FrameGenTexture target = Texture(FrameGenTextureKind::kShiftMap);
    pass(FrameGenPass::kShiftClear, {}, keys, target);
    pass(FrameGenPass::kShiftSplat, {depth, {}, {}, {}, {}, {}}, keys);
    pass(FrameGenPass::kShiftResolve, {depth, {}, keys, {}, {}, {}}, target);
    pass(FrameGenPass::kShiftFill, {keys, {}, {}, {}, {}, {}}, target);
}

// The motion map for a generated frame `step` (synthesis's: content at x in
// the newest frame shows at x - step * v) from the newest frame's resolved
// depth and filtered flow, built into kMotionMap with the eye-shift map's
// passes (their keys reused: built after that map).
void AppendMotionMap(FrameGenViewPlan& plan, const FrameGenConstants& base, float step, FrameGenTexture depth,
                     FrameGenTexture flow) {
    FrameGenConstants mapping = base;
    mapping.options = {0, kFrameGenFlagDepth | kFrameGenFlagDepthSplat, 0, 0};
    mapping.tuning[1] = step;
    auto pass = [&](FrameGenPass kind, std::array<FrameGenTexture, 6> inputs, FrameGenTexture output,
                    FrameGenTexture second = {}) {
        FrameGenDispatch dispatch = Dispatch(kind, plan.levels[1], plan.levels[1], mapping);
        dispatch.inputs = inputs;
        dispatch.outputs = {output, second};
        plan.dispatches.push_back(dispatch);
    };
    const FrameGenTexture keys = Texture(FrameGenTextureKind::kShiftKeys);
    const FrameGenTexture target = Texture(FrameGenTextureKind::kMotionMap);
    pass(FrameGenPass::kShiftClear, {}, keys, target);
    pass(FrameGenPass::kShiftSplat, {depth, flow, {}, {}, {}, {}}, keys);
    pass(FrameGenPass::kShiftResolve, {depth, flow, keys, {}, {}, {}}, target);
    pass(FrameGenPass::kShiftFill, {keys, flow, {}, {}, {}, {}}, target);
}

// The world's own motion (FrameGenViewRequest::world_motion), fitted to the
// measured flow at a grid of samples: each iteration weighs the samples by
// the last fit.
void AppendWorldFit(FrameGenViewPlan& plan, const FrameGenConstants& base, FrameGenTexture depth) {
    for (std::uint32_t iteration = 0; iteration < kFrameGenWorldIterations; ++iteration) {
        FrameGenDispatch reduce = Dispatch(FrameGenPass::kWorldReduce, {kFrameGenWorldGrid, kFrameGenWorldGrid},
                                           plan.levels[plan.final_flow_level], base);
        reduce.inputs = {Texture(FrameGenTextureKind::kFlow, plan.final_flow_level), {}, depth,
                         iteration ? Texture(FrameGenTextureKind::kWorldParams) : FrameGenTexture{}, {}};
        reduce.outputs = {Texture(FrameGenTextureKind::kWorldPartials), {}};
        reduce.constants.options[2] = iteration;
        reduce.constants.options[3] = plan.final_flow_level;
        plan.dispatches.push_back(reduce);
        FrameGenDispatch solve = Dispatch(FrameGenPass::kWorldSolve, {kFrameGenThreadGroupSize, kFrameGenThreadGroupSize},
                                          {kFrameGenThreadGroupSize, kFrameGenThreadGroupSize}, base);
        solve.inputs = {Texture(FrameGenTextureKind::kWorldPartials), {}, {}, {}, {}};
        solve.outputs = {Texture(FrameGenTextureKind::kWorldParams), {}};
        plan.dispatches.push_back(solve);
    }
}

// The quad views compositor's clamp (NaN clamps to `high`, unlike std::clamp).
double ComposeClamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

} // namespace

std::uint8_t FrameGenFinalFlowLevel(FrameGenFlowQuality quality) {
    switch (quality) {
    case FrameGenFlowQuality::kPerformance:
        return 4;
    case FrameGenFlowQuality::kQuality:
        return 2;
    default:
        return 3;
    }
}

std::uint8_t FrameGenFinalFlowLevel(const FrameGenTuning& tuning) {
    return tuning.hardware_flow ? kFrameGenHardwareFlowLevel : FrameGenFinalFlowLevel(tuning.quality);
}

std::uint8_t FrameGenMatchLevel(std::uint8_t flow_level) {
    return static_cast<std::uint8_t>(std::max(1, static_cast<int>(flow_level) - 2));
}

std::array<FrameGenExtent, kFrameGenPyramidLevels + 1> FrameGenPyramid(FrameGenExtent extent) {
    std::array<FrameGenExtent, kFrameGenPyramidLevels + 1> levels{};
    levels[0] = extent;
    for (std::size_t level = 1; level < levels.size(); ++level) {
        levels[level] = {Half(levels[level - 1].width), Half(levels[level - 1].height)};
    }
    return levels;
}

Matrix3 FrameGenRelativeRotation(const std::array<float, 4>& newest, const std::array<float, 4>& previous) {
    // world = R(newest) * d_newest;  d_previous = R(previous)^T * world.
    const Matrix3 a = RotationMatrix(previous);
    const Matrix3 b = RotationMatrix(newest);
    Matrix3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a[k][row] * b[k][column];
            }
            result[row][column] = sum;
        }
    }
    return result;
}

float FrameGenFraction(FrameGenSynthesisMode mode, std::int64_t previous_time, std::int64_t newest_time,
                       std::int64_t target_time) {
    const double interval = static_cast<double>(newest_time - previous_time);
    if (interval <= 0.0) {
        return mode == FrameGenSynthesisMode::kInterpolate ? 1.0f : 0.0f;
    }
    if (mode == FrameGenSynthesisMode::kInterpolate) {
        return static_cast<float>(std::clamp((target_time - previous_time) / interval, 0.0, 1.0));
    }
    return static_cast<float>(std::clamp((target_time - newest_time) / interval, 0.0, 1.0));
}

FrameGenComposeEye BuildFrameGenComposeEye(const FrameGenFov& full, const FrameGenFov& focus, FrameGenExtent output,
                                           FrameGenExtent focus_extent, double transition_percent,
                                           double sharpness_percent) {
    // Tangents of the float angles (std::tan(float)), widened to double.
    const double full_left = std::tan(full.left);
    const double full_right = std::tan(full.right);
    const double full_down = std::tan(full.down);
    const double full_up = std::tan(full.up);
    const double focus_left = std::tan(focus.left);
    const double focus_right = std::tan(focus.right);
    const double focus_down = std::tan(focus.down);
    const double focus_up = std::tan(focus.up);
    const double full_width = std::max(0.0001, full_right - full_left);
    const double full_height = std::max(0.0001, full_up - full_down);

    FrameGenComposeEye eye;
    eye.focus_rect[0] = static_cast<float>(ComposeClamp((focus_left - full_left) / full_width, 0.0, 1.0));
    eye.focus_rect[2] = static_cast<float>(ComposeClamp((focus_right - full_left) / full_width, 0.0, 1.0));
    eye.focus_rect[1] = static_cast<float>(ComposeClamp((full_up - focus_up) / full_height, 0.0, 1.0));
    eye.focus_rect[3] = static_cast<float>(ComposeClamp((full_up - focus_down) / full_height, 0.0, 1.0));

    // The rectangle's size from its rounded edges.
    const double rect_width = std::max(0.0001, static_cast<double>(eye.focus_rect[2]) - eye.focus_rect[0]);
    const double rect_height = std::max(0.0001, static_cast<double>(eye.focus_rect[3]) - eye.focus_rect[1]);
    const double transition = ComposeClamp(transition_percent, 0.0, 100.0) / 100.0;
    const double pixel_width = 1.0 / std::max<std::uint32_t>(1, output.width);
    const double pixel_height = 1.0 / std::max<std::uint32_t>(1, output.height);
    // The band is at least one output pixel wide.
    eye.feather[0] = static_cast<float>(std::max(pixel_width, rect_width * transition));
    eye.feather[1] = static_cast<float>(std::max(pixel_height, rect_height * transition));
    eye.sharpen = static_cast<float>(ComposeClamp(sharpness_percent, 0.0, 100.0) / 100.0);
    eye.focus_texel[0] = 1.0f / static_cast<float>(std::max<std::uint32_t>(1, focus_extent.width));
    eye.focus_texel[1] = 1.0f / static_cast<float>(std::max<std::uint32_t>(1, focus_extent.height));
    // Sharpening samples at output-pixel spacing: the frequencies that survive
    // the focus view's resample into the rectangle.
    eye.output_pixel_in_focus[0] = static_cast<float>(pixel_width / rect_width);
    eye.output_pixel_in_focus[1] = static_cast<float>(pixel_height / rect_height);
    return eye;
}

FrameGenConstants BuildFrameGenComposeConstants(const FrameGenComposeEye& eye, FrameGenExtent output) {
    FrameGenConstants constants{};
    constants.fov_current = eye.focus_rect;
    constants.fov_previous = {eye.feather[0], eye.feather[1], eye.sharpen, 0.0f};
    constants.rot0 = {eye.focus_texel[0], eye.focus_texel[1], eye.output_pixel_in_focus[0],
                      eye.output_pixel_in_focus[1]};
    constants.size = {output.width, output.height, 0u, 0u};
    if (eye.overlay.shown) {
        constants.options[0] = kFrameGenComposeOverlay |
                               (eye.overlay.eye_tracking ? kFrameGenComposeOverlayEyeTracking : 0u) |
                               (eye.overlay.gaze ? kFrameGenComposeOverlayGaze : 0u);
        constants.rot1 = eye.overlay.gaze_markers;
        constants.rot2 = eye.overlay.reference_markers;
        constants.tuning = {1.0f / static_cast<float>(std::max<std::uint32_t>(1, output.width)),
                            1.0f / static_cast<float>(std::max<std::uint32_t>(1, output.height)), 0.0f, 0.0f};
    }
    return constants;
}

FrameGenViewPlan BuildFrameGenViewPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning) {
    FrameGenViewPlan plan;
    plan.extent = request.extent;
    plan.levels = FrameGenPyramid(request.extent);
    plan.final_flow_level = FrameGenFinalFlowLevel(tuning);
    plan.synthesize = request.synthesize && request.extent.width >= 64 && request.extent.height >= 64;

    const FrameGenConstants base = BaseConstants(request, tuning, tuning.mode);
    const FrameGenTexture depth = request.depth ? Texture(FrameGenTextureKind::kDepth) : FrameGenTexture{};
    const std::size_t generated = std::clamp<std::size_t>(tuning.generated, 1, kFrameGenMaxGenerated);
    const auto all_outputs = static_cast<std::uint8_t>((1u << generated) - 1u);

    const FrameGenPass synthesize = request.output == FrameGenOutputKind::kFloat16 ? FrameGenPass::kSynthesizeFloat16
                                                                                   : FrameGenPass::kSynthesizeUnorm8;
    if (!plan.synthesize && tuning.analyze_only) {
        // Nothing to measure (no previous frame); synthesis writes it unchanged.
        return plan;
    }
    if (!plan.synthesize) {
        // Still image: the synthesis pass with the still flag and no flow.
        FrameGenDispatch still = Dispatch(synthesize, plan.levels[0], plan.levels[0], base);
        still.constants.options[1] |= kFrameGenFlagStill;
        still.constants.options[2] = static_cast<std::uint32_t>(FrameGenDiagnosticView::kOff);
        still.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), {}, {}, {}, {}};
        still.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
        still.copy_mask = all_outputs;
        plan.dispatches.push_back(still);
        return plan;
    }

    if (request.depth) {
        // Each depth texel's trust (the other eye's evidence, the previous
        // frame, last frame's score) before anything maps through the depth,
        // then the depth of disproved regions dropped.
        FrameGenDispatch trust = Dispatch(FrameGenPass::kTrustUpdate, plan.levels[1], plan.levels[1], base);
        trust.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), Texture(FrameGenTextureKind::kHistoryPrevious),
                        request.trust_history ? Texture(FrameGenTextureKind::kTrustPrevious) : FrameGenTexture{}, depth,
                        {}};
        // ...and counts them per tile (kFrameGenTrustTile).
        trust.outputs = {Texture(FrameGenTextureKind::kTrustNewest), Texture(FrameGenTextureKind::kTrustTiles)};
        trust.constants.options[0] = kFrameGenTrustMotion | (request.trust_stereo ? kFrameGenTrustStereo : 0u) |
                                     (request.trust_history ? kFrameGenTrustHistory : 0u);
        plan.dispatches.push_back(trust);
        // The scores per tile summed around each tile: texels without
        // evidence of their own follow the surface around them.
        const FrameGenExtent tiles = FrameGenTrustTiles(plan.levels[1]);
        FrameGenDispatch vote = Dispatch(FrameGenPass::kTrustVote, tiles, tiles, base);
        vote.inputs = {Texture(FrameGenTextureKind::kTrustTiles), {}, {}, {}, {}};
        vote.outputs = {Texture(FrameGenTextureKind::kTrustVotes), {}};
        plan.dispatches.push_back(vote);
        if (request.trust_stereo) {
            // The scale each surface's depth lines the eyes up best at (the
            // stereo probe was recorded with this frame's kDepthTrust).
            FrameGenDispatch scale = Dispatch(FrameGenPass::kDepthScaleVote, tiles, tiles, base);
            scale.inputs = {Texture(FrameGenTextureKind::kDepthScales), {}, {}, {}, {}};
            scale.outputs = {Texture(FrameGenTextureKind::kDepthScaleRaw), {}};
            plan.dispatches.push_back(scale);
            FrameGenDispatch accept = Dispatch(FrameGenPass::kDepthScaleVote, tiles, tiles, base);
            accept.inputs = {Texture(FrameGenTextureKind::kDepthScaleRaw), {}, {}, {}, {}};
            accept.outputs = {Texture(FrameGenTextureKind::kDepthScaleVotes), {}};
            accept.constants.options[0] = kFrameGenScaleAccept;
            plan.dispatches.push_back(accept);
        }
        FrameGenDispatch resolve = Dispatch(FrameGenPass::kTrustResolve, plan.levels[1], plan.levels[1], base);
        resolve.inputs = {Texture(FrameGenTextureKind::kTrustNewest), Texture(FrameGenTextureKind::kTrustVotes),
                          request.trust_stereo ? Texture(FrameGenTextureKind::kDepthScaleVotes) : FrameGenTexture{}, {},
                          {}};
        resolve.constants.options[0] = request.trust_stereo ? kFrameGenResolveScale : 0u;
        resolve.outputs = {Texture(FrameGenTextureKind::kDepth), {}};
        plan.dispatches.push_back(resolve);
    }

    // With the previous frame's depth, content it did not see is told apart
    // from content that moved.
    const bool depth_previous = request.depth && request.depth_previous;
    FrameGenDispatch luma = Dispatch(FrameGenPass::kLuma, plan.levels[1], plan.levels[0], base);
    luma.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), Texture(FrameGenTextureKind::kHistoryPrevious), depth,
                   depth_previous ? Texture(FrameGenTextureKind::kDepthPrevious) : FrameGenTexture{}, {}};
    luma.outputs = {Texture(FrameGenTextureKind::kLumaNewest, 1), Texture(FrameGenTextureKind::kLumaPrevious, 1)};
    luma.constants.options[1] |= depth_previous ? kFrameGenFlagDepthPrevious : 0u;
    luma.constants.options[3] = 1;
    plan.dispatches.push_back(luma);

    if (tuning.hardware_flow) {
        // The optical flow hardware matches the half-resolution luma pair
        // (run by the backend right before the unpack), and the unpack scores
        // each of its vectors the way block matching would. Frames it matched
        // as rendered need no pack, and the unpack takes the head movement
        // out of its vectors.
        if (!request.hardware_raw) {
            FrameGenDispatch pack = Dispatch(FrameGenPass::kFlowPack, plan.levels[1], plan.levels[1], base);
            pack.inputs = {Texture(FrameGenTextureKind::kLumaNewest, 1), Texture(FrameGenTextureKind::kLumaPrevious, 1),
                           {}, {}, {}};
            pack.outputs = {Texture(FrameGenTextureKind::kHardwareNewest), Texture(FrameGenTextureKind::kHardwarePrevious)};
            plan.dispatches.push_back(pack);
        }
        FrameGenDispatch unpack = Dispatch(FrameGenPass::kFlowUnpack, plan.levels[kFrameGenHardwareFlowLevel],
                                           plan.levels[1], base);
        unpack.inputs = {Texture(FrameGenTextureKind::kHardwareFlow), Texture(FrameGenTextureKind::kLumaNewest, 1),
                         Texture(FrameGenTextureKind::kLumaPrevious, 1), request.hardware_raw ? depth : FrameGenTexture{},
                         {}};
        unpack.outputs = {Texture(FrameGenTextureKind::kFlow, kFrameGenHardwareFlowLevel), {}};
        unpack.constants.options[0] = request.hardware_raw ? 1u : 0u;
        unpack.constants.options[3] = 1u << (kFrameGenHardwareFlowLevel - 1);
        plan.dispatches.push_back(unpack);
    }

    for (std::uint8_t level = 2; level <= kFrameGenPyramidLevels && !tuning.hardware_flow; ++level) {
        FrameGenDispatch down = Dispatch(FrameGenPass::kDownsample, plan.levels[level], plan.levels[level - 1], base);
        down.inputs = {Texture(FrameGenTextureKind::kLumaNewest, level - 1),
                       Texture(FrameGenTextureKind::kLumaPrevious, level - 1), {}, {}, {}};
        down.outputs = {Texture(FrameGenTextureKind::kLumaNewest, level), Texture(FrameGenTextureKind::kLumaPrevious, level)};
        down.constants.options[3] = level;
        plan.dispatches.push_back(down);
    }

    // Flow passes match on luma at `match` and write one vector per cell of
    // the flow level; options.w carries the cell size in match texels.
    if (!tuning.hardware_flow) {
        FrameGenDispatch search = Dispatch(FrameGenPass::kSearch, plan.levels[kFrameGenPyramidLevels],
                                           plan.levels[kFrameGenPyramidLevels], base);
        search.inputs = {Texture(FrameGenTextureKind::kLumaNewest, kFrameGenPyramidLevels),
                         Texture(FrameGenTextureKind::kLumaPrevious, kFrameGenPyramidLevels), {}, {}, {}};
        search.outputs = {Texture(FrameGenTextureKind::kFlow, kFrameGenPyramidLevels), {}};
        search.constants.options[3] = 1;
        plan.dispatches.push_back(search);
    }

    for (std::uint8_t level = kFrameGenPyramidLevels - 1; level >= plan.final_flow_level && !tuning.hardware_flow;
         --level) {
        const std::uint8_t match = FrameGenMatchLevel(level);
        FrameGenDispatch refine = Dispatch(FrameGenPass::kRefine, plan.levels[level], plan.levels[level + 1], base);
        refine.inputs = {Texture(FrameGenTextureKind::kLumaNewest, match), Texture(FrameGenTextureKind::kLumaPrevious, match),
                         Texture(FrameGenTextureKind::kFlow, level + 1),
                         request.temporal ? Texture(FrameGenTextureKind::kFilteredPrevious, plan.final_flow_level)
                                          : FrameGenTexture{},
                         {}};
        refine.outputs = {Texture(FrameGenTextureKind::kFlow, level), {}};
        refine.constants.options[3] = 1u << (level - match);
        refine.constants.tuning[0] = std::clamp(tuning.interval_ratio, 0.25f, 4.0f);
        plan.dispatches.push_back(refine);
        if (level == plan.final_flow_level) {
            break;
        }
    }

    FrameGenDispatch filter = Dispatch(FrameGenPass::kFilter, plan.levels[plan.final_flow_level],
                                       plan.levels[plan.final_flow_level], base);
    // Last frame's filtered flow: how steady each vector is; the depth:
    // where cockpit content stays still.
    filter.inputs = {Texture(FrameGenTextureKind::kFlow, plan.final_flow_level),
                     request.temporal ? Texture(FrameGenTextureKind::kFilteredPrevious, plan.final_flow_level)
                                      : FrameGenTexture{},
                     depth, {}, {}};
    filter.outputs = {Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level), {}};
    filter.constants.options[3] = plan.final_flow_level;
    filter.constants.tuning[0] = std::clamp(tuning.interval_ratio, 0.25f, 4.0f);
    const bool world = request.depth && request.world_motion;
    if (world && tuning.mode == FrameGenSynthesisMode::kExtrapolate) {
        AppendWorldFit(plan, base, depth);
        filter.inputs[3] = Texture(FrameGenTextureKind::kWorldParams);
        filter.constants.options[1] |= kFrameGenFlagWorld;
    }
    plan.dispatches.push_back(filter);
    if (tuning.analyze_only) {
        if (tuning.cover && tuning.mode == FrameGenSynthesisMode::kInterpolate) {
            // The same flow filtered as extrapolation filters it, for cover
            // frames past the newest frame.
            FrameGenDispatch cover = filter;
            cover.outputs = {Texture(FrameGenTextureKind::kFilteredCover, plan.final_flow_level), {}};
            cover.constants.options[1] |= kFrameGenFlagExtrapolate;
            if (world) {
                AppendWorldFit(plan, base, depth);
                cover.inputs[3] = Texture(FrameGenTextureKind::kWorldParams);
                cover.constants.options[1] |= kFrameGenFlagWorld;
            }
            plan.dispatches.push_back(cover);
        }
        return plan;
    }

    // Eye-shift maps, built into the one map texture right before the frames
    // written from it: one per generated frame whose eye has moved
    // kFrameGenShiftRebuild from where the last map was built for. Which
    // surface is in front changes with the eye, so a map scaled far from
    // its own eye position cut the edges of near content and left its gaps
    // in the wrong place. A spare frame (rarely shown) reuses the frame
    // before's map, scaled to its time. Times are past the newest frame
    // (before it when interpolating); a frame at the newest frame's own time
    // needs none.
    const float shift_length = std::sqrt(request.shift[0] * request.shift[0] + request.shift[1] * request.shift[1] +
                                         request.shift[2] * request.shift[2]);
    float map_time = 0.0f;
    for (std::size_t index = 0; index < generated; ++index) {
        const float fraction = std::clamp(tuning.fractions[index], 0.0f, 1.0f);
        const float time = tuning.mode == FrameGenSynthesisMode::kExtrapolate ? fraction : 1.0f - fraction;
        const bool spare = tuning.spare && generated > 1 && index + 1 == generated;
        const bool shifted = request.depth && time > 0.0f && shift_length * time >= kFrameGenMinimumShift;
        const bool rebuild = map_time <= 0.0f || (!spare && shift_length * std::abs(time - map_time) >= kFrameGenShiftRebuild);
        if (shifted && rebuild) {
            map_time = time;
            AppendShiftMap(plan, base, {request.shift[0] * time, request.shift[1] * time, request.shift[2] * time}, depth);
            plan.frames_in_order = true;
        }
        // The motion map for this frame's own time (FrameGenTuning::depth_splat),
        // extrapolating only.
        const bool motion = request.depth && tuning.depth_splat && time > 0.0f &&
                            tuning.mode == FrameGenSynthesisMode::kExtrapolate;
        if (motion) {
            AppendMotionMap(plan, base, -fraction, depth,
                            Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level));
            plan.frames_in_order = true;
        }
        FrameGenDispatch output = Dispatch(synthesize, plan.levels[0], plan.levels[plan.final_flow_level], base);
        output.inputs = {Texture(FrameGenTextureKind::kHistoryNewest),
                         Texture(FrameGenTextureKind::kHistoryPrevious),
                         Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level),
                         depth,
                         shifted ? Texture(FrameGenTextureKind::kShiftMap) : FrameGenTexture{},
                         motion ? Texture(FrameGenTextureKind::kMotionMap) : FrameGenTexture{}};
        if (shifted) {
            output.constants.options[1] |= kFrameGenFlagShift;
            output.constants.options[3] = FrameGenHalf(map_time);
        }
        if (motion) {
            output.constants.options[1] |= kFrameGenFlagDepthSplat;
        }
        output.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
        output.constants.tuning[0] = fraction;
        output.copy_mask = static_cast<std::uint8_t>(1u << index);
        plan.dispatches.push_back(output);
    }
    return plan;
}

FrameGenViewPlan BuildFrameGenSynthesisPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning,
                                            FrameGenSynthesisMode mode, float fraction, bool cover_flow,
                                            bool stale_flow) {
    FrameGenViewPlan plan;
    plan.extent = request.extent;
    plan.levels = FrameGenPyramid(request.extent);
    plan.final_flow_level = FrameGenFinalFlowLevel(tuning);
    plan.synthesize = request.synthesize && request.extent.width >= 64 && request.extent.height >= 64;
    const bool extrapolate = mode == FrameGenSynthesisMode::kExtrapolate;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    FrameGenConstants base = BaseConstants(request, tuning, mode);
    base.tuning[0] = fraction;
    const FrameGenPass synthesize = request.output == FrameGenOutputKind::kFloat16 ? FrameGenPass::kSynthesizeFloat16
                                                                                   : FrameGenPass::kSynthesizeUnorm8;
    // The time synthesis moves content by (past the newest frame, or before
    // it), and the eye's offset at the frame's time: the shift is the eye's
    // movement per unit forward in time, so interpolated frames sit back
    // toward the previous frame.
    const float time = extrapolate ? fraction : 1.0f - fraction;
    if (!plan.synthesize || time <= 0.0f) {
        FrameGenDispatch still = Dispatch(synthesize, plan.levels[0], plan.levels[0], base);
        still.constants.options[1] |= kFrameGenFlagStill;
        still.constants.options[2] = static_cast<std::uint32_t>(FrameGenDiagnosticView::kOff);
        still.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), {}, {}, {}, {}};
        still.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
        still.copy_mask = 1u;
        plan.synthesize = false;
        plan.dispatches.push_back(still);
        return plan;
    }
    if (stale_flow && extrapolate) {
        // The previous real frame's flow, read where the content was in that
        // frame; neither this frame's depth nor its trust is measured yet.
        // The eye's movement to the frame's time is shown all the same, from
        // the previous frame's resolved depth on this frame's grid: without
        // it, near content held still for the refresh (MSFS 2024 replays:
        // the cockpit as wrong as the frame unchanged).
        const std::array<float, 3> eye{request.shift[0] * fraction, request.shift[1] * fraction,
                                       request.shift[2] * fraction};
        const bool shifted = request.depth && request.depth_previous &&
                             std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]) >= kFrameGenMinimumShift;
        if (shifted) {
            FrameGenDispatch warp = Dispatch(FrameGenPass::kDepthStale, plan.levels[1], plan.levels[1], base);
            warp.inputs = {Texture(FrameGenTextureKind::kDepthPrevious), {}, {}, {}, {}};
            warp.outputs = {Texture(FrameGenTextureKind::kDepthStale), {}};
            plan.dispatches.push_back(warp);
            AppendShiftMap(plan, base, eye, Texture(FrameGenTextureKind::kDepthStale));
        }
        FrameGenDispatch output = Dispatch(synthesize, plan.levels[0], plan.levels[plan.final_flow_level], base);
        output.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), Texture(FrameGenTextureKind::kHistoryPrevious),
                         Texture(FrameGenTextureKind::kFilteredPrevious, plan.final_flow_level), {},
                         shifted ? Texture(FrameGenTextureKind::kShiftMap) : FrameGenTexture{}, {}};
        output.constants.options[1] &= ~(kFrameGenFlagDepth | kFrameGenFlagDepthMatch);
        output.constants.options[1] |= kFrameGenFlagStaleFlow;
        if (shifted) {
            output.constants.options[1] |= kFrameGenFlagShift;
            output.constants.options[3] = FrameGenHalf(fraction);
        }
        output.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
        output.copy_mask = 1u;
        plan.dispatches.push_back(output);
        return plan;
    }
    const FrameGenTexture depth = request.depth ? Texture(FrameGenTextureKind::kDepth) : FrameGenTexture{};
    const float eye_time = extrapolate ? fraction : fraction - 1.0f;
    const std::array<float, 3> eye{request.shift[0] * eye_time, request.shift[1] * eye_time, request.shift[2] * eye_time};
    const bool shifted =
        request.depth && std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]) >= kFrameGenMinimumShift;
    if (shifted) {
        AppendShiftMap(plan, base, eye, depth);
    }
    const FrameGenTexture flow = Texture(extrapolate && cover_flow ? FrameGenTextureKind::kFilteredCover
                                                                   : FrameGenTextureKind::kFilteredNewest,
                                         plan.final_flow_level);
    // The motion map for this frame's time (FrameGenTuning::depth_splat),
    // extrapolating only (cover frames included).
    const bool motion = request.depth && tuning.depth_splat && extrapolate;
    if (motion) {
        AppendMotionMap(plan, base, -fraction, depth, flow);
    }
    FrameGenDispatch output = Dispatch(synthesize, plan.levels[0], plan.levels[plan.final_flow_level], base);
    output.inputs = {Texture(FrameGenTextureKind::kHistoryNewest),
                     Texture(FrameGenTextureKind::kHistoryPrevious),
                     flow,
                     depth,
                     shifted ? Texture(FrameGenTextureKind::kShiftMap) : FrameGenTexture{},
                     motion ? Texture(FrameGenTextureKind::kMotionMap) : FrameGenTexture{}};
    if (motion) {
        output.constants.options[1] |= kFrameGenFlagDepthSplat;
    }
    if (shifted) {
        // The map is built for this frame's own eye position: synthesis
        // scales it by one.
        output.constants.options[1] |= kFrameGenFlagShift;
        output.constants.options[3] = FrameGenHalf(time);
    }
    output.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
    output.copy_mask = 1u;
    plan.dispatches.push_back(output);
    return plan;
}

FrameGenDispatch BuildFrameGenRawPackDispatch(FrameGenExtent view_extent) {
    const FrameGenExtent half = FrameGenPyramid(view_extent)[1];
    FrameGenDispatch dispatch = Dispatch(FrameGenPass::kFlowPackRaw, half, view_extent, {});
    dispatch.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), {}, {}, {}, {}};
    dispatch.outputs = {Texture(FrameGenTextureKind::kHardwareNewest), {}};
    return dispatch;
}

FrameGenDispatch BuildFrameGenDepthCheckDispatch(const FrameGenViewGeometry& view0, const FrameGenViewGeometry& view1,
                                                 const FrameGenDepthEncoding& encoding, float scale, float probe_step) {
    FrameGenDispatch dispatch;
    dispatch.pass = FrameGenPass::kDepthCheck;
    dispatch.width = kFrameGenDepthCheckCells;
    dispatch.height = kFrameGenDepthCheckCells;
    // View 0 plays the newest view and view 1 the previous one in the shared
    // mapping: direction rotation and position, view 0 into view 1.
    dispatch.constants.fov_current = Tangents(view0);
    dispatch.constants.fov_previous = Tangents(view1);
    const Matrix3 rotation = FrameGenRelativeRotation(view0.orientation, view1.orientation);
    const std::array<float, 3> translation = RelativeTranslation(view0, view1);
    dispatch.constants.rot0 = {rotation[0][0], rotation[0][1], rotation[0][2], translation[0]};
    dispatch.constants.rot1 = {rotation[1][0], rotation[1][1], rotation[1][2], translation[1]};
    dispatch.constants.rot2 = {rotation[2][0], rotation[2][1], rotation[2][2], translation[2]};
    dispatch.constants.size = {kFrameGenDepthCheckCells, kFrameGenDepthCheckCells, 0, 0};
    dispatch.constants.options = {0, kFrameGenFlagDepth, 0, kFrameGenDepthCheckSamples};
    // 1/near + 1/far: the swapped reading of a depth decoded to z (before
    // scaling) is 1 / (x - 1/z), whichever way round the decoding currently
    // is.
    auto inverse = [](float z) { return std::isfinite(z) && z > 0.0f ? 1.0f / z : 0.0f; };
    dispatch.constants.tuning = {inverse(encoding.near_z) + inverse(encoding.far_z), scale > 0.0f ? scale : 1.0f,
                                 probe_step, 0.0f};
    dispatch.outputs = {Texture(FrameGenTextureKind::kNone), {}};
    return dispatch;
}

FrameGenDispatch BuildFrameGenDepthCrossDispatch(FrameGenPass pass, std::uint32_t mode, FrameGenExtent source_extent,
                                                 FrameGenExtent target_extent, const FrameGenViewGeometry& source,
                                                 const FrameGenViewGeometry& target) {
    const FrameGenExtent from = FrameGenPyramid(source_extent)[1];
    const FrameGenExtent to = FrameGenPyramid(target_extent)[1];
    FrameGenDispatch dispatch;
    dispatch.pass = pass;
    // Clearing and resolving cover the target eye's grid, splatting the
    // source eye's.
    const bool splat = pass == FrameGenPass::kDepthCrossSplat && mode == 1;
    dispatch.width = splat ? from.width : to.width;
    dispatch.height = splat ? from.height : to.height;
    // The shared mapping (MapToPrevious): the source view plays the newest,
    // the target the previous one; direction rotation and position, source
    // into target.
    dispatch.constants.fov_current = Tangents(source);
    dispatch.constants.fov_previous = Tangents(target);
    const Matrix3 rotation = FrameGenRelativeRotation(source.orientation, target.orientation);
    const std::array<float, 3> translation = RelativeTranslation(source, target);
    dispatch.constants.rot0 = {rotation[0][0], rotation[0][1], rotation[0][2], translation[0]};
    dispatch.constants.rot1 = {rotation[1][0], rotation[1][1], rotation[1][2], translation[1]};
    dispatch.constants.rot2 = {rotation[2][0], rotation[2][1], rotation[2][2], translation[2]};
    dispatch.constants.size = pass == FrameGenPass::kDepthCrossResolve ? std::array<std::uint32_t, 4>{to.width, to.height, 0, 0}
                                                                        : std::array<std::uint32_t, 4>{from.width, from.height,
                                                                                                       to.width, to.height};
    dispatch.constants.options = {mode, kFrameGenFlagDepth, 0, 0};
    return dispatch;
}

FrameGenDispatch BuildFrameGenDepthTrustDispatch(FrameGenExtent view_extent, const FrameGenViewGeometry& own,
                                                 const FrameGenViewGeometry& other) {
    const auto levels = FrameGenPyramid(view_extent);
    FrameGenDispatch dispatch;
    dispatch.pass = FrameGenPass::kDepthTrust;
    dispatch.width = levels[1].width;
    dispatch.height = levels[1].height;
    dispatch.constants.fov_current = Tangents(own);
    dispatch.constants.fov_previous = Tangents(other);
    const Matrix3 rotation = FrameGenRelativeRotation(own.orientation, other.orientation);
    const std::array<float, 3> translation = RelativeTranslation(own, other);
    dispatch.constants.rot0 = {rotation[0][0], rotation[0][1], rotation[0][2], translation[0]};
    dispatch.constants.rot1 = {rotation[1][0], rotation[1][1], rotation[1][2], translation[1]};
    dispatch.constants.rot2 = {rotation[2][0], rotation[2][1], rotation[2][2], translation[2]};
    dispatch.constants.size = {levels[1].width, levels[1].height, 0, 0};
    dispatch.constants.options = {0, kFrameGenFlagDepth, 0, 0};
    // The other view's newest goes in t1, the depth in t2.
    dispatch.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), {}, Texture(FrameGenTextureKind::kDepth), {}, {}};
    dispatch.outputs = {Texture(FrameGenTextureKind::kTrustNewest), {}};
    return dispatch;
}

FrameGenDispatch BuildFrameGenDepthScaleDispatch(FrameGenExtent view_extent, const FrameGenViewGeometry& own,
                                                 const FrameGenViewGeometry& other) {
    FrameGenDispatch dispatch = BuildFrameGenDepthTrustDispatch(view_extent, own, other);
    const FrameGenExtent half = FrameGenPyramid(view_extent)[1];
    const FrameGenExtent tiles = FrameGenTrustTiles(half);
    dispatch.pass = FrameGenPass::kDepthScale;
    dispatch.width = tiles.width;
    dispatch.height = tiles.height;
    dispatch.constants.size = {tiles.width, tiles.height, half.width, half.height};
    dispatch.outputs = {Texture(FrameGenTextureKind::kDepthScales), {}};
    return dispatch;
}

const char* ToString(FrameGenDepthReading reading) {
    switch (reading) {
    case FrameGenDepthReading::kAsSubmitted:
        return "as submitted";
    case FrameGenDepthReading::kReversed:
        return "with near and far swapped";
    case FrameGenDepthReading::kUnusable:
        return "not usable";
    default:
        return "not checked";
    }
}

FrameGenDepthEvidence& FrameGenDepthEvidence::operator+=(const FrameGenDepthEvidence& other) {
    auto add = [](FrameGenDepthScore& a, const FrameGenDepthScore& b) {
        a.error += b.error;
        a.none += b.none;
        a.samples += b.samples;
        a.fallback += b.fallback;
    };
    add(decoded, other.decoded);
    add(swapped, other.swapped);
    binocular += other.binocular;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        probes[i] += other.probes[i];
    }
    probed += other.probed;
    cockpit += other.cockpit;
    source_varied += other.source_varied;
    other_varied += other.other_varied;
    return *this;
}

bool FrameGenDepthEyes::Add(std::size_t source, const FrameGenDepthEvidence& check) {
    // Decided for the session: the empty eye's depth is the other eye's
    // from now on, so its own can no longer be judged.
    if (empty_ || source != source_) {
        return false;
    }
    // Cells of the check's grid where depth differs from the view's centre:
    // a scene shows a few dozen at least (a panel's outline); clear depth
    // none at all.
    constexpr double kVaried = 16.0;
    constexpr std::uint32_t kChecks = 6;
    // Either eye may be the empty one: the checked eye's (its depth was
    // judged until now, so the other eye takes over the check and the
    // validator starts over), or the other eye's (the check already reads
    // the right one).
    const bool source_clear = check.source_varied == 0.0 && check.other_varied >= kVaried;
    const bool other_clear = check.other_varied == 0.0 && check.source_varied >= kVaried;
    if (source_clear != other_clear && (source_clear ? source_streak_ : other_streak_) + 1 >= kChecks) {
        empty_ = source_clear ? source_ : 1 - source_;
        source_ = 1 - *empty_;
        return source_clear;
    }
    source_streak_ = source_clear ? source_streak_ + 1 : 0;
    other_streak_ = other_clear ? other_streak_ + 1 : 0;
    return false;
}

void FrameGenDepthValidator::Add(const FrameGenDepthEvidence& check) {
    sums_ += check;
    if (++checks_ < 3) {
        return;
    }
    evidence_ = sums_;
    sums_ = {};
    checks_ = 0;
    ++batches_;
    const FrameGenDepthReading reading_before = reading_;
    const bool swapped_before = swapped_;

    // A reading is judged where it moves content; too few such samples (the
    // view is all scenery) decide nothing.
    const double enough = std::max(64.0, 0.02 * evidence_.binocular);
    auto judged = [&](const FrameGenDepthScore& score) { return score.samples >= enough; };
    // Depth must line the eyes up clearly better than no depth to be taken
    // up, and stops being used only once it is no better at all: looking
    // out at scenery, or a moment of noise, must not switch it on and off.
    constexpr double kAdopt = 0.7;
    constexpr double kDrop = 0.9;
    const FrameGenDepthScore& decoded = evidence_.decoded;
    const FrameGenDepthScore& swapped = evidence_.swapped;
    const bool decoded_adopts = judged(decoded) && decoded.Ratio() < kAdopt;
    const bool swapped_adopts = judged(swapped) && swapped.Ratio() < kAdopt;
    if (!Usable()) {
        if (swapped_adopts && (!decoded_adopts || swapped.Ratio() < decoded.Ratio())) {
            swapped_ = !swapped_;
        }
        if (decoded_adopts || swapped_adopts) {
            reading_ = swapped_ ? FrameGenDepthReading::kReversed : FrameGenDepthReading::kAsSubmitted;
        } else if (judged(decoded) && judged(swapped)) {
            reading_ = FrameGenDepthReading::kUnusable;
        }
    } else if (swapped_adopts && swapped.Ratio() < kAdopt * decoded.Ratio()) {
        swapped_ = !swapped_;
        reading_ = swapped_ ? FrameGenDepthReading::kReversed : FrameGenDepthReading::kAsSubmitted;
    } else if (judged(decoded) && decoded.Ratio() > kDrop) {
        reading_ = FrameGenDepthReading::kUnusable;
    }

    // The scale that lines the eyes up best: the probes' minimum, refined by
    // a parabola through it and its neighbours; at the edge of the probes,
    // a step that way with wider probes. A swap starts the search over.
    const float scale_before = scale_;
    const float step_before = probe_step_;
    if (swapped_ != swapped_before) {
        scale_ = 1.0f;
        probe_step_ = 0.08f;
    } else if (evidence_.probed >= std::max(200.0, 0.01 * evidence_.binocular)) {
        const auto& e = evidence_.probes;
        const double used = probe_step_; // the step these probes were taken with
        const std::size_t best = static_cast<std::size_t>(std::min_element(e.begin(), e.end()) - e.begin());
        const double centre = static_cast<double>(kFrameGenDepthScaleProbes / 2);
        double offset = static_cast<double>(best) - centre;
        if (best == 0 || best + 1 == kFrameGenDepthScaleProbes) {
            probe_step_ = std::min(probe_step_ * 1.5f, 0.25f);
        } else {
            const double curvature = e[best - 1] - 2.0 * e[best] + e[best + 1];
            if (curvature > 0.0) {
                offset += std::clamp(0.5 * (e[best - 1] - e[best + 1]) / curvature, -0.5, 0.5);
            }
            probe_step_ = std::max(probe_step_ * 0.6f, 0.02f);
        }
        // Moves of under 1% (or that gain under 1% over the current scale)
        // are noise.
        const double step = std::exp(used * offset);
        if (std::abs(offset) * used > 0.01 && e[best] < 0.99 * e[kFrameGenDepthScaleProbes / 2]) {
            scale_ = static_cast<float>(std::clamp(scale_ * step, 0.5, 2.0));
        }
    }
    if (swapped_ != swapped_before || scale_ != scale_before) {
        ++generation_;
    }
    if (swapped_ != swapped_before || scale_ != scale_before || probe_step_ != step_before) {
        ++check_generation_;
    }
    const bool changed = reading_ != reading_before || swapped_ != swapped_before || scale_ != scale_before;
    if (changed) {
        ++decisions_;
    }
    settled_ = !changed && reading_ != FrameGenDepthReading::kUnknown;
}

void FrameGenDepthValidator::Restart() {
    const std::uint32_t generation = generation_ + 1;
    const std::uint32_t check_generation = check_generation_ + 1;
    const std::uint32_t decisions = decisions_;
    const std::uint32_t batches = batches_;
    *this = {};
    generation_ = generation;
    check_generation_ = check_generation;
    decisions_ = decisions;
    batches_ = batches;
}

bool FrameGenDepthValidator::WantsCheck(std::uint64_t submission) const {
    return !settled_ || submission % 20 < 3;
}

FrameGenDispatch BuildFrameGenDepthDispatch(FrameGenExtent view_extent, std::int32_t rect_x, std::int32_t rect_y,
                                            std::uint32_t rect_width, std::uint32_t rect_height,
                                            std::uint32_t array_index, const FrameGenDepthEncoding& encoding) {
    const auto levels = FrameGenPyramid(view_extent);
    FrameGenDispatch dispatch;
    dispatch.pass = FrameGenPass::kDepth;
    dispatch.width = levels[1].width;
    dispatch.height = levels[1].height;
    dispatch.outputs = {Texture(FrameGenTextureKind::kDepth), {}};
    auto inverse = [](float z) { return std::isfinite(z) && z > 0.0f ? 1.0f / z : 0.0f; };
    // 1 / (scale z): the scale divides the decoded inverse depth.
    const float scale = encoding.scale > 0.0f ? encoding.scale : 1.0f;
    const float inverse_near = inverse(encoding.near_z) / scale;
    const float inverse_far = inverse(encoding.far_z) / scale;
    const float range = encoding.max_depth - encoding.min_depth;
    dispatch.constants.size = {levels[1].width, levels[1].height, rect_width, rect_height};
    dispatch.constants.options = {0, 0, 0, array_index};
    dispatch.constants.fov_current = {static_cast<float>(rect_x), static_cast<float>(rect_y),
                                      static_cast<float>(rect_width), static_cast<float>(rect_height)};
    dispatch.constants.tuning = {inverse_near, inverse_far - inverse_near, encoding.min_depth,
                                 std::abs(range) > 1e-9f ? 1.0f / range : 1.0f};
    return dispatch;
}

unsigned FrameGenHistoryState::Configure(FrameGenExtent extent, std::int64_t source_format, FrameGenOutputKind output,
                                         std::uint8_t final_level, std::uint8_t ring) {
    unsigned change = kNone;
    ring = std::clamp<std::uint8_t>(ring, 2, kFrameGenHistoryRingMax);
    if (extent.width != extent_.width || extent.height != extent_.height || source_format != source_format_ ||
        output != output_ || ring != ring_) {
        extent_ = extent;
        source_format_ = source_format;
        output_ = output;
        final_level_ = final_level;
        ring_ = ring;
        Invalidate();
        return kAll;
    }
    if (final_level != final_level_) {
        final_level_ = final_level;
        filtered_valid_ = {};
        change |= kFiltered;
    }
    return change;
}

bool FrameGenHistoryState::Matches(FrameGenExtent extent, std::int64_t source_format, FrameGenOutputKind output,
                                   std::uint8_t final_level) const {
    return MatchesImages(extent, source_format, output) && final_level == final_level_;
}

bool FrameGenHistoryState::MatchesImages(FrameGenExtent extent, std::int64_t source_format,
                                         FrameGenOutputKind output) const {
    return extent.width == extent_.width && extent.height == extent_.height && extent.width && extent.height &&
           source_format == source_format_ && output == output_;
}

FrameGenViewRequest FrameGenHistoryState::Effective(const FrameGenViewRequest& request) const {
    FrameGenViewRequest effective = request;
    effective.synthesize = request.synthesize && history_valid_[Previous()];
    effective.temporal = effective.synthesize && request.temporal && filtered_valid_[Previous()];
    effective.trust_history = effective.synthesize && trust_valid_[Previous()];
    return effective;
}

void FrameGenHistoryState::Commit(bool synthesized, bool trusted) {
    history_valid_[newest_] = true;
    filtered_valid_[newest_] = synthesized;
    trust_valid_[newest_] = trusted;
    newest_ = static_cast<std::uint8_t>((newest_ + 1u) % ring_);
}

void FrameGenHistoryState::Invalidate() {
    history_valid_ = {};
    filtered_valid_ = {};
    trust_valid_ = {};
    newest_ = 0;
}

std::array<float, 2> DecodeFrameGenMotion(const FrameGenMotionEncoding& encoding, const std::array<float, 4>& raw) {
    if (!encoding.ext) {
        return {raw[0], raw[1]};
    }
    return {raw[0] * encoding.scale[0] + encoding.offset[0], raw[1] * encoding.scale[1] + encoding.offset[1]};
}

const char* ToString(FrameGenEngineMode mode) {
    switch (mode) {
    case FrameGenEngineMode::kCensus:
        return "census";
    case FrameGenEngineMode::kCapture:
        return "capture";
    case FrameGenEngineMode::kObserve:
        return "observe";
    case FrameGenEngineMode::kAssist:
        return "assist";
    case FrameGenEngineMode::kLead:
        return "lead";
    default:
        return "off";
    }
}

FrameGenEngineMode ParseFrameGenEngineMode(const char* value) {
    if (!value) {
        return FrameGenEngineMode::kOff;
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "census"    ? FrameGenEngineMode::kCensus
           : text == "capture" ? FrameGenEngineMode::kCapture
                               : FrameGenEngineMode::kOff;
}

FrameGenDispatch BuildFrameGenMotionCaptureDispatch(FrameGenExtent l1, const std::array<std::int32_t, 4>& rect0,
                                                    const std::array<std::int32_t, 4>& rect1, std::uint32_t slice0,
                                                    std::uint32_t slice1, std::uint32_t views) {
    FrameGenDispatch dispatch;
    dispatch.pass = FrameGenPass::kEngineCapture;
    dispatch.width = l1.width;
    dispatch.height = l1.height;
    dispatch.outputs = {Texture(FrameGenTextureKind::kEngineRaw), {}};
    dispatch.constants.size = {l1.width, l1.height, static_cast<std::uint32_t>(std::max(rect0[2], 0)),
                               static_cast<std::uint32_t>(std::max(rect0[3], 0))};
    dispatch.constants.fov_current = {static_cast<float>(rect0[0]), static_cast<float>(rect0[1]),
                                      static_cast<float>(rect1[0]), static_cast<float>(rect1[1])};
    dispatch.constants.fov_previous = {static_cast<float>(rect1[2]), static_cast<float>(rect1[3]),
                                       static_cast<float>(slice0), static_cast<float>(slice1)};
    dispatch.constants.options = {views, 0, 0, 0};
    return dispatch;
}

FrameGenViewMapping BuildFrameGenViewMapping(const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous,
                                             bool depth) {
    FrameGenViewMapping mapping;
    mapping.fov_current = Tangents(newest);
    mapping.fov_previous = Tangents(previous);
    mapping.rotation = FrameGenRelativeRotation(newest.orientation, previous.orientation);
    mapping.translation = depth ? RelativeTranslation(newest, previous) : std::array<float, 3>{0.0f, 0.0f, 0.0f};
    mapping.depth = depth;
    return mapping;
}

bool FrameGenMapToPrevious(const FrameGenViewMapping& mapping, const std::array<float, 2>& uv, float depth,
                           std::array<float, 2>& previous_uv) {
    const auto& current = mapping.fov_current;
    const auto& before = mapping.fov_previous;
    const std::array<float, 3> direction{current[0] + (current[1] - current[0]) * uv[0],
                                         current[2] + (current[3] - current[2]) * uv[1], -1.0f};
    std::array<float, 3> rotated{};
    for (int i = 0; i < 3; ++i) {
        rotated[i] = mapping.rotation[i][0] * direction[0] + mapping.rotation[i][1] * direction[1] +
                     mapping.rotation[i][2] * direction[2];
    }
    if (mapping.depth && depth > 0.0f && depth < kFrameGenFarDepth) {
        for (int i = 0; i < 3; ++i) {
            rotated[i] += mapping.translation[i] / depth;
        }
    }
    previous_uv = {0.0f, 0.0f};
    if (rotated[2] > -1e-4f) {
        return false;
    }
    previous_uv = {(rotated[0] / -rotated[2] - before[0]) / (before[1] - before[0]),
                   (rotated[1] / -rotated[2] - before[2]) / (before[3] - before[2])};
    return previous_uv[0] >= 0.0f && previous_uv[1] >= 0.0f && previous_uv[0] <= 1.0f && previous_uv[1] <= 1.0f;
}

bool FrameGenMapToNewest(const FrameGenViewMapping& mapping, const std::array<float, 2>& previous_uv, float depth,
                         std::array<float, 2>& newest_uv) {
    const auto& current = mapping.fov_current;
    const auto& before = mapping.fov_previous;
    const std::array<float, 3> seen{before[0] + (before[1] - before[0]) * previous_uv[0],
                                    before[2] + (before[3] - before[2]) * previous_uv[1], -1.0f};
    // R transposed: newest-view directions from previous-view ones.
    std::array<float, 3> located{};
    std::array<float, 3> back{};
    for (int i = 0; i < 3; ++i) {
        located[i] = mapping.rotation[0][i] * seen[0] + mapping.rotation[1][i] * seen[1] + mapping.rotation[2][i] * seen[2];
        back[i] = mapping.rotation[0][i] * mapping.translation[0] + mapping.rotation[1][i] * mapping.translation[1] +
                  mapping.rotation[2][i] * mapping.translation[2];
    }
    if (mapping.depth && depth > 0.0f && depth < kFrameGenFarDepth && std::abs(located[2]) > 1e-6f) {
        // The point is s * seen in the previous view and R^T (s seen - t) in
        // the newest, at depth `depth` there.
        const float s = (back[2] - depth) / located[2];
        for (int i = 0; i < 3; ++i) {
            located[i] = s * located[i] - back[i];
        }
    }
    newest_uv = {0.0f, 0.0f};
    if (located[2] > -1e-4f) {
        return false;
    }
    newest_uv = {(located[0] / -located[2] - current[0]) / (current[1] - current[0]),
                 (located[1] / -located[2] - current[2]) / (current[3] - current[2])};
    return true;
}

// --- Engine motion vectors: the image tracker ------------------------------

namespace {

// Informative frames the tracker scores over, and those that decide whether
// the images are written at all.
constexpr std::size_t kTrackerWindow = 120;
constexpr std::size_t kTrackerWrittenWindow = 300;
constexpr std::uint32_t kTrackerLockFrames = 60;
constexpr double kTrackerLockShare = 0.95;
constexpr double kTrackerRunnerUpShare = 0.60;
constexpr std::uint32_t kTrackerMispredictions = 5;
constexpr double kTrackerUnwrittenShare = 0.90;
// Frames in a row a swapchain released an image before its releases decide.
constexpr std::uint32_t kTrackerReleasedFrames = 3;

} // namespace

const char* ToString(FrameGenEngineImageTracker::Policy policy) {
    using Policy = FrameGenEngineImageTracker::Policy;
    switch (policy) {
    case Policy::kReleased:
        return "released";
    case Policy::kConstant:
        return "constant";
    case Policy::kCycle:
        return "cycle";
    case Policy::kColor:
        return "follows color";
    case Policy::kDepth:
        return "follows depth";
    case Policy::kColorNext:
        return "color + 1";
    case Policy::kColorPrevious:
        return "color - 1";
    case Policy::kAll:
        return "all written";
    case Policy::kUnwritten:
        return "not written";
    default:
        return "none";
    }
}

void FrameGenEngineImageTracker::Restart() {
    hashes_ = {};
    window_.clear();
    written_.clear();
    released_streak_ = 0;
    if (locked_) {
        locked_ = false;
        ++unlocks_;
    }
    lock_ = {};
}

std::uint32_t FrameGenEngineImageTracker::Predict(const Hypothesis& hypothesis, const Entry& entry) const {
    const std::uint32_t images = std::max(images_, 1u);
    switch (hypothesis.policy) {
    case Policy::kConstant:
        return hypothesis.parameter % images;
    case Policy::kCycle:
        return static_cast<std::uint32_t>((entry.frame + hypothesis.parameter) % images);
    case Policy::kDepth:
        return entry.depth_index % images;
    case Policy::kColorNext:
        return (entry.color_index + 1) % images;
    case Policy::kColorPrevious:
        return (entry.color_index + images - 1) % images;
    default:
        return entry.color_index % images;
    }
}

std::vector<FrameGenEngineImageTracker::Hypothesis> FrameGenEngineImageTracker::Score() const {
    // In the order ties go: following color first (the best guess before
    // anything is known), then depth, then the rest.
    std::vector<Hypothesis> hypotheses{{Policy::kColor}, {Policy::kDepth}, {Policy::kColorNext}, {Policy::kColorPrevious}};
    for (std::uint32_t k = 0; k < images_; ++k) {
        hypotheses.push_back({Policy::kConstant, k});
    }
    for (std::uint32_t phase = 0; phase < images_; ++phase) {
        hypotheses.push_back({Policy::kCycle, phase});
    }
    const double singles = SingleChanges();
    for (Hypothesis& hypothesis : hypotheses) {
        std::uint32_t explained = 0;
        for (const Entry& entry : window_) {
            explained += entry.change == Change::kSingle && Predict(hypothesis, entry) == entry.image ? 1u : 0u;
        }
        hypothesis.share = singles > 0.0 ? explained / singles : 0.0;
    }
    return hypotheses;
}

void FrameGenEngineImageTracker::Add(const FrameGenEngineTrackerFrame& frame) {
    const std::uint32_t images = std::min(frame.images, kFrameGenEngineImages);
    if (frame.handle != handle_ || frame.rect != rect_ || images != images_) {
        // Other images, or another part of them: nothing learned holds.
        Restart();
        handle_ = frame.handle;
        rect_ = frame.rect;
        images_ = images;
    }
    released_streak_ = frame.released ? released_streak_ + 1 : 0;
    // The images whose hash changed since their own last census. A census
    // of some of them keeps theirs for the next census of every image
    // (production reads the chosen image alone between those): only such a
    // census tells which images a frame wrote.
    bool known = images > 0;
    std::uint32_t changed = 0;
    std::uint32_t last = 0;
    for (std::uint32_t i = 0; i < images; ++i) {
        if (((frame.censused >> i) & 1u) == 0) {
            known = false;
            continue;
        }
        if (!hashes_[i]) {
            known = false;
        } else if (*hashes_[i] != frame.hashes[i]) {
            ++changed;
            last = i;
        }
        hashes_[i] = frame.hashes[i];
    }
    if (!known || !frame.informative) {
        return;
    }
    Entry entry;
    entry.frame = frame.frame;
    entry.color_index = frame.color_index;
    entry.depth_index = frame.depth_index;
    if (changed == 1) {
        entry.change = Change::kSingle;
        entry.image = last;
    } else if (changed > 1) {
        bool alike = changed == images;
        for (std::uint32_t i = 1; i < images && alike; ++i) {
            alike = frame.hashes[i] == frame.hashes[0];
        }
        entry.change = alike ? Change::kAll : Change::kSeveral;
    }
    if (locked_ && entry.change == Change::kSingle && Predict(lock_, entry) != entry.image) {
        entry.mispredicted = true;
        ++mispredictions_;
    }
    window_.push_back(entry);
    while (window_.size() > kTrackerWindow) {
        window_.pop_front();
    }
    written_.push_back(changed > 0 ? 1 : 0);
    while (written_.size() > kTrackerWrittenWindow) {
        written_.pop_front();
    }
    if (locked_) {
        const auto wrong = std::count_if(window_.begin(), window_.end(), [](const Entry& e) { return e.mispredicted; });
        if (wrong >= kTrackerMispredictions) {
            locked_ = false;
            ++unlocks_;
            for (Entry& e : window_) {
                e.mispredicted = false;
            }
        }
    }
    if (!locked_ && SingleChanges() >= kTrackerLockFrames) {
        const Hypothesis best = Best();
        if (best.share >= kTrackerLockShare && RunnerUp().share <= kTrackerRunnerUpShare &&
            best.policy != Policy::kNone && best.policy != Policy::kReleased && best.policy != Policy::kAll &&
            best.policy != Policy::kUnwritten) {
            locked_ = true;
            lock_ = best;
            ++locks_;
        }
    }
}

bool FrameGenEngineImageTracker::NotWritten() const {
    if (written_.size() < kTrackerWrittenWindow) {
        return false;
    }
    const auto unwritten = std::count(written_.begin(), written_.end(), std::uint8_t{0});
    return static_cast<double>(unwritten) > kTrackerUnwrittenShare * static_cast<double>(written_.size());
}

std::uint32_t FrameGenEngineImageTracker::SingleChanges() const {
    return static_cast<std::uint32_t>(
        std::count_if(window_.begin(), window_.end(), [](const Entry& e) { return e.change == Change::kSingle; }));
}

std::uint32_t FrameGenEngineImageTracker::AllChanged() const {
    return static_cast<std::uint32_t>(
        std::count_if(window_.begin(), window_.end(), [](const Entry& e) { return e.change == Change::kAll; }));
}

std::uint32_t FrameGenEngineImageTracker::SeveralChanged() const {
    return static_cast<std::uint32_t>(
        std::count_if(window_.begin(), window_.end(), [](const Entry& e) { return e.change == Change::kSeveral; }));
}

std::uint32_t FrameGenEngineImageTracker::NoneChanged() const {
    return static_cast<std::uint32_t>(
        std::count_if(window_.begin(), window_.end(), [](const Entry& e) { return e.change == Change::kNone; }));
}

FrameGenEngineImageTracker::Hypothesis FrameGenEngineImageTracker::Best() const {
    if (released_streak_ >= kTrackerReleasedFrames) {
        return {Policy::kReleased, 0, 1.0};
    }
    if (NotWritten()) {
        const auto unwritten = std::count(written_.begin(), written_.end(), std::uint8_t{0});
        return {Policy::kUnwritten, 0, static_cast<double>(unwritten) / static_cast<double>(written_.size())};
    }
    const std::uint32_t singles = SingleChanges();
    const std::uint32_t all = AllChanged();
    if (all > singles && !window_.empty()) {
        return {Policy::kAll, 0, static_cast<double>(all) / static_cast<double>(window_.size())};
    }
    const std::vector<Hypothesis> scored = Score();
    if (locked_) {
        for (const Hypothesis& hypothesis : scored) {
            if (hypothesis.policy == lock_.policy && hypothesis.parameter == lock_.parameter) {
                return hypothesis;
            }
        }
    }
    if (singles == 0) {
        return {};
    }
    Hypothesis best = scored.front();
    for (const Hypothesis& hypothesis : scored) {
        if (hypothesis.share > best.share) {
            best = hypothesis;
        }
    }
    return best;
}

FrameGenEngineImageTracker::Hypothesis FrameGenEngineImageTracker::RunnerUp() const {
    const Hypothesis best = Best();
    if (best.policy == Policy::kNone || best.policy == Policy::kReleased || best.policy == Policy::kAll ||
        best.policy == Policy::kUnwritten) {
        return {};
    }
    Hypothesis runner_up;
    for (const Hypothesis& hypothesis : Score()) {
        // A policy predicting what the best does on every frame is the same.
        const bool differs = std::any_of(window_.begin(), window_.end(), [&](const Entry& entry) {
            return entry.change == Change::kSingle && Predict(hypothesis, entry) != Predict(best, entry);
        });
        if (differs && hypothesis.share > runner_up.share) {
            runner_up = hypothesis;
        }
    }
    return runner_up;
}

std::optional<std::uint32_t> FrameGenEngineImageTracker::Image(std::uint64_t frame, std::uint32_t color_index,
                                                               std::uint32_t depth_index,
                                                               std::optional<std::uint32_t> released) const {
    if (released) {
        return released;
    }
    if (NotWritten()) {
        return std::nullopt;
    }
    Entry entry;
    entry.frame = frame;
    entry.color_index = color_index;
    entry.depth_index = depth_index;
    const Hypothesis hypothesis = locked_ ? lock_ : Best();
    switch (hypothesis.policy) {
    case Policy::kConstant:
    case Policy::kCycle:
    case Policy::kDepth:
    case Policy::kColorNext:
    case Policy::kColorPrevious:
        return Predict(hypothesis, entry);
    default:
        return Predict({Policy::kColor}, entry);
    }
}

// --- Engine motion vectors: calibration ------------------------------------

namespace {

using Vector3 = std::array<double, 3>;

Vector3 Rotate(const std::array<float, 4>& q, const Vector3& v) {
    // v + 2 w (u x v) + 2 u x (u x v), u = q.xyz.
    const Vector3 u{q[0], q[1], q[2]};
    const double w = q[3];
    const Vector3 uv{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const Vector3 uuv{u[1] * uv[2] - u[2] * uv[1], u[2] * uv[0] - u[0] * uv[2], u[0] * uv[1] - u[1] * uv[0]};
    return {v[0] + 2.0 * (w * uv[0] + uuv[0]), v[1] + 2.0 * (w * uv[1] + uuv[1]), v[2] + 2.0 * (w * uv[2] + uuv[2])};
}

std::array<float, 4> Conjugate(const std::array<float, 4>& q) {
    return {-q[0], -q[1], -q[2], q[3]};
}

Vector3 Minus(const Vector3& a, const Vector3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vector3 Position(const FrameGenViewGeometry& view) {
    return {view.position[0], view.position[1], view.position[2]};
}

} // namespace

const char* ToString(FrameGenEngineCamera camera) {
    switch (camera) {
    case FrameGenEngineCamera::kRotationOnly:
        return "rotation only";
    case FrameGenEngineCamera::kExcluded:
        return "head excluded";
    case FrameGenEngineCamera::kObjectOnly:
        return "objects only";
    default:
        return "head included";
    }
}

void FrameGenEngineAxisFit::Add(double e, double rotation, double translation) {
    n_ += 1.0;
    r_ += rotation;
    t_ += translation;
    e_ += e;
    rr_ += rotation * rotation;
    tt_ += translation * translation;
    rt_ += rotation * translation;
    er_ += e * rotation;
    et_ += e * translation;
    ee_ += e * e;
}

FrameGenEngineAxisFit& FrameGenEngineAxisFit::operator+=(const FrameGenEngineAxisFit& other) {
    n_ += other.n_;
    r_ += other.r_;
    t_ += other.t_;
    e_ += other.e_;
    rr_ += other.rr_;
    tt_ += other.tt_;
    rt_ += other.rt_;
    er_ += other.er_;
    et_ += other.et_;
    ee_ += other.ee_;
    return *this;
}

FrameGenEngineAxisFit::Solution FrameGenEngineAxisFit::Solve() const {
    Solution solution;
    solution.samples = n_;
    if (n_ < 4.0) {
        return solution;
    }
    // Centered sums: the intercept drops out of the 2x2 system.
    const double srr = rr_ - r_ * r_ / n_;
    const double stt = tt_ - t_ * t_ / n_;
    const double srt = rt_ - r_ * t_ / n_;
    const double ser = er_ - e_ * r_ / n_;
    const double set = et_ - e_ * t_ / n_;
    const double see = ee_ - e_ * e_ / n_;
    const double det = srr * stt - srt * srt;
    if (!(srr > 1e-12 * n_) || !(stt > 1e-12 * n_) || !(det > 1e-9 * srr * stt)) {
        return solution;
    }
    solution.a = (ser * stt - set * srt) / det;
    solution.b = (set * srr - ser * srt) / det;
    solution.c = (e_ - solution.a * r_ - solution.b * t_) / n_;
    const double explained = solution.a * ser + solution.b * set;
    solution.corr2 = see > 0.0 ? std::clamp(explained / see, 0.0, 1.0) : 0.0;
    solution.valid = true;
    return solution;
}

FrameGenEngineAxisFit::Solution FrameGenEngineAxisFit::SolveLine() const {
    Solution solution;
    solution.samples = n_;
    if (n_ < 3.0) {
        return solution;
    }
    const double q = r_ + t_;
    const double sqq = rr_ + 2.0 * rt_ + tt_ - q * q / n_;
    const double seq = er_ + et_ - e_ * q / n_;
    const double see = ee_ - e_ * e_ / n_;
    if (!(sqq > 1e-12 * n_)) {
        return solution;
    }
    solution.a = seq / sqq;
    solution.b = solution.a;
    solution.c = (e_ - solution.a * q) / n_;
    solution.corr2 = see > 0.0 ? std::clamp(seq * seq / (sqq * see), 0.0, 1.0) : 0.0;
    solution.valid = true;
    return solution;
}

std::vector<FrameGenEngineGain> FrameGenEngineCanonicalGains(FrameGenExtent vectors, FrameGenExtent color) {
    const double vw = std::max<std::uint32_t>(vectors.width, 1);
    const double vh = std::max<std::uint32_t>(vectors.height, 1);
    const double cw = std::max<std::uint32_t>(color.width, 1);
    const double ch = std::max<std::uint32_t>(color.height, 1);
    return {
        {"ndc", {1.0, 1.0}},
        {"vulkan ndc", {1.0, -1.0}},
        {"uv", {2.0, -2.0}},
        {"vector pixels", {2.0 / vw, -2.0 / vh}},
        {"color pixels", {2.0 / cw, -2.0 / ch}},
        {"ndc, previous - current", {-1.0, -1.0}},
        {"vulkan ndc, previous - current", {-1.0, 1.0}},
        {"uv, previous - current", {-2.0, 2.0}},
        {"vector pixels, previous - current", {-2.0 / vw, 2.0 / vh}},
        {"color pixels, previous - current", {-2.0 / cw, 2.0 / ch}},
    };
}

FrameGenEngineGain NearestFrameGenEngineGain(const std::array<double, 2>& gain, FrameGenExtent vectors,
                                             FrameGenExtent color, double& error) {
    FrameGenEngineGain nearest;
    error = 1e30;
    for (const FrameGenEngineGain& candidate : FrameGenEngineCanonicalGains(vectors, color)) {
        const double candidate_error = std::max(std::abs(gain[0] - candidate.gain[0]) / std::abs(candidate.gain[0]),
                                                std::abs(gain[1] - candidate.gain[1]) / std::abs(candidate.gain[1]));
        if (candidate_error < error) {
            error = candidate_error;
            nearest = candidate;
        }
    }
    return nearest;
}

bool FrameGenEnginePrediction(const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous,
                              const std::array<float, 4>& delta_orientation, const std::array<float, 3>& delta_position,
                              int delta_sign, FrameGenEngineCamera camera, const std::array<float, 2>& uv, float depth,
                              std::array<double, 2>& prediction) {
    prediction = {0.0, 0.0};
    if (camera == FrameGenEngineCamera::kObjectOnly) {
        return false; // nothing that stays still moves
    }
    const bool infinite = !(depth > 0.0f) || depth >= kFrameGenFarDepth || camera == FrameGenEngineCamera::kRotationOnly;
    const bool cockpit = !infinite && depth < kFrameGenEngineCockpit;
    const bool world = infinite || depth >= kFrameGenEngineWorld;
    if (!cockpit && !world) {
        return false;
    }
    const std::array<float, 4> current = Tangents(newest);
    const Vector3 direction{current[0] + (current[1] - current[0]) * uv[0], current[2] + (current[3] - current[2]) * uv[1],
                            -1.0};
    // The content in the application's space now, and where it was: a
    // direction alone at infinity, else a point.
    const std::array<float, 4> delta =
        delta_sign < 0 ? Conjugate(delta_orientation) : delta_orientation; // X(N) = R_D X(N-1) + p_D
    Vector3 now = Rotate(newest.orientation, direction);
    Vector3 before = now;
    if (!infinite) {
        const Vector3 point{direction[0] * depth, direction[1] * depth, direction[2] * depth};
        const Vector3 rotated = Rotate(newest.orientation, point);
        now = {rotated[0] + newest.position[0], rotated[1] + newest.position[1], rotated[2] + newest.position[2]};
        before = now;
    }
    if (world && camera != FrameGenEngineCamera::kRotationOnly) {
        const Vector3 moved =
            infinite ? now
                     : Minus(now, {delta_position[0], delta_position[1], delta_position[2]});
        before = Rotate(Conjugate(delta), moved);
    }
    // Seen from the previous frame's camera (head included), or from this
    // frame's (head excluded).
    const FrameGenViewGeometry& eye = camera == FrameGenEngineCamera::kExcluded ? newest : previous;
    const Vector3 relative = infinite ? before : Minus(before, Position(eye));
    const Vector3 seen = Rotate(Conjugate(eye.orientation), relative);
    if (seen[2] > -1e-6) {
        return false;
    }
    const std::array<float, 4> tangents = Tangents(eye);
    const double u = (seen[0] / -seen[2] - tangents[0]) / (tangents[1] - tangents[0]);
    const double v = (seen[1] / -seen[2] - tangents[2]) / (tangents[3] - tangents[2]);
    // NDC(u, v) = (2u - 1, 1 - 2v), current minus previous.
    prediction = {2.0 * (uv[0] - u), -2.0 * (uv[1] - v)};
    return true;
}

FrameGenEngineCalibration CalibrateFrameGenEngine(const std::vector<FrameGenEngineCalibrationFrame>& frames) {
    FrameGenEngineCalibration calibration;
    calibration.frames = static_cast<std::uint32_t>(frames.size());
    struct Model {
        FrameGenEngineCamera camera;
        int sign;
    };
    const std::array<Model, 5> models{{{FrameGenEngineCamera::kIncluded, 1},
                                       {FrameGenEngineCamera::kIncluded, -1},
                                       {FrameGenEngineCamera::kRotationOnly, 1},
                                       {FrameGenEngineCamera::kExcluded, 1},
                                       {FrameGenEngineCamera::kExcluded, -1}}};
    std::array<std::array<FrameGenEngineAxisFit, 2>, models.size()> fits{};
    std::array<FrameGenEngineAxisFit, 2> cockpit{};
    std::array<FrameGenEngineAxisFit, 2> cockpit_lag{};
    // Each frame's cockpit samples, summed, for its intercept under the fit.
    std::vector<std::array<FrameGenEngineAxisFit, 2>> frame_sums;
    std::size_t total = 0;
    for (const auto& frame : frames) {
        total += frame.samples.size();
    }
    // Residuals are taken over at most about 100 000 samples.
    const std::size_t stride = std::max<std::size_t>(1, total / 100000);
    for (const auto& frame : frames) {
        std::array<FrameGenEngineAxisFit, 2> frame_cockpit{};
        for (const FrameGenEngineCalibrationSample& sample : frame.samples) {
            const std::array<double, 2> e{sample.vector[0], sample.vector[1]};
            if (!std::isfinite(e[0]) || !std::isfinite(e[1])) {
                continue;
            }
            for (std::size_t m = 0; m < models.size(); ++m) {
                std::array<double, 2> q{};
                if (FrameGenEnginePrediction(frame.newest, frame.previous, frame.delta_orientation, frame.delta_position,
                                             models[m].sign, models[m].camera, sample.uv, sample.depth, q)) {
                    fits[m][0].Add(e[0], q[0], 0.0);
                    fits[m][1].Add(e[1], q[1], 0.0);
                }
            }
            if (!(sample.depth > 0.0f) || sample.depth >= kFrameGenEngineCockpit) {
                continue;
            }
            // Cockpit content stays in the application's space: its
            // prediction from the head alone, split into the rotation's part
            // and what the translation adds at its depth.
            auto split = [&](const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous,
                             std::array<FrameGenEngineAxisFit, 2>& fit, std::array<FrameGenEngineAxisFit, 2>* each) {
                std::array<double, 2> rotation{};
                std::array<double, 2> full{};
                if (!FrameGenEnginePrediction(newest, previous, frame.delta_orientation, frame.delta_position, 1,
                                              FrameGenEngineCamera::kRotationOnly, sample.uv, sample.depth, rotation) ||
                    !FrameGenEnginePrediction(newest, previous, frame.delta_orientation, frame.delta_position, 1,
                                              FrameGenEngineCamera::kIncluded, sample.uv, sample.depth, full)) {
                    return;
                }
                for (int axis = 0; axis < 2; ++axis) {
                    fit[axis].Add(e[axis], rotation[axis], full[axis] - rotation[axis]);
                    if (each) (*each)[axis].Add(e[axis], rotation[axis], full[axis] - rotation[axis]);
                }
            };
            split(frame.newest, frame.previous, cockpit, &frame_cockpit);
            if (frame.before_previous) {
                split(frame.previous, *frame.before_previous, cockpit_lag, nullptr);
            }
        }
        frame_sums.push_back(frame_cockpit);
    }
    auto spread = [](const std::vector<double>& values) {
        if (values.size() < 2) return 0.0;
        double mean = 0.0;
        for (const double c : values) mean += c;
        mean /= static_cast<double>(values.size());
        double variance = 0.0;
        for (const double c : values) variance += (c - mean) * (c - mean);
        return std::sqrt(variance / static_cast<double>(values.size() - 1));
    };
    // Each frame's intercept under the cockpit fit (with the head still, the
    // vectors' mean): a fit of the frame alone would follow tracking noise.
    bool gain = true;
    std::array<double, 2> spread_px{};
    for (int axis = 0; axis < 2; ++axis) {
        calibration.cockpit[axis] = cockpit[axis].Solve();
        calibration.cockpit_lag[axis] = cockpit_lag[axis].Solve();
        const FrameGenEngineAxisFit::Solution& fit = calibration.cockpit[axis];
        gain = gain && GivesFrameGenEngineGain(fit);
        std::vector<double> raw;
        std::vector<double> pixels;
        for (std::size_t f = 0; f < frames.size(); ++f) {
            if (frame_sums[f][axis].Samples() <= 0.0) continue;
            raw.push_back(frame_sums[f][axis].Intercept(0.0, 0.0));
            if (GivesFrameGenEngineGain(fit)) {
                const std::uint32_t extent = axis == 0 ? frames[f].vectors.width : frames[f].vectors.height;
                pixels.push_back(frame_sums[f][axis].Intercept(fit.a, fit.b) / fit.a * 0.5 *
                                 std::max<std::uint32_t>(extent, 1));
            }
        }
        calibration.intercept_spread[axis] = spread(raw);
        spread_px[axis] = spread(pixels);
    }
    if (gain) {
        calibration.intercept_spread_px = spread_px;
    }
    const FrameGenExtent vectors = frames.empty() ? FrameGenExtent{} : frames.front().vectors;
    const FrameGenExtent color = frames.empty() ? FrameGenExtent{} : frames.front().color;
    for (std::size_t m = 0; m < models.size(); ++m) {
        FrameGenEngineCandidate candidate;
        candidate.camera = models[m].camera;
        candidate.delta_sign = models[m].sign;
        candidate.fit = {fits[m][0].SolveLine(), fits[m][1].SolveLine()};
        if (!candidate.fit[0].valid || !candidate.fit[1].valid || candidate.fit[0].a == 0.0 || candidate.fit[1].a == 0.0) {
            candidate.fit[0].corr2 = candidate.fit[1].corr2 = 0.0;
            calibration.leaderboard.push_back(candidate);
            continue;
        }
        candidate.gain = {1.0 / candidate.fit[0].a, 1.0 / candidate.fit[1].a};
        candidate.nearest = NearestFrameGenEngineGain(candidate.gain, vectors, color, candidate.gain_error);
        // The median residual of the samples taken, in vector pixels.
        std::vector<double> residuals;
        std::size_t index = 0;
        for (const auto& frame : frames) {
            const std::array<double, 2> pixels{0.5 * frame.vectors.width, 0.5 * frame.vectors.height};
            for (const FrameGenEngineCalibrationSample& sample : frame.samples) {
                if (index++ % stride != 0 || !std::isfinite(sample.vector[0]) || !std::isfinite(sample.vector[1])) {
                    continue;
                }
                std::array<double, 2> q{};
                if (!FrameGenEnginePrediction(frame.newest, frame.previous, frame.delta_orientation, frame.delta_position,
                                              candidate.delta_sign, candidate.camera, sample.uv, sample.depth, q)) {
                    continue;
                }
                const double dx = (candidate.gain[0] * (sample.vector[0] - candidate.fit[0].c) - q[0]) * pixels[0];
                const double dy = (candidate.gain[1] * (sample.vector[1] - candidate.fit[1].c) - q[1]) * pixels[1];
                residuals.push_back(std::sqrt(dx * dx + dy * dy));
            }
        }
        if (!residuals.empty()) {
            const auto middle = residuals.begin() + static_cast<std::ptrdiff_t>(residuals.size() / 2);
            std::nth_element(residuals.begin(), middle, residuals.end());
            candidate.residual_px = *middle;
        }
        calibration.leaderboard.push_back(candidate);
    }
    std::stable_sort(calibration.leaderboard.begin(), calibration.leaderboard.end(),
                     [](const FrameGenEngineCandidate& a, const FrameGenEngineCandidate& b) {
                         if (std::abs(a.Corr2() - b.Corr2()) > 1e-6) return a.Corr2() > b.Corr2();
                         return a.residual_px < b.residual_px;
                     });
    return calibration;
}

// --- Engine motion vectors: the frame gates --------------------------------

namespace {

constexpr double kDegreesPerRadian = 57.29577951308232;

// a * b, in double.
std::array<double, 4> Multiply(const std::array<double, 4>& a, const std::array<double, 4>& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

std::array<double, 4> Unit(const std::array<float, 4>& q) {
    const double length = std::sqrt(static_cast<double>(q[0]) * q[0] + static_cast<double>(q[1]) * q[1] +
                                    static_cast<double>(q[2]) * q[2] + static_cast<double>(q[3]) * q[3]);
    // A quaternion left unset (all zero) turns nothing.
    if (!(length > 0.0)) return {0.0, 0.0, 0.0, 1.0};
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

// The angle a unit quaternion turns by, in degrees.
double Degrees(const std::array<double, 4>& q) {
    return 2.0 * std::atan2(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), std::abs(q[3])) * kDegreesPerRadian;
}

} // namespace

const char* FrameGenEngineGateName(std::size_t index) {
    static constexpr std::array<const char*, kFrameGenEngineGateCount> kNames{
        "noStruct", "skip", "pairing", "cut", "decode", "recreate", "settle", "uncaptured"};
    return index < kNames.size() ? kNames[index] : "";
}

std::uint32_t FrameGenEngineGates(const FrameGenEngineGateRecord* record, const FrameGenEngineGateRecord* previous,
                                  const FrameGenEngineGateFacts& facts) {
    std::uint32_t gates = 0;
    // G8: swapchains made lately (a game resizing its views makes them anew,
    // Star Citizen churns them at its world entry) settle first.
    if (facts.frames_since_swapchain < 2) gates |= kFrameGenEngineRecreate;
    if (facts.seconds_since_swapchain < 3.0) gates |= kFrameGenEngineSettle;
    // G9: the engine path serves frames analyzed just in time, of two views
    // each captured at its release.
    if (!facts.captured || !facts.just_in_time || facts.views != 2 || facts.composed) {
        gates |= kFrameGenEngineUncaptured;
    }
    // G1, G2: a readable structure on every view, of one extension. Another
    // frame's record is none of this one's (a frame the layer handed none
    // over for).
    if (!record || record->display_time != facts.end_time || !record->complete || record->views.size() != facts.views ||
        std::any_of(record->views.begin(), record->views.end(),
                    [](const FrameGenEngineGateView& view) { return !view.readable; })) {
        return gates | kFrameGenEngineNoStruct;
    }
    // G4: the game says the frame repeats the one before (XR_FB_space_warp).
    if (std::any_of(record->views.begin(), record->views.end(),
                    [](const FrameGenEngineGateView& view) { return view.frame_skip; })) {
        gates |= kFrameGenEngineSkip;
    }
    // G6.
    if (record->cut) gates |= kFrameGenEngineCut;
    // G7: the frame's depth decoded as its structure describes it. The depth
    // captured at its release is decoded as the frame before described its
    // own, so the first frame after a change (MSFS: nearZ 1000 in its menus,
    // 356700 in flight) is not.
    for (const FrameGenEngineGateView& view : record->views) {
        const auto& decoded = view.view < facts.decoded.size() ? facts.decoded[view.view] : std::nullopt;
        if (decoded && !SameFrameGenDepthDescription(*decoded, view.depth)) gates |= kFrameGenEngineDecode;
    }
    // G3, G5: the record before is the one of the frame generated from right
    // before this one, which is the projection frame right before it (none
    // without a projection came between: those have records of their own),
    // with the same vectors in the same regions; and no pause came between.
    bool paired = previous && previous->sequence != 0 && previous->sequence == facts.previous_sequence &&
                  facts.continuous && !facts.paused && previous->complete && previous->projection_frame != 0 &&
                  previous->projection_frame + 1 == record->projection_frame &&
                  previous->views.size() == record->views.size();
    for (std::size_t i = 0; paired && i < record->views.size(); ++i) {
        const FrameGenEngineGateView& before = previous->views[i];
        const FrameGenEngineGateView& now = record->views[i];
        paired = before.view == now.view && before.vectors == now.vectors && before.rect == now.rect;
    }
    if (!paired) gates |= kFrameGenEnginePairing;
    return gates;
}

bool FrameGenEngineCuts::Add(const std::array<float, 4>& orientation, const std::array<float, 3>& position) {
    const double distance =
        std::sqrt(static_cast<double>(position[0]) * position[0] + static_cast<double>(position[1]) * position[1] +
                  static_cast<double>(position[2]) * position[2]);
    // Anything not finite is a cut too (Unit would take a rotation of NaN for
    // an unset one).
    const bool finite = std::isfinite(distance) &&
                        std::all_of(orientation.begin(), orientation.end(), [](float c) { return std::isfinite(c); });
    const bool cut = !finite || !(distance <= std::max(50.0, 8.0 * Median() + 1.0)) || !(Degrees(Unit(orientation)) <= 10.0);
    // A cut joins the median as well: one moves a median of 30 frames
    // nowhere, while a game that keeps moving faster (MSFS's sim rate, slew)
    // makes its new speed the median within 15 frames instead of cutting
    // every frame from then on.
    if (finite) {
        distances_.push_back(distance);
        while (distances_.size() > 30) distances_.pop_front();
    }
    return cut;
}

double FrameGenEngineCuts::Median() const {
    if (distances_.empty()) return 0.0;
    std::vector<double> sorted(distances_.begin(), distances_.end());
    const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
    std::nth_element(sorted.begin(), middle, sorted.end());
    return *middle;
}

FrameGenPoseChange FrameGenDeclaredChange(const FrameGenViewGeometry& game, const FrameGenViewGeometry& declared) {
    // declared = change * game: q_d = q_c q_g and p_d = R_c p_g + p_c.
    std::array<double, 4> game_inverse = Unit(game.orientation);
    game_inverse = {-game_inverse[0], -game_inverse[1], -game_inverse[2], game_inverse[3]};
    const std::array<double, 4> q = Multiply(Unit(declared.orientation), game_inverse);
    FrameGenPoseChange change;
    change.orientation = {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
                          static_cast<float>(q[3])};
    const Vector3 rotated = Rotate(change.orientation, Position(game));
    for (int i = 0; i < 3; ++i) {
        change.position[i] = static_cast<float>(declared.position[i] - rotated[i]);
    }
    return change;
}

bool FrameGenDeclaredChangeMoved(const FrameGenPoseChange& before, const FrameGenPoseChange& now) {
    std::array<double, 4> inverse = Unit(before.orientation);
    inverse = {-inverse[0], -inverse[1], -inverse[2], inverse[3]};
    const Vector3 moved{static_cast<double>(now.position[0]) - before.position[0],
                        static_cast<double>(now.position[1]) - before.position[1],
                        static_cast<double>(now.position[2]) - before.position[2]};
    return Degrees(Multiply(inverse, Unit(now.orientation))) > 0.05 ||
           std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]) > 0.0005;
}

// --- Research switches ------------------------------------------------------

namespace {

// What ReadResearchSwitch took from the research file.
struct ResearchFile {
    std::mutex mutex;
    std::set<std::string> taken; // "NAME=VALUE"
    std::vector<std::string> untaken;
    std::uint64_t reads{0};
};

ResearchFile& Research() {
    static ResearchFile research;
    return research;
}

std::string Trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace

std::optional<std::string> ReadResearchSwitch(const char* name) {
    if (!name) return std::nullopt;
    if (const char* value = std::getenv(name); value && *value) {
        return std::string(value);
    }
    const std::string_view wanted(name);
    if (wanted.rfind("VECTORXR_FRAMEGEN_ENGINE_", 0) != 0 && wanted != "VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK") {
        return std::nullopt;
    }
    const std::filesystem::path local = LocalAppDataPath();
    if (local.empty()) return std::nullopt;
    std::ifstream file(local / "VectorXR" / "framegen-research.txt");
    // The last line naming the switch holds; others (a commented one too)
    // are not its.
    std::optional<std::string> value;
    bool first = true;
    for (std::string line; std::getline(file, line); first = false) {
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3); // Notepad's byte order mark
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos && Trimmed(line.substr(0, equals)) == wanted) {
            value = Trimmed(line.substr(equals + 1));
        }
    }
    if (!value || value->empty()) return std::nullopt;
    ResearchFile& research = Research();
    std::scoped_lock lock(research.mutex);
    ++research.reads;
    std::string taken = std::string(wanted) + "=" + *value;
    if (research.taken.insert(taken).second) research.untaken.push_back(std::move(taken));
    return value;
}

std::vector<std::string> TakeResearchFileSwitches() {
    ResearchFile& research = Research();
    std::scoped_lock lock(research.mutex);
    std::vector<std::string> taken;
    taken.swap(research.untaken);
    return taken;
}

std::uint64_t ResearchFileReads() {
    ResearchFile& research = Research();
    std::scoped_lock lock(research.mutex);
    return research.reads;
}

std::filesystem::path FrameGenEngineRemovedPath() {
    const std::filesystem::path local = LocalAppDataPath();
    if (local.empty()) return {};
    return local / "VectorXR" / (L"engine-motion-" + GetCurrentExecutablePath().filename().wstring() + L".removed");
}

} // namespace depthxr
