#pragma once

namespace depthxr {

// HLSL of the quad views compositor (Direct3D 11): VSMain draws a full-screen
// triangle and PSMain composes one eye from its peripheral (t0) and focus
// (t1) views with the QuadViewsConstants cbuffer (b0), whose focus rectangle,
// blend band and sharpening BuildFrameGenComposeEye computes.
const char* QuadViewsD3D11ShaderSource();

} // namespace depthxr
