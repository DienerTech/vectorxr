#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
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
    // Depth (Direct3D 12): for a frame moved by the previous real frame's
    // motion (FrameGenSynthesisPlan's stale flow), the previous frame's
    // resolved depth on this frame's grid, for its eye-shift map.
    kDepthStale,
    // Depth (Direct3D 12) for a game that fills only one eye's
    // (FrameGenDepthEyes): the other eye's linear depth moved to the empty
    // eye's view, nearest content winning (options.x 0 clears the keys, 1
    // splats), then resolved into the empty eye's linear depth.
    kDepthCrossSplat,
    kDepthCrossResolve,
    // Engine motion vectors, stage 0 (Direct3D 12, FrameGenGpu::CaptureMotion):
    // a game's motion vector images fingerprinted and sampled on its own
    // queue (FrameGenEngineCensus), whether or not it ever acquires them.
    // Nothing generated frames are made from reads what it writes.
    kEngineCensus,
};
inline constexpr std::size_t kFrameGenPassCount = 29;

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
    // kDepthStale's output: the previous frame's resolved depth read where
    // each half-resolution texel's content was in that frame.
    kDepthStale,
    // FrameGenTuning::depth_splat: the motion map at half resolution, built
    // like the eye-shift map (into kShiftKeys, once that map is built) for
    // each generated frame: offsets back to the newest frame.
    kMotionMap,
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
// Synthesis, extrapolating: the previous real frame's flow, read where the
// content was in that frame (BuildFrameGenSynthesisPlan's `stale_flow`).
inline constexpr std::uint32_t kFrameGenFlagStaleFlow = 1024u;
// Synthesis with depth: candidate vectors are also chosen by where they were
// measured against the depth of what they carry (FrameGenTuning::depth_match).
inline constexpr std::uint32_t kFrameGenFlagDepthMatch = 2048u;
// The kShift* passes build the motion map (kMotionMap; tuning[1]: the
// synthesis step), and synthesis reads it (input 5): FrameGenTuning::depth_splat.
inline constexpr std::uint32_t kFrameGenFlagDepthSplat = 4096u;
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

// The quad views diagnostic overlay over one composed eye, drawn last, as the
// layer's Direct3D 11 compositor draws it (QuadViewsD3D11ShaderSource): the
// periphery, blend band and inset tinted, the inset's outline and the band's
// inner edge, and markers for the head's centre, the configured offset and the
// gaze. Drawn into the composed image only, never into a view whose motion is
// measured. Positions are in output uv.
struct FrameGenComposeOverlay {
    bool shown{false};
    bool eye_tracking{false}; // the inset follows the eyes: its outline turns red without a gaze
    bool gaze{false};         // eye tracking had a gaze for the frame: its markers are drawn
    std::array<float, 4> gaze_markers{};      // the raw gaze (xy) and the smoothed gaze (zw)
    std::array<float, 4> reference_markers{}; // the head's centre (xy) and the configured offset (zw)
};

struct FrameGenComposeEye {
    std::uint32_t peripheral{0};                  // analyzed view indices
    std::uint32_t focus{0};
    std::array<float, 4> focus_rect{};            // left, top, right, bottom in output uv
    std::array<float, 2> feather{};               // the blend band in output uv
    float sharpen{0.0f};                          // 0-1
    std::array<float, 2> focus_texel{};           // 1 / the focus view's extent
    std::array<float, 2> output_pixel_in_focus{}; // one output pixel in focus uv
    FrameGenComposeOverlay overlay;               // drawn over the eye when shown
};

