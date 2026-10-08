// Frame Generation pacing against a fake runtime and a fake GPU backend:
// runtime call order, display times, the overlap gate and frame demotion,
// output image ownership, capture at release, failure handling, automatic
// retry and the in-session toggle, threaded presentation against a runtime
// that paces its waits, Dynamic standing by and its benefit guard, and
// GPU-aware pacing against a simulated GPU timeline. No graphics device is
// needed.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "depthxr/framegen_presenter.h"
#include "depthxr/logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace depthxr;
using namespace std::chrono_literals;

namespace {

void Check(bool ok, const std::string& why) {
    if (!ok) {
        std::cerr << "FAILED: " << why << std::endl;
        // Without running destructors: the presenter thread may still be using the fakes' globals, and
        // tearing them down under it crashed the run instead of failing it.
        std::_Exit(1);
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
std::vector<XrSwapchain> g_destroyed; // swapchains the layer destroyed
int g_wait_timeouts = 0; // number of upcoming output waits that time out
bool g_release_violation = false;
std::map<XrSwapchain, int> g_releases; // images released per swapchain
// What the fake GPU drew into output images, as the eye position (x) the
// image was generated for: written ones in acquisition order, and the
// newest released one, which the runtime shows (NaN: released unwritten).
std::map<XrSwapchain, std::deque<float>> g_written;
std::map<XrSwapchain, float> g_shown;

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
    g_destroyed.push_back(swapchain);
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
    ++g_releases[swapchain];
    auto& written = g_written[swapchain];
    g_shown[swapchain] = written.empty() ? std::numeric_limits<float>::quiet_NaN() : written.front();
    if (!written.empty()) written.pop_front();
    return XR_SUCCESS;
}

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

// --- fake GPU -------------------------------------------------------------------

using TimePoint = std::chrono::steady_clock::time_point;

struct GpuLog {
    std::atomic<int> executes{0};
    std::atomic<int> captures{0};
    int wait_idles{0};
    bool fail{false};
    bool uncaptured{false}; // views not captured at release: nothing generated
    bool threaded{false};
    bool depth{false};
    std::atomic<bool> memory_pressure{false}; // process video memory near its budget
    std::atomic<int> depth_captures{0};
    std::vector<FrameGenViewSubmission> last;
    FrameGenImage last_capture;
    FrameGenTuning tuning;
    // Just-in-time synthesis (SupportsJustInTime): frames written one at a
    // time (analyzed frame, mode, fraction, when), in order.
    bool jit{false};
    std::atomic<int> analyzes{0};
    struct Synthesis {
        std::uint64_t frame{0};
        FrameGenSynthesisMode mode{FrameGenSynthesisMode::kInterpolate};
        float fraction{1.0f};
        TimePoint at{};
    };
    std::mutex syntheses_mutex;
    std::vector<Synthesis> syntheses;
    std::atomic<int> synthesis_failures{0};

    // The GPU timeline, zero by default (everything has run at once). A
    // released image is captured `capture_lag` after its release, or, with
    // `game_gpu`, once its game frame has run: each game frame (GameFrame)
    // takes that long after the one before, and with `frames_ahead` the game
    // is held while that many of its frames are queued (a driver's frame
    // latency). Motion is measured `measure_lag` after the newest capture an
    // analysis waits for (`ofa` of it on the optical flow hardware, one
    // analysis at a time). `hold_captures`: no capture ever reports done.
    std::chrono::microseconds capture_lag{0};
    std::chrono::microseconds game_gpu{0};
    int frames_ahead{0};
    std::chrono::microseconds measure_lag{0};
    std::chrono::microseconds ofa{0};
    std::atomic<bool> hold_captures{false};
    // Frames written while their real frame was not captured, and moved
    // frames written before their motion was measured: neither may happen.
    std::atomic<int> uncaptured_writes{0};
    std::atomic<int> unmeasured_moves{0};
    // Analyses whose source images were not captured at their release.
    std::atomic<int> execute_uncaptured{0};
    // Game waits held for the GPU: (mark, the newest begun frame's mark
    // then, as the test's App recorded it), and how many timed out.
    std::mutex waits_mutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> capture_waits;
    std::atomic<int> capture_wait_timeouts{0};
    std::atomic<std::uint64_t> newest_begin_mark{0};
    // Captures from release to done (game frame timeline), and the most game
    // frames queued on the GPU at once.
    std::atomic<int> max_queued{0};
    std::mutex timeline_mutex;
    TimePoint gpu_free{};
    std::deque<TimePoint> queued; // game frames not done yet
    TimePoint newest_done{};      // the newest game frame's
    double capture_latency_sum_ms{0.0};
    int capture_latency_count{0};

    // The game submits a frame's GPU work: call before its views release.
    void GameFrame() {
        if (game_gpu.count() == 0) return;
        std::unique_lock lock(timeline_mutex);
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            while (!queued.empty() && queued.front() <= now) queued.pop_front();
            if (frames_ahead <= 0 || static_cast<int>(queued.size()) < frames_ahead) break;
            const TimePoint until = queued.front();
            lock.unlock();
            SleepPrecise(until);
            lock.lock();
        }
        const auto now = std::chrono::steady_clock::now();
        gpu_free = std::max(now, gpu_free) + game_gpu;
        queued.push_back(gpu_free);
        newest_done = gpu_free;
        max_queued = std::max<int>(max_queued, static_cast<int>(queued.size()));
    }

    double MeanCaptureLatencyMs() {
        std::scoped_lock lock(timeline_mutex);
        return capture_latency_count ? capture_latency_sum_ms / capture_latency_count : 0.0;
    }
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
    bool Capture(std::size_t view, const FrameGenImage& source, const FrameGenViewRequest&, FrameGenFlowQuality,
                 std::string&) override {
        ++log_->captures;
        log_->last_capture = source;
        const TimePoint now = std::chrono::steady_clock::now();
        TimePoint done = now + log_->capture_lag;
        if (log_->game_gpu.count()) {
            std::scoped_lock lock(log_->timeline_mutex);
            done = std::max(now, log_->newest_done);
            log_->capture_latency_sum_ms += std::chrono::duration<double, std::milli>(done - now).count();
            ++log_->capture_latency_count;
        }
        std::scoped_lock lock(marks_mutex_);
        marks_.push_back({++mark_, done});
        while (marks_.size() > 256) marks_.pop_front();
        captured_[view] = {source.swapchain, source.image_index};
        return true;
    }
    std::uint64_t CaptureMark() const override {
        std::scoped_lock lock(marks_mutex_);
        return mark_;
    }
    bool CaptureReached(std::uint64_t mark) const override {
        return !log_->hold_captures && std::chrono::steady_clock::now() >= MarkDone(mark);
    }
    bool WaitCapture(std::uint64_t mark, std::chrono::milliseconds timeout) const override {
        {
            std::scoped_lock lock(log_->waits_mutex);
            log_->capture_waits.push_back({mark, log_->newest_begin_mark.load()});
        }
        const TimePoint deadline = std::chrono::steady_clock::now() + timeout;
        const TimePoint done = MarkDone(mark);
        if (log_->hold_captures || done > deadline) {
            SleepPrecise(deadline);
            ++log_->capture_wait_timeouts;
            return false;
        }
        SleepPrecise(done);
        return true;
    }
    std::vector<FrameGenAnalysisTiming> TakeTimings() override {
        const TimePoint now = std::chrono::steady_clock::now();
        std::vector<FrameGenAnalysisTiming> finished;
        while (!timings_.empty() && timings_.front().end <= now) {
            finished.push_back(timings_.front());
            timings_.pop_front();
        }
        return finished;
    }
    bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                 FrameGenExecuteResult& result, std::string& error) override {
        ++log_->executes;
        log_->last = views;
        log_->tuning = tuning;
        result.submission = ++submission_;
        // The newest capture is when the analysis can start; its views were
        // captured at their release, or are read too late.
        TimePoint captured_at = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock(marks_mutex_);
            if (!marks_.empty()) captured_at = std::max(captured_at, marks_.back().done);
            for (std::size_t i = 0; i < views.size(); ++i) {
                const auto it = captured_.find(i);
                if (it == captured_.end() || it->second.first != views[i].source.swapchain ||
                    it->second.second != views[i].source.image_index) {
                    ++log_->execute_uncaptured;
                    break;
                }
            }
        }
        if (log_->fail) {
            error = "fake GPU failure";
            return false;
        }
        if (log_->uncaptured) {
            result.uncaptured = true;
            error = "not captured";
            return false;
        }
        history_.resize(views.size());
        result.synthesized.clear();
        result.depth.clear();
        Analyzed analyzed;
        for (std::size_t i = 0; i < views.size(); ++i) {
            history_[i].Configure(views[i].request.extent, views[i].source.format, views[i].request.output, 3,
                                  tuning.analyze_only ? 3 : 2);
            const bool synthesized = history_[i].Effective(views[i].request).synthesize;
            history_[i].Commit(synthesized);
            result.synthesized.push_back(synthesized);
            result.depth.push_back(synthesized && views[i].request.depth && log_->depth);
            if (tuning.analyze_only) {
                // Kept for Synthesize: where the eye is and how it moves.
                analyzed.views.push_back({views[i].request.newest.position[0],
                                          result.depth.back() ? views[i].request.shift[0] : 0.0f, synthesized});
                continue;
            }
            // Each output shows the scene from the eye position of its time.
            std::scoped_lock lock(g_xr_mutex);
            for (std::size_t j = 0; j < views[i].outputs.size(); ++j) {
                const float fraction = tuning.fractions[j];
                const float time = tuning.mode == FrameGenSynthesisMode::kExtrapolate ? fraction : 1.0f - fraction;
                g_written[views[i].outputs[j].swapchain].push_back(
                    views[i].request.newest.position[0] + (result.depth.back() ? time * views[i].request.shift[0] : 0.0f));
            }
        }
        if (tuning.analyze_only) {
            ++log_->analyzes;
            analyzed.id = next_frame_++;
            // On the optical flow hardware one analysis at a time, then the
            // rest of the measurement.
            const TimePoint start = std::max(captured_at, ofa_free_);
            const TimePoint hardware_end = start + log_->ofa;
            ofa_free_ = hardware_end;
            analyzed.captured_at = captured_at;
            analyzed.measured_at = hardware_end + log_->measure_lag;
            FrameGenAnalysisTiming timing;
            timing.submission = result.submission;
            timing.capture_end = captured_at;
            timing.start = start;
            if (log_->ofa.count()) {
                timing.hardware_start = start;
                timing.hardware_end = hardware_end;
            }
            timing.end = analyzed.measured_at;
            timings_.push_back(timing);
            result.frame = analyzed.id;
            std::scoped_lock lock(analyzed_mutex_);
            analyzed_.push_back(analyzed);
            while (analyzed_.size() > 3) analyzed_.pop_front();
        }
        return true;
    }
    bool SupportsJustInTime() const override { return log_->jit; }
    bool Captured(std::uint64_t frame) const override {
        std::scoped_lock lock(analyzed_mutex_);
        const auto it = std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == frame; });
        return it == analyzed_.end() || std::chrono::steady_clock::now() >= it->captured_at;
    }
    bool Measured(std::uint64_t frame) const override {
        std::scoped_lock lock(analyzed_mutex_);
        const auto it = std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == frame; });
        return it == analyzed_.end() || std::chrono::steady_clock::now() >= it->measured_at;
    }
    bool Synthesize(std::uint64_t frame, FrameGenSynthesisMode mode, float fraction, const std::vector<FrameGenImage>& outputs,
                    std::string& error) override {
        Analyzed analyzed;
        const TimePoint now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock(analyzed_mutex_);
            const auto it = std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == frame; });
            if (it == analyzed_.end() || outputs.size() < it->views.size()) {
                ++log_->synthesis_failures;
                error = "the real frame is no longer kept";
                return false;
            }
            const bool moved = mode == FrameGenSynthesisMode::kExtrapolate ? fraction > 0.0f : fraction < 1.0f;
            if (now < it->captured_at) {
                ++log_->uncaptured_writes;
            } else if (moved && now < it->measured_at) {
                ++log_->unmeasured_moves;
            }
            analyzed = *it;
        }
        {
            std::scoped_lock lock(log_->syntheses_mutex);
            log_->syntheses.push_back({frame, mode, fraction, now});
        }
        // Seen from the eye's position at the frame's time, as the presenter
        // places the views (where depth is used).
        const float eye_time = mode == FrameGenSynthesisMode::kExtrapolate ? fraction : fraction - 1.0f;
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = 0; i < analyzed.views.size(); ++i) {
            const auto& view = analyzed.views[i];
            g_written[outputs[i].swapchain].push_back(view.x + (view.synthesized ? eye_time * view.shift : 0.0f));
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
    std::optional<FrameGenMemoryBudget> MemoryBudget() const override {
        constexpr std::uint64_t gigabyte = 1024ull * 1024 * 1024;
        return FrameGenMemoryBudget{log_->memory_pressure ? 31 * gigabyte + gigabyte / 2 : 16 * gigabyte, 32 * gigabyte};
    }
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
    struct Analyzed {
        std::uint64_t id{0};
        TimePoint captured_at{}; // its newest capture has run
        TimePoint measured_at{}; // its motion is measured
        struct View {
            float x{0.0f};
            float shift{0.0f};
            bool synthesized{false};
        };
        std::vector<View> views;
    };
    struct Mark {
        std::uint64_t id{0};
        TimePoint done{};
    };
    // When every capture up to `mark` has run ({}: long ago).
    TimePoint MarkDone(std::uint64_t mark) const {
        std::scoped_lock lock(marks_mutex_);
        TimePoint done{};
        for (const Mark& entry : marks_) {
            if (entry.id <= mark) done = std::max(done, entry.done);
        }
        return done;
    }
    GpuLog* log_;
    std::vector<FrameGenHistoryState> history_;
    mutable std::mutex analyzed_mutex_;
    std::deque<Analyzed> analyzed_;
    std::uint64_t next_frame_{1};
    std::uint64_t submission_{0};
    TimePoint ofa_free_{};
    std::deque<FrameGenAnalysisTiming> timings_;
    mutable std::mutex marks_mutex_;
    std::deque<Mark> marks_;
    std::uint64_t mark_{0};
    std::map<std::size_t, std::pair<XrSwapchain, std::uint32_t>> captured_; // per view: the image last captured
};

