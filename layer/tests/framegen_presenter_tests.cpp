// Frame Generation pacing against a fake runtime and a fake GPU backend:
// runtime call order, display times, the overlap gate and frame demotion,
// output image ownership, capture at release, failure handling, automatic
// retry and the in-session toggle, and threaded presentation against a
// runtime that paces its waits. No graphics device is needed.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "depthxr/framegen_presenter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace depthxr;
using namespace std::chrono_literals;

namespace {

void Check(bool ok, const std::string& why) {
    if (!ok) {
        std::cerr << "FAILED: " << why << '\n';
        std::exit(1);
    }
}

constexpr XrDuration kPeriod = 11'111'111;
constexpr std::int64_t kFormat = 29;      // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
constexpr std::int64_t kFloatFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT

// --- fake swapchain dispatch --------------------------------------------------

struct SwapchainImages {
    std::deque<std::uint32_t> acquired; // FIFO, like OpenXR
    int waited{0};
    std::uint32_t cursor{0};
};

std::mutex g_xr_mutex;
std::uint64_t g_next_handle = 0x1000;
std::map<XrSwapchain, SwapchainImages> g_images;
int g_creates = 0;
int g_wait_timeouts = 0; // number of upcoming output waits that time out
bool g_release_violation = false;

XrResult XRAPI_PTR Formats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* formats) {
    *count = 1;
    if (capacity) formats[0] = kFormat;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Create(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* swapchain) {
    std::scoped_lock lock(g_xr_mutex);
    *swapchain = reinterpret_cast<XrSwapchain>(++g_next_handle);
    ++g_creates;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Destroy(XrSwapchain swapchain) {
    std::scoped_lock lock(g_xr_mutex);
    g_images.erase(swapchain);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Images(XrSwapchain, std::uint32_t, std::uint32_t* count, XrSwapchainImageBaseHeader*) {
    *count = 3;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Acquire(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    std::scoped_lock lock(g_xr_mutex);
    auto& images = g_images[swapchain];
    *index = images.cursor++ % 3;
    images.acquired.push_back(*index);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR WaitImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo*) {
    std::scoped_lock lock(g_xr_mutex);
    if (g_wait_timeouts > 0) {
        --g_wait_timeouts;
        return XR_TIMEOUT_EXPIRED;
    }
    ++g_images[swapchain].waited;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Release(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) {
    std::scoped_lock lock(g_xr_mutex);
    auto& images = g_images[swapchain];
    // Only the oldest acquired image that has been waited may be released.
    if (images.acquired.empty() || images.waited == 0) {
        g_release_violation = true;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    images.acquired.pop_front();
    --images.waited;
    return XR_SUCCESS;
}

// --- fake GPU -------------------------------------------------------------------

struct GpuLog {
    std::atomic<int> executes{0};
    std::atomic<int> captures{0};
    int wait_idles{0};
    bool fail{false};
    bool threaded{false};
    bool depth{false};
    std::atomic<int> depth_captures{0};
    std::vector<FrameGenViewSubmission> last;
    FrameGenImage last_capture;
    FrameGenTuning tuning;
};

class FakeGpu final : public FrameGenGpu {
  public:
    explicit FakeGpu(GpuLog* log) : log_(log) {}
    const char* Name() const override { return "Fake"; }
    std::optional<FrameGenOutputKind> Classify(std::int64_t format) const override {
        if (format == kFormat) return FrameGenOutputKind::kSrgb8;
        if (format == kFloatFormat) return FrameGenOutputKind::kFloat16;
        return std::nullopt;
    }
    std::vector<std::int64_t> OutputFormats(FrameGenOutputKind kind) const override {
        return {kind == FrameGenOutputKind::kFloat16 ? kFloatFormat : kFormat};
    }
    XrSwapchainUsageFlags RequiredSourceUsage() const override { return XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT; }
    XrSwapchainUsageFlags OutputUsage() const override {
        return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    }
    bool Track(XrSwapchain, PFN_xrEnumerateSwapchainImages, std::string&) override { return true; }
    void Forget(XrSwapchain) override {}
    bool Capture(std::size_t, const FrameGenImage& source, const FrameGenViewRequest&, FrameGenFlowQuality,
                 std::string&) override {
        ++log_->captures;
        log_->last_capture = source;
        return true;
    }
    bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                 FrameGenExecuteResult& result, std::string& error) override {
        ++log_->executes;
        log_->last = views;
        log_->tuning = tuning;
        if (log_->fail) {
            error = "fake GPU failure";
            return false;
        }
        history_.resize(views.size());
        result.synthesized.clear();
        for (std::size_t i = 0; i < views.size(); ++i) {
            history_[i].Configure(views[i].request.extent, views[i].source.format, views[i].request.output, 3);
            const bool synthesized = history_[i].Effective(views[i].request).synthesize;
            history_[i].Commit(synthesized);
            result.synthesized.push_back(synthesized);
        }
        return true;
    }
    bool WaitIdle() override {
        ++log_->wait_idles;
        return true;
    }
    void ResetHistory() override { history_.clear(); }
    void Reset() override {}
    double LastGpuMilliseconds() const override { return 0.75; }
    bool SupportsThreadedPresentation() const override { return log_->threaded; }
    bool SupportsDepth() const override { return log_->depth; }
    XrSwapchainUsageFlags RequiredDepthUsage() const override {
        return log_->depth ? XR_SWAPCHAIN_USAGE_SAMPLED_BIT : 0;
    }
    bool CaptureDepth(std::size_t, const FrameGenImage&, const FrameGenDepthEncoding&, const FrameGenViewRequest&,
                      std::string& error) override {
        error.clear();
        ++log_->depth_captures;
        return log_->depth;
    }

  private:
    GpuLog* log_;
    std::vector<FrameGenHistoryState> history_;
};

// --- fake runtime ------------------------------------------------------------------

// Sleeps to a deadline with sub-millisecond precision.
void SleepPrecise(std::chrono::steady_clock::time_point deadline) {
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return;
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, 1000);
    } else {
        std::this_thread::sleep_until(deadline);
    }
}

struct Submission {
    XrTime time{0};
    std::vector<XrSwapchain> swapchains;
    std::uint32_t layers{0};
};

struct Runtime {
    std::atomic<XrTime> next_display{1'000'000'000};
    // Paced: each wait blocks until the next refresh, like a compositor.
    std::atomic<bool> paced{false};
    std::chrono::steady_clock::time_point epoch{std::chrono::steady_clock::now()};
    std::int64_t last_tick{0};
    std::atomic<int> outstanding_waits{0};
    std::atomic<bool> ordering_violation{false};
    std::atomic<bool> fail_next_submit{false};
    std::atomic<bool> fail_next_wait{false};
    std::atomic<bool> used_destroyed{false};
    std::vector<XrSwapchain> destroyed; // under g_xr_mutex
    std::vector<XrSpace> destroyed_spaces; // under g_xr_mutex
    std::atomic<bool> used_destroyed_space{false};
    std::atomic<int> wait_sleep_ms{0};
    bool begun{false};
    std::vector<XrTime> submitted_times;
    std::vector<std::chrono::steady_clock::time_point> submitted_at;
    std::vector<std::vector<XrSwapchain>> submitted_swapchains;
    std::vector<bool> submitted_depth;

    FrameGenRuntimeCalls Calls() {
        FrameGenRuntimeCalls calls;
        calls.wait = [this](XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
            if (fail_next_wait.exchange(false)) return XR_ERROR_RUNTIME_FAILURE;
            if (++outstanding_waits > 1) ordering_violation = true;
            if (wait_sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(wait_sleep_ms));
            if (paced) {
                const auto period = std::chrono::nanoseconds(kPeriod);
                std::int64_t tick = (std::chrono::steady_clock::now() - epoch) / period + 1;
                tick = std::max(tick, last_tick + 1);
                last_tick = tick;
                SleepPrecise(epoch + tick * period);
                state->predictedDisplayTime = 1'000'000'000 + (tick + 1) * kPeriod;
                state->predictedDisplayPeriod = kPeriod;
                state->shouldRender = XR_TRUE;
                return XR_SUCCESS;
            }
            state->predictedDisplayTime = next_display.fetch_add(kPeriod);
            state->predictedDisplayPeriod = kPeriod;
            state->shouldRender = XR_TRUE;
            return XR_SUCCESS;
        };
        calls.begin = [this](XrSession, const XrFrameBeginInfo*) {
            if (outstanding_waits.load() != 1) ordering_violation = true;
            --outstanding_waits;
            std::scoped_lock lock(g_xr_mutex);
            if (begun) ordering_violation = true;
            begun = true;
            return XR_SUCCESS;
        };
        calls.submit = [this](XrSession, const XrFrameEndInfo* info) {
            std::scoped_lock lock(g_xr_mutex);
            if (!begun) ordering_violation = true;
            begun = false;
            if (fail_next_submit.exchange(false)) return XR_ERROR_TIME_INVALID;
            submitted_times.push_back(info->displayTime);
            submitted_at.push_back(std::chrono::steady_clock::now());
            for (std::uint32_t layer = 0; layer < info->layerCount; ++layer) {
                std::vector<XrSwapchain> used;
                XrSpace space = XR_NULL_HANDLE;
                if (info->layers[layer]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[layer]);
                    for (std::uint32_t v = 0; v < p->viewCount; ++v) used.push_back(p->views[v].subImage.swapchain);
                    space = p->space;
                } else if (info->layers[layer]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                    const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[layer]);
                    used.push_back(q->subImage.swapchain);
                    space = q->space;
                }
                if (std::find(destroyed_spaces.begin(), destroyed_spaces.end(), space) != destroyed_spaces.end()) {
                    used_destroyed_space = true;
                }
                for (const XrSwapchain swapchain : used) {
                    if (std::find(destroyed.begin(), destroyed.end(), swapchain) != destroyed.end()) used_destroyed = true;
                }
            }
            std::vector<XrSwapchain> swapchains;
            bool depth = false;
            if (info->layerCount) {
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
                for (std::uint32_t i = 0; i < projection->viewCount; ++i) {
                    swapchains.push_back(projection->views[i].subImage.swapchain);
                    depth = depth || projection->views[i].next != nullptr;
                }
            }
            submitted_swapchains.push_back(swapchains);
            submitted_depth.push_back(depth);
            log.push_back({info->displayTime, swapchains, info->layerCount});
            return XR_SUCCESS;
        };
        return calls;
    }

    std::size_t Submissions() {
        std::scoped_lock lock(g_xr_mutex);
        return submitted_times.size();
    }

    std::vector<Submission> Snapshot() {
        std::scoped_lock lock(g_xr_mutex);
        return log;
    }

    std::vector<Submission> log;
};

// --- application ------------------------------------------------------------------

const XrSwapchain kLeft = reinterpret_cast<XrSwapchain>(0x11);
const XrSwapchain kRight = reinterpret_cast<XrSwapchain>(0x12);
const XrSession kSession = reinterpret_cast<XrSession>(0x1);
const XrSpace kSpace = reinterpret_cast<XrSpace>(0x2);

struct App {
    FrameGenPresenter& presenter;
    Runtime& runtime;
    FrameGenRuntimeCalls calls;
    std::uint32_t image{0};
    XrCompositionLayerDepthInfoKHR depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerBaseHeader unknown{static_cast<XrStructureType>(1000999999)};
    std::vector<const XrCompositionLayerBaseHeader*> layers{
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    // MSFS ends each frame one period after the display time it was given.
    int end_offset_periods{0};
    std::mutex periods_mutex;
    std::map<XrTime, XrDuration> periods;

    App(FrameGenPresenter& p, Runtime& r) : presenter(p), runtime(r), calls(r.Calls()) {
        for (int eye = 0; eye < 2; ++eye) {
            auto& view = views[eye];
            view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            view.next = &depth;
            view.pose.orientation.w = 1.0f;
            view.fov = {-0.8f, 0.8f, 0.8f, -0.8f};
            view.subImage.swapchain = eye ? kRight : kLeft;
            view.subImage.imageRect = {{0, 0}, {1600, 1600}};
        }
        projection.space = kSpace;
        projection.viewCount = 2;
        projection.views = views.data();
        quad.space = kSpace;
        quad.subImage.swapchain = reinterpret_cast<XrSwapchain>(0x13);
        quad.size = {1.0f, 0.5f};
        quad.pose.orientation.w = 1.0f;
    }

    XrFrameState Wait() {
        XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        Check(XR_SUCCEEDED(presenter.WaitFrame(kSession, &info, &state, calls)), "app wait");
        std::scoped_lock lock(periods_mutex);
        periods[state.predictedDisplayTime] = state.predictedDisplayPeriod;
        if (periods.size() > 64) periods.erase(periods.begin());
        return state;
    }

    void Begin() {
        XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
        Check(XR_SUCCEEDED(presenter.BeginFrame(kSession, &info, calls)), "app begin");
    }

    // The layer's release hook: note the index, capture, then forward.
    void Release() {
        image = (image + 1) % 3;
        for (const XrSwapchain swapchain : {kLeft, kRight, depth.subImage.swapchain}) {
            if (!swapchain) continue;
            presenter.NoteRelease(swapchain, image);
            presenter.CaptureOnRelease(swapchain, image);
        }
    }

    XrResult End(XrTime display_time, bool layers = true) {
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.displayTime = display_time;
        if (end_offset_periods) {
            std::scoped_lock lock(periods_mutex);
            const auto it = periods.find(display_time);
            if (it != periods.end()) info.displayTime += it->second * end_offset_periods;
        }
        info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        info.layerCount = layers ? static_cast<std::uint32_t>(this->layers.size()) : 0;
        info.layers = layers ? this->layers.data() : nullptr;
        return presenter.EndFrame(kSession, &info, calls);
    }

    XrFrameState Frame(bool layers = true) {
        const XrFrameState state = Wait();
        Begin();
        Release();
        Check(XR_SUCCEEDED(End(state.predictedDisplayTime, layers)), "app end");
        return state;
    }
};

FrameGenSwapchainDispatch Dispatch() {
    return {Formats, Create, Destroy, Images, Acquire, WaitImage, Release};
}

constexpr XrSwapchainUsageFlags kAppUsage =
    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;

void StartPresenter(FrameGenPresenter& presenter, GpuLog& gpu, FrameGenSynthesisMode mode,
                    std::int64_t format = kFormat, XrSwapchainUsageFlags usage = kAppUsage,
                    FrameGenMultiplier multiplier = FrameGenMultiplier::k2) {
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = mode;
    settings.multiplier = multiplier;
    presenter.Configure(settings);
    presenter.SetEngagementPolicy(3, 0ms);
    presenter.Start(kSession, nullptr, Dispatch(), nullptr, nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    presenter.TrackSwapchain(kLeft, format, 1, usage);
    presenter.TrackSwapchain(kRight, format, 1, usage);
    std::scoped_lock lock(g_xr_mutex);
    g_creates = 0;
    g_wait_timeouts = 0;
    g_release_violation = false;
}

void TestInterpolationPacing() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    Check(presenter.OwnsPacing(), "presenter with a backend owns pacing");
    XrSwapchainCreateInfo color{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    color.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    XrSwapchainCreateInfo depth = color;
    depth.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    Check(presenter.SourceUsage(color) == XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT && presenter.SourceUsage(depth) == 0,
          "only color swapchains get capture usage");

    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) {
        Check(app.Frame().predictedDisplayPeriod == kPeriod, "pass-through keeps the runtime period");
    }
    Check(runtime.Submissions() == 3 && gpu.executes == 0, "no generation before engagement");
    Check(presenter.Status().state == FrameGenState::kWaiting, "waiting before engagement");
    Check(gpu.captures > 0 && gpu.last_capture.image_index == app.image && gpu.last_capture.rect.extent.width == 1600,
          "released images are captured with the last frame's view regions");

    // Engaged: each app frame owns two runtime frames.
    const XrTime slot = runtime.next_display.load();
    const XrFrameState first = app.Frame();
    Check(first.predictedDisplayPeriod == 2 * kPeriod, "engaged app sees a doubled period");
    Check(first.predictedDisplayTime == slot + kPeriod, "engaged app renders for the following slot");
    Check(runtime.Submissions() == 5, "engaged frame submits two runtime frames");
    Check(runtime.submitted_times[3] == slot && runtime.submitted_times[4] == slot + kPeriod,
          "generated frame takes slot k, the real frame slot k + 1");
    Check(gpu.executes == 1 && !gpu.last[0].request.synthesize, "first engaged frame primes history with a still");
    Check(runtime.submitted_swapchains[3][0] != kLeft && runtime.submitted_swapchains[3][1] != kRight,
          "slot k presents the layer-owned output images");
    Check(runtime.submitted_depth[3] && runtime.submitted_depth[4],
          "interpolated views carry the newest frame's depth, so the runtime corrects every frame alike");
    Check(runtime.submitted_swapchains[4][0] == kLeft && runtime.submitted_swapchains[4][1] == kRight,
          "slot k + 1 presents the application's own images");
    Check(gpu.last[0].source.image_index == app.image, "generation reads the image the app released");

    app.Frame();
    Check(gpu.executes == 2 && gpu.last[0].request.synthesize && gpu.last[1].request.synthesize,
          "continuous frames synthesize");
    Check(gpu.tuning.generated == 1 && std::abs(gpu.tuning.fractions[0] - 0.5f) < 1e-4f,
          "interpolation sits halfway between real frames");
    Check(presenter.Status().state == FrameGenState::kActive && presenter.Status().generated == 1, "active and generating");

    // A frame without layers keeps its slot and skips the extra runtime frame.
    const std::size_t before = runtime.Submissions();
    app.Frame(false);
    Check(runtime.Submissions() == before + 1, "empty engaged frame submits once");
    Check(app.Frame().predictedDisplayPeriod == kPeriod, "an empty frame drops back to pass-through");
    Check(!runtime.ordering_violation && !g_release_violation, "runtime and image calls stayed ordered");
    presenter.Stop();
    Check(!presenter.Started(), "stop releases the session");
}

void TestExtrapolationPacing() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Frame();
    Check(runtime.submitted_swapchains[3][0] == kLeft, "the first extrapolated slot repeats the real frame");
    Check(gpu.executes == 1, "extrapolation generates after the first submission");
    app.Frame();
    Check(runtime.submitted_swapchains[5][0] != kLeft, "next slot k shows the extrapolated frame");
    Check(gpu.tuning.generated == 1 && std::abs(gpu.tuning.fractions[0] - 0.5f) < 1e-4f,
          "extrapolation pushes half an app frame forward");
    // A recreated reference space must not reach the runtime through a stale
    // extrapolated layer.
    app.projection.space = reinterpret_cast<XrSpace>(0x3);
    app.Frame();
    Check(runtime.submitted_swapchains[7][0] == kLeft, "a space change repeats the real frame instead of a stale layer");
    Check(!runtime.ordering_violation && !g_release_violation, "extrapolation kept calls ordered");
}

bool IsAppImage(const std::vector<XrSwapchain>& swapchains) {
    return swapchains.size() == 2 && swapchains[0] == kLeft && swapchains[1] == kRight;
}

void TestTripleInterpolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate, kFormat, kAppUsage, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();

    // Engaged at 3x: each app frame owns three runtime frames.
    const XrTime slot = runtime.next_display.load();
    const XrFrameState first = app.Frame();
    Check(first.predictedDisplayPeriod == 3 * kPeriod, "3x: the app sees a tripled period");
    Check(first.predictedDisplayTime == slot + 2 * kPeriod, "3x: the app renders for the cycle's last slot");
    Check(runtime.Submissions() == 6, "3x: an engaged frame submits three runtime frames");
    Check(runtime.submitted_times[3] == slot && runtime.submitted_times[4] == slot + kPeriod &&
              runtime.submitted_times[5] == slot + 2 * kPeriod,
          "3x: slots k, k + 1 and k + 2 in order");
    Check(!IsAppImage(runtime.submitted_swapchains[3]) && !IsAppImage(runtime.submitted_swapchains[4]) &&
              IsAppImage(runtime.submitted_swapchains[5]),
          "3x: two generated frames, then the real frame");
    Check(runtime.submitted_swapchains[3][0] != runtime.submitted_swapchains[4][0],
          "3x: each generated frame has its own output swapchain");
    Check(g_creates == 4, "3x: two output sets of two views");

    app.Frame();
    Check(gpu.tuning.generated == 2 && std::abs(gpu.tuning.fractions[0] - 1.0f / 3.0f) < 1e-4f &&
              std::abs(gpu.tuning.fractions[1] - 2.0f / 3.0f) < 1e-4f,
          "3x: interpolation at one and two thirds");
    Check(gpu.last[0].outputs.size() == 2, "3x: one execute writes both generated frames");
    Check(presenter.Status().multiplier == 3 && !presenter.Status().automatic, "3x: status reports the multiplier");
    Check(presenter.OsdText().find("3x") != std::string::npos, "3x: the OSD shows the multiplier");
    Check(!runtime.ordering_violation && !g_release_violation, "3x: runtime and image calls stayed ordered");
}

void TestQuadExtrapolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, kFormat, kAppUsage, FrameGenMultiplier::k4);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    const XrTime slot = runtime.next_display.load();
    const XrFrameState first = app.Frame();
    Check(first.predictedDisplayPeriod == 4 * kPeriod && first.predictedDisplayTime == slot + 3 * kPeriod,
          "4x: the app renders for the fourth slot");
    Check(runtime.Submissions() == 7, "4x: an engaged frame submits four runtime frames");
    for (int i = 3; i < 7; ++i) {
        Check(IsAppImage(runtime.submitted_swapchains[i]), "4x: nothing extrapolated yet repeats the real frame");
    }
    app.Frame();
    Check(!IsAppImage(runtime.submitted_swapchains[7]) && !IsAppImage(runtime.submitted_swapchains[8]) &&
              !IsAppImage(runtime.submitted_swapchains[9]) && IsAppImage(runtime.submitted_swapchains[10]),
          "4x: three extrapolated frames, then the real frame");
    for (int i = 7; i < 11; ++i) {
        Check(runtime.submitted_times[i] == slot + (i - 3) * kPeriod, "4x: one slot per refresh");
    }
    Check(gpu.tuning.generated == 3 && std::abs(gpu.tuning.fractions[0] - 0.25f) < 1e-4f &&
              std::abs(gpu.tuning.fractions[1] - 0.5f) < 1e-4f && std::abs(gpu.tuning.fractions[2] - 0.75f) < 1e-4f,
          "4x: extrapolation one, two and three refreshes past the real frame");
    Check(!runtime.ordering_violation && !g_release_violation, "4x: runtime and image calls stayed ordered");
}

