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
#include <iterator>
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
int g_wait_timeouts_on_create = 0; // g_wait_timeouts once the next output swapchain is made
bool g_release_violation = false;
std::map<XrSwapchain, int> g_releases; // images released per swapchain
// What the fake GPU drew into output images, as the eye position (x) the
// image was generated for: written ones in acquisition order, and the
// newest released one, which the runtime shows (NaN: released unwritten).
std::map<XrSwapchain, std::deque<float>> g_written;
std::map<XrSwapchain, float> g_shown;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_create_sizes; // each output swapchain's width and height

XrResult XRAPI_PTR Formats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* formats) {
    *count = 1;
    if (capacity) formats[0] = kFormat;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    std::scoped_lock lock(g_xr_mutex);
    *swapchain = reinterpret_cast<XrSwapchain>(++g_next_handle);
    ++g_creates;
    g_create_sizes.push_back({info->width, info->height});
    if (g_wait_timeouts_on_create) {
        g_wait_timeouts = g_wait_timeouts_on_create;
        g_wait_timeouts_on_create = 0;
    }
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
    bool idle_fails{false}; // WaitIdle reports GPU work that did not finish
    bool fail{false};
    bool uncaptured{false}; // views not captured at release: nothing generated
    bool threaded{false};
    bool depth{false};
    std::atomic<bool> memory_pressure{false}; // process video memory near its budget
    std::atomic<int> working_memory_queries{0}; // WorkingMemoryBytes calls
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
    // SynthesizeStale: supported, written, and asked of a motion frame that
    // was not the frame's previous one or not measured yet.
    std::atomic<bool> stale{false};
    std::atomic<int> stale_writes{0};
    std::atomic<int> stale_wrong_motion{0};
    // Quad views (SupportsComposition): frames composed per eye from their
    // views (under syntheses_mutex), with the composition each analysis was
    // measured with (FrameGenTuning::composition) by analyzed frame.
    bool composition{false};
    std::atomic<bool> compose_fail{false}; // SynthesizeComposed fails
    std::atomic<int> compose_failures{0};
    std::atomic<int> compose_calls{0}; // SynthesizeComposed called at all
    // SynthesizeComposed sleeps this long the first time it writes into an
    // output image (the real backend allocates its compose targets then,
    // under the caller's GPU lock).
    std::chrono::microseconds compose_first_delay{0};
    struct Composed {
        std::uint64_t frame{0};
        FrameGenSynthesisMode mode{FrameGenSynthesisMode::kInterpolate};
        float fraction{1.0f};
        FrameGenComposition composition;
        std::vector<XrSwapchain> outputs;
    };
    std::vector<Composed> composed;
    std::map<std::uint64_t, FrameGenComposition> analysis_compositions;
    std::atomic<int> composed_analyses{0};
    // Views are set up by an Execute whose views were not captured at their
    // release, as Direct3D 12's are: until then a view's captures fail, and
    // an Execute for another ring size (FrameGenHistoryRing) sets them up
    // again.
    bool setup_views{false};
    std::atomic<int> setups{0};
    // Not 0: the history ring quad views' analyses get instead of
    // FrameGenHistoryRing's (what a shallower ring would do).
    std::uint8_t quad_ring{0};

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
    // Writes refused because newer captures replaced the images they needed
    // in a view's history ring (FrameGenGpu::Writable said so beforehand).
    std::atomic<int> ring_refusals{0};
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
    bool Capture(std::size_t view, const FrameGenImage& source, const FrameGenViewRequest& request, FrameGenFlowQuality,
                 std::string& error) override {
        if (log_->setup_views) {
            std::scoped_lock lock(marks_mutex_);
            const auto it = set_up_.find(view);
            if (it == set_up_.end() || it->second.extent.width != request.extent.width ||
                it->second.extent.height != request.extent.height) {
                error.clear(); // not set up for this region yet
                return false;
            }
        }
        ++log_->captures;
        log_->last_capture = source;
        {
            // The view's history ring, as the real backend keeps it: the
            // capture writes the slot its next analysis calls its newest, and
            // frames analyzed from what that slot held cannot be written from
            // it any more.
            std::scoped_lock lock(ring_mutex_);
            Ring& ring = rings_[view];
            ++ring.generation[ring.newest];
        }
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
        // The views' history ring, as the real backend sizes it.
        const std::uint8_t ring_size =
            tuning.analyze_only && tuning.composition && log_->quad_ring ? log_->quad_ring : FrameGenHistoryRing(tuning);
        // The newest capture is when the analysis can start; its views were
        // captured at their release, or are read too late.
        TimePoint captured_at = std::chrono::steady_clock::now();
        bool uncaptured = false;
        {
            std::scoped_lock lock(marks_mutex_);
            if (!marks_.empty()) captured_at = std::max(captured_at, marks_.back().done);
            for (std::size_t i = 0; i < views.size(); ++i) {
                const auto it = captured_.find(i);
                // A view set up for another ring is set up anew: the real
                // backend replaces the images its captures went into.
                const auto set_up = set_up_.find(i);
                if (it == captured_.end() || it->second.first != views[i].source.swapchain ||
                    it->second.second != views[i].source.image_index ||
                    (log_->setup_views && (set_up == set_up_.end() || set_up->second.ring != ring_size))) {
                    ++log_->execute_uncaptured;
                    uncaptured = true;
                    break;
                }
            }
            if (uncaptured && log_->setup_views) {
                // Set up for these regions: the next releases are captured.
                for (std::size_t i = 0; i < views.size(); ++i) set_up_[i] = {views[i].request.extent, ring_size};
                captured_.clear();
                ++log_->setups;
                result.uncaptured = true;
                error = "not captured";
                return false;
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
                                  ring_size);
            const bool synthesized = history_[i].Effective(views[i].request).synthesize;
            history_[i].Commit(synthesized);
            result.synthesized.push_back(synthesized);
            result.depth.push_back(synthesized && views[i].request.depth && log_->depth);
            Analyzed::View kept{views[i].request.newest.position[0],
                                result.depth.back() ? views[i].request.shift[0] : 0.0f, synthesized};
            {
                // Its newest images are the slot just captured into, its
                // previous ones the slot before; the next capture takes the
                // slot after.
                std::scoped_lock lock(ring_mutex_);
                Ring& ring = rings_[i];
                if (ring.size != ring_size) {
                    // Made anew for another ring size: nothing analyzed
                    // before is written from it.
                    for (auto& generation : ring.generation) ++generation;
                    ring.size = ring_size;
                    ring.newest = 0;
                }
                kept.newest = ring.newest;
                kept.previous = static_cast<std::uint8_t>((ring.newest + ring_size - 1) % ring_size);
                kept.newest_generation = ring.generation[kept.newest];
                kept.previous_generation = ring.generation[kept.previous];
                ring.newest = static_cast<std::uint8_t>((ring.newest + 1) % ring_size);
            }
            if (tuning.analyze_only) {
                // Kept for Synthesize: where the eye is and how it moves.
                analyzed.views.push_back(kept);
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
            if (tuning.composition) {
                ++log_->composed_analyses;
                std::scoped_lock lock(log_->syntheses_mutex);
                log_->analysis_compositions[analyzed.id] = *tuning.composition;
            }
            std::scoped_lock lock(analyzed_mutex_);
            analyzed_.push_back(analyzed);
            while (analyzed_.size() > ring_size) analyzed_.pop_front();
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
    bool Writable(std::uint64_t frame, bool exact) const override {
        std::scoped_lock lock(analyzed_mutex_);
        const auto it = std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == frame; });
        return it != analyzed_.end() && WritableLocked(*it, exact);
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
            if (!WritableLocked(*it, !moved)) {
                ++log_->ring_refusals;
                ++log_->synthesis_failures;
                error = "the real frame's images were replaced by newer ones";
                return false;
            }
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
    bool SynthesizeStale(std::uint64_t frame, std::uint64_t motion, float fraction,
                         const std::vector<FrameGenImage>& outputs, std::string& error) override {
        if (!log_->stale) {
            error = "the fake backend does not move frames by older motion";
            return false;
        }
        Analyzed analyzed;
        const TimePoint now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock(analyzed_mutex_);
            const auto it = std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == frame; });
            const auto before =
                std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == motion; });
            if (it == analyzed_.end() || before == analyzed_.end() || outputs.size() < it->views.size() ||
                !WritableLocked(*it, false) || !WritableLocked(*before, true)) {
                ++log_->synthesis_failures;
                error = "the real frame or the one before it is no longer kept";
                return false;
            }
            if (now < it->captured_at) {
                ++log_->uncaptured_writes;
            }
            // Ids follow analysis order: the frame's previous is the id before.
            if (motion + 1 != frame || now < before->measured_at) {
                ++log_->stale_wrong_motion;
            }
            analyzed = *it;
        }
        {
            std::scoped_lock lock(log_->syntheses_mutex);
            log_->syntheses.push_back({frame, FrameGenSynthesisMode::kExtrapolate, fraction, now});
        }
        ++log_->stale_writes;
        // No eye shift: seen from the real frame's own eye position.
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t i = 0; i < analyzed.views.size(); ++i) {
            g_written[outputs[i].swapchain].push_back(analyzed.views[i].x);
        }
        return true;
    }
    bool SupportsComposition() const override { return log_->composition; }
    // Each eye's output shows its periphery view's frame, from where that
    // view's eye is at the frame's time (as Synthesize's outputs do).
    bool SynthesizeComposed(std::uint64_t frame, FrameGenSynthesisMode mode, float fraction,
                            const FrameGenComposition& composition, const std::vector<FrameGenImage>& outputs,
                            std::string& error) override {
        ++log_->compose_calls;
        if (log_->compose_fail) {
            ++log_->compose_failures;
            error = "fake compose failure";
            return false;
        }
        if (log_->compose_first_delay.count()) {
            bool first = false;
            for (const FrameGenImage& output : outputs) first = composed_into_.insert(output.swapchain).second || first;
            if (first) SleepPrecise(std::chrono::steady_clock::now() + log_->compose_first_delay);
        }
        Analyzed analyzed;
        const TimePoint now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock(analyzed_mutex_);
            const auto it = std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == frame; });
            bool views_known = it != analyzed_.end() && outputs.size() >= composition.eyes.size();
            for (const FrameGenComposeEye& eye : composition.eyes) {
                views_known = views_known && eye.peripheral < it->views.size() && eye.focus < it->views.size();
            }
            if (!views_known) {
                ++log_->synthesis_failures;
                error = "the real frame is no longer kept";
                return false;
            }
            const bool moved = mode == FrameGenSynthesisMode::kExtrapolate ? fraction > 0.0f : fraction < 1.0f;
            if (!WritableLocked(*it, !moved)) {
                ++log_->ring_refusals;
                ++log_->synthesis_failures;
                error = "the real frame's images were replaced by newer ones";
                return false;
            }
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
            GpuLog::Composed entry{frame, mode, fraction, composition, {}};
            for (const FrameGenImage& output : outputs) entry.outputs.push_back(output.swapchain);
            log_->composed.push_back(std::move(entry));
        }
        const float eye_time = mode == FrameGenSynthesisMode::kExtrapolate ? fraction : fraction - 1.0f;
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t e = 0; e < composition.eyes.size(); ++e) {
            const auto& view = analyzed.views[composition.eyes[e].peripheral];
            g_written[outputs[e].swapchain].push_back(view.x + (view.synthesized ? eye_time * view.shift : 0.0f));
        }
        return true;
    }
    bool SynthesizeComposedStale(std::uint64_t frame, std::uint64_t motion, float fraction,
                                 const FrameGenComposition& composition, const std::vector<FrameGenImage>& outputs,
                                 std::string& error) override {
        if (!log_->stale) {
            error = "the fake backend does not move frames by older motion";
            return false;
        }
        ++log_->compose_calls;
        Analyzed analyzed;
        const TimePoint now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock(analyzed_mutex_);
            const auto it = std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == frame; });
            const auto before =
                std::find_if(analyzed_.begin(), analyzed_.end(), [&](const Analyzed& a) { return a.id == motion; });
            bool views_known = it != analyzed_.end() && before != analyzed_.end() && outputs.size() >= composition.eyes.size();
            for (const FrameGenComposeEye& eye : composition.eyes) {
                views_known = views_known && eye.peripheral < it->views.size() && eye.focus < it->views.size();
            }
            if (!views_known || !WritableLocked(*it, false) || !WritableLocked(*before, true)) {
                ++log_->synthesis_failures;
                error = "the real frame or the one before it is no longer kept";
                return false;
            }
            if (now < it->captured_at) {
                ++log_->uncaptured_writes;
            }
            if (motion + 1 != frame || now < before->measured_at) {
                ++log_->stale_wrong_motion;
            }
            analyzed = *it;
        }
        {
            std::scoped_lock lock(log_->syntheses_mutex);
            log_->syntheses.push_back({frame, FrameGenSynthesisMode::kExtrapolate, fraction, now});
            GpuLog::Composed entry{frame, FrameGenSynthesisMode::kExtrapolate, fraction, composition, {}};
            for (const FrameGenImage& output : outputs) entry.outputs.push_back(output.swapchain);
            log_->composed.push_back(std::move(entry));
        }
        ++log_->stale_writes;
        std::scoped_lock lock(g_xr_mutex);
        for (std::size_t e = 0; e < composition.eyes.size(); ++e) {
            g_written[outputs[e].swapchain].push_back(analyzed.views[composition.eyes[e].peripheral].x);
        }
        return true;
    }
    bool WaitIdle() override {
        ++log_->wait_idles;
        return !log_->idle_fails;
    }
    void ResetHistory() override { history_.clear(); }
    void Reset() override {}
    double LastGpuMilliseconds() const override { return 0.75; }
    bool SupportsThreadedPresentation() const override { return log_->threaded; }
    std::optional<FrameGenMemoryBudget> MemoryBudget() const override {
        constexpr std::uint64_t gigabyte = 1024ull * 1024 * 1024;
        return FrameGenMemoryBudget{log_->memory_pressure ? 31 * gigabyte + gigabyte / 2 : 16 * gigabyte, 32 * gigabyte};
    }
    std::uint64_t WorkingMemoryBytes() const override {
        ++log_->working_memory_queries;
        return 64ull * 1024 * 1024;
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
            // Its ring slots and their generations when analyzed.
            std::uint8_t newest{0};
            std::uint8_t previous{0};
            std::uint32_t newest_generation{0};
            std::uint32_t previous_generation{0};
        };
        std::vector<View> views;
    };
    // A view's history ring: each slot's generation, the slot the next
    // capture writes, and its slots (FrameGenHistoryRing).
    struct Ring {
        std::array<std::uint32_t, kFrameGenHistoryRingMax> generation{};
        std::uint8_t newest{0};
        std::uint8_t size{0};
    };
    // As the real backend's KeptFrame: every view's newest images still in
    // its ring, and unless `exact` its previous ones too (where it was
    // synthesized against them).
    bool WritableLocked(const Analyzed& analyzed, bool exact) const {
        std::scoped_lock lock(ring_mutex_);
        for (std::size_t i = 0; i < analyzed.views.size(); ++i) {
            const auto ring = rings_.find(i);
            if (ring == rings_.end()) return false;
            const Analyzed::View& view = analyzed.views[i];
            if (ring->second.generation[view.newest] != view.newest_generation ||
                (!exact && view.synthesized && ring->second.generation[view.previous] != view.previous_generation)) {
                return false;
            }
        }
        return true;
    }
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
    struct SetUp {
        FrameGenExtent extent;
        std::uint8_t ring{0};
    };
    std::map<std::size_t, SetUp> set_up_; // GpuLog::setup_views: each view's region and ring
    mutable std::mutex ring_mutex_;
    std::map<std::size_t, Ring> rings_; // per view
    std::set<XrSwapchain> composed_into_; // output images SynthesizeComposed has written (compose_first_delay)
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
    // Paced: refresh N is at epoch + N periods, and is shown at display time
    // display_base + N periods (a wait released at it is for the next). A
    // new refresh period (change_period, taken by the next wait) counts
    // from the last refresh released.
    std::chrono::steady_clock::time_point epoch{std::chrono::steady_clock::now()};
    XrTime display_base{1'000'000'000};
    std::atomic<XrDuration> change_period{0};
    std::int64_t last_tick{0};
    // The next wait alone reports this period (0: none).
    std::atomic<XrDuration> next_period{0};
    // Paced waits released within max(1 ms, a tenth of a refresh) of being
    // asked for, and waits the echo (below) returned with a multiple.
    std::atomic<int> quick_releases{0};
    std::atomic<int> echoed_multiples{0};
    std::atomic<int> outstanding_waits{0};
    std::atomic<bool> ordering_violation{false};
    std::atomic<bool> fail_next_submit{false};
    std::atomic<bool> fail_next_wait{false};
    // False: the headset is off (waits report shouldRender false).
    std::atomic<bool> should_render{true};
    // Frames begun and never ended, discarded by the next begin
    // (XR_FRAME_DISCARDED): the runtime showed its last frame again.
    std::atomic<int> discarded{0};
    std::atomic<bool> used_destroyed{false};
    std::vector<XrSwapchain> destroyed; // under g_xr_mutex
    std::vector<XrSpace> destroyed_spaces; // under g_xr_mutex
    std::atomic<bool> used_destroyed_space{false};
    // Submissions showing an output image generated for another eye position
    // than the views submitted with it (another frame's, or none).
    std::atomic<int> mismatched_images{0};
    // Projections submitted with other than two views (a stereo runtime's).
    std::atomic<int> not_stereo{0};
    std::atomic<int> wait_sleep_ms{0};
    // Runtime motion smoothing (SteamVR Motion Smoothing, Meta ASW, Virtual
    // Desktop SSW, Pimax Smart Smoothing) holds the app to half the refresh
    // rate (or a third, with throttle_refreshes 3) through its waits and
    // fills the other refreshes itself. How a runtime reports that is not
    // documented, so each way is modeled (paced runtimes only): kWaits
    // releases waits throttle_refreshes refreshes apart and reports the
    // refresh period unchanged; kPeriod also reports throttle_refreshes times
    // the period. `throttled` turns it on and off; with throttle_on_after set
    // the runtime decides itself, as one judging the frames it gets: on after
    // that many late frames (a frame ended after its display time, or less
    // than deadline_lead before it), off after throttle_off_after frames in
    // a row on time. A wait asked for after the refresh it would be held to
    // is released at the next one, unless throttle_late_at_once: then at
    // once, for the refresh just past (on the refresh grid).
    enum class Throttle { kWaits, kPeriod };
    Throttle throttle{Throttle::kWaits};
    int throttle_refreshes{2}; // set before the first wait
    std::atomic<bool> throttled{false};
    int throttle_on_after{0};  // 0: only `throttled` decides (set before the first wait)
    int throttle_off_after{0};
    XrDuration deadline_lead{0};
    bool throttle_late_at_once{false}; // set before the first wait
    // SteamVR with Pimax's headset driver, as SteamVR 2.17.10 was seen to
    // behave in a flight (paced runtimes only, a throttle going first): a
    // wait asked for before the refresh after the last one's release blocks
    // until that refresh, as a paced runtime's does. One asked for later
    // returns at once, for a display time as far after the last as its
    // caller took (off the refresh grid), and reports its caller's own
    // interval since the last release, rounded to whole refreshes, as the
    // period: at most kEchoMostRefreshes, and one again after a gap over
    // kEchoResetAfter. It holds nothing back; the period only echoes the
    // caller's own pace. The cap and the reset are approximate: in the
    // flight, intervals of 46-49 ms were reported as five refreshes (where
    // rounding gives four), and gaps from about 50 ms mostly came back as
    // one. Tests pin how such periods are handled, not where the runtime's
    // limits lie.
    static constexpr std::int64_t kEchoMostRefreshes = 5;
    static constexpr std::chrono::milliseconds kEchoResetAfter{60};
    std::atomic<bool> echo{false};
    std::chrono::steady_clock::time_point last_release{};
    // Varjo's runtime (4.14.2, an Aero, in DCS), as seen in a flight (paced
    // runtimes only): a wait asked for more than clock_jump_after after the
    // last one's release (the game loading a mission) returns a display time
    // far past its refresh's, clock_jump_factor times that gap ahead. From
    // then on display times never go back: each wait still blocks until its
    // refresh, but reports its display time only kClockCreep past the last
    // until the refreshes' own times pass it again (in the flight, +52 s
    // after a 26 s gap, then a minute of 10 µs steps). jump_next does the
    // same once, at the next wait, by that much, without a gap.
    static constexpr XrDuration kClockCreep = 10'000;
    std::chrono::milliseconds clock_jump_after{0}; // 0: off (set before the first wait)
    double clock_jump_factor{2.0};
    std::atomic<XrDuration> jump_next{0};
    // SteamVR (2.17.10, Pimax's headset driver) after a wait asked for more
    // than lag_after_gap after the last one's release: it reports a display
    // time only a refresh after the last, as if no time had passed, and the
    // next wait its refresh's own again: display times lag, then catch up
    // (in flights, by up to two seconds). Nothing ran ahead.
    std::chrono::milliseconds lag_after_gap{0}; // 0: off (set before the first wait)
    std::atomic<int> lagged_waits{0};
    // The next wait alone reports a display time this much earlier than its
    // refresh's (a session's first wait seen late, or a runtime lagging once).
    std::atomic<XrDuration> lag_next{0};
    // While set, display times stop moving (each wait still blocks a
    // refresh): SteamVR returning the same display time during a game's load.
    // Cleared, they are their refreshes' own again at once.
    std::atomic<bool> stall_clock{false};
    XrTime clock_floor{0};          // under g_xr_mutex: the last display time reported
    std::atomic<int> crept_waits{0}; // waits reported at the floor instead of their refresh's time
    // Each submission takes this long (a runtime slow to take frames, or a
    // caller held up after each): its caller asks for its next wait late.
    std::atomic<int> submit_sleep_us{0};
    // The next submission alone takes this much longer (its caller's hitch).
    std::atomic<int> stall_next_submit_us{0};
    // The next wait that blocks (paced runtimes only) alone returns this long
    // after its refresh: its caller's thread woken late, as a system-wide
    // hitch holds it. The runtime released it on time (last_release).
    std::atomic<int> oversleep_next_release_us{0};
    // The next xrBeginFrame alone takes this long: the frame it begins goes
    // to xrEndFrame late. Under g_xr_mutex: that frame's index in
    // submitted_at (-1: none yet).
    std::atomic<int> stall_next_begin_us{0};
    std::int64_t stalled_submission{-1};
    // A wait first waits for the frame begun before it to end, as a runtime
    // may (paced runtimes only): a game that waits for its next frame while
    // it renders this one, as MSFS does, is then released at the first
    // refresh after that frame ends.
    std::atomic<bool> wait_for_end{false};
    std::condition_variable ended_cv; // with g_xr_mutex: a frame ended
    // Under g_xr_mutex: late frames this session, on/off switches, and the
    // automatic throttle's counts.
    int late_frames{0};
    int throttle_switches{0};
    int late_count{0};
    int on_time_in_row{0};
    // Every Nth wait misses a refresh: it is released a refresh later than
    // asked for (paced runtimes only; 0: none).
    std::atomic<int> miss_every{0};
    // ...and reports twice the period for that wait, as SteamVR with Pimax's
    // driver does after its compositor misses a refresh.
    std::atomic<bool> miss_reports_multiple{false};
    std::int64_t paced_waits{0};
    // A runtime holding a call (as one may while its session stops, until
    // the session ends): the next waits (hold_waits) or submissions
    // (hold_submits) block until ReleaseHolds; a held wait then fails, as a
    // runtime's whose session ended.
    std::mutex hold_mutex;
    std::condition_variable hold_cv;
    bool hold_waits{false};
    bool hold_submits{false};
    std::atomic<int> held{0}; // calls inside a hold now
    bool begun{false};

    void HoldWaits() {
        std::scoped_lock lock(hold_mutex);
        hold_waits = true;
    }
    void HoldSubmits() {
        std::scoped_lock lock(hold_mutex);
        hold_submits = true;
    }
    void ReleaseHolds() {
        {
            std::scoped_lock lock(hold_mutex);
            hold_waits = hold_submits = false;
        }
        hold_cv.notify_all();
    }
    // Whether a call was held (it returned once released).
    bool HoldIf(bool Runtime::*flag) {
        std::unique_lock lock(hold_mutex);
        if (!(this->*flag)) return false;
        ++held;
        hold_cv.wait(lock, [&] { return !(this->*flag); });
        --held;
        return true;
    }
    // Until a call is inside a hold, at most `limit`.
    bool WaitHeld(std::chrono::milliseconds limit) {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (held.load() == 0) {
            if (std::chrono::steady_clock::now() > until) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }
    // The automatic throttle (under g_xr_mutex): late frames turn it on,
    // unless a run of frames on time came between them.
    void JudgeFrame(bool late) {
        late_frames += late ? 1 : 0;
        on_time_in_row = late ? 0 : on_time_in_row + 1;
        if (throttle_on_after <= 0) return;
        if (!throttled) {
            late_count = on_time_in_row >= throttle_off_after ? 0 : late_count + (late ? 1 : 0);
            if (late_count >= throttle_on_after) {
                throttled = true;
                ++throttle_switches;
                late_count = on_time_in_row = 0;
            }
        } else if (on_time_in_row >= throttle_off_after) {
            throttled = false;
            ++throttle_switches;
            on_time_in_row = 0;
        }
    }
    int LateFrames() {
        std::scoped_lock lock(g_xr_mutex);
        return late_frames;
    }
    int ThrottleSwitches() {
        std::scoped_lock lock(g_xr_mutex);
        return throttle_switches;
    }
    std::vector<XrTime> submitted_times;
    std::vector<std::chrono::steady_clock::time_point> submitted_at;
    std::vector<std::vector<XrSwapchain>> submitted_swapchains;
    std::vector<bool> submitted_depth;
    std::vector<float> submitted_left_x; // the first projection view's position

    FrameGenRuntimeCalls Calls() {
        FrameGenRuntimeCalls calls;
        calls.wait = [this](XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
            if (fail_next_wait.exchange(false)) return XR_ERROR_RUNTIME_FAILURE;
            if (HoldIf(&Runtime::hold_waits)) return XR_ERROR_SESSION_NOT_RUNNING;
            if (++outstanding_waits > 1) ordering_violation = true;
            if (wait_sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(wait_sleep_ms));
            const auto report = [&](XrTime display_time, XrDuration reported) {
                state->predictedDisplayTime = display_time;
                const XrDuration once = next_period.exchange(0);
                state->predictedDisplayPeriod = once ? once : reported;
                state->shouldRender = should_render ? XR_TRUE : XR_FALSE;
                return XR_SUCCESS;
            };
            if (paced) {
                if (wait_for_end) {
                    std::unique_lock lock(g_xr_mutex);
                    ended_cv.wait_for(lock, 1s, [&] { return !begun; });
                }
                if (const XrDuration changed = change_period.exchange(0)) {
                    std::scoped_lock lock(g_xr_mutex);
                    epoch += std::chrono::nanoseconds(last_tick * period);
                    display_base += last_tick * period;
                    last_tick = 0;
                    period = changed;
                }
                const auto refresh = std::chrono::nanoseconds(period);
                const auto quick = std::max<std::chrono::nanoseconds>(1ms, refresh / 10);
                const bool throttling = throttled.load();
                const auto asked = std::chrono::steady_clock::now();
                if (echo && !throttling && last_release != std::chrono::steady_clock::time_point{} &&
                    (asked - epoch) / refresh > last_tick) {
                    const auto interval = asked - last_release;
                    std::int64_t refreshes = (interval + refresh / 2) / refresh;
                    if (refreshes < 1 || interval > kEchoResetAfter) refreshes = 1;
                    refreshes = std::min(refreshes, kEchoMostRefreshes);
                    last_tick = (asked - epoch) / refresh;
                    last_release = asked;
                    ++quick_releases;
                    echoed_multiples += refreshes >= 2 ? 1 : 0;
                    return report(display_base + std::chrono::duration_cast<std::chrono::nanoseconds>(asked - epoch).count() +
                                      period,
                                  refreshes * period);
                }
                const auto previous_release = last_release;
                const bool miss = miss_every > 0 && ++paced_waits % miss_every == 0;
                const std::int64_t held_to = last_tick + (throttling ? throttle_refreshes : 1);
                std::int64_t tick = (asked - epoch) / refresh + (miss ? 2 : 1);
                if (throttling && throttle_late_at_once && (asked - epoch) / refresh >= held_to) {
                    tick = (asked - epoch) / refresh;
                } else {
                    tick = std::max(tick, held_to);
                }
                last_tick = tick;
                last_release = epoch + tick * refresh;
                quick_releases += last_release - asked < quick ? 1 : 0;
                SleepPrecise(epoch + tick * refresh);
                if (const int over = oversleep_next_release_us.exchange(0)) {
                    SleepPrecise(epoch + tick * refresh + std::chrono::microseconds(over));
                }
                XrTime display = display_base + (tick + 1) * period;
                const XrDuration jump = jump_next.exchange(0);
                {
                    std::scoped_lock lock(g_xr_mutex);
                    const auto gap = asked - previous_release;
                    if (clock_jump_after > 0ms && previous_release != std::chrono::steady_clock::time_point{} &&
                        gap > clock_jump_after) {
                        const auto ahead = static_cast<XrDuration>(
                            clock_jump_factor * static_cast<double>(std::chrono::nanoseconds(gap).count()));
                        clock_floor = std::max(clock_floor, display + ahead);
                    }
                    if (jump > 0) clock_floor = std::max(clock_floor, display + jump);
                    if (lag_after_gap > 0ms && clock_floor > 0 && previous_release != std::chrono::steady_clock::time_point{} &&
                        gap > lag_after_gap && clock_floor + period < display) {
                        display = clock_floor + period;
                        ++lagged_waits;
                    }
                    if (const XrDuration lag = lag_next.exchange(0)) {
                        display = std::max(clock_floor + kClockCreep, display - lag);
                    }
                    if (display <= clock_floor || (stall_clock && clock_floor > 0)) {
                        display = clock_floor + kClockCreep;
                        ++crept_waits;
                    }
                    clock_floor = display;
                }
                return report(display, throttling && throttle == Throttle::kPeriod ? throttle_refreshes * period
                                       : miss && miss_reports_multiple   ? 2 * period
                                                                         : period);
            }
            return report(next_display.fetch_add(period), period);
        };
        calls.begin = [this](XrSession, const XrFrameBeginInfo*) {
            if (outstanding_waits.load() != 1) ordering_violation = true;
            --outstanding_waits;
            if (const int stall = stall_next_begin_us.exchange(0)) {
                {
                    std::scoped_lock lock(g_xr_mutex);
                    stalled_submission = static_cast<std::int64_t>(submitted_at.size());
                }
                SleepPrecise(std::chrono::steady_clock::now() + std::chrono::microseconds(stall));
            }
            std::scoped_lock lock(g_xr_mutex);
            const bool open = begun;
            begun = true;
            if (open) {
                // A frame begun and never ended: discarded, as OpenXR does
                // (the runtime keeps showing its last frame).
                ++discarded;
                return XR_FRAME_DISCARDED;
            }
            return XR_SUCCESS;
        };
        calls.submit = [this](XrSession, const XrFrameEndInfo* info) {
            HoldIf(&Runtime::hold_submits);
            if (const int sleep = submit_sleep_us + stall_next_submit_us.exchange(0)) {
                SleepPrecise(std::chrono::steady_clock::now() + std::chrono::microseconds(sleep));
            }
            std::scoped_lock lock(g_xr_mutex);
            if (!begun) ordering_violation = true;
            begun = false;
            ended_cv.notify_all();
            if (fail_next_submit.exchange(false)) return XR_ERROR_TIME_INVALID;
            if (paced) {
                // Display time t is shown at epoch + (t - display_base) (the
                // wait for it was released a refresh before).
                const auto shown = epoch + std::chrono::nanoseconds(info->displayTime - display_base);
                JudgeFrame(std::chrono::steady_clock::now() + std::chrono::nanoseconds(deadline_lead) > shown);
            }
            submitted_times.push_back(info->displayTime);
            submitted_at.push_back(std::chrono::steady_clock::now());
            for (std::uint32_t layer = 0; layer < info->layerCount; ++layer) {
                std::vector<XrSwapchain> used;
                XrSpace space = XR_NULL_HANDLE;
                if (info->layers[layer]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[layer]);
                    for (std::uint32_t v = 0; v < p->viewCount; ++v) used.push_back(p->views[v].subImage.swapchain);
                    space = p->space;
                    not_stereo += p->viewCount != 2 ? 1 : 0;
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

// As QuickDynamic, for a game that must stay standing by: a hitch lowers
// its rate (measured over a second) for that second, and standing by
// engages only after longer than that below the refresh rate (as with the
// default policy).
FrameGenDynamicPolicy SteadyDynamic() {
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.engage_after = 1200ms;
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
                    FrameGenMultiplier multiplier = FrameGenMultiplier::k2, Logger* logger = nullptr) {
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = mode;
    settings.multiplier = multiplier;
    presenter.Configure(settings);
    presenter.SetEngagementPolicy(3, 0ms);
    presenter.SetDynamicPolicy(QuickDynamic());
    presenter.Start(kSession, nullptr, Dispatch(), logger, nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    presenter.TrackSwapchain(kLeft, format, 1, usage);
    presenter.TrackSwapchain(kRight, format, 1, usage);
    std::scoped_lock lock(g_xr_mutex);
    g_creates = 0;
    g_create_sizes.clear();
    g_destroyed.clear();
    g_wait_timeouts = 0;
    g_wait_timeouts_on_create = 0;
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

// The backend's working memory (a driver query per image it holds) feeds
// only the log: asked for at most once a second, not on every real frame.
void TestWorkingMemoryAskedOncePerSecond() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kInterpolate);
    App app(presenter, runtime);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i) {
        app.Frame();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const int executes = gpu.executes.load();
    const int queries = gpu.working_memory_queries.load();
    Check(executes >= 30, "working memory: frames were generated (" + std::to_string(executes) + ")");
    Check(queries >= 1 && queries <= 1 + static_cast<int>(seconds),
          "working memory: asked " + std::to_string(queries) + " times for " + std::to_string(executes) +
              " executes in " + std::to_string(seconds) + " s");
    presenter.Stop();
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
          "auto: an app that holds full refresh runs without generation (" +
              std::string(ToString(presenter.Status().state)) + ", " + presenter.Status().reason + ", " +
              std::to_string(presenter.Status().multiplier) + "x)");
    Check(app.Frame().predictedDisplayPeriod == kPeriod, "auto: full rate passes frames through");
    Check(presenter.OsdText().find("Standby  /  Auto") != std::string::npos && presenter.OsdDescription().badge == "STBY",
          "auto: at full rate the OSD says Auto stands by (" + presenter.OsdText() + ")");

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
                // Paced like a runtime paces it: told the refresh period.
                Check(state.predictedDisplayPeriod == kPeriod,
                      "dynamic: the app is told the refresh period (" + std::to_string(state.predictedDisplayPeriod) +
                          ")");
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

// Until the presenter thread has stopped (a thread left behind ends on its
// own once its call returns), at most `limit`.
bool PresenterStops(FrameGenPresenter& presenter, std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (presenter.PresenterActivity().rfind("presenter thread stopped", 0) != 0) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(2ms);
    }
    std::this_thread::sleep_for(50ms); // its last steps after it says so
    return true;
}

// DCS, quitting with Frame Generation presenting from its own thread
// (2026-10-09): the game hung in its teardown. A runtime holding the
// presenter thread inside xrEndFrame (which holds submit_mutex_) never holds
// the game's teardown past the teardown wait: a swapchain destroyed meanwhile
// is forgotten and kept (the later ones only try briefly), ending the session
// leaves the thread stopping, and stopping leaves it behind with what it
// uses (logged with the call it is in). Once the runtime lets it go it ends
// on its own; Frame Generation stays off for the rest of the process.
void TestTeardownPresenterHeldInSubmit() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("vectorxr-presenter-teardown-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    auto logger = std::make_unique<Logger>();
    logger->Initialize(directory / "teardown.log");
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    gpu.threaded = true;
    gpu.jit = g_jit;
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
    settings.multiplier = FrameGenMultiplier::k2;
    presenter.Configure(settings);
    presenter.SetEngagementPolicy(3, 0ms);
    presenter.SetTeardownWait(300ms);
    presenter.Start(kSession, nullptr, Dispatch(), logger.get(), nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    presenter.TrackSwapchain(kLeft, kFormat, 1, kAppUsage);
    presenter.TrackSwapchain(kRight, kFormat, 1, kAppUsage);
    {
        std::scoped_lock lock(g_xr_mutex);
        g_destroyed.clear();
        g_release_violation = false;
    }
    App app(presenter, runtime);
    for (int i = 0; i < 12; ++i) app.Frame();
    Check(presenter.Status().threaded, "held submit: presenting from its own thread");
    runtime.HoldSubmits();
    Check(runtime.WaitHeld(2000ms), "held submit: the runtime holds the presenter thread in xrEndFrame");
    const std::string activity = presenter.PresenterActivity();
    Check(activity.find("in the runtime's xrEndFrame for") != std::string::npos,
          "held submit: what the presenter thread is in is told (" + activity + ")");

    auto start = std::chrono::steady_clock::now();
    Check(!presenter.ForgetSwapchain(kLeft), "held submit: a swapchain destroyed meanwhile is kept");
    const auto first = std::chrono::steady_clock::now() - start;
    Check(first >= 250ms && first < 1000ms,
          "held submit: the destroy waits the teardown wait, no more (" + std::to_string(Seconds(first)) + " s)");
    start = std::chrono::steady_clock::now();
    Check(!presenter.ForgetSwapchain(kRight), "held submit: the next swapchain is kept too");
    Check(std::chrono::steady_clock::now() - start < 200ms, "held submit: later destroys only try briefly");
    start = std::chrono::steady_clock::now();
    Check(!presenter.ForgetSpace(kSpace), "held submit: a space destroyed meanwhile is kept");
    Check(std::chrono::steady_clock::now() - start < 200ms, "held submit: so does the space's");
    start = std::chrono::steady_clock::now();
    Check(!presenter.ResetFrames(), "held submit: ending the session leaves the thread stopping");
    Check(std::chrono::steady_clock::now() - start < 1000ms, "held submit: ending the session waits no more");
    start = std::chrono::steady_clock::now();
    presenter.Stop();
    Check(std::chrono::steady_clock::now() - start < 1000ms, "held submit: stopping waits no more");
    Check(presenter.PresenterAbandoned() && !presenter.Started(), "held submit: the thread is left behind");

    // The runtime lets it go: it ends on its own.
    runtime.ReleaseHolds();
    Check(PresenterStops(presenter, 2000ms), "held submit: the thread left behind ends once its call returns (" +
                                                 presenter.PresenterActivity() + ")");
    // Frame Generation stays off for the process.
    presenter.Start(kSession, nullptr, Dispatch(), logger.get(), nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    const FrameGenStatus after = presenter.Status();
    Check(after.state == FrameGenState::kUnavailable && !presenter.OwnsPacing(),
          "held submit: Frame Generation stays off for the process (" + after.reason + ")");
    const std::filesystem::path path = logger->ActiveLogPath();
    logger.reset(); // flushed
    std::ifstream file(path);
    const std::string log((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (const char* line : {"a swapchain being destroyed waited 300 ms for its presenter thread (presenter thread",
                             "its presenter thread has not stopped after",
                             "Frame Generation leaves its presenter thread behind: it never left the runtime",
                             "in the runtime's xrEndFrame for",
                             "Frame Generation stays off for this process"}) {
        Check(log.find(line) != std::string::npos, std::string("held submit: the log tells \"") + line + "\"");
    }
    file.close();
    std::filesystem::remove_all(directory, ignored);
}

// A runtime holding the presenter thread inside xrWaitFrame until its
// session ends (a runtime may hold its waits while the session stops): a
// stop gives up within the teardown wait; once the session ends (the held
// wait fails) the thread stops, nothing is left behind, and the next
// session presents again.
// A session whose teardown found GPU work unfinished keeps its swapchains,
// for the runtime to free with the session. The next session, here one
// without Frame Generation (never Started), owes that work nothing: its
// swapchains are destroyed downstream as usual.
void TestStalledTeardownDoesNotKeepLaterSwapchains() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    StartPresenter(presenter, gpu, FrameGenSynthesisMode::kExtrapolate);
    gpu.idle_fails = true;
    presenter.Stop();
    Check(!presenter.ForgetSwapchain(kLeft), "stalled teardown: the session's own swapchain is kept");
    gpu.idle_fails = false;
    presenter.BeginSession();
    Check(presenter.ForgetSwapchain(kRight), "stalled teardown: a later session's swapchain is destroyed");
    Check(presenter.ForgetSwapchain(reinterpret_cast<XrSwapchain>(0x77)),
          "stalled teardown: an untracked later swapchain is destroyed");
}

void TestTeardownPresenterReleasedBySessionEnd() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    presenter.SetTeardownWait(300ms);
    App app(presenter, runtime);
    for (int i = 0; i < 12; ++i) app.Frame();
    Check(presenter.Status().threaded, "held wait: presenting from its own thread");
    runtime.HoldWaits();
    Check(runtime.WaitHeld(2000ms), "held wait: the runtime holds the presenter thread in xrWaitFrame");
    Check(presenter.PresenterActivity().find("in the runtime's xrWaitFrame for") != std::string::npos,
          "held wait: what the presenter thread is in is told (" + presenter.PresenterActivity() + ")");
    // Destroying a swapchain does not wait for a thread holding nothing it needs.
    auto start = std::chrono::steady_clock::now();
    Check(presenter.ForgetSwapchain(kLeft), "held wait: a swapchain destroyed meanwhile goes");
    Check(std::chrono::steady_clock::now() - start < 200ms, "held wait: at once");
    start = std::chrono::steady_clock::now();
    Check(!presenter.StopPresenter(), "held wait: the stop gives up while the runtime holds the thread");
    const auto waited = std::chrono::steady_clock::now() - start;
    Check(waited >= 250ms && waited < 1000ms,
          "held wait: after the teardown wait (" + std::to_string(Seconds(waited)) + " s)");
    runtime.ReleaseHolds(); // the session ended
    start = std::chrono::steady_clock::now();
    Check(presenter.StopPresenter() && !presenter.PresenterAbandoned(),
          "held wait: the thread stops once the runtime lets its wait go");
    Check(std::chrono::steady_clock::now() - start < 300ms, "held wait: at once");
    start = std::chrono::steady_clock::now();
    presenter.Stop();
    Check(std::chrono::steady_clock::now() - start < 500ms && !presenter.PresenterAbandoned(),
          "held wait: the session stops normally");
    // The next session presents from its own thread again.
    Runtime next;
    next.paced = true;
    StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
    App again(presenter, next);
    for (int i = 0; i < 12; ++i) again.Frame();
    Check(presenter.Status().threaded, "held wait: the next session presents from its own thread");
    presenter.Stop();
    Check(!next.ordering_violation && !g_release_violation, "held wait: runtime calls stayed ordered");
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
              App*& app_out, Runtime& runtime, std::unique_ptr<App>& app, Logger* logger = nullptr) {
    gpu.threaded = true;
    gpu.jit = true;
    gpu.depth = true;
    StartPresenter(presenter, gpu, mode, kFormat, kAppUsage, multiplier, logger);
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
    // ...unless, with a lead of four refreshes, the backend's history no
    // longer holds what moving them needs: those are shown unmoved.
    const int moved = SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
        return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.0f;
    });
    Check(moved + static_cast<int>(status.ring_exhausted) > static_cast<int>(refresh * elapsed * 0.4) &&
              gpu.ring_refusals == 0,
          "jit unready: the frames between real frames are moved on (" + std::to_string(moved) + " of " +
              std::to_string(static_cast<int>(refresh * elapsed)) + " refreshes; lead " +
              std::to_string(status.lead_slots) + ", " + std::to_string(status.ring_exhausted) + " ring exhausted, " +
              std::to_string(status.unmeasured_copies) + " unmeasured, " + std::to_string(status.unready_capture) +
              " uncaptured)");
    // Refreshes show new frames, but for those the backend's history no
    // longer allowed to move.
    const double exhausted = static_cast<double>(status.ring_exhausted) / elapsed;
    Check(status.distinct_fps + exhausted > 0.8 * status.output_fps,
          "jit unready: refreshes show new frames (" + std::to_string(status.distinct_fps) + " of " +
              std::to_string(status.output_fps) + " fps, " + std::to_string(exhausted) + "/s ring exhausted)");
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
        int engaged = 0, not_off = 0;
        for (int i = 0; i < 8; ++i) {
            // Turned back on, Dynamic stands by and judges the game's rate
            // afresh before it engages again.
            auto active = [&] {
                const FrameGenStatus status = presenter.Status();
                return status.state == FrameGenState::kActive && (!jit || (status.threaded && !status.standby));
            };
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!active() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(100 + 7 * i));
            engaged += active() ? 1 : 0;
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

// Just in time, frames moved with the eyes (the game's depth in use) say how
// far in the summary's eyeShiftMm, as frames written together do.
void TestJitEyeShiftReported() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                            ("vectorxr-presenter-eyeshift-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    auto logger = std::make_unique<Logger>();
    logger->Initialize(directory / "eyeshift.log");
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned,
             logger.get());
    presenter.SetSummaryInterval(300ms);
    app->eye_speed = 0.5;
    for (int i = 0; i < 3; ++i) app->Frame();
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 1200ms) {
        const XrFrameState state = app->Wait();
        app->Begin();
        std::this_thread::sleep_for(4ms);
        app->Release();
        Check(XR_SUCCEEDED(app->End(state.predictedDisplayTime)), "jit eye shift: end");
    }
    presenter.Stop();
    const std::filesystem::path path = logger->ActiveLogPath();
    logger.reset(); // flushed
    std::ifstream file(path);
    double largest = 0.0;
    int summaries = 0;
    const std::string key = "eyeShiftMm=";
    for (std::string line; std::getline(file, line);) {
        const std::size_t at = line.find(key);
        if (line.find("threaded=1") == std::string::npos || at == std::string::npos) continue;
        ++summaries;
        largest = std::max(largest, std::stod(line.substr(at + key.size())));
    }
    file.close();
    std::filesystem::remove_all(directory, ignored);
    // 0.5 m/s, a frame moved half a 45 fps interval on: about 5.6 mm.
    Check(summaries > 0 && largest > 1.0,
          "jit eye shift: the summaries say how far frames moved with the eyes (" + std::to_string(largest) +
              " mm in " + std::to_string(summaries) + " summaries)");
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
    // engaged, it runs at its own rate.
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
              presenter.OsdText() == "Unavailable" && presenter.OsdDetailText() == "No backend for this API",
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
// refreshes (its work ends 3 ms before the last of them): {1, 2, 2} runs at
// 0.6x the refresh rate. Engaged, Dynamic paces the waits instead, at the
// game's own rate: the same work runs unquantized ({1, 2, 2}: 0.72x at
// 90 Hz), at most once a refresh.
int RunTicks(App& app, std::chrono::milliseconds duration, const std::vector<int>& ticks,
             const std::function<bool(const XrFrameState&)>& each = {}) {
    std::vector<std::chrono::microseconds> work;
    for (const int tick : ticks) {
        work.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::nanoseconds(tick * app.runtime.period)) -
                       3ms);
    }
    return RunWork(app, duration, work, each);
}

bool Engaged(FrameGenPresenter& presenter) {
    const FrameGenStatus status = presenter.Status();
    return status.state == FrameGenState::kActive && status.threaded && !status.standby;
}

// A game whose frame times jitter about the refresh period: three light
// frames and one of two refreshes' work. Standing by, a paced runtime
// quantizes it to refreshes {1, 1, 1, 2} (0.8x); engaged at its own rate,
// released at once when late, it shows a real frame every refresh, with
// room to spare on a busy machine.
std::vector<std::chrono::microseconds> JitterWork(XrDuration period) {
    return {4ms, 4ms, 4ms, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::nanoseconds(2 * period)) - 3ms};
}

// Runs a game at 0.6x the refresh rate until Dynamic engages (or fails the test).
void EngageDynamic(FrameGenPresenter& presenter, App& app, const std::string& name) {
    RunTicks(app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), name + ": dynamic engages for a game at 0.6x the refresh rate");
}

