// The space-warp probe (spacewarp_probe.h): its frame handling on its own,
// then through the layer's own hooks, from xrCreateApiLayerInstance on,
// against a stand-in runtime (CPU only): the layer keeps XR_FB_space_warp and
// XR_EXT_frame_synthesis from a runtime that lacks them while
// VECTORXR_SPACEWARP_PROBE is set, answers their recommended motion vector
// sizes and takes their structures off the frames the runtime gets, handing
// Frame Generation a record of each frame's (engine_motion.h); a runtime that
// lists one, or the probe off, gets the application's own. Settings and logs
// go to a temporary directory.
#include <windows.h>

#include "depthxr/engine_motion.h"
#include "depthxr/openxr_layer.h"
#include "depthxr/openxr_loader_api_layer.h"
#include "depthxr/spacewarp_probe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// The layer's loader entry point (layer_entry.cpp).
extern "C" XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                        const depthxr::XrApiLayerCreateInfo* layer_info,
                                                        XrInstance* instance);

namespace depthxr {

// Test-only access to the singleton (DEPTHXR_TESTING).
class SpaceWarpProbeTestPeer {
  public:
    static void SetActiveSession(XrSession session) {
        std::scoped_lock lock(OpenXrLayer::Instance().mutex_);
        OpenXrLayer::Instance().active_session_ = session;
    }
    // Frame Generation's engine motion records as the layer handed them
    // over: the last frame's, the one before's, and the session's counts.
    struct EngineRecords {
        FrameGenEngineMotionFrame frame;
        FrameGenEngineMotionFrame previous;
        std::uint64_t noted{0};
        std::uint64_t invalid{0};
    };
    static EngineRecords Engine() {
        FrameGenPresenter& presenter = OpenXrLayer::Instance().framegen_;
        std::scoped_lock lock(presenter.mutex_);
        return {presenter.engine_frame_.frame, presenter.engine_previous_.frame, presenter.engine_noted_,
                presenter.engine_invalid_};
    }
    // The last instance's log (an error reaches the file at once, with
    // everything logged before it).
    static std::string Log() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        layer.logger_.Error("Test harness: log read.");
        std::ifstream stream(layer.logger_.ActiveLogPath(), std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        const std::string log = text.str();
        const std::size_t start = log.rfind("VectorXR layer version: ");
        return start == std::string::npos ? log : log.substr(start);
    }
};

} // namespace depthxr

namespace {

using depthxr::SpaceWarpFrame;
using depthxr::SpaceWarpProbeTestPeer;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
T Handle(std::uintptr_t value) {
    return reinterpret_cast<T>(value);
}

constexpr XrSystemId kSystem = 1;
constexpr std::uint32_t kEyeWidth = 2000;
constexpr std::uint32_t kEyeHeight = 2400;
const XrSession kSession = Handle<XrSession>(0x5e55);
const XrSwapchain kColor = Handle<XrSwapchain>(0xc0);
const XrSwapchain kDepth = Handle<XrSwapchain>(0xd0);
const XrSwapchain kMotion = Handle<XrSwapchain>(0x30);
const XrSwapchain kMotionDepth = Handle<XrSwapchain>(0x3d);
constexpr XrStructureType kUnknownType = static_cast<XrStructureType>(1999999999);

// --- The probe's frame handling on its own -----------------------------------

XrCompositionLayerSpaceWarpInfoFB SpaceWarpInfo(const void* next = nullptr) {
    XrCompositionLayerSpaceWarpInfoFB info{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB};
    info.next = next;
    info.layerFlags = XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB;
    info.motionVectorSubImage.swapchain = kMotion;
    info.motionVectorSubImage.imageRect = {{0, 0}, {500, 600}};
    info.depthSubImage.swapchain = kMotionDepth;
    info.depthSubImage.imageRect = {{0, 0}, {500, 600}};
    info.appSpaceDeltaPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -0.25f}};
    info.minDepth = 0.0f;
    info.maxDepth = 1.0f;
    info.nearZ = 0.05f;
    info.farZ = 10000.0f;
    return info;
}

XrFrameSynthesisInfoEXT FrameSynthesisInfo(const void* next = nullptr) {
    XrFrameSynthesisInfoEXT info{XR_TYPE_FRAME_SYNTHESIS_INFO_EXT};
    info.next = next;
    info.motionVectorSubImage.swapchain = kMotion;
    info.motionVectorSubImage.imageRect = {{0, 0}, {500, 600}};
    info.motionVectorScale = {1.0f, 1.0f, 0.0f, 0.0f};
    info.depthSubImage.swapchain = kMotionDepth;
    info.depthSubImage.imageRect = {{0, 0}, {500, 600}};
    info.nearZ = 0.05f;
    info.farZ = 10000.0f;
    return info;
}

XrCompositionLayerDepthInfoKHR DepthInfo(const void* next = nullptr) {
    XrCompositionLayerDepthInfoKHR depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
    depth.next = next;
    depth.subImage.swapchain = kDepth;
    depth.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(kEyeWidth), static_cast<int32_t>(kEyeHeight)}};
    depth.maxDepth = 1.0f;
    depth.nearZ = 0.05f;
    depth.farZ = 10000.0f;
    return depth;
}

std::vector<XrStructureType> ChainTypes(const void* next) {
    std::vector<XrStructureType> types;
    for (auto* node = static_cast<const XrBaseInStructure*>(next); node; node = node->next) {
        types.push_back(node->type);
    }
    return types;
}

// A stereo projection layer (behind a quad layer) whose views' chains are given.
struct Frame {
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[2]{};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};

    Frame(const void* left, const void* right) {
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            views[eye].pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};
            views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
            views[eye].subImage.swapchain = kColor;
            views[eye].subImage.imageRect = {{0, 0},
                                             {static_cast<int32_t>(kEyeWidth), static_cast<int32_t>(kEyeHeight)}};
            views[eye].subImage.imageArrayIndex = eye;
        }
        views[0].next = left;
        views[1].next = right;
        projection.viewCount = 2;
        projection.views = views;
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        info.displayTime = 123456789;
        info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        info.layerCount = 2;
        info.layers = layers;
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

const XrCompositionLayerProjection* Projection(const XrFrameEndInfo* frame) {
    return reinterpret_cast<const XrCompositionLayerProjection*>(frame->layers[1]);
}

void TestSelectionAndSizes() {
    using depthxr::ParseSpaceWarpProbe;
    Check(!ParseSpaceWarpProbe(nullptr).Any() && !ParseSpaceWarpProbe("").Any() && !ParseSpaceWarpProbe("1").Any() &&
              !ParseSpaceWarpProbe("on").Any(),
          "only fb, ext and both name the probe's extensions");
    _putenv_s("VECTORXR_SPACEWARP_PROBE", "On");
    Check(depthxr::ReadSpaceWarpProbeVariable() == "On" && !depthxr::ReadSpaceWarpProbeEnvironment().Any(),
          "the variable as set, whatever it names");
    _putenv_s("VECTORXR_SPACEWARP_PROBE", "");
    Check(depthxr::ReadSpaceWarpProbeVariable().empty(), "the variable unset");
    Check(ParseSpaceWarpProbe("fb").fb && !ParseSpaceWarpProbe("fb").ext, "fb is XR_FB_space_warp alone");
    Check(ParseSpaceWarpProbe("EXT").ext && !ParseSpaceWarpProbe("EXT").fb, "ext is XR_EXT_frame_synthesis alone");
    Check(ParseSpaceWarpProbe("Both").fb && ParseSpaceWarpProbe("Both").ext, "both is both");
    Check(std::string(depthxr::SpaceWarpProbeName(ParseSpaceWarpProbe("both"))) == "both" &&
              std::string(depthxr::SpaceWarpProbeName({})) == "off",
          "the probe's names");

    using depthxr::SpaceWarpProbeOwns;
    Check(SpaceWarpProbeOwns(true, true, false), "the probe takes over what the runtime lacks");
    Check(!SpaceWarpProbeOwns(true, true, true), "what the runtime lists goes to it");
    Check(!SpaceWarpProbeOwns(true, false, false), "nothing the application did not enable");
    Check(!SpaceWarpProbeOwns(false, true, false), "nothing while the probe is off");

    using depthxr::MotionVectorSize;
    const XrExtent2Di half = depthxr::RecommendedMotionVectorSize(2001, 2399, MotionVectorSize::kHalf);
    Check(half.width == 1001 && half.height == 1200, "half the eye each way, rounded up");
    const XrExtent2Di quarter = depthxr::RecommendedMotionVectorSize(2000, 2399, MotionVectorSize::kQuarter);
    Check(quarter.width == 500 && quarter.height == 600, "a quarter of the eye each way, rounded up");
    const XrExtent2Di eighth = depthxr::RecommendedMotionVectorSize(2000, 2399, MotionVectorSize::kEighth);
    Check(eighth.width == 250 && eighth.height == 300, "an eighth of the eye each way, rounded up");
    const XrExtent2Di none = depthxr::RecommendedMotionVectorSize(0, 0, MotionVectorSize::kHalf);
    Check(none.width == 1 && none.height == 1, "at least 1x1");
    // Half where Frame Generation will read the vectors (its half-resolution
    // grid), an eighth where not; VECTORXR_FRAMEGEN_ENGINE_MV_SIZE wins.
    Check(depthxr::ChooseMotionVectorSize(true, std::nullopt) == MotionVectorSize::kHalf &&
              depthxr::ChooseMotionVectorSize(false, std::nullopt) == MotionVectorSize::kEighth &&
              depthxr::ChooseMotionVectorSize(true, MotionVectorSize::kQuarter) == MotionVectorSize::kQuarter &&
              depthxr::ChooseMotionVectorSize(false, MotionVectorSize::kHalf) == MotionVectorSize::kHalf,
          "the size answered per policy");
    Check(depthxr::ParseMotionVectorSize("Half") == MotionVectorSize::kHalf &&
              depthxr::ParseMotionVectorSize("quarter") == MotionVectorSize::kQuarter &&
              depthxr::ParseMotionVectorSize("EIGHTH") == MotionVectorSize::kEighth &&
              !depthxr::ParseMotionVectorSize("2") && !depthxr::ParseMotionVectorSize("") &&
              std::string(depthxr::ToString(MotionVectorSize::kEighth)) == "eighth",
          "VECTORXR_FRAMEGEN_ENGINE_MV_SIZE's values");

    Check(depthxr::IsHalfFloatRgbaFormat(10) && depthxr::IsHalfFloatRgbaFormat(97) &&
              depthxr::IsHalfFloatRgbaFormat(0x881A) && !depthxr::IsHalfFloatRgbaFormat(29),
          "RGBA16F in DXGI, Vulkan and GL only");
}