// --- fake runtime ------------------------------------------------------------------

struct Submission {
    XrTime time{0};
    std::vector<XrSwapchain> swapchains;
    std::uint32_t layers{0};
    // Per swapchain, how many images it had released: the image shown.
    std::vector<int> releases;
};

struct Runtime {
    std::atomic<XrTime> next_display{1'000'000'000};
    // Paced: each wait blocks until the next refresh, like a compositor.
    std::atomic<bool> paced{false};
    XrDuration period{kPeriod}; // the refresh period (set before the first wait)
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
    // Submissions showing an output image generated for another eye position
    // than the views submitted with it (another frame's, or none).
    std::atomic<int> mismatched_images{0};
    std::atomic<int> wait_sleep_ms{0};
    bool begun{false};
    std::vector<XrTime> submitted_times;
    std::vector<std::chrono::steady_clock::time_point> submitted_at;
    std::vector<std::vector<XrSwapchain>> submitted_swapchains;
    std::vector<bool> submitted_depth;
    std::vector<float> submitted_left_x; // the first projection view's position

    FrameGenRuntimeCalls Calls() {
        FrameGenRuntimeCalls calls;
        calls.wait = [this](XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
            if (fail_next_wait.exchange(false)) return XR_ERROR_RUNTIME_FAILURE;
            if (++outstanding_waits > 1) ordering_violation = true;
            if (wait_sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(wait_sleep_ms));
            if (paced) {
                const auto refresh = std::chrono::nanoseconds(period);
                std::int64_t tick = (std::chrono::steady_clock::now() - epoch) / refresh + 1;
                tick = std::max(tick, last_tick + 1);
                last_tick = tick;
                SleepPrecise(epoch + tick * refresh);
                state->predictedDisplayTime = 1'000'000'000 + (tick + 1) * period;
                state->predictedDisplayPeriod = period;
                state->shouldRender = XR_TRUE;
                return XR_SUCCESS;
            }
            state->predictedDisplayTime = next_display.fetch_add(period);
            state->predictedDisplayPeriod = period;
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
                    if (std::find(destroyed.begin(), destroyed.end(), swapchain) != destroyed.end() ||
                        std::find(g_destroyed.begin(), g_destroyed.end(), swapchain) != g_destroyed.end()) {
                        used_destroyed = true;
                    }
                }
            }
            std::vector<XrSwapchain> swapchains;
            std::vector<int> releases;
            bool depth = false;
            float left_x = 0.0f;
            if (info->layerCount) {
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
                for (std::uint32_t i = 0; i < projection->viewCount; ++i) {
                    swapchains.push_back(projection->views[i].subImage.swapchain);
                    releases.push_back(g_releases[projection->views[i].subImage.swapchain]);
                    depth = depth || projection->views[i].next != nullptr;
                    const auto shown = g_shown.find(projection->views[i].subImage.swapchain);
                    if (shown != g_shown.end() && !(std::abs(shown->second - projection->views[i].pose.position.x) < 1e-5f)) {
                        ++mismatched_images;
                    }
                }
                left_x = projection->viewCount ? projection->views[0].pose.position.x : 0.0f;
            }
            submitted_swapchains.push_back(swapchains);
            submitted_depth.push_back(depth);
            submitted_left_x.push_back(left_x);
            log.push_back({info->displayTime, swapchains, info->layerCount, releases});
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
    // The eyes move sideways at this many m/s: each frame is rendered from
    // where they are at the display time it was given.
    double eye_speed{0.0};
    std::mutex periods_mutex;
    std::map<XrTime, XrDuration> periods;
    // Run before each begin and each release: the game's own GPU work
    // (GpuLog::GameFrame) and what a test records around it.
    std::function<void()> before_begin;
    std::function<void()> before_release;

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
        if (before_begin) before_begin();
        XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
        Check(XR_SUCCEEDED(presenter.BeginFrame(kSession, &info, calls)), "app begin");
    }

    // The layer's release hook: note the index, capture, then forward.
    void Release() {
        if (before_release) before_release();
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
        for (auto& view : views) {
            view.pose.position.x = static_cast<float>(eye_speed * static_cast<double>(display_time - 1'000'000'000) * 1e-9);
        }
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

// Dynamic engages after a short run below the refresh rate; the benefit
// guard and probes keep their own times unless a test shortens them.
FrameGenDynamicPolicy QuickDynamic() {
    FrameGenDynamicPolicy policy;
    policy.engage_after = 300ms;
    policy.settle = 250ms;
    return policy;
}

// Dynamic with every decision quick: the guard judges 300 ms windows, a
// stand-down holds 600 ms; probes stay off.
FrameGenDynamicPolicy GuardDynamic() {
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.settle = 200ms;
    policy.guard_window = 300ms;
    policy.hold = 600ms;
    policy.hold_max = 2400ms;
    policy.probe_after = 600s;
    policy.probe_max = 600s;
    return policy;
}

void StartPresenter(FrameGenPresenter& presenter, GpuLog& gpu, FrameGenSynthesisMode mode,
                    std::int64_t format = kFormat, XrSwapchainUsageFlags usage = kAppUsage,
                    FrameGenMultiplier multiplier = FrameGenMultiplier::k2) {
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = mode;
    settings.multiplier = multiplier;
    presenter.Configure(settings);
    presenter.SetEngagementPolicy(3, 0ms);
    presenter.SetDynamicPolicy(QuickDynamic());
    presenter.Start(kSession, nullptr, Dispatch(), nullptr, nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    presenter.TrackSwapchain(kLeft, format, 1, usage);
    presenter.TrackSwapchain(kRight, format, 1, usage);
    std::scoped_lock lock(g_xr_mutex);
    g_creates = 0;
    g_destroyed.clear();
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
    Check(gpu.captures == 0, "frames passed straight through are not captured (" + std::to_string(gpu.captures.load()) + ")");

    // Engaged: each app frame owns two runtime frames.
    const XrTime slot = runtime.next_display.load();
    const XrFrameState first = app.Frame();
    Check(gpu.captures == 2 && gpu.last_capture.image_index == app.image && gpu.last_capture.rect.extent.width == 1600,
          "the first engaged frame's released images are captured with the last frame's view regions");
    Check(first.predictedDisplayPeriod == 2 * kPeriod, "engaged app sees a doubled period");
    Check(first.predictedDisplayTime == slot + kPeriod, "engaged app renders for the following slot");
    Check(runtime.Submissions() == 5, "engaged frame submits two runtime frames");
    Check(runtime.submitted_times[3] == slot && runtime.submitted_times[4] == slot + kPeriod,
          "generated frame takes slot k, the real frame slot k + 1");
    Check(gpu.executes == 1 && !gpu.last[0].request.synthesize, "first engaged frame primes history with a still");
    Check(runtime.submitted_swapchains[3][0] != kLeft && runtime.submitted_swapchains[3][1] != kRight,
          "slot k presents the layer-owned output images");
    Check(runtime.submitted_depth[3] && runtime.submitted_depth[4],
          "the newest frame and an unchanged copy of it (the priming still) carry its depth");
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
    Check(gpu.captures == 0, "auto: nothing is captured at full rate (" + std::to_string(gpu.captures.load()) + ")");

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

// Threaded tests run twice: generated frames written together after each
// real frame, then each written just in time (FrameGenGpu::Synthesize).
bool g_jit = false;

void StartThreaded(FrameGenPresenter& presenter, GpuLog& gpu, FrameGenMultiplier multiplier) {
    gpu.threaded = true;
    gpu.jit = g_jit;
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
                 std::chrono::steady_clock::duration* longest_gap = nullptr,
                 std::chrono::milliseconds faster_every_other = 0ms) {
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
        SleepPrecise(std::chrono::steady_clock::now() + render - (ended % 2 ? faster_every_other : 0ms));
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
        Check(log[i].time > log[i - 1].time,
              what + ": display times only move forward (submission " + std::to_string(i) + " of " +
                  std::to_string(log.size()) + ": " + std::to_string(log[i - 1].time) + " then " +
                  std::to_string(log[i].time) + (IsAppImage(log[i - 1].swapchains) ? ", real" : ", generated") +
                  (IsAppImage(log[i].swapchains) ? " then real)" : " then generated)"));
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
    Check(status.distinct_fps > 0.8 * status.output_fps && status.distinct_fps <= status.output_fps + 1.0,
          "threaded: a steady app shows a new frame each refresh (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps)");
    Check(gpu.executes > frames / 2, "threaded: frames are generated from the real frames");
    const auto stop_start = std::chrono::steady_clock::now();
    presenter.Stop();
    Check(std::chrono::steady_clock::now() - stop_start < 500ms, "threaded: stop ends the presenter thread promptly");
    Check(!runtime.ordering_violation && !g_release_violation, "threaded: runtime and image calls stayed ordered");
}

// Dynamic: an MSFS-like pipelined game that could do about 45 fps is not held
// at 30 (3x) or 45 with misses (2x); it runs at its own rate, varying frame to
// frame, and generated frames fill every refresh between its frames.
void TestThreadedDynamic() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::kDynamic);
    App app(presenter, runtime);
    app.end_offset_periods = 1;
    // Standing by first: a game at about half the refresh rate engages
    // after a short run (QuickDynamic).
    RunPipelined(app, 900ms, 16ms, 26ms, {}, nullptr, 8ms);
    Check(presenter.Status().threaded && !presenter.Status().standby, "dynamic: engages below the refresh rate");
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration gap{};
    int max_generated = 0;
    // Render alternates 26 and 18 ms: refreshes per real frame alternate too.
    const int frames = RunPipelined(
        app, 1500ms, 16ms, 26ms,
        [&](int frame, const XrFrameState& state) {
            max_generated = std::max<int>(max_generated, gpu.tuning.generated);
            if (frame >= 10) {
                Check(state.predictedDisplayPeriod % kPeriod == 0 && state.predictedDisplayPeriod >= 2 * kPeriod &&
                          state.predictedDisplayPeriod <= 4 * kPeriod,
                      "dynamic: the app is told a whole number of refreshes per frame (" +
                          std::to_string(state.predictedDisplayPeriod) + ")");
            }
        },
        &gap, 8ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.dynamic && status.state == FrameGenState::kActive,
          "dynamic: presents from its own thread");
    Check(presenter.OsdText().find("Dynamic") != std::string::npos, "dynamic: the OSD says Dynamic");
    Check(frames / elapsed > refresh / 3 * 1.15,
          "dynamic: the app is not held to a third of the refresh (" + std::to_string(frames / elapsed) + " fps)");
    const auto log = runtime.Snapshot();
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85,
          "dynamic: every refresh gets a frame (" + std::to_string((log.size() - first) / elapsed) + " fps)");
    std::size_t real = 0, generated = 0;
    for (std::size_t i = first + 20; i < log.size(); ++i) {
        (IsAppImage(log[i].swapchains) ? real : generated) += 1;
    }
    const double share = static_cast<double>(real) / static_cast<double>(real + generated);
    Check(share > 0.36 && share < 0.6, "dynamic: real frames fill the refreshes they reach (" + std::to_string(share) + ")");
    Check(status.distinct_fps > 0.8 * status.output_fps,
          "dynamic: refreshes show new frames (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps)");
    Check(gpu.executes > frames / 2, "dynamic: frames are generated from the real frames");
    Check(max_generated >= 1 && max_generated <= 3,
          "dynamic: at most two generated frames and a spare per real frame (" + std::to_string(max_generated) + ")");
    Check(gap < 200ms, "dynamic: no stalls");
    CheckOrderedTimes(log, "dynamic");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "dynamic: calls stayed ordered");
}

// Dynamic near the video memory budget: the third set of output images
// (a full headset-resolution swapchain per eye) is freed, not just left
// unused, and nothing submits it afterwards.
void TestDynamicFreesOutputsNearBudget() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::kDynamic);
    App app(presenter, runtime);
    // About 31 fps: standing by at first, then three refreshes per real
    // frame, two generated frames and a spare.
    RunPipelined(app, 900ms, 26ms, 32ms);
    Check(presenter.Status().threaded && !presenter.Status().standby, "memory: dynamic engaged");
    RunPipelined(app, 1200ms, 26ms, 32ms);
    int creates = 0;
    {
        std::scoped_lock lock(g_xr_mutex);
        creates = g_creates;
    }
    Check(creates == 6, "memory: three sets of two views at a third of the refresh (" + std::to_string(creates) + ")");
    gpu.memory_pressure = true;
    const auto start = std::chrono::steady_clock::now();
    const std::size_t first = runtime.Submissions();
    RunPipelined(app, 1500ms, 26ms, 32ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    std::size_t destroyed = 0;
    {
        std::scoped_lock lock(g_xr_mutex);
        destroyed = g_destroyed.size();
        creates = g_creates;
    }
    Check(destroyed == 2, "memory: near the budget the third set is freed (" + std::to_string(destroyed) + " destroyed)");
    Check(creates == 6, "memory: nothing is made again");
    Check(!runtime.used_destroyed, "memory: a freed output swapchain is never submitted");
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    Check(static_cast<double>(runtime.Submissions() - first) / elapsed > refresh * 0.85,
          "memory: every refresh still gets a frame");
    Check(presenter.Status().threaded && presenter.Status().dynamic, "memory: still presenting dynamically");

    // The budget grows back: after the delay the third set returns.
    presenter.SetMemoryRecoveryDelay(300ms);
    gpu.memory_pressure = false;
    int max_generated = 0;
    RunPipelined(app, 1500ms, 26ms, 32ms,
                 [&](int, const XrFrameState&) { max_generated = std::max<int>(max_generated, gpu.tuning.generated); });
    {
        std::scoped_lock lock(g_xr_mutex);
        creates = g_creates;
    }
    Check(creates == 8, "memory: with room again the third set is made again (" + std::to_string(creates) + " created)");
    Check(max_generated == 3, "memory: three generated frames per real frame again (" + std::to_string(max_generated) + ")");
    Check(!runtime.used_destroyed, "memory: the freed swapchains stay unused");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "memory: calls stayed ordered");
}

