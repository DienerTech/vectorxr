#include "depthxr/framegen_plan.h"

#include <algorithm>
#include <cmath>

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

FrameGenViewPlan BuildFrameGenViewPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning) {
    FrameGenViewPlan plan;
    plan.extent = request.extent;
    plan.levels = FrameGenPyramid(request.extent);
    plan.final_flow_level = FrameGenFinalFlowLevel(tuning.quality);
    plan.synthesize = request.synthesize && request.extent.width >= 64 && request.extent.height >= 64;

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
    if (tuning.mode == FrameGenSynthesisMode::kExtrapolate) {
        flags |= kFrameGenFlagExtrapolate;
    }
    if (request.temporal) {
        flags |= kFrameGenFlagTemporal;
    }
    if (request.depth) {
        flags |= kFrameGenFlagDepth;
    }
    const FrameGenTexture depth = request.depth ? Texture(FrameGenTextureKind::kDepth) : FrameGenTexture{};
    base.options = {kFrameGenSearchRadius, flags, static_cast<std::uint32_t>(tuning.diagnostic), 0};
    const std::size_t generated = std::clamp<std::size_t>(tuning.generated, 1, kFrameGenMaxGenerated);
    const auto all_outputs = static_cast<std::uint8_t>((1u << generated) - 1u);
    base.tuning = {std::clamp(tuning.fractions[0], 0.0f, 1.0f), limit, 4.0f + 26.0f * suppression,
                   3.0f + 21.0f * suppression};

    const FrameGenPass synthesize = request.output == FrameGenOutputKind::kFloat16 ? FrameGenPass::kSynthesizeFloat16
                                                                                   : FrameGenPass::kSynthesizeUnorm8;
    if (!plan.synthesize) {
        // Still image: the synthesis pass with the still flag and no flow.
        FrameGenDispatch still = Dispatch(synthesize, plan.levels[0], plan.levels[0], base);
        still.constants.options[1] |= kFrameGenFlagStill;
        still.constants.options[2] = static_cast<std::uint32_t>(FrameGenDiagnosticView::kOff);
        still.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), {}, {}, {}};
        still.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
        still.copy_mask = all_outputs;
        plan.dispatches.push_back(still);
        return plan;
    }

    FrameGenDispatch luma = Dispatch(FrameGenPass::kLuma, plan.levels[1], plan.levels[0], base);
    luma.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), Texture(FrameGenTextureKind::kHistoryPrevious), depth, {}};
    luma.outputs = {Texture(FrameGenTextureKind::kLumaNewest, 1), Texture(FrameGenTextureKind::kLumaPrevious, 1)};
    luma.constants.options[3] = 1;
    plan.dispatches.push_back(luma);

    for (std::uint8_t level = 2; level <= kFrameGenPyramidLevels; ++level) {
        FrameGenDispatch down = Dispatch(FrameGenPass::kDownsample, plan.levels[level], plan.levels[level - 1], base);
        down.inputs = {Texture(FrameGenTextureKind::kLumaNewest, level - 1),
                       Texture(FrameGenTextureKind::kLumaPrevious, level - 1), {}, {}};
        down.outputs = {Texture(FrameGenTextureKind::kLumaNewest, level), Texture(FrameGenTextureKind::kLumaPrevious, level)};
        down.constants.options[3] = level;
        plan.dispatches.push_back(down);
    }

    // Flow passes match on luma at `match` and write one vector per cell of
    // the flow level; options.w carries the cell size in match texels.
    FrameGenDispatch search = Dispatch(FrameGenPass::kSearch, plan.levels[kFrameGenPyramidLevels],
                                       plan.levels[kFrameGenPyramidLevels], base);
    search.inputs = {Texture(FrameGenTextureKind::kLumaNewest, kFrameGenPyramidLevels),
                     Texture(FrameGenTextureKind::kLumaPrevious, kFrameGenPyramidLevels), {}, {}};
    search.outputs = {Texture(FrameGenTextureKind::kFlow, kFrameGenPyramidLevels), {}};
    search.constants.options[3] = 1;
    plan.dispatches.push_back(search);

    for (std::uint8_t level = kFrameGenPyramidLevels - 1; level >= plan.final_flow_level; --level) {
        const std::uint8_t match = FrameGenMatchLevel(level);
        FrameGenDispatch refine = Dispatch(FrameGenPass::kRefine, plan.levels[level], plan.levels[level + 1], base);
        refine.inputs = {Texture(FrameGenTextureKind::kLumaNewest, match), Texture(FrameGenTextureKind::kLumaPrevious, match),
                         Texture(FrameGenTextureKind::kFlow, level + 1),
                         request.temporal ? Texture(FrameGenTextureKind::kFilteredPrevious, plan.final_flow_level)
                                          : FrameGenTexture{}};
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
    // Last frame's filtered flow and the depth: how steady each vector is.
    filter.inputs = {Texture(FrameGenTextureKind::kFlow, plan.final_flow_level),
                     request.temporal ? Texture(FrameGenTextureKind::kFilteredPrevious, plan.final_flow_level)
                                      : FrameGenTexture{},
                     depth, {}};
    filter.outputs = {Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level), {}};
    filter.constants.options[3] = plan.final_flow_level;
    filter.constants.tuning[0] = std::clamp(tuning.interval_ratio, 0.25f, 4.0f);
    plan.dispatches.push_back(filter);

    for (std::size_t index = 0; index < generated; ++index) {
        FrameGenDispatch output = Dispatch(synthesize, plan.levels[0], plan.levels[plan.final_flow_level], base);
        output.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), Texture(FrameGenTextureKind::kHistoryPrevious),
                         Texture(FrameGenTextureKind::kFilteredNewest, plan.final_flow_level), depth};
        output.outputs = {Texture(FrameGenTextureKind::kOutput), {}};
        output.constants.tuning[0] = std::clamp(tuning.fractions[index], 0.0f, 1.0f);
        output.copy_mask = static_cast<std::uint8_t>(1u << index);
        plan.dispatches.push_back(output);
    }
    return plan;
}