void TestWithoutSpaceWarp() {
    // No structure, or one of an extension not stripped: the frame itself.
    {
        XrCompositionLayerDepthInfoKHR depth = DepthInfo();
        Frame frame(&depth, nullptr);
        SpaceWarpFrame storage;
        Check(depthxr::WithoutSpaceWarp(&frame.info, true, true, storage) == &frame.info && storage.found.empty(),
              "a frame without the structures is forwarded as it is");
        XrFrameSynthesisInfoEXT ext = FrameSynthesisInfo();
        Frame other(&ext, nullptr);
        Check(depthxr::WithoutSpaceWarp(&other.info, true, false, storage) == &other.info && storage.found.empty(),
              "XR_EXT_frame_synthesis' structure stays while only XR_FB_space_warp is stripped");
    }
    // The structure ahead of depth, and behind it.
    {
        XrCompositionLayerDepthInfoKHR left_depth = DepthInfo();
        XrCompositionLayerSpaceWarpInfoFB left = SpaceWarpInfo(&left_depth);
        XrCompositionLayerSpaceWarpInfoFB right = SpaceWarpInfo();
        XrCompositionLayerDepthInfoKHR right_depth = DepthInfo(&right);
        Frame frame(&left, &right_depth);
        SpaceWarpFrame storage;
        const XrFrameEndInfo* stripped = depthxr::WithoutSpaceWarp(&frame.info, true, false, storage);
        Check(stripped != &frame.info && stripped->layerCount == 2 && stripped->displayTime == frame.info.displayTime,
              "a copy of the frame");
        Check(stripped->layers[0] == frame.layers[0], "other layers are the application's");
        const XrCompositionLayerProjection* projection = Projection(stripped);
        Check(projection != &frame.projection && projection->viewCount == 2, "a copy of the projection layer");
        Check(ChainTypes(projection->views[0].next) ==
                      std::vector<XrStructureType>{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR} &&
                  projection->views[0].next == &left_depth,
              "the structure ahead of depth goes; depth is the application's");
        Check(ChainTypes(projection->views[1].next) ==
                      std::vector<XrStructureType>{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR} &&
                  projection->views[1].next != &right_depth &&
                  static_cast<const XrCompositionLayerDepthInfoKHR*>(projection->views[1].next)->subImage.swapchain ==
                      kDepth,
              "the structure behind depth goes with a copy of depth");
        Check(frame.views[0].next == &left && right_depth.next == &right, "the application's chains are untouched");
        Check(projection->views[1].subImage.imageArrayIndex == 1 && projection->views[1].pose.position.x > 0.0f,
              "the views are the application's otherwise");
        Check(storage.found.size() == 2 && storage.found[0].stripped && storage.found[1].stripped &&
                  storage.found[0].layer_index == 1 && storage.found[1].view_index == 1,
              "both structures are recorded");
        const depthxr::SpaceWarpViewInfo& info = storage.found[0];
        Check(info.motion_vector.swapchain == kMotion && info.motion_vector.imageRect.extent.width == 500 &&
                  info.depth.swapchain == kMotionDepth && info.far_z == 10000.0f &&
                  info.app_space_delta_pose.position.z == -0.25f &&
                  info.layer_flags == XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB,
              "the structure's fields are recorded");
        const std::string described = depthxr::DescribeSpaceWarpView(info);
        Check(described.find("XrCompositionLayerSpaceWarpInfoFB") != std::string::npos &&
                  described.find("mv=0,0+500x600[0]") != std::string::npos &&
                  described.find("farZ=10000") != std::string::npos &&
                  described.find("flags=0x1") != std::string::npos,
              "the description: " + described);
    }
    // Behind a structure the probe cannot copy, the structure stays.
    {
        XrCompositionLayerSpaceWarpInfoFB behind = SpaceWarpInfo();
        XrBaseInStructure unknown{kUnknownType, reinterpret_cast<const XrBaseInStructure*>(&behind)};
        XrFrameSynthesisInfoEXT ext = FrameSynthesisInfo();
        Frame frame(&unknown, &ext);
        SpaceWarpFrame storage;
        const XrFrameEndInfo* stripped = depthxr::WithoutSpaceWarp(&frame.info, true, true, storage);
        const XrCompositionLayerProjection* projection = Projection(stripped);
        Check(projection->views[0].next == &unknown, "a chain behind an unknown structure is the application's");
        Check(projection->views[1].next == nullptr, "XR_EXT_frame_synthesis' structure goes");
        Check(storage.found.size() == 2 && !storage.found[0].stripped && storage.found[1].stripped &&
                  storage.found[1].type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT &&
                  storage.found[1].motion_vector_scale.x == 1.0f,
              "the structure left behind is recorded as left");
    }
}

