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
    // Depth (Direct3D 12): drops the depth of regions the trust scores
    // disprove (kTrustUpdate scores, this decides).
    kTrustResolve,
    // Direct3D 12: the shading-rate image of the synthesis draw (coarse
    // shading off the view axis where no cockpit content is), recorded by
    // the backend itself.
    kShadingRate,
    // World motion (FrameGenViewRequest::world_motion): one iteration of a
    // robust fit of the world's rotation and translation to the measured
    // flow of world content, summed per group, then solved.
    kWorldReduce,
    kWorldSolve,
    // Depth (Direct3D 12): the trust scores (counted per tile by
    // kTrustUpdate) summed over the tiles around each, for kTrustResolve to
    // judge texels without evidence of their own by the surface around them.
    kTrustVote,
    // Depth (Direct3D 12): per tile, how well the other eye lines up at
    // scales of the depth (recorded with kDepthTrust), then the scale each
    // surface lines up best at, which kTrustResolve applies.
    kDepthScale,
    kDepthScaleVote,
};
inline constexpr std::size_t kFrameGenPassCount = 25;

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
    // The previous frame's linear depth (kDepth one submission on), for
    // content it did not see (FrameGenViewRequest::depth_previous).
    kDepthPrevious,
    // World motion: the per-group sums of kWorldReduce (8 RGBA32F texels per
    // group in one row) and the fit (2 RGBA32F texels: rotation and 1 when
    // valid; translation and the inlier share).
    kWorldPartials,
    kWorldParams,
    // Just-in-time interpolation (FrameGenTuning::cover): the newest frame's
    // flow filtered for extrapolation as well, for cover frames shown when
    // the next real frame is late. Kept per real frame like the history.
    kFilteredCover,
    // Depth trust per tile of kFrameGenTrustTile half-resolution texels:
    // disproved and confirmed counts with their mean inverse depths
    // (kTrustUpdate's second output), and the same summed over the tiles
    // around (kTrustVotes).
    kTrustTiles,
    kTrustVotes,
    // The stereo depth-scale probe per trust tile (kDepthScale, three texels
    // per tile side by side) and the vote on it (kDepthScaleVote).
    kDepthScales,
    kDepthScaleVotes,
    kDepthScaleRaw, // kDepthScaleVote's first stage, before the neighbours agree
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
// time (half float bits) the map is for.
inline constexpr std::uint32_t kFrameGenFlagShift = 32u;
// kLuma reads the previous frame's depth (input 3): content it did not see
// is marked like content outside its view.
inline constexpr std::uint32_t kFrameGenFlagDepthPrevious = 64u;
// kFilter: world content takes the fitted world motion (input 3).
inline constexpr std::uint32_t kFrameGenFlagWorld = 128u;
// Synthesis, interpolating with depth: content only one real frame sees is
// shown from that frame alone (FrameGenTuning::occlusion).
inline constexpr std::uint32_t kFrameGenFlagOcclusion = 256u;
// Synthesis, interpolating: untrusted motion fades between the two real
// frames (FrameGenTuning::cross_fade, research).
inline constexpr std::uint32_t kFrameGenFlagCrossFade = 512u;
// World motion: samples per side of the fit's grid, and its iterations.
inline constexpr std::uint32_t kFrameGenWorldGrid = 128u;
// The trust vote's tile side in half-resolution texels (FG_TRUST_TILE):
// kTrustUpdate counts a tile per thread group.
inline constexpr std::uint32_t kFrameGenTrustTile = 8u;
// kDepthScales texels per tile.
inline constexpr std::uint32_t kFrameGenDepthScaleTexels = 3u;
// kTrustResolve's options.x: this frame's stereo scale vote is in its t2.
inline constexpr std::uint32_t kFrameGenResolveScale = 1u;
// kDepthScaleVote's options.x: the second stage (the votes neighbours agree with).
inline constexpr std::uint32_t kFrameGenScaleAccept = 1u;
inline constexpr std::uint32_t kFrameGenWorldIterations = 3u;
// kTrustUpdate's options.x: this frame's stereo evidence is in the newest
// trust texture, last frame's scores are valid, the previous frame is.
inline constexpr std::uint32_t kFrameGenTrustStereo = 1u;
inline constexpr std::uint32_t kFrameGenTrustHistory = 2u;
inline constexpr std::uint32_t kFrameGenTrustMotion = 4u;
inline constexpr std::uint32_t kFrameGenThreadGroupSize = 8u;
static_assert(kFrameGenTrustTile == kFrameGenThreadGroupSize, "kTrustUpdate counts a tile per thread group");
inline constexpr std::uint8_t kFrameGenPyramidLevels = 5;
inline constexpr std::uint32_t kFrameGenSearchRadius = 6;
// Generated frames per real frame at most (a 4x multiplier).
inline constexpr std::size_t kFrameGenMaxGenerated = 4;
// Eye movement (meters) below which generated frames are not shifted: a
// fraction of a pixel even for content a hand's width away.
inline constexpr float kFrameGenMinimumShift = 5e-5f;
// A generated frame gets an eye-shift map of its own once its eye is this
// far (meters) from where the last map was built for: closer, it scales
// that map (a few pixels of near edge at most), as each map costs about a
// fifth of a millisecond per eye at headset resolution.
inline constexpr float kFrameGenShiftRebuild = 7.5e-4f;