const std::vector<std::chrono::microseconds> kLightWork{4ms};

// Extrapolating just in time, motion measured a refresh after the real
// frame's own (as NVIDIA's flow beside MSFS 2024 was, 2026-10-09): the
// refresh after a frame's own needs its motion before it is measured. The
// previous real frame's motion moves it instead (SynthesizeStale) rather
// than its exact copy standing still: those refreshes are distinct, every
// such write names the frame's previous, already measured, and nothing is
// written before its frame was captured. VECTORXR_FRAMEGEN_STALE_MOTION=0,
// or a backend that cannot, shows the exact copy as before.
void TestJitStaleMotion() {
    struct Run {
        FrameGenStatus status;
        int stale_writes{0};
        int wrong_motion{0};
        int uncaptured{0};
        int unmeasured_moves{0};
        bool ordered{true};
        int mismatched{0};
    };
    auto run = [](bool backend, bool enabled) {
        _putenv_s("VECTORXR_FRAMEGEN_STALE_MOTION", enabled ? "" : "0");
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.stale = backend;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
        _putenv_s("VECTORXR_FRAMEGEN_STALE_MOTION", "");
        gpu.measure_lag = 35ms; // past the refresh after its own, up to a lead of two
        for (int i = 0; i < 3; ++i) app->Frame();
        RunWork(*app, 900ms, kLightWork);
        Run result;
        result.status = presenter.Status();
        result.stale_writes = gpu.stale_writes;
        result.wrong_motion = gpu.stale_wrong_motion;
        result.uncaptured = gpu.uncaptured_writes;
        result.unmeasured_moves = gpu.unmeasured_moves;
        result.mismatched = runtime.mismatched_images;
        presenter.Stop();
        result.ordered = !runtime.ordering_violation && !g_release_violation;
        return result;
    };
    const Run stale = run(true, true);
    const Run off = run(true, false);
    const Run unsupported = run(false, true);
    std::cerr << "  stale: " << stale.status.stale_motion << " stale, " << stale.status.unmeasured_copies
              << " unmeasured copies, distinct " << stale.status.distinct_fps << " of " << stale.status.output_fps
              << "; off: " << off.status.unmeasured_copies << " unmeasured copies, distinct " << off.status.distinct_fps
              << "; unsupported: " << unsupported.status.unmeasured_copies << " unmeasured copies" << std::endl;
    Check(off.status.unmeasured_copies > 10 && off.status.stale_motion == 0 && off.stale_writes == 0,
          "stale motion: off, moved frames whose motion is late are shown unchanged (" +
              std::to_string(off.status.unmeasured_copies) + ")");
    Check(stale.status.stale_motion > 10 && stale.stale_writes == static_cast<int>(stale.status.stale_motion) &&
              stale.status.unmeasured_copies * 4 < off.status.unmeasured_copies,
          "stale motion: the previous real frame's motion moves them instead (" +
              std::to_string(stale.status.stale_motion) + " moved so, " +
              std::to_string(stale.status.unmeasured_copies) + " unchanged)");
    Check(stale.wrong_motion == 0 && stale.uncaptured == 0 && stale.unmeasured_moves == 0,
          "stale motion: always the frame's previous, measured, from a captured frame (" +
              std::to_string(stale.wrong_motion) + " wrong, " + std::to_string(stale.uncaptured) + " uncaptured)");
    Check(stale.status.distinct_fps > off.status.distinct_fps + 5.0,
          "stale motion: more refreshes show a new frame (" + std::to_string(stale.status.distinct_fps) + " vs " +
              std::to_string(off.status.distinct_fps) + ")");
    Check(unsupported.status.stale_motion == 0 && unsupported.status.unmeasured_copies > 10 &&
              unsupported.status.failures == 0,
          "stale motion: a backend that cannot shows the exact copy, without a failure (" +
              std::to_string(unsupported.status.unmeasured_copies) + ")");
    for (const Run* r : {&stale, &off, &unsupported}) {
        Check(r->ordered && r->mismatched == 0, "stale motion: calls stayed ordered, images match their views");
    }
}

// Interpolating just in time with each real frame's motion measured late
// (35 ms after its capture): a refresh between two real frames needs the
// newer one's motion before it is measured. The older one moves on past
// itself instead (a cover frame) rather than the newer one being shown
// unchanged before its time, which jumps ahead and then holds: refreshes
// keep showing content of their own time. VECTORXR_FRAMEGEN_STALE_MOTION=0
// shows the newer one unchanged as before.
void TestJitInterpolateCoversLateMotion() {
    struct Run {
        FrameGenStatus status;
        double timeline{0.0};
        bool ordered{true};
    };
    auto run = [](bool enabled) {
        _putenv_s("VECTORXR_FRAMEGEN_STALE_MOTION", enabled ? "" : "0");
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k2, app, runtime, owned);
        _putenv_s("VECTORXR_FRAMEGEN_STALE_MOTION", "");
        app->eye_speed = 0.5;
        gpu.measure_lag = 35ms;
        for (int i = 0; i < 3; ++i) app->Frame();
        const std::size_t first = runtime.Submissions();
        RunWork(*app, 1200ms, kLightWork);
        Run result;
        result.status = presenter.Status();
        result.timeline = TimelineShare(runtime, first, 20, app->eye_speed);
        presenter.Stop();
        result.ordered = !runtime.ordering_violation && !g_release_violation;
        return result;
    };
    const Run cover = run(true);
    const Run off = run(false);
    std::cerr << "  cover: " << cover.status.stale_motion << " covered, " << cover.status.unmeasured_copies
              << " unchanged, timeline " << cover.timeline << "; off: " << off.status.unmeasured_copies
              << " unchanged, timeline " << off.timeline << std::endl;
    // The lead grows to make room for the motion (a few refreshes meet it
    // late meanwhile): each of those is a cover frame instead.
    Check(off.status.unmeasured_copies > 0 && off.status.stale_motion == 0,
          "interpolate late motion: off, the newer frame is shown unchanged (" +
              std::to_string(off.status.unmeasured_copies) + ")");
    Check(cover.status.stale_motion > 0 && cover.status.unmeasured_copies == 0,
          "interpolate late motion: the older frame moves on instead (" + std::to_string(cover.status.stale_motion) +
              " covered, " + std::to_string(cover.status.unmeasured_copies) + " unchanged)");
    Check(cover.timeline >= off.timeline - 0.02,
          "interpolate late motion: refreshes show content of their own time as often (" +
              std::to_string(cover.timeline) + " vs " + std::to_string(off.timeline) + ")");
    Check(cover.ordered && off.ordered, "interpolate late motion: calls stayed ordered");
}

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
    const Run free = run("off");
    const Run held = run("always");
    Check(!free.status.gpu_gating && free.status.gpu_waits == 0, "gpu pacing: off never holds the game's waits");
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

    // By default it arms on evidence: a game running frames ahead on its
    // GPU, engaged by Dynamic, has its frames captured late; within two
    // benefit guard windows its waits are held, and one frame stays queued.
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.game_gpu = 26ms;
        gpu.frames_ahead = 4;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
        // What the late frames cost before holding starts stands nothing down here.
        FrameGenDynamicPolicy policy = GuardDynamic();
        policy.distinct_margin_fps = -1000.0;
        policy.unready_share = 2.0;
        presenter.SetDynamicPolicy(policy);
        app->before_release = [&] { gpu.GameFrame(); };
        RunWork(*app, 3000ms, {2ms}, [&](const XrFrameState&) { return Engaged(presenter); });
        Check(Engaged(presenter) && !presenter.Status().gpu_gating, "gpu pacing armed: engaged, not armed yet");
        const auto engaged = std::chrono::steady_clock::now();
        RunWork(*app, 2000ms, {2ms}, [&](const XrFrameState&) { return presenter.Status().gpu_gating; });
        const double armed_after = Seconds(std::chrono::steady_clock::now() - engaged);
        Check(presenter.Status().gpu_gating && armed_after < Seconds(policy.settle + 3 * policy.guard_window),
              "gpu pacing armed: frames captured late arm it within two windows (" + std::to_string(armed_after) +
                  " s)");
        RunWork(*app, 300ms, {2ms});
        {
            std::scoped_lock lock(gpu.timeline_mutex);
            gpu.max_queued = 0;
        }
        RunWork(*app, 600ms, {2ms});
        const FrameGenStatus status = presenter.Status();
        Check(Engaged(presenter) && status.dynamic_stand_downs == 0,
              "gpu pacing armed: stays engaged (" + status.dynamic_reason + ")");
        Check(status.gpu_waits > 0 && gpu.max_queued <= 2, "gpu pacing armed: one game frame stays queued (" +
                                                               std::to_string(gpu.max_queued.load()) + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "gpu pacing armed: calls stayed ordered");
    }

    // A pipelined game whose frames reach the GPU in time (captured before
    // their refresh) gives no evidence: its waits are never held.
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
        presenter.SetDynamicPolicy(GuardDynamic());
        app->end_offset_periods = 1;
        RunPipelined(*app, 900ms, 16ms, 26ms, {}, nullptr, 8ms);
        Check(Engaged(presenter), "gpu pacing unarmed: dynamic engages");
        RunPipelined(*app, 2000ms, 16ms, 26ms, {}, nullptr, 8ms);
        const FrameGenStatus status = presenter.Status();
        Check(!status.gpu_gating && status.gpu_waits == 0, "gpu pacing unarmed: a game on time is never held");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "gpu pacing unarmed: calls stayed ordered");
    }

    // A GPU hitch (frames taking 150 ms for a while): waits time out in a
    // row, but captures keep finishing, so holding stays on (only a GPU that
    // makes no progress turns it off), and one frame stays queued after.
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.game_gpu = 8ms;
        gpu.frames_ahead = 4;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "always");
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
        _putenv_s("VECTORXR_FRAMEGEN_GPU_AHEAD", "");
        app->before_release = [&] { gpu.GameFrame(); };
        for (int i = 0; i < 3; ++i) app->Frame();
        RunWork(*app, 400ms, {2ms});
        {
            std::scoped_lock lock(gpu.timeline_mutex);
            gpu.game_gpu = 150ms;
        }
        RunWork(*app, 900ms, {2ms});
        const FrameGenStatus hitch = presenter.Status();
        {
            std::scoped_lock lock(gpu.timeline_mutex);
            gpu.game_gpu = 8ms;
        }
        RunWork(*app, 500ms, {2ms});
        {
            std::scoped_lock lock(gpu.timeline_mutex);
            gpu.max_queued = 0;
        }
        RunWork(*app, 500ms, {2ms});
        const FrameGenStatus status = presenter.Status();
        Check(hitch.gpu_wait_timeouts >= 5 && status.gpu_gating,
              "gpu pacing hitch: timeouts while the GPU still makes progress keep holding on (" +
                  std::to_string(hitch.gpu_wait_timeouts) + " timeouts)");
        Check(gpu.max_queued <= 2, "gpu pacing hitch: one game frame stays queued afterwards (" +
                                       std::to_string(gpu.max_queued.load()) + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "gpu pacing hitch: calls stayed ordered");
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

// Dynamic standing by: a game at 0.96x the refresh rate passes straight
// through, at the runtime's period, and nothing is captured or analyzed.
void TestDynamicPassThroughNearRefresh() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(SteadyDynamic());
    bool refresh_period = true;
    const auto start = std::chrono::steady_clock::now();
    std::vector<int> ticks(24, 1);
    ticks.push_back(2); // 25 frames in 26 refreshes
    const int frames = RunTicks(*app, 1500ms, ticks, [&](const XrFrameState& state) {
        refresh_period = refresh_period && state.predictedDisplayPeriod == kPeriod;
        return false;
    });
    const double rate = frames / Seconds(std::chrono::steady_clock::now() - start);
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    const FrameGenStatus status = presenter.Status();
    Check(rate > 0.9 * refresh, "standing by: the game runs at about 0.96x (" + std::to_string(rate) + " fps)");
    Check(status.standby && status.full_rate && status.state == FrameGenState::kActive && !status.threaded &&
              status.dynamic_engagements == 0,
          "standing by: a game near the refresh rate passes straight through");
    Check(gpu.captures == 0 && gpu.executes == 0, "standing by: nothing is captured or analyzed (" +
                                                      std::to_string(gpu.captures.load()) + " captures)");
    Check(refresh_period, "standing by: the game is told the runtime's period");
    Check(presenter.OsdText().find("Standby  /  Dynamic") != std::string::npos &&
              presenter.CompactOsdText().find("Standby") != std::string::npos &&
              presenter.OsdDescription().badge == "STBY",
          "standing by: the OSD says so (" + presenter.OsdText() + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "standing by: calls stayed ordered");
}

// A game at 0.6x the refresh rate engages Dynamic once it has run below 0.9x
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
    Check(gpu.captures > 0 && gpu.analyzes > 0 && status.generated > 0,
          "engage: frames are generated once engaged (" + std::to_string(gpu.captures.load()) + " captures, " +
              std::to_string(gpu.analyzes.load()) + " analyses, " + std::to_string(status.generated) + " generated)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "engage: calls stayed ordered");
}

// Engaged, Dynamic keeps the game at its own rate: a game taking 1.35
// refreshes a frame is never held in its waits (66.7 fps at 90 Hz), where a
// fixed 2x holds it at half the refresh rate; it is told the refresh
// period, every refresh gets a frame, and the refreshes it misses show
// generated frames. At 72, 90 and 120 Hz alike.
void TestDynamicOwnRate() {
    // The game's frames for `duration`: per second, and the mean time its
    // waits held it (refreshes).
    struct Pace {
        double rate{0.0};
        double held{0.0};
        bool told{true}; // every wait reported `period`
    };
    auto pace = [](App& app, std::chrono::milliseconds duration, std::chrono::microseconds work, XrDuration period,
                   XrDuration told) {
        Pace result;
        int frames = 0;
        double held_ms = 0.0;
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < duration) {
            const auto waiting = std::chrono::steady_clock::now();
            const XrFrameState state = app.Wait();
            held_ms += Seconds(std::chrono::steady_clock::now() - waiting) * 1000.0;
            result.told = result.told && state.predictedDisplayPeriod == told;
            app.Begin();
            SleepPrecise(std::chrono::steady_clock::now() + work);
            app.Release();
            Check(XR_SUCCEEDED(app.End(state.predictedDisplayTime)), "own rate: end");
            ++frames;
        }
        result.rate = frames / Seconds(std::chrono::steady_clock::now() - start);
        result.held = held_ms / std::max(frames, 1) / (static_cast<double>(period) / 1e6);
        return result;
    };
    for (const int hz : {72, 90, 120}) {
        const std::string name = "own rate " + std::to_string(hz) + " Hz";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        runtime.period = 1'000'000'000 / hz;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
        // The benefit guard judges every 300 ms window while engaged (three
        // or more below): staying engaged is its verdict, not its absence.
        presenter.SetDynamicPolicy(GuardDynamic());
        EngageDynamic(presenter, *app, name);
        const auto work = std::chrono::microseconds(runtime.period * 135 / 100 / 1000);
        RunWork(*app, 300ms, {work});
        auto moved = [&] {
            return SynthesesWhere(gpu, [](FrameGenSynthesisMode mode, float fraction) {
                return mode == FrameGenSynthesisMode::kExtrapolate && fraction > 0.0f;
            });
        };
        const int moved_before = moved();
        const std::size_t first = runtime.Submissions();
        const auto start = std::chrono::steady_clock::now();
        const Pace own = pace(*app, 1000ms, work, runtime.period, runtime.period);
        const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
        const double refresh = 1e9 / static_cast<double>(runtime.period);
        Check(own.held < 0.15, name + ": the game's waits never hold it (" + std::to_string(own.held) +
                                   " refreshes a frame)");
        Check(own.rate > refresh / 2 * 1.1 && own.rate < refresh,
              name + ": it runs at its own rate, between half the refresh rate and the refresh rate (" +
                  std::to_string(own.rate) + " fps)");
        Check(own.told, name + ": the game is told the refresh period");
        Check(static_cast<double>(runtime.Submissions() - first) / elapsed > refresh * 0.85,
              name + ": every refresh gets a frame");
        const double gaps = (refresh - own.rate) * elapsed;
        Check(moved() - moved_before > gaps * 0.6, name + ": generated frames fill the refreshes the game misses (" +
                                                       std::to_string(moved() - moved_before) + " of about " +
                                                       std::to_string(gaps) + ")");
        const FrameGenStatus status = presenter.Status();
        Check(status.distinct_fps > 0.85 * status.output_fps,
              name + ": every refresh is a new frame (" + std::to_string(status.distinct_fps) + " of " +
                  std::to_string(status.output_fps) + " fps)");
        Check(Engaged(presenter) && status.dynamic_stand_downs == 0, name + ": stays engaged");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
    // Fixed 2x: the same game is held at half the refresh rate (its waits
    // hold it most of a refresh a frame), told two refreshes per frame.
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
    for (int i = 0; i < 3; ++i) app->Frame();
    const auto work = std::chrono::microseconds(kPeriod * 135 / 100 / 1000);
    RunWork(*app, 300ms, {work});
    const Pace held = pace(*app, 800ms, work, kPeriod, 2 * kPeriod);
    const double half = 1e9 / static_cast<double>(kPeriod) / 2;
    Check(held.rate > half * 0.9 && held.rate < half * 1.1 && held.held > 0.25,
          "fixed 2x: held at half the refresh rate (" + std::to_string(held.rate) + " fps, held " +
              std::to_string(held.held) + " refreshes a frame)");
    Check(held.told, "fixed 2x: the game is told two refreshes per frame");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "fixed 2x: calls stayed ordered");
}

// Between the thresholds nothing changes: standing by at 0.92x stays standing
// by, engaged at 0.92x stays engaged. After a stand-down nothing engages
// until the hold has passed, however slow the game.
void TestDynamicHysteresis() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenDynamicPolicy policy = GuardDynamic();
    policy.engage_after = SteadyDynamic().engage_after;
    presenter.SetDynamicPolicy(policy);
    std::vector<int> between(11, 1);
    between.push_back(2); // 0.92x
    RunTicks(*app, 700ms, between);
    Check(presenter.Status().standby && presenter.Status().dynamic_engagements == 0,
          "hysteresis: 0.92x stays standing by");
    presenter.SetDynamicPolicy(GuardDynamic());
    EngageDynamic(presenter, *app, "hysteresis");
    // Engaged, the game runs at its own rate: 0.92x is a frame of 1.09
    // refreshes' work. Long enough for the guard to judge three windows.
    RunWork(*app, 1300ms, {std::chrono::microseconds(kPeriod * 109 / 100 / 1000)});
    FrameGenStatus status = presenter.Status();
    Check(Engaged(presenter) && status.dynamic_engagements == 1 && status.dynamic_stand_downs == 0,
          "hysteresis: 0.92x stays engaged");
    // Motion never measured: nothing new to show, and the game slower
    // than standing by; the guard stands down.
    gpu.measure_lag = 1s;
    RunTicks(*app, 3000ms, {3}, [&](const XrFrameState&) { return presenter.Status().standby; });
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
// (b), (c) and (d) judge only an engagement not showing more distinct frames
// than the game did alone, so (a) fails with them; a cause is named before
// that effect. The other causes are turned off for each. The game, engaged
// at its own rate, runs slower than it did standing by. After the hold,
// with the fault gone, it engages again.
void TestBenefitGuardBacksOff() {
    struct Case {
        const char* name;
        const char* reason;
        std::function<void(GpuLog&)> fault;
        std::function<void(FrameGenDynamicPolicy&)> isolate;
        std::vector<int> fault_ticks{3}; // the game's frames while the fault lasts
        FrameGenSynthesisMode mode{FrameGenSynthesisMode::kExtrapolate};
    };
    const std::vector<Case> cases{
        {"guard (a)", "distinct frames/s", [](GpuLog& gpu) { gpu.measure_lag = 1s; },
         [](FrameGenDynamicPolicy& policy) {
             policy.generated_share = 0.0;
             policy.late_analyses_share = 2.0;
         }},
        {"guard (b)", "too few", [](GpuLog& gpu) { gpu.measure_lag = 1s; },
         [](FrameGenDynamicPolicy& policy) { policy.late_analyses_share = 2.0; }},
        {"guard (c)", "reached the GPU too late", [](GpuLog& gpu) { gpu.capture_lag = 150ms; },
         [](FrameGenDynamicPolicy& policy) {
             policy.generated_share = 0.0;
             policy.late_analyses_share = 2.0;
         },
         {1, 2, 2}},
        {"guard (d)", "measured too late",
         [](GpuLog& gpu) { gpu.measure_lag = std::chrono::microseconds(4 * kPeriod / 1000); },
         [](FrameGenDynamicPolicy& policy) {
             policy.generated_share = 0.0;
             policy.unready_share = 2.0;
         }},
        // Interpolating, the frames between real frames need the newer one's
        // motion at the refresh after the older one's: measured later than
        // any lead makes up for.
        {"guard (d) interpolating", "measured too late", [](GpuLog& gpu) { gpu.measure_lag = 120ms; },
         [](FrameGenDynamicPolicy& policy) {
             policy.generated_share = 0.0;
             policy.unready_share = 2.0;
         },
         {3}, FrameGenSynthesisMode::kInterpolate},
    };
    for (const Case& test : cases) {
        const std::string name = test.name;
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, test.mode, FrameGenMultiplier::kDynamic, app, runtime, owned);
        FrameGenDynamicPolicy policy = GuardDynamic();
        test.isolate(policy);
        presenter.SetDynamicPolicy(policy);
        EngageDynamic(presenter, *app, name);
        RunTicks(*app, 300ms, {1, 2, 2});
        Check(Engaged(presenter), name + ": no stand-down without a fault");
        test.fault(gpu);
        RunTicks(*app, 3000ms, test.fault_ticks, [&](const XrFrameState&) { return presenter.Status().standby; });
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

// Real frames on the GPU too late for more than (c)'s share of refreshes do
// not stand down an engagement showing more distinct frames than the game
// alone (DCS, flight 2: a hitch's late frames in two 2.5 s windows stood
// down an engagement filling the refresh rate, for 20-80 s).
void TestWinningEngagementSurvivesLateCaptures() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenDynamicPolicy policy = GuardDynamic();
    policy.distinct_margin_fps = -1000.0; // always winning
    policy.generated_share = 0.0;
    policy.late_analyses_share = 2.0;
    presenter.SetDynamicPolicy(policy);
    EngageDynamic(presenter, *app, "winning late captures");
    RunTicks(*app, 300ms, {1, 2, 2});
    const FrameGenStatus before = presenter.Status();
    gpu.capture_lag = 150ms;
    RunTicks(*app, 3000ms, {1, 2, 2});
    const FrameGenStatus status = presenter.Status();
    const double refreshes = 3.0 * 1e9 / static_cast<double>(kPeriod);
    Check(static_cast<double>(status.unready_capture - before.unready_capture) > policy.unready_share * refreshes,
          "winning late captures: real frames reached the GPU late for more than (c)'s share (" +
              std::to_string(status.unready_capture - before.unready_capture) + " uncaptured)");
    Check(Engaged(presenter) && status.dynamic_stand_downs == 0,
          "winning late captures: the engagement stays (" + status.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "winning late captures: calls stayed ordered");
}