// Frame Generation's record of a frame's structures (engine_motion.h).
void TestEngineMotionRecord() {
    using depthxr::ToEngineMotionFrame;
    const auto swapchains = [](XrSwapchain swapchain) -> std::optional<depthxr::EngineMotionSwapchain> {
        if (swapchain == kMotion) return depthxr::EngineMotionSwapchain{500, 600, 2, 10};
        return std::nullopt;
    };
    // Both views carry XR_FB_space_warp's structure; the projection is the
    // frame's second layer (behind a quad), and only its views were copied.
    {
        XrCompositionLayerDepthInfoKHR left_depth = DepthInfo();
        XrCompositionLayerSpaceWarpInfoFB left = SpaceWarpInfo(&left_depth);
        // The left structure's depth is the view's own depth (MSFS 2024).
        left.depthSubImage = left_depth.subImage;
        XrCompositionLayerSpaceWarpInfoFB right = SpaceWarpInfo();
        right.motionVectorSubImage.imageArrayIndex = 1;
        Frame frame(&left, &right);
        frame.views[0].pose.orientation = {0.0f, 0.0998f, 0.0f, 0.995f};
        frame.views[1].pose.position = {0.031f, 1.25f, -0.5f};
        frame.views[1].fov = {-0.9f, 0.7f, 0.75f, -0.85f};
        SpaceWarpFrame storage;
        const XrFrameEndInfo* stripped = depthxr::WithoutSpaceWarp(&frame.info, true, false, storage);
        const auto record = ToEngineMotionFrame(storage, *stripped, 7, swapchains);
        Check(record && record->primary_layer == 1 && record->projection_frame == 7 &&
                  record->display_time == frame.info.displayTime && record->views.size() == 2 && record->complete &&
                  record->invalid == 0,
              "the record names the primary projection's two structures");
        const depthxr::FrameGenEngineMotionView& view0 = record->views[0];
        const depthxr::FrameGenEngineMotionView& view1 = record->views[1];
        Check(view0.view == 0 && view1.view == 1 && view0.source == depthxr::FrameGenMotionSource::kSpaceWarpFb &&
                  view0.vectors.swapchain == kMotion && view1.vectors.imageArrayIndex == 1 && view0.vectors_format == 10 &&
                  !view0.encoding.ext && view0.frame_skip && view0.color.swapchain == kColor &&
                  view1.color.imageArrayIndex == 1,
              "each view's structure is recorded");
        Check(view0.depth_is_khr && !view1.depth_is_khr && view0.depth_encoding.far_z == 10000.0f &&
                  view0.app_delta.position.z == -0.25f,
              "the structure's depth is the view's own only where it names the same image");
        Check(view0.game.orientation[1] == 0.0998f && view1.game.position[1] == 1.25f && view1.game.angle_left == -0.9f &&
                  view1.game.angle_up == 0.75f && view0.game.position[0] == -0.032f,
              "the game's own poses and fields of view are read by layer index from the forwarded frame");
    }
    // XR_EXT_frame_synthesis' decoding, and structures with problems.
    {
        XrFrameSynthesisInfoEXT left = FrameSynthesisInfo();
        left.layerFlags = XR_FRAME_SYNTHESIS_INFO_USE_2D_MOTION_VECTOR_BIT_EXT |
                          XR_FRAME_SYNTHESIS_INFO_REQUEST_RELAXED_FRAME_INTERVAL_BIT_EXT;
        left.motionVectorScale = {2.0f, -2.0f, 1.0f, 0.0f};
        left.motionVectorOffset = {-1.0f, 1.0f, 0.0f, 0.0f};
        left.nearZ = left.farZ = 1000.0f; // nearZ == farZ (MSFS 2024's menus?)
        XrFrameSynthesisInfoEXT right = FrameSynthesisInfo();
        right.motionVectorSubImage.imageRect = {{100, 0}, {500, 600}}; // past the swapchain's 500 texels
        Frame frame(&left, &right);
        SpaceWarpFrame storage;
        const auto record = ToEngineMotionFrame(storage, *depthxr::WithoutSpaceWarp(&frame.info, false, true, storage), 8,
                                                swapchains);
        Check(record && record->views.size() == 2 && record->invalid == 2 && record->complete,
              "structures with problems are counted, and still recorded");
        const depthxr::FrameGenEngineMotionView& view0 = record->views[0];
        Check(view0.source == depthxr::FrameGenMotionSource::kFrameSynthesisExt && view0.encoding.ext &&
                  view0.encoding.two_d && view0.encoding.scale[1] == -2.0f && view0.encoding.offset[0] == -1.0f &&
                  view0.encoding.scale[2] == 1.0f && view0.problems == depthxr::kEngineMotionNearFar &&
                  record->views[1].problems == depthxr::kEngineMotionRect,
              "XR_EXT_frame_synthesis' decoding and the problems found");
        Check(view0.relaxed_interval && !view0.frame_skip && !record->views[1].relaxed_interval &&
                  !record->views[1].encoding.two_d,
              "XR_EXT_frame_synthesis' flags: REQUEST_RELAXED_FRAME_INTERVAL and USE_2D_MOTION_VECTOR");
        const auto decoded = depthxr::DecodeFrameGenMotion(view0.encoding, {0.5f, 0.25f, 9.0f, 9.0f});
        Check(decoded[0] == 0.0f && decoded[1] == 0.5f, "an EXT vector is rg * scale + offset");
        // As the shaders take it: in uv, y down (n = (1, -1) * e, Vulkan-style
        // NDC as XR_EXT_frame_synthesis' vectors are, halved), A * raw + B.
        depthxr::FrameGenEngineConvention y_down;
        y_down.gain = {1.0f, -1.0f};
        const std::array<float, 4> packed = depthxr::FrameGenEngineDecode(y_down, view0.encoding);
        Check(packed[0] * 0.5f + packed[2] == 0.5f * decoded[0] && packed[1] * 0.25f + packed[3] == 0.5f * decoded[1],
              "an EXT vector in Vulkan-style NDC is half itself in uv, y down");
        std::string why;
        Check(!depthxr::ValidateEngineMotionView(storage.found[0], why) && why.find("nearZ equals farZ") != std::string::npos,
              "the problem is told: " + why);
    }
    // No structure the layer took on the primary projection: no record.
    {
        XrFrameSynthesisInfoEXT ext = FrameSynthesisInfo();
        Frame frame(&ext, nullptr);
        SpaceWarpFrame storage;
        Check(!ToEngineMotionFrame(storage, *depthxr::WithoutSpaceWarp(&frame.info, true, false, storage), 9),
              "a structure the layer did not take makes no record");
        XrBaseInStructure unknown{kUnknownType, nullptr};
        XrCompositionLayerSpaceWarpInfoFB behind = SpaceWarpInfo();
        unknown.next = reinterpret_cast<const XrBaseInStructure*>(&behind);
        Frame left_behind(&unknown, nullptr);
        Check(!ToEngineMotionFrame(storage, *depthxr::WithoutSpaceWarp(&left_behind.info, true, false, storage), 9),
              "a structure left behind an unknown one makes no record");
    }
}

// --- The stand-in runtime ----------------------------------------------------

struct StandIn {
    std::vector<std::string> extensions;              // what it lists before the instance
    std::vector<std::string> instance_extensions;     // what its instance was created with
    std::vector<std::vector<XrStructureType>> ended;  // each submitted projection view's chain, last frame
    std::uint32_t end_frames{0};
    XrInstance instance{XR_NULL_HANDLE};
    std::uintptr_t handles{0x1000};
};
StandIn g_runtime;

template <typename F>
PFN_xrVoidFunction Function(F function) {
    return reinterpret_cast<PFN_xrVoidFunction>(function);
}

