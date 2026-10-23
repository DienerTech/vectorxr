// Frame Generation: pose-compensated optical flow and frame synthesis.
//
// One source feeds every graphics API. The build compiles each pass with
// exactly one FG_PASS_* define: fxc emits DXBC for Direct3D 11 and 12, dxc
// emits SPIR-V for Vulkan. All passes share one binding layout:
//   t0-t3 / bindings 0-3  sampled textures
//   u0-u1 / bindings 4-5  storage textures (write only)
//   s0    / binding 6     linear clamp sampler
//   b0    / push constant FrameGenConstants (128 bytes)
//
// Flow is the displacement of image content from the previous real frame to
// the newest one, measured in the newest view and expressed in normalized
// view coordinates. Head motion between the two frames is removed before
// matching by sampling the previous frame through the relative view pose:
// rotation only, or with the newest frame's depth the full pose, so content
// fixed in the head's reference space (a cockpit) lines up exactly. The flow
// then holds only world, vehicle and object motion: the runtime's own
// reprojection corrects head rotation for every submitted frame, and with
// depth, generated frames are seen from where the eye will have moved to
// (FG_PASS_SHIFT_*), which runtimes that only rotate frames never correct.

// The engine motion map's passes (stage 7) are the shift passes built with
// EngineLanding in place of Landing (FG_SHIFT_ENGINE, Direct3D 12 only): the
// eye-shift map's and the motion map's own passes stay as they were.
#if defined(FG_PASS_SHIFT_SPLAT_ENGINE)
#define FG_PASS_SHIFT_SPLAT 1
#elif defined(FG_PASS_SHIFT_RESOLVE_ENGINE)
#define FG_PASS_SHIFT_RESOLVE 1
#elif defined(FG_PASS_SHIFT_FILL_ENGINE)
#define FG_PASS_SHIFT_FILL 1
#endif
#if (defined(FG_PASS_SHIFT_SPLAT_ENGINE) || defined(FG_PASS_SHIFT_RESOLVE_ENGINE) || \
     defined(FG_PASS_SHIFT_FILL_ENGINE)) && !defined(__spirv__)
#define FG_SHIFT_ENGINE 1
#endif

#ifdef __spirv__
#define FG_BIND(slot) [[vk::binding(slot, 0)]]
#define FG_FORMAT(format) [[vk::image_format(format)]]
#else
#define FG_BIND(slot)
#define FG_FORMAT(format)
#endif