// The compose draw's options.x with the overlay shown (FrameGenComposeOverlay).
inline constexpr std::uint32_t kFrameGenComposeOverlay = 1u;
inline constexpr std::uint32_t kFrameGenComposeOverlayEyeTracking = 2u;
inline constexpr std::uint32_t kFrameGenComposeOverlayGaze = 4u;

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
// size = {output.width, output.height, 0, 0}; options = 0. With the overlay
// shown: options.x its kFrameGenComposeOverlay flags, rot1 its gaze markers,
// rot2 its reference markers, tuning.xy one output pixel in output uv (as
// the Direct3D 11 compositor computes it).
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
    // Engine motion vectors (the presenter's frame gates): this frame's
    // vectors and the frame before's pair, Pivot's change to the views held
    // still between them, and neither frame came with a camera cut, a
    // depth encoding change, a pause or a new swapchain. Only such frames
    // may calibrate the vectors' convention.
    bool engine_calibrate{false};
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
    // With depth, at a motion boundary: a candidate vector measured on
    // another surface than the content it would carry costs more (depth
    // engine mk2, M4.2; research, off by default).
    bool depth_match{false};
    // With depth: content with a motion model (head-fixed nearer than 4 m,
    // world from 30 m) is moved to each generated frame's time with a depth
    // test, and shown where its surface and the chosen vector's disagree
    // (depth engine mk2, M3; research, off by default).
    bool depth_splat{false};
    // Quad views: how each eye is composed from its views, for the flow
    // merge and the focus views' motion limit; null on the canvas path. Not
    // owned: the caller keeps it alive for the Execute call.
    const FrameGenComposition* composition{nullptr};
    // With analyze_only: the real frames each view's ring keeps
    // (FrameGenHistoryRing); 0 for the default of the path.
    std::uint8_t history_ring{0};
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
// ones were captured (shows it until n). Quad views
// (FrameGenTuning::composition) keep five, as their lead at the game's own
// rate spans a few real frames, and theirs are smaller. A canvas keeps three
// by default (its images are large); the presenter gives it five where video
// memory allows (FrameGenTuning::history_ring): a game at 30 fps keeps a real
// frame in use for 80-130 ms after its capture (Interpolate, and Extrapolate
// at its slowest), three captures and more (MSFS 2024, 2026-10-09: up to
// 308 refreshes in 30 s found the frame they needed replaced).
constexpr std::uint8_t FrameGenHistoryRing(bool composed) { return composed ? 5 : 3; }
inline constexpr std::uint8_t kFrameGenCanvasRingRoomy = 5;
// The ring a submission with `tuning` uses (two unless just in time).
inline std::uint8_t FrameGenHistoryRing(const FrameGenTuning& tuning) {
    if (!tuning.analyze_only) return 2;
    if (tuning.history_ring) {
        return tuning.history_ring < 2 ? std::uint8_t{2}
               : tuning.history_ring > kFrameGenHistoryRingMax ? kFrameGenHistoryRingMax
                                                               : tuning.history_ring;
    }
    return FrameGenHistoryRing(tuning.composition != nullptr);
}
static_assert(FrameGenHistoryRing(true) <= kFrameGenHistoryRingMax &&
              FrameGenHistoryRing(false) <= kFrameGenHistoryRingMax &&
              kFrameGenCanvasRingRoomy <= kFrameGenHistoryRingMax);

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

// Whether two depth descriptions are the same (minDepth, maxDepth, nearZ and
// farZ; the scale is not the game's).
inline bool SameFrameGenDepthDescription(const FrameGenDepthEncoding& a, const FrameGenDepthEncoding& b) {
    return a.min_depth == b.min_depth && a.max_depth == b.max_depth && a.near_z == b.near_z && a.far_z == b.far_z;
}

// Linear depth at or beyond this many meters is infinitely far (FG_FAR_DEPTH).
inline constexpr float kFrameGenFarDepth = 100000.0f;

// How a game's motion vector image holds its vectors (XR_FB_space_warp,
// XR_EXT_frame_synthesis), whatever the API: a texel's rgb as it is (FB), or
// rgb * scale + offset (EXT; `two_d`: rg alone, USE_2D_MOTION_VECTOR).
struct FrameGenMotionEncoding {
    bool ext{false};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::array<float, 3> offset{0.0f, 0.0f, 0.0f};
    bool two_d{false};
};

// A texel's vector (xy) as `encoding` decodes it, from the image's raw rgba.
std::array<float, 2> DecodeFrameGenMotion(const FrameGenMotionEncoding& encoding, const std::array<float, 4>& raw);