struct FrameGenDispatch {
    FrameGenPass pass{FrameGenPass::kLuma};
    // Sampled textures t0-t5; only Direct3D 12 binds the fifth and sixth
    // (the eye-shift map, used with depth).
    std::array<FrameGenTexture, 6> inputs{};
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

// The trust vote's tiles over a half-resolution extent.
inline FrameGenExtent FrameGenTrustTiles(FrameGenExtent half) {
    return {(half.width + kFrameGenTrustTile - 1) / kFrameGenTrustTile,
            (half.height + kFrameGenTrustTile - 1) / kFrameGenTrustTile};
}

// Quad views: each eye's image is composed from a peripheral view and a
// focus view inset where its field of view lands, as the layer's quad views
// compositor composes them.
struct FrameGenFov {
    float left{0.0f}; // radians, XrFovf order
    float right{0.0f};
    float up{0.0f};
    float down{0.0f};
};

struct FrameGenComposeEye {
    std::uint32_t peripheral{0};                  // analyzed view indices
    std::uint32_t focus{0};
    std::array<float, 4> focus_rect{};            // left, top, right, bottom in output uv
    std::array<float, 2> feather{};               // the blend band in output uv
    float sharpen{0.0f};                          // 0-1
    std::array<float, 2> focus_texel{};           // 1 / the focus view's extent
    std::array<float, 2> output_pixel_in_focus{}; // one output pixel in focus uv
};

struct FrameGenComposition {
    FrameGenExtent output;                // each eye's composed image
    std::vector<FrameGenComposeEye> eyes; // one per output image, in output order
};

// Exactly the Direct3D 11 compositor's focus rectangle (bit for bit:
// computed in double, rounded to float where it rounds) from the full and
// focus fields of view, the composed image's and the focus view's extents.
// The view indices are left 0 for the caller.
FrameGenComposeEye BuildFrameGenComposeEye(const FrameGenFov& full, const FrameGenFov& focus, FrameGenExtent output,
                                           FrameGenExtent focus_extent, double transition_percent,
                                           double sharpness_percent);
// The compose draw's constants: fov_current = focus_rect; fov_previous =
// {feather, sharpen, 0}; rot0 = {focus_texel, output_pixel_in_focus};
// size = {output.width, output.height, 0, 0}; options = 0.
FrameGenConstants BuildFrameGenComposeConstants(const FrameGenComposeEye& eye, FrameGenExtent output);

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
    // Set by backends: the previous frame's depth was used and kept
    // (kDepthPrevious), so content it did not see can be told apart.
    bool depth_previous{false};
    // Set by backends: the optical flow hardware already matched this frame
    // and the previous one as rendered (kFlowPackRaw at capture), so the plan
    // packs nothing and kFlowUnpack takes the head movement out.
    bool hardware_raw{false};
    // Set by backends (with depth): world content (beyond 30 m) moves with
    // one rotation and translation fitted to its measured flow, instead of
    // the flow itself (kWorldReduce, kWorldSolve).
    bool world_motion{false};
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
    std::array<float, kFrameGenMaxGenerated> fractions{0.5f, 0.5f, 0.5f, 0.5f};
    std::uint8_t generated{1};
    // The newest real frame pair's interval over the pair before's: last
    // pair's motion times this is what steady motion moves now.
    float interval_ratio{1.0f};
    // Motion measured by the GPU's optical flow hardware instead of block
    // matching (kFlowPack, the hardware, kFlowUnpack), at
    // kFrameGenHardwareFlowLevel whatever the quality.
    bool hardware_flow{false};
    // The last generated frame is a spare, shown only when the next real
    // frame is late: it reuses the frame before's eye-shift map.
    bool spare{false};
    // Just-in-time synthesis (FrameGenGpu::Synthesize): this submission only
    // measures and filters motion (`generated` is ignored); each generated
    // frame is written later, one at a time, right before its refresh.
    bool analyze_only{false};
    // With analyze_only, interpolating: also filter the flow for
    // extrapolation (kFilteredCover), so a refresh the next real frame is
    // late for shows motion past the newest frame instead of repeating it.
    bool cover{false};
    // Interpolating with depth: at a motion boundary where no vector has
    // both real frames agree, content only one of them sees (newly revealed
    // or being covered) is shown from that one alone, ordered by depth.
    bool occlusion{true};
    // Research: where interpolated motion is not trusted, fade between the
    // two real frames instead of showing the newest one.
    bool cross_fade{false};
    // Quad views: how each eye is composed from its views, for the flow
    // merge and the focus views' motion limit; null on the canvas path. Not
    // owned: the caller keeps it alive for the Execute call.
    const FrameGenComposition* composition{nullptr};
};