// A game frame far later than its slot leaves the presenter thread showing
// its last frame again: those refreshes are submitted, but they are not
// distinct frames, which is what the OSD reports.
void TestThreadedRepeatsAreNotDistinct() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    App app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    auto frame = [&](std::chrono::milliseconds work) {
        const XrFrameState state = app.Wait();
        app.Begin();
        std::this_thread::sleep_for(work);
        app.Release();
        Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "repeats: end");
    };
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 600ms) frame(4ms);
    frame(250ms); // over twenty refreshes with nothing new to show
    for (int i = 0; i < 4; ++i) frame(4ms);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.output_fps > 0.0 && status.distinct_fps < 0.9 * status.output_fps,
          "repeats: refreshes showing the last frame again are not counted as new frames (" +
              std::to_string(status.distinct_fps) + " of " + std::to_string(status.output_fps) + " fps)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "repeats: runtime and image calls stayed ordered");
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

// Interpolate presents from the presenter thread too: each real frame is
// shown after the frames interpolated between it and the previous one, at
// the display time it was rendered for, and the game keeps its overlap.
void TestThreadedInterpolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.threaded = true;
    Runtime runtime;
    runtime.paced = true;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate, kFormat, kAppUsage, FrameGenMultiplier::k3);
    App app(presenter, runtime);
    app.eye_speed = 0.5;
    for (int i = 0; i < 3; ++i) app.Frame();
    const std::size_t first = runtime.Submissions();
    std::mutex times_mutex;
    std::vector<XrTime> rendered_for;
    const auto start = std::chrono::steady_clock::now();
    // Every other frame renders much faster, as game frames vary: those
    // arrive well before their slot, while the previous real frame's
    // interpolated frames are still being shown.
    const int frames = RunPipelined(
        app, 1500ms, 20ms, 30ms,
        [&](int, const XrFrameState& state) {
            std::scoped_lock lock(times_mutex);
            rendered_for.push_back(state.predictedDisplayTime);
        },
        nullptr, 26ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.state == FrameGenState::kActive && !status.full_rate,
          "threaded interpolation: engages on the presenter thread");
    Check(frames / elapsed > refresh / 3 * 0.8,
          "threaded interpolation: the app holds a third of the refresh (" + std::to_string(frames / elapsed) + " fps)");
    const auto log = runtime.Snapshot();
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85,
          "threaded interpolation: every refresh gets a frame");
    CheckOrderedTimes(log, "threaded interpolation");
    Check(gpu.tuning.mode == FrameGenSynthesisMode::kInterpolate && gpu.tuning.generated == 3 &&
              gpu.tuning.fractions[0] > 0.2f && gpu.tuning.fractions[0] < 0.5f && gpu.tuning.fractions[1] > 0.5f &&
              gpu.tuning.fractions[1] < 0.8f && gpu.tuning.fractions[2] == 1.0f,
          "threaded interpolation: frames a third and two thirds of the way between real frames, then the real "
          "frame's own copy (" +
              std::to_string(gpu.tuning.fractions[0]) + ", " + std::to_string(gpu.tuning.fractions[1]) + ", " +
              std::to_string(gpu.tuning.fractions[2]) + ")");
    // Each real frame's slot, the display time it was rendered for, shows
    // its copy (the last output set; the game may have released its next
    // image by then) right after the frames interpolated toward it.
    const XrSwapchain copy_swapchain = gpu.last[0].outputs.back().swapchain;
    std::size_t real = 0, as_copy = 0, after_two = 0;
    for (std::size_t i = first + 12; i < log.size(); ++i) {
        {
            std::scoped_lock lock(times_mutex);
            if (std::find(rendered_for.begin(), rendered_for.end(), log[i].time) == rendered_for.end()) continue;
        }
        ++real;
        as_copy += log[i].swapchains.size() && log[i].swapchains[0] == copy_swapchain ? 1 : 0;
        auto interpolated = [&](std::size_t j) {
            return !IsAppImage(log[j].swapchains) && log[j].swapchains.size() && log[j].swapchains[0] != copy_swapchain;
        };
        after_two += interpolated(i - 1) && interpolated(i - 2) ? 1 : 0;
    }
    Check(real > 10 && as_copy * 10 >= real * 7 && after_two * 10 >= real * 7,
          "threaded interpolation: real frames' slots show their copies after their interpolated frames (" +
              std::to_string(as_copy) + " and " + std::to_string(after_two) + " of " + std::to_string(real) + ")");
    // No slot shows an image generated for another frame than its views.
    Check(runtime.mismatched_images == 0,
          "threaded interpolation: every output image shown is the one generated with its views (" +
              std::to_string(runtime.mismatched_images.load()) + " mismatched)");
    // The eyes moved +x: interpolated frames sit back toward the previous
    // real frame, a real frame interval (three periods) per unit.
    const float per_unit = static_cast<float>(0.5 * 3.0 * kPeriod * 1e-9);
    Check(std::abs(gpu.last[0].request.shift[0] + per_unit) < 0.05f * per_unit,
          "threaded interpolation: generated frames sit back where the eyes were (" +
              std::to_string(gpu.last[0].request.shift[0]) + " vs " + std::to_string(-per_unit) + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation,
          "threaded interpolation: runtime and image calls stayed ordered");
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
    bool all = true, any_generated = false, real_depth = true, generated_without_depth = false;
    {
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = first; i < log.size(); ++i) {
            all = all && log[i].layers == 2;
            const bool generated = !IsAppImage(log[i].swapchains);
            any_generated = any_generated || generated;
            if (generated) {
                generated_without_depth = generated_without_depth || !runtime.submitted_depth[i];
            } else {
                real_depth = real_depth && runtime.submitted_depth[i];
            }
        }
    }
    Check(any_generated && all, "threaded: generated frames keep the app's quad layer");
    Check(real_depth, "threaded: real frames keep their depth");
    // Its depth describes the real frame's image and eye position, not a
    // generated frame's moved content seen from where the eye will be.
    Check(generated_without_depth, "threaded: generated frames do not carry the real frame's depth");
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

// --- just-in-time synthesis ----------------------------------------------------------

const XrSwapchain kDepthChain = reinterpret_cast<XrSwapchain>(0x21);

// Frames written one at a time on the presenter thread, with the game's
// depth (generated frames are then seen from where the eye is at their time).
void StartJit(FrameGenPresenter& presenter, GpuLog& gpu, FrameGenSynthesisMode mode, FrameGenMultiplier multiplier,
              App*& app_out, Runtime& runtime, std::unique_ptr<App>& app) {
    gpu.threaded = true;
    gpu.jit = true;
    gpu.depth = true;
    StartPresenter(presenter, gpu, mode, kFormat, kAppUsage, multiplier);
    presenter.TrackSwapchain(kDepthChain, 20, 1,
                             XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    app = std::make_unique<App>(presenter, runtime);
    app->depth.subImage.swapchain = kDepthChain;
    app->depth.subImage.imageRect = {{0, 0}, {1600, 1600}};
    app->depth.nearZ = 0.1f;
    app->depth.farZ = 1000.0f;
    app_out = app.get();
}

// The eyes move at `speed` m/s and every frame is rendered from where they
// are at its display time: a refresh showing the content of its own display
// time has its first view there. Returns the share of refreshes after
// `first` (the first `skip` left out) that do, up to a constant offset (an
// app ending frames later than told): frozen or jumping content does not.
double TimelineShare(Runtime& runtime, std::size_t first, std::size_t skip, double speed, double* worst = nullptr) {
    std::vector<double> residuals;
    {
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = first + skip; i < runtime.submitted_times.size(); ++i) {
            if (runtime.submitted_swapchains[i].empty()) continue;
            const double expected = speed * static_cast<double>(runtime.submitted_times[i] - 1'000'000'000) * 1e-9;
            residuals.push_back(runtime.submitted_left_x[i] - expected);
        }
    }
    if (residuals.empty()) return 0.0;
    std::vector<double> sorted = residuals;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    const double tolerance = 0.6 * speed * static_cast<double>(kPeriod) * 1e-9;
    std::size_t good = 0;
    double largest = 0.0;
    for (const double residual : residuals) {
        good += std::abs(residual - median) <= tolerance ? 1 : 0;
        largest = std::max(largest, std::abs(residual - median));
    }
    if (worst) *worst = largest / (speed * static_cast<double>(kPeriod) * 1e-9);
    return static_cast<double>(good) / static_cast<double>(residuals.size());
}

int SynthesesWhere(GpuLog& gpu, const std::function<bool(FrameGenSynthesisMode, float)>& match) {
    std::scoped_lock lock(gpu.syntheses_mutex);
    return static_cast<int>(std::count_if(gpu.syntheses.begin(), gpu.syntheses.end(),
                                          [&](const GpuLog::Synthesis& entry) { return match(entry.mode, entry.fraction); }));
}

// Extrapolate just in time while each real frame's game frame is still on
// the GPU at its own refresh (it finishes a refresh and a half after the
// frame is analyzed): nothing is written from it meanwhile (written, a frame
// would wait for it on the runtime's queue, holding the refresh); the frame
// before it is moved on instead.
void TestJitWaitsForNoGameFrame() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.capture_lag = std::chrono::milliseconds(17);
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
    app->eye_speed = 0.5;
    for (int i = 0; i < 3; ++i) app->Frame();
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 1000ms) {
        const XrFrameState state = app->Wait();
        app->Begin();
        std::this_thread::sleep_for(4ms);
        app->Release();
        Check(XR_SUCCEEDED(app->End(state.predictedDisplayTime)), "jit unready: end");
    }
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const FrameGenStatus status = presenter.Status();
    const auto log = runtime.Snapshot();
    Check(gpu.uncaptured_writes == 0 && gpu.unmeasured_moves == 0,
          "jit unready: nothing written from a real frame still on the GPU (" +
              std::to_string(gpu.uncaptured_writes.load()) + " uncaptured, " +
              std::to_string(gpu.unmeasured_moves.load()) + " unmeasured)");
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85, "jit unready: every refresh gets a frame");
    // A real frame still on the GPU at its refresh would leave the frame
    // before it moved on instead: the lead is never shortened past what the
    // game's GPU needs (a frame ending early but captured only just in time
    // has no refresh to spare), so half the refreshes stay real frames'
    // copies at 2x and the rest are moved on.
    Check(status.lead_slots >= 2, "jit unready: the lead covers the game's GPU (" +
                                      std::to_string(status.lead_slots) + " refreshes)");
    Check(SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
              return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.0f;
          }) > static_cast<int>(refresh * elapsed * 0.4),
          "jit unready: the frames between real frames are moved on");
    Check(status.distinct_fps > 0.8 * status.output_fps,
          "jit unready: refreshes show new frames (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps)");
    Check(!runtime.ordering_violation && !g_release_violation, "jit unready: calls stayed ordered");
    presenter.Stop();
}

