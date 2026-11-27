// The stabilized mirror window through the layer, against a stand-in runtime
// and a stand-in GPU, with the real window on its own thread (never put on
// the desktop): the frame that goes on to the runtime is the same, byte for
// byte, with the mirror on and off;
// off, nothing exists; on, the chosen eye's view of the frame as it is handed
// on is drawn into the window at most as often as set, aimed by the
// smoothing; the binding and the window's close button hide it; settings
// apply live; failures give up and a setting tries again; the swap chain goes
// before the window does; and the status the app reads.
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr_platform.h>

#include "depthxr/openxr_layer.h"
#include "depthxr/mirror.h"
#include "depthxr/mirror_window.h"
#include "depthxr/submitted_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

const XrSession kSession = reinterpret_cast<XrSession>(std::uintptr_t{0x51});
const XrSpace kLocalSpace = reinterpret_cast<XrSpace>(std::uintptr_t{2});
const XrSwapchain kColor = reinterpret_cast<XrSwapchain>(std::uintptr_t{0xc0});
const XrSwapchain kDepth = reinterpret_cast<XrSwapchain>(std::uintptr_t{0xd0});
const XrSwapchain kHud = reinterpret_cast<XrSwapchain>(std::uintptr_t{0xe0});
const XrSwapchain kFocus = reinterpret_cast<XrSwapchain>(std::uintptr_t{0xf0});
constexpr std::int64_t kFormat = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
constexpr XrDuration kFrame = 11'111'111; // 90 Hz

int checks = 0;

void Expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// The stand-in runtime's swapchain calls.
struct FakeRuntime {
    struct Swapchain {
        XrSwapchainCreateInfo info{};
        std::uint32_t next{0};
        bool acquired{false};
    };
    inline static std::map<XrSwapchain, Swapchain> swapchains;
    inline static std::uintptr_t next_handle = 0x1000;

    static void Reset() {
        swapchains.clear();
        next_handle = 0x1000;
    }
    static XrResult XRAPI_CALL Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
        *swapchain = reinterpret_cast<XrSwapchain>(next_handle++);
        swapchains[*swapchain].info = *info;
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL Destroy(XrSwapchain swapchain) {
        return swapchains.erase(swapchain) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
    }
    static XrResult XRAPI_CALL Enumerate(XrSwapchain swapchain, uint32_t capacity, uint32_t* count,
                                         XrSwapchainImageBaseHeader* images) {
        *count = 3;
        if (capacity == 0) return XR_SUCCESS;
        if (capacity < 3 || images->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;
        auto* typed = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (uint32_t i = 0; i < 3; ++i) {
            // Never dereferenced: the stand-in GPU only counts them.
            typed[i].texture =
                reinterpret_cast<ID3D11Texture2D*>(reinterpret_cast<std::uintptr_t>(swapchain) * 16 + i + 1);
        }
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL Acquire(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
        Swapchain& state = swapchains.at(swapchain);
        if (state.acquired) return XR_ERROR_CALL_ORDER_INVALID;
        state.acquired = true;
        *index = state.next;
        state.next = (state.next + 1) % 3;
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL Wait(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
    static XrResult XRAPI_CALL Release(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) {
        Swapchain& state = swapchains.at(swapchain);
        if (!state.acquired) return XR_ERROR_CALL_ORDER_INVALID;
        state.acquired = false;
        return XR_SUCCESS;
    }
};

// Records what the layer asks of the GPU and draws nothing.
class FakeGpu final : public depthxr::SubmittedImageGpu {
  public:
    struct Shown {
        depthxr::SubmittedView view;
        depthxr::MirrorDraw draw;
    };
    std::map<XrSwapchain, std::uint32_t> tracked;
    std::vector<Shown> mirror_draws;
    int closes{0};
    // Whether the window still stood each time its swap chain was let go of.
    std::vector<bool> window_alive_at_close;
    void* last_window{nullptr};
    bool supports_mirror{true};
    depthxr::MirrorDrawResult result{depthxr::MirrorDrawResult::kShown};

    const char* Name() const override { return "stand-in"; }
    depthxr::SubmittedImageApi Api() const override { return depthxr::SubmittedImageApi::kD3D11; }
    bool Track(XrSwapchain swapchain, const XrSwapchainImageBaseHeader*, std::uint32_t count, std::string&) override {
        tracked[swapchain] = count;
        return true;
    }
    void Forget(XrSwapchain swapchain) override { tracked.erase(swapchain); }
    bool Tracked(XrSwapchain swapchain) const override { return tracked.count(swapchain) != 0; }
    // `reads`: readbacks start and never finish.
    bool reads{false};
    std::uint64_t tickets{0};
    std::vector<depthxr::SubmittedView> readbacks;
    struct Composed {
        depthxr::SubmittedView view;
        depthxr::SubmittedFocus focus;
        std::uint32_t width{0}, height{0};
    };
    std::vector<Composed> composed_readbacks;
    bool supports_focus{false};
    // Draws and readbacks that carry a focus view fail.
    bool fail_focus{false};
    int failed_focus_draws{0};
    std::uint64_t Readback(const depthxr::SubmittedView& view, std::string&) override {
        if (!reads) return 0;
        readbacks.push_back(view);
        return ++tickets;
    }
    bool SupportsFocus() const override { return supports_focus; }
    std::uint64_t ReadbackComposed(const depthxr::SubmittedView& view, const depthxr::SubmittedFocus& focus,
                                   std::uint32_t width, std::uint32_t height, std::string&) override {
        if (!reads || fail_focus) return 0;
        composed_readbacks.push_back({view, focus, width, height});
        return ++tickets;
    }
    depthxr::ReadbackState Poll(std::uint64_t, depthxr::SubmittedPixels&, std::string&) override {
        return reads ? depthxr::ReadbackState::kPending : depthxr::ReadbackState::kNone;
    }
    void Finish(std::uint64_t) override {}
    bool SupportsMirror() const override { return supports_mirror; }
    // As a Vulkan session's reader: the mirror only, with the copy usage.
    bool vulkan_like{false};
    XrSwapchainUsageFlags MirrorSourceUsage() const override {
        return vulkan_like ? XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT : 0;
    }
    bool SupportsReadback() const override { return !vulkan_like; }
    depthxr::MirrorDrawResult DrawMirror(const depthxr::SubmittedView& view, const depthxr::MirrorDraw& draw,
                                         std::string& error) override {
        if (!Tracked(view.swapchain)) {
            error = "not tracked";
            return depthxr::MirrorDrawResult::kFailed;
        }
        if (result == depthxr::MirrorDrawResult::kFailed) {
            error = "the stand-in draw failed";
            return result;
        }
        if (fail_focus && draw.focus.present) {
            ++failed_focus_draws;
            error = "the stand-in cannot draw a focus view";
            return depthxr::MirrorDrawResult::kFailed;
        }
        last_window = draw.window;
        if (result == depthxr::MirrorDrawResult::kShown) mirror_draws.push_back({view, draw});
        return result;
    }
    void CloseMirror() override {
        ++closes;
        ++total_closes;
        if (last_window) window_alive_at_close.push_back(IsWindow(static_cast<HWND>(last_window)) != FALSE);
        last_window = nullptr;
    }
    void Reset() override {
        ++total_resets;
        tracked.clear();
    }
    void Abandon() override { ++total_abandons; }
    void SetQueueMutex(std::mutex* mutex) override { queue_mutex = mutex; }
    std::mutex* queue_mutex{nullptr};
    // Of every stand-in so far (one is freed with its session).
    inline static int total_closes = 0, total_resets = 0, total_abandons = 0;
};

// A game's stereo frame: both eyes in one double-wide image, each view with
// its depth chained, and a HUD quad after the projection.
struct GameFrame {
    XrCompositionLayerDepthInfoKHR depth[2]{};
    XrCompositionLayerProjectionView views[2]{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[2]{};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};

    GameFrame() {
        for (uint32_t i = 0; i < 2; ++i) {
            depth[i] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
            depth[i].subImage.swapchain = kDepth;
            depth[i].subImage.imageRect = {{static_cast<int32_t>(i) * 1000, 0}, {1000, 900}};
            depth[i].maxDepth = 1.0f;
            depth[i].nearZ = 0.1f;
            depth[i].farZ = 100.0f;
            views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[i].next = &depth[i];
            views[i].pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {i == 0 ? -0.032f : 0.032f, 1.6f, 0.0f}};
            views[i].fov = i == 0 ? XrFovf{-0.9f, 0.8f, 0.85f, -0.9f} : XrFovf{-0.8f, 0.9f, 0.85f, -0.9f};
            views[i].subImage.swapchain = kColor;
            views[i].subImage.imageRect = {{static_cast<int32_t>(i) * 1000, 0}, {1000, 900}};
            views[i].subImage.imageArrayIndex = 1;
        }
        projection.space = kLocalSpace;
        projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        projection.viewCount = 2;
        projection.views = views;
        quad.space = kLocalSpace;
        quad.subImage.swapchain = kHud;
        quad.subImage.imageRect = {{0, 0}, {512, 512}};
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        info.displayTime = 1'000'000'000;
        info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        info.layerCount = 2;
        info.layers = layers;
    }
    // The head turned to this yaw (radians) with a roll, both eyes.
    void Turn(double yaw, double roll = 0.0) {
        const float ys = static_cast<float>(std::sin(yaw / 2)), yc = static_cast<float>(std::cos(yaw / 2));
        const float rs = static_cast<float>(std::sin(roll / 2)), rc = static_cast<float>(std::cos(roll / 2));
        // yaw about +Y times roll about +Z
        const XrQuaternionf q{ys * rs, ys * rc, yc * rs, yc * rc};
        views[0].pose.orientation = q;
        views[1].pose.orientation = q;
    }
    // Every byte of the frame the runtime would read.
    std::string Bytes() const {
        std::string bytes;
        const auto add = [&](const void* data, std::size_t size) {
            bytes.append(static_cast<const char*>(data), size);
        };
        add(&info, sizeof(info));
        add(layers, sizeof(layers));
        add(&projection, sizeof(projection));
        add(views, sizeof(views));
        add(depth, sizeof(depth));
        add(&quad, sizeof(quad));
        return bytes;
    }
};

// Every byte of a frame as it goes on (which may be the layer's copy).
std::string FrameBytes(const XrFrameEndInfo& frame) {
    // Field by field: a structure's padding is whatever its memory held, and
    // two frames with the same fields may differ there.
    std::string bytes;
    const auto add = [&](const void* data, std::size_t size) { bytes.append(static_cast<const char*>(data), size); };
    const auto add_sub_image = [&](const XrSwapchainSubImage& image) {
        add(&image.swapchain, sizeof(image.swapchain));
        add(&image.imageRect, sizeof(image.imageRect));
        add(&image.imageArrayIndex, sizeof(image.imageArrayIndex));
    };
    add(&frame.displayTime, sizeof(frame.displayTime));
    add(&frame.environmentBlendMode, sizeof(frame.environmentBlendMode));
    add(&frame.layerCount, sizeof(frame.layerCount));
    for (uint32_t i = 0; i < frame.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* layer = frame.layers[i];
        add(&layer->type, sizeof(layer->type));
        add(&layer->layerFlags, sizeof(layer->layerFlags));
        add(&layer->space, sizeof(layer->space));
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            add(&projection->viewCount, sizeof(projection->viewCount));
            for (uint32_t v = 0; v < projection->viewCount; ++v) {
                const XrCompositionLayerProjectionView& view = projection->views[v];
                add(&view.pose, sizeof(view.pose));
                add(&view.fov, sizeof(view.fov));
                add_sub_image(view.subImage);
                if (view.next) {
                    const auto* depth = static_cast<const XrCompositionLayerDepthInfoKHR*>(view.next);
                    add(&depth->type, sizeof(depth->type));
                    add_sub_image(depth->subImage);
                    add(&depth->minDepth, sizeof(depth->minDepth));
                    add(&depth->maxDepth, sizeof(depth->maxDepth));
                    add(&depth->nearZ, sizeof(depth->nearZ));
                    add(&depth->farZ, sizeof(depth->farZ));
                }
            }
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
            add(&quad->eyeVisibility, sizeof(quad->eyeVisibility));
            add_sub_image(quad->subImage);
            add(&quad->pose, sizeof(quad->pose));
            add(&quad->size, sizeof(quad->size));
        }
    }
    return bytes;
}

} // namespace

namespace depthxr {

class MirrorTestPeer {
  public:
    static OpenXrLayer& Layer() { return OpenXrLayer::Instance(); }