struct FrameGenViewPlan {
    FrameGenExtent extent;
    std::array<FrameGenExtent, kFrameGenPyramidLevels + 1> levels{}; // [0] = full resolution
    std::uint8_t final_flow_level{3};
    bool synthesize{false};
    // Each generated frame has its own eye-shift map, built into the one
    // map texture right before it: the frames are written one at a time in
    // plan order, not drawn together at the end.
    bool frames_in_order{false};
    std::vector<FrameGenDispatch> dispatches;
};

// Slots of per-real-frame textures (history, filtered flow, depth, trust)
// at most (see FrameGenHistoryRing).
inline constexpr std::uint8_t kFrameGenHistoryRingMax = 5;

// The slots of a view's ring. Two let the newest real frame be generated
// from while the next is captured. Just in time, frames are written from a
// real frame (moved, from its previous images too) while newer ones are
// captured, a slot each: a ring of n moves a real frame until n - 1 newer
// ones were captured (shows it until n). A canvas keeps three (its images
// are large); quad views (FrameGenTuning::composition) keep five, as their
// lead at the game's own rate spans a few real frames, and theirs are
// smaller.
constexpr std::uint8_t FrameGenHistoryRing(bool composed) { return composed ? 5 : 3; }
// The ring a submission with `tuning` uses (two unless just in time).
inline std::uint8_t FrameGenHistoryRing(const FrameGenTuning& tuning) {
    return tuning.analyze_only ? FrameGenHistoryRing(tuning.composition != nullptr) : 2;
}
static_assert(FrameGenHistoryRing(true) <= kFrameGenHistoryRingMax &&
              FrameGenHistoryRing(false) <= kFrameGenHistoryRingMax);

// Per-view ring bookkeeping shared by every backend. Slots hold the history
// color copy and the filtered flow that seeds the next search; Newest() is
// the slot the next capture writes and the next Execute generates from,
// Previous() the one the Execute before used.
class FrameGenHistoryState {
  public:
    enum Change : unsigned {
        kNone = 0,
        kAll = 1,      // extent, source format, output kind or ring size changed: recreate every texture
        kFiltered = 2, // flow quality changed: recreate the filtered flow pair
    };

    unsigned Configure(FrameGenExtent extent, std::int64_t source_format, FrameGenOutputKind output,
                       std::uint8_t final_level, std::uint8_t ring = 2);
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
    std::uint8_t Previous() const { return static_cast<std::uint8_t>((newest_ + ring_ - 1u) % ring_); }
    std::uint8_t Ring() const { return ring_; }
    FrameGenExtent Extent() const { return extent_; }