// Interpolate and Extrapolate switched while the game runs, just in time,
// with a frame always in flight (the game waits for its next frame before
// ending the last, as MSFS does): the presenter thread restarts in the new
// mode after a short run of steady frames. Engaging frames inline instead
// would keep it from ever starting again (the game held at half rate, inline).
void TestJitModeSwitchRestarts() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    for (int i = 0; i < 3; ++i) app->Frame();
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.multiplier = FrameGenMultiplier::kDynamic;
    auto extrapolated = [&] {
        return SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
            return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.0f;
        });
    };
    auto between = [&] {
        return SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
            return mode == FrameGenSynthesisMode::kInterpolate && fraction > 0.0f && fraction < 1.0f;
        });
    };
    auto engaged = [&] {
        const FrameGenStatus status = presenter.Status();
        return status.threaded && status.state == FrameGenState::kActive;
    };
    bool before = false, extrapolating = false, interpolating = false;
    int moved_at_switch = 0, moved_after = 0, between_at_switch = 0, between_after = 0;
    // Dynamic stands by until the game has run below the refresh rate for a
    // while (QuickDynamic); it stays engaged across the mode switches.
    RunPipelined(*app, 900ms, 20ms, 30ms);
    RunPipelined(*app, 4000ms, 20ms, 30ms, [&](int frame, const XrFrameState&) {
        if (frame == 20) {
            before = engaged();
            moved_at_switch = extrapolated();
            settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
            presenter.Configure(settings);
        } else if (frame == 55) {
            extrapolating = engaged();
            moved_after = extrapolated();
            between_at_switch = between();
            settings.synthesis_mode = FrameGenSynthesisMode::kInterpolate;
            presenter.Configure(settings);
        } else if (frame == 90) {
            interpolating = engaged();
            between_after = between();
        }
    });
    Check(before, "mode switch: interpolating from the presenter thread before the switch");
    Check(extrapolating, "mode switch: the presenter thread runs again after switching to Extrapolate");
    Check(moved_after - moved_at_switch > 20, "mode switch: frames are extrapolated after the switch (" +
                                                  std::to_string(moved_after - moved_at_switch) + ")");
    Check(interpolating, "mode switch: the presenter thread runs again after switching back to Interpolate");
    Check(between_after - between_at_switch > 20, "mode switch: frames are interpolated after switching back (" +
                                                      std::to_string(between_after - between_at_switch) + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "mode switch: calls stayed ordered");
}

// Turned off while the game's frame wait is under way (where a paced game
// spends most of its time), Frame Generation reads off: the frame that wait
// returns is not engaged, and nothing marks the state active, then waiting.
void TestToggleOffReadsOff() {
    for (const bool jit : {false, true}) {
        const std::string name = jit ? "toggle off (just in time)" : "toggle off (inline)";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        if (jit) {
            StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
        } else {
            StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate);
            owned = std::make_unique<App>(presenter, runtime);
            app = owned.get();
        }
        std::atomic<bool> stop{false};
        // Dynamic engages only for a game well below the refresh rate: it
        // works 20 ms a frame (about 45 fps). Engaged means presenting from
        // the presenter thread there.
        std::thread game([&] {
            while (!stop) {
                const XrFrameState state = app->Wait();
                app->Begin();
                if (jit) SleepPrecise(std::chrono::steady_clock::now() + 20ms);
                app->Release();
                Check(XR_SUCCEEDED(app->End(state.predictedDisplayTime)), name + ": end");
            }
        });
        if (jit) std::this_thread::sleep_for(500ms); // standing by first
        int engaged = 0, not_off = 0;
        for (int i = 0; i < 8; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250 + 7 * i));
            const FrameGenStatus status = presenter.Status();
            engaged += status.state == FrameGenState::kActive && (!jit || (status.threaded && !status.standby)) ? 1 : 0;
            Check(!presenter.Toggle(), name + ": turned off");
            std::this_thread::sleep_for(150ms);
            const FrameGenState state = presenter.Status().state;
            if (state != FrameGenState::kOff) {
                ++not_off;
                if (std::getenv("FG_TEST_VERBOSE")) std::cerr << name << ": reads " << ToString(state) << std::endl;
            }
            Check(presenter.Toggle(), name + ": turned on");
        }
        stop = true;
        game.join();
        Check(engaged >= 6, name + ": engaged before each toggle (" + std::to_string(engaged) + " of 8)");
        Check(not_off == 0, name + ": reads off once turned off (" + std::to_string(not_off) + " of 8 did not)");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Extrapolate at 3x, each generated frame written right before its refresh:
// one output image per eye whatever the multiplier, the game's own frame in
// its refresh, and every other refresh moved on from it by its time.
void TestJitExtrapolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k3, app, runtime, owned);
    app->eye_speed = 0.5;
    for (int i = 0; i < 3; ++i) app->Frame();
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    while (std::chrono::steady_clock::now() - start < 1200ms) {
        const XrFrameState state = app->Wait();
        app->Begin();
        std::this_thread::sleep_for(4ms);
        app->Release();
        Check(XR_SUCCEEDED(app->End(state.predictedDisplayTime)), "jit extrapolation: end");
        ++frames;
    }
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.state == FrameGenState::kActive, "jit extrapolation: presents from its own thread");
    const auto log = runtime.Snapshot();
    // Real frames are shown as exact copies, like the frames moved on from
    // them: the runtime never shows the game's own images.
    std::size_t app_images = 0;
    for (std::size_t i = first + 20; i < log.size(); ++i) app_images += IsAppImage(log[i].swapchains) ? 1 : 0;
    Check(app_images == 0, "jit extrapolation: the game's own images are never shown (" + std::to_string(app_images) + ")");
    const int copies = SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
        return (mode == FrameGenSynthesisMode::kInterpolate && fraction == 1.0f) ||
               (mode == FrameGenSynthesisMode::kExtrapolate && fraction == 0.0f);
    });
    const int moved = SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
        return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.0f;
    });
    const double share = static_cast<double>(copies) / static_cast<double>(copies + moved);
    Check(share > 0.25 && share < 0.42, "jit extrapolation: one real frame's copy in three refreshes (" +
                                            std::to_string(share) + ")");
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85, "jit extrapolation: every refresh gets a frame");
    Check(status.distinct_fps > 0.8 * status.output_fps,
          "jit extrapolation: refreshes show new frames (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps)");
    Check(gpu.analyzes > frames / 2 && gpu.executes == gpu.analyzes,
          "jit extrapolation: each real frame is measured once, nothing generated with it");
    Check(SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
              return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.2f && fraction < 0.9f;
          }) > frames / 2,
          "jit extrapolation: frames written a third and two thirds of an interval past the real frame");
    int creates = 0;
    {
        std::scoped_lock lock(g_xr_mutex);
        creates = g_creates;
    }
    Check(creates == 2, "jit extrapolation: one output swapchain per eye at 3x (" + std::to_string(creates) + ")");
    Check(runtime.mismatched_images == 0, "jit extrapolation: every image shown was written for its views (" +
                                              std::to_string(runtime.mismatched_images.load()) + " mismatched)");
    double worst = 0.0;
    const double timeline = TimelineShare(runtime, first, 20, app->eye_speed, &worst);
    Check(timeline > 0.95, "jit extrapolation: refreshes show their own time (" + std::to_string(timeline) + ", worst " +
                               std::to_string(worst) + " periods)");
    CheckOrderedTimes(log, "jit extrapolation");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "jit extrapolation: calls stayed ordered");
}

// Interpolate at 3x just in time: the presenter thread shows every refresh,
// each real frame's own refresh its exact copy, the two refreshes before it
// the frames between it and the previous one, and the game keeps its pace.
void TestJitInterpolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k3, app, runtime, owned);
    app->eye_speed = 0.5;
    for (int i = 0; i < 3; ++i) app->Frame();
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration gap{};
    const int frames = RunPipelined(*app, 1500ms, 20ms, 30ms, {}, &gap, 26ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.state == FrameGenState::kActive, "jit interpolation: presents from its own thread");
    Check(frames / elapsed > refresh / 3 * 0.8,
          "jit interpolation: the app holds a third of the refresh (" + std::to_string(frames / elapsed) + " fps)");
    const auto log = runtime.Snapshot();
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85, "jit interpolation: every refresh gets a frame");
    std::size_t app_images = 0;
    for (std::size_t i = first + 12; i < log.size(); ++i) app_images += IsAppImage(log[i].swapchains) ? 1 : 0;
    Check(app_images == 0, "jit interpolation: refreshes show written frames, not the game's swapchain (" +
                               std::to_string(app_images) + ")");
    const int between = SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
        return mode == FrameGenSynthesisMode::kInterpolate && fraction > 0.2f && fraction < 0.8f;
    });
    const int copies = SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
        return mode == FrameGenSynthesisMode::kInterpolate && fraction == 1.0f;
    });
    Check(between > frames && copies > frames / 2 && copies < frames * 3 / 2,
          "jit interpolation: two frames between real frames, then the real frame's copy (" + std::to_string(between) +
              " between, " + std::to_string(copies) + " copies, " + std::to_string(frames) + " real)");
    Check(runtime.mismatched_images == 0, "jit interpolation: every image shown was written for its views (" +
                                              std::to_string(runtime.mismatched_images.load()) + " mismatched)");
    double worst = 0.0;
    const double timeline = TimelineShare(runtime, first, 12, app->eye_speed, &worst);
    Check(timeline > 0.95, "jit interpolation: refreshes show their own time (" + std::to_string(timeline) + ", worst " +
                               std::to_string(worst) + " periods)");
    Check(status.distinct_fps > 0.9 * status.output_fps,
          "jit interpolation: every refresh is a new frame (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps)");
    Check(gap < 200ms, "jit interpolation: no stalls");
    int creates = 0;
    {
        std::scoped_lock lock(g_xr_mutex);
        creates = g_creates;
    }
    Check(creates == 2, "jit interpolation: one output swapchain per eye (" + std::to_string(creates) + ")");
    CheckOrderedTimes(log, "jit interpolation");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "jit interpolation: calls stayed ordered");
}

// Dynamic interpolation: an MSFS-like game runs at its own varying rate,
// every refresh shows its own time between the real frames either side, and
// a real frame far later than its pace is covered by frames moved on past
// the newest one instead of a freeze and a jump.
void TestJitDynamicInterpolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    app->eye_speed = 0.5;
    app->end_offset_periods = 1;
    // Standing by until the game has run below the refresh rate a while.
    RunPipelined(*app, 900ms, 16ms, 26ms, {}, nullptr, 8ms);
    Check(presenter.Status().threaded && !presenter.Status().standby, "jit dynamic interpolation: engages");
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration gap{};
    int covers_before = 0;
    const int frames = RunPipelined(
        *app, 2000ms, 16ms, 26ms,
        [&](int frame, const XrFrameState&) {
            if (frame == 40) {
                covers_before = SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
                    return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.0f;
                });
                std::this_thread::sleep_for(40ms); // one frame far slower than the rest
            }
        },
        &gap, 8ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.dynamic && status.state == FrameGenState::kActive,
          "jit dynamic interpolation: presents from its own thread, dynamically");
    Check(presenter.OsdText().find("Interpolate") != std::string::npos &&
              presenter.OsdText().find("Dynamic") != std::string::npos,
          "jit dynamic interpolation: the OSD says Interpolate and Dynamic (" + presenter.OsdText() + ")");
    Check(frames / elapsed > refresh / 3 * 1.15,
          "jit dynamic interpolation: the app is not held to a third of the refresh (" + std::to_string(frames / elapsed) +
              " fps)");
    const auto log = runtime.Snapshot();
    double longest_ms = 0.0;
    std::size_t longest_at = 0;
    {
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = first + 1; i < runtime.submitted_at.size(); ++i) {
            const double ms = std::chrono::duration<double, std::milli>(runtime.submitted_at[i] - runtime.submitted_at[i - 1]).count();
            if (ms > longest_ms) {
                longest_ms = ms;
                longest_at = i - first;
            }
        }
    }
    if (std::getenv("FG_TEST_GAPS")) {
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = first + 1; i < runtime.submitted_at.size(); ++i) {
            const double ms = std::chrono::duration<double, std::milli>(runtime.submitted_at[i] - runtime.submitted_at[i - 1]).count();
            const double t = std::chrono::duration<double, std::milli>(runtime.submitted_at[i] - start).count();
            if (ms > 16.0) std::cerr << "gap " << ms << " ms at " << t << " ms (submission " << i - first << ", time "
                                     << (runtime.submitted_times[i] - runtime.submitted_times[i - 1]) / 1e6 << " ms)\n";
        }
    }
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85,
          "jit dynamic interpolation: every refresh gets a frame (" +
              std::to_string(static_cast<double>(log.size() - first) / elapsed) + " fps, longest gap " +
              std::to_string(longest_ms) + " ms at submission " + std::to_string(longest_at) + " of " +
              std::to_string(log.size() - first) + ")");
    double worst = 0.0;
    const double timeline = TimelineShare(runtime, first, 30, app->eye_speed, &worst);
    Check(timeline > 0.95, "jit dynamic interpolation: refreshes show their own time (" + std::to_string(timeline) +
                               ", worst " + std::to_string(worst) + " periods)");
    const int covers = SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
        return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.0f;
    });
    Check(covers > covers_before, "jit dynamic interpolation: the slow frame's refreshes were covered (" +
                                      std::to_string(covers_before) + " then " + std::to_string(covers) + ")");
    Check(status.distinct_fps > 0.9 * status.output_fps,
          "jit dynamic interpolation: refreshes show new frames (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps)");
    Check(runtime.mismatched_images == 0, "jit dynamic interpolation: every image shown was written for its views (" +
                                              std::to_string(runtime.mismatched_images.load()) + " mismatched)");
    Check(gap < 200ms, "jit dynamic interpolation: no stalls");
    CheckOrderedTimes(log, "jit dynamic interpolation");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "jit dynamic interpolation: calls stayed ordered");
}

// Repeats the one-interval clamp of extrapolation predicts between real frames shown at `times`: a frame whose
// interval from the one before is p refreshes, followed by the next after g, is moved a whole interval on by its
// p-th refresh, and shows that again in each of the g - 1 - p refreshes left.
std::size_t ClampedRepeats(const std::vector<XrTime>& times) {
    std::size_t repeats = 0;
    for (std::size_t i = 2; i < times.size(); ++i) {
        const XrTime p = (times[i - 1] - times[i - 2] + kPeriod / 2) / kPeriod;
        const XrTime g = (times[i] - times[i - 1] + kPeriod / 2) / kPeriod;
        if (g - 1 > p) repeats += static_cast<std::size_t>(g - 1 - p);
    }
    return repeats;
}