    // A fresh session whose game has a double-wide two-slice color swapchain
    // (image 2 released last), with the mirror resolved as given.
    static FakeGpu* Start(const MirrorSettings& mirror, bool enabled = true, bool with_gpu = true) {
        auto& layer = Layer();
        {
            // As a session's end: the window goes and its thread is waited for.
            std::scoped_lock lock(layer.mutex_);
            layer.StopMirrorLocked();
            layer.JoinMirrorWindowLocked();
        }
        layer.ResetInstanceState();
        layer.ResetSessionState();
        layer.config_ = {};
        layer.has_loaded_config_ = true;
        layer.active_session_ = kSession;
        layer.resolved_settings_ = {};
        layer.resolved_settings_.core.enabled = true;
        layer.resolved_settings_generation_ = layer.config_generation_;
        layer.resolved_settings_session_ = layer.active_session_;
        layer.current_exe_name_ = "MirrorGame.exe";
        layer.next_create_swapchain_ = &FakeRuntime::Create;
        layer.next_destroy_swapchain_ = &FakeRuntime::Destroy;
        layer.next_enumerate_swapchain_images_ = &FakeRuntime::Enumerate;
        layer.next_acquire_swapchain_image_ = &FakeRuntime::Acquire;
        layer.next_wait_swapchain_image_ = &FakeRuntime::Wait;
        layer.next_release_swapchain_image_ = &FakeRuntime::Release;
        FakeRuntime::Reset();
        std::scoped_lock lock(layer.mutex_);
        layer.framegen_owns_pacing_.store(false);
        layer.pending_framegen_quad_.reset();
        layer.layer_released_images_.clear();
        layer.submitted_images_.reset();
        layer.submitted_binding_ = {};
        layer.submitted_binding_.known = true;
        layer.submitted_binding_.tried = true;
        layer.submitted_images_unavailable_ = "Vulkan images cannot be read back yet";
        layer.mirror_unavailable_.clear();
        layer.mirror_.on_desktop = false; // the window is never put on the desktop
        layer.mirror_.failed = false;
        layer.mirror_.failure.clear();
        layer.mirror_.unsupported_logged = false;
        FakeGpu* gpu = nullptr;
        if (with_gpu) {
            auto owned = std::make_unique<FakeGpu>();
            gpu = owned.get();
            layer.submitted_images_ = std::move(owned);
        }
        layer.tracked_swapchains_.clear();
        OpenXrLayer::SwapchainInfo color;
        color.session = kSession;
        color.width = 2000;
        color.height = 900;
        color.array_size = 2;
        color.sample_count = 1;
        color.format = kFormat;
        color.usage_flags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        color.last_released_image_index = 2;
        color.has_last_released_image_index = true;
        // Its release held to the frame's end (CaptureHoldsReleasesLocked).
        color.deferred_release_count = 1;
        layer.tracked_swapchains_[kColor] = color;
        Configure(mirror, enabled);
        return gpu;
    }

    // New settings, as a config reload resolves them. mutex_ held.
    static void Configure(const MirrorSettings& mirror, bool enabled = true, bool suite = true) {
        auto& layer = Layer();
        const ResolvedRuntimeConfig previous = layer.resolved_settings_;
        layer.resolved_settings_.core.enabled = suite;
        static_cast<MirrorSettings&>(layer.resolved_settings_.mirror) = mirror;
        layer.resolved_settings_.mirror.enabled = enabled;
        layer.UpdateMirrorSettingsLocked(previous);
    }
    static void Reconfigure(const MirrorSettings& mirror, bool enabled = true, bool suite = true) {
        std::scoped_lock lock(Layer().mutex_);
        Configure(mirror, enabled, suite);
    }

