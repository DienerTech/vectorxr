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
    kDepth,      // the application's depth image to linear view depth (capture time)
    kDepthCheck, // scores readings of the depth against the other eye
    kDepthTrust, // per texel, drops depth the other eye disproves
    // Optical flow hardware (FrameGenTuning::hardware_flow): the half-
    // resolution luma pair to 8-bit images for it; the backend runs it right
    // after this dispatch.
    kFlowPack,
    kFlowUnpack, // its vectors into the flow field, with our match cost
    // Depth (Direct3D 12): each texel's trust score from the other eye's
    // evidence (kDepthTrust), the previous frame and last frame's score;
    // drops depth the score disproves.
    kTrustUpdate,
    // The eye-shift map (FrameGenViewRequest::shift): where each texel of the
    // newest frame lands seen from the eye's position at a generated frame's
    // time; nearest content wins, uncovered texels stretch the background.
    kShiftClear,
    kShiftSplat,
    kShiftResolve,
    kShiftFill,
    // Optical flow hardware matching frames as rendered, started when the
    // game releases each image: its half-resolution luma (no head movement
    // taken out), recorded with the capture (FrameGenViewRequest::hardware_raw).
    kFlowPackRaw,
};
inline constexpr std::size_t kFrameGenPassCount = 18;

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
    kDepth, // newest frame's linear view depth in meters, at half resolution
    // Optical flow hardware: its 8-bit inputs at half resolution and its
    // vectors (16-bit S10.5 pixels of the inputs, from newest to previous)
    // at kFrameGenHardwareFlowLevel.
    kHardwareNewest,
    kHardwarePrevious,
    kHardwareFlow,
    // Depth trust scores (kTrustUpdate) at half resolution, a ping-pong pair
    // like the history.
    kTrustNewest,
    kTrustPrevious,
    // The eye-shift map at half resolution: the splat keys (nearest content
    // reaching each texel) and the offsets back to the newest frame.
    kShiftKeys,
    kShiftMap,
};

// The flow level the optical flow hardware fills: a 4x4 grid on the
// half-resolution luma.
inline constexpr std::uint8_t kFrameGenHardwareFlowLevel = 3;

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
// The newest frame's depth is bound: the previous frame is mapped into the
// newest view by the full relative head pose, not rotation alone.
inline constexpr std::uint32_t kFrameGenFlagDepth = 16u;
// Synthesis reads the eye-shift map (input 4); options.w holds the frame
// time (float bits) the map is for.
inline constexpr std::uint32_t kFrameGenFlagShift = 32u;
// kTrustUpdate's options.x: this frame's stereo evidence is in the newest
// trust texture, last frame's scores are valid, the previous frame is.
inline constexpr std::uint32_t kFrameGenTrustStereo = 1u;
inline constexpr std::uint32_t kFrameGenTrustHistory = 2u;
inline constexpr std::uint32_t kFrameGenTrustMotion = 4u;
inline constexpr std::uint32_t kFrameGenThreadGroupSize = 8u;
inline constexpr std::uint8_t kFrameGenPyramidLevels = 5;
inline constexpr std::uint32_t kFrameGenSearchRadius = 6;
// Generated frames per real frame at most (a 4x multiplier).
inline constexpr std::size_t kFrameGenMaxGenerated = 3;
// Eye movement (meters) below which generated frames are not shifted: a
// fraction of a pixel even for content a hand's width away.
inline constexpr float kFrameGenMinimumShift = 5e-5f;