// Everything the layer resolves that the probe does not need. The x64
// calling convention lets a function that takes nothing stand in for one
// that takes arguments: the caller owns them.
XrResult XRAPI_CALL RtSucceed() {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateInstanceExtensions(const char*, std::uint32_t capacity, std::uint32_t* count,
                                                   XrExtensionProperties* properties) {
    *count = static_cast<std::uint32_t>(g_runtime.extensions.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < g_runtime.extensions.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    for (std::size_t i = 0; i < g_runtime.extensions.size(); ++i) {
        std::strncpy(properties[i].extensionName, g_runtime.extensions[i].c_str(), XR_MAX_EXTENSION_NAME_SIZE - 1);
        properties[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateApiLayerInstance(const XrInstanceCreateInfo* info, const depthxr::XrApiLayerCreateInfo*,
                                             XrInstance* instance) {
    g_runtime.instance_extensions.clear();
    for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const std::string name = info->enabledExtensionNames[i];
        // A runtime refuses an extension it does not have.
        if (std::find(g_runtime.extensions.begin(), g_runtime.extensions.end(), name) == g_runtime.extensions.end()) {
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        g_runtime.instance_extensions.push_back(name);
    }
    *instance = g_runtime.instance = Handle<XrInstance>(++g_runtime.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
    std::strncpy(properties->runtimeName, "Space-warp stand-in", XR_MAX_RUNTIME_NAME_SIZE - 1);
    properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
    std::strncpy(properties->systemName, "Stand-in headset", XR_MAX_SYSTEM_NAME_SIZE - 1);
    properties->graphicsProperties.maxSwapchainImageWidth = 8192;
    properties->graphicsProperties.maxSwapchainImageHeight = 8192;
    properties->graphicsProperties.maxLayerCount = 16;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
                                                       std::uint32_t capacity, std::uint32_t* count,
                                                       XrViewConfigurationView* views) {
    *count = 2;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (std::uint32_t i = 0; i < 2; ++i) {
        views[i].recommendedImageRectWidth = kEyeWidth;
        views[i].recommendedImageRectHeight = kEyeHeight;
        views[i].maxImageRectWidth = 8192;
        views[i].maxImageRectHeight = 8192;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateSwapchainFormats(XrSession, std::uint32_t capacity, std::uint32_t* count,
                                                std::int64_t* formats) {
    constexpr std::int64_t kFormats[] = {29, 28, 10, 40, 20};
    *count = 5;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 5) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(std::begin(kFormats), std::end(kFormats), formats);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetDisplayRefreshRate(XrSession, float* rate) {
    *rate = 90.0f;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* swapchain) {
    *swapchain = Handle<XrSwapchain>(++g_runtime.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEndFrame(XrSession, const XrFrameEndInfo* info) {
    ++g_runtime.end_frames;
    g_runtime.ended.clear();
    for (std::uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        for (std::uint32_t v = 0; v < projection->viewCount; ++v) {
            g_runtime.ended.push_back(ChainTypes(projection->views[v].next));
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    *function = nullptr;
    const std::string_view requested(name);
    if (instance == XR_NULL_HANDLE) {
        // Before the instance: the layer's extension scan.
        if (requested == "xrEnumerateInstanceExtensionProperties") {
            *function = Function(RtEnumerateInstanceExtensions);
            return XR_SUCCESS;
        }
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (requested == "xrGetInstanceProperties") {
        *function = Function(RtGetInstanceProperties);
    } else if (requested == "xrGetSystemProperties") {
        *function = Function(RtGetSystemProperties);
    } else if (requested == "xrEnumerateViewConfigurationViews") {
        *function = Function(RtEnumerateViewConfigurationViews);
    } else if (requested == "xrCreateSwapchain") {
        *function = Function(RtCreateSwapchain);
    } else if (requested == "xrEndFrame") {
        *function = Function(RtEndFrame);
    } else if (requested == "xrEnumerateSwapchainFormats") {
        *function = Function(RtEnumerateSwapchainFormats);
    } else if (requested == "xrGetDisplayRefreshRateFB") {
        *function = Function(RtGetDisplayRefreshRate);
    } else if (requested.starts_with("xr") && !requested.ends_with("KHR") && !requested.ends_with("EXT") &&
               !requested.ends_with("VARJO") && !requested.ends_with("FB")) {
        *function = Function(RtSucceed);
    } else {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

// --- The application ---------------------------------------------------------

void SetProbe(const char* value) {
    _putenv_s("VECTORXR_SPACEWARP_PROBE", value ? value : "");
}

XrResult CreateInstance(const std::vector<const char*>& extensions, XrInstance& instance) {
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, "VectorXR space-warp probe test",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.enabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    depthxr::XrApiLayerNextInfo next{};
    next.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    next.structVersion = depthxr::XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    next.structSize = sizeof(next);
    std::strncpy(next.layerName, "XR_APILAYER_TEST_stand_in_runtime", XR_MAX_API_LAYER_NAME_SIZE - 1);
    next.nextGetInstanceProcAddr = RtGetInstanceProcAddr;
    next.nextCreateApiLayerInstance = RtCreateApiLayerInstance;
    depthxr::XrApiLayerCreateInfo create{};
    create.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
    create.structVersion = depthxr::XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
    create.structSize = sizeof(create);
    create.nextInfo = &next;
    instance = XR_NULL_HANDLE;
    return xrCreateApiLayerInstance(&info, &create, &instance);
}

template <typename T>
T LayerFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    Check(XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, &function)) && function,
          std::string("the layer hands out ") + name);
    return reinterpret_cast<T>(function);
}

bool Has(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// What an application sees and the runtime gets in one instance and a frame.
struct Outcome {
    XrResult create{XR_ERROR_RUNTIME_FAILURE};
    std::vector<std::string> runtime_instance;
    XrSystemSpaceWarpPropertiesFB fb{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
    XrFrameSynthesisConfigViewEXT ext[2]{{XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT},
                                         {XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT}};
    std::vector<std::vector<XrStructureType>> ended;
    std::string log;
};

Outcome Run(const char* probe, std::vector<std::string> runtime, std::vector<const char*> application) {
    SetProbe(probe);
    g_runtime = {};
    g_runtime.extensions = std::move(runtime);
    Outcome outcome;
    XrInstance instance = XR_NULL_HANDLE;
    outcome.create = CreateInstance(application, instance);
    if (XR_FAILED(outcome.create)) {
        return outcome;
    }
    outcome.runtime_instance = g_runtime.instance_extensions;

    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    properties.next = &outcome.fb;
    Check(XR_SUCCEEDED(LayerFunction<PFN_xrGetSystemProperties>(instance, "xrGetSystemProperties")(
              instance, kSystem, &properties)),
          "xrGetSystemProperties");

    XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    views[0].next = &outcome.ext[0];
    views[1].next = &outcome.ext[1];
    std::uint32_t count = 0;
    Check(XR_SUCCEEDED(LayerFunction<PFN_xrEnumerateViewConfigurationViews>(
              instance, "xrEnumerateViewConfigurationViews")(
              instance, kSystem, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views)) &&
              count == 2 && views[0].recommendedImageRectWidth == kEyeWidth,
          "xrEnumerateViewConfigurationViews");

    SpaceWarpProbeTestPeer::SetActiveSession(kSession);
    // The motion vector swapchain, as an application makes it: as big as the
    // layer recommended (500x600 where it recommended nothing).
    XrExtent2Di answered{500, 600};
    if (outcome.fb.recommendedMotionVectorImageRectWidth) {
        answered = {static_cast<std::int32_t>(outcome.fb.recommendedMotionVectorImageRectWidth),
                    static_cast<std::int32_t>(outcome.fb.recommendedMotionVectorImageRectHeight)};
    } else if (outcome.ext[0].recommendedMotionVectorImageRectWidth) {
        answered = {static_cast<std::int32_t>(outcome.ext[0].recommendedMotionVectorImageRectWidth),
                    static_cast<std::int32_t>(outcome.ext[0].recommendedMotionVectorImageRectHeight)};
    }
    XrSwapchainCreateInfo motion{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    motion.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    motion.format = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
    motion.sampleCount = 1;
    motion.width = static_cast<std::uint32_t>(answered.width);
    motion.height = static_cast<std::uint32_t>(answered.height);
    motion.faceCount = 1;
    motion.arraySize = 2;
    motion.mipCount = 1;
    XrSwapchain motion_swapchain = XR_NULL_HANDLE;
    Check(XR_SUCCEEDED(LayerFunction<PFN_xrCreateSwapchain>(instance, "xrCreateSwapchain")(kSession, &motion,
                                                                                            &motion_swapchain)),
          "xrCreateSwapchain");

    XrCompositionLayerDepthInfoKHR left_depth = DepthInfo();
    XrCompositionLayerSpaceWarpInfoFB left = SpaceWarpInfo(&left_depth);
    left.motionVectorSubImage.swapchain = motion_swapchain;
    left.motionVectorSubImage.imageRect = {{0, 0}, answered};
    XrCompositionLayerDepthInfoKHR right_depth = DepthInfo();
    XrFrameSynthesisInfoEXT right = FrameSynthesisInfo(&right_depth);
    right.motionVectorSubImage.swapchain = motion_swapchain;
    right.motionVectorSubImage.imageRect = {{0, 0}, answered};
    right.layerFlags = XR_FRAME_SYNTHESIS_INFO_REQUEST_RELAXED_FRAME_INTERVAL_BIT_EXT;
    Frame frame(&left, &right);
    Check(XR_SUCCEEDED(LayerFunction<PFN_xrEndFrame>(instance, "xrEndFrame")(kSession, &frame.info)) &&
              g_runtime.end_frames == 1,
          "xrEndFrame reaches the runtime");
    Check(frame.views[0].next == &left && frame.views[1].next == &right, "the application's frame is untouched");
    outcome.ended = g_runtime.ended;

    // The session ends with the instance.
    Check(XR_SUCCEEDED(LayerFunction<PFN_xrDestroyInstance>(instance, "xrDestroyInstance")(instance)),
          "xrDestroyInstance");
    outcome.log = SpaceWarpProbeTestPeer::Log();
    return outcome;
}

const std::vector<XrStructureType> kDepthOnly{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};

// Frame Generation gets each frame's record from the layer's xrEndFrame:
// the game's poses as submitted, structures with problems counted, and a
// record without views for a frame without structures; the runtime never
// sees one.
void TestEngineRecordsThroughTheLayer() {
    const char* fb = XR_FB_SPACE_WARP_EXTENSION_NAME;
    const char* depth = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
    SetProbe("fb");
    g_runtime = {};
    g_runtime.extensions = {depth};
    XrInstance instance = XR_NULL_HANDLE;
    Check(CreateInstance({depth, fb}, instance) == XR_SUCCESS, "the instance exists");
    SpaceWarpProbeTestPeer::SetActiveSession(kSession);
    XrSwapchainCreateInfo motion{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    motion.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    motion.format = 10;
    motion.sampleCount = 1;
    motion.width = 500;
    motion.height = 600;
    motion.faceCount = 1;
    motion.arraySize = 2;
    motion.mipCount = 1;
    XrSwapchain motion_swapchain = XR_NULL_HANDLE;
    Check(XR_SUCCEEDED(LayerFunction<PFN_xrCreateSwapchain>(instance, "xrCreateSwapchain")(kSession, &motion,
                                                                                            &motion_swapchain)),
          "xrCreateSwapchain");
    const auto end_frame = LayerFunction<PFN_xrEndFrame>(instance, "xrEndFrame");
    const SpaceWarpProbeTestPeer::EngineRecords before = SpaceWarpProbeTestPeer::Engine();

    XrCompositionLayerDepthInfoKHR left_depth = DepthInfo();
    XrCompositionLayerSpaceWarpInfoFB left = SpaceWarpInfo(&left_depth);
    left.motionVectorSubImage.swapchain = motion_swapchain;
    left.nearZ = left.farZ = 1000.0f; // a structure with a problem
    XrCompositionLayerDepthInfoKHR right_depth = DepthInfo();
    XrCompositionLayerSpaceWarpInfoFB right = SpaceWarpInfo(&right_depth);
    right.motionVectorSubImage.swapchain = motion_swapchain;
    right.motionVectorSubImage.imageArrayIndex = 1;
    {
        Frame frame(&left, &right);
        frame.views[1].pose.orientation = {0.0f, 0.0499792f, 0.0f, 0.99875f};
        frame.views[1].pose.position = {0.032f, 1.6f, 0.1f};
        Check(XR_SUCCEEDED(end_frame(kSession, &frame.info)), "xrEndFrame with the structures");
        Check(g_runtime.ended.size() == 2 && g_runtime.ended[0] == kDepthOnly && g_runtime.ended[1] == kDepthOnly,
              "the runtime never sees the structures");
        const SpaceWarpProbeTestPeer::EngineRecords records = SpaceWarpProbeTestPeer::Engine();
        Check(records.noted == before.noted + 1 && records.frame.views.size() == 2 && records.frame.complete &&
                  records.frame.display_time == frame.info.displayTime && records.frame.primary_layer == 1,
              "Frame Generation gets the frame's record");
        Check(records.frame.views[1].game.orientation[1] == 0.0499792f && records.frame.views[1].game.position[1] == 1.6f &&
                  records.frame.views[0].game.position[0] == -0.032f &&
                  records.frame.views[1].vectors.swapchain == motion_swapchain,
              "the record has the game's own poses");
        Check(records.frame.invalid == 1 && records.invalid == before.invalid + 1 &&
                  records.frame.views[0].problems == depthxr::kEngineMotionNearFar,
              "structures with problems are counted");
    }
    const std::uint64_t carried = SpaceWarpProbeTestPeer::Engine().frame.projection_frame;
    {
        XrCompositionLayerDepthInfoKHR plain_left = DepthInfo();
        XrCompositionLayerDepthInfoKHR plain_right = DepthInfo();
        Frame frame(&plain_left, &plain_right);
        frame.info.displayTime += 11'111'111;
        Check(XR_SUCCEEDED(end_frame(kSession, &frame.info)), "xrEndFrame without the structures");
        const SpaceWarpProbeTestPeer::EngineRecords records = SpaceWarpProbeTestPeer::Engine();
        Check(records.noted == before.noted + 2 && records.frame.views.empty() &&
                  records.frame.display_time == frame.info.displayTime && records.frame.projection_frame == carried + 1 &&
                  records.previous.views.size() == 2,
              "a frame without structures hands over an empty record, the frame before's kept as its pair");
    }
    {
        // The quad alone: a frame without a projection has no number (never
        // the last projection frame's).
        XrCompositionLayerDepthInfoKHR plain_left = DepthInfo();
        XrCompositionLayerDepthInfoKHR plain_right = DepthInfo();
        Frame frame(&plain_left, &plain_right);
        frame.info.layerCount = 1;
        frame.info.displayTime += 22'222'222;
        Check(XR_SUCCEEDED(end_frame(kSession, &frame.info)), "xrEndFrame without a projection");
        const SpaceWarpProbeTestPeer::EngineRecords records = SpaceWarpProbeTestPeer::Engine();
        Check(records.noted == before.noted + 3 && records.frame.views.empty() && records.frame.projection_frame == 0,
              "a frame without a projection hands over a record without a number");
    }
    Check(XR_SUCCEEDED(LayerFunction<PFN_xrDestroyInstance>(instance, "xrDestroyInstance")(instance)),
          "xrDestroyInstance");
    const std::string log = SpaceWarpProbeTestPeer::Log();
    Check(log.find("structuresWithProblems=1") != std::string::npos, "the probe's summary counts the structure's problem");
}

void TestThroughTheLayer() {
    const char* fb = XR_FB_SPACE_WARP_EXTENSION_NAME;
    const char* ext = XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME;
    const char* depth = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;

    // The probe on (fb): XR_FB_space_warp is the layer's, on a runtime without it.
    {
        const Outcome outcome = Run("fb", {depth}, {depth, fb});
        Check(outcome.create == XR_SUCCESS, "the instance exists: " + std::to_string(outcome.create));
        Check(!Has(outcome.runtime_instance, fb) && Has(outcome.runtime_instance, depth),
              "the runtime's instance lacks XR_FB_space_warp, with the application's other extensions");
        Check(outcome.fb.recommendedMotionVectorImageRectWidth == 250 &&
                  outcome.fb.recommendedMotionVectorImageRectHeight == 300,
              "XrSystemSpaceWarpPropertiesFB recommends an eighth of the eye (Frame Generation reads none): " +
                  std::to_string(outcome.fb.recommendedMotionVectorImageRectWidth) + "x" +
                  std::to_string(outcome.fb.recommendedMotionVectorImageRectHeight));
        Check(outcome.ext[0].recommendedMotionVectorImageRectWidth == 0,
              "XR_EXT_frame_synthesis' configuration is not answered for an instance without it");
        Check(outcome.ended.size() == 2 && outcome.ended[0] == kDepthOnly,
              "the runtime gets the left view's depth without the space-warp structure");
        Check(outcome.ended[1] ==
                  std::vector<XrStructureType>{XR_TYPE_FRAME_SYNTHESIS_INFO_EXT,
                                               XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
              "a structure of an extension the layer does not own goes on");
        Check(outcome.log.find("takenOver=XR_FB_space_warp") != std::string::npos,
              "the instance's probe line says what the layer took over");
        Check(outcome.log.find("XrSystemSpaceWarpPropertiesFB answered: recommendedMotionVectorImageRect=250x300 (an "
                               "eighth of the eye each way)") != std::string::npos,
              "the answer is logged");
        Check(outcome.log.find("swapchain that looks like motion vectors") != std::string::npos &&
                  outcome.log.find("rgba16f=1, motionVectorSize=1") != std::string::npos,
              "the motion vector swapchain is logged");
        Check(outcome.log.find("the session's first frame with motion vectors") != std::string::npos &&
                  outcome.log.find("XrCompositionLayerSpaceWarpInfoFB layer=1 view=0 stripped=1") !=
                      std::string::npos &&
                  outcome.log.find("(250x300 format=10") != std::string::npos,
              "the first frame's structure is logged with its swapchain");
        Check(outcome.log.find("relaxedIntervalFlags=0") != std::string::npos,
              "a structure of an extension the layer does not own is not counted");
        Check(outcome.log.find("Space-warp probe summary (session end): frames=1, framesWithFbSpaceWarp=1") !=
                  std::string::npos,
              "the session's summary at its end");
    }

    // The probe on (ext): XR_EXT_frame_synthesis, answered for each view.
    {
        const Outcome outcome = Run("ext", {depth}, {ext, depth});
        Check(outcome.create == XR_SUCCESS && !Has(outcome.runtime_instance, ext),
              "the runtime's instance lacks XR_EXT_frame_synthesis");
        Check(outcome.ext[0].recommendedMotionVectorImageRectWidth == 250 &&
                  outcome.ext[1].recommendedMotionVectorImageRectHeight == 300,
              "XrFrameSynthesisConfigViewEXT recommends an eighth of each view");
        Check(outcome.fb.recommendedMotionVectorImageRectWidth == 0, "XR_FB_space_warp's properties are not answered");
        Check(outcome.ended.size() == 2 && outcome.ended[1] == kDepthOnly &&
                  outcome.ended[0] ==
                      std::vector<XrStructureType>{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB,
                                                   XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
              "the runtime gets the right view without XR_EXT_frame_synthesis' structure");
        Check(outcome.log.find("XrFrameSynthesisConfigViewEXT answered: view0=250x300 view1=250x300 (an eighth of the "
                               "eye each way)") != std::string::npos,
              "the answer is logged");
        Check(outcome.log.find("relaxedIntervalFlags=1") != std::string::npos,
              "REQUEST_RELAXED_FRAME_INTERVAL is counted in the probe's summary");
    }

    // The size answered: half the eye where Frame Generation will read the
    // vectors (enabled for the game, VECTORXR_FRAMEGEN_ENGINE_MV from
    // capture, on Direct3D 12: a Direct3D 12 game's, or a Vulkan game's
    // through the Vulkan bridge), else an eighth;
    // VECTORXR_FRAMEGEN_ENGINE_MV_SIZE wins.
    {
        const char* d3d12 = "XR_KHR_D3D12_enable";
        const char* vulkan = "XR_KHR_vulkan_enable2";
        const char* opengl = "XR_KHR_opengl_enable";
        const std::filesystem::path settings =
            std::filesystem::temp_directory_path() /
            ("vectorxr-spacewarp-framegen-" + std::to_string(GetCurrentProcessId()) + ".json");
        std::ofstream(settings, std::ios::trunc)
            << R"({"version": 3, "core": {"enabled": true, "logLevel": "info", "trackSeenApps": false}, )"
            << R"("applications": [], "modules": {"depthxr": {"enabled": false}, "pivotxr": {"enabled": false}, )"
            << R"("quadviews": {"enabled": false}, "turbo": {"enabled": false}, "framegen": {"enabled": true}}})"
            << "\n";
        _putenv_s("VECTORXR_CONFIG_PATH", settings.string().c_str());
        _putenv_s("VECTORXR_FRAMEGEN_ENGINE_MV", "census");
        const Outcome census = Run("both", {depth, d3d12}, {depth, fb, ext, d3d12});
        _putenv_s("VECTORXR_FRAMEGEN_ENGINE_MV", "capture");
        const Outcome read = Run("both", {depth, d3d12}, {depth, fb, ext, d3d12});
        const Outcome bridged = Run("both", {depth, d3d12, vulkan}, {depth, fb, ext, vulkan});
        const Outcome gl = Run("both", {depth, opengl}, {depth, fb, ext, opengl});
        _putenv_s("VECTORXR_VULKAN_BRIDGE", "off");
        const Outcome unbridged = Run("both", {depth, d3d12, vulkan}, {depth, fb, ext, vulkan});
        _putenv_s("VECTORXR_VULKAN_BRIDGE", "");
        _putenv_s("VECTORXR_FRAMEGEN_ENGINE_BRIDGE", "0");
        const Outcome bridge_off = Run("both", {depth, d3d12, vulkan}, {depth, fb, ext, vulkan});
        _putenv_s("VECTORXR_FRAMEGEN_ENGINE_BRIDGE", "");
        _putenv_s("VECTORXR_FRAMEGEN_ENGINE_MV_SIZE", "quarter");
        const Outcome research = Run("both", {depth}, {depth, fb, ext});
        _putenv_s("VECTORXR_FRAMEGEN_ENGINE_MV_SIZE", "");
        _putenv_s("VECTORXR_FRAMEGEN_ENGINE_MV", "");
        _putenv_s("VECTORXR_CONFIG_PATH", "");
        std::error_code ignored;
        std::filesystem::remove(settings, ignored);
        Check(census.fb.recommendedMotionVectorImageRectWidth == 250 &&
                  census.ext[1].recommendedMotionVectorImageRectHeight == 300,
              "an eighth of the eye while Frame Generation only takes a census");
        Check(read.fb.recommendedMotionVectorImageRectWidth == 1000 &&
                  read.fb.recommendedMotionVectorImageRectHeight == 1200 &&
                  read.ext[0].recommendedMotionVectorImageRectWidth == 1000 &&
                  read.ext[1].recommendedMotionVectorImageRectHeight == 1200 &&
                  read.log.find("recommendedMotionVectorImageRect=1000x1200 (half the eye each way)") != std::string::npos,
              "half the eye where Frame Generation reads a Direct3D 12 game's vectors: " +
                  std::to_string(read.fb.recommendedMotionVectorImageRectWidth));
        Check(bridged.create == XR_SUCCESS && Has(bridged.runtime_instance, d3d12) &&
                  bridged.fb.recommendedMotionVectorImageRectWidth == 1000 &&
                  bridged.ext[1].recommendedMotionVectorImageRectHeight == 1200,
              "half the eye for a Vulkan game through the Vulkan bridge: " +
                  std::to_string(bridged.fb.recommendedMotionVectorImageRectWidth));
        Check(unbridged.create == XR_SUCCESS && !Has(unbridged.runtime_instance, d3d12) &&
                  unbridged.fb.recommendedMotionVectorImageRectWidth == 250 &&
                  unbridged.ext[1].recommendedMotionVectorImageRectHeight == 300,
              "an eighth for a Vulkan game without the Vulkan bridge (Frame Generation's Vulkan backend reads none): " +
                  std::to_string(unbridged.fb.recommendedMotionVectorImageRectWidth));
        Check(bridge_off.create == XR_SUCCESS && bridge_off.fb.recommendedMotionVectorImageRectWidth == 250,
              "an eighth for a Vulkan game with VECTORXR_FRAMEGEN_ENGINE_BRIDGE=0");
        Check(gl.create == XR_SUCCESS && gl.fb.recommendedMotionVectorImageRectWidth == 250 &&
                  gl.ext[0].recommendedMotionVectorImageRectWidth == 250,
              "an eighth for an OpenGL game: " + std::to_string(gl.fb.recommendedMotionVectorImageRectWidth));
        Check(research.fb.recommendedMotionVectorImageRectWidth == 500 &&
                  research.ext[0].recommendedMotionVectorImageRectHeight == 600,
              "VECTORXR_FRAMEGEN_ENGINE_MV_SIZE=quarter: a quarter of the eye");
    }

    // A runtime that lists the extension gets it, and its structures.
    {
        const Outcome outcome = Run("both", {depth, fb}, {depth, fb});
        Check(outcome.create == XR_SUCCESS && Has(outcome.runtime_instance, fb),
              "the runtime's own XR_FB_space_warp is forwarded");
        Check(outcome.fb.recommendedMotionVectorImageRectWidth == 0, "the runtime answers for its own extension");
        Check(outcome.ended.size() == 2 &&
                  outcome.ended[0] ==
                      std::vector<XrStructureType>{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB,
                                                   XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
              "the runtime gets the structures");
        Check(outcome.log.find("runtimeListsFbSpaceWarp=1") != std::string::npos &&
                  outcome.log.find("takenOver=none") != std::string::npos,
              "the probe line says the runtime has it");
    }

    // Set to anything but fb, ext or both: a probe manifest loads the layer
    // for any value and advertises its extensions, so what the application
    // enabled and the runtime lacks is taken over all the same, with a
    // warning (the runtime would refuse it, and the instance fail).
    {
        const Outcome one = Run("1", {depth}, {depth, fb});
        Check(one.create == XR_SUCCESS && !Has(one.runtime_instance, fb) && Has(one.runtime_instance, depth),
              "VECTORXR_SPACEWARP_PROBE=1: the instance exists, its XR_FB_space_warp the layer's: " +
                  std::to_string(one.create));
        Check(one.fb.recommendedMotionVectorImageRectWidth == 250 && one.ended.size() == 2 && one.ended[0] == kDepthOnly,
              "VECTORXR_SPACEWARP_PROBE=1: answered and taken off the frame as with fb");
        Check(one.log.find("VECTORXR_SPACEWARP_PROBE='1'") != std::string::npos &&
                  one.log.find("takenOver=XR_FB_space_warp") != std::string::npos &&
                  one.log.find("WARNING: VECTORXR_SPACEWARP_PROBE='1' is not fb, ext or both") != std::string::npos &&
                  one.log.find("Space-warp probe: off") == std::string::npos,
              "VECTORXR_SPACEWARP_PROBE=1: logged as set, with a warning");
        const Outcome spaced = Run("fb ", {depth}, {depth, fb, ext});
        Check(spaced.create == XR_SUCCESS && !Has(spaced.runtime_instance, fb) && !Has(spaced.runtime_instance, ext) &&
                  spaced.ended.size() == 2 && spaced.ended[0] == kDepthOnly && spaced.ended[1] == kDepthOnly &&
                  spaced.log.find("is not fb, ext or both") != std::string::npos,
              "VECTORXR_SPACEWARP_PROBE='fb ': both extensions the application enabled are the layer's");
        const Outcome listed = Run("0", {depth, fb}, {depth, fb});
        Check(listed.create == XR_SUCCESS && Has(listed.runtime_instance, fb) &&
                  listed.log.find("takenOver=none") != std::string::npos,
              "VECTORXR_SPACEWARP_PROBE=0: what the runtime lists goes to it");
    }

    // The probe off: nothing changes (a runtime without the extension refuses it).
    {
        const Outcome refused = Run(nullptr, {depth}, {depth, fb});
        Check(refused.create == XR_ERROR_EXTENSION_NOT_PRESENT,
              "with the probe off the application's XR_FB_space_warp goes to the runtime as it is");
        const Outcome forwarded = Run(nullptr, {depth, fb}, {depth, fb});
        Check(forwarded.create == XR_SUCCESS && Has(forwarded.runtime_instance, fb) &&
                  forwarded.ended[0].front() == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB &&
                  forwarded.fb.recommendedMotionVectorImageRectWidth == 0,
              "with the probe off a runtime with XR_FB_space_warp gets everything");
        Check(forwarded.log.find("Space-warp probe: off") != std::string::npos, "the log says the probe is off");
        const Outcome plain = Run(nullptr, {depth}, {depth});
        Check(plain.create == XR_SUCCESS && plain.log.find("Space-warp probe") == std::string::npos,
              "an application without the extensions sees no probe at all");
    }
}

std::size_t Occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// The handshake census (handshake_census.h): with VECTORXR_SPACEWARP_PROBE
// set, what the application asks and gets is logged, each distinct item once
// (XrSystemSpaceWarpPropertiesFB's every call); unset, nothing is, and the
// census's hooks are never handed out.
void TestHandshakeCensus() {
    const char* fb = XR_FB_SPACE_WARP_EXTENSION_NAME;
    const char* depth = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;

    // One instance, the application asking as a game might, twice over.
    struct Census {
        XrResult create{XR_ERROR_RUNTIME_FAILURE};
        bool get_system_is_runtimes{false};
        std::string log;
    };
    const auto run = [&](const char* probe) {
        SetProbe(probe);
        g_runtime = {};
        g_runtime.extensions = {depth};
        Census census;
        XrInstance instance = XR_NULL_HANDLE;
        census.create = CreateInstance(probe ? std::vector<const char*>{depth, fb} : std::vector<const char*>{depth},
                                       instance);
        if (XR_FAILED(census.create)) {
            return census;
        }
        const auto get_system = LayerFunction<PFN_xrGetSystem>(instance, "xrGetSystem");
        census.get_system_is_runtimes = reinterpret_cast<PFN_xrVoidFunction>(get_system) == Function(RtSucceed);
        const auto instance_properties = LayerFunction<PFN_xrGetInstanceProperties>(instance, "xrGetInstanceProperties");
        const auto system_properties = LayerFunction<PFN_xrGetSystemProperties>(instance, "xrGetSystemProperties");
        PFN_xrVoidFunction unresolved = nullptr;
        Check(XR_FAILED(xrGetInstanceProcAddr(instance, "xrEnumerateDisplayRefreshRatesFB", &unresolved)),
              "a function the runtime lacks is not handed out");
        const auto refresh_rate = LayerFunction<PFN_xrGetDisplayRefreshRateFB>(instance, "xrGetDisplayRefreshRateFB");
        LayerFunction<PFN_xrGetSystemProperties>(instance, "xrGetSystemProperties"); // looked up again
        for (int twice = 0; twice < 2; ++twice) {
            XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
            Check(XR_SUCCEEDED(instance_properties(instance, &properties)), "xrGetInstanceProperties");
            XrSystemGetInfo get_info{XR_TYPE_SYSTEM_GET_INFO};
            get_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            XrSystemId system = kSystem;
            Check(XR_SUCCEEDED(get_system(instance, &get_info, &system)), "xrGetSystem");
            XrSystemProperties plain{XR_TYPE_SYSTEM_PROPERTIES};
            Check(XR_SUCCEEDED(system_properties(instance, kSystem, &plain)), "xrGetSystemProperties");
            XrSystemSpaceWarpPropertiesFB space_warp{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
            XrSystemProperties chained{XR_TYPE_SYSTEM_PROPERTIES};
            chained.next = &space_warp;
            Check(XR_SUCCEEDED(system_properties(instance, kSystem, &chained)), "xrGetSystemProperties, chained");
            float rate = 0.0f;
            Check(XR_SUCCEEDED(refresh_rate(kSession, &rate)) && rate == 90.0f, "xrGetDisplayRefreshRateFB");
        }

        SpaceWarpProbeTestPeer::SetActiveSession(kSession);
        const auto formats = LayerFunction<PFN_xrEnumerateSwapchainFormats>(instance, "xrEnumerateSwapchainFormats");
        std::uint32_t count = 0;
        std::int64_t offered[5]{};
        Check(XR_SUCCEEDED(formats(kSession, 0, &count, nullptr)) && count == 5 &&
                  XR_SUCCEEDED(formats(kSession, 5, &count, offered)) && XR_SUCCEEDED(formats(kSession, 5, &count, offered)),
              "xrEnumerateSwapchainFormats");
        XrSwapchainCreateInfo motion{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        motion.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        motion.format = 10;
        motion.sampleCount = 1;
        motion.width = 250;
        motion.height = 300;
        motion.faceCount = 1;
        motion.arraySize = 2;
        motion.mipCount = 1;
        XrSwapchain motion_swapchain = XR_NULL_HANDLE;
        Check(XR_SUCCEEDED(LayerFunction<PFN_xrCreateSwapchain>(instance, "xrCreateSwapchain")(kSession, &motion,
                                                                                                &motion_swapchain)),
              "xrCreateSwapchain");
        std::uint32_t index = 0;
        Check(XR_SUCCEEDED(LayerFunction<PFN_xrAcquireSwapchainImage>(instance, "xrAcquireSwapchainImage")(
                  motion_swapchain, nullptr, &index)),
              "xrAcquireSwapchainImage");
        const auto end_frame = LayerFunction<PFN_xrEndFrame>(instance, "xrEndFrame");
        for (int frames = 0; frames < 2; ++frames) {
            XrCompositionLayerDepthInfoKHR left_depth = DepthInfo();
            XrCompositionLayerSpaceWarpInfoFB left = SpaceWarpInfo(&left_depth);
            left.motionVectorSubImage.swapchain = motion_swapchain;
            XrCompositionLayerDepthInfoKHR right_depth = DepthInfo();
            XrCompositionLayerSpaceWarpInfoFB right = SpaceWarpInfo(&right_depth);
            right.motionVectorSubImage.swapchain = motion_swapchain;
            Frame frame(&left, &right);
            Check(XR_SUCCEEDED(end_frame(kSession, &frame.info)), "xrEndFrame");
        }
        Check(XR_SUCCEEDED(LayerFunction<PFN_xrDestroyInstance>(instance, "xrDestroyInstance")(instance)),
              "xrDestroyInstance");
        census.log = SpaceWarpProbeTestPeer::Log();
        return census;
    };

    // Off: no census line, and the runtime's own functions.
    {
        const Census off = run(nullptr);
        Check(off.create == XR_SUCCESS && off.log.find("Handshake census") == std::string::npos,
              "with the probe off nothing is logged");
        Check(off.get_system_is_runtimes, "with the probe off xrGetSystem is the runtime's own");
    }

    // On: each item once.
    const Census on = run("fb");
    const std::string& log = on.log;
    Check(on.create == XR_SUCCESS && !on.get_system_is_runtimes, "with the probe on the census hooks xrGetSystem");
    const auto once = [&log](const std::string& needle, const std::string& what) {
        Check(Occurrences(log, needle) == 1,
              what + ": \"" + needle + "\" " + std::to_string(Occurrences(log, needle)) + " times in\n" + log);
    };
    // 1. xrCreateInstance against the runtime's list, and what the layer took out.
    once("Handshake census: xrCreateInstance: application=\"VectorXR space-warp probe test\"", "the instance");
    once("apiVersion=1.0.0", "the API version");
    once("extensions=(2)[XR_KHR_composition_layer_depth XR_FB_space_warp]", "the extensions requested");
    once("Handshake census: xrCreateInstance extensions: runtimeLists(scan=complete)=(1)[XR_KHR_composition_layer_depth]; "
         "requestedNotInRuntimeList=(1)[XR_FB_space_warp]; strippedByLayer=(1)[XR_FB_space_warp]; addedByLayer=(0)[]",
         "the extensions against the runtime's list");
    once("xrEnumerateInstanceExtensionProperties: unseen", "the list the loader answers is said to be unseen");
    // 2. Every name looked up, once, unresolved ones said so.
    once("xrGetInstanceProcAddr, 5 new name(s) before the application's first call the census watches",
         "the names before the first call");
    once("first lookups): xrGetSystem xrGetInstanceProperties xrGetSystemProperties "
         "xrEnumerateDisplayRefreshRatesFB=unresolved(XR_ERROR_FUNCTION_UNSUPPORTED) xrGetDisplayRefreshRateFB",
         "the names, in order, once each, an unresolved one said so");
    once("xrGetInstanceProcAddr, 10 name(s) in all, 1 looked up more than once: xrGetSystemPropertiesx2",
         "the names looked up more than once are counted");
    // 3. Each distinct xrGetSystemProperties call, the space-warp one every time.
    once("xrGetSystemProperties call 1: systemId=1 result=XR_SUCCESS chain=[] systemName=\"Stand-in headset\"",
         "xrGetSystemProperties without a chain");
    Check(Occurrences(log, "xrGetSystemProperties call ") == 3 &&
              log.find("xrGetSystemProperties call 2: systemId=1 result=XR_SUCCESS chain=[SYSTEM_SPACE_WARP_PROPERTIES_FB]") !=
                  std::string::npos &&
              log.find("xrGetSystemProperties call 4: systemId=1 result=XR_SUCCESS chain=[SYSTEM_SPACE_WARP_PROPERTIES_FB]") !=
                  std::string::npos &&
              Occurrences(log, "SYSTEM_SPACE_WARP_PROPERTIES_FB.recommendedMotionVectorImageRect=250x300") == 2,
          "xrGetSystemProperties with XrSystemSpaceWarpPropertiesFB on each call, with its answer:\n" + log);
    // 4. xrGetInstanceProperties.
    once("Handshake census: xrGetInstanceProperties call 1: result=XR_SUCCESS chain=[] runtimeName=\"Space-warp "
         "stand-in\" runtimeVersion=1.0.0",
         "xrGetInstanceProperties");
    // 5. The formats given, and those the frames' swapchains have.
    once("xrEnumerateSwapchainFormats call 1: capacity=0 count=5", "the count query");
    once("formats=(5)[29 28 10 40 20]", "the formats given (a session without a graphics API: no DXGI names)");
    once("Handshake census: the swapchains the frames use by role (frame 1): motionVectors ", "the roles");
    once("(frame 1): motionVectors 0x1002(250x300 format=10 offered=#3/5 samples=1 array=2",
         "the motion vectors' format, third of the list");
    Check(log.find("spaceWarpDepth 0x3d(untracked)") != std::string::npos &&
              log.find("color 0xc0(untracked)") != std::string::npos &&
              log.find("depth 0xd0(untracked)") != std::string::npos,
          "the space-warp depth, color and depth swapchains by role:\n" + log);
    // 6. Other queries.
    once("Handshake census: xrGetSystem call 1: formFactor=HEAD_MOUNTED_DISPLAY chain=[] result=XR_SUCCESS",
         "xrGetSystem");
    once("Handshake census: xrGetDisplayRefreshRateFB call 1: result=XR_SUCCESS rate=90", "xrGetDisplayRefreshRateFB");
    // 7. The motion vector swapchains' calls (here at the session's end).
    once("Handshake census (session end): motion vector and space-warp depth swapchains", "the calls line");
    Check(log.find("images=0 acquires=1 waits=0 releases=0") != std::string::npos,
          "the motion vectors' acquire is counted:\n" + log);
}

} // namespace

// The census worker on records and censuses made up here: a head turning
// and leaning at a steady rate, its vectors in the vectors' pixels (y down)
// and offset by +-0.5 pixels (x) and +-0.25 (y) frame by frame. The record
// lists view 1 first (its color view empty); a record without a projection
// sent under a real frame's number is not taken for it.
void TestEngineCensusWorker() {
    using depthxr::FrameGenEngineCensus;
    using depthxr::FrameGenEngineCensusWorker;
    depthxr::FrameGenEngineCensusWorker worker(nullptr, "", std::chrono::hours(1));
    const std::array<double, 2> gain{2.0 / 4339.0, -2.0 / 4285.0};
    const depthxr::FrameGenExtent color{5424, 5356};
    const depthxr::FrameGenExtent half = depthxr::FrameGenPyramid(color)[1];
    auto head = [](int frame) {
        depthxr::FrameGenViewGeometry view;
        view.angle_left = -0.95f;
        view.angle_right = 0.8f;
        view.angle_up = 0.85f;
        view.angle_down = -0.9f;
        const float yaw = 0.3f + 0.02f * static_cast<float>(frame);
        view.orientation = {0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2)};
        view.position = {0.01f * static_cast<float>(frame), 1.2f, 0.0f};
        return view;
    };
    std::uint64_t sequence = 0;
    for (int f = 1; f <= 9; ++f) {
        FrameGenEngineCensusWorker::Frame frame;
        frame.record.projection_frame = static_cast<std::uint64_t>(f);
        frame.record.views.resize(2);
        frame.record.views[0].view = 1;
        frame.record.views[1].view = 0;
        frame.record.views[1].game = head(f);
        frame.color_index = {0, 0};
        frame.depth_index = {0, 0};
        frame.released = {std::nullopt, std::nullopt};
        frame.color_extent = {{0, 0}, color};
        frame.continuous = f > 1;
        worker.NoteFrame(frame);
        if (f == 5) {
            FrameGenEngineCensusWorker::Frame stray;
            stray.record.projection_frame = 5;
            worker.NoteFrame(stray);
        }
        FrameGenEngineCensus census;
        census.sequence = ++sequence;
        census.frame = static_cast<std::uint64_t>(f);
        census.view = 0;
        census.images = 1;
        census.image[0].xor_hash = static_cast<std::uint32_t>(f);
        census.rect = {0, 0, 4339, 4285};
        census.samples.resize(16 * 16 * 4);
        const double sign = f % 2 ? -1.0 : 1.0;
        for (std::uint32_t s = 0; s < 256; ++s) {
            const std::uint32_t qx = static_cast<std::uint32_t>(((s % 16) + 0.5f) * half.width / 16.0f);
            const std::uint32_t qy = static_cast<std::uint32_t>(((s / 16) + 0.5f) * half.height / 16.0f);
            const float depth = 0.5f + 0.15f * static_cast<float>((s * 7) % 17);
            const std::array<float, 2> uv{(qx + 0.5f) / static_cast<float>(half.width),
                                          (qy + 0.5f) / static_cast<float>(half.height)};
            std::array<double, 2> q{};
            if (f > 1) {
                depthxr::FrameGenEnginePrediction(head(f), head(f - 1), {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 1,
                                                  depthxr::FrameGenEngineCamera::kIncluded, uv, depth, q);
            }
            census.samples[s * 4] = {static_cast<float>(q[0] / gain[0] + sign * 0.5),
                                     static_cast<float>(q[1] / gain[1] + sign * 0.25), 0.0f, 0.0f};
            census.samples[s * 4 + 3] = {depth, 0.0f, static_cast<float>(qx), static_cast<float>(qy)};
        }
        depthxr::FrameGenEngineStatus status;
        status.census.push_back(census);
        worker.NoteStatus(status);
    }
    const std::string summary = worker.Flush();
    Check(summary.find("view0: ") != std::string::npos && summary.find("fit a=(") != std::string::npos &&
              summary.find(" n=2048") != std::string::npos,
          "the census worker fits the view at its place in the record, every frame's pair kept: " + summary);
    const std::size_t jitter = summary.find("jitter=(");
    double x = 0.0, y = 0.0;
    Check(jitter != std::string::npos && std::sscanf(summary.c_str() + jitter, "jitter=(%lf, %lf) px", &x, &y) == 2 &&
              std::abs(x - std::sqrt(2.0 / 7.0)) < 0.02 && std::abs(y - 0.5 * std::sqrt(2.0 / 7.0)) < 0.02,
          "the census worker's jitter is in vector pixels: " + summary);
}

int main() {
    // Settings and logs go beside the test, never into the user's VectorXR folder.
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::filesystem::path app_data =
        std::filesystem::path(exe_path).parent_path() / "spacewarp_probe_tests_appdata";
    std::error_code ignored;
    std::filesystem::remove_all(app_data, ignored);
    std::filesystem::create_directories(app_data);
    _wputenv_s(L"LOCALAPPDATA", app_data.c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", app_data.c_str());
    try {
        TestSelectionAndSizes();
        TestWithoutSpaceWarp();
        TestEngineMotionRecord();
        TestEngineCensusWorker();
        TestThroughTheLayer();
        TestEngineRecordsThroughTheLayer();
        TestHandshakeCensus();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Space-warp probe: selection, stripping, engine motion records and census, advertise/strip/forward "
                 "through the layer, handshake census passed\n";
    return 0;
}