void TestOutputsKeptAcrossMultipliers() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, kFormat, kAppUsage, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    for (int i = 0; i < 6; ++i) app.Frame();
    Check(g_creates == 4, "outputs: two sets of two views at 3x");
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
    settings.multiplier = FrameGenMultiplier::k2;
    presenter.Configure(settings);
    for (int i = 0; i < 6; ++i) app.Frame();
    settings.multiplier = FrameGenMultiplier::k3;
    presenter.Configure(settings);
    for (int i = 0; i < 6; ++i) app.Frame();
    Check(g_creates == 4, "outputs: stepping down and back up reallocates nothing");
    Check(!runtime.ordering_violation && !g_release_violation, "outputs: calls stayed ordered");
}

void TestSubmissionSpacing() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime; // its waits return at once, like PiOpenXR's inside a cycle
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, kFormat, kAppUsage, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    const std::size_t first = runtime.Submissions();
    for (int i = 0; i < 4; ++i) app.Frame();
    const auto minimum = std::chrono::nanoseconds(kPeriod * 8 / 10);
    bool spaced = true;
    for (std::size_t i = first + 1; i < runtime.submitted_at.size(); ++i) {
        spaced = spaced && runtime.submitted_at[i] - runtime.submitted_at[i - 1] >= minimum;
    }
    Check(spaced, "consecutive engaged submissions are most of a refresh apart");
}