FrameGenDispatch BuildFrameGenDepthCheckDispatch(const FrameGenViewGeometry& view0, const FrameGenViewGeometry& view1,
                                                 const FrameGenDepthEncoding& encoding) {
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
    // 1/near + 1/far: the swapped reading of a depth decoded to z is
    // 1 / (x - 1/z), whichever way round the decoding currently is.
    auto inverse = [](float z) { return std::isfinite(z) && z > 0.0f ? 1.0f / z : 0.0f; };
    dispatch.constants.tuning = {inverse(encoding.near_z) + inverse(encoding.far_z), 0.0f, 0.0f, 0.0f};
    dispatch.outputs = {Texture(FrameGenTextureKind::kNone), {}};
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
    dispatch.inputs = {Texture(FrameGenTextureKind::kHistoryNewest), {}, {}, {}}; // the other view's newest goes in t1
    dispatch.outputs = {Texture(FrameGenTextureKind::kDepth), {}};
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
    return *this;
}

void FrameGenDepthValidator::Add(const FrameGenDepthEvidence& check) {
    sums_ += check;
    if (++checks_ < 3) {
        return;
    }
    evidence_ = sums_;
    sums_ = {};
    checks_ = 0;
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

    if (swapped_ != swapped_before) {
        ++generation_;
    }
    const bool changed = reading_ != reading_before || swapped_ != swapped_before;
    if (changed) {
        ++decisions_;
    }
    settled_ = !changed && reading_ != FrameGenDepthReading::kUnknown;
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
    const float inverse_near = inverse(encoding.near_z);
    const float inverse_far = inverse(encoding.far_z);
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
                                         std::uint8_t final_level) {
    unsigned change = kNone;
    if (extent.width != extent_.width || extent.height != extent_.height || source_format != source_format_ ||
        output != output_) {
        extent_ = extent;
        source_format_ = source_format;
        output_ = output;
        final_level_ = final_level;
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
    return extent.width == extent_.width && extent.height == extent_.height && extent.width && extent.height &&
           source_format == source_format_ && output == output_ && final_level == final_level_;
}

FrameGenViewRequest FrameGenHistoryState::Effective(const FrameGenViewRequest& request) const {
    FrameGenViewRequest effective = request;
    effective.synthesize = request.synthesize && history_valid_[Previous()];
    effective.temporal = effective.synthesize && request.temporal && filtered_valid_[Previous()];
    return effective;
}

void FrameGenHistoryState::Commit(bool synthesized) {
    history_valid_[newest_] = true;
    filtered_valid_[newest_] = synthesized;
    newest_ ^= 1u;
}

void FrameGenHistoryState::Invalidate() {
    history_valid_ = {};
    filtered_valid_ = {};
    newest_ = 0;
}

} // namespace depthxr