struct FrameGenConstants {
    float4 fov_current;  // tangents left, right, up, down of the newest real view
    float4 fov_previous; // tangents of the previous real view
    float4 rot0;         // rows of R: newest-view direction -> previous-view direction;
    float4 rot1;         // w: the newest view's position in the previous view's
    float4 rot2;         // space (meters), used with depth
    uint4 size;          // xy destination extent, zw source extent
    uint4 options;       // x search radius, y flags, z diagnostic view, w level
    float4 tuning;       // x fraction, y motion limit, z cost scale, w photometric scale
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<FrameGenConstants> g;
#else
cbuffer FrameGenConstantBuffer : register(b0) {
    FrameGenConstants g;
};
#endif

#define FG_FLAG_SRGB_ENCODE 1u
#define FG_FLAG_EXTRAPOLATE 2u
#define FG_FLAG_TEMPORAL 4u
#define FG_FLAG_STILL 8u
#define FG_FLAG_DEPTH 16u
// Synthesis reads the eye-shift map (FG_PASS_SHIFT_*): generated frames
// are seen from where the eye will be, not where the newest frame was.
#define FG_FLAG_SHIFT 32u
// FG_PASS_LUMA: the previous frame's depth is bound (t3), so content the
// previous frame did not see (something nearer stood in front of it) is
// marked unseen like content outside its view.
#define FG_FLAG_DEPTH_PREVIOUS 64u
// FG_PASS_FILTER: the world's own motion (FG_PASS_WORLD_SOLVE, input 3)
// replaces the measured flow of world content.
#define FG_FLAG_WORLD 128u
// Synthesis, interpolating with depth: content only one real frame sees (at
// a motion boundary) is shown from that frame alone (see Synthesize).
#define FG_FLAG_OCCLUSION 256u
// Research: interpolation falls back to a cross-fade of the two real frames
// (head movement taken out) where its motion is not trusted, instead of to
// the newest frame.
#define FG_FLAG_CROSSFADE 512u
// Synthesis, extrapolating: the flow is the previous real frame's (this
// frame's own motion is not measured yet), read where each pixel's content
// was in that frame by head rotation alone.
#define FG_FLAG_STALE_FLOW 1024u
// Synthesis with depth, at a motion boundary: a candidate vector measured on
// another surface than the content it would carry here costs more (see
// DepthMismatch).
#define FG_FLAG_DEPTH_MATCH 2048u
// FG_PASS_SHIFT_*: the map is the motion map (depth engine mk2, M3), not the
// eye shift: content with a motion model (cockpit, head-fixed; world, moving
// with its flow) is splatted to the generated frame's time, nearest in
// front. Synthesis with it (input 5): where the map's surface and the chosen
// vector's disagree, the map's motion wins (see Synthesize).
#define FG_FLAG_DEPTH_SPLAT 4096u
// FG_PASS_FILTER: the sky (FG_WORLD_SKY and beyond) turns with the world's
// rotation where the world's motion is the application's own movement
// (FG_PASS_WORLD_SET, input 3), never with a fitted rotation.
#define FG_FLAG_WORLD_SKY 16384u
// Engine motion vectors, stage 5. FG_PASS_FILTER: cells the game's vectors
// moved follow their label's class (input 4: FG_PASS_ENGINE_MERGE's labels),
// not their distance. Synthesis with the confidence view: tinted by the
// labels (input 5). Stage 7, with FG_FLAG_DEPTH_SPLAT: FG_PASS_SHIFT_*_ENGINE
// build the engine motion map (each texel moved by its cell's label:
// EngineLanding), and synthesis reads it as the merged map (the eye's offset
// in it too).
#define FG_FLAG_ENGINE 8192u
// Engine motion vectors, stage 8. Synthesis, interpolating with depth: at a
// motion boundary, content whose flow cell's label says the previous frame
// did not see it (FG_ENGINE_LABEL_UNSEEN; input 5, the real frame's labels)
// is shown from the newest frame alone, however well the two frames agree.
#define FG_FLAG_ENGINE_VISIBILITY 32768u
// Research, stage 8: interpolating, content being covered (only the previous
// frame shows it, see Occlusion) is read from the previous frame where the
// forward map (input 5: FG_PASS_SHIFT_*_ENGINE with FG_ENGINE_MAP_FORWARD,
// the previous frame's content moved along its own flow) puts it; at a
// motion boundary, the forward map's content is taken as being covered
// wherever the chosen vector carries content the newest frame does not show
// moving that way.
#define FG_FLAG_ENGINE_BIDIR 65536u
// Research (head-motion shimmer design, fix A): synthesis reads the newest
// frame's colour by Catmull-Rom (SampleResampled: five bilinear taps, or
// nine with FG_FLAG_RESAMPLE_WIDE) instead of one bilinear tap, and with
// FG_FLAG_RESAMPLE_PREVIOUS the previous frame's too.
#define FG_FLAG_RESAMPLE_CUBIC 131072u
#define FG_FLAG_RESAMPLE_WIDE 262144u
#define FG_FLAG_RESAMPLE_PREVIOUS 524288u
// Research (fix B1): synthesis with depth reads the eye-shift map between its
// texels on the surface the texel under the pixel holds (SmoothShift).
#define FG_FLAG_SHIFT_SMOOTH 2097152u
// Research: the Flow view tints each pixel by its motion in pixels, in full
// from 2 pixels (sub-pixel motion shows), instead of by its share of the
// view. Bits 20 and 22 are kept for the head-motion shimmer design's matched
// sharpness and rotor research.
#define FG_FLAG_FLOW_FINE 8388608u
// The candidates' flags (FG_PASS_ENGINE_CONVERT; the nearest surface's in
// bits 0-3 of its info, the farthest's in 4-7): not a vector (the capture's
// sentinel), mapped behind the view, absent (a game that clears its vectors:
// an exact zero where the head moved far content), no depth under the cell.
#define FG_ENGINE_FLAG_INVALID 1u
#define FG_ENGINE_FLAG_UNMAPPED 2u
#define FG_ENGINE_FLAG_ABSENT 4u
#define FG_ENGINE_FLAG_NO_DEPTH 8u
// Bit 8 of every cell's info: the frame's verdict held this eye's vectors
// back, or the other eye's verdict held its own back, so this eye's are held
// back too (the eyes take them, or not, together).
#define FG_ENGINE_FLAG_HELD 256u
// Bits 9 and 10 (stage 8, with FG_ENGINE_CONVERT_SEEN): the previous frame
// did not see the nearest surface's content where its vector puts it then,
// or the farthest's (out of its view, or something nearer stood there).
#define FG_ENGINE_FLAG_NEAR_UNSEEN 512u
#define FG_ENGINE_FLAG_FAR_UNSEEN 1024u
// Absent: with the head's movement in the vectors, an exact zero on content
// without depth or from this many meters (the world, the sky), where the
// head's movement alone (its rotation) moves it by more than this many
// full-resolution pixels. Such a vector is cleared, not measured: the
// evidence leaves it out (FG_PASS_ENGINE_REDUCE), the merge never takes it.
#define FG_ENGINE_ABSENT_DEPTH 30.0
#define FG_ENGINE_ABSENT_PIXELS 0.5
// A flow cell's label (kFrameGenEngineLabel*): bits 0-2 what moved it, 3-4
// the class of its vector (the head's movement alone, the world's, its own),
// 5 the previous frame did not see the content the vector taken moves (stage
// 8), 6 the frame's verdict held the vectors back, 7 the cell is textured;
// with the engine motion map (stage 7), the high half the depth of the
// surface the vector taken was chosen for (a half float; 0 for other cells).
#define FG_ENGINE_LABEL_SOURCE 7u
#define FG_ENGINE_LABEL_MEASURED 0u
#define FG_ENGINE_LABEL_NEAR 1u
#define FG_ENGINE_LABEL_FAR 2u
#define FG_ENGINE_LABEL_ZERO 3u
#define FG_ENGINE_LABEL_SEARCHED 4u
#define FG_ENGINE_LABEL_ABSENT 5u
#define FG_ENGINE_LABEL_DISPROVED 6u
#define FG_ENGINE_CLASS_HEAD 1u
#define FG_ENGINE_CLASS_WORLD 2u
#define FG_ENGINE_CLASS_OWN 3u
#define FG_ENGINE_LABEL_UNSEEN 32u
#define FG_ENGINE_LABEL_INVALID 64u
#define FG_ENGINE_LABEL_TEXTURED 128u
// A texel takes its flow cell's label (the engine motion map's class, stage
// 7; synthesis's unseen bit, stage 8) where it lies on the surface the label
// was chosen for: its depth within this (natural log of the ratio) of the
// depth FG_PASS_ENGINE_MERGE kept in the label's high half. The cells along
// a canopy frame's edge hold the hangar or the sky behind it too.
#define FG_ENGINE_MAP_SURFACE 0.3
// The tiles' side in flow cells (kFrameGenEngineTile).
#define FG_ENGINE_TILE 8u
// The motion map's distance classes (stand-ins for fitted motion labels):
// nearer than this is head-fixed (FG_RIGID_DEPTH), at or beyond the second
// (to the sky) moves with the world (FG_WORLD_DEPTH); between, no model.
#define FG_SPLAT_HEAD 4.0
#define FG_SPLAT_WORLD 30.0
// The sky (FG_WORLD_SKY and beyond) moves in the motion map too (0: it
// stays as distant), by the measured flow of the sky around it, behind
// everything else (MSFS replays: better beside the cockpit edges).
#ifndef FG_SPLAT_SKY
#define FG_SPLAT_SKY 1
#endif

// Linear depth at or beyond this many meters is treated as infinitely far.
#define FG_FAR_DEPTH 100000.0

// The eye-shift map's value for "no offset written": a texel distant
// content keeps, or one FG_PASS_SHIFT_RESOLVE did not reach.
#define FG_SHIFT_NONE 0xffffffffu
// The splat key of distant content staying on its own texel (no depth, and
// the lowest distance score: see Key).
#define FG_SHIFT_FAR_KEY (64u | 31u)

// Depth trust (FG_PASS_TRUST_UPDATE): a score per depth texel in [-1, 1],
// positive where the other eye or the previous frame confirms the depth,
// negative where content lines up as infinitely far instead. Depth is used
// (head movement compensated in full, content moved with the eye) unless
// it is scored below -threshold: then the texel is known to be far
// (scenery behind glass the game wrote depth for). Requiring a score above
// +threshold instead tore cockpit labels apart in MSFS: the plain texels
// between their letters are scored by neither view, stayed behind while
// the letters moved, and broke up the motion matched across them.
#define FG_TRUST_THRESHOLD 0.25
// Trust scores are counted per tile of this many half-resolution texels a
// side, a thread group of FG_PASS_TRUST_UPDATE each, for the surface vote
// (FG_PASS_TRUST_VOTE).
#define FG_TRUST_TILE 8
// Tiles either way a tile's vote counts.
#define FG_VOTE_RADIUS 2
// Scales of a texel's depth the stereo probe tries (FG_PASS_DEPTH_SCALE),
// FG_SCALE_AS_READ its depth as read.
#define FG_SCALE_PROBES 7
#define FG_SCALE_AS_READ 3
static const float kDepthScaleProbes[FG_SCALE_PROBES] = {0.7, 0.8, 0.9, 1.0, 1.12, 1.25, 1.4};
// options.x of FG_PASS_TRUST_RESOLVE: the stereo scale vote is in t2.
#define FG_RESOLVE_SCALE 1u

#define FG_VIEW_FLOW 1u
#define FG_VIEW_CONFIDENCE 2u
#define FG_VIEW_TINT 3u

#define FG_PATCH_RADIUS 2
#define FG_INVALID_PENALTY 0.25
#define FG_MOTION_PENALTY 0.05

float Luma(float3 rgb) {
    // Square-root luma keeps shadow detail matchable; linear values crowd
    // dark scenery into a few codes and make the flow ghost there.
    return sqrt(max(dot(rgb, float3(0.2126, 0.7152, 0.0722)), 0.0));
}

// Reprojection of a newest-view coordinate into the previous view: by the
// relative rotation, plus the relative position when the content's linear
// depth (meters, along the view axis) is known. Returns false behind the
// previous view or outside its image.
bool MapToPrevious(float2 uv, float depth, out float2 previous_uv) {
    const float3 direction = float3(lerp(g.fov_current.x, g.fov_current.y, uv.x),
                                    lerp(g.fov_current.z, g.fov_current.w, uv.y), -1.0);
    float3 rotated = float3(dot(g.rot0.xyz, direction), dot(g.rot1.xyz, direction),
                            dot(g.rot2.xyz, direction));
    if ((g.options.y & FG_FLAG_DEPTH) != 0u && depth > 0.0 && depth < FG_FAR_DEPTH) {
        // The point is direction * depth; in the previous view R p + t, and
        // projection ignores scale, so R d + t / depth.
        rotated += float3(g.rot0.w, g.rot1.w, g.rot2.w) / depth;
    }
    previous_uv = float2(0.0, 0.0);
    if (rotated.z > -1e-4) {
        return false;
    }
    const float2 tangent = rotated.xy / -rotated.z;
    previous_uv = float2((tangent.x - g.fov_previous.x) / (g.fov_previous.y - g.fov_previous.x),
                         (tangent.y - g.fov_previous.z) / (g.fov_previous.w - g.fov_previous.z));
    return all(previous_uv >= 0.0) && all(previous_uv <= 1.0);
}

// Newest-frame linear depth at a newest-view coordinate (nearest texel: a
// filtered depth across a silhouette belongs to neither surface). Negative
// where the other eye disproved it (FG_PASS_DEPTH_TRUST): MapToPrevious then
// uses rotation only there.
float DepthNearest(Texture2D<float4> depth, float2 uv) {
    if ((g.options.y & FG_FLAG_DEPTH) == 0u) {
        return 0.0;
    }
    uint width, height;
    depth.GetDimensions(width, height);
    const int2 texel = clamp(int2(uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
    return depth.Load(int3(texel, 0)).r;
}

#if defined(FG_PASS_LUMA) || (defined(FG_PASS_ENGINE_CONVERT) && !defined(__spirv__))
// Previous-frame content nearer than this share of where the newest frame's
// depth puts a point (and by FG_OCCLUDER_METERS) stood in front of it.
#define FG_OCCLUDER_SHARE 0.92
#define FG_OCCLUDER_METERS 0.01

// Whether the previous frame saw the content at newest-view `uv` (linear
// depth `depth`, which mapped it to `previous_uv`), by its depth
// (`depth_previous`): false where its own depth there is clearly nearer, so
// something stood in front of it (newly revealed by head or object
// movement). Matching such texels finds aliases (black panel against black
// rim), not motion. Depth the other eye disproved (negative: scenery behind
// glass) never hides anything.
bool SeenBefore(Texture2D<float4> depth_previous, float2 uv, float depth, float2 previous_uv) {
    uint width, height;
    depth_previous.GetDimensions(width, height);
    const int2 texel = clamp(int2(previous_uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
    const float observed = depth_previous.Load(int3(texel, 0)).r;
    if (!(observed > 0.0) || observed >= FG_FAR_DEPTH) {
        return true;
    }
    if (!(depth > 0.0) || depth >= FG_FAR_DEPTH) {
        return false; // far content behind something the previous frame saw nearby
    }
    // The point's depth in the previous view: R d + t / depth scaled by depth.
    const float3 direction = float3(lerp(g.fov_current.x, g.fov_current.y, uv.x),
                                    lerp(g.fov_current.z, g.fov_current.w, uv.y), -1.0);
    const float3 rotated = float3(dot(g.rot0.xyz, direction), dot(g.rot1.xyz, direction),
                                  dot(g.rot2.xyz, direction)) +
                           float3(g.rot0.w, g.rot1.w, g.rot2.w) / depth;
    const float expected = -rotated.z * depth;
    return observed >= expected * FG_OCCLUDER_SHARE - FG_OCCLUDER_METERS;
}
#endif

#if defined(FG_PASS_LUMA)
FG_BIND(0) Texture2D<float4> g_current : register(t0);
FG_BIND(1) Texture2D<float4> g_previous : register(t1);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(3) Texture2D<float4> g_depth_previous : register(t3);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_luma_current : register(u0);
FG_BIND(5) FG_FORMAT("r32f") RWTexture2D<float> g_luma_previous : register(u1);

// Half-resolution luma of the newest frame and of the previous frame mapped
// into the newest view. Invalid previous texels (outside the previous view,
// or hidden there) are marked negative.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    g_luma_current[id.xy] = Luma(g_current.SampleLevel(g_linear, uv, 0.0).rgb);
    // The depth grid is this pass's grid (half resolution).
    const float depth = (g.options.y & FG_FLAG_DEPTH) != 0u ? g_depth.Load(int3(id.xy, 0)).r : 0.0;
    float2 previous_uv;
    const bool inside = MapToPrevious(uv, depth, previous_uv);
    const bool seen = inside && ((g.options.y & FG_FLAG_DEPTH_PREVIOUS) == 0u ||
                                 SeenBefore(g_depth_previous, uv, depth, previous_uv));
    g_luma_previous[id.xy] = seen ? Luma(g_previous.SampleLevel(g_linear, previous_uv, 0.0).rgb) : -1.0;
}
#endif

#if defined(FG_PASS_DOWNSAMPLE)
FG_BIND(0) Texture2D<float4> g_luma_current : register(t0);
FG_BIND(1) Texture2D<float4> g_luma_previous : register(t1);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_out_current : register(u0);
FG_BIND(5) FG_FORMAT("r32f") RWTexture2D<float> g_out_previous : register(u1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int2 limit = int2(g.size.zw) - 1;
    const int2 base = int2(id.xy) * 2;
    float current = 0.0;
    float previous = 0.0;
    float valid = 0.0;
    [unroll]
    for (int y = 0; y < 2; ++y) {
        [unroll]
        for (int x = 0; x < 2; ++x) {
            const int3 texel = int3(min(base + int2(x, y), limit), 0);
            current += g_luma_current.Load(texel).r;
            const float sample_previous = g_luma_previous.Load(texel).r;
            if (sample_previous >= 0.0) {
                previous += sample_previous;
                valid += 1.0;
            }
        }
    }
    g_out_current[id.xy] = current * 0.25;
    g_out_previous[id.xy] = valid > 0.0 ? previous / valid : -1.0;
}
#endif

#if defined(FG_PASS_SEARCH) || defined(FG_PASS_REFINE) || (defined(FG_PASS_ENGINE_MERGE) && !defined(__spirv__))
// Flow passes write one vector per cell of the flow level (size.xy) but match
// on a finer luma level: options.w holds the cell size in match-level texels,
// so fine texture such as instrument faces and terrain detail still
// constrains the vector of a coarse cell. FG_PASS_ENGINE_MERGE costs its
// candidates as FG_PASS_REFINE does.
FG_BIND(0) Texture2D<float4> g_luma_current : register(t0);
FG_BIND(1) Texture2D<float4> g_luma_previous : register(t1);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_flow : register(u0);

int2 MatchExtent() {
    uint width, height;
    g_luma_current.GetDimensions(width, height);
    return int2(width, height);
}

// Previous-frame luma at s, or -1 outside the image or the previous view.
float PreviousLuma(int2 s, int2 extent) {
    return (any(s < 0) || any(s >= extent)) ? -1.0 : g_luma_previous.Load(int3(s, 0)).r;
}

// Previous texels that are invalid cost a fixed penalty so borders never
// attract matches; a small motion term keeps flat regions still.
float MatchTerm(float current, float previous) {
    return previous < 0.0 ? FG_INVALID_PENALTY : abs(current - previous);
}

float MotionTerm(int2 d, int2 extent) {
    return FG_MOTION_PENALTY * length(float2(d) / float2(extent));
}
#endif

#if defined(FG_PASS_SEARCH)
// The previous frame's luma around one thread group's cells, as far as any
// candidate patch reaches: every candidate of every cell reads from here.
#define FG_SEARCH_MAX_RADIUS 6
#define FG_SEARCH_APRON (FG_SEARCH_MAX_RADIUS + FG_PATCH_RADIUS)
#define FG_SEARCH_TILE (8 + 2 * FG_SEARCH_APRON)
#define FG_PATCH_SIDE (2 * FG_PATCH_RADIUS + 1)
groupshared float g_tile[FG_SEARCH_TILE * FG_SEARCH_TILE];

struct Patch {
    float luma[FG_PATCH_SIDE * FG_PATCH_SIDE];
    bool valid[FG_PATCH_SIDE * FG_PATCH_SIDE];
};

// Mean absolute luma difference of a 5x5 patch around the cell, whose top
// left patch texel is at `corner` in the tile, displaced by -d. Newest
// texels outside the image are skipped.
float PatchCost(Patch patch, int2 corner, int2 d, int2 extent) {
    float sum = 0.0;
    float count = 0.0;
    [unroll]
    for (int i = 0; i < FG_PATCH_SIDE * FG_PATCH_SIDE; ++i) {
        if (patch.valid[i]) {
            const int2 t = corner + int2(i % FG_PATCH_SIDE, i / FG_PATCH_SIDE) - d;
            sum += MatchTerm(patch.luma[i], g_tile[t.y * FG_SEARCH_TILE + t.x]);
            count += 1.0;
        }
    }
    return sum / max(count, 1.0) + MotionTerm(d, extent);
}

// Exhaustive search on the coarsest level.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex) {
    const int2 extent = MatchExtent();
    const int2 origin = int2(group.xy) * 8 - FG_SEARCH_APRON;
    for (uint t = index; t < FG_SEARCH_TILE * FG_SEARCH_TILE; t += 64) {
        g_tile[t] = PreviousLuma(origin + int2(t % FG_SEARCH_TILE, t / FG_SEARCH_TILE), extent);
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int2 cell = int2(id.xy);
    Patch patch;
    [unroll]
    for (int i = 0; i < FG_PATCH_SIDE * FG_PATCH_SIDE; ++i) {
        const int2 q = cell + int2(i % FG_PATCH_SIDE, i / FG_PATCH_SIDE) - FG_PATCH_RADIUS;
        patch.valid[i] = all(q >= 0) && all(q < extent);
        patch.luma[i] = patch.valid[i] ? g_luma_current.Load(int3(q, 0)).r : 0.0;
    }
    const int2 corner = cell - origin - FG_PATCH_RADIUS;
    const int radius = min(int(g.options.x), FG_SEARCH_MAX_RADIUS);
    int2 best = int2(0, 0);
    float best_cost = PatchCost(patch, corner, best, extent);
    [loop]
    for (int y = -radius; y <= radius; ++y) {
        [loop]
        for (int x = -radius; x <= radius; ++x) {
            const float cost = PatchCost(patch, corner, int2(x, y), extent);
            if (cost < best_cost) {
                best_cost = cost;
                best = int2(x, y);
            }
        }
    }
    g_flow[id.xy] = float4(float2(best) / float2(extent), best_cost, 1.0);
}
#endif

#if defined(FG_PASS_REFINE) || (defined(FG_PASS_ENGINE_MERGE) && !defined(__spirv__))
FG_BIND(6) SamplerState g_linear : register(s0);

// Every refine cell compares a fixed 4x4 sample block: the cell's own texels
// at scale 4 (an 8x8 full-resolution block on half-resolution luma), or the
// cell plus a one-texel border at scale 2. The newest frame's block is read
// once into registers; each candidate then costs 16 previous-frame loads.
#define FG_BLOCK 4

struct Block {
    float luma[FG_BLOCK * FG_BLOCK];
    bool valid[FG_BLOCK * FG_BLOCK];
    bool all_valid;
    int2 first;
    int2 extent;
};

Block LoadBlock(int2 cell, int scale) {
    Block block;
    block.extent = MatchExtent();
    block.first = cell * scale - (scale >= FG_BLOCK ? 0 : 1);
    block.all_valid = all(block.first >= 0) && all(block.first + (FG_BLOCK - 1) < block.extent);
    [unroll]
    for (int i = 0; i < FG_BLOCK * FG_BLOCK; ++i) {
        const int2 q = block.first + int2(i % FG_BLOCK, i / FG_BLOCK);
        block.valid[i] = all(q >= 0) && all(q < block.extent);
        block.luma[i] = block.valid[i] ? g_luma_current.Load(int3(q, 0)).r : 0.0;
    }
    return block;
}

float BlockCost(Block block, int2 d) {
    const int2 first = block.first - d;
    float sum = 0.0;
    float count = 0.0;
    [branch]
    if (block.all_valid && all(first >= 0) && all(first + (FG_BLOCK - 1) < block.extent)) {
        // The whole previous block is inside the image: four gathers of 2x2
        // texels each instead of sixteen loads (the most frequent case, and
        // the bulk of Frame Generation's matching time). Gather returns
        // (bottom left, bottom right, top right, top left) of the quad.
        const float2 texel = 1.0 / float2(block.extent);
        [unroll]
        for (int y = 0; y < FG_BLOCK; y += 2) {
            [unroll]
            for (int x = 0; x < FG_BLOCK; x += 2) {
                const float4 quad = g_luma_previous.GatherRed(g_linear, (float2(first + int2(x, y)) + 1.0) * texel);
                sum += MatchTerm(block.luma[(y + 1) * FG_BLOCK + x], quad.x) +
                       MatchTerm(block.luma[(y + 1) * FG_BLOCK + x + 1], quad.y) +
                       MatchTerm(block.luma[y * FG_BLOCK + x + 1], quad.z) + MatchTerm(block.luma[y * FG_BLOCK + x], quad.w);
            }
        }
        count = float(FG_BLOCK * FG_BLOCK);
    } else {
        [unroll]
        for (int i = 0; i < FG_BLOCK * FG_BLOCK; ++i) {
            if (block.valid[i]) {
                const int2 q = block.first + int2(i % FG_BLOCK, i / FG_BLOCK);
                sum += MatchTerm(block.luma[i], PreviousLuma(q - d, block.extent));
                count += 1.0;
            }
        }
    }
    return sum / max(count, 1.0) + MotionTerm(d, block.extent);
}

// The block's cost if nothing moved, over the texels the previous frame
// saw; -1 when it saw fewer than half of them (outside its view, or hidden
// behind something nearer: FG_PASS_LUMA): no motion can be measured there.
float ZeroCost(Block block) {
    float sum = 0.0;
    float count = 0.0;
    [unroll]
    for (int i = 0; i < FG_BLOCK * FG_BLOCK; ++i) {
        if (block.valid[i]) {
            const float previous = PreviousLuma(block.first + int2(i % FG_BLOCK, i / FG_BLOCK), block.extent);
            if (previous >= 0.0) {
                sum += abs(block.luma[i] - previous);
                count += 1.0;
            }
        }
    }
    return count >= 0.5 * float(FG_BLOCK * FG_BLOCK) ? sum / count : -1.0;
}

void Consider(Block block, int2 d, inout int2 best, inout float best_cost) {
    const float cost = BlockCost(block, d);
    if (cost < best_cost) {
        best_cost = cost;
        best = d;
    }
}

// Parabolic sub-texel offset from three costs around a minimum.
float SubTexel(float minus, float center, float plus) {
    const float curvature = minus - 2.0 * center + plus;
    return curvature > 1e-6 ? clamp(0.5 * (minus - plus) / curvature, -0.5, 0.5) : 0.0;
}

// Step-halving search from `best`: one flow-level cell (scale match texels)
// first, then half that, down to single texels, so a level can correct its
// parent by a full parent cell while still landing on exact texels. Coarse
// steps probe a cross; the final single-texel step probes all eight
// neighbours.
void StepSearch(Block block, int scale, inout int2 best, inout float best_cost) {
    const int2 cross[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [loop]
    for (int step = scale; step > 1; step >>= 1) {
        const int2 center = best;
        [unroll]
        for (int i = 0; i < 4; ++i) {
            Consider(block, center + cross[i] * step, best, best_cost);
        }
    }
    const int2 center = best;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            if (x != 0 || y != 0) {
                Consider(block, center + int2(x, y), best, best_cost);
            }
        }
    }
}

// The sub-texel fit around `best` (cost `best_cost`), in match texels.
float2 SubTexelAt(Block block, int2 best, float best_cost) {
    return float2(SubTexel(BlockCost(block, best - int2(1, 0)), best_cost, BlockCost(block, best + int2(1, 0))),
                  SubTexel(BlockCost(block, best - int2(0, 1)), best_cost, BlockCost(block, best + int2(0, 1))));
}
#endif

#if defined(FG_PASS_REFINE)
FG_BIND(2) Texture2D<float4> g_parent : register(t2);
FG_BIND(3) Texture2D<float4> g_temporal : register(t3);

// Candidate refinement: the parent vectors around this cell, last frame's
// flow and zero motion compete; the winner gets a step-halving search and a
// sub-texel fit at the match level.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int2 cell = int2(id.xy);
    const int scale = int(g.options.w);
    const Block block = LoadBlock(cell, scale);
    const float2 match = float2(block.extent);
    const int2 parent_limit = int2(g.size.zw) - 1;
    const int2 parent = clamp(int2(uint2(cell) >> 1), int2(0, 0), parent_limit);

    int2 best = int2(0, 0);
    float best_cost = BlockCost(block, best);
    const int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll]
    for (int i = 0; i < 5; ++i) {
        const int2 q = clamp(parent + offsets[i], int2(0, 0), parent_limit);
        Consider(block, int2(round(g_parent.Load(int3(q, 0)).xy * match)), best, best_cost);
    }
    if ((g.options.y & FG_FLAG_TEMPORAL) != 0u) {
        // Last frame pair's motion, scaled to this pair's interval (tuning.x).
        const float2 uv = (float2(cell) + 0.5) / float2(g.size.xy);
        Consider(block, int2(round(g_temporal.SampleLevel(g_linear, uv, 0.0).xy * g.tuning.x * match)), best, best_cost);
    }
    StepSearch(block, scale, best, best_cost);
    const float2 sub = SubTexelAt(block, best, best_cost);
    // w: the cost of no motion (FG_PASS_FILTER's rigid test).
    g_flow[id.xy] = float4((float2(best) + sub) / match, best_cost, ZeroCost(block));
}
#endif

#if defined(FG_PASS_WORLD_REDUCE) || defined(FG_PASS_WORLD_SOLVE) || defined(FG_PASS_FILTER) || \
    defined(FG_PASS_ENGINE_CONVERT)
// World motion (FrameGenViewRequest::world_motion): content beyond this many
// meters is the world outside the vehicle, and it moves rigidly relative to
// the view with the vehicle's own motion (or the player's, walking): one
// rotation and one translation between two real frames explain its flow, and
// depth gives each texel its share of the translation. Measured block flow
// aliases on repeating detail (windows, roofs, fields) and is noisy on flat
// texture; the fitted motion is neither (MSFS captures 2026-10-05: the fit
// explains outside flow to a median 0.6 px).
#define FG_WORLD_DEPTH 30.0
// Depth at or beyond this is the far sentinel (FG_FAR_DEPTH: the sky, or
// terrain past the game's depth range): it stays out of the fit (no texture
// to match in the sky) and keeps its measured flow. A rotation fitted where
// world content with depth fills only a few percent of the view is the
// least certain part of the fit, and the sky is a third of it (replay of 12
// MSFS captures: outside bad pixels 2.75% -> 2.81% with the sky moved by
// the fit, 2.61% without). The application's own movement turns it exactly
// (FG_FLAG_WORLD_SKY with FG_PASS_WORLD_SET's motion).
#define FG_WORLD_SKY (0.5 * FG_FAR_DEPTH)
// The fit solves for the translation in units of this many meters, so its
// columns are about as large as the rotation's for distant world content.
#define FG_WORLD_TRANSLATION_UNIT 100.0

// The newest view's tangents at uv (fov_current: left, right, up, down).
float2 WorldTangents(float2 uv) {
    return float2(lerp(g.fov_current.x, g.fov_current.y, uv.x), lerp(g.fov_current.z, g.fov_current.w, uv.y));
}

// Tangent units per uv unit: flow in uv times this is flow in tangents.
float2 WorldTangentScale() {
    return float2(g.fov_current.y - g.fov_current.x, g.fov_current.w - g.fov_current.z);
}

// The inverse depth of world content (0: infinitely far, the sky), or -1 for
// content that is not world (cockpit and vehicle, disproved, no depth).
float WorldInverseDepth(float depth) {
    return depth >= FG_WORLD_DEPTH ? 1.0 / depth : -1.0;
}

// The derivatives of the flow (the newest minus the previous position, in
// tangents) with respect to the world's rotation w (radians) and
// translation t (meters) between the frames, in the newest view's space
// (x right, y up, -z forward), at tangent `at` and inverse depth inv_z.
void WorldRows(float2 at, float inv_z, out float3 x_w, out float3 x_t, out float3 y_w, out float3 y_t) {
    x_w = float3(at.x * at.y, -1.0 - at.x * at.x, -at.y);
    x_t = float3(inv_z, 0.0, at.x * inv_z);
    y_w = float3(1.0 + at.y * at.y, -at.x * at.y, at.x);
    y_t = float3(0.0, inv_z, at.y * inv_z);
}

// The flow the world motion in `world` (texel 0: rotation and validity,
// texel 1: translation) gives content at uv and inverse depth inv_z, in
// tangents.
float2 WorldFlow(Texture2D<float4> world, float2 uv, float inv_z) {
    float3 x_w, x_t, y_w, y_t;
    WorldRows(WorldTangents(uv), inv_z, x_w, x_t, y_w, y_t);
    const float3 rotation = world.Load(int3(0, 0, 0)).xyz;
    const float3 translation = world.Load(int3(1, 0, 0)).xyz;
    return float2(dot(x_w, rotation) + dot(x_t, translation), dot(y_w, rotation) + dot(y_t, translation));
}

// The flow (in tangents, as WorldFlow) of content at uv and inverse depth
// inv_z (0: infinitely far) under the world's motion from the application's
// movement (FG_PASS_WORLD_SET: the rotation as a quaternion in texel 3, the
// translation in texel 1), exactly: the point moved back to where it was
// the frame before, P(N-1) = R^T (P(N) - t), and seen from the newest view.
// WorldFlow's small-angle form is off by about t_z / z of the flow (a tenth
// of a pixel at 130 m and 3 m a frame; more at 30 m); the fit keeps it, as it
// was fitted with it.
float2 WorldFlowExact(Texture2D<float4> world, float2 uv, float inv_z) {
    const float2 at = WorldTangents(uv);
    const float4 q = world.Load(int3(3, 0, 0));
    // R^T (P - t) up to scale (P = (at, -1) / inv_z): by the conjugate.
    const float3 d = float3(at, -1.0) - world.Load(int3(1, 0, 0)).xyz * inv_z;
    const float3 u = -q.xyz;
    const float3 before = d + 2.0 * cross(u, cross(u, d) + q.w * d);
    float2 flow;
    if (before.z < -1e-6) {
        flow = at - before.xy / -before.z;
    } else {
        flow = WorldFlow(world, uv, inv_z); // behind the view the frame before: no projection
    }
    return flow;
}
#endif

#if defined(FG_PASS_WORLD_REDUCE)
FG_BIND(0) Texture2D<float4> g_flow : register(t0);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(3) Texture2D<float4> g_world : register(t3);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_partials : register(u0);

// Once a fit exists, residuals past this many pixels count less (Huber):
// aliases, moving objects and depth errors do not pull the motion.
#define FG_WORLD_HUBER 1.5
// The terms a sample adds: the 21 of the symmetric normal matrix, the 6 of
// the right side, its weight and 1 (the count); padded to 8 float4.
#define FG_WORLD_TERMS 29
groupshared float g_terms[64][FG_WORLD_TERMS];

// One iteration of a robust least-squares fit of the world motion to the
// measured flow (the final level, input 0) at a grid of samples (size.xy;
// size.zw: the flow level; options.w: its level; options.z: the iteration,
// whose weights come from the previous fit in input 3). Each group writes the
// sums of its samples to its 8 texels of the partials (FG_PASS_WORLD_SOLVE).
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex) {
    float terms[FG_WORLD_TERMS];
    [unroll]
    for (int i = 0; i < FG_WORLD_TERMS; ++i) {
        terms[i] = 0.0;
    }
    if (all(id.xy < g.size.xy)) {
        const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
        const int2 cell = min(int2(uv * float2(g.size.zw)), int2(g.size.zw) - 1);
        const float4 flow = g_flow.Load(int3(cell, 0));
        const float depth = DepthNearest(g_depth, uv);
        const float inv_z = WorldInverseDepth(depth);
        // World content with depth (not the sky) the previous frame saw (w:
        // its no-motion cost, negative where unseen).
        if (inv_z >= 0.0 && depth < FG_WORLD_SKY && flow.w >= 0.0) {
            const float2 measured = flow.xy * WorldTangentScale();
            const float pixels = float(g.size.z << g.options.w) / abs(WorldTangentScale().x);
            float weight = length(measured) * pixels < 64.0 ? 1.0 : 0.0;
            if (g.options.z > 0u && g_world.Load(int3(0, 0, 0)).w > 0.0) {
                const float residual = length(WorldFlow(g_world, uv, inv_z) - measured) * pixels;
                weight = residual <= FG_WORLD_HUBER ? 1.0 : FG_WORLD_HUBER / residual;
            }
            float3 x_w, x_t, y_w, y_t;
            WorldRows(WorldTangents(uv), inv_z, x_w, x_t, y_w, y_t);
            x_t *= FG_WORLD_TRANSLATION_UNIT;
            y_t *= FG_WORLD_TRANSLATION_UNIT;
            const float x[6] = {x_w.x, x_w.y, x_w.z, x_t.x, x_t.y, x_t.z};
            const float y[6] = {y_w.x, y_w.y, y_w.z, y_t.x, y_t.y, y_t.z};
            int k = 0;
            [unroll]
            for (int r = 0; r < 6; ++r) {
                [unroll]
                for (int c = r; c < 6; ++c) {
                    terms[k++] = weight * (x[r] * x[c] + y[r] * y[c]);
                }
            }
            [unroll]
            for (int b = 0; b < 6; ++b) {
                terms[21 + b] = weight * (x[b] * measured.x + y[b] * measured.y);
            }
            terms[27] = weight;
            terms[28] = 1.0;
        }
    }
    [unroll]
    for (int term = 0; term < FG_WORLD_TERMS; ++term) {
        g_terms[index][term] = terms[term];
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = 32u; stride > 0u; stride >>= 1) {
        if (index < stride) {
            [unroll]
            for (int sum = 0; sum < FG_WORLD_TERMS; ++sum) {
                g_terms[index][sum] += g_terms[index + stride][sum];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0u) {
        const uint slot = group.y * ((g.size.x + 7u) / 8u) + group.x;
        [unroll]
        for (int texel = 0; texel < 8; ++texel) {
            float4 value = float4(0.0, 0.0, 0.0, 0.0);
            [unroll]
            for (int lane = 0; lane < 4; ++lane) {
                if (4 * texel + lane < FG_WORLD_TERMS) value[lane] = g_terms[0][4 * texel + lane];
            }
            g_partials[uint2(slot * 8u + uint(texel), 0u)] = value;
        }
    }
}
#endif

#if defined(FG_PASS_WORLD_SOLVE)
FG_BIND(0) Texture2D<float4> g_partials : register(t0);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_world : register(u0);

// A fit needs this many samples, with this share of their weight left after
// the robust weights (most of the world agrees with one motion).
#define FG_WORLD_MIN_SAMPLES 200.0
#define FG_WORLD_MIN_INLIERS 0.5
groupshared float g_sums[64][32];

// Sums the partials (FG_PASS_WORLD_REDUCE) and solves the normal equations:
// texel 0 of the output holds the rotation and 1 when the fit is valid,
// texel 1 the translation and the inlier share. One group.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    uint width, height;
    g_partials.GetDimensions(width, height);
    const uint groups = width / 8u;
    float sums[32];
    [unroll]
    for (int i = 0; i < 32; ++i) {
        sums[i] = 0.0;
    }
    for (uint slot = index; slot < groups; slot += 64u) {
        [unroll]
        for (int j = 0; j < 8; ++j) {
            const float4 value = g_partials.Load(int3(slot * 8u + uint(j), 0, 0));
            sums[4 * j] += value.x;
            sums[4 * j + 1] += value.y;
            sums[4 * j + 2] += value.z;
            sums[4 * j + 3] += value.w;
        }
    }
    [unroll]
    for (int term = 0; term < 32; ++term) {
        g_sums[index][term] = sums[term];
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = 32u; stride > 0u; stride >>= 1) {
        if (index < stride) {
            [unroll]
            for (int sum = 0; sum < 32; ++sum) {
                g_sums[index][sum] += g_sums[index + stride][sum];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index != 0u) {
        return;
    }
    float a[6][7];
    int k = 0;
    [unroll]
    for (int r = 0; r < 6; ++r) {
        [unroll]
        for (int c = r; c < 6; ++c) {
            a[r][c] = g_sums[0][k];
            a[c][r] = g_sums[0][k];
            ++k;
        }
        a[r][6] = g_sums[0][21 + r];
    }
    const float weight = g_sums[0][27];
    const float count = g_sums[0][28];
    // A trace of damping: with the world far away (or only sky) the
    // translation is barely seen and must not make the system singular.
    [unroll]
    for (int d = 0; d < 6; ++d) {
        a[d][d] += weight * 1e-9;
    }
    // Gaussian elimination with partial pivoting.
    bool solved = count >= FG_WORLD_MIN_SAMPLES && weight >= FG_WORLD_MIN_INLIERS * count;
    [unroll]
    for (int col = 0; col < 6; ++col) {
        int pivot = col;
        [unroll]
        for (int row = col + 1; row < 6; ++row) {
            if (abs(a[row][col]) > abs(a[pivot][col])) pivot = row;
        }
        [unroll]
        for (int swap = 0; swap < 7; ++swap) {
            const float held = a[col][swap];
            a[col][swap] = a[pivot][swap];
            a[pivot][swap] = held;
        }
        solved = solved && abs(a[col][col]) > 1e-20;
        const float inverse = abs(a[col][col]) > 1e-20 ? 1.0 / a[col][col] : 0.0;
        [unroll]
        for (int other = 0; other < 6; ++other) {
            if (other != col) {
                const float factor = a[other][col] * inverse;
                [unroll]
                for (int entry = 0; entry < 7; ++entry) {
                    a[other][entry] -= factor * a[col][entry];
                }
            }
        }
    }
    float x[6];
    [unroll]
    for (int v = 0; v < 6; ++v) {
        x[v] = abs(a[v][v]) > 1e-20 ? a[v][6] / a[v][v] : 0.0;
        solved = solved && !isnan(x[v]) && !isinf(x[v]);
    }
    const float4 rotation = float4(x[0], x[1], x[2], solved ? 1.0 : 0.0);
    const float4 translation = float4(float3(x[3], x[4], x[5]) * FG_WORLD_TRANSLATION_UNIT, count > 0.0 ? weight / count : 0.0);
    g_world[uint2(0u, 0u)] = rotation;
    g_world[uint2(1u, 0u)] = translation;
    // Fitted, not the application's own movement (texel 2's y 0: the sky
    // keeps its measured flow, FG_PASS_FILTER), whatever FG_PASS_WORLD_SET
    // wrote before.
    g_world[uint2(2u, 0u)] = float4(0.0, 0.0, 0.0, 0.0);
    g_world[uint2(3u, 0u)] = float4(0.0, 0.0, 0.0, 1.0);
    // The fit again in texels 4 and 5, which FG_PASS_WORLD_SET leaves: where
    // the set follows the fit (the calibration's watchdog) the backend reads
    // the fit back from there.
    g_world[uint2(4u, 0u)] = rotation;
    g_world[uint2(5u, 0u)] = translation;
}
#endif

#if defined(FG_PASS_WORLD_SET)
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_world : register(u0);

// The world's motion worked out from the application's movement
// (appSpaceDeltaPose: FrameGenEngineWorldParams on the CPU) in place of the
// fit (FG_PASS_WORLD_REDUCE, FG_PASS_WORLD_SOLVE), from the constants:
// texel 0 the rotation and 1 (valid), texel 1 the translation and 1, texel
// 2 the translation's scale, 1 (the application's movement), the rotation's
// sign and 0, texel 3 the rotation as a quaternion; texels 4 and 5 (a fit's
// copy, the watchdog's) are left. One thread.
[numthreads(1, 1, 1)]
void CSMain() {
    g_world[uint2(0u, 0u)] = g.fov_current;
    g_world[uint2(1u, 0u)] = g.fov_previous;
    g_world[uint2(2u, 0u)] = g.rot0;
    g_world[uint2(3u, 0u)] = g.rot1;
}
#endif

#if defined(FG_PASS_FILTER)
FG_BIND(0) Texture2D<float4> g_flow_in : register(t0);
FG_BIND(1) Texture2D<float4> g_flow_before : register(t1);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(3) Texture2D<float4> g_world : register(t3);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_flow_out : register(u0);

// The match cost given to world content's fitted motion (FG_FLAG_WORLD):
// that of a clear match.
#define FG_WORLD_COST 0.01

// A vector continues the motion the previous frame pair showed when it is
// within this many full-resolution pixels of it, plus this share of the speed.
#define FG_STEADY_PIXELS 1.0
#define FG_STEADY_SHARE 0.25
// Rigid content: with depth in use (not disproved) and nearer than this
// many meters, content is cockpit, fixed in the head's reference space, and
// lines up exactly once head movement is taken out. Extrapolating, it keeps
// no measured vector: cockpit content barely moves between frames (a moving
// map scrolls a pixel or two), while screen text and repeating instrument
// detail (gauge bars, screen borders, the digits of a compass rose) alias to
// vectors of 2-180 px, pushed past the newest frame with nothing to correct
// them. Those tore MFD text apart (MSFS captures 2026-10-05: 9% of MFD cells
// moved, by 8 px or more on half of them). Cockpit content then moves with
// the eye; its own motion (a map scrolling, a lever, a hand) shows at the
// game's frame rate. Interpolating between two known frames, it keeps a
// vector when no motion matches clearly worse (by this share of the
// vector's cost plus the margin, in luma) and the vector is longer than the
// pixels (full resolution): flat panels and bezels match any vector about
// equally, and an alias carried their edges' colour across the cockpit
// (holding all cockpit motion measured worse there: bad pixels 0.71% to 0.77%).
#define FG_RIGID_DEPTH 4.0
#define FG_RIGID_SHARE 1.5
#define FG_RIGID_MARGIN 0.01
#define FG_RIGID_PIXELS 2.0
// The cost given to vectors of content the previous frame never saw
// (outside its view, or behind something nearer): their matches are aliases,
// and the median prefers its neighbours'.
#define FG_UNSEEN_COST 0.5

#if !defined(__spirv__)
// FG_FLAG_ENGINE: the cells' labels (FG_PASS_ENGINE_MERGE), Direct3D 12 only.
Texture2D<uint> g_labels : register(t4);
#endif
// options.x with FG_FLAG_ENGINE: near content of its own movement keeps it
// when extrapolating where it matches clearly (levers, a yoke, hands).
#define FG_ENGINE_COCKPIT 1u

// A flow sample (w: its cost with no motion, negative where the previous
// frame did not see the content: FG_PASS_REFINE) with the rigid test applied
// at its cell; `rigid` when the cell is cockpit content.
// With FG_FLAG_ENGINE, cells the game's vectors moved go by their label's
// class instead of their distance: the head's movement alone is rigid, at
// any distance; the world's is world content (the world's motion beyond
// 30 m); their own keep it beyond 4 m, never the world's (another aircraft),
// and nearer, extrapolating, only with FG_ENGINE_COCKPIT where no motion
// matches clearly worse (as the rigid keep test) and they move more than the
// pixels. The rest keep the distance's rules.
float4 Rigid(float4 flow, int2 cell, float2 pixels, out bool rigid) {
    const float zero = flow.w;
    const float2 uv = (float2(cell) + 0.5) / float2(g.size.xy);
    const float depth = DepthNearest(g_depth, uv);
    rigid = depth > 0.0 && depth < FG_RIGID_DEPTH;
    uint engine = 0u; // the class of a cell the game's vectors moved (0: none)
#if !defined(__spirv__)
    if ((g.options.y & FG_FLAG_ENGINE) != 0u) {
        const uint label = g_labels.Load(int3(cell, 0));
        const uint source = label & FG_ENGINE_LABEL_SOURCE;
        engine = source == FG_ENGINE_LABEL_NEAR || source == FG_ENGINE_LABEL_FAR ? (label >> 3) & 3u : 0u;
        if (engine != 0u) {
            rigid = engine == FG_ENGINE_CLASS_HEAD;
        }
    }
#endif
    float4 result = flow;
    if (rigid) {
        const bool keep = (g.options.y & FG_FLAG_EXTRAPOLATE) == 0u && zero >= 0.0 &&
                          zero > flow.z * FG_RIGID_SHARE + FG_RIGID_MARGIN && length(flow.xy * pixels) > FG_RIGID_PIXELS;
        if (!keep) {
            result = float4(0.0, 0.0, zero < 0.0 ? flow.z : min(flow.z, zero), 1.0);
        }
    } else if (engine == FG_ENGINE_CLASS_OWN) {
        if ((g.options.y & FG_FLAG_EXTRAPOLATE) != 0u && depth > 0.0 && depth < FG_RIGID_DEPTH) {
            const bool keep = (g.options.x & FG_ENGINE_COCKPIT) != 0u && zero >= 0.0 &&
                              zero > flow.z * FG_RIGID_SHARE + FG_RIGID_MARGIN && length(flow.xy * pixels) > FG_RIGID_PIXELS;
            if (!keep) {
                result = float4(0.0, 0.0, zero < 0.0 ? flow.z : min(flow.z, zero), 1.0);
            }
        } else if (zero < 0.0) {
            result.z = max(flow.z, FG_UNSEEN_COST);
        }
    } else {
        // World content takes the world's fitted motion, also where the
        // previous frame did not see it (no measured vector to trust there);
        // the application's own movement (FG_PASS_WORLD_SET, texel 2's y 1)
        // moves it exactly.
        const float inv_z = WorldInverseDepth(depth);
        if ((g.options.y & FG_FLAG_WORLD) != 0u && inv_z >= 0.0 && depth < FG_WORLD_SKY &&
            g_world.Load(int3(0, 0, 0)).w > 0.0) {
            const bool delta = g_world.Load(int3(2, 0, 0)).y == 1.0;
            result = float4((delta ? WorldFlowExact(g_world, uv, inv_z) : WorldFlow(g_world, uv, inv_z)) /
                                WorldTangentScale(),
                            FG_WORLD_COST, max(zero, 0.0));
        } else if ((g.options.y & FG_FLAG_WORLD_SKY) != 0u && depth >= FG_WORLD_SKY &&
                   g_world.Load(int3(0, 0, 0)).w > 0.0 && g_world.Load(int3(2, 0, 0)).y == 1.0) {
            // The sky turns with the world's rotation alone (infinitely far)
            // where that is the application's own movement: exact, unlike a
            // rotation fitted where world content with depth fills a few
            // percent of the view.
            result = float4(WorldFlowExact(g_world, uv, 0.0) / WorldTangentScale(), FG_WORLD_COST, max(zero, 0.0));
        } else if (zero < 0.0) {
            result.z = max(flow.z, FG_UNSEEN_COST);
        }
    }
    return result;
}

// Cost-weighted vector median over 3x3: removes isolated mismatches without
// blurring motion boundaries the way averaging would. Cockpit and scenery
// cells only vote among their own kind, so the canopy frame never takes the
// scenery's motion past it, nor the scenery the frame's stillness.
//
// When extrapolating, w carries how steady the chosen motion is: compared
// with the vector the same content had one frame pair earlier (scaled by
// tuning.x, the ratio of the two pairs' intervals), 1 when it continues it
// and 0 when it changes by twice the tolerance. Extrapolation
// pushes content on by its motion with nothing after it to correct it, so
// matching noise (instrument faces, fine text, anti-aliasing shimmer on
// still cockpit detail) would make still content jitter at the real frame
// rate; noise does not repeat from one frame pair to the next, motion does.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int2 limit = int2(g.size.xy) - 1;
    const float2 aspect = float2(1.0, float(g.size.y) / float(g.size.x));
    const float2 pixels = float2(g.size.xy) * float(1u << g.options.w);
    float4 samples[9];
    bool rigid[9];
    [unroll]
    for (int i = 0; i < 9; ++i) {
        const int2 q = clamp(int2(id.xy) + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), limit);
        samples[i] = Rigid(g_flow_in.Load(int3(q, 0)), q, pixels, rigid[i]);
    }
    int chosen = 4;
    float chosen_score = 1e30;
    [unroll]
    for (int c = 0; c < 9; ++c) {
        float score = 0.0;
        [unroll]
        for (int j = 0; j < 9; ++j) {
            const float weight = rigid[j] == rigid[4] ? 1.0 / (samples[j].z + 0.01) : 0.0;
            score += weight * length((samples[c].xy - samples[j].xy) * aspect);
        }
        if (rigid[c] == rigid[4] && score < chosen_score) {
            chosen_score = score;
            chosen = c;
        }
    }
    const float2 v = samples[chosen].xy;
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    const float depth = DepthNearest(g_depth, uv);
    float steadiness = 1.0;
    const uint steady_flags = FG_FLAG_EXTRAPOLATE | FG_FLAG_TEMPORAL;
    if ((g.options.y & steady_flags) == steady_flags && any(v != 0.0)) {
        // Where this content was in the previous real frame, and the motion
        // it had there.
        float2 before_uv;
        if (MapToPrevious(uv - v, depth, before_uv)) {
            const float2 before = g_flow_before.SampleLevel(g_linear, before_uv, 0.0).xy * g.tuning.x;
            const float change = length((v - before) * pixels);
            const float speed = max(length(v * pixels), length(before * pixels));
            steadiness = saturate(2.0 - change / (FG_STEADY_PIXELS + FG_STEADY_SHARE * speed));
        }
    }
    g_flow_out[id.xy] = float4(v, min(samples[chosen].z, samples[4].z), steadiness);
}
#endif

#if defined(FG_PASS_FLOW_MERGE)
// Quad views (Direct3D 12, a shader of its own): inside an eye's focus
// rectangle, the peripheral view's filtered flow (u0) and cover flow (u1,
// options.x 1 when made) take the focus view's (t0, t1) under each cell,
// rescaled from focus to peripheral view coordinates. Both views then move
// content the same way where the compositor blends them.
// fov_current: the focus rectangle (left, top, right, bottom) in peripheral
// uv; size.xy: the peripheral flow's cells, size.zw: the focus flow's.
FG_BIND(0) Texture2D<float4> g_focus_flow : register(t0);
FG_BIND(1) Texture2D<float4> g_focus_cover : register(t1);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_flow : register(u0);
FG_BIND(5) FG_FORMAT("rgba16f") RWTexture2D<float4> g_cover : register(u1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    const float4 rect = g.fov_current;
    if (any(uv < rect.xy) || any(uv > rect.zw)) {
        return;
    }
    const float2 extent = max(rect.zw - rect.xy, float2(0.0001, 0.0001));
    // The focus cells under this cell (4x4 at most, about 3x3 for DCS): their
    // cost-weighted vector median, as FG_PASS_FILTER chooses within a view.
    const float2 focus_cells = float2(g.size.zw);
    const float2 first = (uv - 0.5 / float2(g.size.xy) - rect.xy) / extent * focus_cells;
    const float2 last = (uv + 0.5 / float2(g.size.xy) - rect.xy) / extent * focus_cells;
    const int2 centre = int2((uv - rect.xy) / extent * focus_cells);
    const int2 low = clamp(max(int2(floor(first)), centre - 1), int2(0, 0), int2(g.size.zw) - 1);
    const int2 high = clamp(min(int2(ceil(last)) - 1, low + 3), low, int2(g.size.zw) - 1);
    const float2 aspect = float2(1.0, focus_cells.y / focus_cells.x);
    float4 samples[16];
    float4 covers[16];
    int count = 0;
    [loop]
    for (int y = low.y; y <= high.y; ++y) {
        [loop]
        for (int x = low.x; x <= high.x; ++x) {
            samples[count] = g_focus_flow.Load(int3(x, y, 0));
            covers[count] = g.options.x != 0u ? g_focus_cover.Load(int3(x, y, 0)) : float4(0.0, 0.0, 0.0, 0.0);
            ++count;
        }
    }
    int chosen = 0;
    float chosen_score = 1e30;
    [loop]
    for (int c = 0; c < count; ++c) {
        float score = 0.0;
        [loop]
        for (int j = 0; j < count; ++j) {
            score += length((samples[c].xy - samples[j].xy) * aspect) / (samples[j].z + 0.01);
        }
        if (score < chosen_score) {
            chosen_score = score;
            chosen = c;
        }
    }
    // Vectors are in view coordinates: a focus view spans `extent` of its
    // peripheral view. Costs and steadiness carry over as measured.
    g_flow[id.xy] = float4(samples[chosen].xy * extent, samples[chosen].zw);
    if (g.options.x != 0u) {
        g_cover[id.xy] = float4(covers[chosen].xy * extent, covers[chosen].zw);
    }
}
#endif

#if defined(FG_PASS_SYNTHESIZE_UNORM8) || defined(FG_PASS_SYNTHESIZE_FLOAT16) || defined(FG_PASS_SYNTHESIZE_DRAW)
FG_BIND(0) Texture2D<float4> g_current : register(t0);
FG_BIND(1) Texture2D<float4> g_previous : register(t1);
FG_BIND(2) Texture2D<float4> g_flow : register(t2);
FG_BIND(3) Texture2D<float4> g_depth : register(t3);
FG_BIND(6) SamplerState g_linear : register(s0);
#if !defined(__spirv__)
// The eye-shift map (FG_PASS_SHIFT_*) built for this generated frame (or,
// for a spare, the frame before), Direct3D 12 only, as depth is.
Texture2D<uint> g_shift : register(t4);
// FG_FLAG_DEPTH_SPLAT: the motion map built for this generated frame; or,
// FG_FLAG_ENGINE with the confidence view or FG_FLAG_ENGINE_VISIBILITY (no
// motion map then), the real frame's flow cell labels (FG_PASS_ENGINE_MERGE);
// or, FG_FLAG_ENGINE_BIDIR, the forward map built for this generated frame.
Texture2D<uint> g_motion : register(t5);
#endif
#if defined(FG_PASS_SYNTHESIZE_UNORM8)
FG_BIND(4) FG_FORMAT("rgba8") RWTexture2D<float4> g_output : register(u0);
#elif defined(FG_PASS_SYNTHESIZE_FLOAT16)
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_output : register(u0);
#endif

float3 LinearToSrgb(float3 value) {
    const float3 c = saturate(value);
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, step(c, 0.0031308));
}

float3 Hue(float h) {
    return saturate(float3(abs(h * 6.0 - 3.0) - 1.0, 2.0 - abs(h * 6.0 - 2.0), 2.0 - abs(h * 6.0 - 4.0)));
}

// Synthesis (FG_FLAG_OCCLUSION): the candidate error (CandidateError) above
// which the two real frames see different content wherever a vector points.
#define FG_OCCLUSION_ERROR 0.12

float MaxComponent(float3 value) {
    return max(value.x, max(value.y, value.z));
}

float3 Perceptual(float3 value) {
    return sqrt(max(value, 0.0));
}

float DepthAt(float2 uv) {
    return DepthNearest(g_depth, uv);
}