void TestAutomaticMultiplier() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, kFormat, kAppUsage, FrameGenMultiplier::kAuto);
    presenter.SetAutoPolicy(200ms, 500ms, 600ms, 300ms);
    App app(presenter, runtime);
    Check(presenter.Status().automatic && presenter.Status().multiplier == 1,
          "auto starts at full rate: an app that holds the refresh is never slowed");
    for (int i = 0; i < 20; ++i) app.Frame();
    Check(presenter.Status().full_rate && runtime.Submissions() >= 20 && gpu.executes == 0,
          "auto: a fast app passes straight through");

    // An app slower than 2x can hold steps up.
    auto run = [&](std::chrono::milliseconds duration, std::chrono::milliseconds render) {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end) {
            const XrFrameState state = app.Wait();
            app.Begin();
            std::this_thread::sleep_for(render);
            app.Release();
            Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "auto: end");
        }
    };
    run(1500ms, 30ms);
    Check(presenter.Status().multiplier >= 3, "auto: a slow app steps up to 3x or more");
    Check(app.Frame().predictedDisplayPeriod == presenter.Status().multiplier * kPeriod,
          "auto: the app's period follows the multiplier");

    // A fast app is probed back down, all the way to full rate.
    run(4000ms, 0ms);
    Check(presenter.Status().full_rate && presenter.Status().state == FrameGenState::kActive,
          "auto: an app that holds full refresh runs without generation");
    Check(app.Frame().predictedDisplayPeriod == kPeriod, "auto: full rate passes frames through");
    Check(presenter.OsdText().find("full rate") != std::string::npos, "auto: the OSD says full rate");

    // Slow again: generation comes back.
    run(1500ms, 20ms); // the fake runtime let the fast phase run thousands of frames a second
    Check(!presenter.Status().full_rate && presenter.Status().multiplier >= 2, "auto: a slower app re-engages");
    Check(!runtime.ordering_violation && !g_release_violation, "auto: calls stayed ordered through changes");
}

