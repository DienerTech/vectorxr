#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "depthxr/settings.h"

namespace depthxr {

// API-neutral description of one Frame Generation GPU submission. The plan is
// computed once on the CPU and replayed by the Direct3D 11, Direct3D 12 and
// Vulkan backends, so the algorithm (pass order, pyramid sizes and shader
// constants) has exactly one implementation.

enum class FrameGenPass : std::uint8_t {
    kLuma,
    kDownsample,
    kSearch,
    kRefine,
    kFilter,
    kSynthesizeUnorm8,
    kSynthesizeFloat16,
};
inline constexpr std::size_t kFrameGenPassCount = 7;

// Logical textures owned per view. History and filtered flow are ping-pong
// pairs: "newest" is written this submission, "previous" is last
// submission's. The backend flips the pair after each successful view.
enum class FrameGenTextureKind : std::uint8_t {
    kNone,
    kHistoryNewest,
    kHistoryPrevious,
    kLumaNewest,
    kLumaPrevious,
    kFlow,
    kFilteredNewest,
    kFilteredPrevious,
    kOutput,
};

struct FrameGenTexture {
    FrameGenTextureKind kind{FrameGenTextureKind::kNone};
    std::uint8_t level{0}; // pyramid level for luma and flow textures (1 = half resolution)
};

// Must match FrameGenConstants in shaders/framegen.hlsl (128 bytes).
struct FrameGenConstants {
    std::array<float, 4> fov_current{};
    std::array<float, 4> fov_previous{};
    std::array<float, 4> rot0{};
    std::array<float, 4> rot1{};
    std::array<float, 4> rot2{};
    std::array<std::uint32_t, 4> size{};
    std::array<std::uint32_t, 4> options{};
    std::array<float, 4> tuning{};
};
static_assert(sizeof(FrameGenConstants) == 128, "Frame Generation constants must fit Vulkan's minimum push constant size");

inline constexpr std::uint32_t kFrameGenFlagSrgbEncode = 1u;
inline constexpr std::uint32_t kFrameGenFlagExtrapolate = 2u;
inline constexpr std::uint32_t kFrameGenFlagTemporal = 4u;
inline constexpr std::uint32_t kFrameGenFlagStill = 8u;
inline constexpr std::uint32_t kFrameGenThreadGroupSize = 8u;
inline constexpr std::uint8_t kFrameGenPyramidLevels = 5;
inline constexpr std::uint32_t kFrameGenSearchRadius = 6;

struct FrameGenDispatch {
    FrameGenPass pass{FrameGenPass::kLuma};
    std::array<FrameGenTexture, 4> inputs{};
    std::array<FrameGenTexture, 2> outputs{};
    std::uint32_t width{0};  // threads, not groups
    std::uint32_t height{0};
    FrameGenConstants constants{};
};

enum class FrameGenOutputKind : std::uint8_t {
    kSrgb8,  // 8-bit sRGB swapchain: the shader encodes, the copy moves raw bits
    kUnorm8, // 8-bit UNORM swapchain: values pass through unchanged
    kFloat16,
};

struct FrameGenExtent {
    std::uint32_t width{0};
    std::uint32_t height{0};
};

// Pose data for one projection view: unit quaternion (x, y, z, w) and the
// four FOV angles in radians, as submitted in XrCompositionLayerProjectionView.
struct FrameGenViewGeometry {
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    float angle_left{0.0f};
    float angle_right{0.0f};
    float angle_up{0.0f};
    float angle_down{0.0f};
};

struct FrameGenViewRequest {
    FrameGenExtent extent;
    FrameGenOutputKind output{FrameGenOutputKind::kSrgb8};
    // False writes the newest frame unchanged (priming, discontinuity, or a
    // view whose geometry changed); the flow passes are skipped.
    bool synthesize{false};
    // False when last submission's filtered flow belongs to another view
    // geometry or quality and must not seed this frame's search.
    bool temporal{false};
    FrameGenViewGeometry newest;
    FrameGenViewGeometry previous;
};

struct FrameGenTuning {
    FrameGenSynthesisMode mode{FrameGenSynthesisMode::kInterpolate};
    FrameGenFlowQuality quality{FrameGenFlowQuality::kBalanced};
    FrameGenDiagnosticView diagnostic{FrameGenDiagnosticView::kOff};
    int artifact_suppression{50};
    int motion_limit_percent{10};
    // Interpolation: generated time between the previous (0) and newest (1)
    // real frames. Extrapolation: distance past the newest frame, in units of
    // the interval between the two real frames.
    float fraction{0.5f};
};

struct FrameGenViewPlan {
    FrameGenExtent extent;
    std::array<FrameGenExtent, kFrameGenPyramidLevels + 1> levels{}; // [0] = full resolution
    std::uint8_t final_flow_level{3};
    bool synthesize{false};
    std::vector<FrameGenDispatch> dispatches;
};

// Per-view ping-pong bookkeeping shared by every backend. Slots hold the
// history color copy and the filtered flow that seeds the next search.
class FrameGenHistoryState {
  public:
    enum Change : unsigned {
        kNone = 0,
        kAll = 1,      // extent, source format or output kind changed: recreate every texture
        kFiltered = 2, // flow quality changed: recreate the filtered flow pair
    };

    unsigned Configure(FrameGenExtent extent, std::int64_t source_format, FrameGenOutputKind output,
                       std::uint8_t final_level);
    FrameGenViewRequest Effective(const FrameGenViewRequest& request) const;
    void Commit(bool synthesized);
    void Invalidate();

    std::uint8_t Newest() const { return newest_; }
    std::uint8_t Previous() const { return static_cast<std::uint8_t>(newest_ ^ 1u); }
    FrameGenExtent Extent() const { return extent_; }

  private:
    FrameGenExtent extent_{};
    std::int64_t source_format_{0};
    FrameGenOutputKind output_{FrameGenOutputKind::kSrgb8};
    std::uint8_t final_level_{0};
    std::uint8_t newest_{0};
    std::array<bool, 2> history_valid_{};
    std::array<bool, 2> filtered_valid_{};
};

std::uint8_t FrameGenFinalFlowLevel(FrameGenFlowQuality quality);
// Luma level a flow level matches against: two levels finer, at most half resolution.
std::uint8_t FrameGenMatchLevel(std::uint8_t flow_level);
std::array<FrameGenExtent, kFrameGenPyramidLevels + 1> FrameGenPyramid(FrameGenExtent extent);
FrameGenViewPlan BuildFrameGenViewPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning);

// Interpolation fraction or extrapolation distance for a generated frame
// displayed at target_time, given the two real frames' display times. Clamped
// so a hitch never pushes content past the real frames' motion.
float FrameGenFraction(FrameGenSynthesisMode mode, std::int64_t previous_time, std::int64_t newest_time,
                       std::int64_t target_time);

// Row-major rotation taking newest-view directions into previous-view space.
std::array<std::array<float, 3>, 3> FrameGenRelativeRotation(const std::array<float, 4>& newest,
                                                             const std::array<float, 4>& previous);

} // namespace depthxr
