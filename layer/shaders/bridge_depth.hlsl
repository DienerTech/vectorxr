// The Direct3D 11 bridge's depth (framegen_d3d11_bridge.cpp). Direct3D 12
// shares no depth texture with simultaneous access, and Direct3D 11 opens a
// Direct3D 12 texture only as a render target, so a game's depth reaches the
// runtime's Direct3D 12 depth image in two passes through a shared twin
// (R16_UNORM for D16, R32_FLOAT otherwise): Direct3D 11 copies the game's
// depth into the twin at its release
// (BRIDGE_DEPTH_TO_TWIN), and the bridge's queue writes the twin into the
// runtime's image as depth (BRIDGE_DEPTH_VS and BRIDGE_DEPTH_PS). The depth
// view gives every format's depth as a float (D16 and D24 normalized), and
// writing it as SV_Depth converts it back, so each value arrives as it was.

#if defined(BRIDGE_DEPTH_TO_TWIN)

// The game's depth, viewed as its depth plane (R32_FLOAT,
// R32_FLOAT_X8X24_TYPELESS, R24_UNORM_X8_TYPELESS or R16_UNORM).
Texture2DArray<float> g_depth : register(t0);
RWTexture2DArray<float> g_twin : register(u0);

[numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    uint width, height, slices;
    g_twin.GetDimensions(width, height, slices);
    if (id.x < width && id.y < height) {
        g_twin[id] = g_depth.Load(int4(id, 0));
    }
}

#elif defined(BRIDGE_DEPTH_VS)

// One triangle covering the viewport.
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

#elif defined(BRIDGE_DEPTH_PS)

Texture2DArray<float> g_twin : register(t0);
cbuffer Slice : register(b0) {
    uint g_slice;
};

float PSMain(float4 position : SV_Position) : SV_Depth {
    return g_twin.Load(int4(int2(position.xy), g_slice, 0));
}

#endif