// --- threaded presentation --------------------------------------------------------

void StartThreaded(FrameGenPresenter& presenter, GpuLog& gpu, FrameGenMultiplier multiplier) {
    gpu.threaded = true;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, kFormat, kAppUsage, multiplier);
}

double Seconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

// MSFS-style pipelining: a main thread waits for frame N + 1 and simulates
// it while the render thread renders frame N. `between` runs on the render
// thread after each end. Returns the frames ended.
int RunPipelined(App& app, std::chrono::milliseconds duration, std::chrono::milliseconds simulate,
                 std::chrono::milliseconds render, const std::function<void(int, const XrFrameState&)>& between = {},
                 std::chrono::steady_clock::duration* longest_gap = nullptr) {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<XrFrameState> ready;
    bool done = false;
    int begun = 0;
    const auto start = std::chrono::steady_clock::now();
    std::thread main_thread([&] {
        for (int i = 0; std::chrono::steady_clock::now() - start < duration; ++i) {
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return begun >= i; });
            }
            const XrFrameState state = app.Wait();
            SleepPrecise(std::chrono::steady_clock::now() + simulate);
            std::scoped_lock lock(mutex);
            ready.push_back(state);
            cv.notify_all();
        }
        std::scoped_lock lock(mutex);
        done = true;
        cv.notify_all();
    });
    int ended = 0;
    auto last_end = std::chrono::steady_clock::now();
    if (longest_gap) *longest_gap = {};
    for (;;) {
        XrFrameState state{};
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return !ready.empty() || done; });
            if (ready.empty()) break;
            state = ready.front();
            ready.pop_front();
        }
        app.Begin();
        {
            std::scoped_lock lock(mutex);
            ++begun;
            cv.notify_all();
        }
        SleepPrecise(std::chrono::steady_clock::now() + render);
        app.Release();
        Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "pipelined: end");
        const auto now = std::chrono::steady_clock::now();
        if (longest_gap) *longest_gap = std::max(*longest_gap, now - last_end);
        last_end = now;
        if (between) between(ended, state);
        ++ended;
    }
    main_thread.join();
    return ended;
}

