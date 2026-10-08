#include "depthxr/quadviews_compose.h"

namespace depthxr {

const char* QuadViewsD3D11ShaderSource() {
    return R"(
cbuffer QuadViewsConstants : register(b0) {
    float4 focusRect;
    float4 blendParams;
    float4 focusTexel;
    float4 peripheralSrcRect;
    float4 focusSrcRect;
    float4 diagnosticParams;
    float4 outputTexel;
    float4 gazeMarkers;
    float4 referenceMarkers;
};

Texture2D peripheralTexture : register(t0);
Texture2D focusTexture : register(t1);
SamplerState linearSampler : register(s0);

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID) {
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VSOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

float RectOutlineMask(float2 uv, float2 rectMin, float2 rectMax, float thicknessPixels) {
    float2 pixelSize = max(outputTexel.xy, float2(0.000001, 0.000001));
    float2 center = (rectMin + rectMax) * 0.5;
    float2 halfSizePixels = max((rectMax - rectMin) * 0.5 / pixelSize, float2(0.0, 0.0));
    float2 pointPixels = (uv - center) / pixelSize;
    float2 q = abs(pointPixels) - halfSizePixels;
    float signedDistance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
    return 1.0 - smoothstep(thicknessPixels, thicknessPixels + 1.0, abs(signedDistance));
}

float CrossMask(float2 uv, float2 center, float lengthPixels, float thicknessPixels) {
    float2 deltaPixels = abs((uv - center) / max(outputTexel.xy, float2(0.000001, 0.000001)));
    float horizontal = (1.0 - smoothstep(thicknessPixels, thicknessPixels + 1.0, deltaPixels.y)) *
                       (1.0 - smoothstep(lengthPixels, lengthPixels + 1.0, deltaPixels.x));
    float vertical = (1.0 - smoothstep(thicknessPixels, thicknessPixels + 1.0, deltaPixels.x)) *
                     (1.0 - smoothstep(lengthPixels, lengthPixels + 1.0, deltaPixels.y));
    return max(horizontal, vertical);
}

float RingMask(float2 uv, float2 center, float radiusPixels, float thicknessPixels) {
    float2 deltaPixels = (uv - center) / max(outputTexel.xy, float2(0.000001, 0.000001));
    float ringDistance = abs(length(deltaPixels) - radiusPixels);
    return 1.0 - smoothstep(thicknessPixels, thicknessPixels + 1.0, ringDistance);
}

float SegmentMask(float2 uv, float2 start, float2 end, float thicknessPixels) {
    float2 pixelSize = max(outputTexel.xy, float2(0.000001, 0.000001));
    float2 offsetPixels = (uv - start) / pixelSize;
    float2 segment = (end - start) / pixelSize;
    float segmentLengthSquared = max(dot(segment, segment), 0.0001);
    float along = saturate(dot(offsetPixels, segment) / segmentLengthSquared);
    float distancePixels = length(offsetPixels - segment * along);
    return 1.0 - smoothstep(thicknessPixels, thicknessPixels + 1.0, distancePixels);
}


float4 PSMain(VSOut input) : SV_Target {
    float2 uv = saturate(input.uv);
    float2 peripheralUv = peripheralSrcRect.xy + uv * peripheralSrcRect.zw;
    float4 peripheral = peripheralTexture.Sample(linearSampler, peripheralUv);

    float2 inside = step(focusRect.xy, uv) * step(uv, focusRect.zw);
    float inRect = inside.x * inside.y;
    float edgeAlpha = 0.0;
    float4 composed = peripheral;

    if (inRect >= 0.5) {
        float2 distToEdge = min(uv - focusRect.xy, focusRect.zw - uv);
        edgeAlpha = saturate(min(distToEdge.x / max(blendParams.x, 0.0001),
                                 distToEdge.y / max(blendParams.y, 0.0001)));
        edgeAlpha = edgeAlpha * edgeAlpha * (3.0 - 2.0 * edgeAlpha);

        float2 focusUv = saturate((uv - focusRect.xy) / max(focusRect.zw - focusRect.xy, float2(0.0001, 0.0001)));
        float2 focusSampleUv = focusSrcRect.xy + focusUv * focusSrcRect.zw;
        float2 focusSampleMin = focusSrcRect.xy;
        float2 focusSampleMax = focusSrcRect.xy + focusSrcRect.zw;
        float4 focus = focusTexture.Sample(linearSampler, focusSampleUv);
        // Sharpening follows the same smooth transition as the focus image. This prevents
        // maximum local contrast from appearing at the first pixels of the feather.
        float sharpenAmount = saturate(blendParams.z) * edgeAlpha;
        if (sharpenAmount > 0.001) {
            // Sample neighbors at output-pixel spacing (focusTexel.zw is one output pixel expressed
            // in focus-UV), not focus-texel spacing. The focus texture is minified into the focus
            // rect, so a focus-texel-scale unsharp mask synthesizes detail that the resample averages
            // away; output-space offsets target the frequencies the user actually sees.
            float2 off = focusTexel.zw * focusSrcRect.zw;
            float3 n = focusTexture.Sample(linearSampler, clamp(focusSampleUv + float2(0.0, -off.y), focusSampleMin, focusSampleMax)).rgb;
            float3 s = focusTexture.Sample(linearSampler, clamp(focusSampleUv + float2(0.0,  off.y), focusSampleMin, focusSampleMax)).rgb;
            float3 e = focusTexture.Sample(linearSampler, clamp(focusSampleUv + float2( off.x, 0.0), focusSampleMin, focusSampleMax)).rgb;
            float3 w = focusTexture.Sample(linearSampler, clamp(focusSampleUv + float2(-off.x, 0.0), focusSampleMin, focusSampleMax)).rgb;
            float3 avg = (n + s + e + w) * 0.25;
            float3 lo = min(focus.rgb, min(min(n, s), min(e, w)));
            float3 hi = max(focus.rgb, max(max(n, s), max(e, w)));
            float3 localRange = max(hi - lo, float3(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0));
            float3 detail = (focus.rgb - avg) * (sharpenAmount * 2.0);
            detail = clamp(detail, -localRange * 0.5, localRange * 0.5);
            focus.rgb = saturate(focus.rgb + detail);
        }
        composed = lerp(peripheral, focus, edgeAlpha);
    }

    if (diagnosticParams.x < 0.5) {
        return composed;
    }

    float transition = inRect * (1.0 - step(0.999, edgeAlpha));
    float3 zoneColor = inRect < 0.5 ? float3(0.08, 0.28, 0.95) :
                       (transition > 0.5 ? float3(1.0, 0.55, 0.04) : float3(0.08, 0.85, 0.28));
    float zoneStrength = inRect < 0.5 ? 0.13 : (transition > 0.5 ? 0.20 : 0.055);
    composed.rgb = lerp(composed.rgb, zoneColor, zoneStrength);

    float trackingUnavailable = diagnosticParams.y * (1.0 - diagnosticParams.z);
    float3 focusOutlineColor = lerp(float3(0.15, 1.0, 0.3), float3(1.0, 0.08, 0.05), trackingUnavailable);
    float focusOutline = RectOutlineMask(uv, focusRect.xy, focusRect.zw, 2.0);
    composed.rgb = lerp(composed.rgb, focusOutlineColor, focusOutline);

    float2 innerMin = min(focusRect.xy + blendParams.xy, focusRect.zw);
    float2 innerMax = max(focusRect.zw - blendParams.xy, focusRect.xy);
    float innerOutline = RectOutlineMask(uv, innerMin, innerMax, 1.25) *
                         step(outputTexel.x * 2.0, blendParams.x) * step(outputTexel.y * 2.0, blendParams.y);
    composed.rgb = lerp(composed.rgb, float3(1.0, 0.65, 0.05), innerOutline * 0.9);

    float2 offsetCorner = float2(referenceMarkers.z, referenceMarkers.y);
    float offsetAxes = max(SegmentMask(uv, referenceMarkers.xy, offsetCorner, 1.0),
                           SegmentMask(uv, offsetCorner, referenceMarkers.zw, 1.0));
    float headMarker = CrossMask(uv, referenceMarkers.xy, 14.0, 1.5);
    float offsetMarker = RingMask(uv, referenceMarkers.zw, 12.0, 1.5);
    float rawGazeMarker = RingMask(uv, gazeMarkers.xy, 7.0, 1.75) * diagnosticParams.z;
    float smoothedGazeMarker = CrossMask(uv, gazeMarkers.zw, 8.0, 1.5) * diagnosticParams.z;
    composed.rgb = lerp(composed.rgb, float3(1.0, 0.1, 0.85), offsetAxes * 0.8);
    composed.rgb = lerp(composed.rgb, float3(1.0, 1.0, 1.0), headMarker);
    composed.rgb = lerp(composed.rgb, float3(1.0, 0.1, 0.85), offsetMarker);
    composed.rgb = lerp(composed.rgb, float3(0.0, 0.95, 1.0), rawGazeMarker);
    composed.rgb = lerp(composed.rgb, float3(1.0, 0.95, 0.05), smoothedGazeMarker);
    return composed;
}
)";
}

} // namespace depthxr
