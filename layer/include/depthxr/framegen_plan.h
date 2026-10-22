#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
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
    // Engine motion vectors, stage 2 (Direct3D 12, FrameGenGpu::CaptureMotion):
    // the vectors of the image a frame's are in, as rendered, resampled to
    // each eye's half-resolution grid (kEngineRaw) on the game's queue, both
    // eyes in one dispatch (BuildFrameGenMotionCaptureDispatch).
    kEngineCapture,
    // Engine motion vectors, stage 3 (FrameGenViewRequest::engine_motion):
    // the evidence on a frame's vectors (against the poses, the frame's luma
    // and its measured flow) summed per group of a grid of samples, then
    // solved into the frame's verdict (see AppendEngineEvidence). Nothing
    // generated frames are made from reads what they write.
    kEngineReduce,
    kEngineSolve,
    // Engine motion vectors, stage 4 (FrameGenTuning::world_delta): the
    // world's motion worked out on the CPU from the game's appSpaceDeltaPose
    // (FrameGenEngineWorldParams), written into kWorldParams in place of the
    // fit (kWorldReduce, kWorldSolve) by one thread.
    kWorldSet,
    // Engine motion vectors, stage 5 (FrameGenTuning::engine_mode kAssist,
    // AppendEngineAssist): each flow cell's candidates from the frame's raw
    // vectors (its nearest and farthest surface's, as Frame Generation's flow),
    // then the measured flow or the vectors per cell, whichever the luma
    // follows better (kEngineFlow, the flow the filters read, and the cells'
    // labels), then each tile's trust in the vectors from the labels.
    kEngineConvert,
    kEngineMerge,
    kEngineTiles,
    // Engine motion vectors, stage 7 (FrameGenTuning::engine_map): the
    // engine motion map, built as kShiftSplat, kShiftResolve and kShiftFill
    // build the motion map, each texel moved by its flow cell's label (passes
    // of their own: the eye-shift map's stay as they were).
    kShiftSplatEngine,
    kShiftResolveEngine,
    kShiftFillEngine,
};
inline constexpr std::size_t kFrameGenPassCount = 39;

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
    // group in one row) and the fit (6 RGBA32F texels: rotation and 1 when
    // valid; translation and the inlier share; then, from kWorldSet, the
    // translation's scale, 1 (the application's movement: 0 for the fit), the
    // rotation's sign and 0; the rotation as a quaternion; and the fit's
    // first two texels again, which kWorldSet leaves: the watchdog's
    // readback, FrameGenTuning::delta_watchdog).
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
    // Engine motion vectors (kEngineCapture): the game's raw vectors at half
    // resolution, the newest capture and the one before it.
    kEngineRaw,
    kEngineRawPrevious,
    // Stage 3: the census of the image each of those was resampled from
    // (kEngineCensus's output, four texels per image), the evidence passes'
    // per-group sums (kFrameGenEnginePartialTexels RGBA32F texels per group in
    // one row), the solve (kFrameGenEngineParamsTexels: the bias, the
    // verdict) and the frame's evidence summed (kFrameGenEngineEvidenceTexels).
    kEngineCensus,
    kEngineCensusPrevious,
    kEnginePartials,
    kEngineParams,
    kEngineEvidence,
    // Stage 5 (kEngineConvert, kEngineMerge, kEngineTiles), at the final flow
    // level: each cell's two candidates (the nearest surface's vector in xy,
    // the farthest's in zw) and what kEngineMerge needs to know of them (the
    // spread, flags, the tile's trust and their classes); the flow the
    // filters read (kFlow's contract: kFlow itself where the frame's verdict
    // holds the vectors back); each cell's label (kFrameGenEngineLabel*), in
    // the ring like the filtered flow (synthesis's tint reads a real frame's);
    // and per tile of kFrameGenEngineTile cells the trust in the vectors, a
    // ping-pong pair like the history.
    kEngineCandidate,
    kEngineInfo,
    kEngineFlow,
    kEngineLabels,
    kEngineTiles,
    kEngineTilesPrevious,
    // The other view's kEngineParams, for kEngineConvert: its verdict holds
    // this view's vectors back too (the eyes take them, or not, together).
    kEngineParamsOther,
    // Stage 6 (Lead): the frame's verdict for both eyes (one RGBA32F texel,
    // kEngineSolve's stage 2), whose first 8 bytes are zero exactly when
    // both eyes' vectors may be used: copied into the view's predicate.
    kEngineVerdict,
    // Stage 7 (FrameGenTuning::engine_map): the world's motion a real
    // frame's filters read (kWorldParams' first four texels), copied at the
    // end of its Execute into the ring like the labels, for the engine
    // motion map of the frames written from it later
    // (FrameGenViewRequest::engine_world).
    kWorldParamsNewest,
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
// Engine motion vectors (stage 5). kFilter: cells the game's vectors moved
// follow their label's class (input 4, kEngineLabels; options.x 1: near
// content of its own motion keeps it when it matches clearly,
// FrameGenTuning::engine_cockpit), not their distance (FrameGenTuning::
// engine_filter). Synthesis with the confidence view: tinted by the labels
// (input 5; FrameGenTuning::engine_tint). Stage 7, with
// kFrameGenFlagDepthSplat: kShiftClear and the kShift*Engine passes build
// the engine motion map (FrameGenTuning::engine_map: each texel moved by its
// cell's label, input 4, and the world's motion, input 3), and synthesis
// reads the merged one (FrameGenEngineMap::kMerged: the eye's offset in it,
// no eye-shift map).
inline constexpr std::uint32_t kFrameGenFlagEngine = 8192u;
// The engine motion map's options.x (stage 7): input 3 holds the frame's
// world motion; the map is merged (the eye's offset, rot*.w as the eye-shift
// map's, added to each texel's motion).
inline constexpr std::uint32_t kFrameGenEngineMapWorld = 1u;
inline constexpr std::uint32_t kFrameGenEngineMapMerged = 2u;
// A generated frame within this time (in real frame intervals) of the one
// its real frame's engine motion map was built for reads that map again,
// scaled to its own time (FrameGenTuning::engine_map_reuse).
inline constexpr float kFrameGenEngineMapReuse = 0.35f;
// FrameGenTuning::engine_reach at most.
inline constexpr float kFrameGenEngineReachMost = 2.0f;
// kFilter: the sky (FG_WORLD_SKY and beyond) turns with the world's rotation
// where the world's motion is the application's own (kWorldSet: input 3's
// texel 2 y is 1), never with a fitted one (FrameGenTuning::world_sky).
inline constexpr std::uint32_t kFrameGenFlagWorldSky = 16384u;
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
    // Stage 6 (Lead, Direct3D 12): kFrameGenPredicateSkip where the frame's
    // verdict lets the game's vectors be used, kFrameGenPredicateSet for the
    // dispatch whose output is that predicate (see kFrameGenPredicateNone).
    std::uint8_t predicate{0};
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