// Research (FG_FLAG_RESAMPLE_*, the head-motion shimmer design's fix A): a
// real frame's colour at uv by Catmull-Rom instead of one bilinear tap.
// Content moved by a fraction of a pixel (the eye's movement with depth,
// sub-pixel flow) loses fine detail to the bilinear tap, most halfway between
// texels, while real frames show theirs whole: generated frames come out
// softer at every phase the head's movement leaves, and alternate with sharp
// real frames. Five bilinear taps (the 4x4 filter's corners dropped, the rest
// renormalized), or nine with FG_FLAG_RESAMPLE_WIDE (the whole filter);
// either clamped to the four texels around the sample, so edges do not ring.
// A sample on a texel's centre (content not moved) reads that texel, and one
// halfway between four (a coarse pixel's centre under variable-rate shading,
// content not moved) keeps the bilinear tap, the 2x2 box the coarse pixel
// stands for: frames the head's movement leaves alone are as before.
float4 SampleResampled(Texture2D<float4> image, float2 uv) {
    uint width, height;
    image.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const int2 last = int2(width, height) - 1;
    const float2 p = uv * size - 0.5;
    const float2 b = floor(p);
    const float2 f = p - b;
    float4 result;
    [branch]
    if (all(abs(f - round(f)) < 1.0 / 512.0)) {
        result = image.Load(int3(clamp(int2(round(p)), int2(0, 0), last), 0));
    } else if (all(abs(f - 0.5) < 1.0 / 512.0)) {
        result = image.SampleLevel(g_linear, uv, 0.0);
    } else {
        // The weights of the texels at b - 1 .. b + 2; the middle two are
        // read with one bilinear tap between them.
        const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
        const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
        const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
        const float2 w3 = f * f * (-0.5 + 0.5 * f);
        const float2 w12 = w1 + w2;
        const float2 t0 = (b - 0.5) / size;
        const float2 t12 = (b + 0.5 + w2 / w12) / size;
        const float2 t3 = (b + 2.5) / size;
        float4 sum = image.SampleLevel(g_linear, t12, 0.0) * (w12.x * w12.y) +
                     image.SampleLevel(g_linear, float2(t12.x, t0.y), 0.0) * (w12.x * w0.y) +
                     image.SampleLevel(g_linear, float2(t0.x, t12.y), 0.0) * (w0.x * w12.y) +
                     image.SampleLevel(g_linear, float2(t3.x, t12.y), 0.0) * (w3.x * w12.y) +
                     image.SampleLevel(g_linear, float2(t12.x, t3.y), 0.0) * (w12.x * w3.y);
        float weight = w12.x * w12.y + w12.x * w0.y + w0.x * w12.y + w3.x * w12.y + w12.x * w3.y;
        if ((g.options.y & FG_FLAG_RESAMPLE_WIDE) != 0u) {
            sum += image.SampleLevel(g_linear, t0, 0.0) * (w0.x * w0.y) +
                   image.SampleLevel(g_linear, float2(t3.x, t0.y), 0.0) * (w3.x * w0.y) +
                   image.SampleLevel(g_linear, float2(t0.x, t3.y), 0.0) * (w0.x * w3.y) +
                   image.SampleLevel(g_linear, t3, 0.0) * (w3.x * w3.y);
            weight += w0.x * w0.y + w3.x * w0.y + w0.x * w3.y + w3.x * w3.y;
        }
        const int2 i0 = clamp(int2(b), int2(0, 0), last);
        const int2 i1 = clamp(int2(b) + 1, int2(0, 0), last);
        const float4 a = image.Load(int3(i0, 0));
        const float4 c = image.Load(int3(i1.x, i0.y, 0));
        const float4 d = image.Load(int3(i0.x, i1.y, 0));
        const float4 e = image.Load(int3(i1, 0));
        result = clamp(sum / weight, min(min(a, c), min(d, e)), max(max(a, c), max(d, e)));
    }
    return result;
}

// A real frame's colour at uv: one bilinear tap, or with `cubic`
// SampleResampled's.
float4 SampleColor(Texture2D<float4> image, float2 uv, bool cubic) {
    float4 result;
    [branch]
    if (cubic) {
        result = SampleResampled(image, uv);
    } else {
        result = image.SampleLevel(g_linear, uv, 0.0);
    }
    return result;
}

// Depth for front-to-back order: unknown or disproved depth is far.
float OrderDepth(float depth) {
    return depth > 0.0 ? depth : FG_FAR_DEPTH;
}

// The newest frame sees content moving by `v` at `newest_uv` when its own
// flow there is `v` (within a pixel and a half, or a quarter of the motion).
bool SeesMoving(float2 newest_uv, float2 v) {
    if (any(newest_uv < 0.0) || any(newest_uv > 1.0)) {
        return false;
    }
    uint width, height;
    g_flow.GetDimensions(width, height);
    const int2 cell = clamp(int2(newest_uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
    const float2 there = g_flow.Load(int3(cell, 0)).xy;
    const float2 pixels = float2(g.size.xy);
    return length((there - v) * pixels) <= max(1.5, 0.25 * length(v * pixels));
}

// Interpolating with depth, at a motion boundary where no candidate vector
// has both real frames agree, the content belongs to one of them: newly
// revealed (the newest frame sees it, the previous one saw what moved off
// it) or being covered (the previous frame sees it, the newest one already
// shows what moved over it). A candidate the newest frame sees moving its
// way where it lands is shown from the newest frame alone, the nearest such
// surface in front; with none, the content is being covered: the farthest
// candidate's surface (the background, whose motion the cell beside it
// measured) is shown from the previous frame alone. Returns 1 or 2 with the
// vector and the depth to map it with, or 0.
int Occlusion(float2 uv, float step, float4 candidates[7], float candidate_depth[7], out float2 v, out float depth) {
    v = float2(0.0, 0.0);
    depth = 0.0;
    float nearest = 2.0 * FG_FAR_DEPTH;
    int kind = 0;
    [unroll]
    for (int k = 0; k < 7; ++k) {
        const float2 landing = uv + step * candidates[k].xy;
        if (SeesMoving(landing, candidates[k].xy)) {
            const float there = OrderDepth(DepthAt(landing));
            if (there < nearest) {
                nearest = there;
                kind = 1;
                v = candidates[k].xy;
                depth = DepthAt(landing);
            }
        }
    }
    if (kind != 0) {
        return kind;
    }
    float farthest = -1.0;
    [unroll]
    for (int c = 0; c < 7; ++c) {
        const float own = OrderDepth(candidate_depth[c]);
        if (own > farthest) {
            farthest = own;
            v = candidates[c].xy;
            depth = candidate_depth[c];
        }
    }
    return 2;
}

// FG_FLAG_DEPTH_MATCH: how much a candidate vector measured at depth
// `measured` costs for carrying content at depth `carried` (meters; unknown
// or disproved depth, 0 or less, costs nothing). A block vector measured on
// a near feature does not carry the background beside it, nor the reverse:
// beyond FG_MATCH_TOLERANCE (natural log of the depth ratio) the cost grows
// to FG_MATCH_WEIGHT, in CandidateError's units, over FG_MATCH_RANGE.
#ifndef FG_MATCH_WEIGHT
#define FG_MATCH_WEIGHT 0.12
#endif
#define FG_MATCH_TOLERANCE 0.05
#define FG_MATCH_RANGE 0.25
float DepthMismatch(float measured, float carried) {
    if (!(measured > 0.0) || !(carried > 0.0)) {
        return 0.0;
    }
    const float ratio = abs(log(min(measured, 0.5 * FG_FAR_DEPTH) / min(carried, 0.5 * FG_FAR_DEPTH)));
    return FG_MATCH_WEIGHT * saturate((ratio - FG_MATCH_TOLERANCE) / FG_MATCH_RANGE);
}

// How differently the two real frames see the content a candidate vector
// claims to carry to this pixel. Block flow straddles object edges; picking
// the neighbouring block vector the frames agree on restores pixel-sharp
// motion boundaries. Returns 1 when the vector leaves either image.
float CandidateError(float2 uv, float2 v, float step) {
    const float2 newest_uv = uv + step * v;
    float2 previous_uv;
    if (any(newest_uv < 0.0) || any(newest_uv > 1.0) || !MapToPrevious(newest_uv - v, DepthAt(newest_uv), previous_uv)) {
        return 1.0;
    }
    const float3 newest = Perceptual(g_current.SampleLevel(g_linear, newest_uv, 0.0).rgb);
    const float3 previous = Perceptual(g_previous.SampleLevel(g_linear, previous_uv, 0.0).rgb);
    return MaxComponent(abs(newest - previous));
}

#if !defined(__spirv__)
// The eye-shift map's offset at `texel` (map texels), none where it has none.
float2 ShiftBack(int2 texel) {
    const uint packed = g_shift.Load(int3(texel, 0));
    return packed == FG_SHIFT_NONE ? float2(0.0, 0.0) : float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16));
}

// The depth the eye-shift map's offset `back` at `texel` reads its content
// at (map texels; the map lies on the depth's grid): distant, unknown or
// dropped depth as FG_FAR_DEPTH.
float ShiftSourceDepth(int2 texel, float2 back, float2 size) {
    const float z = DepthAt((float2(texel) + 0.5 + back) / size);
    return z > 0.0 && z < FG_FAR_DEPTH ? max(z, 0.01) : FG_FAR_DEPTH;
}

// Research (FG_FLAG_SHIFT_SMOOTH, the head-motion shimmer design's fix B1):
// the eye-shift map read between its texels instead of at the one under the
// pixel. The map has a texel per 2x2 pixels, so a surface whose offset
// varies across it (a panel at an angle to the eye) moves in steps of two
// pixels, and the steps shift as the eye moves. The texel under the pixel
// decides the surface: of the four texels around the pixel's centre, those
// on it (offsets within 2 texels of its own, content read within 3% of its
// depth) are blended bilinearly, the rest left out. Four offsets within a
// quarter texel of its own are blended outright. `own_texel`, `own`: the
// texel under the pixel and its offset; `size`: the map's.
float2 SmoothShift(float2 uv, int2 own_texel, float2 own, float2 size) {
    const int2 last = int2(size) - 1;
    const float2 q = uv * size - 0.5;
    const int2 base = int2(floor(q));
    const float2 f = q - float2(base);
    const float4 weights = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    float2 backs[4];
    bool close = true;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        backs[i] = ShiftBack(clamp(base + int2(i & 1, i >> 1), int2(0, 0), last));
        close = close && all(abs(backs[i] - own) <= 0.25);
    }
    float2 result = weights.x * backs[0] + weights.y * backs[1] + weights.z * backs[2] + weights.w * backs[3];
    [branch]
    if (!close) {
        const float own_depth = ShiftSourceDepth(own_texel, own, size);
        float2 blended = float2(0.0, 0.0);
        float total = 0.0;
        [unroll]
        for (int k = 0; k < 4; ++k) {
            const float there = ShiftSourceDepth(clamp(base + int2(k & 1, k >> 1), int2(0, 0), last), backs[k], size);
            const bool surface =
                own_depth >= FG_FAR_DEPTH ? there >= FG_FAR_DEPTH : abs(there - own_depth) <= 0.03 * own_depth;
            const float weight = all(abs(backs[k] - own) <= 2.0) && surface ? weights[k] : 0.0;
            blended += weight * backs[k];
            total += weight;
        }
        result = total > 0.0 ? blended / total : own;
    }
    return result;
}
#endif

// Where a generated pixel's content sits in the newest view: the pixel
// itself, or with FG_FLAG_SHIFT where the eye-shift map says it comes from.
// options.w holds the map's frame time (half float bits), in units of
// `fraction` past the newest frame, or before it when interpolating: the
// frame's own time, except a spare's, which scales the map to its time.
// `position`: the pixel's centre in pixels (a coarse pixel's centre under
// variable-rate shading).
float2 SourceUv(float2 position, float fraction) {
    float2 uv = position / float2(g.size.xy);
#if !defined(__spirv__)
    if ((g.options.y & FG_FLAG_SHIFT) != 0u) {
        const uint2 pixel = uint2(position);
        const float map_time = f16tof32(g.options.w & 0xffffu);
        const float time = (g.options.y & FG_FLAG_EXTRAPOLATE) != 0u ? fraction : 1.0 - fraction;
        uint width, height;
        g_shift.GetDimensions(width, height);
        const int2 own_texel = min(int2(pixel >> 1), int2(width, height) - 1);
        float2 back = ShiftBack(own_texel);
        // FG_FLAG_SHIFT_SMOOTH needs the depth the map was built from (not
        // bound for a frame moved by the previous frame's motion).
        const uint smooth = FG_FLAG_SHIFT_SMOOTH | FG_FLAG_DEPTH;
        [branch]
        if ((g.options.y & smooth) == smooth) {
            back = SmoothShift(uv, own_texel, back, float2(width, height));
        }
        uv += time / max(map_time, 1e-6) * back / float2(width, height);
    }
#endif
    return uv;
}

#if !defined(__spirv__)
// FG_FLAG_DEPTH_SPLAT: the motion map's offset at uv back to where its
// content is in the newest frame (uv units), false where it has none. A map
// built for another frame of the same real frame (FrameGenTuning::
// engine_map_reuse: its time in options.w's high half; 0, this frame's own)
// is scaled to `fraction`.
bool MotionMapBack(float2 uv, float fraction, out float2 back) {
    uint width, height;
    g_motion.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const uint packed = g_motion.Load(int3(min(int2(uv * size), int2(size) - 1), 0));
    const uint built = g.options.w >> 16;
    const float scale = built != 0u ? fraction / max(f16tof32(built), 1e-6) : 1.0;
    back = scale * float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16)) / size;
    return packed != FG_SHIFT_NONE;
}

// FG_FLAG_ENGINE_VISIBILITY (stage 8): whether the previous frame did not see
// the content the newest frame shows at newest_uv, by its flow cell's label:
// the game's vector taken there put it out of the previous frame's view or
// behind something nearer (FG_ENGINE_LABEL_UNSEEN), and the content lies on
// the surface that vector was taken for (FG_ENGINE_MAP_SURFACE: a cell at an
// edge holds the surface in front too). False for cells the vectors did not
// move.
bool EngineUnseen(float2 newest_uv) {
    if (any(newest_uv < 0.0) || any(newest_uv > 1.0)) {
        return false;
    }
    uint width, height;
    g_motion.GetDimensions(width, height);
    const uint label = g_motion.Load(int3(min(int2(newest_uv * float2(width, height)), int2(width, height) - 1), 0));
    const float surface = f16tof32(label >> 16);
    const float depth = DepthAt(newest_uv);
    return (label & FG_ENGINE_LABEL_UNSEEN) != 0u && surface > 0.0 && depth > 0.0 &&
           abs(log(min(depth, 0.5 * FG_FAR_DEPTH) / surface)) <= FG_ENGINE_MAP_SURFACE;
}
#endif

// One generated pixel at `fraction` (see FrameGenTuning::fractions);
// `position` as for SourceUv.
float4 Synthesize(float2 position, float fraction) {
    const float2 uv = SourceUv(position, fraction);
    const float2 aspect = float2(1.0, float(g.size.y) / float(g.size.x));
    const bool extrapolate = (g.options.y & FG_FLAG_EXTRAPOLATE) != 0u;
    const bool still = (g.options.y & FG_FLAG_STILL) != 0u;
    const float limit = max(g.tuning.y, 1e-4);
    bool boundary = false;

    // Content at x in the newest frame sits at x - (1 - f) * v at the
    // interpolated time, or at x + a * v when extrapolating; this pixel reads
    // the newest frame at uv + step * v. Flow moves content one interval at
    // most: past it (the engine motion map's FrameGenTuning::engine_reach)
    // only the map's models go on.
    const float step = extrapolate ? -min(fraction, 1.0) : 1.0 - fraction;
    float4 chosen = float4(0.0, 0.0, 0.0, 1.0);
    float chosen_error = 0.0;
    int occlusion = 0; // 1: newest frame alone, 2: previous frame alone (Occlusion)
    float2 occlusion_v = float2(0.0, 0.0);
    float occlusion_depth = 0.0;
    // The engine's merged map (FG_FLAG_DEPTH_SPLAT with FG_FLAG_ENGINE, stage
    // 7): where it has an entry, the content there by its model or its flow
    // (the motion limit applied to the flow by the map) and the eye's offset,
    // outright; elsewhere (no depth or depth dropped, which the eye-shift map
    // leaves too) the field at the pixel, without an eye shift.
    bool mapped = false;
#if !defined(__spirv__)
    const uint merged = FG_FLAG_DEPTH_SPLAT | FG_FLAG_ENGINE | FG_FLAG_DEPTH;
    if (!still && (g.options.y & merged) == merged) {
        float2 back;
        mapped = MotionMapBack(uv, fraction, back);
        if (mapped) {
            chosen = float4(back / step, 0.02, 1.0);
        }
    }
#endif
    if (!still && !mapped) {
        // FG_FLAG_STALE_FLOW: the field is laid out in the previous real
        // frame's view; it is read where this pixel's content was there (head
        // rotation alone: depth is not measured yet either).
        float2 flow_offset = float2(0.0, 0.0);
        if ((g.options.y & FG_FLAG_STALE_FLOW) != 0u) {
            float2 mapped;
            if (MapToPrevious(uv, 0.0, mapped)) {
                flow_offset = mapped - uv;
            }
        }
        // Candidates: the smooth field solved with a fixed-point iteration,
        // the field at this pixel, the four surrounding block vectors, and
        // zero motion. Later candidates carry a small bias so noise alone
        // never pulls a pixel off the smooth field.
        float4 candidates[7];
        // Where each candidate's motion was measured, in the newest frame:
        // its depth orders surfaces (FG_FLAG_OCCLUSION).
        float candidate_depth[7];
        const float4 here = g_flow.SampleLevel(g_linear, uv + flow_offset, 0.0);
        float4 solved = g_flow.SampleLevel(g_linear, uv + step * here.xy + flow_offset, 0.0);
        // Where the solved field is read: the depth it was measured at
        // (FG_FLAG_DEPTH_MATCH).
        const float2 solved_uv = uv + step * solved.xy + flow_offset;
        solved = g_flow.SampleLevel(g_linear, solved_uv, 0.0);
        candidates[0] = solved;
        candidates[1] = here;
        uint flow_width, flow_height;
        g_flow.GetDimensions(flow_width, flow_height);
        const int2 limit_cell = int2(flow_width, flow_height) - 1;
        const int2 base = int2(floor((uv + flow_offset) * float2(flow_width, flow_height) - 0.5));
        [unroll]
        for (int i = 0; i < 4; ++i) {
            const int2 cell = clamp(base + int2(i & 1, i >> 1), int2(0, 0), limit_cell);
            candidates[2 + i] = g_flow.Load(int3(cell, 0));
            candidate_depth[2 + i] = DepthAt((float2(cell) + 0.5) / float2(flow_width, flow_height));
        }
        candidates[6] = float4(0.0, 0.0, here.z, 1.0);
        candidate_depth[0] = candidate_depth[1] = candidate_depth[6] = DepthAt(uv);
        chosen = candidates[0];
        // Smooth motion, most of a frame, leaves nothing to choose between;
        // only pixels near a motion boundary, where the surrounding block
        // vectors disagree, pay for the comparisons. Zero motion is not one
        // of those vectors: counting it made every moving pixel choose on
        // its own, and sharp repeating edges (windows, railings, cockpit
        // trim) tore apart.
        float spread = 0.0;
        [unroll]
        for (int s = 1; s < 6; ++s) {
            spread = max(spread, length((candidates[s].xy - chosen.xy) * float2(g.size.xy)));
        }
        boundary = spread > 0.5;
        [branch]
        if (boundary) {
            chosen_error = CandidateError(uv, chosen.xy, step);
            // With FG_FLAG_DEPTH_MATCH the choice also weighs where each
            // vector was measured against the depth of what it carries here
            // (zero motion carries this pixel's own content: no cost). The
            // cost only chooses: chosen_error stays the frames' agreement.
            const bool match = (g.options.y & (FG_FLAG_DEPTH_MATCH | FG_FLAG_DEPTH)) == (FG_FLAG_DEPTH_MATCH | FG_FLAG_DEPTH);
            float chosen_score = chosen_error;
            if (match) {
                chosen_score += DepthMismatch(DepthAt(solved_uv - flow_offset), DepthAt(uv + step * chosen.xy));
            }
            [unroll]
            for (int k = 1; k < 7; ++k) {
                const float error = CandidateError(uv, candidates[k].xy, step) + (k == 6 ? 0.03 : 0.01);
                float score = error;
                if (match && k < 6) {
                    score += DepthMismatch(candidate_depth[k], DepthAt(uv + step * candidates[k].xy));
                }
                if (score < chosen_score) {
                    chosen_score = score;
                    chosen_error = error;
                    chosen = candidates[k];
                }
            }
            if (!extrapolate && (g.options.y & (FG_FLAG_OCCLUSION | FG_FLAG_DEPTH)) == (FG_FLAG_OCCLUSION | FG_FLAG_DEPTH)) {
                if (chosen_error > FG_OCCLUSION_ERROR) {
                    occlusion = Occlusion(uv, step, candidates, candidate_depth, occlusion_v, occlusion_depth);
                }
#if !defined(__spirv__)
                // FG_FLAG_ENGINE_VISIBILITY: content the game's vectors say
                // the previous frame did not see is shown from the newest
                // frame alone where the two frames agree too (what was
                // revealed can look like what stood in front of it).
                else if ((g.options.y & FG_FLAG_ENGINE_VISIBILITY) != 0u && EngineUnseen(uv + step * chosen.xy)) {
                    occlusion = 1;
                    occlusion_v = chosen.xy;
                }
                // FG_FLAG_ENGINE_BIDIR: where the newest frame does not show
                // the chosen vector's content moving that way (the vector
                // carries something else here) and the forward map has the
                // previous frame's content here, that is being covered.
                else if ((g.options.y & FG_FLAG_ENGINE_BIDIR) != 0u && !SeesMoving(uv + step * chosen.xy, chosen.xy)) {
                    float2 back;
                    occlusion = MotionMapBack(uv, fraction, back) ? 2 : 0;
                }
#endif
            }
        }
    }

#if !defined(__spirv__)
    // FG_FLAG_DEPTH_SPLAT: the motion map says which surface with a motion
    // model is nearest here at this frame's time. Where it is in front of
    // what the chosen vector carries (something nearer covers the pixel),
    // or, extrapolating, where the chosen vector carries a surface with a
    // model that the map put elsewhere (uncovered: the map holds what lies
    // behind), the map's motion is shown, matched or not. Surfaces without
    // a model (4-30 m, sky) keep the chosen vector. The engine's separate map
    // (stage 7) is read so too, and past one interval (FrameGenTuning::
    // engine_reach) its entries are shown outright, as the merged map's: only
    // its models go on. The merged map was read above.
    if (!still && (g.options.y & (FG_FLAG_DEPTH_SPLAT | FG_FLAG_DEPTH | FG_FLAG_ENGINE)) == (FG_FLAG_DEPTH_SPLAT | FG_FLAG_DEPTH)) {
        float2 back;
        if (MotionMapBack(uv, fraction, back)) {
            const float surface = OrderDepth(DepthAt(uv + back));
            const float carried_depth = DepthAt(uv + step * chosen.xy);
            const float carried = OrderDepth(carried_depth);
            const bool modelled = carried_depth > 0.0 &&
                                  (carried < FG_SPLAT_HEAD ||
                                   (carried >= FG_SPLAT_WORLD && (FG_SPLAT_SKY != 0 || carried < 0.5 * FG_FAR_DEPTH)));
            if (fraction > 1.0 || surface < 0.97 * carried || (extrapolate && modelled && carried < 0.97 * surface)) {
                chosen = float4(back / step, 0.02, 1.0);
                chosen_error = 0.0;
                occlusion = 0;
                boundary = false;
            }
        }
    }
#endif

    // The motion limit guards flow, not the eye's movement: the merged map's
    // entries hold the eye's offset, and the map limited their flow itself.
    const float shift = length(step * chosen.xy * aspect);
    const float motion_confidence = mapped ? 1.0 : saturate((limit - shift) / (0.25 * limit));
    const float cost_confidence = saturate(1.0 - max(chosen.z - 0.02, 0.0) * g.tuning.z);
    const float agreement = saturate(1.0 - max(chosen_error - 0.03, 0.0) * g.tuning.w);
    // Uncertain motion shrinks toward a still image instead of blending two
    // positions, which would ghost. w: how steady the motion is (FG_PASS_FILTER;
    // 1 unless extrapolating).
    float confidence = motion_confidence * cost_confidence * chosen.w;
    if (extrapolate && boundary) {
        // Extrapolation sees only the newest frame at display time, so at a
        // motion boundary a vector the two real frames disagree on (newly
        // revealed or hidden content) is not trusted either. Inside smooth
        // motion this per-pixel check only reacts to sampling sharp edges
        // between texels; applying it there shifted edges less than the
        // surfaces around them.
        confidence *= agreement;
    }

    const float2 source = uv + confidence * step * chosen.xy;
    // FG_FLAG_RESAMPLE_* (research): the real frames' colour by Catmull-Rom;
    // a still frame is a copy and keeps the bilinear tap.
    const bool cubic = !still && (g.options.y & FG_FLAG_RESAMPLE_CUBIC) != 0u;
    const bool cubic_previous = cubic && (g.options.y & FG_FLAG_RESAMPLE_PREVIOUS) != 0u;
    const float4 current = SampleColor(g_current, source, cubic);
    float4 result = current;
    float photometric_confidence = 1.0;
    if (occlusion == 1) {
        // Revealed (or in front): the newest frame alone.
        result = SampleColor(g_current, uv + confidence * step * occlusion_v, cubic);
    } else if (occlusion == 2) {
        // Being covered: the previous frame alone, where the content was
        // (FG_FLAG_ENGINE_BIDIR: where the forward map puts it, else by the
        // farthest candidate's motion).
        float2 previous_uv;
        bool forward = false;
#if !defined(__spirv__)
        float2 back;
        forward = (g.options.y & FG_FLAG_ENGINE_BIDIR) != 0u && MotionMapBack(uv, fraction, back);
        if (forward) {
            result = SampleColor(g_previous, uv + back, cubic_previous);
        }
#endif
        if (!forward && MapToPrevious(uv - confidence * fraction * occlusion_v, occlusion_depth, previous_uv)) {
            result = SampleColor(g_previous, previous_uv, cubic_previous);
        }
    } else if (!extrapolate && !still) {
        float2 previous_uv;
        if (MapToPrevious(source - confidence * chosen.xy, DepthAt(source), previous_uv)) {
            const float4 previous = SampleColor(g_previous, previous_uv, cubic_previous);
            const float difference = MaxComponent(abs(Perceptual(previous.rgb) - Perceptual(current.rgb)));
            photometric_confidence = saturate(1.0 - max(difference - 0.03, 0.0) * g.tuning.w);
            result = lerp(current, previous, (1.0 - fraction) * photometric_confidence);
            if ((g.options.y & FG_FLAG_CROSSFADE) != 0u) {
                // Research: untrusted motion fades between the two real
                // frames where they are, instead of showing the newest one.
                float2 still_uv;
                const float4 newest_here = SampleColor(g_current, uv, cubic);
                float4 previous_here = newest_here;
                if (MapToPrevious(uv, DepthAt(uv), still_uv)) {
                    previous_here = SampleColor(g_previous, still_uv, cubic_previous);
                }
                const float4 fade = lerp(newest_here, previous_here, 1.0 - fraction);
                const float4 paired = lerp(current, previous, 1.0 - fraction);
                result = lerp(fade, paired, confidence * photometric_confidence);
            }
        }
    } else if (extrapolate && boundary) {
        photometric_confidence = agreement;
    }
    const float4 flow_here = chosen;

    const uint view = g.options.z;
    if (view == FG_VIEW_FLOW) {
        const float2 v = flow_here.xy * aspect;
        float magnitude = saturate(length(v) * 20.0);
        if ((g.options.y & FG_FLAG_FLOW_FINE) != 0u) {
            // In pixels: full at 2, the blend's 0.6 from half a pixel.
            magnitude = saturate(length(flow_here.xy * float2(g.size.xy)) * 0.5);
        }
        if (magnitude > 0.0) {
            // atan2(0, 0) is undefined on some GPUs; zero motion keeps the image.
            const float3 tint = Hue(frac(atan2(v.y, v.x) / 6.2831853 + 1.0)) * magnitude;
            result.rgb = lerp(result.rgb, tint, 0.6 * saturate(magnitude * 4.0));
        }
    } else if (view == FG_VIEW_CONFIDENCE) {
        const float trust = confidence * photometric_confidence;
        result.rgb = lerp(result.rgb, float3(1.0, 0.1, 0.05), 0.75 * (1.0 - trust));
#if !defined(__spirv__)
        if ((g.options.y & (FG_FLAG_ENGINE | FG_FLAG_DEPTH_SPLAT)) == FG_FLAG_ENGINE) {
            // What moved the content's flow cell: the game's vectors green
            // (the nearest surface's) or cyan (the farthest's), searched or no
            // motion blue, vectors cleared yellow, depth disproved magenta; the
            // measured flow keeps its colours, and a frame whose verdict held
            // the vectors back is shown as measured.
            uint label_width, label_height;
            g_motion.GetDimensions(label_width, label_height);
            const uint label = g_motion.Load(
                int3(min(int2(uv * float2(label_width, label_height)), int2(label_width, label_height) - 1), 0));
            const uint source = (label & FG_ENGINE_LABEL_INVALID) != 0u ? 0u : label & FG_ENGINE_LABEL_SOURCE;
            const float3 tints[7] = {float3(0.0, 0.0, 0.0), float3(0.1, 1.0, 0.2), float3(0.1, 0.9, 1.0),
                                     float3(0.2, 0.3, 1.0), float3(0.2, 0.3, 1.0), float3(1.0, 0.9, 0.1),
                                     float3(1.0, 0.2, 0.9)};
            if (source != 0u && source < 7u) {
                result.rgb = lerp(result.rgb, tints[source], 0.35);
            }
        }
#endif
    } else if (view == FG_VIEW_TINT) {
        result.rgb *= float3(1.0, 0.8, 1.0);
    }

    if ((g.options.y & FG_FLAG_SRGB_ENCODE) != 0u) {
        result.rgb = LinearToSrgb(result.rgb);
    }
    return result;
}

#if defined(FG_PASS_SYNTHESIZE_DRAW)
// Direct3D 12 renders the generated frames of a view straight into the
// output swapchain images in one draw: no intermediate image and no copy,
// and the frames share their flow reads. FG_DRAW_OUTPUTS render targets,
// fractions in tuning.x, then size.z, size.w and options.x (as float bits;
// synthesis reads none of those otherwise). An sRGB render target encodes
// in hardware, so the encode flag is off.
struct DrawOutput {
    float4 frame0 : SV_Target0;
#if FG_DRAW_OUTPUTS > 1
    float4 frame1 : SV_Target1;
#endif
#if FG_DRAW_OUTPUTS > 2
    float4 frame2 : SV_Target2;
#endif
#if FG_DRAW_OUTPUTS > 3
    float4 frame3 : SV_Target3;
#endif
};

DrawOutput PSMain(float4 position : SV_Position) {
    const float2 pixel = position.xy;
    DrawOutput output;
    output.frame0 = Synthesize(pixel, g.tuning.x);
#if FG_DRAW_OUTPUTS > 1
    output.frame1 = Synthesize(pixel, asfloat(g.size.z));
#endif
#if FG_DRAW_OUTPUTS > 2
    output.frame2 = Synthesize(pixel, asfloat(g.size.w));
#endif
#if FG_DRAW_OUTPUTS > 3
    output.frame3 = Synthesize(pixel, asfloat(g.options.x));
#endif
    return output;
}

