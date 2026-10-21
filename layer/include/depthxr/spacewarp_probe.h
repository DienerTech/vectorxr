#pragma once

// A research probe, off by default: does an application send its engine's
// motion vectors through XR_FB_space_warp or XR_EXT_frame_synthesis when
// VectorXR advertises them on a runtime that has neither? The probe's
// manifests (spacewarp-probe-fb and spacewarp-probe-ext beside the layer's
// own, each listing one of them and loaded only while VECTORXR_SPACEWARP_PROBE
// is set) advertise them; VECTORXR_SPACEWARP_PROBE=fb|ext|both turns the
// layer's side on. The layer then keeps the extension from the runtime,
// answers its recommended motion vector size and takes its structure off each
// projection view before xrEndFrame, logging what it carried. Nothing uses the
// motion vectors yet. A runtime that lists the extension gets it as it is.

#include <openxr/openxr.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace depthxr {

struct SpaceWarpProbeSelection {
    bool fb{false};  // XR_FB_space_warp
    bool ext{false}; // XR_EXT_frame_synthesis

    bool Any() const { return fb || ext; }
};

// "fb", "ext" or "both" (any case); anything else is off.
SpaceWarpProbeSelection ParseSpaceWarpProbe(const char* value);
// VECTORXR_SPACEWARP_PROBE.
SpaceWarpProbeSelection ReadSpaceWarpProbeEnvironment();
// "off", "fb", "ext" or "both".
const char* SpaceWarpProbeName(SpaceWarpProbeSelection selection);

// The layer takes an extension over while the probe is on, the application
// enabled it and the runtime's list before the instance does not name it (a
// list that could not be read names nothing: the runtime would refuse an
// extension it lacks, so the instance would fail).
bool SpaceWarpProbeOwns(bool probe_on, bool application_enabled, bool runtime_lists);

// The motion vector image the layer recommends for an eye's recommended
// image: a quarter of it each way, at least 1x1.
XrExtent2Di RecommendedMotionVectorSize(std::uint32_t eye_width, std::uint32_t eye_height);

// RGBA 16-bit float, the format both extensions take motion vectors in:
// DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT or GL_RGBA16F.
bool IsHalfFloatRgbaFormat(std::int64_t format);

// One projection view's structure, of either extension.
struct SpaceWarpViewInfo {
    XrStructureType type{XR_TYPE_UNKNOWN}; // XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB or _FRAME_SYNTHESIS_INFO_EXT
    std::uint32_t layer_index{0};
    std::uint32_t view_index{0};
    std::uint64_t layer_flags{0};
    XrSwapchainSubImage motion_vector{};
    XrSwapchainSubImage depth{};
    XrPosef app_space_delta_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float min_depth{0.0f};
    float max_depth{0.0f};
    float near_z{0.0f};
    float far_z{0.0f};
    // XR_EXT_frame_synthesis only.
    XrVector4f motion_vector_scale{};
    XrVector4f motion_vector_offset{};
    bool stripped{false};
};

// A frame without the probe's structures, and the copies it needs.
struct SpaceWarpFrame {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::deque<XrCompositionLayerProjection> projections;
    std::deque<std::vector<XrCompositionLayerProjectionView>> views;
    std::deque<XrCompositionLayerDepthInfoKHR> depth_infos;
    // Every structure found, stripped or not, in layer then view order.
    std::vector<SpaceWarpViewInfo> found;
};

// `frame` itself when no projection view carries a structure of an extension
// to strip (`strip_fb`, `strip_ext`); otherwise a copy whose views' chains
// lack them. A structure behind XrCompositionLayerDepthInfoKHR is reached by
// copying that; behind a structure the probe does not know it stays (its
// SpaceWarpViewInfo says so), since that cannot be copied.
const XrFrameEndInfo* WithoutSpaceWarp(const XrFrameEndInfo* frame, bool strip_fb, bool strip_ext,
                                       SpaceWarpFrame& storage);

// The structure's fields, without its swapchains' own descriptions.
std::string DescribeSpaceWarpView(const SpaceWarpViewInfo& view);

} // namespace depthxr