// How a game's motion vector image holds its vectors (XR_FB_space_warp,
// XR_EXT_frame_synthesis), whatever the API: a texel's rgb as it is (FB), or
// rgb * scale + offset (EXT; `two_d`: rg alone, USE_2D_MOTION_VECTOR).
struct FrameGenMotionEncoding {
    bool ext{false};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::array<float, 3> offset{0.0f, 0.0f, 0.0f};
    bool two_d{false};
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
    // Stage 3 (FrameGenTuning::engine_mode from kObserve): no frame gate
    // holds this frame's vectors back, so their evidence is taken. The
    // presenter asks; the backend confirms for every view or none (each
    // view's vectors captured for this frame, its depth in use).
    bool engine_motion{false};
    // The poses the game rendered this frame and the frame before with
    // (before Pivot's change). With engine_lag, the head turned 2 degrees a
    // frame faster or slower than the frame before (both frames may
    // calibrate): the vectors are fitted against the pair before as well
    // (game_previous after game_before), which fits better when they come a
    // frame late.
    FrameGenViewGeometry game_newest;
    FrameGenViewGeometry game_previous;
    bool engine_lag{false};
    FrameGenViewGeometry game_before;
    // Stage 4: how the application's space moved from the frame before
    // (appSpaceDeltaPose, the same on every view: a still world point is
    // X(N) = R(delta_orientation) X(N-1) + delta_position in it), with
    // game_newest and game_previous. The presenter sets it for frames paired
    // with the frame before's record, without a camera cut or FRAME_SKIP,
    // whatever their vectors show: the world's motion follows from it
    // (FrameGenTuning::world_delta).
    bool app_delta{false};
    std::array<float, 4> delta_orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> delta_position{0.0f, 0.0f, 0.0f};
    // Set by backends with engine_motion: how the captured vectors decode,
    // the images the newest and the previous raw fields were resampled from
    // (their censuses' stale test), the vectors' region (its extent), and
    // whether the previous field holds the frame before's vectors.
    FrameGenMotionEncoding engine_encoding;
    std::array<std::uint32_t, 2> engine_images{};
    FrameGenExtent engine_vectors;
    bool engine_previous{false};
    // Set by backends with engine_motion from kAssist: the tiles' trust in
    // the vectors from the frame before is in kEngineTilesPrevious (none
    // after a discontinuity: each tile starts at half).
    bool engine_tiles{false};
    // Set by backends after the analysis (stage 7, FrameGenTuning::
    // engine_map): the frame's world motion was kept beside its labels
    // (kWorldParamsNewest), for the engine motion map of the frames written
    // from it.
    bool engine_world{false};
};

// How far Frame Generation goes with a game's motion vectors
// (VECTORXR_FRAMEGEN_ENGINE_MV). The census fingerprints the images and
// samples them every frame (stage 0); the capture resamples the image a
// frame's vectors are in to each eye's half-resolution grid as well, with
// a census of that image (of every image while it is not known which one,
// and every 30th frame) (stage 2); observe takes the evidence on each
// frame's vectors as well, works out their convention from it
// (FrameGenEngineValidator) and judges each frame by it (stage 3).
// Generated frames are made as without them in all three. Assist (stage 5)
// measures the flow as before and takes the vectors per flow cell where the
// luma follows them better, on frames whose verdict lets them be used: the
// measured flow, bit for bit, on the others. Interpolating, the mode goes no
// further than assist. Lead (stage 6) takes the vectors first on frames
// whose verdict lets them be used, with the measured passes recorded for
// those whose verdict does not and skipped on the GPU otherwise; the
// optical flow hardware is not used for those frames.
enum class FrameGenEngineMode : std::uint8_t {
    kOff,
    kCensus,
    kCapture,
    kObserve,
    kAssist,
    kLead,
};

const char* ToString(FrameGenEngineMode mode); // "off", "census", ...
// "off", "census", "capture", "observe", "assist" or "lead" (any case);
// anything else is off.
FrameGenEngineMode ParseFrameGenEngineMode(const char* value);

// What a game's motion vectors leave of the movement.
enum class FrameGenEngineCamera : std::uint8_t {
    kIncluded,     // the head's movement and the application's (the specification's intent)
    kRotationOnly, // the head's rotation alone
    kExcluded,     // the application's movement only: head movement removed
    kObjectOnly,   // objects' own movement only
};
const char* ToString(FrameGenEngineCamera camera);

// How a game's decoded vectors e relate to the movement: in D3D NDC (x
// right, y up), the current frame minus the previous one, n = gain *
// (e - offset) per axis, leaving `camera` of the movement in; with `jitter`,
// each frame's vectors carry a bias of their own (a jittered projection),
// which the GPU measures on still content and takes out. `offset` is a bias
// they all carry alike (in e's units; none when jittered: the frame's bias
// holds it).
struct FrameGenEngineConvention {
    std::array<float, 2> gain{1.0f, 1.0f};
    FrameGenEngineCamera camera{FrameGenEngineCamera::kIncluded};
    bool jitter{false};
    std::array<float, 2> offset{0.0f, 0.0f};
};
// The same gains (to a thousandth) and camera; the jitter and offset aside.
bool SameFrameGenEngineConvention(const FrameGenEngineConvention& a, const FrameGenEngineConvention& b);
// Stage 7 (VECTORXR_FRAMEGEN_ENGINE_MAP): extrapolated frames of a real
// frame whose flow cells the game's vectors labelled move its content with
// a depth test by each texel's label (the head's movement holds it, the
// world's moves it by the world's motion at its own depth, its own by its
// flow; the distance's rules elsewhere), in a motion map of their own beside
// the eye-shift map (separate), or in one map with the eye's offset added
// in place of both (merged).
enum class FrameGenEngineMap : std::uint8_t {
    kOff,
    kSeparate,
    kMerged,
};
const char* ToString(FrameGenEngineMap map); // "off", "separate", "merged"
// "separate" or "merged" (any case); anything else is off.
FrameGenEngineMap ParseFrameGenEngineMap(const std::string& value);