// One triangle covering the viewport.
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#else
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    g_output[id.xy] = Synthesize(float2(id.xy) + 0.5, g.tuning.x);
}
#endif
#endif

#if defined(FG_PASS_COMPOSE_DRAW)
// Quad views (Direct3D 12): one eye of the composed image from its
// peripheral view (t0) and focus view (t1), as the layer's Direct3D 11 quad
// views compositor composes them, drawn by FG_PASS_SYNTHESIZE_DRAW's VSMain.
// Both are read through sRGB views and written to an sRGB target: blending is
// linear. fov_current: the focus rectangle (left, top, right, bottom) in
// output uv; fov_previous.xy: the blend band in output uv, .z: sharpening
// (0-1); rot0.zw: one output pixel in focus uv; size.xy: the output extent.
// options.x (FG_COMPOSE_OVERLAY): the layer's quad views diagnostic overlay
// is drawn over the eye (Overlay).
FG_BIND(0) Texture2D<float4> g_peripheral : register(t0);
FG_BIND(1) Texture2D<float4> g_focus : register(t1);
FG_BIND(6) SamplerState g_linear : register(s0);

#define FG_COMPOSE_OVERLAY 1u
#define FG_COMPOSE_OVERLAY_EYE_TRACKING 2u
#define FG_COMPOSE_OVERLAY_GAZE 4u

// The overlay's shapes, as the Direct3D 11 compositor's (RectOutlineMask,
// CrossMask, RingMask, SegmentMask): 1 on the shape, fading out over the
// next output pixel; sizes in output pixels, tuning.xy one output pixel in uv.
float2 OverlayPixel() {
    return max(g.tuning.xy, float2(0.000001, 0.000001));
}

float OverlayRectOutline(float2 uv, float2 rect_min, float2 rect_max, float thickness) {
    const float2 pixel = OverlayPixel();
    const float2 center = (rect_min + rect_max) * 0.5;
    const float2 half_size = max((rect_max - rect_min) * 0.5 / pixel, float2(0.0, 0.0));
    const float2 point_pixels = (uv - center) / pixel;
    const float2 q = abs(point_pixels) - half_size;
    const float signed_distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
    return 1.0 - smoothstep(thickness, thickness + 1.0, abs(signed_distance));
}

float OverlayCross(float2 uv, float2 center, float length_pixels, float thickness) {
    const float2 delta = abs((uv - center) / OverlayPixel());
    const float horizontal = (1.0 - smoothstep(thickness, thickness + 1.0, delta.y)) *
                             (1.0 - smoothstep(length_pixels, length_pixels + 1.0, delta.x));
    const float vertical = (1.0 - smoothstep(thickness, thickness + 1.0, delta.x)) *
                           (1.0 - smoothstep(length_pixels, length_pixels + 1.0, delta.y));
    return max(horizontal, vertical);
}

float OverlayRing(float2 uv, float2 center, float radius, float thickness) {
    const float2 delta = (uv - center) / OverlayPixel();
    return 1.0 - smoothstep(thickness, thickness + 1.0, abs(length(delta) - radius));
}

float OverlaySegment(float2 uv, float2 start, float2 end, float thickness) {
    const float2 pixel = OverlayPixel();
    const float2 offset = (uv - start) / pixel;
    const float2 segment = (end - start) / pixel;
    const float along = saturate(dot(offset, segment) / max(dot(segment, segment), 0.0001));
    return 1.0 - smoothstep(thickness, thickness + 1.0, length(offset - segment * along));
}

// The layer's quad views diagnostic overlay over the composed colour, exactly
// as its Direct3D 11 compositor draws it: the periphery tinted blue, the
// blend band orange and the inset green; the inset's outline (green, red
// while eye tracking has no gaze) and the band's inner edge (amber); the
// head's centre (white cross), the configured offset (magenta ring, joined
// to the centre); with a gaze, the raw gaze (cyan ring) and the smoothed one
// (yellow cross). rot1: the gaze markers, rot2: the centre and the offset.
// Drawn last, into the output image alone: nothing measures motion there.
float3 Overlay(float3 composed, float2 uv, bool inside, float alpha) {
    const float4 rect = g.fov_current;
    const float2 feather = g.fov_previous.xy;
    const float eye_tracking = (g.options.x & FG_COMPOSE_OVERLAY_EYE_TRACKING) != 0u ? 1.0 : 0.0;
    const float gaze = (g.options.x & FG_COMPOSE_OVERLAY_GAZE) != 0u ? 1.0 : 0.0;

    const bool transition = inside && alpha < 0.999;
    const float3 zone = !inside ? float3(0.08, 0.28, 0.95) : transition ? float3(1.0, 0.55, 0.04) : float3(0.08, 0.85, 0.28);
    composed = lerp(composed, zone, !inside ? 0.13 : transition ? 0.20 : 0.055);

    const float tracking_unavailable = eye_tracking * (1.0 - gaze);
    const float3 outline_color = lerp(float3(0.15, 1.0, 0.3), float3(1.0, 0.08, 0.05), tracking_unavailable);
    composed = lerp(composed, outline_color, OverlayRectOutline(uv, rect.xy, rect.zw, 2.0));

    const float2 inner_min = min(rect.xy + feather, rect.zw);
    const float2 inner_max = max(rect.zw - feather, rect.xy);
    const float inner = OverlayRectOutline(uv, inner_min, inner_max, 1.25) * step(g.tuning.x * 2.0, feather.x) *
                        step(g.tuning.y * 2.0, feather.y);
    composed = lerp(composed, float3(1.0, 0.65, 0.05), inner * 0.9);

    const float4 reference = g.rot2;
    const float2 offset_corner = float2(reference.z, reference.y);
    const float offset_axes = max(OverlaySegment(uv, reference.xy, offset_corner, 1.0),
                                  OverlaySegment(uv, offset_corner, reference.zw, 1.0));
    composed = lerp(composed, float3(1.0, 0.1, 0.85), offset_axes * 0.8);
    composed = lerp(composed, float3(1.0, 1.0, 1.0), OverlayCross(uv, reference.xy, 14.0, 1.5));
    composed = lerp(composed, float3(1.0, 0.1, 0.85), OverlayRing(uv, reference.zw, 12.0, 1.5));
    composed = lerp(composed, float3(0.0, 0.95, 1.0), OverlayRing(uv, g.rot1.xy, 7.0, 1.75) * gaze);
    composed = lerp(composed, float3(1.0, 0.95, 0.05), OverlayCross(uv, g.rot1.zw, 8.0, 1.5) * gaze);
    return composed;
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    // The pixel's centre: the Direct3D 11 compositor's interpolated uv.
    const float2 uv = saturate(position.xy / float2(g.size.xy));
    const float4 rect = g.fov_current;
    float4 composed = g_peripheral.SampleLevel(g_linear, uv, 0.0);
    const bool inside = all(uv >= rect.xy) && all(uv <= rect.zw);
    float alpha = 0.0;
    if (inside) {
        const float2 to_edge = min(uv - rect.xy, rect.zw - uv);
        alpha = saturate(min(to_edge.x / max(g.fov_previous.x, 0.0001), to_edge.y / max(g.fov_previous.y, 0.0001)));
        alpha = alpha * alpha * (3.0 - 2.0 * alpha);
        const float2 focus_uv = saturate((uv - rect.xy) / max(rect.zw - rect.xy, float2(0.0001, 0.0001)));
        float4 focus = g_focus.SampleLevel(g_linear, focus_uv, 0.0);
        // Sharpening fades in with the focus image, sampling neighbours one
        // output pixel apart: the detail that survives the resample into the
        // rectangle.
        const float sharpen = saturate(g.fov_previous.z) * alpha;
        if (sharpen > 0.001) {
            const float2 off = g.rot0.zw;
            const float3 n = g_focus.SampleLevel(g_linear, saturate(focus_uv + float2(0.0, -off.y)), 0.0).rgb;
            const float3 s = g_focus.SampleLevel(g_linear, saturate(focus_uv + float2(0.0, off.y)), 0.0).rgb;
            const float3 e = g_focus.SampleLevel(g_linear, saturate(focus_uv + float2(off.x, 0.0)), 0.0).rgb;
            const float3 w = g_focus.SampleLevel(g_linear, saturate(focus_uv + float2(-off.x, 0.0)), 0.0).rgb;
            const float3 average = (n + s + e + w) * 0.25;
            const float3 low = min(focus.rgb, min(min(n, s), min(e, w)));
            const float3 high = max(focus.rgb, max(max(n, s), max(e, w)));
            const float3 range = max(high - low, float3(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0));
            focus.rgb = saturate(focus.rgb + clamp((focus.rgb - average) * (sharpen * 2.0), -range * 0.5, range * 0.5));
        }
        composed = lerp(composed, focus, alpha);
    }
    [branch]
    if ((g.options.x & FG_COMPOSE_OVERLAY) != 0u) {
        composed.rgb = Overlay(composed.rgb, uv, inside, alpha);
    }
    return composed;
}
#endif

#if defined(FG_PASS_DEPTH)
FG_BIND(0) Texture2DArray<float4> g_app_depth : register(t0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_linear_depth : register(u0);

// The application's depth to linear view depth (meters) on the view's
// half-resolution grid. fov_current holds the depth region (offset, extent
// in depth texels), options.w its array slice, tuning the encoding:
// 1 / z = x + normalized * y, normalized = (raw - z) * w.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    const int2 texel = int2(g.fov_current.xy + min(uv * g.fov_current.zw, g.fov_current.zw - 1.0));
    const float raw = g_app_depth.Load(int4(texel, int(g.options.w), 0)).r;
    const float normalized = saturate((raw - g.tuning.z) * g.tuning.w);
    const float inverse = g.tuning.x + normalized * g.tuning.y;
    g_linear_depth[id.xy] = inverse > 1.0 / FG_FAR_DEPTH ? 1.0 / inverse : FG_FAR_DEPTH;
}
#endif

#if defined(FG_PASS_DEPTH_CHECK) || defined(FG_PASS_DEPTH_TRUST) || defined(FG_PASS_TRUST_UPDATE) || \
    defined(FG_PASS_DEPTH_SCALE)
// Depth within this share of a texel's own across the 3x3 texels its
// evidence patch covers: the patch shows one surface.
#define FG_FLAT_SHARE 0.03

// Whether the evidence patch around `texel` lies on one surface. A patch
// straddling a silhouette matches neither reading, near or far (the
// background behind the edge differs between the eyes and between frames):
// judged anyway, the edges of the tablet and the panels were "disproved",
// left behind as the rest moved with the eye, and stretched into the gaps
// beside them.
bool FlatDepth(Texture2D<float4> depth, int2 texel, float center) {
    uint width, height;
    depth.GetDimensions(width, height);
    const int2 limit = int2(width, height) - 1;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        const float neighbour = abs(depth.Load(int3(clamp(texel + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), limit), 0)).r);
        if (abs(neighbour - center) > FG_FLAT_SHARE * center) {
            return false;
        }
    }
    return true;
}

// Error of a sample whose reading throws it out of the other eye's view.
#define FG_CHECK_OUTSIDE 0.25
// Depth must beat "infinitely far" by this much luma error to count as
// right; within it, noise decides.
#define FG_CHECK_MARGIN 0.02
#define FG_CHECK_TAPS 9

// Luma of a 3x3 patch around uv, `spacing` apart in uv.
void LoadPatch(Texture2D<float4> image, SamplerState linear_sampler, float2 uv, float2 spacing,
               out float patch[FG_CHECK_TAPS]) {
    [unroll]
    for (int y = 0; y < 3; ++y) {
        [unroll]
        for (int x = 0; x < 3; ++x) {
            const float2 offset = float2(x - 1, y - 1) * spacing;
            patch[y * 3 + x] = Luma(image.SampleLevel(linear_sampler, uv + offset, 0.0).rgb);
        }
    }
}

// Mean luma error between `reference` and the other eye's patch at uv.
float PatchError(Texture2D<float4> image, SamplerState linear_sampler, float2 uv, float2 spacing,
                 float reference[FG_CHECK_TAPS]) {
    float sum = 0.0;
    [unroll]
    for (int y = 0; y < 3; ++y) {
        [unroll]
        for (int x = 0; x < 3; ++x) {
            const float2 offset = float2(x - 1, y - 1) * spacing;
            sum += abs(Luma(image.SampleLevel(linear_sampler, uv + offset, 0.0).rgb) - reference[y * 3 + x]);
        }
    }
    return sum / float(FG_CHECK_TAPS);
}

// The lowest PatchError at uv and one texel (`step`) either side along the
// line content moves on with depth: depth and poses a texel off still line
// the views up.
float LineError(Texture2D<float4> image, SamplerState linear_sampler, float2 uv, float2 step, float2 spacing,
                float reference[FG_CHECK_TAPS]) {
    float best = PatchError(image, linear_sampler, uv, spacing, reference);
    best = min(best, PatchError(image, linear_sampler, uv - step, spacing, reference));
    return min(best, PatchError(image, linear_sampler, uv + step, spacing, reference));
}

// How clearly a view lines up with the depth (positive, up to 1) or as
// infinitely far (negative): the errors' difference over the larger one.
// Within FG_CHECK_MARGIN noise decides, so there is no evidence.
float Evidence(float near_error, float far_error) {
    const float difference = far_error - near_error;
    return abs(difference) < FG_CHECK_MARGIN
               ? 0.0
               : clamp(difference / max(max(near_error, far_error), 0.04), -1.0, 1.0);
}

// Evidence for `depth` at uv from `other`, an image the mapping
// (MapToPrevious) takes uv into: the other eye, or the previous frame.
// Zero where depth and "infinitely far" land within a texel of each other
// or outside `other`.
float DepthEvidence(Texture2D<float4> own, Texture2D<float4> other, SamplerState linear_sampler, float2 uv, float depth,
                    float2 size) {
    float evidence = 0.0;
    float2 near_uv, far_uv;
    if (MapToPrevious(uv, depth, near_uv) && MapToPrevious(uv, FG_FAR_DEPTH, far_uv)) {
        const float2 apart = (near_uv - far_uv) * size;
        if (dot(apart, apart) >= 1.0) {
            const float2 spacing = 1.0 / size;
            const float2 step = normalize(apart) * spacing;
            float reference[FG_CHECK_TAPS];
            LoadPatch(own, linear_sampler, uv, spacing, reference);
            evidence = Evidence(LineError(other, linear_sampler, near_uv, step, spacing, reference),
                                LineError(other, linear_sampler, far_uv, step, spacing, reference));
        }
    }
    return evidence;
}
#endif

#if defined(FG_PASS_DEPTH_CHECK)
FG_BIND(0) Texture2D<float4> g_view0 : register(t0);
FG_BIND(1) Texture2D<float4> g_view1 : register(t1);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(3) Texture2D<float4> g_depth_other : register(t3);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_scores : register(u0);

// One sample's evidence for a reading of its depth: (error with fallback,
// error without depth, 1, 1 if it fell back), or zero where the reading
// cannot be told apart from no depth. A sample that falls back costs what
// synthesis then gets there, the error without depth.
float4 Score(float2 uv, float depth, float2 far_uv, float none, float2 spacing, float2 size,
             float reference[FG_CHECK_TAPS]) {
    float2 mapped;
    const bool inside = MapToPrevious(uv, depth, mapped);
    if (inside && all(abs(mapped - far_uv) * size < 1.0)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float error = inside ? PatchError(g_view1, g_linear, mapped, spacing, reference) : FG_CHECK_OUTSIDE;
    const bool fallback = error > none + FG_CHECK_MARGIN;
    return float4(fallback ? none : error, none, 1.0, fallback ? 1.0 : 0.0);
}

// One cell of the depth check: view 0 samples warped into view 1 by their
// depth read two ways (as decoded; near and far swapped), each compared with
// no depth. fov_current/previous and the rotation rows describe view 0 ->
// view 1; tuning.x is 1/near + 1/far, tuning.y the scale the depth was
// decoded with, tuning.z the scale probes' step (natural log); options.w
// samples per cell side.
//
// Scale probes: games describe their depth's near plane wrongly too (MSFS
// 2024's lines the eyes up 1.2 times further than described), which scales
// every depth and makes head-movement compensation over- or under-correct
// near content. Where the decoded depth moves a sample and lines it up
// better than no depth, the other eye's error is also measured with the
// depth scaled by exp(step * (j - 2)), j = 0..4, for FrameGenDepthValidator
// to search the scale.
//
// Only samples both eyes see count (the part of a wide field of view one
// eye sees alone says nothing), and each reading is judged where it matters,
// where it moves the sample by more than a texel from no depth. There a
// reading may fall back to "infinitely far" per sample, as synthesis does
// (FG_PASS_DEPTH_TRUST): depth wrong in places (cockpit glass written to
// depth) does not hide that it is right elsewhere.
// Writes five texels per cell: a Score sum per reading, then (samples both
// eyes see, how many of them are nearer than FG_CHECK_NEAR meters, 1 if view
// 0's depth differs anywhere in the cell from its depth at the view's centre,
// the same for view 1's depth: t3, unbound reads 0), then the probes' error
// sums (j = 0..3) and (j = 4, probed samples, 0, 0). A game that leaves an
// eye's depth clear (Star Citizen's left) varies in no cell, however the
// depth is decoded (FrameGenDepthEyes).
#define FG_CHECK_NEAR 10.0
#define FG_CHECK_PROBES 5
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    uint width, height;
    g_depth.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float2 spacing = 1.0 / size;
    const int samples = int(g.options.w);
    float4 decoded = float4(0.0, 0.0, 0.0, 0.0);
    float4 swapped = float4(0.0, 0.0, 0.0, 0.0);
    float binocular = 0.0;
    float cockpit = 0.0;
    float probes[FG_CHECK_PROBES] = {0.0, 0.0, 0.0, 0.0, 0.0};
    float probed = 0.0;
    uint other_width, other_height;
    g_depth_other.GetDimensions(other_width, other_height);
    const float2 other_size = float2(max(other_width, 1u), max(other_height, 1u));
    const float centre = abs(g_depth.Load(int3(int2(size * 0.5), 0)).r);
    const float other_centre = abs(g_depth_other.Load(int3(int2(other_size * 0.5), 0)).r);
    bool varied = false;
    bool other_varied = false;
    [loop]
    for (int sy = 0; sy < samples; ++sy) {
        [loop]
        for (int sx = 0; sx < samples; ++sx) {
            const float2 uv = (float2(id.xy) + (float2(sx, sy) + 0.5) / float(samples)) / float2(g.size.xy);
            const int2 own_texel = clamp(int2(uv * size), int2(0, 0), int2(size) - 1);
            const int2 other_texel = clamp(int2(uv * other_size), int2(0, 0), int2(other_size) - 1);
            varied = varied || abs(g_depth.Load(int3(own_texel, 0)).r) != centre;
            other_varied = other_varied || abs(g_depth_other.Load(int3(other_texel, 0)).r) != other_centre;
            float2 far_uv;
            if (!MapToPrevious(uv, FG_FAR_DEPTH, far_uv)) {
                continue;
            }
            binocular += 1.0;
            const int2 texel = clamp(int2(uv * size), int2(0, 0), int2(size) - 1);
            // Negative where synthesis already disproved it: the check reads
            // the depth itself.
            const float depth = max(abs(g_depth.Load(int3(texel, 0)).r), 1e-6);
            cockpit += depth < FG_CHECK_NEAR ? 1.0 : 0.0;
            // The swapped reading of the depth as the game describes it.
            const float swapped_inverse = g.tuning.x - g.tuning.y / depth;
            float reference[FG_CHECK_TAPS];
            LoadPatch(g_view0, g_linear, uv, spacing, reference);
            const float none = PatchError(g_view1, g_linear, far_uv, spacing, reference);
            const float4 score = Score(uv, depth, far_uv, none, spacing, size, reference);
            decoded += score;
            swapped += Score(uv, swapped_inverse > 1.0 / FG_FAR_DEPTH ? 1.0 / swapped_inverse : FG_FAR_DEPTH, far_uv,
                             none, spacing, size, reference);
            if (depth < FG_CHECK_NEAR && score.z > 0.0 && score.w == 0.0) {
                probed += 1.0;
                [unroll]
                for (int j = 0; j < FG_CHECK_PROBES; ++j) {
                    float2 mapped;
                    const bool inside = MapToPrevious(uv, depth * exp(g.tuning.z * float(j - 2)), mapped);
                    probes[j] += inside ? PatchError(g_view1, g_linear, mapped, spacing, reference) : FG_CHECK_OUTSIDE;
                }
            }
        }
    }
    const int2 base = int2(id.x * 5, id.y);
    g_scores[base] = decoded;
    g_scores[base + int2(1, 0)] = swapped;
    g_scores[base + int2(2, 0)] = float4(binocular, cockpit, varied ? 1.0 : 0.0, other_varied ? 1.0 : 0.0);
    g_scores[base + int2(3, 0)] = float4(probes[0], probes[1], probes[2], probes[3]);
    g_scores[base + int2(4, 0)] = float4(probes[4], probed, 0.0, 0.0);
}
#endif

#if defined(FG_PASS_DEPTH_CROSS_SPLAT)
FG_BIND(0) Texture2D<float4> g_depth : register(t0);
FG_BIND(4) FG_FORMAT("r32ui") RWTexture2D<uint> g_keys : register(u0);

// One eye's depth for the other, for a game that fills only one eye's
// (FrameGenDepthEyes). options.x 0 clears the keys (the target eye's grid,
// size.zw); 1 moves each texel of the source eye's linear depth (its grid,
// size.xy) to where the target eye sees it (fov_current/previous and the
// rotation rows: source -> target, position in .w) and keeps the nearest
// (largest 1/z, as unsigned bits) on the 2x2 texels around it. Distant
// content moves by rotation alone and claims its texel as far (key 1), so
// sky beside the cockpit stays sky.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (g.options.x == 0u) {
        if (all(id.xy < g.size.zw)) {
            g_keys[id.xy] = 0u;
        }
        return;
    }
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float depth = g_depth.Load(int3(id.xy, 0)).r;
    const bool near = depth > 0.0 && depth < FG_FAR_DEPTH;
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    const float3 direction = float3(lerp(g.fov_current.x, g.fov_current.y, uv.x),
                                    lerp(g.fov_current.z, g.fov_current.w, uv.y), -1.0);
    float3 moved = float3(dot(g.rot0.xyz, direction), dot(g.rot1.xyz, direction), dot(g.rot2.xyz, direction));
    if (near) {
        // The point is direction * depth; in the target view R p + t, so
        // R d + t / depth scaled by depth (its depth there: -z * depth).
        moved += float3(g.rot0.w, g.rot1.w, g.rot2.w) / depth;
    }
    if (moved.z > -1e-4) {
        return;
    }
    const float2 tangent = moved.xy / -moved.z;
    const float2 target_uv = float2((tangent.x - g.fov_previous.x) / (g.fov_previous.y - g.fov_previous.x),
                                    (tangent.y - g.fov_previous.z) / (g.fov_previous.w - g.fov_previous.z));
    const float2 landing = target_uv * float2(g.size.zw);
    if (!near) {
        const int2 texel = int2(floor(landing));
        if (all(texel >= 0) && all(texel < int2(g.size.zw))) {
            InterlockedMax(g_keys[texel], 1u);
        }
        return;
    }
    const uint key = max(asuint(1.0 / (-moved.z * depth)), 2u);
    const int2 first = int2(floor(landing - 0.5));
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const int2 texel = first + int2(i & 1, i >> 1);
        if (all(texel >= 0) && all(texel < int2(g.size.zw))) {
            InterlockedMax(g_keys[texel], key);
        }
    }
}
#endif

#if defined(FG_PASS_DEPTH_CROSS_RESOLVE)
FG_BIND(0) Texture2D<uint> g_keys : register(t0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_linear_depth : register(u0);

// The target eye's linear depth from FG_PASS_DEPTH_CROSS_SPLAT's keys: the
// nearest content that landed on each texel (key 1: distant). A texel
// nothing landed on (uncovered beside near content, or outside the source
// eye's view) takes the farthest of its neighbours that something landed on,
// the background an edge uncovers; else it is as far.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    uint key = g_keys.Load(int3(id.xy, 0));
    if (key == 0u) {
        uint farthest = 0xffffffffu;
        [unroll]
        for (int i = 0; i < 9; ++i) {
            const int2 texel = clamp(int2(id.xy) + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), int2(g.size.xy) - 1);
            const uint value = g_keys.Load(int3(texel, 0));
            if (value != 0u) {
                farthest = min(farthest, value);
            }
        }
        key = farthest == 0xffffffffu ? 0u : farthest;
    }
    g_linear_depth[id.xy] = key > 1u ? 1.0 / asfloat(key) : FG_FAR_DEPTH;
}
#endif

#if defined(FG_PASS_FLOW_PACK)
FG_BIND(0) Texture2D<float4> g_luma_current : register(t0);
FG_BIND(1) Texture2D<float4> g_luma_previous : register(t1);
FG_BIND(4) FG_FORMAT("r8") RWTexture2D<float> g_hardware_current : register(u0);
FG_BIND(5) FG_FORMAT("r8") RWTexture2D<float> g_hardware_previous : register(u1);

// The half-resolution luma pair (the previous frame already in the newest
// view) as 8-bit images for the optical flow hardware. Previous texels
// outside the previous view take the newest frame's: no motion there.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float current = g_luma_current.Load(int3(id.xy, 0)).r;
    const float previous = g_luma_previous.Load(int3(id.xy, 0)).r;
    g_hardware_current[id.xy] = saturate(current);
    g_hardware_previous[id.xy] = saturate(previous < 0.0 ? current : previous);
}
#endif

#if defined(FG_PASS_FLOW_PACK_RAW)
FG_BIND(0) Texture2D<float4> g_color : register(t0);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("r8") RWTexture2D<float> g_raw : register(u0);

// A frame's half-resolution luma as it was rendered, head movement and all,
// for the optical flow hardware to match against the previous frame's as
// soon as the game releases the image (FG_PASS_FLOW_UNPACK then takes the
// head movement out with the poses).
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    g_raw[id.xy] = saturate(Luma(g_color.SampleLevel(g_linear, uv, 0.0).rgb));
}
#endif

#if defined(FG_PASS_FLOW_UNPACK) || defined(FG_PASS_ENGINE_REDUCE) || defined(FG_PASS_ENGINE_CONVERT) || \
    defined(FG_SHIFT_ENGINE)
// The inverse of MapToPrevious: where a previous-view coordinate lies in the
// newest view, for content at newest-view depth `depth` (rotation only
// without it). False behind the newest view.
bool MapToNewest(float2 previous_uv, float depth, out float2 newest_uv) {
    const float3 seen = float3(lerp(g.fov_previous.x, g.fov_previous.y, previous_uv.x),
                               lerp(g.fov_previous.z, g.fov_previous.w, previous_uv.y), -1.0);
    // R transposed: newest-view directions from previous-view ones.
    const float3 c0 = float3(g.rot0.x, g.rot1.x, g.rot2.x);
    const float3 c1 = float3(g.rot0.y, g.rot1.y, g.rot2.y);
    const float3 c2 = float3(g.rot0.z, g.rot1.z, g.rot2.z);
    float3 located = float3(dot(c0, seen), dot(c1, seen), dot(c2, seen));
    if ((g.options.y & FG_FLAG_DEPTH) != 0u && depth > 0.0 && depth < FG_FAR_DEPTH && abs(located.z) > 1e-6) {
        // The point is s * seen in the previous view and R^T (s seen - t) in
        // the newest, at depth `depth` there.
        const float3 t = float3(g.rot0.w, g.rot1.w, g.rot2.w);
        const float3 back = float3(dot(c0, t), dot(c1, t), dot(c2, t));
        located = (back.z - depth) / located.z * located - back;
    }
    newest_uv = float2(0.0, 0.0);
    if (located.z > -1e-4) {
        return false;
    }
    const float2 tangent = located.xy / -located.z;
    newest_uv = float2((tangent.x - g.fov_current.x) / (g.fov_current.y - g.fov_current.x),
                       (tangent.y - g.fov_current.z) / (g.fov_current.w - g.fov_current.z));
    return true;
}
#endif

#if defined(FG_PASS_ENGINE_REDUCE) || defined(FG_PASS_ENGINE_CONVERT)
// Whether a raw vector at half-resolution uv (resolved depth z; size.zw the
// half-resolution extent) is absent: the camera (options.x from bit 4) has
// the head's movement in the vectors, and it is an exact zero where the
// head's rotation moves far content (FG_ENGINE_ABSENT_DEPTH) by more than
// FG_ENGINE_ABSENT_PIXELS: the game cleared it.
bool EngineAbsent(float2 raw, float2 uv, float z, uint camera) {
    if (camera != 0u || raw.x != 0.0 || raw.y != 0.0 || (z > 0.0 && z < FG_ENGINE_ABSENT_DEPTH)) {
        return false;
    }
    float2 previous_uv;
    MapToPrevious(uv, 0.0, previous_uv);
    return length((uv - previous_uv) * 2.0 * float2(g.size.zw)) > FG_ENGINE_ABSENT_PIXELS;
}
#endif

#if defined(FG_PASS_FLOW_UNPACK)
FG_BIND(0) Texture2D<int2> g_hardware_flow : register(t0);
FG_BIND(1) Texture2D<float4> g_luma_current : register(t1);
FG_BIND(2) Texture2D<float4> g_luma_previous : register(t2);
FG_BIND(3) Texture2D<float4> g_depth : register(t3);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_flow : register(u0);

// options.x: the hardware matched the frames as rendered (FG_PASS_FLOW_PACK_RAW).
#define FG_UNPACK_RAW 1u

// The hardware's vectors (S10.5 half-resolution pixels, from the newest
// frame to the previous one) as flow (previous to newest, normalized), each
// scored like block matching scores its vectors (FG_PASS_REFINE): the mean
// luma difference of the block of options.w x options.w half-resolution
// texels it carries, so synthesis trusts both kinds of flow alike. Vectors
// between frames as rendered (FG_UNPACK_RAW) first lose the head movement:
// the previous position they point at is mapped back into the newest view.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    uint width, height;
    g_luma_current.GetDimensions(width, height);
    const int2 extent = int2(width, height);
    const int scale = int(g.options.w);
    float2 d = -float2(g_hardware_flow.Load(int3(id.xy, 0)).xy) / 32.0;
    if ((g.options.x & FG_UNPACK_RAW) != 0u) {
        const float2 centre = (float2(id.xy) * float(scale) + 0.5 * float(scale)) / float2(extent);
        float2 newest_uv;
        d = MapToNewest(centre - d / float2(extent), DepthNearest(g_depth, centre), newest_uv)
                ? (centre - newest_uv) * float2(extent)
                : float2(0.0, 0.0);
    }
    const int2 offset = int2(round(d));
    float sum = 0.0;
    float count = 0.0;
    // The cost of no motion over the texels the previous frame saw, as
    // FG_PASS_REFINE scores it (w).
    float zero_sum = 0.0;
    float zero_count = 0.0;
    [loop]
    for (int y = 0; y < scale; ++y) {
        [loop]
        for (int x = 0; x < scale; ++x) {
            const int2 q = int2(id.xy) * scale + int2(x, y);
            if (any(q >= extent)) {
                continue;
            }
            const float current = g_luma_current.Load(int3(q, 0)).r;
            const int2 s = q - offset;
            const float previous = any(s < 0) || any(s >= extent) ? -1.0 : g_luma_previous.Load(int3(s, 0)).r;
            sum += previous < 0.0 ? FG_INVALID_PENALTY : abs(current - previous);
            count += 1.0;
            const float still = g_luma_previous.Load(int3(q, 0)).r;
            if (still >= 0.0) {
                zero_sum += abs(current - still);
                zero_count += 1.0;
            }
        }
    }
    const float cost = sum / max(count, 1.0) + FG_MOTION_PENALTY * length(float2(offset) / float2(extent));
    const float zero = zero_count >= 0.5 * max(count, 1.0) ? zero_sum / zero_count : -1.0;
    g_flow[id.xy] = float4(d / float2(extent), cost, zero);
}
#endif

