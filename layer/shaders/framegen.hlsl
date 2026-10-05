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
// reprojection corrects head motion for every submitted frame.

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

// Linear depth at or beyond this many meters is treated as infinitely far.
#define FG_FAR_DEPTH 100000.0

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

#if defined(FG_PASS_LUMA)
FG_BIND(0) Texture2D<float4> g_current : register(t0);
FG_BIND(1) Texture2D<float4> g_previous : register(t1);
FG_BIND(2) Texture2D<float4> g_depth : register(t2);
FG_BIND(6) SamplerState g_linear : register(s0);
FG_BIND(4) FG_FORMAT("r32f") RWTexture2D<float> g_luma_current : register(u0);
FG_BIND(5) FG_FORMAT("r32f") RWTexture2D<float> g_luma_previous : register(u1);

// Half-resolution luma of the newest frame and of the previous frame rotated
// into the newest view. Invalid previous texels are marked negative.
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
    g_luma_previous[id.xy] = MapToPrevious(uv, depth, previous_uv)
        ? Luma(g_previous.SampleLevel(g_linear, previous_uv, 0.0).rgb)
        : -1.0;
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
// Mean absolute luma difference of a 5x5 patch around `cell` displaced by
// -d. Newest texels outside the image are skipped.
float PatchCost(int2 cell, int2 d, int2 extent) {
    float sum = 0.0;
    float count = 0.0;
    [loop]
    for (int y = -FG_PATCH_RADIUS; y <= FG_PATCH_RADIUS; ++y) {
        [unroll]
        for (int x = -FG_PATCH_RADIUS; x <= FG_PATCH_RADIUS; ++x) {
            const int2 q = cell + int2(x, y);
            if (any(q < 0) || any(q >= extent)) {
                continue;
            }
            sum += MatchTerm(g_luma_current.Load(int3(q, 0)).r, PreviousLuma(q - d, extent));
            count += 1.0;
        }
    }
    return sum / max(count, 1.0) + MotionTerm(d, extent);
}

// Exhaustive search on the coarsest level.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int2 cell = int2(id.xy);
    const int2 extent = MatchExtent();
    const int radius = int(g.options.x);
    int2 best = int2(0, 0);
    float best_cost = PatchCost(cell, best, extent);
    [loop]
    for (int y = -radius; y <= radius; ++y) {
        [loop]
        for (int x = -radius; x <= radius; ++x) {
            const float cost = PatchCost(cell, int2(x, y), extent);
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
    int2 first;
    int2 extent;
};

Block LoadBlock(int2 cell, int scale) {
    Block block;
    block.extent = MatchExtent();
    block.first = cell * scale - (scale >= FG_BLOCK ? 0 : 1);
    [unroll]
    for (int i = 0; i < FG_BLOCK * FG_BLOCK; ++i) {
        const int2 q = block.first + int2(i % FG_BLOCK, i / FG_BLOCK);
        block.valid[i] = all(q >= 0) && all(q < block.extent);
        block.luma[i] = block.valid[i] ? g_luma_current.Load(int3(q, 0)).r : 0.0;
    }
    return block;
}

float BlockCost(Block block, int2 d) {
    float sum = 0.0;
    float count = 0.0;
    [unroll]
    for (int i = 0; i < FG_BLOCK * FG_BLOCK; ++i) {
        if (block.valid[i]) {
            const int2 q = block.first + int2(i % FG_BLOCK, i / FG_BLOCK);
            sum += MatchTerm(block.luma[i], PreviousLuma(q - d, block.extent));
            count += 1.0;
        }
    }
    return sum / max(count, 1.0) + MotionTerm(d, block.extent);
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
        const float2 uv = (float2(cell) + 0.5) / float2(g.size.xy);
        Consider(block, int2(round(g_temporal.SampleLevel(g_linear, uv, 0.0).xy * match)), best, best_cost);
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
    g_flow[id.xy] = float4((float2(best) + sub) / match, best_cost, 1.0);
}
#endif

#if defined(FG_PASS_FILTER)
FG_BIND(0) Texture2D<float4> g_flow_in : register(t0);
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_flow_out : register(u0);

// Cost-weighted vector median over 3x3: removes isolated mismatches without
// blurring motion boundaries the way averaging would.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const int2 limit = int2(g.size.xy) - 1;
    const float2 aspect = float2(1.0, float(g.size.y) / float(g.size.x));
    float4 samples[9];
    [unroll]
    for (int i = 0; i < 9; ++i) {
        const int2 q = clamp(int2(id.xy) + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), limit);
        samples[i] = g_flow_in.Load(int3(q, 0));
    }
    int chosen = 4;
    float chosen_score = 1e30;
    [unroll]
    for (int c = 0; c < 9; ++c) {
        float score = 0.0;
        [unroll]
        for (int j = 0; j < 9; ++j) {
            const float weight = 1.0 / (samples[j].z + 0.01);
            score += weight * length((samples[c].xy - samples[j].xy) * aspect);
        }
        if (score < chosen_score) {
            chosen_score = score;
            chosen = c;
        }
    }
    g_flow_out[id.xy] = float4(samples[chosen].xy, min(samples[chosen].z, samples[4].z), 1.0);
}
#endif