// "gx,gy,camera[,jitter]" (camera: included, rotation, excluded or objects),
// as VECTORXR_FRAMEGEN_ENGINE_MV_CONVENTION fixes one; none when it is not.
std::optional<FrameGenEngineConvention> ParseFrameGenEngineConvention(const std::string& text);
// The shaders' decode of raw vectors for a convention: tuning (A.x, A.y,
// B.x, B.y), the vector as rendered in uv (y down, current minus previous)
// being A * raw + B (raw: a texel's rg as the image holds it).
std::array<float, 4> FrameGenEngineDecode(const FrameGenEngineConvention& convention,
                                          const FrameGenMotionEncoding& encoding);

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
    // Engine motion vectors: from kCapture the backend keeps each view's
    // raw vector fields (kEngineRaw), made when the view is set up.
    FrameGenEngineMode engine_mode{FrameGenEngineMode::kOff};
    // Set by backends from kObserve: the convention the vectors' evidence is
    // taken with (FrameGenEngineValidator's, under test or locked), and
    // whether it is locked.
    FrameGenEngineConvention engine;
    bool engine_locked{false};
    // The flow ends at the optical flow hardware's level even where block
    // matching measures it (FrameGenFinalFlowLevel): from kAssist, a choice
    // between the game's vectors and measured flow never changes the level.
    bool hardware_level{false};
    // Stage 4, set by backends (VECTORXR_FRAMEGEN_ENGINE_DELTA=1, once their
    // calibration locked): world motion is worked out from the application's
    // movement where a request has it (FrameGenViewRequest::app_delta:
    // kWorldSet, FrameGenEngineWorldParams with the rotation's `delta_sign`
    // and the translation's `delta_scale`) instead of fitted to the measured
    // flow. With world_sky (VECTORXR_FRAMEGEN_ENGINE_SKY=1) extrapolating
    // filters turn the sky with that rotation (kFrameGenFlagWorldSky). On
    // the watchdog's frames (`delta_watchdog`, kFrameGenWorldDeltaWatchdog)
    // the fit runs as well, before kWorldSet, for the backend to read back:
    // the filters still read the application's movement.
    bool world_delta{false};
    int delta_sign{1};
    float delta_scale{1.0f};
    bool world_sky{false};
    bool delta_watchdog{false};
    // Stage 5, from kAssist (research, the presenter's switches): the filters
    // treat cells the game's vectors moved by their label's class instead of
    // their distance (VECTORXR_FRAMEGEN_ENGINE_FILTER=1, kFrameGenFlagEngine:
    // the head's, rigid; their own movement, kept beyond 4 m and never the
    // world's; the world's, as world content); near content of its own
    // movement (levers, a yoke) keeps it when extrapolating where it matches
    // clearly (VECTORXR_FRAMEGEN_ENGINE_COCKPIT=1); and the confidence view
    // tints the generated frames by the labels (VECTORXR_FRAMEGEN_ENGINE_TINT=1,
    // without depth_splat, whose motion map synthesis reads there).
    bool engine_filter{false};
    bool engine_cockpit{false};
    bool engine_tint{false};
    // Stage 6, Lead (research, the presenter's switches): the measured passes
    // are recorded predicated on the frame's verdict, for frames whose verdict
    // holds the vectors back; with engine_predicate false
    // (VECTORXR_FRAMEGEN_ENGINE_PREDICATE=0) none are, and kEngineMerge
    // searches every cell the vectors do not move (all of a frame whose
    // verdict holds them back). Interpolating goes as far as Lead only with
    // engine_interpolate (VECTORXR_FRAMEGEN_ENGINE_INTERPOLATE=lead).
    bool engine_predicate{true};
    bool engine_interpolate{false};
    // Stage 7, extrapolated frames of real frames whose cells were labelled
    // (research, the presenter's switches): the engine motion map
    // (VECTORXR_FRAMEGEN_ENGINE_MAP); just in time, a frame within
    // kFrameGenEngineMapReuse of the time the real frame's map was last built
    // for reads it again, scaled (VECTORXR_FRAMEGEN_ENGINE_MAP_REUSE=1); and
    // how far past the newest frame such frames may go, in real frame
    // intervals (VECTORXR_FRAMEGEN_ENGINE_REACH, 1 to kFrameGenEngineReachMost):
    // the head's and the world's motion go that far, the measured flow and
    // content's own motion one interval at most.
    FrameGenEngineMap engine_map{FrameGenEngineMap::kOff};
    bool engine_map_reuse{false};
    float engine_reach{1.0f};
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
// The level the plan's flow ends at: the quality's, or the hardware's (with
// hardware_flow or hardware_level).
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

// A texel's vector (xy) as `encoding` decodes it, from the image's raw rgba.
std::array<float, 2> DecodeFrameGenMotion(const FrameGenMotionEncoding& encoding, const std::array<float, 4>& raw);

// kEngineCapture: a raw vector not finite, or longer than kFrameGenEngineRawLimit
// on either axis in its own units (eight image widths in NDC, sixteen in uv;
// vectors in pixels would need a larger limit), is stored as this on both
// axes (half float's largest); an exact zero stays zero.
inline constexpr float kFrameGenEngineInvalid = 65504.0f;
inline constexpr float kFrameGenEngineRawLimit = 16.0f;
// The raw fields each view keeps (VECTORXR_FRAMEGEN_ENGINE_RAW_SLOTS=3: one
// more, so a capture waits for an older Execute).
inline constexpr std::size_t kFrameGenEngineRawSlots = 3;

// The kEngineCapture dispatch for the eyes of one half-resolution extent
// `l1` (`views` of them, 1 or 2): each eye's motion vector region
// (`rect0`, `rect1`: x, y, width, height in the image's texels, at array
// slice `slice0`, `slice1`) resampled to the grid by kDepth's nearest rule.
// size = (l1, rect0's extent), fov_current = (rect0's offset, rect1's
// offset), fov_previous = (rect1's extent, slice0, slice1), options.x =
// views. The backend dispatches `views` deep: id.z picks the eye.
FrameGenDispatch BuildFrameGenMotionCaptureDispatch(FrameGenExtent l1, const std::array<std::int32_t, 4>& rect0,
                                                    const std::array<std::int32_t, 4>& rect1, std::uint32_t slice0,
                                                    std::uint32_t slice1, std::uint32_t views);

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
// Stage 7: extrapolating a real frame whose cells were labelled, with
// FrameGenTuning::engine_map, the engine motion map is built for the frame
// (merged: in place of the eye-shift map), and `fraction` may go up to
// FrameGenTuning::engine_reach; with `map_time` (FrameGenTuning::
// engine_map_reuse), the map built for this real frame at that time is read
// again, scaled to this frame's, and none is built.
FrameGenViewPlan BuildFrameGenSynthesisPlan(const FrameGenViewRequest& request, const FrameGenTuning& tuning,
                                            FrameGenSynthesisMode mode, float fraction, bool cover_flow,
                                            bool stale_flow = false, float map_time = 0.0f);
// The time (FrameGenTuning::fractions' units) of the engine motion map a
// synthesis plan builds (stage 7); none when it builds none.
std::optional<float> FrameGenEngineMapTime(const FrameGenViewPlan& plan);
// Whether a frame at `time` may read the engine motion map built for its real
// frame at `map_time` again, scaled (FrameGenTuning::engine_map_reuse): within
// kFrameGenEngineMapReuse of it, both within one interval. Past it a map's
// flow stops at one interval while its models go on, which no one scale keeps.
bool FrameGenEngineMapReusable(float time, float map_time);