void CheckOrderedTimes(const std::vector<Submission>& log, const std::string& what) {
    for (std::size_t i = 1; i < log.size(); ++i) {
        Check(log[i].time > log[i - 1].time, what + ": display times only move forward");
    }
}

void TestThreadedExtrapolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    Check(!presenter.Status().threaded, "threaded: frames pass through before engagement");

    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    XrFrameState state{};
    while (std::chrono::steady_clock::now() - start < 1200ms) {
        state = app.Wait();
        app.Begin();
        std::this_thread::sleep_for(4ms);
        app.Release();
        Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "threaded: end");
        ++frames;
    }
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.state == FrameGenState::kActive, "threaded: presents from its own thread");
    Check(state.predictedDisplayPeriod == 3 * kPeriod, "threaded: the app sees a tripled period");
    const auto log = runtime.Snapshot();
    std::size_t real = 0, generated = 0;
    for (std::size_t i = first + 20; i < log.size(); ++i) {
        (IsAppImage(log[i].swapchains) ? real : generated) += 1;
    }
    const double share = static_cast<double>(real) / static_cast<double>(real + generated);
    Check(share > 0.25 && share < 0.42, "threaded: one real frame in three refreshes (" + std::to_string(share) + ")");
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const double output = static_cast<double>(log.size() - first) / elapsed;
    Check(output > refresh * 0.85, "threaded: every refresh gets a frame (" + std::to_string(output) + " fps)");
    Check(frames / elapsed > refresh / 3 * 0.8 && frames / elapsed < refresh / 3 * 1.15,
          "threaded: the app runs at a third of the refresh (" + std::to_string(frames / elapsed) + " fps)");
    CheckOrderedTimes(log, "threaded");
    Check(gpu.executes > frames / 2, "threaded: frames are generated from the real frames");
    const auto stop_start = std::chrono::steady_clock::now();
    presenter.Stop();
    Check(std::chrono::steady_clock::now() - stop_start < 500ms, "threaded: stop ends the presenter thread promptly");
    Check(!runtime.ordering_violation && !g_release_violation, "threaded: runtime and image calls stayed ordered");
}

void TestThreadedPipelinedApp() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    // Simulation and rendering each take 20+ ms but overlap, as in MSFS: the
    // app can hold 3x (33 ms) only if its next wait is not held behind the
    // current frame's generated slots.
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    const int frames = RunPipelined(app, 1500ms, 20ms, 22ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    Check(presenter.Status().threaded, "pipelined: presents from its own thread");
    Check(frames / elapsed > refresh / 3 * 0.8,
          "pipelined: the app keeps its overlap and holds a third of the refresh (" + std::to_string(frames / elapsed) +
              " fps)");
    const auto log = runtime.Snapshot();
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85, "pipelined: every refresh gets a frame");
    CheckOrderedTimes(log, "pipelined");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "pipelined: runtime and image calls stayed ordered");
}

void TestThreadedToggleAndReset() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    bool saw_threaded = false, saw_passthrough = false, saw_rethreaded = false;
    std::chrono::steady_clock::duration gap{};
    RunPipelined(app, 2000ms, 8ms, 10ms, [&](int frame, const XrFrameState& state) {
        if (frame == 20) {
            saw_threaded = presenter.Status().threaded && state.predictedDisplayPeriod == 2 * kPeriod;
            presenter.Toggle(); // off: the presenter thread hands back while frames are in flight
        } else if (frame == 40) {
            saw_passthrough = !presenter.Status().threaded && state.predictedDisplayPeriod == kPeriod;
            presenter.Toggle(); // on again
        } else if (frame == 70) {
            saw_rethreaded = presenter.Status().threaded;
        }
    }, &gap);
    Check(gap < 200ms, "toggle: leaving and re-entering threaded presentation mid-pipeline never stalls (" +
                           std::to_string(std::chrono::duration<double, std::milli>(gap).count()) + " ms)");
    Check(saw_threaded, "toggle: threaded before the toggle");
    Check(saw_passthrough, "toggle: frames pass straight through after toggling off");
    Check(saw_rethreaded, "toggle: toggling on presents from the thread again");
    CheckOrderedTimes(runtime.Snapshot(), "toggle");

    // A session state change stops the thread between frames.
    presenter.ResetFrames();
    Check(!presenter.Status().threaded, "reset: the presenter thread stops");
    for (int i = 0; i < 8; ++i) app.Frame();
    Check(presenter.Status().threaded, "reset: re-engages after the session settles");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "toggle: runtime and image calls stayed ordered");
}