// Refreshes the runtime was given after `from` and before `to`, and how many of them showed the same eye position
// as the refresh before: the same frame again.
std::pair<std::size_t, std::size_t> ShownRepeats(Runtime& runtime, XrTime from, XrTime to) {
    std::scoped_lock lock(g_xr_mutex);
    std::size_t refreshes = 0;
    std::size_t repeats = 0;
    for (std::size_t i = 1; i < runtime.submitted_times.size(); ++i) {
        const XrTime time = runtime.submitted_times[i];
        if (time <= from || time >= to || runtime.submitted_swapchains[i].empty()) continue;
        ++refreshes;
        repeats += runtime.submitted_left_x[i] == runtime.submitted_left_x[i - 1] ? 1 : 0;
    }
    return {refreshes, repeats};
}

// Dynamic extrapolation just in time: the game runs at its own rate and
// every refresh between its frames is written for its own time.
void TestJitDynamicExtrapolation() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    app->eye_speed = 0.5;
    app->end_offset_periods = 1;
    // Standing by until the game has run below the refresh rate a while;
    // engaged, it is held at half the refresh rate at most.
    RunPipelined(*app, 900ms, 16ms, 26ms, {}, nullptr, 8ms);
    Check(presenter.Status().threaded && !presenter.Status().standby, "jit dynamic extrapolation: engages");
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration gap{};
    // Each real frame's display time as the game ends it (one told period late, as MSFS does), and as predicted.
    std::vector<XrTime> ended_times;
    std::vector<XrTime> predicted_times;
    const int frames = RunPipelined(
        *app, 1500ms, 16ms, 26ms,
        [&](int, const XrFrameState& state) {
            ended_times.push_back(state.predictedDisplayTime + state.predictedDisplayPeriod * app->end_offset_periods);
            predicted_times.push_back(state.predictedDisplayTime);
        },
        &gap, 8ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    // Past the first frames (engagement), up to the last.
    const std::vector<XrTime> steady_ended(ended_times.begin() + std::min<std::size_t>(10, ended_times.size()),
                                           ended_times.end());
    const std::vector<XrTime> steady_predicted(
        predicted_times.begin() + std::min<std::size_t>(10, predicted_times.size()), predicted_times.end());
    const auto [window, shown] =
        steady_ended.size() > 2 ? ShownRepeats(runtime, steady_ended[1], steady_ended.back())
                                : std::pair<std::size_t, std::size_t>{0, 0};
    if (std::getenv("FG_TEST_STATS")) {
        std::map<XrTime, int> steps;
        for (std::size_t i = 1; i < steady_ended.size(); ++i) {
            ++steps[(steady_ended[i] - steady_ended[i - 1] + kPeriod / 2) / kPeriod];
        }
        std::cerr << "jit dynamic extrapolation: distinct " << status.distinct_fps << " of " << status.output_fps
                  << " fps, app " << frames / elapsed << " fps; window " << window << " refreshes, shown repeats "
                  << shown << ", clamp predicts " << ClampedRepeats(steady_ended) << " (ended times), "
                  << ClampedRepeats(steady_predicted) << " (predicted times); steps";
        for (const auto& [refreshes, count] : steps) std::cerr << ' ' << refreshes << 'x' << count;
        std::cerr << '\n';
    }
    Check(status.threaded && status.dynamic, "jit dynamic extrapolation: presents dynamically from its own thread");
    Check(frames / elapsed > refresh / 3 * 1.15,
          "jit dynamic extrapolation: the app is not held to a third of the refresh (" + std::to_string(frames / elapsed) +
              " fps)");
    const auto log = runtime.Snapshot();
    Check(static_cast<double>(log.size() - first) / elapsed > refresh * 0.85,
          "jit dynamic extrapolation: every refresh gets a frame");
    // A real frame later than a whole interval of its varying pace holds its motion at one interval (as frames
    // written together do): repeats. How many depends on how the game's frames fall on refreshes. This one
    // alternates 18 and 26 ms frames, which land 2 and 2 refreshes apart in one phase (no repeats) and 1 and 3
    // in another (one refresh in four repeats), so a fixed share of new frames fails in the second phase. The
    // refreshes shown may repeat only where the clamp says, give or take a late frame or two (+2).
    Check(window > 50 && shown <= ClampedRepeats(steady_ended) + 2,
          "jit dynamic extrapolation: refreshes repeat only where a frame is more than an interval late (" +
              std::to_string(shown) + " of " + std::to_string(window) + " repeated, " +
              std::to_string(ClampedRepeats(steady_ended)) + " expected)");
    Check(runtime.mismatched_images == 0, "jit dynamic extrapolation: every image shown was written for its views");
    int creates = 0;
    {
        std::scoped_lock lock(g_xr_mutex);
        creates = g_creates;
    }
    Check(creates == 2, "jit dynamic extrapolation: one output swapchain per eye (" + std::to_string(creates) + ")");
    Check(gap < 200ms, "jit dynamic extrapolation: no stalls");
    CheckOrderedTimes(log, "jit dynamic extrapolation");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "jit dynamic extrapolation: calls stayed ordered");
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
    Check(gpu.last[0].request.shift == std::array<float, 3>{0.0f, 0.0f, 0.0f},
          "depth: still eyes do not shift generated frames");
    // The fake backend never decides how to read the depth.
    Check(presenter.Status().depth == FrameGenDepthState::kChecking, "depth: reported as being checked");

    // The eyes keep moving: generated frames are asked to follow them as
    // they moved between the real frames, and are submitted from where the
    // eye will be.
    app.eye_speed = 0.5;
    for (int i = 0; i < 6; ++i) app.Frame();
    // 2x extrapolation: one generated frame a period past the newest, half
    // a real frame interval: the eye moves 0.5 m/s * 2 periods per unit.
    const float per_unit = static_cast<float>(0.5 * 2.0 * kPeriod * 1e-9);
    Check(std::abs(gpu.last[0].request.shift[0] - per_unit) < 0.05f * per_unit &&
              std::abs(gpu.last[1].request.shift[0] - per_unit) < 0.05f * per_unit &&
              std::abs(gpu.last[0].request.shift[1]) < 1e-6f,
          "depth: generated frames follow the eyes (" + std::to_string(gpu.last[0].request.shift[0]) + " vs " +
              std::to_string(per_unit) + ")");
    bool shifted_pose = false;
    {
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = 1; i < runtime.submitted_swapchains.size(); ++i) {
            if (!IsAppImage(runtime.submitted_swapchains[i]) && IsAppImage(runtime.submitted_swapchains[i - 1]) &&
                runtime.submitted_left_x[i] > runtime.submitted_left_x[i - 1] + 1e-4f) {
                shifted_pose = true; // ahead of the real frame before it
            }
        }
    }
    Check(shifted_pose, "depth: generated frames are submitted from the moved eye position");
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
    Check(plain.Status().depth == FrameGenDepthState::kNotReadable, "depth: unreadable depth is reported");

    // A game that sends no depth.
    FrameGenPresenter none;
    GpuLog none_gpu;
    none_gpu.depth = true;
    Runtime none_runtime;
    StartPresenter(none, none_gpu, FrameGenSynthesisMode::kExtrapolate);
    App none_app(none, none_runtime);
    for (auto& view : none_app.views) view.next = nullptr;
    Check(none.Status().depth == FrameGenDepthState::kUnknown, "depth: nothing reported before generated frames");
    for (int i = 0; i < 8; ++i) none_app.Frame();
    Check(none.Status().depth == FrameGenDepthState::kNotSent, "depth: a game without depth is reported");
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

// A frame whose images were not captured when the game released them (a
// view's first frame, or its region changed) is skipped, never read after
// release: it is not a GPU failure, so it never pauses generation.
void TestUncapturedFramesAreSkipped() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    for (int i = 0; i < 5; ++i) app.Frame();
    Check(presenter.Status().state == FrameGenState::kActive, "uncaptured: engaged");
    gpu.uncaptured = true;
    for (int i = 0; i < 6; ++i) app.Frame();
    const FrameGenStatus status = presenter.Status();
    Check(status.state == FrameGenState::kActive && status.failures == 0,
          "uncaptured: skipped frames are not failures and do not pause");
    Check(IsAppImage(runtime.Snapshot().back().swapchains), "uncaptured: the real frame is still shown");
    gpu.uncaptured = false;
    const std::uint64_t generated = presenter.Status().generated;
    for (int i = 0; i < 3; ++i) app.Frame();
    Check(presenter.Status().generated > generated, "uncaptured: generation resumes with captured frames");
    Check(!runtime.ordering_violation && !g_release_violation, "uncaptured: calls stayed ordered");
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

// --- Dynamic stand-by, GPU-aware pacing, pauses, telemetry --------------------------

// A game that works work[i % size] after each wait, then releases and ends
// its frame. `each` runs after every frame with its wait's state; true stops.
int RunWork(App& app, std::chrono::milliseconds duration, const std::vector<std::chrono::microseconds>& work,
            const std::function<bool(const XrFrameState&)>& each = {}) {
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    while (std::chrono::steady_clock::now() - start < duration) {
        const XrFrameState state = app.Wait();
        app.Begin();
        SleepPrecise(std::chrono::steady_clock::now() + work[static_cast<std::size_t>(frames) % work.size()]);
        app.Release();
        Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "work: end");
        ++frames;
        if (each && each(state)) break;
    }
    return frames;
}

// Against a paced runtime, standing by, each frame takes ticks[i % size]
// refreshes: {1, 2, 2} runs at 0.6x the refresh rate. Engaged, the
// presenter paces the waits instead (the work still fits those refreshes).
int RunTicks(App& app, std::chrono::milliseconds duration, const std::vector<int>& ticks,
             const std::function<bool(const XrFrameState&)>& each = {}) {
    std::vector<std::chrono::microseconds> work;
    for (const int tick : ticks) {
        work.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::nanoseconds((tick - 1) * app.runtime.period)) +
                       2ms);
    }
    return RunWork(app, duration, work, each);
}

bool Engaged(FrameGenPresenter& presenter) {
    const FrameGenStatus status = presenter.Status();
    return status.state == FrameGenState::kActive && status.threaded && !status.standby;
}

// Runs a game at 0.6x the refresh rate until Dynamic engages (or fails the test).
void EngageDynamic(FrameGenPresenter& presenter, App& app, const std::string& name) {
    RunTicks(app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), name + ": dynamic engages for a game at 0.6x the refresh rate");
}

const std::vector<std::chrono::microseconds> kLightWork{4ms};