// Interpolation fraction or extrapolation distance for a generated frame
// displayed at target_time, given the two real frames' display times. Clamped
// so a hitch never pushes content past the real frames' motion: one interval,
// or extrapolating `reach` of them (stage 7, FrameGenTuning::engine_reach).
float FrameGenFraction(FrameGenSynthesisMode mode, std::int64_t previous_time, std::int64_t newest_time,
                       std::int64_t target_time, float reach = 1.0f);

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
    // The images the census read (bits; the others' hashes mean nothing):
    // in production the image the frame's vectors are thought to be in,
    // and every image now and then.
    std::uint32_t censused{~0u};
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
// frames changing nothing: the images are not written. A census of some of
// the images (FrameGenEngineTrackerFrame::censused, production's chosen
// image) only keeps their hashes: frames are scored from censuses of every
// image, each compared with that image's own last census.
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
    // The sums as the evidence passes write them (kFrameGenEngineAxisTerms
    // floats): n, the sums of p_r, p_t and e, then of p_r^2, p_t^2,
    // p_r p_t, e p_r, e p_t and e^2.
    static FrameGenEngineAxisFit FromSums(const float* sums);
    // The sums with each e read as e / gain + offset.
    FrameGenEngineAxisFit Unscaled(double gain, double offset) const;
    Solution Solve() const;
    Solution SolveLine() const;
    // e fitted to one part alone (e = a * p_r + c, or e = b * p_t + c).
    Solution SolveRotation() const;
    Solution SolveTranslation() const;
    // The mean of e - a * p_r - b * p_t: these samples' intercept under
    // another fit's a and b (their mean e with both 0).
    double Intercept(double a, double b) const { return n_ > 0.0 ? (e_ - a * r_ - b * t_) / n_ : 0.0; }
    double Samples() const { return n_; }
    // Sum of the squared rotation part: the regression's leverage.
    double RotationSpread() const { return rr_ - (n_ > 0.0 ? r_ * r_ / n_ : 0.0); }
    double TranslationSpread() const { return tt_ - (n_ > 0.0 ? t_ * t_ / n_ : 0.0); }
    // Sums of the squares about the means: of e, and of p_r + p_t.
    double Spread() const { return ee_ - (n_ > 0.0 ? e_ * e_ / n_ : 0.0); }
    double PredictionSpread() const {
        const double p = r_ + t_;
        return rr_ + 2.0 * rt_ + tt_ - (n_ > 0.0 ? p * p / n_ : 0.0);
    }
    // Sums of e p_r and e^2 about the origin (the off-diagonal test).
    double ErrorRotation() const { return er_; }
    double ErrorSquares() const { return ee_; }
    double RotationSquares() const { return rr_; }

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

// --- Engine motion vectors: evidence and validation (stage 3) --------------
//
// Each frame whose vectors no gate holds back (FrameGenViewRequest::
// engine_motion) has its evidence taken on the GPU on a grid of
// kFrameGenEngineGrid squared samples (AppendEngineEvidence): kEngineReduce
// sums each group's samples by kind, kEngineSolve sums the groups, takes the
// frame's bias out and judges the frame (the verdict), and the backend reads
// the sums back for FrameGenEngineValidator, which works out the vectors'
// convention.
//
// kEngineReduce's kinds (options.x bits 0-3, those a dispatch sums
// together; the cockpit fit against the pair before alone):
//   cockpit: per axis, still content's decoded vectors e against what the
//     declared poses predict (C1: e = a * p_r + b * p_t + c), with the
//     cross-axis sums; and against what the measured flow says (C2: q, as
//     rendered with the head; h, with the head taken out);
//   photometric: per sample, how well the frame's luma follows each of 8
//     variants of the convention under test (y flipped, direction flipped,
//     head toggled; variant 0 the convention itself), no motion and the
//     measured flow (C3), with the cockpit's inliers, the samples moving,
//     the vectors' distance from the measured flow by depth band and the
//     measured flow's length (the stale test of head-excluded vectors);
//   lag: the field before scored against this frame pair, with this
//   frame's beside it on the same samples.
inline constexpr std::uint32_t kFrameGenEngineGrid = 128u;
inline constexpr std::uint32_t kFrameGenEngineKindCockpit = 1u;
inline constexpr std::uint32_t kFrameGenEngineKindPhotometric = 2u;
inline constexpr std::uint32_t kFrameGenEngineKindLag = 4u;
inline constexpr std::uint32_t kFrameGenEngineKindCockpitLag = 8u;
// Each group's texels in kEnginePartials, and the kinds' places among them
// (kEngineEvidence holds their sums in the same places).
inline constexpr std::uint32_t kFrameGenEnginePartialTexels = 32u;
inline constexpr std::uint32_t kFrameGenEngineCockpitTexel = 0u;      // 11 texels
inline constexpr std::uint32_t kFrameGenEnginePhotometricTexel = 11u; // 7
inline constexpr std::uint32_t kFrameGenEngineLagTexel = 18u;         // 2
inline constexpr std::uint32_t kFrameGenEngineCockpitLagTexel = 20u;  // 5
inline constexpr std::uint32_t kFrameGenEngineEvidenceTexels = 32u;
inline constexpr std::uint32_t kFrameGenEngineParamsTexels = 8u;
inline constexpr std::uint32_t kFrameGenEngineAxisTerms = 10u;
// The frame's verdict (kEngineParams texel 0's z, and texel 4's x with zeros
// after it, so that the texel's first 8 bytes are zero exactly when the
// vectors may be used; Lead's predicate is both eyes' together,
// kEngineVerdict): 0 when the vectors may be used, else why not (bits).
inline constexpr std::uint32_t kFrameGenEngineVerdictUnlocked = 1u;    // no convention locked
inline constexpr std::uint32_t kFrameGenEngineVerdictStale = 2u;       // the image read did not change while the head moved
inline constexpr std::uint32_t kFrameGenEngineVerdictCockpit = 4u;     // still content's vectors do not follow the head
inline constexpr std::uint32_t kFrameGenEngineVerdictPhotometric = 8u; // the luma follows them worse than the measured flow
inline constexpr std::uint32_t kFrameGenEngineVerdictLag = 16u;        // the vectors before follow this frame as well
inline constexpr std::uint32_t kFrameGenEngineVerdictJitter = 32u;     // jittered, with no still content to take the bias from
inline constexpr std::size_t kFrameGenEngineVerdictReasons = 6;
const char* FrameGenEngineVerdictName(std::size_t index); // "unlocked", "stale", ...

