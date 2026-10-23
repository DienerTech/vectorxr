#include "depthxr/framegen_plan.h"

#include "depthxr/config_path.h"
#include "depthxr/process_info.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
    // Only with the Flow view on: elsewhere the constants stay as they were.
    if (tuning.flow_view_fine && tuning.diagnostic == FrameGenDiagnosticView::kFlow) {
        flags |= kFrameGenFlagFlowFine;
    }
    // Research (the head-motion shimmer design): the real frames read by
    // Catmull-Rom, and the eye-shift map (with depth) between its texels.
    if (tuning.resample != FrameGenResample::kBilinear) {
        flags |= kFrameGenFlagResampleCubic;
        if (tuning.resample == FrameGenResample::kCubicWide) {
            flags |= kFrameGenFlagResampleWide;
        }
        if (tuning.resample_previous) {
            flags |= kFrameGenFlagResamplePrevious;
        }
    }
    if (tuning.shift_smooth && request.depth) {
        flags |= kFrameGenFlagShiftSmooth;
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

// Stage 7: the engine motion map for a generated frame `step` (as the motion
// map's), built into kMotionMap with the engine's own shift passes: each
// texel of the newest frame moved by its flow cell's label (input 4, the real
// frame's labels) and, for the world's, by the world's motion (input 3:
// `world`, the real frame's kWorldParams or its copy; none without). With
// `eye` (merged: AppendShiftMap's mapping, the eye's offset at the frame's
// time) the eye's offset is added to each texel's motion, and the map stands
// in for the eye-shift map too; synthesis then takes its entries as they
// are, so the map limits their flow by synthesis's motion limit (tuning.z).
void AppendEngineMotionMap(FrameGenViewPlan& plan, const FrameGenConstants& base, float step, FrameGenTexture depth,
                           FrameGenTexture flow, FrameGenTexture world, const std::array<float, 3>* eye) {
    FrameGenConstants mapping = base;
    if (eye) {
        mapping.fov_previous = mapping.fov_current;
        mapping.rot0 = {1.0f, 0.0f, 0.0f, -(*eye)[0]};
        mapping.rot1 = {0.0f, 1.0f, 0.0f, -(*eye)[1]};
        mapping.rot2 = {0.0f, 0.0f, 1.0f, -(*eye)[2]};
    }
    mapping.options = {(world.kind != FrameGenTextureKind::kNone ? kFrameGenEngineMapWorld : 0u) |
                           (eye ? kFrameGenEngineMapMerged : 0u),
                       kFrameGenFlagDepth | kFrameGenFlagDepthSplat | kFrameGenFlagEngine, 0, 0};
    mapping.tuning[2] = base.tuning[1];
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
    const FrameGenTexture labels = Texture(FrameGenTextureKind::kEngineLabels);
    pass(FrameGenPass::kShiftClear, {}, keys, target);
    pass(FrameGenPass::kShiftSplatEngine, {depth, flow, {}, world, labels, {}}, keys);
    pass(FrameGenPass::kShiftResolveEngine, {depth, flow, keys, world, labels, {}}, target);
    pass(FrameGenPass::kShiftFillEngine, {keys, flow, {}, {}, {}, {}}, target);
}

// Stage 8, research (FrameGenTuning::engine_bidir): the forward map for a
// frame interpolated `fraction` of the way from the previous real frame,
// built into kMotionMap with the engine motion map's passes (their keys
// reused, as the motion map's): the previous frame's content (its resolved
// depth and filtered flow, in the ring) moved that far along its own motion
// and seen in the newest view, nearest in front. That flow is the pair
// before's: steady motion moves it the intervals' ratio as far now
// (`interval_ratio`, as the filters scale it). Nothing fills its gaps: no
// content of the previous frame reaches them.
void AppendForwardMap(FrameGenViewPlan& plan, const FrameGenConstants& base, float fraction, float interval_ratio) {
    FrameGenConstants mapping = base;
    mapping.options = {kFrameGenEngineMapForward, kFrameGenFlagDepth | kFrameGenFlagDepthSplat | kFrameGenFlagEngine, 0, 0};
    mapping.tuning[1] = fraction * std::clamp(interval_ratio, 0.25f, 4.0f);
    auto pass = [&](FrameGenPass kind, std::array<FrameGenTexture, 6> inputs, FrameGenTexture output,
                    FrameGenTexture second = {}) {
        FrameGenDispatch dispatch = Dispatch(kind, plan.levels[1], plan.levels[1], mapping);
        dispatch.inputs = inputs;
        dispatch.outputs = {output, second};
        plan.dispatches.push_back(dispatch);
    };
    const FrameGenTexture depth = Texture(FrameGenTextureKind::kDepthPrevious);
    const FrameGenTexture flow = Texture(FrameGenTextureKind::kFilteredPrevious, plan.final_flow_level);
    const FrameGenTexture keys = Texture(FrameGenTextureKind::kShiftKeys);
    const FrameGenTexture target = Texture(FrameGenTextureKind::kMotionMap);
    pass(FrameGenPass::kShiftClear, {}, keys, target);
    pass(FrameGenPass::kShiftSplatEngine, {depth, flow, {}, {}, {}, {}}, keys);
    pass(FrameGenPass::kShiftResolveEngine, {depth, flow, keys, {}, {}, {}}, target);
}

// Stage 7 (FrameGenTuning::engine_map): whether a frame `extrapolate`d from
// a real frame whose flow cells the game's vectors labelled (its analysis
// merged them in, from Assist) takes the engine motion map.
bool EngineMapped(const FrameGenViewRequest& request, const FrameGenTuning& tuning, bool extrapolate) {
    return tuning.engine_map != FrameGenEngineMap::kOff && extrapolate && request.depth && request.engine_motion &&
           FrameGenEngineModeFor(tuning) >= FrameGenEngineMode::kAssist;
}

// How far past the newest frame a frame with the engine motion map may go,
// in real frame intervals (FrameGenTuning::engine_reach).
float EngineReach(const FrameGenTuning& tuning) {
    return std::clamp(tuning.engine_reach, 1.0f, kFrameGenEngineReachMost);
}

// The world's own motion (FrameGenViewRequest::world_motion), fitted to the
// flow at a grid of samples (the measured flow; in Lead the merged flow,
// kEngineFlow): each iteration weighs the samples by the last fit.
void AppendWorldFit(FrameGenViewPlan& plan, const FrameGenConstants& base, FrameGenTexture depth, FrameGenTexture flow) {
    for (std::uint32_t iteration = 0; iteration < kFrameGenWorldIterations; ++iteration) {
        FrameGenDispatch reduce = Dispatch(FrameGenPass::kWorldReduce, {kFrameGenWorldGrid, kFrameGenWorldGrid},
                                           plan.levels[plan.final_flow_level], base);
        reduce.inputs = {flow, {}, depth, iteration ? Texture(FrameGenTextureKind::kWorldParams) : FrameGenTexture{}, {}};
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

// The world's motion from the application's movement (stage 4,
// FrameGenEngineWorldParams) in place of the fit: kWorldParams texel 0 the
// rotation and 1 (valid), texel 1 the translation and 1, texel 2 the
// translation's scale, 1 (the application's movement), the rotation's sign
// and 0, texel 3 the rotation as a quaternion. Readers of the fit read texels
// 0 and 1 as they read the fit's; the filter moves world content exactly by
// texels 1 and 3 where texel 2 says the motion is the application's. Texels
// 4 and 5 (a fit run before it: the watchdog's) are left as they are.
void AppendWorldSet(FrameGenViewPlan& plan, const FrameGenEngineWorld& world, const FrameGenTuning& tuning) {
    FrameGenDispatch set = Dispatch(FrameGenPass::kWorldSet, {1, 1}, {1, 1}, {});
    set.outputs = {Texture(FrameGenTextureKind::kWorldParams), {}};
    set.constants.fov_current = {world.rotation[0], world.rotation[1], world.rotation[2], 1.0f};
    set.constants.fov_previous = {world.translation[0], world.translation[1], world.translation[2], 1.0f};
    set.constants.rot0 = {tuning.delta_scale, 1.0f, static_cast<float>(tuning.delta_sign), 0.0f};
    set.constants.rot1 = world.quaternion;
    plan.dispatches.push_back(set);
}

// Engine motion vectors (FrameGenViewRequest::engine_motion): the evidence
// on the frame's vectors (its raw field) against the declared poses, its
// luma, its resolved depth and the measured flow at the final level (none
// where the vectors lead: the evidence comes before it), summed per group
// into kEnginePartials by kind (cockpit, photometric, the field before when
// it is the frame before's, the cockpit against the pair before), then
// solved into kEngineParams and kEngineEvidence. Writes those alone: nothing
// measured or generated changes. The kinds share one pass, which reads each
// sample's luma and sums the group once, but for a jittered convention's
// photometric kind, which takes the frame's bias out (the solve's first
// stage finds it in the cockpit kind's sums), and the cockpit against the
// pair before, which needs that pair's constants.
void AppendEngineEvidence(FrameGenViewPlan& plan, const FrameGenConstants& base, const FrameGenViewRequest& request,
                          const FrameGenTuning& tuning) {
    // Observe and Assist measure the flow before it; where the vectors lead
    // (stage 6) it is measured after it, if at all.
    const std::uint32_t measured = FrameGenEngineLeads(request, tuning) ? 0u : 1u << 8;
    const FrameGenExtent l1 = plan.levels[1];
    const FrameGenTexture raw = Texture(FrameGenTextureKind::kEngineRaw);
    const FrameGenTexture luma = Texture(FrameGenTextureKind::kLumaNewest, 1);
    const FrameGenTexture before = Texture(FrameGenTextureKind::kLumaPrevious, 1);
    const FrameGenTexture depth = Texture(FrameGenTextureKind::kDepth);
    const FrameGenTexture flow = measured ? Texture(FrameGenTextureKind::kFlow, plan.final_flow_level) : FrameGenTexture{};
    const FrameGenTexture partials = Texture(FrameGenTextureKind::kEnginePartials);
    const FrameGenTexture params = Texture(FrameGenTextureKind::kEngineParams);
    // The lag kind's field before, when it is the frame before's.
    const std::uint32_t lag = request.engine_previous ? kFrameGenEngineKindLag : 0u;
    const FrameGenTexture field_before =
        request.engine_previous ? Texture(FrameGenTextureKind::kEngineRawPrevious) : FrameGenTexture{};
    // Every kind reads the vectors in the convention under test; the cockpit
    // kinds' sums come back to the vectors as decoded (the validator fits
    // the gains to those) in ReadFrameGenEngineEvidence.
    const std::array<float, 4> converted = FrameGenEngineDecode(tuning.engine, request.engine_encoding);
    const std::uint32_t camera = static_cast<std::uint32_t>(tuning.engine.camera) << 4;
    // The fifth input: the field before, or with bit 9 the solve's first
    // stage (the frame's bias in texel 0).
    auto reduce = [&](std::uint32_t kinds, const FrameGenConstants& constants, const FrameGenTexture& measured_flow,
                      const FrameGenTexture& fifth, std::uint32_t bias) {
        FrameGenDispatch dispatch =
            Dispatch(FrameGenPass::kEngineReduce, {kFrameGenEngineGrid, kFrameGenEngineGrid}, l1, constants);
        dispatch.inputs = {raw, luma, before, depth, measured_flow, fifth};
        dispatch.outputs = {partials, {}};
        dispatch.constants.options = {kinds | camera | measured | bias, constants.options[1], 0, plan.final_flow_level};
        dispatch.constants.tuning = converted;
        plan.dispatches.push_back(dispatch);
    };
    // The solve's options: its stage (0: the bias alone), whether the
    // convention is locked and jittered, which kinds ran, the camera; the
    // images the newest and previous fields came from (the stale test).
    auto solve = [&](std::uint32_t stage) {
        FrameGenDispatch dispatch =
            Dispatch(FrameGenPass::kEngineSolve, {kFrameGenThreadGroupSize, kFrameGenThreadGroupSize}, l1, base);
        dispatch.constants.size = {l1.width, l1.height, request.engine_vectors.width, request.engine_vectors.height};
        dispatch.inputs = {partials, Texture(FrameGenTextureKind::kEngineCensus),
                           Texture(FrameGenTextureKind::kEngineCensusPrevious), {}, {}, {}};
        dispatch.outputs = {params, stage ? Texture(FrameGenTextureKind::kEngineEvidence) : FrameGenTexture{}};
        const std::uint32_t flags = (tuning.engine_locked ? 1u << 4 : 0u) | (tuning.engine.jitter ? 1u << 5 : 0u) |
                                    (measured ? 0u : 1u << 6) | (request.engine_previous ? 1u << 7 : 0u) |
                                    (request.engine_lag ? 1u << 8 : 0u) |
                                    (static_cast<std::uint32_t>(tuning.engine.camera) << 12);
        dispatch.constants.options = {stage | flags, base.options[1],
                                      (request.engine_images[0] & 0xffu) | ((request.engine_images[1] & 0xffu) << 8), 0};
        plan.dispatches.push_back(dispatch);
    };
    if (tuning.engine.jitter) {
        reduce(kFrameGenEngineKindCockpit | lag, base, flow, field_before, 0u);
        solve(0);
        reduce(kFrameGenEngineKindPhotometric, base, flow, params, 1u << 9);
    } else {
        reduce(kFrameGenEngineKindCockpit | kFrameGenEngineKindPhotometric | lag, base, flow, field_before, 0u);
    }
    if (request.engine_lag) {
        // The cockpit fit against the pair before, by the game's poses (the
        // same relative pose as the declared ones while Pivot's change holds).
        FrameGenViewRequest pair = request;
        pair.newest = request.game_previous;
        pair.previous = request.game_before;
        reduce(kFrameGenEngineKindCockpitLag, BaseConstants(pair, tuning, tuning.mode), {}, {}, 0u);
    }
    solve(1);
}

// Stage 6: the frame's verdict for both eyes (kEngineSolve's stage 2, from
// this view's kEngineParams and the other view's), the predicate of the
// measured passes after it: kEngineVerdict, copied into the view's predicate
// by the backend (kFrameGenPredicateSet). Stage 8's seen test reads it
// without a predicate (`predicate` false).
void AppendEngineVerdict(FrameGenViewPlan& plan, const FrameGenConstants& base, bool predicate) {
    FrameGenDispatch verdict = Dispatch(FrameGenPass::kEngineSolve, {kFrameGenThreadGroupSize, kFrameGenThreadGroupSize},
                                        plan.levels[1], base);
    verdict.inputs = {FrameGenTexture{}, {}, {}, Texture(FrameGenTextureKind::kEngineParams), {},
                      Texture(FrameGenTextureKind::kEngineParamsOther)};
    verdict.outputs = {Texture(FrameGenTextureKind::kEngineVerdict), {}};
    verdict.constants.options = {2u, base.options[1], 0, 0};
    verdict.predicate = predicate ? kFrameGenPredicateSet : kFrameGenPredicateNone;
    plan.dispatches.push_back(verdict);
}

// How far the head turned between the request's two views, in degrees.
float HeadTurnDegrees(const FrameGenViewRequest& request) {
    const auto& a = request.newest.orientation;
    const auto& b = request.previous.orientation;
    const float dot = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2.0f * std::acos(std::min(dot, 1.0f)) * 57.2957795f;
}

// Engine motion vectors from Assist (FrameGenViewRequest::engine_motion),
// after their evidence (the frame's verdict is in kEngineParams): each flow
// cell's two candidates from the raw vectors under it, converted by the
// convention (kEngineConvert; `world`: the world's motion was set or fitted
// before them, for the candidates' classes; with the other view's verdict,
// which holds them back here too), then the flow the filters read
// with each cell's label (kEngineMerge: the measured flow wherever the
// verdict holds the vectors back), then each tile's trust in the vectors
// (kEngineTiles: from the frame before's where it has them, faster while the
// head turns more than 2 degrees a frame). Where the vectors lead (stage 6),
// the merge takes Lead's rule, and with no measured flow at all
// (FrameGenTuning::engine_predicate false) reads none. For the engine motion
// map (stage 7, with depth) and stage 8's seen test the labels keep the depth
// of the surface each vector taken was chosen for, from the resolved depth.
// Interpolating with the seen test (FrameGenEngineVisibility, with the
// previous frame's depth), the conversion also tells whether the previous
// frame saw each candidate's content where its vector puts it then, and
// reads both eyes' verdict from kEngineVerdict (written here unless Lead's
// predicate wrote it), so the previous frame's depth takes the other view's
// place; the merge takes such content's vector without matching it.
void AppendEngineAssist(FrameGenViewPlan& plan, const FrameGenConstants& base, const FrameGenViewRequest& request,
                        const FrameGenTuning& tuning, bool world) {
    const bool lead = FrameGenEngineLeads(request, tuning);
    const bool measured = !lead || tuning.engine_predicate;
    const bool seen = FrameGenEngineVisibility(tuning) && request.depth && request.depth_previous;
    const std::uint8_t level = plan.final_flow_level;
    const FrameGenExtent cells = plan.levels[level];
    const FrameGenTexture tiles_before =
        request.engine_tiles ? Texture(FrameGenTextureKind::kEngineTilesPrevious) : FrameGenTexture{};
    if (seen && !(lead && tuning.engine_predicate)) {
        AppendEngineVerdict(plan, base, false);
    }
    FrameGenDispatch convert = Dispatch(FrameGenPass::kEngineConvert, cells, plan.levels[1], base);
    convert.inputs = {Texture(FrameGenTextureKind::kEngineRaw),
                      Texture(FrameGenTextureKind::kDepth),
                      Texture(seen ? FrameGenTextureKind::kEngineVerdict : FrameGenTextureKind::kEngineParams),
                      tiles_before,
                      world ? Texture(FrameGenTextureKind::kWorldParams) : FrameGenTexture{},
                      Texture(seen ? FrameGenTextureKind::kDepthPrevious : FrameGenTextureKind::kEngineParamsOther)};
    convert.outputs = {Texture(FrameGenTextureKind::kEngineCandidate), Texture(FrameGenTextureKind::kEngineInfo)};
    convert.constants.options = {(static_cast<std::uint32_t>(tuning.engine.camera) << 4) |
                                     (request.engine_tiles ? kFrameGenEngineConvertTiles : 0u) |
                                     (world ? kFrameGenEngineConvertWorld : 0u) | (seen ? kFrameGenEngineConvertSeen : 0u),
                                 base.options[1], 0, level};
    convert.constants.tuning = FrameGenEngineDecode(tuning.engine, request.engine_encoding);
    plan.dispatches.push_back(convert);
    // The candidates cost what REFINE's would: its block at its match level.
    const std::uint8_t match = FrameGenMatchLevel(level);
    const bool depths = (tuning.engine_map != FrameGenEngineMap::kOff || seen) && request.depth;
    FrameGenDispatch merge = Dispatch(FrameGenPass::kEngineMerge, cells, plan.levels[match], base);
    merge.inputs = {Texture(FrameGenTextureKind::kLumaNewest, match),
                    Texture(FrameGenTextureKind::kLumaPrevious, match),
                    Texture(FrameGenTextureKind::kEngineCandidate),
                    Texture(FrameGenTextureKind::kEngineInfo),
                    measured ? Texture(FrameGenTextureKind::kFlow, level) : FrameGenTexture{},
                    depths ? Texture(FrameGenTextureKind::kDepth) : FrameGenTexture{}};
    merge.outputs = {Texture(FrameGenTextureKind::kEngineFlow), Texture(FrameGenTextureKind::kEngineLabels)};
    merge.constants.options = {(!lead      ? kFrameGenEngineMergeAssist
                                : measured ? kFrameGenEngineMergeLead
                                           : kFrameGenEngineMergeLead | kFrameGenEngineMergeUnmeasured) |
                                   (depths ? kFrameGenEngineMergeDepths : 0u),
                               base.options[1], level, 1u << (level - match)};
    plan.dispatches.push_back(merge);
    FrameGenDispatch tiles = Dispatch(FrameGenPass::kEngineTiles, FrameGenEngineTiles(cells), cells, base);
    tiles.inputs = {Texture(FrameGenTextureKind::kEngineLabels), tiles_before, {}, {}, {}, {}};
    tiles.outputs = {Texture(FrameGenTextureKind::kEngineTiles), {}};
    tiles.constants.options = {request.engine_tiles ? 1u : 0u, base.options[1], 0, level};
    tiles.constants.tuning[1] = HeadTurnDegrees(request);
    plan.dispatches.push_back(tiles);
}

// Synthesis with the confidence view, tinted by the cells' labels
// (FrameGenTuning::engine_tint): where a real frame's analysis wrote them,
// and synthesis reads no motion map in their place.
bool EngineTint(const FrameGenViewRequest& request, const FrameGenTuning& tuning, bool motion) {
    return tuning.engine_tint && tuning.diagnostic == FrameGenDiagnosticView::kConfidence && request.engine_motion &&
           FrameGenEngineModeFor(tuning) >= FrameGenEngineMode::kAssist && !motion;
}

// Stage 8: interpolated frames with depth read the real frame's labels for
// what the previous frame did not see (FrameGenEngineVisibility,
// FrameGenTuning::engine_visibility_synthesis), where its analysis labelled
// the cells (input 5, free when interpolating).
bool EngineVisible(const FrameGenViewRequest& request, const FrameGenTuning& tuning, bool extrapolate) {
    return tuning.engine_visibility_synthesis && !extrapolate && request.depth && request.engine_motion &&
           FrameGenEngineVisibility(tuning);
}

// Stage 8, research (FrameGenTuning::engine_bidir): interpolated frames with
// depth read content being covered through the forward map, where the
// previous real frame's depth and filtered flow are kept and its analysis
// merged the game's vectors in (input 5, in place of the labels; not with
// the confidence view's tint, which reads them). A frame whose vectors were
// held back or missing is mk1's.
bool EngineBidir(const FrameGenViewRequest& request, const FrameGenTuning& tuning, bool extrapolate) {
    return tuning.engine_bidir && !extrapolate && request.depth && request.depth_previous && request.temporal &&
           request.engine_motion && FrameGenEngineModeFor(tuning) >= FrameGenEngineMode::kAssist &&
           !EngineTint(request, tuning, false);
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
    return tuning.hardware_flow || tuning.hardware_level ? kFrameGenHardwareFlowLevel
                                                         : FrameGenFinalFlowLevel(tuning.quality);
}

FrameGenEngineMode FrameGenEngineModeFor(const FrameGenTuning& tuning) {
    return tuning.mode == FrameGenSynthesisMode::kInterpolate && !tuning.engine_interpolate
               ? std::min(tuning.engine_mode, FrameGenEngineMode::kAssist)
               : tuning.engine_mode;
}

bool FrameGenEngineVisibility(const FrameGenTuning& tuning) {
    return tuning.engine_visibility && tuning.mode == FrameGenSynthesisMode::kInterpolate &&
           FrameGenEngineModeFor(tuning) >= FrameGenEngineMode::kAssist;
}

bool FrameGenEngineLeads(const FrameGenViewRequest& request, const FrameGenTuning& tuning) {
    return request.engine_motion && FrameGenEngineModeFor(tuning) == FrameGenEngineMode::kLead && tuning.engine_locked;
}

namespace {
// FrameGenEngineLeadControl: the verdicts it judges by, the share of them
// held back that holds the vectors at Assist, for how many real frames, and
// the verdicts in a row that let them lead again.
constexpr std::size_t kLeadWindow = 30;
constexpr std::size_t kLeadHeldMost = 3; // 10% of the window
constexpr std::uint32_t kLeadHoldFrames = 300;
constexpr std::uint32_t kLeadStreak = 30;
} // namespace

void FrameGenEngineLeadControl::AddVerdicts(std::uint64_t frames, std::uint64_t held) {
    held = std::min(held, frames);
    for (std::uint64_t i = 0; i < frames; ++i) {
        const bool back = i >= frames - held;
        window_.push_back(back ? 1 : 0);
        while (window_.size() > kLeadWindow) window_.pop_front();
        streak_ = back ? 0 : streak_ + 1;
        const auto held_back = static_cast<std::size_t>(std::count(window_.begin(), window_.end(), std::uint8_t{1}));
        if (leads_ && window_.size() >= kLeadWindow && held_back > kLeadHeldMost) {
            leads_ = false;
            held_frames_ = kLeadHoldFrames;
            ++demotions_;
        }
    }
    Update();
}

void FrameGenEngineLeadControl::AddFrame() {
    held_frames_ -= held_frames_ ? 1 : 0;
    Update();
}

void FrameGenEngineLeadControl::Update() {
    if (!leads_ && held_frames_ == 0 && streak_ >= kLeadStreak) {
        leads_ = true;
        promotions_ += demotions_ ? 1 : 0;
    }
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
                       std::int64_t target_time, float reach) {
    const double interval = static_cast<double>(newest_time - previous_time);
    if (interval <= 0.0) {
        return mode == FrameGenSynthesisMode::kInterpolate ? 1.0f : 0.0f;
    }
    if (mode == FrameGenSynthesisMode::kInterpolate) {
        return static_cast<float>(std::clamp((target_time - previous_time) / interval, 0.0, 1.0));
    }
    return static_cast<float>(std::clamp((target_time - newest_time) / interval, 0.0,
                                         static_cast<double>(std::clamp(reach, 1.0f, kFrameGenEngineReachMost))));
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
        resolve.outputs = {Texture(FrameGenTextureKind::kDepth), Texture(FrameGenTextureKind::kDepthDecided)};
        plan.dispatches.push_back(resolve);
        // The texels it left open, by the surface around them as decided; a
        // thread per kFrameGenTrustSurfaceSpan texels each way.
        FrameGenDispatch surface = Dispatch(FrameGenPass::kTrustSurface, plan.levels[1], plan.levels[1], base);
        surface.width = (plan.levels[1].width + kFrameGenTrustSurfaceSpan - 1) / kFrameGenTrustSurfaceSpan;
        surface.height = (plan.levels[1].height + kFrameGenTrustSurfaceSpan - 1) / kFrameGenTrustSurfaceSpan;
        surface.inputs = {Texture(FrameGenTextureKind::kTrustNewest), {}, resolve.inputs[2],
                          Texture(FrameGenTextureKind::kDepthDecided), {}};
        surface.constants.options[0] = resolve.constants.options[0];
        surface.outputs = {Texture(FrameGenTextureKind::kDepth), {}};
        plan.dispatches.push_back(surface);
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

    // Stage 6: where the game's vectors lead, their evidence (the frame's
    // verdict) comes right after the luma, and the measured passes after it
    // are skipped where the verdict for both eyes lets the vectors be used
    // (FrameGenEngineLeads); the optical flow hardware is not used. With
    // tuning.engine_predicate false (research) no measured pass is planned:
    // only the downsample to the luma the merge matches on.
    const bool lead = FrameGenEngineLeads(request, tuning);
    const bool hardware = tuning.hardware_flow && !lead;
    const bool measure = !lead || tuning.engine_predicate;
    const std::uint8_t measured_predicate = lead ? kFrameGenPredicateSkip : kFrameGenPredicateNone;
    if (lead) {
        AppendEngineEvidence(plan, base, request, tuning);
        if (tuning.engine_predicate) {
            AppendEngineVerdict(plan, base, true);
        }
    }

    if (hardware) {
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

    // Where the vectors lead, kEngineMerge matches on the final level's
    // match level: the downsample to it is never skipped.
    const std::uint8_t merge_match = FrameGenMatchLevel(plan.final_flow_level);
    for (std::uint8_t level = 2; level <= kFrameGenPyramidLevels && !hardware; ++level) {
        const bool skippable = lead && level > merge_match;
        if (skippable && !measure) {
            break;
        }
        FrameGenDispatch down = Dispatch(FrameGenPass::kDownsample, plan.levels[level], plan.levels[level - 1], base);
        down.inputs = {Texture(FrameGenTextureKind::kLumaNewest, level - 1),
                       Texture(FrameGenTextureKind::kLumaPrevious, level - 1), {}, {}, {}};
        down.outputs = {Texture(FrameGenTextureKind::kLumaNewest, level), Texture(FrameGenTextureKind::kLumaPrevious, level)};
        down.constants.options[3] = level;
        down.predicate = skippable ? kFrameGenPredicateSkip : kFrameGenPredicateNone;
        plan.dispatches.push_back(down);
    }

    // Flow passes match on luma at `match` and write one vector per cell of
    // the flow level; options.w carries the cell size in match texels.
    if (!hardware && measure) {
        FrameGenDispatch search = Dispatch(FrameGenPass::kSearch, plan.levels[kFrameGenPyramidLevels],
                                           plan.levels[kFrameGenPyramidLevels], base);
        search.inputs = {Texture(FrameGenTextureKind::kLumaNewest, kFrameGenPyramidLevels),
                         Texture(FrameGenTextureKind::kLumaPrevious, kFrameGenPyramidLevels), {}, {}, {}};
        search.outputs = {Texture(FrameGenTextureKind::kFlow, kFrameGenPyramidLevels), {}};
        search.constants.options[3] = 1;
        search.predicate = measured_predicate;
        plan.dispatches.push_back(search);
    }

    for (std::uint8_t level = kFrameGenPyramidLevels - 1; level >= plan.final_flow_level && !hardware && measure;
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
        refine.predicate = measured_predicate;
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
    // Research (hypothesis E): content with nothing to track keeps the head's
    // predicted motion unless its match is clearly better; the cover filter
    // below is a copy and holds it too.
    const bool prior = tuning.flow_prior.enabled && (!tuning.flow_prior.still || FrameGenWorldStill(request));
    if (prior) {
        filter.pass = FrameGenPass::kFilterPrior;
        filter.constants.options[1] |= kFrameGenFlagFlowPrior | (tuning.flow_prior.distant ? kFrameGenFlagFlowPriorFar : 0u);
        filter.constants.tuning[1] = tuning.flow_prior.share;
        filter.constants.tuning[2] = tuning.flow_prior.margin;
        filter.constants.tuning[3] = tuning.flow_prior.per_pixel;
    }
    const bool world = request.depth && request.world_motion;
    // Stage 4: the world's motion from the application's movement where the
    // request has it and the backend's calibration locked, else fitted; the
    // sky turns with it (the filter's flag: the fitted rotation never moves
    // it, FG_PASS_FILTER tells them apart).
    const FrameGenEngineWorld delta =
        world && tuning.world_delta && request.app_delta
            ? FrameGenEngineWorldParams(request.game_newest, request.game_previous, request.newest, request.previous,
                                        request.delta_orientation, request.delta_position, tuning.delta_sign,
                                        tuning.delta_scale)
            : FrameGenEngineWorld{};
    // On the watchdog's frames the fit runs first, kept in texels 4 and 5
    // for the backend's readback, and the set follows it: the filters read
    // the same motion on every frame.
    auto append_world = [&] {
        if (!delta.valid || tuning.delta_watchdog) {
            AppendWorldFit(plan, base, depth, Texture(FrameGenTextureKind::kFlow, plan.final_flow_level));
        }
        if (delta.valid) {
            AppendWorldSet(plan, delta, tuning);
        }
    };
    const std::uint32_t world_flags = kFrameGenFlagWorld | (tuning.world_sky ? kFrameGenFlagWorldSky : 0u);
    const bool covered = tuning.analyze_only && tuning.cover && tuning.mode == FrameGenSynthesisMode::kInterpolate;
    // Engine motion vectors from Assist: their evidence (the frame's
    // verdict), then the flow the filters read in place of the measured flow,
    // the world's motion first for the cells' classes (the fit reads the
    // measured flow still: a frame whose verdict holds the vectors back is
    // filtered as without them). Where they lead (stage 6) the evidence came
    // first; the world's motion is the application's before the conversion,
    // or fitted to the merged flow after it (the measured flow, bit for bit,
    // where the verdict holds the vectors back): on the watchdog's frames the
    // fit after it and the application's motion set again.
    const bool assist = request.engine_motion && FrameGenEngineModeFor(tuning) >= FrameGenEngineMode::kAssist;
    if (assist) {
        const bool world_first = world && (tuning.mode == FrameGenSynthesisMode::kExtrapolate || covered);
        if (!lead) {
            AppendEngineEvidence(plan, base, request, tuning);
            if (world_first) {
                append_world();
            }
            AppendEngineAssist(plan, base, request, tuning, world_first);
        } else {
            const bool set_first = world_first && delta.valid;
            if (set_first) {
                AppendWorldSet(plan, delta, tuning);
            }
            AppendEngineAssist(plan, base, request, tuning, set_first);
            if (world_first && (!delta.valid || tuning.delta_watchdog)) {
                AppendWorldFit(plan, base, depth, Texture(FrameGenTextureKind::kEngineFlow));
                if (delta.valid) {
                    AppendWorldSet(plan, delta, tuning);
                }
            }
        }
        filter.inputs[0] = Texture(FrameGenTextureKind::kEngineFlow);
        if (tuning.engine_filter) {
            filter.inputs[4] = Texture(FrameGenTextureKind::kEngineLabels);
            filter.constants.options[0] = tuning.engine_cockpit ? 1u : 0u;
            filter.constants.options[1] |= kFrameGenFlagEngine;
        }
    }
    // The flow prior's prediction for the sky turns with the world (its
    // rotation alone) where the world's motion is known; interpolating, the
    // world's motion is worked out for the prior alone (the filter moves no
    // world content by it: kFrameGenFlagFlowPriorWorld, not kFrameGenFlagWorld).
    const bool prior_world = world && prior && tuning.mode == FrameGenSynthesisMode::kInterpolate;
    if (world && (tuning.mode == FrameGenSynthesisMode::kExtrapolate || prior_world)) {
        if (!assist || (prior_world && !covered)) {
            append_world();
        }
        filter.inputs[3] = Texture(FrameGenTextureKind::kWorldParams);
        filter.constants.options[1] |= prior_world ? kFrameGenFlagFlowPriorWorld : world_flags;
    }
    plan.dispatches.push_back(filter);
    // Observe: the game's motion vectors judged against what the frame shows
    // (its luma and measured flow exist by now); nothing generated reads it.
    if (request.engine_motion && FrameGenEngineModeFor(tuning) == FrameGenEngineMode::kObserve) {
        AppendEngineEvidence(plan, base, request, tuning);
    }
    if (tuning.analyze_only) {
        if (covered) {
            // The same flow filtered as extrapolation filters it, for cover
            // frames past the newest frame.
            FrameGenDispatch cover = filter;
            cover.outputs = {Texture(FrameGenTextureKind::kFilteredCover, plan.final_flow_level), {}};
            cover.constants.options[1] |= kFrameGenFlagExtrapolate;
            cover.constants.options[1] &= ~kFrameGenFlagFlowPriorWorld;
            if (world) {
                if (!assist && !prior_world) {
                    append_world();
                }
                cover.inputs[3] = Texture(FrameGenTextureKind::kWorldParams);
                cover.constants.options[1] |= world_flags;
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
    // Stage 7: extrapolated frames take the engine motion map where this
    // analysis labelled the cells, with the world's motion it set or fitted.
    const bool mapped = EngineMapped(request, tuning, tuning.mode == FrameGenSynthesisMode::kExtrapolate);
    const FrameGenTexture mapped_world =
        world && tuning.mode == FrameGenSynthesisMode::kExtrapolate ? Texture(FrameGenTextureKind::kWorldParams) : FrameGenTexture{};
    float map_time = 0.0f;
    for (std::size_t index = 0; index < generated; ++index) {
        const float fraction = std::clamp(tuning.fractions[index], 0.0f, mapped ? EngineReach(tuning) : 1.0f);
        const float time = tuning.mode == FrameGenSynthesisMode::kExtrapolate ? fraction : 1.0f - fraction;
        // The engine motion map for this frame's own time; merged, with the
        // eye's offset in it in place of the eye-shift map.
        const bool engine_map = mapped && time > 0.0f;
        const bool merged = engine_map && tuning.engine_map == FrameGenEngineMap::kMerged;
        const bool spare = tuning.spare && generated > 1 && index + 1 == generated;
        const bool shifted = request.depth && time > 0.0f && shift_length * time >= kFrameGenMinimumShift && !merged;
        const bool rebuild = map_time <= 0.0f || (!spare && shift_length * std::abs(time - map_time) >= kFrameGenShiftRebuild);
        if (shifted && rebuild) {
            map_time = time;
            AppendShiftMap(plan, base, {request.shift[0] * time, request.shift[1] * time, request.shift[2] * time}, depth);
            plan.frames_in_order = true;
        }
        // The motion map for this frame's own time (FrameGenTuning::depth_splat),
        // extrapolating only.
        const bool motion = request.depth && tuning.depth_splat && time > 0.0f &&
                            tuning.mode == FrameGenSynthesisMode::kExtrapolate && !engine_map;
        if (motion) {
            AppendMotionMap(plan, base, -fraction, depth,
                            Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level));
            plan.frames_in_order = true;
        }
        if (engine_map) {
            const std::array<float, 3> eye{request.shift[0] * time, request.shift[1] * time, request.shift[2] * time};
            AppendEngineMotionMap(plan, base, -fraction, depth,
                                  Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level), mapped_world,
                                  merged ? &eye : nullptr);
            plan.frames_in_order = true;
        }
        // Stage 8 (research): the forward map for this frame's time.
        const bool forward = time > 0.0f && EngineBidir(request, tuning, tuning.mode == FrameGenSynthesisMode::kExtrapolate);
        if (forward) {
            AppendForwardMap(plan, base, fraction, tuning.interval_ratio);
            plan.frames_in_order = true;
        }
        FrameGenDispatch output = Dispatch(synthesize, plan.levels[0], plan.levels[plan.final_flow_level], base);
        output.inputs = {Texture(FrameGenTextureKind::kHistoryNewest),
                         Texture(FrameGenTextureKind::kHistoryPrevious),
                         Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level),
                         depth,
                         shifted ? Texture(FrameGenTextureKind::kShiftMap) : FrameGenTexture{},
                         motion || engine_map ? Texture(FrameGenTextureKind::kMotionMap) : FrameGenTexture{}};
        if (shifted) {
            output.constants.options[1] |= kFrameGenFlagShift;
            output.constants.options[3] = FrameGenHalf(map_time);
        }
        if (motion || engine_map) {
            output.constants.options[1] |= kFrameGenFlagDepthSplat;
        }
        if (merged) {
            output.constants.options[1] |= kFrameGenFlagEngine;
        }
        if (EngineTint(request, tuning, motion || engine_map)) {
            output.inputs[5] = Texture(FrameGenTextureKind::kEngineLabels);
            output.constants.options[1] |= kFrameGenFlagEngine;
        }
        if (EngineVisible(request, tuning, tuning.mode == FrameGenSynthesisMode::kExtrapolate) && !forward) {
            output.inputs[5] = Texture(FrameGenTextureKind::kEngineLabels);
            output.constants.options[1] |= kFrameGenFlagEngineVisibility;
        }
        if (forward) {
            output.inputs[5] = Texture(FrameGenTextureKind::kMotionMap);
            output.constants.options[1] |= kFrameGenFlagEngineBidir;
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
                                            bool stale_flow, float map_time) {
    FrameGenViewPlan plan;
    plan.extent = request.extent;
    plan.levels = FrameGenPyramid(request.extent);
    plan.final_flow_level = FrameGenFinalFlowLevel(tuning);
    plan.synthesize = request.synthesize && request.extent.width >= 64 && request.extent.height >= 64;
    const bool extrapolate = mode == FrameGenSynthesisMode::kExtrapolate;
    // Stage 7: the engine motion map where the real frame's cells were
    // labelled (never moved by an older frame's motion), which takes the
    // head's and the world's motion past one interval (engine_reach).
    const bool mapped = !stale_flow && EngineMapped(request, tuning, extrapolate);
    fraction = std::clamp(fraction, 0.0f, mapped ? EngineReach(tuning) : 1.0f);
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
    // Merged, the engine motion map holds the eye's offset too: no eye-shift
    // map of its own.
    const bool merged = mapped && tuning.engine_map == FrameGenEngineMap::kMerged;
    const bool shifted =
        request.depth && std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]) >= kFrameGenMinimumShift && !merged;
    if (shifted) {
        AppendShiftMap(plan, base, eye, depth);
    }
    const FrameGenTexture flow = Texture(extrapolate && cover_flow ? FrameGenTextureKind::kFilteredCover
                                                                   : FrameGenTextureKind::kFilteredNewest,
                                         plan.final_flow_level);
    // The motion map for this frame's time (FrameGenTuning::depth_splat),
    // extrapolating only (cover frames included).
    const bool motion = request.depth && tuning.depth_splat && extrapolate && !mapped;
    if (motion) {
        AppendMotionMap(plan, base, -fraction, depth, flow);
    }
    // The engine motion map, with the real frame's world motion where its
    // analysis kept it; or the one built for this real frame at map_time,
    // read again.
    if (mapped && map_time <= 0.0f) {
        AppendEngineMotionMap(plan, base, -fraction, depth, flow,
                              request.engine_world ? Texture(FrameGenTextureKind::kWorldParamsNewest) : FrameGenTexture{},
                              merged ? &eye : nullptr);
    }
    // Stage 8 (research): the forward map for this frame's time.
    const bool forward = EngineBidir(request, tuning, extrapolate);
    if (forward) {
        AppendForwardMap(plan, base, fraction, tuning.interval_ratio);
    }
    FrameGenDispatch output = Dispatch(synthesize, plan.levels[0], plan.levels[plan.final_flow_level], base);
    output.inputs = {Texture(FrameGenTextureKind::kHistoryNewest),
                     Texture(FrameGenTextureKind::kHistoryPrevious),
                     flow,
                     depth,
                     shifted ? Texture(FrameGenTextureKind::kShiftMap) : FrameGenTexture{},
                     motion || mapped ? Texture(FrameGenTextureKind::kMotionMap) : FrameGenTexture{}};
    if (motion || mapped) {
        output.constants.options[1] |= kFrameGenFlagDepthSplat;
    }
    if (merged) {
        output.constants.options[1] |= kFrameGenFlagEngine;
    }
    if (EngineTint(request, tuning, motion || mapped)) {
        output.inputs[5] = Texture(FrameGenTextureKind::kEngineLabels);
        output.constants.options[1] |= kFrameGenFlagEngine;
    }
    if (EngineVisible(request, tuning, extrapolate) && !forward) {
        output.inputs[5] = Texture(FrameGenTextureKind::kEngineLabels);
        output.constants.options[1] |= kFrameGenFlagEngineVisibility;
    }
    if (forward) {
        output.inputs[5] = Texture(FrameGenTextureKind::kMotionMap);
        output.constants.options[1] |= kFrameGenFlagEngineBidir;
    }
    if (shifted) {
        // The map is built for this frame's own eye position: synthesis
        // scales it by one.
        output.constants.options[1] |= kFrameGenFlagShift;
        output.constants.options[3] = FrameGenHalf(time);
    }
    if (mapped && map_time > 0.0f) {
        // The engine motion map built for map_time: synthesis scales it by
        // time / map_time (options.w's high half; the eye-shift map's time
        // keeps the low half).
        output.constants.options[3] |= FrameGenHalf(map_time) << 16;
    }
    output.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
    output.copy_mask = 1u;
    plan.dispatches.push_back(output);
    return plan;
}

std::optional<float> FrameGenEngineMapTime(const FrameGenViewPlan& plan) {
    for (const FrameGenDispatch& dispatch : plan.dispatches) {
        if (dispatch.pass == FrameGenPass::kShiftClear && dispatch.outputs[1].kind == FrameGenTextureKind::kMotionMap &&
            (dispatch.constants.options[1] & kFrameGenFlagEngine) != 0 &&
            (dispatch.constants.options[0] & kFrameGenEngineMapForward) == 0) {
            return -dispatch.constants.tuning[1];
        }
    }
    return std::nullopt;
}

bool FrameGenEngineMapReusable(float time, float map_time) {
    return map_time > 0.0f && std::abs(time - map_time) <= kFrameGenEngineMapReuse && time <= 1.0f && map_time <= 1.0f;
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
    constexpr std::uint32_t kChecks = 6;
    // Either eye may be the empty one: the checked eye's (its depth was
    // judged until now, so the other eye takes over the check and the
    // validator starts over), or the other eye's (the check already reads
    // the right one).
    const bool source_clear = check.source_varied == 0.0 && check.other_varied >= kFrameGenDepthVariedCells;
    const bool other_clear = check.other_varied == 0.0 && check.source_varied >= kFrameGenDepthVariedCells;
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
    if (++checks_ < kFrameGenDepthBatchChecks) {
        return;
    }
    evidence_ = sums_;
    sums_ = {};
    checks_ = 0;
    ++batches_;
    if (locked_) {
        return;
    }
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
    // Depth that is the same all over the checked eye shows nothing of the
    // scene, however it is read. Star Citizen Evocati's is all zero in both
    // eyes (reversed, described correctly): as described all of it is
    // infinitely far, swapped all of it 2 cm away, and the few samples that
    // line up whatever moves them (dark space, plain panels) once made the
    // swapped reading look better than no depth: it was taken up, and every
    // pixel moved with the head. Only depth that varies (FrameGenDepthEyes'
    // measure, on average over the batch's checks) is judged.
    const bool varied = evidence_.source_varied >= kFrameGenDepthBatchChecks * kFrameGenDepthVariedCells;
    // Swapping needs the reading as decoded now to move content too, and to
    // line the eyes up worse there: swapped, depth that decodes all far (a
    // buffer left clear) is all near, and would win against nothing.
    const bool swap_beats = swapped_adopts && judged(decoded);
    if (!varied) {
        // Never taken up until it varies. Where it reads as infinitely far
        // it decides nothing, in use or not: MSFS 2024 clears its depth in
        // its menus (a session that starts there is still being checked,
        // and the first batch of the cockpit takes it up; depth in use stays
        // in use, nothing to take up again afterwards), and Star Citizen
        // Evocati's is all zero. Where it moves content it is not usable.
        if (judged(decoded)) {
            reading_ = FrameGenDepthReading::kUnusable;
        }
    } else if (!Usable()) {
        const bool swap = swap_beats && (!decoded_adopts || swapped.Ratio() < decoded.Ratio());
        if (swap) {
            swapped_ = !swapped_;
        }
        if (decoded_adopts || swap) {
            reading_ = swapped_ ? FrameGenDepthReading::kReversed : FrameGenDepthReading::kAsSubmitted;
        } else if (judged(decoded) && judged(swapped)) {
            reading_ = FrameGenDepthReading::kUnusable;
        }
    } else if (swap_beats && swapped.Ratio() < kAdopt * decoded.Ratio()) {
        swapped_ = !swapped_;
        reading_ = swapped_ ? FrameGenDepthReading::kReversed : FrameGenDepthReading::kAsSubmitted;
    } else if (judged(decoded) && decoded.Ratio() > kDrop) {
        reading_ = FrameGenDepthReading::kUnusable;
    }

    // The scale that lines the eyes up best: the probes' minimum, refined by
    // a parabola through it and its neighbours; at the edge of the probes,
    // a step that way with wider probes. A swap starts the search over. It
    // is searched only on depth that varies, where the reading moves content
    // (before it is taken up too: a near plane described far enough off
    // lines the eyes up only once scaled): the probes on Star Citizen's
    // clear depth swapped were all alike, and their noise stepped the scale
    // up to 1.88.
    const float scale_before = scale_;
    const float step_before = probe_step_;
    if (swapped_ != swapped_before) {
        scale_ = 1.0f;
        probe_step_ = 0.08f;
    } else if (varied && judged(decoded) && evidence_.probed >= std::max(200.0, 0.01 * evidence_.binocular)) {
        const auto& e = evidence_.probes;
        const double used = probe_step_; // the step these probes were taken with
        const std::size_t best = static_cast<std::size_t>(std::min_element(e.begin(), e.end()) - e.begin());
        const double centre = static_cast<double>(kFrameGenDepthScaleProbes / 2);
        double offset = static_cast<double>(best) - centre;
        const bool edge = best == 0 || best + 1 == kFrameGenDepthScaleProbes;
        // At the edge the probes must fall toward it all the way, and from
        // clearly higher: the probe at the other edge 1% above the centre,
        // as the edge's must be 1% below it. One probe a little below the
        // rest, which are alike and fall in order only by noise, is noise.
        const std::size_t other = best == 0 ? kFrameGenDepthScaleProbes - 1 : 0;
        bool trend = edge && e[other] > 1.01 * e[kFrameGenDepthScaleProbes / 2];
        for (std::size_t j = 0; trend && j + 1 < kFrameGenDepthScaleProbes; ++j) {
            trend = best == 0 ? e[j] < e[j + 1] : e[j + 1] < e[j];
        }
        if (trend) {
            probe_step_ = std::min(probe_step_ * 1.5f, 0.25f);
        } else if (!edge) {
            const double curvature = e[best - 1] - 2.0 * e[best] + e[best + 1];
            if (curvature > 0.0) {
                offset += std::clamp(0.5 * (e[best - 1] - e[best + 1]) / curvature, -0.5, 0.5);
            }
            probe_step_ = std::max(probe_step_ * 0.6f, 0.02f);
        }
        // Moves of under 1% (or that gain under 1% over the current scale)
        // are noise.
        const double step = std::exp(used * offset);
        if ((trend || !edge) && std::abs(offset) * used > 0.01 && e[best] < 0.99 * e[kFrameGenDepthScaleProbes / 2]) {
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

void FrameGenDepthValidator::LockForTest(FrameGenDepthReading reading, bool swapped, float scale) {
    reading_ = reading;
    swapped_ = swapped;
    scale_ = scale;
    settled_ = true;
    locked_ = true;
    ++generation_;
    ++check_generation_;
    ++decisions_;
}

void FrameGenDepthValidator::Restart() {
    if (locked_) {
        return;
    }
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
           : text == "observe" ? FrameGenEngineMode::kObserve
           : text == "assist"  ? FrameGenEngineMode::kAssist
           : text == "lead"    ? FrameGenEngineMode::kLead
                               : FrameGenEngineMode::kOff;
}

const char* ToString(FrameGenEngineMap map) {
    switch (map) {
    case FrameGenEngineMap::kSeparate:
        return "separate";
    case FrameGenEngineMap::kMerged:
        return "merged";
    default:
        return "off";
    }
}

FrameGenEngineMap ParseFrameGenEngineMap(const std::string& value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "separate" ? FrameGenEngineMap::kSeparate
           : text == "merged" ? FrameGenEngineMap::kMerged
                              : FrameGenEngineMap::kOff;
}

const char* ToString(FrameGenResample resample) {
    switch (resample) {
    case FrameGenResample::kCubic:
        return "cubic";
    case FrameGenResample::kCubicWide:
        return "cubic9";
    default:
        return "bilinear";
    }
}

FrameGenResampleChoice ParseFrameGenResample(const std::string& value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    FrameGenResampleChoice choice;
    if (text == "cubic" || text == "cubic-newest") {
        choice.resample = FrameGenResample::kCubic;
    } else if (text == "cubic9" || text == "cubic9-newest") {
        choice.resample = FrameGenResample::kCubicWide;
    }
    choice.previous = choice.resample != FrameGenResample::kBilinear && text.find("-newest") == std::string::npos;
    return choice;
}

const char* ToString(FrameGenEyePredictor predictor) {
    switch (predictor) {
    case FrameGenEyePredictor::kDamped:
        return "damped";
    case FrameGenEyePredictor::kTwoInterval:
        return "two";
    case FrameGenEyePredictor::kQuadratic:
        return "quadratic";
    case FrameGenEyePredictor::kTwist:
        return "twist";
    default:
        return "velocity";
    }
}

FrameGenEyePrediction ParseFrameGenEyePrediction(const std::string& value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    FrameGenEyePrediction prediction;
    if (text == "damped" || text.rfind("damped:", 0) == 0) {
        prediction.predictor = FrameGenEyePredictor::kDamped;
        if (text.size() > 7) {
            const double share = std::atof(text.c_str() + 7);
            if (share >= 0.5 && share <= 1.0) prediction.damping = share;
        }
    } else if (text == "two") {
        prediction.predictor = FrameGenEyePredictor::kTwoInterval;
    } else if (text == "quadratic") {
        prediction.predictor = FrameGenEyePredictor::kQuadratic;
    } else if (text == "twist") {
        prediction.predictor = FrameGenEyePredictor::kTwist;
    }
    return prediction;
}

std::optional<std::array<double, 3>> FrameGenPredictEye(const FrameGenEyePrediction& prediction,
                                                        const FrameGenEyeSample* samples, std::size_t count,
                                                        double time) {
    if (!samples || count < 2) return std::nullopt;
    for (std::size_t i = 1; i < count; ++i) {
        if (!(samples[i].time > samples[i - 1].time)) return std::nullopt;
    }
    const FrameGenEyeSample& newest = samples[count - 1];
    const FrameGenEyeSample& before = samples[count - 2];
    const double last = newest.time - before.time;
    const double ahead = time - newest.time;
    std::array<double, 3> moved{};
    for (int i = 0; i < 3; ++i) moved[i] = newest.position[i] - before.position[i];
    std::array<double, 3> eye = newest.position;
    FrameGenEyePredictor predictor = prediction.predictor;
    if (count < 3 && (predictor == FrameGenEyePredictor::kTwoInterval || predictor == FrameGenEyePredictor::kQuadratic)) {
        predictor = FrameGenEyePredictor::kVelocity;
    }
    switch (predictor) {
    case FrameGenEyePredictor::kDamped:
        for (int i = 0; i < 3; ++i) eye[i] += prediction.damping * moved[i] * ahead / last;
        break;
    case FrameGenEyePredictor::kTwoInterval: {
        const FrameGenEyeSample& oldest = samples[count - 3];
        const double span = newest.time - oldest.time;
        for (int i = 0; i < 3; ++i) eye[i] += (newest.position[i] - oldest.position[i]) * ahead / span;
        break;
    }
    case FrameGenEyePredictor::kQuadratic: {
        // Lagrange's parabola through the three, at `time`.
        const FrameGenEyeSample* p[3] = {&samples[count - 3], &before, &newest};
        for (int i = 0; i < 3; ++i) eye[i] = 0.0;
        for (int a = 0; a < 3; ++a) {
            double weight = 1.0;
            for (int b = 0; b < 3; ++b) {
                if (b != a) weight *= (time - p[b]->time) / (p[a]->time - p[b]->time);
            }
            for (int i = 0; i < 3; ++i) eye[i] += weight * p[a]->position[i];
        }
        break;
    }
    case FrameGenEyePredictor::kTwist: {
        // The last movement into the older eye's view, out of the newer's:
        // turned on by the turn between them.
        const auto rotate = [](const std::array<float, 4>& q, const std::array<double, 3>& v, double sign) {
            const double x = sign * q[0], y = sign * q[1], z = sign * q[2], w = q[3];
            // v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
            const double cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0];
            return std::array<double, 3>{v[0] + 2.0 * (w * cx + y * cz - z * cy), v[1] + 2.0 * (w * cy + z * cx - x * cz),
                                         v[2] + 2.0 * (w * cz + x * cy - y * cx)};
        };
        const std::array<double, 3> turned = rotate(newest.orientation, rotate(before.orientation, moved, -1.0), 1.0);
        for (int i = 0; i < 3; ++i) eye[i] += turned[i] * ahead / last;
        break;
    }
    default:
        for (int i = 0; i < 3; ++i) eye[i] += moved[i] * ahead / last;
        break;
    }
    return eye;
}

FrameGenFlowPrior ParseFrameGenFlowPrior(const std::string& value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    FrameGenFlowPrior prior;
    const std::size_t colon = text.find(':');
    std::string scope = text.substr(0, colon);
    const std::string still = "-still";
    if (scope.size() > still.size() && scope.compare(scope.size() - still.size(), still.size(), still) == 0) {
        prior.still = true;
        scope.erase(scope.size() - still.size());
    }
    if (scope != "sky" && scope != "1" && scope != "far") {
        return FrameGenFlowPrior{};
    }
    prior.enabled = true;
    prior.distant = scope == "far";
    if (colon == std::string::npos) {
        return prior;
    }
    float* const fields[3] = {&prior.share, &prior.margin, &prior.per_pixel};
    std::size_t start = colon + 1;
    for (float* field : fields) {
        const std::size_t comma = text.find(',', start);
        const std::string number = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        char* end = nullptr;
        const float parsed = std::strtof(number.c_str(), &end);
        if (!number.empty() && end && *end == '\0' && parsed >= 0.0f && parsed <= 10.0f) {
            *field = parsed;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return prior;
}

bool FrameGenWorldStill(const FrameGenViewRequest& request) {
    const auto& q = request.delta_orientation;
    const auto& p = request.delta_position;
    return request.app_delta && std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) < 0.005f &&
           std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) < 2.5e-4f;
}

const char* ToString(FrameGenEngineBridgeMode mode) {
    switch (mode) {
    case FrameGenEngineBridgeMode::kOff:
        return "off";
    case FrameGenEngineBridgeMode::kEndFrame:
        return "end";
    default:
        return "released";
    }
}

FrameGenEngineBridgeMode ParseFrameGenEngineBridgeMode(const std::string& value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "0" || text == "off" ? FrameGenEngineBridgeMode::kOff
           : text == "end"              ? FrameGenEngineBridgeMode::kEndFrame
                                        : FrameGenEngineBridgeMode::kReleased;
}

bool SameFrameGenEngineConvention(const FrameGenEngineConvention& a, const FrameGenEngineConvention& b) {
    auto same = [](float x, float y) { return std::abs(x - y) <= 1e-3f * std::max(std::abs(x), std::abs(y)); };
    return same(a.gain[0], b.gain[0]) && same(a.gain[1], b.gain[1]) && a.camera == b.camera;
}

std::optional<FrameGenEngineConvention> ParseFrameGenEngineConvention(const std::string& text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t comma = text.find(','); ; comma = text.find(',', start)) {
        std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        parts.push_back(part);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() < 3 || parts.size() > 4) return std::nullopt;
    FrameGenEngineConvention convention;
    for (int axis = 0; axis < 2; ++axis) {
        char* end = nullptr;
        const float gain = std::strtof(parts[axis].c_str(), &end);
        if (end == parts[axis].c_str() || *end != '\0' || !std::isfinite(gain) || gain == 0.0f) return std::nullopt;
        convention.gain[axis] = gain;
    }
    if (parts[2] == "included") {
        convention.camera = FrameGenEngineCamera::kIncluded;
    } else if (parts[2] == "rotation") {
        convention.camera = FrameGenEngineCamera::kRotationOnly;
    } else if (parts[2] == "excluded") {
        convention.camera = FrameGenEngineCamera::kExcluded;
    } else if (parts[2] == "objects") {
        convention.camera = FrameGenEngineCamera::kObjectOnly;
    } else {
        return std::nullopt;
    }
    if (parts.size() == 4) {
        if (parts[3] != "jitter" && parts[3] != "1") return std::nullopt;
        convention.jitter = true;
    }
    return convention;
}

std::array<float, 4> FrameGenEngineDecode(const FrameGenEngineConvention& convention,
                                          const FrameGenMotionEncoding& encoding) {
    // n = g * (e - the convention's offset) in NDC (y up), e = raw * scale +
    // the extension's offset; in uv (y down) half of n, y negated.
    const float scale_x = encoding.ext ? encoding.scale[0] : 1.0f;
    const float scale_y = encoding.ext ? encoding.scale[1] : 1.0f;
    const float offset_x = (encoding.ext ? encoding.offset[0] : 0.0f) - convention.offset[0];
    const float offset_y = (encoding.ext ? encoding.offset[1] : 0.0f) - convention.offset[1];
    return {0.5f * convention.gain[0] * scale_x, -0.5f * convention.gain[1] * scale_y, 0.5f * convention.gain[0] * offset_x,
            -0.5f * convention.gain[1] * offset_y};
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

FrameGenEngineAxisFit FrameGenEngineAxisFit::FromSums(const float* sums) {
    FrameGenEngineAxisFit fit;
    fit.n_ = sums[0];
    fit.r_ = sums[1];
    fit.t_ = sums[2];
    fit.e_ = sums[3];
    fit.rr_ = sums[4];
    fit.tt_ = sums[5];
    fit.rt_ = sums[6];
    fit.er_ = sums[7];
    fit.et_ = sums[8];
    fit.ee_ = sums[9];
    return fit;
}

FrameGenEngineAxisFit FrameGenEngineAxisFit::Unscaled(double gain, double offset) const {
    FrameGenEngineAxisFit fit = *this;
    const double k = 1.0 / gain;
    fit.e_ = k * e_ + offset * n_;
    fit.er_ = k * er_ + offset * r_;
    fit.et_ = k * et_ + offset * t_;
    fit.ee_ = k * k * ee_ + 2.0 * k * offset * e_ + offset * offset * n_;
    return fit;
}

FrameGenEngineAxisFit::Solution FrameGenEngineAxisFit::SolveRotation() const {
    Solution solution;
    solution.samples = n_;
    const double srr = RotationSpread();
    const double ser = er_ - (n_ > 0.0 ? e_ * r_ / n_ : 0.0);
    const double see = Spread();
    if (n_ < 3.0 || !(srr > 1e-12 * n_)) {
        return solution;
    }
    solution.a = ser / srr;
    solution.c = (e_ - solution.a * r_) / n_;
    solution.corr2 = see > 0.0 ? std::clamp(ser * ser / (srr * see), 0.0, 1.0) : 0.0;
    solution.valid = true;
    return solution;
}

FrameGenEngineAxisFit::Solution FrameGenEngineAxisFit::SolveTranslation() const {
    Solution solution;
    solution.samples = n_;
    const double stt = TranslationSpread();
    const double set = et_ - (n_ > 0.0 ? e_ * t_ / n_ : 0.0);
    const double see = Spread();
    if (n_ < 3.0 || !(stt > 1e-12 * n_)) {
        return solution;
    }
    solution.b = set / stt;
    solution.c = (e_ - solution.b * t_) / n_;
    solution.corr2 = see > 0.0 ? std::clamp(set * set / (stt * see), 0.0, 1.0) : 0.0;
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

// --- Engine motion vectors: evidence and validation ------------------------

namespace {

// A batch: each axis's head rotation spread by this many samples of three
// half-resolution texels (in NDC), over this many frames or more. One that
// has not in kBatchMaxFrames starts over: the head held still, or nothing
// was still to fit, and minutes-old sums would decide it.
constexpr double kBatchSpreadSamples = 300.0;
constexpr std::size_t kBatchFrames = 3;
constexpr std::size_t kBatchMaxFrames = 300;
// Classification: a gain within this share of a canonical one, the fit
// explaining this share of the vectors, b / a with the head included or
// rotation only, and still content's vectors this small against the
// head's movement (head excluded).
constexpr double kSnapError = 0.10;
constexpr double kFitCorr2 = 0.7;
constexpr double kIncludedLow = 0.75;
constexpr double kIncludedHigh = 1.35;
constexpr double kRotationOnly = 0.25;
constexpr double kExcludedShare = 0.04;
// A photometric batch's textured samples, and the margins it needs.
constexpr double kPhotometricSamples = 2000.0;
constexpr double kVariantMargin = 1.15;
constexpr double kMovingShare = 0.85;
constexpr double kLagMargin = 1.1;
constexpr double kMeasuredMargin = 1.1;
constexpr double kMeasuredSlack = 0.005;
constexpr double kWinShare = 0.7;
constexpr double kWorseThanMeasured = 1.3;
constexpr double kPhotometricMinimum = 200.0; // samples a term needs to count
// Adoption and its checks.
constexpr std::uint32_t kAdoptBatches = 3;
constexpr std::uint32_t kAdoptPhotometric = 2;
constexpr std::uint32_t kLagBatches = 3;
constexpr double kJitterPx = 0.15;
constexpr double kJitterMeanPx = 1.0;
constexpr std::size_t kJitterFrames = 30;
constexpr std::size_t kJitterMinimumFrames = 6;
constexpr double kOffsetPx = 0.1;
constexpr std::size_t kVerdictWindow = 90;
constexpr double kInvalidShare = 0.2;
constexpr double kSuspendSeconds = 10.0;
constexpr double kSuspensionWindow = 300.0;
constexpr std::size_t kSuspensionsToDisable = 3;
// Unlocked this long over batches disables, each gap between two batches
// counting up to kBatchGapSeconds.
constexpr double kUnknownSeconds = 300.0;
constexpr double kBatchGapSeconds = 2.0;

void AddPhotometric(FrameGenEngineEvidence& sums, const FrameGenEngineEvidence& view) {
    for (std::size_t c = 0; c < sums.variants.size(); ++c) sums.variants[c] += view.variants[c];
    sums.zero += view.zero;
    sums.measured_cost += view.measured_cost;
    sums.textured += view.textured;
    sums.with_measured += view.with_measured;
    sums.moving += view.moving;
    sums.variant_moving += view.variant_moving;
    sums.zero_moving += view.zero_moving;
    sums.wins += view.wins;
    sums.lag_cost += view.lag_cost;
    sums.lag_variant += view.lag_variant;
    sums.lag_samples += view.lag_samples;
}

// A view's verdict as Lead judges it (stage 6): its photometric reason from
// the vectors' luma error against no motion's (15% better where more than
// 100 samples move, else no worse, both with 0.005 of slack), as kEngineSolve
// judges a Lead plan's evidence, in its float arithmetic; the other reasons
// as the GPU gave them. Evidence taken for Assist was judged against the
// measured flow instead, which Lead does not measure.
std::uint32_t LeadVerdict(const FrameGenEngineEvidence& view) {
    std::uint32_t verdict = view.verdict & ~kFrameGenEngineVerdictPhotometric;
    const auto textured = static_cast<float>(view.textured);
    if (textured >= 200.0f) {
        const float variant = static_cast<float>(view.variants[0]) / textured;
        const float zero = static_cast<float>(view.zero) / textured;
        const float share = static_cast<float>(view.moving) > 100.0f ? 0.85f : 1.0f;
        verdict |= variant > share * zero + 0.005f ? kFrameGenEngineVerdictPhotometric : 0u;
    }
    return verdict;
}

std::string Fixed(double value, int decimals) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

} // namespace

const char* FrameGenEngineVerdictName(std::size_t index) {
    static constexpr std::array<const char*, kFrameGenEngineVerdictReasons> kNames{
        "unlocked", "stale", "cockpit", "photometric", "lag", "jitter"};
    return index < kNames.size() ? kNames[index] : "";
}

FrameGenEngineEvidence ReadFrameGenEngineEvidence(const float* evidence, const float* params,
                                                  const FrameGenEngineConvention& convention) {
    FrameGenEngineEvidence view;
    // The cockpit kinds' e as the convention reads the vectors, g (e - its
    // offset), back to e (gains are never 0: ParseFrameGenEngineConvention,
    // the canonical ones).
    const std::array<double, 2> gain{convention.gain[0], convention.gain[1]};
    const std::array<double, 2> offset{convention.offset[0], convention.offset[1]};
    auto unscaled = [&](const float* sums, int axis) {
        return FrameGenEngineAxisFit::FromSums(sums).Unscaled(gain[axis], offset[axis]);
    };
    const float* cockpit = evidence + 4 * kFrameGenEngineCockpitTexel;
    view.cockpit = {unscaled(cockpit, 0), unscaled(cockpit + 10, 1)};
    // e_x p_r,y and e_y p_r,x, with the other axis's sum of p_r.
    view.swapped = {cockpit[20] / gain[0] + offset[0] * cockpit[11], cockpit[21] / gain[1] + offset[1] * cockpit[1]};
    view.measured = {unscaled(cockpit + 22, 0), unscaled(cockpit + 32, 1)};
    view.head_gap = cockpit[42];
    const float* photometric = evidence + 4 * kFrameGenEnginePhotometricTexel;
    for (std::size_t c = 0; c < view.variants.size(); ++c) view.variants[c] = photometric[c];
    view.zero = photometric[8];
    view.measured_cost = photometric[9];
    view.textured = photometric[10];
    view.with_measured = photometric[11];
    view.moving = photometric[12];
    view.wins = photometric[13];
    view.variant_moving = photometric[14];
    view.zero_moving = photometric[15];
    view.inliers = photometric[16];
    view.cockpit_samples = photometric[17];
    for (std::size_t band = 0; band < 3; ++band) {
        view.epe[band] = photometric[18 + 2 * band];
        view.epe_samples[band] = photometric[19 + 2 * band];
    }
    view.head_px = photometric[24];
    view.vectors_px = photometric[25];
    view.valid = photometric[26];
    view.flow_px = photometric[27];
    const float* lag = evidence + 4 * kFrameGenEngineLagTexel;
    view.lag_cost = lag[0];
    view.lag_variant = lag[1];
    view.lag_samples = lag[2];
    view.lag_cost_moving = lag[3];
    view.lag_variant_moving = lag[4];
    view.lag_moving = lag[5];
    // The solve leaves the sums of kinds that did not run at zero.
    view.lag_photometric = view.lag_samples > 0.0;
    const float* cockpit_lag = evidence + 4 * kFrameGenEngineCockpitLagTexel;
    view.cockpit_lag = {unscaled(cockpit_lag, 0), unscaled(cockpit_lag + 10, 1)};
    view.lag = view.cockpit_lag[0].Samples() > 0.0;
    view.bias = {params[0], params[1]};
    view.verdict = static_cast<std::uint32_t>(std::max(params[2], 0.0f) + 0.5f);
    view.stale = params[4 * 2 + 3] > 0.5f;
    return view;
}

const char* ToString(FrameGenEngineValidation::State state) {
    switch (state) {
    case FrameGenEngineValidation::State::kCalibrating:
        return "calibrating";
    case FrameGenEngineValidation::State::kLocked:
        return "locked";
    case FrameGenEngineValidation::State::kSuspended:
        return "suspended";
    case FrameGenEngineValidation::State::kDisabled:
        return "disabled";
    default:
        return "probing";
    }
}

void FrameGenEngineValidator::SetPrior(const FrameGenEngineConvention& convention) {
    prior_ = convention;
    if (state_ == State::kProbing && !fixed_) {
        StartCandidate(convention, "", true);
        state_ = State::kCalibrating;
    }
}

void FrameGenEngineValidator::Fix(const FrameGenEngineConvention& convention) {
    fixed_ = convention;
    StartCandidate(convention, "", true);
    state_ = State::kCalibrating;
}

void FrameGenEngineValidator::LockForTest(const FrameGenEngineConvention& convention) {
    fixed_ = convention;
    StartCandidate(convention, "", true);
    held_for_test_ = true;
    Decide(State::kLocked, "locked for a test");
}

void FrameGenEngineValidator::StartCandidate(const FrameGenEngineConvention& convention, const std::string& name,
                                             bool prior) {
    // Photometric batches are of the convention the GPU took them with:
    // another one starts them over.
    if (!SameFrameGenEngineConvention(convention, candidate_)) {
        photometric_ = {};
        passed_ = 0;
        jitter_ = false;
    }
    candidate_ = convention;
    candidate_.jitter = jitter_ || convention.jitter;
    if (candidate_.jitter) candidate_.offset = {0.0f, 0.0f}; // the frame's bias holds it
    candidate_prior_ = prior;
    agreeing_ = 0;
    if (!name.empty()) report_.name = name;
}

void FrameGenEngineValidator::Decide(State state, const std::string& reason) {
    state_ = state;
    ++report_.decisions;
    report_.reason = reason;
}

void FrameGenEngineValidator::Suspend(const std::string& reason, double seconds) {
    suspended_at_ = seconds;
    suspensions_.push_back(seconds);
    while (!suspensions_.empty() && seconds - suspensions_.front() > kSuspensionWindow) suspensions_.pop_front();
    batch_ = {};
    photometric_ = {};
    agreeing_ = passed_ = worse_ = 0;
    verdicts_.clear();
    if (suspensions_.size() >= kSuspensionsToDisable) {
        Decide(State::kDisabled, std::to_string(suspensions_.size()) + " suspensions within 5 minutes, the last: " + reason);
        return;
    }
    Decide(State::kSuspended, reason);
}

FrameGenEngineValidator::Classified FrameGenEngineValidator::Classify(const Batch& batch) {
    Classified result;
    auto snap = [&](const std::array<double, 2>& gain, FrameGenEngineCamera camera) {
        double error = 0.0;
        const FrameGenEngineGain nearest = NearestFrameGenEngineGain(gain, batch.vectors, batch.color, error);
        if (!(error <= kSnapError)) {
            result.why = "gains " + Fixed(gain[0], 4) + "/" + Fixed(gain[1], 4) + " are no canonical ones (nearest " +
                         nearest.name + ", " + Fixed(100.0 * error, 0) + "% off)";
            return;
        }
        FrameGenEngineConvention convention;
        convention.gain = {static_cast<float>(nearest.gain[0]), static_cast<float>(nearest.gain[1])};
        convention.camera = camera;
        result.convention = convention;
        result.name = nearest.name;
    };
    // Head excluded: the measured flow with the head taken out explains the
    // vectors (e = b * h + c), and by the gain that gives them (1 / b) still
    // content's vectors are near zero in NDC against the head's movement
    // there. e is in the vectors' own units: compared unscaled, vectors in
    // pixels would pass only when exactly zero.
    std::array<double, 2> excluded_gain{};
    double excluded_corr2 = 1.0;
    bool explained = true;
    bool unmoved = true; // still content's vectors do not change at all (no gain needed to tell)
    for (int axis = 0; axis < 2; ++axis) {
        const FrameGenEngineAxisFit::Solution h = batch.measured[axis].SolveTranslation();
        explained = explained && h.valid && h.b != 0.0;
        if (explained) {
            excluded_gain[axis] = 1.0 / h.b;
            excluded_corr2 = std::min(excluded_corr2, h.corr2);
        }
        unmoved = unmoved && batch.cockpit[axis].Spread() <= 0.0;
    }
    if (explained && excluded_corr2 >= kFitCorr2) {
        bool still = true;
        for (int axis = 0; axis < 2; ++axis) {
            still = still && batch.cockpit[axis].Spread() * excluded_gain[axis] * excluded_gain[axis] <
                                 kExcludedShare * batch.cockpit[axis].PredictionSpread();
        }
        if (still) {
            result.corr2 = excluded_corr2;
            report_.corr2 = {excluded_corr2, excluded_corr2};
            snap(excluded_gain, FrameGenEngineCamera::kExcluded);
            return result;
        }
    } else if (unmoved) {
        result.corr2 = explained ? excluded_corr2 : 0.0;
        report_.corr2 = {result.corr2, result.corr2};
        result.why = explained ? "still content's vectors are near zero, and the measured flow explains them to corr2 " +
                                     Fixed(excluded_corr2, 2)
                               : "still content's vectors are near zero, and the measured flow does not explain them";
        return result;
    }
    // The cockpit fit per axis; without the head's translation to tell b
    // from a, the line through both.
    std::array<FrameGenEngineAxisFit::Solution, 2> fit{};
    bool translation = true;
    for (int axis = 0; axis < 2; ++axis) {
        fit[axis] = batch.cockpit[axis].Solve();
        if (!fit[axis].valid) {
            translation = false;
            fit[axis] = batch.cockpit[axis].SolveLine();
        }
        if (!fit[axis].valid || fit[axis].a == 0.0) {
            result.why = "the fit did not solve";
            return result;
        }
    }
    if (!translation) {
        for (int axis = 0; axis < 2; ++axis) fit[axis] = batch.cockpit[axis].SolveLine();
    }
    result.corr2 = std::min(fit[0].corr2, fit[1].corr2);
    report_.corr2 = {fit[0].corr2, fit[1].corr2};
    report_.gain = {1.0 / fit[0].a, 1.0 / fit[1].a};
    report_.ratio = translation ? std::array<double, 2>{fit[0].b / fit[0].a, fit[1].b / fit[1].a} : std::array<double, 2>{};
    if (result.corr2 < kFitCorr2) {
        result.why = "the fit explains the vectors to corr2 " + Fixed(fit[0].corr2, 2) + "/" + Fixed(fit[1].corr2, 2);
        return result;
    }
    // An axis the other axis's rotation explains better: swapped axes.
    for (int axis = 0; axis < 2; ++axis) {
        const FrameGenEngineAxisFit& own = batch.cockpit[axis];
        const FrameGenEngineAxisFit& other = batch.cockpit[1 - axis];
        const double diagonal = std::abs(own.ErrorRotation()) / std::sqrt(std::max(own.ErrorSquares() * own.RotationSquares(), 1e-30));
        const double crossed =
            std::abs(batch.swapped[axis]) / std::sqrt(std::max(own.ErrorSquares() * other.RotationSquares(), 1e-30));
        if (crossed > diagonal) {
            result.why = std::string("the other axis's rotation explains ") + (axis ? "y" : "x") + " better (swapped axes)";
            return result;
        }
    }
    FrameGenEngineCamera camera = FrameGenEngineCamera::kIncluded;
    if (translation) {
        const std::array<double, 2> ratio{fit[0].b / fit[0].a, fit[1].b / fit[1].a};
        const bool included = ratio[0] >= kIncludedLow && ratio[0] <= kIncludedHigh && ratio[1] >= kIncludedLow &&
                              ratio[1] <= kIncludedHigh;
        const bool rotation = std::abs(ratio[0]) < kRotationOnly && std::abs(ratio[1]) < kRotationOnly;
        if (!included && !rotation) {
            result.why = "b/a " + Fixed(ratio[0], 2) + "/" + Fixed(ratio[1], 2) +
                         " is neither the head's translation (0.75-1.35) nor none of it";
            return result;
        }
        camera = included ? FrameGenEngineCamera::kIncluded : FrameGenEngineCamera::kRotationOnly;
    }
    snap({1.0 / fit[0].a, 1.0 / fit[1].a}, camera);
    if (!result.convention) {
        return result;
    }
    // Jitter: the frames' intercepts under their batch's fit, in the vectors'
    // pixels (e = a p + c: c / a in NDC), over the last kJitterFrames frames
    // of the batches of this convention.
    if (!report_.name.empty() && report_.name != result.name) intercepts_.clear();
    for (const auto& frame : batch.frames) {
        if (frame[0].Samples() <= 0.0 || frame[1].Samples() <= 0.0) continue;
        std::array<double, 2> intercept{};
        for (int axis = 0; axis < 2; ++axis) {
            const double pixels = 0.5 * (axis == 0 ? batch.vectors.width : batch.vectors.height);
            intercept[axis] = frame[axis].Intercept(fit[axis].a, fit[axis].b) / fit[axis].a * pixels;
        }
        intercepts_.push_back(intercept);
        while (intercepts_.size() > kJitterFrames) intercepts_.pop_front();
    }
    std::array<double, 2> spread{}, mean{};
    if (intercepts_.size() >= kJitterMinimumFrames) {
        for (int axis = 0; axis < 2; ++axis) {
            for (const auto& c : intercepts_) mean[axis] += c[axis];
            mean[axis] /= static_cast<double>(intercepts_.size());
            for (const auto& c : intercepts_) spread[axis] += (c[axis] - mean[axis]) * (c[axis] - mean[axis]);
            spread[axis] = std::sqrt(spread[axis] / static_cast<double>(intercepts_.size() - 1));
        }
    }
    report_.jitter_px = spread;
    result.convention->jitter = std::max(spread[0], spread[1]) > kJitterPx &&
                                std::max(std::abs(mean[0]), std::abs(mean[1])) <= kJitterMeanPx;
    // An intercept every frame shares is the convention's offset (in e's
    // units: vector pixels to NDC, over the gain); smaller is the fit's
    // noise. Jittered, the frame's bias holds it.
    if (!result.convention->jitter && intercepts_.size() >= kJitterMinimumFrames &&
        std::max(std::abs(mean[0]), std::abs(mean[1])) >= kOffsetPx) {
        for (int axis = 0; axis < 2; ++axis) {
            const double pixels = 0.5 * std::max<std::uint32_t>(axis == 0 ? batch.vectors.width : batch.vectors.height, 1);
            result.convention->offset[axis] = static_cast<float>(mean[axis] / pixels / result.convention->gain[axis]);
        }
    }
    return result;
}

void FrameGenEngineValidator::JudgeBatch(double seconds) {
    const Classified result = Classify(batch_);
    ++report_.batches;
    // The pair before against this pair on the same frames: the vectors come
    // a frame late where it explains them better, batch after batch.
    if (batch_.lagged[0].Samples() >= kPhotometricMinimum && batch_.lagged_now[0].Samples() >= kPhotometricMinimum) {
        double lagged = 0.0, now = 0.0;
        for (int axis = 0; axis < 2; ++axis) {
            lagged += 0.5 * batch_.lagged[axis].SolveLine().corr2;
            now += 0.5 * batch_.lagged_now[axis].SolveLine().corr2;
        }
        lagging_ = lagged > now ? lagging_ + 1 : 0;
    }
    report_.lag_batches = lagging_;
    report_.lag = lagging_ >= kLagBatches;
    extents_ = {batch_.vectors, batch_.color};
    batch_ = {};
    const double gap = last_batch_ ? std::clamp(seconds - *last_batch_, 0.0, kBatchGapSeconds) : 0.0;
    last_batch_ = seconds;
    if (state_ == State::kSuspended) {
        return;
    }
    if (state_ == State::kLocked) {
        if (report_.lag) {
            Suspend("the vectors come a frame late (the head's movement a frame before fits them better)", seconds);
        } else if (result.convention && !SameFrameGenEngineConvention(*result.convention, candidate_) &&
                   result.corr2 >= kFitCorr2) {
            Suspend("a batch fitted " + result.name + ", " + ToString(result.convention->camera) + " instead", seconds);
        }
        return;
    }
    // Unlocked over 5 minutes of batches (each counting the time since the
    // one before, up to kBatchGapSeconds): disabled.
    unknown_seconds_ += gap;
    if (unknown_seconds_ >= kUnknownSeconds) {
        Decide(State::kDisabled, "no convention locked over 5 minutes of head movement (the last batch: " +
                                     (result.convention ? "fitted " + result.name : result.why) + ")");
        return;
    }
    if (!result.convention || (fixed_ && !SameFrameGenEngineConvention(*result.convention, *fixed_))) {
        agreeing_ = 0;
        report_.reason = result.convention ? "a batch fitted " + result.name + ", not the fixed convention" : result.why;
        return;
    }
    if (state_ == State::kProbing || !SameFrameGenEngineConvention(*result.convention, candidate_)) {
        StartCandidate(*result.convention, result.name,
                       (prior_ && SameFrameGenEngineConvention(*result.convention, *prior_)) || fixed_.has_value());
        Decide(State::kCalibrating, std::string("calibrating ") + result.name + ", " + ToString(candidate_.camera));
    }
    report_.name = result.name;
    if (result.convention->jitter && !jitter_) {
        // The bias comes out from now on: photometric batches start over.
        jitter_ = true;
        candidate_.jitter = true;
        candidate_.offset = {0.0f, 0.0f};
        photometric_ = {};
        passed_ = 0;
    }
    if (!candidate_.jitter) {
        candidate_.offset = result.convention->offset;
    }
    ++agreeing_;
    TryAdopt();
}

void FrameGenEngineValidator::JudgePhotometric(double seconds) {
    const FrameGenEngineEvidence sums = photometric_.sums;
    photometric_ = {};
    const double variant = sums.variants[0];
    std::string failed;
    for (std::size_t c = 1; c < sums.variants.size() && failed.empty(); ++c) {
        if (variant * kVariantMargin > sums.variants[c]) {
            failed = "variant " + std::to_string(c) + " explains the image within 15% (" +
                     Fixed(sums.variants[c] / std::max(variant, 1e-12), 3) + ")";
        }
    }
    if (failed.empty() && sums.moving >= kPhotometricMinimum / 2.0 && sums.variant_moving > kMovingShare * sums.zero_moving) {
        failed = "where the vectors move content, no motion explains it as well (" +
                 Fixed(sums.variant_moving / std::max(sums.zero_moving, 1e-12), 3) + " of " + Fixed(sums.moving, 0) + ")";
    }
    if (failed.empty() && sums.lag_samples >= kPhotometricMinimum && sums.lag_cost < kLagMargin * sums.lag_variant) {
        failed = "the field before explains the image as well (" + Fixed(sums.lag_cost / std::max(sums.lag_variant, 1e-12), 3) + ")";
    }
    bool worse = false;
    if (sums.with_measured >= kPhotometricMinimum && sums.textured > 0.0) {
        const double mean = variant / sums.textured;
        const double measured = sums.measured_cost / sums.with_measured;
        if (failed.empty() && mean > kMeasuredMargin * measured + kMeasuredSlack) {
            failed = "the measured flow explains the image better (" + Fixed(mean / std::max(measured, 1e-12), 3) + ")";
        }
        if (failed.empty() && sums.wins < kWinShare * sums.textured) {
            failed = "the vectors win on " + Fixed(100.0 * sums.wins / sums.textured, 0) + "% of the samples only";
        }
        worse = mean > kWorseThanMeasured * measured;
    }
    const bool pass = failed.empty();
    passed_ = pass ? passed_ + 1 : 0;
    report_.photometric = pass ? "passed" : failed;
    report_.photometric_passed = passed_;
    if (state_ == State::kLocked) {
        worse_ = worse ? worse_ + 1 : 0;
        if (worse_ >= 2) {
            Suspend("the vectors follow the image 30% worse than the measured flow", seconds);
        }
        return;
    }
    TryAdopt();
}

void FrameGenEngineValidator::TryAdopt() {
    if (state_ != State::kCalibrating) return;
    const std::uint32_t needed = candidate_prior_ ? 1u : kAdoptBatches;
    if (agreeing_ >= needed && passed_ >= kAdoptPhotometric && !report_.lag) {
        unknown_seconds_ = 0.0;
        verdicts_.clear();
        worse_ = 0;
        Decide(State::kLocked,
               "locked " + report_.name + ", " + ToString(candidate_.camera) + (candidate_.jitter ? ", jittered" : "") +
                   " after " + std::to_string(agreeing_) + " batch(es)");
    }
}

void FrameGenEngineValidator::Add(const FrameGenEngineFrameEvidence& frame, double seconds) {
    if (state_ == State::kDisabled || frame.views.empty()) {
        return;
    }
    ++report_.frames;
    bool held = false;
    bool lead_held = false;
    bool calibrate = true;
    for (const FrameGenEngineEvidence& view : frame.views) {
        ++report_.verdicts;
        lead_held = lead_held || LeadVerdict(view) != 0;
        if (view.verdict) {
            held = true;
            ++report_.invalid;
            for (std::size_t i = 0; i < kFrameGenEngineVerdictReasons; ++i) {
                report_.reasons[i] += (view.verdict >> i) & 1u;
            }
        }
        for (std::size_t band = 0; band < 3; ++band) {
            report_.epe[band] += view.epe[band];
            report_.epe_samples[band] += view.epe_samples[band];
        }
        report_.variant += view.variants[0];
        report_.zero += view.zero;
        report_.measured_cost += view.measured_cost;
        report_.textured += view.textured;
        report_.with_measured += view.with_measured;
        report_.lag_cost += view.lag_cost;
        report_.lag_variant += view.lag_variant;
        calibrate = calibrate && view.calibrate;
        extents_ = {view.vectors, view.color};
    }
    report_.held += held ? 1 : 0;
    report_.lead_held += lead_held ? 1 : 0;
    if (held_for_test_) {
        return;
    }
    if (state_ == State::kSuspended) {
        if (seconds - suspended_at_ < kSuspendSeconds) {
            return;
        }
        // Calibrating again, with the convention suspended as the prior.
        StartCandidate(candidate_, report_.name, true);
        Decide(State::kCalibrating, "calibrating " + report_.name + " again after 10 s suspended");
    }
    if (state_ == State::kLocked) {
        verdicts_.push_back(held ? 1 : 0);
        while (verdicts_.size() > kVerdictWindow) verdicts_.pop_front();
        const auto invalid = std::count(verdicts_.begin(), verdicts_.end(), std::uint8_t{1});
        if (verdicts_.size() >= kVerdictWindow && static_cast<double>(invalid) > kInvalidShare * kVerdictWindow) {
            Suspend(std::to_string(invalid) + " of the last " + std::to_string(kVerdictWindow) +
                        " frames' verdicts held the vectors back",
                    seconds);
            return;
        }
    }
    // A photometric batch: frames taken with the convention under test.
    if (SameFrameGenEngineConvention(frame.convention, candidate_) && frame.convention.jitter == candidate_.jitter) {
        for (const FrameGenEngineEvidence& view : frame.views) AddPhotometric(photometric_.sums, view);
        ++photometric_.frames;
        if (photometric_.sums.textured >= kPhotometricSamples && photometric_.frames >= kBatchFrames) {
            JudgePhotometric(seconds);
            if (state_ == State::kSuspended || state_ == State::kDisabled) return;
        }
    }
    // A batch: frames that may calibrate (every view).
    if (calibrate) {
        std::array<FrameGenEngineAxisFit, 2> own{};
        for (const FrameGenEngineEvidence& view : frame.views) {
            for (int axis = 0; axis < 2; ++axis) {
                batch_.cockpit[axis] += view.cockpit[axis];
                batch_.measured[axis] += view.measured[axis];
                batch_.swapped[axis] += view.swapped[axis];
                own[axis] += view.cockpit[axis];
                if (view.lag) {
                    batch_.lagged_now[axis] += view.cockpit[axis];
                    batch_.lagged[axis] += view.cockpit_lag[axis];
                }
            }
            batch_.l1 = view.l1;
            batch_.vectors = view.vectors;
            batch_.color = view.color;
        }
        batch_.frames.push_back(own);
        const double width = std::max<std::uint32_t>(batch_.l1.width, 1);
        const double height = std::max<std::uint32_t>(batch_.l1.height, 1);
        if (batch_.frames.size() >= kBatchFrames &&
            batch_.cockpit[0].RotationSpread() >= kBatchSpreadSamples * (3.0 / width) * (3.0 / width) &&
            batch_.cockpit[1].RotationSpread() >= kBatchSpreadSamples * (3.0 / height) * (3.0 / height)) {
            JudgeBatch(seconds);
        } else if (batch_.frames.size() >= kBatchMaxFrames) {
            batch_ = {};
        }
    }
}

FrameGenEngineValidation FrameGenEngineValidator::Report() const {
    FrameGenEngineValidation report = report_;
    report.state = state_;
    report.convention = candidate_;
    report.fixed = fixed_.has_value();
    report.prior = prior_.has_value();
    if (report.name.empty() && extents_.second.width) {
        double error = 0.0;
        const FrameGenEngineGain nearest = NearestFrameGenEngineGain({candidate_.gain[0], candidate_.gain[1]},
                                                                     extents_.first, extents_.second, error);
        if (error <= kSnapError) report.name = nearest.name;
    }
    return report;
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
        "noStruct", "skip", "pairing", "cut", "decode", "recreate", "settle", "uncaptured", "oneEye"};
    return index < kNames.size() ? kNames[index] : "";
}

std::uint32_t FrameGenEngineGates(const FrameGenEngineGateRecord* record, const FrameGenEngineGateRecord* previous,
                                  const FrameGenEngineGateFacts& facts) {
    std::uint32_t gates = 0;
    // G8: swapchains made lately (a game resizing its views makes them anew,
    // Star Citizen churns them at its world entry) settle first.
    if (facts.frames_since_swapchain < 2) gates |= kFrameGenEngineRecreate;
    if (facts.seconds_since_swapchain < kFrameGenEngineSettleSeconds) gates |= kFrameGenEngineSettle;
    // G10: one eye's vectors alone.
    if (facts.one_eye) gates |= kFrameGenEngineOneEye;
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

// --- Engine motion vectors: the world's motion from the application's ------

namespace {

using Rows = std::array<Vector3, 3>;

Rows Rotation(const std::array<double, 4>& q) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    return {{{1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)},
             {2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)},
             {2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)}}};
}

Rows Transposed(const Rows& m) {
    return {{{m[0][0], m[1][0], m[2][0]}, {m[0][1], m[1][1], m[2][1]}, {m[0][2], m[1][2], m[2][2]}}};
}

Rows Times(const Rows& a, const Rows& b) {
    Rows result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result[row][column] = a[row][0] * b[0][column] + a[row][1] * b[1][column] + a[row][2] * b[2][column];
        }
    }
    return result;
}

Vector3 Times(const Rows& m, const Vector3& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

// A rotation matrix's quaternion (x, y, z, w), w >= 0.
std::array<double, 4> Quaternion(const Rows& m) {
    std::array<double, 4> q{};
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0) {
        const double s = 0.5 / std::sqrt(trace + 1.0);
        q = {(m[2][1] - m[1][2]) * s, (m[0][2] - m[2][0]) * s, (m[1][0] - m[0][1]) * s, 0.25 / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
        q = {0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
        q = {(m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const double s = 2.0 * std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s};
    }
    if (q[3] < 0.0) q = {-q[0], -q[1], -q[2], -q[3]};
    return q;
}

double Dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return static_cast<double>(a[0]) * b[0] + static_cast<double>(a[1]) * b[1] + static_cast<double>(a[2]) * b[2];
}

// What makes a fit informative (per frame), and the lock's and the
// watchdog's thresholds (FrameGenWorldDeltaCalibration).
constexpr double kWorldDeltaMeters = 0.5;
constexpr double kWorldDeltaRadians = 0.002;
constexpr std::uint64_t kWorldDeltaLockFrames = 300;
constexpr std::uint64_t kWorldDeltaTurningFrames = 30;
constexpr double kWorldDeltaCorrelation = 0.9;
constexpr double kWorldDeltaScaleLow = 0.7;
constexpr double kWorldDeltaScaleHigh = 1.4;
constexpr double kWorldDeltaCheckError = 0.25;
constexpr std::uint32_t kWorldDeltaFailedChecks = 10;

} // namespace

FrameGenEngineWorld FrameGenEngineWorldParams(const FrameGenViewGeometry& game_newest,
                                              const FrameGenViewGeometry& game_previous,
                                              const FrameGenViewGeometry& declared_newest,
                                              const FrameGenViewGeometry& declared_previous,
                                              const std::array<float, 4>& delta_orientation,
                                              const std::array<float, 3>& delta_position, int sign, float scale) {
    std::array<double, 4> delta = Unit(delta_orientation);
    if (sign < 0) delta = {-delta[0], -delta[1], -delta[2], delta[3]};
    const Rows game_now = Rotation(Unit(game_newest.orientation));
    const Rows game_before = Rotation(Unit(game_previous.orientation));
    const Rows turn = Rotation(delta);
    const Vector3 moved{delta_position[0], delta_position[1], delta_position[2]};
    // G takes a point of the newest game camera to the previous one's (where
    // the world held it, as rendered), g is where its origin lands.
    const Rows g_rotation = Times(Transposed(game_before), Times(Transposed(turn), game_now));
    const Vector3 g_offset = Times(Transposed(game_before),
                                   Minus(Times(Transposed(turn), Minus(Position(game_newest), moved)), Position(game_previous)));
    // The declared relative pose exactly as the plan's constants carry it
    // (BaseConstants): the measured flow has it taken out.
    const Matrix3 declared = FrameGenRelativeRotation(declared_newest.orientation, declared_previous.orientation);
    const std::array<float, 3> declared_offset = RelativeTranslation(declared_newest, declared_previous);
    Rows q_d{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) q_d[row][column] = declared[row][column];
    }
    const Rows g_inverse = Transposed(g_rotation);
    const Rows r_w = Times(g_inverse, q_d);
    const Vector3 t_w = Times(g_inverse, Minus({declared_offset[0], declared_offset[1], declared_offset[2]}, g_offset));
    const std::array<double, 4> q = Quaternion(r_w);
    FrameGenEngineWorld world;
    bool finite = std::isfinite(scale);
    for (int i = 0; i < 3; ++i) {
        // WorldRows' small-angle form, as the fit solves for it.
        world.rotation[i] = static_cast<float>(2.0 * q[i]);
        world.translation[i] = static_cast<float>(scale * t_w[i]);
        finite = finite && std::isfinite(world.rotation[i]) && std::isfinite(world.translation[i]);
    }
    world.quaternion = {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
                        static_cast<float>(q[3])};
    world.valid = finite && std::isfinite(world.quaternion[3]);
    return world;
}

const char* ToString(FrameGenWorldDeltaMode mode) {
    switch (mode) {
    case FrameGenWorldDeltaMode::kCalibrate:
        return "calibrate";
    case FrameGenWorldDeltaMode::kOn:
        return "on";
    default:
        return "off";
    }
}

FrameGenWorldDeltaMode ParseFrameGenWorldDeltaMode(const std::string& value) {
    std::string text = value;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text == "calibrate") return FrameGenWorldDeltaMode::kCalibrate;
    if (text == "1") return FrameGenWorldDeltaMode::kOn;
    return FrameGenWorldDeltaMode::kOff;
}

std::optional<std::pair<int, float>> ParseFrameGenWorldDeltaFixed(const std::string& text) {
    int sign = 0;
    float scale = 0.0f;
    char rest = 0;
    if (std::sscanf(text.c_str(), " %d , %f %c", &sign, &scale, &rest) != 2 || (sign != 1 && sign != -1) ||
        !(scale >= kWorldDeltaScaleLow && scale <= kWorldDeltaScaleHigh)) {
        return std::nullopt;
    }
    return std::make_pair(sign, scale);
}

void FrameGenWorldDeltaCalibration::Fix(int sign, float scale) {
    status_.locked = status_.fixed = true;
    status_.sign = sign < 0 ? -1 : 1;
    status_.scale = scale;
    ++status_.decisions;
    status_.reason = "fixed (VECTORXR_FRAMEGEN_ENGINE_DELTA_FIXED)";
}

void FrameGenWorldDeltaCalibration::Restart() {
    rotation_squares_ = translation_squares_ = 0.0;
    rotation_products_ = rotation_model_ = translation_products_ = translation_model_ = {};
    failing_ = 0;
    status_.locked = status_.fixed = false;
    status_.sign = 1;
    status_.scale = 1.0f;
    status_.frames = status_.informative = status_.turning = 0;
    status_.rotation_correlation = {};
    status_.translation_correlation = status_.fitted_scale = 0.0;
}

void FrameGenWorldDeltaCalibration::Add(const std::vector<FrameGenWorldFit>& fits) {
    bool counted = false;
    bool informative = false;
    bool turning = false;
    // Locked: the fits against the locked model's translation.
    double error = 0.0, reference = 0.0;
    std::size_t views = 0;
    for (const FrameGenWorldFit& fit : fits) {
        if (!fit.valid) continue;
        // The model under either sign of the delta's rotation, at scale 1.
        std::array<FrameGenEngineWorld, 2> models;
        for (int i = 0; i < 2; ++i) {
            models[i] = FrameGenEngineWorldParams(fit.game_newest, fit.game_previous, fit.newest, fit.previous,
                                                  fit.delta_orientation, fit.delta_position, i ? -1 : 1, 1.0f);
        }
        if (!models[0].valid || !models[1].valid) continue;
        counted = true;
        if (status_.locked) {
            const std::array<float, 3>& model = models[status_.sign > 0 ? 0 : 1].translation;
            double off = 0.0, length = 0.0;
            for (int i = 0; i < 3; ++i) {
                const double expected = static_cast<double>(status_.scale) * model[i];
                off += (fit.translation[i] - expected) * (fit.translation[i] - expected);
                length += expected * expected;
            }
            error += std::sqrt(off);
            reference += std::sqrt(length);
            ++views;
            continue;
        }
        const double rotation = std::sqrt(Dot(fit.rotation, fit.rotation));
        const double translation = std::sqrt(Dot(fit.translation, fit.translation));
        if (!(translation > kWorldDeltaMeters || rotation > kWorldDeltaRadians)) continue;
        informative = true;
        turning = turning || rotation > kWorldDeltaRadians;
        rotation_squares_ += rotation * rotation;
        translation_squares_ += translation * translation;
        for (int i = 0; i < 2; ++i) {
            rotation_products_[i] += Dot(fit.rotation, models[i].rotation);
            rotation_model_[i] += Dot(models[i].rotation, models[i].rotation);
            translation_products_[i] += Dot(fit.translation, models[i].translation);
            translation_model_[i] += Dot(models[i].translation, models[i].translation);
        }
    }
    if (!counted) return;
    ++status_.frames;
    if (status_.locked) {
        // The watchdog: only a model moving far enough to judge by.
        if (views == 0 || reference < kWorldDeltaMeters * static_cast<double>(views)) return;
        ++status_.checks;
        status_.last_error = error / reference;
        if (status_.last_error <= kWorldDeltaCheckError) {
            failing_ = 0;
            return;
        }
        ++status_.failed;
        if (++failing_ < kWorldDeltaFailedChecks) return;
        const std::string last = std::to_string(static_cast<int>(std::lround(100.0 * status_.last_error)));
        Restart();
        ++status_.drops;
        ++status_.decisions;
        status_.reason = "the fit's translation was more than 25% off the delta's in " +
                         std::to_string(kWorldDeltaFailedChecks) + " checks in a row (the last " + last +
                         "%): the fit again, calibrating anew";
        return;
    }
    if (!informative) return;
    ++status_.informative;
    status_.turning += turning ? 1 : 0;
    for (int i = 0; i < 2; ++i) {
        const double spread = rotation_squares_ * rotation_model_[i];
        status_.rotation_correlation[i] = spread > 0.0 ? rotation_products_[i] / std::sqrt(spread) : 0.0;
    }
    const int best = status_.rotation_correlation[1] > status_.rotation_correlation[0] ? 1 : 0;
    const double spread = translation_squares_ * translation_model_[best];
    status_.translation_correlation = spread > 0.0 ? translation_products_[best] / std::sqrt(spread) : 0.0;
    status_.fitted_scale = translation_model_[best] > 0.0 ? translation_products_[best] / translation_model_[best] : 0.0;
    if (status_.informative >= kWorldDeltaLockFrames && status_.turning >= kWorldDeltaTurningFrames &&
        status_.rotation_correlation[best] > kWorldDeltaCorrelation &&
        status_.translation_correlation > kWorldDeltaCorrelation && status_.fitted_scale >= kWorldDeltaScaleLow &&
        status_.fitted_scale <= kWorldDeltaScaleHigh) {
        status_.locked = true;
        status_.sign = best ? -1 : 1;
        status_.scale = static_cast<float>(status_.fitted_scale);
        ++status_.decisions;
        status_.reason = std::string("rotation ") + (best ? "conjugated" : "as given") + " (correlation " +
                         Fixed(status_.rotation_correlation[best], 3) + " against " +
                         Fixed(status_.rotation_correlation[1 - best], 3) + "), s_t " + Fixed(status_.fitted_scale, 3) +
                         " (correlation " + Fixed(status_.translation_correlation, 3) + ") after " +
                         std::to_string(status_.informative) + " informative frames, " +
                         std::to_string(status_.turning) + " turning";
    }
}

std::optional<FrameGenEngineConvention> FrameGenEnginePriorFor(const std::string& executable, bool ext,
                                                               const std::string& api) {
    // MSFS 2024 writes XR_FB_space_warp's vectors on Direct3D 12. The entry is
    // the specification's convention (D3D NDC, y up, current minus previous,
    // the head's movement included) until a flight's census confirms it: a
    // prior only lets one batch adopt it, the evidence decides all the same.
    struct Entry {
        const char* executable;
        bool ext;
        const char* api;
        FrameGenEngineConvention convention;
    };
    static const Entry kPriors[] = {
        {"flightsimulator2024.exe", false, "D3D12", FrameGenEngineConvention{}},
    };
    std::string name = executable;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const Entry& entry : kPriors) {
        if (name == entry.executable && ext == entry.ext && api == entry.api) {
            return entry.convention;
        }
    }
    // XR_EXT_frame_synthesis' vectors are in Vulkan-style NDC (y down) on every
    // API, and XR_FB_space_warp's in the API's own NDC, y down in Vulkan's:
    // decoded, the current frame minus the previous, n = (1, -1) * e. A game
    // that flips its projection (as some ports to Vulkan do) is told by the
    // evidence all the same.
    if (ext || api == "Vulkan") {
        FrameGenEngineConvention convention;
        convention.gain = {1.0f, -1.0f};
        return convention;
    }
    return std::nullopt;
}

double FrameGenAngularAcceleration(const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous,
                                   const FrameGenViewGeometry& before) {
    // Each frame's turn as a rotation vector in the world (q_n q_p^-1).
    auto turn = [](const FrameGenViewGeometry& to, const FrameGenViewGeometry& from) {
        std::array<double, 4> inverse = Unit(from.orientation);
        inverse = {-inverse[0], -inverse[1], -inverse[2], inverse[3]};
        std::array<double, 4> q = Multiply(Unit(to.orientation), inverse);
        if (q[3] < 0.0) q = {-q[0], -q[1], -q[2], -q[3]};
        const double sine = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        const double angle = 2.0 * std::atan2(sine, q[3]) * kDegreesPerRadian;
        const double scale = sine > 1e-12 ? angle / sine : 2.0 * kDegreesPerRadian;
        return std::array<double, 3>{q[0] * scale, q[1] * scale, q[2] * scale};
    };
    const std::array<double, 3> now = turn(newest, previous);
    const std::array<double, 3> then = turn(previous, before);
    return std::sqrt((now[0] - then[0]) * (now[0] - then[0]) + (now[1] - then[1]) * (now[1] - then[1]) +
                     (now[2] - then[2]) * (now[2] - then[2]));
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
    if (wanted.rfind("VECTORXR_FRAMEGEN_ENGINE_", 0) != 0 && wanted.rfind("VECTORXR_FRAMEGEN_DEPTH_", 0) != 0 &&
        wanted.rfind("VECTORXR_FRAMEGEN_SYNTH_", 0) != 0 && wanted.rfind("VECTORXR_FRAMEGEN_FLOW_", 0) != 0 &&
        wanted != "VECTORXR_FRAMEGEN_WORLD") {
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

void ResetResearchFileSwitches() {
    ResearchFile& research = Research();
    std::scoped_lock lock(research.mutex);
    research.taken.clear();
    research.untaken.clear();
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

std::filesystem::path WriteFrameGenEngineRemoved(const std::string& cause) {
    const std::filesystem::path path = FrameGenEngineRemovedPath();
    if (path.empty()) return {};
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream(path) << cause
                        << " within 2 s of VectorXR Frame Generation reading this game's motion vectors.\n"
                           "While this file is here, its sessions read none. Delete it to read them again.\n";
    return path;
}

} // namespace depthxr