#if defined(FG_PASS_DEPTH_TRUST)
FG_BIND(0) Texture2D<float4> g_own : register(t0);
FG_BIND(1) Texture2D<float4> g_other : register(t1);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_evidence : register(u0);

// Per texel of one view's linear depth: does the other eye see what the
// depth says it should (DepthEvidence)? Negative where the content lines up
// clearly better as infinitely far: scenery behind cockpit glass the game
// wrote depth for, or depth that is wrong there. Zero where the other eye
// cannot tell (seen by one eye only, or as near and far alike).
// FG_PASS_TRUST_UPDATE turns it into the texel's trust score. Silhouettes
// say nothing (FlatDepth).
// fov_current/previous and the rotation rows describe this view -> the other.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float depth = g_depth.Load(int3(id.xy, 0)).r;
    const float2 size = float2(g.size.xy);
    g_evidence[id.xy] = depth > 0.0 && depth < FG_FAR_DEPTH && FlatDepth(g_depth, int2(id.xy), depth)
                            ? DepthEvidence(g_own, g_other, g_linear, (float2(id.xy) + 0.5) / size, depth, size)
                            : 0.0;
}
#endif

#if defined(FG_PASS_DEPTH_SCALE)
FG_BIND(0) Texture2D<float4> g_own : register(t0);
FG_BIND(1) Texture2D<float4> g_other : register(t1);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_scales : register(u0);

// Depth nearer than this is probed: further out, a scale barely moves a
// sample between the eyes.
#define FG_SCALE_NEAR 10.0
// A patch needs this much luma range to tell scales apart.
#define FG_SCALE_CONTRAST 0.04

// Per FG_TRUST_TILE x FG_TRUST_TILE texels of one view's depth: how well the
// other eye lines up with textured samples on one surface at each probed
// scale of their depth (kDepthScaleProbes). The game's depth is not always
// the depth of what shows: MSFS 2024 writes a box at the glareshield's depth
// over the EFB tablet in front of it (and over the scenery behind), and
// content moved with the eye by the box's depth landed pixels out of place.
// Writes three texels per tile: the error sums for probes 0-3, then 4-6 and
// the samples, then the samples' summed inverse depth.
// fov_current/previous and the rotation rows describe this view -> the
// other (as FG_PASS_DEPTH_TRUST); size.xy: the tiles.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    uint width, height;
    g_depth.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float2 spacing = 1.0 / size;
    float errors[FG_SCALE_PROBES] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    float samples = 0.0;
    float inverse = 0.0;
    [loop]
    for (uint s = 0; s < 8; ++s) {
        // Every other texel of every other row, every other row's half
        // offset by one: a checkerboard of eight.
        const uint2 cell = uint2(s % 4u, s / 4u);
        const int2 texel = int2(id.xy) * FG_TRUST_TILE + int2(cell.x * 2u + (cell.y & 1u), cell.y * 4u + 1u);
        if (texel.x >= int(width) || texel.y >= int(height)) {
            continue;
        }
        const float depth = g_depth.Load(int3(texel, 0)).r;
        if (!(depth > 0.0) || depth >= FG_SCALE_NEAR) {
            continue;
        }
        const float2 uv = (float2(texel) + 0.5) / size;
        float reference[FG_CHECK_TAPS];
        LoadPatch(g_own, g_linear, uv, spacing, reference);
        float low = reference[0];
        float high = reference[0];
        [unroll]
        for (int t = 1; t < FG_CHECK_TAPS; ++t) {
            low = min(low, reference[t]);
            high = max(high, reference[t]);
        }
        float2 near_uv, far_uv;
        if (high - low < FG_SCALE_CONTRAST || !FlatDepth(g_depth, texel, depth) || !MapToPrevious(uv, depth, near_uv) ||
            !MapToPrevious(uv, FG_FAR_DEPTH, far_uv)) {
            continue;
        }
        const float2 apart = (near_uv - far_uv) * size;
        if (dot(apart, apart) < 1.0) {
            continue;
        }
        const float2 step = normalize(apart) * spacing;
        [unroll]
        for (int j = 0; j < FG_SCALE_PROBES; ++j) {
            float2 mapped;
            errors[j] += MapToPrevious(uv, depth * kDepthScaleProbes[j], mapped)
                             ? LineError(g_other, g_linear, mapped, step, spacing, reference)
                             : FG_CHECK_OUTSIDE;
        }
        samples += 1.0;
        inverse += 1.0 / depth;
    }
    const int2 base = int2(id.x * 3, id.y);
    g_scales[base] = float4(errors[0], errors[1], errors[2], errors[3]);
    g_scales[base + int2(1, 0)] = float4(errors[4], errors[5], errors[6], samples);
    g_scales[base + int2(2, 0)] = float4(inverse, 0.0, 0.0, 0.0);
}
#endif

#if defined(FG_PASS_DEPTH_SCALE_VOTE)
FG_BIND(0) Texture2D<float4> g_scales : register(t0);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_votes : register(u0);

// options.x: the second stage, accepting the first stage's votes (t0).
#define FG_SCALE_ACCEPT 1u
// A tile's scale stands where at least this many of the eight tiles around
// agree (a scale within FG_SCALE_AGREE of it in log, on the same surface):
// a lone tile's pick on a seat's edge is noise.
#define FG_SCALE_NEIGHBOURS 4
#define FG_SCALE_AGREE 0.08

// Tiles either way the vote sums, those on the surface: sampled inverse
// depth within this share of the tile's.
#define FG_SCALE_RADIUS 5
#define FG_SCALE_SURFACE 0.1
// A scale other than the depth as read wins with this many samples, at
// most this share of the as-read error, and this much less per sample.
#define FG_SCALE_SAMPLES 12.0
#define FG_SCALE_GAIN 0.6
#define FG_SCALE_MARGIN 0.01

// Second stage: a tile's vote where enough of its neighbours agree, else 1.
float4 Accept(int2 centre) {
    const float4 vote = g_scales.Load(int3(centre, 0));
    if (vote.x == 1.0 || !(vote.x > 0.0)) {
        return float4(1.0, vote.yzw);
    }
    int agree = 0;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        const int2 other = centre + int2(i % 3 - 1, i / 3 - 1);
        if (i == 4 || any(other < 0) || any(other >= int2(g.size.xy))) {
            continue;
        }
        const float4 near = g_scales.Load(int3(other, 0));
        agree += near.x > 0.0 && abs(log(near.x / vote.x)) <= FG_SCALE_AGREE &&
                         abs(near.y - vote.y) <= FG_SCALE_SURFACE * vote.y
                     ? 1
                     : 0;
    }
    return agree >= FG_SCALE_NEIGHBOURS ? vote : float4(1.0, vote.yzw);
}

// Per tile, the stereo probe (FG_PASS_DEPTH_SCALE) of the tiles around it
// on its surface summed: (the scale its depth lines up best at, 1 when the
// depth as read does or nothing tells; the surface's inverse depth; the
// samples). A tile without samples of its own takes its neighbours'
// surface. Then (FG_SCALE_ACCEPT) the votes enough neighbours agree with.
// size.xy: the tiles.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int2 centre = int2(id.xy);
    if ((g.options.x & FG_SCALE_ACCEPT) != 0u) {
        g_votes[id.xy] = Accept(centre);
        return;
    }
    float reference = 0.0;
    {
        const float own = g_scales.Load(int3(centre.x * 3 + 1, centre.y, 0)).w;
        float samples = own;
        float inverse = own > 0.0 ? g_scales.Load(int3(centre.x * 3 + 2, centre.y, 0)).x : 0.0;
        if (own <= 0.0) {
            [loop]
            for (int y = -FG_SCALE_RADIUS; y <= FG_SCALE_RADIUS; ++y) {
                [loop]
                for (int x = -FG_SCALE_RADIUS; x <= FG_SCALE_RADIUS; ++x) {
                    const int2 tile = centre + int2(x, y);
                    if (any(tile < 0) || any(tile >= int2(g.size.xy))) {
                        continue;
                    }
                    samples += g_scales.Load(int3(tile.x * 3 + 1, tile.y, 0)).w;
                    inverse += g_scales.Load(int3(tile.x * 3 + 2, tile.y, 0)).x;
                }
            }
        }
        reference = samples > 0.0 ? inverse / samples : 0.0;
    }
    float errors[FG_SCALE_PROBES] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    float samples = 0.0;
    [loop]
    for (int y = -FG_SCALE_RADIUS; y <= FG_SCALE_RADIUS; ++y) {
        [loop]
        for (int x = -FG_SCALE_RADIUS; x <= FG_SCALE_RADIUS; ++x) {
            const int2 tile = centre + int2(x, y);
            if (reference <= 0.0 || any(tile < 0) || any(tile >= int2(g.size.xy))) {
                continue;
            }
            const float4 low = g_scales.Load(int3(tile.x * 3, tile.y, 0));
            const float4 high = g_scales.Load(int3(tile.x * 3 + 1, tile.y, 0));
            const float inverse = g_scales.Load(int3(tile.x * 3 + 2, tile.y, 0)).x;
            if (high.w <= 0.0 || abs(inverse / high.w - reference) > FG_SCALE_SURFACE * reference) {
                continue;
            }
            errors[0] += low.x;
            errors[1] += low.y;
            errors[2] += low.z;
            errors[3] += low.w;
            errors[4] += high.x;
            errors[5] += high.y;
            errors[6] += high.z;
            samples += high.w;
        }
    }
    int best = FG_SCALE_AS_READ;
    [unroll]
    for (int j = 0; j < FG_SCALE_PROBES; ++j) {
        best = errors[j] < errors[best] ? j : best;
    }
    const float as_read = errors[FG_SCALE_AS_READ];
    const bool better = samples >= FG_SCALE_SAMPLES && errors[best] <= FG_SCALE_GAIN * as_read &&
                        as_read - errors[best] >= FG_SCALE_MARGIN * samples;
    float scale = 1.0;
    if (better) {
        // Between the probes either side (a parabola through the three
        // errors, in log scale): a surface 0.76 times its depth is not
        // picked as 0.7 here and 0.8 next door.
        float position = 0.0;
        if (best > 0 && best < FG_SCALE_PROBES - 1) {
            const float below = errors[best - 1];
            const float above = errors[best + 1];
            const float curve = below - 2.0 * errors[best] + above;
            position = curve > 0.0 ? clamp(0.5 * (below - above) / curve, -0.5, 0.5) : 0.0;
        }
        const int towards = position < 0.0 ? max(best - 1, 0) : min(best + 1, FG_SCALE_PROBES - 1);
        scale = exp(lerp(log(kDepthScaleProbes[best]), log(kDepthScaleProbes[towards]), abs(position)));
    }
    g_votes[id.xy] = float4(scale, reference, samples, 0.0);
}
#endif

#if defined(FG_PASS_TRUST_UPDATE)
FG_BIND(0) Texture2D<float4> g_newest : register(t0);
FG_BIND(1) Texture2D<float4> g_previous : register(t1);
FG_BIND(2) Texture2D<float4> g_trust_before : register(t2);
FG_BIND(3) Texture2D<float4> g_depth : register(t3);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_trust : register(u0);
FG_BIND(5) FG_FORMAT("rgba32f") RWTexture2D<float4> g_tiles : register(u1);

groupshared float4 g_tile_sums[FG_TRUST_TILE * FG_TRUST_TILE];

#define FG_TRUST_STEREO 1u  // g_trust holds this frame's stereo evidence (FG_PASS_DEPTH_TRUST)
#define FG_TRUST_HISTORY 2u // g_trust_before is last frame's score
#define FG_TRUST_MOTION 4u  // g_previous is the previous real frame
// Last frame's score carries over at this weight; stereo and motion
// evidence add theirs.
#define FG_TRUST_KEEP 0.8
#define FG_TRUST_STEREO_WEIGHT 0.6
#define FG_TRUST_MOTION_WEIGHT 0.4

// Each depth texel's trust score, from three sources:
//   - the other eye (FG_PASS_DEPTH_TRUST), where both eyes see it;
//   - the previous frame: head movement between the frames moves content
//     at its depth differently from infinitely far content, so cockpit
//     detail confirms its depth where one eye sees it alone (scenery that
//     moves on its own lines up neither way and says nothing);
//   - last frame's score at the texel's previous position: cockpit and
//     glass stay where they are in the cockpit, so the depth maps a texel
//     to where it was.
// FG_PASS_TRUST_RESOLVE then drops the depth regions score below -threshold.
// Each thread group also counts its tile (u1) for the surface vote
// (FG_PASS_TRUST_VOTE): texels disproved (below -threshold) and confirmed
// (above +threshold), and the mean inverse depth of each kind.
// options.x: FG_TRUST_* flags; the mapping describes newest -> previous.
[numthreads(FG_TRUST_TILE, FG_TRUST_TILE, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex) {
    const bool inside = all(id.xy < g.size.xy);
    const float depth = inside ? g_depth.Load(int3(id.xy, 0)).r : 0.0;
    float score = 0.0;
    if (inside && depth > 0.0 && depth < FG_FAR_DEPTH) {
        const float2 size = float2(g.size.xy);
        const float2 uv = (float2(id.xy) + 0.5) / size;
        const float stereo = (g.options.x & FG_TRUST_STEREO) != 0u ? g_trust[id.xy] : 0.0;
        const float motion = (g.options.x & FG_TRUST_MOTION) != 0u && FlatDepth(g_depth, int2(id.xy), depth)
                                 ? DepthEvidence(g_newest, g_previous, g_linear, uv, depth, size)
                                 : 0.0;
        float carried = 0.0;
        float2 before_uv;
        if ((g.options.x & FG_TRUST_HISTORY) != 0u && MapToPrevious(uv, depth, before_uv)) {
            const int2 texel = clamp(int2(before_uv * size), int2(0, 0), int2(g.size.xy) - 1);
            carried = g_trust_before.Load(int3(texel, 0)).r;
        }
        score = clamp(FG_TRUST_KEEP * carried + FG_TRUST_STEREO_WEIGHT * stereo + FG_TRUST_MOTION_WEIGHT * motion,
                      -1.0, 1.0);
    }
    if (inside) {
        g_trust[id.xy] = score;
    }
    float4 sums = float4(0.0, 0.0, 0.0, 0.0);
    if (inside && depth > 0.0 && depth < FG_FAR_DEPTH) {
        sums = score < -FG_TRUST_THRESHOLD ? float4(1.0, 0.0, 1.0 / depth, 0.0)
               : score > FG_TRUST_THRESHOLD ? float4(0.0, 1.0, 0.0, 1.0 / depth)
                                            : sums;
    }
    g_tile_sums[index] = sums;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = FG_TRUST_TILE * FG_TRUST_TILE / 2; stride > 0; stride /= 2) {
        if (index < stride) {
            g_tile_sums[index] += g_tile_sums[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        const float4 total = g_tile_sums[0];
        g_tiles[group.xy] =
            float4(total.xy, total.x > 0.0 ? total.z / total.x : 0.0, total.y > 0.0 ? total.w / total.y : 0.0);
    }
}
#endif

#if defined(FG_PASS_TRUST_VOTE)
FG_BIND(0) Texture2D<float4> g_tiles : register(t0);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_votes : register(u0);

// Per tile, the tile counts (FG_PASS_TRUST_UPDATE) around it summed: texels
// disproved and confirmed, and the mean inverse depth of each kind, for
// FG_PASS_TRUST_RESOLVE to judge texels without evidence of their own by.
// size.xy: the tiles.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    float4 sums = float4(0.0, 0.0, 0.0, 0.0);
    [loop]
    for (int y = -FG_VOTE_RADIUS; y <= FG_VOTE_RADIUS; ++y) {
        [loop]
        for (int x = -FG_VOTE_RADIUS; x <= FG_VOTE_RADIUS; ++x) {
            const int2 tile = int2(id.xy) + int2(x, y);
            if (any(tile < 0) || any(tile >= int2(g.size.xy))) {
                continue;
            }
            const float4 counts = g_tiles.Load(int3(tile, 0));
            sums += float4(counts.xy, counts.x * counts.z, counts.y * counts.w);
        }
    }
    g_votes[id.xy] = float4(sums.xy, sums.x > 0.0 ? sums.z / sums.x : 0.0, sums.y > 0.0 ? sums.w / sums.y : 0.0);
}
#endif

#if defined(FG_PASS_TRUST_RESOLVE)
FG_BIND(0) Texture2D<float4> g_scores : register(t0);
FG_BIND(1) Texture2D<float4> g_votes : register(t1);
FG_BIND(2) Texture2D<float4> g_scale_votes : register(t2);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_depth : register(u0);

// Neighbours within this share of a texel's depth are its surface.
#define FG_RESOLVE_SURFACE 0.03
#define FG_RESOLVE_RADIUS 2
// The share of the surface around a disproved texel (FG_RESOLVE_RADIUS
// texels either way) that must be disproved too.
#define FG_RESOLVE_SHARE 0.6
// The surface vote (FG_PASS_TRUST_VOTE) around a texel: a kind counts at
// inverse depth within this share of the texel's; the disproved decide
// where they outnumber the confirmed and cover this share of the vote's
// area (a surface of them, not a few specks: plain cockpit texels near a
// handful kept their depth); the confirmed where they outnumber the
// disproved this many times.
#define FG_VOTE_DEPTH 0.15
#define FG_VOTE_AREA 0.25
#define FG_VOTE_CONFIRMED 8.0
// The accepted stereo scale votes apply to texels within this share of
// their surface's inverse depth.
#define FG_RESOLVE_SCALE_SURFACE 0.1

// A texel keeps its depth: corrected where the accepted stereo scale votes
// (FG_PASS_DEPTH_SCALE_VOTE, options.x FG_RESOLVE_SCALE) of its surface line
// the eyes up better at another scale of its depth. The tiles around blend
// in (in log, weighted 1-2-1 by distance; tiles that keep the depth as
// read count as 1), so where a correction stops, a flat surface stretches
// over a few tiles instead of tearing at a tile's edge.
void Keep(uint2 texel, float depth) {
    if ((g.options.x & FG_RESOLVE_SCALE) == 0u) {
        return;
    }
    uint width, height;
    g_scale_votes.GetDimensions(width, height);
    // The four tiles whose centres surround the texel, and the ring
    // around them: a tent over tile centres two tiles wide.
    const float2 position = (float2(texel) + 0.5) / float(FG_TRUST_TILE) - 0.5;
    const int2 corner = int2(floor(position));
    const float2 within = position - float2(corner);
    const float inverse = 1.0 / depth;
    float sum = 0.0;
    float weight = 0.0;
    bool any_scaled = false;
    [unroll]
    for (int i = 0; i < 16; ++i) {
        const int2 offset = int2(i % 4, i / 4) - 1;
        const int2 tile = corner + offset;
        if (any(tile < 0) || tile.x >= int(width) || tile.y >= int(height)) {
            continue;
        }
        const float4 vote = g_scale_votes.Load(int3(tile, 0));
        if (!(vote.x > 0.0) || abs(vote.y - inverse) > FG_RESOLVE_SCALE_SURFACE * inverse) {
            continue;
        }
        const float2 distance = abs(float2(offset) - within);
        const float w = max(0.0, 1.0 - 0.5 * distance.x) * max(0.0, 1.0 - 0.5 * distance.y);
        sum += w * log(vote.x);
        weight += w;
        any_scaled = any_scaled || vote.x != 1.0;
    }
    if (any_scaled && weight > 0.0) {
        g_depth[texel] = depth * exp(sum / weight);
    }
}

// Drops the depth (negated: head movement is compensated for rotation only
// there, and the content does not move with the eye) of
//   - texels scored below -threshold (FG_PASS_TRUST_UPDATE) where most of
//     the surface around them is too: cockpit glass the game writes depth
//     for is disproved over its whole surface; a texel or a thin line of
//     noise among confirmed ones is not glass, and dropping it tore it out
//     of the panel around it. Nor are specks on a surface the vote confirms
//     (left behind, they were pixels out of place on MSFS's displays);
//   - texels without clear evidence of their own on a surface the vote
//     around them disproves: plain scenery behind glass the game wrote
//     depth for (an invisible box over MSFS's glareshield) tells near and
//     far apart nowhere, kept its depth in islands among the disproved
//     texels and moved with the eye while they did not, jittering. Texels
//     between confirmed ones (the plain ones between a label's letters)
//     keep theirs.
// The rest keep theirs, corrected by the stereo scale vote (Keep).
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float depth = g_depth[id.xy];
    const float score = g_scores.Load(int3(id.xy, 0)).r;
    if (!(depth > 0.0) || depth >= FG_FAR_DEPTH) {
        return;
    }
    if (score > FG_TRUST_THRESHOLD) {
        Keep(id.xy, depth);
        return;
    }
    const float4 vote = g_votes.Load(int3(id.xy / uint(FG_TRUST_TILE), 0));
    const float inverse = 1.0 / depth;
    const float disproved_votes = abs(vote.z - inverse) <= FG_VOTE_DEPTH * inverse ? vote.x : 0.0;
    const float confirmed_votes = abs(vote.w - inverse) <= FG_VOTE_DEPTH * inverse ? vote.y : 0.0;
    const float side = float((2 * FG_VOTE_RADIUS + 1) * FG_TRUST_TILE);
    const bool surface_disproved =
        disproved_votes >= FG_VOTE_AREA * side * side && disproved_votes >= confirmed_votes;
    if (score >= -FG_TRUST_THRESHOLD) {
        if (surface_disproved) {
            g_depth[id.xy] = -depth;
        } else {
            Keep(id.xy, depth);
        }
        return;
    }
    if (confirmed_votes >= FG_VOTE_CONFIRMED * max(disproved_votes, 1.0)) {
        Keep(id.xy, depth);
        return;
    }
    if (surface_disproved) {
        g_depth[id.xy] = -depth;
        return;
    }
    const int2 limit = int2(g.size.xy) - 1;
    float surface = 0.0;
    float disproved = 0.0;
    [loop]
    for (int y = -FG_RESOLVE_RADIUS; y <= FG_RESOLVE_RADIUS; ++y) {
        [loop]
        for (int x = -FG_RESOLVE_RADIUS; x <= FG_RESOLVE_RADIUS; ++x) {
            const int2 texel = clamp(int2(id.xy) + int2(x, y), int2(0, 0), limit);
            // Other threads may have negated it already.
            const float neighbour = abs(g_depth[texel]);
            if (abs(neighbour - depth) <= FG_RESOLVE_SURFACE * depth) {
                surface += 1.0;
                disproved += g_scores.Load(int3(texel, 0)).r < -FG_TRUST_THRESHOLD ? 1.0 : 0.0;
            }
        }
    }
    if (disproved >= FG_RESOLVE_SHARE * surface) {
        g_depth[id.xy] = -depth;
    } else {
        Keep(id.xy, depth);
    }
}
#endif

#if defined(FG_PASS_SHIFT_CLEAR)
FG_BIND(4) FG_FORMAT("r32ui") RWTexture2D<uint> g_target : register(u0);
FG_BIND(5) FG_FORMAT("r32ui") RWTexture2D<uint> g_shift : register(u1);

// Keys to nothing reached; the map to FG_SHIFT_NONE (FG_PASS_SHIFT_RESOLVE
// keeps the smallest offset written). Direct3D 12 clears the two in
// hardware instead (the same values).
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (all(id.xy < g.size.xy)) {
        g_target[id.xy] = 0u;
        g_shift[id.xy] = FG_SHIFT_NONE;
    }
}
#endif

#if defined(FG_PASS_DEPTH_STALE)
FG_BIND(0) Texture2D<float4> g_depth_previous : register(t0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_depth_stale : register(u0);

// A frame moved by the previous real frame's motion (FG_FLAG_STALE_FLOW)
// before its own depth is resolved: the previous frame's resolved depth,
// read where each half-resolution texel's content was in that frame
// (through the head's rotation, then its movement at the depth found
// there), for the frame's eye-shift map. Negative (dropped) depth stays so;
// texels outside the previous view have none (as far).
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    float depth = 0.0;
    float2 mapped;
    if (MapToPrevious(uv, 0.0, mapped)) {
        depth = DepthNearest(g_depth_previous, mapped);
        float2 moved;
        if (depth > 0.0 && MapToPrevious(uv, depth, moved)) {
            depth = DepthNearest(g_depth_previous, moved);
        }
    }
    g_depth_stale[id.xy] = depth;
}
#endif

#if defined(FG_PASS_SHIFT_SPLAT) || defined(FG_PASS_SHIFT_RESOLVE) || defined(FG_PASS_SHIFT_FILL)
// FG_FLAG_DEPTH_SPLAT: the newest frame's filtered flow (FG_PASS_FILTER).
FG_BIND(1) Texture2D<float4> g_motion_flow : register(t1);
FG_BIND(6) SamplerState g_linear : register(s0);

bool MotionMap() {
    return (g.options.y & FG_FLAG_DEPTH_SPLAT) != 0u;
}

#if defined(FG_SHIFT_ENGINE)
// Stage 7: the engine motion map (FG_PASS_SHIFT_*_ENGINE); options.x: input 3
// holds the real frame's world motion, the map is merged (the eye's offset in
// rot*.w, as the eye-shift map's, added to each texel's motion). Stage 8
// (research): the forward map instead (ForwardLanding).
#define FG_ENGINE_MAP_WORLD 1u
#define FG_ENGINE_MAP_MERGED 2u
#define FG_ENGINE_MAP_FORWARD 4u
#endif
#endif

#if defined(FG_PASS_SHIFT_SPLAT) || defined(FG_PASS_SHIFT_RESOLVE)
FG_BIND(0) Texture2D<float4> g_depth : register(t0);
#if defined(FG_SHIFT_ENGINE)
// The engine motion map's: the real frame's world motion (FG_PASS_WORLD_SOLVE's
// or FG_PASS_WORLD_SET's texels: 0 the rotation w and 1 when valid, 1 the
// translation t, 2 y 1 where it is the application's own movement), with
// FG_ENGINE_MAP_WORLD; its flow cells' labels (FG_PASS_ENGINE_MERGE).
Texture2D<float4> g_engine_world : register(t3);
Texture2D<uint> g_engine_labels : register(t4);
#endif

// The motion map's world motion at uv: the filtered flow of the cells
// around whose content is world too (bilinear among those), so the
// cockpit's stillness beside it does not slow it; the nearest cell's where
// none is. `sky`: of the sky's cells instead (FG_SPLAT_SKY).
float2 WorldMotion(float2 uv, bool sky) {
    uint width, height;
    g_motion_flow.GetDimensions(width, height);
    const float2 cells = float2(width, height);
    const float2 position = uv * cells - 0.5;
    const int2 base = int2(floor(position));
    const float2 f = position - float2(base);
    float2 sum = float2(0.0, 0.0);
    float weight = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const int2 cell = clamp(base + int2(i & 1, i >> 1), int2(0, 0), int2(cells) - 1);
        const float depth = DepthNearest(g_depth, (float2(cell) + 0.5) / cells);
        const float w = ((i & 1) != 0 ? f.x : 1.0 - f.x) * ((i >> 1) != 0 ? f.y : 1.0 - f.y);
        if (sky ? depth >= 0.5 * FG_FAR_DEPTH : depth >= FG_SPLAT_WORLD && depth < 0.5 * FG_FAR_DEPTH) {
            sum += w * g_motion_flow.Load(int3(cell, 0)).xy;
            weight += w;
        }
    }
    return weight > 0.0 ? sum / weight
                        : g_motion_flow.Load(int3(clamp(int2(uv * cells), int2(0, 0), int2(cells) - 1), 0)).xy;
}

#if defined(FG_SHIFT_ENGINE)
// The class of a flow cell's vector where the game's vectors moved it (its
// label's: FG_ENGINE_CLASS_*), else 0.
uint EngineClass(uint label) {
    const uint source = label & FG_ENGINE_LABEL_SOURCE;
    return (label & FG_ENGINE_LABEL_INVALID) == 0u && (source == FG_ENGINE_LABEL_NEAR || source == FG_ENGINE_LABEL_FAR)
               ? (label >> 3) & 3u
               : 0u;
}

// The filtered flow at uv of the cells around that move as the texel's
// content does (bilinear among those), so the cells beside it do not slow it:
// those whose label is of class `kind`, or with none (0) those whose content
// lies at `depth` (at their centre, within FG_ENGINE_MAP_SURFACE), so a
// surface's edge moves with the surface, not with what stands in front of
// it. The nearest cell's where none is (as WorldMotion).
float2 EngineMotion(float2 uv, uint kind, float depth) {
    uint width, height;
    g_motion_flow.GetDimensions(width, height);
    const float2 cells = float2(width, height);
    const float2 position = uv * cells - 0.5;
    const int2 base = int2(floor(position));
    const float2 f = position - float2(base);
    float2 sum = float2(0.0, 0.0);
    float weight = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        const int2 cell = clamp(base + int2(i & 1, i >> 1), int2(0, 0), int2(cells) - 1);
        const float w = ((i & 1) != 0 ? f.x : 1.0 - f.x) * ((i >> 1) != 0 ? f.y : 1.0 - f.y);
        bool alike;
        if (kind != 0u) {
            alike = EngineClass(g_engine_labels.Load(int3(cell, 0))) == kind;
        } else {
            const float at = DepthNearest(g_depth, (float2(cell) + 0.5) / cells);
            alike = at > 0.0 && abs(log(at / depth)) <= FG_ENGINE_MAP_SURFACE;
        }
        if (alike) {
            sum += w * g_motion_flow.Load(int3(cell, 0)).xy;
            weight += w;
        }
    }
    return weight > 0.0 ? sum / weight
                        : g_motion_flow.Load(int3(clamp(int2(uv * cells), int2(0, 0), int2(cells) - 1), 0)).xy;
}