void TestThreadedPipelinedDouble() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    App app(presenter, runtime);
    // Rendering takes 90% of a 2x frame. Holding early frames must not
    // inflate the predicted lead until the app falls behind.
    const auto start = std::chrono::steady_clock::now();
    const int frames = RunPipelined(app, 2500ms, 16ms, 20ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double target = 1e9 / static_cast<double>(kPeriod) / 2;
    Check(frames / elapsed > target * 0.82,
          "2x pipelined: the app holds half the refresh (" + std::to_string(frames / elapsed) + " fps)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "2x pipelined: calls stayed ordered");
}

void TestThreadedDestroyedSwapchain() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k4);
    App app(presenter, runtime);
    app.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&app.quad));
    for (int i = 0; i < 8; ++i) app.Frame();
    Check(presenter.Status().threaded, "destroy: presenting from its own thread");
    // The app destroys its quad swapchain between frames, as the layer's
    // destroy hook does: forget, then destroy.
    app.layers.pop_back();
    presenter.ForgetSwapchain(app.quad.subImage.swapchain);
    {
        std::scoped_lock lock(g_xr_mutex);
        runtime.destroyed.push_back(app.quad.subImage.swapchain);
    }
    std::this_thread::sleep_for(60ms); // the presenter thread fills slots from the older frame meanwhile
    for (int i = 0; i < 8; ++i) app.Frame();
    Check(!runtime.used_destroyed, "destroy: a destroyed swapchain is never submitted again");
    presenter.Stop();
    Check(!runtime.ordering_violation, "destroy: calls stayed ordered");
}

void TestThreadedRuntimeFailureRecovers() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    presenter.SetRetryPolicy(0ms, 0);
    App app(presenter, runtime);
    for (int i = 0; i < 8; ++i) app.Frame();
    Check(presenter.Status().threaded, "failure: presenting from its own thread");
    runtime.fail_next_wait = true;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 12; ++i) app.Frame();
    Check(std::chrono::steady_clock::now() - start < 600ms, "failure: frames keep flowing after the thread stops");
    const FrameGenStatus status = presenter.Status();
    Check(!status.threaded && status.state == FrameGenState::kPaused, "failure: a runtime error pauses and stops the thread");
    presenter.ResetFrames();
    const auto again = std::chrono::steady_clock::now();
    for (int i = 0; i < 6; ++i) app.Frame();
    Check(std::chrono::steady_clock::now() - again < 400ms, "failure: no frame left in flight holds later waits");
    Check(!runtime.ordering_violation && !g_release_violation, "failure: calls stayed ordered");
}

void TestThreadedMsfsTiming() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    app.end_offset_periods = 1;
    const auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration gap{};
    const int frames = RunPipelined(app, 2500ms, 20ms, 22ms, {}, &gap);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double target = 1e9 / static_cast<double>(kPeriod) / 3;
    Check(presenter.Status().threaded, "msfs timing: presents from its own thread");
    Check(frames / elapsed > target * 0.8,
          "msfs timing: the app holds a third of the refresh (" + std::to_string(frames / elapsed) + " fps)");
    Check(gap < 200ms, "msfs timing: no stalls");
    // Frames are told a period earlier, so their content is rendered for when
    // they are shown and the frames generated after them move forward.
    Check(gpu.tuning.fractions[0] > 0.15f,
          "msfs timing: extrapolation is not clamped away (" + std::to_string(gpu.tuning.fractions[0]) + ")");
    CheckOrderedTimes(runtime.Snapshot(), "msfs timing");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "msfs timing: calls stayed ordered");
}

void TestThreadedPauseRetryMsfsTiming() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    presenter.SetRetryPolicy(0ms, 3);
    App app(presenter, runtime);
    app.end_offset_periods = 1;
    // The runtime rejects one frame mid-run (as Pimax did after a recenter):
    // the session pauses, retries and presents from the thread again, with
    // frames in flight across each switch.
    bool rethreaded = false;
    std::chrono::steady_clock::duration gap{};
    RunPipelined(app, 3000ms, 8ms, 10ms, [&](int frame, const XrFrameState&) {
        if (frame == 30) runtime.fail_next_submit = true;
        if (frame > 120) rethreaded = rethreaded || presenter.Status().threaded;
    }, &gap);
    Check(gap < 300ms, "pause and retry: no stalls (" +
                           std::to_string(std::chrono::duration<double, std::milli>(gap).count()) + " ms)");
    Check(rethreaded, "pause and retry: presents from its own thread again after the retry");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "pause and retry: calls stayed ordered");
}

void TestThreadedDestroyedSpace() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k4);
    App app(presenter, runtime);
    for (int i = 0; i < 8; ++i) app.Frame();
    Check(presenter.Status().threaded, "space: presenting from its own thread");
    // A recenter: the app makes a new reference space and destroys the old.
    const XrSpace old_space = app.projection.space;
    app.projection.space = reinterpret_cast<XrSpace>(0x7);
    presenter.ForgetSpace(old_space);
    {
        std::scoped_lock lock(g_xr_mutex);
        runtime.destroyed_spaces.push_back(old_space);
    }
    std::this_thread::sleep_for(60ms); // the presenter thread fills slots meanwhile
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 8; ++i) app.Frame();
    Check(std::chrono::steady_clock::now() - start < 600ms, "space: frames keep flowing");
    Check(!runtime.used_destroyed_space, "space: a destroyed space is never submitted again");
    presenter.Stop();
    Check(!runtime.ordering_violation, "space: calls stayed ordered");
}

void TestThreadedKeepsOtherLayers() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    app.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&app.quad));
    for (int i = 0; i < 3; ++i) app.Frame();
    const std::size_t first = runtime.Submissions();
    for (int i = 0; i < 20; ++i) app.Frame();
    presenter.Stop();
    const auto log = runtime.Snapshot();
    bool all = true, any_generated = false, depth = true;
    {
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = first; i < log.size(); ++i) {
            all = all && log[i].layers == 2;
            any_generated = any_generated || !IsAppImage(log[i].swapchains);
            depth = depth && runtime.submitted_depth[i];
        }
    }
    Check(any_generated && all, "threaded: generated frames keep the app's quad layer");
    Check(depth, "threaded: generated frames keep the real frame's depth");
    Check(!runtime.ordering_violation, "layers: runtime calls stayed ordered");
}