// How far Frame Generation goes with a game's motion vectors
// (VECTORXR_FRAMEGEN_ENGINE_MV), each step after the census doing what the
// one before does. Stage 0 has the census alone: the images are fingerprinted
// and sampled, and generated frames are made as without them.
enum class FrameGenEngineMode : std::uint8_t {
    kOff,
    kCensus,
    kCapture,
    kObserve,
    kAssist,
    kLead,
};

const char* ToString(FrameGenEngineMode mode); // "off", "census", ...
// "off" or "census" (any case); anything else is off: the later modes do not
// exist yet.
FrameGenEngineMode ParseFrameGenEngineMode(const char* value);

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
    // Check cells (of kFrameGenDepthCheckCells squared) where the checked
    // eye's depth, and the other eye's, differs from its depth at the view's
    // centre: none where an eye's depth is uniform (a game that leaves it
    // clear), whatever the decoding.
    double source_varied{0.0};
    double other_varied{0.0};
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
    // Counts the evidence batches judged (three checks each), changed or not.
    std::uint32_t Batches() const { return batches_; }
    // The evidence behind the last decision.
    const FrameGenDepthEvidence& Evidence() const { return evidence_; }
    // Forgets every reading, as for a new game; the generations keep
    // counting, so depth decoded or checked before is never taken as new.
    void Restart();

  private:
    std::uint32_t batches_{0};
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

// Which eye's depth the check reads, and whether an eye takes the other's.
// Star Citizen describes depth for both eyes but fills only the right eye's
// (the left stays clear): its left depth is uniform while the right eye's
// varies. Six checks in a row that find one eye's depth uniform and the
// other eye's not decide it for the session, whichever eye is empty: the
// check then reads the eye with depth, and the empty eye takes that eye's
// depth moved to its view (kDepthCrossSplat/kDepthCrossResolve).
class FrameGenDepthEyes {
  public:
    // A check's evidence (one check, not a batch), made from view `source`'s
    // depth. True when it decided the source eye is empty: the depth
    // validator judged that eye's depth until now and must start over (an
    // empty other eye changes nothing the validator judged).
    bool Add(std::size_t source, const FrameGenDepthEvidence& check);
    std::size_t Source() const { return source_; }
    // The eye that takes the other's depth, once decided.
    std::optional<std::size_t> Empty() const { return empty_; }

  private:
    std::size_t source_{0};
    std::optional<std::size_t> empty_;
    std::uint32_t source_streak_{0}; // checks in a row finding the checked eye's depth clear
    std::uint32_t other_streak_{0};  // ... the other eye's
};

struct FrameGenDepthStatus {
    FrameGenDepthReading reading{FrameGenDepthReading::kUnknown};
    std::uint32_t decisions{0};
    FrameGenDepthEvidence evidence;
    float scale{1.0f};
    // For the depth check's Debug line (one per batch).
    std::uint32_t batches{0};
    bool swapped{false};
    float probe_step{0.0f};
    // FrameGenDepthEyes: the eye given the other eye's depth, -1 for none.
    int empty_view{-1};
};

// The kDepthCrossSplat (mode 0: clear the keys on the target eye's grid; 1:
// splat the source eye's depth) or kDepthCrossResolve dispatch moving one
// eye's half-resolution linear depth to the other eye's view.
FrameGenDispatch BuildFrameGenDepthCrossDispatch(FrameGenPass pass, std::uint32_t mode, FrameGenExtent source_extent,
                                                 FrameGenExtent target_extent, const FrameGenViewGeometry& source,
                                                 const FrameGenViewGeometry& target);
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
// newest frame unchanged. `stale_flow` (extrapolating): the frame's own
// motion is not measured yet, so the previous real frame's flow moves it
// (kFilteredPrevious, read where the content was in that frame); nothing
// of its own analysis is read (no depth, so no eye shift: one refresh's head
// movement is a fraction of a millimetre).
FrameGenViewPlan BuildFrameGenSynthesisPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning,
                                            FrameGenSynthesisMode mode, float fraction, bool cover_flow,
                                            bool stale_flow = false);