// NVIDIA's optical flow hardware chosen (SetInterpolateOnShaders, the
// default): interpolating in Dynamic, motion is measured on the shader cores;
// extrapolating, and interpolating at a fixed rate, on the hardware.
void TestInterpolateOnShaders() {
    for (const FrameGenSynthesisMode mode : {FrameGenSynthesisMode::kInterpolate, FrameGenSynthesisMode::kExtrapolate}) {
        const bool interpolate = mode == FrameGenSynthesisMode::kInterpolate;
        const std::string name = interpolate ? "interpolate on shaders" : "extrapolate on hardware";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, mode, FrameGenMultiplier::kDynamic, app, runtime, owned);
        presenter.SetInterpolateOnShaders(true);
        FrameGenResolvedSettings settings;
        settings.enabled = true;
        settings.synthesis_mode = mode;
        settings.multiplier = FrameGenMultiplier::kDynamic;
        settings.motion_estimation = FrameGenMotionEstimation::kHardware;
        presenter.Configure(settings);
        EngageDynamic(presenter, *app, name);
        const int before = gpu.executes;
        RunTicks(*app, 300ms, {1, 2, 2});
        presenter.Stop();
        Check(gpu.executes > before, name + ": real frames were analyzed");
        Check(gpu.tuning.hardware_flow == !interpolate,
              name + ": motion measured on the " + (gpu.tuning.hardware_flow ? "hardware" : "shader cores"));
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
    // At a fixed rate the game is held to every second refresh: interpolating
    // keeps the hardware.
    const std::string name = "interpolate at 2x on hardware";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k2, app, runtime, owned);
    presenter.SetInterpolateOnShaders(true);
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = FrameGenSynthesisMode::kInterpolate;
    settings.multiplier = FrameGenMultiplier::k2;
    settings.motion_estimation = FrameGenMotionEstimation::kHardware;
    presenter.Configure(settings);
    RunTicks(*app, 300ms, {2});
    const int before = gpu.executes;
    RunTicks(*app, 300ms, {2});
    presenter.Stop();
    Check(gpu.executes > before, name + ": real frames were analyzed");
    Check(gpu.tuning.hardware_flow,
          name + ": motion measured on the " + (gpu.tuning.hardware_flow ? "hardware" : "shader cores"));
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// Interpolate with NVIDIA's optical flow hardware chosen, switched during
// play between 2x and Dynamic: the motion source follows each switch (a
// settings change, never per frame), and the presenter keeps running.
void TestInterpolateSourceFollowsLiveSwitch() {
    const std::string name = "interpolate source follows live switches";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k2, app, runtime, owned);
    presenter.SetInterpolateOnShaders(true);
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = FrameGenSynthesisMode::kInterpolate;
    settings.multiplier = FrameGenMultiplier::k2;
    settings.motion_estimation = FrameGenMotionEstimation::kHardware;
    presenter.Configure(settings);
    RunTicks(*app, 300ms, {2});
    int before = gpu.executes;
    RunTicks(*app, 300ms, {2});
    Check(gpu.executes > before && gpu.tuning.hardware_flow, name + ": at 2x on the hardware");
    settings.multiplier = FrameGenMultiplier::kDynamic;
    presenter.Configure(settings);
    EngageDynamic(presenter, *app, name);
    before = gpu.executes;
    RunTicks(*app, 300ms, {1, 2, 2});
    Check(gpu.executes > before && !gpu.tuning.hardware_flow, name + ": in Dynamic on the shader cores");
    settings.multiplier = FrameGenMultiplier::k2;
    presenter.Configure(settings);
    RunTicks(*app, 300ms, {2});
    before = gpu.executes;
    RunTicks(*app, 300ms, {2});
    Check(gpu.executes > before && gpu.tuning.hardware_flow, name + ": back at 2x on the hardware");
    Check(presenter.Status().threaded, name + ": the presenter thread kept running");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// Engaged without a guard trigger for probe_after, Dynamic stands by to see
// whether the game holds the refresh rate alone: a game still slow engages
// again and the next probe waits twice as long; a game holding 0.95x stays
// standing by. Probes come only while the game, engaged at its own rate,
// runs near what standing by needs: one far below never probes.
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
    // Engaged at 0.72x of the refresh rate ({1, 2, 2} at its own rate):
    // standing by it would not come near 0.95x.
    RunTicks(*app, 1200ms, {1, 2, 2});
    Check(Engaged(presenter) && presenter.Status().dynamic_probes == 0,
          "probe: a game far below the refresh rate engaged is not probed (" +
              std::to_string(presenter.Status().dynamic_probes) + " probes)");
    // Quantized to 0.8x standing by, about the refresh rate engaged.
    const auto quantized = JitterWork(runtime.period);
    auto engaged_for = [&](std::uint64_t probes) {
        const auto start = std::chrono::steady_clock::now();
        RunWork(*app, 4000ms, quantized, [&](const XrFrameState&) { return presenter.Status().dynamic_probes >= probes; });
        return Seconds(std::chrono::steady_clock::now() - start);
    };
    const double first = engaged_for(1);
    Check(presenter.Status().dynamic_probes == 1 && first < 2.0,
          "probe: probes once the game runs near the refresh rate engaged (" + std::to_string(first) + " s)");
    RunWork(*app, 3000ms, quantized, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter) && presenter.Status().dynamic_engagements == 2,
          "probe: a game still below 0.95x engages again");
    const double second = engaged_for(2);
    Check(presenter.Status().dynamic_probes == 2 && second >= 0.95,
          "probe: a failed probe doubles the wait before the next (" + std::to_string(first) + " s, then " +
              std::to_string(second) + " s)");
    // The game now holds the refresh rate (with room to spare, so a busy
    // machine does not make it miss refreshes): the probe succeeds and stays.
    RunWork(*app, 1100ms, kLightWork);
    const FrameGenStatus status = presenter.Status();
    Check(status.standby && !status.threaded && status.dynamic_engagements == 2 &&
              status.dynamic_reason.find("the game runs at") == 0,
          "probe: a game holding the refresh rate stays standing by (" + status.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "probe: calls stayed ordered");
}

// Each real frame has a refresh of its own, extrapolating too: a 72 Hz game
// waited every refresh (Dynamic runs it at its own rate) whose lead shortens
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

// A game that stops sending frames: its last frame moves on for at most four
// of its intervals (and never past the pause limit, at least 250 ms and four
// of the game's frame intervals), then shows unchanged; the frame after the
// gap is not measured against the one before it (no motion across the gap),
// and refreshes show their own time again from the next.
void TestNoExtrapolationAcrossPauses() {
    for (const auto pause : {1000ms, 400ms}) {
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
        // moved on; past it, at most four intervals of four refreshes at 4x.
        Check(paused.generated - before.generated <= 16 + before.lead_slots + 1,
              name + ": the last frame moves on a few intervals only (" +
                  std::to_string(paused.generated - before.generated) + " generated, lead " +
                  std::to_string(before.lead_slots) + ")");
        // Its refresh is its lead after its wait, which came right before
        // its end; then four intervals, and a refresh to spare.
        const auto moves_until = last_end + std::chrono::nanoseconds((before.lead_slots + 16 + 1) * kPeriod);
        int moved_late = 0;
        {
            std::scoped_lock lock(gpu.syntheses_mutex);
            for (std::size_t i = syntheses_before; i < gpu.syntheses.size(); ++i) {
                const GpuLog::Synthesis& entry = gpu.syntheses[i];
                moved_late += entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction > 0.0f &&
                                      entry.at > moves_until
                                  ? 1
                                  : 0;
            }
        }
        Check(moved_late == 0, name + ": nothing moves past four intervals (" + std::to_string(moved_late) + ")");
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
    presenter.SetDynamicPolicy(SteadyDynamic());
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

// The lead after a burst of hitches (DCS on Cold War Germany, 2026-10-09:
// 4-5 slots after each restart, 7-8 after minutes, the latency felt). A
// GPU-bound game (19 ms of GPU a frame, three frames queued at most, Dynamic
// engaged at its own ~52 fps) whose GPU slows to 45 ms a frame for a while
// (the hitches: the lead climbs), then back. Its frames reach the GPU
// history at the GPU's pace whatever the lead (an early frame is held until
// its slot, which delays the game's next one), so above Dynamic's cap they
// show little of the slack that takes a slot off; with probes
// (FrameGenLeadPolicy) the lead steps back down to the lowest lead that
// holds (a probe below it fails, and is tried again only rarely), costing a
// few late frames per probe.
struct LeadRun {
    std::uint32_t settled{0}; // before the hitches
    std::uint32_t peak{0};    // after them
    std::uint32_t final{0};   // a while after
    FrameGenStatus before;    // at the hitches' end
    FrameGenStatus after;     // at the end
};

LeadRun RunLeadAfterHitches(bool probes) {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.game_gpu = 19ms;
    gpu.frames_ahead = 3;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenLeadPolicy policy;
    policy.probe_after = probes ? 250ms : 600s;
    policy.judge = 200ms;
    policy.explore_after = 700ms;
    policy.explore_max = 1400ms;
    // Without: the lead as it stepped before either (a step down for slack
    // judged as a probe skips the frames in flight across it, so it too
    // brings the lead back down here).
    policy.slack_hysteresis = probes;
    presenter.SetLeadPolicy(policy);
    app->before_release = [&] { gpu.GameFrame(); };
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 3000ms, {6ms}, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), "lead probes: Dynamic engages for the GPU-bound game");
    RunWork(*app, 800ms, {6ms});
    LeadRun run;
    run.settled = presenter.Status().lead_slots;
    gpu.game_gpu = 45ms; // the hitches
    RunWork(*app, 600ms, {6ms});
    run.peak = presenter.Status().lead_slots;
    gpu.game_gpu = 19ms;
    run.before = presenter.Status();
    RunWork(*app, 3000ms, {6ms});
    run.after = presenter.Status();
    run.final = run.after.lead_slots;
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "lead probes: calls stayed ordered");
    return run;
}

void TestLeadStepsDownAfterHitches() {
    const LeadRun held = RunLeadAfterHitches(false);
    const LeadRun probed = RunLeadAfterHitches(true);
    const auto late = [](const LeadRun& run) {
        return (run.after.late_cpu - run.before.late_cpu) + (run.after.late_gpu - run.before.late_gpu);
    };
    std::cerr << "  without probes or hysteresis: lead " << held.settled << " -> " << held.peak << " -> " << held.final
              << " (late " << late(held) << "); with probes: " << probed.settled << " -> " << probed.peak << " -> "
              << probed.final << " (floor " << probed.after.lead_floor << ", probes " << probed.after.lead_probes
              << ", failed " << probed.after.lead_probes_failed << ", late " << late(probed) << ")" << std::endl;
    Check(held.peak > held.settled && probed.peak > probed.settled, "lead probes: the hitches grow the lead");
    // Without probes the lead comes down only by slack: once the game's rate
    // is back, Dynamic's cap (4 refreshes per real frame) is 8 again, below
    // the peak, and the lead comes down with it; frames scored there show
    // slack. (Until 0.19.2 a lead above a lowered cap was never scored again,
    // so it stayed up.)
    Check(held.after.lead_probes == 0 && held.final <= 8,
          "lead probes: without probes the lead still comes down with the cap (" + std::to_string(held.settled) +
              " -> " + std::to_string(held.peak) + " -> " + std::to_string(held.final) + ")");
    Check(probed.after.lead_probes > probed.before.lead_probes && probed.final < probed.peak &&
              probed.final <= probed.settled,
          "lead probes: the lead steps back down after the hitches (" + std::to_string(probed.settled) + " -> " +
              std::to_string(probed.peak) + " -> " + std::to_string(probed.final) + ")");
    // The game's frames are late about as often whatever the lead (it is the
    // GPU's pace, not the lead, that makes them late): probing adds little.
    Check(late(probed) <= late(held) * 3 / 2 + 10,
          "lead probes: frames are late about as often with probes (" + std::to_string(late(probed)) + " vs " +
              std::to_string(late(held)) + ")");
}


// A game whose frames mostly fit a lead but a few in ten need one more
// refresh (DCS and MSFS 2024, 2026-10-09: the lead stepped 4 <-> 5 about
// once a second; every step down cost two late frames, each shown as a
// cover or a repeat). A lead that failed is retried only rarely.
struct LeadFlapRun {
    int changes{0};          // lead changes in the window
    std::uint64_t late{0};   // real frames late in the window
    double mean_lead{0.0};   // sampled each frame
    std::uint32_t floor{0};
    std::uint64_t undone{0}; // steps down undone by late frames
};

LeadFlapRun RunLeadFlap(bool hysteresis) {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.frames_ahead = 3;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenLeadPolicy policy;
    policy.probe_after = 600s;
    policy.judge = 1000ms;
    policy.explore_after = 1500ms;
    policy.explore_max = 3000ms;
    policy.slack_hysteresis = hysteresis;
    presenter.SetLeadPolicy(policy);
    // GPU-bound near 50 fps, one frame in five a refresh longer.
    const std::vector<std::chrono::microseconds> pattern = {18ms, 18ms, 18ms, 18ms, 30ms};
    std::size_t n = 0;
    app->before_release = [&] {
        gpu.game_gpu = pattern[n++ % pattern.size()];
        gpu.GameFrame();
    };
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 3000ms, {6ms}, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), "lead flapping: Dynamic engages for the GPU-bound game");
    RunWork(*app, 4000ms, {6ms});
    const FrameGenStatus before = presenter.Status();
    const bool trace = std::getenv("FG_TEST_LEAD_TRACE") != nullptr;
    const auto trace_start = std::chrono::steady_clock::now();
    LeadFlapRun run;
    std::uint32_t lead = before.lead_slots;
    double lead_sum = 0.0;
    int samples = 0;
    RunWork(*app, 6000ms, {6ms}, [&](const XrFrameState&) {
        const FrameGenStatus now = presenter.Status();
        lead_sum += now.lead_slots;
        ++samples;
        if (now.lead_slots != lead) {
            ++run.changes;
            lead = now.lead_slots;
            if (trace) {
                std::cerr << "    " << Seconds(std::chrono::steady_clock::now() - trace_start) << " s: lead " << lead
                          << " floor " << now.lead_floor << " undone " << now.lead_downs_undone << " late "
                          << now.late_cpu << "/" << now.late_gpu << std::endl;
            }
        }
        return false;
    });
    const FrameGenStatus after = presenter.Status();
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "lead flapping: calls stayed ordered");
    run.late = (after.late_cpu - before.late_cpu) + (after.late_gpu - before.late_gpu);
    run.mean_lead = samples ? lead_sum / samples : 0.0;
    run.floor = after.lead_floor;
    run.undone = after.lead_downs_undone - before.lead_downs_undone;
    return run;
}

void TestLeadDoesNotFlap() {
    const LeadFlapRun flapping = RunLeadFlap(false);
    const LeadFlapRun held = RunLeadFlap(true);
    std::cerr << "  lead flapping without hysteresis: " << flapping.changes << " changes, " << flapping.late
              << " late in 6 s, mean lead " << flapping.mean_lead << "; with: " << held.changes << " changes ("
              << held.undone << " undone), " << held.late << " late, mean lead " << held.mean_lead << ", floor "
              << held.floor << std::endl;
    // Retried after 1.5 s, then every 3 s: two retries, each undone, in 6 s.
    Check(flapping.changes >= 8, "lead flapping: the game makes the lead flap without hysteresis (" +
                                     std::to_string(flapping.changes) + " changes in 6 s)");
    Check(held.changes <= 6 && held.changes * 3 <= flapping.changes * 2,
          "lead flapping: a lead that failed is retried only rarely (" + std::to_string(held.changes) + " vs " +
              std::to_string(flapping.changes) + " changes in 6 s)");
    Check(held.late < flapping.late, "lead flapping: fewer frames are late (" + std::to_string(held.late) + " vs " +
                                         std::to_string(flapping.late) + ")");
    Check(held.mean_lead <= flapping.mean_lead + 1.0,
          "lead flapping: the lead holds about where it flapped (" + std::to_string(held.mean_lead) + " vs " +
              std::to_string(flapping.mean_lead) + ")");
}

// A slow game (GPU-bound near 33 fps) that hitches every 0.6 s: each hitch
// makes a few of its frames late at the lowest lead known to hold. Taken as
// that lead no longer holding, each hitch raised it a refresh, and below it
// the lead is retried only every explore_after: the lead climbed a refresh
// per hitch and never came back down before the next, past what the kept
// real frames cover, so refreshes showed old frames unmoved until Dynamic
// stood down (Star Citizen on Pimax's runtime, 2026-10-13: 6 -> 7 -> 8 -> 9
// within a minute). A hitch no lead cures lifts that floor one refresh above
// the lowest lead a probe proved at most; between hitches the lead comes back
// down to it.
void TestLeadAfterRepeatedHitches() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.frames_ahead = 2;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenLeadPolicy policy;
    policy.probe_after = 250ms;
    policy.judge = 200ms;
    policy.explore_after = 700ms;
    policy.explore_max = 1400ms;
    presenter.SetLeadPolicy(policy);
    bool hitches = false;
    int frames = 0;
    app->before_release = [&] {
        gpu.game_gpu = hitches && ++frames % 18 == 0 ? 150ms : 30ms;
        gpu.GameFrame();
    };
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 3000ms, {6ms}, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), "repeated hitches: Dynamic engages for the slow game");
    // Probes find the lowest lead that holds.
    RunWork(*app, 3000ms, {6ms});
    const FrameGenStatus settled = presenter.Status();
    hitches = true;
    std::uint32_t highest = settled.lead_slots;
    std::uint32_t highest_floor = settled.lead_floor;
    std::uint64_t lead_sum = 0;
    std::uint64_t samples = 0;
    RunWork(*app, 6000ms, {6ms}, [&](const XrFrameState&) {
        const FrameGenStatus now = presenter.Status();
        highest = std::max(highest, now.lead_slots);
        highest_floor = std::max(highest_floor, now.lead_floor);
        lead_sum += now.lead_slots;
        ++samples;
        return false;
    });
    const FrameGenStatus after = presenter.Status();
    presenter.Stop();
    const double mean = samples ? static_cast<double>(lead_sum) / static_cast<double>(samples) : 0.0;
    std::cerr << "  repeated hitches: lead " << settled.lead_slots << " (floor " << settled.lead_floor << ") -> mean "
              << mean << ", highest " << highest << " (floor " << highest_floor << "), last " << after.lead_slots
              << " (floor " << after.lead_floor << "); ring exhausted +"
              << after.ring_exhausted - settled.ring_exhausted << ", stand-downs +"
              << after.dynamic_stand_downs - settled.dynamic_stand_downs << std::endl;
    Check(!runtime.ordering_violation && !g_release_violation, "repeated hitches: calls stayed ordered");
    Check(settled.lead_floor > 0, "repeated hitches: probes found the lowest lead that holds before the hitches");
    Check(highest_floor <= settled.lead_floor + 1,
          "repeated hitches: the hitches lift the lowest lead known to hold one refresh at most (" +
              std::to_string(settled.lead_floor) + " -> " + std::to_string(highest_floor) + ")");
    Check(mean <= static_cast<double>(settled.lead_floor) + 1.5,
          "repeated hitches: between hitches the lead comes back down (mean " + std::to_string(mean) + ", floor " +
              std::to_string(settled.lead_floor) + ")");
}

// Dynamic caps the lead at 4 refreshes per real frame, its refreshes per real
// frame rounded (2-4): 8, 12 or 16. A lead raised past a cap that then drops
// (the game speeding up) was told to the game as the cap, matched no frame's,
// and so was never scored again: it stayed above the cap until a probe, and
// raises at the cap were counted though nothing moved (DCS 231933 and 220716,
// 0.19.1: leadSlots=9 while the game was told 8; +54/-0 at the cap).
void TestLeadFollowsLoweredCap() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    FrameGenLeadPolicy policy;
    policy.probe_after = 600s; // no probes: only scored frames move the lead
    presenter.SetLeadPolicy(policy);
    for (int i = 0; i < 3; ++i) app->Frame();
    // A game at a third of the refresh rate (cap 12), its frames reaching the
    // GPU history late: the lead climbs past 8.
    RunTicks(*app, 3000ms, {3}, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), "lowered cap: Dynamic engages");
    gpu.capture_lag = 100ms;
    RunTicks(*app, 4000ms, {3}, [&](const XrFrameState&) { return presenter.Status().lead_slots > 9; });
    const FrameGenStatus raised = presenter.Status();
    gpu.capture_lag = 0us;
    Check(raised.lead_slots > 8, "lowered cap: the lead climbs past 8 (" + std::to_string(raised.lead_slots) + ")");
    // At 1.5 refreshes a frame the cap is 8.
    RunTicks(*app, 2500ms, {1, 2});
    const FrameGenStatus after = presenter.Status();
    presenter.Stop();
    std::cerr << "  lowered cap: lead " << raised.lead_slots << " -> " << after.lead_slots << " (floor "
              << after.lead_floor << "), moves +" << after.lead_ups << "/-" << after.lead_downs << std::endl;
    Check(!runtime.ordering_violation && !g_release_violation, "lowered cap: calls stayed ordered");
    Check(after.lead_slots <= 8 && after.lead_floor <= 8,
          "lowered cap: the lead comes down with the cap (" + std::to_string(raised.lead_slots) + " -> " +
              std::to_string(after.lead_slots) + ", floor " + std::to_string(after.lead_floor) + ")");
}

// Refreshes the presenter thread misses (Varjo's runtime under a GPU-bound
// DCS, 2026-10-14: about one in ten) pass over the real frames made for them
// when the next real frame is a refresh behind: those frames came in time,
// and no lead cures a refresh nobody wrote. Scored late, two in ten raised
// the lead a refresh each time, and it sat at its cap (4 per real frame, 89
// ms) all flight: leadMoves=+100/-0 every 30 s, with no frame late on the
// CPU or the GPU.
void TestPassedOverFramesKeepLead() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    // GPU-bound at ~70 fps (real frames a refresh or two apart), so its frames
    // never show the slack that takes a refresh off the lead; no probes.
    gpu.game_gpu = 14ms;
    gpu.frames_ahead = 3;
    FrameGenLeadPolicy policy;
    policy.probe_after = 600s;
    presenter.SetLeadPolicy(policy);
    app->before_release = [&] { gpu.GameFrame(); };
    const std::vector<std::chrono::microseconds> work{6ms};
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), "passed over: Dynamic engages");
    RunWork(*app, 1000ms, work);
    const FrameGenStatus settled = presenter.Status();
    runtime.miss_every = 9;
    std::uint32_t highest = settled.lead_slots;
    RunWork(*app, 3000ms, work, [&](const XrFrameState&) {
        highest = std::max(highest, presenter.Status().lead_slots);
        return false;
    });
    runtime.miss_every = 0;
    const FrameGenStatus after = presenter.Status();
    presenter.Stop();
    std::cerr << "  passed over: lead " << settled.lead_slots << " -> highest " << highest << ", last "
              << after.lead_slots << " (cap " << 4 * 2 << "); moves +" << after.lead_ups - settled.lead_ups << "/-"
              << after.lead_downs - settled.lead_downs << ", late " << after.late_cpu - settled.late_cpu << "/"
              << after.late_gpu - settled.late_gpu << std::endl;
    Check(!runtime.ordering_violation && !g_release_violation, "passed over: calls stayed ordered");
    Check(Engaged(presenter) || after.threaded, "passed over: stays engaged");
    Check(highest <= settled.lead_slots + 1 && after.lead_slots < 8,
          "passed over: missed refreshes do not raise the lead (" + std::to_string(settled.lead_slots) + " -> " +
              std::to_string(highest) + ", last " + std::to_string(after.lead_slots) + ")");
}

// Frames with no projection (a loading screen or a menu: nothing to generate
// from) say nothing about the lead the game's frames need. Scored, late ones
// raised it a refresh every two, to its cap: Star Citizen on Virtual Desktop
// (2026-10-14) showed a cylinder layer alone for 93 s, the lead went from 2
// to 8 and stayed at 7-8 for 12 s of play, and the game predicted its poses
// 152 ms ahead (about 65 ms without Frame Generation). The lead holds across
// them. The game ends its frames a period after the time it is told, as Star
// Citizen does, and renders for that time. Just in time, then with frames
// written after each real frame.
void TestLoadingFramesDoNotRaiseLead() {
    const std::string name =
        std::string("loading frames keep the lead") + (g_jit ? "" : ", written after each real frame");
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    if (g_jit) {
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
    } else {
        StartThreaded(presenter, gpu, FrameGenMultiplier::k2);
        owned = std::make_unique<App>(presenter, runtime);
        app = owned.get();
    }
    app->end_offset_periods = 1;
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 2000ms, kLightWork);
    const FrameGenStatus settled = presenter.Status();
    Check(settled.threaded, name + ": the presenter thread runs");
    // Each loading frame ends three refreshes after its own refresh was due.
    std::uint32_t peak = settled.lead_slots;
    app->before_begin = [&] {
        const std::uint32_t lead = presenter.Status().lead_slots;
        peak = std::max(peak, lead);
        SleepPrecise(std::chrono::steady_clock::now() + std::chrono::nanoseconds((lead + 3) * kPeriod));
    };
    const auto loading = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - loading < 1500ms) app->Frame(false);
    app->before_begin = nullptr;
    const FrameGenStatus loaded = presenter.Status();
    // The game's frames again: the lead each is told, and how far past the
    // runtime's newest display time it renders (in refreshes).
    std::uint32_t lead = 0;
    double horizon = 0.0;
    for (int i = 0; i < 20; ++i) {
        const XrFrameState state = app->Wait();
        {
            std::scoped_lock lock(g_xr_mutex);
            const XrTime rendered_for =
                state.predictedDisplayTime + state.predictedDisplayPeriod * app->end_offset_periods;
            horizon = std::max(horizon, static_cast<double>(rendered_for - runtime.clock_floor) /
                                            static_cast<double>(kPeriod));
        }
        lead = std::max(lead, presenter.Status().lead_slots);
        app->Begin();
        SleepPrecise(std::chrono::steady_clock::now() + kLightWork[0]);
        app->Release();
        Check(XR_SUCCEEDED(app->End(state.predictedDisplayTime)), name + ": end");
    }
    presenter.Stop();
    std::cerr << "  " << name << ": lead " << settled.lead_slots << " -> " << peak << " loading ("
              << loaded.late_cpu - settled.late_cpu << " late) -> " << lead << " after, horizon " << horizon
              << " refreshes" << std::endl;
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    Check(loaded.late_cpu - settled.late_cpu >= 8, name + ": the loading frames came late");
    Check(lead <= settled.lead_slots + 1, name + ": loading frames do not raise the lead (" +
                                              std::to_string(settled.lead_slots) + " -> " + std::to_string(peak) +
                                              " loading, " + std::to_string(lead) + " after)");
    Check(horizon <= settled.lead_slots + 1.0,
          name + ": the game renders no further ahead after loading (" + std::to_string(horizon) + " refreshes, lead " +
              std::to_string(settled.lead_slots) + ")");
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
    // Moved: the frame before up to the last one's refresh, then the last
    // one for four of its intervals (two refreshes each).
    Check(exact <= 2 && moved <= static_cast<int>(before.lead_slots) + 8 + 1,
          "skip copies: the stalled frame's exact copy is written once (" +
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
                              "lateReal=", "gpuWaits=", "captureDoneMs=", "analysisMs=", "measureLagMs=", "ofaMs=",
                              "gpuStartMs=", "gpuDoneMs=", "endCallMs=", "runtimeEndMs=", "synthLockMs=", "unwritten=",
                              "unready=", "pauses=",
                              "leadSlots=", "leadFloor=", "leadMoves=+", "leadProbes=", "leadUndone=",
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
    // The session's first window starts with it (it had no start: rates 0.0).
    Check(number(inline_lines.front(), "appFps=") > 0.0,
          "telemetry: the first summary's rates cover its window (" + inline_lines.front() + ")");
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
    // Engaged at its own rate: {1, 2, 2} refreshes of work run unquantized
    // (a refresh, then 1.7 twice: 60.6 fps).
    Check(app_fps > 52.0 && app_fps < 70.0, "telemetry: appFps is the window's average (" + std::to_string(app_fps) + ")");
    Check(!status.dynamic_reason.empty() && last.find(status.dynamic_reason) != std::string::npos,
          "telemetry: the summary carries Status's dynamic reason");
    Check(!runtime.ordering_violation && !g_release_violation, "telemetry: calls stayed ordered");
}

// A game GPU-bound at 1.05x two refreshes, its waits held for its GPU: the
// cadence counts from where it released each wait, so real frames stay at
// the GPU's own pace (2.1 refreshes), not a refresh later each time a GPU
// wait crosses one.
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
    // Measured after its refresh is due, before the next. The game (at its
    // own rate, 0.75x) leaves refreshes for generated frames to fill.
    RunWork(*app, 1500ms, {std::chrono::microseconds(kPeriod * 4 / 3 / 1000)});
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

// A game whose frame times jitter about the refresh period: standing by, the
// runtime's waits quantize it to 0.8x ({1, 1, 1, 2}), so Dynamic engages;
// engaged at its own rate it fills every refresh itself (released at once
// when late, its lead absorbs its long frames), so hardly any frame is
// generated. Showing clearly more distinct frames than the game did alone,
// the engagement pays: too few generated frames (b) or analyses (d) never
// stand it down.
void TestWinningEngagementStays() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(GuardDynamic());
    const auto jitter = JitterWork(runtime.period);
    RunWork(*app, 3000ms, jitter, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), "winning: dynamic engages for a game quantized to 0.8x standing by");
    // Settle, then four or more 300 ms windows judged.
    RunWork(*app, 1600ms, jitter);
    const FrameGenStatus status = presenter.Status();
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    Check(status.distinct_fps > 0.9 * refresh, "winning: engaged, nearly every refresh shows a new frame (" +
                                                   std::to_string(status.distinct_fps) + " fps)");
    Check(Engaged(presenter) && status.dynamic_stand_downs == 0 && status.dynamic_engagements == 1,
          "winning: an engagement showing more than the game alone stays (" + status.dynamic_reason + ")");
    // Its motion measured too late for the few frames it would move: the
    // real frames still fill the refreshes, and it stays.
    gpu.measure_lag = std::chrono::microseconds(4 * kPeriod / 1000);
    const FrameGenStatus before = presenter.Status();
    RunWork(*app, 1200ms, jitter);
    const FrameGenStatus late = presenter.Status();
    Check(late.analyses_late - before.analyses_late > 20 && Engaged(presenter) && late.dynamic_stand_downs == 0,
          "winning: analyses measured late do not stand it down either (" +
              std::to_string(late.analyses_late - before.analyses_late) + " late; " + late.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "winning: calls stayed ordered");
}

// Turned off while engaged and on again, Dynamic stands by and judges the
// game's rate afresh (its guard and probes start over): a game that holds
// the refresh rate meanwhile is not engaged again.
void TestDynamicToggleStandsBy() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(SteadyDynamic());
    EngageDynamic(presenter, *app, "toggle standby");
    Check(!presenter.Toggle(), "toggle standby: turned off");
    RunTicks(*app, 300ms, {1});
    Check(!presenter.Status().threaded, "toggle standby: turned off, frames pass straight through");
    Check(presenter.Toggle(), "toggle standby: turned on");
    RunTicks(*app, 1000ms, {1});
    const FrameGenStatus status = presenter.Status();
    Check(status.standby && !status.threaded && status.state == FrameGenState::kActive && status.dynamic_engagements == 1,
          "toggle standby: turned on again, a game holding the refresh rate is not engaged (" +
              std::string(status.standby ? "standing by" : "engaged") + ", " +
              std::to_string(status.dynamic_engagements) + " engagements)");
    // Standing by, the reason is not the engagement's from before the toggle.
    Check(status.dynamic_reason.find("ran at") == std::string::npos,
          "toggle standby: standing by again, the old engagement's reason is gone (" + status.dynamic_reason + ")");
    // Slow again: it engages after its own run below the refresh rate.
    EngageDynamic(presenter, *app, "toggle standby");
    Check(presenter.Status().dynamic_engagements == 2, "toggle standby: engages again once the game is slow");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "toggle standby: calls stayed ordered");
}