  private:
    FrameGenExtent extent_{};
    std::int64_t source_format_{0};
    FrameGenOutputKind output_{FrameGenOutputKind::kSrgb8};
    std::uint8_t final_level_{0};
    std::uint8_t ring_{2};
    std::uint8_t newest_{0};
    std::array<bool, kFrameGenHistoryRingMax> history_valid_{};
    std::array<bool, kFrameGenHistoryRingMax> filtered_valid_{};
    std::array<bool, kFrameGenHistoryRingMax> trust_valid_{};
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
    // Decoded depth is multiplied by this (FrameGenDepthValidator::Scale):
    // games describe their near plane wrongly too.
    float scale{1.0f};
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
inline constexpr std::uint32_t kFrameGenDepthCheckTexels = 5;  // per cell, side by side
// Scale probes per check: the depth scaled by exp(step * (j - 2)).
inline constexpr std::size_t kFrameGenDepthScaleProbes = 5;
// `scale` is the scale the checked depth was decoded with, `probe_step`
// the probes' step (natural log).
FrameGenDispatch BuildFrameGenDepthCheckDispatch(const FrameGenViewGeometry& view0, const FrameGenViewGeometry& view1,
                                                 const FrameGenDepthEncoding& encoding, float scale = 1.0f,
                                                 float probe_step = 0.08f);

// The kDepthTrust dispatch for one view of a stereo pair: compares each
// texel of `own`'s linear depth with what `other` sees and writes the
// evidence (positive confirms the depth, negative says the content is
// infinitely far) into the view's newest trust texture, from which
// kTrustUpdate scores the texel.
FrameGenDispatch BuildFrameGenDepthTrustDispatch(FrameGenExtent view_extent, const FrameGenViewGeometry& own,
                                                 const FrameGenViewGeometry& other);
// The kDepthScale dispatch for the same view pair, recorded right after
// kDepthTrust: per trust tile, the other eye's error at each probed scale
// of the depth (see FG_PASS_DEPTH_SCALE).
FrameGenDispatch BuildFrameGenDepthScaleDispatch(FrameGenExtent view_extent, const FrameGenViewGeometry& own,
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
    // The other eye's error with the depth scaled by each probe's factor,
    // summed over `probed` samples (cockpit-near, moved and lined up better
    // than without depth).
    std::array<double, kFrameGenDepthScaleProbes> probes{};
    double probed{0.0};
    double cockpit{0.0};        // ... whose depth is nearer than 10 m (none: empty depth)
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
    // What decoded depth is multiplied by: found by searching the scale that
    // lines the eyes up best (the check's probes), 1 until then.
    float Scale() const { return scale_; }
    // The probes' step (natural log): wide while searching, narrowing as
    // the scale settles.
    float ProbeStep() const { return probe_step_; }
    // Changes whenever the decoding does (Swapped, Scale): depth captured
    // with an older decoding is not used.
    std::uint32_t Generation() const { return generation_; }
    // Changes with the decoding and with the probes' step: checks recorded
    // with either older are not added.
    std::uint32_t CheckGeneration() const { return check_generation_; }
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
    float scale_{1.0f};
    float probe_step_{0.08f};
    bool settled_{false};
    std::uint32_t generation_{0};
    std::uint32_t check_generation_{0};
    std::uint32_t decisions_{0};
    FrameGenDepthEvidence evidence_;
};

struct FrameGenDepthStatus {
    FrameGenDepthReading reading{FrameGenDepthReading::kUnknown};
    std::uint32_t decisions{0};
    FrameGenDepthEvidence evidence;
    float scale{1.0f};
};
// The kFlowPackRaw dispatch for a view of this extent (half resolution).
FrameGenDispatch BuildFrameGenRawPackDispatch(FrameGenExtent view_extent);

// Luma level a flow level matches against: two levels finer, at most half resolution.
std::uint8_t FrameGenMatchLevel(std::uint8_t flow_level);
std::array<FrameGenExtent, kFrameGenPyramidLevels + 1> FrameGenPyramid(FrameGenExtent extent);
FrameGenViewPlan BuildFrameGenViewPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning);

// Just-in-time synthesis: one generated frame of a view from a real frame
// an analyze_only plan measured (`request` as that plan was built from, its
// shift the eye's movement per unit forward in time, as when extrapolating),
// at `fraction` (FrameGenTuning::fractions) in `mode`: its eye-shift map
// when the eye moves, then the frame into output 0. Extrapolating from a
// frame analyzed for interpolation reads the cover flow (kFilteredCover). A
// frame at the newest frame's own time, or without motion measured, is the
// newest frame unchanged.
FrameGenViewPlan BuildFrameGenSynthesisPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning,
                                            FrameGenSynthesisMode mode, float fraction, bool cover_flow);

// Interpolation fraction or extrapolation distance for a generated frame
// displayed at target_time, given the two real frames' display times. Clamped
// so a hitch never pushes content past the real frames' motion.
float FrameGenFraction(FrameGenSynthesisMode mode, std::int64_t previous_time, std::int64_t newest_time,
                       std::int64_t target_time);

// Row-major rotation taking newest-view directions into previous-view space.
std::array<std::array<float, 3>, 3> FrameGenRelativeRotation(const std::array<float, 4>& newest,
                                                             const std::array<float, 4>& previous);

} // namespace depthxr