// Interpolation fraction or extrapolation distance for a generated frame
// displayed at target_time, given the two real frames' display times. Clamped
// so a hitch never pushes content past the real frames' motion.
float FrameGenFraction(FrameGenSynthesisMode mode, std::int64_t previous_time, std::int64_t newest_time,
                       std::int64_t target_time);

// Row-major rotation taking newest-view directions into previous-view space.
std::array<std::array<float, 3>, 3> FrameGenRelativeRotation(const std::array<float, 4>& newest,
                                                             const std::array<float, 4>& previous);

// The shaders' mapping between two views (MapToPrevious and MapToNewest in
// framegen.hlsl) on the CPU: both views' tangents and the relative pose the
// plan's constants carry (with depth, the newest view's position in the
// previous view's space too).
struct FrameGenViewMapping {
    std::array<float, 4> fov_current{};
    std::array<float, 4> fov_previous{};
    std::array<std::array<float, 3>, 3> rotation{}; // newest-view direction -> previous-view direction
    std::array<float, 3> translation{};
    bool depth{false};
};
FrameGenViewMapping BuildFrameGenViewMapping(const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous,
                                             bool depth);
// Where newest-view `uv` (content at linear depth `depth`, rotation only
// without it) lies in the previous view; false behind it or outside its
// image (`previous_uv` is still set outside it).
bool FrameGenMapToPrevious(const FrameGenViewMapping& mapping, const std::array<float, 2>& uv, float depth,
                           std::array<float, 2>& previous_uv);
// Its inverse, for content at newest-view depth `depth`; false behind the
// newest view.
bool FrameGenMapToNewest(const FrameGenViewMapping& mapping, const std::array<float, 2>& previous_uv, float depth,
                         std::array<float, 2>& newest_uv);

// --- Engine motion vectors: which image a frame's vectors are in -----------

// Images of a motion vector swapchain the census reads (runtimes make three).
inline constexpr std::uint32_t kFrameGenEngineImages = 3;

// One frame's census of a view's motion vector images (kEngineCensus), as
// the image tracker reads it.
struct FrameGenEngineTrackerFrame {
    std::uint64_t frame{0}; // frames that carried a projection layer, counted by the layer
    // The head moved more than 0.3 degrees or 2 mm since the frame before, or
    // the application's space more than 5 cm (appSpaceDeltaPose): its vectors
    // must differ from the frame before's. Other frames show nothing.
    bool informative{false};
    std::uint64_t handle{0};             // the motion vector swapchain
    std::array<std::int32_t, 4> rect{}; // its region: x, y, width, height
    std::uint32_t images{0};             // up to kFrameGenEngineImages
    std::array<std::uint64_t, kFrameGenEngineImages> hashes{};
    // The images the frame's color and depth swapchains released for it.
    std::uint32_t color_index{0};
    std::uint32_t depth_index{0};
    // A game following the specification released a motion vector image for
    // the frame: that one.
    std::optional<std::uint32_t> released;
};

// Which of a game's motion vector images a frame's vectors are in. MSFS 2024
// never acquires, waits on or releases them, so the census says: per
// informative frame, the images whose hash changed since their own last
// census. Frames where exactly one changed score every policy a game could
// follow over a window of 120 informative frames; one explaining 95% of 60
// such frames or more, with no policy that predicts otherwise above 60%, is
// locked. Five mispredictions while locked (in the window), another
// swapchain or another region unlock it. More than 90% of 300 informative
// frames changing nothing: the images are not written.
class FrameGenEngineImageTracker {
  public:
    enum class Policy : std::uint8_t {
        kNone,          // nothing known yet
        kReleased,      // the image the swapchain released (the specification's way)
        kConstant,      // always image `parameter`
        kCycle,         // (frame + parameter) % images
        kColor,         // the image index the color swapchain released
        kDepth,         // ... the depth swapchain released
        kColorNext,     // the color index + 1
        kColorPrevious, // the color index - 1
        kAll,           // every image is written alike: any of them
        kUnwritten,     // none is
    };
    struct Hypothesis {
        Policy policy{Policy::kNone};
        std::uint32_t parameter{0};
        double share{0.0}; // of the single-change frames in the window it explains
    };