// A hitch is part of a game's pace, not a pause: a 30 fps game whose frames
// take six refreshes now and then still engages Dynamic, and a frame of 150
// ms at a 4x multiplier (an interval of 44 ms) is measured and covered
// across like any other. A pause is at least 250 ms, and at least four of
// the game's own frame intervals.
void TestHitchesAreNotPauses() {
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
        // A six-refresh frame about every quarter second, closer together
        // than the run below the refresh rate engaging takes (QuickDynamic).
        RunTicks(*app, 3000ms, {3, 3, 3, 3, 3, 6}, [&](const XrFrameState&) { return Engaged(presenter); });
        Check(Engaged(presenter), "hitches: a 30 fps game with hitches engages Dynamic");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "hitches (standing by): calls stayed ordered");
    }
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
    const FrameGenStatus before = presenter.Status();
    RunWork(*app, 1ms, {150ms});
    RunWork(*app, 400ms, kLightWork);
    const FrameGenStatus after = presenter.Status();
    Check(after.pauses == before.pauses, "hitches: a 150 ms frame at 4x is not a pause (" +
                                             std::to_string(after.pauses - before.pauses) + " counted)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "hitches: calls stayed ordered");
}

// Interpolating at the game's own rate, the refresh rate, a real frame
// follows the newest one by a refresh: its own refresh shows its exact copy
// (its capture is needed then), and its motion only a refresh later, for a
// cover frame. An analysis ending after its frame's refresh, within the
// next, is on time.
void TestInterpolateMeasureLagIsOnTime() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(GuardDynamic());
    RunWork(*app, 3000ms, JitterWork(runtime.period), [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), "interpolate on time: dynamic engages");
    // Engaged, the game now ends each frame well within a refresh: one real
    // frame a refresh, each shown its lead of refreshes after its wait (a
    // game that misses none keeps the lead it has).
    XrDuration lead = 0;
    RunWork(*app, 600ms, kLightWork, [&](const XrFrameState& state) {
        // The refresh its wait was released in, and the one it is shown in.
        const auto tick = (std::chrono::steady_clock::now() - runtime.epoch) / std::chrono::nanoseconds(runtime.period);
        const XrTime released = 1'000'000'000 + (tick + 1) * runtime.period;
        lead = std::max<XrDuration>(lead, (state.predictedDisplayTime - released + runtime.period / 2) / runtime.period);
        return false;
    });
    // Measured once that lead has passed after its end: past its own
    // refresh, before the next.
    gpu.measure_lag = std::chrono::microseconds(lead * kPeriod / 1000);
    const FrameGenStatus before = presenter.Status();
    RunWork(*app, 1000ms, kLightWork);
    const FrameGenStatus after = presenter.Status();
    Check(lead >= 1 && after.analyses_timed - before.analyses_timed > 20, "interpolate on time: analyses are timed");
    Check(after.analyses_late == before.analyses_late,
          "interpolate on time: none counts late (" + std::to_string(after.analyses_late - before.analyses_late) +
              " of " + std::to_string(after.analyses_timed - before.analyses_timed) + ", lead " +
              std::to_string(lead) + ")");
    Check(Engaged(presenter) && after.dynamic_stand_downs == 0,
          "interpolate on time: no stand-down (" + after.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "interpolate on time: calls stayed ordered");
}

// Interpolating at 2x, the refresh before a real frame's own shows it moved:
// its motion is needed then. Measured a refresh and a half after its
// capture, a lead of two refreshes (what a quick game settles at) shows it
// unmoved there (unmeasured); such refreshes count against the lead, which
// grows until its motion is ready in time, and holds there.
void TestInterpolateLeadMakesRoomForAnalysis() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k2, app, runtime, owned);
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 800ms, kLightWork);
    const FrameGenStatus settled = presenter.Status();
    gpu.measure_lag = std::chrono::microseconds(kPeriod * 3 / 2 / 1000);
    RunWork(*app, 800ms, kLightWork);
    const FrameGenStatus before = presenter.Status();
    const int frames = RunWork(*app, 800ms, kLightWork);
    const FrameGenStatus after = presenter.Status();
    Check(settled.lead_slots <= 2, "analysis lead: a quick game's lead settles at two refreshes or fewer (" +
                                       std::to_string(settled.lead_slots) + ")");
    Check(after.lead_slots >= 3, "analysis lead: the lead makes room for the analysis (" +
                                     std::to_string(after.lead_slots) + " refreshes)");
    Check((after.unmeasured_copies - before.unmeasured_copies) * 10 <= static_cast<std::uint64_t>(frames),
          "analysis lead: frames between real frames are moved, not shown unmeasured (" +
              std::to_string(after.unmeasured_copies - before.unmeasured_copies) + " unmeasured of " +
              std::to_string(frames) + " real frames)");
    Check(gpu.unmeasured_moves == 0, "analysis lead: nothing moved before its motion was measured");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "analysis lead: calls stayed ordered");
}

// An MSFS-like game on Interpolate and Dynamic (pipelined, ending frames a
// period late, rendering 26 and 18 ms in turn, its analyses 6 ms on the
// optical flow hardware): the benefit guard judges window after window and
// stands nothing down; most analyses end before the frames moved by them
// are needed.
void TestBenefitGuardInterpolateMsfs() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.ofa = 6ms;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    presenter.SetDynamicPolicy(GuardDynamic());
    app->end_offset_periods = 1;
    RunPipelined(*app, 900ms, 16ms, 26ms, {}, nullptr, 8ms);
    Check(Engaged(presenter), "interpolate guard: dynamic engages");
    const FrameGenStatus before = presenter.Status();
    RunPipelined(*app, 2500ms, 16ms, 26ms, {}, nullptr, 8ms);
    const FrameGenStatus after = presenter.Status();
    const std::uint64_t timed = after.analyses_timed - before.analyses_timed;
    const std::uint64_t late = after.analyses_late - before.analyses_late;
    Check(timed > 30 && late * 2 < timed, "interpolate guard: analyses are mostly on time (" + std::to_string(late) +
                                              " of " + std::to_string(timed) + " late)");
    Check(Engaged(presenter) && after.dynamic_stand_downs == 0,
          "interpolate guard: nothing stands down (" + after.dynamic_reason + ")");
    Check(after.distinct_fps > 0.85 * after.output_fps, "interpolate guard: refreshes show new frames (" +
                                                            std::to_string(after.distinct_fps) + " of " +
                                                            std::to_string(after.output_fps) + " fps)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "interpolate guard: calls stayed ordered");
}

// --- quad views composed by Frame Generation -----------------------------------------

// Emulated quad views as the layer's compose site hands them over: views 0/1
// peripheral and 2/3 focus, each with a swapchain of its own and of its own
// size, and the layer's compositor outputs the eyes are composed into when
// Frame Generation does not compose them.
const std::array<XrSwapchain, 4> kQuadViews{reinterpret_cast<XrSwapchain>(0x41), reinterpret_cast<XrSwapchain>(0x42),
                                            reinterpret_cast<XrSwapchain>(0x43), reinterpret_cast<XrSwapchain>(0x44)};
const std::array<XrSwapchain, 2> kComposedOutputs{reinterpret_cast<XrSwapchain>(0x45),
                                                  reinterpret_cast<XrSwapchain>(0x46)};
constexpr FrameGenExtent kPeripheryExtent{1000, 1000};
constexpr FrameGenExtent kFocusExtent{800, 700};
constexpr FrameGenExtent kCanvasExtent{1200, 1100};

// Each eye composed from its periphery and its focus view, inset at `left`.
FrameGenComposition QuadComposition(float left, FrameGenExtent canvas = kCanvasExtent) {
    FrameGenComposition composition;
    composition.output = canvas;
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        FrameGenComposeEye composed;
        composed.peripheral = eye;
        composed.focus = eye + 2;
        composed.focus_rect = {left, 0.3f, left + 0.4f, 0.7f};
        composed.feather = {0.01f, 0.01f};
        composition.eyes.push_back(composed);
    }
    return composition;
}

// A quad views game behind the layer: its four views released (each release
// captured, as the bridge's copy is, unless `uncaptured_release`), then at
// its end the layer offers the frame (ClaimQuadFrame). A claimed frame ends
// with its four views and its composition; any other is composed by the
// layer into its compositor outputs (released as the layer releases them)
// and ends with those two views. The eyes move sideways at `eye_speed` m/s:
// every view of a frame is posed where they are at its display time.
struct QuadApp {
    FrameGenPresenter& presenter;
    Runtime& runtime;
    FrameGenRuntimeCalls calls;
    std::uint32_t images{3}; // each view swapchain's images, cycled
    std::uint32_t image{0};
    std::uint32_t composed_image{0};
    double eye_speed{0.5};
    bool offer{true};       // false: the layer declines (kDeclined)
    std::optional<FrameGenQuadRoute> force; // the layer routes the frame so without offering it
    std::array<bool, 4> uncaptured_release{};
    std::function<float(int)> inset; // each frame's focus rectangle (left edge in output uv)
    FrameGenExtent canvas{kCanvasExtent}; // each eye's composed image
    // Between the claim and the end of a frame (the layer's xrEndFrame, then
    // its forward); false: the layer abandons the frame instead.
    std::function<bool(FrameGenQuadRoute)> after_claim;
    // The game renders nothing (told not to, the headset off): it releases
    // no view and ends its frames without layers.
    bool no_layers{false};
    struct Ended {
        XrTime time{0};
        FrameGenQuadRoute route{FrameGenQuadRoute::kDeclined};
        float x{0.0f};
    };
    std::vector<Ended> ended;
    std::vector<double> release_end_ms; // per frame: its views' releases and its end, milliseconds
    int frames{0};

    QuadApp(FrameGenPresenter& p, Runtime& r) : presenter(p), runtime(r), calls(r.Calls()) {}

    float X(XrTime time) const {
        return static_cast<float>(eye_speed * static_cast<double>(time - 1'000'000'000) * 1e-9);
    }

    XrFrameState Wait() {
        XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        Check(XR_SUCCEEDED(presenter.WaitFrame(kSession, &info, &state, calls)), "quad app wait");
        return state;
    }

    void Begin() {
        XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
        Check(XR_SUCCEEDED(presenter.BeginFrame(kSession, &info, calls)), "quad app begin");
    }

    void Release() {
        image = (image + 1) % images;
        for (std::size_t i = 0; i < kQuadViews.size(); ++i) {
            presenter.NoteRelease(kQuadViews[i], image);
            if (!uncaptured_release[i]) presenter.CaptureOnRelease(kQuadViews[i], image);
        }
        uncaptured_release = {};
    }

    XrResult End(XrTime time) {
        if (no_layers) {
            XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
            info.displayTime = time;
            info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            return presenter.EndFrame(kSession, &info, calls);
        }
        const float x = X(time);
        std::array<XrCompositionLayerProjectionView, 4> source{};
        for (std::size_t i = 0; i < source.size(); ++i) {
            source[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            source[i].pose.orientation.w = 1.0f;
            source[i].pose.position.x = x;
            source[i].fov = i < 2 ? XrFovf{-0.8f, 0.8f, 0.8f, -0.8f} : XrFovf{-0.3f, 0.3f, 0.3f, -0.3f};
            const FrameGenExtent extent = i < 2 ? kPeripheryExtent : kFocusExtent;
            source[i].subImage.swapchain = kQuadViews[i];
            source[i].subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(extent.width),
                                                     static_cast<std::int32_t>(extent.height)}};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = kSpace;
        projection.viewCount = 4;
        projection.views = source.data();
        FrameGenQuadFrame quad;
        if (force) {
            quad.route = *force;
        } else if (offer) {
            quad.route = presenter.ClaimQuadFrame(projection);
        }
        ended.push_back({time, quad.route, x});
        const int frame = frames++;
        if (after_claim && !after_claim(quad.route)) {
            presenter.AbandonFrame(time);
            return XR_SUCCESS;
        }
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.displayTime = time;
        info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        std::array<XrCompositionLayerProjectionView, 2> composed{};
        XrCompositionLayerProjection composed_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        if (quad.route == FrameGenQuadRoute::kClaimed) {
            quad.composition = QuadComposition(inset ? inset(frame) : 0.3f, canvas);
            for (std::size_t eye = 0; eye < 2; ++eye) {
                quad.eyes[eye] = source[eye];
                quad.eyes[eye].subImage = {};
            }
        } else {
            composed_image = (composed_image + 1) % 3;
            for (std::size_t eye = 0; eye < 2; ++eye) {
                presenter.CaptureOnRelease(kComposedOutputs[eye], composed_image);
                presenter.NoteRelease(kComposedOutputs[eye], composed_image);
                {
                    // What the runtime shows from the layer's compositor
                    // outputs from now on: this frame's image (its eye at x).
                    std::scoped_lock lock(g_xr_mutex);
                    g_shown[kComposedOutputs[eye]] = x;
                }
                composed[eye] = source[eye];
                composed[eye].subImage.swapchain = kComposedOutputs[eye];
                composed[eye].subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(canvas.width),
                                                             static_cast<std::int32_t>(canvas.height)}};
            }
            composed_layer.space = kSpace;
            composed_layer.viewCount = 2;
            composed_layer.views = composed.data();
            layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&composed_layer);
            if (quad.route == FrameGenQuadRoute::kUncaptured) {
                quad.composition = QuadComposition(inset ? inset(frame) : 0.3f, canvas);
                quad.source_views = source;
                quad.source_space = kSpace;
            }
        }
        info.layerCount = 1;
        info.layers = &layer;
        return presenter.EndFrame(kSession, &info, calls, &quad);
    }

    XrFrameState Frame(std::chrono::microseconds work = 0us) {
        const XrFrameState state = Wait();
        Begin();
        if (work.count()) SleepPrecise(std::chrono::steady_clock::now() + work);
        const auto released = std::chrono::steady_clock::now();
        if (!no_layers) Release();
        Check(XR_SUCCEEDED(End(state.predictedDisplayTime)), "quad app end");
        release_end_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - released).count());
        return state;
    }

    int Run(std::chrono::milliseconds duration, std::chrono::microseconds work = 4000us,
            const std::function<bool()>& until = {}) {
        return RunWorks(duration, {work}, until);
    }

    // Frames working works[i % size] each.
    int RunWorks(std::chrono::milliseconds duration, const std::vector<std::chrono::microseconds>& works,
                 const std::function<bool()>& until = {}) {
        const auto start = std::chrono::steady_clock::now();
        int count = 0;
        while (std::chrono::steady_clock::now() - start < duration) {
            Frame(works[static_cast<std::size_t>(count) % works.size()]);
            ++count;
            if (until && until()) break;
        }
        return count;
    }

    std::size_t Routed(FrameGenQuadRoute route, std::size_t from = 0) const {
        return static_cast<std::size_t>(std::count_if(ended.begin() + static_cast<std::ptrdiff_t>(std::min(from, ended.size())),
                                                      ended.end(), [&](const Ended& e) { return e.route == route; }));
    }
};

void StartQuad(FrameGenPresenter& presenter, GpuLog& gpu, FrameGenSynthesisMode mode, FrameGenMultiplier multiplier) {
    gpu.threaded = true;
    gpu.jit = true;
    gpu.composition = true;
    StartPresenter(presenter, gpu, mode, kFormat, kAppUsage, multiplier);
    for (const XrSwapchain view : kQuadViews) presenter.TrackSwapchain(view, kFormat, 1, kAppUsage);
    for (const XrSwapchain output : kComposedOutputs) presenter.TrackSwapchain(output, kFormat, 1, kAppUsage);
    std::scoped_lock lock(g_xr_mutex);
    for (const XrSwapchain output : kComposedOutputs) g_shown.erase(output);
}

// A quad views game's work per frame against a paced runtime, standing by:
// each frame takes ticks[i] refreshes, its work ending 3 ms before the last
// (QuadApp::RunWorks; as RunTicks).
std::vector<std::chrono::microseconds> QuadTicks(const Runtime& runtime, const std::vector<int>& ticks) {
    std::vector<std::chrono::microseconds> works;
    for (const int tick : ticks) {
        works.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::nanoseconds(tick * runtime.period)) -
                        3ms);
    }
    return works;
}

// The submissions' first view positions (the eyes move +x) never step back:
// nothing older is shown after anything newer. Returns the steps back.
std::size_t StepsBack(Runtime& runtime, std::size_t first) {
    std::scoped_lock lock(g_xr_mutex);
    std::size_t backwards = 0;
    std::optional<float> last;
    for (std::size_t i = first; i < runtime.submitted_left_x.size(); ++i) {
        if (runtime.submitted_swapchains[i].empty()) continue;
        backwards += last && runtime.submitted_left_x[i] + 1e-6f < *last ? 1 : 0;
        last = runtime.submitted_left_x[i];
    }
    return backwards;
}

bool ShowsComposedOutputs(const Submission& submission) {
    return submission.swapchains.size() == 2 && submission.swapchains[0] == kComposedOutputs[0] &&
           submission.swapchains[1] == kComposedOutputs[1];
}

// Frame Generation's own output images, not the game's or the layer's.
bool ShowsFrameGenOutputs(const Submission& submission) {
    if (submission.swapchains.size() != 2) return false;
    for (const XrSwapchain swapchain : submission.swapchains) {
        if (std::find(kQuadViews.begin(), kQuadViews.end(), swapchain) != kQuadViews.end() ||
            std::find(kComposedOutputs.begin(), kComposedOutputs.end(), swapchain) != kComposedOutputs.end() ||
            swapchain == kLeft || swapchain == kRight) {
            return false;
        }
    }
    return true;
}

int Composed(GpuLog& gpu, const std::function<bool(const GpuLog::Composed&)>& match) {
    std::scoped_lock lock(gpu.syntheses_mutex);
    return static_cast<int>(std::count_if(gpu.composed.begin(), gpu.composed.end(), match));
}

// Engaged, every frame is claimed: its four views are captured at release,
// measured with their composition, and every refresh, real or generated, is
// composed into one output image per eye of the canvas's size; the runtime
// gets two views, posed as the periphery, showing what was written for them.
void TestQuadClaimAndCompose() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    Check(gpu.captures == 0, "quad: nothing captured before engagement");
    const std::size_t first = runtime.Submissions();
    const std::size_t first_frame = app.ended.size();
    const auto start = std::chrono::steady_clock::now();
    const int frames = app.Run(1200ms);
    const double elapsed = Seconds(std::chrono::steady_clock::now() - start);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && status.quad, "quad: Frame Generation composes the quad views itself");
    const std::size_t claimed = app.Routed(FrameGenQuadRoute::kClaimed, first_frame);
    Check(claimed + 2 >= static_cast<std::size_t>(frames) && status.quad_claimed == claimed,
          "quad: every engaged frame is claimed (" + std::to_string(claimed) + " of " + std::to_string(frames) + ")");
    Check(gpu.captures >= static_cast<int>(4 * claimed),
          "quad: each claimed frame's four views are captured (" + std::to_string(gpu.captures.load()) + ")");
    Check(gpu.last.size() == 4 && gpu.last[0].request.extent.width == kPeripheryExtent.width &&
              gpu.last[2].request.extent.width == kFocusExtent.width && gpu.last[2].source.swapchain == kQuadViews[2],
          "quad: the four views are measured, each at its own size");
    Check(gpu.composed_analyses == gpu.analyzes && gpu.analyzes >= static_cast<int>(claimed) - 1,
          "quad: every analysis has the frame's composition (" + std::to_string(gpu.composed_analyses.load()) + " of " +
              std::to_string(gpu.analyzes.load()) + ")");
    const auto log = runtime.Snapshot();
    std::size_t own = 0;
    for (std::size_t i = first + 4; i < log.size(); ++i) own += ShowsFrameGenOutputs(log[i]) ? 1 : 0;
    Check(own + 4 + first >= log.size(), "quad: refreshes show Frame Generation's composed outputs (" +
                                             std::to_string(own) + " of " + std::to_string(log.size() - first) + ")");
    Check(runtime.not_stereo == 0, "quad: the runtime never gets four views");
    Check(runtime.mismatched_images == 0, "quad: every image shown was composed for its views (" +
                                              std::to_string(runtime.mismatched_images.load()) + " mismatched)");
    {
        std::scoped_lock lock(g_xr_mutex);
        Check(g_creates == 2 && g_create_sizes[0].first == kCanvasExtent.width &&
                  g_create_sizes[0].second == kCanvasExtent.height && g_create_sizes[1] == g_create_sizes[0],
              "quad: one output image per eye, of the canvas's size (" + std::to_string(g_creates) + " made)");
    }
    const int moved = Composed(gpu, [](const GpuLog::Composed& entry) {
        return entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction > 0.0f;
    });
    bool composed_only = false;
    {
        std::scoped_lock lock(gpu.syntheses_mutex);
        composed_only = gpu.syntheses.size() == gpu.composed.size();
    }
    Check(moved > frames / 2 && composed_only,
          "quad: frames between real frames are composed from them (" + std::to_string(moved) + ")");
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    Check(static_cast<double>(runtime.Submissions() - first) / elapsed > refresh * 0.85, "quad: every refresh gets a frame");
    Check(status.distinct_fps > 0.8 * status.output_fps, "quad: refreshes show new frames (" +
                                                             std::to_string(status.distinct_fps) + " of " +
                                                             std::to_string(status.output_fps) + " fps)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "quad: calls stayed ordered");
}

// The views are set up from the first engaged frame (the backend sets a view
// up when its release was not captured): its claim reports it uncaptured,
// the layer composes it, it is shown as it is at its own refresh, and the
// frames after it are claimed.
void TestQuadSetupThroughComposedFrame() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.setup_views = true;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    const std::size_t first_frame = app.ended.size();
    app.Run(600ms);
    std::size_t setup = app.ended.size();
    for (std::size_t i = first_frame; i < app.ended.size(); ++i) {
        if (app.ended[i].route != FrameGenQuadRoute::kNotPresenting) {
            setup = i;
            break;
        }
    }
    Check(setup < app.ended.size() && app.ended[setup].route == FrameGenQuadRoute::kUncaptured,
          "quad set-up: the first engaged frame's views were not captured");
    Check(app.Routed(FrameGenQuadRoute::kUncaptured) == 1 && app.ended.size() > setup + 5 &&
              app.Routed(FrameGenQuadRoute::kClaimed, setup + 1) == app.ended.size() - setup - 1,
          "quad set-up: every frame after it is claimed");
    const FrameGenStatus status = presenter.Status();
    Check(status.quad_setups == 1 && gpu.setups == 1, "quad set-up: the views are set up once");
    const auto log = runtime.Snapshot();
    const float x = app.ended[setup].x;
    const auto shown = std::find_if(log.begin(), log.end(), [&](const Submission& submission) {
        return ShowsComposedOutputs(submission) && submission.time >= app.ended[setup].time - kPeriod / 2;
    });
    Check(shown != log.end(), "quad set-up: the frame is shown as the layer composed it");
    {
        std::scoped_lock lock(g_xr_mutex);
        const std::size_t index = static_cast<std::size_t>(shown - log.begin());
        Check(std::abs(runtime.submitted_left_x[index] - x) < 1e-6f,
              "quad set-up: at its own refresh, its own composed views");
    }
    Check(runtime.not_stereo == 0 && runtime.mismatched_images == 0, "quad set-up: two views, images matching them");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "quad set-up: calls stayed ordered");
}

// Frames the layer composes while Frame Generation composes the rest are
// queued in order at their own display time: each is shown from its own
// refresh, the frames before it are shown before it, and nothing older (the
// frame passed through before the start) is shown after anything newer.
void TestQuadFallbackShownAsIs() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(300ms);
    const std::size_t first = runtime.Submissions();
    const std::size_t first_frame = app.ended.size();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; std::chrono::steady_clock::now() - start < 1200ms; ++i) {
        // Every fifth frame is not a frame of Frame Generation's own (the
        // layer composes it, as when the claim finds it busy).
        app.force = i % 5 == 4 ? std::optional<FrameGenQuadRoute>(FrameGenQuadRoute::kBusy) : std::nullopt;
        app.Frame(4000us);
    }
    app.force.reset();
    app.Run(100ms); // the last frames the layer composed reach their refreshes
    const auto log = runtime.Snapshot();
    std::vector<float> left_x;
    {
        std::scoped_lock lock(g_xr_mutex);
        left_x.assign(runtime.submitted_left_x.begin() + static_cast<std::ptrdiff_t>(first), runtime.submitted_left_x.end());
    }
    std::size_t busy = 0, shown_in_time = 0;
    std::string off;
    for (std::size_t i = first_frame; i < app.ended.size(); ++i) {
        if (app.ended[i].route != FrameGenQuadRoute::kBusy) continue;
        ++busy;
        // First shown at its own refresh (half a refresh either side), never before.
        bool shown = false;
        for (std::size_t s = first; s < log.size() && !shown; ++s) {
            if (ShowsComposedOutputs(log[s]) && std::abs(left_x[s - first] - app.ended[i].x) < 1e-6f) {
                shown = true;
                const XrDuration early = app.ended[i].time - log[s].time;
                if (std::llabs(early) <= kPeriod / 2) {
                    ++shown_in_time;
                } else {
                    off += " " + std::to_string(static_cast<double>(early) / static_cast<double>(kPeriod)) + " early";
                }
            }
        }
        if (!shown) off += " never";
    }
    Check(busy >= 10 && shown_in_time == busy, "fallback as is: each frame the layer composed is first shown at its own "
                                               "refresh (" + std::to_string(shown_in_time) + " of " +
                                                   std::to_string(busy) + ";" + off + ")");
    std::size_t backwards = 0;
    for (std::size_t i = 1; i < left_x.size(); ++i) backwards += left_x[i] + 1e-6f < left_x[i - 1] ? 1 : 0;
    Check(backwards == 0, "fallback as is: nothing older is shown after anything newer (" + std::to_string(backwards) +
                              " steps back)");
    const FrameGenStatus status = presenter.Status();
    Check(status.quad && status.threaded, "fallback as is: Frame Generation still composes the rest");
    Check(Composed(gpu, [](const GpuLog::Composed& entry) { return entry.fraction > 0.0f; }) > 20,
          "fallback as is: frames are still generated between the claimed ones");
    Check(runtime.not_stereo == 0 && runtime.mismatched_images == 0, "fallback as is: two views, images matching them");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "fallback as is: calls stayed ordered");
}

// A claimed frame never reaches the runtime with its four views: not when
// the session stops between its claim and its end, not when presenting from
// the presenter thread ends meanwhile (it is shown composed if the thread is
// still presenting when it ends, else not at all), and an abandoned claim
// does not keep later frames from being claimed.
void TestQuadNeverSubmitsFourViews() {
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
        QuadApp app(presenter, runtime);
        for (int i = 0; i < 3; ++i) app.Frame();
        app.Run(300ms);
        bool stopped = false;
        std::size_t before = 0;
        app.after_claim = [&](FrameGenQuadRoute route) {
            if (route == FrameGenQuadRoute::kClaimed && !stopped) {
                stopped = true;
                presenter.Stop();
                before = runtime.Submissions();
            }
            return true;
        };
        const XrFrameState state = app.Wait();
        app.Begin();
        app.Release();
        Check(app.End(state.predictedDisplayTime) == XR_SUCCESS && stopped,
              "four views (stop): a claimed frame ending after the session stopped is swallowed");
        Check(runtime.Submissions() == before && runtime.not_stereo == 0,
              "four views (stop): nothing reaches the runtime");
        Check(!runtime.ordering_violation && !g_release_violation, "four views (stop): calls stayed ordered");
    }
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
        QuadApp app(presenter, runtime);
        for (int i = 0; i < 3; ++i) app.Frame();
        app.Run(300ms);
        // Turned off between the claim and the end: the next wait (another
        // thread's) drains the presenter thread, which presents the frame at
        // its own refresh before it stops.
        std::optional<XrFrameState> next;
        std::thread waiter;
        float claimed_x = 0.0f;
        app.after_claim = [&](FrameGenQuadRoute route) {
            if (route == FrameGenQuadRoute::kClaimed && !waiter.joinable()) {
                claimed_x = app.ended.back().x;
                Check(!presenter.Toggle(), "four views (exit): turned off");
                waiter = std::thread([&] { next = app.Wait(); });
                // The frame then ends before its own refresh, already draining.
                SleepPrecise(std::chrono::steady_clock::now() + 4ms);
            }
            return true;
        };
        const XrFrameState state = app.Wait();
        app.Begin();
        app.Release();
        Check(app.End(state.predictedDisplayTime) == XR_SUCCESS, "four views (exit): the claimed frame ends");
        waiter.join();
        app.after_claim = nullptr;
        Check(next.has_value() && !presenter.Status().threaded, "four views (exit): the next wait went inline");
        app.Begin();
        app.Release();
        Check(app.End(next->predictedDisplayTime) == XR_SUCCESS, "four views (exit): the inline frame ends");
        app.Run(200ms);
        const auto log = runtime.Snapshot();
        bool shown = false;
        {
            std::scoped_lock lock(g_xr_mutex);
            for (std::size_t i = 0; i < log.size(); ++i) {
                shown = shown || (ShowsFrameGenOutputs(log[i]) && std::abs(runtime.submitted_left_x[i] - claimed_x) < 1e-6f);
            }
        }
        // Draining, the presenter thread shows every kept frame up to its
        // own refresh before it stops: the claimed frame is not dropped.
        Check(shown, "four views (exit): the claimed frame is shown, composed, before the presenter thread stops");
        Check(app.Routed(FrameGenQuadRoute::kNotPresenting) >= 10 && ShowsComposedOutputs(log.back()),
              "four views (exit): the layer composes the frames after it");
        Check(runtime.not_stereo == 0, "four views (exit): the runtime never gets four views");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "four views (exit): calls stayed ordered");
    }
    {
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
        QuadApp app(presenter, runtime);
        for (int i = 0; i < 3; ++i) app.Frame();
        app.Run(300ms);
        bool abandoned = false;
        app.after_claim = [&](FrameGenQuadRoute route) {
            if (route == FrameGenQuadRoute::kClaimed && !abandoned) {
                abandoned = true;
                return false;
            }
            return true;
        };
        const std::size_t from = app.ended.size();
        const auto start = std::chrono::steady_clock::now();
        const int frames = app.Run(500ms);
        Check(abandoned, "four views (abandon): a claimed frame was abandoned");
        Check(app.Routed(FrameGenQuadRoute::kClaimed, from) + 3 >= static_cast<std::size_t>(frames) &&
                  frames / Seconds(std::chrono::steady_clock::now() - start) > 35.0,
              "four views (abandon): the frames after it are claimed, at the game's pace");
        Check(runtime.not_stereo == 0, "four views (abandon): the runtime never gets four views");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "four views (abandon): calls stayed ordered");
    }
    {
        // Claimed frames none of which can be written: until the layer
        // composes again, every refresh shows the frame passed through
        // before the start, never a claimed frame's own layers.
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.compose_fail = true;
        Runtime runtime;
        runtime.paced = true;
        StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
        QuadApp app(presenter, runtime);
        for (int i = 0; i < 3; ++i) app.Frame();
        const std::size_t first = runtime.Submissions();
        app.Run(400ms);
        Check(app.Routed(FrameGenQuadRoute::kClaimed) >= 3, "four views (never written): frames are claimed");
        const auto log = runtime.Snapshot();
        std::size_t shown = 0;
        for (std::size_t i = first; i < log.size(); ++i) shown += log[i].layers == 1 && log[i].swapchains.size() == 2 ? 1 : 0;
        Check(shown == log.size() - first && runtime.not_stereo == 0,
              "four views (never written): every refresh shows two views (" + std::to_string(shown) + " of " +
                  std::to_string(log.size() - first) + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, "four views (never written): calls stayed ordered");
    }
}

// A claimed frame's end is its own, ahead of a frame dropped after its
// claim (the presenter thread stopped, its unclaimed frame dropped by the
// next wait): the dropped frame's end is swallowed, and presenting resumes
// without frames left in flight.
void TestQuadClaimAheadOfDroppedFrames() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    presenter.SetRetryPolicy(50ms, 3);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(300ms);
    // Frame N begun, frame N + 1 waited (pipelined), then N claimed.
    const XrFrameState n = app.Wait();
    app.Begin();
    app.Release();
    const XrFrameState n1 = app.Wait();
    std::optional<XrFrameState> n2;
    app.after_claim = [&](FrameGenQuadRoute route) {
        Check(route == FrameGenQuadRoute::kClaimed, "dropped: frame N is claimed");
        // The presenter thread stops (a runtime failure); the wait for N + 2
        // drops N + 1, which is not claimed.
        runtime.fail_next_wait = true;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (presenter.Status().threaded && std::chrono::steady_clock::now() < deadline) {
            SleepPrecise(std::chrono::steady_clock::now() + 2ms);
        }
        Check(!presenter.Status().threaded, "dropped: the presenter thread stopped");
        n2 = app.Wait();
        return true;
    };
    Check(app.End(n.predictedDisplayTime) == XR_SUCCESS, "dropped: frame N ends");
    app.after_claim = nullptr;
    const std::size_t before = runtime.Submissions();
    app.Begin();
    app.Release();
    Check(app.End(n1.predictedDisplayTime) == XR_SUCCESS, "dropped: frame N + 1 ends");
    Check(runtime.Submissions() == before, "dropped: the dropped frame reaches nothing");
    app.Begin();
    app.Release();
    Check(app.End(n2->predictedDisplayTime) == XR_SUCCESS, "dropped: frame N + 2 ends");
    Check(runtime.Submissions() == before + 1 && ShowsComposedOutputs(runtime.Snapshot().back()),
          "dropped: frame N + 2 is shown as the layer composed it");
    // Retried after the pause: composing resumes with nothing in flight.
    const std::size_t from = app.ended.size();
    std::chrono::steady_clock::duration longest{};
    auto last = std::chrono::steady_clock::now();
    const auto start = last;
    while (std::chrono::steady_clock::now() - start < 1500ms) {
        app.Frame(4000us);
        const auto now = std::chrono::steady_clock::now();
        longest = std::max(longest, now - last);
        last = now;
    }
    Check(presenter.Status().quad && app.Routed(FrameGenQuadRoute::kClaimed, from) > 20,
          "dropped: composing resumes after the pause");
    Check(longest < 300ms, "dropped: no frame is held for one left in flight (" +
                               std::to_string(Seconds(longest)) + " s)");
    Check(runtime.not_stereo == 0, "dropped: the runtime never gets four views");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "dropped: calls stayed ordered");
}