// The engine motion map's Landing (stage 7): each texel moved by its flow
// cell's label (the cell it lies in) where it lies on the surface the label
// was chosen for (FG_ENGINE_MAP_SURFACE), else by its distance:
//   the sky turns with the world's rotation where that is the application's
//     own movement (texel 2 y 1), else by the sky's flow around, as the motion
//     map moves it;
//   nearer than FG_SPLAT_HEAD content stays (the head's), but where its cell
//     moves on its own (a lever: the flow of the cells of its class around);
//   the head's stays nearer than FG_SPLAT_WORLD; the world's moves by the
//     world's motion (input 3) at its own depth, P' = P + a (w x P + t) with a
//     = -tuning.y, exactly in perspective (by the flow of the world's cells
//     around without one); their own by the flow of the cells of their class;
//   the rest as the motion map moves it: from FG_SPLAT_WORLD by the world's
//     flow; nearer, without a model, it claims its own texel (separate: the
//     eye-shift map and synthesis's own vector move it), or merged moves by
//     the flow of the cells around on its surface, as synthesis would.
// Past one interval (FrameGenTuning::engine_reach: a above 1) the models go
// that far, the flow one interval at most. Merged (FG_ENGINE_MAP_MERGED),
// flow is limited as synthesis limits a vector (the motion limit in tuning.z;
// synthesis takes the merged map's entries as they are), and the eye's offset
// is added to content with depth but the sky, as the eye-shift map's Landing
// adds it. Depth dropped (negative) or missing stays, as far.
bool EngineLanding(uint2 id, out float2 landing, out uint key) {
    const float2 size = float2(g.size.xy);
    const float2 uv = (float2(id) + 0.5) / size;
    const float depth = g_depth.Load(int3(id, 0)).r;
    landing = uv * size;
    key = 0u;
    bool inside = false;
    const bool sky = FG_SPLAT_SKY != 0 && depth >= 0.5 * FG_FAR_DEPTH;
    if (depth > 0.0 && (depth < 0.5 * FG_FAR_DEPTH || sky)) {
        uint width, height;
        g_engine_labels.GetDimensions(width, height);
        const uint label = g_engine_labels.Load(int3(min(int2(uv * float2(width, height)), int2(width, height) - 1), 0));
        const float surface = f16tof32(label >> 16);
        const uint kind = surface > 0.0 && abs(log(depth / surface)) <= FG_ENGINE_MAP_SURFACE ? EngineClass(label) : 0u;
        const bool merged = (g.options.x & FG_ENGINE_MAP_MERGED) != 0u;
        const float a = -g.tuning.y;
        const float bounded = min(a, 1.0);
        const bool world = (g.options.x & FG_ENGINE_MAP_WORLD) != 0u && g_engine_world.Load(int3(0, 0, 0)).w > 0.0;
        const float3 w = g_engine_world.Load(int3(0, 0, 0)).xyz;
        const float2 fov_left_up = g.fov_current.xz;
        const float2 fov_span = g.fov_current.yw - g.fov_current.xz;
        const float3 direction = float3(fov_left_up + uv * fov_span, -1.0);
        // The content at the frame's time over its depth (a point at depth z
        // along direction d is d * z; projection ignores the scale), or where
        // flow moves it in uv.
        float3 moved = direction;
        float2 flow = float2(0.0, 0.0);
        bool model = false;
        bool none = false;
        if (sky) {
            model = world && g_engine_world.Load(int3(2, 0, 0)).y == 1.0;
            if (model) {
                moved = direction + a * cross(w, direction);
            } else {
                flow = bounded * WorldMotion(uv, true);
            }
        } else if (depth < FG_SPLAT_HEAD) {
            model = kind != FG_ENGINE_CLASS_OWN;
            if (!model) {
                flow = bounded * EngineMotion(uv, kind, depth);
            }
        } else if (kind == FG_ENGINE_CLASS_HEAD && depth < FG_SPLAT_WORLD) {
            model = true;
        } else if (kind == FG_ENGINE_CLASS_WORLD && world) {
            model = true;
            moved = direction + a * (cross(w, direction) + g_engine_world.Load(int3(1, 0, 0)).xyz / depth);
        } else if (kind == FG_ENGINE_CLASS_WORLD || kind == FG_ENGINE_CLASS_OWN) {
            flow = bounded * EngineMotion(uv, kind, depth);
        } else if (depth >= FG_SPLAT_WORLD) {
            flow = bounded * WorldMotion(uv, false);
        } else if (merged) {
            flow = bounded * EngineMotion(uv, 0u, depth);
        } else {
            none = true;
        }
        if (none) {
            key = (asuint(1.0 / depth) + 64u) & ~63u;
        } else {
            if (!model) {
                if (merged) {
                    const float limit = max(g.tuning.z, 1e-4);
                    const float shift = length(flow * float2(1.0, size.y / size.x));
                    flow *= saturate((limit - shift) / (0.25 * limit));
                }
                moved = float3(fov_left_up + (uv + flow) * fov_span, -1.0);
            }
            if (merged && !sky) {
                moved += float3(g.rot0.w, g.rot1.w, g.rot2.w) / depth;
            }
            key = ((asuint(1.0 / depth) + 64u) & ~63u) | 32u;
            if (moved.z <= -1e-4) {
                const float2 shifted = (moved.xy / -moved.z - fov_left_up) / fov_span;
                landing = shifted * size;
                inside = all(shifted >= 0.0) && all(shifted <= 1.0);
            }
        }
    }
    return inside;
}

// Stage 8's forward map (FG_ENGINE_MAP_FORWARD, research, interpolating): the
// previous real frame's content (t0 its resolved depth, t1 its filtered flow)
// where it is at the frame's time, tuning.y on along its own motion (the
// flow of the cells around on its surface: EngineMotion; the sky's by the
// sky's cells around, as EngineLanding moves it; tuning.y is the frame's
// fraction of the newest interval times the intervals' ratio, as that flow is
// the pair before's), seen in the newest frame's view (the constants' pose:
// MapToNewest at its depth). The map's offsets lead from a texel of the
// newest view back to the content of the previous frame there (which the
// newest frame may show covered already). Depth dropped or missing stays out
// of it.
bool ForwardLanding(uint2 id, out float2 landing, out uint key) {
    const float2 size = float2(g.size.xy);
    const float2 uv = (float2(id) + 0.5) / size;
    const float depth = g_depth.Load(int3(id, 0)).r;
    landing = uv * size;
    key = 0u;
    bool inside = false;
    if (depth > 0.0) {
        const bool sky = depth >= 0.5 * FG_FAR_DEPTH;
        const float2 moved = uv + g.tuning.y * (sky ? WorldMotion(uv, true) : EngineMotion(uv, 0u, depth));
        float2 newest_uv;
        if (MapToNewest(moved, depth, newest_uv)) {
            landing = newest_uv * size;
            inside = all(newest_uv >= 0.0) && all(newest_uv <= 1.0);
        }
        key = ((asuint(1.0 / depth) + 64u) & ~63u) | 32u;
    }
    return inside;
}
#endif

// The eye-shift map: where each texel of the newest frame lands when seen
// from the eye's position at a generated frame's time (head movement the
// runtime does not correct: it only rotates frames). Content moves with
// the eye unless its depth was dropped (FG_PASS_TRUST_RESOLVE); that and
// scenery stay, as if infinitely far. Every half-resolution texel splats onto the
// 2x2 texels around where it lands; nearer content wins (largest key), so
// cockpit edges cover what they move over.
//
// The mapping (MapToPrevious) takes the newest view to the shifted eye's:
// no rotation, position in rot*.w. Returns false where the texel leaves the
// view; `landing` is in texels (centres at +0.5); `key` ranks the texel by
// its depth alone (see Key).

// False for distant content (and content whose depth was dropped: negative,
// FG_PASS_TRUST_RESOLVE), which does not move with the eye and only claims
// its own texel (FG_PASS_SHIFT_SPLAT), and for content the eye's movement
// takes out of the view.
//
// The motion map (FG_FLAG_DEPTH_SPLAT) moves content by its model instead:
// head-fixed content not at all, world content by its flow (WorldMotion)
// times tuning.y (synthesis's step, negated: content at x reaches
// x - step * v); the sky is distant (moved by its flow with FG_SPLAT_SKY).
// Content without a model stays on its own texel with its depth's key,
// without the moved bit, so what lands behind it there does not cover it.
bool Landing(uint2 id, out float2 landing, out uint key) {
    const float2 size = float2(g.size.xy);
    const float2 uv = (float2(id) + 0.5) / size;
    const float depth = g_depth.Load(int3(id, 0)).r;
    landing = uv * size;
    key = 0u;
    const bool sky = FG_SPLAT_SKY != 0 && MotionMap() && depth >= 0.5 * FG_FAR_DEPTH;
    if (!(depth > 0.0) || (depth >= FG_FAR_DEPTH && !sky)) {
        return false;
    }
    float2 shifted = uv;
    bool inside = false;
    if (MotionMap()) {
        const bool head = depth < FG_SPLAT_HEAD;
        if (!head && !sky && (depth < FG_SPLAT_WORLD || depth >= 0.5 * FG_FAR_DEPTH)) {
            key = depth < FG_SPLAT_WORLD ? (asuint(1.0 / depth) + 64u) & ~63u : 0u;
            return false;
        }
        shifted = head ? uv : uv - g.tuning.y * WorldMotion(uv, sky);
        inside = all(shifted >= 0.0) && all(shifted <= 1.0);
    } else {
        inside = MapToPrevious(uv, depth, shifted);
    }
    landing = shifted * size;
    // Positive floats order like their bits; 0 marks texels nothing reached.
    // Moved content's bit (32) puts it ahead of distant content at the same
    // (lowest) depth key.
    key = ((asuint(1.0 / depth) + 64u) & ~63u) | 32u;
    return inside;
}

// A texel's key at one of the texels around its landing: at one depth, the
// source landing nearest the texel's centre wins, so neighbours on a surface
// facing the eye do not both claim it.
uint Key(uint key, float2 landing, int2 texel) {
    const float2 offset = float2(texel) + 0.5 - landing;
    return key | (31u - min(31u, uint(dot(offset, offset) * 15.5 + 0.5)));
}

// A texel's landing in the map being built: the engine motion map's
// (EngineLanding, FG_PASS_SHIFT_*_ENGINE) or the forward map's
// (ForwardLanding), else Landing's.
bool MapLanding(uint2 id, out float2 landing, out uint key) {
#if defined(FG_SHIFT_ENGINE)
    bool inside;
    if ((g.options.x & FG_ENGINE_MAP_FORWARD) != 0u) {
        inside = ForwardLanding(id, landing, key);
    } else {
        inside = EngineLanding(id, landing, key);
    }
    return inside;
#else
    return Landing(id, landing, key);
#endif
}
#endif

#if defined(FG_PASS_SHIFT_SPLAT)
FG_BIND(4) FG_FORMAT("r32ui") RWTexture2D<uint> g_keys : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    float2 landing;
    uint key;
    if (MapLanding(id.xy, landing, key)) {
        const int2 first = int2(floor(landing - 0.5));
        [unroll]
        for (int i = 0; i < 4; ++i) {
            const int2 texel = first + int2(i & 1, i >> 1);
            if (all(texel >= 0) && all(texel < int2(g.size.xy))) {
                InterlockedMax(g_keys[texel], Key(key, landing, texel));
            }
        }
    } else if (key == 0u) {
        // Distant content stays on its own texel (no offset is written:
        // FG_SHIFT_NONE), unless nearer content lands there.
        InterlockedMax(g_keys[id.xy], FG_SHIFT_FAR_KEY);
    } else if ((key & 32u) == 0u) {
        // The motion map: content without a model stays too, in front of
        // what lands behind it.
        InterlockedMax(g_keys[id.xy], key | 31u);
    }
}
#endif

#if defined(FG_PASS_SHIFT_RESOLVE)
FG_BIND(2) Texture2D<uint> g_keys : register(t2);
FG_BIND(4) FG_FORMAT("r32ui") RWTexture2D<uint> g_shift : register(u0);

// Whether linear depth `a` (negative: dropped, as far) is clearly nearer
// than `b`: another surface in front of it.
bool InFront(float a, float b) {
    const bool far_a = !(a > 0.0) || a >= FG_FAR_DEPTH;
    const bool far_b = !(b > 0.0) || b >= FG_FAR_DEPTH;
    return !far_a && (far_b || a < 0.97 * b);
}

// The winners of FG_PASS_SHIFT_SPLAT write where their content comes from:
// the offset from the shifted texel back to its source, in texels, as two
// halves; of winners still tied, the smallest value, so the map is the same
// whichever thread runs first. A source covers the 2x2 texels around where
// it lands with one offset, so a surface moves smoothly; but where that
// offset would read something nearer in the newest frame (the edge of what
// stood in front of the source), the texel reads the source itself: sky
// took the colour of the cockpit wall it had been next to.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float2 landing;
    uint key;
    if (all(id.xy < g.size.xy) && MapLanding(id.xy, landing, key)) {
        const float2 back = float2(id.xy) + 0.5 - landing;
        const float own = g_depth.Load(int3(id.xy, 0)).r;
        const int2 first = int2(floor(landing - 0.5));
        [unroll]
        for (int i = 0; i < 4; ++i) {
            const int2 texel = first + int2(i & 1, i >> 1);
            if (all(texel >= 0) && all(texel < int2(g.size.xy)) &&
                g_keys.Load(int3(texel, 0)) == Key(key, landing, texel)) {
                float2 offset = back;
                const int2 read = int2(floor(float2(texel) + 0.5 + back));
                if (any(read != int2(id.xy)) &&
                    (any(read < 0) || any(read >= int2(g.size.xy)) || InFront(g_depth.Load(int3(read, 0)).r, own))) {
                    offset = float2(int2(id.xy) - texel);
                }
                InterlockedMin(g_shift[texel], f32tof16(offset.x) | (f32tof16(offset.y) << 16));
            }
        }
    }
}
#endif

#if defined(FG_PASS_SHIFT_FILL)
FG_BIND(0) Texture2D<uint> g_keys : register(t0);
FG_BIND(4) FG_FORMAT("r32ui") RWTexture2D<uint> g_shift : register(u0);

// Texels 1, 2, 4 ... 512 away: beside content a hand's width from the eye,
// a centimetre of head movement uncovers a gap hundreds of texels across.
// Gap texels farther than the reach from either side found nothing and kept
// the newest frame unshifted: a ghost of the near edge where it had been.
#define FG_FILL_REACH 10
#define FG_FILL_REFINE 6
#define FG_FILL_INSET 6

// The first texel some content reached from `start` along `direction`, at
// doubling distances then halving back toward the gap; `key` 0 if none.
int2 Reached(int2 start, float2 direction, out uint key) {
    key = 0u;
    int2 found = start;
    float inside = 0.0; // distance known to be in the gap
    float outside = 0.0;
    float distance = 1.0;
    [loop]
    for (int i = 0; i < FG_FILL_REACH && outside == 0.0; ++i) {
        const int2 texel = int2(round(float2(start) + direction * distance));
        if (any(texel < 0) || any(texel >= int2(g.size.xy))) {
            break;
        }
        const uint value = g_keys.Load(int3(texel, 0));
        if (value != 0u) {
            key = value;
            found = texel;
            outside = distance;
        } else {
            inside = distance;
            distance *= 2.0;
        }
    }
    [loop]
    for (int r = 0; r < FG_FILL_REFINE && outside > inside + 1.0; ++r) {
        const float middle = 0.5 * (inside + outside);
        const int2 texel = int2(round(float2(start) + direction * middle));
        const uint value = g_keys.Load(int3(texel, 0));
        if (value != 0u) {
            key = value;
            found = texel;
            outside = middle;
        } else {
            inside = middle;
        }
    }
    return found;
}

// Texels nothing reached: content the eye's movement uncovers, which the
// newest frame never saw. Moving content leaves the gap along the line it
// moves on, so the nearest texel either side is found there and the farther
// one (background) is stretched over the gap.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy) || g_keys.Load(int3(id.xy, 0)) != 0u) {
        return;
    }
    const float2 size = float2(g.size.xy);
    const float2 uv = (float2(id.xy) + 0.5) / size;
    const float2 tangent = float2(lerp(g.fov_current.x, g.fov_current.y, uv.x), lerp(g.fov_current.z, g.fov_current.w, uv.y));
    // A point d/z moves by (t.xy + d t.z) / z for an eye at -t (first order).
    const float3 t = float3(g.rot0.w, g.rot1.w, g.rot2.w);
    float2 direction = (t.xy + tangent * t.z) /
                       float2(g.fov_current.y - g.fov_current.x, g.fov_current.w - g.fov_current.z) * size;
    if (MotionMap()) {
#if defined(FG_SHIFT_ENGINE)
        // The engine motion map (stage 7): content moved along its flow; the
        // merged map moves it with the eye too, so the gap opens along the
        // flow's motion against the eye's parallax (above, as for content a
        // metre off).
        const float2 motion = g_motion_flow.SampleLevel(g_linear, uv, 0.0).xy * size;
        direction = (g.options.x & FG_ENGINE_MAP_MERGED) != 0u ? min(-g.tuning.y, 1.0) * motion - direction : motion;
#else
        // The motion map: content moved along its flow.
        direction = g_motion_flow.SampleLevel(g_linear, uv, 0.0).xy * size;
#endif
    }
    direction = dot(direction, direction) > 1e-12 ? normalize(direction) : float2(1.0, 0.0);
    uint ahead_key, behind_key;
    const int2 ahead = Reached(int2(id.xy), direction, ahead_key);
    const int2 behind = Reached(int2(id.xy), -direction, behind_key);
    const bool use_ahead = ahead_key != 0u && (behind_key == 0u || ahead_key <= behind_key);
    int2 source = use_ahead ? ahead : behind;
    // A few texels into that surface, past the edge itself: edge texels
    // blend both surfaces (anti-aliasing), and stretched across the gap they
    // streak it with the near edge's colour.
    const uint surface = (use_ahead ? ahead_key : behind_key) & ~63u;
    const float2 away = use_ahead ? direction : -direction;
    [unroll]
    for (int inset = 1; inset <= FG_FILL_INSET; ++inset) {
        const int2 texel = int2(round(float2(use_ahead ? ahead : behind) + away * float(inset)));
        if (any(texel < 0) || any(texel >= int2(g.size.xy)) || (g_keys.Load(int3(texel, 0)) & ~63u) != surface) {
            break;
        }
        source = texel;
    }
    uint packed = 0u;
    if (ahead_key != 0u || behind_key != 0u) {
        const uint neighbour = g_shift[source];
        const float2 back = (neighbour == FG_SHIFT_NONE ? float2(0.0, 0.0)
                                                        : float2(f16tof32(neighbour & 0xffffu), f16tof32(neighbour >> 16))) +
                            float2(source - int2(id.xy));
        packed = f32tof16(back.x) | (f32tof16(back.y) << 16);
    }
    g_shift[id.xy] = packed;
}
#endif

#if defined(FG_PASS_SHADING_RATE)
FG_BIND(0) Texture2D<float4> g_depth : register(t0);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("r8ui") RWTexture2D<uint> g_rates : register(u0);

// D3D12_SHADING_RATE_1X1 and D3D12_SHADING_RATE_2X2.
#define FG_RATE_1X1 0u
#define FG_RATE_2X2 5u
// Nearer than this is cockpit (FG_RIGID_DEPTH): always shaded per pixel.
#define FG_RATE_NEAR 4.0

// The shading-rate image for the synthesis draw (Direct3D 12): one rate per
// screen tile. A tile far off the view axis (tuning.x: the tangent of the
// angle) is shaded per 2x2 pixels unless it shows cockpit content: a wide
// view's periphery is rendered far denser than the headset shows it, while
// cockpit edges there are what generated frames must keep sharp. size.xy:
// tiles, size.zw: the output extent, options.x: the tile's side in pixels,
// options.y: FG_FLAG_DEPTH when the newest frame's depth is bound (without
// it, the angle alone decides).
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float tile = float(g.options.x);
    const float2 extent = float2(g.size.zw);
    const float2 centre = min((float2(id.xy) + 0.5) * tile, extent) / extent;
    const float2 tangent = float2(lerp(g.fov_current.x, g.fov_current.y, centre.x),
                                  lerp(g.fov_current.z, g.fov_current.w, centre.y));
    uint rate = length(tangent) >= g.tuning.x ? FG_RATE_2X2 : FG_RATE_1X1;
    if (rate != FG_RATE_1X1 && (g.options.y & FG_FLAG_DEPTH) != 0u) {
        uint width, height;
        g_depth.GetDimensions(width, height);
        const float2 scale = float2(width, height) / extent;
        const int2 first = int2(floor(float2(id.xy) * tile * scale));
        const int2 last = min(int2(ceil((float2(id.xy) + 1.0) * tile * scale)), int2(width, height)) - 1;
        // Four texels a fetch: the 2x2 block from (x, y) (clamped at edges).
        const float2 texel = 1.0 / float2(width, height);
        [loop]
        for (int y = first.y; y <= last.y && rate != FG_RATE_1X1; y += 2) {
            [loop]
            for (int x = first.x; x <= last.x; x += 2) {
                const float4 depth = g_depth.GatherRed(g_linear, (float2(x, y) + 1.0) * texel);
                // Per texel: depth in use (positive) and nearer than cockpit range.
                if (any(step(1e-6, depth) * step(depth, FG_RATE_NEAR) > 0.0)) {
                    rate = FG_RATE_1X1;
                    break;
                }
            }
        }
    }
    g_rates[id.xy] = rate;
}
#endif

#if defined(FG_PASS_ENGINE_CENSUS)
#if defined(__spirv__)
// Direct3D 12 only (FrameGenGpu::CaptureMotion): the SPIR-V build compiles
// every pass, and this one never runs there.
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
}
#else
FG_BIND(0) Texture2DArray<float4> g_vectors0 : register(t0);
FG_BIND(1) Texture2DArray<float4> g_vectors1 : register(t1);
FG_BIND(2) Texture2DArray<float4> g_vectors2 : register(t2);
FG_BIND(3) Texture2D<float4> g_depth : register(t3);
FG_BIND(4) RWTexture2D<uint4> g_census : register(u0);
FG_BIND(5) RWTexture2D<float4> g_samples : register(u1);

// Engine motion vectors, stage 0 (FrameGenGpu::CaptureMotion): a game's
// motion vector images fingerprinted, so the census can tell whether, which
// and when it writes them (it may never acquire or release them), and
// sampled where Frame Generation knows the depth, for its fits. A group of
// 64 threads per image (size.z of them: t0-t2 at array slice size.w) reads
//   a hash grid of 64x64 texels spread over the whole image, so another
//     region can never look like a write;
//   a statistics grid of 32x32 texels inside the region (fov_current:
//     offset, extent);
// and writes four texels per image into u0: the hashes and the exact-zero
// and non-finite counts; the large, |e| summed, the longest |e| and the
// finite count; r, g, b and a summed; the frame's tag (options.y), the
// image, the region's extent. Only the images options.x's mask names are
// read (stage 2 reads the chosen one alone most frames); the others' groups
// write nothing. With FG_CENSUS_SAMPLES one group more writes
// the sparse samples into u1: on a 16x16 grid of the color view's
// half-resolution texels (size.xy), the three images' raw texels by
// FG_PASS_DEPTH's nearest rule, then the linear depth there (t3, with
// FG_CENSUS_DEPTH), the color image index (options.z) and the texel.
#define FG_CENSUS_THREADS 64u
#define FG_CENSUS_HASH_GRID 64u
#define FG_CENSUS_STATS_GRID 32u
#define FG_CENSUS_SAMPLE_GRID 16u
// options.x: flags, and from bit 8 the images to read (a bit each).
#define FG_CENSUS_SAMPLES 1u
#define FG_CENSUS_DEPTH 2u
#define FG_CENSUS_IMAGES_SHIFT 8u
// A vector longer than this, in the image's own units, is counted as large.
#define FG_CENSUS_LARGE 4.0

groupshared uint g_census_xor[FG_CENSUS_THREADS];
groupshared uint g_census_sum[FG_CENSUS_THREADS];
groupshared uint g_census_zeros[FG_CENSUS_THREADS];
groupshared uint g_census_nonfinite[FG_CENSUS_THREADS];
groupshared uint g_census_large[FG_CENSUS_THREADS];
groupshared uint g_census_count[FG_CENSUS_THREADS];
groupshared float g_census_abs[FG_CENSUS_THREADS];
groupshared float g_census_max[FG_CENSUS_THREADS];
groupshared float4 g_census_rgba[FG_CENSUS_THREADS];

float4 CensusLoad(uint image, int2 texel) {
    const int4 at = int4(texel, int(g.size.w), 0);
    float4 value;
    if (image == 0u) {
        value = g_vectors0.Load(at);
    } else if (image == 1u) {
        value = g_vectors1.Load(at);
    } else {
        value = g_vectors2.Load(at);
    }
    return value;
}

float2 CensusDimensions(uint image) {
    uint width = 0u, height = 0u, elements = 0u;
    if (image == 0u) {
        g_vectors0.GetDimensions(width, height, elements);
    } else if (image == 1u) {
        g_vectors1.GetDimensions(width, height, elements);
    } else {
        g_vectors2.GetDimensions(width, height, elements);
    }
    return float2(width, height);
}

// By its bits: the compiler may take every float to be finite.
bool CensusFinite(float value) {
    return (asuint(value) & 0x7f800000u) != 0x7f800000u;
}

// An integer hash (lowbias32): a texel's channels mixed this way change its
// value whenever either changes, a sign flip of both included (multiplied
// and xored alone, those cancel).
uint CensusMix(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint t = thread.x;
    const uint image = group.x;
    const bool census = image < g.size.z && ((g.options.x >> FG_CENSUS_IMAGES_SHIFT) & (1u << image)) != 0u;
    uint hash_xor = 0u, hash_sum = 0u, zeros = 0u, nonfinite = 0u;
    uint large = 0u, count = 0u;
    float total = 0.0, longest = 0.0;
    float4 sums = float4(0.0, 0.0, 0.0, 0.0);
    if (census) {
        // Hash grid: this thread's row of it, at fixed texels of the image.
        const float2 dimensions = CensusDimensions(image);
        [loop]
        for (uint x = 0u; x < FG_CENSUS_HASH_GRID; ++x) {
            const float2 cell = (float2(float(x), float(t)) + 0.5) * dimensions / float(FG_CENSUS_HASH_GRID);
            const float4 v = CensusLoad(image, int2(min(floor(cell), dimensions - 1.0)));
            const uint index = t * FG_CENSUS_HASH_GRID + x;
            const uint value =
                CensusMix((asuint(v.r) * 0x9E3779B1u) ^ CensusMix((asuint(v.g) * 0x85EBCA77u) ^ (index * 0xC2B2AE3Du)));
            hash_xor ^= value;
            hash_sum += value;
            zeros += v.r == 0.0 && v.g == 0.0 ? 1u : 0u;
            nonfinite += CensusFinite(v.r) && CensusFinite(v.g) ? 0u : 1u;
        }
        // Statistics grid: this thread's share of it, inside the region.
        [loop]
        for (uint s = t; s < FG_CENSUS_STATS_GRID * FG_CENSUS_STATS_GRID; s += FG_CENSUS_THREADS) {
            const float2 cell = float2(float(s % FG_CENSUS_STATS_GRID), float(s / FG_CENSUS_STATS_GRID));
            const float2 offset = min(floor((cell + 0.5) * g.fov_current.zw / float(FG_CENSUS_STATS_GRID)),
                                      g.fov_current.zw - 1.0);
            const float4 v = CensusLoad(image, int2(g.fov_current.xy + offset));
            if (CensusFinite(v.r) && CensusFinite(v.g)) {
                const float magnitude = length(v.rg);
                ++count;
                large += magnitude > FG_CENSUS_LARGE ? 1u : 0u;
                total += magnitude;
                longest = max(longest, magnitude);
                sums += float4(v.r, v.g, CensusFinite(v.b) ? v.b : 0.0, CensusFinite(v.a) ? v.a : 0.0);
            }
        }
    }
    g_census_xor[t] = hash_xor;
    g_census_sum[t] = hash_sum;
    g_census_zeros[t] = zeros;
    g_census_nonfinite[t] = nonfinite;
    g_census_large[t] = large;
    g_census_count[t] = count;
    g_census_abs[t] = total;
    g_census_max[t] = longest;
    g_census_rgba[t] = sums;
    [unroll]
    for (uint stride = FG_CENSUS_THREADS / 2u; stride > 0u; stride >>= 1u) {
        GroupMemoryBarrierWithGroupSync();
        if (t < stride) {
            g_census_xor[t] ^= g_census_xor[t + stride];
            g_census_sum[t] += g_census_sum[t + stride];
            g_census_zeros[t] += g_census_zeros[t + stride];
            g_census_nonfinite[t] += g_census_nonfinite[t + stride];
            g_census_large[t] += g_census_large[t + stride];
            g_census_count[t] += g_census_count[t + stride];
            g_census_abs[t] += g_census_abs[t + stride];
            g_census_max[t] = max(g_census_max[t], g_census_max[t + stride]);
            g_census_rgba[t] += g_census_rgba[t + stride];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (census && t == 0u) {
        const uint base = image * 4u;
        g_census[uint2(base, 0u)] = uint4(g_census_xor[0], g_census_sum[0], g_census_zeros[0], g_census_nonfinite[0]);
        g_census[uint2(base + 1u, 0u)] =
            uint4(g_census_large[0], asuint(g_census_abs[0]), asuint(g_census_max[0]), g_census_count[0]);
        g_census[uint2(base + 2u, 0u)] = asuint(g_census_rgba[0]);
        g_census[uint2(base + 3u, 0u)] = uint4(g.options.y, image, uint(g.fov_current.z), uint(g.fov_current.w));
    }
    if (image >= g.size.z && (g.options.x & FG_CENSUS_SAMPLES) != 0u) {
        [loop]
        for (uint s = t; s < FG_CENSUS_SAMPLE_GRID * FG_CENSUS_SAMPLE_GRID; s += FG_CENSUS_THREADS) {
            const float2 cell = float2(float(s % FG_CENSUS_SAMPLE_GRID), float(s / FG_CENSUS_SAMPLE_GRID));
            const uint2 q = uint2((cell + 0.5) * float2(g.size.xy) / float(FG_CENSUS_SAMPLE_GRID));
            const float2 uv = (float2(q) + 0.5) / float2(g.size.xy);
            const int2 texel = int2(g.fov_current.xy + min(uv * g.fov_current.zw, g.fov_current.zw - 1.0));
            [unroll]
            for (uint k = 0u; k < 3u; ++k) {
                g_samples[uint2(s * 4u + k, 0u)] = k < g.size.z ? CensusLoad(k, texel) : float4(0.0, 0.0, 0.0, 0.0);
            }
            const float depth = (g.options.x & FG_CENSUS_DEPTH) != 0u ? g_depth.Load(int3(q, 0)).r : 0.0;
            g_samples[uint2(s * 4u + 3u, 0u)] = float4(depth, float(g.options.z), float(q.x), float(q.y));
        }
    }
}
#endif
#endif

#if defined(FG_PASS_ENGINE_CAPTURE)
#if defined(__spirv__)
// Direct3D 12 only (FrameGenGpu::CaptureMotion), like FG_PASS_ENGINE_CENSUS.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
}
#else
FG_BIND(0) Texture2DArray<float4> g_vectors0 : register(t0);
FG_BIND(1) Texture2DArray<float4> g_vectors1 : register(t1);
FG_BIND(4) RWTexture2D<float4> g_raw0 : register(u0);
FG_BIND(5) RWTexture2D<float4> g_raw1 : register(u1);

// Engine motion vectors, stage 2 (FrameGenGpu::CaptureMotion): each eye's
// motion vector image, as the game rendered it, on the view's half-resolution
// grid (size.xy) by FG_PASS_DEPTH's nearest rule, so a texel's vector and
// its depth come from the same rendered texel. No convention is applied: the
// raw rg goes into the field (rg16f; rgba16f keeps b and a too, research).
// id.z picks the eye: t0 into u0 from the region at fov_current.xy, extent
// size.zw, slice fov_previous.z; t1 into u1 from fov_current.zw, extent
// fov_previous.xy, slice fov_previous.w. options.x eyes.
// A vector not finite, or longer than FG_ENGINE_RAW_LIMIT on either axis,
// is stored as FG_ENGINE_INVALID on both; an exact zero stays zero (a cleared
// sky says something too).
#define FG_ENGINE_INVALID 65504.0
#define FG_ENGINE_RAW_LIMIT 16.0

// By its bits: the compiler may take every float to be finite.
bool EngineFinite(float value) {
    return (asuint(value) & 0x7f800000u) != 0x7f800000u;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy) || id.z >= g.options.x) {
        return;
    }
    const bool second = id.z == 1u;
    const float2 offset = second ? g.fov_current.zw : g.fov_current.xy;
    const float2 extent = second ? g.fov_previous.xy : float2(g.size.zw);
    const int slice = int(second ? g.fov_previous.w : g.fov_previous.z);
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    const int2 texel = int2(offset + min(uv * extent, extent - 1.0));
    const float4 raw = second ? g_vectors1.Load(int4(texel, slice, 0)) : g_vectors0.Load(int4(texel, slice, 0));
    const bool valid = EngineFinite(raw.r) && EngineFinite(raw.g) && abs(raw.r) <= FG_ENGINE_RAW_LIMIT &&
                       abs(raw.g) <= FG_ENGINE_RAW_LIMIT;
    const float4 value = float4(valid ? raw.rg : float2(FG_ENGINE_INVALID, FG_ENGINE_INVALID),
                                EngineFinite(raw.b) ? raw.b : FG_ENGINE_INVALID,
                                EngineFinite(raw.a) ? raw.a : FG_ENGINE_INVALID);
    if (second) {
        g_raw1[id.xy] = value;
    } else {
        g_raw0[id.xy] = value;
    }
}
#endif
#endif

