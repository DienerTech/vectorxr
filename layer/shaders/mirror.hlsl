// Stabilized mirror window (mirror.h, submitted_image_mirror.cpp): one eye's
// view rectangle of the game's image, drawn into a desktop window's back
// buffer through a homography (the window re-aimed by a rotation). Direct3D
// 11 and 12 share the two shaders.
//
// Each window pixel is the mean of four bilinear samples around its centre,
// spread by how much the picture is shrunk (none where it is not: the texels
// as they are). Samples outside the rectangle are black. Texels that are
// linear (MIRROR_FLAG_ENCODE_SRGB) are encoded for the window, whose back
// buffer holds display-encoded values. Alpha is 1.

#if defined(MIRROR_VS)

// One triangle covering the viewport.
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

#elif defined(MIRROR_PS)

#define MIRROR_FLAG_ENCODE_SRGB 1u

// One array slice of the image (a view of that slice alone).
Texture2DArray<float4> g_source : register(t0);

cbuffer MirrorConstantBuffer : register(b0) {
    float4 g_row0;
    float4 g_row1;
    float4 g_row2;
    int2 g_source_offset;
    int2 g_source_size;
    float2 g_target_size;
    uint g_flags;
    float g_spread;
};

float3 Texel(int2 texel) {
    return g_source.Load(int4(g_source_offset + clamp(texel, int2(0, 0), g_source_size - 1), 0, 0)).rgb;
}

float3 Sample(float2 pixel) {
    const float3 point_ = float3(pixel.x / g_target_size.x * 2.0 - 1.0, 1.0 - pixel.y / g_target_size.y * 2.0, 1.0);
    const float w = dot(g_row2.xyz, point_);
    if (w <= 1e-6) {
        return float3(0.0, 0.0, 0.0);
    }
    const float2 uv = float2(dot(g_row0.xyz, point_), dot(g_row1.xyz, point_)) / w;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float2 position = uv * float2(g_source_size) - 0.5;
    const float2 base = floor(position);
    const float2 part = position - base;
    const int2 texel = int2(base);
    const float3 top = lerp(Texel(texel), Texel(texel + int2(1, 0)), part.x);
    const float3 bottom = lerp(Texel(texel + int2(0, 1)), Texel(texel + int2(1, 1)), part.x);
    return lerp(top, bottom, part.y);
}

float3 SrgbEncode(float3 value) {
    const float3 clamped = saturate(value);
    return lerp(1.055 * pow(clamped, 1.0 / 2.4) - 0.055, 12.92 * clamped, step(clamped, 0.0031308));
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    const float offset = 0.25 * g_spread;
    float3 color = Sample(position.xy + float2(-offset, -offset));
    color += Sample(position.xy + float2(offset, -offset));
    color += Sample(position.xy + float2(-offset, offset));
    color += Sample(position.xy + float2(offset, offset));
    color *= 0.25;
    if (g_flags & MIRROR_FLAG_ENCODE_SRGB) {
        color = SrgbEncode(color);
    }
    return float4(color, 1.0);
}

#endif