// --- Engine motion vectors: assist (stage 5) --------------------------------
//
// On frames whose evidence is taken (FrameGenViewRequest::engine_motion),
// after it (AppendEngineAssist): kEngineConvert gives each final-level flow
// cell two candidates from the raw vectors under its half-resolution
// texels, the nearest surface's and the farthest's (the background, the far
// sentinel included), each converted to Frame Generation's flow by the
// convention (the head's movement taken out through the declared poses at
// its depth); kEngineMerge keeps the measured flow, bit for bit, wherever the
// frame's verdict holds the vectors back, and elsewhere takes the candidate
// whose block cost (REFINE's own) beats the measured flow's (Assist; by a
// margin the tile's trust widens), polished by a texel where that matches
// clearly better, labelling each cell; kEngineTiles moves each tile's trust
// toward the share of its textured cells the vectors won.
//
// A cell's label (kEngineLabels, R32_UINT): bits 0-2 what moved it, bits 3-4
// the class of its vector (the engine's candidates': the head's movement, the
// world's, their own), bit 6 the frame's verdict held the vectors back, bit 7
// the cell is textured (its block's luma varies: the tiles' share counts
// those); with the engine motion map (stage 7, kFrameGenEngineMergeDepths),
// bits 16-31 the depth of the surface the vector taken was chosen for (a half
// float; 0 elsewhere).
inline constexpr std::uint32_t kFrameGenEngineLabelSource = 7u;      // mask
inline constexpr std::uint32_t kFrameGenEngineLabelMeasured = 0u;    // the measured flow
inline constexpr std::uint32_t kFrameGenEngineLabelNear = 1u;        // the vectors: the nearest surface's
inline constexpr std::uint32_t kFrameGenEngineLabelFar = 2u;         // ... the farthest's
inline constexpr std::uint32_t kFrameGenEngineLabelZero = 3u;        // no motion (Lead)
inline constexpr std::uint32_t kFrameGenEngineLabelSearched = 4u;    // searched from the better of them and none (Lead)
inline constexpr std::uint32_t kFrameGenEngineLabelAbsent = 5u;      // measured: the vectors were cleared (an exact zero on far content the head moved)
inline constexpr std::uint32_t kFrameGenEngineLabelDisproved = 6u;   // measured: no depth under the cell the other eye did not disprove
inline constexpr std::uint32_t kFrameGenEngineClassShift = 3u;
inline constexpr std::uint32_t kFrameGenEngineClassHead = 1u;        // within a texel (or 8%) of no motion
inline constexpr std::uint32_t kFrameGenEngineClassWorld = 2u;       // ... of the world's motion (kWorldParams)
inline constexpr std::uint32_t kFrameGenEngineClassOwn = 3u;         // neither
inline constexpr std::uint32_t kFrameGenEngineLabelInvalid = 64u;    // the frame's verdict held the vectors back
inline constexpr std::uint32_t kFrameGenEngineLabelTextured = 128u;
// The tiles' side in final-level cells.
inline constexpr std::uint32_t kFrameGenEngineTile = 8u;
// kEngineMerge's options.x: Assist's rule, or Lead's (stage 6), with no
// measured flow at all (FrameGenTuning::engine_predicate false); the labels
// keep their surface's depth (stage 7, FrameGenTuning::engine_map: input 5
// the resolved depth).
inline constexpr std::uint32_t kFrameGenEngineMergeAssist = 1u;
inline constexpr std::uint32_t kFrameGenEngineMergeLead = 2u;
inline constexpr std::uint32_t kFrameGenEngineMergeUnmeasured = 4u;
inline constexpr std::uint32_t kFrameGenEngineMergeDepths = 8u;
// kEngineConvert's options.x: the camera from bit 4 (as kEngineReduce's),
// the frame before's tiles bound (input 3), the world's motion bound (input
// 4: the classes' world).
inline constexpr std::uint32_t kFrameGenEngineConvertTiles = 1u << 8;
inline constexpr std::uint32_t kFrameGenEngineConvertWorld = 1u << 9;
// The tiles over a final-level extent.
inline FrameGenExtent FrameGenEngineTiles(FrameGenExtent cells) {
    return {(cells.width + kFrameGenEngineTile - 1) / kFrameGenEngineTile,
            (cells.height + kFrameGenEngineTile - 1) / kFrameGenEngineTile};
}
// The mode a submission's engine passes run in: tuning.engine_mode, no
// further than Assist when interpolating (Lead's model-driven flow needs
// Interpolate's own gate) unless FrameGenTuning::engine_interpolate.
FrameGenEngineMode FrameGenEngineModeFor(const FrameGenTuning& tuning);

// --- Engine motion vectors: lead (stage 6) ----------------------------------
//
// A view's vectors lead (FrameGenEngineLeads) where its evidence is taken in
// Lead with the convention locked: the plan takes the evidence right after
// the luma (without measured flow: Lead's photometric test, against no
// motion), then the frame's verdict for both eyes (kEngineSolve's stage 2:
// kEngineVerdict) becomes the predicate of the measured passes after it
// (downsample, search, refine), which Direct3D 12 skips where the vectors
// may be used and runs in the same submission where they may not, so
// kEngineMerge copies their flow then as Assist does. The downsample to the
// merge's match level is never skipped (it reads that luma). No optical flow
// hardware: the backend leaves it idle for such frames, and the flow ends at
// its level all the same (FrameGenTuning::hardware_level). kEngineMerge
// takes a vector unless it matches worse than no motion; elsewhere it
// searches from the better of the two. The world's motion is the
// application's (kWorldSet) before the conversion, or fitted to the merged
// flow after it.
inline constexpr std::uint8_t kFrameGenPredicateNone = 0;
inline constexpr std::uint8_t kFrameGenPredicateSkip = 1; // skipped where the frame's verdict lets the vectors be used
inline constexpr std::uint8_t kFrameGenPredicateSet = 2;  // its output (kEngineVerdict) is the predicate from here
bool FrameGenEngineLeads(const FrameGenViewRequest& request, const FrameGenTuning& tuning);

// Whether a session in Lead lets the vectors lead its frames or holds them
// at Assist, from the frames' verdicts as Lead judges them (the validator's
// lead_held, read back a few frames late; frames held at Assist are judged
// by Lead's photometric test too): Lead after 30 verdicts in a row that let
// the vectors be used; held at Assist for 300 real frames when more than
// 10% of the last 30 frames' verdicts held them back, then Lead again after
// 30 in a row. A session starts at Assist.
class FrameGenEngineLeadControl {
  public:
    // Frames whose verdict was read since the last call, and those of them
    // whose verdict held the vectors back (on either eye) as Lead judges it:
    // taken as read in that order, the held ones last.
    void AddVerdicts(std::uint64_t frames, std::uint64_t held);
    // A real frame analysed while the session is in Lead.
    void AddFrame();
    bool Leads() const { return leads_; }
    // Session totals: times held at Assist, and back to Lead after.
    std::uint64_t Demotions() const { return demotions_; }
    std::uint64_t Promotions() const { return promotions_; }

  private:
    void Update();
    bool leads_{false};
    std::uint32_t held_frames_{0}; // real frames still to hold at Assist
    std::uint32_t streak_{0};      // verdicts in a row that let the vectors be used
    std::deque<std::uint8_t> window_; // the last 30 frames' verdicts, 1 held back
    std::uint64_t demotions_{0};
    std::uint64_t promotions_{0};
};