// The game's frames reach the GPU history (captured) long before their
// motion is measured, or the other way round: an exact copy needs only its
// capture, a moved frame its measurement too; nothing is written early.
void TestJitCaptureVsMeasureLag() {
    for (const bool measure : {true, false}) {
        const std::string name = measure ? "measure lag" : "capture lag";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
        if (measure) {
            gpu.measure_lag = 35ms; // the refresh after a real frame's, up to a lead of two
        } else {
            gpu.capture_lag = 40ms; // later than the first leads
        }
        for (int i = 0; i < 3; ++i) app->Frame();
        const std::size_t first = runtime.Submissions();
        const auto start = std::chrono::steady_clock::now();
        RunWork(*app, 700ms, kLightWork);
        const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
        const FrameGenStatus status = presenter.Status();
        Check(gpu.uncaptured_writes == 0 && gpu.unmeasured_moves == 0,
              name + ": nothing is written from a frame not on the GPU yet (" +
                  std::to_string(gpu.uncaptured_writes.load()) + " uncaptured, " +
                  std::to_string(gpu.unmeasured_moves.load()) + " unmeasured)");
        if (measure) {
            Check(status.unmeasured_copies > 0 && status.unready_capture == 0,
                  name + ": captured frames are shown, unmeasured ones unmoved (" +
                      std::to_string(status.unmeasured_copies) + " unmeasured copies, " +
                      std::to_string(status.unready_capture) + " uncaptured)");
        } else {
            Check(status.unready_capture > 0 && status.unmeasured_copies == 0,
                  name + ": frames not captured at their refresh are counted, never measured late (" +
                      std::to_string(status.unready_capture) + " uncaptured, " +
                      std::to_string(status.unmeasured_copies) + " unmeasured copies)");
        }
        const double refresh = 1e9 / static_cast<double>(kPeriod);
        Check(static_cast<double>(runtime.Submissions() - first) / elapsed > refresh * 0.85,
              name + ": every refresh gets a frame");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// A game that runs frames ahead on its GPU: without GPU-aware pacing its
// frames queue behind each other and are captured late; holding its waits
// until the capture of the frame before the newest begun one has run leaves
// one frame queued, as a runtime does. After timeouts in a row, holding
// stops for the session. Waits on another thread than ends (a pipelined
// game's wait for frame N + 1 before frame N ends) still hold for the frame
// before frame N.
void TestGpuAwarePacing() {
    struct Run {
        int max_queued{0};
        double capture_ms{0.0};
        FrameGenStatus status;
    };
    auto run = [](const char* gate) {
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.game_gpu = 26ms; // slower than two refreshes: the GPU sets the pace
        gpu.frames_ahead = 4;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", gate);
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "");
        app->before_release = [&] { gpu.GameFrame(); };
        for (int i = 0; i < 3; ++i) app->Frame();
        RunWork(*app, 400ms, {2ms});
        {
            std::scoped_lock lock(gpu.timeline_mutex);
            gpu.max_queued = 0;
            gpu.capture_latency_sum_ms = 0.0;
            gpu.capture_latency_count = 0;
        }
        RunWork(*app, 900ms, {2ms});
        Run result;
        result.max_queued = gpu.max_queued;
        result.capture_ms = gpu.MeanCaptureLatencyMs();
        result.status = presenter.Status();
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "gpu pacing: calls stayed ordered");
        return result;
    };
    const Run free = run("");
    const Run held = run("always");
    Check(!free.status.gpu_gating && free.status.gpu_waits == 0, "gpu pacing: not armed without evidence");
    Check(held.status.gpu_gating && held.status.gpu_waits > 0, "gpu pacing: always holds the game's waits (" +
                                                                    std::to_string(held.status.gpu_waits) + ")");
    Check(free.max_queued >= 3 && held.max_queued <= 2,
          "gpu pacing: one game frame stays queued on the GPU (" + std::to_string(held.max_queued) + " held, " +
              std::to_string(free.max_queued) + " free)");
    Check(held.capture_ms < 0.8 * free.capture_ms,
          "gpu pacing: frames are captured sooner after their release (" + std::to_string(held.capture_ms) + " ms vs " +
              std::to_string(free.capture_ms) + " ms)");

    // A capture that never reports done: five timeouts in a row, then the
    // game's waits are no longer held.
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "always");
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "");
        for (int i = 0; i < 3; ++i) app->Frame();
        gpu.hold_captures = true;
        const auto start = std::chrono::steady_clock::now();
        const int frames = RunWork(*app, 900ms, kLightWork);
        const FrameGenStatus status = presenter.Status();
        Check(status.gpu_wait_timeouts == 5 && !status.gpu_gating,
              "gpu pacing: five timeouts in a row turn it off (" + std::to_string(status.gpu_wait_timeouts) + ")");
        Check(frames / Seconds(std::chrono::steady_clock::now() - start) > 30.0,
              "gpu pacing: the game keeps its pace after it turned off");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "gpu pacing timeouts: calls stayed ordered");
    }

    // Pipelined: each wait enters before the frame before it ends; it holds
    // for the captures released before the newest begun frame (the frame
    // before that one), never for that frame's own or an older one's.
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.game_gpu = 30ms;
        gpu.frames_ahead = 4;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "always");
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "");
        // FakeGpu::CaptureMark counts captures, as the presenter's marks do.
        app->before_begin = [&] { gpu.newest_begin_mark = static_cast<std::uint64_t>(gpu.captures.load()); };
        app->before_release = [&] { gpu.GameFrame(); };
        RunPipelined(*app, 900ms, 6ms, 14ms);
        std::size_t waits = 0, matched = 0;
        {
            std::scoped_lock lock(gpu.waits_mutex);
            waits = gpu.capture_waits.size();
            for (const auto& [mark, begin_mark] : gpu.capture_waits) matched += mark == begin_mark ? 1 : 0;
        }
        Check(waits > 10 && matched == waits,
              "gpu pacing: a wait holds for the captures before the newest begun frame (" + std::to_string(matched) +
                  " of " + std::to_string(waits) + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "gpu pacing pipelined: calls stayed ordered");
    }
}

// A real frame whose game frame is not on the GPU when its refresh first
// needs it is late, though its end came in time: the lead grows by a
// refresh and then holds (one ending early but captured just in time has no
// refresh to spare).
void TestUncapturedAtOwnRefreshIsLate() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 600ms, kLightWork);
    const FrameGenStatus settled = presenter.Status();
    Check(settled.lead_slots == 1, "uncaptured late: a quick game's lead shortens to one refresh (" +
                                       std::to_string(settled.lead_slots) + ")");
    // Captured a refresh and a quarter after release: after its one-refresh
    // lead, before a second.
    gpu.capture_lag = std::chrono::microseconds(kPeriod * 5 / 4 / 1000);
    RunWork(*app, 450ms, kLightWork);
    const FrameGenStatus grown = presenter.Status();
    RunWork(*app, 450ms, kLightWork);
    const FrameGenStatus held = presenter.Status();
    Check(grown.late_gpu > settled.late_gpu && grown.lead_slots >= 2 && grown.lead_slots <= 3,
          "uncaptured late: frames not on the GPU at their refresh lengthen the lead (" +
              std::to_string(grown.late_gpu - settled.late_gpu) + " late, lead " + std::to_string(grown.lead_slots) + ")");
    Check(grown.late_cpu == settled.late_cpu, "uncaptured late: their ends were on time");
    Check(held.lead_slots >= 2 && held.unready_capture - grown.unready_capture <= 2,
          "uncaptured late: the longer lead holds (lead " + std::to_string(held.lead_slots) + ", " +
              std::to_string(held.unready_capture - grown.unready_capture) + " more uncaptured)");
    Check(gpu.uncaptured_writes == 0, "uncaptured late: nothing written from them early");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "uncaptured late: calls stayed ordered");
}

// Dynamic standing by: a game at 0.9x the refresh rate passes straight
// through, at the runtime's period, and nothing is captured or analyzed.
void TestDynamicPassThroughNearRefresh() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    bool refresh_period = true;
    const auto start = std::chrono::steady_clock::now();
    const int frames = RunTicks(*app, 1500ms, {1, 1, 1, 1, 1, 1, 1, 1, 1, 2}, [&](const XrFrameState& state) {
        refresh_period = refresh_period && state.predictedDisplayPeriod == kPeriod;
        return false;
    });
    const double rate = frames / Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    Check(rate > 0.8 * refresh, "standing by: the game runs at about 0.9x (" + std::to_string(rate) + " fps)");
    Check(status.standby && status.full_rate && status.state == FrameGenState::kActive && !status.threaded &&
              status.dynamic_engagements == 0,
          "standing by: a game near the refresh rate passes straight through");
    Check(gpu.captures == 0 && gpu.executes == 0, "standing by: nothing is captured or analyzed (" +
                                                      std::to_string(gpu.captures.load()) + " captures)");
    Check(refresh_period, "standing by: the game is told the runtime's period");
    Check(presenter.OsdText().find("standing by") != std::string::npos &&
              presenter.CompactOsdText().find("standing by") != std::string::npos,
          "standing by: the OSD says so (" + presenter.OsdText() + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "standing by: calls stayed ordered");
}

// A game at 0.6x the refresh rate engages Dynamic once it has run below 0.78x
// for engage_after, and frames are generated from then on.
void TestDynamicEngagesBelow() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    const auto start = std::chrono::steady_clock::now();
    EngageDynamic(presenter, *app, "engage");
    const double engaged_after = Seconds(std::chrono::steady_clock::now() - start);
    const FrameGenDynamicPolicy policy = QuickDynamic();
    const double limit = Seconds(policy.engage_after + policy.settle) + 0.8;
    Check(engaged_after >= Seconds(policy.engage_after) && engaged_after <= limit,
          "engage: after engage_after, within engage_after + settle (" + std::to_string(engaged_after) + " s)");
    RunTicks(*app, 400ms, {1, 2, 2});
    const FrameGenStatus status = presenter.Status();
    Check(status.dynamic_engagements == 1 && status.dynamic_reason.find("the game ran at") != std::string::npos,
          "engage: one engagement, with its reason (" + status.dynamic_reason + ")");
    Check(gpu.captures > 0 && gpu.analyzes > 0 && status.generated > 0, "engage: frames are generated once engaged");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "engage: calls stayed ordered");
}

// Engaged, a game that could hold the refresh rate is held at half of it:
// it is told two refreshes per frame, runs at 45 fps, and every refresh is
// a new frame.
void TestDynamicHalfRateLock() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    EngageDynamic(presenter, *app, "half rate");
    RunTicks(*app, 400ms, {1});
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    bool two_refreshes = true;
    const int frames = RunTicks(*app, 1000ms, {1}, [&](const XrFrameState& state) {
        two_refreshes = two_refreshes && state.predictedDisplayPeriod == 2 * kPeriod;
        return false;
    });
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const double rate = frames / elapsed;
    Check(rate > refresh / 2 * 0.9 && rate < refresh / 2 * 1.1,
          "half rate: a game able to hold the refresh rate runs at half of it (" + std::to_string(rate) + " fps)");
    Check(two_refreshes, "half rate: the game is told two refreshes per frame");
    Check(static_cast<double>(runtime.Submissions() - first) / elapsed > refresh * 0.85,
          "half rate: every refresh gets a frame");
    const FrameGenStatus status = presenter.Status();
    Check(status.distinct_fps > 0.9 * status.output_fps,
          "half rate: every refresh is a new frame (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps)");
    Check(Engaged(presenter) && status.dynamic_stand_downs == 0, "half rate: stays engaged");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "half rate: calls stayed ordered");
}

// Between the thresholds nothing changes: standing by at 0.83x stays standing
// by, engaged at 0.83x stays engaged. After a stand-down nothing engages
// until the hold has passed, however slow the game.
void TestDynamicHysteresis() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(GuardDynamic());
    const std::vector<int> between{1, 1, 1, 1, 2}; // 0.83x
    RunTicks(*app, 700ms, between);
    Check(presenter.Status().standby && presenter.Status().dynamic_engagements == 0,
          "hysteresis: 0.83x stays standing by");
    EngageDynamic(presenter, *app, "hysteresis");
    RunTicks(*app, 700ms, between);
    FrameGenStatus status = presenter.Status();
    Check(Engaged(presenter) && status.dynamic_engagements == 1 && status.dynamic_stand_downs == 0,
          "hysteresis: 0.83x stays engaged");
    // Motion never measured: nothing new to show, the guard stands down.
    gpu.measure_lag = 1s;
    RunTicks(*app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return presenter.Status().standby; });
    Check(presenter.Status().standby && presenter.Status().dynamic_stand_downs == 1, "hysteresis: the guard stands down");
    const auto stood_down = std::chrono::steady_clock::now();
    gpu.measure_lag = 0us;
    RunTicks(*app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return Engaged(presenter); });
    const double held = Seconds(std::chrono::steady_clock::now() - stood_down);
    status = presenter.Status();
    Check(Engaged(presenter) && status.dynamic_engagements == 2,
          "hysteresis: engages again after the hold once the fault is gone");
    Check(held >= Seconds(GuardDynamic().hold), "hysteresis: nothing engages inside the hold (" + std::to_string(held) + " s)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "hysteresis: calls stayed ordered");
}

// The benefit guard stands down on each trigger, after two failing windows:
// (a) no more distinct frames than the game alone, (b) too few generated
// frames, (c) real frames on the GPU too late, (d) motion measured too late.
// The others are turned off for (b)-(d). After the hold, with the fault
// gone, it engages again.
void TestBenefitGuardBacksOff() {
    struct Case {
        const char* name;
        const char* reason;
        std::function<void(GpuLog&)> fault;
        std::function<void(FrameGenDynamicPolicy&)> isolate;
    };
    const std::vector<Case> cases{
        {"guard (a)", "distinct frames/s", [](GpuLog& gpu) { gpu.measure_lag = 1s; },
         [](FrameGenDynamicPolicy&) {}},
        {"guard (b)", "too few", [](GpuLog& gpu) { gpu.measure_lag = 1s; },
         [](FrameGenDynamicPolicy& policy) { policy.distinct_margin_fps = -1000.0; }},
        {"guard (c)", "reached the GPU too late", [](GpuLog& gpu) { gpu.capture_lag = 150ms; },
         [](FrameGenDynamicPolicy& policy) {
             policy.distinct_margin_fps = -1000.0;
             policy.generated_share = 0.0;
             policy.late_analyses_share = 2.0;
         }},
        {"guard (d)", "measured too late",
         [](GpuLog& gpu) { gpu.measure_lag = std::chrono::microseconds(4 * kPeriod / 1000); },
         [](FrameGenDynamicPolicy& policy) {
             policy.distinct_margin_fps = -1000.0;
             policy.generated_share = 0.0;
             policy.unready_share = 2.0;
         }},
    };
    for (const Case& test : cases) {
        const std::string name = test.name;
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
        FrameGenDynamicPolicy policy = GuardDynamic();
        test.isolate(policy);
        presenter.SetDynamicPolicy(policy);
        EngageDynamic(presenter, *app, name);
        RunTicks(*app, 300ms, {1, 2, 2});
        Check(Engaged(presenter), name + ": no stand-down without a fault");
        test.fault(gpu);
        RunTicks(*app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return presenter.Status().standby; });
        FrameGenStatus status = presenter.Status();
        Check(status.standby && status.dynamic_stand_downs == 1 &&
                  status.dynamic_reason.find(test.reason) != std::string::npos,
              name + ": stands down (" + status.dynamic_reason + ")");
        RunTicks(*app, 100ms, {1, 2, 2});
        Check(!presenter.Status().threaded, name + ": frames pass straight through");
        if (name == "guard (a)") {
            gpu.measure_lag = 0us;
            RunTicks(*app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return Engaged(presenter); });
            status = presenter.Status();
            Check(Engaged(presenter) && status.dynamic_engagements == 2,
                  name + ": engages again after the hold once the fault is gone");
        }
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Engaged without a guard trigger for probe_after, Dynamic stands by to see
// whether the game holds the refresh rate alone: a game still slow engages
// again and the next probe waits twice as long; a game holding 0.85x stays
// standing by.
void TestDynamicProbe() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenDynamicPolicy policy = GuardDynamic();
    policy.probe_after = 500ms;
    policy.probe_max = 4s;
    presenter.SetDynamicPolicy(policy);
    EngageDynamic(presenter, *app, "probe");
    auto engaged_for = [&](std::uint64_t probes) {
        const auto start = std::chrono::steady_clock::now();
        RunTicks(*app, 4000ms, {1, 2, 2},
                 [&](const XrFrameState&) { return presenter.Status().dynamic_probes >= probes; });
        return Seconds(std::chrono::steady_clock::now() - start);
    };
    const double first = engaged_for(1);
    Check(presenter.Status().dynamic_probes == 1 && first >= 0.45,
          "probe: probes after probe_after engaged (" + std::to_string(first) + " s)");
    RunTicks(*app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter) && presenter.Status().dynamic_engagements == 2,
          "probe: a game still below 0.85x engages again");
    const double second = engaged_for(2);
    Check(presenter.Status().dynamic_probes == 2 && second >= 0.95,
          "probe: a failed probe doubles the wait before the next (" + std::to_string(first) + " s, then " +
              std::to_string(second) + " s)");
    // The game now holds the refresh rate: the probe succeeds and stays.
    RunTicks(*app, 1100ms, {1});
    const FrameGenStatus status = presenter.Status();
    Check(status.standby && !status.threaded && status.dynamic_engagements == 2 &&
              status.dynamic_reason.find("holds") != std::string::npos,
          "probe: a game holding the refresh rate stays standing by (" + status.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "probe: calls stayed ordered");
}