// Every frame written from a claimed frame, its own refresh's or a generated
// one, is composed with that frame's composition (its inset) and shows its
// eye views, even while newer frames with another inset are kept.
void TestQuadKeepsRealFramesInset() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    app.inset = [](int frame) { return 0.2f + 0.005f * static_cast<float>(frame % 40); };
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(1000ms);
    std::size_t matched = 0, total = 0;
    {
        std::scoped_lock lock(gpu.syntheses_mutex);
        for (const GpuLog::Composed& entry : gpu.composed) {
            const auto analysis = gpu.analysis_compositions.find(entry.frame);
            ++total;
            matched += analysis != gpu.analysis_compositions.end() &&
                               analysis->second.eyes[0].focus_rect == entry.composition.eyes[0].focus_rect &&
                               analysis->second.eyes[1].focus_rect == entry.composition.eyes[1].focus_rect
                           ? 1
                           : 0;
        }
    }
    Check(total > 60 && matched == total, "inset: every frame is composed with its real frame's inset (" +
                                              std::to_string(matched) + " of " + std::to_string(total) + ")");
    Check(runtime.mismatched_images == 0, "inset: every frame shows its real frame's eye views (" +
                                              std::to_string(runtime.mismatched_images.load()) + " mismatched)");
    Check(runtime.not_stereo == 0, "inset: the runtime never gets four views");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "inset: calls stayed ordered");
}

// The layer declines its frames: the presenter
// thread restarts on the layer's composed views (canvas), then, offered
// again, composing quad views itself; the output images serve both.
void TestQuadDeclinedSwitchesToCanvas() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(400ms);
    Check(presenter.Status().quad, "declined: composing quad views first");
    app.offer = false;
    app.Run(2000ms, 4000us, [&] {
        const FrameGenStatus status = presenter.Status();
        return status.threaded && !status.quad;
    });
    Check(presenter.Status().threaded && !presenter.Status().quad, "declined: restarts on the composed views");
    const std::size_t first = runtime.Submissions();
    app.Run(500ms);
    Check(gpu.last.size() == 2 && gpu.last[0].source.swapchain == kComposedOutputs[0] &&
              presenter.CapturesFrom(kComposedOutputs[0]) && !presenter.CapturesFrom(kQuadViews[0]),
          "declined: frames are generated from the composed views");
    const auto log = runtime.Snapshot();
    std::size_t own = 0;
    for (std::size_t i = first; i < log.size(); ++i) own += ShowsFrameGenOutputs(log[i]) ? 1 : 0;
    Check(own * 10 >= (log.size() - first) * 9, "declined: refreshes show Frame Generation's frames");
    app.offer = true;
    app.Run(2000ms, 4000us, [&] { return presenter.Status().quad; });
    const std::size_t from = app.ended.size();
    app.Run(300ms);
    Check(presenter.Status().quad && app.Routed(FrameGenQuadRoute::kClaimed, from) > 10,
          "declined: offered again, it composes quad views again");
    {
        std::scoped_lock lock(g_xr_mutex);
        Check(g_creates == 2, "declined: switching reallocates no output (" + std::to_string(g_creates) + " made)");
    }
    Check(runtime.not_stereo == 0 && runtime.mismatched_images == 0, "declined: two views, images matching them");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "declined: calls stayed ordered");
}

// Composed frames that keep failing to be written hand the quad views back
// to the layer for a while (doubling), without pausing Frame Generation or
// using its retries: it stands by meanwhile (the presenter thread stops, and
// the layer's frames go straight to the runtime, each with its own views),
// and composing resumes afterwards.
void TestQuadWriteFailuresDecline() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    presenter.SetQuadDecline(500ms, 2000ms);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(400ms);
    const FrameGenStatus before = presenter.Status();
    Check(before.quad && before.quad_write_declines == 0, "write failures: composing first");
    gpu.compose_fail = true;
    app.Run(1000ms, 4000us, [&] { return presenter.Status().quad_write_declines > 0; });
    const auto declined_at = std::chrono::steady_clock::now();
    FrameGenStatus status = presenter.Status();
    Check(status.quad_write_declines == 1 && status.quad_declining && gpu.compose_failures >= 10 &&
              gpu.compose_failures <= 20,
          "write failures: ten failed writes in a row hand the views back (" +
              std::to_string(gpu.compose_failures.load()) + " failed)");
    Check(status.state == FrameGenState::kActive && status.failures == before.failures && !status.retry_pending,
          "write failures: Frame Generation is not paused, nor counts failures toward a pause");
    app.Run(100ms); // the frames in flight drain
    const std::size_t first = runtime.Submissions();
    const std::size_t from = app.ended.size();
    app.Run(300ms);
    status = presenter.Status();
    Check(app.Routed(FrameGenQuadRoute::kNotPresenting, from) == app.ended.size() - from,
          "write failures: while declined, the layer composes every frame");
    Check(!status.threaded && status.state == FrameGenState::kActive && status.full_rate && status.quad_declining &&
              presenter.OsdText().find("Standby") != std::string::npos,
          "write failures: while declined, Frame Generation stands by (" + presenter.OsdText() + ")");
    const auto log = runtime.Snapshot();
    std::size_t as_is = 0;
    for (std::size_t i = first; i < log.size(); ++i) as_is += ShowsComposedOutputs(log[i]) ? 1 : 0;
    Check(as_is == log.size() - first && log.size() - first + 3 >= app.ended.size() - from,
          "write failures: each frame the layer composes goes straight to the runtime (" + std::to_string(as_is) +
              " of " + std::to_string(log.size() - first) + " refreshes, " + std::to_string(app.ended.size() - from) +
              " frames)");
    Check(runtime.mismatched_images == 0, "write failures: every image shown matches its views");
    gpu.compose_fail = false;
    app.Run(1000ms, 4000us, [&] { return !presenter.Status().quad_declining; });
    const double first_decline = Seconds(std::chrono::steady_clock::now() - declined_at);
    const std::size_t resumed = app.ended.size();
    app.Run(300ms);
    status = presenter.Status();
    Check(first_decline >= 0.45 && app.Routed(FrameGenQuadRoute::kClaimed, resumed) > 10 && status.quad_write_declines == 1,
          "write failures: composing resumes after the decline (" + std::to_string(first_decline) + " s)");
    // Failing again: the next decline lasts twice as long.
    gpu.compose_fail = true;
    app.Run(1000ms, 4000us, [&] { return presenter.Status().quad_write_declines > 1; });
    const auto again = std::chrono::steady_clock::now();
    gpu.compose_fail = false;
    app.Run(2000ms, 4000us, [&] { return !presenter.Status().quad_declining; });
    const double second_decline = Seconds(std::chrono::steady_clock::now() - again);
    Check(presenter.Status().quad_write_declines == 2 && second_decline >= 0.9,
          "write failures: the next decline doubles (" + std::to_string(second_decline) + " s)");
    Check(runtime.not_stereo == 0, "write failures: the runtime never gets four views");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "write failures: calls stayed ordered");
}

// Dynamic standing by on quad views: the four views stay the capture
// targets (the layer's composed views never take them over), nothing is
// captured, and the layer composes every frame.
void TestQuadStandbyRecordsViewTargets() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic);
    QuadApp app(presenter, runtime);
    const int frames = app.Run(800ms);
    const FrameGenStatus status = presenter.Status();
    Check(status.standby && !status.threaded && !status.quad, "standby: Dynamic stands by");
    Check(std::all_of(kQuadViews.begin(), kQuadViews.end(), [&](XrSwapchain view) { return presenter.CapturesFrom(view); }) &&
              !presenter.CapturesFrom(kComposedOutputs[0]) && !presenter.CapturesFrom(kComposedOutputs[1]),
          "standby: the four views stay the capture targets");
    Check(gpu.captures == 0 && gpu.executes == 0, "standby: nothing is captured or analyzed");
    Check(app.Routed(FrameGenQuadRoute::kNotPresenting) == static_cast<std::size_t>(frames) &&
              status.quad_declined == static_cast<std::uint64_t>(frames),
          "standby: the layer composes every frame");
    Check(runtime.not_stereo == 0, "standby: the runtime never gets four views");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "standby: calls stayed ordered");
}

// The claim tests what the backend's analysis does: each view's latest
// release captured. A view whose latest release was not captured (the same
// image as its last capture: only the release count tells) is not claimed;
// the layer composes that frame, and the next is claimed again.
void TestQuadClaimNeedsLatestCapture() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    app.images = 1; // every release is image 0
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(300ms);
    Check(app.ended.back().route == FrameGenQuadRoute::kClaimed, "latest capture: frames are claimed");
    const std::size_t skipped = app.ended.size();
    app.uncaptured_release[2] = true;
    app.Frame(4000us);
    app.Run(200ms);
    Check(app.ended[skipped].route == FrameGenQuadRoute::kUncaptured,
          "latest capture: a view whose latest release was not captured is not claimed");
    Check(app.ended[skipped - 1].route == FrameGenQuadRoute::kClaimed &&
              app.ended[skipped + 1].route == FrameGenQuadRoute::kClaimed &&
              app.Routed(FrameGenQuadRoute::kClaimed, skipped + 1) == app.ended.size() - skipped - 1,
          "latest capture: the frames either side are claimed");
    Check(presenter.Status().quad_setups == 1 && runtime.not_stereo == 0, "latest capture: set up from it once");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "latest capture: calls stayed ordered");
}


// The canvas changes size (a live focus scale change): each eye's output
// image is made anew once, frames kept at the old size are not written
// again, and the old images stay (shown again meanwhile) until a frame is
// written into the new ones: no refresh shows nothing or a destroyed image.
void TestQuadOutputsResized() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(400ms);
    const std::size_t first = runtime.Submissions();
    const std::vector<XrSwapchain> old_outputs = runtime.Snapshot().back().swapchains;
    Check(ShowsFrameGenOutputs(runtime.Snapshot().back()), "resized: composing first");
    {
        // The new images are not handed back at once: the last frame
        // written is shown again meanwhile.
        std::scoped_lock lock(g_xr_mutex);
        g_wait_timeouts_on_create = 2;
    }
    app.canvas = {1400, 1300};
    app.Run(600ms);
    {
        std::scoped_lock lock(g_xr_mutex);
        Check(g_creates == 4 && g_create_sizes[2].first == 1400 && g_create_sizes[3].second == 1300,
              "resized: each eye's output is made anew once, at the new size (" + std::to_string(g_creates) + " made)");
        Check(std::count(g_destroyed.begin(), g_destroyed.end(), old_outputs[0]) == 1 &&
                  std::count(g_destroyed.begin(), g_destroyed.end(), old_outputs[1]) == 1,
              "resized: the old outputs are destroyed");
    }
    const auto log = runtime.Snapshot();
    std::size_t shown = 0;
    for (std::size_t i = first; i < log.size(); ++i) shown += log[i].layers == 1 && log[i].swapchains.size() == 2 ? 1 : 0;
    Check(shown == log.size() - first, "resized: every refresh shows two views (" + std::to_string(shown) + " of " +
                                           std::to_string(log.size() - first) + ")");
    Check(ShowsFrameGenOutputs(log.back()) && log.back().swapchains != old_outputs,
          "resized: refreshes show the new outputs");
    Check(!runtime.used_destroyed && runtime.mismatched_images == 0 && runtime.not_stereo == 0,
          "resized: never a destroyed image, every image matching its views");
    std::size_t old_after_new = 0;
    {
        std::scoped_lock lock(gpu.syntheses_mutex);
        bool resized = false;
        for (const GpuLog::Composed& entry : gpu.composed) {
            resized = resized || entry.composition.output.width == 1400;
            old_after_new += resized && entry.composition.output.width != 1400 ? 1 : 0;
        }
    }
    Check(old_after_new == 0, "resized: frames of the old size are not written again (" +
                                  std::to_string(old_after_new) + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "resized: calls stayed ordered");
}

// Frames the layer composes one after another (the claim finds every frame
// busy for a while) at 3x, interpolating: each is released into the layer's
// compositor outputs as it ends, a lead of refreshes before its own, so the
// refreshes after a frame's own, which still choose it (the nearer of two
// frames shown as they are), would show the next one's image there with its
// views. Such a refresh shows the next frame instead, early, with its own
// views: no image is ever shown with another frame's views, and nothing
// steps back.
void TestQuadAsIsShowsNewestImage() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k3);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(400ms);
    Check(presenter.Status().quad, "as is newest: composing quad views first");
    const std::size_t first = runtime.Submissions();
    app.force = FrameGenQuadRoute::kBusy;
    app.Run(700ms);
    app.force.reset();
    app.Run(200ms);
    const FrameGenStatus status = presenter.Status();
    const auto log = runtime.Snapshot();
    std::size_t as_is = 0;
    for (std::size_t i = first; i < log.size(); ++i) as_is += ShowsComposedOutputs(log[i]) ? 1 : 0;
    Check(as_is > 30, "as is newest: the frames the layer composed are shown (" + std::to_string(as_is) + ")");
    Check(runtime.mismatched_images == 0, "as is newest: no frame the layer composed is shown with another's views (" +
                                              std::to_string(runtime.mismatched_images.load()) + " mismatched)");
    Check(StepsBack(runtime, first) == 0, "as is newest: nothing older is shown after anything newer");
    Check(runtime.discarded <= static_cast<int>(status.stale_skipped) && runtime.not_stereo == 0,
          "as is newest: refreshes left to the runtime are the ones counted, two views each");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "as is newest: calls stayed ordered");
}

// A refresh nothing can be written for shows again what was submitted last:
// after a claimed frame written, then a frame the layer composed shown as it
// is, a claimed frame whose write fails shows the composed frame again (its
// images still the newest), never the older written one.
void TestQuadFailedWriteRepeatsLastShown() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(400ms);
    const std::size_t first = runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; std::chrono::steady_clock::now() - start < 1200ms; ++i) {
        // Every fifth frame the layer composes; the frame after it, and its
        // refreshes, cannot be written (two refreshes, never ten in a row).
        app.force = i % 5 == 3 ? std::optional<FrameGenQuadRoute>(FrameGenQuadRoute::kBusy) : std::nullopt;
        gpu.compose_fail = i % 5 == 4 || i % 5 == 0;
        app.Frame(4000us);
    }
    gpu.compose_fail = false;
    app.force.reset();
    app.Run(100ms);
    const FrameGenStatus status = presenter.Status();
    Check(gpu.compose_failures > 10, "failed write: writes failed (" + std::to_string(gpu.compose_failures.load()) + ")");
    Check(status.quad_write_declines == 0 && status.quad, "failed write: composing goes on");
    Check(StepsBack(runtime, first) == 0, "failed write: a failed write never shows an older frame again");
    Check(runtime.mismatched_images == 0 && runtime.not_stereo == 0, "failed write: images matching their views");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "failed write: calls stayed ordered");
}

// Output images replaced (a canvas size change) are kept while the last
// frame written shows them, but no longer once the presenter thread stops:
// writing into the new ones failing, Frame Generation stands by, and the
// old images (headset resolution) are destroyed, never submitted again.
void TestQuadRetiredOutputsFreed() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
    presenter.SetQuadDecline(3000ms, 3000ms);
    QuadApp app(presenter, runtime);
    for (int i = 0; i < 3; ++i) app.Frame();
    app.Run(400ms);
    const std::vector<XrSwapchain> old_outputs = runtime.Snapshot().back().swapchains;
    Check(ShowsFrameGenOutputs(runtime.Snapshot().back()), "retired: composing first");
    {
        // The new images are never handed back.
        std::scoped_lock lock(g_xr_mutex);
        g_wait_timeouts_on_create = 1000000;
    }
    app.canvas = {1400, 1300};
    app.Run(2000ms, 4000us, [&] {
        const FrameGenStatus status = presenter.Status();
        return status.quad_declining && !status.threaded;
    });
    const FrameGenStatus status = presenter.Status();
    std::size_t destroyed = 0;
    {
        std::scoped_lock lock(g_xr_mutex);
        g_wait_timeouts = 0;
        for (const XrSwapchain output : old_outputs) {
            destroyed += static_cast<std::size_t>(std::count(g_destroyed.begin(), g_destroyed.end(), output));
        }
    }
    Check(status.quad_declining && !status.threaded, "retired: writes failing, Frame Generation stands by");
    Check(destroyed == 2, "retired: standing by, the replaced output images are destroyed (" + std::to_string(destroyed) +
                              " of 2)");
    app.Run(200ms);
    Check(!runtime.used_destroyed && runtime.not_stereo == 0, "retired: a destroyed image is never submitted");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "retired: calls stayed ordered");
}

// The headset taken off (the runtime's waits say shouldRender false) while
// quad views are composed under Dynamic: nothing is written while the
// runtime does not render (writes that would fail say nothing about
// writing), its refreshes say nothing about what generation adds, and
// claims resume at once afterwards. Once with a game that renders anyway,
// once with one ending its frames without layers.
void TestQuadHeadsetOffBreak() {
    for (const bool layers : {true, false}) {
        const std::string name = layers ? "headset off (rendering)" : "headset off (no layers)";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic);
        presenter.SetDynamicPolicy(GuardDynamic());
        QuadApp app(presenter, runtime);
        const auto works = QuadTicks(runtime, {1, 2, 2});
        auto composing = [&] {
            const FrameGenStatus status = presenter.Status();
            return status.threaded && status.quad && !status.standby;
        };
        app.RunWorks(3000ms, works, composing);
        Check(composing(), name + ": composing under Dynamic");
        app.RunWorks(400ms, works);
        const int composes = gpu.compose_calls;
        runtime.should_render = false;
        gpu.compose_fail = true; // writing would fail now
        app.no_layers = !layers;
        app.RunWorks(1200ms, works);
        const int during = gpu.compose_calls - composes;
        runtime.should_render = true;
        gpu.compose_fail = false;
        app.no_layers = false;
        const std::size_t resumed = app.ended.size();
        app.RunWorks(400ms, works);
        const FrameGenStatus status = presenter.Status();
        Check(during <= 2, name + ": nothing is written while the runtime does not render (" + std::to_string(during) +
                               " writes)");
        Check(status.quad_write_declines == 0, name + ": the break hands nothing back to the layer");
        Check(status.dynamic_stand_downs == 0 && composing(),
              name + ": the break stands nothing down (" + status.dynamic_reason + ")");
        Check(app.ended.size() > resumed + 5 && app.Routed(FrameGenQuadRoute::kClaimed, resumed + 2) ==
                                                    app.ended.size() - resumed - 2,
              name + ": claims resume within three frames");
        Check(runtime.not_stereo == 0 && runtime.mismatched_images == 0, name + ": two views, images matching them");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Quad views under Dynamic through its cycle: engaged, probed (the game,
// quantized to 0.8x standing by, runs about the refresh rate engaged),
// engaged again, then stood down by a fault (frames reaching the GPU late).
// Each engagement claims its frames from the first (nothing set up again,
// hardly a frame busy), and across every switch the runtime gets two views,
// each image with its own views, never an older frame after a newer one.
void TestQuadDynamicCycle() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic);
    FrameGenDynamicPolicy policy = GuardDynamic();
    policy.probe_after = 500ms;
    policy.probe_max = 4s;
    presenter.SetDynamicPolicy(policy);
    QuadApp app(presenter, runtime);
    const auto quantized = JitterWork(runtime.period);
    app.RunWorks(4000ms, quantized, [&] { return presenter.Status().dynamic_probes >= 1; });
    Check(presenter.Status().dynamic_probes == 1 && presenter.Status().dynamic_engagements == 1,
          "quad dynamic: engaged, then probed (" + std::to_string(presenter.Status().dynamic_engagements) +
              " engagements, " + std::to_string(presenter.Status().dynamic_stand_downs) + " stand-downs: " +
              presenter.Status().dynamic_reason + "; " + std::to_string(presenter.Status().app_fps) + " fps, " +
              std::to_string(presenter.Status().distinct_fps) + " distinct)");
    app.RunWorks(3000ms, quantized, [&] {
        const FrameGenStatus status = presenter.Status();
        return status.dynamic_engagements >= 2 && status.threaded && status.quad;
    });
    Check(presenter.Status().dynamic_engagements == 2 && presenter.Status().quad, "quad dynamic: engaged again");
    app.RunWorks(300ms, quantized);
    gpu.capture_lag = 150ms;
    app.RunWorks(4000ms, quantized, [&] { return presenter.Status().dynamic_stand_downs >= 1; });
    gpu.capture_lag = 0us;
    app.RunWorks(300ms, quantized);
    const FrameGenStatus status = presenter.Status();
    Check(status.dynamic_stand_downs == 1 && status.standby &&
              status.dynamic_reason.find("reached the GPU too late") != std::string::npos,
          "quad dynamic: a fault stands it down (" + status.dynamic_reason + ")");
    // Each engagement: the frames from its first claimed one to its last
    // one not composed by the layer for want of presenting.
    std::size_t engagements = 0;
    std::string off;
    for (std::size_t i = 0; i < app.ended.size();) {
        if (app.ended[i].route == FrameGenQuadRoute::kNotPresenting) {
            ++i;
            continue;
        }
        std::size_t end = i;
        std::size_t claimed = 0, uncaptured = 0, busy = 0;
        for (; end < app.ended.size() && app.ended[end].route != FrameGenQuadRoute::kNotPresenting; ++end) {
            claimed += app.ended[end].route == FrameGenQuadRoute::kClaimed ? 1 : 0;
            uncaptured += app.ended[end].route == FrameGenQuadRoute::kUncaptured ? 1 : 0;
            busy += app.ended[end].route == FrameGenQuadRoute::kBusy ? 1 : 0;
        }
        const std::size_t frames = end - i;
        ++engagements;
        if (uncaptured > 1 || busy * 50 > frames + 50 || claimed * 100 < frames * 95) {
            off += " engagement " + std::to_string(engagements) + ": " + std::to_string(claimed) + " claimed, " +
                   std::to_string(uncaptured) + " set up, " + std::to_string(busy) + " busy of " +
                   std::to_string(frames) + ";";
        }
        i = end;
    }
    Check(engagements == 2 && off.empty(), "quad dynamic: each engagement claims its frames (" +
                                               std::to_string(engagements) + " engagements;" + off + ")");
    Check(runtime.not_stereo == 0 && runtime.mismatched_images == 0, "quad dynamic: two views, images matching them");
    Check(StepsBack(runtime, 0) == 0, "quad dynamic: nothing older is shown after anything newer");
    Check(runtime.discarded <= static_cast<int>(status.stale_skipped), "quad dynamic: only counted refreshes left out");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "quad dynamic: calls stayed ordered");
}

// Quad views (composed per eye) with motion measured late, extrapolating:
// as TestJitStaleMotion, through SynthesizeComposedStale.
void TestQuadStaleMotion() {
    auto run = [](bool stale) {
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.stale = stale;
        Runtime runtime;
        runtime.paced = true;
        StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
        gpu.measure_lag = 35ms;
        QuadApp app(presenter, runtime);
        for (int i = 0; i < 3; ++i) app.Frame();
        app.Run(1200ms);
        const FrameGenStatus status = presenter.Status();
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation && runtime.not_stereo == 0 &&
                  runtime.mismatched_images == 0,
              "quad stale motion: calls stayed ordered, two views, images matching them");
        Check(gpu.stale_wrong_motion == 0 && gpu.uncaptured_writes == 0 && gpu.unmeasured_moves == 0,
              "quad stale motion: the frame's previous, measured, from a captured frame");
        return status;
    };
    const FrameGenStatus stale = run(true);
    const FrameGenStatus unchanged = run(false);
    std::cerr << "  quad stale: " << stale.stale_motion << " stale, " << stale.unmeasured_copies
              << " unchanged; without: " << unchanged.unmeasured_copies << " unchanged" << std::endl;
    Check(unchanged.unmeasured_copies > 10 && unchanged.stale_motion == 0,
          "quad stale motion: a backend that cannot shows them unchanged (" +
              std::to_string(unchanged.unmeasured_copies) + ")");
    Check(stale.stale_motion > 10 && stale.unmeasured_copies * 4 < unchanged.unmeasured_copies,
          "quad stale motion: the previous real frame's motion moves them (" + std::to_string(stale.stale_motion) +
              " moved so, " + std::to_string(stale.unmeasured_copies) + " unchanged)");
}

// The backend keeps a real frame's images in a history ring: in a ring of
// three, once two newer frames were captured, the frame can no longer be
// moved (its previous images are gone), once three, not even shown. With a
// lead of four refreshes or more at 2x (frames reaching the GPU late), that
// is so by the refresh after a frame's own: such refreshes show its exact
// copy instead of asking the backend for what it no longer has (writes that
// would fail, and, composing quad views, hand them back to the layer).
// Quad views keep five (FrameGenHistoryRing), and so does a canvas where
// video memory allows (CanvasRingLocked): the same lead moves them. A
// canvas short of video memory keeps three.
void TestJitRingExhaustion() {
    enum class Case { kCanvas, kCanvasShortMemory, kQuad, kQuadRingOfThree };
    for (const Case test : {Case::kCanvas, Case::kCanvasShortMemory, Case::kQuad, Case::kQuadRingOfThree}) {
        const bool quad = test == Case::kQuad || test == Case::kQuadRingOfThree;
        const bool shallow = test == Case::kCanvasShortMemory || test == Case::kQuadRingOfThree;
        const std::string name = test == Case::kCanvas              ? "ring"
                                 : test == Case::kCanvasShortMemory ? "ring (video memory short)"
                                 : test == Case::kQuad              ? "ring (quad views)"
                                                                    : "ring (quad views, a ring of three)";
        FrameGenPresenter presenter;
        GpuLog gpu;
        gpu.capture_lag = 40ms;
        if (test == Case::kQuadRingOfThree) gpu.quad_ring = 3;
        if (test == Case::kCanvasShortMemory) gpu.memory_pressure = true;
        Runtime runtime;
        runtime.paced = true;
        FrameGenStatus status;
        if (quad) {
            StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2);
            QuadApp app(presenter, runtime);
            for (int i = 0; i < 3; ++i) app.Frame();
            app.Run(1200ms);
            status = presenter.Status();
        } else {
            std::unique_ptr<App> owned;
            App* app = nullptr;
            StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned);
            for (int i = 0; i < 3; ++i) app->Frame();
            RunWork(*app, 1200ms, kLightWork);
            status = presenter.Status();
        }
        if (std::getenv("FG_TEST_STATS")) {
            std::cerr << name << ": lead " << status.lead_slots << ", " << status.ring_exhausted << " ring exhausted, "
                      << Composed(gpu, [](const GpuLog::Composed& entry) {
                             return entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction > 0.0f;
                         })
                      << " composed moved\n";
        }
        Check(status.lead_slots >= 4, name + ": the lead grew (" + std::to_string(status.lead_slots) + ")");
        Check(gpu.ring_refusals == 0 && gpu.synthesis_failures == 0,
              name + ": nothing is asked of the backend it no longer has (" + std::to_string(gpu.ring_refusals.load()) +
                  " refused)");
        Check(status.quad_write_declines == 0, name + ": nothing is handed back to the layer");
        if (test == Case::kQuad) {
            const int moved = Composed(gpu, [](const GpuLog::Composed& entry) {
                return entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction > 0.0f;
            });
            Check(status.ring_exhausted <= 2 && moved > 30,
                  name + ": refreshes still move their frame (" + std::to_string(moved) + " moved, " +
                      std::to_string(status.ring_exhausted) + " found its images replaced)");
        } else if (!shallow) {
            Check(status.ring_exhausted <= 2 && status.generated > 30,
                  name + ": refreshes still move their frame (" + std::to_string(status.generated) + " generated, " +
                      std::to_string(status.ring_exhausted) + " found its images replaced)");
        } else {
            Check(status.ring_exhausted > 10, name + ": refreshes found their frame's images replaced (" +
                                                  std::to_string(status.ring_exhausted) + ")");
        }
        Check(runtime.mismatched_images == 0 && runtime.not_stereo == 0, name + ": images matching their views");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Quad views under Dynamic at the game's own rate (a real frame every one or
// two refreshes), its frames reaching the GPU late enough for a lead of four
// refreshes: by the refresh after a real frame's own, two newer ones were
// captured, and a ring of three (a canvas's) no longer holds its previous
// images. Quad views keep five (FrameGenHistoryRing): those refreshes still
// move it. With a ring of three they show its exact copy.
void TestQuadDynamicDeepRing() {
    for (const bool shallow : {false, true}) {
        const std::string name = shallow ? "quad own rate (a ring of three)" : "quad own rate";
        FrameGenPresenter presenter;
        GpuLog gpu;
        if (shallow) gpu.quad_ring = 3;
        Runtime runtime;
        runtime.paced = true;
        StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic);
        presenter.SetDynamicPolicy(GuardDynamic());
        QuadApp app(presenter, runtime);
        const auto works = QuadTicks(runtime, {1, 2, 2});
        auto composing = [&] {
            const FrameGenStatus status = presenter.Status();
            return status.threaded && status.quad && !status.standby;
        };
        app.RunWorks(3000ms, works, composing);
        Check(composing(), name + ": composing under Dynamic");
        // Captured 14 ms after their release: a lead of four refreshes.
        gpu.capture_lag = 14ms;
        app.RunWorks(1000ms, works);
        const FrameGenStatus before = presenter.Status();
        auto moved = [&] {
            return Composed(gpu, [](const GpuLog::Composed& entry) {
                return entry.mode == FrameGenSynthesisMode::kExtrapolate && entry.fraction > 0.0f;
            });
        };
        const int moved_before = moved();
        const std::size_t first = runtime.Submissions();
        app.RunWorks(1500ms, works);
        const FrameGenStatus status = presenter.Status();
        const int refreshes = static_cast<int>(runtime.Submissions() - first);
        const int moved_during = moved() - moved_before;
        const int exhausted = static_cast<int>(status.ring_exhausted - before.ring_exhausted);
        const std::string counts = std::to_string(moved_during) + " moved, " + std::to_string(exhausted) +
                                   " found its images replaced of " + std::to_string(refreshes) + " refreshes; lead " +
                                   std::to_string(before.lead_slots) + " then " + std::to_string(status.lead_slots);
        if (std::getenv("FG_TEST_STATS")) std::cerr << name << ": " << counts << '\n';
        Check(before.lead_slots >= 4 && status.lead_slots >= 4 && composing(),
              name + ": composing at a lead of four refreshes or more (" + counts + ")");
        Check(gpu.ring_refusals == 0, name + ": nothing is asked of the backend it no longer has");
        if (shallow) {
            Check(exhausted > refreshes / 10, name + ": refreshes find their frame's images replaced (" + counts + ")");
        } else {
            Check(exhausted <= refreshes / 50 && moved_during > refreshes / 4,
                  name + ": refreshes between real frames move them (" + counts + ")");
        }
        Check(runtime.not_stereo == 0 && runtime.mismatched_images == 0, name + ": two views, images matching them");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Composing into new output images takes the backend a while the first time
// (it allocates under the GPU lock, which the game's releases also need):
// the game's frame is held once a session, not at every engagement (output
// images are kept while standing by).
void TestQuadComposeStallOnce() {
    FrameGenPresenter presenter;
    GpuLog gpu;
    gpu.compose_first_delay = 60ms;
    Runtime runtime;
    runtime.paced = true;
    StartQuad(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic);
    presenter.SetDynamicPolicy(GuardDynamic());
    QuadApp app(presenter, runtime);
    const auto works = QuadTicks(runtime, {1, 2, 2});
    auto composing = [&] {
        const FrameGenStatus status = presenter.Status();
        return status.threaded && status.quad && !status.standby;
    };
    app.RunWorks(3000ms, works, composing);
    app.RunWorks(500ms, works);
    Check(!presenter.Toggle(), "compose stall: turned off");
    app.RunWorks(300ms, works);
    Check(presenter.Toggle(), "compose stall: turned on");
    app.RunWorks(3000ms, works, composing);
    Check(composing() && presenter.Status().dynamic_engagements == 2, "compose stall: engaged twice");
    app.RunWorks(500ms, works);
    const std::size_t held = static_cast<std::size_t>(
        std::count_if(app.release_end_ms.begin(), app.release_end_ms.end(), [](double ms) { return ms > 45.0; }));
    Check(held <= 1, "compose stall: the game's frame is held at most once (" + std::to_string(held) + " times)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, "compose stall: calls stayed ordered");
}

// --- Runtime motion smoothing --------------------------------------------------------

constexpr const char* kHalvingLogged = "the runtime held Frame Generation's frames to every";
constexpr const char* kHalvingDetail = "Runtime holds frames to half rate: check its smoothing";
constexpr const char* kHalvingReason = "the runtime holds frames to half rate: check its smoothing";
constexpr const char* kHalvingStandDown = "the runtime holds frames to half rate";

// A session log of its own in the temporary folder (not beside this
// executable: on a worktree whose build is the installed layer that is the
// active layer folder), read back once the presenter has stopped.
struct SessionLog {
    std::filesystem::path directory;
    std::unique_ptr<Logger> logger{std::make_unique<Logger>()};
    std::filesystem::path path; // the log, once Lines closed it

    explicit SessionLog(const std::string& name)
        : directory(std::filesystem::temp_directory_path() /
                    ("vectorxr-presenter-test-log-" + std::to_string(GetCurrentProcessId()) + "-" + name)) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        logger->Initialize(directory / "session.log");
    }
    ~SessionLog() {
        logger.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    // The lines holding `text` (closes the log: call once the presenter has
    // stopped). Later calls read the closed log again.
    std::vector<std::string> Lines(const std::string& text) {
        if (logger) {
            path = logger->ActiveLogPath();
            logger.reset(); // flushed
        }
        std::ifstream file(path);
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);) {
            if (line.find(text) != std::string::npos) lines.push_back(line);
        }
        return lines;
    }
};