void TestThreadedUnsupportedLayerPresentsInline() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    App app(presenter, runtime);
    app.layers.push_back(&app.unknown);
    for (int i = 0; i < 6; ++i) app.Frame();
    const FrameGenStatus status = presenter.Status();
    Check(!status.threaded && status.state == FrameGenState::kActive,
          "unsupported layer: generation runs inline instead of from the thread");
    Check(!runtime.ordering_violation, "unsupported layer: runtime calls stayed ordered");
}

void TestDepthCapture() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.depth = true;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate);
    const XrSwapchain depth_chain = reinterpret_cast<XrSwapchain>(0x21);
    XrSwapchainCreateInfo depth_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    depth_info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    Check(presenter.SourceUsage(depth_info) == XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
          "depth: depth swapchains get the usage depth capture needs");
    presenter.TrackSwapchain(depth_chain, 20, 1,
                             XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    App app(presenter, runtime);
    app.depth.subImage.swapchain = depth_chain; // both views share one depth chain here
    app.depth.subImage.imageRect = {{0, 0}, {1200, 1200}};
    app.depth.minDepth = 0.0f;
    app.depth.maxDepth = 1.0f;
    app.depth.nearZ = 0.1f;
    app.depth.farZ = 1000.0f;
    for (int i = 0; i < 8; ++i) app.Frame();
    Check(gpu.depth_captures > 0, "depth: released depth images are captured");
    Check(gpu.executes > 0 && gpu.last[0].request.depth && gpu.last[1].request.depth,
          "depth: generation asks for the captured depth");
    Check(!runtime.ordering_violation && !g_release_violation, "depth: calls stayed ordered");

    // Without the depth usage the image is never read.
    FrameGenPresenter plain;
    GpuLog plain_gpu;
    plain_gpu.depth = true;
    Runtime plain_runtime;
    StartPresenter(plain, plain_gpu, FrameGenSynthesisMode::kExtrapolate);
    plain.TrackSwapchain(depth_chain, 20, 1, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    App plain_app(plain, plain_runtime);
    plain_app.depth.subImage.swapchain = depth_chain;
    for (int i = 0; i < 8; ++i) plain_app.Frame();
    Check(plain_gpu.depth_captures == 0 && plain_gpu.executes > 0 && !plain_gpu.last[0].request.depth,
          "depth: a depth image without the sampling usage is left alone");
}

void TestOverlappingWaitIsHeld() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 4; ++i) app.Frame();
    // DCS-style threading: the next wait arrives while the previous frame is
    // inside its end, which itself waits on the runtime for longer than the
    // hold. A claimed frame is waited for, never demoted.
    runtime.wait_sleep_ms = 320;
    const XrFrameState state = app.Wait();
    app.Begin();
    app.Release();
    std::thread ender([&] { Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "overlapped end"); });
    std::this_thread::sleep_for(20ms);
    App other(presenter, runtime);
    const XrFrameState next = other.Wait();
    ender.join();
    runtime.wait_sleep_ms = 0;
    other.Begin();
    other.Release();
    Check(XR_SUCCEEDED(other.End(next.predictedDisplayTime)), "follow-up end");
    Check(!runtime.ordering_violation, "an early wait never reached the runtime mid-pair");
    Check(presenter.Status().state != FrameGenState::kPaused && presenter.Status().demoted == 0,
          "a slow claimed frame neither pauses nor demotes");
}

void TestUnclaimedFrameIsDemoted() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 4; ++i) app.Frame();
    // Another thread waits while this engaged frame is still rendering (a
    // hitch longer than the hold): the frame is demoted to one submission.
    const XrFrameState stalled = app.Wait();
    app.Begin();
    std::atomic<bool> waited{false};
    XrFrameState next{};
    App other(presenter, runtime);
    std::thread waiter([&] {
        next = other.Wait();
        waited = true;
    });
    std::this_thread::sleep_for(400ms);
    Check(waited.load(), "a wait held behind an unclaimed frame is released after the hold");
    const std::size_t before = runtime.Submissions();
    app.Release();
    Check(XR_SUCCEEDED(app.End(stalled.predictedDisplayTime)), "demoted frame end");
    Check(runtime.Submissions() == before + 1, "a demoted frame submits once");
    waiter.join();
    other.Begin();
    other.Release();
    Check(XR_SUCCEEDED(other.End(next.predictedDisplayTime)), "follow-up end");
    Check(!runtime.ordering_violation, "demotion kept runtime calls ordered");
    Check(presenter.Status().demoted == 1, "the demotion is counted");
}

void TestSameThreadDoubleWait() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 4; ++i) app.Frame();
    // One thread: wait N, begin N, wait N+1, end N. Holding would deadlock
    // until the timeout, so the frame is demoted at once.
    const XrFrameState first = app.Wait();
    app.Begin();
    const auto start = std::chrono::steady_clock::now();
    const XrFrameState second = app.Wait();
    Check(std::chrono::steady_clock::now() - start < 150ms, "a same-thread wait is not held");
    app.Release();
    Check(XR_SUCCEEDED(app.End(first.predictedDisplayTime)), "first end");
    app.Begin();
    app.Release();
    Check(XR_SUCCEEDED(app.End(second.predictedDisplayTime)), "second end");
    Check(!runtime.ordering_violation, "same-thread overlap kept runtime calls ordered");
}

void TestWaitThreadSeparateFromEndThread() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    // The spec allows waiting on one thread while another begins and ends
    // frames, and the waiter may wait for frame N + 1 before frame N ends.
    // That must hold the waiter, not demote the frame.
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<XrFrameState> ready;
    int begun = 0;
    constexpr int kFrames = 24;
    std::thread waiter([&] {
        for (int i = 0; i < kFrames; ++i) {
            {
                // A real runtime blocks the next wait until the previous
                // frame has begun; it may still be rendering.
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return begun >= i; });
            }
            const XrFrameState state = app.Wait();
            std::scoped_lock lock(mutex);
            ready.push_back(state);
            cv.notify_all();
        }
    });
    for (int i = 0; i < kFrames; ++i) {
        XrFrameState state{};
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return !ready.empty(); });
            state = ready.front();
            ready.pop_front();
        }
        app.Begin();
        {
            std::scoped_lock lock(mutex);
            ++begun;
            cv.notify_all();
        }
        std::this_thread::sleep_for(2ms); // render
        app.Release();
        Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "end on the render thread");
    }
    waiter.join();
    const FrameGenStatus status = presenter.Status();
    Check(status.demoted == 0 && status.state == FrameGenState::kActive && status.generated >= kFrames / 2,
          "a separate wait thread keeps generating without demotions");
    Check(!runtime.ordering_violation && !g_release_violation, "separate wait and end threads kept calls ordered");
}