struct FrameGenDispatch {
    FrameGenPass pass{FrameGenPass::kLuma};
    // Sampled textures t0-t4; only Direct3D 12 binds the fifth (the eye-shift
    // map, used with depth).
    std::array<FrameGenTexture, 5> inputs{};
    std::array<FrameGenTexture, 2> outputs{};
    std::uint32_t width{0};  // threads, not groups
    std::uint32_t height{0};
    FrameGenConstants constants{};
    // After this dispatch, copy the output texture to every generated-frame
    // output whose bit is set.
    std::uint8_t copy_mask{0};
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

// Pose data for one projection view: unit quaternion (x, y, z, w), position
// in meters and the four FOV angles in radians, as submitted in
// XrCompositionLayerProjectionView.
struct FrameGenViewGeometry {
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
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
    // The newest frame's depth was captured for this view: content fixed
    // relative to the head's reference space (a cockpit) then lines up
    // exactly, and the flow holds only motion relative to that space.
    bool depth{false};
    FrameGenViewGeometry newest;
    FrameGenViewGeometry previous;
    // With depth: how far the eye moves per unit of generated-frame time
    // (FrameGenTuning::fractions past the newest frame when extrapolating,
    // 1 - fraction before it when interpolating), in meters in the newest
    // view's space. Generated frames then show trusted cockpit content from
    // where the eye will be: runtimes that only rotate frames never correct
    // head translation, so near content would jump at the real frame rate.
    std::array<float, 3> shift{0.0f, 0.0f, 0.0f};
    // Set by backends: this frame's stereo trust evidence was recorded for
    // the view (kDepthTrust), and last frame's trust scores are valid.
    bool trust_stereo{false};
    bool trust_history{false};
    // Set by backends: the optical flow hardware already matched this frame
    // and the previous one as rendered (kFlowPackRaw at capture), so the plan
    // packs nothing and kFlowUnpack takes the head movement out.
    bool hardware_raw{false};
};

struct FrameGenTuning {
    FrameGenSynthesisMode mode{FrameGenSynthesisMode::kInterpolate};
    FrameGenFlowQuality quality{FrameGenFlowQuality::kBalanced};
    FrameGenDiagnosticView diagnostic{FrameGenDiagnosticView::kOff};
    int artifact_suppression{50};
    int motion_limit_percent{10};
    // One entry per generated frame (`generated` of them). Interpolation:
    // generated time between the previous (0) and newest (1) real frames.
    // Extrapolation: distance past the newest frame, in units of the interval
    // between the two real frames. One flow field serves every fraction.
    std::array<float, kFrameGenMaxGenerated> fractions{0.5f, 0.5f, 0.5f};
    std::uint8_t generated{1};
    // The newest real frame pair's interval over the pair before's: last
    // pair's motion times this is what steady motion moves now.
    float interval_ratio{1.0f};
    // Motion measured by the GPU's optical flow hardware instead of block
    // matching (kFlowPack, the hardware, kFlowUnpack), at
    // kFrameGenHardwareFlowLevel whatever the quality.
    bool hardware_flow{false};
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
    // True when Configure with these arguments would change nothing.
    bool Matches(FrameGenExtent extent, std::int64_t source_format, FrameGenOutputKind output,
                 std::uint8_t final_level) const;
    // True when Configure would keep the history images (the flow level
    // may still change).
    bool MatchesImages(FrameGenExtent extent, std::int64_t source_format, FrameGenOutputKind output) const;
    FrameGenViewRequest Effective(const FrameGenViewRequest& request) const;
    // `trusted`: the newest trust scores were written (kTrustUpdate ran).
    void Commit(bool synthesized, bool trusted = false);
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
    std::array<bool, 2> trust_valid_{};
};

std::uint8_t FrameGenFinalFlowLevel(FrameGenFlowQuality quality);
// The level the plan's flow ends at: the quality's, or the hardware's.
std::uint8_t FrameGenFinalFlowLevel(const FrameGenTuning& tuning);

// How an application's depth swapchain encodes view depth
// (XrCompositionLayerDepthInfoKHR): minDepth..maxDepth spans nearZ..farZ in
// meters with 1/z linear; nearZ > farZ is reversed depth; either may be
// infinite.
struct FrameGenDepthEncoding {
    float min_depth{0.0f};
    float max_depth{1.0f};
    float near_z{0.1f};
    float far_z{1000.0f};
};

// The kDepth dispatch for one view: resamples the region `depth_rect` (in
// depth texels, array slice `array_index`) of the application's depth image
// to the view's half-resolution grid as linear view depth.
FrameGenDispatch BuildFrameGenDepthDispatch(FrameGenExtent view_extent, std::int32_t rect_x, std::int32_t rect_y,
                                            std::uint32_t rect_width, std::uint32_t rect_height,
                                            std::uint32_t array_index, const FrameGenDepthEncoding& encoding);

// Games do not always describe their depth correctly (reversed depth marked
// as standard is common, transparent cockpit glass is written to depth), and
// wrong depth makes head-movement compensation distort the image. Depth is
// therefore checked against the other eye: view 0 is warped into view 1 by
// its depth and the eyes' relative pose, which only lines up where the depth
// is read correctly. The kDepthCheck pass scores two readings of view 0's
// captured depth (as decoded, near and far swapped) against no depth,
// writing kFrameGenDepthCheckTexels texels per cell of a
// kFrameGenDepthCheckCells grid: a FrameGenDepthScore per reading, then the
// samples both eyes see.
inline constexpr std::uint32_t kFrameGenDepthCheckCells = 32;
inline constexpr std::uint32_t kFrameGenDepthCheckSamples = 4; // per cell side
inline constexpr std::uint32_t kFrameGenDepthCheckTexels = 3;  // per cell, side by side
FrameGenDispatch BuildFrameGenDepthCheckDispatch(const FrameGenViewGeometry& view0, const FrameGenViewGeometry& view1,
                                                 const FrameGenDepthEncoding& encoding);

// The kDepthTrust dispatch for one view of a stereo pair: compares each
// texel of `own`'s linear depth with what `other` sees and writes the
// evidence (positive confirms the depth, negative says the content is
// infinitely far) into the view's newest trust texture, from which
// kTrustUpdate scores the texel.
FrameGenDispatch BuildFrameGenDepthTrustDispatch(FrameGenExtent view_extent, const FrameGenViewGeometry& own,
                                                 const FrameGenViewGeometry& other);

enum class FrameGenDepthReading : std::uint8_t {
    kUnknown,     // not checked yet: depth is not used
    kAsSubmitted, // the game's description is right
    kReversed,    // near and far swapped (reversed depth described as standard, or the reverse)
    kUnusable,    // no reading lines the eyes up better than no depth
};

const char* ToString(FrameGenDepthReading reading);

// One reading's evidence from kDepthCheck, over the samples both eyes see
// where the reading moves content by more than a texel compared with no
// depth.
struct FrameGenDepthScore {
    double error{0.0};    // luma error, per sample "infinitely far" instead where that is clearly better
    double none{0.0};     // luma error of the same samples without depth
    double samples{0.0};
    double fallback{0.0}; // samples where the other eye disproved the reading
    // Below 1 where the reading lines the eyes up better than no depth.
    double Ratio() const { return none > 0.0 ? error / none : 1.0; }
};

struct FrameGenDepthEvidence {
    FrameGenDepthScore decoded; // as currently decoded
    FrameGenDepthScore swapped; // near and far swapped
    double binocular{0.0};      // samples both eyes see
    FrameGenDepthEvidence& operator+=(const FrameGenDepthEvidence& other);
};

// Decides how to read a game's depth from kDepthCheck evidence: whether to
// use it, and whether to swap its near and far.
class FrameGenDepthValidator {
  public:
    void Add(const FrameGenDepthEvidence& check);
    FrameGenDepthReading Reading() const { return reading_; }
    // Whether the depth pass should swap the game's near and far.
    bool Swapped() const { return swapped_; }
    // Changes whenever the decoding does: depth captured and checks
    // recorded with an older decoding are not used.
    std::uint32_t Generation() const { return generation_; }
    bool Usable() const { return reading_ == FrameGenDepthReading::kAsSubmitted || reading_ == FrameGenDepthReading::kReversed; }
    // Checks run every submission until a decision holds, then three in a
    // row every 20 submissions.
    bool WantsCheck(std::uint64_t submission) const;
    // Counts decisions that changed something, so callers can log each once.
    std::uint32_t Decisions() const { return decisions_; }
    // The evidence behind the last decision.
    const FrameGenDepthEvidence& Evidence() const { return evidence_; }

  private:
    FrameGenDepthEvidence sums_;
    std::uint32_t checks_{0};
    FrameGenDepthReading reading_{FrameGenDepthReading::kUnknown};
    bool swapped_{false};
    bool settled_{false};
    std::uint32_t generation_{0};
    std::uint32_t decisions_{0};
    FrameGenDepthEvidence evidence_;
};

struct FrameGenDepthStatus {
    FrameGenDepthReading reading{FrameGenDepthReading::kUnknown};
    std::uint32_t decisions{0};
    FrameGenDepthEvidence evidence;
};
// The kFlowPackRaw dispatch for a view of this extent (half resolution).
FrameGenDispatch BuildFrameGenRawPackDispatch(FrameGenExtent view_extent);

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