// One view's evidence for a frame, as read back (kEngineEvidence, the
// solve's kEngineParams), with what it was taken for.
struct FrameGenEngineEvidence {
    // Cockpit kind. C1 per axis (e in the vectors' decoded units, p in NDC),
    // the sums of e_x p_r,y and e_y p_r,x (an axis swap shows there), and C2
    // per axis with q as p_r and h as p_t, with the sum of |q - h|^2.
    std::array<FrameGenEngineAxisFit, 2> cockpit{};
    std::array<double, 2> swapped{};
    std::array<FrameGenEngineAxisFit, 2> measured{};
    double head_gap{0.0};
    // C1 against the pair before (FrameGenViewRequest::engine_lag), when taken.
    bool lag{false};
    std::array<FrameGenEngineAxisFit, 2> cockpit_lag{};
    // Photometric kind: the 3x3 luma error summed per variant, of no motion
    // and of the measured flow; the textured samples the luma was seen at,
    // those with measured flow, those the vectors move more than a pixel
    // where the previous frame was seen under their reading too (with
    // variant 0's and no motion's error there), those where the
    // vectors explain the luma about as well as the measured flow; still
    // near content and its inliers; the vectors' distance from the measured
    // flow (full-resolution pixels) summed by band (nearer than 4 m, from
    // 30 m, the sky) with the samples; the head movement and vector length
    // (pixels) summed over the samples with valid vectors; and the measured
    // flow's length (pixels) summed over those with measured flow.
    std::array<double, 8> variants{};
    double zero{0.0};
    double measured_cost{0.0};
    double textured{0.0};
    double with_measured{0.0};
    double moving{0.0};
    double variant_moving{0.0};
    double zero_moving{0.0};
    double wins{0.0};
    double inliers{0.0};
    double cockpit_samples{0.0};
    std::array<double, 3> epe{};
    std::array<double, 3> epe_samples{};
    double head_px{0.0};
    double vectors_px{0.0};
    double valid{0.0};
    double flow_px{0.0};
    // Lag kind (when the field before was the frame before's): the error of
    // the field before and of this frame's, on the samples both have, and
    // on those moving.
    bool lag_photometric{false};
    double lag_cost{0.0};
    double lag_variant{0.0};
    double lag_samples{0.0};
    double lag_cost_moving{0.0};
    double lag_variant_moving{0.0};
    double lag_moving{0.0};
    // The solve: the bias taken out (uv), the verdict and whether the image
    // read was stale.
    std::array<float, 2> bias{};
    std::uint32_t verdict{0};
    bool stale{false};
    // What it was taken for: whether the frame may calibrate, the view's
    // half-resolution extent, the vectors' region and the color view.
    bool calibrate{false};
    FrameGenExtent l1;
    FrameGenExtent vectors;
    FrameGenExtent color;
};
// A view's evidence from kEngineEvidence's and kEngineParams' texels
// (RGBA32F, kFrameGenEngineEvidenceTexels and kFrameGenEngineParamsTexels),
// taken with `convention`: the evidence passes sum the cockpit kinds' e as
// it reads the vectors (its gain times e less its offset), taken back here
// to e as decoded.
FrameGenEngineEvidence ReadFrameGenEngineEvidence(const float* evidence, const float* params,
                                                  const FrameGenEngineConvention& convention);

// A frame's evidence, every view's, with the convention it was taken with.
struct FrameGenEngineFrameEvidence {
    FrameGenEngineConvention convention;
    bool locked{false};
    std::vector<FrameGenEngineEvidence> views;
};

// The validator as the log tells it (FrameGenEngineValidator::Report).
struct FrameGenEngineValidation {
    enum class State : std::uint8_t { kProbing, kCalibrating, kLocked, kSuspended, kDisabled };
    State state{State::kProbing};
    FrameGenEngineConvention convention; // under test, or locked
    std::string name;                    // its canonical gain's name ("ndc", ...), or "" before one
    bool fixed{false};                   // VECTORXR_FRAMEGEN_ENGINE_MV_CONVENTION
    bool prior{false};                   // the game's table entry
    std::uint32_t decisions{0};          // locks, suspensions, disables and candidates
    std::string reason;                  // of the last decision
    // Session totals: frames read, batches judged, the last batch's fit
    // (gain, b/a: 0 without translation), the jitter's spread across its
    // frames (vector pixels), and how many batches in a row the pair before
    // fitted better (lag: 3).
    std::uint64_t frames{0};
    std::uint64_t batches{0};
    std::array<double, 2> gain{};
    std::array<double, 2> ratio{};
    std::array<double, 2> corr2{};
    std::array<double, 2> jitter_px{};
    std::uint32_t lag_batches{0};
    bool lag{false};
    // The last photometric batch: "passed", or what failed; and how many
    // passed in a row.
    std::string photometric;
    std::uint32_t photometric_passed{0};
    // Views judged, and those whose verdict held them back per reason
    // (kFrameGenEngineVerdict* bit order); frames whose verdict held them
    // back on either view (the eyes take them, or not, together), and those
    // Lead's would have (stage 6: the luma following the vectors worse than
    // no motion in place of Assist's test against the measured flow, the
    // other reasons as they are), which Lead's FrameGenEngineLeadControl
    // counts against `frames`: a session held at Assist leads again only
    // where Lead's own test lets it.
    std::uint64_t verdicts{0};
    std::uint64_t invalid{0};
    std::uint64_t held{0};
    std::uint64_t lead_held{0};
    std::array<std::uint64_t, kFrameGenEngineVerdictReasons> reasons{};
    // The photometric evidence summed (FrameGenEngineEvidence's).
    std::array<double, 3> epe{};
    std::array<double, 3> epe_samples{};
    double variant{0.0};
    double zero{0.0};
    double measured_cost{0.0};
    double textured{0.0};
    double with_measured{0.0};
    double lag_cost{0.0};
    double lag_variant{0.0};
    // Stage 5, the backend's (FrameGenGpu::EngineStatus; the validator
    // leaves them): frames the vectors were merged into, and of their flow
    // cells the textured ones and those the vectors moved.
    std::uint64_t merged{0};
    double textured_cells{0.0};
    double moved_cells{0.0};
    // Stage 7, the backend's: views' frames written with the engine motion
    // map, and those of them that read the map built for an earlier frame
    // again (FrameGenTuning::engine_map_reuse).
    std::uint64_t maps{0};
    std::uint64_t maps_reused{0};
};
const char* ToString(FrameGenEngineValidation::State state); // "probing", "calibrating", ...