void TestRefusedCaptureUsage() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    // The runtime refused TRANSFER_SRC, so the swapchains lack the usage the
    // copy needs: generation must not touch them.
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate, kFormat, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    App app(presenter, runtime);
    for (int i = 0; i < 6; ++i) app.Frame();
    Check(gpu.captures == 0 && gpu.executes == 0, "no copy from a swapchain without the copy usage");
    Check(presenter.Status().state == FrameGenState::kUnavailable, "refused copy usage makes generation unavailable");
    Check(!runtime.ordering_violation, "an unavailable session keeps calls ordered");
}

void TestOutputWaitTimeout() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 4; ++i) app.Frame();
    const int executes = gpu.executes;
    {
        std::scoped_lock lock(g_xr_mutex);
        g_wait_timeouts = 1;
    }
    app.Frame();
    Check(gpu.executes == executes, "an output that is still in use is not written");
    app.Frame();
    Check(gpu.executes == executes + 1, "the next frame retries the wait and generates");
    Check(!g_release_violation, "an image is never released before its wait succeeded");
    Check(presenter.Status().state == FrameGenState::kActive, "a timeout neither pauses nor disengages");
}

void TestOutputFormatFallback() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    // The app renders FP16, but the runtime only offers 8-bit sRGB outputs.
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate, kFloatFormat);
    App app(presenter, runtime);
    for (int i = 0; i < 6; ++i) app.Frame();
    Check(gpu.executes >= 3 && gpu.last[0].request.output == FrameGenOutputKind::kSrgb8,
          "the sRGB fallback reaches the shader so it encodes");
    Check(g_creates == 2, "fallback outputs are created once, not every frame");
}

void TestFailuresRetryAndToggle() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    presenter.SetRetryPolicy(0ms, 1);
    App app(presenter, runtime);
    for (int i = 0; i < 5; ++i) app.Frame();

    // GPU failures fall back to repeating the real frame, then pause.
    gpu.fail = true;
    for (int i = 0; i < 3; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kPaused && presenter.Status().retry_pending,
          "repeated GPU failures pause with a retry pending");
    gpu.fail = false;
    // The automatic retry rebuilds the cadence and re-engages.
    for (int i = 0; i < 5; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kActive, "the automatic retry re-engages");

    // A runtime rejection of the generated frame pauses immediately; the
    // single retry is spent, so it stays paused until the toggle.
    runtime.fail_next_submit = true;
    const XrFrameState state = app.Wait();
    app.Begin();
    app.Release();
    Check(app.End(state.predictedDisplayTime) == XR_ERROR_TIME_INVALID, "runtime error reaches the app");
    Check(presenter.Status().state == FrameGenState::kPaused && !presenter.Status().retry_pending,
          "a runtime rejection pauses; retries are bounded");
    for (int i = 0; i < 4; ++i) {
        Check(app.Frame().predictedDisplayPeriod == kPeriod, "a paused session passes through");
    }
    Check(presenter.Toggle() && presenter.Status().state == FrameGenState::kWaiting, "the toggle retries after a pause");
    for (int i = 0; i < 4; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kActive, "re-engaged after the toggle");

    Check(!presenter.Toggle() && presenter.Status().state == FrameGenState::kOff, "toggle off");
    for (int i = 0; i < 6; ++i) {
        Check(app.Frame().predictedDisplayPeriod == kPeriod, "toggled off never engages");
    }
    Check(presenter.OsdText() == "Off" && presenter.CompactOsdText() == "Off", "OSD shows off");
    Check(!runtime.ordering_violation && !g_release_violation, "calls stayed ordered through failures");
}

void TestSettingsAndSessionChanges() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 4; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kActive, "engaged");

    // Turned off in settings mid-session: frames pass through; back on resumes.
    presenter.SetConfigured(false);
    Check(presenter.Status().state == FrameGenState::kOff, "settings turn a running session off");
    for (int i = 0; i < 4; ++i) Check(app.Frame().predictedDisplayPeriod == kPeriod, "off in settings passes through");
    presenter.SetConfigured(true);
    for (int i = 0; i < 4; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kActive, "settings turn it back on without a restart");

    // An engaged frame the layer never forwards must not hold the next wait.
    const XrFrameState abandoned = app.Wait();
    app.Begin();
    presenter.AbandonFrame(abandoned.predictedDisplayTime);
    const auto start = std::chrono::steady_clock::now();
    app.Wait();
    Check(std::chrono::steady_clock::now() - start < 150ms, "an abandoned frame does not hold the next wait");
    presenter.ResetFrames();
    Check(presenter.Status().state == FrameGenState::kWaiting, "a session state change starts over");
}

void TestUnavailableWithoutBackend() {
    FrameGenPresenter presenter;
    presenter.Start(kSession, nullptr, Dispatch(), nullptr, nullptr, [](const void*, std::string& error) {
        error = "no backend for this API";
        return std::unique_ptr<FrameGenGpu>();
    });
    Check(presenter.Started() && !presenter.OwnsPacing(), "a session without a backend leaves pacing alone");
    Check(presenter.Status().state == FrameGenState::kUnavailable &&
              presenter.OsdText() == "Unavailable  /  no backend for this API",
          "unavailable reason reaches the OSD");
}

} // namespace

int main() {
    TestInterpolationPacing();
    TestExtrapolationPacing();
    TestTripleInterpolation();
    TestQuadExtrapolation();
    TestSubmissionSpacing();
    TestOutputsKeptAcrossMultipliers();
    TestDepthCapture();
    TestAutomaticMultiplier();
    TestThreadedExtrapolation();
    TestThreadedPipelinedApp();
    TestThreadedToggleAndReset();
    TestThreadedPipelinedDouble();
    TestThreadedDestroyedSwapchain();
    TestThreadedRuntimeFailureRecovers();
    TestThreadedMsfsTiming();
    TestThreadedPauseRetryMsfsTiming();
    TestThreadedDestroyedSpace();
    TestThreadedKeepsOtherLayers();
    TestThreadedUnsupportedLayerPresentsInline();
    TestOverlappingWaitIsHeld();
    TestUnclaimedFrameIsDemoted();
    TestSameThreadDoubleWait();
    TestWaitThreadSeparateFromEndThread();
    TestRefusedCaptureUsage();
    TestOutputWaitTimeout();
    TestOutputFormatFallback();
    TestFailuresRetryAndToggle();
    TestSettingsAndSessionChanges();
    TestUnavailableWithoutBackend();
    std::cout << "depthxr_framegen_presenter_tests passed\n";
    return 0;
}