// Each real frame has a refresh of its own, extrapolating too: a 72 Hz game
// waited every refresh (no half-rate lock below 40 fps) whose lead shortens
// would give two frames the same refresh, and the older would never be shown.
void TestExtrapolateDuplicateTargetGuard() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    runtime.period = 13'888'889;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.settle = 60s; // no guard or probe decisions here
    presenter.SetDynamicPolicy(policy);
    EngageDynamic(presenter, *app, "duplicate targets");
    const int analyzed_before = gpu.analyzes;
    const std::uint64_t late_before = presenter.Status().late_cpu;
    std::vector<XrTime> predicted;
    RunTicks(*app, 1200ms, {1}, [&](const XrFrameState& state) {
        predicted.push_back(state.predictedDisplayTime);
        return false;
    });
    const FrameGenStatus status = presenter.Status();
    Check(status.lead_slots <= 2, "duplicate targets: the lead shortened (" + std::to_string(status.lead_slots) + ")");
    for (std::size_t i = 1; i < predicted.size(); ++i) {
        Check(predicted[i] - predicted[i - 1] >= runtime.period / 2,
              "duplicate targets: consecutive frames are told refreshes apart (" +
                  std::to_string(predicted[i] - predicted[i - 1]) + " ns at frame " + std::to_string(i) + ")");
    }
    std::set<std::uint64_t> exact;
    {
        std::scoped_lock lock(gpu.syntheses_mutex);
        for (const GpuLog::Synthesis& entry : gpu.syntheses) {
            if ((entry.mode == FrameGenSynthesisMode::kInterpolate && entry.fraction == 1.0f) ||
                (entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction == 0.0f)) {
                exact.insert(entry.frame);
            }
        }
    }
    std::uint64_t missing = 0;
    for (int id = analyzed_before + 1; id + 3 <= gpu.analyzes; ++id) {
        missing += exact.count(static_cast<std::uint64_t>(id)) ? 0 : 1;
    }
    Check(missing <= status.late_cpu - late_before,
          "duplicate targets: every analysed frame on time gets its exact copy (" + std::to_string(missing) + " missing)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "duplicate targets: calls stayed ordered");
}

// A game that stops sending frames: its last frame moves on for at most the
// pause limit (max(5 refreshes, 50 ms)), then shows unchanged; the frame
// after the gap is not measured against the one before it (no motion across
// the gap), and refreshes show their own time again from the next.
void TestNoExtrapolationAcrossPauses() {
    for (const auto pause : {1000ms, 250ms}) {
        const std::string name = "pause " + std::to_string(pause.count()) + " ms";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k4, app, runtime, owned);
        app->eye_speed = 0.5;
        for (int i = 0; i < 3; ++i) app->Frame();
        RunWork(*app, 500ms, kLightWork);
        const auto last_end = std::chrono::steady_clock::now();
        const FrameGenStatus before = presenter.Status();
        std::size_t syntheses_before = 0;
        {
            std::scoped_lock lock(gpu.syntheses_mutex);
            syntheses_before = gpu.syntheses.size();
        }
        SleepPrecise(last_end + pause);
        const FrameGenStatus paused = presenter.Status();
        // Up to the last frame's refresh (its lead) the one before is still
        // moved on; past it, at most its four steps at 4x.
        Check(paused.generated - before.generated <= 4 + before.lead_slots,
              name + ": the last frame moves on a few refreshes only (" +
                  std::to_string(paused.generated - before.generated) + " generated, lead " +
                  std::to_string(before.lead_slots) + ")");
        int moved_late = 0;
        {
            std::scoped_lock lock(gpu.syntheses_mutex);
            for (std::size_t i = syntheses_before; i < gpu.syntheses.size(); ++i) {
                const GpuLog::Synthesis& entry = gpu.syntheses[i];
                moved_late += entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction > 0.0f &&
                                      entry.at > last_end + 120ms
                                  ? 1
                                  : 0;
            }
        }
        Check(moved_late == 0, name + ": nothing moves past the pause limit (" + std::to_string(moved_late) + ")");
        std::size_t syntheses_resumed = 0;
        {
            std::scoped_lock lock(gpu.syntheses_mutex);
            syntheses_resumed = gpu.syntheses.size();
        }
        const std::size_t resumed = runtime.Submissions();
        RunWork(*app, 600ms, kLightWork);
        const FrameGenStatus after = presenter.Status();
        Check(after.pauses == before.pauses + 1, name + ": the gap is counted as a pause");
        int tiny = 0;
        {
            std::scoped_lock lock(gpu.syntheses_mutex);
            for (std::size_t i = syntheses_resumed; i < gpu.syntheses.size(); ++i) {
                const GpuLog::Synthesis& entry = gpu.syntheses[i];
                tiny += entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction > 0.0f && entry.fraction < 0.2f
                            ? 1
                            : 0;
            }
        }
        Check(tiny == 0, name + ": no motion is measured across the gap (" + std::to_string(tiny) + " frames barely moved)");
        double worst = 0.0;
        const double timeline = TimelineShare(runtime, resumed, 8, app->eye_speed, &worst);
        Check(timeline > 0.95, name + ": refreshes show their own time after the gap (" + std::to_string(timeline) + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Passing frames straight through captures nothing (Auto at full rate,
// Dynamic standing by); the first engaged frame is still captured at its
// release, so generation starts from captured history.
void TestCaptureOnlyWhileArmed() {
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        StartThreaded(presenter, gpu, FrameGenMultiplier::kAuto);
        App app(presenter, runtime);
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 600ms) app.Frame();
        Check(presenter.Status().full_rate && gpu.captures == 0 && gpu.executes == 0,
              "armed: auto at full rate captures nothing (" + std::to_string(gpu.captures.load()) + ")");
        presenter.Stop();
    }
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::kDynamic);
    App app(presenter, runtime);
    RunTicks(app, 600ms, {1});
    Check(presenter.Status().standby && gpu.captures == 0 && gpu.executes == 0,
          "armed: dynamic standing by captures nothing (" + std::to_string(gpu.captures.load()) + ")");
    EngageDynamic(presenter, app, "armed");
    RunTicks(app, 300ms, {1, 2, 2});
    Check(gpu.captures > 0 && gpu.executes > 0 && gpu.execute_uncaptured == 0,
          "armed: engaged frames are captured at release, the first one included (" +
              std::to_string(gpu.execute_uncaptured.load()) + " generated from uncaptured images)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "armed: calls stayed ordered");
}

// The lead a presenter thread learned is not the next one's: restarted, it
// starts from the game's latency again.
void TestLeadResetOnRestart() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
    gpu.capture_lag = 40ms; // real frames reach the GPU history late: the lead grows
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 500ms, kLightWork);
    const std::uint32_t grown = presenter.Status().lead_slots;
    Check(grown >= 4, "lead reset: the lead grew (" + std::to_string(grown) + ")");
    gpu.capture_lag = 0us;
    presenter.Toggle();
    for (int i = 0; i < 4; ++i) app->Frame();
    Check(!presenter.Status().threaded, "lead reset: toggled off, the presenter thread stopped");
    presenter.Toggle();
    RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) { return presenter.Status().threaded; });
    RunWork(*app, 30ms, kLightWork);
    const std::uint32_t restarted = presenter.Status().lead_slots;
    Check(presenter.Status().threaded && restarted <= 2,
          "lead reset: the restarted presenter thread measures its lead afresh (" + std::to_string(grown) + " then " +
              std::to_string(restarted) + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "lead reset: calls stayed ordered");
}

// A game showing its last frame (stalled): its exact copy is written once,
// then the runtime shows the output images' newest again each refresh.
void TestSkipIdenticalExactCopies() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
    app->eye_speed = 0.5;
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 400ms, kLightWork);
    const FrameGenStatus before = presenter.Status();
    const std::size_t first = runtime.Submissions();
    std::size_t syntheses_before = 0;
    {
        std::scoped_lock lock(gpu.syntheses_mutex);
        syntheses_before = gpu.syntheses.size();
    }
    const auto start = std::chrono::steady_clock::now();
    SleepPrecise(start + 300ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const FrameGenStatus stalled = presenter.Status();
    int exact = 0, moved = 0;
    {
        std::scoped_lock lock(gpu.syntheses_mutex);
        for (std::size_t i = syntheses_before; i < gpu.syntheses.size(); ++i) {
            const GpuLog::Synthesis& entry = gpu.syntheses[i];
            const bool copy = entry.mode == FrameGenSynthesisMode::kExtrapolate ? entry.fraction <= 0.0f : entry.fraction >= 1.0f;
            (copy ? exact : moved) += 1;
        }
    }
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const std::size_t refreshes = runtime.Submissions() - first;
    Check(static_cast<double>(refreshes) / elapsed > refresh * 0.85, "skip copies: every refresh still gets a frame");
    Check(exact <= 2 && moved <= 8, "skip copies: the stalled frame's exact copy is written once (" +
                                        std::to_string(exact) + " exact, " + std::to_string(moved) + " moved)");
    Check(stalled.copies_skipped - before.copies_skipped >= refreshes / 2,
          "skip copies: the refreshes after it repeat the outputs' images (" +
              std::to_string(stalled.copies_skipped - before.copies_skipped) + " of " + std::to_string(refreshes) + ")");
    // Generated frames count once, when first shown: the frame before moved
    // on up to the last one's refresh, then the last one at half and a whole
    // interval (held there until the pause limit, not counted again).
    Check(stalled.generated - before.generated <= before.lead_slots + 2,
          "skip copies: repeated frames are not counted as generated (" +
              std::to_string(stalled.generated - before.generated) + ", lead " + std::to_string(before.lead_slots) + ")");
    const auto log = runtime.Snapshot();
    Check(!IsAppImage(log.back().swapchains), "skip copies: the stalled refreshes show Frame Generation's images");
    for (int i = 0; i < 3; ++i) app->Frame();
    Check(runtime.mismatched_images == 0, "skip copies: every image shown was written for its views");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "skip copies: calls stayed ordered");
}