// Works out a game's motion vector convention from the evidence on its
// frames (FrameGenEngineEvidence), and keeps checking it.
//   Batches: the C1 sums of frames that may calibrate, until each axis's
//     head rotation spreads by 300 * (3 / W1)^2 (NDC^2) over 3 frames or
//     more (300 frames without: the batch starts over); solved per axis and
//     classified: its gain (1 / a) within 10% of a canonical one with
//     corr^2 >= 0.7 and b / a in [0.75, 1.35] (head included; |b / a| <
//     0.25: rotation only; no translation to tell: head included), or C2's
//     head-excluded target explaining the vectors (corr^2 >= 0.7) while by
//     the gain it gives them still content's vectors are near zero in NDC
//     (head excluded); else unknown, as where the other axis's rotation
//     explains an axis better.
//   Photometric batches (C3): the photometric sums of frames taken with the
//     convention under test, 2000 textured samples or more over 3 frames:
//     variant 0 beats every other variant by 15%, the moving content's no
//     motion by 15%, the field before by 10%, and with measured flow comes
//     within 10% of it, as well as it on 70% of the samples.
//   Adopted (locked) after 3 batches in a row of one convention (1 when it is
//     the prior or fixed one, never 0) with the last 2 photometric batches
//     passing, unless the pair before fitted better in 3 batches in a row
//     (the vectors come a frame late). Jitter: the last 30 frames'
//     intercepts under their batch's fit spread by more than 0.15 vector
//     pixels about a mean within 1. Without jitter, a mean of 0.1 vector
//     pixels or more is the convention's offset.
//   Locked: suspended by a batch of another convention (corr^2 >= 0.7), 2
//     photometric batches 30% worse than the measured flow, more than 20% of
//     the last 90 frames held back by their verdict, or the lag; after 10 s
//     suspended it calibrates again with that convention as its prior.
//   Disabled: 3 suspensions within 5 minutes, or no lock over 5 minutes of
//     batches (unlocked, the time from each batch judged to the next, up to
//     2 s of it each: a longer gap had no movement to judge, or no frames).
//     A lack of still content never disables anything: it only leaves
//     batches unfinished.
class FrameGenEngineValidator {
  public:
    using State = FrameGenEngineValidation::State;
    // The game's table entry (FrameGenEnginePriorFor): one batch adopts it.
    void SetPrior(const FrameGenEngineConvention& convention);
    // VECTORXR_FRAMEGEN_ENGINE_MV_CONVENTION: only this one is ever under
    // test or locked.
    void Fix(const FrameGenEngineConvention& convention);
    // Tests: this one locked from now on, whatever the evidence says (each
    // frame's verdict still holds the vectors back where it fails): what the
    // vectors do once locked, tested apart from working out the convention.
    void LockForTest(const FrameGenEngineConvention& convention);
    // A frame's evidence, at `seconds` (any steady clock).
    void Add(const FrameGenEngineFrameEvidence& frame, double seconds);
    State GetState() const { return state_; }
    bool Locked() const { return state_ == State::kLocked; }
    // The convention evidence is taken with: under test, or locked.
    const FrameGenEngineConvention& Convention() const { return candidate_; }
    std::uint32_t Decisions() const { return report_.decisions; }
    FrameGenEngineValidation Report() const;

  private:
    struct Batch {
        std::array<FrameGenEngineAxisFit, 2> cockpit{};
        std::array<double, 2> swapped{};
        std::array<FrameGenEngineAxisFit, 2> measured{};
        // The frames' own cockpit sums (each frame's intercept), and the
        // cockpit on frames fitted against the pair before, with that fit.
        std::vector<std::array<FrameGenEngineAxisFit, 2>> frames;
        std::array<FrameGenEngineAxisFit, 2> lagged_now{};
        std::array<FrameGenEngineAxisFit, 2> lagged{};
        FrameGenExtent l1, vectors, color;
    };
    struct Photometric {
        FrameGenEngineEvidence sums;
        std::uint32_t frames{0};
    };
    struct Classified {
        std::optional<FrameGenEngineConvention> convention;
        std::string name;
        std::string why; // unknown: why
        double corr2{0.0};
    };
    Classified Classify(const Batch& batch);
    void JudgeBatch(double seconds);
    void JudgePhotometric(double seconds);
    void TryAdopt();
    void Decide(State state, const std::string& reason);
    void Suspend(const std::string& reason, double seconds);
    void StartCandidate(const FrameGenEngineConvention& convention, const std::string& name, bool prior);

    State state_{State::kProbing};
    FrameGenEngineConvention candidate_;
    std::optional<FrameGenEngineConvention> prior_;
    std::optional<FrameGenEngineConvention> fixed_;
    bool candidate_prior_{false}; // the candidate is the prior (or fixed) one: one batch adopts it
    bool held_for_test_{false};   // LockForTest: the verdicts are counted, nothing else changes
    std::uint32_t agreeing_{0};   // batches in a row of the candidate
    std::uint32_t passed_{0};     // photometric batches in a row passing
    std::uint32_t worse_{0};      // ... locked, worse than the measured flow
    std::uint32_t lagging_{0};    // batches in a row the pair before fitted better
    bool jitter_{false};
    Batch batch_;
    Photometric photometric_;
    std::deque<std::array<double, 2>> intercepts_; // the last batches' frames' intercepts (vector pixels)
    std::deque<std::uint8_t> verdicts_; // locked: the last 90 frames, 1 held back
    std::deque<double> suspensions_;    // when, the last 5 minutes'
    double suspended_at_{0.0};
    std::optional<double> last_batch_; // when the last batch was judged
    double unknown_seconds_{0.0};      // unlocked: the batches' time (see Disabled above)
    // The newest evidence's vectors and color extents (canonical names).
    std::pair<FrameGenExtent, FrameGenExtent> extents_{};
    FrameGenEngineValidation report_;
};

// The convention a game is expected to use (a table by executable,
// extension and graphics API: MSFS 2024's XR_FB_space_warp on Direct3D 12),
// none for the rest. `executable` is the file name, any case; `api` the
// backend's Name().
std::optional<FrameGenEngineConvention> FrameGenEnginePriorFor(const std::string& executable, bool ext,
                                                               const std::string& api);

// How much faster or slower the head turned between `previous` and `newest`
// than between `before` and `previous` (degrees per frame, per frame).
double FrameGenAngularAcceleration(const FrameGenViewGeometry& newest, const FrameGenViewGeometry& previous,
                                   const FrameGenViewGeometry& before);

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