#if defined(FG_PASS_ENGINE_REDUCE) || defined(FG_PASS_ENGINE_SOLVE)
// Engine motion vectors, stage 3 (AppendEngineEvidence): each group's sums
// take FG_ENGINE_PARTIAL_TEXELS texels of the partials, each kind its own
// of them (kFrameGenEngine*Texel), so the kinds' passes leave each other's be.
#define FG_ENGINE_PARTIAL_TEXELS 32u
#define FG_ENGINE_COCKPIT_TEXEL 0u
#define FG_ENGINE_PHOTOMETRIC_TEXEL 11u
#define FG_ENGINE_LAG_TEXEL 18u
#define FG_ENGINE_COCKPIT_LAG_TEXEL 20u
#define FG_ENGINE_USED_TEXELS 25u
#define FG_ENGINE_KIND_COCKPIT 1u
#define FG_ENGINE_KIND_PHOTOMETRIC 2u
#define FG_ENGINE_KIND_LAG 4u
#define FG_ENGINE_KIND_COCKPIT_LAG 8u
#endif

#if defined(FG_PASS_ENGINE_REDUCE)
#if defined(__spirv__)
// Direct3D 12 only (FrameGenViewRequest::engine_motion), like FG_PASS_ENGINE_CENSUS.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
}
#else
FG_BIND(0) Texture2D<float4> g_raw : register(t0);
FG_BIND(1) Texture2D<float4> g_luma_current : register(t1);
FG_BIND(2) Texture2D<float4> g_luma_previous : register(t2);
FG_BIND(3) Texture2D<float4> g_depth : register(t3);
Texture2D<float4> g_flow : register(t4); // the measured flow at the final level
// The field before; with FG_ENGINE_BIAS the solve's first stage (texel 0:
// the frame's bias).
Texture2D<float4> g_before : register(t5);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_partials : register(u0);
FG_BIND(6) SamplerState g_linear : register(s0);

// The evidence on a frame's raw vectors (t0, half resolution, as the game
// wrote them: the capture's sentinel, and anything past its limit, is no
// vector), one sample per thread of a grid of size.xy over the half-
// resolution extent size.zw, each at the centre of its texel. A vector as
// rendered (uv, y down, current minus previous) is tuning.xy * raw +
// tuning.zw in the convention under test; the cockpit kinds take it as that
// convention reads it in NDC (e = (2 x, -2 y): its gain times the vector as
// decoded less its offset), which ReadFrameGenEngineEvidence takes back to
// as decoded. Samples are chosen by what the luma shows (t1, and t2: the
// previous frame mapped into this view by the declared poses, negative
// where unseen), never by assuming near content is still:
//   textured: the 3x3 luma around varies by FG_ENGINE_TEXTURE;
//   still: the previous frame's 3x3 luma within FG_ENGINE_STILL of it, the
//     head's movement taken out, every texel seen.
// Kinds (options.x bits 0-3, those the dispatch sums; from bit 4 the camera:
// 0 the head included, 1 its rotation only, 2 excluded, 3 objects only; bit
// 8: the measured flow is bound; bit 9: t5 holds the frame's bias):
//   cockpit (and, alone, the cockpit against the pair before, whose
//     constants carry that pair's poses): per axis, still content's e
//     against the head's movement there (p, NDC current minus previous: p_r
//     by rotation alone, p_t what translation adds at the resolved depth,
//     t3; any depth, but none or disproved), with the cross-axis sums; and
//     against the measured flow (t4) where it is clear (cost below
//     FG_ENGINE_CLEAR, seen, shorter than 64 pixels): q, its displacement as
//     rendered with the head's movement, and h, without it;
//   photometric: textured, seen samples' 3x3 luma error with the previous
//     frame read where each of 8 variants of the convention moves the
//     content (bit 0 flips y, bit 1 the direction, bit 2 toggles the head;
//     variant 0 the convention itself, the frame's bias taken out with bit
//     9), with no motion and with the measured flow; still near content's
//     inliers; the distance from the measured flow by depth band; the head's
//     movement and the vectors' length (pixels), and the measured flow's;
//   lag: the field before (t5) and this frame's by the same error, the bias
//     taken out of neither (a jittered field carries its own frame's).
// The photometric and lag kinds leave out what the merge never takes from
// the vectors (FG_PASS_ENGINE_MERGE): absent vectors, and with depth,
// samples without it (disproved by the other eye: scenery behind glass, the
// box MSFS 2024 draws into its depth).
// Each thread writes its sample's terms into the group's shared memory, a
// row per term; a thread per row then sums it into the kinds' texels of the
// partials.
#define FG_ENGINE_RAW_VALID 16.0
#define FG_ENGINE_TEXTURE 0.02
#define FG_ENGINE_STILL 0.03
#define FG_ENGINE_CLEAR 0.06
#define FG_ENGINE_NEAR 4.0
#define FG_ENGINE_WORLD 30.0
#define FG_ENGINE_SKY (0.5 * FG_FAR_DEPTH)
#define FG_ENGINE_MEASURED 256u
#define FG_ENGINE_BIAS 512u
// The terms of texels 0 to 19 (the cockpit kind's, the photometric's, the
// lag's; the cockpit against the pair before takes the cockpit's first 20),
// each row a word longer than the group so that the threads summing rows
// read different banks.
#define FG_ENGINE_TERMS 80u
#define FG_ENGINE_ROW 65u
groupshared float g_engine_terms[FG_ENGINE_TERMS * FG_ENGINE_ROW];

void EngineTerm(uint index, uint term, float value) {
    g_engine_terms[term * FG_ENGINE_ROW + index] = value;
}

// The luma images are the half-resolution extent (size.zw).
float EngineLuma(int2 q) {
    return g_luma_current.Load(int3(clamp(q, int2(0, 0), int2(g.size.zw) - 1), 0)).r;
}

// The previous frame's luma at q: -1 unseen or outside.
float EnginePrevious(int2 q) {
    return any(q < 0) || any(q >= int2(g.size.zw)) ? -1.0 : g_luma_previous.Load(int3(q, 0)).r;
}

// The mean luma error of the 3x3 texels around q (`current`: their luma)
// with the content moved by v (uv): the previous frame read v before each,
// bilinear (FG_INVALID_PENALTY where any of a read's four texels is unseen;
// `seen` false then). The nine reads share their fraction, so the 4x4
// texels around are fetched once: inside the image (nearly always), four
// gathers of 2x2 texels each, which return (bottom left, bottom right, top
// right, top left) of their quad.
float EngineCost(int2 q, float current[9], float2 v, out bool seen) {
    const float2 p = float2(q) - v * float2(g.size.zw);
    const int2 base = int2(floor(p)) - 1;
    const float2 f = p - floor(p);
    float previous[16];
    [branch]
    if (all(base >= 0) && all(base + 3 < int2(g.size.zw))) {
        const float2 texel = 1.0 / float2(g.size.zw);
        [unroll]
        for (uint y = 0u; y < 4u; y += 2u) {
            [unroll]
            for (uint x = 0u; x < 4u; x += 2u) {
                const float4 quad = g_luma_previous.GatherRed(g_linear, (float2(base + int2(x, y)) + 1.0) * texel);
                previous[(y + 1u) * 4u + x] = quad.x;
                previous[(y + 1u) * 4u + x + 1u] = quad.y;
                previous[y * 4u + x + 1u] = quad.z;
                previous[y * 4u + x] = quad.w;
            }
        }
    } else {
        [unroll]
        for (uint i = 0u; i < 16u; ++i) {
            previous[i] = EnginePrevious(base + int2(i & 3u, i >> 2));
        }
    }
    float sum = 0.0;
    seen = true;
    [unroll]
    for (uint k = 0u; k < 9u; ++k) {
        const uint corner = (k / 3u) * 4u + k % 3u;
        const float a = previous[corner], b = previous[corner + 1u];
        const float c = previous[corner + 4u], d = previous[corner + 5u];
        const bool inside = min(min(a, b), min(c, d)) >= 0.0;
        seen = seen && inside;
        sum += inside ? abs(current[k] - lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y)) : FG_INVALID_PENALTY;
    }
    return sum / 9.0;
}

// A vector as rendered as Frame Generation's flow (the head's movement taken
// out through the declared poses): with the head in it, the content's place
// as rendered in the previous frame mapped back into this view (z its depth,
// 0 for rotation alone), as FG_PASS_FLOW_UNPACK maps the hardware's; without
// it, as it is. False where that place maps behind this view.
bool EngineFlow(float2 uv, float2 delta, bool head, float z, out float2 v) {
    v = delta;
    if (!head) {
        return true;
    }
    float2 newest_uv;
    const bool mapped = MapToNewest(uv - delta, z, newest_uv);
    v = mapped ? uv - newest_uv : float2(0.0, 0.0);
    return mapped;
}

// What still content at uv (depth z; 0: rotation alone) does between the
// declared poses, in NDC current minus previous; false where the previous
// view did not see it.
bool EnginePrediction(float2 uv, float z, out float2 p) {
    float2 previous_uv;
    const bool inside = MapToPrevious(uv, z, previous_uv);
    p = float2(2.0 * (uv.x - previous_uv.x), -2.0 * (uv.y - previous_uv.y));
    return inside;
}

// One axis's fit terms from `first`, in FrameGenEngineAxisFit::FromSums' order.
void EngineAxis(uint index, uint first, float e, float rotation, float translation) {
    EngineTerm(index, first, 1.0);
    EngineTerm(index, first + 1u, rotation);
    EngineTerm(index, first + 2u, translation);
    EngineTerm(index, first + 3u, e);
    EngineTerm(index, first + 4u, rotation * rotation);
    EngineTerm(index, first + 5u, translation * translation);
    EngineTerm(index, first + 6u, rotation * translation);
    EngineTerm(index, first + 7u, e * rotation);
    EngineTerm(index, first + 8u, e * translation);
    EngineTerm(index, first + 9u, e * e);
}

// The measured flow (t4) of the cell at uv.
float4 EngineMeasured(float2 uv) {
    uint width, height;
    g_flow.GetDimensions(width, height);
    return g_flow.Load(int3(min(int2(uv * float2(width, height)), int2(width, height) - 1), 0));
}