    void Add(const FrameGenEngineTrackerFrame& frame);
    // The image a frame's vectors are in: the locked policy's, else the best
    // guess (the color index while nothing explains more); none while the
    // images are not written.
    std::optional<std::uint32_t> Image(std::uint64_t frame, std::uint32_t color_index, std::uint32_t depth_index,
                                       std::optional<std::uint32_t> released) const;
    bool Locked() const { return locked_; }
    bool NotWritten() const;
    // The policy explaining the most single-change frames in the window, and
    // the best of those predicting otherwise on at least one of them (equal
    // predictions are one policy: a color swapchain cycling with the frames
    // follows both).
    Hypothesis Best() const;
    Hypothesis RunnerUp() const;
    // In the window: informative frames, and those where one image changed,
    // every image alike, more than one (not alike) or none.
    std::uint32_t WindowFrames() const { return static_cast<std::uint32_t>(window_.size()); }
    std::uint32_t SingleChanges() const;
    std::uint32_t AllChanged() const;
    std::uint32_t SeveralChanged() const;
    std::uint32_t NoneChanged() const;
    // Session totals.
    std::uint64_t Mispredictions() const { return mispredictions_; }
    std::uint64_t Locks() const { return locks_; }
    std::uint64_t Unlocks() const { return unlocks_; }

  private:
    enum class Change : std::uint8_t { kNone, kSingle, kAll, kSeveral };
    struct Entry {
        Change change{Change::kNone};
        std::uint32_t image{0}; // kSingle: the image that changed
        std::uint64_t frame{0};
        std::uint32_t color_index{0};
        std::uint32_t depth_index{0};
        bool mispredicted{false}; // locked, and the locked policy said otherwise
    };
    std::uint32_t Predict(const Hypothesis& hypothesis, const Entry& entry) const;
    // Every policy the images could follow, scored over the window.
    std::vector<Hypothesis> Score() const;
    void Restart();

    std::uint64_t handle_{0};
    std::array<std::int32_t, 4> rect_{};
    std::uint32_t images_{0};
    std::array<std::optional<std::uint64_t>, kFrameGenEngineImages> hashes_{};
    std::deque<Entry> window_;            // the last 120 informative frames
    std::deque<std::uint8_t> written_;    // the last 300 informative frames: 1 where an image changed
    std::uint32_t released_streak_{0};    // frames in a row the swapchain released an image
    bool locked_{false};
    Hypothesis lock_{};
    std::uint64_t mispredictions_{0};
    std::uint64_t locks_{0};
    std::uint64_t unlocks_{0};
};

const char* ToString(FrameGenEngineImageTracker::Policy policy);

// --- Engine motion vectors: calibration (stage 0 research) -----------------
//
// How a game's decoded vectors e relate to what the poses predict, in D3D
// NDC (x right, y up), the current frame minus the previous one: n = g * e
// per axis. Used by the census's online fit and by
// `depthxr_framegen_tests engine-calibrate`.

// What the vectors leave of the movement (FrameGenEngineConvention's camera).
enum class FrameGenEngineCamera : std::uint8_t {
    kIncluded,     // the head's movement and the application's (the specification's intent)
    kRotationOnly, // the head's rotation alone
    kExcluded,     // the application's movement only: head movement removed
    kObjectOnly,   // objects' own movement only
};
const char* ToString(FrameGenEngineCamera camera);

