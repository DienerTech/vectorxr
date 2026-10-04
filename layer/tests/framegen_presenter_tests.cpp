// Frame Generation pacing against a fake runtime and a fake GPU backend:
// runtime call order, display times, the overlap gate, failure handling and
// the in-session toggle. No graphics device is needed.

#include "depthxr/framegen_presenter.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace depthxr;

namespace {

void Check(bool ok, const std::string& why) {
    if (!ok) {
        std::cerr << "FAILED: " << why << '\n';
        std::exit(1);
    }
}

constexpr XrDuration kPeriod = 11'111'111;
constexpr std::int64_t kFormat = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB

// --- fake swapchain dispatch --------------------------------------------------

std::mutex g_xr_mutex;
std::uint64_t g_next_handle = 0x1000;
std::map<XrSwapchain, std::uint32_t> g_acquire_cursor;
std::vector<std::string> g_events;

void Event(const std::string& event) {
    std::scoped_lock lock(g_xr_mutex);
    g_events.push_back(event);
}

XrResult XRAPI_PTR Formats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* formats) {
    *count = 1;
    if (capacity) formats[0] = kFormat;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    std::scoped_lock lock(g_xr_mutex);
    *swapchain = reinterpret_cast<XrSwapchain>(++g_next_handle);
    g_events.push_back("create " + std::to_string(info->width) + "x" + std::to_string(info->height));
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Destroy(XrSwapchain) {
    Event("destroy");
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Images(XrSwapchain, std::uint32_t, std::uint32_t* count, XrSwapchainImageBaseHeader*) {
    *count = 3;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Acquire(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    std::scoped_lock lock(g_xr_mutex);
    *index = g_acquire_cursor[swapchain]++ % 3;
    g_events.push_back("acquire");
    return XR_SUCCESS;
}
XrResult XRAPI_PTR WaitImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
XrResult XRAPI_PTR Release(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
    Event("release");
    return XR_SUCCESS;
}

// --- fake GPU -------------------------------------------------------------------

struct GpuLog {
    int executes{0};
    bool fail{false};
    std::vector<FrameGenViewSubmission> last;
    FrameGenTuning tuning;
};

class FakeGpu final : public FrameGenGpu {
  public:
    explicit FakeGpu(GpuLog* log) : log_(log) {}
    const char* Name() const override { return "Fake"; }
    std::optional<FrameGenOutputKind> Classify(std::int64_t format) const override {
        return format == kFormat ? std::optional(FrameGenOutputKind::kSrgb8) : std::nullopt;
    }
    std::vector<std::int64_t> OutputFormats(FrameGenOutputKind) const override { return {kFormat}; }
    XrSwapchainUsageFlags RequiredSourceUsage() const override { return XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT; }
    XrSwapchainUsageFlags OutputUsage() const override {
        return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    }
    bool Track(XrSwapchain, PFN_xrEnumerateSwapchainImages, std::string&) override { return true; }
    void Forget(XrSwapchain) override {}
    bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                 FrameGenExecuteResult& result, std::string& error) override {
        Event("execute");
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
    void ResetHistory() override { history_.clear(); }
    void Reset() override {}
    double LastGpuMilliseconds() const override { return 0.75; }

  private:
    GpuLog* log_;
    std::vector<FrameGenHistoryState> history_;
};

// --- fake runtime ------------------------------------------------------------------

struct Runtime {
    std::atomic<XrTime> next_display{1'000'000'000};
    std::atomic<int> outstanding_waits{0};
    std::atomic<bool> ordering_violation{false};
    std::atomic<bool> fail_next_submit{false};
    std::atomic<int> wait_sleep_ms{0};
    bool begun{false};
    std::vector<XrTime> submitted_times;
    std::vector<std::vector<XrSwapchain>> submitted_swapchains;
    std::vector<bool> submitted_depth;

    FrameGenRuntimeCalls Calls() {
        FrameGenRuntimeCalls calls;
        calls.wait = [this](XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
            if (++outstanding_waits > 1) ordering_violation = true;
            if (wait_sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(wait_sleep_ms));
            state->predictedDisplayTime = next_display.fetch_add(kPeriod);
            state->predictedDisplayPeriod = kPeriod;
            state->shouldRender = XR_TRUE;
            Event("wait");
            return XR_SUCCESS;
        };
        calls.begin = [this](XrSession, const XrFrameBeginInfo*) {
            if (outstanding_waits.load() != 1) ordering_violation = true;
            --outstanding_waits;
            {
                std::scoped_lock lock(g_xr_mutex);
                if (begun) ordering_violation = true;
                begun = true;
            }
            Event("begin");
            return XR_SUCCESS;
        };
        calls.submit = [this](XrSession, const XrFrameEndInfo* info) {
            std::scoped_lock lock(g_xr_mutex);
            if (!begun) ordering_violation = true;
            begun = false;
            g_events.push_back("submit");
            if (fail_next_submit.exchange(false)) return XR_ERROR_TIME_INVALID;
            submitted_times.push_back(info->displayTime);
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
            return XR_SUCCESS;
        };
        return calls;
    }
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
    XrBaseInStructure depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layer{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};

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
    }

    XrFrameState Wait() {
        XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        Check(XR_SUCCEEDED(presenter.WaitFrame(kSession, &info, &state, calls)), "app wait");
        return state;
    }

    void Begin() {
        XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
        Check(XR_SUCCEEDED(presenter.BeginFrame(kSession, &info, calls)), "app begin");
    }

    XrResult End(XrTime display_time, bool layers = true) {
        image = (image + 1) % 3;
        presenter.NoteRelease(kLeft, image);
        presenter.NoteRelease(kRight, image);
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.displayTime = display_time;
        info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        info.layerCount = layers ? 1 : 0;
        info.layers = layers ? &layer : nullptr;
        return presenter.EndFrame(kSession, &info, calls);
    }

    XrFrameState Frame(bool layers = true) {
        const XrFrameState state = Wait();
        Begin();
        Check(XR_SUCCEEDED(End(state.predictedDisplayTime, layers)), "app end");
        return state;
    }
};

FrameGenSwapchainDispatch Dispatch() {
    return {Formats, Create, Destroy, Images, Acquire, WaitImage, Release};
}

void StartPresenter(FrameGenPresenter& presenter, GpuLog& gpu, FrameGenSynthesisMode mode) {
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = mode;
    presenter.Configure(settings);
    presenter.SetEngagementPolicy(3, std::chrono::milliseconds(0));
    presenter.Start(kSession, nullptr, Dispatch(), nullptr, nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    presenter.TrackSwapchain(kLeft, kFormat, 1);
    presenter.TrackSwapchain(kRight, kFormat, 1);
    {
        std::scoped_lock lock(g_xr_mutex);
        g_events.clear();
    }
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
    // Until the cadence is steady the app passes straight through.
    for (int i = 0; i < 3; ++i) {
        const XrFrameState state = app.Frame();
        Check(state.predictedDisplayPeriod == kPeriod, "pass-through keeps the runtime period");
    }
    Check(runtime.submitted_times.size() == 3 && gpu.executes == 0, "no generation before engagement");
    Check(presenter.Status().state == FrameGenState::kWaiting, "waiting before engagement");

    // Engaged: each app frame owns two runtime frames.
    const XrTime slot = runtime.next_display.load();
    const XrFrameState first = app.Frame();
    Check(first.predictedDisplayPeriod == 2 * kPeriod, "engaged app sees a doubled period");
    Check(first.predictedDisplayTime == slot + kPeriod, "engaged app renders for the following slot");
    Check(runtime.submitted_times.size() == 5, "engaged frame submits two runtime frames");
    Check(runtime.submitted_times[3] == slot && runtime.submitted_times[4] == slot + kPeriod,
          "generated frame takes slot k, the real frame slot k + 1");
    Check(gpu.executes == 1 && !gpu.last[0].request.synthesize, "first engaged frame primes history with a still");
    Check(runtime.submitted_swapchains[3][0] != kLeft && runtime.submitted_swapchains[3][1] != kRight,
          "slot k presents the layer-owned output images");
    Check(!runtime.submitted_depth[3] && runtime.submitted_depth[4],
          "generated views drop depth; the real frame keeps the app's chain");
    Check(runtime.submitted_swapchains[4][0] == kLeft && runtime.submitted_swapchains[4][1] == kRight,
          "slot k + 1 presents the application's own images");

    app.Frame();
    Check(gpu.executes == 2 && gpu.last[0].request.synthesize && gpu.last[1].request.synthesize,
          "continuous frames synthesize");
    Check(std::abs(gpu.tuning.fraction - 0.5f) < 1e-4f, "interpolation sits halfway between real frames");
    Check(presenter.Status().state == FrameGenState::kActive && presenter.Status().generated == 1, "active and generating");
    Check(!runtime.ordering_violation, "runtime frame calls stayed ordered");

    // A frame without layers keeps its slot and skips the extra runtime frame.
    const std::size_t before = runtime.submitted_times.size();
    app.Frame(false);
    Check(runtime.submitted_times.size() == before + 1, "empty engaged frame submits once");
    // The broken cadence disengages until the policy's streak is met again.
    const XrFrameState after = app.Frame();
    Check(after.predictedDisplayPeriod == kPeriod, "an empty frame drops back to pass-through");
    Check(!runtime.ordering_violation, "runtime frame calls stayed ordered after an empty frame");
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
    Check(std::abs(gpu.tuning.fraction - 0.5f) < 1e-4f, "extrapolation pushes half an app frame forward");
    Check(gpu.tuning.mode == FrameGenSynthesisMode::kExtrapolate, "mode reaches the backend");
    Check(!runtime.ordering_violation, "extrapolation kept runtime calls ordered");
}

void TestOverlappingWaitIsHeld() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 4; ++i) app.Frame();
    // DCS-style threading: the next wait arrives while the previous frame is
    // still inside its end (which itself waits on the runtime).
    runtime.wait_sleep_ms = 5;
    const XrFrameState state = app.Wait();
    app.Begin();
    std::thread ender([&] { Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "overlapped end"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    App other(presenter, runtime);
    const XrFrameState next = other.Wait();
    ender.join();
    other.Begin();
    Check(XR_SUCCEEDED(other.End(next.predictedDisplayTime)), "follow-up end");
    Check(!runtime.ordering_violation, "an early wait never reached the runtime mid-pair");
    Check(presenter.Status().state != FrameGenState::kPaused, "a short overlap does not pause");
}

void TestFailuresAndToggle() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 5; ++i) app.Frame();

    // GPU failures fall back to repeating the real frame, then pause.
    gpu.fail = true;
    for (int i = 0; i < 3; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kPaused, "repeated GPU failures pause");
    const XrFrameState paused = app.Frame();
    Check(paused.predictedDisplayPeriod == kPeriod, "a paused session passes through");
    gpu.fail = false;
    Check(presenter.Toggle() && presenter.Status().state == FrameGenState::kWaiting, "the toggle retries after a pause");
    for (int i = 0; i < 4; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kActive, "re-engaged after retry");

    // A runtime rejection of the generated frame pauses immediately.
    runtime.fail_next_submit = true;
    const XrFrameState state = app.Wait();
    app.Begin();
    Check(app.End(state.predictedDisplayTime) == XR_ERROR_TIME_INVALID, "runtime error reaches the app");
    Check(presenter.Status().state == FrameGenState::kPaused, "a runtime rejection pauses");

    // Toggling off is a clean pass-through.
    Check(presenter.Toggle(), "retry");
    Check(!presenter.Toggle() && presenter.Status().state == FrameGenState::kOff, "toggle off");
    for (int i = 0; i < 6; ++i) {
        Check(app.Frame().predictedDisplayPeriod == kPeriod, "toggled off never engages");
    }
    Check(presenter.OsdText() == "Off" && presenter.CompactOsdText() == "Off", "OSD shows off");
    Check(!runtime.ordering_violation, "runtime calls stayed ordered through failures");
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
    TestOverlappingWaitIsHeld();
    TestFailuresAndToggle();
    TestUnavailableWithoutBackend();
    std::cout << "depthxr_framegen_presenter_tests passed\n";
    return 0;
}