bool EngineValid(float2 raw) {
    return abs(raw.x) <= FG_ENGINE_RAW_VALID && abs(raw.y) <= FG_ENGINE_RAW_VALID;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex) {
    const uint kinds = g.options.x & 15u;
    const uint camera = (g.options.x >> 4) & 3u;
    const bool measured = (g.options.x & FG_ENGINE_MEASURED) != 0u;
    const bool cockpit_kind = (kinds & (FG_ENGINE_KIND_COCKPIT | FG_ENGINE_KIND_COCKPIT_LAG)) != 0u;
    // The terms this dispatch's kinds sum, from the first kind's to the last
    // kind's (the cockpit against the pair before: the cockpit's first 20).
    const uint pair_texels = FG_ENGINE_USED_TEXELS - FG_ENGINE_COCKPIT_LAG_TEXEL;
    const uint first = cockpit_kind                                 ? 0u
                       : (kinds & FG_ENGINE_KIND_PHOTOMETRIC) != 0u ? 4u * FG_ENGINE_PHOTOMETRIC_TEXEL
                                                                   : 4u * FG_ENGINE_LAG_TEXEL;
    const uint last = (kinds & FG_ENGINE_KIND_LAG) != 0u           ? 4u * (FG_ENGINE_LAG_TEXEL + 2u)
                      : (kinds & FG_ENGINE_KIND_PHOTOMETRIC) != 0u ? 4u * FG_ENGINE_LAG_TEXEL
                      : (kinds & FG_ENGINE_KIND_COCKPIT) != 0u     ? 4u * FG_ENGINE_PHOTOMETRIC_TEXEL
                                                                   : 4u * pair_texels;
    [unroll]
    for (uint cleared = 0u; cleared < FG_ENGINE_TERMS; ++cleared) {
        EngineTerm(index, cleared, 0.0);
    }
    if (all(id.xy < g.size.xy)) {
        const float2 extent = float2(g.size.zw);
        const int2 q = min(int2((float2(id.xy) + 0.5) / float2(g.size.xy) * extent), int2(g.size.zw) - 1);
        const float2 uv = (float2(q) + 0.5) / extent;
        // uv to full-resolution pixels (and NDC to them, at half that).
        const float2 pixels = 2.0 * extent;
        const float2 raw = g_raw.Load(int3(q, 0)).rg;
        // The resolved depth: negative where the other eye disproved it.
        const float z = g_depth.Load(int3(q, 0)).r;
        // Cleared vectors (FG_ENGINE_ABSENT_DEPTH) are none.
        const bool valid = EngineValid(raw) && !EngineAbsent(raw, uv, z, camera);
        const bool measurable = z > 0.0 || (g.options.y & FG_FLAG_DEPTH) == 0u;
        const float depth = z > 0.0 ? z : 0.0;
        const bool head = camera <= 1u;
        const float head_z = camera == 0u ? depth : 0.0;
        float current[9];
        float mean = 0.0, squares = 0.0, still = 0.0;
        bool seen = true;
        [unroll]
        for (int t = 0; t < 9; ++t) {
            const int2 at = q + int2(t % 3 - 1, t / 3 - 1);
            current[t] = EngineLuma(at);
            const float previous = EnginePrevious(at);
            seen = seen && previous >= 0.0;
            still += abs(current[t] - previous);
            mean += current[t];
            squares += current[t] * current[t];
        }
        mean /= 9.0;
        const bool textured = sqrt(max(squares / 9.0 - mean * mean, 0.0)) > FG_ENGINE_TEXTURE;
        const bool still_content = seen && still / 9.0 < FG_ENGINE_STILL;
        const float2 delta = g.tuning.xy * raw + g.tuning.zw;
        if (cockpit_kind) {
            const float2 e = float2(2.0 * delta.x, -2.0 * delta.y);
            float2 rotation, full;
            if (valid && textured && still_content && z > 0.0 && EnginePrediction(uv, 0.0, rotation) &&
                EnginePrediction(uv, depth, full)) {
                const float2 translation = full - rotation;
                EngineAxis(index, 0u, e.x, rotation.x, translation.x);
                EngineAxis(index, 10u, e.y, rotation.y, translation.y);
                EngineTerm(index, 20u, e.x * rotation.y);
                EngineTerm(index, 21u, e.y * rotation.x);
            }
            if ((kinds & FG_ENGINE_KIND_COCKPIT) != 0u && measured && valid && z > 0.0) {
                const float4 flow = EngineMeasured(uv);
                float2 before_uv;
                if (flow.z < FG_ENGINE_CLEAR && flow.w >= 0.0 && length(flow.xy * pixels) < 64.0 &&
                    MapToPrevious(uv - flow.xy, depth, before_uv)) {
                    const float2 with_head = float2(2.0 * (uv.x - before_uv.x), -2.0 * (uv.y - before_uv.y));
                    const float2 without = float2(2.0 * flow.x, -2.0 * flow.y);
                    EngineAxis(index, 22u, e.x, with_head.x, without.x);
                    EngineAxis(index, 32u, e.y, with_head.y, without.y);
                    EngineTerm(index, 42u, dot(with_head - without, with_head - without));
                }
            }
        }
        // Variant 0 where the photometric kind scored it with no bias taken
        // out: this frame's for the lag kind.
        float2 variant0 = float2(0.0, 0.0);
        float cost0 = 0.0;
        bool scored = false;
        if ((kinds & FG_ENGINE_KIND_PHOTOMETRIC) != 0u) {
            const uint p = 4u * FG_ENGINE_PHOTOMETRIC_TEXEL;
            const bool bias = (g.options.x & FG_ENGINE_BIAS) != 0u;
            const float2 rendered = bias ? delta - g_before.Load(int3(0, 0, 0)).xy : delta;
            float2 full;
            const bool predicted = z > 0.0 && EnginePrediction(uv, depth, full);
            if (valid && predicted) {
                EngineTerm(index, p + 24u, length(full * extent));
                EngineTerm(index, p + 25u, length(rendered * pixels));
                EngineTerm(index, p + 26u, 1.0);
            }
            if (valid && textured && seen && measurable) {
                bool seen0 = false;
                [unroll]
                for (uint c = 0u; c < 8u; ++c) {
                    const float direction = (c & 2u) != 0u ? -1.0 : 1.0;
                    const float flip = (c & 1u) != 0u ? -1.0 : 1.0;
                    const bool toggled = (c & 4u) != 0u ? !head : head;
                    float2 v;
                    const bool mapped = EngineFlow(uv, rendered * float2(direction, direction * flip), toggled,
                                                   (c & 4u) != 0u ? depth : head_z, v);
                    bool complete = false;
                    const float cost = mapped ? EngineCost(q, current, v, complete) : FG_INVALID_PENALTY;
                    EngineTerm(index, p + c, cost);
                    if (c == 0u) {
                        variant0 = v;
                        cost0 = cost;
                        seen0 = mapped && complete;
                    }
                }
                scored = !bias;
                bool unused;
                const float zero = EngineCost(q, current, float2(0.0, 0.0), unused);
                EngineTerm(index, p + 8u, zero);
                EngineTerm(index, p + 10u, 1.0);
                // Content the vectors move, where the previous frame was seen
                // under both readings. The luma pass's static-world test marks
                // a moving object's place before unseen where it stood in
                // front of what shows there now: the vectors' reading pays
                // FG_INVALID_PENALTY there, no motion's (the 3x3 around, seen)
                // never does.
                if (seen0 && length(variant0 * pixels) > 1.0) {
                    EngineTerm(index, p + 12u, 1.0);
                    EngineTerm(index, p + 14u, cost0);
                    EngineTerm(index, p + 15u, zero);
                }
                if (measured) {
                    const float4 flow = EngineMeasured(uv);
                    const float cost_measured = EngineCost(q, current, flow.xy, unused);
                    EngineTerm(index, p + 9u, cost_measured);
                    EngineTerm(index, p + 11u, 1.0);
                    EngineTerm(index, p + 13u, cost0 <= 1.25 * cost_measured + 0.01 ? 1.0 : 0.0);
                    EngineTerm(index, p + 27u, length(flow.xy * pixels));
                    // Bands: nearer than 4 m, from 30 m to the sky, the sky.
                    if (flow.w >= 0.0 && z > 0.0) {
                        const float apart = length((variant0 - flow.xy) * pixels);
                        if (z < FG_ENGINE_NEAR) {
                            EngineTerm(index, p + 18u, apart);
                            EngineTerm(index, p + 19u, 1.0);
                        } else if (z >= FG_ENGINE_WORLD && z < FG_ENGINE_SKY) {
                            EngineTerm(index, p + 20u, apart);
                            EngineTerm(index, p + 21u, 1.0);
                        } else if (z >= FG_ENGINE_SKY) {
                            EngineTerm(index, p + 22u, apart);
                            EngineTerm(index, p + 23u, 1.0);
                        }
                    }
                }
            }
            // Still near content follows the head (the camera's share of it).
            float2 rotation;
            if (valid && textured && still_content && z > 0.0 && z < FG_ENGINE_NEAR && predicted &&
                EnginePrediction(uv, 0.0, rotation)) {
                const float2 expected = camera == 0u ? full : camera == 1u ? rotation : float2(0.0, 0.0);
                const float2 moved = float2(2.0 * rendered.x, -2.0 * rendered.y);
                const float miss = length((moved - expected) * extent);
                EngineTerm(index, p + 16u, miss < max(1.5, 0.2 * length(expected * extent)) ? 1.0 : 0.0);
                EngineTerm(index, p + 17u, 1.0);
            }
        }
        if ((kinds & FG_ENGINE_KIND_LAG) != 0u) {
            // The field before (t5) and this frame's, on the samples both have.
            const uint l = 4u * FG_ENGINE_LAG_TEXEL;
            const float2 before_raw = g_before.Load(int3(q, 0)).rg;
            if (valid && EngineValid(before_raw) && !EngineAbsent(before_raw, uv, z, camera) && textured && seen &&
                measurable) {
                bool unused;
                float2 before;
                const bool mapped_before = EngineFlow(uv, g.tuning.xy * before_raw + g.tuning.zw, head, head_z, before);
                const float cost_before = mapped_before ? EngineCost(q, current, before, unused) : FG_INVALID_PENALTY;
                float2 now = variant0;
                float cost_now = cost0;
                if (!scored) {
                    const bool mapped_now = EngineFlow(uv, delta, head, head_z, now);
                    cost_now = mapped_now ? EngineCost(q, current, now, unused) : FG_INVALID_PENALTY;
                }
                EngineTerm(index, l, cost_before);
                EngineTerm(index, l + 1u, cost_now);
                EngineTerm(index, l + 2u, 1.0);
                if (length(now * pixels) > 1.0) {
                    EngineTerm(index, l + 3u, cost_before);
                    EngineTerm(index, l + 4u, cost_now);
                    EngineTerm(index, l + 5u, 1.0);
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // A thread per term sums its row (four sums at a time, so the reads
    // overlap) into the row's first word.
    [unroll]
    for (uint turn = 0u; turn < 2u; ++turn) {
        const uint term = index + 64u * turn;
        if (term >= first && term < last) {
            const uint row = term * FG_ENGINE_ROW;
            float4 sums = float4(0.0, 0.0, 0.0, 0.0);
            [unroll]
            for (uint i = 0u; i < 64u; i += 4u) {
                sums += float4(g_engine_terms[row + i], g_engine_terms[row + i + 1u], g_engine_terms[row + i + 2u],
                               g_engine_terms[row + i + 3u]);
            }
            g_engine_terms[row] = (sums.x + sums.y) + (sums.z + sums.w);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // The kinds' texels of the group's partials (the cockpit against the
    // pair before's at its own).
    const uint texel = index;
    const bool written = texel < FG_ENGINE_PHOTOMETRIC_TEXEL
                             ? (kinds & FG_ENGINE_KIND_COCKPIT) != 0u ||
                                   ((kinds & FG_ENGINE_KIND_COCKPIT_LAG) != 0u && texel < pair_texels)
                         : texel < FG_ENGINE_LAG_TEXEL      ? (kinds & FG_ENGINE_KIND_PHOTOMETRIC) != 0u
                         : texel < FG_ENGINE_LAG_TEXEL + 2u ? (kinds & FG_ENGINE_KIND_LAG) != 0u
                                                            : false;
    if (written) {
        const uint slot = group.y * ((g.size.x + 7u) / 8u) + group.x;
        const uint place = (kinds & FG_ENGINE_KIND_COCKPIT_LAG) != 0u ? FG_ENGINE_COCKPIT_LAG_TEXEL + texel : texel;
        const uint row = 4u * texel * FG_ENGINE_ROW;
        g_partials[uint2(slot * FG_ENGINE_PARTIAL_TEXELS + place, 0u)] =
            float4(g_engine_terms[row], g_engine_terms[row + FG_ENGINE_ROW], g_engine_terms[row + 2u * FG_ENGINE_ROW],
                   g_engine_terms[row + 3u * FG_ENGINE_ROW]);
    }
}
#endif
#endif

#if defined(FG_PASS_ENGINE_SOLVE)
#if defined(__spirv__)
// Direct3D 12 only, like FG_PASS_ENGINE_REDUCE.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
}
#else
FG_BIND(0) Texture2D<float4> g_partials : register(t0);
FG_BIND(1) Texture2D<uint4> g_census : register(t1);
FG_BIND(2) Texture2D<uint4> g_census_previous : register(t2);
FG_BIND(3) Texture2D<float4> g_params_view : register(t3);
Texture2D<float4> g_params_other : register(t5);
FG_BIND(4) FG_FORMAT("rgba32f") RWTexture2D<float4> g_params : register(u0);
FG_BIND(5) FG_FORMAT("rgba32f") RWTexture2D<float4> g_evidence : register(u1);

// Sums the evidence passes' partials (one group) and solves the frame:
//   stage 0 (options.x bits 0-3), after the cockpit kind: with a jittered
//     convention, the bias in still content's vectors (their mean less what
//     the convention makes of the head's movement there) into params texel
//     0 in uv, for the photometric kind to take out; texel 1: the cockpit
//     fit's a and b per axis as the convention reads the vectors (1 and b/a
//     where its gain is right; 0 where it does not solve);
//   stage 1, after every kind: the same, the frame's verdict (0: its
//     vectors may be used, else the kFrameGenEngineVerdict* reasons) in
//     texel 0's z with the cockpit's inlier share in its w, the photometric
//     ratios with the head's mean movement and the stale test (texels 2 and
//     3), the verdict alone in texel 4 (its first 8 bytes are zero exactly
//     when the vectors may be used); into u1 the sums themselves, those of
//     kinds that did not run zero;
//   stage 2 (Lead, stage 6; Interpolate's seen test, stage 8), after every
//     view's stage 1: the frame's verdict for both eyes (t3's texel 0 z,
//     this view's, and t5's, the other view's: either holds both back) into
//     u0's texel 0 x (kEngineVerdict), 0 in its y: its first 8 bytes are
//     zero exactly when both views' vectors may be used, what the view's
//     predicate copies; this view's bias (t3's texel 0 xy) in its zw, for
//     FG_PASS_ENGINE_CONVERT with FG_ENGINE_CONVERT_SEEN.
// The first stage sums the cockpit fit's texels alone. The plan dispatches
// one group's worth of threads (8 x 8); the group is 256 threads, 8 a texel,
// each summing every eighth group's texel with 16 loads in flight at a time
// (one group waits on its loads, not on its arithmetic), then a thread a
// texel summing theirs.
// From bit 4: locked, jittered, no measured flow (Lead's photometric test,
// against no motion), the field before was the frame before's (the lag
// kind ran, and its census is the frame before's: the stale test), the
// cockpit against the pair before ran; from bit 12 the camera. options.z:
// the images the newest and previous fields came from (bits 0-7, 8-15).
#define FG_ENGINE_LOCKED 16u
#define FG_ENGINE_JITTER 32u
#define FG_ENGINE_LEAD 64u
#define FG_ENGINE_BEFORE 128u
#define FG_ENGINE_PAIR_BEFORE 256u
#define FG_ENGINE_FIT_TEXELS 5u
#define FG_ENGINE_SOLVE_PARTS 8u
groupshared float4 g_engine_sum[FG_ENGINE_PARTIAL_TEXELS * FG_ENGINE_SOLVE_PARTS];

[numthreads(32, 8, 1)]
void CSMain(uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
    uint width, height;
    g_partials.GetDimensions(width, height);
    const uint groups = width / FG_ENGINE_PARTIAL_TEXELS;
    const uint stage = g.options.x & 15u;
    const uint texels = stage == 0u ? FG_ENGINE_FIT_TEXELS : FG_ENGINE_USED_TEXELS;
    const bool before = (g.options.x & FG_ENGINE_BEFORE) != 0u;
    const uint texel = thread.x;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    if (texel < texels) {
        // No branch between the loads, so that they are in flight together:
        // past the last group they read outside the partials, which is zero.
        [loop]
        for (uint slot = thread.y; slot < groups; slot += 16u * FG_ENGINE_SOLVE_PARTS) {
            float4 loaded[16];
            [unroll]
            for (uint k = 0u; k < 16u; ++k) {
                loaded[k] = g_partials.Load(
                    int3(int((slot + FG_ENGINE_SOLVE_PARTS * k) * FG_ENGINE_PARTIAL_TEXELS + texel), 0, 0));
            }
            [unroll]
            for (uint span = 8u; span > 0u; span >>= 1) {
                [unroll]
                for (uint k = 0u; k < span; ++k) {
                    loaded[k] += loaded[k + span];
                }
            }
            sum += loaded[0];
        }
    }
    g_engine_sum[index] = sum;
    GroupMemoryBarrierWithGroupSync();
    if (index < FG_ENGINE_PARTIAL_TEXELS) {
        float4 total = float4(0.0, 0.0, 0.0, 0.0);
        [unroll]
        for (uint part = 0u; part < FG_ENGINE_SOLVE_PARTS; ++part) {
            total += g_engine_sum[part * FG_ENGINE_PARTIAL_TEXELS + index];
        }
        // Kinds that did not run left another frame's sums in their texels.
        const bool lag = index == FG_ENGINE_LAG_TEXEL || index == FG_ENGINE_LAG_TEXEL + 1u;
        const bool pair = index >= FG_ENGINE_COCKPIT_LAG_TEXEL && index < FG_ENGINE_USED_TEXELS;
        if (index >= texels || (lag && !before) || (pair && (g.options.x & FG_ENGINE_PAIR_BEFORE) == 0u)) {
            total = float4(0.0, 0.0, 0.0, 0.0);
        }
        g_engine_sum[index] = total;
        if (stage != 0u) {
            g_evidence[uint2(index, 0u)] = total;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index != 0u) {
        return;
    }
    // Stage 2 sums nothing (no partials bound).
    if (stage == 2u) {
        const float4 own = g_params_view.Load(int3(0, 0, 0));
        const uint reasons = uint(own.z) | uint(g_params_other.Load(int3(0, 0, 0)).z);
        g_params[uint2(0u, 0u)] = float4(float(reasons), 0.0, own.xy);
        return;
    }
    const uint camera = (g.options.x >> 12) & 3u;
    // Per axis: the bias (still content's vectors less the convention's
    // reading of the head's movement there) and the cockpit fit.
    float2 bias = float2(0.0, 0.0);
    float4 fit = float4(0.0, 0.0, 0.0, 0.0);
    [unroll]
    for (int axis = 0; axis < 2; ++axis) {
        float s[10];
        [unroll]
        for (int k = 0; k < 10; ++k) {
            const int term = axis * 10 + k;
            s[k] = g_engine_sum[term / 4][term % 4];
        }
        const float n = s[0];
        if (n >= 50.0 && (g.options.x & FG_ENGINE_JITTER) != 0u) {
            const float model = camera == 0u ? s[1] + s[2] : camera == 1u ? s[1] : 0.0;
            const float c = (s[3] - model) / n;
            if (axis == 0) {
                bias.x = 0.5 * c;
            } else {
                bias.y = -0.5 * c;
            }
        }
        if (n >= 4.0) {
            const float srr = s[4] - s[1] * s[1] / n;
            const float stt = s[5] - s[2] * s[2] / n;
            const float srt = s[6] - s[1] * s[2] / n;
            const float ser = s[7] - s[3] * s[1] / n;
            const float set = s[8] - s[3] * s[2] / n;
            const float det = srr * stt - srt * srt;
            if (srr > 0.0 && stt > 0.0 && det > 1e-6 * srr * stt) {
                if (axis == 0) {
                    fit.x = (ser * stt - set * srt) / det;
                    fit.z = (set * srr - ser * srt) / det;
                } else {
                    fit.y = (ser * stt - set * srt) / det;
                    fit.w = (set * srr - ser * srt) / det;
                }
            }
        }
    }
    if (stage == 0u) {
        g_params[uint2(0u, 0u)] = float4(bias, 0.0, 0.0);
        g_params[uint2(1u, 0u)] = fit;
        return;
    }
    const uint p = FG_ENGINE_PHOTOMETRIC_TEXEL;
    const float variant = g_engine_sum[p].x;
    const float zero = g_engine_sum[p + 2u].x;
    const float cost_measured = g_engine_sum[p + 2u].y;
    const float textured = g_engine_sum[p + 2u].z;
    const float with_measured = g_engine_sum[p + 2u].w;
    const float moving = g_engine_sum[p + 3u].x;
    const float inliers = g_engine_sum[p + 4u].x;
    const float cockpit = g_engine_sum[p + 4u].y;
    const float head_px = g_engine_sum[p + 6u].x;
    const float vectors_px = g_engine_sum[p + 6u].y;
    const float valid = g_engine_sum[p + 6u].z;
    const float flow_px = g_engine_sum[p + 6u].w;
    const float lag_cost = g_engine_sum[FG_ENGINE_LAG_TEXEL].x;
    const float lag_variant = g_engine_sum[FG_ENGINE_LAG_TEXEL].y;
    const float lag_samples = g_engine_sum[FG_ENGINE_LAG_TEXEL].z;
    const float mean_head = valid > 0.0 ? head_px / valid : 0.0;
    const float mean_vectors = valid > 0.0 ? vectors_px / valid : 0.0;
    const float mean_flow = with_measured > 0.0 ? flow_px / with_measured : 0.0;
    const float mean_variant = textured > 0.0 ? variant / textured : 0.0;
    const float mean_zero = textured > 0.0 ? zero / textured : 0.0;
    const float mean_measured = with_measured > 0.0 ? cost_measured / with_measured : 0.0;
    // A still frame: neither the head nor the vectors move.
    const bool still = valid > 0.0 && mean_head < 0.5 && mean_vectors < 0.5;
    // Stale: the image read holds what the image read the frame before held
    // (by their censuses, of two frames), though what the vectors show
    // moved: the head, with the head in them; the content as measured, with
    // it excluded (still vectors of a still world are no stale ones); else
    // the content as they move it.
    bool stale = false;
    if (before) {
        const int newest = int(g.options.z & 0xffu);
        const int previous = int((g.options.z >> 8) & 0xffu);
        const uint4 hash = g_census.Load(int3(newest * 4, 0, 0));
        const uint4 tag = g_census.Load(int3(newest * 4 + 3, 0, 0));
        const uint4 hash_before = g_census_previous.Load(int3(previous * 4, 0, 0));
        const uint4 tag_before = g_census_previous.Load(int3(previous * 4 + 3, 0, 0));
        const float moved = camera <= 1u                       ? mean_head
                            : camera == 2u && with_measured > 0.0 ? mean_flow
                                                                  : mean_vectors;
        stale = tag.x != tag_before.x && hash.x == hash_before.x && hash.y == hash_before.y && moved > 0.5;
    }
    uint reasons = (g.options.x & FG_ENGINE_LOCKED) != 0u ? 0u : 1u;
    reasons |= stale ? 2u : 0u;
    // Still near content (enough of it) follows the head, unless nothing moves.
    if (!still && cockpit >= 200.0 && inliers < 0.7 * cockpit) {
        reasons |= 4u;
    }
    if (textured >= 200.0) {
        const bool lead = (g.options.x & FG_ENGINE_LEAD) != 0u;
        const bool worse = lead ? mean_variant > (moving > 100.0 ? 0.85 : 1.0) * mean_zero + 0.005
                                : with_measured >= 200.0 && mean_variant > 1.15 * mean_measured + 0.005;
        reasons |= worse ? 8u : 0u;
    }
    if (lag_samples >= 200.0 && lag_cost < 0.9 * lag_variant) {
        reasons |= 16u;
    }
    if ((g.options.x & FG_ENGINE_JITTER) != 0u && cockpit < 200.0) {
        reasons |= 32u;
    }
    g_params[uint2(0u, 0u)] = float4(bias, float(reasons), cockpit > 0.0 ? inliers / cockpit : 0.0);
    g_params[uint2(1u, 0u)] = fit;
    g_params[uint2(2u, 0u)] = float4(mean_measured > 0.0 ? mean_variant / mean_measured : 0.0,
                                     mean_zero > 0.0 ? mean_variant / mean_zero : 0.0, mean_head, stale ? 1.0 : 0.0);
    g_params[uint2(3u, 0u)] = float4(cockpit, textured, moving, lag_variant > 0.0 ? lag_cost / lag_variant : 0.0);
    g_params[uint2(4u, 0u)] = float4(float(reasons), 0.0, 0.0, 0.0);
    [unroll]
    for (uint rest = 5u; rest < 8u; ++rest) {
        g_params[uint2(rest, 0u)] = float4(0.0, 0.0, 0.0, 0.0);
    }
}
#endif
#endif


#if defined(FG_PASS_ENGINE_CONVERT)
#if defined(__spirv__)
// Direct3D 12 only (FrameGenViewRequest::engine_motion), like FG_PASS_ENGINE_REDUCE.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
}
#else
FG_BIND(0) Texture2D<float4> g_raw : register(t0);
FG_BIND(1) Texture2D<float4> g_depth : register(t1);
FG_BIND(2) Texture2D<float4> g_params : register(t2);
FG_BIND(3) Texture2D<float4> g_tiles_before : register(t3);
Texture2D<float4> g_world : register(t4);
// The other view's verdict (its kEngineParams), or with FG_ENGINE_CONVERT_SEEN
// the previous frame's resolved depth.
Texture2D<float4> g_other : register(t5);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_candidate : register(u0);
FG_BIND(5) FG_FORMAT("rgba16f") RWTexture2D<float4> g_info : register(u1);

// Each flow cell of the final level (size.xy; options.w its level) from the
// half-resolution texels under it (size.zw; t0 the raw vectors as the game
// wrote them, t1 the resolved depth, negative where the other eye disproved
// it): two candidates, the vector of the texel of its nearest surface with
// depth (a thin near feature still has one) and of its farthest (the far
// sentinel included: the background). Each is decoded by the convention
// (tuning: A * raw + B, as FG_PASS_ENGINE_REDUCE) with the frame's bias (t2's
// texel 0) taken out and made Frame Generation's flow: with the head's
// movement in the vectors (options.x from bit 4: 0), the content's place as
// rendered in the previous frame mapped back into this view at the texel's
// depth (1: by rotation alone), as FG_PASS_ENGINE_REDUCE's variant 0; without
// it (2, 3), as it is. A cell without depth takes its centre texel's.
// u0: the two vectors (xy the nearest's, zw the farthest's). u1: the spread
// of the nearest surface's vectors about its own (full-resolution pixels),
// the flags (FG_ENGINE_FLAG_HELD in every cell where the frame's verdict, t2's
// texel 0 z, or the other eye's, t5's, held the vectors back), the tile's
// trust in the vectors
// (t3, the frame before's tiles with FG_ENGINE_CONVERT_TILES; half without),
// and the classes (the nearest's plus 4 times the farthest's): the head's
// movement alone (within FG_ENGINE_CLASS_TEXELS half-resolution texels or
// FG_ENGINE_CLASS_SHARE of the vector of no motion), the world's (as close to
// the world's motion in t4, with FG_ENGINE_CONVERT_WORLD; standing still,
// closer to it than to no motion and as far as the world: FG_WORLD_DEPTH or
// no depth), their own. Nearer still content is the head's: the cockpit
// stays rigid where the world barely moves.
// With FG_ENGINE_CONVERT_SEEN (stage 8, interpolating with the previous
// frame's depth in t5), each candidate also says whether the previous frame
// saw its content where the vector puts it then, at its own depth (as
// FG_PASS_LUMA's test: FG_ENGINE_FLAG_NEAR_UNSEEN, FG_ENGINE_FLAG_FAR_UNSEEN);
// t2 is then kEngineVerdict (FG_PASS_ENGINE_SOLVE's stage 2): both eyes'
// verdict in texel 0's x, the bias in its zw.
#define FG_ENGINE_CONVERT_TILES 256u
#define FG_ENGINE_CONVERT_WORLD 512u
#define FG_ENGINE_CONVERT_SEEN 1024u
#define FG_ENGINE_VALID_RAW 16.0
#define FG_ENGINE_CLASS_TEXELS 0.6
#define FG_ENGINE_CLASS_SHARE 0.08
// The nearest surface: depth within this share of its own.
#define FG_ENGINE_SURFACE 0.05

struct EngineCandidate {
    float2 v;
    uint flags;
    uint kind;
    bool unseen;
};

// Stage 8's seen test (FG_ENGINE_CONVERT_SEEN): t2 and t5 hold what it reads.
bool EngineSeenTest() {
    return (g.options.x & FG_ENGINE_CONVERT_SEEN) != 0u;
}

EngineCandidate EngineConvert(int2 q, float z) {
    EngineCandidate candidate;
    candidate.v = float2(0.0, 0.0);
    candidate.flags = z > 0.0 ? 0u : FG_ENGINE_FLAG_NO_DEPTH;
    candidate.kind = 0u;
    candidate.unseen = false;
    const float2 extent = float2(g.size.zw);
    const float2 uv = (float2(q) + 0.5) / extent;
    const float2 raw = g_raw.Load(int3(q, 0)).rg;
    if (!(abs(raw.x) <= FG_ENGINE_VALID_RAW && abs(raw.y) <= FG_ENGINE_VALID_RAW)) {
        candidate.flags |= FG_ENGINE_FLAG_INVALID;
        return candidate;
    }
    const uint camera = (g.options.x >> 4) & 3u;
    const float depth = z > 0.0 ? z : 0.0;
    const float4 own = g_params.Load(int3(0, 0, 0));
    const float2 delta = g.tuning.xy * raw + g.tuning.zw - (EngineSeenTest() ? own.zw : own.xy);
    if (camera <= 1u) {
        float2 newest_uv;
        if (!MapToNewest(uv - delta, camera == 0u ? depth : 0.0, newest_uv)) {
            candidate.flags |= FG_ENGINE_FLAG_UNMAPPED;
            return candidate;
        }
        candidate.v = uv - newest_uv;
        if (EngineAbsent(raw, uv, z, camera)) {
            candidate.flags |= FG_ENGINE_FLAG_ABSENT;
        }
    } else {
        candidate.v = delta;
    }
    const float moved = length(candidate.v * extent);
    const float tolerance = max(FG_ENGINE_CLASS_TEXELS, FG_ENGINE_CLASS_SHARE * moved);
    float off_world = 1e30;
    if ((g.options.x & FG_ENGINE_CONVERT_WORLD) != 0u && g_world.Load(int3(0, 0, 0)).w > 0.0) {
        // Content without depth (disproved: scenery behind glass) as far.
        const float inv_z = depth > 0.0 && depth < FG_FAR_DEPTH ? 1.0 / depth : 0.0;
        const bool delta_model = g_world.Load(int3(2, 0, 0)).y == 1.0;
        const float2 world = (delta_model ? WorldFlowExact(g_world, uv, inv_z) : WorldFlow(g_world, uv, inv_z)) /
                             WorldTangentScale();
        off_world = length((candidate.v - world) * extent);
    }
    const bool nearer = depth > 0.0 && depth < FG_WORLD_DEPTH; // than the world
    candidate.kind = moved <= tolerance && (moved <= off_world || nearer) ? FG_ENGINE_CLASS_HEAD
                     : off_world <= tolerance                              ? FG_ENGINE_CLASS_WORLD
                                                                           : FG_ENGINE_CLASS_OWN;
    // Stage 8: where the vector puts the content in the previous frame, out
    // of its view or behind something nearer there.
    if (EngineSeenTest() && depth > 0.0) {
        float2 previous_uv;
        candidate.unseen = !MapToPrevious(uv - candidate.v, depth, previous_uv) ||
                           !SeenBefore(g_other, uv - candidate.v, depth, previous_uv);
    }
    return candidate;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int side = int(1u << (g.options.w - 1u));
    const int2 limit = int2(g.size.zw) - 1;
    const int2 first = int2(id.xy) * side;
    int2 near = min(first + (side >> 1), limit);
    int2 far = near;
    float near_z = 2.0 * FG_FAR_DEPTH;
    float far_z = 0.0;
    [loop]
    for (int y = 0; y < side; ++y) {
        [loop]
        for (int x = 0; x < side; ++x) {
            const int2 q = min(first + int2(x, y), limit);
            const float z = g_depth.Load(int3(q, 0)).r;
            if (z > 0.0 && z < near_z) {
                near_z = z;
                near = q;
            }
            if (z > 0.0 && z > far_z) {
                far_z = z;
                far = q;
            }
        }
    }
    const bool found = far_z > 0.0;
    near_z = found ? near_z : 0.0;
    const EngineCandidate nearest = EngineConvert(near, near_z);
    const EngineCandidate farthest = EngineConvert(far, far_z);
    // How far the vectors of the nearest surface spread about its own: a
    // vector the cell's content does not share.
    float spread = 0.0;
    if (found && (nearest.flags & FG_ENGINE_FLAG_INVALID) == 0u) {
        const float2 raw_near = g_raw.Load(int3(near, 0)).rg;
        const float2 pixels = 2.0 * float2(g.size.zw);
        [loop]
        for (int sy = 0; sy < side; ++sy) {
            [loop]
            for (int sx = 0; sx < side; ++sx) {
                const int2 q = min(first + int2(sx, sy), limit);
                const float z = g_depth.Load(int3(q, 0)).r;
                const float2 raw = g_raw.Load(int3(q, 0)).rg;
                if (z > 0.0 && z <= near_z * (1.0 + FG_ENGINE_SURFACE) && abs(raw.x) <= FG_ENGINE_VALID_RAW &&
                    abs(raw.y) <= FG_ENGINE_VALID_RAW) {
                    spread = max(spread, length(g.tuning.xy * (raw - raw_near) * pixels));
                }
            }
        }
    }
    const float trust = (g.options.x & FG_ENGINE_CONVERT_TILES) != 0u
                            ? g_tiles_before.Load(int3(id.xy / FG_ENGINE_TILE, 0)).x
                            : 0.5;
    const float4 own = g_params.Load(int3(0, 0, 0));
    const bool held_back = EngineSeenTest() ? own.x != 0.0 : own.z != 0.0 || g_other.Load(int3(0, 0, 0)).z != 0.0;
    const uint held = held_back ? FG_ENGINE_FLAG_HELD : 0u;
    const uint unseen =
        (nearest.unseen ? FG_ENGINE_FLAG_NEAR_UNSEEN : 0u) | (farthest.unseen ? FG_ENGINE_FLAG_FAR_UNSEEN : 0u);
    g_candidate[id.xy] = float4(nearest.v, farthest.v);
    g_info[id.xy] = float4(spread, float(nearest.flags | (farthest.flags << 4) | held | unseen), trust,
                           float(nearest.kind + 4u * farthest.kind));
}
#endif
#endif

#if defined(FG_PASS_ENGINE_MERGE)
#if defined(__spirv__)
// Direct3D 12 only, like FG_PASS_ENGINE_CONVERT.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
}
#else
FG_BIND(2) Texture2D<float4> g_candidate : register(t2);
FG_BIND(3) Texture2D<float4> g_info : register(t3);
Texture2D<float4> g_measured : register(t4);
Texture2D<float4> g_depth : register(t5);
FG_BIND(5) FG_FORMAT("r32ui") RWTexture2D<uint> g_labels : register(u1);

// Each flow cell of the final level (size.xy; options.z its level): the flow
// the filters read (u0, kFlow's contract) and the cell's label (u1). Where the
// frame's verdict or the other eye's (FG_ENGINE_FLAG_HELD in t3) holds the
// vectors back, the measured flow (t4) as it is. Elsewhere the
// candidates (t2, FG_PASS_ENGINE_CONVERT's; t3 what it found of them) cost
// what FG_PASS_REFINE's would: its block at its match level (t0, t1;
// options.w the block's scale), the vector rounded to the match texels. A
// candidate is left out when it is not a vector, maps behind the view, is
// absent, or the cell has no depth the other eye did not disprove while the
// frame has depth (glass, the invisible box MSFS 2024 writes over its
// glareshield); the farthest one where it moves the content
// within a pixel of the nearest; the nearest pays FG_ENGINE_SPREAD_COST where
// its surface's vectors spread more than FG_ENGINE_SPREAD_PIXELS or
// FG_ENGINE_SPREAD_SHARE of it. The better candidate:
//   Assist (options.x 1): beats the measured flow, cost for cost, within a
//     margin the tile's trust widens (none below a quarter, 25% and 0.01 up
//     to three quarters, 40% beyond);
//   Lead (2, stage 6): beats no motion by that much; else the step-halving
//     search from the better of it and no motion moves the cell (content
//     without vectors: screens, propellers, clouds). With no measured flow
//     at all (4, research: t4 unbound), every cell of a frame whose verdict
//     holds the vectors back is searched from no motion, labelled held back;
//   either way (stage 8, with FG_PASS_ENGINE_CONVERT's seen test) a candidate
//     whose content the previous frame did not see where its vector puts it
//     then, where the tile trusts the vectors at all and the vector is of
//     the head's or the world's class: nothing there to match.
// A candidate taken is polished, a texel across and to the sub-texel, only
// where that matches clearly better (FG_ENGINE_POLISH) within a texel of it
// and the previous frame saw all of the block where the candidate puts it
// (content coming into view or out from behind an edge matches better
// shifted toward what was seen): else its own vector, unrounded. w: the
// block's cost of no motion, -1 where the previous frame did not see it, as
// FG_PASS_REFINE's. With FG_ENGINE_MERGE_DEPTHS (the engine motion map, stage
// 7, and stage 8's seen test; t5 the resolved depth at half resolution), the
// label of a candidate taken keeps the depth of the surface it was chosen for
// (its cell's nearest or farthest, as FG_PASS_ENGINE_CONVERT found them) in
// its high half, so the map moves only that surface's texels by it (and
// synthesis reads its unseen bit only there). Where that candidate's content
// was not seen by the previous frame (stage 8: FG_ENGINE_FLAG_NEAR_UNSEEN or
// FG_ENGINE_FLAG_FAR_UNSEEN), its label says so (FG_ENGINE_LABEL_UNSEEN).
#define FG_ENGINE_MERGE_LEAD 2u
#define FG_ENGINE_MERGE_UNMEASURED 4u
#define FG_ENGINE_MERGE_DEPTHS 8u
#define FG_ENGINE_SPREAD_PIXELS 2.0
#define FG_ENGINE_SPREAD_SHARE 0.3
#define FG_ENGINE_SPREAD_COST 0.02
#define FG_ENGINE_POLISH 0.02
// A block whose luma varies by this much (its standard deviation) is
// textured: what the tiles' share counts.
#define FG_ENGINE_TEXTURE 0.02

// Whether the previous frame saw every texel of the block moved by d.
bool BlockSeen(Block block, int2 d) {
    bool seen = true;
    [unroll]
    for (int i = 0; i < FG_BLOCK * FG_BLOCK; ++i) {
        if (block.valid[i]) {
            seen = seen && PreviousLuma(block.first + int2(i % FG_BLOCK, i / FG_BLOCK) - d, block.extent) >= 0.0;
        }
    }
    return seen;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float4 measured = g_measured.Load(int3(id.xy, 0));
    const bool held = (uint(g_info.Load(int3(id.xy, 0)).y) & FG_ENGINE_FLAG_HELD) != 0u;
    if (held && (g.options.x & FG_ENGINE_MERGE_UNMEASURED) == 0u) {
        g_flow[id.xy] = measured;
        g_labels[id.xy] = FG_ENGINE_LABEL_MEASURED | FG_ENGINE_LABEL_INVALID;
        return;
    }
    const int2 cell = int2(id.xy);
    const int scale = int(g.options.w);
    const Block block = LoadBlock(cell, scale);
    const float2 match = float2(block.extent);
    const float2 pixels = float2(g.size.xy) * float(1u << g.options.z);
    float mean = 0.0, squares = 0.0, count = 0.0;
    [unroll]
    for (int i = 0; i < FG_BLOCK * FG_BLOCK; ++i) {
        if (block.valid[i]) {
            mean += block.luma[i];
            squares += block.luma[i] * block.luma[i];
            count += 1.0;
        }
    }
    mean /= max(count, 1.0);
    const bool textured = sqrt(max(squares / max(count, 1.0) - mean * mean, 0.0)) > FG_ENGINE_TEXTURE;

    const float4 candidates = g_candidate.Load(int3(cell, 0));
    const float4 info = g_info.Load(int3(cell, 0));
    const uint flags = uint(info.y);
    const uint kinds = uint(info.w);
    const uint refused = FG_ENGINE_FLAG_INVALID | FG_ENGINE_FLAG_UNMAPPED | FG_ENGINE_FLAG_ABSENT |
                         ((g.options.y & FG_FLAG_DEPTH) != 0u ? FG_ENGINE_FLAG_NO_DEPTH : 0u);
    const uint near_refused = flags & refused;
    const uint far_refused = (flags >> 4) & refused;
    // Without a candidate, what kept the vectors out.
    uint source = ((flags | (flags >> 4)) & FG_ENGINE_FLAG_ABSENT) != 0u     ? FG_ENGINE_LABEL_ABSENT
                  : ((flags & refused) & FG_ENGINE_FLAG_NO_DEPTH) != 0u ? FG_ENGINE_LABEL_DISPROVED
                                                                          : FG_ENGINE_LABEL_MEASURED;
    float2 engine = float2(0.0, 0.0);
    float engine_cost = 1e30; // with the spread's
    uint kind = 0u;
    if (near_refused == 0u) {
        const float spread = info.x > max(FG_ENGINE_SPREAD_PIXELS, FG_ENGINE_SPREAD_SHARE * length(candidates.xy * pixels))
                                 ? FG_ENGINE_SPREAD_COST
                                 : 0.0;
        engine = candidates.xy;
        engine_cost = BlockCost(block, int2(round(candidates.xy * match))) + spread;
        source = FG_ENGINE_LABEL_NEAR;
        kind = kinds & 3u;
    }
    if (far_refused == 0u && (near_refused != 0u || length((candidates.zw - candidates.xy) * pixels) > 1.0)) {
        const float cost = BlockCost(block, int2(round(candidates.zw * match)));
        if (cost < engine_cost) {
            engine = candidates.zw;
            engine_cost = cost;
            source = FG_ENGINE_LABEL_FAR;
            kind = (kinds >> 2) & 3u;
        }
    }
    const bool candidate = !held && (source == FG_ENGINE_LABEL_NEAR || source == FG_ENGINE_LABEL_FAR);
    const float trust = info.z;
    const float alpha = trust < 0.25 ? 0.0 : trust > 0.75 ? 0.4 : 0.25;
    const float beta = trust < 0.25 ? 0.0 : 0.01;
    // Stage 8: content the previous frame did not see where the candidate's
    // vector puts it then (FG_PASS_ENGINE_CONVERT's seen test) has nothing to
    // match there: its cost says nothing against the measured flow's or no
    // motion's, which match something else. Such a candidate is taken where
    // the tile trusts the vectors at all and the vector moves as the head or
    // the world does: one of the content's own motion still has to match (a
    // wrong vector long enough to leave the view is never seen).
    const uint unseen_flag = source == FG_ENGINE_LABEL_NEAR ? FG_ENGINE_FLAG_NEAR_UNSEEN : FG_ENGINE_FLAG_FAR_UNSEEN;
    const bool unseen = candidate && (flags & unseen_flag) != 0u;
    const bool unmatched = unseen && trust >= 0.25 && kind != FG_ENGINE_CLASS_OWN;
    bool taken = false;
    float4 result = measured;
    if ((g.options.x & FG_ENGINE_MERGE_LEAD) != 0u) {
        const float zero_cost = BlockCost(block, int2(0, 0));
        taken = candidate && (engine_cost <= zero_cost + beta || unmatched);
        if (!taken) {
            int2 best = int2(0, 0);
            float best_cost = zero_cost;
            if (candidate) {
                Consider(block, int2(round(engine * match)), best, best_cost);
            }
            StepSearch(block, scale, best, best_cost);
            const float2 sub = SubTexelAt(block, best, best_cost);
            result = float4((float2(best) + sub) / match, best_cost, ZeroCost(block));
            source = all(best == int2(0, 0)) && all(sub == 0.0) ? FG_ENGINE_LABEL_ZERO : FG_ENGINE_LABEL_SEARCHED;
            kind = 0u;
        }
    } else {
        taken = candidate && (engine_cost <= (1.0 + alpha) * measured.z + beta || unmatched);
        if (!taken) {
            source = candidate ? FG_ENGINE_LABEL_MEASURED : source;
            kind = 0u;
        }
    }
    if (taken) {
        // Polished only where a texel across matches clearly better.
        const int2 rounded = int2(round(engine * match));
        const float cost = BlockCost(block, rounded);
        int2 best = rounded;
        float best_cost = cost;
        const int2 cross[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        [unroll]
        for (int c = 0; c < 4; ++c) {
            Consider(block, rounded + cross[c], best, best_cost);
        }
        float2 v = engine;
        float chosen = cost;
        if (best_cost < cost - FG_ENGINE_POLISH && BlockSeen(block, rounded)) {
            const float2 polished = (float2(best) + SubTexelAt(block, best, best_cost)) / match;
            if (length((polished - engine) * match) <= 1.0) {
                v = polished;
                chosen = best_cost;
            }
        }
        result = float4(v, chosen, ZeroCost(block));
    }
    // The surface the candidate taken was chosen for: its cell's nearest or
    // farthest depth, as FG_PASS_ENGINE_CONVERT found them.
    float surface = 0.0;
    if (taken && (g.options.x & FG_ENGINE_MERGE_DEPTHS) != 0u) {
        uint width, height;
        g_depth.GetDimensions(width, height);
        const int side = int(1u << (g.options.z - 1u));
        const int2 limit = int2(width, height) - 1;
        float near_z = 2.0 * FG_FAR_DEPTH;
        float far_z = 0.0;
        [loop]
        for (int y = 0; y < side; ++y) {
            [loop]
            for (int x = 0; x < side; ++x) {
                const float z = g_depth.Load(int3(min(cell * side + int2(x, y), limit), 0)).r;
                if (z > 0.0) {
                    near_z = min(near_z, z);
                    far_z = max(far_z, z);
                }
            }
        }
        surface = min(source == FG_ENGINE_LABEL_NEAR ? near_z : far_z, 0.5 * FG_FAR_DEPTH);
    }
    g_flow[id.xy] = result;
    g_labels[id.xy] = source | (kind << 3) | (taken && unseen ? FG_ENGINE_LABEL_UNSEEN : 0u) |
                      (textured ? FG_ENGINE_LABEL_TEXTURED : 0u) | (held ? FG_ENGINE_LABEL_INVALID : 0u) |
                      (f32tof16(surface) << 16);
}
#endif
#endif

#if defined(FG_PASS_ENGINE_TILES)
#if defined(__spirv__)
// Direct3D 12 only, like FG_PASS_ENGINE_CONVERT.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
}
#else
FG_BIND(0) Texture2D<uint> g_labels : register(t0);
FG_BIND(1) Texture2D<float4> g_tiles_before : register(t1);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_tiles : register(u0);

// Each tile (size.xy) of FG_ENGINE_TILE flow cells a side (size.zw) of the
// labels (t0): its trust in the game's vectors, moved from the frame
// before's (t1, with options.x 1; else half) toward the share of its
// textured cells the vectors won, by FG_ENGINE_TILE_RATE, or
// FG_ENGINE_TILE_TURNING while the head turns more than FG_ENGINE_TILE_TURN
// degrees a frame (tuning.y): what a moving view shows changes faster. A
// tile without textured cells (or a frame whose verdict held the vectors
// back) keeps its trust. Cells labelled unseen (stage 8: the previous frame
// did not see the content their vector moves) are not counted: matching
// there says nothing of the vectors, and the trust decides where the merge
// takes such vectors unmatched. Into u0: the trust, the share, the textured
// cells.
#define FG_ENGINE_TILE_RATE 0.2
#define FG_ENGINE_TILE_TURNING 0.6
#define FG_ENGINE_TILE_TURN 2.0

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float before = g.options.x != 0u ? g_tiles_before.Load(int3(id.xy, 0)).x : 0.5;
    float textured = 0.0;
    float won = 0.0;
    [loop]
    for (uint y = 0u; y < FG_ENGINE_TILE; ++y) {
        [loop]
        for (uint x = 0u; x < FG_ENGINE_TILE; ++x) {
            const uint2 cell = id.xy * FG_ENGINE_TILE + uint2(x, y);
            if (any(cell >= g.size.zw)) {
                continue;
            }
            const uint label = g_labels.Load(int3(cell, 0));
            if ((label & (FG_ENGINE_LABEL_TEXTURED | FG_ENGINE_LABEL_INVALID | FG_ENGINE_LABEL_UNSEEN)) ==
                FG_ENGINE_LABEL_TEXTURED) {
                const uint source = label & FG_ENGINE_LABEL_SOURCE;
                textured += 1.0;
                won += source == FG_ENGINE_LABEL_NEAR || source == FG_ENGINE_LABEL_FAR ? 1.0 : 0.0;
            }
        }
    }
    const float share = textured > 0.0 ? won / textured : 0.0;
    const float rate = g.tuning.y > FG_ENGINE_TILE_TURN ? FG_ENGINE_TILE_TURNING : FG_ENGINE_TILE_RATE;
    g_tiles[id.xy] = float4(textured > 0.0 ? lerp(before, share, rate) : before, share, textured, 0.0);
}
#endif
#endif