// e on one axis fitted as a * p_r + b * p_t + c: p_r the prediction from the
// head's rotation alone (depth 0), p_t what its translation adds at the
// content's depth; or as a line, e = a * (p_r + p_t) + c.
class FrameGenEngineAxisFit {
  public:
    struct Solution {
        bool valid{false};
        double a{0.0};
        double b{0.0};
        double c{0.0};
        double corr2{0.0}; // the share of e's variance the fit explains
        double samples{0.0};
    };
    void Add(double e, double rotation, double translation);
    FrameGenEngineAxisFit& operator+=(const FrameGenEngineAxisFit& other);
    Solution Solve() const;
    Solution SolveLine() const;
    // The mean of e - a * p_r - b * p_t: these samples' intercept under
    // another fit's a and b (their mean e with both 0).
    double Intercept(double a, double b) const { return n_ > 0.0 ? (e_ - a * r_ - b * t_) / n_ : 0.0; }
    double Samples() const { return n_; }
    // Sum of the squared rotation part: the regression's leverage.
    double RotationSpread() const { return rr_ - (n_ > 0.0 ? r_ * r_ / n_ : 0.0); }

  private:
    double n_{0.0}, r_{0.0}, t_{0.0}, e_{0.0};
    double rr_{0.0}, tt_{0.0}, rt_{0.0}, er_{0.0}, et_{0.0}, ee_{0.0};
};

// A fit whose a gives the vectors' gain (1 / a): one explaining at least
// this share of the vectors' variance. With the head still a fit only
// follows tracking noise.
inline constexpr double kFrameGenEngineGainCorr2 = 0.5;
inline bool GivesFrameGenEngineGain(const FrameGenEngineAxisFit::Solution& fit) {
    return fit.valid && fit.a != 0.0 && fit.corr2 >= kFrameGenEngineGainCorr2;
}

// A convention's gain (n = g * e) with its name.
struct FrameGenEngineGain {
    const char* name{""};
    std::array<double, 2> gain{1.0, 1.0};
};
// The canonical gains for vectors of a region `vectors` in size and a color
// view of `color`: NDC with y up (XR_FB_space_warp's text), Vulkan NDC (y
// down, XR_EXT_frame_synthesis), uv (y down), the vectors' pixels and the
// color view's pixels (y down), and each as previous minus current.
std::vector<FrameGenEngineGain> FrameGenEngineCanonicalGains(FrameGenExtent vectors, FrameGenExtent color);
// The canonical gain nearest `gain`, and the larger of its axes' relative
// errors.
FrameGenEngineGain NearestFrameGenEngineGain(const std::array<double, 2>& gain, FrameGenExtent vectors,
                                             FrameGenExtent color, double& error);

// Where content at newest-view `uv` (linear depth `depth`: 0 or less, or
// kFrameGenFarDepth and beyond, infinitely far) was in the previous frame,
// as NDC current minus previous, for vectors leaving `camera` of the
// movement. `newest` and `previous` are the poses the game rendered with;
// content nearer than kFrameGenEngineCockpit stays in the application's
// space, content from kFrameGenEngineWorld on (and infinitely far) moved
// with it by appSpaceDeltaPose (`delta_*`; `delta_sign` -1 takes its
// rotation conjugated). False where the content was behind the view, or is
// between the two (no model).
inline constexpr float kFrameGenEngineCockpit = 4.0f;
inline constexpr float kFrameGenEngineWorld = 30.0f;
bool FrameGenEnginePrediction(const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous,
                              const std::array<float, 4>& delta_orientation, const std::array<float, 3>& delta_position,
                              int delta_sign, FrameGenEngineCamera camera, const std::array<float, 2>& uv, float depth,
                              std::array<double, 2>& prediction);

// One sample of a view's vectors: where, its content's linear depth, and
// its vector as decoded.
struct FrameGenEngineCalibrationSample {
    std::array<float, 2> uv{};
    float depth{0.0f};
    std::array<float, 2> vector{};
};

// A frame's samples, with the poses the game rendered it and the frame
// before with (and the one before that, for the lag test).
struct FrameGenEngineCalibrationFrame {
    FrameGenViewGeometry newest;
    FrameGenViewGeometry previous;
    std::optional<FrameGenViewGeometry> before_previous;
    std::array<float, 4> delta_orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> delta_position{};
    FrameGenExtent vectors; // the vectors' region (pixels as units)
    FrameGenExtent color;   // the color view
    std::vector<FrameGenEngineCalibrationSample> samples;
};