// --- Engine motion vectors: the world's motion from the application's (stage 4)
//
// A game sending appSpaceDeltaPose says how its application space moved from
// the frame before: a still world point is X(N) = R_D X(N-1) + p_D in it.
// With the poses the game rendered both frames with (R_gN, c_gN; R_gP, c_gP)
// and the relative pose Frame Generation is told (Q_d, t_d, as the plan's
// constants carry it: Pivot's change to the views in it), that is the
// world's motion the fit (kWorldReduce, kWorldSolve) estimates from the
// measured flow, exactly: in the newest view's space, the head held as the
// declared poses hold it, a world point P at N-1 is at R_w P + t_w at N, with
//   G = R_gP^T R_D^T R_gN,  g = R_gP^T (R_D^T (c_gN - p_D) - c_gP),
//   R_w = G^T Q_d,          t_w = s_t G^T (t_d - g).
// `sign` -1 takes the delta's rotation conjugated (which way a game turns it
// is calibrated, as s_t, the scale between its meters and the depth's, is:
// FrameGenWorldDeltaCalibration).
struct FrameGenEngineWorld {
    std::array<float, 3> rotation{};    // w = 2 sign(q.w) q.xyz of R_w, radians (kWorldParams texel 0)
    std::array<float, 3> translation{}; // t_w, meters (texel 1)
    std::array<float, 4> quaternion{0.0f, 0.0f, 0.0f, 1.0f}; // R_w, w >= 0
    bool valid{false}; // every input and output finite
};
FrameGenEngineWorld FrameGenEngineWorldParams(const FrameGenViewGeometry& game_newest,
                                              const FrameGenViewGeometry& game_previous,
                                              const FrameGenViewGeometry& declared_newest,
                                              const FrameGenViewGeometry& declared_previous,
                                              const std::array<float, 4>& delta_orientation,
                                              const std::array<float, 3>& delta_position, int sign, float scale);

// How far Frame Generation goes with the application's movement
// (VECTORXR_FRAMEGEN_ENGINE_DELTA): calibrate compares every world fit with
// it (generated frames are made as without it); on takes the world's motion
// from it once the comparison locked (FrameGenTuning::world_delta).
enum class FrameGenWorldDeltaMode : std::uint8_t { kOff, kCalibrate, kOn };
const char* ToString(FrameGenWorldDeltaMode mode); // "off", "calibrate", "on"
// "calibrate" or "1" (any case for calibrate); anything else is off.
FrameGenWorldDeltaMode ParseFrameGenWorldDeltaMode(const std::string& value);
// "sign,scale" (VECTORXR_FRAMEGEN_ENGINE_DELTA_FIXED: "1,1", "-1,1.05"): the
// sign 1 or -1, the scale within the calibration's range; none otherwise.
std::optional<std::pair<int, float>> ParseFrameGenWorldDeltaFixed(const std::string& text);

// The fit runs before kWorldSet on every this-many-th frame that takes the
// application's movement: the calibration's watchdog (the filters read the
// set motion all the same).
inline constexpr std::uint64_t kFrameGenWorldDeltaWatchdog = 30;

// One view's world fit as kWorldSolve left it (kWorldParams texels 4 and 5,
// its copy of texels 0 and 1) beside what the model needs for the frame: its
// request's poses and delta.
struct FrameGenWorldFit {
    std::array<float, 3> rotation{};
    std::array<float, 3> translation{};
    bool valid{false}; // the fit solved
    FrameGenViewGeometry game_newest;
    FrameGenViewGeometry game_previous;
    FrameGenViewGeometry newest; // declared
    FrameGenViewGeometry previous;
    std::array<float, 4> delta_orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> delta_position{0.0f, 0.0f, 0.0f};
};

// The calibration as the log tells it (FrameGenGpu::WorldDeltaStatus).
struct FrameGenWorldDeltaStatus {
    FrameGenWorldDeltaMode mode{FrameGenWorldDeltaMode::kOff};
    bool sky{false}; // VECTORXR_FRAMEGEN_ENGINE_SKY=1
    // Locked, with the rotation's sign and the translation's scale; fixed
    // (VECTORXR_FRAMEGEN_ENGINE_DELTA_FIXED) until a drop.
    bool locked{false};
    bool fixed{false};
    int sign{1};
    float scale{1.0f};
    std::uint32_t decisions{0}; // locks and drops
    std::string reason;         // of the last
    std::string refused;        // a VECTORXR_FRAMEGEN_ENGINE_DELTA_FIXED that is none
    bool unread{false};         // the fits' readback could not be made: no calibration, the fit only this session
    // Since the calibration (re)started: frames with a fit beside the delta,
    // those informative (a fit moving 0.5 m or turning 0.002 radians a frame),
    // those turning; the fitted rotation's correlation with the delta's under
    // each sign (+1, -1), the fitted translation's under the better one, and
    // its scale (sum t_fit . t_D / sum |t_D|^2).
    std::uint64_t frames{0};
    std::uint64_t informative{0};
    std::uint64_t turning{0};
    std::array<double, 2> rotation_correlation{};
    double translation_correlation{0.0};
    double fitted_scale{0.0};
    // Session totals: the watchdog's checks while locked (frames whose model
    // moves 0.5 m or more), those whose fit is 25% off the model's
    // translation, the last one's |t_fit - t_D| / |t_D|, and the drops back
    // to the fit (10 such checks in a row); and frames the application's
    // movement stood in for the fit (the backend's count).
    std::uint64_t checks{0};
    std::uint64_t failed{0};
    double last_error{0.0};
    std::uint64_t drops{0};
    std::uint64_t set{0};
};

// Calibrates the application's movement against the world fit. Frames whose
// fit moves 0.5 m or turns 0.002 radians a frame (per view) are compared with
// the model under either sign of the delta's rotation (scale 1). The sign
// whose rotation correlates best with the fitted one is taken (that needs
// turns: 30 such frames or more), and s_t = sum t_fit . t_D / sum |t_D|^2.
// After 300 informative frames, with both correlations above 0.9 and s_t in
// [0.7, 1.4], it locks. Locked, each fit read is a check of the model (where
// it moves 0.5 m or more): 10 checks in a row 25% off its translation drop it
// back to the fit, and it calibrates again from scratch.
class FrameGenWorldDeltaCalibration {
  public:
    // VECTORXR_FRAMEGEN_ENGINE_DELTA_FIXED: locked from the start with these.
    void Fix(int sign, float scale);
    // A frame's fits (those that did not solve, or whose model is not
    // finite, are left out).
    void Add(const std::vector<FrameGenWorldFit>& fits);
    bool Locked() const { return status_.locked; }
    int Sign() const { return status_.sign; }
    float Scale() const { return status_.scale; }
    const FrameGenWorldDeltaStatus& Status() const { return status_; }

  private:
    void Restart();
    // Sums over informative views: the fitted rotation's and translation's
    // squares, and per sign their products with the model's and its squares.
    double rotation_squares_{0.0};
    double translation_squares_{0.0};
    std::array<double, 2> rotation_products_{};
    std::array<double, 2> rotation_model_{};
    std::array<double, 2> translation_products_{};
    std::array<double, 2> translation_model_{};
    std::uint32_t failing_{0}; // checks in a row 25% off
    FrameGenWorldDeltaStatus status_;
};

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

// Engine motion vectors: %LOCALAPPDATA%\VectorXR\engine-motion-<exe>.removed,
// which the Direct3D 12 backend writes when the device is removed within 2 s
// of reading this game's motion vectors. While it is there, sessions of this
// game read none (deleting it reads them again). Empty without %LOCALAPPDATA%.
std::filesystem::path FrameGenEngineRemovedPath();

} // namespace depthxr
