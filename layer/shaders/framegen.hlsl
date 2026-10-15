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

#if defined(FG_PASS_LUMA)
FG_BIND(0) Texture2D<float4> g_current : register(t0);
FG_BIND(1) Texture2D<float4> g_previous : register(t1);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(3) Texture2D<float4> g_depth_previous : register(t3);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_luma_current : register(u0);
FG_BIND(5) FG_FORMAT("r32f") RWTexture2D<float> g_luma_previous : register(u1);

// Previous-frame content nearer than this share of where the newest frame's
// depth puts a point (and by FG_OCCLUDER_METERS) stood in front of it.
#define FG_OCCLUDER_SHARE 0.92
#define FG_OCCLUDER_METERS 0.01

// Whether the previous frame saw the content at newest-view `uv` (linear
// depth `depth`, which mapped it to `previous_uv`): false where its own
// depth there is clearly nearer, so something stood in front of it (newly
// revealed by head or object movement). Matching such texels finds aliases
// (black panel against black rim), not motion. Depth the other eye
// disproved (negative: scenery behind glass) never hides anything.
bool SeenBefore(float2 uv, float depth, float2 previous_uv) {
    uint width, height;
    g_depth_previous.GetDimensions(width, height);
    const int2 texel = clamp(int2(previous_uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
    const float observed = g_depth_previous.Load(int3(texel, 0)).r;
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
    const bool seen = inside && ((g.options.y & FG_FLAG_DEPTH_PREVIOUS) == 0u || SeenBefore(uv, depth, previous_uv));
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

#if defined(FG_PASS_SEARCH) || defined(FG_PASS_REFINE)
// Flow passes write one vector per cell of the flow level (size.xy) but match
// on a finer luma level: options.w holds the cell size in match-level texels,
// so fine texture such as instrument faces and terrain detail still
// constrains the vector of a coarse cell.
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

#if defined(FG_PASS_REFINE)
FG_BIND(2) Texture2D<float4> g_parent : register(t2);
FG_BIND(3) Texture2D<float4> g_temporal : register(t3);
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

    // Step-halving search: one flow-level cell (scale match texels) first,
    // then half that, down to single texels, so a level can correct its
    // parent by a full parent cell while still landing on exact texels.
    // Coarse steps probe a cross; the final single-texel step probes all
    // eight neighbours.
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

    const float2 sub = float2(
        SubTexel(BlockCost(block, best - int2(1, 0)), best_cost, BlockCost(block, best + int2(1, 0))),
        SubTexel(BlockCost(block, best - int2(0, 1)), best_cost, BlockCost(block, best + int2(0, 1))));
    // w: the cost of no motion (FG_PASS_FILTER's rigid test).
    g_flow[id.xy] = float4((float2(best) + sub) / match, best_cost, ZeroCost(block));
}
#endif

#if defined(FG_PASS_WORLD_REDUCE) || defined(FG_PASS_WORLD_SOLVE) || defined(FG_PASS_FILTER)
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
// the fit, 2.61% without).
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
    g_world[uint2(0u, 0u)] = float4(x[0], x[1], x[2], solved ? 1.0 : 0.0);
    g_world[uint2(1u, 0u)] = float4(float3(x[3], x[4], x[5]) * FG_WORLD_TRANSLATION_UNIT,
                                    count > 0.0 ? weight / count : 0.0);
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

// A flow sample (w: its cost with no motion, negative where the previous
// frame did not see the content: FG_PASS_REFINE) with the rigid test applied
// at its cell; `rigid` when the cell is cockpit content.
float4 Rigid(float4 flow, int2 cell, float2 pixels, out bool rigid) {
    const float zero = flow.w;
    const float2 uv = (float2(cell) + 0.5) / float2(g.size.xy);
    const float depth = DepthNearest(g_depth, uv);
    rigid = depth > 0.0 && depth < FG_RIGID_DEPTH;
    float4 result = flow;
    if (rigid) {
        const bool keep = (g.options.y & FG_FLAG_EXTRAPOLATE) == 0u && zero >= 0.0 &&
                          zero > flow.z * FG_RIGID_SHARE + FG_RIGID_MARGIN && length(flow.xy * pixels) > FG_RIGID_PIXELS;
        if (!keep) {
            result = float4(0.0, 0.0, zero < 0.0 ? flow.z : min(flow.z, zero), 1.0);
        }
    } else {
        // World content takes the world's fitted motion, also where the
        // previous frame did not see it (no measured vector to trust there).
        const float inv_z = WorldInverseDepth(depth);
        if ((g.options.y & FG_FLAG_WORLD) != 0u && inv_z >= 0.0 && depth < FG_WORLD_SKY &&
            g_world.Load(int3(0, 0, 0)).w > 0.0) {
            result = float4(WorldFlow(g_world, uv, inv_z) / WorldTangentScale(), FG_WORLD_COST, max(zero, 0.0));
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
        const uint packed = g_shift.Load(int3(min(int2(pixel >> 1), int2(width, height) - 1), 0));
        const float2 back = packed == FG_SHIFT_NONE ? float2(0.0, 0.0)
                                                    : float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16));
        uv += time / max(map_time, 1e-6) * back / float2(width, height);
    }
#endif
    return uv;
}

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
    // the newest frame at uv + step * v.
    const float step = extrapolate ? -fraction : 1.0 - fraction;
    float4 chosen = float4(0.0, 0.0, 0.0, 1.0);
    float chosen_error = 0.0;
    int occlusion = 0; // 1: newest frame alone, 2: previous frame alone (Occlusion)
    float2 occlusion_v = float2(0.0, 0.0);
    float occlusion_depth = 0.0;
    if (!still) {
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
        solved = g_flow.SampleLevel(g_linear, uv + step * solved.xy + flow_offset, 0.0);
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
            [unroll]
            for (int k = 1; k < 7; ++k) {
                const float error = CandidateError(uv, candidates[k].xy, step) + (k == 6 ? 0.03 : 0.01);
                if (error < chosen_error) {
                    chosen_error = error;
                    chosen = candidates[k];
                }
            }
            if (!extrapolate && (g.options.y & (FG_FLAG_OCCLUSION | FG_FLAG_DEPTH)) == (FG_FLAG_OCCLUSION | FG_FLAG_DEPTH) &&
                chosen_error > FG_OCCLUSION_ERROR) {
                occlusion = Occlusion(uv, step, candidates, candidate_depth, occlusion_v, occlusion_depth);
            }
        }
    }

    const float shift = length(step * chosen.xy * aspect);
    const float motion_confidence = saturate((limit - shift) / (0.25 * limit));
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
    const float4 current = g_current.SampleLevel(g_linear, source, 0.0);
    float4 result = current;
    float photometric_confidence = 1.0;
    if (occlusion == 1) {
        // Revealed (or in front): the newest frame alone.
        result = g_current.SampleLevel(g_linear, uv + confidence * step * occlusion_v, 0.0);
    } else if (occlusion == 2) {
        // Being covered: the previous frame alone, where the content was.
        float2 previous_uv;
        if (MapToPrevious(uv - confidence * fraction * occlusion_v, occlusion_depth, previous_uv)) {
            result = g_previous.SampleLevel(g_linear, previous_uv, 0.0);
        }
    } else if (!extrapolate && !still) {
        float2 previous_uv;
        if (MapToPrevious(source - confidence * chosen.xy, DepthAt(source), previous_uv)) {
            const float4 previous = g_previous.SampleLevel(g_linear, previous_uv, 0.0);
            const float difference = MaxComponent(abs(Perceptual(previous.rgb) - Perceptual(current.rgb)));
            photometric_confidence = saturate(1.0 - max(difference - 0.03, 0.0) * g.tuning.w);
            result = lerp(current, previous, (1.0 - fraction) * photometric_confidence);
            if ((g.options.y & FG_FLAG_CROSSFADE) != 0u) {
                // Research: untrusted motion fades between the two real
                // frames where they are, instead of showing the newest one.
                float2 still_uv;
                const float4 newest_here = g_current.SampleLevel(g_linear, uv, 0.0);
                const float4 previous_here = MapToPrevious(uv, DepthAt(uv), still_uv)
                                                 ? g_previous.SampleLevel(g_linear, still_uv, 0.0)
                                                 : newest_here;
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
        const float magnitude = saturate(length(v) * 20.0);
        if (magnitude > 0.0) {
            // atan2(0, 0) is undefined on some GPUs; zero motion keeps the image.
            const float3 tint = Hue(frac(atan2(v.y, v.x) / 6.2831853 + 1.0)) * magnitude;
            result.rgb = lerp(result.rgb, tint, 0.6 * saturate(magnitude * 4.0));
        }
    } else if (view == FG_VIEW_CONFIDENCE) {
        const float trust = confidence * photometric_confidence;
        result.rgb = lerp(result.rgb, float3(1.0, 0.1, 0.05), 0.75 * (1.0 - trust));
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
// eyes see, how many of them are nearer than FG_CHECK_NEAR meters, 0, 0),
// then the probes' error sums (j = 0..3) and (j = 4, probed samples, 0, 0).
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
    [loop]
    for (int sy = 0; sy < samples; ++sy) {
        [loop]
        for (int sx = 0; sx < samples; ++sx) {
            const float2 uv = (float2(id.xy) + (float2(sx, sy) + 0.5) / float(samples)) / float2(g.size.xy);
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
    g_scores[base + int2(2, 0)] = float4(binocular, cockpit, 0.0, 0.0);
    g_scores[base + int2(3, 0)] = float4(probes[0], probes[1], probes[2], probes[3]);
    g_scores[base + int2(4, 0)] = float4(probes[4], probed, 0.0, 0.0);
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

#if defined(FG_PASS_FLOW_UNPACK)
FG_BIND(0) Texture2D<int2> g_hardware_flow : register(t0);
FG_BIND(1) Texture2D<float4> g_luma_current : register(t1);
FG_BIND(2) Texture2D<float4> g_luma_previous : register(t2);
FG_BIND(3) Texture2D<float4> g_depth : register(t3);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_flow : register(u0);

// options.x: the hardware matched the frames as rendered (FG_PASS_FLOW_PACK_RAW).
#define FG_UNPACK_RAW 1u

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

#if defined(FG_PASS_SHIFT_SPLAT) || defined(FG_PASS_SHIFT_RESOLVE)
FG_BIND(0) Texture2D<float4> g_depth : register(t0);

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
bool Landing(uint2 id, out float2 landing, out uint key) {
    const float2 size = float2(g.size.xy);
    const float2 uv = (float2(id) + 0.5) / size;
    const float depth = g_depth.Load(int3(id, 0)).r;
    landing = uv * size;
    key = 0u;
    if (!(depth > 0.0) || depth >= FG_FAR_DEPTH) {
        return false;
    }
    float2 shifted;
    const bool inside = MapToPrevious(uv, depth, shifted);
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
    if (Landing(id.xy, landing, key)) {
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
    if (all(id.xy < g.size.xy) && Landing(id.xy, landing, key)) {
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