// A camera model's fit over every sample with a model (cockpit and world).
struct FrameGenEngineCandidate {
    FrameGenEngineCamera camera{FrameGenEngineCamera::kIncluded};
    int delta_sign{1};
    std::array<FrameGenEngineAxisFit::Solution, 2> fit{}; // e = a * prediction + c per axis
    std::array<double, 2> gain{0.0, 0.0};                  // 1 / a
    FrameGenEngineGain nearest;
    double gain_error{0.0};  // relative, the larger axis's
    double residual_px{0.0}; // median |g * (e - c) - prediction| in vector pixels
    double Corr2() const { return 0.5 * (fit[0].corr2 + fit[1].corr2); }
};

struct FrameGenEngineCalibration {
    // Every candidate, the best fit first.
    std::vector<FrameGenEngineCandidate> leaderboard;
    // Cockpit samples (nearer than kFrameGenEngineCockpit) with the head
    // included: e = a * p_r + b * p_t + c per axis (b / a about 1 when Frame
    // Generation's depth agrees with the game's), with the newest pair's
    // poses and with the pair before's (the lag test).
    std::array<FrameGenEngineAxisFit::Solution, 2> cockpit{};
    std::array<FrameGenEngineAxisFit::Solution, 2> cockpit_lag{};
    // Jitter: the spread across frames of each frame's intercept under the
    // cockpit fit, in vector pixels (the intercept over a, in NDC; none
    // without the fit), and of the frames' mean cockpit vectors, in the
    // vectors' own units (the jitter where the head was too still to fit).
    std::optional<std::array<double, 2>> intercept_spread_px;
    std::array<double, 2> intercept_spread{0.0, 0.0};
    std::uint32_t frames{0};
};
FrameGenEngineCalibration CalibrateFrameGenEngine(const std::vector<FrameGenEngineCalibrationFrame>& frames);

// --- Engine motion vectors: the frame gates (stage 1) ----------------------
//
// Whatever a frame's vectors show, they cannot be used for it where a gate
// says so (G1-G9, bits; 0: none does). The presenter checks them in Generate
// from the frame's record, the record of the frame generated from before it,
// and what it knows of the frame.
inline constexpr std::uint32_t kFrameGenEngineNoStruct = 1u;   // G1, G2: a view without a structure the vectors can be read by
inline constexpr std::uint32_t kFrameGenEngineSkip = 2u;       // G4: the game marked the frame FRAME_SKIP
inline constexpr std::uint32_t kFrameGenEnginePairing = 4u;    // G3, G5: the record before is not this frame's pair
inline constexpr std::uint32_t kFrameGenEngineCut = 8u;        // G6: the application's space jumped (a camera cut)
inline constexpr std::uint32_t kFrameGenEngineDecode = 16u;    // G7: the depth was decoded as another frame described it
inline constexpr std::uint32_t kFrameGenEngineRecreate = 32u;  // G8: the game made a swapchain in the last 2 frames...
inline constexpr std::uint32_t kFrameGenEngineSettle = 64u;    // ...or in the last 3 s
// G9: the frame's color was not captured at its release, or the frame is
// not one the engine path serves (just in time, two views, no quad views).
inline constexpr std::uint32_t kFrameGenEngineUncaptured = 128u;
inline constexpr std::size_t kFrameGenEngineGateCount = 8;
// Bit `index`'s name in the summaries: "noStruct", "skip", "pairing", "cut",
// "decode", "recreate", "settle", "uncaptured".
const char* FrameGenEngineGateName(std::size_t index);

// One view's structure as the gates read it.
struct FrameGenEngineGateView {
    std::uint32_t view{0};
    std::uint64_t vectors{0};           // the motion vector swapchain
    std::array<std::int32_t, 4> rect{}; // its region: x, y, width, height
    // No problems with the structure (ValidateEngineMotionView), and a
    // swapchain the presenter tracks, of float texels and one sample (G2).
    bool readable{false};
    bool frame_skip{false};
    FrameGenDepthEncoding depth; // the structure's minDepth, maxDepth, nearZ and farZ
};