#if defined(FG_PASS_SYNTHESIZE_UNORM8) || defined(FG_PASS_SYNTHESIZE_FLOAT16)
FG_BIND(0) Texture2D<float4> g_current : register(t0);
FG_BIND(1) Texture2D<float4> g_previous : register(t1);
FG_BIND(2) Texture2D<float4> g_flow : register(t2);
FG_BIND(3) Texture2D<float4> g_depth : register(t3);
FG_BIND(6) SamplerState g_linear : register(s0);
#if defined(FG_PASS_SYNTHESIZE_UNORM8)
FG_BIND(4) FG_FORMAT("rgba8") RWTexture2D<float4> g_output : register(u0);
#else
FG_BIND(4) FG_FORMAT("rgba16f") RWTexture2D<float4> g_output : register(u0);
#endif

float3 LinearToSrgb(float3 value) {
    const float3 c = saturate(value);
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, step(c, 0.0031308));
}

float3 Hue(float h) {
    return saturate(float3(abs(h * 6.0 - 3.0) - 1.0, 2.0 - abs(h * 6.0 - 2.0), 2.0 - abs(h * 6.0 - 4.0)));
}

float MaxComponent(float3 value) {
    return max(value.x, max(value.y, value.z));
}

float3 Perceptual(float3 value) {
    return sqrt(max(value, 0.0));
}

// Newest-frame linear depth at a newest-view coordinate (nearest texel: a
// filtered depth across a silhouette belongs to neither surface).
float DepthAt(float2 uv) {
    if ((g.options.y & FG_FLAG_DEPTH) == 0u) {
        return 0.0;
    }
    uint width, height;
    g_depth.GetDimensions(width, height);
    const int2 texel = clamp(int2(uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
    return g_depth.Load(int3(texel, 0)).r;
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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g.size.xy)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(g.size.xy);
    const float2 aspect = float2(1.0, float(g.size.y) / float(g.size.x));
    const bool extrapolate = (g.options.y & FG_FLAG_EXTRAPOLATE) != 0u;
    const bool still = (g.options.y & FG_FLAG_STILL) != 0u;
    const float fraction = g.tuning.x;
    const float limit = max(g.tuning.y, 1e-4);
    bool boundary = false;

    // Content at x in the newest frame sits at x - (1 - f) * v at the
    // interpolated time, or at x + a * v when extrapolating; this pixel reads
    // the newest frame at uv + step * v.
    const float step = extrapolate ? -fraction : 1.0 - fraction;
    float4 chosen = float4(0.0, 0.0, 0.0, 1.0);
    float chosen_error = 0.0;
    if (!still) {
        // Candidates: the smooth field solved with a fixed-point iteration,
        // the field at this pixel, the four surrounding block vectors, and
        // zero motion. Later candidates carry a small bias so noise alone
        // never pulls a pixel off the smooth field.
        float4 candidates[7];
        const float4 here = g_flow.SampleLevel(g_linear, uv, 0.0);
        float4 solved = g_flow.SampleLevel(g_linear, uv + step * here.xy, 0.0);
        solved = g_flow.SampleLevel(g_linear, uv + step * solved.xy, 0.0);
        candidates[0] = solved;
        candidates[1] = here;
        uint flow_width, flow_height;
        g_flow.GetDimensions(flow_width, flow_height);
        const int2 limit_cell = int2(flow_width, flow_height) - 1;
        const int2 base = int2(floor(uv * float2(flow_width, flow_height) - 0.5));
        [unroll]
        for (int i = 0; i < 4; ++i) {
            const int2 cell = clamp(base + int2(i & 1, i >> 1), int2(0, 0), limit_cell);
            candidates[2 + i] = g_flow.Load(int3(cell, 0));
        }
        candidates[6] = float4(0.0, 0.0, here.z, 1.0);
        chosen = candidates[0];
        chosen_error = CandidateError(uv, chosen.xy, step);
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
        if (boundary) {
            [unroll]
            for (int k = 1; k < 7; ++k) {
                const float error = CandidateError(uv, candidates[k].xy, step) + (k == 6 ? 0.03 : 0.01);
                if (error < chosen_error) {
                    chosen_error = error;
                    chosen = candidates[k];
                }
            }
        }
    }

    const float shift = length(step * chosen.xy * aspect);
    const float motion_confidence = saturate((limit - shift) / (0.25 * limit));
    const float cost_confidence = saturate(1.0 - max(chosen.z - 0.02, 0.0) * g.tuning.z);
    const float agreement = saturate(1.0 - max(chosen_error - 0.03, 0.0) * g.tuning.w);
    // Uncertain motion shrinks toward a still image instead of blending two
    // positions, which would ghost.
    float confidence = motion_confidence * cost_confidence;
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
    if (!extrapolate && !still) {
        float2 previous_uv;
        if (MapToPrevious(source - confidence * chosen.xy, DepthAt(source), previous_uv)) {
            const float4 previous = g_previous.SampleLevel(g_linear, previous_uv, 0.0);
            const float difference = MaxComponent(abs(Perceptual(previous.rgb) - Perceptual(current.rgb)));
            photometric_confidence = saturate(1.0 - max(difference - 0.03, 0.0) * g.tuning.w);
            result = lerp(current, previous, (1.0 - fraction) * photometric_confidence);
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
    g_output[id.xy] = result;
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