// Summaries every window: window averages and deltas, session totals in
// parentheses, standing-by time, the Dynamic state; the windows add up to
// what Status reports.
void TestTelemetryWindows() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("vectorxr-presenter-tests-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    auto logger = std::make_unique<Logger>();
    logger->Initialize(directory / "telemetry.log");
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.threaded = gpu.jit = true;
    Runtime runtime;
    runtime.paced = true;
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
    settings.multiplier = FrameGenMultiplier::kDynamic;
    presenter.Configure(settings);
    presenter.SetEngagementPolicy(3, 0ms);
    presenter.SetDynamicPolicy(QuickDynamic());
    presenter.SetSummaryInterval(300ms);
    presenter.Start(kSession, nullptr, Dispatch(), logger.get(), nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    presenter.TrackSwapchain(kLeft, kFormat, 1, kAppUsage);
    presenter.TrackSwapchain(kRight, kFormat, 1, kAppUsage);
    {
        std::scoped_lock lock(g_xr_mutex);
        g_creates = 0;
        g_destroyed.clear();
        g_wait_timeouts = 0;
        g_release_violation = false;
    }
    App app(presenter, runtime);
    for (auto& view : app.views) view.next = nullptr;
    const auto start = std::chrono::steady_clock::now();
    EngageDynamic(presenter, app, "telemetry");
    const double standing_by = Seconds(std::chrono::steady_clock::now() - start);
    RunTicks(app, 1300ms, {1, 2, 2});
    const FrameGenStatus status = presenter.Status();
    presenter.Stop();
    const std::filesystem::path path = logger->ActiveLogPath();
    logger.reset(); // flushed
    std::ifstream file(path);
    std::vector<std::string> threaded, inline_lines;
    for (std::string line; std::getline(file, line);) {
        // Presenter-thread summaries, and those of frames passed straight
        // through (standing by, or the frame that engaged).
        if (line.find("Frame Generation: multiplier=dynamic") != std::string::npos) threaded.push_back(line);
        if (line.find("Frame Generation: multiplier=") != std::string::npos && line.find("threaded=0") != std::string::npos) {
            inline_lines.push_back(line);
        }
    }
    file.close();
    std::filesystem::remove_all(directory, ignored);
    Check(threaded.size() >= 3, "telemetry: summaries every window while engaged (" + std::to_string(threaded.size()) + ")");
    Check(!inline_lines.empty(), "telemetry: summaries while standing by");
    for (const char* field : {"appFps=", "outputFps=", "distinctFps=", "standbyS=", "generated=+", "copiesSkipped=",
                              "lateReal=", "gpuWaits=", "captureDoneMs=", "analysisMs=", "ofaMs=", "gpuStartMs=",
                              "gpuDoneMs=", "endCallMs=", "synthLockMs=", "unwritten=", "unready=", "pauses=",
                              "dynamic=engaged (the game ran at"}) {
        Check(threaded.back().find(field) != std::string::npos,
              std::string("telemetry: the summary has ") + field + " (" + threaded.back() + ")");
    }
    auto number = [](const std::string& line, const std::string& key) {
        const std::size_t at = line.find(key);
        Check(at != std::string::npos, "telemetry: " + key + " in " + line);
        return std::stod(line.substr(at + key.size()));
    };
    double generated = 0.0, skipped = 0.0, standby = 0.0;
    std::string standby_windows;
    for (const std::string& line : threaded) {
        generated += number(line, "generated=+");
        skipped += number(line, "copiesSkipped=");
        standby += number(line, "standbyS=");
        standby_windows += " " + std::to_string(number(line, "standbyS="));
    }
    for (const std::string& line : inline_lines) {
        standby += number(line, "standbyS=");
        standby_windows += " inline " + std::to_string(number(line, "standbyS="));
    }
    const std::string& last = threaded.back();
    // "generated=+N (M)": the window's, then the session's.
    const std::size_t generated_at = last.find("generated=+");
    const std::size_t total_at = last.find(" (", generated_at);
    Check(total_at != std::string::npos, "telemetry: the session's generated frames follow the window's");
    const double total = std::stod(last.substr(total_at + 2));
    Check(generated == total && total <= static_cast<double>(status.generated) && total > 0.0,
          "telemetry: the windows' generated frames add up to the session's (" + std::to_string(generated) + " of " +
              std::to_string(total) + ", status " + std::to_string(status.generated) + ")");
    Check(skipped <= static_cast<double>(status.copies_skipped), "telemetry: copies skipped mirror Status");
    Check(standby > 0.2 && standby <= standing_by + 0.1,
          "telemetry: standing-by time is reported (" + std::to_string(standby) + " of " + std::to_string(standing_by) +
              " s:" + standby_windows + ")");
    const double app_fps = number(last, "appFps=");
    Check(app_fps > 36.0 && app_fps < 52.0, "telemetry: appFps is the window's average (" + std::to_string(app_fps) + ")");
    Check(!status.dynamic_reason.empty() && last.find(status.dynamic_reason) != std::string::npos,
          "telemetry: the summary carries Status's dynamic reason");
    Check(!runtime.ordering_violation && !g_release_violation, "telemetry: calls stayed ordered");
}

// A game GPU-bound just past the half-rate cadence (1.05x two refreshes),
// its waits held for its GPU: the cadence counts from where it released
// each wait, so real frames stay at the GPU's own pace (2.1 refreshes), not
// a refresh later each time a GPU wait crosses one.
void TestGpuWaitKeepsCadence() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.game_gpu = std::chrono::microseconds(2 * kPeriod * 105 / 100 / 1000);
    gpu.frames_ahead = 4;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "always");
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "");
    app->before_release = [&] { gpu.GameFrame(); };
    EngageDynamic(presenter, *app, "gpu cadence");
    RunTicks(*app, 400ms, {1});
    std::vector<XrTime> predicted;
    RunTicks(*app, 1200ms, {1}, [&](const XrFrameState& state) {
        predicted.push_back(state.predictedDisplayTime);
        return false;
    });
    Check(predicted.size() > 20, "gpu cadence: frames ran");
    const double mean = static_cast<double>(predicted.back() - predicted.front()) /
                        static_cast<double>(predicted.size() - 1) / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    Check(status.gpu_waits > 0, "gpu cadence: the game's waits are held for its GPU");
    Check(mean >= 1.95 && mean <= 2.2, "gpu cadence: real frames come every " + std::to_string(mean) + " refreshes");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "gpu cadence: calls stayed ordered");
}

// Extrapolating, motion is needed a refresh after the real frame's own (its
// exact copy needs only its capture): analyses ending within that refresh
// are on time, and the guard's (d) never stands down for them.
void TestExtrapolateMeasureLagIsOnTime() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(GuardDynamic());
    EngageDynamic(presenter, *app, "measure lag on time");
    RunTicks(*app, 300ms, {1, 2, 2});
    gpu.measure_lag = std::chrono::microseconds(kPeriod * 6 / 10 / 1000);
    const FrameGenStatus before = presenter.Status();
    // Each frame ends about half a refresh before it is shown: measured
    // after its refresh is due, before the next.
    RunWork(*app, 1500ms, {7ms});
    const FrameGenStatus after = presenter.Status();
    Check(after.analyses_timed - before.analyses_timed > 20, "measure lag on time: analyses are timed");
    Check(after.analyses_late == before.analyses_late,
          "measure lag on time: none counts late (" + std::to_string(after.analyses_late - before.analyses_late) + " of " +
              std::to_string(after.analyses_timed - before.analyses_timed) + ")");
    Check(Engaged(presenter) && after.dynamic_stand_downs == 0,
          "measure lag on time: no stand-down (" + after.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "measure lag on time: calls stayed ordered");
}

// Pauses and loading frames (no projection) while engaged: each spoils a
// benefit guard window in a row (nothing new to show), but such windows say
// nothing about what generation adds, and nothing stands down.
void TestPauseAndLoadingDoNotStandDown() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(GuardDynamic());
    EngageDynamic(presenter, *app, "pause and loading");
    RunTicks(*app, 400ms, {1, 2, 2});
    for (int i = 0; i < 2; ++i) {
        SleepPrecise(std::chrono::steady_clock::now() + 1s);
        app->Frame();
    }
    for (int i = 0; i < 4; ++i) {
        app->Frame(false);
        SleepPrecise(std::chrono::steady_clock::now() + 100ms);
    }
    RunTicks(*app, 900ms, {1, 2, 2});
    const FrameGenStatus status = presenter.Status();
    Check(status.pauses >= 2, "pause and loading: the pauses are counted (" + std::to_string(status.pauses) + ")");
    Check(Engaged(presenter) && status.dynamic_stand_downs == 0,
          "pause and loading: nothing stands down (" + status.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "pause and loading: calls stayed ordered");
}

// The policy at 72, 90 and 120 Hz: engaged, a game able to hold the refresh
// rate is held at half of it where that is 40 fps or more, else (72 Hz) it
// runs at its own rate.
void TestDynamicCadenceByRefresh() {
    for (const int hz : {72, 90, 120}) {
        const std::string name = "cadence " + std::to_string(hz) + " Hz";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        runtime.period = 1'000'000'000 / hz;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
        EngageDynamic(presenter, *app, name);
        RunTicks(*app, 300ms, {1});
        const auto start = std::chrono::steady_clock::now();
        const int frames = RunTicks(*app, 800ms, {1});
        const double rate = frames / Seconds(std::chrono::steady_clock::now() - start);
        const double half = hz / 2.0;
        if (half >= 40.0) {
            Check(rate > half * 0.88 && rate < half * 1.1,
                  name + ": held at half the refresh rate (" + std::to_string(rate) + " fps)");
        } else {
            Check(rate > half * 1.3, name + ": not held at half the refresh rate (" + std::to_string(rate) + " fps)");
        }
        Check(Engaged(presenter), name + ": engaged");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

} // namespace

// FG_TEST_VERBOSE=1 names each test as it starts; FG_TEST_ONLY=<name> runs
// only the tests whose name contains it.
#define RUN(test)                                                                                                          do {                                                                                                                       const char* only = std::getenv("FG_TEST_ONLY");                                                                        if (only && std::string(#test).find(only) == std::string::npos) break;                                                 if (std::getenv("FG_TEST_VERBOSE")) std::cerr << #test << (g_jit ? " (just in time)" : "") << std::endl;              test();                                                                                                            } while (0)

// Dynamic stand-by and its benefit guard, GPU-aware pacing, pauses and
// telemetry.
void RunPolicyTests() {
    for (const bool jit : {false, true}) {
        g_jit = jit;
        RUN(TestCaptureOnlyWhileArmed);
    }
    g_jit = false;
    RUN(TestJitCaptureVsMeasureLag);
    RUN(TestGpuAwarePacing);
    RUN(TestUncapturedAtOwnRefreshIsLate);
    RUN(TestDynamicPassThroughNearRefresh);
    RUN(TestDynamicEngagesBelow);
    RUN(TestDynamicHalfRateLock);
    RUN(TestDynamicHysteresis);
    RUN(TestBenefitGuardBacksOff);
    RUN(TestDynamicProbe);
    RUN(TestExtrapolateDuplicateTargetGuard);
    RUN(TestNoExtrapolationAcrossPauses);
    RUN(TestLeadResetOnRestart);
    RUN(TestSkipIdenticalExactCopies);
    RUN(TestTelemetryWindows);
    RUN(TestGpuWaitKeepsCadence);
    RUN(TestExtrapolateMeasureLagIsOnTime);
    RUN(TestPauseAndLoadingDoNotStandDown);
    RUN(TestDynamicCadenceByRefresh);
}

// The policy tests in a second process of this executable, alongside the
// rest: CTest gives the executable one timeout, and each half runs about a
// minute against a paced runtime. The process is in a job that closes with
// this one, so a failure here ends it too. Null when it could not start.
HANDLE StartPolicyTests() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return nullptr;
    std::wstring command = L"\"" + std::wstring(path) + L"\" --policy-tests";
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup,
                        &process)) {
        if (job) CloseHandle(job);
        return nullptr;
    }
    // The job handle stays open until this process ends.
    if (job) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    return process.hProcess;
}

// FG_TEST_SERIAL=1 runs the policy tests after the rest, in this process.
int main(int argc, char** argv) {
    // An exception escaping a thread (the presenter's included) ends the run
    // with what it was, not a bare fail-fast.
    std::set_terminate([] {
        std::string what = "unknown";
        try {
            if (const auto exception = std::current_exception()) std::rethrow_exception(exception);
            what = "terminate without an exception";
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        std::cerr << "FAILED: terminate: " << what << std::endl;
        std::_Exit(3);
    });
    if (argc > 1 && std::string(argv[1]) == "--policy-tests") {
        RunPolicyTests();
        return 0;
    }
    const HANDLE policy = std::getenv("FG_TEST_SERIAL") ? nullptr : StartPolicyTests();
    RUN(TestInterpolationPacing);
    RUN(TestExtrapolationPacing);
    RUN(TestTripleInterpolation);
    RUN(TestQuadExtrapolation);
    RUN(TestSubmissionSpacing);
    RUN(TestOutputsKeptAcrossMultipliers);
    RUN(TestDepthCapture);
    RUN(TestAutomaticMultiplier);
    RUN(TestThreadedExtrapolation);
    RUN(TestThreadedDynamic);
    RUN(TestDynamicFreesOutputsNearBudget);
    RUN(TestThreadedPipelinedApp);
    RUN(TestThreadedInterpolation);
    RUN(TestJitExtrapolation);
    RUN(TestJitDynamicExtrapolation);
    RUN(TestJitInterpolation);
    RUN(TestJitDynamicInterpolation);
    RUN(TestJitWaitsForNoGameFrame);
    RUN(TestJitModeSwitchRestarts);
    RUN(TestToggleOffReadsOff);
    // The threaded presenter's lifecycle with frames written together, then
    // just in time.
    for (const bool jit : {false, true}) {
        g_jit = jit;
        RUN(TestThreadedToggleAndReset);
        RUN(TestThreadedPipelinedDouble);
        RUN(TestThreadedDestroyedSwapchain);
        RUN(TestThreadedRuntimeFailureRecovers);
        RUN(TestThreadedPauseRetryMsfsTiming);
        RUN(TestThreadedDestroyedSpace);
        RUN(TestThreadedKeepsOtherLayers);
        RUN(TestThreadedRepeatsAreNotDistinct);
    }
    g_jit = false;
    RUN(TestThreadedMsfsTiming);
    RUN(TestThreadedUnsupportedLayerPresentsInline);
    RUN(TestOverlappingWaitIsHeld);
    RUN(TestUnclaimedFrameIsDemoted);
    RUN(TestSameThreadDoubleWait);
    RUN(TestWaitThreadSeparateFromEndThread);
    RUN(TestRefusedCaptureUsage);
    RUN(TestOutputWaitTimeout);
    RUN(TestOutputFormatFallback);
    RUN(TestFailuresRetryAndToggle);
    RUN(TestUncapturedFramesAreSkipped);
    RUN(TestSettingsAndSessionChanges);
    RUN(TestUnavailableWithoutBackend);
    if (!policy) {
        RunPolicyTests();
    } else {
        DWORD code = 1;
        WaitForSingleObject(policy, INFINITE);
        GetExitCodeProcess(policy, &code);
        CloseHandle(policy);
        if (code != 0) {
            std::cerr << "FAILED: the policy tests (exit code " << code << ")" << std::endl;
            return 1;
        }
    }
    std::cout << "depthxr_framegen_presenter_tests passed\n";
    return 0;
}