// A frame's record (FrameGenEngineMotionFrame) as the gates read it.
struct FrameGenEngineGateRecord {
    std::int64_t display_time{0};      // the time the game ended the frame with
    std::uint64_t projection_frame{0}; // the layer's count of frames with a projection (0: none here)
    bool complete{false};              // every view carried a structure, of one extension
    std::vector<FrameGenEngineGateView> views;
    bool cut{false}; // its appSpaceDeltaPose is a camera cut (FrameGenEngineCuts)
    // The real frame Frame Generation generated from with it (the presenter's
    // frame sequence; 0: none).
    std::uint64_t sequence{0};
};

// What the presenter knows of the frame it generates from.
struct FrameGenEngineGateFacts {
    std::size_t views{0};     // the projection's
    std::int64_t end_time{0}; // the display time the game ended the frame with: its record's
    // The real frame generated from before it, frames begun over and never
    // ended after it aside (Frame Generation bridges those; 0: none, or
    // forgotten since: a pause, a frame the layer abandoned, a failure).
    std::uint64_t previous_sequence{0};
    bool continuous{false}; // it is the frame right after that one, in the same space, with as many views
    bool paused{false};     // past the pause limit after that one
    // Per projection view: the encoding the depth captured for the frame was
    // decoded with (none: no depth captured).
    std::vector<std::optional<FrameGenDepthEncoding>> decoded;
    std::uint64_t frames_since_swapchain{~0ull}; // real frames since the game made a swapchain
    double seconds_since_swapchain{1e9};
    bool captured{false}; // every view's color was captured at its release
    bool just_in_time{false};
    bool composed{false}; // quad views composed by Frame Generation
};

// The gates holding a frame's vectors back: `record` is the frame's (none
// where the layer handed none over), `previous` the record of the frame
// generated from before it.
std::uint32_t FrameGenEngineGates(const FrameGenEngineGateRecord* record, const FrameGenEngineGateRecord* previous,
                                  const FrameGenEngineGateFacts& facts);

// G6: whether a frame's appSpaceDeltaPose is a camera cut rather than the
// application's own movement: further than max(50 m, 8 x the median distance
// of the last 30 frames + 1 m), or turned more than 10 degrees. Until frames
// are known the median is 0 (50 m); every frame with a finite delta joins it,
// a cut too.
class FrameGenEngineCuts {
  public:
    // The frame's delta (a unit quaternion, meters): whether it is a cut.
    bool Add(const std::array<float, 4>& orientation, const std::array<float, 3>& position);
    double Median() const;
    void Reset() { distances_.clear(); }

  private:
    std::deque<double> distances_; // the last 30 frames'
};

// Pivot's change to a view (its reverse delta, applied in the application's
// space): the pose Frame Generation is told is `change` times the pose the
// game rendered with.
struct FrameGenPoseChange {
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
};
FrameGenPoseChange FrameGenDeclaredChange(const FrameGenViewGeometry& game, const FrameGenViewGeometry& declared);
// Whether that change moved between two frames: more than 0.05 degrees or
// 0.5 mm. The vectors are calibrated against the declared poses only while
// it holds still.
bool FrameGenDeclaredChangeMoved(const FrameGenPoseChange& before, const FrameGenPoseChange& now);

// --- Research switches ------------------------------------------------------

// A research switch's value: the environment's, else, for the engine motion
// research's switches (VECTORXR_FRAMEGEN_ENGINE_*,
// VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK), a NAME=VALUE line of
// %LOCALAPPDATA%\VectorXR\framegen-research.txt, read at each call: a flight
// changes them between a game's sessions without signing out to change the
// environment. None where neither has it (an empty value is none).
std::optional<std::string> ReadResearchSwitch(const char* name);
// The switches ReadResearchSwitch took from the file ("NAME=VALUE"), each
// once in the process: those no earlier call returned.
std::vector<std::string> TakeResearchFileSwitches();
// How many reads the file answered in the process (a session's: the count
// after its reads against the count before).
std::uint64_t ResearchFileReads();

} // namespace depthxr