// Turned off while the game's wait is held for its cadence (fixed 2x holds
// it about half of each frame): the wait returned into the presenter thread,
// was handed one more frame and logged "engaged" right after "disengaged".
// Each press on logs one engagement and nothing else does. Switched from
// Dynamic standing by to fixed 2x, the engagement is logged too (it was not:
// the state read active throughout).
void TestToggleOffLogsNoEngagement() {
    const std::string name = "toggle off logs no engagement";
    SessionLog log("toggle-engaged");
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned,
             log.logger.get());
    std::atomic<bool> stop{false};
    std::thread game([&] {
        while (!stop) {
            const XrFrameState state = app->Wait();
            app->Begin();
            SleepPrecise(std::chrono::steady_clock::now() + 6ms);
            app->Release();
            Check(XR_SUCCEEDED(app->End(state.predictedDisplayTime)), name + ": end");
        }
    });
    const auto threaded = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const FrameGenStatus status = presenter.Status();
            if (status.state == FrameGenState::kActive && status.threaded) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    };
    constexpr int kPresses = 10;
    int ons = 0;
    for (int i = 0; i < kPresses; ++i) {
        Check(threaded(), name + ": engaged before each press");
        ++ons;
        std::this_thread::sleep_for(std::chrono::milliseconds(60 + 3 * i));
        Check(!presenter.Toggle(), name + ": turned off");
        std::this_thread::sleep_for(100ms);
        Check(presenter.Status().state == FrameGenState::kOff, name + ": reads off");
        Check(presenter.Toggle(), name + ": turned on");
    }
    Check(threaded(), name + ": engaged after the last press");
    ++ons;
    // Dynamic standing by for a game at the refresh rate, then fixed 2x.
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
    settings.multiplier = FrameGenMultiplier::kDynamic;
    presenter.Configure(settings);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const auto standing_by = [&] {
        const FrameGenStatus status = presenter.Status();
        return status.standby && !status.threaded && status.state == FrameGenState::kActive;
    };
    while (!standing_by() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(100ms);
    Check(standing_by(), name + ": Dynamic stands by for a game at the refresh rate");
    settings.multiplier = FrameGenMultiplier::k2;
    presenter.Configure(settings);
    Check(threaded(), name + ": fixed 2x engages from standing by");
    ++ons;
    std::this_thread::sleep_for(100ms);
    stop = true;
    game.join();
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    const std::vector<std::string> lines = log.Lines("Frame Generation");
    const auto engaged = static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("Frame Generation engaged") != std::string::npos;
    }));
    if (std::getenv("FG_TEST_VERBOSE")) {
        for (const std::string& line : lines) std::cerr << "  " << line << '\n';
    }
    Check(engaged == static_cast<std::size_t>(ons),
          name + ": one engagement logged for each (" + std::to_string(engaged) + " logged, " + std::to_string(ons) +
              " expected)");
}

// A just-in-time Extrapolate session against a paced runtime, the halving
// window shortened to a second; Dynamic's benefit guard keeps its own windows
// (it would judge only after these tests' decisions).
void StartHalving(FrameGenPresenter& presenter, GpuLog& gpu, Runtime& runtime, FrameGenMultiplier multiplier, App*& app,
                  std::unique_ptr<App>& owned, Logger* logger) {
    runtime.paced = true;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, multiplier, app, runtime, owned, logger);
    presenter.SetHalvingWindow(1000ms);
}

// The trace records the game's swapchain acquires and releases only while
// frames are generated or about to be: standing by they are every image of
// every frame, and tell nothing about Frame Generation.
void TestTraceSwapchainCallsOnlyWhileCapturing() {
    SessionLog log("trace-swapchain-calls");
    log.logger->SetLevel(LogLevel::Debug);
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned,
             log.logger.get());
    presenter.SetDynamicPolicy(SteadyDynamic());
    Check(presenter.Trace().Enabled(), "trace: debug logging starts the session's trace");
    RunTicks(*app, 600ms, {1});
    const FrameGenStatus standing_by = presenter.Status();
    Check(standing_by.standby && !presenter.TracesSwapchainCalls(),
          "trace: standing by, the game's acquires and releases are not traced");
    presenter.SetDynamicPolicy(QuickDynamic());
    EngageDynamic(presenter, *app, "trace");
    Check(presenter.TracesSwapchainCalls(), "trace: engaged, the game's acquires and releases are traced");
    presenter.Stop();
    Check(!presenter.TracesSwapchainCalls(), "trace: a stopped session traces nothing");
}

// Submissions the runtime got per second while the game ran `work` for `duration`.
double SubmissionRate(App& app, std::chrono::milliseconds duration, const std::vector<std::chrono::microseconds>& work) {
    const std::size_t first = app.runtime.Submissions();
    const auto start = std::chrono::steady_clock::now();
    RunWork(app, duration, work);
    return static_cast<double>(app.runtime.Submissions() - first) / Seconds(std::chrono::steady_clock::now() - start);
}

std::vector<std::chrono::microseconds> PeriodsOfWork(XrDuration period, double periods) {
    return {std::chrono::microseconds(static_cast<std::int64_t>(static_cast<double>(period) * periods / 1000.0))};
}

double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

// The median time between submissions to the runtime from submission
// `first` on, in milliseconds: the cadence they reach it at.
double MedianSubmitInterval(Runtime& runtime, std::size_t first) {
    std::vector<double> intervals;
    std::scoped_lock lock(g_xr_mutex);
    for (std::size_t i = first + 1; i < runtime.submitted_at.size(); ++i) {
        intervals.push_back(
            std::chrono::duration<double, std::milli>(runtime.submitted_at[i] - runtime.submitted_at[i - 1]).count());
    }
    return Median(intervals);
}

// A runtime that holds the frames Frame Generation presents to half its
// refresh rate (Runtime::Throttle), for longer than the halving window: its
// own output is halved too (the runtime makes the frames between), so it is
// logged once, the OSD and the app say so, and Dynamic stands down for it
// and stays standing by through its hold, still saying so. Standing by,
// the game's rate is what the runtime lets it run (half the refresh rate):
// it cannot be told from its own, so Dynamic engages first. Each way a
// runtime may halve the rate: its waits two refreshes apart, from the start;
// automatically, for late frames, Frame Generation's too (a runtime judging
// frames by when their GPU work ends may find them late); and twice its
// period reported, after a while at full rate, where the game's rate is
// judged against the refresh rate, not the halved period. A runtime cutting
// the rate to a third either way is reported the same, the log naming what
// it measured. A fixed multiplier is reported the same way and has nothing
// to stand down to.
void TestRuntimeHalvingStandsDown() {
    struct Case {
        const char* name;
        Runtime::Throttle throttle{Runtime::Throttle::kWaits};
        int refreshes{2}; // Runtime::throttle_refreshes
        bool from_start{false};
        bool automatic{false};
        bool after_full_rate{false};
        FrameGenMultiplier multiplier{FrameGenMultiplier::kDynamic};
        const char* evidence;
    };
    const std::vector<Case> cases{
        {"halving waits", Runtime::Throttle::kWaits, 2, true, false, false, FrameGenMultiplier::kDynamic,
         "to every other refresh at 90 Hz for 1 s (its frame waits blocked)."},
        {"halving late frames", Runtime::Throttle::kWaits, 2, false, true, false, FrameGenMultiplier::kDynamic,
         "to every other refresh at 90 Hz for 1 s (its frame waits blocked)."},
        {"halving the period", Runtime::Throttle::kPeriod, 2, false, false, true, FrameGenMultiplier::kDynamic,
         "to every other refresh at 90 Hz for 1 s (its frame waits blocked; it reported a frame period of 22.2 ms, "
         "twice its refresh period)."},
        {"halving fixed 2x", Runtime::Throttle::kWaits, 2, true, false, false, FrameGenMultiplier::k2,
         "to every other refresh at 90 Hz for 1 s (its frame waits blocked)."},
        {"a third, waits", Runtime::Throttle::kWaits, 3, true, false, false, FrameGenMultiplier::kDynamic,
         "to every third refresh at 90 Hz for 1 s (its frame waits blocked)."},
        {"a third, the period", Runtime::Throttle::kPeriod, 3, false, false, true, FrameGenMultiplier::kDynamic,
         "to every third refresh at 90 Hz for 1 s (its frame waits blocked; it reported a frame period of 33.3 ms, "
         "three times its refresh period)."},
    };
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    for (const Case& test : cases) {
        const std::string name = test.name;
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.throttle = test.throttle;
        runtime.throttle_refreshes = test.refreshes;
        runtime.throttled = test.from_start;
        // Late at full rate (a frame of 1.35 refreshes), and every frame late
        // for a runtime wanting them two refreshes before they are shown.
        std::vector<std::chrono::microseconds> work = kLightWork;
        if (test.automatic) {
            runtime.throttle_on_after = 5;
            runtime.throttle_off_after = 20;
            runtime.deadline_lead = 2 * kPeriod;
            work = PeriodsOfWork(kPeriod, 1.35);
        }
        SessionLog log(name);
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, test.multiplier, app, owned, log.logger.get());
        if (test.after_full_rate) {
            RunWork(*app, 600ms, work);
            Check(presenter.Status().standby, name + ": a game holding the refresh rate stands by");
            runtime.throttled = true;
        }
        const bool dynamic = test.multiplier == FrameGenMultiplier::kDynamic;
        const std::string share = test.refreshes == 2 ? "half" : "a third of";
        if (dynamic) {
            RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return Engaged(presenter); });
            Check(Engaged(presenter), name + ": the game held to " + share + " the refresh rate engages Dynamic");
        } else {
            for (int i = 0; i < 3; ++i) app->Frame();
        }
        // Frame Generation's own output is halved (or cut to a third) too:
        // the runtime makes the frames between.
        const double output = SubmissionRate(*app, 500ms, work);
        const double held = refresh / test.refreshes;
        Check(output < held + 0.1 * refresh && output > held - 0.1 * refresh,
              name + ": the runtime takes Frame Generation's frames at " + share + " the refresh rate (" +
                  std::to_string(output) + " fps)");
        RunWork(*app, 3000ms, work, [&](const XrFrameState&) {
            const FrameGenStatus status = presenter.Status();
            return dynamic ? status.standby : status.runtime_halving;
        });
        FrameGenStatus status = presenter.Status();
        if (dynamic) {
            Check(status.standby && status.runtime_halving && status.dynamic_stand_downs == 1 &&
                      status.dynamic_reason == kHalvingStandDown,
                  name + ": reported, Dynamic stands down (" + status.dynamic_reason + ")");
        } else {
            Check(status.runtime_halving && status.state == FrameGenState::kActive && status.threaded,
                  name + ": reported; a fixed multiplier stays engaged");
        }
        const FrameGenOsdText text = presenter.OsdDescription();
        Check(text.detail == kHalvingDetail && text.reason.size() > std::string(kHalvingReason).size() &&
                  text.reason.substr(text.reason.size() - std::string(kHalvingReason).size()) == kHalvingReason,
              name + ": the OSD and the app say so (" + text.detail + " | " + text.reason + ")");
        // Through the stand-down's hold (a fixed multiplier: engaged), still
        // reported.
        bool cleared = false;
        RunWork(*app, 1000ms, work, [&](const XrFrameState&) {
            cleared = cleared || !presenter.Status().runtime_halving;
            return false;
        });
        status = presenter.Status();
        Check(!cleared && (dynamic ? status.standby && status.dynamic_engagements == 1 : status.threaded),
              name + ": the report stays while the runtime halves the frame rate, and nothing engages again");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
        const std::vector<std::string> lines = log.Lines(kHalvingLogged);
        Check(lines.size() == 1 && lines.front().find(test.evidence) != std::string::npos &&
                  lines.front().find("Settings that can do this: SteamVR's Motion Smoothing (Settings > Video), and "
                                     "the game's Motion Smoothing and Throttling Behavior in SteamVR's "
                                     "Per-Application Video Settings; Pimax Play's Smart Smoothing and Lock Frame "
                                     "Rate to Half Refresh Rate (Device settings > Frame rate); Virtual Desktop's "
                                     "SSW; Meta's ASW.") != std::string::npos &&
                  lines.front().find("keep the runtime at full refresh") != std::string::npos &&
                  lines.front().find("probably") == std::string::npos,
              name + ": logged once (" + std::to_string(lines.size()) + " lines" +
                  (lines.empty() ? std::string() : ": " + lines.front()) + ")");
    }
}

// A runtime that halves the frame rate for late frames (Runtime::
// throttle_on_after), with a game late at full rate (a frame of 1.35
// refreshes): standing by, the game is held to half the refresh rate, and
// Dynamic engages, judging it against the refresh rate even where the
// runtime reports the halved period. Frame Generation presents on time, so
// the runtime soon gives every refresh again: nothing is reported, nothing
// stands down, and every refresh gets a frame.
void TestRuntimeHalvingRecovers() {
    for (const Runtime::Throttle throttle : {Runtime::Throttle::kWaits, Runtime::Throttle::kPeriod}) {
        const std::string name =
            std::string("halving recovers, ") + (throttle == Runtime::Throttle::kWaits ? "waits" : "period");
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.throttle = throttle;
        runtime.throttle_on_after = 5;
        runtime.throttle_off_after = 20;
        SessionLog log(throttle == Runtime::Throttle::kWaits ? "recovers-waits" : "recovers-period");
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, log.logger.get());
        const auto work = PeriodsOfWork(kPeriod, 1.35);
        RunWork(*app, 4000ms, work, [&](const XrFrameState&) { return Engaged(presenter); });
        Check(runtime.ThrottleSwitches() >= 1, name + ": standing by, the runtime halved the late game's frame rate");
        Check(Engaged(presenter), name + ": Dynamic engages for the game held to half the refresh rate (" +
                                      presenter.OsdDetailText() + ")");
        RunWork(*app, 1500ms, work);
        const double output = SubmissionRate(*app, 800ms, work);
        const FrameGenStatus status = presenter.Status();
        const double refresh = 1e9 / static_cast<double>(kPeriod);
        Check(!runtime.throttled && Engaged(presenter) && status.dynamic_stand_downs == 0 && !status.runtime_halving,
              name + ": presented on time, the runtime gives every refresh again; nothing reported or stood down (" +
                  status.dynamic_reason + ")");
        Check(output > 0.85 * refresh, name + ": every refresh gets a frame (" + std::to_string(output) + " fps)");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
        Check(log.Lines(kHalvingLogged).empty(), name + ": nothing logged");
    }
}

// Nothing is reported against a runtime giving every refresh: engaged at 72,
// 90 and 120 Hz with a game at its own rate below the refresh rate, or at a
// third of it; with a third of the presenter's waits missing a refresh; with
// the presenter thread asking for every wait late, so that it misses every
// other refresh itself; or standing by with a game at a third of the refresh
// rate, whose waits come late (it is slow, the runtime does not hold it), and
// then with one that waits for its next frame while it renders this one, as
// MSFS does, against a runtime whose wait waits for that frame to end: those
// waits are asked for at once and released two refreshes apart, as a
// runtime halving the frame rate releases them, and only the presenter
// thread's waits report that.
void TestRuntimeHalvingNotReported() {
    auto quiet = [](FrameGenPresenter& presenter, Runtime& runtime, SessionLog& log, const std::string& name) {
        const FrameGenStatus status = presenter.Status();
        Check(!status.runtime_halving && status.dynamic_stand_downs == 0 && presenter.OsdDetailText() != kHalvingDetail,
              name + ": nothing reported (" + presenter.OsdDetailText() + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
        Check(log.Lines(kHalvingLogged).empty(), name + ": nothing logged");
    };
    for (const int hz : {72, 90, 120}) {
        const std::string name = "not halving " + std::to_string(hz) + " Hz";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.period = 1'000'000'000 / hz;
        SessionLog log("quiet-" + std::to_string(hz));
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, log.logger.get());
        EngageDynamic(presenter, *app, name);
        RunTicks(*app, 1300ms, {1, 2, 2});
        RunWork(*app, 1300ms, PeriodsOfWork(runtime.period, 3.0));
        Check(Engaged(presenter), name + ": engaged throughout");
        quiet(presenter, runtime, log, name);
    }
    {
        const std::string name = "not halving, missed refreshes";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.miss_every = 3;
        SessionLog log("quiet-missed");
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, log.logger.get());
        EngageDynamic(presenter, *app, name);
        RunTicks(*app, 2000ms, {1, 2, 2});
        Check(Engaged(presenter), name + ": engaged throughout");
        quiet(presenter, runtime, log, name);
    }
    {
        const std::string name = "not halving, presenter late";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        SessionLog log("quiet-late");
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::k2, app, owned, log.logger.get());
        for (int i = 0; i < 3; ++i) app->Frame();
        // Each submission takes 1.2 refreshes: the presenter thread asks for
        // its next wait after the next refresh, and the runtime releases it
        // at the one after, two refreshes after the last.
        runtime.submit_sleep_us = static_cast<int>(kPeriod * 6 / 5 / 1000);
        RunWork(*app, 300ms, kLightWork);
        const double output = SubmissionRate(*app, 2000ms, kLightWork);
        const double refresh = 1e9 / static_cast<double>(kPeriod);
        const FrameGenStatus status = presenter.Status();
        Check(output < 0.6 * refresh && status.state == FrameGenState::kActive && status.threaded,
              name + ": engaged, the late presenter thread made only every other refresh (" + std::to_string(output) +
                  " fps)");
        quiet(presenter, runtime, log, name);
    }
    {
        const std::string name = "not halving, slow game standing by";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        SessionLog log("quiet-standby");
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, log.logger.get());
        FrameGenDynamicPolicy policy = QuickDynamic();
        policy.engage_after = 60s; // stands by throughout
        presenter.SetDynamicPolicy(policy);
        RunTicks(*app, 2000ms, {3});
        Check(presenter.Status().standby, name + ": standing by throughout");
        // MSFS-style: the main thread waits for the next frame 2 ms after
        // each wait's release, while the render thread renders this one for
        // 14 ms; the runtime releases that wait a refresh after the frame
        // ends, two refreshes after the last.
        runtime.wait_for_end = true;
        const auto start = std::chrono::steady_clock::now();
        const int frames = RunPipelined(*app, 2000ms, 2ms, 14ms);
        const double rate = frames / Seconds(std::chrono::steady_clock::now() - start);
        const double refresh = 1e9 / static_cast<double>(kPeriod);
        Check(rate < 0.6 * refresh && rate > 0.4 * refresh && presenter.Status().standby,
              name + ": standing by, the pipelined game's waits are released two refreshes apart (" +
                  std::to_string(rate) + " fps)");
        quiet(presenter, runtime, log, name);
    }
}

// Reported and stood down for, standing by: the report stays while the
// game's own waits are halved too, never flapping; once the runtime gives
// every refresh again (its smoothing turned off), the game's waits clear it
// within the window, and a game holding the refresh rate is not engaged
// again. A slower game (frames of 9.5 ms and two refreshes, 60 fps at
// 90 Hz) asks for every wait late, more than three quarters of a refresh
// after the last one's release: its waits held two refreshes apart say
// nothing, but those released at the next refresh clear it too.
void TestRuntimeHalvingClears() {
    for (const bool slow : {false, true}) {
        const std::string name = slow ? "halving clears, slow game" : "halving clears";
        const std::vector<std::chrono::microseconds> work =
            slow ? std::vector<std::chrono::microseconds>{9500us, std::chrono::microseconds(2 * kPeriod / 1000) - 3ms}
                 : kLightWork;
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.throttled = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
        RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return Engaged(presenter); });
        RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return presenter.Status().standby; });
        Check(presenter.Status().standby && presenter.Status().runtime_halving, name + ": reported and stood down");
        bool cleared = false;
        RunWork(*app, 1500ms, work, [&](const XrFrameState&) {
            cleared = cleared || !presenter.Status().runtime_halving;
            return false;
        });
        Check(!cleared, name + ": standing by, the report stays while the runtime halves the frame rate");
        runtime.throttled = false;
        const auto off = std::chrono::steady_clock::now();
        RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return !presenter.Status().runtime_halving; });
        const double took = Seconds(std::chrono::steady_clock::now() - off);
        const FrameGenStatus status = presenter.Status();
        Check(!status.runtime_halving && took < 2.0,
              name + ": the game's waits at the refresh rate clear it (" + std::to_string(took) + " s)");
        if (slow) {
            Check(status.standby && status.dynamic_engagements == 1 && presenter.OsdDetailText() != kHalvingDetail,
                  name + ": standing by through the hold, no longer saying so (" + presenter.OsdDetailText() + ")");
        } else {
            Check(status.standby && status.dynamic_engagements == 1 &&
                      presenter.OsdDetailText().find("The game runs at") == 0,
                  name + ": a game holding the refresh rate stays standing by (" + presenter.OsdDetailText() + ")");
        }
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Reported and stood down for, standing by with a game slower than the rate
// the runtime holds it to: each of its waits is asked for late. Released
// just before the held refresh (frames a millisecond short of two
// refreshes), or at once by a runtime that lets a caller later than its held
// refresh go at once (frames of two and a half refreshes,
// Runtime::throttle_late_at_once), such a wait gets a display time on the
// refresh grid: unlike the waits SteamVR with Pimax's headset driver
// releases at once (TestEchoRuntimeClearsHalving), it says nothing about
// halving, and the report stays. Each way a runtime may halve the rate: its
// waits, and its period reported.
void TestRuntimeHalvingStaysForSlowGame() {
    struct Case {
        const char* name;
        Runtime::Throttle throttle{Runtime::Throttle::kWaits};
        bool late_at_once{false};
    };
    const std::vector<Case> cases{
        {"halving stays, waits just in time", Runtime::Throttle::kWaits, false},
        {"halving stays, period just in time", Runtime::Throttle::kPeriod, false},
        {"halving stays, waits at once", Runtime::Throttle::kWaits, true},
        {"halving stays, period at once", Runtime::Throttle::kPeriod, true},
    };
    for (const Case& test : cases) {
        const std::string name = test.name;
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.throttle = test.throttle;
        runtime.throttle_late_at_once = test.late_at_once;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
        FrameGenDynamicPolicy policy = QuickDynamic();
        policy.hold = policy.hold_max = 60s; // stood down, it stands by throughout
        presenter.SetDynamicPolicy(policy);
        RunWork(*app, 600ms, kLightWork);
        Check(presenter.Status().standby, name + ": a game holding the refresh rate stands by");
        runtime.throttled = true;
        RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) { return Engaged(presenter); });
        RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) { return presenter.Status().standby; });
        FrameGenStatus status = presenter.Status();
        Check(status.standby && status.runtime_halving && status.dynamic_stand_downs == 1,
              name + ": reported and stood down (" + status.dynamic_reason + ")");
        const std::vector<std::chrono::microseconds> work =
            test.late_at_once ? PeriodsOfWork(kPeriod, 2.5)
                              : std::vector<std::chrono::microseconds>{std::chrono::microseconds(2 * kPeriod / 1000) - 1ms};
        const int quick = runtime.quick_releases;
        bool cleared = false;
        const int frames = RunWork(*app, 2000ms, work, [&](const XrFrameState&) {
            cleared = cleared || !presenter.Status().runtime_halving;
            return false;
        });
        const int released = runtime.quick_releases - quick;
        if (std::getenv("FG_TEST_STATS")) std::cerr << name << ": " << released << " of " << frames << " at once\n";
        Check(frames > 0 && released * 4 >= frames,
              name + ": the slow game's waits came late and were released at once (" + std::to_string(released) +
                  " of " + std::to_string(frames) + ")");
        status = presenter.Status();
        Check(!cleared && status.standby && status.dynamic_stand_downs == 1 &&
                  presenter.OsdDetailText() == kHalvingDetail,
              name + ": the report stays while the runtime halves the frame rate (" + presenter.OsdDetailText() + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Reported and cleared under a fixed multiplier (nothing to stand down to),
// then Dynamic again, engaged as before: a report gone says nothing about
// this engagement, and nothing stands down for it.
void TestRuntimeHalvingClearedDoesNotStandDown() {
    const std::string name = "halving cleared";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
    EngageDynamic(presenter, *app, name);
    auto configure = [&](FrameGenMultiplier multiplier) {
        FrameGenResolvedSettings settings;
        settings.enabled = true;
        settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
        settings.multiplier = multiplier;
        presenter.Configure(settings);
    };
    configure(FrameGenMultiplier::k2);
    runtime.throttled = true;
    RunTicks(*app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return presenter.Status().runtime_halving; });
    Check(presenter.Status().runtime_halving, name + ": reported at a fixed multiplier");
    runtime.throttled = false;
    RunTicks(*app, 3000ms, {1, 2, 2}, [&](const XrFrameState&) { return !presenter.Status().runtime_halving; });
    Check(!presenter.Status().runtime_halving, name + ": cleared once the runtime gives every refresh");
    configure(FrameGenMultiplier::kDynamic);
    RunTicks(*app, 1500ms, {1, 2, 2});
    const FrameGenStatus status = presenter.Status();
    Check(Engaged(presenter) && status.dynamic_stand_downs == 0 && !status.runtime_halving,
          name + ": Dynamic stays engaged (" + status.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// Switched from Dynamic standing by to 2x and back while running: only
// Dynamic stands by, so no summary window's standing-by time goes negative
// and the stretch at 2x is not counted; back in Dynamic the game's rate is
// judged afresh after the settle, not from the frames held at 2x.
void TestDynamicSwitchStandby() {
    const std::string name = "dynamic switch";
    SessionLog log("dynamic-switch");
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    const auto start = std::chrono::steady_clock::now();
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned,
             log.logger.get());
    const FrameGenDynamicPolicy policy = SteadyDynamic();
    presenter.SetDynamicPolicy(policy);
    presenter.SetSummaryInterval(300ms);
    auto configure = [&](FrameGenMultiplier multiplier) {
        FrameGenResolvedSettings settings;
        settings.enabled = true;
        settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
        settings.multiplier = multiplier;
        presenter.Configure(settings);
        return std::chrono::steady_clock::now();
    };
    // A game at the refresh rate stands by; then 2x for one at 0.6x of it
    // (held to half), then Dynamic again.
    RunWork(*app, 1000ms, kLightWork);
    const bool stood_by = presenter.Status().standby;
    const auto fixed = configure(FrameGenMultiplier::k2);
    RunTicks(*app, 2500ms, {1, 2, 2});
    const auto switched = configure(FrameGenMultiplier::kDynamic);
    EngageDynamic(presenter, *app, name);
    const auto engaged = std::chrono::steady_clock::now();
    RunTicks(*app, 600ms, {1, 2, 2});
    presenter.Stop();
    Check(stood_by, name + ": standing by for a game at the refresh rate");
    const std::vector<std::string> lines = log.Lines("Frame Generation");
    double standby = 0.0;
    std::string windows;
    std::ptrdiff_t at_2x = -1, standing_by = -1, engages = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const auto index = static_cast<std::ptrdiff_t>(i);
        if (const std::size_t at = line.find("standbyS="); at != std::string::npos) {
            const double window = std::stod(line.substr(at + 9));
            Check(window >= 0.0, name + ": no window's standing-by time is negative (" + line + ")");
            standby += window;
            windows += " " + std::to_string(window);
        }
        if (at_2x < 0 && line.find("multiplier=2x") != std::string::npos) at_2x = index;
        // The last standing-by line before Dynamic engages: the one after the switch.
        if (engages < 0 && line.find("(dynamic) standing by") != std::string::npos) standing_by = index;
        if (engages < 0 && line.find("(dynamic) engages") != std::string::npos) engages = index;
    }
    Check(at_2x >= 0 && standing_by > at_2x && engages > standing_by,
          name + ": back in Dynamic, it stands by before it engages");
    Check(standing_by >= 0 && lines[standing_by].find("frames pass straight through") != std::string::npos,
          name + ": standing by after the switch does not report the rate held at 2x (" +
              (standing_by >= 0 ? lines[standing_by] : std::string()) + ")");
    const double since_switch = Seconds(engaged - switched);
    Check(since_switch >= Seconds(policy.settle + policy.engage_after) - 0.05,
          name + ": it engages only after the settle and a run below the refresh rate (" +
              std::to_string(since_switch) + " s)");
    const double counted = Seconds(fixed - start) + since_switch;
    Check(standby > 1.0 && standby <= counted + 0.4,
          name + ": the stretch at 2x is not standing by (" + std::to_string(standby) + " of " +
              std::to_string(counted) + " s:" + windows + ")");
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// A runtime that keeps halving the frame rate: Dynamic stands down after
// the window, and each time its hold ends it engages again for the game held
// to half the refresh rate. With the report standing, the presenter thread's
// waits show it again within a quarter of the window: each try is brief.
void TestRuntimeHalvingRetriesBriefly() {
    const std::string name = "halving retries";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.throttled = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.hold = policy.hold_max = 600ms;
    presenter.SetDynamicPolicy(policy);
    // Each engagement: when it began and ended (stood down), in seconds from
    // the start.
    std::vector<std::pair<double, double>> engagements;
    std::uint64_t engaged = 0;
    std::uint64_t stood_down = 0;
    bool reported = false;
    bool cleared = false;
    const auto start = std::chrono::steady_clock::now();
    RunWork(*app, 9000ms, kLightWork, [&](const XrFrameState&) {
        const FrameGenStatus status = presenter.Status();
        const double at = Seconds(std::chrono::steady_clock::now() - start);
        if (status.dynamic_engagements > engaged) {
            engaged = status.dynamic_engagements;
            engagements.push_back({at, -1.0});
        }
        if (status.dynamic_stand_downs > stood_down) {
            stood_down = status.dynamic_stand_downs;
            if (!engagements.empty()) engagements.back().second = at;
        }
        cleared = cleared || (reported && !status.runtime_halving);
        reported = reported || status.runtime_halving;
        return engagements.size() >= 4 && engagements.back().second >= 0.0;
    });
    const FrameGenStatus status = presenter.Status();
    std::string seen;
    for (const auto& [from, to] : engagements) {
        seen += " " + std::to_string(from) + "-" + std::to_string(to);
    }
    Check(engagements.size() >= 4 && engagements.back().second >= 0.0 &&
              status.dynamic_stand_downs == engagements.size() &&
              status.dynamic_reason == kHalvingStandDown && reported && !cleared,
          name + ": stood down for halving after each engagement, reported throughout (" + status.dynamic_reason +
              ";" + seen + ")");
    if (engagements.size() >= 4) {
        Check(engagements[0].second - engagements[0].first >= 0.8,
              name + ": the first engagement is judged over the whole window (" + seen + ")");
        for (std::size_t i = 1; i < engagements.size(); ++i) {
            Check(engagements[i].second >= 0.0 && engagements[i].second - engagements[i].first < 0.7,
                  name + ": engaging again under the report is brief (" + seen + ")");
        }
    }
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// Auto against a runtime holding the frames presented to half its refresh
// rate (Pimax Play's Smart Smoothing in issue #95): engaged, every frame
// takes twice its refreshes and reads as late, and Auto stepped up to 4x,
// holding a game the runtime let run at half the refresh rate to an eighth
// of it. Once the presenter thread's waits report the halving, Auto goes
// back to full rate (frames pass straight through, as Dynamic stands down),
// logs why once, and stays there through its hold while the runtime still
// halves; once the runtime gives every refresh again, a game at 60 fps,
// whose shorter frames' waits show that, engages Auto before the hold would
// have ended.
void TestAutoStandsDownForRuntimeHalving() {
    const std::string name = "auto stands down for halving";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.throttled = true;
    SessionLog log("auto-halving");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kAuto, app, owned, log.logger.get());
    presenter.SetAutoPolicy(200ms, 500ms, 600ms, 300ms);
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.hold = 6000ms;
    policy.hold_max = 12000ms;
    presenter.SetDynamicPolicy(policy);
    // Held to every other refresh, the game's frames at full rate read as
    // late: Auto engages.
    RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) {
        return presenter.Status().multiplier >= 2 && presenter.Status().threaded;
    });
    Check(presenter.Status().multiplier >= 2 && presenter.Status().threaded,
          name + ": the game held to half the refresh rate engages Auto (" +
              std::to_string(presenter.Status().multiplier) + "x)");
    std::uint32_t highest = 0;
    RunWork(*app, 4000ms, kLightWork, [&](const XrFrameState&) {
        const FrameGenStatus status = presenter.Status();
        highest = std::max(highest, status.multiplier);
        return status.runtime_halving && status.full_rate;
    });
    const auto stood_down = std::chrono::steady_clock::now();
    FrameGenStatus status = presenter.Status();
    Check(status.runtime_halving && status.full_rate && status.multiplier == 1 && !status.threaded,
          name + ": reported, Auto goes back to full rate (" + std::to_string(status.multiplier) + "x, highest " +
              std::to_string(highest) + "x)");
    const FrameGenOsdText text = presenter.OsdDescription();
    Check(text.detail == kHalvingDetail, name + ": the OSD and the app say why (" + text.detail + ")");
    // Through the hold, still halved: nothing engages again.
    bool engaged_again = false;
    RunWork(*app, 1500ms, kLightWork, [&](const XrFrameState&) {
        engaged_again = engaged_again || presenter.Status().multiplier > 1;
        return false;
    });
    Check(!engaged_again && presenter.Status().runtime_halving,
          name + ": the runtime still halving, Auto stays at full rate through its hold");
    // Every refresh again: the game's own waits clear the report (a game at
    // 60 fps, as in TestRuntimeHalvingClears: its waits released at the
    // next refresh do), and it engages Auto without waiting out the hold.
    runtime.throttled = false;
    const std::vector<std::chrono::microseconds> slow{9500us,
                                                      std::chrono::microseconds(2 * kPeriod / 1000) - 3ms};
    RunWork(*app, 4000ms, slow, [&](const XrFrameState&) {
        return presenter.Status().multiplier >= 2 && presenter.Status().threaded;
    });
    const double after = Seconds(std::chrono::steady_clock::now() - stood_down);
    status = presenter.Status();
    Check(!status.runtime_halving && status.multiplier >= 2 && status.threaded && after < 5.5,
          name + ": the halving cleared, a slow game engages Auto again before the hold ends (" +
              std::to_string(status.multiplier) + "x after " + std::to_string(after) + " s)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    const std::vector<std::string> lines = log.Lines("-> full rate: the runtime holds frames to half rate");
    Check(lines.size() == 1 &&
              lines.front().find("frames pass straight through, retrying in 6 s.") != std::string::npos &&
              lines.front().find("sooner") == std::string::npos,
          name + ": logged once (" + std::to_string(lines.size()) + " lines" +
              (lines.empty() ? std::string() : ": " + lines.front()) + ")");
}