    // Follow the screenshot and mirror taps of a frame's end, its releases
    // held (CaptureAndReleaseEndFrameLocked).
    static std::string HandOn(const XrFrameEndInfo& frame, bool held = true) {
        std::scoped_lock lock(Layer().mutex_);
        Layer().UpdateScreenshotsLocked(frame, held);
        Layer().UpdateMirrorLocked(frame, held);
        return FrameBytes(frame);
    }
    // Only the mirror's part.
    static void Show(const XrFrameEndInfo& frame, bool held = true) {
        std::scoped_lock lock(Layer().mutex_);
        Layer().UpdateMirrorLocked(frame, held);
    }
    // Whether the next frame's releases are held, as a frame's end decides.
    static bool HoldsNext() {
        std::scoped_lock lock(Layer().mutex_);
        return Layer().CaptureHoldsReleasesLocked();
    }
    // A release the runtime has already been given.
    static void ReleaseToRuntime(XrSwapchain swapchain) {
        std::scoped_lock lock(Layer().mutex_);
        Layer().tracked_swapchains_.at(swapchain).deferred_release_count = 0;
    }
    static bool WindowStarted() { return Layer().mirror_.window->Started(); }
    static HWND Window() { return static_cast<HWND>(Layer().mirror_.window->Handle()); }
    static bool WindowShown() { return Layer().mirror_.window->Shown(); }
    static void PictureSize(std::uint32_t& width, std::uint32_t& height) {
        Layer().mirror_.window->PictureSize(width, height);
    }
    static bool PostToWindow(std::function<void()> task) { return Layer().mirror_.window->Post(std::move(task)); }
    static void SetMirrorUnavailable(const std::string& why) {
        std::scoped_lock lock(Layer().mutex_);
        Layer().mirror_unavailable_ = why;
    }
    static XrSwapchainUsageFlags MirrorUsage(const XrSwapchainCreateInfo& info) {
        std::scoped_lock lock(Layer().mutex_);
        return Layer().MirrorSourceUsageLocked(info);
    }
    static bool SetAppVisibility(bool visible) {
        std::scoped_lock lock(Layer().mutex_);
        return Layer().SetMirrorWindowVisibilityLocked(visible);
    }
    static void PressBinding() {
        std::scoped_lock lock(Layer().mutex_);
        InputBinding binding;
        binding.sound.enabled = false;
        Layer().resolved_settings_.mirror.bindings = {binding};
        Layer().mirror_pressed_ = binding;
        Layer().mirror_toggle_requested_ = true;
    }
    static std::string State() {
        std::scoped_lock lock(Layer().mutex_);
        return std::string(MirrorStateName(Layer().MirrorStateLocked()));
    }
    static std::string Reason() {
        std::scoped_lock lock(Layer().mutex_);
        return Layer().MirrorReasonLocked(Layer().MirrorStateLocked());
    }
    static std::uint64_t Skipped() { return Layer().mirror_.skipped_frames; }
    static void SetFrameGenOwnsPacing(bool owns) { Layer().framegen_owns_pacing_.store(owns); }
    static void Release(XrSwapchain swapchain, uint32_t index) {
        std::scoped_lock lock(Layer().mutex_);
        auto& info = Layer().tracked_swapchains_.at(swapchain);
        info.last_released_image_index = index;
        info.has_last_released_image_index = true;
        info.deferred_release_count = 1;
    }
    static void CreateSession(const void* binding_chain) {
        std::scoped_lock lock(Layer().mutex_);
        Layer().StartScreenshotsLocked(binding_chain);
    }
    // As xrDestroySession and xrDestroyInstance stop it.
    static void DestroySession() { Layer().StopSessionImaging(); }
    // xrDestroyInstance with the session never destroyed (the layer's own
    // path; the runtime below is not called).
    static XrResult EndInstance() { return Layer().EndInstance(XR_NULL_HANDLE, false); }
    static bool HasReader() { return Layer().submitted_images_ != nullptr; }
    static bool WindowStopping() { return Layer().mirror_.window->Stopping(); }
    static depthxr::MirrorWindow* WindowObject() { return Layer().mirror_.window.get(); }
    static bool ReaderAskedFor() { return Layer().submitted_binding_.tried; }
    static bool ReaderWanted() {
        std::scoped_lock lock(Layer().mutex_);
        return Layer().SubmittedImagesWantedLocked();
    }
    static void LogTo(const std::filesystem::path& path) { Layer().logger_.Initialize(path); }
    static std::string Log() {
        static int reads = 0;
        Layer().logger_.Error("Mirror window tests: log read " + std::to_string(++reads) + ".");
        std::ifstream stream(Layer().logger_.ActiveLogPath(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }
    // A swapchain of the game's with an image released and held, as kColor.
    static void AddSwapchain(XrSwapchain swapchain, std::uint32_t width, std::uint32_t height, std::uint32_t image) {
        std::scoped_lock lock(Layer().mutex_);
        OpenXrLayer::SwapchainInfo info;
        info.session = kSession;
        info.width = width;
        info.height = height;
        info.array_size = 1;
        info.sample_count = 1;
        info.format = kFormat;
        info.usage_flags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.last_released_image_index = image;
        info.has_last_released_image_index = true;
        info.deferred_release_count = 1;
        Layer().tracked_swapchains_[swapchain] = info;
    }
    // Frame Generation composes the frame's quad views itself, each eye as
    // given (ClaimQuadFrame's route), or does not.
    static void ClaimQuad(const FrameGenComposition* composition) {
        std::scoped_lock lock(Layer().mutex_);
        Layer().pending_framegen_quad_.reset();
        if (composition) {
            FrameGenQuadFrame& pending = Layer().pending_framegen_quad_.emplace();
            pending.route = FrameGenQuadRoute::kClaimed;
            pending.composition = *composition;
        }
    }
    static void SetTransition(double percent) {
        std::scoped_lock lock(Layer().mutex_);
        Layer().resolved_settings_.quadviews.transition_thickness_percent = percent;
    }
    // The screenshot binding (a silent one) pressed, the frame handed over
    // with its releases held.
    static void PressScreenshot(const XrFrameEndInfo& frame) {
        std::scoped_lock lock(Layer().mutex_);
        InputBinding binding;
        binding.sound.enabled = false;
        Layer().resolved_settings_.core.screenshot.bindings = {binding};
        Layer().resolved_settings_.core.screenshot.folder = "D:\\Shots";
        Layer().screenshot_pressed_ = binding;
        Layer().screenshot_requested_ = true;
        Layer().UpdateScreenshotsLocked(frame, true);
        // The stand-in never finishes a readback: the press is dropped.
        Layer().screenshot_.Stop(nullptr);
        Layer().screenshot_sounds_.clear();
    }
    static void SetSettingsOnly(const MirrorSettings& mirror) {
        std::scoped_lock lock(Layer().mutex_);
        static_cast<MirrorSettings&>(Layer().resolved_settings_.mirror) = mirror;
    }
};

} // namespace depthxr

namespace {

using Peer = depthxr::MirrorTestPeer;

bool WaitFor(const std::function<bool()>& condition, int milliseconds = 5000);
struct GameFrame;
// Frames go by until the window that was asked to go is gone, its thread
// ended and joined (no frame waits for it).
bool WindowGone(const XrFrameEndInfo& frame, HWND window);

bool WaitFor(const std::function<bool()>& condition, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Frames until the window stands (its thread makes it), then `count` more.
// Each is 1/90 s after the last.
void Frames(GameFrame& game, int count, bool wait_for_window = true) {
    if (wait_for_window) {
        // A window that was asked to go is joined by a later frame, and the
        // new one started by the frame after: frames until that is done.
        while (Peer::WindowStopping() || !Peer::WindowStarted()) {
            Peer::Show(game.info);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        game.info.displayTime += kFrame;
        Expect(WaitFor([] { return Peer::Window() != nullptr; }), "The mirror's window is made on its own thread");
    }
    for (int i = 0; i < count; ++i) {
        Peer::Show(game.info);
        game.info.displayTime += kFrame;
    }
}

depthxr::MirrorHomography PlainCrop(const XrFovf& fov, const depthxr::MirrorSettings& settings) {
    return depthxr::MirrorHomographyFor(
        fov, depthxr::MirrorCropFor(fov, depthxr::MirrorAspectRatio(settings.aspect), settings.margin),
        depthxr::MirrorQuat{});
}

void TestOffDoesNothing() {
    GameFrame game;
    for (const int how : {0, 1}) {
        FakeGpu* gpu = Peer::Start(depthxr::MirrorSettings{}, how == 0 ? false : true);
        if (how == 1) Peer::Reconfigure(depthxr::MirrorSettings{}, true, false); // the suite switch
        const std::string before = game.Bytes();
        for (int i = 0; i < 5; ++i) {
            Expect(Peer::HandOn(game.info) == FrameBytes(game.info), "With the mirror off the game's frame goes on");
            game.info.displayTime += kFrame;
        }
        game.info.displayTime -= 5 * kFrame;
        Expect(game.Bytes() == before, "The game's frame is not written");
        Expect(!Peer::WindowStarted() && gpu->mirror_draws.empty() && gpu->tracked.empty() && gpu->closes == 0,
               "Off, there is no window, no thread, no draw, and no image is held");
        Expect(Peer::State() == "off" && Peer::Reason().empty(), "Off is off in the status");
        Expect(!Peer::ReaderWanted(), "Off, the mirror does not ask for the session's image reader");
    }
    FakeGpu* gpu = Peer::Start(depthxr::MirrorSettings{});
    Expect(Peer::ReaderWanted() && gpu, "On, it does");
}

void TestShowsTheEye() {
    GameFrame game;
    depthxr::MirrorSettings settings;
    FakeGpu* gpu = Peer::Start(settings);
    Expect(Peer::State() == "pending", "On and nothing shown yet is pending");
    const HWND foreground = GetForegroundWindow();
    Frames(game, 3);
    Expect(GetForegroundWindow() == foreground, "The mirror's window does not take the foreground");
    Expect(!gpu->mirror_draws.empty(), "Frames are drawn into the window");
    const FakeGpu::Shown& first = gpu->mirror_draws.front();
    Expect(first.view.swapchain == kColor && first.view.view_index == 0 && first.view.image_index == 2 &&
               first.view.array_slice == 1 && first.view.rect.offset.x == 0 && first.view.rect.extent.width == 1000 &&
               first.view.rect.extent.height == 900 && first.view.format == kFormat,
           "The left eye's rectangle of the image last released is shown");
    Expect(first.draw.window == Peer::Window() && first.draw.width == 1280 && first.draw.height == 720 &&
               first.draw.target == XR_NULL_HANDLE,
           "It is drawn into the window, at the settings' 1280x720");
    Expect(first.draw.homography == PlainCrop(game.views[0].fov, settings),
           "A still head is shown as the plain 16:9 crop with its margin");
    // The swap chain's making is handed to the window's thread, not done here.
    {
        std::atomic<DWORD> ran_on{0};
        Expect(first.draw.on_window_thread && first.draw.on_window_thread([&] { ran_on = GetCurrentThreadId(); }),
               "The draw carries a way onto the window's thread");
        Expect(WaitFor([&] { return ran_on.load() != 0; }) && ran_on.load() != GetCurrentThreadId() &&
                   ran_on.load() == GetWindowThreadProcessId(Peer::Window(), nullptr),
               "What is handed over runs on the window's own thread");
    }
    Expect(gpu->tracked.count(kColor) == 1 && gpu->tracked.size() == 1, "Only the shown swapchain is tracked");
    wchar_t title[128]{};
    GetWindowTextW(Peer::Window(), title, 127);
    Expect(std::wstring(title) == L"VectorXR Mirror \u2013 MirrorGame", "The window is named after the game");
    Expect(Peer::State() == "active" && Peer::Reason() == "showing the left eye at 1280x720",
           "Showing is active in the status: " + Peer::Reason());

    // The image the game releases next is the one shown next.
    Peer::Release(kColor, 0);
    gpu->mirror_draws.clear();
    Frames(game, 3, false);
    Expect(!gpu->mirror_draws.empty() && gpu->mirror_draws.back().view.image_index == 0,
           "Each frame shows the image released for it");

    // The right eye.
    settings.eye = depthxr::MirrorEye::Right;
    Peer::Reconfigure(settings);
    gpu->mirror_draws.clear();
    Frames(game, 3, false);
    Expect(!gpu->mirror_draws.empty() && gpu->mirror_draws.back().view.view_index == 1 &&
               gpu->mirror_draws.back().view.rect.offset.x == 1000 &&
               gpu->mirror_draws.back().draw.homography == PlainCrop(game.views[1].fov, settings),
           "The right eye's rectangle and field of view are shown when it is chosen");
    Expect(Peer::Reason() == "showing the right eye at 1280x720", "The status names the eye");
}

// A quad views frame as it is handed on when nothing of VectorXR's composed
// it into one image (the runtime composes the four views, or Frame
// Generation does): the two peripheral views in the game's double-wide
// image, the two focus views in an image of their own.
struct QuadFrame : GameFrame {
    XrCompositionLayerProjectionView quad_views[4]{};
    QuadFrame() {
        for (uint32_t i = 0; i < 4; ++i) {
            quad_views[i] = views[i % 2];
            quad_views[i].next = nullptr;
        }
        for (uint32_t eye = 0; eye < 2; ++eye) {
            XrCompositionLayerProjectionView& focus = quad_views[eye + 2];
            // The middle of the eye's field of view, in tangents: a quarter
            // to three quarters across, three tenths to seven tenths down.
            const XrFovf& full = quad_views[eye].fov;
            const double left = std::tan(full.angleLeft), right = std::tan(full.angleRight);
            const double up = std::tan(full.angleUp), down = std::tan(full.angleDown);
            focus.fov.angleLeft = static_cast<float>(std::atan(left + 0.25 * (right - left)));
            focus.fov.angleRight = static_cast<float>(std::atan(left + 0.75 * (right - left)));
            focus.fov.angleUp = static_cast<float>(std::atan(up - 0.3 * (up - down)));
            focus.fov.angleDown = static_cast<float>(std::atan(up - 0.7 * (up - down)));
            focus.subImage.swapchain = kFocus;
            focus.subImage.imageRect = {{static_cast<int32_t>(eye) * 1600, 0}, {1500, 1080}};
            focus.subImage.imageArrayIndex = 0;
        }
        projection.viewCount = 4;
        projection.views = quad_views;
    }
};

bool Near(float a, double b, double tolerance = 1e-4) {
    return std::fabs(static_cast<double>(a) - b) <= tolerance;
}

// Quad views the runtime or Frame Generation composes reach the frame's end
// as four views: the mirror window and screenshots draw the eye's focus view
// over its peripheral view, placed as the composer places it, where the
// session's images can be drawn so; otherwise they show the peripheral view
// as before. The frame that goes on is not changed.
void TestQuadViewsFocus() {
    const auto logs = std::filesystem::temp_directory_path() / "vectorxr-test-mirror-layer";
    std::filesystem::create_directories(logs);
    Peer::LogTo(logs / "mirror-quad.log");
    QuadFrame game;
    depthxr::MirrorSettings settings;
    settings.frame_rate = 1000; // every frame

    // A backend that cannot draw a focus view: the peripheral view, as before.
    FakeGpu* gpu = Peer::Start(settings);
    Peer::AddSwapchain(kFocus, 3200, 1080, 1);
    Frames(game, 3);
    Expect(!gpu->mirror_draws.empty() && !gpu->mirror_draws.back().draw.focus.present &&
               gpu->mirror_draws.back().view.view_index == 0 && gpu->tracked.count(kFocus) == 0,
           "A backend that draws no focus view shows the eye's peripheral view alone");
    Expect(Peer::Log().find("shows the stereo view without the focus view") != std::string::npos,
           "...and the log says the focus view is not shown");

    // The runtime composes the four views: the focus view placed by the
    // views' fields of view.
    gpu = Peer::Start(settings);
    gpu->supports_focus = true;
    Peer::AddSwapchain(kFocus, 3200, 1080, 1);
    Peer::SetTransition(20.0);
    const std::string before = game.Bytes();
    const XrTime began = game.info.displayTime;
    const std::string on = Peer::HandOn(game.info);
    Expect(game.Bytes() == before && on == FrameBytes(game.info), "The frame that goes on is the game's, unchanged");
    Frames(game, 3);
    Expect(!gpu->mirror_draws.empty(), "A quad views frame is drawn into the window");
    {
        const FakeGpu::Shown& shown = gpu->mirror_draws.back();
        const depthxr::SubmittedFocus& focus = shown.draw.focus;
        Expect(shown.view.view_index == 0 && shown.view.swapchain == kColor && focus.present &&
                   focus.view.view_index == 2 && focus.view.swapchain == kFocus && focus.view.image_index == 1 &&
                   focus.view.rect.offset.x == 0 && focus.view.rect.extent.width == 1500 &&
                   focus.view.rect.extent.height == 1080 && focus.view.resolved,
               "The left eye's focus view is drawn over its peripheral view");
        Expect(Near(focus.place.rect[0], 0.25) && Near(focus.place.rect[1], 0.3) && Near(focus.place.rect[2], 0.75) &&
                   Near(focus.place.rect[3], 0.7),
               "The focus view lies where its field of view puts it in the eye's");
        Expect(Near(focus.place.feather[0], 0.5 * 0.2) && Near(focus.place.feather[1], 0.4 * 0.2),
               "It is blended in over the Quadviews transition band");
        Expect(shown.draw.homography == PlainCrop(game.quad_views[0].fov, settings),
               "The window is aimed by the eye's own view, as without a focus view");
        Expect(gpu->tracked.count(kFocus) == 1 && gpu->tracked.count(kColor) == 1, "Both images are tracked");
    }
    // Shown, the frame is still what the game made (a frame later on).
    const XrTime now = game.info.displayTime;
    game.info.displayTime = began;
    Expect(game.Bytes() == before, "The game's frame is not written");
    game.info.displayTime = now;
    Expect(Peer::Log().find("draws the eye's focus view over it") != std::string::npos,
           "The log says the focus view is drawn over the eye");
    // The right eye.
    settings.eye = depthxr::MirrorEye::Right;
    Peer::Reconfigure(settings);
    gpu->mirror_draws.clear();
    Frames(game, 2, false);
    Expect(!gpu->mirror_draws.empty() && gpu->mirror_draws.back().view.view_index == 1 &&
               gpu->mirror_draws.back().draw.focus.present &&
               gpu->mirror_draws.back().draw.focus.view.view_index == 3 &&
               gpu->mirror_draws.back().draw.focus.view.rect.offset.x == 1600,
           "The right eye is shown with the right eye's focus view");
    settings.eye = depthxr::MirrorEye::Left;
    Peer::Reconfigure(settings);

    // A focus view whose image is already the runtime's is not read: the
    // peripheral view alone for that frame.
    Peer::ReleaseToRuntime(kFocus);
    gpu->mirror_draws.clear();
    Frames(game, 2, false);
    Expect(!gpu->mirror_draws.empty() && !gpu->mirror_draws.back().draw.focus.present,
           "A focus view the runtime already has is not read");
    Peer::Release(kFocus, 1);

    // A screenshot of such a frame: each eye with its focus view, large
    // enough to hold the focus view at its own density (1500 texels over
    // half the eye's 1000: three times the eye's size).
    gpu->reads = true;
    Peer::PressScreenshot(game.info);
    Expect(gpu->composed_readbacks.size() == 2 && gpu->readbacks.empty(), "Both eyes are saved with their focus views");
    for (std::uint32_t eye = 0; eye < 2 && gpu->composed_readbacks.size() == 2; ++eye) {
        const FakeGpu::Composed& composed = gpu->composed_readbacks[eye];
        Expect(composed.view.view_index == eye && composed.focus.present && composed.focus.view.view_index == eye + 2 &&
                   composed.width == 3000 && composed.height == 2700 && Near(composed.focus.place.rect[0], 0.25) &&
                   Near(composed.focus.place.rect[3], 0.7),
               "An eye's screenshot draws its focus view over it at the focus view's density");
    }
    Expect(Peer::Log().find("each eye is saved with its focus view drawn over it") != std::string::npos,
           "The log says what a quad views screenshot holds");

    // Frame Generation composes the frame: the eye as it places and blends
    // it, at the size it composes.
    gpu = Peer::Start(settings);
    gpu->supports_focus = true;
    Peer::AddSwapchain(kFocus, 3200, 1080, 1);
    depthxr::FrameGenComposition composition;
    composition.output = {2400, 2160};
    composition.eyes.resize(2);
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        composition.eyes[eye].peripheral = eye;
        composition.eyes[eye].focus = eye + 2;
        composition.eyes[eye].focus_rect = {0.2f + 0.05f * eye, 0.35f, 0.7f + 0.05f * eye, 0.8f};
        composition.eyes[eye].feather = {0.03f, 0.04f};
    }
    Peer::ClaimQuad(&composition);
    Frames(game, 3);
    Expect(!gpu->mirror_draws.empty() && gpu->mirror_draws.back().draw.focus.present &&
               gpu->mirror_draws.back().draw.focus.place.rect == composition.eyes[0].focus_rect &&
               gpu->mirror_draws.back().draw.focus.place.feather == composition.eyes[0].feather &&
               gpu->mirror_draws.back().draw.focus.view.view_index == 2,
           "A frame Frame Generation composes is shown with the focus view where Frame Generation puts it");
    gpu->reads = true;
    Peer::PressScreenshot(game.info);
    Expect(gpu->composed_readbacks.size() == 2 && gpu->composed_readbacks[1].width == 2400 &&
               gpu->composed_readbacks[1].height == 2160 &&
               gpu->composed_readbacks[1].focus.place.rect == composition.eyes[1].focus_rect &&
               gpu->composed_readbacks[1].focus.view.view_index == 3,
           "Its screenshot is the eye at the size Frame Generation composes, each eye with its own focus view");
    // A composition that does not name the eye's own views is not used.
    composition.eyes[0].focus = 3;
    Peer::ClaimQuad(&composition);
    gpu->mirror_draws.clear();
    Frames(game, 2, false);
    Expect(!gpu->mirror_draws.empty() && !gpu->mirror_draws.back().draw.focus.present,
           "A composition of other views than the eye's is not drawn");
    Peer::ClaimQuad(nullptr);

    // A focus view that cannot be drawn costs neither the window nor the
    // screenshot: the eye's own view, as before, and it is not tried again.
    gpu = Peer::Start(settings);
    gpu->supports_focus = true;
    gpu->fail_focus = true;
    Peer::AddSwapchain(kFocus, 3200, 1080, 1);
    Frames(game, 5);
    Expect(gpu->failed_focus_draws == 1 && gpu->mirror_draws.size() >= 4 && !gpu->mirror_draws.front().draw.focus.present &&
               !gpu->mirror_draws.back().draw.focus.present && Peer::State() == "active",
           "A focus view that cannot be drawn is tried once; the window shows the eye's own view from that frame on");
    Expect(Peer::Log().find("focus view cannot be drawn over it (the stand-in cannot draw a focus view)") != std::string::npos,
           "...and the log says why");
    gpu->reads = true;
    Peer::PressScreenshot(game.info);
    Expect(gpu->readbacks.size() == 2 && gpu->composed_readbacks.empty() && gpu->readbacks[1].view_index == 1,
           "A screenshot whose focus views cannot be drawn saves the two stereo views");

    // A stereo frame has no focus view, whatever the backend can draw.
    GameFrame stereo;
    gpu = Peer::Start(settings);
    gpu->supports_focus = true;
    Frames(stereo, 3);
    Expect(!gpu->mirror_draws.empty() && !gpu->mirror_draws.back().draw.focus.present,
           "A stereo frame is shown as it is");
    gpu->reads = true;
    Peer::PressScreenshot(stereo.info);
    Expect(gpu->readbacks.size() == 2 && gpu->composed_readbacks.empty(), "...and saved as it is");
}

// The runtime may read an image, or change its state, once its release has
// reached it: the mirror reads only frames whose releases the layer still
// holds, and asks for the next frame's to be held while it can draw.
void TestReadsOnlyHeldImages() {
    GameFrame game;
    depthxr::MirrorSettings settings;
    settings.frame_rate = 1000; // every frame
    FakeGpu* gpu = Peer::Start(settings);
    Expect(!Peer::HoldsNext(), "Before its window stands, no release is held");
    Frames(game, 1);
    Expect(Peer::HoldsNext(), "A window that can be drawn into has the next frame's releases held");
    gpu->mirror_draws.clear();
    Peer::Show(game.info, false);
    game.info.displayTime += kFrame;
    Expect(gpu->mirror_draws.empty(), "A frame whose releases went on is not read");
    Peer::ReleaseToRuntime(kColor);
    Peer::Show(game.info, true);
    game.info.displayTime += kFrame;
    Expect(gpu->mirror_draws.empty() && Peer::State() != "failed",
           "An image already the runtime's is not read, and that is no failure");
    Peer::Release(kColor, 1);
    Peer::Show(game.info, true);
    game.info.displayTime += kFrame;
    Expect(!gpu->mirror_draws.empty() && gpu->mirror_draws.back().view.image_index == 1, "A held image is read");
    Peer::Reconfigure(settings, false);
    Peer::Show(game.info, true);
    Expect(!Peer::HoldsNext(), "With the mirror off nothing is held");
}

void TestFrameIsTheSameOnAndOff() {
    // The same 120 frames of a shaking, turning, rolling head, mirror off and
    // on (smoothing and level horizon at their strongest).
    const auto run = [](bool mirror_on, std::string& game_bytes_changed) {
        GameFrame game;
        depthxr::MirrorSettings settings;
        settings.smoothing = 100.0;
        settings.level_horizon = true;
        settings.margin = 30.0;
        FakeGpu* gpu = Peer::Start(settings, mirror_on);
        if (mirror_on) {
            Frames(game, 0);
        } else {
            game.info.displayTime += kFrame;
        }
        std::vector<std::string> sent;
        for (int frame = 0; frame < 120; ++frame) {
            game.Turn(0.4 * std::sin(frame * 0.05) + 0.01 * std::sin(frame * 2.1), 0.2 * std::sin(frame * 0.11));
            const std::string before = game.Bytes();
            sent.push_back(Peer::HandOn(game.info));
            if (game.Bytes() != before) game_bytes_changed = "the game's frame was written at frame " + std::to_string(frame);
            game.info.displayTime += kFrame;
        }
        Expect(mirror_on ? !gpu->mirror_draws.empty() : gpu->mirror_draws.empty(),
               mirror_on ? "With the mirror on the frames are shown" : "With the mirror off nothing is shown");
        if (mirror_on) {
            // The mirror did aim: its picture is not the plain crop throughout.
            int aimed = 0;
            for (const FakeGpu::Shown& shown : gpu->mirror_draws) {
                aimed += shown.draw.homography != PlainCrop(game.views[0].fov, settings);
            }
            Expect(aimed > 10, "The mirror re-aims its own picture");
        }
        return sent;
    };
    std::string changed_off, changed_on;
    const std::vector<std::string> off = run(false, changed_off);
    const std::vector<std::string> on = run(true, changed_on);
    Expect(changed_off.empty() && changed_on.empty(), "The game's own frame is never written: " + changed_on);
    Expect(off.size() == on.size(), "Both runs hand on every frame");
    for (std::size_t i = 0; i < off.size(); ++i) {
        std::size_t at = 0;
        while (at < off[i].size() && at < on[i].size() && off[i][at] == on[i][at]) ++at;
        Expect(off[i] == on[i], "The runtime receives the same frame with the mirror on and off, frame " +
                                   std::to_string(i) + " (first difference at byte " + std::to_string(at) + " of " +
                                   std::to_string(off[i].size()) + "/" + std::to_string(on[i].size()) + ")");
    }
    GameFrame game;
    Peer::Start(depthxr::MirrorSettings{});
    Frames(game, 2);
    Expect(Peer::HandOn(game.info) == FrameBytes(game.info), "The mirror hands the game's own frame on");
}

void TestSmoothingAndPacing() {
    GameFrame game;
    depthxr::MirrorSettings settings;
    settings.smoothing = 80.0;
    FakeGpu* gpu = Peer::Start(settings);
    Frames(game, 0);
    gpu->mirror_draws.clear();
    // A 90 Hz game at the default 60: two frames in three.
    int differing = 0;
    for (int frame = 0; frame < 180; ++frame) {
        game.Turn(0.008 * std::sin(frame * 0.6));
        Peer::Show(game.info);
        game.info.displayTime += kFrame;
    }
    for (const FakeGpu::Shown& shown : gpu->mirror_draws) {
        differing += shown.draw.homography != PlainCrop(game.views[0].fov, settings);
    }
    Expect(gpu->mirror_draws.size() >= 118 && gpu->mirror_draws.size() <= 122,
           "A 90 Hz game is shown at 60 frames a second: " + std::to_string(gpu->mirror_draws.size()) + " of 180");
    Expect(differing > 100, "A shaking head is re-aimed in the window");

    // Smoothing 0: the plain crop on every frame; a higher cap shows every frame.
    settings.smoothing = 0.0;
    settings.frame_rate = 120;
    Peer::Reconfigure(settings);
    gpu->mirror_draws.clear();
    for (int frame = 0; frame < 90; ++frame) {
        game.Turn(0.3 * std::sin(frame * 0.3), 0.1);
        Peer::Show(game.info);
        game.info.displayTime += kFrame;
    }
    bool plain = true;
    for (const FakeGpu::Shown& shown : gpu->mirror_draws) {
        plain = plain && shown.draw.homography == PlainCrop(game.views[0].fov, settings);
    }
    Expect(gpu->mirror_draws.size() == 90 && plain,
           "With smoothing 0 every frame is the plain crop, and a 120 cap shows all 90");
    settings.frame_rate = 30;
    Peer::Reconfigure(settings);
    gpu->mirror_draws.clear();
    Frames(game, 180, false);
    Expect(gpu->mirror_draws.size() >= 58 && gpu->mirror_draws.size() <= 62, "A 30 cap shows 30 a second");

    // A window or GPU that is busy: the frame is skipped, nothing fails.
    gpu->result = depthxr::MirrorDrawResult::kSkipped;
    gpu->mirror_draws.clear();
    Frames(game, 90, false);
    Expect(gpu->mirror_draws.empty() && Peer::Skipped() >= 28 && Peer::State() == "active",
           "Skipped frames are counted and are no failure");
    gpu->result = depthxr::MirrorDrawResult::kShown;

    // In a session Frame Generation paces, the game's frames are shown as before.
    Peer::SetFrameGenOwnsPacing(true);
    gpu->mirror_draws.clear();
    Frames(game, 30, false);
    Expect(!gpu->mirror_draws.empty() && gpu->mirror_draws.back().view.swapchain == kColor,
           "With Frame Generation the game's own frames are shown");
    Peer::SetFrameGenOwnsPacing(false);
}

void TestHideAndShow() {
    GameFrame game;
    depthxr::MirrorSettings settings;
    FakeGpu* gpu = Peer::Start(settings);
    Frames(game, 3);
    const HWND window = Peer::Window();
    // The app sets an explicit state even without an input binding.
    Expect(Peer::SetAppVisibility(false) && WaitFor([] { return !Peer::WindowShown(); }), "The app hides the mirror");
    Expect(Peer::SetAppVisibility(false) && Peer::SetAppVisibility(true) && WaitFor([] { return Peer::WindowShown(); }),
           "Repeating hide does not toggle, and show reopens the existing mirror");
    // The binding hides it.
    Peer::PressBinding();
    Frames(game, 1, false);
    Expect(WaitFor([] { return !Peer::WindowShown(); }), "The binding hides the window");
    gpu->mirror_draws.clear();
    Frames(game, 30, false);
    Expect(gpu->mirror_draws.empty() && Peer::Window() == window && IsWindow(window),
           "A hidden window is kept and not drawn into");
    Expect(Peer::State() == "hidden" && Peer::Reason().find("hidden") != std::string::npos,
           "Hidden is hidden in the status");
    // And shows it again.
    Peer::PressBinding();
    Frames(game, 1, false);
    Expect(WaitFor([] { return Peer::WindowShown(); }), "The binding shows the window again");
    Frames(game, 10, false);
    Expect(!gpu->mirror_draws.empty() && Peer::State() == "active", "A window shown again is drawn into");
    // Its close button hides it too; the binding brings it back.
    PostMessageW(window, WM_CLOSE, 0, 0);
    Expect(WaitFor([] { return !Peer::WindowShown(); }), "Closing the window hides it");
    gpu->mirror_draws.clear();
    Frames(game, 10, false);
    Expect(gpu->mirror_draws.empty() && IsWindow(window) && Peer::State() == "hidden",
           "A closed window is hidden, not destroyed, and not drawn into");
    Peer::PressBinding();
    Frames(game, 1, false);
    Expect(WaitFor([] { return Peer::WindowShown(); }), "The binding brings a closed window back");
    // Turning the mirror off and on brings it back as well, as a new window.
    PostMessageW(window, WM_CLOSE, 0, 0);
    Expect(WaitFor([] { return !Peer::WindowShown(); }), "Closed again");
    Peer::Reconfigure(settings, false);
    Frames(game, 1, false);
    Expect(gpu->closes == 1 && gpu->window_alive_at_close.size() == 1 && gpu->window_alive_at_close[0] &&
               Peer::State() == "off",
           "Turned off, the swap chain is let go of first, while the window still stands");
    Expect(!Peer::SetAppVisibility(true), "The app cannot enable a disabled mirror profile");
    Expect(WindowGone(game.info, window) && gpu->closes == 1,
           "...and then the window is destroyed, its thread joined by a later frame");
    Peer::Reconfigure(settings, true);
    gpu->mirror_draws.clear();
    Frames(game, 5);
    Expect(Peer::WindowShown() && !gpu->mirror_draws.empty() && Peer::State() == "active",
           "Turned on again, the window is back and shown");
}

void TestLiveSettings() {
    GameFrame game;
    depthxr::MirrorSettings settings;
    FakeGpu* gpu = Peer::Start(settings);
    Frames(game, 3);
    const HWND window = Peer::Window();
    settings.width = 960;
    settings.aspect = depthxr::MirrorAspect::k4x3;
    settings.margin = 5.0;
    Peer::Reconfigure(settings);
    Frames(game, 1, false);
    std::uint32_t width = 0, height = 0;
    Expect(WaitFor([&] {
               Peer::PictureSize(width, height);
               return width == 960 && height == 720;
           }),
           "A new width and aspect resize the window");
    gpu->mirror_draws.clear();
    Frames(game, 5, false);
    Expect(Peer::Window() == window && !gpu->mirror_draws.empty() && gpu->mirror_draws.back().draw.width == 960 &&
               gpu->mirror_draws.back().draw.height == 720 &&
               gpu->mirror_draws.back().draw.homography == PlainCrop(game.views[0].fov, settings),
           "The same window goes on at the new size, aspect and margin");
    Expect(Peer::Reason() == "showing the left eye at 960x720", "The status follows the window's size");
}

// Dragged or resized on the desktop, the window keeps its picture: every
// frame goes on being drawn, at the settings' size (one swap chain, which
// Windows stretches to the window), and only a setting changes that size.
void TestResizingKeepsThePicture() {
    GameFrame game;
    depthxr::MirrorSettings settings;
    settings.smoothing = 0.0;
    settings.frame_rate = 120; // every frame of the 90 Hz game
    FakeGpu* gpu = Peer::Start(settings);
    Frames(game, 3);
    const HWND window = Peer::Window();
    gpu->mirror_draws.clear();
    RECT frame{};
    GetWindowRect(window, &frame);
    int sizes = 0;
    for (int step = 0; step < 40; ++step) {
        // What a drag of the corner does, 40 times: another client size each.
        const int width = 400 + step * 37;
        const int height = width * 9 / 16;
        SetWindowPos(window, nullptr, frame.left + step * 3, frame.top + step * 2, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        std::uint32_t client_width = 0, client_height = 0;
        Peer::PictureSize(client_width, client_height);
        sizes += client_width != 1280;
        const std::size_t before = gpu->mirror_draws.size();
        Frames(game, 1, false);
        Expect(gpu->mirror_draws.size() == before + 1, "A frame is drawn while the window is being resized");
        const FakeGpu::Shown& shown = gpu->mirror_draws.back();
        Expect(shown.draw.window == window && shown.draw.width == 1280 && shown.draw.height == 720,
               "It is drawn at the settings' size, whatever the window's");
        Expect(shown.draw.homography == PlainCrop(game.views[0].fov, settings),
               "The picture's crop does not follow the window's size");
    }
    Expect(sizes >= 38, "The window did have other sizes");
    Expect(gpu->closes == 0, "Nothing of the window's swap chain is let go of for a resize");
    Expect(Peer::Reason() == "showing the left eye at 1280x720", "The status names the picture's size");
    // With the window's thread busy (as it is inside the move/resize loop
    // of a drag) frames go on too: the frame thread waits for nothing of
    // the window's.
    std::atomic<bool> busy_done{false};
    Expect(Peer::PostToWindow([&] {
               Sleep(400);
               busy_done.store(true);
           }),
           "A task is handed to the window's thread");
    const std::size_t before = gpu->mirror_draws.size();
    const auto begun = std::chrono::steady_clock::now();
    Frames(game, 20, false);
    Expect(gpu->mirror_draws.size() == before + 20 && !busy_done.load() &&
               std::chrono::steady_clock::now() - begun < std::chrono::milliseconds(300),
           "Frames are drawn while the window's thread is busy");
    Expect(WaitFor([&] { return busy_done.load(); }), "The window's thread comes back");
    // The settings' size is what makes it anew.
    settings.width = 1920;
    Peer::Reconfigure(settings);
    Frames(game, 1, false);
    Expect(WaitFor([&] {
               Frames(game, 1, false);
               return !gpu->mirror_draws.empty() && gpu->mirror_draws.back().draw.width == 1920 &&
                      gpu->mirror_draws.back().draw.height == 1080;
           }),
           "A new width setting changes the picture's size");
}

void TestFillsAMonitor() {
    GameFrame game;
    const std::vector<depthxr::MirrorMonitor> monitors = depthxr::ListMirrorMonitors();
    Expect(!monitors.empty(), "The desktop has a monitor");
    const depthxr::MirrorMonitor& monitor = monitors.front();
    std::string name; // device names are ASCII
    for (const wchar_t letter : monitor.name) name.push_back(static_cast<char>(letter));
    depthxr::MirrorSettings settings;
    settings.smoothing = 0.0;
    settings.fill_monitor = true;
    // A window smaller than any desktop it runs on: Windows keeps a window to
    // about the screen's size, and CI runners have a 1024x768 one.
    settings.width = 960;
    settings.monitor = name;
    FakeGpu* gpu = Peer::Start(settings);
    const HWND foreground = GetForegroundWindow();
    Frames(game, 5);
    Expect(!gpu->mirror_draws.empty(), "A mirror that fills a monitor is drawn");
    const FakeGpu::Shown& shown = gpu->mirror_draws.back();
    Expect(shown.draw.width == static_cast<std::uint32_t>(monitor.width) &&
               shown.draw.height == static_cast<std::uint32_t>(monitor.height),
           "Its picture is of the monitor's size");
    const double aspect = static_cast<double>(monitor.width) / monitor.height;
    Expect(shown.draw.homography ==
               depthxr::MirrorHomographyFor(game.views[0].fov,
                                            depthxr::MirrorCropFor(game.views[0].fov, aspect, settings.margin),
                                            depthxr::MirrorQuat{}),
           "The picture is cropped to the monitor's shape, not the aspect setting's");
    Expect(Peer::Reason() == "showing the left eye at " + std::to_string(monitor.width) + "x" +
                                 std::to_string(monitor.height) + ", filling monitor " + name,
           "The status names the monitor: " + Peer::Reason());
    Expect(GetForegroundWindow() == foreground && (GetWindowLongPtrW(Peer::Window(), GWL_EXSTYLE) & WS_EX_NOACTIVATE),
           "Filling a monitor does not take the foreground");
    const HWND window = Peer::Window();

    // The monitor gone: a window of the settings' size, and the status says why.
    settings.monitor = "\\\\.\\DISPLAY77";
    Peer::Reconfigure(settings);
    Frames(game, 1, false);
    std::uint32_t width = 0, height = 0;
    Expect(WaitFor([&] {
               Peer::PictureSize(width, height);
               return width == 960 && height == 540;
           }),
           "A monitor that is not connected leaves the mirror a window");
    gpu->mirror_draws.clear();
    Frames(game, 5, false);
    Expect(Peer::Window() == window && !gpu->mirror_draws.empty() && gpu->mirror_draws.back().draw.width == 960 &&
               gpu->mirror_draws.back().draw.homography == PlainCrop(game.views[0].fov, settings),
           "The same window goes on at the settings' size and aspect");
    Expect(Peer::Reason().find("in a window (monitor ") != std::string::npos &&
               Peer::Reason().find("is not connected)") != std::string::npos,
           "The status says the monitor is not connected: " + Peer::Reason());
    // Back on the monitor, live.
    settings.monitor = name;
    Peer::Reconfigure(settings);
    Frames(game, 1, false);
    Expect(WaitFor([&] {
               Peer::PictureSize(width, height);
               return width == static_cast<std::uint32_t>(monitor.width);
           }),
           "Choosing a connected monitor fills it, live");
    // Fill a monitor on with none chosen: a window; no monitor is picked for the user.
    settings.monitor.clear();
    Peer::Reconfigure(settings);
    Frames(game, 1, false);
    Expect(WaitFor([&] {
               Peer::PictureSize(width, height);
               return width == 960 && height == 540;
           }),
           "With no monitor chosen the mirror is a window");
    Frames(game, 3, false);
    Expect(Peer::Reason() == "showing the left eye at 960x540 in a window (no monitor is chosen to fill)",
           "The status says no monitor is chosen: " + Peer::Reason());
}

void TestFailures() {
    GameFrame game;
    depthxr::MirrorSettings settings;
    settings.frame_rate = 120;
    const std::filesystem::path log =
        std::filesystem::temp_directory_path() / ("vectorxr-mirror-layer-" + std::to_string(GetCurrentProcessId()) + ".log");
    Peer::LogTo(log);
    FakeGpu* gpu = Peer::Start(settings);
    Frames(game, 3);
    const HWND window = Peer::Window();
    // A draw that fails now and then recovers.
    gpu->result = depthxr::MirrorDrawResult::kFailed;
    Frames(game, 10, false);
    gpu->result = depthxr::MirrorDrawResult::kShown;
    gpu->mirror_draws.clear();
    Frames(game, 3, false);
    Expect(Peer::State() == "active" && !gpu->mirror_draws.empty() && Peer::Window() == window,
           "A few failed frames are passed over");
    // Thirty in a row: the mirror gives up, lets go of the swap chain and the window.
    gpu->result = depthxr::MirrorDrawResult::kFailed;
    Frames(game, 40, false);
    Expect(Peer::State() == "failed" && Peer::Reason() == "the stand-in draw failed" && WindowGone(game.info, window) &&
               gpu->closes == 1 && gpu->window_alive_at_close.back(),
           "After 30 failed frames in a row the mirror is off for the session: " + Peer::Reason());
    gpu->result = depthxr::MirrorDrawResult::kShown;
    gpu->mirror_draws.clear();
    Frames(game, 20, false);
    Expect(gpu->mirror_draws.empty() && !Peer::WindowStarted(), "A mirror that gave up stays off");
    // A changed setting tries again.
    settings.smoothing = 10.0;
    Peer::Reconfigure(settings);
    Frames(game, 5);
    Expect(Peer::State() == "active" && !gpu->mirror_draws.empty(), "A changed setting tries again");

    // A view that names a swapchain the layer does not know.
    GameFrame unknown;
    unknown.info.displayTime = game.info.displayTime;
    unknown.views[0].subImage.swapchain = reinterpret_cast<XrSwapchain>(std::uintptr_t{0xbad});
    gpu->mirror_draws.clear();
    Frames(unknown, 40, false);
    Expect(gpu->mirror_draws.empty() && Peer::State() == "failed" &&
               Peer::Reason().find("left eye") != std::string::npos,
           "An eye whose image cannot be found fails the mirror, with the reason: " + Peer::Reason());
    // A frame without a projection layer (a loading screen) is not a failure.
    Peer::Reconfigure(depthxr::MirrorSettings{});
    game.info.displayTime = unknown.info.displayTime;
    Frames(game, 3);
    GameFrame empty;
    empty.info.displayTime = game.info.displayTime;
    empty.info.layerCount = 1;
    empty.info.layers = &empty.layers[1];
    Frames(empty, 60, false);
    XrFrameEndInfo none{XR_TYPE_FRAME_END_INFO};
    none.displayTime = empty.info.displayTime;
    Peer::Show(none);
    Expect(Peer::State() == "active" && Peer::WindowStarted(), "Frames without a projection layer are passed over");

    const std::string text = Peer::Log();
    Expect(text.find("Mirror window: opening for MirrorGame.exe (stand-in), left eye, 1280x720") != std::string::npos &&
               text.find("The headset's frame is not changed.") != std::string::npos &&
               text.find("Mirror window: showing 1000x900 of the left eye in 1280x720.") != std::string::npos &&
               text.find("Mirror window: off for this session (the stand-in draw failed).") != std::string::npos &&
               text.find("Mirror window: closed (") != std::string::npos,
           "The log says what the mirror did");
    Peer::DestroySession();
    Peer::LogTo(std::filesystem::temp_directory_path() / "vectorxr-mirror-layer-idle.log");
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
}

bool WindowGone(const XrFrameEndInfo& frame, HWND window) {
    return WaitFor([&] {
        Peer::Show(frame);
        return !Peer::WindowStarted() && !IsWindow(window);
    });
}

// The thread that ends the game's frames never waits for the window's
// thread: it may hold a lock that thread is waiting for (the game's Direct3D
// 11 device lock, while a swap chain is made). Here a task on the window's
// thread waits for a lock this thread holds, and the mirror is turned off.
void TestClosingNeverWaitsForTheWindowsThread() {
    FakeGpu* gpu = Peer::Start(depthxr::MirrorSettings{});
    GameFrame game;
    Frames(game, 3);
    const HWND window = Peer::Window();
    std::mutex device_lock; // stands in for the game's device lock
    std::atomic<bool> waiting{false}, got_it{false};
    std::unique_lock held(device_lock);
    Expect(Peer::PostToWindow([&] {
               waiting = true;
               std::scoped_lock lock(device_lock);
               got_it = true;
           }),
           "A task is posted to the window's thread");
    Expect(WaitFor([&] { return waiting.load(); }), "The window's thread waits for the lock this thread holds");
    // Turned off while this thread holds it: every frame returns at once.
    Peer::Reconfigure(depthxr::MirrorSettings{}, false);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        Peer::Show(game.info);
        game.info.displayTime += kFrame;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Expect(ms < 1000.0 && !got_it && gpu->closes == 1 && Peer::WindowStopping(),
           "Frames go on while the window's thread cannot end (" + std::to_string(ms) + " ms for 20)");
    // Turned on again meanwhile: no second window while the first is going.
    Peer::Reconfigure(depthxr::MirrorSettings{});
    Peer::Show(game.info);
    Expect(Peer::WindowStopping() && gpu->mirror_draws.size() <= 3, "No new window while the old one is going");
    held.unlock();
    Expect(WaitFor([&] {
               Peer::Show(game.info);
               game.info.displayTime += kFrame;
               return got_it.load() && !IsWindow(window) && Peer::Window() != nullptr && Peer::Window() != window;
           }),
           "Once its thread can end, the window goes and the mirror opens another");
    Frames(game, 3, false);
    Expect(Peer::State() == "active", "The mirror shows again");
}

// A task the window's thread took is always run, the window gone or not:
// what posted it waits for what it hands back (a swap chain being made, a
// Vulkan mirror's shared images), and it lets go of what it was given.
void TestPostedTasksAreAlwaysRun() {
    depthxr::MirrorWindow window;
    depthxr::MirrorWindow::Options options;
    options.title = L"VectorXR Mirror test";
    options.on_desktop = false;
    std::string error;
    Expect(window.Start(options, error), "A window starts: " + error);
    Expect(WaitFor([&] { return window.Handle() != nullptr; }), "The window is made");
    std::atomic<int> ran{0};
    std::atomic<bool> first_running{false}, let_go{false};
    auto resource = std::make_shared<int>(7);
    Expect(window.Post([&] {
               first_running = true;
               while (!let_go) std::this_thread::sleep_for(std::chrono::milliseconds(1));
               ++ran;
           }),
           "The first task is taken");
    Expect(WaitFor([&] { return first_running.load(); }), "It runs on the window's thread");
    for (int i = 0; i < 5; ++i) {
        Expect(window.Post([&ran, resource] { ++ran; }), "A task is taken while another runs");
    }
    Expect(resource.use_count() == 6, "The tasks hold what they were given");
    // The window is told to go with five tasks waiting.
    window.RequestStop();
    Expect(window.Stopping() && !window.Join(50), "Its thread is busy: asking it to stop does not wait, a join does");
    let_go = true;
    Expect(window.Join(10000) && !window.Started(), "Its thread ends");
    Expect(ran == 6, "Every task taken was run before the thread ended: " + std::to_string(ran.load()) + " of 6");
    Expect(resource.use_count() == 1, "...and let go of what it was given");
    Expect(!window.Post([&ran] { ++ran; }) && ran == 6, "With no window a task is not taken");
    // Started again, the same object makes a new window.
    Expect(window.Start(options, error) && WaitFor([&] { return window.Handle() != nullptr; }),
           "The window starts again");
    window.Stop();
    Expect(!window.Started() && window.Handle() == nullptr, "Stop asks and waits");
}

// An instance destroyed with its session alive (a game may never call
// xrDestroySession): the mirror, the screenshots and the reader stop there,
// while the device is the game's to keep. What a session leaves behind
// anyway is forgotten at the next one without a call on its device.
void TestInstanceEndStopsTheSession() {
    FakeGpu* gpu = Peer::Start(depthxr::MirrorSettings{});
    GameFrame game;
    Frames(game, 3);
    const HWND window = Peer::Window();
    Expect(IsWindow(window) && gpu->mirror_draws.size() >= 1, "The mirror shows");
    const int closes = FakeGpu::total_closes, resets = FakeGpu::total_resets, abandons = FakeGpu::total_abandons;
    Expect(Peer::EndInstance() == XR_SUCCESS, "The instance ends with its session alive");
    Expect(!IsWindow(window) && !Peer::WindowStarted(), "The mirror's window and thread are gone with the instance");
    Expect(!Peer::HasReader() && FakeGpu::total_closes == closes + 1 && FakeGpu::total_resets == resets + 1 &&
               FakeGpu::total_abandons == abandons,
           "The reader let go of its swap chain and of the device, while the device is there");

    // A reader a session left behind (its end never seen): the next session
    // forgets it without touching its device.
    gpu = Peer::Start(depthxr::MirrorSettings{});
    Frames(game, 3);
    const HWND second = Peer::Window();
    const int before_resets = FakeGpu::total_resets, before_abandons = FakeGpu::total_abandons;
    Peer::CreateSession(nullptr);
    Expect(FakeGpu::total_abandons == before_abandons + 1 && FakeGpu::total_resets == before_resets,
           "A leftover reader is abandoned, not reset through a device that may be gone");
    Expect(!IsWindow(second) && !Peer::WindowStarted(), "Its window goes too");
    Peer::DestroySession();
}

void TestUnsupportedAndSessionEnd() {
    GameFrame game;
    // No reader for this session (a Vulkan game).
    Peer::Start(depthxr::MirrorSettings{}, true, false);
    for (int i = 0; i < 5; ++i) Peer::Show(game.info);
    Expect(!Peer::WindowStarted() && Peer::State() == "unsupported" &&
               Peer::Reason() == "Vulkan images cannot be read back yet",
           "A session without a reader has no mirror, and the status says so: " + Peer::Reason());
    // A Vulkan game whose device cannot share the picture: the status says
    // what to do about it.
    Peer::SetMirrorUnavailable("the mirror was turned on after the game started: restart the game");
    for (int i = 0; i < 2; ++i) Peer::Show(game.info);
    Expect(Peer::State() == "unsupported" &&
               Peer::Reason() == "the mirror was turned on after the game started: restart the game",
           "A Vulkan game started with the mirror off says so: " + Peer::Reason());
    // A Vulkan game's reader (the mirror only): shown like any other, and
    // the game's colour swapchains are asked for the copy usage, only while
    // the mirror is on and only where it is missing.
    {
        FakeGpu* vulkan = Peer::Start(depthxr::MirrorSettings{});
        vulkan->vulkan_like = true;
        Frames(game, 3);
        Expect(Peer::State() == "active" && !vulkan->mirror_draws.empty(),
               "A Vulkan session with a reader shows the mirror: " + Peer::State());
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = kFormat;
        info.sampleCount = 1;
        info.width = 1000;
        info.height = 900;
        info.arraySize = 1;
        Expect(Peer::MirrorUsage(info) == XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT,
               "A colour swapchain is asked for the copy usage");
        info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        Expect(Peer::MirrorUsage(info) == 0, "One that has it is left as it is");
        info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        Expect(Peer::MirrorUsage(info) == 0, "A depth swapchain is left as it is");
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.sampleCount = 4;
        Expect(Peer::MirrorUsage(info) == 0, "A multisampled swapchain is left as it is");
        info.sampleCount = 1;
        Peer::Reconfigure(depthxr::MirrorSettings{}, false);
        Expect(Peer::MirrorUsage(info) == 0, "With the mirror off nothing is added to a swapchain");
        Peer::Reconfigure(depthxr::MirrorSettings{});
        vulkan->vulkan_like = false;
        Expect(Peer::MirrorUsage(info) == 0, "A Direct3D session's swapchains get nothing added");
    }
    // A reader without a mirror.
    FakeGpu* gpu = Peer::Start(depthxr::MirrorSettings{});
    gpu->supports_mirror = false;
    for (int i = 0; i < 5; ++i) Peer::Show(game.info);
    Expect(!Peer::WindowStarted() && Peer::State() == "unsupported" && gpu->mirror_draws.empty(),
           "A backend without a mirror makes no window");

    // The session's end: the swap chain first, then the window, and the next
    // session starts clean.
    gpu = Peer::Start(depthxr::MirrorSettings{});
    Frames(game, 3);
    const HWND window = Peer::Window();
    Expect(IsWindow(window), "The window stands while the session runs");
    // Peer::DestroySession frees the stand-in GPU; what it saw is copied first.
    Peer::DestroySession();
    Expect(!IsWindow(window) && !Peer::WindowStarted(), "The session's end destroys the window");
    // The session's image reader is asked for at session creation when the mirror is on.
    Peer::Start(depthxr::MirrorSettings{}, true, false);
    Peer::CreateSession(nullptr);
    Expect(Peer::ReaderAskedFor(), "With the mirror on, a new session asks for its image reader");
    Peer::DestroySession();
    Peer::Start(depthxr::MirrorSettings{}, false, false);
    Peer::CreateSession(nullptr);
    Expect(!Peer::ReaderAskedFor(), "With it off, it does not");
    Peer::DestroySession();
}

} // namespace

int main() {
    // The window's thread sees real pixels on a scaled display; so must the
    // test, to speak of the same monitor sizes.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        TestOffDoesNothing();
        TestShowsTheEye();
        TestReadsOnlyHeldImages();
        TestQuadViewsFocus();
        TestFrameIsTheSameOnAndOff();
        TestSmoothingAndPacing();
        TestHideAndShow();
        TestLiveSettings();
        TestResizingKeepsThePicture();
        TestFillsAMonitor();
        TestFailures();
        TestClosingNeverWaitsForTheWindowsThread();
        TestPostedTasksAreAlwaysRun();
        TestInstanceEndStopsTheSession();
        TestUnsupportedAndSessionEnd();
        Peer::DestroySession();
    } catch (const std::exception& failure) {
        std::cerr << "FAILED: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "Mirror window through the layer: " << checks << " checks passed\n";
    return 0;
}