// Auto stood down for runtime halving, then the runtime gives every refresh
// again (its smoothing turned off) with a game slower than the refresh rate
// (frames of 1.35 refreshes), as in issue #95: its waits, asked for late and
// released two refreshes apart either way, cannot show it, so the report
// and Auto's hold stay until the hold runs out. Auto then engages, the
// presenter thread's waits clear the report, and it stays engaged.
void TestAutoHalvingHoldRunsOut() {
    const std::string name = "auto halving hold runs out";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.throttled = true;
    SessionLog log("auto-halving-hold");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kAuto, app, owned, log.logger.get());
    // No probing down: engaged again, it stays engaged unless it stands down.
    presenter.SetAutoPolicy(200ms, 30s, 600ms, 300ms);
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.hold = policy.hold_max = 3000ms;
    presenter.SetDynamicPolicy(policy);
    const auto work = PeriodsOfWork(kPeriod, 1.35);
    RunWork(*app, 5000ms, work, [&](const XrFrameState&) {
        const FrameGenStatus status = presenter.Status();
        return status.runtime_halving && status.full_rate;
    });
    const auto stood_down = std::chrono::steady_clock::now();
    FrameGenStatus status = presenter.Status();
    Check(status.runtime_halving && status.full_rate && status.multiplier == 1,
          name + ": reported, Auto goes back to full rate (" + std::to_string(status.multiplier) + "x)");
    runtime.throttled = false;
    bool engaged = false;
    bool cleared = false;
    RunWork(*app, 1500ms, work, [&](const XrFrameState&) {
        const FrameGenStatus now = presenter.Status();
        engaged = engaged || now.multiplier > 1;
        cleared = cleared || !now.runtime_halving;
        return false;
    });
    Check(!engaged && !cleared,
          name + ": the slow game's waits leave the report, and Auto stays at full rate through its hold");
    RunWork(*app, 4000ms, work, [&](const XrFrameState&) {
        const FrameGenStatus now = presenter.Status();
        return now.multiplier >= 2 && now.threaded && !now.runtime_halving;
    });
    const double after = Seconds(std::chrono::steady_clock::now() - stood_down);
    status = presenter.Status();
    Check(status.multiplier >= 2 && status.threaded && !status.runtime_halving && after > 2.5,
          name + ": once the hold runs out, Auto engages and the presenter thread's waits clear the report (" +
              std::to_string(status.multiplier) + "x after " + std::to_string(after) + " s)");
    bool stood_down_again = false;
    RunWork(*app, 1000ms, work, [&](const XrFrameState&) {
        stood_down_again = stood_down_again || presenter.Status().multiplier < 2;
        return false;
    });
    Check(!stood_down_again && !presenter.Status().runtime_halving, name + ": Auto stays engaged");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    const std::vector<std::string> lines = log.Lines("-> full rate: the runtime holds frames to half rate");
    Check(lines.size() == 1, name + ": stood down once (" + std::to_string(lines.size()) + " lines)");
}

// Auto against a runtime holding the frames presented to half its refresh
// rate and reporting twice its period (Runtime::Throttle::kPeriod), with a
// game slower than that (frames of 2.7 refreshes): once the halving is
// reported, the game's frames are timed by the period reported and keep it,
// so Auto stays engaged and its generated frames fill every refresh the
// runtime takes, twice what the game alone gets through. Nothing stands down.
void TestAutoKeepsReportedHalving() {
    const std::string name = "auto keeps reported halving";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.throttle = Runtime::Throttle::kPeriod;
    SessionLog log("auto-reported-halving");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kAuto, app, owned, log.logger.get());
    // No probing down: only what the halving does to Auto is judged.
    presenter.SetAutoPolicy(200ms, 30s, 600ms, 300ms);
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.hold = policy.hold_max = 6000ms;
    presenter.SetDynamicPolicy(policy);
    // The refresh period is learned at full refresh first.
    RunWork(*app, 600ms, kLightWork);
    runtime.throttled = true;
    const auto work = PeriodsOfWork(kPeriod, 2.7);
    // Only the presenter thread's waits report it: Auto engaged.
    RunWork(*app, 4000ms, work, [&](const XrFrameState&) { return presenter.Status().runtime_halving; });
    FrameGenStatus status = presenter.Status();
    Check(status.runtime_halving, name + ": the slow game engages Auto, and the halving is reported");
    bool full_rate = false;
    RunWork(*app, 1500ms, work, [&](const XrFrameState&) {
        full_rate = full_rate || presenter.Status().multiplier < 2;
        return false;
    });
    const double output = SubmissionRate(*app, 800ms, work);
    status = presenter.Status();
    const double refresh = 1e9 / static_cast<double>(kPeriod);
    Check(!full_rate && status.runtime_halving && status.multiplier >= 2 && status.threaded,
          name + ": Auto stays engaged under the halving it is timed by (" + std::to_string(status.multiplier) + "x)");
    Check(output > 0.4 * refresh,
          name + ": every refresh the runtime takes gets a frame (" + std::to_string(output) + " fps)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    Check(log.Lines("-> full rate: the runtime holds frames to half rate").empty(), name + ": nothing stands down");
}

// The same runtime and game, Auto probing down: until the halving is
// reported, every engaged frame reads as late and Auto steps up past 2x.
// Once it is, the game's frames are timed by the period reported, and so is
// whether a probe is within reach: 2x (two reported periods, 44 ms, for the
// game's 30 ms) holds the game, so Auto probes down to it and settles there
// instead of staying a step higher (two generated frames for each real one,
// and the game held to 15 fps), and it never probes full rate, which the
// game cannot hold.
void TestAutoProbesReportedHalving() {
    const std::string name = "auto probes reported halving";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.throttle = Runtime::Throttle::kPeriod;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kAuto, app, owned, nullptr);
    // Reported after 2 s of halving: Auto has stepped up by then.
    presenter.SetHalvingWindow(2000ms);
    // Steady for a second: eight frames or more at 4x (89 ms a frame).
    presenter.SetAutoPolicy(200ms, 1000ms, 600ms, 300ms);
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.hold = policy.hold_max = 6000ms;
    presenter.SetDynamicPolicy(policy);
    RunWork(*app, 600ms, kLightWork);
    runtime.throttled = true;
    const auto work = PeriodsOfWork(kPeriod, 2.7);
    std::uint32_t highest = 0;
    RunWork(*app, 5000ms, work, [&](const XrFrameState&) {
        highest = std::max(highest, presenter.Status().multiplier);
        return presenter.Status().runtime_halving;
    });
    Check(presenter.Status().runtime_halving && highest >= 3,
          name + ": the slow game steps Auto up past 2x until the halving is reported (" + std::to_string(highest) +
              "x)");
    // Probes come after a steady second, each confirmed after 0.6 s.
    std::string steps = std::to_string(presenter.Status().multiplier) + "x";
    std::uint32_t last = presenter.Status().multiplier;
    const auto track = [&] {
        const std::uint32_t now = presenter.Status().multiplier;
        if (now != last) steps += " -> " + std::to_string(now) + "x";
        last = now;
        return now;
    };
    RunWork(*app, 8000ms, work, [&](const XrFrameState&) { return track() == 2; });
    std::uint32_t lowest = last, settled_highest = last;
    RunWork(*app, 3000ms, work, [&](const XrFrameState&) {
        const std::uint32_t now = track();
        lowest = std::min(lowest, now);
        settled_highest = std::max(settled_highest, now);
        return false;
    });
    const FrameGenStatus status = presenter.Status();
    Check(lowest == 2 && settled_highest == 2 && status.runtime_halving && status.threaded,
          name + ": Auto probes down to 2x on the period reported and stays there (" + steps + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// The session's first line names the diagnostic view and what it draws, and
// a change during play is logged once: a view left on looks like a fault
// (issue #95's red flashing around the eye-tracked foveation was the
// Confidence view), and the OSD that names it may be off.
void TestDiagnosticViewLogged() {
    const std::string name = "diagnostic view logged";
    FrameGenPresenter presenter;
    GpuLog gpu;
    SessionLog log("diagnostic-view");
    FrameGenResolvedSettings settings;
    settings.enabled = true;
    settings.synthesis_mode = FrameGenSynthesisMode::kExtrapolate;
    settings.diagnostic_view = FrameGenDiagnosticView::kConfidence;
    presenter.Configure(settings);
    presenter.Start(kSession, nullptr, Dispatch(), log.logger.get(), nullptr,
                    [&](const void*, std::string&) { return std::make_unique<FakeGpu>(&gpu); });
    presenter.Configure(settings); // unchanged: nothing more
    settings.diagnostic_view = FrameGenDiagnosticView::kOff;
    presenter.Configure(settings);
    presenter.Stop();
    const std::vector<std::string> lines = log.Lines("Frame Generation");
    const auto count = [&](const std::string& text) {
        return std::count_if(lines.begin(), lines.end(),
                             [&](const std::string& line) { return line.find(text) != std::string::npos; });
    };
    Check(count("Frame Generation enabled for this session") == 1 &&
              count("diagnosticView=confidence (generated frames are drawn red where they fall back to the real "
                    "image); frame pacing") == 1,
          name + ": the session's first line names the view and what it draws");
    Check(count("Frame Generation diagnostic view changed: confidence (generated frames are drawn red where they "
                "fall back to the real image) -> off.") == 1 &&
              count("diagnostic view changed") == 1,
          name + ": a change is logged once");
}

// The first frames Frame Generation makes are logged once, just in time
// (the default) too, with each view as the game submits it: its region of
// the swapchain image and its field of view in degrees (left/right/up/down),
// where a focus view an eye-tracked foveation moves would show.
void TestFirstFramesLogged() {
    const std::string name = "first frames logged";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    SessionLog log("first-frames");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, app, runtime, owned,
             log.logger.get());
    RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) { return presenter.Status().generated >= 4; });
    Check(presenter.Status().generated >= 4, name + ": generates");
    RunWork(*app, 200ms, kLightWork);
    presenter.Stop();
    const std::vector<std::string> lines = log.Lines("Frame Generation produced its first frames");
    const std::string view = "1600x1600 at 0,0 fov -45.8/45.8/45.8/-45.8";
    Check(lines.size() == 1 &&
              lines.front().find("views=2 (" + view + "; " + view + "), generatedPerFrame=just in time") !=
                  std::string::npos,
          name + ": once, with each view's region and field of view (" +
              (lines.empty() ? std::string("none") : lines.front()) + ")");
}

// A runtime that blocks every wait until its refresh (as Pimax's own,
// Meta's and Varjo's do), with a quick xrEndFrame: one of Frame
// Generation's submissions goes 7 ms late (its xrBeginFrame held up, as a
// write held up on the GPU would hold it), and the submission grid restarts
// from it. The waits after it keep blocking, so the runtime's own cadence is
// known and the grid only guards: within a few refreshes, submissions go at
// their waits' release again, rather than each held past it only a little
// less than the last.
void TestLateSubmissionRecovers() {
    const std::string name = "a late submission recovers";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::k2, app, owned, nullptr);
    for (int i = 0; i < 3; ++i) app->Frame();
    RunWork(*app, 500ms, kLightWork);
    runtime.stall_next_begin_us = 7000;
    RunWork(*app, 400ms, kLightWork);
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    // How long after the refresh that released its wait each of the twelve
    // submissions after the late one went (in milliseconds), beyond the usual
    // (the median of the twelve before it).
    std::vector<double> after;
    {
        std::scoped_lock lock(g_xr_mutex);
        const auto past_refresh = [&](std::size_t i) {
            const auto since = (runtime.submitted_at[i] - runtime.epoch) % std::chrono::nanoseconds(kPeriod);
            return std::chrono::duration<double, std::milli>(since).count();
        };
        const std::int64_t late = runtime.stalled_submission;
        if (late >= 12 && static_cast<std::size_t>(late) + 12 < runtime.submitted_at.size()) {
            const auto index = static_cast<std::size_t>(late);
            std::vector<double> before;
            for (std::size_t i = index - 12; i < index; ++i) before.push_back(past_refresh(i));
            const double usual = Median(before);
            for (std::size_t i = index + 1; i <= index + 12; ++i) after.push_back(past_refresh(i) - usual);
        }
    }
    std::string seen;
    for (const double ms : after) seen += " " + std::to_string(ms).substr(0, 5);
    if (std::getenv("FG_TEST_STATS")) std::cerr << name << ":" << seen << '\n';
    Check(after.size() == 12 && after.front() >= 3.0,
          name + ": the submission after the late one is held past its wait's release (" + seen + ")");
    if (after.size() == 12) {
        Check(Median(std::vector<double>(after.begin() + 3, after.begin() + 9)) <= 1.5,
              name + ": from the fourth refresh after it on, submissions go at their waits' release (" + seen + ")");
    }
}

// --- SteamVR with Pimax's headset driver ------------------------------------------

// Its waits asked for late return at once and report the caller's own
// interval, in whole refreshes, as the period (Runtime::echo), and its
// xrEndFrame takes 2.5 ms. Engaged (Dynamic, and a fixed 2x) for a game at
// 40 fps, Frame Generation's own submissions are back at the refresh rate
// right after a hitch (one of them 20 ms late: an interval of about 25 ms,
// which the runtime reports as two refreshes) and stay there, a refresh
// apart. Spaced by the period reported, they would keep the hitch's
// interval, the runtime echoing it back each time; spaced from when the
// last xrEndFrame returned, that call's 2.5 ms would add to each interval,
// and the runtime would release every wait at once (11.9-13.1 ms apart, not
// 11.1). Nothing is reported as halving, and Dynamic does not stand down.
void TestEchoRuntimeKeepsRefresh() {
    for (const FrameGenMultiplier multiplier : {FrameGenMultiplier::kDynamic, FrameGenMultiplier::k2}) {
        const bool dynamic = multiplier == FrameGenMultiplier::kDynamic;
        const std::string name = std::string("echo runtime, ") + (dynamic ? "dynamic" : "fixed 2x");
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.echo = true;
        runtime.submit_sleep_us = 2500;
        SessionLog log(dynamic ? "echo-dynamic" : "echo-2x");
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, multiplier, app, owned, log.logger.get());
        const auto work = PeriodsOfWork(kPeriod, 2.0); // with its end, 25 ms a frame: 40 fps
        if (dynamic) {
            RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return Engaged(presenter); });
            Check(Engaged(presenter), name + ": a game at 40 fps engages Dynamic");
        } else {
            for (int i = 0; i < 3; ++i) app->Frame();
        }
        RunWork(*app, 500ms, work);
        runtime.stall_next_submit_us = 20000;
        RunWork(*app, 300ms, work);
        const double refresh = 1e9 / static_cast<double>(kPeriod);
        for (int window = 1; window <= 2; ++window) {
            const std::size_t first = runtime.Submissions();
            const double output = SubmissionRate(*app, 1000ms, work);
            const double interval = MedianSubmitInterval(runtime, first);
            if (std::getenv("FG_TEST_STATS")) {
                std::cerr << name << ": second " << window << ": " << output << " fps, " << interval << " ms apart\n";
            }
            Check(output >= 0.85 * refresh && interval <= 1.03 * static_cast<double>(kPeriod) / 1e6,
                  name + ": after the hitch, Frame Generation's frames reach the runtime at the refresh rate, a "
                         "refresh apart (second " +
                      std::to_string(window) + ": " + std::to_string(output) + " fps, " + std::to_string(interval) +
                      " ms apart)");
        }
        const FrameGenStatus status = presenter.Status();
        Check(!status.runtime_halving && status.dynamic_stand_downs == 0 &&
                  (dynamic ? Engaged(presenter) : status.state == FrameGenState::kActive && status.threaded),
              name + ": nothing reported, engaged throughout (" + presenter.OsdDetailText() + " | " +
                  status.dynamic_reason + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
        Check(log.Lines(kHalvingLogged).empty(), name + ": nothing logged");
    }
}

// That runtime, the presenter thread woken late from a wait that blocked
// (Runtime::oversleep_next_release_us), 9 ms and then 24 ms after its
// refresh, as a system-wide hitch would hold it: the phase that wait sets is
// that late. Held to it, each submission would go that late, the next wait
// would come after the next refresh and return at once (no wait blocking to
// set the phase again), and submissions would stay 12-17 ms apart until the
// phase expired, two seconds on. Within a fifth of a second of each, they
// are back at the refresh rate, a refresh apart.
void TestEchoRuntimeLateWake() {
    for (const FrameGenMultiplier multiplier : {FrameGenMultiplier::kDynamic, FrameGenMultiplier::k2}) {
        const bool dynamic = multiplier == FrameGenMultiplier::kDynamic;
        const std::string name = std::string("echo runtime, late wake, ") + (dynamic ? "dynamic" : "fixed 2x");
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.echo = true;
        runtime.submit_sleep_us = 2500;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, multiplier, app, owned, nullptr);
        const auto work = PeriodsOfWork(kPeriod, 2.0);
        if (dynamic) {
            RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return Engaged(presenter); });
            Check(Engaged(presenter), name + ": a game at 40 fps engages Dynamic");
        } else {
            for (int i = 0; i < 3; ++i) app->Frame();
        }
        RunWork(*app, 500ms, work);
        const double refresh = 1e9 / static_cast<double>(kPeriod);
        for (const int late_ms : {9, 24}) {
            runtime.oversleep_next_release_us = late_ms * 1000;
            RunWork(*app, 1000ms, work, [&](const XrFrameState&) { return runtime.oversleep_next_release_us == 0; });
            Check(runtime.oversleep_next_release_us == 0, name + ": a wait blocked (" + std::to_string(late_ms) + " ms)");
            RunWork(*app, 200ms, work);
            const std::size_t first = runtime.Submissions();
            const double output = SubmissionRate(*app, 1000ms, work);
            const double interval = MedianSubmitInterval(runtime, first);
            if (std::getenv("FG_TEST_STATS")) {
                std::cerr << name << ": " << late_ms << " ms: " << output << " fps, " << interval << " ms apart\n";
            }
            Check(output >= 0.85 * refresh && interval <= 1.03 * static_cast<double>(kPeriod) / 1e6,
                  name + ": woken " + std::to_string(late_ms) +
                      " ms late, Frame Generation's frames are back at the refresh rate, a refresh apart (" +
                      std::to_string(output) + " fps, " + std::to_string(interval) + " ms apart)");
        }
        const FrameGenStatus status = presenter.Status();
        Check(!status.runtime_halving && status.dynamic_stand_downs == 0 &&
                  (dynamic ? Engaged(presenter) : status.state == FrameGenState::kActive && status.threaded),
              name + ": nothing reported, engaged throughout (" + presenter.OsdDetailText() + " | " +
                  status.dynamic_reason + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// That runtime once a throttle that reports its period (Runtime::Throttle::
// kPeriod) has been reported and stood down for: the throttle off, a game at
// 50 fps passing frames through asks for each wait late, and the runtime
// reports two refreshes for each at once, holding nothing. Those waits
// clear the report.
void TestEchoRuntimeClearsHalving() {
    const std::string name = "echo runtime clears halving";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.echo = true;
    runtime.submit_sleep_us = 2500;
    runtime.throttle = Runtime::Throttle::kPeriod;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
    FrameGenDynamicPolicy policy = QuickDynamic();
    policy.hold = policy.hold_max = 60s; // stood down, it stands by throughout
    presenter.SetDynamicPolicy(policy);
    RunWork(*app, 600ms, kLightWork);
    Check(presenter.Status().standby, name + ": a game holding the refresh rate stands by");
    runtime.throttled = true;
    RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) { return Engaged(presenter); });
    Check(Engaged(presenter), name + ": the game held to half the refresh rate engages Dynamic");
    RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) { return presenter.Status().standby; });
    FrameGenStatus status = presenter.Status();
    Check(status.standby && status.runtime_halving && status.dynamic_stand_downs == 1,
          name + ": the throttle reported and stood down for (" + status.dynamic_reason + ")");
    runtime.throttled = false;
    const auto work = PeriodsOfWork(kPeriod, 1.55); // 17 ms of work: about 50 fps
    const auto off = std::chrono::steady_clock::now();
    RunWork(*app, 3000ms, work, [&](const XrFrameState&) { return !presenter.Status().runtime_halving; });
    const double took = Seconds(std::chrono::steady_clock::now() - off);
    status = presenter.Status();
    if (std::getenv("FG_TEST_STATS")) std::cerr << name << ": cleared after " << took << " s\n";
    Check(!status.runtime_halving && took < 2.0 && status.standby && presenter.OsdDetailText() != kHalvingDetail,
          name + ": the slow game's waits clear it (" + std::to_string(took) + " s; " + presenter.OsdDetailText() +
              ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// That runtime with the presenter thread late every refresh (a fixed 2x, each
// xrEndFrame taking 1.6 refreshes, as a GPU-bound presenter's may): each of
// its waits is asked for late and released at once, reported as two
// refreshes, for a display time off the refresh grid. The runtime holds
// nothing back, so nothing is reported as halving.
void TestEchoRuntimeLatePresenter() {
    const std::string name = "echo runtime, presenter late";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.echo = true;
    SessionLog log("echo-late");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::k2, app, owned, log.logger.get());
    for (int i = 0; i < 3; ++i) app->Frame();
    runtime.submit_sleep_us = static_cast<int>(kPeriod * 16 / 10 / 1000);
    RunWork(*app, 300ms, kLightWork);
    const int echoed = runtime.echoed_multiples;
    const std::size_t first = runtime.Submissions();
    RunWork(*app, 2000ms, kLightWork);
    const int multiples = runtime.echoed_multiples - echoed;
    const auto submitted = static_cast<int>(runtime.Submissions() - first);
    if (std::getenv("FG_TEST_STATS")) std::cerr << name << ": " << multiples << " multiples, " << submitted << " submitted\n";
    Check(submitted > 0 && multiples * 4 >= submitted * 3,
          name + ": the presenter thread's waits come late, reported as multiples (" + std::to_string(multiples) +
              " of " + std::to_string(submitted) + ")");
    const FrameGenStatus status = presenter.Status();
    Check(!status.runtime_halving && status.dynamic_stand_downs == 0 && status.state == FrameGenState::kActive &&
              status.threaded && presenter.OsdDetailText() != kHalvingDetail,
          name + ": nothing reported, engaged throughout (" + presenter.OsdDetailText() + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    Check(log.Lines(kHalvingLogged).empty(), name + ": nothing logged");
}

// A hitch on that runtime: one interval of about 55 ms, reported as five
// refreshes, then a game at about 31 fps, each wait reported as three. The
// refresh period stays 90 Hz's (a hitch's period is not another refresh
// rate), so Dynamic engages for the game, judged against 90 Hz.
void TestEchoHitchKeepsRefresh() {
    const std::string name = "echo hitch keeps the refresh period";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.echo = true;
    runtime.submit_sleep_us = 2500;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
    RunWork(*app, 600ms, kLightWork);
    Check(presenter.Status().standby, name + ": a game holding the refresh rate stands by");
    // The hitch's interval (its work and its 2.5-ms end) in the middle of
    // what the runtime reports as five refreshes: from four and a half to
    // its reset.
    const auto five = (std::chrono::nanoseconds(kPeriod * 9 / 2) + Runtime::kEchoResetAfter) / 2;
    const auto hitch = std::chrono::duration_cast<std::chrono::microseconds>(five) - 2500us;
    RunWork(*app, 1000ms, {hitch}, [](const XrFrameState&) { return true; });
    std::vector<XrDuration> periods;
    RunWork(*app, 3000ms, PeriodsOfWork(kPeriod, 2.6), [&](const XrFrameState& state) {
        periods.push_back(state.predictedDisplayPeriod);
        return Engaged(presenter);
    });
    Check(!periods.empty() && periods.front() == 5 * kPeriod &&
              std::count(periods.begin() + 1, periods.end(), 3 * kPeriod) * 4 >=
                  static_cast<std::ptrdiff_t>(periods.size() - 1) * 3,
          name + ": the runtime reported the hitch as five refreshes, then mostly three (" +
              (periods.empty() ? std::string("none") : std::to_string(periods.front())) + " first, " +
              std::to_string(periods.size()) + " waits)");
    const FrameGenStatus status = presenter.Status();
    if (std::getenv("FG_TEST_STATS")) std::cerr << name << ": " << status.dynamic_reason << '\n';
    Check(Engaged(presenter) && status.dynamic_reason.find(" of 90.0)") != std::string::npos,
          name + ": Dynamic engages, the game judged against 90 Hz (" + status.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// The refresh period follows a longer period only once the periods reported
// have agreed with it for a while. On that runtime, one wait reported as
// four and a half refreshes (no whole number of them, so not its echo of
// the game's interval), then a game at 40 fps, each wait reported as two:
// the refresh period stays 90 Hz's, and Dynamic engages for the game,
// judged against 90 Hz. On a runtime whose refresh rate drops from 90 to
// 72 Hz, with the game keeping up: the refresh period is 72 Hz's within
// about a second and a half (Dynamic, engaging after a second below 90% of
// the refresh rate, would otherwise engage for the game at 72 fps), and a
// slower game is judged against 72 Hz.
void TestRefreshPeriodChanges() {
    {
        const std::string name = "one longer period";
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.echo = true;
        runtime.submit_sleep_us = 2500;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
        RunWork(*app, 600ms, kLightWork);
        Check(presenter.Status().standby, name + ": a game holding the refresh rate stands by");
        runtime.next_period = kPeriod * 9 / 2;
        std::vector<XrDuration> periods;
        RunWork(*app, 3000ms, PeriodsOfWork(kPeriod, 2.0), [&](const XrFrameState& state) {
            periods.push_back(state.predictedDisplayPeriod);
            return Engaged(presenter);
        });
        Check(periods.size() >= 2 && periods.front() == kPeriod * 9 / 2 &&
                  std::count(periods.begin() + 1, periods.end(), 2 * kPeriod) * 4 >=
                      static_cast<std::ptrdiff_t>(periods.size() - 1) * 3,
              name + ": the runtime reported 4.5 refreshes once, then mostly two (" + std::to_string(periods.size()) +
                  " waits)");
        const FrameGenStatus status = presenter.Status();
        Check(Engaged(presenter) && status.dynamic_reason.find(" of 90.0)") != std::string::npos,
              name + ": Dynamic engages, the game judged against 90 Hz (" + status.dynamic_reason + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
    {
        const std::string name = "refresh rate change";
        constexpr XrDuration k72Hz = 1'000'000'000 / 72;
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartHalving(presenter, gpu, runtime, FrameGenMultiplier::kDynamic, app, owned, nullptr);
        FrameGenDynamicPolicy policy = QuickDynamic();
        policy.engage_after = 1000ms;
        presenter.SetDynamicPolicy(policy);
        RunWork(*app, 600ms, kLightWork);
        Check(presenter.Status().standby, name + ": a game holding the refresh rate stands by");
        runtime.change_period = k72Hz;
        bool engaged = false;
        RunWork(*app, 2500ms, kLightWork, [&](const XrFrameState&) {
            engaged = engaged || Engaged(presenter);
            return engaged;
        });
        Check(!engaged && presenter.Status().standby,
              name + ": the game at the new refresh rate stands by (" + presenter.Status().dynamic_reason + ")");
        std::vector<std::chrono::microseconds> work;
        for (const int ticks : {1, 2, 2}) {
            work.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::nanoseconds(ticks * k72Hz)) -
                           3ms);
        }
        RunWork(*app, 4000ms, work, [&](const XrFrameState&) { return Engaged(presenter); });
        const FrameGenStatus status = presenter.Status();
        Check(Engaged(presenter) && status.dynamic_reason.find(" of 72.0)") != std::string::npos,
              name + ": Dynamic engages for a slower game, judged against 72 Hz (" + status.dynamic_reason + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// Varjo's runtime after a game paused its frame loop (a mission loading):
// display times far ahead, then barely moving until the refreshes catch up
// (Runtime::clock_jump_after). Dynamic stands by meanwhile (presented from
// such display times, refreshes would never reach the game's frames: nothing
// generated, the oldest frame kept shown, and a stand-down waiting for the
// newest frame's refresh), then engages and generates once they advance.
void TestVarjoClockJumpDynamic() {
    const std::string name = "varjo clock jump, dynamic";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    runtime.clock_jump_after = 500ms;
    runtime.clock_jump_factor = 2.0;
    SessionLog log("varjo-jump-dynamic");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, app, runtime, owned,
             log.logger.get());
    presenter.SetDynamicPolicy(GuardDynamic());
    app->eye_speed = 0.5;
    RunWork(*app, 600ms, kLightWork);
    Check(presenter.Status().standby, name + ": a game holding the refresh rate stands by");
    // The mission loads: no frame for 1.2 s; the runtime's display times run
    // 2.4 s ahead and creep until the refreshes catch up.
    std::this_thread::sleep_for(1200ms);
    const std::uint64_t generated_before = presenter.Status().generated;
    const std::uint64_t engagements_before = presenter.Status().dynamic_engagements;
    bool engaged_while_creeping = false;
    const auto creep_start = std::chrono::steady_clock::now();
    RunPipelined(*app, 1800ms, 16ms, 26ms,
                 [&](int, const XrFrameState&) {
                     engaged_while_creeping = engaged_while_creeping || presenter.Status().threaded;
                 },
                 nullptr, 8ms);
    Check(runtime.crept_waits > 60, name + ": the runtime's display times crept (" +
                                        std::to_string(runtime.crept_waits.load()) + " waits)");
    Check(!engaged_while_creeping && presenter.Status().dynamic_engagements == engagements_before,
          name + ": Dynamic stands by while the runtime's display times do not advance (" +
              presenter.Status().dynamic_reason + ")");
    // The refreshes pass the runtime's last display time about 2.4 s after
    // the game resumed; then they advance again (the game keeps rendering: an
    // idle half second would be another stall).
    for (int crept_before = -1; crept_before != runtime.crept_waits &&
                                std::chrono::steady_clock::now() - creep_start < 8000ms;) {
        crept_before = runtime.crept_waits;
        RunPipelined(*app, 400ms, 16ms, 26ms, {}, nullptr, 8ms);
    }
    const int crept = runtime.crept_waits;
    std::chrono::steady_clock::duration gap{};
    RunPipelined(*app, 3000ms, 16ms, 26ms, {}, &gap, 8ms);
    const FrameGenStatus status = presenter.Status();
    Check(runtime.crept_waits == crept, name + ": the runtime's display times advance again");
    Check(status.dynamic_engagements > engagements_before && status.threaded && !status.standby,
          name + ": Dynamic engages once they do (" + status.dynamic_reason + ")");
    Check(status.generated > generated_before + 40,
          name + ": and generates (" + std::to_string(status.generated - generated_before) + " frames)");
    Check(status.distinct_fps > 1.3 * status.app_fps,
          name + ": refreshes show more distinct frames than the game makes (" + std::to_string(status.distinct_fps) +
              " of " + std::to_string(status.app_fps) + " fps)");
    Check(gap < 200ms, name + ": the game is never held (longest frame " +
                           std::to_string(std::chrono::duration<double, std::milli>(gap).count()) + " ms)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    const std::vector<std::string> lines = log.Lines("Frame Generation");
    const auto logged = [&](const std::string& text) {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
    };
    Check(!logged("did not end within"), name + ": no drain timed out");
    Check(logged("display times ran") && logged("advance with the time again"),
          name + ": the log says why Dynamic waited, and when it stopped waiting");
}

// SteamVR's display times after a game's pause lag (a refresh after the
// last, as if no time had passed) and then catch up (Runtime::lag_after_gap):
// the catch-up runs no further ahead than before, so the runtime's clock is
// never taken as displaced, and Dynamic engages as usual.
void TestSteamVrLagIsNotDisplaced() {
    const std::string name = "steamvr lag after pauses";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    runtime.lag_after_gap = 150ms;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, app, runtime, owned);
    // Standing by at full rate, the game pauses for 300-600 ms a few times
    // (a loading hitch): each time the runtime's next display time lags. The
    // pauses lower the game's rate over a second; engaging only after longer
    // below it (as by default) it stays standing by.
    FrameGenDynamicPolicy standing = GuardDynamic();
    standing.engage_after = 2000ms;
    presenter.SetDynamicPolicy(standing);
    app->eye_speed = 0.5;
    int frames = 0;
    RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) {
        if (++frames % 40 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(300 + 100 * (frames / 40 % 4)));
        return false;
    });
    Check(runtime.lagged_waits >= 3, name + ": the runtime's display times lagged after the pauses (" +
                                         std::to_string(runtime.lagged_waits.load()) + " times, " +
                                         std::to_string(frames) + " frames, engagements " +
                                         std::to_string(presenter.Status().dynamic_engagements) + ", " +
                                         presenter.Status().dynamic_reason + ")");
    Check(presenter.Status().runtime_clock_displacements == 0 && !presenter.Status().runtime_clock_displaced,
          name + ": its clock is never taken as displaced");
    presenter.SetDynamicPolicy(GuardDynamic());
    RunPipelined(*app, 1500ms, 16ms, 26ms, {}, nullptr, 8ms);
    const FrameGenStatus status = presenter.Status();
    Check(status.threaded && !status.standby && status.runtime_clock_displacements == 0,
          name + ": Dynamic engages for a slower game (" + status.dynamic_reason + ")");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// A session's first runtime wait seen with a display time far behind its
// refresh's (Runtime::lag_next; a hitch while the session starts): the
// clock's ceiling is taken over its first waits, so the waits after it never
// look ahead, and Frame Generation presents as usual.
void TestLowFirstClockSample() {
    const std::string name = "low first clock sample";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    runtime.lag_next = 800'000'000;
    SessionLog log("low-first-sample");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k2, app, runtime, owned,
             log.logger.get());
    app->eye_speed = 0.5;
    RunPipelined(*app, 2000ms, 16ms, 26ms, {}, nullptr, 8ms);
    const FrameGenStatus status = presenter.Status();
    presenter.Stop();
    std::string said;
    for (const std::string& line : log.Lines("display times")) said += " | " + line;
    // A displacement that holds Frame Generation off for good would leave
    // nothing generated. A brief one recovers within a second; it is printed
    // with the waits that caused it (seen twice in about ten full runs under
    // the policy tests' load, never alone), not failed.
    if (status.runtime_clock_displacements > 0) {
        std::cerr << name << ": note: " << status.runtime_clock_displacements << " displacement(s)" << said << '\n';
    }
    Check(status.runtime_clock_displacements <= 1 && !status.runtime_clock_displaced && status.threaded &&
              status.generated > 20,
          name + ": Frame Generation presents and generates (" + std::to_string(status.runtime_clock_displacements) +
              " displacements, " + std::to_string(status.generated) + " generated" + said + ")");
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// Display times that stop moving while each wait still blocks a refresh
// (SteamVR during a game's load; Runtime::stall_clock): three such waits
// stop the presenter thread, the game is not held, and presenting resumes
// once they move with the time again.
void TestStalledClockWhilePresenting() {
    const std::string name = "stalled clock while presenting";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    SessionLog log("stalled-clock");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::k2, app, runtime, owned,
             log.logger.get());
    app->eye_speed = 0.5;
    RunPipelined(*app, 1000ms, 16ms, 26ms, {}, nullptr, 8ms);
    Check(presenter.Status().threaded, name + ": presents from its own thread");
    runtime.stall_clock = true;
    std::chrono::steady_clock::duration gap{};
    bool stopped = false;
    RunPipelined(*app, 800ms, 16ms, 26ms,
                 [&](int, const XrFrameState&) { stopped = stopped || !presenter.Status().threaded; }, &gap, 8ms);
    Check(stopped && presenter.Status().runtime_clock_displaced,
          name + ": the presenter thread stops while display times stand still");
    Check(gap < 400ms, name + ": the game is never held long (longest frame " +
                           std::to_string(std::chrono::duration<double, std::milli>(gap).count()) + " ms)");
    runtime.stall_clock = false;
    const std::uint64_t generated = presenter.Status().generated;
    RunPipelined(*app, 2000ms, 16ms, 26ms, {}, nullptr, 8ms);
    const FrameGenStatus status = presenter.Status();
    Check(!status.runtime_clock_displaced && status.threaded && status.generated > generated + 20,
          name + ": presenting resumes once they move again (" + std::to_string(status.generated - generated) +
              " frames)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    const std::vector<std::string> lines = log.Lines("Frame Generation");
    Check(std::any_of(lines.begin(), lines.end(),
                      [](const std::string& line) { return line.find("stopped advancing with the time") != std::string::npos; }),
          name + ": the log says the display times stopped advancing");
}

// Auto, its multiplier chosen for a slow game, while the runtime's clock is
// displaced: frames pass straight through, and the multiplier is held (each
// frame, judged against one refresh, would otherwise step it up to 4x for
// nothing, and the game would be held to a quarter of the refresh rate when
// presenting resumed).
void TestAutoHoldsWhileClockDisplaced() {
    const std::string name = "auto holds while the clock is displaced";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kAuto, app, runtime, owned);
    presenter.SetAutoPolicy(200ms, 500ms, 600ms, 300ms);
    RunWork(*app, 2000ms, PeriodsOfWork(kPeriod, 1.6));
    const std::uint32_t settled = presenter.Status().multiplier;
    Check(settled >= 2 && presenter.Status().threaded, name + ": a slow game engages Auto (" +
                                                           std::to_string(settled) + "x)");
    runtime.jump_next = 2'000'000'000;
    std::uint32_t highest = 0;
    RunWork(*app, 1500ms, PeriodsOfWork(kPeriod, 1.6), [&](const XrFrameState&) {
        if (presenter.Status().runtime_clock_displaced) highest = std::max(highest, presenter.Status().multiplier);
        return false;
    });
    Check(highest > 0 && highest <= settled, name + ": the multiplier is held (" + std::to_string(settled) + "x, then " +
                                                 std::to_string(highest) + "x while displaced)");
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// Displaced runtime clocks are logged one by one for the first few, then
// only in summaries: each logged end ends a logged displacement (past the
// first three, the ends were logged without them).
void TestClockLogPairs() {
    const std::string name = "clock log pairs";
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    SessionLog log("clock-pairs");
    std::unique_ptr<App> owned;
    App* app = nullptr;
    StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kDynamic, app, runtime, owned,
             log.logger.get());
    RunWork(*app, 600ms, kLightWork);
    int displacements = 0;
    for (int i = 0; i < 5; ++i) {
        runtime.jump_next = 400'000'000; // display times run 0.4 s ahead, then creep
        RunWork(*app, 3000ms, kLightWork, [&](const XrFrameState&) { return presenter.Status().runtime_clock_displaced; });
        displacements += presenter.Status().runtime_clock_displaced ? 1 : 0;
        RunWork(*app, 4000ms, kLightWork, [&](const XrFrameState&) { return !presenter.Status().runtime_clock_displaced; });
        Check(!presenter.Status().runtime_clock_displaced, name + ": display times advance again");
        RunWork(*app, 200ms, kLightWork); // the ceiling is taken anew
    }
    presenter.Stop();
    Check(displacements == 5, name + ": each jump displaced the clock (" + std::to_string(displacements) + " of 5)");
    const std::vector<std::string> lines = log.Lines("Frame Generation: the runtime's display times");
    const auto count = [&](const char* text) {
        return std::count_if(lines.begin(), lines.end(),
                             [&](const std::string& line) { return line.find(text) != std::string::npos; });
    };
    const auto began = count("nothing engages");
    const auto ended = count("advance with the time again (after");
    Check(began == 3 && ended == began,
          name + ": the first three are logged, each with its end (" + std::to_string(began) + " began, " +
              std::to_string(ended) + " ended)");
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// Auto probes a step down once its frames run steady a while: a game whose
// own frames take longer than that step allows fails every probe, with a
// second or two of late frames each (DCS on Varjo, 2026-10-14: 2x -> full
// rate and back every 35 s, "37% / 87% of frames late", at 17-22 ms a frame
// against 11.1 ms refreshes). Auto probes only a game within reach of the
// step, as Dynamic does; one that gets lighter is probed and steps down.
// Run just in time, then with frames written after each real frame, where
// the game's xrEndFrame also waits for its frame's slot (the cadence's hold,
// like its waits', not its own time) and hands the frame to the runtime
// itself (3 ms here: its own time).
void TestAutoProbesNeedRoom() {
    const std::string name = std::string("auto probes need room") + (g_jit ? "" : ", written after each real frame");
    FrameGenPresenter presenter;
    GpuLog gpu;
    Runtime runtime;
    runtime.paced = true;
    std::unique_ptr<App> owned;
    App* app = nullptr;
    if (g_jit) {
        StartJit(presenter, gpu, FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::kAuto, app, runtime, owned);
    } else {
        runtime.submit_sleep_us = 3000;
        StartThreaded(presenter, gpu, FrameGenMultiplier::kAuto);
        owned = std::make_unique<App>(presenter, runtime);
        app = owned.get();
    }
    presenter.SetAutoPolicy(200ms, 500ms, 600ms, 300ms);
    RunWork(*app, 2000ms, PeriodsOfWork(kPeriod, 1.6));
    const std::uint32_t settled = presenter.Status().multiplier;
    Check(settled == 2 && presenter.Status().threaded,
          name + ": a game of 1.6 refreshes a frame engages Auto at 2x (" + std::to_string(settled) + "x)");
    int probes = 0;
    std::uint32_t last = settled;
    const auto count_probes = [&](const XrFrameState&) {
        const std::uint32_t now = presenter.Status().multiplier;
        probes += now < last ? 1 : 0;
        last = now;
        return false;
    };
    // Probes would come every 0.3 s, then 0.6, 1.2 and 2.4 s.
    RunWork(*app, 3000ms, PeriodsOfWork(kPeriod, 1.6), count_probes);
    Check(probes == 0 && presenter.Status().multiplier == 2,
          name + ": a game that cannot run at full rate is not probed there (" + std::to_string(probes) + " probes)");
    RunWork(*app, 3000ms, PeriodsOfWork(kPeriod, 0.4), [&](const XrFrameState&) {
        return presenter.Status().multiplier == 1;
    });
    Check(presenter.Status().multiplier == 1,
          name + ": a game that got lighter is probed at full rate and stays there (" +
              std::to_string(presenter.Status().multiplier) + "x)");
    if (!g_jit) {
        // A pipelined game (MSFS): its render thread's xrEndFrame waits for
        // its frame's slot while its main thread works on the next frame.
        // That wait is not taken off the main thread's time: 14 ms a frame
        // cannot run at full rate.
        RunPipelined(*app, 2000ms, 14ms, 12ms);
        Check(presenter.Status().multiplier == 2,
              name + ": a pipelined game of 14 ms a frame engages Auto at 2x (" +
                  std::to_string(presenter.Status().multiplier) + "x)");
        int pipelined_probes = 0;
        std::uint32_t pipelined_last = presenter.Status().multiplier;
        RunPipelined(*app, 3000ms, 14ms, 12ms, [&](int, const XrFrameState&) {
            const std::uint32_t now = presenter.Status().multiplier;
            pipelined_probes += now < pipelined_last ? 1 : 0;
            pipelined_last = now;
        });
        Check(pipelined_probes == 0,
              name + ": a pipelined game that cannot run at full rate is not probed there (" +
                  std::to_string(pipelined_probes) + " probes)");
    }
    presenter.Stop();
    Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
}

// The same jump while Frame Generation presents (no game pause causes it
// then, but a runtime may): the presenter thread's refreshes stop advancing,
// so it stops and frames pass straight through within moments, the game never
// held for long, until the display times advance again; then it presents
// again. Fixed 2x and Dynamic alike.
void TestVarjoClockJumpWhilePresenting() {
    for (const FrameGenMultiplier multiplier : {FrameGenMultiplier::k2, FrameGenMultiplier::kDynamic}) {
        const std::string name =
            std::string("varjo clock jump while presenting, ") + (multiplier == FrameGenMultiplier::k2 ? "2x" : "dynamic");
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        SessionLog log(multiplier == FrameGenMultiplier::k2 ? "varjo-presenting-2x" : "varjo-presenting-dynamic");
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, FrameGenSynthesisMode::kInterpolate, multiplier, app, runtime, owned,
                 log.logger.get());
        presenter.SetDynamicPolicy(GuardDynamic());
        app->eye_speed = 0.5;
        RunPipelined(*app, 1200ms, 16ms, 26ms, {}, nullptr, 8ms);
        Check(presenter.Status().threaded, name + ": presents from its own thread");
        runtime.jump_next = 2'000'000'000;
        std::chrono::steady_clock::duration gap{};
        bool stopped = false;
        const auto jumped = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration stopped_after{};
        RunPipelined(*app, 1500ms, 16ms, 26ms,
                     [&](int, const XrFrameState&) {
                         if (!stopped && !presenter.Status().threaded) {
                             stopped = true;
                             stopped_after = std::chrono::steady_clock::now() - jumped;
                         }
                     },
                     &gap, 8ms);
        Check(stopped && stopped_after < 800ms,
              name + ": the presenter thread stops soon after the display times stop advancing (" +
                  std::to_string(std::chrono::duration<double, std::milli>(stopped_after).count()) + " ms)");
        Check(gap < 400ms, name + ": the game is never held long (longest frame " +
                               std::to_string(std::chrono::duration<double, std::milli>(gap).count()) + " ms)");
        // Until the refreshes pass the runtime's last display time, nothing
        // presents again.
        bool presented_while_creeping = false;
        for (int crept_before = -1; crept_before != runtime.crept_waits &&
                                    std::chrono::steady_clock::now() - jumped < 8000ms;) {
            crept_before = runtime.crept_waits;
            RunPipelined(*app, 300ms, 16ms, 26ms,
                         [&](int, const XrFrameState&) {
                             presented_while_creeping = presented_while_creeping ||
                                                        (presenter.Status().threaded &&
                                                         presenter.Status().runtime_clock_displaced);
                         },
                         nullptr, 8ms);
        }
        Check(runtime.crept_waits > 0, name + ": the runtime's display times crept");
        Check(!presented_while_creeping, name + ": nothing presents while they creep");
        const std::uint64_t generated = presenter.Status().generated;
        RunPipelined(*app, 2500ms, 16ms, 26ms, {}, nullptr, 8ms);
        const FrameGenStatus status = presenter.Status();
        Check(status.threaded && status.generated > generated + 40,
              name + ": it presents and generates again once they advance (" +
                  std::to_string(status.generated - generated) + " frames, " + status.dynamic_reason + ")");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
        Check(log.Lines("did not end within").empty(), name + ": no drain timed out");
    }
}

// Varjo's runtime after a late submission holds frames to every second
// refresh for a few, reporting twice the period (Throttle::kPeriod), then
// returns to full rate. Dynamic keeps timing the game's frames on the refresh
// grid: timed by the doubled period, they ran that much further ahead of the
// refreshes, past the real frames kept, and stayed there (nothing times a
// frame earlier than the one before), so refreshes showed the oldest kept
// frame as it is, again and again, and the guard stood down.
void TestVarjoTransientThrottle() {
    for (const FrameGenSynthesisMode mode : {FrameGenSynthesisMode::kInterpolate, FrameGenSynthesisMode::kExtrapolate}) {
        const std::string name = std::string("varjo transient throttle, ") +
                                 (mode == FrameGenSynthesisMode::kInterpolate ? "interpolate" : "extrapolate");
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        runtime.throttle = Runtime::Throttle::kPeriod;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, mode, FrameGenMultiplier::kDynamic, app, runtime, owned);
        presenter.SetHalvingWindow(4000ms);
        app->eye_speed = 0.5;
        RunPipelined(*app, 900ms, 16ms, 26ms, {}, nullptr, 8ms);
        Check(presenter.Status().threaded && !presenter.Status().standby, name + ": engages");
        const std::size_t first = runtime.Submissions();
        // Every 40th frame, the runtime holds frames to every second refresh
        // for the next 4 of the game's frames.
        std::vector<XrDuration> leads;
        std::vector<XrTime> times; // each real frame's display time
        RunPipelined(
            *app, 3000ms, 16ms, 26ms,
            [&](int frame, const XrFrameState& state) {
                if (frame % 40 == 0) runtime.throttled = true;
                if (frame % 40 == 4) runtime.throttled = false;
                times.push_back(state.predictedDisplayTime);
                std::scoped_lock lock(g_xr_mutex);
                if (!runtime.submitted_times.empty()) leads.push_back(state.predictedDisplayTime - runtime.submitted_times.back());
            },
            nullptr, 8ms);
        const FrameGenStatus status = presenter.Status();
        Check(status.threaded && !status.standby && !status.runtime_halving,
              name + ": stays engaged, the throttles too brief to be halving (" + status.dynamic_reason + ")");
        std::sort(leads.begin(), leads.end());
        const double median_lead = leads.empty() ? 0.0 : static_cast<double>(leads[leads.size() / 2]) / kPeriod;
        const double high_lead = leads.empty() ? 0.0 : static_cast<double>(leads[leads.size() * 9 / 10]) / kPeriod;
        Check(high_lead < median_lead + 3.0,
              name + ": the game's frames stay about as far ahead of the refreshes (median " +
                  std::to_string(median_lead) + ", 90th percentile " + std::to_string(high_lead) + " refreshes)");
        double worst = 0.0;
        const double timeline = TimelineShare(runtime, first, 30, app->eye_speed, &worst);
        // Extrapolating, a real frame later than a whole interval of its
        // varying pace holds its motion at one interval, so the refreshes it
        // repeats are a refresh off their own time. This game's 16 and 26 ms
        // frames land 2 and 2 refreshes apart in one phase and 1 and 3 in
        // another, where one refresh in four repeats (as in
        // TestJitDynamicExtrapolation): those the clamp predicts from the
        // game's own frame times are allowed besides. Without them the bar
        // failed about one run in seven on hosted CI (0.793, PR #91).
        const std::size_t refreshes = runtime.Submissions() - first;
        const double clamped = mode == FrameGenSynthesisMode::kExtrapolate && refreshes > 0
                                   ? static_cast<double>(ClampedRepeats(times)) / static_cast<double>(refreshes)
                                   : 0.0;
        if (std::getenv("FG_TEST_STATS")) {
            std::cerr << name << ": timeline " << timeline << ", clamp predicts " << clamped << " of " << refreshes
                      << " refreshes\n";
        }
        // A tenth of the frames go to a runtime holding every second refresh:
        // their refreshes may show a refresh off.
        Check(timeline > 0.8 - clamped, name + ": refreshes show their own time (" + std::to_string(timeline) +
                                            ", worst " + std::to_string(worst) + " periods, " +
                                            std::to_string(clamped) + " repeats the clamp predicts)");
        // ...and refreshes held back by the runtime show nothing new.
        Check(status.distinct_fps > 0.75 * status.output_fps,
              name + ": refreshes show new frames (" + std::to_string(status.distinct_fps) + " of " +
                  std::to_string(status.output_fps) + " fps)");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// SteamVR with Pimax's driver, its GPU busy: now and then its compositor
// misses a refresh, and the wait it releases a refresh late reports twice
// the period, once. The game's frames were timed by that period: the next
// one ran five refreshes further ahead, and the four after it came a
// refresh apart instead of two while the lead came back (a stutter each
// time; seen every 0.3-0.5 s in a DCS flight on SteamVR 2.18). Timed on the
// refresh grid, the game's lead stays put.
void TestSteamVrMissedRefresh() {
    struct Case {
        FrameGenSynthesisMode mode;
        FrameGenMultiplier multiplier;
        const char* name;
    };
    for (const Case test : {Case{FrameGenSynthesisMode::kExtrapolate, FrameGenMultiplier::k2, "extrapolate 2x"},
                            Case{FrameGenSynthesisMode::kInterpolate, FrameGenMultiplier::kDynamic, "interpolate dynamic"}}) {
        const std::string name = std::string("steamvr missed refresh, ") + test.name;
        FrameGenPresenter presenter;
        GpuLog gpu;
        Runtime runtime;
        runtime.paced = true;
        runtime.miss_reports_multiple = true;
        std::unique_ptr<App> owned;
        App* app = nullptr;
        StartJit(presenter, gpu, test.mode, test.multiplier, app, runtime, owned);
        app->eye_speed = 0.5;
        RunPipelined(*app, 900ms, 16ms, 26ms, {}, nullptr, 8ms);
        Check(presenter.Status().threaded && !presenter.Status().standby, name + ": engages");
        // About every 0.4 s, one of the runtime's waits misses a refresh.
        runtime.miss_every = 37;
        std::vector<XrDuration> leads;
        RunPipelined(
            *app, 3000ms, 16ms, 26ms,
            [&](int, const XrFrameState& state) {
                std::scoped_lock lock(g_xr_mutex);
                if (!runtime.submitted_times.empty()) leads.push_back(state.predictedDisplayTime - runtime.submitted_times.back());
            },
            nullptr, 8ms);
        const FrameGenStatus status = presenter.Status();
        Check(status.threaded && !status.standby && !status.runtime_halving,
              name + ": stays engaged, the misses too rare to be halving (" + status.dynamic_reason + ")");
        std::vector<XrDuration> sorted = leads;
        std::sort(sorted.begin(), sorted.end());
        const XrDuration median = sorted.empty() ? 0 : sorted[sorted.size() / 2];
        const auto ahead = std::count_if(leads.begin(), leads.end(),
                                         [&](XrDuration lead) { return lead > median + 2 * kPeriod; });
        // Timed by the reported period, 8-20% of the frames ran more than 2
        // refreshes past the median (up to 9); on the refresh grid none do.
        Check(!leads.empty() && ahead <= 2,
              name + ": the game's frames stay about as far ahead of the refreshes (" + std::to_string(ahead) + " of " +
                  std::to_string(leads.size()) + " over 2 refreshes past the median of " +
                  std::to_string(static_cast<double>(median) / kPeriod) + ")");
        Check(status.distinct_fps > 0.8 * status.output_fps,
              name + ": refreshes show new frames (" + std::to_string(status.distinct_fps) + " of " +
                  std::to_string(status.output_fps) + " fps)");
        presenter.Stop();
        Check(!runtime.ordering_violation && !g_release_violation, name + ": calls stayed ordered");
    }
}

// What the OSD and the app say while the runtime halves the frame rate:
// engaged and standing by, the line under the row says so and the app's
// line ends with it; other states keep their own reasons.
void TestOsdRuntimeHalving() {
    const std::string arrow = " \xE2\x86\x92 ";
    const std::string dot = " \xC2\xB7 ";
    std::vector<FrameGenOsdText> said;
    FrameGenStatus status;
    status.state = FrameGenState::kActive;
    status.mode = FrameGenSynthesisMode::kExtrapolate;
    status.dynamic = true;
    status.app_fps = 45.0;
    status.distinct_fps = 45.0;
    status.gpu_ms = 1.2;
    status.runtime_halving = true;
    said.push_back(DescribeFrameGen(status, true));
    FrameGenOsdText text = said.back();
    Check(text.row == "On  /  Extrapolate  /  Dynamic" && text.detail == kHalvingDetail &&
              text.compact == "Dynamic  /  45" + arrow + "45 fps" && text.badge == "ON" && text.state == "active",
          "halving engaged: " + text.row + " | " + text.detail + " | " + text.compact);
    Check(text.reason == "Extrapolate" + dot + "Dynamic" + dot + "45" + arrow + "45 fps" + dot + "GPU 1.2 ms" + dot +
                             kHalvingReason,
          "halving engaged: the app's line " + text.reason);
    status.standby = status.full_rate = true;
    said.push_back(DescribeFrameGen(status, true));
    text = said.back();
    Check(text.row == "Standby  /  Dynamic" && text.detail == kHalvingDetail &&
              text.compact == "Standby  /  game at 45 fps" && text.badge == "STBY" && text.state == "standby" &&
              text.reason == "Dynamic" + dot + "game at 45 fps" + dot + kHalvingReason,
          "halving standing by: " + text.row + " | " + text.detail + " | " + text.reason);
    status.runtime_halving = false;
    Check(DescribeFrameGen(status, true).detail == "The game runs at 45 fps", "not halving: standing by as before");
    status.runtime_halving = true;
    status.state = FrameGenState::kPaused;
    status.reason = "runtime rejected a generated frame (-2)";
    said.push_back(DescribeFrameGen(status, true));
    Check(said.back().detail == "Runtime rejected a generated frame (-2)" &&
              said.back().reason.find(kHalvingReason) == std::string::npos,
          "halving paused: the pause's own reason (" + said.back().detail + ")");
    status.state = FrameGenState::kOff;
    said.push_back(DescribeFrameGen(status, true));
    Check(said.back().detail.empty() && said.back().reason.empty(), "halving off: nothing to say");
    for (const FrameGenOsdText& words : said) {
        for (const std::string& line : {words.row, words.compact, words.detail, words.reason}) {
            for (const char* jargon : {"cockpit", "Direct3D", "Vulkan", "quad", "MSFS", "DCS", "full rate", "->"}) {
                Check(line.find(jargon) == std::string::npos, "plain words: \"" + line + "\" says " + jargon);
            }
        }
    }
}

} // namespace

// FG_TEST_ONLY=<name> runs only the tests whose name contains it. Each test
// is named on stderr as it starts, so a failure, a hang or a CTest timeout
// says which one it was in (the policy tests' process too).
// What the OSD and the app say about each status: short, general words in
// one vocabulary (On, Standby, Starting, Paused, Unavailable, Off).
void TestOsdDescription() {
    const std::string arrow = " \xE2\x86\x92 ";
    const std::string dot = " \xC2\xB7 ";
    std::vector<FrameGenOsdText> said;
    const auto describe = [&](const FrameGenStatus& status, bool started = true) {
        said.push_back(DescribeFrameGen(status, started));
        return said.back();
    };
    FrameGenStatus status;
    status.state = FrameGenState::kActive;
    status.dynamic = true;
    status.multiplier = 2;
    status.app_fps = 45.2;
    status.distinct_fps = 89.6;
    status.gpu_ms = 3.14;
    FrameGenOsdText text = describe(status);
    Check(text.row == "On  /  Interpolate  /  Dynamic" && text.detail == "45" + arrow + "90 fps  /  GPU 3.1 ms",
          "generating: row " + text.row + " | " + text.detail);
    Check(text.compact == "Dynamic  /  45" + arrow + "90 fps", "generating: compact " + text.compact);
    Check(text.badge == "ON" && text.shown_fps == 89.6 && text.state == "active", "generating: the FPS badge");
    Check(text.reason == "Interpolate" + dot + "Dynamic" + dot + "45" + arrow + "90 fps" + dot + "GPU 3.1 ms",
          "generating: the app's line " + text.reason);

    status.dynamic = false;
    status.multiplier = 3;
    status.mode = FrameGenSynthesisMode::kExtrapolate;
    status.gpu_ms = -1.0;
    status.app_fps = 30.0;
    status.distinct_fps = 90.0;
    text = describe(status);
    Check(text.row == "On  /  Extrapolate  /  3x" && text.detail == "30" + arrow + "90 fps", "fixed: " + text.row + " | " + text.detail);
    status.automatic = true;
    text = describe(status);
    Check(text.row == "On  /  Extrapolate  /  Auto 3x" && text.compact == "Auto 3x  /  30" + arrow + "90 fps" &&
              text.shown_fps == 90.0,
          "auto: " + text.row + " | " + text.compact);

    status.full_rate = true;
    status.app_fps = 89.7;
    text = describe(status);
    Check(text.row == "Standby  /  Auto" && text.detail == "The game runs at 90 fps" &&
              text.compact == "Standby  /  game at 90 fps" && text.badge == "STBY" && text.shown_fps == 0.0 &&
              text.state == "standby" && text.reason == "Auto" + dot + "game at 90 fps",
          "auto at full rate stands by: " + text.row + " | " + text.reason);
    status.dynamic = true;
    status.standby = true;
    status.automatic = false;
    status.app_fps = 84.0;
    text = describe(status);
    Check(text.row == "Standby  /  Dynamic" && text.detail == "The game runs at 84 fps" && text.badge == "STBY" &&
              text.state == "standby",
          "dynamic stands by: " + text.row);
    // The runtime's display times do not advance with the time (Varjo's after
    // a loading stall): standing by and starting say they wait for that.
    status.runtime_clock_displaced = true;
    text = describe(status);
    Check(text.row == "Standby  /  Dynamic" && text.detail == "Waiting for the runtime's frame timing to recover" &&
              text.reason == "Dynamic" + dot + "game at 84 fps" + dot + "waiting for the runtime's frame timing to recover",
          "dynamic stands by for the runtime's clock: " + text.detail + " | " + text.reason);
    {
        FrameGenStatus starting;
        starting.state = FrameGenState::kWaiting;
        starting.runtime_clock_displaced = true;
        starting.reason = "the runtime's display times ran 2.4 s ahead of the time that passed";
        const FrameGenOsdText waiting = describe(starting);
        Check(waiting.row == "Starting" && waiting.badge == "WAIT" &&
                  waiting.detail == "Waiting for the runtime's frame timing to recover" &&
                  waiting.reason == "waiting for the runtime's frame timing to recover",
              "starting waits for the runtime's clock: " + waiting.detail + " | " + waiting.reason);
    }
    status.runtime_clock_displaced = false;
    status.quad_declining = true;
    status.reason = "the layer composes the quad views for a while";
    text = describe(status);
    Check(text.row == "Standby" && text.badge == "STBY" && text.state == "standby" &&
              text.detail.find("retrying soon") != std::string::npos &&
              text.reason.find("could not be shown") != std::string::npos &&
              text.reason.find("quad") == std::string::npos,
          "frames that could not be written stand by: " + text.row + " | " + text.reason);

    status = FrameGenStatus{};
    status.state = FrameGenState::kWaiting;
    text = describe(status);
    Check(text.row == "Starting" && text.detail == "Waiting for steady frames" && text.compact == "Starting" &&
              text.badge == "WAIT" && text.state == "waiting" && text.reason.empty(),
          "waiting: " + text.row);
    status.state = FrameGenState::kPaused;
    status.reason = "runtime rejected a generated frame (-2)";
    status.retry_pending = true;
    text = describe(status);
    Check(text.row == "Paused  /  retrying soon" && text.detail == "Runtime rejected a generated frame (-2)" &&
              text.compact == "Paused" && text.badge == "PAUSED" && text.state == "paused" &&
              text.reason == "runtime rejected a generated frame (-2)" + dot + "retrying soon",
          "paused: " + text.row + " | " + text.reason);
    status.retry_pending = false;
    Check(describe(status).reason == "runtime rejected a generated frame (-2)", "paused for good: the reason alone");
    status.state = FrameGenState::kUnavailable;
    status.reason = "no backend for this API";
    text = describe(status);
    Check(text.row == "Unavailable" && text.detail == "No backend for this API" && text.compact == "Unavailable" &&
              text.badge == "N/A" && text.state == "unavailable" && text.reason == "no backend for this API",
          "unavailable: " + text.row);
    status = FrameGenStatus{};
    text = describe(status);
    Check(text.row == "Off" && text.compact == "Off" && text.detail.empty() && text.badge == "OFF" && text.state == "off" &&
              text.reason.empty(),
          "off: " + text.row);
    text = describe(status, false);
    Check(text.row == "Disabled" && text.badge.empty(), "never started: " + text.row);

    for (const FrameGenOsdText& words : said) {
        for (const std::string& line : {words.row, words.compact, words.detail, words.reason}) {
            for (const char* jargon : {"cockpit", "Direct3D", "Vulkan", "quad", "MSFS", "DCS", "full rate", "->"}) {
                Check(line.find(jargon) == std::string::npos, "plain words: \"" + line + "\" says " + jargon);
            }
        }
    }
}

#define RUN(test)                                                                                \
    do {                                                                                         \
        const char* only = std::getenv("FG_TEST_ONLY");                                          \
        if (only && std::string(#test).find(only) == std::string::npos) break;                   \
        std::cerr << #test << (g_jit ? " (just in time)" : "") << std::endl;                     \
        test();                                                                                  \
    } while (0)

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
    RUN(TestTraceSwapchainCallsOnlyWhileCapturing);
    RUN(TestDynamicOwnRate);
    RUN(TestDynamicHysteresis);
    RUN(TestBenefitGuardBacksOff);
    RUN(TestDynamicProbe);
    RUN(TestExtrapolateDuplicateTargetGuard);
    RUN(TestNoExtrapolationAcrossPauses);
    RUN(TestLeadResetOnRestart);
    RUN(TestLeadStepsDownAfterHitches);
    RUN(TestLeadDoesNotFlap);
    RUN(TestSkipIdenticalExactCopies);
    RUN(TestTelemetryWindows);
    RUN(TestDynamicSwitchStandby);
    RUN(TestGpuWaitKeepsCadence);
    RUN(TestExtrapolateMeasureLagIsOnTime);
    RUN(TestPauseAndLoadingDoNotStandDown);
    RUN(TestWinningEngagementStays);
    RUN(TestWinningEngagementSurvivesLateCaptures);
    RUN(TestInterpolateOnShaders);
    RUN(TestInterpolateSourceFollowsLiveSwitch);
    RUN(TestDynamicToggleStandsBy);
    RUN(TestHitchesAreNotPauses);
    RUN(TestInterpolateMeasureLagIsOnTime);
    RUN(TestInterpolateLeadMakesRoomForAnalysis);
    RUN(TestBenefitGuardInterpolateMsfs);
    // Runtime halving, submission pacing and the refresh period (here, not
    // with the rest of their kind, to keep the two halves' times even).
    RUN(TestRuntimeHalvingStaysForSlowGame);
    RUN(TestLateSubmissionRecovers);
    RUN(TestEchoRuntimeLatePresenter);
    RUN(TestRefreshPeriodChanges);
}

// The policy tests in a second process of this executable, alongside the
// rest: CTest gives the executable one timeout, and each half runs one to
// two minutes against a paced runtime. The process is in a job that closes
// with this one, so a failure here ends it too. Null when it could not
// start.
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

// FG_TEST_SERIAL=1 runs the policy tests after the rest, in this process:
// both halves assert wall-clock pacing, each with presenter threads at the
// highest priority, so side by side each is a load the other is judged
// under (serially, the run takes about twice as long).
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
    RUN(TestWorkingMemoryAskedOncePerSecond);
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
    RUN(TestJitEyeShiftReported);
    RUN(TestJitDynamicExtrapolation);
    RUN(TestJitStaleMotion);
    RUN(TestJitInterpolateCoversLateMotion);
    RUN(TestJitInterpolation);
    RUN(TestJitDynamicInterpolation);
    RUN(TestJitWaitsForNoGameFrame);
    RUN(TestJitModeSwitchRestarts);
    RUN(TestToggleOffReadsOff);
    RUN(TestToggleOffLogsNoEngagement);
    // The threaded presenter's lifecycle with frames written together, then
    // just in time.
    for (const bool jit : {false, true}) {
        g_jit = jit;
        RUN(TestThreadedToggleAndReset);
        RUN(TestThreadedPipelinedDouble);
        RUN(TestThreadedDestroyedSwapchain);
        RUN(TestThreadedRuntimeFailureRecovers);
        RUN(TestTeardownPresenterHeldInSubmit);
        RUN(TestTeardownPresenterReleasedBySessionEnd);
        RUN(TestStalledTeardownDoesNotKeepLaterSwapchains);
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
    RUN(TestDiagnosticViewLogged);
    RUN(TestFirstFramesLogged);
    RUN(TestUnavailableWithoutBackend);
    RUN(TestOsdDescription);
    RUN(TestOsdRuntimeHalving);
    // A runtime halving the frame rate (its motion smoothing).
    RUN(TestRuntimeHalvingStandsDown);
    RUN(TestRuntimeHalvingRecovers);
    RUN(TestRuntimeHalvingNotReported);
    RUN(TestRuntimeHalvingClears);
    RUN(TestRuntimeHalvingClearedDoesNotStandDown);
    RUN(TestRuntimeHalvingRetriesBriefly);
    RUN(TestAutoStandsDownForRuntimeHalving);
    RUN(TestAutoHalvingHoldRunsOut);
    RUN(TestAutoKeepsReportedHalving);
    RUN(TestAutoProbesReportedHalving);
    // SteamVR with Pimax's headset driver.
    RUN(TestEchoRuntimeKeepsRefresh);
    RUN(TestEchoRuntimeLateWake);
    RUN(TestEchoRuntimeClearsHalving);
    RUN(TestEchoHitchKeepsRefresh);
    // Quad views Frame Generation composes itself.
    RUN(TestQuadClaimAndCompose);
    RUN(TestQuadSetupThroughComposedFrame);
    RUN(TestQuadFallbackShownAsIs);
    RUN(TestQuadNeverSubmitsFourViews);
    RUN(TestQuadClaimAheadOfDroppedFrames);
    RUN(TestQuadKeepsRealFramesInset);
    RUN(TestQuadDeclinedSwitchesToCanvas);
    RUN(TestQuadWriteFailuresDecline);
    RUN(TestQuadStandbyRecordsViewTargets);
    RUN(TestQuadClaimNeedsLatestCapture);
    RUN(TestQuadOutputsResized);
    RUN(TestQuadAsIsShowsNewestImage);
    RUN(TestQuadFailedWriteRepeatsLastShown);
    RUN(TestQuadRetiredOutputsFreed);
    RUN(TestQuadHeadsetOffBreak);
    RUN(TestQuadDynamicCycle);
    RUN(TestJitRingExhaustion);
    RUN(TestQuadStaleMotion);
    RUN(TestQuadDynamicDeepRing);
    RUN(TestQuadComposeStallOnce);
    // Runtime clocks that misbehave (Varjo's, SteamVR's): last, so the tests
    // above keep the timing they had beside the policy tests.
    RUN(TestVarjoClockJumpDynamic);
    RUN(TestVarjoClockJumpWhilePresenting);
    RUN(TestVarjoTransientThrottle);
    RUN(TestSteamVrLagIsNotDisplaced);
    RUN(TestLowFirstClockSample);
    RUN(TestStalledClockWhilePresenting);
    RUN(TestAutoHoldsWhileClockDisplaced);
    for (const bool jit : {true, false}) {
        g_jit = jit;
        RUN(TestAutoProbesNeedRoom);
    }
    g_jit = false;
    RUN(TestClockLogPairs);
    RUN(TestSteamVrMissedRefresh);
    RUN(TestLeadAfterRepeatedHitches);
    RUN(TestPassedOverFramesKeepLead);
    RUN(TestLeadFollowsLoweredCap);
    for (const bool jit : {true, false}) {
        g_jit = jit;
        RUN(TestLoadingFramesDoNotRaiseLead);
    }
    g_jit = false;
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
