#include <windows.h>

#include "depthxr/framegen_presenter.h"

#include "depthxr/config_path.h"
#include "depthxr/logger.h"
#include "depthxr/process_info.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <utility>

namespace depthxr {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kOverlapWait{250};
constexpr std::chrono::seconds kClaimedFrameWait{5};
constexpr XrDuration kOutputWaitTimeout = 100'000'000; // 100 ms
constexpr std::uint32_t kFailuresBeforePause = 3;
constexpr std::uint32_t kDemotionsBeforePause = 3;
constexpr std::uint32_t kMaxMultiplier = 4;
// Times a session the video memory ceiling may rise again (see UpdateAutoLocked).
constexpr std::uint32_t kMemoryRecoveries = 2;
constexpr std::chrono::seconds kPhaseLifetime{2};
constexpr int kAutoMaxBackoffDoublings = 3; // 10 s, 20 s, 40 s, 80 s
// A lower-multiplier probe starts from a steady game. Let its one-off
// transition settle briefly, then judge its actual cadence rather than
// spending the full engagement warmup on an unsustainable rate.
constexpr std::chrono::milliseconds kAutoProbeSettle{250};
// ...and judges it on this many frames at least (a slow game's half second
// holds few: a 4x -> 3x probe at 90 Hz about fifteen).
constexpr std::size_t kAutoProbeJudgedFrames = 16;
// Auto counts a frame late once it takes this much longer than its refreshes.
constexpr double kAutoLateFactor = 1.25;
// Auto stays at full rate this long after inline cycles kept starting late.
constexpr std::chrono::seconds kInlineHold{60};
// The presenter thread waits this long for frames in flight when it stops.
constexpr std::chrono::seconds kThreadedDrain{2};
// Dynamic: a stand-down this soon after engaging doubles the next hold; this
// long engaged without one, the hold starts over at the policy's.
constexpr std::chrono::seconds kDynamicShortEngagement{60};
constexpr std::chrono::minutes kDynamicLongEngagement{5};
// GPU-aware pacing turns itself off for the session after this many game
// waits in a row timed out (a backend whose captures never report done).
constexpr std::uint32_t kGpuTimeoutsBeforeOff = 5;
// A gap between real frames is a pause past four of the game's own frame
// intervals, and never shorter than kPauseFloor (a hitch of a slow game is
// not one) nor longer than kPauseCeiling (a loading stall always is).
constexpr std::chrono::milliseconds kPauseFloor{250};
constexpr std::chrono::milliseconds kPauseCeiling{1000};
// Dynamic probes standing by only for a game whose engaged rate is within
// this share of the refresh rate of stay_above (what it runs at standing by
// is not much faster: the generated frames' own cost is about that).
constexpr double kProbeReach = 0.10;
// Pauses logged one by one per session; later ones only in summaries.
constexpr std::uint32_t kLoggedPauses = 3;
// Moves of the lead's ceiling from the history ring logged per session;
// later ones only in summaries (ringMoves).
constexpr std::uint32_t kLoggedRingMoves = 16;
// A lowering of the lead's ceiling is the policy's calm probe when that
// probe is due within this (a few frames): judged in its place, the policy
// keeps what it showed. Further ahead the probe may never come (a hitch
// resets its calm time first), and taking a hitch during the lowering for
// its failure raised the policy's floors to the lead lowered.
constexpr std::chrono::milliseconds kRingProbeAhead{100};
// Summary sample windows keep at most this many values.
constexpr std::size_t kMaxSamples = 4096;
// ...and of those taken once per game frame, this many (a 30 s window at
// up to 270 fps).
constexpr std::size_t kMaxFrameSamples = 8192;
// Composing quad views, this many attempted writes failing in a row (about
// 110 ms at 90 Hz) hand them back to the layer for a while.
constexpr std::uint32_t kQuadUnwrittenBeforeDecline = 10;
// Composing quad views: first few write failures logged one by one.
constexpr std::uint32_t kLoggedQuadWriteFailures = 3;
// What the On-Screen Display and the desktop app say while the runtime
// holds the frames Frame Generation presents to half its refresh rate or
// less (FrameGenStatus::runtime_halving), and why Dynamic stands down then.
constexpr const char* kHalvingDetail = "Runtime holds frames to half rate: check its smoothing";
constexpr const char* kHalvingReason = "the runtime holds frames to half rate: check its smoothing";
constexpr const char* kHalvingStandDown = "the runtime holds frames to half rate";
// ...and while the runtime's display times do not advance with the time
// (FrameGenStatus::runtime_clock_displaced).
constexpr const char* kClockDetail = "Waiting for the runtime's frame timing to recover";
constexpr const char* kClockReason = "waiting for the runtime's frame timing to recover";
// A period reported as two to this many times the refresh period is not
// taken as another refresh rate: a runtime cutting the frame rate (its
// motion smoothing) holds frames to every second, third or fourth refresh,
// and SteamVR with Pimax's headset driver (2.17.10) reports a late caller's
// own frame interval in whole refreshes, up to five.
constexpr std::uint32_t kMaxThrottleRefreshes = 5;
// A longer period that is no such multiple is a new refresh rate only once
// every period reported for this long agreed with it (a hitch is one wait).
constexpr std::chrono::seconds kRefreshChangeAfter{1};
// The runtime's display clock (NoteRuntimeClockLocked): displaced by a wait
// whose display time ran more than this (or 16 refreshes, if longer) further
// past this process's clock than any wait's while steady, or by this many
// waits in a row, each half a refresh or more after the last, whose display
// times moved less than a quarter refresh; steady again after this many waits
// over this long advanced with the time between them.
constexpr XrDuration kClockJumpFloor = 250'000'000; // 250 ms
constexpr std::uint32_t kClockStalledWaits = 3;
constexpr std::uint32_t kClockWarmupWaits = 10; // waits the ceiling is first taken over
constexpr std::uint32_t kClockSteadyWaits = 30;
constexpr std::chrono::milliseconds kClockSteadyTime{500};
// Draining the presenter thread, the newest real frame waits at most this
// many refreshes for its own (more than any lead), or until the runtime's
// display clock is displaced: a refresh that never reaches it would hold the
// game for the whole drain.
constexpr std::uint64_t kDrainRefreshes = 24;

std::string MultiplierText(std::uint32_t multiplier) {
    return std::to_string(multiplier) + "x";
}

// A diagnostic view for the log, with what it draws: one left on looks like
// a fault (the Confidence view's red was reported as flashing, issue #95),
// and only the OSD, when shown, names it otherwise.
std::string DiagnosticViewText(FrameGenDiagnosticView view) {
    switch (view) {
    case FrameGenDiagnosticView::kFlow:
        return "flow (generated frames are colored by the motion measured)";
    case FrameGenDiagnosticView::kConfidence:
        return "confidence (generated frames are drawn red where they fall back to the real image)";
    case FrameGenDiagnosticView::kTint:
        return "tint (generated frames are tinted)";
    default:
        return "off";
    }
}

// "twice", "three times": how many times the refresh period.
std::string TimesText(std::uint32_t times) {
    switch (times) {
    case 2:
        return "twice";
    case 3:
        return "three times";
    case 4:
        return "four times";
    case 5:
        return "five times";
    default:
        return std::to_string(times) + " times";
    }
}

// "other", "third": released only every ... refresh.
std::string EveryText(std::uint32_t refreshes) {
    switch (refreshes) {
    case 2:
        return "other";
    case 3:
        return "third";
    case 4:
        return "fourth";
    case 5:
        return "fifth";
    default:
        return std::to_string(refreshes) + "th";
    }
}

const XrCompositionLayerProjection* FindProjection(const XrFrameEndInfo& info, std::uint32_t* index = nullptr) {
    for (std::uint32_t i = 0; i < info.layerCount; ++i) {
        const auto* base = info.layers ? info.layers[i] : nullptr;
        if (base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            if (projection->viewCount > 0 && projection->views) {
                if (index) *index = i;
                return projection;
            }
        }
    }
    return nullptr;
}

FrameGenViewGeometry Geometry(const XrCompositionLayerProjectionView& view) {
    FrameGenViewGeometry geometry;
    geometry.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                            view.pose.orientation.w};
    geometry.position = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
    geometry.angle_left = view.fov.angleLeft;
    geometry.angle_right = view.fov.angleRight;
    geometry.angle_up = view.fov.angleUp;
    geometry.angle_down = view.fov.angleDown;
    return geometry;
}

// A head moves well under 2 m/s (meters per second); eyes that moved faster
// between two real frames are a tracking glitch.
constexpr double kMaxEyeSpeed = 2.0;

// The eye predictors each summary measures against the next real frame
// (eyePredictMissMm), in its order.
constexpr std::array<FrameGenEyePredictor, 5> kFrameGenEyeCandidates{
    FrameGenEyePredictor::kVelocity, FrameGenEyePredictor::kDamped, FrameGenEyePredictor::kTwoInterval,
    FrameGenEyePredictor::kQuadratic, FrameGenEyePredictor::kTwist};

// The eye probe (VECTORXR_FRAMEGEN_SYNTH_EYE_PROBE): a locate longer than
// this is counted and its answer dropped (it is made right before a
// refresh); this many in a session stop the probe for it. Real frames kept
// to bracket probed frames' times, and probed frames kept waiting for them.
constexpr double kEyeProbeSlowMs = 3.0;
constexpr std::uint32_t kEyeProbeSlowStop = 8;
constexpr std::size_t kEyeProbeReals = 6;
constexpr std::size_t kEyeProbePending = 64;

// `v` rotated by the unit quaternion `q`, and by its inverse.
std::array<double, 3> Rotate(const XrQuaternionf& q, const std::array<double, 3>& v) {
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    // v + 2 w (u x v) + 2 u x (u x v), u = q.xyz.
    const std::array<double, 3> uv{y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]};
    const std::array<double, 3> uuv{y * uv[2] - z * uv[1], z * uv[0] - x * uv[2], x * uv[1] - y * uv[0]};
    return {v[0] + 2.0 * (w * uv[0] + uuv[0]), v[1] + 2.0 * (w * uv[1] + uuv[1]), v[2] + 2.0 * (w * uv[2] + uuv[2])};
}

std::array<double, 3> RotateInverse(const XrQuaternionf& q, const std::array<double, 3>& v) {
    return Rotate(XrQuaternionf{-q.x, -q.y, -q.z, q.w}, v);
}

// The depth chained to a projection view, if any.
const XrCompositionLayerDepthInfoKHR* DepthInfo(const XrCompositionLayerProjectionView& view) {
    for (auto* next = static_cast<const XrBaseInStructure*>(view.next); next; next = next->next) {
        if (next->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
            return reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(next);
        }
    }
    return nullptr;
}

// A copied frame outlives xrEndFrame, and generated views name other images.
// Only metadata whose values and meaning can be kept is accepted. In
// particular, never silently remove an extension the application enabled.
bool SupportedLayerChains(const XrCompositionLayerBaseHeader& layer, std::string& unsupported) {
    for (auto* next = static_cast<const XrBaseInStructure*>(layer.next); next; next = next->next) {
        if (next->type != XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR &&
            next->type != XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB) {
            unsupported = "composition layer extension type " + std::to_string(next->type);
            return false;
        }
    }
    if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const auto& projection = reinterpret_cast<const XrCompositionLayerProjection&>(layer);
        for (std::uint32_t i = 0; projection.views && i < projection.viewCount; ++i) {
            for (auto* next = static_cast<const XrBaseInStructure*>(projection.views[i].next); next; next = next->next) {
                if (next->type != XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                    unsupported = "projection view extension type " + std::to_string(next->type);
                    return false;
                }
            }
        }
    }
    return true;
}

bool SupportedFrameChains(const XrFrameEndInfo& info, std::string& unsupported) {
    if (info.next) {
        const auto* next = static_cast<const XrBaseInStructure*>(info.next);
        unsupported = "frame end extension type " + std::to_string(next->type);
        return false;
    }
    for (std::uint32_t i = 0; info.layers && i < info.layerCount; ++i) {
        if (info.layers[i] && !SupportedLayerChains(*info.layers[i], unsupported)) return false;
    }
    return true;
}

FrameGenExtent Extent(const XrCompositionLayerProjectionView& view) {
    return {static_cast<std::uint32_t>(std::max(0, view.subImage.imageRect.extent.width)),
            static_cast<std::uint32_t>(std::max(0, view.subImage.imageRect.extent.height))};
}

// One history slot of these views, estimated: each view's image, and
// depth, trust and flow at half resolution or less (about one and a half
// images' worth); with the engine motion map (`map`, stage 7:
// FrameGenPresenter::EngineMapRingLocked) the cells' labels too, 4 bytes per
// flow cell at level 2 at most, and the world's motion kept beside them (a
// committed texture of its own: 64 KiB).
std::uint64_t HistorySlotBytes(const XrCompositionLayerProjectionView* views, std::size_t count, bool map) {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; views && i < count; ++i) {
        const FrameGenExtent extent = Extent(views[i]);
        bytes += std::uint64_t{extent.width} * extent.height * 6;
        bytes += map ? std::uint64_t{extent.width} * extent.height / 4 + 64 * 1024 : 0;
    }
    return bytes;
}

std::string Number(double value, int decimals) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

// Each candidate eye predictor's mean miss (kFrameGenEyeCandidates,
// millimeters), "name:miss/..." ("damped0.90" with its share), "-" when no
// frame was measured.
std::string EyePredictMissText(const std::array<double, 5>& sums, std::uint64_t frames, double damping) {
    if (frames == 0) {
        return "-";
    }
    std::string text;
    for (std::size_t i = 0; i < kFrameGenEyeCandidates.size(); ++i) {
        const FrameGenEyePredictor predictor = kFrameGenEyeCandidates[i];
        text += (text.empty() ? "" : "/") + std::string(ToString(predictor)) +
                (predictor == FrameGenEyePredictor::kDamped ? Number(damping, 2) : std::string()) + ":" +
                Number(sums[i] / static_cast<double>(frames), 3);
    }
    return text;
}

// A summary mean, "-" when nothing was counted.
std::string EyeProbeMean(double sum, std::uint64_t count, int decimals) {
    return count ? Number(sum / static_cast<double>(count), decimals) : std::string("-");
}

double Distance(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// "median/largest" of a summary window, "-" when empty.
std::string MedianAndMax(std::vector<double> values) {
    if (values.empty()) {
        return "-";
    }
    std::sort(values.begin(), values.end());
    return Number(values[values.size() / 2], 1) + "/" + Number(values.back(), 1);
}

// "low/high" percentiles (0-1) of a summary window, "-" when empty.
std::string Percentiles(std::vector<double> values, double low, double high) {
    if (values.empty()) {
        return "-";
    }
    const auto at = [&](double share) {
        const std::size_t index =
            std::min(values.size() - 1, static_cast<std::size_t>(share * static_cast<double>(values.size())));
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
        return values[index];
    };
    return Number(at(low), 1) + "/" + Number(at(high), 1);
}

// "lead:frames" for each lead told in a summary window, "-" when none.
std::string LeadCountsText(const std::map<std::uint32_t, std::uint64_t>& counts) {
    std::string text;
    for (const auto& [lead, frames] : counts) {
        text += (text.empty() ? "" : " ") + std::to_string(lead) + ":" + std::to_string(frames);
    }
    return text.empty() ? "-" : text;
}

std::optional<double> Median(std::vector<double> values) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
    return values[values.size() / 2];
}

void AddSample(std::vector<double>& samples, double value, std::size_t limit = kMaxSamples) {
    if (samples.size() < limit) {
        samples.push_back(value);
    }
}

double Milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Per second over a window of `seconds` (0 when too short to say).
double PerSecond(std::uint64_t count, double seconds) {
    return seconds > 0.05 ? static_cast<double>(count) / seconds : 0.0;
}

double RatePerSecond(std::deque<Clock::time_point>& stamps, Clock::time_point now) {
    while (!stamps.empty() && now - stamps.front() > std::chrono::seconds(1)) {
        stamps.pop_front();
    }
    if (stamps.size() < 2) {
        return 0.0;
    }
    const double span = std::chrono::duration<double>(stamps.back() - stamps.front()).count();
    return span > 0.0 ? static_cast<double>(stamps.size() - 1) / span : 0.0;
}

// As RatePerSecond, counting only the stamps after `since` (frames up to a
// switch, the one whose end made it included, say nothing about the frames
// after it, nor does the gap the switch itself takes).
double RateSince(std::deque<Clock::time_point>& stamps, Clock::time_point now, Clock::time_point since) {
    RatePerSecond(stamps, now);
    const auto first = std::upper_bound(stamps.begin(), stamps.end(), since);
    const auto count = stamps.end() - first;
    if (count < 2) {
        return 0.0;
    }
    const double span = std::chrono::duration<double>(stamps.back() - *first).count();
    return span > 0.0 ? static_cast<double>(count - 1) / span : 0.0;
}

// Sleeps with sub-millisecond precision; condition variables and plain
// sleeps round to the 15.6 ms system tick on Windows.
void SleepUntil(void* timer, Clock::time_point deadline) {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return;
    }
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (SetWaitableTimer(static_cast<HANDLE>(timer), &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(static_cast<HANDLE>(timer), 100) == WAIT_OBJECT_0) {
            return;
        }
    }
    std::this_thread::sleep_until(deadline);
}

} // namespace

const char* ToString(FrameGenDepthState state) {
    switch (state) {
    case FrameGenDepthState::kNotSent: return "not-sent";
    case FrameGenDepthState::kNotReadable: return "not-readable";
    case FrameGenDepthState::kChecking: return "checking";
    case FrameGenDepthState::kInUse: return "in-use";
    case FrameGenDepthState::kEmpty: return "empty";
    case FrameGenDepthState::kUnusable: return "unusable";
    default: return "unknown";
    }
}

const char* ToString(FrameGenState state) {
    switch (state) {
    case FrameGenState::kWaiting:
        return "waiting";
    case FrameGenState::kActive:
        return "active";
    case FrameGenState::kPaused:
        return "paused";
    case FrameGenState::kUnavailable:
        return "unavailable";
    default:
        return "off";
    }
}

FrameGenPresenter::~FrameGenPresenter() {
    // Process teardown: the runtime and graphics device may already be gone,
    // so never call into them here. Sessions end through Stop(), which the
    // layer runs on xrDestroySession and xrDestroyInstance.
    (void)gpu_.release();
    if (compositor_.joinable()) {
        compositor_.detach();
    }
    // The census worker's thread too: it stops by itself, and the worker it
    // uses is kept for the process.
    if (engine_worker_) {
        engine_worker_->Abandon();
        static_cast<void>(new std::shared_ptr<FrameGenEngineCensusWorker>(std::move(engine_worker_)));
    }
    if (timer_) {
        CloseHandle(static_cast<HANDLE>(timer_));
    }
    if (compositor_timer_) {
        CloseHandle(static_cast<HANDLE>(compositor_timer_));
    }
    if (compositor_wake_) {
        CloseHandle(static_cast<HANDLE>(compositor_wake_));
    }
}

void FrameGenPresenter::BeginSession() {
    // As Start: what an abandoned thread still uses is never reset under it
    // (and its GPU lock may be held for good).
    if (abandoned_) return;
    std::scoped_lock lock(gpu_mutex_);
    gpu_stalled_ = false;
}

void FrameGenPresenter::Start(XrSession session, const void* binding_chain, const FrameGenSwapchainDispatch& dispatch,
                              Logger* logger, std::mutex* queue_mutex, GpuFactory factory) {
    Stop();
    std::scoped_lock lock(gpu_mutex_, mutex_);
    if (abandoned_) {
        // What the thread left behind still uses is never reset under it.
        logger_ = logger;
        const std::string reason = "its presenter thread of an earlier session never left the runtime";
        UnavailableLocked(reason);
        if (logger_ && !logged_abandoned_start_) {
            logged_abandoned_start_ = true;
            logger_->Error("Frame Generation stays off for this process: " + reason + ".");
        }
        return;
    }
    session_ = session;
    xr_ = dispatch;
    logger_ = logger;
    queue_mutex_ = queue_mutex;
    if (logger_ && logger_->IsDebugEnabled()) {
        trace_.Start(*logger_);
    }
    started_ = true;
    user_enabled_ = true;
    gpu_stalled_ = false; // the last session's stalled work went with its swapchains
    started_at_ = Clock::now();
    interpolate_flow_env_.reset();
    if (const char* hardware = std::getenv("VECTORXR_FRAMEGEN_INTERPOLATE_HARDWARE");
        hardware && std::string(hardware) == "1") {
        interpolate_flow_env_ = FrameGenInterpolateFlow::kHardware;
    }
    interpolate_flow_selected_.reset();
    interpolate_flow_hardware_selected_ = false;
    interpolate_flow_hardware_ = true;
    interpolate_flow_available_.reset();
    interpolate_flow_active_.clear();
    interpolate_flow_reason_.clear();
    interpolate_flow_window_.reset();
    interpolate_flow_bad_windows_ = 0;
    interpolate_flow_probing_ = false;
    interpolate_flow_probe_started_ = {};
    interpolate_flow_probe_due_ = {};
    interpolate_flow_backoff_ = interpolate_flow_policy_.shader_hold;
    interpolate_flow_switches_ = interpolate_flow_probes_ = interpolate_flow_fallbacks_ = 0;
    interpolate_flow_delay_started_.reset();
    // The first summary's window starts with the session: without a start,
    // its rates were over no time and read 0.0 (in 14 of 14 sessions).
    last_summary_ = started_at_;
    healthy_streak_ = 0;
    automatic_retries_ = 0;
    paused_at_.reset();
    last_pause_at_.reset();
    state_ = configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff;
    reason_.clear();
    standby_since_ = started_at_;
    // GPU-aware pacing arms itself once frames reach the GPU late; the
    // variable sets it for measurements: off, held for the newest capture
    // (0) or one or two game frames before it, or always (from the start).
    gpu_gate_enabled_ = true;
    gpu_gate_armed_ = false;
    {
        const char* stale = std::getenv("VECTORXR_FRAMEGEN_STALE_MOTION");
        stale_motion_enabled_ = !(stale && std::string(stale) == "0");
    }
    // Engine motion vectors (research, stage 0): their census while the
    // space-warp probe takes the game's structures over. The research
    // switches may come from framegen-research.txt too (ReadResearchSwitch),
    // the backend's among them: this session's are told from here.
    ResetResearchFileSwitches();
    const std::uint64_t research_reads = ResearchFileReads();
    // A positive-control experiment: accept completed hardware vectors late
    // for a finite period, then normally again. GPU work and its fences run
    // unchanged. Inert unless explicitly set; never a GPU stall experiment.
    const auto delay_ms = [](const std::optional<std::string>& value, long long fallback, long long most) {
        if (!value || value->empty()) return std::chrono::milliseconds(fallback);
        char* end = nullptr;
        const long long parsed = std::strtoll(value->c_str(), &end, 10);
        return std::chrono::milliseconds(end && !*end && parsed >= 0 ? std::min(parsed, most) : fallback);
    };
    interpolate_flow_delay_ = delay_ms(ReadResearchSwitch("VECTORXR_FRAMEGEN_INTERPOLATE_DELAY_MS"), 0, 1000);
    interpolate_flow_delay_for_ =
        delay_ms(ReadResearchSwitch("VECTORXR_FRAMEGEN_INTERPOLATE_DELAY_FOR_MS"), 25000, 600000);
    if (logger_ && interpolate_flow_delay_.count()) {
        logger_->Info("Frame Generation Interpolate flow research: accepting completed hardware vectors " +
                      std::to_string(interpolate_flow_delay_.count()) + " ms late for the first " +
                      std::to_string(interpolate_flow_delay_for_.count()) +
                      " ms from the first hardware Interpolate analysis "
                      "(VECTORXR_FRAMEGEN_INTERPOLATE_DELAY_MS/_DELAY_FOR_MS); GPU work is unchanged.");
    }
    engine_requested_ =
        ParseFrameGenEngineMode(ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV").value_or("").c_str());
    // A game whose GPU device was removed within 2 s of its motion vectors
    // being read has them read no more, until the file the backend wrote
    // then is deleted (FrameGenEngineRemovedPath).
    std::string engine_removed;
    if (engine_requested_ != FrameGenEngineMode::kOff) {
        const std::filesystem::path removed = FrameGenEngineRemovedPath();
        std::error_code ignored;
        if (!removed.empty() && std::filesystem::exists(removed, ignored)) {
            engine_removed = std::string("Frame Generation: VECTORXR_FRAMEGEN_ENGINE_MV=") + ToString(engine_requested_) +
                             " is ignored: the GPU device was removed right after this game's motion vectors were read "
                             "in an earlier session. Delete " +
                             PathToUtf8(removed) + " to read them again.";
            engine_requested_ = FrameGenEngineMode::kOff;
        }
    }
    // A row per view and frame (VECTORXR_FRAMEGEN_ENGINE_MV_CSV=1), beside
    // the logs.
    const bool engine_rows = engine_requested_ != FrameGenEngineMode::kOff &&
                             ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_CSV") == "1";
    // Stage 2: the image a frame's vectors are taken from
    // (VECTORXR_FRAMEGEN_ENGINE_MV_IMAGE=released|color|depth|0|1|2; auto,
    // the default: the census worker's tracker).
    engine_image_.reset();
    if (engine_requested_ >= FrameGenEngineMode::kCapture) {
        using Policy = FrameGenEngineImageTracker::Policy;
        const std::string image = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_IMAGE").value_or("auto");
        if (image == "released") {
            engine_image_ = FrameGenEngineImageTracker::Hypothesis{Policy::kReleased};
        } else if (image == "color" || image == "colour") {
            engine_image_ = FrameGenEngineImageTracker::Hypothesis{Policy::kColor};
        } else if (image == "depth") {
            engine_image_ = FrameGenEngineImageTracker::Hypothesis{Policy::kDepth};
        } else if (image == "0" || image == "1" || image == "2") {
            engine_image_ = FrameGenEngineImageTracker::Hypothesis{Policy::kConstant,
                                                                   static_cast<std::uint32_t>(image[0] - '0')};
        }
    }
    engine_released_.clear();
    engine_capturable_ = false;
    motion_targets_.clear();
    engine_frame_ = {};
    engine_previous_ = {};
    engine_noted_ = engine_invalid_ = 0;
    engine_census_after_ = 0;
    engine_notes_logged_ = 0;
    logged_engine_failure_ = logged_engine_bridged_ = false;
    // Stage 9: a Vulkan game's vectors through the Vulkan bridge, at their
    // releases (the default), at the frame's end too for images it never
    // acquires (=end), or not at all (=0).
    engine_bridge_ = FrameGenEngineBridge::kNone;
    engine_bridge_mode_ = ParseFrameGenEngineBridgeMode(ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_BRIDGE").value_or(""));
    logged_engine_bridge_read_ = false;
    engine_empty_.clear();
    logged_engine_one_eye_ = false;
    engine_validation_ = engine_validation_logged_ = {};
    engine_decisions_logged_ = 0;
    engine_prior_checked_ = false;
    world_delta_ = world_delta_logged_ = {};
    world_delta_decisions_logged_ = 0;
    // Stage 1: the frame gates are counted for every session; these two act
    // on what they find.
    depth_encoding_check_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK") == "1";
    engine_cuts_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_CUTS") == "1";
    // Stage 5: how the filters and the confidence view treat the cells the
    // vectors moved (again at each config reload).
    engine_started_ = engine_requested_;
    engine_filter_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_FILTER") == "1";
    engine_cockpit_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_COCKPIT") == "1";
    engine_tint_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_TINT") == "1";
    // Research: the Flow view tinting sub-pixel motion (again at each config
    // reload).
    flow_view_fine_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_SYNTH_FLOW_VIEW") == "fine";
    // Research (the head-motion shimmer diagnosis, D), for the session: where
    // extrapolated frames' eyes go (VECTORXR_FRAMEGEN_SYNTH_EYE).
    eye_prediction_ = ParseFrameGenEyePrediction(ReadResearchSwitch("VECTORXR_FRAMEGEN_SYNTH_EYE").value_or(""));
    eye_history_.clear();
    eye_predict_sum_mm_ = {};
    eye_predict_frames_ = 0;
    // Research (D, log only), for the session: the runtime asked where the
    // views are at each generated frame's display time
    // (VECTORXR_FRAMEGEN_SYNTH_EYE_PROBE=1).
    eye_probe_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_SYNTH_EYE_PROBE") == "1";
    // Research (log only), for the session: the Vulkan bridge's depth probe
    // (VECTORXR_FRAMEGEN_DEPTH_BRIDGE_PROBE=1; the layer hands it on).
    depth_bridge_probe_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_DEPTH_BRIDGE_PROBE") == "1";
    eye_probe_stopped_ = false;
    eye_probe_slow_calls_ = 0;
    eye_probe_pending_.clear();
    eye_probe_reals_.clear();
    eye_probe_last_.reset();
    eye_probe_last_time_ = 0;
    eye_probe_window_ = {};
    // Stage 6, Lead: no measured passes at all (VECTORXR_FRAMEGEN_ENGINE_PREDICATE=0)
    // and interpolating led too (VECTORXR_FRAMEGEN_ENGINE_INTERPOLATE=lead),
    // for the session; the vectors lead once their verdicts let them.
    engine_predicate_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_PREDICATE") != "0";
    engine_interpolate_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_INTERPOLATE") == "lead";
    // Stage 7, for the session: frames extrapolated from real frames whose
    // cells the vectors labelled move their content by each cell's label with
    // a depth test (VECTORXR_FRAMEGEN_ENGINE_MAP=separate|merged), just in
    // time reading a map built for another frame of the same real frame again
    // (VECTORXR_FRAMEGEN_ENGINE_MAP_REUSE=1), and go past one interval with
    // the head's and the world's motion (VECTORXR_FRAMEGEN_ENGINE_REACH, 1 to
    // 2 intervals).
    engine_map_ = ParseFrameGenEngineMap(ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MAP").value_or(""));
    engine_map_reuse_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MAP_REUSE") == "1";
    engine_reach_ = 1.0f;
    if (const std::optional<std::string> reach = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_REACH")) {
        const float value = static_cast<float>(std::atof(reach->c_str()));
        engine_reach_ = value > 1.0f ? std::min(value, kFrameGenEngineReachMost) : 1.0f;
    }
    // Stage 8, for the session, interpolating: the merge takes the game's
    // vectors for content the previous frame did not see, and interpolated
    // frames show such content from the newest frame alone at motion
    // boundaries (VECTORXR_FRAMEGEN_ENGINE_VISIBILITY=1; =merge, the merge
    // alone); content being covered is read where the previous frame's own
    // motion puts it (VECTORXR_FRAMEGEN_ENGINE_BIDIR=1, the forward map).
    const std::string visibility = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_VISIBILITY").value_or("");
    engine_visibility_ = visibility == "1" || visibility == "merge";
    engine_visibility_synthesis_ = visibility == "1";
    engine_bidir_ = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_BIDIR") == "1";
    engine_lead_ = {};
    logged_engine_lead_ = false;
    engine_cut_window_.Reset();
    logged_engine_cut_ = false;
    depth_decoded_.clear();
    swapchain_created_at_.reset();
    swapchain_created_frame_ = 0;
    engine_window_ = {};
    gpu_frames_ahead_ = 1;
    gpu_timeouts_in_row_ = 0;
    // VECTORXR_FRAMEGEN_LEAD=events: the lead steps by hitch events
    // (FrameGenLeadPolicy::hitch_events), for comparisons in flight; off by
    // default until flown.
    const char* lead_setting = std::getenv("VECTORXR_FRAMEGEN_LEAD");
    const bool event_lead = lead_setting && std::string(lead_setting) == "events";
    if (event_lead) {
        lead_policy_.hitch_events = true;
    }
    // VECTORXR_FRAMEGEN_LEAD_CEILING=ring (or on) or off: the lead's ceiling
    // follows the history ring or not (FrameGenLeadPolicy::ring_ceiling,
    // whose default this outranks), for comparisons in flight.
    ring_ceiling_env_.reset();
    if (const char* ceiling = std::getenv("VECTORXR_FRAMEGEN_LEAD_CEILING"); ceiling) {
        const std::string setting = ceiling;
        if (setting == "ring" || setting == "on") {
            ring_ceiling_env_ = true;
        } else if (setting == "off") {
            ring_ceiling_env_ = false;
        }
    }
    std::string gpu_gate_setting;
    if (const char* ahead = std::getenv("VECTORXR_FRAMEGEN_GPU_AHEAD"); ahead && *ahead) {
        gpu_gate_setting = ahead;
        if (gpu_gate_setting == "off") {
            gpu_gate_enabled_ = false;
        } else if (gpu_gate_setting == "always") {
            gpu_gate_armed_ = true;
        } else if (gpu_gate_setting == "0" || gpu_gate_setting == "1" || gpu_gate_setting == "2") {
            gpu_frames_ahead_ = gpu_gate_setting[0] - '0';
            gpu_gate_armed_ = true;
        } else {
            gpu_gate_setting.clear(); // not a setting: the default
        }
    }
    if (!timer_) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    std::string error;
    gpu_ = factory ? factory(binding_chain, error) : nullptr;
    // The session's research switches from the file, the backend's among
    // them (read as it was made): each logged once per session, an earlier
    // session's in the process or not, a session without a backend's too.
    const std::vector<std::string> research = TakeResearchFileSwitches();
    if (logger_ && !research.empty()) {
        std::string switches;
        for (const std::string& line : research) {
            switches += (switches.empty() ? "" : ", ") + line;
        }
        logger_->Info("Frame Generation research switches from %LOCALAPPDATA%\\VectorXR\\framegen-research.txt "
                      "(the environment has none of them): " +
                      switches + ".");
    }
    if (logger_ && eye_prediction_.predictor != FrameGenEyePredictor::kVelocity) {
        logger_->Info(std::string("Frame Generation puts extrapolated frames' eyes where the ") +
                      ToString(eye_prediction_.predictor) +
                      (eye_prediction_.predictor == FrameGenEyePredictor::kDamped
                           ? " (" + Number(eye_prediction_.damping, 2) + ")"
                           : std::string()) +
                      " predictor says (VECTORXR_FRAMEGEN_SYNTH_EYE, research); interpolated frames keep theirs on "
                      "the line between the pair.");
    }
    if (logger_ && eye_probe_) {
        logger_->Info(std::string("Frame Generation asks the runtime where the views are at each generated frame's "
                                  "display time and logs how far that is from the eye the frame is shown from and "
                                  "from the real frames' path (eyeLocate* in the summaries; "
                                  "VECTORXR_FRAMEGEN_SYNTH_EYE_PROBE, research): nothing shown changes.") +
                      (xr_.locate_views ? "" : " The layer gave it no xrLocateViews: nothing is asked."));
    }
    if (!gpu_) {
        UnavailableLocked(error.empty() ? "no graphics backend" : error);
        return;
    }
    // The OSD says so while the session runs with them.
    research_file_ = ResearchFileReads() != research_reads;
    if (xr_.formats) {
        std::uint32_t count = 0;
        if (XR_SUCCEEDED(xr_.formats(session, 0, &count, nullptr)) && count) {
            runtime_formats_.resize(count);
            if (XR_FAILED(xr_.formats(session, count, &count, runtime_formats_.data()))) {
                runtime_formats_.clear();
            }
        }
    }
    std::string engine_text;
    if (engine_requested_ != FrameGenEngineMode::kOff) {
        if (!gpu_->SupportsEngineMotion()) {
            engine_text = std::string("Frame Generation: VECTORXR_FRAMEGEN_ENGINE_MV=") + ToString(engine_requested_) +
                          " is ignored: the " + gpu_->Name() + " backend cannot read a game's motion vectors.";
        } else {
            std::string csv;
            const char* local = std::getenv("LOCALAPPDATA");
            if (engine_rows && local && *local) {
                const std::time_t time = std::time(nullptr);
                std::tm local_time{};
                localtime_s(&local_time, &time);
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local_time);
                const std::filesystem::path logs = std::filesystem::path(local) / "VectorXR" / "logs";
                std::error_code ignored;
                std::filesystem::create_directories(logs, ignored);
                csv = (logs / (std::string("mv-research-") + stamp + ".csv")).string();
            }
            engine_worker_ = std::make_shared<FrameGenEngineCensusWorker>(logger_, csv);
            engine_text =
                std::string("Frame Generation engine motion vectors: ") + ToString(engine_requested_) +
                (engine_requested_ == FrameGenEngineMode::kCensus
                     ? " (research, stage 0): the game's motion vector images are fingerprinted and sampled while "
                       "frames are generated"
                     : std::string(engine_requested_ == FrameGenEngineMode::kLead      ? " (research, stage 6)"
                                   : engine_requested_ == FrameGenEngineMode::kAssist  ? " (research, stage 5)"
                                   : engine_requested_ == FrameGenEngineMode::kObserve ? " (research, stage 3)"
                                                                                       : " (research, stage 2)") +
                           ": while frames are generated, the image each frame's vectors are in is resampled to each "
                           "eye's half-resolution grid, at the game's release of it or at its xrEndFrame (image: " +
                           (engine_image_ ? ToString(engine_image_->policy) +
                                                (engine_image_->policy == FrameGenEngineImageTracker::Policy::kConstant
                                                     ? " " + std::to_string(engine_image_->parameter)
                                                     : std::string()) +
                                                ", VECTORXR_FRAMEGEN_ENGINE_MV_IMAGE"
                                          : std::string("tracked")) +
                           "), with a census of it" +
                           (engine_requested_ >= FrameGenEngineMode::kObserve
                                ? ", and each frame's vectors are judged against what the frame shows to work out "
                                  "their convention"
                                : "")) +
                (engine_requested_ >= FrameGenEngineMode::kAssist
                     ? std::string(", and where a frame's verdict lets them be used they take the measured flow's "
                                   "place per flow cell wherever the frame's luma follows them better") +
                           (engine_filter_ ? ", the filters treating those cells by their class "
                                             "(VECTORXR_FRAMEGEN_ENGINE_FILTER=1" +
                                                 std::string(engine_cockpit_ ? ", _COCKPIT=1)" : ")")
                                           : std::string()) +
                           (engine_tint_ ? ", tinted in the confidence view (VECTORXR_FRAMEGEN_ENGINE_TINT=1)"
                                         : std::string()) +
                           (engine_requested_ == FrameGenEngineMode::kLead
                                ? std::string("; leading once 30 verdicts in a row let them (held at assist for 300 "
                                              "frames when more than 10% of the last 30 do not), the measured passes ") +
                                      (engine_predicate_ ? "run only where a frame's verdict holds them back"
                                                         : "never run (VECTORXR_FRAMEGEN_ENGINE_PREDICATE=0)") +
                                      ", NVIDIA's optical flow hardware idle on those frames" +
                                      (engine_interpolate_ ? ", interpolating too (VECTORXR_FRAMEGEN_ENGINE_INTERPOLATE=lead)"
                                                           : ", extrapolating only")
                                : std::string()) +
                           (engine_map_ != FrameGenEngineMap::kOff
                                ? std::string("; extrapolated frames move each texel by its cell's label with a depth test "
                                              "(VECTORXR_FRAMEGEN_ENGINE_MAP=") +
                                      ToString(engine_map_) +
                                      (engine_map_ == FrameGenEngineMap::kMerged ? ": one map with the eye's offset" : "") +
                                      (engine_map_reuse_ ? ", read again within " + Number(kFrameGenEngineMapReuse, 2) +
                                                               " of an interval (_MAP_REUSE=1)"
                                                         : std::string()) +
                                      (engine_reach_ > 1.0f ? ", the head's and the world's motion up to " +
                                                                  Number(engine_reach_, 2) + " intervals on (_REACH)"
                                                            : std::string()) +
                                      ")"
                                : std::string()) +
                           (engine_visibility_
                                ? std::string("; interpolating, the vectors are taken for content the previous frame "
                                              "did not see (by its depth)") +
                                      (engine_visibility_synthesis_
                                           ? ", and generated frames show it from the newest frame alone at motion "
                                             "boundaries (VECTORXR_FRAMEGEN_ENGINE_VISIBILITY=1)"
                                           : " (VECTORXR_FRAMEGEN_ENGINE_VISIBILITY=merge)")
                                : std::string()) +
                           (engine_bidir_ ? "; interpolated frames read content being covered where the previous "
                                            "frame's own motion puts it (VECTORXR_FRAMEGEN_ENGINE_BIDIR=1, the forward "
                                            "map in place of the labels' read)"
                                          : "")
                     : std::string(", generated frames are made as without them")) +
                (csv.empty() ? std::string(".") : ", rows in " + csv + ".");
        }
    }
    if (logger_) {
        logger_->Info(std::string("Frame Generation enabled for this session: backend=") + gpu_->Name() +
                      ", mode=" + ToString(settings_.synthesis_mode) + ", flowQuality=" + ToString(settings_.flow_quality) +
                      ", motionEstimation=" + ToString(settings_.motion_estimation) +
                      ", diagnosticView=" + DiagnosticViewText(settings_.diagnostic_view) +
                      "; frame pacing is owned by Frame Generation (Turbo stays off).");
        if (!engine_removed.empty()) {
            logger_->Info(engine_removed);
        }
        if (!engine_text.empty()) {
            logger_->Info(engine_text);
        }
        if (depth_encoding_check_) {
            logger_->Info("Frame Generation: VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK=1 (research): a view whose depth "
                          "was decoded as the frame before described it (the first frame after a game changes its "
                          "nearZ or farZ) is generated without depth.");
        }
        if (engine_cuts_) {
            logger_->Info("Frame Generation: VECTORXR_FRAMEGEN_ENGINE_CUTS=1 (research): nothing is generated across "
                          "a camera cut (the game's space moving further in a frame than 50 m and 8 times its usual "
                          "movement, or turning more than 10 degrees, by the appSpaceDeltaPose it sends with its "
                          "motion vectors).");
        }
        // Stage 4: the backend's (VECTORXR_FRAMEGEN_ENGINE_DELTA, read as it
        // was made).
        if (const FrameGenWorldDeltaStatus delta = gpu_->WorldDeltaStatus(); delta.mode != FrameGenWorldDeltaMode::kOff) {
            logger_->Info(
                std::string("Frame Generation world motion from the game's appSpaceDeltaPose: ") + ToString(delta.mode) +
                " (research, stage 4): " +
                (delta.mode == FrameGenWorldDeltaMode::kCalibrate
                     ? "each world fit is compared with it, generated frames are made as without it"
                     : "it stands in for the world fit once the comparison locked (the fit runs as well every " +
                           std::to_string(kFrameGenWorldDeltaWatchdog) + "th frame, its watchdog)") +
                (delta.fixed ? ", fixed: rotation " + std::string(delta.sign < 0 ? "conjugated" : "as given") +
                                   ", s_t " + Number(delta.scale, 3)
                             : std::string()) +
                (delta.refused.empty() ? std::string()
                                       : ", VECTORXR_FRAMEGEN_ENGINE_DELTA_FIXED=" + delta.refused +
                                             " ignored (sign,scale: 1 or -1, 0.7-1.4)") +
                (delta.sky ? "; the sky turns with it (VECTORXR_FRAMEGEN_ENGINE_SKY=1)." : "."));
        }
        if (event_lead) {
            logger_->Info("Frame Generation: the lead steps by hitch events, not frame by frame "
                          "(VECTORXR_FRAMEGEN_LEAD=events).");
        }
        // Always: the default is on, and every flight log names its arm.
        if (RingCeilingLocked()) {
            logger_->Info(std::string("Frame Generation: the lead's ceiling follows the history ring: a lead whose "
                                      "moved refreshes find their real frame's previous images replaced is lowered a "
                                      "refresh ") +
                          (ring_ceiling_env_ ? "(VECTORXR_FRAMEGEN_LEAD_CEILING=ring)."
                                             : "(default; VECTORXR_FRAMEGEN_LEAD_CEILING=off turns it off)."));
        } else {
            logger_->Info("Frame Generation: the lead's ceiling is 4 refreshes per real frame; ring-short refreshes "
                          "are counted only (VECTORXR_FRAMEGEN_LEAD_CEILING=off).");
        }
        if (!gpu_gate_setting.empty()) {
            logger_->Info("Frame Generation: GPU-aware pacing set by VECTORXR_FRAMEGEN_GPU_AHEAD=" + gpu_gate_setting +
                          (gpu_gate_enabled_ ? ": the game's waits are held until its GPU has run the capture of " +
                                                   (gpu_frames_ahead_ == 0 ? std::string("its newest frame")
                                                                           : std::to_string(gpu_frames_ahead_) +
                                                                                 " frame(s) before its newest begun one") +
                                                   ", from the start."
                                             : ": off."));
        }
    }
}

void FrameGenPresenter::Stop() {
    trace_.Stop();
    // The census worker, stopped once the presenter's lock is let go.
    std::shared_ptr<FrameGenEngineCensusWorker> engine_worker;
    std::unique_lock lock(mutex_);
    if (!started_) {
        return;
    }
    if (!abandoned_ && !StopCompositorLocked(lock, TeardownWait())) {
        AbandonPresenterLocked();
    }
    // From here no release thread starts a capture.
    started_ = false;
    // The census worker shares nothing with the presenter thread: stopped
    // even when that thread is left behind.
    engine_worker = std::move(engine_worker_);
    engine_requested_ = engine_started_ = FrameGenEngineMode::kOff;
    // Nor do the session's research switches outlast it (a later Start of
    // an abandoned presenter reads none).
    depth_encoding_check_ = engine_cuts_ = research_file_ = false;
    engine_filter_ = engine_cockpit_ = engine_tint_ = false;
    engine_predicate_ = true;
    engine_interpolate_ = false;
    engine_map_ = FrameGenEngineMap::kOff;
    engine_map_reuse_ = false;
    engine_reach_ = 1.0f;
    engine_visibility_ = engine_visibility_synthesis_ = engine_bidir_ = false;
    ForgetEngineFramesLocked();
    if (abandoned_) {
        // The thread left behind may still use the backend, the output
        // images and the runtime when its call returns: all of them are kept
        // for the process's exit, as is the rest of the session's state.
        state_ = FrameGenState::kOff;
        reason_.clear();
        return;
    }
    // A game wait held for its GPU uses the backend without the GPU lock;
    // it returns within its timeout, before the backend is reset. One that
    // does not (logged) keeps the backend, unreset, and its outputs.
    const bool waiters_returned = idle_cv_.wait_for(lock, TeardownWait(), [&] { return gpu_waiters_ == 0; });
    if (!waiters_returned && logger_) {
        logger_->Error("Frame Generation: a game wait held for its GPU did not return within " +
                       std::to_string(TeardownWait().count()) + " ms of the session's end; its backend is kept.");
    }
    lock.unlock();
    {
        // Finish outstanding GPU work before the runtime frees the output
        // images it writes. Teardown runs between frames, so no EndFrame
        // touches the outputs concurrently.
        std::scoped_lock gpu_lock(gpu_mutex_);
        if (gpu_ && !waiters_returned) {
            gpu_stalled_ = true;
        } else if (gpu_) {
            gpu_->Reset();
            // A backend whose work did not finish keeps what it used; the
            // swapchains that work reads and writes must stay too.
            if (!gpu_->WaitIdle()) {
                gpu_stalled_ = true;
            }
        }
    }
    DestroyOutputs();
    {
        std::scoped_lock gpu_lock(gpu_mutex_);
        lock.lock();
        if (waiters_returned) {
            gpu_.reset();
        } else {
            (void)gpu_.release(); // the wait still reads it
        }
    }
    if (logger_ && (generated_ || repeated_ || failures_ || demoted_ || abandoned_frames_)) {
        logger_->Info("Frame Generation session summary: generated=" + std::to_string(generated_) + ", repeated=" +
                      std::to_string(repeated_) + ", failures=" + std::to_string(failures_) + ", demoted=" +
                      std::to_string(demoted_) + ", abandoned=" + std::to_string(abandoned_frames_) +
                      ", captured=" + std::to_string(captured_) + ".");
    }
    state_ = FrameGenState::kOff;
    reason_.clear();
    swapchains_.clear();
    output_kinds_.clear();
    end_thread_ = {};
    logged_claimed_wait_ = false;
    abandons_frames_ = logged_abandoned_frames_ = false;
    submit_grid_ = {};
    last_period_ = 0;
    refresh_period_ = 0;
    refresh_candidate_.reset();
    halving_last_.reset();
    halving_samples_.clear();
    halving_since_ = {};
    runtime_halving_ = false;
    halving_confirmed_at_.reset();
    logged_runtime_halving_ = false;
    phase_wall_ = {};
    phase_xr_ = 0;
    phase_echoed_ = false;
    clock_last_.reset();
    clock_history_next_ = 0;
    clock_warmup_waits_ = 0;
    clock_offset_ceiling_ = 0;
    clock_displaced_ = false;
    clock_ahead_ = 0;
    clock_gap_ = 0;
    clock_stalled_waits_ = 0;
    clock_steady_waits_ = 0;
    logged_clock_displacements_ = 0;
    clock_displacement_logged_ = false;
    auto_multiplier_ = 1;
    auto_memory_ceiling_ = kMaxMultiplier;
    memory_budget_.reset();
    // The OSD's PERFORMANCE row shows these as Frame Generation's video
    // memory: none until the next session allocates again.
    working_bytes_ = output_bytes_ = output_set_bytes_ = 0;
    working_bytes_due_ = {};
    memory_checked_at_ = {};
    memory_room_since_.reset();
    memory_recoveries_ = 0;
    // The backend that held the history went with it; the next session's
    // first ring choice is logged again.
    held_ring_ = 0;
    held_slot_bytes_ = 0;
    logged_canvas_ring_ = 0;
    full_rate_ = false;
    cadence_.clear();
    last_app_end_.reset();
    real_end_.reset();
    real_interval_ns_ = 0.0;
    multiplier_changed_at_ = {};
    probe_not_before_ = {};
    probe_backoff_ = auto_initial_backoff_;
    probing_ = false;
    late_at_ceiling_since_.reset();
    runtime_formats_.clear();
    frames_.clear();
    capture_targets_.clear();
    // A session's frame times say nothing about the next one's.
    last_returned_time_ = 0;
    last_real_ = {};
    extrapolated_ = {};
    consecutive_failures_ = consecutive_demotions_ = 0;
    // A claimed quad frame ending after this is swallowed (EndFrame).
    quad_offered_ = quad_mode_ = quad_switch_pending_ = false;
    quad_claim_id_.reset();
    quad_extents_ = {};
    quad_captures_ = {};
    quad_declined_until_ = {};
    quad_decline_ = quad_decline_first_;
    quad_write_declines_ = 0;
    quad_decline_resumes_ = false;
    quad_unwritten_in_row_ = 0;
    logged_quad_write_failures_ = 0;
    logged_quad_lost_claim_ = logged_quad_unanalyzed_ = false;
    newest_copy_fresh_ = false;
    generated_ = repeated_ = failures_ = demoted_ = abandoned_frames_ = captured_ = 0;
    threaded_frames_.clear();
    dropped_unended_ = dropped_superseded_ = 0;
    threaded_disabled_ = logged_threaded_ = false;
    unsupported_composition_ = false;
    runtime_waits_ = runtime_begins_ = runtime_ends_ = 0;
    presenter_frame_open_ = false;
    logged_order_guard_ = logged_watchdog_ = false;
    display_offset_periods_ = 0.0;
    display_offset_samples_ = 0;
    slot_index_ = 0;
    slot_time_ = slot_period_ = 0;
    app_latency_ms_ = 0.0;
    passthrough_copy_ = {};
    jit_covers_ = 0;
    logged_jit_ = false;
    logged_ring_moves_ = 0;
    ResetLeadLocked();
    dynamic_interval_slots_ = 0.0;
    dynamic_standby_ = true;
    dynamic_below_since_.reset();
    dynamic_hold_until_ = {};
    dynamic_hold_ = dynamic_policy_.hold;
    dynamic_probe_after_ = dynamic_policy_.probe_after;
    dynamic_engaged_at_ = {};
    dynamic_probing_ = false;
    passthrough_fps_ = 0.0;
    dynamic_reason_.clear();
    dynamic_engagements_ = dynamic_stand_downs_ = dynamic_probes_ = 0;
    standby_since_.reset();
    standby_seconds_ = 0.0;
    totals_ = {};
    summary_totals_ = {};
    summary_standby_seconds_ = 0.0;
    last_summary_ = {};
    guard_ = {};
    guard_failing_ = {};
    guard_capture_done_ms_.clear();
    gpu_gate_armed_ = false;
    gpu_timeouts_in_row_ = 0;
    gpu_reached_mark_ = 0;
    last_capture_mark_ = 0;
    begun_capture_marks_.clear();
    gpu_due_.clear();
    gpu_start_ms_.clear();
    gpu_done_ms_.clear();
    capture_done_ms_.clear();
    analysis_ms_.clear();
    measure_lag_ms_.clear();
    ofa_ms_.clear();
    analysis_lag_ms_ = 0.0;
    gpu_wait_ms_.clear();
    end_call_ms_.clear();
    runtime_end_ms_.clear();
    synth_lock_ms_.clear();
    ring_short_write_ms_.clear();
    told_lead_sum_ = 0;
    told_lead_frames_ = 0;
    told_lead_max_ = 0;
    game_ahead_ms_.clear();
    told_ahead_ms_.clear();
    told_lead_counts_.clear();
    ahead_jitter_ms_.clear();
    end_slack_ms_.clear();
    last_ahead_ms_ = 0.0;
    last_ahead_at_ = {};
    pause_longest_ms_ = 0.0;
    logged_pauses_ = 0;
    last_offer_slot_.reset();
    dropped_unbegun_ = 0;
    gpu_ms_ = -1.0;
    inline_hold_until_.reset();
    auto_halving_hold_until_.reset();
    auto_halving_hold_ = dynamic_policy_.hold;
    last_cycle_started_late_ = false;
    app_frames_.clear();
    submissions_.clear();
    distinct_submissions_.clear();
    repeated_submissions_ = 0;
    last_shown_set_.reset();
    logged_first_generation_ = logged_capture_failure_ = logged_depth_ = logged_uncaptured_ = false;
    logged_depth_decisions_ = 0;
    logged_depth_info_ = false;
    depth_description_.clear();
    logged_depth_description_.clear();
    depth_description_changes_ = depth_description_frames_ = 0;
    depth_description_lines_ = 0;
    view_extents_.clear();
    logged_view_extents_.clear();
    view_extent_changes_ = view_extent_frames_ = 0;
    view_extent_lines_ = 0;
    logged_depth_batches_ = 0;
    logged_empty_view_ = -1;
    session_ = XR_NULL_HANDLE;
    idle_cv_.notify_all();
}

bool FrameGenPresenter::OwnsPacing() const {
    std::scoped_lock lock(mutex_);
    // Mirrors the layer, which reads this once at session creation and keeps
    // pacing with Frame Generation for the session: a composition refusal
    // does not hand pacing back.
    return started_ && gpu_ && (state_ != FrameGenState::kUnavailable || unsupported_composition_);
}

bool FrameGenPresenter::Started() const {
    std::scoped_lock lock(mutex_);
    return started_;
}

void FrameGenPresenter::SetTraceEnabled(bool enabled) {
    Logger* logger = nullptr;
    {
        std::scoped_lock lock(mutex_);
        if (!started_ || abandoned_) {
            return;
        }
        logger = logger_;
    }
    // Outside the presenter lock: stopping joins the trace's writer.
    if (!enabled) {
        trace_.Stop();
    } else if (logger) {
        trace_.Start(*logger);
    }
}

void FrameGenPresenter::Configure(const FrameGenResolvedSettings& settings) {
    // Stage 5's live comparison: the research switches again, read before
    // the lock (the research file is read at each call).
    const std::optional<std::string> engine_mode = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV");
    const bool engine_filter = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_FILTER") == "1";
    const bool engine_cockpit = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_COCKPIT") == "1";
    const bool engine_tint = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_TINT") == "1";
    const bool flow_view_fine = ReadResearchSwitch("VECTORXR_FRAMEGEN_SYNTH_FLOW_VIEW") == "fine";
    std::scoped_lock lock(mutex_);
    ReconfigureEngineLocked(ParseFrameGenEngineMode(engine_mode.value_or("").c_str()), engine_filter, engine_cockpit,
                            engine_tint);
    flow_view_fine_ = flow_view_fine;
    // Just in time, the presenter thread keeps the mode it started with
    // (StayThreadedLocked): another one starts it again, after a fresh run of
    // steady frames, as turning Frame Generation off and on does. Frames
    // engaged inline in between would keep it from starting again (an app
    // that waits before ending its last frame always has one in flight).
    const bool restart = jit_ && compositor_running_ && settings.synthesis_mode != settings_.synthesis_mode;
    const bool was_dynamic = DynamicLocked();
    const FrameGenMultiplier multiplier = settings_.multiplier;
    const FrameGenDiagnosticView diagnostic_view = settings_.diagnostic_view;
    settings_ = settings;
    const bool dynamic = DynamicLocked();
    // Diagnostic views change during play: the log says when.
    if (started_ && logger_ && settings_.diagnostic_view != diagnostic_view) {
        logger_->Info("Frame Generation diagnostic view changed: " + DiagnosticViewText(diagnostic_view) + " -> " +
                      DiagnosticViewText(settings_.diagnostic_view) + ".");
    }
    if (settings_.multiplier != multiplier) {
        // Interpolate's motion source follows the frame rate: the log says
        // which it uses again after a switch.
        logged_interpolate_on_shaders_ = false;
        logged_interpolate_on_hardware_ = false;
    }
    if (started_ && (settings_.multiplier != multiplier || dynamic != was_dynamic)) {
        // Switched while running: frames held to the old multiplier say
        // nothing about the game's own rate, so Dynamic and Auto settle and
        // judge it afresh. Only Dynamic stands by: time at a fixed multiplier
        // is not standing by, and standing by up to the switch stays counted.
        const auto now = Clock::now();
        multiplier_changed_at_ = now;
        cadence_.clear();
        dynamic_below_since_.reset();
        // The ring's ceilings are keyed by multiplier, and their floors
        // follow the mode (a refresh per real frame for Dynamic): one learned
        // under Dynamic capped a fixed 2x run below what its ring holds.
        ResetRingCeilingLocked();
        if (was_dynamic && !dynamic) {
            if (standby_since_ && dynamic_standby_ && now > *standby_since_) {
                standby_seconds_ += std::chrono::duration<double>(now - *standby_since_).count();
            }
            standby_since_.reset();
        } else if (dynamic && !was_dynamic && dynamic_standby_) {
            standby_since_ = now;
        }
    }
    if (restart) {
        healthy_streak_ = 0;
        ++totals_.restarts; // the benefit guard's window says nothing about either mode
        if (state_ == FrameGenState::kActive) {
            SetStateLocked(FrameGenState::kWaiting, "");
        }
        if (logger_) {
            logger_->Info(std::string("Frame Generation restarts for ") + ToString(settings_.synthesis_mode) +
                          " once frames are steady again.");
        }
    }
}

void FrameGenPresenter::ReconfigureEngineLocked(FrameGenEngineMode mode, bool filter, bool cockpit, bool tint) {
    if (!started_ || engine_started_ < FrameGenEngineMode::kObserve) {
        return;
    }
    // Observe and Assist only, and Lead in a session started at Assist or Lead
    // (its flow ends at the hardware's level already, where the hardware is
    // chosen): the other modes capture otherwise.
    const bool allowed = mode == FrameGenEngineMode::kObserve || mode == FrameGenEngineMode::kAssist ||
                         (mode == FrameGenEngineMode::kLead && engine_started_ >= FrameGenEngineMode::kAssist);
    const FrameGenEngineMode next = allowed ? mode : engine_requested_;
    if (next == engine_requested_ && filter == engine_filter_ && cockpit == engine_cockpit_ && tint == engine_tint_) {
        return;
    }
    if (logger_) {
        logger_->Info(std::string("Frame Generation engine motion vectors from the next real frame (config reload): ") +
                      ToString(engine_requested_) + " -> " + ToString(next) + ", VECTORXR_FRAMEGEN_ENGINE_FILTER=" +
                      (filter ? "1" : "0") + ", _COCKPIT=" + (cockpit ? "1" : "0") + ", _TINT=" + (tint ? "1" : "0") +
                      (mode == next ? "." : std::string(" (") + ToString(mode) + " waits for the next session)."));
    }
    // Into Lead, the vectors lead again once their verdicts let them.
    if (next == FrameGenEngineMode::kLead && engine_requested_ != FrameGenEngineMode::kLead) {
        engine_lead_ = {};
    }
    engine_requested_ = next;
    engine_filter_ = filter;
    engine_cockpit_ = cockpit;
    engine_tint_ = tint;
}

void FrameGenPresenter::SetConfigured(bool enabled) {
    std::scoped_lock lock(mutex_);
    if (configured_enabled_ == enabled) {
        return;
    }
    configured_enabled_ = enabled;
    healthy_streak_ = 0;
    if (enabled) {
        DynamicReenableLocked(Clock::now());
    }
    if (started_ && state_ != FrameGenState::kUnavailable && state_ != FrameGenState::kPaused) {
        SetStateLocked(enabled && user_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
    }
}

void FrameGenPresenter::SetAutoPolicy(std::chrono::milliseconds settle, std::chrono::milliseconds steady,
                                      std::chrono::milliseconds probe_confirm, std::chrono::milliseconds probe_backoff) {
    std::scoped_lock lock(mutex_);
    auto_settle_ = settle;
    auto_steady_ = steady;
    auto_probe_confirm_ = probe_confirm;
    auto_initial_backoff_ = probe_backoff;
    probe_backoff_ = probe_backoff;
}

void FrameGenPresenter::SetLeadPolicy(const FrameGenLeadPolicy& policy) {
    std::scoped_lock lock(mutex_);
    lead_policy_ = policy;
    lead_explore_after_ = policy.explore_after;
    lead_return_after_ = policy.explore_after;
    for (LeadRing& ring : lead_ring_) {
        ring.retry = policy.explore_after;
    }
}

void FrameGenPresenter::ClearLeadHistoryLocked() {
    lead_history_.clear();
    lead_event_start_.reset();
    lead_events_.clear();
}

void FrameGenPresenter::ResetLeadLocked() {
    lead_slots_ = 0;
    ClearLeadHistoryLocked();
    lead_calm_since_ = {};
    lead_probe_.reset();
    lead_floor_ = 0;
    lead_floor_proven_ = 0;
    lead_explore_after_ = lead_policy_.explore_after;
    lead_return_after_ = lead_policy_.explore_after;
    lead_floor_failed_ = false;
    // The ring's ceilings were this run's: another mode, rate or engagement
    // captures on another timing.
    ResetRingCeilingLocked();
    logged_ring_held_ = false;
}

void FrameGenPresenter::ResetRingCeilingLocked() {
    for (LeadRing& ring : lead_ring_) {
        ring = LeadRing{};
        ring.retry = lead_policy_.explore_after;
    }
    lead_ring_event_.reset();
}

std::uint32_t FrameGenPresenter::RingLeadFloorLocked(std::uint32_t multiplier) const {
    return FrameGenRingLeadFloor(JitRingLocked(quad_mode_), jit_interpolate_, DynamicLocked() ? 1 : multiplier);
}

std::uint32_t FrameGenPresenter::LeadCapLocked(std::uint32_t multiplier) const {
    if (!RingCeilingLocked() || multiplier > 4 || !jit_) {
        return 4 * multiplier;
    }
    const std::uint32_t ceiling = lead_ring_[multiplier].ceiling;
    // A ceiling is never below the lead the ring holds at any capture time,
    // in the mode the run is in now.
    return ceiling ? std::min(4 * multiplier, std::max(ceiling, RingLeadFloorLocked(multiplier))) : 4 * multiplier;
}

bool FrameGenPresenter::RingJudgedLocked(const LeadRing& ring, Clock::time_point now) const {
    if (!ring.trial) {
        return false;
    }
    if (now - *ring.trial < lead_policy_.judge) {
        return true;
    }
    // A late event begun within judge awaits its second for event_window: a
    // lead too short is late again a hitch_span on, which may be past judge.
    return ring.late_event && *ring.late_event - *ring.trial < lead_policy_.judge &&
           now - *ring.late_event <= lead_policy_.event_window;
}

void FrameGenPresenter::ExpireRingCeilingLocked(std::uint32_t multiplier, Clock::time_point now) {
    if (multiplier > 4 || !DynamicLocked()) {
        return;
    }
    LeadRing& ring = lead_ring_[multiplier];
    const std::uint8_t slots = JitRingLocked(quad_mode_);
    if (!ring.ceiling || ring.interval <= 0.0 || dynamic_interval_slots_ <= 0.0 || slots < 3) {
        return;
    }
    // Each real frame's capture moves a frame's previous images a real frame
    // interval nearer its release: the ring holds (slots - 2) intervals
    // extrapolating, (slots - 1) interpolating, so the interval a refresh in
    // that many longer holds a lead more. One ceiling covers 1.5 to 2.5
    // refreshes per real frame at 2x: learned at 1.5 it capped the lead of
    // the same run slowed to 2.3, whose ring holds two refreshes more.
    const double more = 1.0 / static_cast<double>(slots - (jit_interpolate_ ? 1 : 2));
    if (dynamic_interval_slots_ < ring.interval + more) {
        return;
    }
    const std::uint32_t ceiling = ring.ceiling;
    ring.ceiling = 0;
    ring.trial.reset();
    ring.late_event.reset();
    ring.changed_at = now;
    trace_.Record("ringcap", multiplier, 0, lead_slots_);
    if (logger_ && logged_ring_moves_ < kLoggedRingMoves) {
        ++logged_ring_moves_;
        logger_->Info("Frame Generation drops the lead's ceiling of " + std::to_string(ceiling) + " at " +
                      std::to_string(multiplier) + "x: the game slowed from " + Number(ring.interval, 2) + " to " +
                      Number(dynamic_interval_slots_, 2) + " refreshes per real frame, and the history ring holds a " +
                      "lead more.");
    }
}

void FrameGenPresenter::ClampLeadLocked(std::uint32_t cap) {
    lead_slots_ = cap;
    lead_floor_ = std::min(lead_floor_, lead_slots_);
    lead_floor_proven_ = std::min(lead_floor_proven_, lead_slots_);
    lead_probe_.reset();
    ClearLeadHistoryLocked();
}

void FrameGenPresenter::NoteRingShortLocked(const JitWrite& write, Clock::time_point returned, XrDuration period) {
    ++totals_.ring_short;
    const std::uint32_t multiplier = std::max(2u, EffectiveMultiplierLocked());
    const auto found_after = write.ring_short_at - returned;
    AddSample(ring_short_write_ms_, Milliseconds(found_after));
    // Only the lead's doing: the run's own path with a deep ring, a frame
    // told the current lead at the current multiplier, and found so within
    // three quarters of a refresh of the presenter thread's wait returning
    // (later, the presenter thread itself was late: its acquire or output
    // wait, not the lead). Draining, frames are kept to their own refresh.
    const bool late_write = found_after > std::chrono::nanoseconds(period * 3 / 4);
    const bool counts = !compositor_drain_ && write.ring_short_composed == quad_mode_ &&
                        JitRingLocked(write.ring_short_composed) >= kFrameGenHistoryRingMax &&
                        write.ring_short_lead == lead_slots_ && write.ring_short_multiplier == multiplier &&
                        multiplier <= 4 && !late_write;
    if (!counts) {
        ++totals_.ring_short_ignored;
        totals_.ring_short_late += late_write ? 1 : 0;
        return;
    }
    const auto now = Clock::now();
    // Each real frame counts once; two at the same lead within event_window
    // lower its ceiling.
    if (lead_ring_event_ && lead_ring_event_->frame == write.ring_short_id) {
        return;
    }
    if (!lead_ring_event_ || lead_ring_event_->lead != write.ring_short_lead ||
        lead_ring_event_->multiplier != multiplier || now - lead_ring_event_->at > lead_policy_.event_window) {
        lead_ring_event_ = LeadRingEvent{now, write.ring_short_id, write.ring_short_lead, multiplier};
        return;
    }
    // While a probe or step down is judged the policy is bringing the lead
    // down itself: the second frame waits for its verdict (the next
    // ring-short frame after it lowers the ceiling). Lowered under it, the
    // probe was dropped unjudged, and with it the wait it would have reset:
    // a GPU-bound game's lead held a refresh higher for 1.4 s while its own
    // steps down went on to it (TestLeadCeilingGpuBoundTrade). Turned off,
    // too: ringWould counts the lowerings the ceiling would have made by its
    // own rules, comparable with ringMoves of a flight with it on.
    if (lead_probe_) {
        return;
    }
    const LeadRingEvent first = *lead_ring_event_;
    lead_ring_event_.reset();
    const std::uint32_t lead = lead_slots_;
    LeadRing& ring = lead_ring_[multiplier];
    // Never below the lead the ring holds at any capture timing, nor one
    // that frames late a refresh below it showed the game needs (for
    // explore_max). Only a lowering's own verdict sets that: a probe or step
    // down of the policy's that a hitch failed says nothing of the ring, and
    // held the ceiling at a lead the ring is short for (DCS on Virtual
    // Desktop, Pierpaolo 2026-10-20: lead 8 from hitches, three steps down
    // undone, 198 refreshes in 30 s finding their images replaced). Nor the
    // lowest lead the policy found to hold (lead_floor_proven_): a step down
    // judged good proves only that, and it held a lead the ring was short for
    // while the policy itself later went three refreshes lower
    // (TestLeadCeilingGpuBoundTrade).
    const bool needed = ring.needed && now - ring.needed_at < lead_policy_.explore_max;
    const std::uint32_t floor = std::max({RingLeadFloorLocked(multiplier), needed ? ring.needed : 0u, 1u});
    const std::uint32_t target = std::max(lead - 1, floor);
    if (!RingCeilingLocked()) {
        // What it would have done: a lowering for each lead it would have
        // reached, not one for every pair of frames at a lead it would have
        // left already (the lead stays here, its frames ring-short pair
        // after pair).
        if (target < lead && (!ring.would || lead <= ring.would)) {
            ring.would = target;
            ++totals_.ring_short_would;
        }
        return;
    }
    if (target >= lead) {
        ++totals_.lead_ring_held;
        if (logger_ && !logged_ring_held_) {
            logged_ring_held_ = true;
            logger_->Info("Frame Generation keeps a lead of " + std::to_string(lead) + " at " +
                          std::to_string(multiplier) +
                          "x though the history ring is a capture short there: frames were late a refresh below it, "
                          "or the ring holds that lead at any capture time.");
        }
        return;
    }
    // A raise undone within explore_after: the next waits twice as long.
    if (ring.raised && now - ring.changed_at < lead_policy_.explore_after) {
        ring.retry = std::min(ring.retry * 2, lead_policy_.explore_max);
    } else if (ring.raised) {
        ring.retry = lead_policy_.explore_after;
    }
    ring.raised = false;
    // The policy's own calm probe from this lead, already due (within
    // kRingProbeAhead): the lowering is that probe. Its verdict is the
    // policy's (late frames make this lead the lowest known to hold and the
    // next try below it waits explore_after, doubling; held, the lead below
    // is). Judged apart, its failure taught the policy nothing: the policy
    // then failed its own probe from the same lead, and each later probe
    // came that much later (a GPU-bound game, its captures replaced at 8 and
    // late at 7: the lowering came 8 ms after the policy's probe was due, the
    // policy came down from 8 0.35 s later than with the ceiling off, and 40
    // refreshes found their images replaced against 29:
    // TestLeadCeilingGpuBoundTrade). Any other lowering is a trial of its
    // own: undone, the policy's floors and calm time are put back.
    const auto calm_since = lead_calm_since_ == Clock::time_point{} ? now : lead_calm_since_;
    const bool as_probe =
        !lead_policy_.pinned_lead && lead > 1 && calm_since + CalmProbeWaitLocked() <= now + kRingProbeAhead;
    // Judged as a probe is, by hitch events (UpdateLeadLocked): two late
    // events at it put the ceiling before it back at once.
    ring.trial = now;
    ring.late_event.reset();
    ring.trial_from = lead;
    ring.trial_previous = ring.ceiling;
    ring.trial_calm = lead_calm_since_;
    ring.trial_floor = lead_floor_;
    ring.trial_proven = lead_floor_proven_;
    ring.trial_floor_failed = lead_floor_failed_;
    ring.ceiling = target;
    ring.changed_at = now;
    ring.interval = dynamic_interval_slots_;
    ++totals_.lead_ring_downs;
    ClampLeadLocked(target);
    if (as_probe) {
        // As the policy's probe from this lead leaves them: held, the lead
        // below is the lowest known to hold; undone, this one.
        lead_floor_ = ring.trial_floor;
        lead_floor_proven_ = ring.trial_proven;
        lead_probe_ = LeadProbe{now, lead};
        lead_probe_->ring = true;
    }
    lead_calm_since_ = now;
    trace_.Record("ringcap", multiplier, target, lead);
    if (logger_ && logged_ring_moves_ < kLoggedRingMoves) {
        ++logged_ring_moves_;
        logger_->Info("Frame Generation lowers the lead's ceiling at " + std::to_string(multiplier) + "x to " +
                      std::to_string(target) + " refreshes: real frames " + std::to_string(first.frame) + " and " +
                      std::to_string(write.ring_short_id) + ", told " + std::to_string(lead) +
                      ", found their previous images replaced in the " +
                      std::to_string(JitRingLocked(quad_mode_)) + "-frame history before their last moved refresh (" +
                      Number(std::chrono::duration<double>(now - first.at).count(), 2) +
                      " s apart); frames late at " + std::to_string(target) + " in two hitch events within " +
                      Number(std::chrono::duration<double>(lead_policy_.judge).count(), 1) + " s put " +
                      std::to_string(lead) + " back at once, later ones retry it after " +
                      Number(std::chrono::duration<double>(ring.retry).count(), 1) + " s.");
    }
}

void FrameGenPresenter::SetDynamicPolicy(const FrameGenDynamicPolicy& policy) {
    std::scoped_lock lock(mutex_);
    dynamic_policy_ = policy;
    dynamic_hold_ = policy.hold;
    auto_halving_hold_ = policy.hold;
    dynamic_probe_after_ = policy.probe_after;
}

void FrameGenPresenter::SetSummaryInterval(std::chrono::milliseconds interval) {
    std::scoped_lock lock(mutex_);
    summary_interval_ = interval;
}

void FrameGenPresenter::SetHalvingWindow(std::chrono::milliseconds window) {
    std::scoped_lock lock(mutex_);
    halving_window_ = window;
}

void FrameGenPresenter::SetQuadDecline(std::chrono::milliseconds first, std::chrono::milliseconds longest) {
    std::scoped_lock lock(mutex_);
    quad_decline_first_ = first;
    quad_decline_longest_ = std::max(first, longest);
    quad_decline_ = first;
}

void FrameGenPresenter::SetRuntimeViewConfiguration(XrViewConfigurationType type) {
    std::scoped_lock lock(mutex_);
    runtime_view_configuration_ = type;
}

void FrameGenPresenter::SetInterpolateOnShaders(bool on) {
    std::scoped_lock lock(mutex_);
    interpolate_on_shaders_ = on;
    logged_interpolate_on_shaders_ = false;
    logged_interpolate_on_hardware_ = false;
}

void FrameGenPresenter::SetInterpolateFlowPolicy(const FrameGenInterpolateFlowPolicy& policy) {
    std::scoped_lock lock(mutex_);
    interpolate_flow_policy_ = policy;
    interpolate_flow_policy_.window = std::max(policy.window, std::chrono::milliseconds(1));
    interpolate_flow_policy_.probe = std::max(policy.probe, std::chrono::milliseconds(1));
    interpolate_flow_policy_.shader_hold = std::max(policy.shader_hold, std::chrono::milliseconds(1));
    interpolate_flow_policy_.hold_max = std::max(policy.hold_max, interpolate_flow_policy_.shader_hold);
    interpolate_flow_policy_.bad_windows = std::max(1u, policy.bad_windows);
    interpolate_flow_backoff_ = interpolate_flow_policy_.shader_hold;
    interpolate_flow_window_.reset();
    interpolate_flow_bad_windows_ = 0;
}

Clock::time_point FrameGenPresenter::InterpolateFlowDelayUntilLocked(Clock::time_point now,
                                                                     FrameGenSynthesisMode mode,
                                                                     bool hardware_active) {
    if (!hardware_active || mode != FrameGenSynthesisMode::kInterpolate ||
        !interpolate_flow_delay_.count() || !interpolate_flow_delay_for_.count()) {
        return {};
    }
    // Driver/session setup can take seconds. It is not part of the
    // experiment, which starts only once hardware vectors exist to delay.
    if (!interpolate_flow_delay_started_) interpolate_flow_delay_started_ = now;
    return now - *interpolate_flow_delay_started_ < interpolate_flow_delay_for_ ? now + interpolate_flow_delay_
                                                                             : Clock::time_point{};
}

void FrameGenPresenter::ResetInterpolateFlowWindowLocked(Clock::time_point now) {
    interpolate_flow_window_ = InterpolateFlowWindow{now, totals_};
}

void FrameGenPresenter::SwitchInterpolateFlowLocked(bool hardware, Clock::time_point now,
                                                  const std::string& reason) {
    const bool changed = hardware != interpolate_flow_hardware_;
    const char* before = interpolate_flow_hardware_ ? "hardware" : "shader";
    interpolate_flow_hardware_ = hardware;
    interpolate_flow_reason_ = reason;
    if (changed) {
        ++interpolate_flow_switches_;
        // The next real frame uses this source. Existing analyzed frames
        // retain their source and are presented normally; no GPU reset.
        analysis_lag_ms_ = 0.0;
        if (logger_) {
            logger_->Info(std::string("Frame Generation Interpolate flow switch: ") + before + " -> " +
                          (hardware ? "hardware" : "shader") + " (" + reason + ").");
        }
    }
    interpolate_flow_bad_windows_ = 0;
    ResetInterpolateFlowWindowLocked(now);
}

bool FrameGenPresenter::UpdateInterpolateFlowLocked(Clock::time_point now, FrameGenSynthesisMode mode,
                                                   bool hardware_selected) {
    if (mode != FrameGenSynthesisMode::kInterpolate) {
        // Another synthesis mode has a different deadline and benefit. Its
        // counters and elapsed time must not judge an Interpolate probe.
        interpolate_flow_selected_.reset();
        interpolate_flow_window_.reset();
        interpolate_flow_bad_windows_ = 0;
        interpolate_flow_probing_ = false;
        interpolate_flow_probe_due_ = {};
        interpolate_flow_backoff_ = interpolate_flow_policy_.shader_hold;
        return hardware_selected;
    }
    const FrameGenInterpolateFlow selected =
        interpolate_on_shaders_ && settings_.multiplier == FrameGenMultiplier::kDynamic
            ? FrameGenInterpolateFlow::kShader
            : interpolate_flow_env_.value_or(settings_.interpolate_flow);
    // Never perturb a multiplier probe, including a live user selection.
    // Its measurements must all use one motion source. Afterwards the next
    // real frame applies the setting and starts a fresh evidence window.
    if (probing_ || dynamic_probing_) {
        interpolate_flow_window_.reset();
        interpolate_flow_bad_windows_ = 0;
        return interpolate_flow_selected_ ? interpolate_flow_hardware_ : hardware_selected;
    }
    const bool available = !interpolate_flow_available_ || *interpolate_flow_available_;
    const bool changed = !interpolate_flow_selected_ || *interpolate_flow_selected_ != selected ||
                         interpolate_flow_hardware_selected_ != hardware_selected;
    if (changed) {
        interpolate_flow_selected_ = selected;
        interpolate_flow_hardware_selected_ = hardware_selected;
        interpolate_flow_probing_ = false;
        interpolate_flow_backoff_ = interpolate_flow_policy_.shader_hold;
        interpolate_flow_probe_due_ = {};
        const bool hardware = hardware_selected && selected != FrameGenInterpolateFlow::kShader && available;
        SwitchInterpolateFlowLocked(hardware, now,
                                    !hardware_selected ? "shader motion estimation selected"
                                    : !available ? "hardware unavailable; shader flow"
                                    : selected == FrameGenInterpolateFlow::kShader ? "shader selected"
                                    : selected == FrameGenInterpolateFlow::kHardware ? "hardware selected"
                                                                                  : "auto starts on hardware");
    }
    if (!hardware_selected || selected == FrameGenInterpolateFlow::kShader) return false;
    if (!available) {
        interpolate_flow_probing_ = false;
        SwitchInterpolateFlowLocked(false, now, "hardware unavailable; shader flow");
        return false;
    }
    if (selected == FrameGenInterpolateFlow::kHardware) {
        SwitchInterpolateFlowLocked(true, now, "hardware selected");
        return true;
    }
    // A request on an unsupported GPU must not be reported as hardware or
    // start judging an automatic window: the backend answers after Execute.
    if (!interpolate_flow_available_) return interpolate_flow_hardware_;
    const auto& policy = interpolate_flow_policy_;
    if (!interpolate_flow_hardware_) {
        if (interpolate_flow_probe_due_ != Clock::time_point{} && now >= interpolate_flow_probe_due_) {
            interpolate_flow_probing_ = true;
            interpolate_flow_probe_started_ = now;
            ++interpolate_flow_probes_;
            SwitchInterpolateFlowLocked(true, now, "auto hardware probe after shader hold");
        }
        return interpolate_flow_hardware_;
    }
    if (!interpolate_flow_window_) ResetInterpolateFlowWindowLocked(now);
    const auto& start = *interpolate_flow_window_;
    // Pauses, loading/non-rendered refreshes and presenter restarts are not
    // hardware-flow evidence. Do not count a hitch window across them.
    if (totals_.pauses != start.totals.pauses || totals_.non_renderable != start.totals.non_renderable ||
        totals_.unrendered != start.totals.unrendered || totals_.restarts != start.totals.restarts) {
        interpolate_flow_bad_windows_ = 0;
        if (interpolate_flow_probing_) interpolate_flow_probe_started_ = now;
        ResetInterpolateFlowWindowLocked(now);
        return true;
    }
    const auto length = interpolate_flow_probing_ ? policy.probe : policy.window;
    if (now - start.start < length) return true;
    const double seconds = std::chrono::duration<double>(now - start.start).count();
    const auto delta = [&](std::uint64_t Totals::*field) { return totals_.*field - start.totals.*field; };
    const auto generated = delta(&Totals::generated_distinct);
    const auto stale = delta(&Totals::stale_motion);
    const auto motion_outputs = delta(&Totals::motion_outputs);
    // Both parts of the summary's unready=a/b matter: a real frame not
    // captured yet, and a moved frame shown as a copy without ready motion.
    const auto unready = delta(&Totals::unready) + delta(&Totals::unmeasured);
    const auto timed = delta(&Totals::hardware_timed);
    const auto late = delta(&Totals::hardware_late);
    const double lag_ms = static_cast<double>(delta(&Totals::hardware_lag_ns)) / 1e6;
    const bool stale_bad = motion_outputs >= policy.min_generated &&
                           static_cast<double>(stale) > policy.stale_share * motion_outputs;
    const bool unready_bad = seconds > 0.0 && static_cast<double>(unready) * 30.0 > policy.unready_per_30s * seconds;
    const bool hardware_bad = timed >= policy.min_hardware_samples &&
                              static_cast<double>(late) > policy.hardware_late_share * timed;
    const bool evidence = generated || stale || unready || timed;
    const bool bad = stale_bad || unready_bad || hardware_bad;
    const std::string why = stale_bad ? "stale motion " +
                                           Number(100.0 * stale / std::max<std::uint64_t>(1, motion_outputs), 1) +
                                           "% of motion outputs (" + std::to_string(stale) + "/" +
                                           std::to_string(motion_outputs) + ")"
                            : unready_bad ? "unready " + Number(30.0 * unready / seconds, 1) + " per 30 s"
                            : "hardware late " + Number(100.0 * late / std::max<std::uint64_t>(1, timed), 1) +
                                  "% (" + Number(late ? lag_ms / late : 0.0, 1) + " ms past first in-between deadline)";
    if (!evidence) {
        // No generated work is not a successful probe. Wait for a complete
        // active window rather than declaring recovery during a pause.
        interpolate_flow_bad_windows_ = 0;
        ResetInterpolateFlowWindowLocked(now);
        return true;
    }
    interpolate_flow_bad_windows_ = bad ? interpolate_flow_bad_windows_ + 1 : 0;
    if (bad && (interpolate_flow_probing_ || interpolate_flow_bad_windows_ >= policy.bad_windows)) {
        const bool failed_probe = interpolate_flow_probing_;
        interpolate_flow_probing_ = false;
        ++interpolate_flow_fallbacks_;
        if (failed_probe) {
            interpolate_flow_backoff_ = std::min(interpolate_flow_backoff_ * 2, policy.hold_max);
        }
        interpolate_flow_probe_due_ = now + interpolate_flow_backoff_;
        SwitchInterpolateFlowLocked(false, now,
                                    std::string(failed_probe ? "auto hardware probe failed: " : "auto fallback: ") +
                                        why + "; shader hold " +
                                        Number(std::chrono::duration<double>(interpolate_flow_backoff_).count(), 0) + " s");
        return false;
    }
    const bool enough_to_recover = generated >= policy.min_generated || timed >= policy.min_hardware_samples;
    if (interpolate_flow_probing_ && !bad && enough_to_recover) {
        interpolate_flow_probing_ = false;
        interpolate_flow_backoff_ = policy.shader_hold;
        interpolate_flow_reason_ = "auto hardware probe recovered; healthy window";
        if (logger_) logger_->Info("Frame Generation Interpolate flow probe recovered: hardware (healthy window).");
    }
    ResetInterpolateFlowWindowLocked(now);
    return true;
}

void FrameGenPresenter::SetEngagementPolicy(std::uint32_t steady_frames, std::chrono::milliseconds minimum_session_age) {
    std::scoped_lock lock(mutex_);
    engage_streak_frames_ = steady_frames;
    engage_min_age_ = minimum_session_age;
}

void FrameGenPresenter::SetMemoryRecoveryDelay(std::chrono::milliseconds delay) {
    std::scoped_lock lock(mutex_);
    memory_recovery_delay_ = delay;
}

void FrameGenPresenter::SetRetryPolicy(std::chrono::milliseconds delay, std::uint32_t attempts,
                                       std::chrono::milliseconds restore_after) {
    std::scoped_lock lock(mutex_);
    retry_delay_ = delay;
    retry_attempts_ = attempts;
    retry_restore_after_ = restore_after;
}

bool FrameGenPresenter::Toggle() {
    std::scoped_lock lock(mutex_);
    if (state_ == FrameGenState::kPaused) {
        // A press after a pause retries rather than turning the feature off.
        // The time it stayed paused is no quiet stretch: the retries come
        // back only once it has run that long again (MaybeRetryLocked).
        user_enabled_ = true;
        consecutive_failures_ = consecutive_demotions_ = 0;
        paused_at_.reset();
        last_pause_at_ = Clock::now();
        DynamicReenableLocked(Clock::now());
        SetStateLocked(configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
    } else {
        user_enabled_ = !user_enabled_;
        if (user_enabled_) {
            // Turned back on: Auto gets a fresh look, not the rest of a
            // full-rate hold from before, and Dynamic judges the game's rate
            // again before engaging.
            inline_hold_until_.reset();
            auto_halving_hold_until_.reset();
            DynamicReenableLocked(Clock::now());
        }
        if (state_ != FrameGenState::kUnavailable) {
            SetStateLocked(user_enabled_ && configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
        }
    }
    healthy_streak_ = 0;
    return user_enabled_;
}

bool FrameGenPresenter::ResetFrames() {
    std::unique_lock lock(mutex_);
    // The session is starting or stopping: the presenter thread must not
    // touch the runtime frame loop past this point. One still inside a
    // runtime call is left stopping (the runtime may hold it until the
    // session ends: the layer ends it, then stops the thread again).
    if (!abandoned_ && !StopCompositorLocked(lock, TeardownWait())) {
        return false;
    }
    dropped_unended_ = dropped_superseded_ = 0;
    dropped_unbegun_ = 0;
    // A new session starts with no runtime frame in flight.
    runtime_waits_ = runtime_begins_ = runtime_ends_ = 0;
    presenter_frame_open_ = false;
    frames_.clear();
    healthy_streak_ = 0;
    interpolate_flow_selected_.reset();
    interpolate_flow_window_.reset();
    interpolate_flow_bad_windows_ = 0;
    interpolate_flow_probing_ = false;
    interpolate_flow_probe_due_ = {};
    interpolate_flow_backoff_ = interpolate_flow_policy_.shader_hold;
    InvalidateContinuityLocked();
    if (state_ == FrameGenState::kActive) {
        SetStateLocked(IdleStateLocked(), "");
    }
    idle_cv_.notify_all();
    return true;
}

void FrameGenPresenter::SetTeardownWait(std::chrono::milliseconds wait) {
    teardown_wait_ms_ = std::max<std::int64_t>(wait.count(), 1);
}

std::chrono::milliseconds FrameGenPresenter::TeardownWait() const {
    return std::chrono::milliseconds(teardown_wait_ms_.load());
}

bool FrameGenPresenter::StopPresenter() {
    std::unique_lock lock(mutex_);
    return abandoned_ || StopCompositorLocked(lock, TeardownWait());
}

void FrameGenPresenter::AbandonPresenter() {
    std::scoped_lock lock(mutex_);
    AbandonPresenterLocked();
}

void FrameGenPresenter::AbandonPresenterLocked() {
    if (!compositor_running_ || !compositor_.joinable()) {
        return;
    }
    // It ends when its call returns (compositor_stop_ stays set); until then
    // it holds what its call uses, so nothing of the session is let go.
    compositor_stop_ = true;
    compositor_.detach();
    abandoned_ = true;
    if (logger_) {
        logger_->Error("Frame Generation leaves its presenter thread behind: it never left the runtime (" +
                       PresenterActivity() +
                       "). Its GPU work, output images and the runtime's session are kept for the process's exit, "
                       "and Frame Generation stays off for the rest of the process.");
    }
}

bool FrameGenPresenter::PresenterAbandoned() const {
    return abandoned_;
}

std::string FrameGenPresenter::PresenterActivity() const {
    std::string text = std::string("presenter thread ") + compositor_phase_.load();
    if (const char* call = presenter_call_.load()) {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        const double seconds = static_cast<double>(now - presenter_call_since_.load()) / 1e9;
        text += std::string(", in the runtime's ") + call + " for " + Number(seconds, 1) + " s";
    }
    if (abandoned_) {
        text += ", left behind";
    }
    return text;
}

bool FrameGenPresenter::LockForTeardown(std::unique_lock<std::timed_mutex>& lock, const std::string& what) {
    // The presenter thread holds it over runtime calls: a runtime holding
    // one must not hold this caller too. While one call has held it past
    // the teardown wait already, later callers only try briefly.
    const auto limit = PresenterWedged() ? std::chrono::milliseconds(50) : TeardownWait();
    if (lock.try_lock_for(limit)) {
        return true;
    }
    if (logger_) {
        logger_->Error("Frame Generation: " + what + " waited " + std::to_string(limit.count()) +
                       " ms for its presenter thread (" + PresenterActivity() + ") and went on without it.");
    }
    return false;
}

bool FrameGenPresenter::PresenterWedged() const {
    if (!presenter_call_.load()) {
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    return now - presenter_call_since_.load() >
           std::chrono::duration_cast<std::chrono::nanoseconds>(TeardownWait()).count();
}

FrameGenPresenter::PresenterCall::PresenterCall(FrameGenPresenter& owner, const char* call) {
    if (GetCurrentThreadId() != owner.compositor_thread_id_.load()) {
        return;
    }
    presenter = &owner;
    owner.presenter_call_since_ =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    owner.presenter_call_ = call;
}

FrameGenPresenter::PresenterCall::~PresenterCall() {
    if (presenter) {
        presenter->presenter_call_ = nullptr;
    }
}

void FrameGenPresenter::AbandonFrame(XrTime display_time) {
    {
        std::scoped_lock lock(mutex_);
        // The oldest open frame is the one ending, as in EndFrame; the time
        // is only a hint (apps may end with another time than they were given).
        (void)display_time;
        if (quad_claim_id_) {
            // Claimed in this xrEndFrame (ClaimQuadFrame), which the layer
            // then returned from: that frame's end never comes.
            const std::uint64_t id = *quad_claim_id_;
            quad_claim_id_.reset();
            threaded_frames_.erase(std::remove_if(threaded_frames_.begin(), threaded_frames_.end(),
                                                  [&](const ThreadedFrame& frame) { return frame.id == id; }),
                                   threaded_frames_.end());
        } else if (dropped_unended_ > 0) {
            --dropped_unended_;
            dropped_superseded_ = std::min(dropped_superseded_, dropped_unended_);
        } else if (!frames_.empty()) {
            frames_.pop_front();
        } else {
            const auto it = std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                         [](const ThreadedFrame& frame) { return !frame.claimed; });
            if (it != threaded_frames_.end()) {
                threaded_frames_.erase(it);
            }
        }
        healthy_streak_ = 0;
        InvalidateContinuityLocked();
    }
    idle_cv_.notify_all();
    WakeSlots();
}

XrSwapchainUsageFlags FrameGenPresenter::SourceUsage(const XrSwapchainCreateInfo& info) const {
    std::scoped_lock lock(mutex_);
    if (!started_ || !gpu_) {
        return 0;
    }
    if ((info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        return gpu_->SupportsDepth() ? gpu_->RequiredDepthUsage() : 0;
    }
    return gpu_->RequiredSourceUsage();
}

void FrameGenPresenter::TrackSwapchain(XrSwapchain swapchain, std::int64_t format, std::uint32_t sample_count,
                                       XrSwapchainUsageFlags usage) {
    std::scoped_lock lock(mutex_);
    if (!started_) {
        return;
    }
    const XrSwapchainUsageFlags required = gpu_ ? gpu_->RequiredSourceUsage() : 0;
    SwapchainRecord record;
    record.format = format;
    record.sample_count = std::max(1u, sample_count);
    record.depth = (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    record.capturable = !record.depth && (usage & required) == required;
    const XrSwapchainUsageFlags depth_required = gpu_ ? gpu_->RequiredDepthUsage() : 0;
    record.depth_capturable = record.depth && gpu_ && gpu_->SupportsDepth() && record.sample_count == 1 &&
                              (usage & depth_required) == depth_required;
    swapchains_[swapchain] = record;
    // A game's vectors settle after it made a swapchain (G8).
    swapchain_created_at_ = Clock::now();
    swapchain_created_frame_ = totals_.app_frames;
}

bool FrameGenPresenter::ForgetSwapchain(XrSwapchain swapchain) {
    // Holding submit_mutex_ keeps the presenter thread from submitting the
    // swapchain while it goes; afterwards no copy refers to it. A presenter
    // thread inside a runtime call past the teardown wait holds it: the
    // swapchain is forgotten all the same (no later submission names it) and
    // kept (false), as the submission in flight may name it.
    std::unique_lock submit(submit_mutex_, std::defer_lock);
    const bool submit_held = LockForTeardown(submit, "a swapchain being destroyed");
    {
        std::scoped_lock lock(mutex_);
        swapchains_.erase(swapchain);
        capture_targets_.erase(std::remove_if(capture_targets_.begin(), capture_targets_.end(),
                                              [&](const CaptureTarget& target) { return target.swapchain == swapchain; }),
                               capture_targets_.end());
        for (QuadCapture& capture : quad_captures_) {
            if (capture.swapchain == swapchain) capture = {};
        }
        // No census reads a swapchain that is gone.
        bool named = false;
        for (const EngineFrame* frame : {&engine_frame_, &engine_previous_}) {
            for (const FrameGenEngineMotionView& view : frame->frame.views) {
                named = named || view.vectors.swapchain == swapchain || view.depth.swapchain == swapchain ||
                        view.color.swapchain == swapchain;
            }
        }
        if (named) {
            ForgetEngineFramesLocked();
        }
        StripSwapchain(newest_copy_, swapchain);
        StripSwapchain(passthrough_copy_, swapchain);
        StripSwapchain(jit_content_, swapchain);
        for (JitFrame& frame : jit_frames_) {
            StripSwapchain(frame.copy, swapchain);
        }
    }
    std::unique_lock gpu_lock(gpu_mutex_, std::defer_lock);
    if (!LockForTeardown(gpu_lock, "a swapchain being destroyed")) {
        // The backend may still use its images: kept.
        return false;
    }
    bool idle = !gpu_stalled_;
    if (gpu_ && idle) {
        // A recent submission may still read or write this swapchain's
        // images; destroys are rare, so always let it finish first.
        idle = gpu_->WaitIdle() || gpu_->WaitIdle() || gpu_->WaitIdle();
    }
    if (gpu_) {
        gpu_->Forget(swapchain);
    }
    if (!idle && logger_) {
        logger_->Error("Frame Generation: the GPU did not finish its work on a swapchain being destroyed within 6 s.");
    }
    return idle && submit_held;
}

bool FrameGenPresenter::ForgetSpace(XrSpace space) {
    // As for swapchains: under submit_mutex_, no presenter-thread submission
    // is using the space, and none will after this. Without it (a presenter
    // thread inside a runtime call past the teardown wait) the space is
    // forgotten all the same and kept (false): the submission in flight may
    // name it.
    std::unique_lock submit(submit_mutex_, std::defer_lock);
    const bool submit_held = LockForTeardown(submit, "a space being destroyed");
    std::scoped_lock lock(mutex_);
    StripSpace(newest_copy_, space);
    StripSpace(passthrough_copy_, space);
    StripSpace(jit_content_, space);
    // Frames generated from a real frame share its projection's space.
    for (auto it = jit_frames_.begin(); it != jit_frames_.end();) {
        if (it->copy.has_projection && it->copy.layers[it->copy.projection_index].projection.space == space) {
            ScoreErasedLocked(*it);
            it = jit_frames_.erase(it);
        } else {
            ++it;
        }
    }
    for (JitFrame& frame : jit_frames_) {
        StripSpace(frame.copy, space); // other layers in that space
    }
    if (jit_content_.has_projection && jit_content_.layers[jit_content_.projection_index].projection.space == space) {
        jit_content_ = {};
        jit_views_.clear();
        jit_shown_as_is_ = false;
    }
    if (extrapolated_.space == space) {
        extrapolated_.valid = false;
        generated_ready_ = false;
    }
    if (last_real_.space == space) {
        InvalidateContinuityLocked();
    }
    return submit_held;
}

void FrameGenPresenter::NoteAcquire(XrSwapchain swapchain, std::uint32_t image_index) {
    std::scoped_lock lock(mutex_);
    if (const auto it = swapchains_.find(swapchain); it != swapchains_.end() && image_index < 32) {
        it->second.acquired |= 1u << image_index;
    }
}

void FrameGenPresenter::NoteRelease(XrSwapchain swapchain, std::uint32_t image_index) {
    std::scoped_lock lock(mutex_);
    if (const auto it = swapchains_.find(swapchain); it != swapchains_.end()) {
        it->second.released = image_index;
        ++it->second.releases;
        if (image_index < 32) it->second.acquired &= ~(1u << image_index);
    }
}

std::optional<FrameGenOutputKind> FrameGenPresenter::OutputKindLocked(std::size_t view, std::int64_t source_format) const {
    const auto requested = gpu_ ? gpu_->Classify(source_format) : std::nullopt;
    // Composing quad views, the four views' history never depends on the two
    // eyes' output images: the backend's own images take the composed eyes'.
    if (quad_mode_) {
        return requested;
    }
    if (requested && view < output_kinds_.size() && output_kinds_[view].valid &&
        output_kinds_[view].requested == *requested) {
        return output_kinds_[view].kind;
    }
    return requested;
}

bool FrameGenPresenter::CapturingLocked() const {
    if (!started_ || !gpu_ || !user_enabled_ || !configured_enabled_ || state_ == FrameGenState::kPaused ||
        state_ == FrameGenState::kUnavailable) {
        return false;
    }
    // Only while frames are generated or about to be: the wait that engages
    // comes before the game's release, so the first engaged frame is
    // captured. Passing frames straight through (Auto at full rate, Dynamic
    // standing by, waiting for steady frames) captures nothing.
    return compositor_running_ || WantsEngagementLocked() ||
           std::any_of(frames_.begin(), frames_.end(),
                       [](const PendingFrame& frame) { return frame.engaged && !frame.demoted; });
}

bool FrameGenPresenter::TracesSwapchainCalls() const {
    if (!trace_.Enabled()) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    return CapturingLocked();
}

void FrameGenPresenter::CaptureOnRelease(XrSwapchain swapchain, std::uint32_t image_index) {
    auto capturing = [&] {
        if (!CapturingLocked()) {
            return false;
        }
        const auto record = swapchains_.find(swapchain);
        if (record == swapchains_.end() || record->second.sample_count != 1) {
            return false;
        }
        auto named = [&](const std::vector<CaptureTarget>& targets) {
            return std::any_of(targets.begin(), targets.end(),
                               [&](const CaptureTarget& target) { return target.swapchain == swapchain; });
        };
        // A frame's motion vectors (never a color image, whatever its
        // format): captured as a motion target only (through the Vulkan
        // bridge, not while the game's swapchains settle).
        if (record->second.motion) {
            return named(motion_targets_) &&
                   (engine_bridge_ != FrameGenEngineBridge::kVulkan || EngineBridgeSettledLocked());
        }
        return (record->second.depth ? record->second.depth_capturable : record->second.capturable) &&
               named(capture_targets_);
    };
    {
        // Most releases have nothing to capture; check before the queue lock.
        std::scoped_lock lock(mutex_);
        if (!capturing()) {
            return;
        }
    }
    // Lock order everywhere: submit, queue, GPU, presenter. The copy may wait
    // on a fence, so it runs without the presenter lock.
    std::unique_lock<std::mutex> queue_lock;
    if (queue_mutex_) {
        queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
    }
    std::scoped_lock gpu_lock(gpu_mutex_);
    struct Job {
        std::size_t view{0};
        FrameGenImage source;
        FrameGenViewRequest request;
        bool depth{false};
        FrameGenDepthEncoding encoding;
        bool ok{false}; // submitted
        // A depth image whose view's motion vectors the last frame named:
        // their early census once the depth is captured.
        std::optional<FrameGenMotionJob> motion;
    };
    std::vector<Job> jobs;
    // A frame's motion vectors at their release (stage 2), with each view's
    // color request.
    std::vector<FrameGenMotionJob> motion;
    std::vector<FrameGenViewRequest> motion_color;
    FrameGenFlowQuality quality;
    std::uint64_t releases = 0; // the swapchain's releases the jobs capture
    {
        std::scoped_lock lock(mutex_);
        if (!capturing()) {
            return;
        }
        quality = settings_.flow_quality;
        const auto record = swapchains_.find(swapchain);
        releases = record->second.releases;
        for (const CaptureTarget& target : capture_targets_) {
            if (target.swapchain != swapchain) {
                continue;
            }
            if (target.depth) {
                Job job;
                job.view = target.view;
                job.depth = true;
                job.encoding = target.encoding;
                job.source = {swapchain, image_index, target.array_index, target.rect, record->second.format};
                job.request.extent = target.color_extent;
                // (Not through the Vulkan bridge: its runtime images hold the
                // vectors only behind its copies.)
                if (engine_requested_ == FrameGenEngineMode::kCensus && engine_worker_ && engine_frame_.valid &&
                    engine_bridge_ == FrameGenEngineBridge::kNone) {
                    std::vector<FrameGenMotionJob> motion;
                    std::vector<FrameGenViewRequest> color;
                    EngineJobsLocked(engine_frame_, motion, color);
                    for (std::size_t i = 0; i < motion.size(); ++i) {
                        FrameGenMotionJob& early = motion[i];
                        if (early.view != target.view) {
                            continue;
                        }
                        // The frame being ended next, as far as its depth
                        // (and its color, if released by now): census only.
                        early.early = true;
                        early.samples = false;
                        early.frame = engine_frame_.frame.projection_frame + 1;
                        early.depth_index = image_index;
                        const auto color_record = swapchains_.find(engine_frame_.frame.views[i].color.swapchain);
                        if (color_record != swapchains_.end() && color_record->second.released) {
                            early.color_index = *color_record->second.released;
                        }
                        job.motion = early;
                    }
                }
                jobs.push_back(job);
                continue;
            }
            const auto kind = OutputKindLocked(target.view, record->second.format);
            if (!kind) {
                continue;
            }
            Job job;
            job.view = target.view;
            job.source = {swapchain, image_index, target.array_index, target.rect, record->second.format};
            job.request.extent = {static_cast<std::uint32_t>(target.rect.extent.width),
                                  static_cast<std::uint32_t>(target.rect.extent.height)};
            job.request.output = *kind;
            jobs.push_back(job);
        }
        // Stage 2: a view's motion vectors released (the specification's
        // way), captured as released and paired with the view's color of
        // this frame: captured already when released before them, else the
        // next one (`pair_next`). xrEndFrame captures the views not captured
        // here (CaptureEngineMotion).
        if (!motion_targets_.empty() && engine_frame_.valid) {
            std::vector<FrameGenMotionJob> all;
            std::vector<FrameGenViewRequest> color;
            EngineJobsLocked(engine_frame_, all, color);
            for (const CaptureTarget& target : motion_targets_) {
                for (std::size_t i = 0; i < all.size() && target.swapchain == swapchain; ++i) {
                    FrameGenMotionJob job = all[i];
                    if (job.view != target.view) {
                        continue;
                    }
                    job.vectors.image_index = image_index;
                    job.released = image_index;
                    job.frame = engine_frame_.frame.projection_frame + 1;
                    job.census_all = job.samples = false;
                    job.capture = true;
                    const FrameGenEngineMotionView& view = engine_frame_.frame.views[i];
                    const auto color_record = swapchains_.find(view.color.swapchain);
                    const bool color_released = color_record != swapchains_.end() && i < engine_frame_.color_releases.size() &&
                                                color_record->second.releases > engine_frame_.color_releases[i];
                    job.pair_next = !color_released;
                    if (color_released && color_record->second.released) {
                        job.color_index = *color_record->second.released;
                    }
                    if (const auto depth = swapchains_.find(view.depth.swapchain);
                        depth != swapchains_.end() && depth->second.released) {
                        job.depth_index = *depth->second.released;
                    }
                    motion.push_back(job);
                    motion_color.resize(std::max(motion_color.size(), job.view + 1));
                    motion_color[job.view] = color[job.view];
                }
            }
        }
    }
    std::uint64_t captured = 0;
    bool color_captured = false;
    std::string failure;
    for (Job& job : jobs) {
        std::string error;
        if (!gpu_->Track(swapchain, xr_.images, error) ||
            !(job.depth ? gpu_->CaptureDepth(job.view, job.source, job.encoding, job.request, error)
                        : gpu_->Capture(job.view, job.source, job.request, quality, error))) {
            // Generation then copies the image itself. An empty error means
            // the view is not set up for this region yet; nothing to report.
            if (failure.empty()) {
                failure = error;
            }
            continue;
        }
        job.ok = true;
        ++captured;
        color_captured = color_captured || !job.depth;
    }
    // The early census: whether the motion vectors are written by the time
    // the frame's depth is released (before its xrEndFrame).
    std::vector<FrameGenMotionJob> early;
    std::vector<FrameGenViewRequest> early_color;
    for (const Job& job : jobs) {
        if (job.ok && job.motion) {
            early.push_back(*job.motion);
            early_color.resize(std::max(early_color.size(), job.view + 1));
            early_color[job.view] = job.request;
        }
    }
    bool census = false;
    if (!early.empty()) {
        std::string error;
        bool tracked = true;
        for (const FrameGenMotionJob& job : early) {
            tracked = tracked && gpu_->Track(job.vectors.swapchain, xr_.images, error);
        }
        census = tracked && gpu_->CaptureMotion(early, early_color, error);
    }
    bool motion_captured = false;
    std::vector<bool> resampled; // per motion job
    if (!motion.empty()) {
        std::string error;
        motion_captured =
            gpu_->Track(swapchain, xr_.images, error) && gpu_->CaptureMotion(motion, motion_color, error, &resampled);
    }
    // Still under the GPU lock: what the game's waits are held for.
    const std::uint64_t mark = captured || census || motion_captured ? gpu_->CaptureMark() : 0;
    trace_.Record("cap", reinterpret_cast<std::uint64_t>(swapchain), image_index, static_cast<std::int64_t>(captured),
                  static_cast<std::int64_t>(jobs.size()), static_cast<std::int64_t>(releases),
                  static_cast<std::int64_t>(mark));
    std::scoped_lock lock(mutex_);
    captured_ += captured;
    totals_.captured += captured;
    last_capture_mark_ = std::max(last_capture_mark_, mark);
    if (color_captured) {
        if (const auto record = swapchains_.find(swapchain); record != swapchains_.end()) {
            record->second.captured_releases = releases;
        }
    }
    // Each view's latest capture, as Execute matches it (ClaimQuadFrame).
    for (const Job& job : jobs) {
        if (!job.depth && job.view < quad_captures_.size()) {
            quad_captures_[job.view] = {job.ok, swapchain, job.source.image_index, job.source.array_index,
                                        job.source.rect, releases};
        }
        // ...and the encoding each view's depth was decoded with: the frame
        // before's description (its capture target), which Generate checks
        // against the frame's own.
        if (job.depth && job.ok) {
            depth_decoded_.resize(std::max(depth_decoded_.size(), job.view + 1));
            depth_decoded_[job.view] = job.encoding;
        }
    }
    // ...and the views whose motion vectors were captured at their release
    // (resampled: a view the backend left out is tried again at the frame's
    // end): the frame's end leaves them be.
    for (std::size_t k = 0; k < motion.size() && motion_captured; ++k) {
        if (k < resampled.size() && resampled[k]) {
            engine_released_.resize(std::max(engine_released_.size(), motion[k].view + 1));
            engine_released_[motion[k].view] = 1;
        }
    }
    if (logger_ && !failure.empty() && !logged_capture_failure_) {
        logged_capture_failure_ = true;
        logger_->Error("Frame Generation could not capture a released image: " + failure);
    }
}

bool FrameGenPresenter::EngineBridgeSettledLocked() const {
    return !swapchain_created_at_ ||
           std::chrono::duration<double>(Clock::now() - *swapchain_created_at_).count() >= kFrameGenEngineSettleSeconds;
}

bool FrameGenPresenter::NoteEngineMotion(FrameGenEngineMotionFrame frame, FrameGenEngineBridge bridge) {
    std::shared_ptr<FrameGenEngineCensusWorker> worker;
    FrameGenEngineCensusWorker::Frame noted;
    bool read = false;
    {
        std::scoped_lock lock(mutex_);
        ++engine_noted_;
        engine_invalid_ += frame.invalid;
        engine_bridge_ = bridge;
        // Through the Direct3D 11 bridge the game's depth stays its own, and
        // vectors are never read without it; through the Vulkan bridge none
        // are with VECTORXR_FRAMEGEN_ENGINE_BRIDGE=0.
        const bool vulkan = bridge == FrameGenEngineBridge::kVulkan;
        if (bridge == FrameGenEngineBridge::kD3D11 || (vulkan && engine_bridge_mode_ == FrameGenEngineBridgeMode::kOff)) {
            ForgetEngineFramesLocked();
            if (engine_worker_ && engine_requested_ != FrameGenEngineMode::kOff && logger_ && !logged_engine_bridged_) {
                logged_engine_bridged_ = true;
                logger_->Info(vulkan ? "Frame Generation engine motion: none in this Vulkan-bridged session "
                                       "(VECTORXR_FRAMEGEN_ENGINE_BRIDGE=0)."
                                     : "Frame Generation engine motion: none in this Direct3D 11-bridged session (the "
                                       "game's depth stays its own there).");
            }
            return false;
        }
        // Through the Vulkan bridge, from 3 s after the game last made a
        // swapchain: Star Citizen churns them at its world entry, and nothing
        // new is asked of its queue meanwhile (no hand-backs, no captures).
        const bool settled = !vulkan || EngineBridgeSettledLocked();
        EngineFrame record;
        record.valid = true;
        const std::size_t views = frame.views.size();
        record.color_index.assign(views, 0);
        record.depth_index.assign(views, 0);
        record.color_releases.assign(views, 0);
        record.vector_releases.assign(views, 0);
        record.released.assign(views, std::nullopt);
        record.captured.assign(views, 0);
        for (std::size_t i = 0; i < views; ++i) {
            const FrameGenEngineMotionView& view = frame.views[i];
            if (const auto color = swapchains_.find(view.color.swapchain); color != swapchains_.end()) {
                record.color_index[i] = color->second.released.value_or(0);
                record.color_releases[i] = color->second.releases;
            }
            if (const auto depth = swapchains_.find(view.depth.swapchain); depth != swapchains_.end()) {
                record.depth_index[i] = depth->second.released.value_or(0);
            }
            if (const auto vectors = swapchains_.find(view.vectors.swapchain); vectors != swapchains_.end()) {
                vectors->second.motion = true;
                record.vector_releases[i] = vectors->second.releases;
                // A game following the specification released the image
                // since the frame before.
                if (engine_frame_.valid && i < engine_frame_.vector_releases.size() &&
                    i < engine_frame_.frame.views.size() &&
                    engine_frame_.frame.views[i].vectors.swapchain == view.vectors.swapchain &&
                    vectors->second.releases > engine_frame_.vector_releases[i]) {
                    record.released[i] = vectors->second.released;
                }
            }
        }
        // G6: a camera cut moves the application's space further, or turns
        // it more, than its own movement does (appSpaceDeltaPose, the same
        // for every view: the first's). Nothing measured across it is the
        // game's motion; with VECTORXR_FRAMEGEN_ENGINE_CUTS=1 nothing is
        // generated across it (a runtime's own space warp synthesizes nothing
        // across one either).
        if (!frame.views.empty()) {
            const XrPosef& delta = frame.views.front().app_delta;
            record.cut = engine_cut_window_.Add(
                {delta.orientation.x, delta.orientation.y, delta.orientation.z, delta.orientation.w},
                {delta.position.x, delta.position.y, delta.position.z});
            if (record.cut) {
                ++engine_window_.cuts;
            }
            if (record.cut && engine_cuts_) {
                InvalidateContinuityLocked();
                if (logger_ && !logged_engine_cut_) {
                    logged_engine_cut_ = true;
                    const double meters = std::sqrt(static_cast<double>(delta.position.x) * delta.position.x +
                                                    static_cast<double>(delta.position.y) * delta.position.y +
                                                    static_cast<double>(delta.position.z) * delta.position.z);
                    logger_->Info("Frame Generation: the game's space jumped " + Number(meters, 1) +
                                  " m in a frame, or turned past 10 degrees (a camera cut): nothing is generated "
                                  "across it (VECTORXR_FRAMEGEN_ENGINE_CUTS=1). Cuts are counted in the engine "
                                  "motion summary.");
                }
            }
        }
        // Vectors to capture: the backend's raw fields are made from now on
        // (Generate's FrameGenTuning::engine_mode).
        engine_capturable_ =
            engine_capturable_ || (engine_requested_ >= FrameGenEngineMode::kCapture && views > 0 && settled);
        // Stage 2: motion vector images the game released for this frame are
        // captured at their next release, for the next frame (its end
        // captures the others). Through the Vulkan bridge the layer hands the
        // next frame's releases over before that frame's record comes (in
        // its xrEndFrame, after the bridge's copy): they are captured there.
        motion_targets_.clear();
        for (std::size_t i = 0; i < views && engine_requested_ >= FrameGenEngineMode::kCapture && settled; ++i) {
            if (!record.released[i]) {
                continue;
            }
            const FrameGenEngineMotionView& view = frame.views[i];
            CaptureTarget target;
            target.swapchain = view.vectors.swapchain;
            target.array_index = view.vectors.imageArrayIndex;
            target.rect = view.vectors.imageRect;
            target.view = view.view;
            target.motion = true;
            target.color_extent = {static_cast<std::uint32_t>(std::max(view.color.imageRect.extent.width, 0)),
                                   static_cast<std::uint32_t>(std::max(view.color.imageRect.extent.height, 0))};
            motion_targets_.push_back(target);
        }
        // The frame before's record is this frame's pair: the frame right
        // before it, with no pause between.
        const bool continuous = engine_frame_.valid && engine_frame_.frame.projection_frame + 1 == frame.projection_frame &&
                                frame.display_time - engine_frame_.frame.display_time <= PauseLimitLocked();
        record.frame = std::move(frame);
        engine_previous_ = std::move(engine_frame_);
        engine_frame_ = std::move(record);
        if (engine_worker_ && engine_requested_ != FrameGenEngineMode::kOff) {
            worker = engine_worker_;
            noted.record = engine_frame_.frame;
            noted.color_index = engine_frame_.color_index;
            noted.depth_index = engine_frame_.depth_index;
            noted.released = engine_frame_.released;
            for (const FrameGenEngineMotionView& view : engine_frame_.frame.views) {
                noted.color_extent.push_back({static_cast<std::uint32_t>(std::max(view.color.imageRect.extent.width, 0)),
                                              static_cast<std::uint32_t>(std::max(view.color.imageRect.extent.height, 0))});
            }
            noted.continuous = continuous;
        }
        // Through the Vulkan bridge the vectors are read at their releases
        // from stage 2, and at the frame's end only by a hand-over
        // (VECTORXR_FRAMEGEN_ENGINE_BRIDGE=end): their images go back to the
        // game's queue behind each copy from now on (VulkanBridge::MarkMotion),
        // until they are not read (VulkanBridge::UnmarkMotion): while the game
        // made a swapchain within the settle time, or once the validator
        // disabled them.
        read = vulkan && settled && views > 0 && engine_worker_ &&
               engine_validation_.state != FrameGenEngineValidation::State::kDisabled &&
               (engine_requested_ >= FrameGenEngineMode::kCapture ||
                engine_bridge_mode_ == FrameGenEngineBridgeMode::kEndFrame);
        if (read && logger_ && !logged_engine_bridge_read_) {
            logged_engine_bridge_read_ = true;
            logger_->Info(std::string("Frame Generation engine motion through the Vulkan bridge: the game's vectors are "
                                      "read in the runtime's images behind the bridge's copy, at their releases") +
                          (engine_bridge_mode_ == FrameGenEngineBridgeMode::kEndFrame
                               ? " and, for images the game never acquires, by a hand-over at the frame's end "
                                 "(VECTORXR_FRAMEGEN_ENGINE_BRIDGE=end)"
                               : "") +
                          "; each release gives their image back to the game's queue behind the copy. From " +
                          Number(kFrameGenEngineSettleSeconds, 0) +
                          " s after the game last made a swapchain; VECTORXR_FRAMEGEN_ENGINE_BRIDGE=0 reads none.");
        }
    }
    if (worker) {
        worker->NoteFrame(std::move(noted));
    }
    return read;
}

void FrameGenPresenter::EngineJobsLocked(const EngineFrame& frame, std::vector<FrameGenMotionJob>& jobs,
                                         std::vector<FrameGenViewRequest>& color) const {
    jobs.clear();
    color.clear();
    for (std::size_t i = 0; i < frame.frame.views.size(); ++i) {
        const FrameGenEngineMotionView& view = frame.frame.views[i];
        std::int64_t format = view.vectors_format;
        std::uint32_t acquired = 0;
        if (const auto vectors = swapchains_.find(view.vectors.swapchain); vectors != swapchains_.end()) {
            format = format ? format : vectors->second.format;
            acquired = vectors->second.acquired;
        }
        FrameGenMotionJob job;
        job.acquired = acquired; // left alone: a game following the specification holds them in a state of its own
        job.view = view.view;
        // The image is the census worker's guess, made outside the lock.
        job.vectors = {view.vectors.swapchain, frame.color_index[i] % kFrameGenEngineImages, view.vectors.imageArrayIndex,
                       view.vectors.imageRect, format};
        job.released = frame.released[i];
        job.color_index = frame.color_index[i];
        job.depth_index = frame.depth_index[i];
        job.frame = frame.frame.projection_frame;
        job.encoding = view.encoding;
        job.census_all = true;
        job.samples = true;
        jobs.push_back(job);
        color.resize(std::max<std::size_t>(color.size(), view.view + 1));
        color[view.view].extent = {static_cast<std::uint32_t>(std::max(view.color.imageRect.extent.width, 0)),
                                   static_cast<std::uint32_t>(std::max(view.color.imageRect.extent.height, 0))};
    }
}

void FrameGenPresenter::CaptureEngineMotion(const FrameGenMotionHandOver& hand_over) {
    std::vector<FrameGenMotionJob> jobs;
    std::vector<std::size_t> places; // each job's view in the record
    std::vector<FrameGenViewRequest> color;
    std::shared_ptr<FrameGenEngineCensusWorker> worker;
    std::optional<FrameGenEngineImageTracker::Hypothesis> image;
    bool capture = false;
    bool bridged = false;
    bool hand_overs = false; // VECTORXR_FRAMEGEN_ENGINE_BRIDGE=end
    std::uint64_t frame = 0;
    // Stage 3: the game's expected convention is looked for with its first
    // vectors (by the extension it sends them with, and its graphics API:
    // a Vulkan game's, on the bridge's Direct3D 12 backend).
    std::optional<bool> prior_ext;
    {
        std::scoped_lock lock(mutex_);
        // The views captured at their release since the last frame's end
        // (CaptureOnRelease): this frame's.
        const std::vector<std::uint8_t> released = std::exchange(engine_released_, {});
        bridged = engine_bridge_ == FrameGenEngineBridge::kVulkan;
        hand_overs = engine_bridge_mode_ == FrameGenEngineBridgeMode::kEndFrame;
        if (engine_requested_ == FrameGenEngineMode::kOff || !engine_worker_ || !engine_frame_.valid ||
            engine_frame_.frame.views.empty() || !CapturingLocked() || (bridged && !EngineBridgeSettledLocked())) {
            return;
        }
        worker = engine_worker_;
        capture = engine_requested_ >= FrameGenEngineMode::kCapture;
        image = engine_image_;
        frame = engine_frame_.frame.projection_frame;
        if (engine_requested_ >= FrameGenEngineMode::kObserve && !engine_prior_checked_) {
            engine_prior_checked_ = true;
            prior_ext = engine_frame_.frame.views.front().source == FrameGenMotionSource::kFrameSynthesisExt;
        }
        std::vector<FrameGenMotionJob> all;
        EngineJobsLocked(engine_frame_, all, color);
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].view < released.size() && released[all[i].view]) {
                engine_frame_.captured[i] = 1;
            } else {
                jobs.push_back(all[i]);
                places.push_back(i);
            }
        }
    }
    for (FrameGenMotionJob& job : jobs) {
        const std::optional<std::uint32_t> tracked =
            worker->Image(job.view, job.frame, job.color_index, job.depth_index, job.released);
        job.vectors.image_index = tracked.value_or(job.vectors.image_index);
        if (!capture) {
            continue;
        }
        // Stage 2: the image VECTORXR_FRAMEGEN_ENGINE_MV_IMAGE names, else the
        // tracker's; its census alone, but every image's while the tracker is
        // not sure which one is written, and every 30th frame (a change of
        // the game's way shows only there).
        using Policy = FrameGenEngineImageTracker::Policy;
        if (image) {
            job.vectors.image_index = image->policy == Policy::kReleased ? job.released.value_or(job.color_index)
                                      : image->policy == Policy::kColor  ? job.color_index
                                      : image->policy == Policy::kDepth  ? job.depth_index
                                                                         : image->parameter;
            job.vectors.image_index %= kFrameGenEngineImages;
        }
        const std::optional<FrameGenEngineCensusWorker::ViewSummary> summary = worker->Summarize(job.view);
        job.census_all = job.frame % 30 == 0 || (!image && !(summary && summary->locked));
        job.samples = false;
        job.capture = true;
    }
    // Through the Vulkan bridge the runtime's images hold the game's vectors
    // only behind the bridge's copy: at the frame's end a view is read only
    // where `hand_over` copied the image its vectors are thought to be in
    // (every image is handed over while the census reads every one, so the
    // tracker sees which one changes), with VECTORXR_FRAMEGEN_ENGINE_BRIDGE=end.
    // Every image is read (the census of them all, the samples, a research
    // dump of them all) only when every one was copied this frame: an image
    // the hand-over left out (its first, or a stalled queue) holds an older
    // frame, or nothing the bridge ever copied.
    bool every_copied = true;
    if (bridged) {
        std::vector<std::pair<XrSwapchain, std::uint32_t>> handed;
        std::vector<std::size_t> owners; // each image's job
        for (std::size_t k = 0; k < jobs.size() && hand_over && hand_overs; ++k) {
            // The swapchain's images (up to three), for the census and the hand-over alike.
            std::uint32_t count = 0;
            if (XR_FAILED(xr_.images(jobs[k].vectors.swapchain, 0, &count, nullptr))) {
                count = 0;
            }
            jobs[k].image_count = std::min(count, kFrameGenEngineImages);
            for (std::uint32_t i = 0; i < jobs[k].image_count; ++i) {
                if (jobs[k].census_all || jobs[k].samples || i == jobs[k].vectors.image_index) {
                    handed.push_back({jobs[k].vectors.swapchain, i});
                    owners.push_back(k);
                }
            }
        }
        const std::vector<bool> copied = handed.empty() ? std::vector<bool>{} : hand_over(handed);
        std::vector<FrameGenMotionJob> kept;
        std::vector<std::size_t> kept_places;
        for (std::size_t k = 0; k < jobs.size(); ++k) {
            bool chosen = false;
            bool all = true;
            for (std::size_t n = 0; n < handed.size(); ++n) {
                if (owners[n] != k) continue;
                const bool done = n < copied.size() && copied[n];
                chosen = chosen || (handed[n].second == jobs[k].vectors.image_index && done);
                all = all && done;
            }
            if (!chosen) continue;
            if (!all) {
                jobs[k].census_all = jobs[k].samples = false;
                every_copied = false;
            }
            kept.push_back(jobs[k]);
            kept_places.push_back(places[k]);
        }
        jobs = std::move(kept);
        places = std::move(kept_places);
    }
    std::string error;
    bool ok = true;
    std::vector<bool> resampled; // per job (stage 2)
    FrameGenEngineStatus status;
    std::uint64_t mark = 0;
    {
        // Lock order everywhere: submit, queue, GPU, presenter (CaptureOnRelease).
        std::unique_lock<std::mutex> queue_lock;
        if (queue_mutex_) {
            queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
        }
        std::scoped_lock gpu_lock(gpu_mutex_);
        if (!gpu_) {
            return;
        }
        if (prior_ext) {
            if (const std::optional<FrameGenEngineConvention> prior = FrameGenEnginePriorFor(
                    PathToUtf8(GetCurrentExecutablePath().filename()), *prior_ext, bridged ? "Vulkan" : gpu_->Name())) {
                gpu_->SetEnginePrior(*prior);
            }
        }
        for (const FrameGenMotionJob& job : jobs) {
            ok = ok && gpu_->Track(job.vectors.swapchain, xr_.images, error);
        }
        // A research capture under way copies the images into the frame's
        // directory, which the frame's dump (Generate) writes to next: once per
        // directory (a frame Generate did not dump leaves it to the next frame's
        // dump, never to a second copy of the images), every image until one
        // copy of them all is taken, then the image the vectors are thought to
        // be in.
        bool dumping = false;
        if (ok && !jobs.empty() && every_copied && CaptureUnderWay() && capture_motion_ &&
            capture_motion_armed_ != capture_index_) {
            const std::filesystem::path directory =
                std::filesystem::path(capture_root_) / ("frame-" + std::to_string(capture_index_));
            std::error_code ignored;
            std::filesystem::create_directories(directory, ignored);
            capture_motion_armed_ = capture_index_;
            dumping = gpu_->DumpMotion(directory.string(), capture_motion_all_);
        }
        // Views all captured at their release need no call; their censuses
        // are taken from the backend all the same.
        ok = ok && !jobs.empty() && gpu_->CaptureMotion(jobs, color, error, &resampled);
        if (ok && dumping) {
            capture_motion_all_ = false;
        }
        // Only the censuses read back since the last call: each carries its
        // samples.
        status = gpu_->EngineStatus(engine_census_after_);
        if (!status.census.empty()) {
            engine_census_after_ = status.census.back().sequence;
        }
        mark = ok ? gpu_->CaptureMark() : 0;
    }
    {
        std::scoped_lock lock(mutex_);
        last_capture_mark_ = std::max(last_capture_mark_, mark);
        // A view's vectors were read: their census (stage 0), from stage 2
        // their resample (a view the backend left out was not).
        if (ok && engine_frame_.valid && engine_frame_.frame.projection_frame == frame) {
            for (std::size_t k = 0; k < places.size(); ++k) {
                const bool read = !capture || (k < resampled.size() && resampled[k]);
                if (read && places[k] < engine_frame_.captured.size()) engine_frame_.captured[places[k]] = 1;
            }
        }
        // Stage 9: whether each view's newest census found its vectors
        // exactly zero everywhere it read in the view's region (G10: one
        // eye's alone). The statistics grid, inside the region, says: the
        // hash grid covers the whole image, which two views side by side in
        // one slice share.
        for (const FrameGenEngineCensus& census : status.census) {
            if (census.early) continue;
            bool read_any = false;
            bool empty = true;
            for (std::uint32_t i = 0; i < census.images && i < kFrameGenEngineImages; ++i) {
                if (((census.censused >> i) & 1u) == 0) continue;
                read_any = true;
                empty = empty && census.image[i].finite == kFrameGenEngineStatsTexels && census.image[i].max_abs == 0.0f;
            }
            if (!read_any) continue;
            engine_empty_.resize(std::max(engine_empty_.size(), census.view + 1));
            engine_empty_[census.view] = empty;
        }
        // Stage 6: the frames' verdicts read back since the last call, as Lead
        // judges them (those of frames held at Assist too), steer a session
        // in Lead (FrameGenEngineLeadControl).
        const FrameGenEngineValidation& before = engine_validation_;
        if (engine_requested_ == FrameGenEngineMode::kLead && status.validation.frames >= before.frames &&
            status.validation.lead_held >= before.lead_held) {
            const std::uint64_t demotions = engine_lead_.Demotions();
            engine_lead_.AddVerdicts(status.validation.frames - before.frames,
                                     status.validation.lead_held - before.lead_held);
            engine_window_.demotions += engine_lead_.Demotions() - demotions;
        }
        // Stage 3: the validator's state for the summary, and each of its
        // decisions logged once.
        engine_validation_ = status.validation;
        if (logger_) {
            for (; engine_notes_logged_ < status.notes.size(); ++engine_notes_logged_) {
                logger_->Info("Frame Generation engine motion: " + status.notes[engine_notes_logged_]);
            }
            if (!ok && !error.empty() && !logged_engine_failure_) {
                logged_engine_failure_ = true;
                logger_->Error("Frame Generation could not read the game's motion vectors: " + error);
            }
            if (status.validation.decisions > engine_decisions_logged_) {
                engine_decisions_logged_ = status.validation.decisions;
                logger_->Info(std::string("Frame Generation engine convention: ") + ToString(status.validation.state) +
                              " (" + status.validation.reason + "), after " + std::to_string(status.validation.frames) +
                              " frames and " + std::to_string(status.validation.batches) + " batches.");
            }
        }
    }
    worker->NoteStatus(status);
}

void FrameGenPresenter::RecordCaptureTargetsLocked(const XrCompositionLayerProjection& projection) {
    capture_targets_.clear();
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        const auto& sub = projection.views[i].subImage;
        CaptureTarget color;
        color.swapchain = sub.swapchain;
        color.array_index = sub.imageArrayIndex;
        color.rect = sub.imageRect;
        color.view = i;
        capture_targets_.push_back(color);
        if (const auto* depth = DepthInfo(projection.views[i])) {
            CaptureTarget target;
            target.swapchain = depth->subImage.swapchain;
            target.array_index = depth->subImage.imageArrayIndex;
            target.rect = depth->subImage.imageRect;
            target.view = i;
            target.depth = true;
            target.encoding = {depth->minDepth, depth->maxDepth, depth->nearZ, depth->farZ};
            target.color_extent = Extent(projection.views[i]);
            capture_targets_.push_back(target);
        }
    }
    LogDepthDescriptionLocked(projection);
    LogViewExtentsLocked(projection);
}

void FrameGenPresenter::LogViewExtentsLocked(const XrCompositionLayerProjection& projection) {
    if (!logger_) {
        return;
    }
    std::vector<FrameGenExtent> extents;
    for (std::uint32_t i = 0; projection.views && i < projection.viewCount; ++i) {
        extents.push_back(Extent(projection.views[i]));
    }
    const auto same = [](const std::vector<FrameGenExtent>& a, const std::vector<FrameGenExtent>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](FrameGenExtent x, FrameGenExtent y) {
            return x.width == y.width && x.height == y.height;
        });
    };
    if (!same(extents, view_extents_)) {
        view_extents_ = std::move(extents);
        ++view_extent_changes_;
    }
    ++view_extent_frames_;
    if (same(view_extents_, logged_view_extents_)) {
        return;
    }
    // Every change for the first lines, then at most a line per 5 s with
    // the changes between (a game resizing every frame stays readable).
    const auto now = Clock::now();
    if (view_extent_lines_ >= 20 && now - view_extents_logged_at_ < std::chrono::seconds(5)) {
        return;
    }
    const bool first = logged_view_extents_.empty();
    std::string sizes;
    for (std::size_t i = 0; i < view_extents_.size(); ++i) {
        sizes += (i ? ", " : "") + std::to_string(view_extents_[i].width) + "x" + std::to_string(view_extents_[i].height);
    }
    logger_->Info(first ? "Frame Generation: the game's views are " + sizes + "."
                        : "Frame Generation: the game's view sizes changed to " + sizes + " (" +
                              std::to_string(view_extent_changes_) + " change" +
                              (view_extent_changes_ == 1 ? "" : "s") + " in " + std::to_string(view_extent_frames_) +
                              " frames since the last line; each replaces Frame Generation's images for its views).");
    logged_view_extents_ = view_extents_;
    view_extent_changes_ = 0;
    view_extent_frames_ = 0;
    ++view_extent_lines_;
    view_extents_logged_at_ = now;
}

namespace {

// Every digit a float holds: the game's depth description, read exactly.
std::string Precise(double value) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

std::string Handle(const void* handle) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle)));
    return buffer;
}

std::string Rect(const std::array<std::int32_t, 4>& rect) {
    return std::to_string(rect[2]) + "x" + std::to_string(rect[3]) + " at " + std::to_string(rect[0]) + "," +
           std::to_string(rect[1]);
}

} // namespace

void FrameGenPresenter::LogDepthDescriptionLocked(const XrCompositionLayerProjection& projection) {
    if (!logger_) {
        return;
    }
    std::vector<DepthViewDescription> description;
    bool any_depth = false;
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        const XrSwapchainSubImage& color = projection.views[i].subImage;
        DepthViewDescription view;
        view.color = color.swapchain;
        view.color_slice = color.imageArrayIndex;
        view.color_rect = {color.imageRect.offset.x, color.imageRect.offset.y, color.imageRect.extent.width,
                           color.imageRect.extent.height};
        if (const auto* depth = DepthInfo(projection.views[i])) {
            any_depth = true;
            const XrSwapchainSubImage& sub = depth->subImage;
            view.depth = sub.swapchain;
            view.depth_slice = sub.imageArrayIndex;
            view.depth_rect = {sub.imageRect.offset.x, sub.imageRect.offset.y, sub.imageRect.extent.width,
                               sub.imageRect.extent.height};
            view.encoding = {depth->minDepth, depth->maxDepth, depth->nearZ, depth->farZ};
        }
        description.push_back(view);
    }
    if (!any_depth) {
        description.clear();
    }
    if (description != depth_description_) {
        depth_description_ = std::move(description);
        ++depth_description_changes_;
    }
    ++depth_description_frames_;
    if (depth_description_ == logged_depth_description_) {
        return;
    }
    const auto now = Clock::now();
    if (depth_description_lines_ >= 20 && now - depth_description_logged_at_ < std::chrono::seconds(5)) {
        return;
    }
    const bool first = !logged_depth_info_;
    std::string line;
    if (depth_description_.empty()) {
        line = "Frame Generation: the game stopped submitting depth";
    } else {
        line = first ? "Frame Generation: the game submits depth described as"
                     : "Frame Generation: the game's depth description changed to";
        for (std::size_t i = 0; i < depth_description_.size(); ++i) {
            const DepthViewDescription& view = depth_description_[i];
            line += (i ? "; view " : " view ") + std::to_string(i) + ": ";
            if (!view.depth) {
                line += "no depth";
            } else {
                const auto record = swapchains_.find(view.depth);
                line += "minDepth=" + Precise(view.encoding[0]) + ", maxDepth=" + Precise(view.encoding[1]) +
                        ", nearZ=" + Precise(view.encoding[2]) + ", farZ=" + Precise(view.encoding[3]) + ", depth " +
                        Handle(view.depth) + " slice " + std::to_string(view.depth_slice) + " region " +
                        Rect(view.depth_rect) + ", format " +
                        std::to_string(record != swapchains_.end() ? record->second.format : 0) +
                        (record != swapchains_.end() && record->second.depth_capturable ? ", readable"
                                                                                       : ", not readable");
            }
            line += "; color " + Handle(view.color) + " slice " + std::to_string(view.color_slice) + " region " +
                    Rect(view.color_rect);
        }
    }
    if (!first) {
        line += " (" + std::to_string(depth_description_changes_) + " change" +
                (depth_description_changes_ == 1 ? "" : "s") + " in " + std::to_string(depth_description_frames_) +
                " frames since the last line)";
    }
    logger_->Info(line + (first ? "; it is checked against the other eye before use." : "."));
    logged_depth_info_ = true;
    logged_depth_description_ = depth_description_;
    depth_description_changes_ = 0;
    depth_description_frames_ = 0;
    ++depth_description_lines_;
    depth_description_logged_at_ = now;
}

bool FrameGenPresenter::EngineMapRingLocked() const {
    return engine_map_ != FrameGenEngineMap::kOff && engine_requested_ >= FrameGenEngineMode::kAssist &&
           engine_capturable_ && gpu_ && gpu_->SupportsEngineMotion();
}

std::uint8_t FrameGenPresenter::CanvasRingLocked() {
    // Each further slot keeps a real frame per view (HistorySlotBytes).
    const FrameCopy& copy = newest_copy_.valid ? newest_copy_ : passthrough_copy_;
    const std::uint64_t slot_bytes =
        copy.valid && copy.has_projection && copy.projection_index < copy.layers.size()
            ? HistorySlotBytes(copy.layers[copy.projection_index].views.data(),
                               copy.layers[copy.projection_index].views.size(), EngineMapRingLocked())
            : 0;
    const std::uint8_t roomy = kFrameGenCanvasRingRoomy;
    const std::uint8_t lean = FrameGenHistoryRing(false);
    const std::uint64_t extra = slot_bytes * (roomy - lean);
    const std::optional<FrameGenMemoryBudget> memory = gpu_ ? gpu_->MemoryBudget() : std::nullopt;
    // The backend keeps its views' images from one presenter thread run to
    // the next (until Stop): slots it holds beyond three are in the
    // process's usage already, and are counted once, not again as extra
    // (which made a ring of five look short of memory at the next start and
    // a ring of three roomy at the one after: every history image replaced,
    // a wait for the GPU, each time).
    const std::uint64_t held =
        held_ring_ > lean ? std::uint64_t{static_cast<std::uint8_t>(held_ring_ - lean)} * held_slot_bytes_ : 0;
    const std::uint64_t usage = memory ? memory->usage_bytes - std::min(memory->usage_bytes, held) : 0;
    // Unknown sizes or budget: the default. Room means the extra slots and a
    // tenth of the budget still fit; five already held stay while a
    // twentieth does (the multiplier's own near-budget line), so usage
    // moving a little between runs does not replace them back and forth.
    const bool keeping = held_ring_ >= roomy;
    const bool room = slot_bytes && memory && memory->budget_bytes &&
                      usage + extra + memory->budget_bytes / (keeping ? 20 : 10) < memory->budget_bytes;
    const std::uint8_t ring = room ? roomy : lean;
    if (logger_ && ring != logged_canvas_ring_ && slot_bytes && memory && memory->budget_bytes) {
        // Every change is logged: a later one (the game's video memory grown
        // over a long session) changes what slow games can be moved from.
        const bool first = logged_canvas_ring_ == 0;
        logged_canvas_ring_ = ring;
        constexpr double megabytes = 1024.0 * 1024.0;
        const std::string budget = "usage " + Number(static_cast<double>(memory->usage_bytes) / megabytes, 0) +
                                   " of " + Number(static_cast<double>(memory->budget_bytes) / megabytes, 0) + " MB";
        logger_->Info("Frame Generation " + std::string(first ? "keeps" : "now keeps") + " the last " +
                      std::to_string(ring) + " real frames to generate from" +
                      (room ? " (" + Number(static_cast<double>(extra) / megabytes, 0) +
                                  " MB more video memory than three, so slow games keep every frame they need; " +
                                  budget + ")."
                            : " (video memory is short: " + budget + ")."));
    }
    return ring;
}

bool FrameGenPresenter::QuadCapableLocked() const {
    return started_ && gpu_ && ThreadedEligibleLocked() && gpu_->SupportsJustInTime() && gpu_->SupportsComposition();
}

bool FrameGenPresenter::QuadTargetsLocked(const FrameGenQuadFrame* quad) const {
    return quad_mode_ || (quad && quad->route != FrameGenQuadRoute::kDeclined && QuadCapableLocked());
}

void FrameGenPresenter::RecordQuadCaptureTargetsLocked(const XrCompositionLayerProjection& source) {
    capture_targets_.clear();
    std::array<FrameGenExtent, 4> extents{};
    for (std::uint32_t i = 0; i < 4 && i < source.viewCount; ++i) {
        const auto& sub = source.views[i].subImage;
        CaptureTarget color;
        color.swapchain = sub.swapchain;
        color.array_index = sub.imageArrayIndex;
        color.rect = sub.imageRect;
        color.view = i;
        capture_targets_.push_back(color);
        extents[i] = Extent(source.views[i]);
    }
    bool changed = false;
    for (std::size_t i = 0; i < extents.size(); ++i) {
        changed = changed || extents[i].width != quad_extents_[i].width || extents[i].height != quad_extents_[i].height;
    }
    if (changed) {
        quad_extents_ = extents;
        if (logger_) {
            logger_->Info("Frame Generation can compose the quad views itself: periphery " +
                          std::to_string(extents[0].width) + "x" + std::to_string(extents[0].height) + ", focus " +
                          std::to_string(extents[2].width) + "x" + std::to_string(extents[2].height) +
                          "; their releases are captured while it presents.");
        }
    }
}

bool FrameGenPresenter::QuadCapturedLocked(const XrCompositionLayerProjection& source) const {
    if (source.viewCount < quad_captures_.size() || !source.views) {
        return false;
    }
    for (std::size_t i = 0; i < quad_captures_.size(); ++i) {
        const XrSwapchainSubImage& sub = source.views[i].subImage;
        const QuadCapture& capture = quad_captures_[i];
        const auto record = swapchains_.find(sub.swapchain);
        if (!capture.ok || record == swapchains_.end() || record->second.sample_count != 1 ||
            !record->second.released || capture.swapchain != sub.swapchain ||
            capture.image_index != *record->second.released || capture.array_index != sub.imageArrayIndex ||
            capture.rect.offset.x != sub.imageRect.offset.x || capture.rect.offset.y != sub.imageRect.offset.y ||
            capture.rect.extent.width != sub.imageRect.extent.width ||
            capture.rect.extent.height != sub.imageRect.extent.height || capture.releases != record->second.releases) {
            return false;
        }
    }
    return true;
}

FrameGenQuadRoute FrameGenPresenter::ClaimQuadFrame(const XrCompositionLayerProjection& source) {
    std::scoped_lock lock(mutex_);
    // Without composition, the layer's composed views are Frame
    // Generation's frames as before (its canvas), captured from them.
    if (!QuadCapableLocked() || source.viewCount < 4 || !source.views) {
        return FrameGenQuadRoute::kNotPresenting;
    }
    std::string unsupported;
    if (!SupportedLayerChains(reinterpret_cast<const XrCompositionLayerBaseHeader&>(source), unsupported)) {
        unsupported_composition_ = true;
        UnavailableLocked(unsupported + " cannot be preserved, so Frame Generation is off until the game restarts; "
                          "frames pass straight through");
        return FrameGenQuadRoute::kNotPresenting;
    }
    quad_offered_ = true;
    RecordQuadCaptureTargetsLocked(source);
    if (!compositor_running_ || !jit_ || !quad_mode_ || quad_switch_pending_ || compositor_drain_ ||
        compositor_stop_ || !StayThreadedLocked() || Clock::now() < quad_declined_until_) {
        return FrameGenQuadRoute::kNotPresenting;
    }
    // The frame ending is the oldest open one, as EndFrame matches it: a
    // just-in-time frame of the presenter thread's, with nothing older.
    if (dropped_unended_ > 0 || !frames_.empty() || quad_claim_id_) {
        return FrameGenQuadRoute::kBusy;
    }
    const auto it = std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                 [](const ThreadedFrame& frame) { return !frame.claimed; });
    if (it == threaded_frames_.end() || !it->jit) {
        return FrameGenQuadRoute::kBusy;
    }
    if (!QuadCapturedLocked(source)) {
        return FrameGenQuadRoute::kUncaptured;
    }
    // Claimed under the same lock as the decision: no stop drops it (only
    // unclaimed frames are), and the presenter thread drains it.
    it->claimed = true;
    it->quad = true;
    quad_claim_id_ = it->id;
    return FrameGenQuadRoute::kClaimed;
}

void FrameGenPresenter::NoteQuadEndLocked(const FrameGenQuadFrame* quad, bool projection) {
    if (quad) {
        switch (quad->route) {
        case FrameGenQuadRoute::kClaimed: ++totals_.quad_claimed; break;
        case FrameGenQuadRoute::kUncaptured: ++totals_.quad_uncaptured; break;
        case FrameGenQuadRoute::kBusy: ++totals_.quad_busy; break;
        case FrameGenQuadRoute::kDeclined: ++totals_.quad_declined; break;
        default: ++totals_.quad_not_presenting; break;
        }
        if (quad->route == FrameGenQuadRoute::kDeclined) {
            quad_offered_ = false;
        }
    } else if (projection) {
        // A projection that did not come from the quad compose site.
        quad_offered_ = false;
    }
    if (!compositor_running_ || !jit_ || (!quad && !projection)) {
        return;
    }
    // Frames that now want the other composition restart the presenter
    // thread in it, as a synthesis mode switch does.
    const bool composed = quad && quad->route != FrameGenQuadRoute::kDeclined && QuadCapableLocked();
    if (composed != quad_mode_ && !quad_switch_pending_) {
        quad_switch_pending_ = true;
        healthy_streak_ = 0;
        ++totals_.restarts;
        if (logger_) {
            logger_->Info(composed ? "Frame Generation restarts to compose the quad views itself once frames are "
                                     "steady again."
                                   : "Frame Generation restarts on the layer's composed quad views once frames are "
                                     "steady again.");
        }
    }
}

void FrameGenPresenter::DeclineQuadLocked(Clock::time_point now, const std::string& error) {
    quad_unwritten_in_row_ = 0;
    // A decline long after the last one starts the doubling over.
    if (quad_declined_until_ != Clock::time_point{} && now - quad_declined_until_ > std::chrono::minutes(5)) {
        quad_decline_ = quad_decline_first_;
    }
    const std::chrono::milliseconds decline = quad_decline_;
    quad_declined_until_ = now + decline;
    quad_decline_ = std::min(quad_decline_ * 2, quad_decline_longest_);
    ++quad_write_declines_;
    quad_decline_resumes_ = true;
    if (logger_) {
        logger_->Error("Frame Generation could not write " + std::to_string(kQuadUnwrittenBeforeDecline) +
                       " composed frames in a row (" + error + "); it stands by for " +
                       Number(std::chrono::duration<double>(decline).count(), 1) +
                       " s: the layer composes the quad views and its frames pass straight through.");
    }
}

bool FrameGenPresenter::QuadDeclinedLocked() const {
    // Only where the layer offers quad views (composing them is what
    // failed): its composed views alone still generate frames.
    return quad_offered_ && quad_declined_until_ != Clock::time_point{} && Clock::now() < quad_declined_until_ &&
           QuadCapableLocked();
}

std::string FrameGenPresenter::QuadSummaryLocked(const Totals& start) const {
    const std::uint64_t claimed = totals_.quad_claimed - start.quad_claimed;
    const std::uint64_t uncaptured = totals_.quad_uncaptured - start.quad_uncaptured;
    const std::uint64_t busy = totals_.quad_busy - start.quad_busy;
    const std::uint64_t declined = totals_.quad_declined - start.quad_declined;
    const std::uint64_t not_presenting = totals_.quad_not_presenting - start.quad_not_presenting;
    if (!quad_mode_ && claimed + uncaptured + busy + declined + not_presenting == 0) {
        return {};
    }
    // fgComposed: frames Frame Generation composed; d3d11Composed: frames
    // the layer composed (not captured / not a frame of its own / declined
    // by the layer / not presenting); setups: views set up from a frame.
    return ", quad=" + std::to_string(quad_mode_ && compositor_running_) + ", fgComposed=+" + std::to_string(claimed) +
           ", d3d11Composed=+" + std::to_string(uncaptured + busy + declined + not_presenting) + " (" +
           std::to_string(uncaptured) + "/" + std::to_string(busy) + "/" + std::to_string(declined) + "/" +
           std::to_string(not_presenting) + "), setups=+" + std::to_string(totals_.quad_setups - start.quad_setups) +
           ", claimedUnanalyzed=+" + std::to_string(totals_.quad_unanalyzed - start.quad_unanalyzed) +
           (Clock::now() < quad_declined_until_ ? ", quadDeclined=1" : "");
}

bool FrameGenPresenter::RunnableLocked() const {
    return started_ && gpu_ && !unsupported_composition_ && user_enabled_ && configured_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && healthy_streak_ >= engage_streak_frames_ &&
           Clock::now() - started_at_ >= engage_min_age_;
}

bool FrameGenPresenter::WantsEngagementLocked() const {
    // Never from display times that do not advance with the time: refreshes
    // presented from them would never reach the game's frames.
    return RunnableLocked() && EffectiveMultiplierLocked() >= 2 && !QuadDeclinedLocked() && !clock_displaced_;
}

FrameGenState FrameGenPresenter::IdleStateLocked() const {
    return user_enabled_ && configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff;
}

bool FrameGenPresenter::StayThreadedLocked() const {
    // Just in time, the presenter thread interpolates or extrapolates as it
    // started: another mode starts it again.
    // Likewise frames that want the other quad views composition. While
    // composed quad frames could not be written for a while, nothing is
    // presented from it: the layer's frames pass straight through (shown as
    // they are by the presenter thread, the runtime could show a newer
    // composed image with an older frame's views).
    const bool same_mode = !jit_ || jit_interpolate_ == (settings_.synthesis_mode == FrameGenSynthesisMode::kInterpolate);
    return started_ && gpu_ && !unsupported_composition_ && user_enabled_ && configured_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && EffectiveMultiplierLocked() >= 2 && same_mode &&
           !quad_switch_pending_ && !QuadDeclinedLocked() && !clock_displaced_;
}

bool FrameGenPresenter::ThreadedEligibleLocked() const {
    return gpu_ && gpu_->SupportsThreadedPresentation() && !threaded_disabled_;
}

std::uint32_t FrameGenPresenter::EffectiveMultiplierLocked() const {
    switch (settings_.multiplier) {
    case FrameGenMultiplier::k2:
        return 2;
    case FrameGenMultiplier::k3:
        return 3;
    case FrameGenMultiplier::k4:
        return 4;
    case FrameGenMultiplier::kDynamic:
        // Standing by is full rate: frames pass straight through.
        return DynamicLocked() ? (dynamic_standby_ ? 1 : DynamicSlotsLocked()) : auto_multiplier_;
    default:
        return auto_multiplier_;
    }
}

XrDuration FrameGenPresenter::PauseLimitLocked() const {
    // A hitch of a game at 30 fps (a frame of five or six refreshes) is part
    // of its pace: frames are still measured and covered across it.
    const auto own = std::chrono::nanoseconds(static_cast<std::int64_t>(4.0 * real_interval_ns_));
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::max<Clock::duration>(kPauseFloor, std::min<Clock::duration>(own, kPauseCeiling)))
        .count();
}

void FrameGenPresenter::NoteRealEndLocked(Clock::time_point end) {
    if (real_end_ && end > *real_end_) {
        const double interval = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - *real_end_).count());
        // A pause is not the game's pace.
        if (interval <= static_cast<double>(PauseLimitLocked())) {
            real_interval_ns_ = real_interval_ns_ > 0.0 ? real_interval_ns_ * 0.9 + interval * 0.1 : interval;
        }
    }
    real_end_ = end;
}

double FrameGenPresenter::StandbySecondsLocked(Clock::time_point now) const {
    double seconds = standby_seconds_;
    if (standby_since_ && dynamic_standby_ && DynamicLocked() && now > *standby_since_) {
        seconds += std::chrono::duration<double>(now - *standby_since_).count();
    }
    return seconds;
}

std::string FrameGenPresenter::ClockSummaryLocked(const Totals& start) const {
    // runtimeClock: times the runtime's display clock was displaced / its
    // waits that found it so, in the window (NoteRuntimeClockLocked).
    const std::uint64_t displaced = totals_.clock_displacements - start.clock_displacements;
    const std::uint64_t unsteady = totals_.clock_unsteady_waits - start.clock_unsteady_waits;
    if (!clock_displaced_ && displaced == 0 && unsteady == 0) {
        return {};
    }
    return ", runtimeClock=" + std::to_string(displaced) + "/" + std::to_string(unsteady) +
           (clock_displaced_ ? " (displaced)" : "");
}

std::string FrameGenPresenter::DynamicSummaryLocked() const {
    if (!DynamicLocked()) {
        return {};
    }
    // standDownHoldS: how long the next stand-down passes frames straight
    // through (doubling after short engagements, DynamicStandDownLocked).
    return std::string(", dynamic=") + (dynamic_standby_ ? (dynamic_probing_ ? "probing" : "standby") : "engaged") +
           (dynamic_reason_.empty() ? std::string() : " (" + dynamic_reason_ + ")") +
           ", standDownHoldS=" + Number(std::chrono::duration<double>(dynamic_hold_).count(), 1) +
           ", standDowns=" + std::to_string(dynamic_stand_downs_);
}

void FrameGenPresenter::NotePauseLocked(XrDuration gap) {
    ++totals_.pauses;
    const double gap_ms = static_cast<double>(gap) / 1e6;
    pause_longest_ms_ = std::max(pause_longest_ms_, gap_ms);
    if (logger_ && logged_pauses_ < kLoggedPauses) {
        ++logged_pauses_;
        logger_->Info("Frame Generation: the game sent no frame for " + Number(gap_ms / 1000.0, 2) +
                      " s; nothing is generated across the gap.");
    }
}

bool FrameGenPresenter::DynamicLocked() const {
    // Interpolating, the frames between two real frames are only known once
    // the second ends: written just in time, any number of them fit.
    return settings_.multiplier == FrameGenMultiplier::kDynamic && ThreadedEligibleLocked() &&
           (settings_.synthesis_mode == FrameGenSynthesisMode::kExtrapolate || gpu_->SupportsJustInTime());
}

std::uint32_t FrameGenPresenter::DynamicSlotsLocked() const {
    // Two until measured: always engaged, so frames the game misses are filled.
    if (dynamic_interval_slots_ <= 0.0) return 2;
    return static_cast<std::uint32_t>(std::clamp(std::lround(dynamic_interval_slots_), 2l, 4l));
}

std::uint32_t FrameGenPresenter::DynamicSetCapLocked() const {
    // Two generated frames and a spare cover a game at a third of the refresh
    // rate or faster; slower, the last frame is shown again until the next
    // real one (a fourth set costs as much video memory as the game's own
    // frame at headset resolution). Near the video memory budget, one fewer.
    return std::min<std::uint32_t>(3, auto_memory_ceiling_);
}

void FrameGenPresenter::UpdateAutoLocked(Clock::time_point now, const std::optional<PendingFrame>& frame,
                                         bool started_late) {
    const bool dynamic = DynamicLocked();
    // Dynamic where the presenter thread cannot extrapolate is Auto.
    const bool automatic = settings_.multiplier == FrameGenMultiplier::kAuto ||
                           (settings_.multiplier == FrameGenMultiplier::kDynamic && !dynamic);
    const XrDuration period = frame && frame->period > 0 ? frame->period : last_period_;
    // Nor while the runtime's display times do not advance with the time:
    // frames pass straight through then, and judged against one refresh each
    // they would step the multiplier up for nothing.
    if (!(automatic || dynamic) || period <= 0 || !RunnableLocked() || clock_displaced_) {
        last_app_end_ = now;
        cadence_.clear();
        dynamic_below_since_.reset(); // standing by, the game's rate is measured afresh
        if (clock_displaced_ && dynamic && dynamic_standby_) {
            dynamic_reason_ = RuntimeClockTextLocked();
        }
        return;
    }
    // Paging makes the game late too, and reading that as a reason to step
    // up allocates more output images and compounds it. Sample the process
    // budget twice a second; within 5% of it, step down (EnsureOutputs frees
    // the sets above the new ceiling). Budgets shrink while other processes
    // need memory and grow back: with room for another set and a tenth of the
    // budget to spare for memory_recovery_delay_, the ceiling rises again, at
    // most kMemoryRecoveries times a session so a budget hovering at the edge
    // does not reallocate headset-size images back and forth.
    const bool sampled_memory =
        memory_checked_at_ == Clock::time_point{} || now - memory_checked_at_ >= std::chrono::milliseconds(500);
    if (sampled_memory) {
        memory_checked_at_ = now;
        memory_budget_ = gpu_ ? gpu_->MemoryBudget() : std::nullopt;
    }
    // Just in time one set of output images serves any multiplier: there is
    // nothing to give back by generating fewer frames.
    const bool memory_pressure = !(compositor_running_ && jit_) && memory_budget_ && memory_budget_->budget_bytes &&
                                 memory_budget_->usage_bytes > memory_budget_->budget_bytes - memory_budget_->budget_bytes / 20;
    if (memory_pressure) {
        memory_room_since_.reset();
    } else if (sampled_memory && auto_memory_ceiling_ < kMaxMultiplier && memory_recoveries_ < kMemoryRecoveries &&
               memory_budget_ && memory_budget_->budget_bytes && output_set_bytes_) {
        const std::uint64_t budget = memory_budget_->budget_bytes;
        if (memory_budget_->usage_bytes + output_set_bytes_ + budget / 10 >= budget) {
            memory_room_since_.reset();
        } else if (!memory_room_since_) {
            memory_room_since_ = now;
        } else if (now - *memory_room_since_ >= memory_recovery_delay_) {
            memory_room_since_.reset();
            ++memory_recoveries_;
            auto_memory_ceiling_ = dynamic ? kMaxMultiplier : auto_memory_ceiling_ + 1;
            if (logger_) {
                logger_->Info(std::string("Frame Generation: process video memory has room again; ") +
                              (dynamic ? "dynamic uses up to three generated frames per real frame."
                                       : "automatic may step up to " + MultiplierText(auto_memory_ceiling_) + "."));
                LogMemoryBudgetLocked();
            }
        }
    }
    if (dynamic) {
        // No multiplier to choose: near the budget, real frames use at most
        // two sets of output images (EnsureOutputs frees the third).
        if (memory_pressure && sampled_memory && auto_memory_ceiling_ > 2) {
            auto_memory_ceiling_ = 2;
            if (logger_) {
                logger_->Info("Frame Generation (dynamic) uses at most two generated frames per real frame until video "
                              "memory frees up: process video memory is near its budget.");
                LogMemoryBudgetLocked();
            }
        }
        // The game's rate is judged against the refresh rate, not half of it
        // where the runtime halves the frame rate and reports that period.
        UpdateDynamicLocked(now, refresh_period_ > 0 ? refresh_period_ : period);
        last_app_end_ = now;
        cadence_.clear();
        return;
    }
    if (memory_pressure) {
        if (sampled_memory) {
            auto_memory_ceiling_ = std::min(auto_memory_ceiling_, auto_multiplier_ > 2 ? auto_multiplier_ - 1 : 2u);
        }
        if (auto_multiplier_ > auto_memory_ceiling_) {
            const std::uint32_t before = auto_multiplier_;
            auto_multiplier_ = auto_memory_ceiling_;
            probing_ = false;
            multiplier_changed_at_ = now;
            last_app_end_ = now;
            cadence_.clear();
            if (logger_) {
                logger_->Info("Frame Generation automatic multiplier " + MultiplierText(before) + " -> " +
                              MultiplierText(auto_multiplier_) +
                              ": process video memory is near its budget; it stays at most this until video memory "
                              "frees up.");
                LogMemoryBudgetLocked();
            }
            return;
        }
    }
    // The runtime holds the frames presented to half its refresh rate (or
    // less), shown by the presenter thread's waits at this multiplier, and
    // reports the refresh period: every engaged frame then takes twice its
    // refreshes and reads as late, and stepping up only slows the game more
    // (Pimax Play's smoothing held a game it let run at 36 fps to 9 at 4x,
    // the output still 36). Engaged gains nothing while that lasts: back to
    // full rate, frames pass straight through as Dynamic stands down, and
    // stay so while the runtime halves. One reporting the period it holds
    // frames to times the game's frames by it (GameGridLocked): they read as
    // late only where the game misses that rate, and the generated frames
    // between still fill every refresh the runtime takes.
    if (runtime_halving_ && halving_confirmed_at_ && *halving_confirmed_at_ > multiplier_changed_at_ &&
        auto_multiplier_ > 1 && GameGridLocked() < refresh_period_ + refresh_period_ / 2) {
        const std::uint32_t before = auto_multiplier_;
        const std::chrono::milliseconds hold = auto_halving_hold_;
        auto_multiplier_ = 1;
        probing_ = false;
        auto_halving_hold_until_ = now + hold;
        auto_halving_hold_ = std::min(auto_halving_hold_ * 2, dynamic_policy_.hold_max);
        multiplier_changed_at_ = now;
        last_app_end_ = now;
        cadence_.clear();
        if (logger_) {
            logger_->Info("Frame Generation automatic multiplier " + MultiplierText(before) + " -> full rate: " +
                          kHalvingStandDown + "; frames pass straight through, retrying in " +
                          Number(std::chrono::duration<double>(hold).count(), 0) + " s.");
        }
        return;
    }
    // Frames right after a change (a new multiplier, engaging, new output
    // images) carry its one-off cost and say nothing about the app. Probes
    // start from an already steady game: after a shorter transition, eight
    // actual frames still have to establish persistent lateness before the
    // old multiplier is restored. Capture latency alone cannot tell whether
    // the game has the throughput for the lower multiplier.
    // Only the probe's own switch: a change since (new settings, the
    // runtime's clock recovering, new output images) settles in full.
    const bool probe_switch = probing_ && multiplier_changed_at_ == probe_started_at_;
    const auto settle = probe_switch ? std::min(auto_settle_, kAutoProbeSettle) : auto_settle_;
    if (last_app_end_ && now - multiplier_changed_at_ >= settle) {
        // An engaged frame should take N refreshes; a passed-through one, one.
        const double multiplier = frame && frame->engaged ? static_cast<double>(frame->multiplier) : 1.0;
        const auto expected = std::chrono::nanoseconds(static_cast<std::int64_t>(period * multiplier * kAutoLateFactor));
        const bool late = now - *last_app_end_ > expected;
        cadence_.push_back({now, late, late && started_late});
    }
    last_app_end_ = now;
    while (!cadence_.empty() && (now - cadence_.front().time > std::chrono::seconds(6) || cadence_.size() > 4096)) {
        cadence_.pop_front();
    }
    if (now - multiplier_changed_at_ < settle) {
        return;
    }
    std::size_t recent = 0, recent_late = 0, recent_started_late = 0, steady = 0, steady_late = 0;
    for (const Cadence& entry : cadence_) {
        if (now - entry.time <= std::chrono::seconds(1)) {
            ++recent;
            recent_late += entry.late ? 1 : 0;
            recent_started_late += entry.started_late ? 1 : 0;
        }
        if (now - entry.time <= auto_steady_) {
            ++steady;
            steady_late += entry.late ? 1 : 0;
        }
    }
    // ...and the probe is judged once it has a quarter of the configured
    // settle's frames past that (500 ms by default) and kAutoProbeJudgedFrames
    // of them, or the configured settle has passed (a slow game), never on
    // the first eight: a few late frames in a row as the short settle ends
    // (a transition not quite over) made half of those late and failed a
    // probe the game then held.
    const auto since_switch = now - multiplier_changed_at_;
    const bool judged = !probe_switch || (since_switch >= settle + auto_settle_ / 4 &&
                                          (cadence_.size() >= kAutoProbeJudgedFrames || since_switch >= auto_settle_));
    const std::uint32_t before = auto_multiplier_;
    // A halving hold ends early where the game's own waits clear the report.
    // Only waits asked for well within a refresh, or released at the next
    // one, can (NoteRuntimeHalvingLocked): a game taking longer than a
    // refresh for every frame, the kind Auto engages for, gives none that
    // do, and its hold runs out before the presenter thread's waits judge
    // the runtime again.
    const bool held = (inline_hold_until_ && now < *inline_hold_until_) ||
                      (runtime_halving_ && auto_halving_hold_until_ && now < *auto_halving_hold_until_);
    bool gave_up_inline = false;
    bool late_at_ceiling = false;
    std::chrono::duration<double> left_ceiling_after{}; // for the log: a probe down from there
    // More than 30% of the last second's frames missed the cadence: step up.
    if (judged && recent >= 8 && recent_late * 10 > recent * 3) {
        auto_late_percent_ = recent_late * 100 / recent;
        if (!compositor_running_ && auto_multiplier_ > 1 && recent_started_late * 2 >= recent_late) {
            // Inline cycles that start late hold the game's frame thread for
            // the generated slots, so a higher multiplier only slows it more.
            auto_multiplier_ = 1;
            probing_ = false;
            inline_hold_until_ = now + kInlineHold;
            gave_up_inline = true;
        } else if (held && auto_multiplier_ == 1) {
            // Stay at full rate until the hold ends.
        } else if (probing_) {
            // The lower multiplier could not keep up: go back and wait longer
            // before probing again.
            auto_multiplier_ = std::min(probe_from_, auto_memory_ceiling_);
            probing_ = false;
            probe_backoff_ = std::min(probe_backoff_ * 2, auto_initial_backoff_ * (1 << kAutoMaxBackoffDoublings));
        } else if (!memory_pressure && auto_multiplier_ < auto_memory_ceiling_) {
            ++auto_multiplier_;
        } else if (auto_multiplier_ > 1) {
            // Nothing higher to step to, and no probe down from a game that
            // stays late. A game late here as long as a probe backs off,
            // though its own time per frame is within reach of a step down,
            // is held back by the multiplier, not by its own work: probe that
            // step (velocity7 in Star Citizen on Virtual Desktop at 144 Hz,
            // 2026-10-14: 22-24 fps at 4x for 16 s with about 16 ms of its own
            // work a frame, where it had held 48 at 3x). A probe that cannot
            // keep up goes back and doubles the wait, as any probe. Only where
            // the game's own time is measured: inline, its waits say nothing
            // of it.
            late_at_ceiling = true;
            if (!late_at_ceiling_since_ || *late_at_ceiling_since_ < multiplier_changed_at_) {
                late_at_ceiling_since_ = now;
            } else if (now - *late_at_ceiling_since_ >= probe_backoff_ && compositor_running_ &&
                       auto_busy_count_ >= 8 && AutoProbeInReachLocked(auto_multiplier_ - 1, GameGridLocked())) {
                left_ceiling_after = now - *late_at_ceiling_since_;
                probe_from_ = auto_multiplier_;
                --auto_multiplier_;
                probing_ = true;
            }
        }
        probe_not_before_ = now + probe_backoff_;
    } else if (probing_ && judged && now - multiplier_changed_at_ >= auto_probe_confirm_) {
        // Not before it was judged: a probe still too short of frames to
        // fail is not confirmed.
        probing_ = false;
        probe_backoff_ = auto_initial_backoff_;
        probe_not_before_ = now + probe_backoff_;
    } else if (!probing_ && auto_multiplier_ > 1 && now >= probe_not_before_ && !cadence_.empty() &&
               now - cadence_.front().time >= auto_steady_ && steady >= 8 && steady_late * 50 <= steady &&
               AutoProbeInReachLocked(auto_multiplier_ - 1, GameGridLocked())) {
        // Five steady seconds with at most 2% of frames late, and a game
        // whose own time per frame is within reach of one step down: try it.
        // Within reach on the grid the step's frames are timed by
        // (GameGridLocked): where the runtime holds frames to twice its
        // refresh period and reports that, a 2x frame has two such periods,
        // and judged on the refresh period Auto would stay a step higher.
        probe_from_ = auto_multiplier_;
        --auto_multiplier_;
        probing_ = true;
    }
    if (!late_at_ceiling) {
        late_at_ceiling_since_.reset();
    }
    if (auto_multiplier_ != before) {
        multiplier_changed_at_ = now;
        if (probing_) {
            probe_started_at_ = now;
        }
        cadence_.clear();
        if (logger_ && gave_up_inline) {
            logger_->Info("Frame Generation automatic multiplier " + MultiplierText(before) +
                          " -> full rate: generated cycles kept starting late (" + std::to_string(auto_late_percent_) +
                          "% of frames late), so the game's frame takes longer than the runtime allows before the "
                          "first generated slot and Frame Generation would only slow it. Retrying in " +
                          std::to_string(kInlineHold.count()) + " s. Extrapolate on Direct3D 12 presents from its "
                          "own thread and avoids this.");
        } else if (logger_) {
            const bool up = auto_multiplier_ > before;
            logger_->Info("Frame Generation automatic multiplier " +
                          (before == 1 ? std::string("full rate") : MultiplierText(before)) + " -> " +
                          (auto_multiplier_ == 1 ? std::string("full rate") : MultiplierText(auto_multiplier_)) +
                          (up ? " (" + std::to_string(auto_late_percent_) + "% of frames late" +
                                    (probe_backoff_ > auto_initial_backoff_
                                         ? ", next probe in " + std::to_string(probe_backoff_.count() / 1000) + " s"
                                         : std::string()) +
                                    ")."
                              : left_ceiling_after.count() > 0.0
                                    ? " (probing a lower multiplier: " + std::to_string(auto_late_percent_) +
                                          "% of frames late for " + Number(left_ceiling_after.count(), 1) +
                                          " s with no higher one to step to, though the game's own time per frame "
                                          "is within reach of this one)."
                                    : " (probing a lower multiplier)."));
        }
    }
}

bool FrameGenPresenter::AutoProbeInReachLocked(std::uint32_t slots, XrDuration period) const {
    // Inline, the game's frame thread is held for the generated slots, so its
    // waits say nothing of its own time; until measured, probes go as before.
    const std::size_t count = std::min(auto_busy_count_, auto_busy_.size());
    if (!compositor_running_ || count < 8 || period <= 0) {
        return true;
    }
    // A probe a game cannot keep up with costs seconds of late frames (each
    // about 2 s at 37-87% late in DCS on Varjo, 2026-10-14: 17-22 ms a frame
    // at 2x, against 11.1 ms refreshes). Within reach as Dynamic judges its
    // probes: the game's own rate at least that share of the step's.
    std::array<double, 32> sorted = auto_busy_;
    std::nth_element(sorted.begin(), sorted.begin() + count / 2, sorted.begin() + count);
    const double reach = dynamic_policy_.stay_above - kProbeReach;
    return sorted[count / 2] * reach <= static_cast<double>(slots) * static_cast<double>(period) / 1e6;
}

void FrameGenPresenter::UpdateDynamicLocked(Clock::time_point now, XrDuration period) {
    const FrameGenDynamicPolicy& policy = dynamic_policy_;
    const double refresh = 1e9 / static_cast<double>(period);
    if (dynamic_standby_) {
        // A gap (a pause, a loading stall) is not the game's rate; a hitch is.
        if (last_app_end_ && now - *last_app_end_ > std::chrono::nanoseconds(PauseLimitLocked())) {
            dynamic_below_since_.reset();
        }
        // Nothing engages from display times that do not advance with the
        // time (Varjo's after a loading stall): the game's rate is judged
        // afresh once they do.
        if (clock_displaced_) {
            dynamic_below_since_.reset();
            dynamic_reason_ = RuntimeClockTextLocked();
            return;
        }
        // Right after a switch the game's frames carry its cost; after a
        // stand-down, nothing engages until the hold ends.
        if (now - multiplier_changed_at_ < policy.settle || now < dynamic_hold_until_) {
            dynamic_below_since_.reset();
            return;
        }
        const double rate = RateSince(app_frames_, now, multiplier_changed_at_);
        const double share = dynamic_probing_ ? policy.stay_above : policy.engage_below;
        if (rate > 0.0 && rate < share * refresh) {
            if (!dynamic_below_since_) {
                dynamic_below_since_ = now;
            } else if (now - *dynamic_below_since_ >= policy.engage_after) {
                DynamicEngageLocked(now, rate, refresh);
            }
            return;
        }
        dynamic_below_since_.reset();
        if (dynamic_probing_ && now - multiplier_changed_at_ >= policy.settle + policy.engage_after) {
            // The probe held: the game keeps the rate alone.
            dynamic_probing_ = false;
            dynamic_probe_after_ = policy.probe_after;
            dynamic_hold_ = policy.hold;
            dynamic_reason_ = "the game runs at " + Number(rate, 1) + " fps";
            if (logger_) {
                logger_->Info("Frame Generation (dynamic): the game runs at " + Number(rate, 1) +
                              " fps: stays standing by.");
            }
        }
        return;
    }
    if (!compositor_running_) {
        return; // engaging, or presenting inline for a moment: nothing to judge
    }
    // The runtime holds the frames presented to half its refresh rate (or
    // less): engaged gains nothing while that lasts. Standing by, frames pass
    // straight through as they would without Frame Generation. Only the
    // presenter thread's waits during this engagement count, and only while
    // halving is still reported; re-engaged while it is, they decide within a
    // quarter of the window, so trying again costs little.
    if (runtime_halving_ && halving_confirmed_at_ && *halving_confirmed_at_ > dynamic_engaged_at_) {
        DynamicStandDownLocked(now, kHalvingStandDown, false);
        return;
    }
    // Engaged: the benefit guard. Windows with a pause, a frame without a
    // projection (loading), a refresh the runtime did not render (the
    // headset off), a presenter restart or a mode switch say nothing about
    // what generation adds and are left out, unless pauses are most of the
    // game's frames (a game too slow to generate for at all).
    if (now - multiplier_changed_at_ < policy.settle) {
        guard_.valid = false;
        return;
    }
    if (!guard_.valid) {
        guard_ = {true, now, totals_};
        guard_capture_done_ms_.clear();
        return;
    }
    if (now - guard_.start < policy.guard_window) {
        return;
    }
    const Totals start = guard_.totals;
    const double seconds = std::chrono::duration<double>(now - guard_.start).count();
    guard_ = {true, now, totals_};
    const std::uint64_t app_frames = totals_.app_frames - start.app_frames;
    const std::uint64_t pauses = totals_.pauses - start.pauses;
    std::vector<double> capture_done = std::move(guard_capture_done_ms_);
    guard_capture_done_ms_.clear();
    NoteGpuEvidenceLocked(app_frames, totals_.late_gpu - start.late_gpu, std::move(capture_done));
    const bool mostly_pauses = app_frames >= 4 && pauses * 2 >= app_frames;
    if ((pauses > 0 && !mostly_pauses) || totals_.non_renderable != start.non_renderable ||
        totals_.unrendered != start.unrendered || totals_.restarts != start.restarts) {
        return;
    }
    const double distinct = PerSecond(totals_.distinct - start.distinct, seconds);
    const double app_rate = PerSecond(app_frames, seconds);
    const double generated = PerSecond(totals_.generated_distinct - start.generated_distinct, seconds);
    const double gap = std::max(1.0, refresh - app_rate);
    const std::uint64_t refreshes = totals_.submissions - start.submissions;
    const std::uint64_t unready = totals_.unready - start.unready;
    const std::uint64_t timed = totals_.analyses_timed - start.analyses_timed;
    const std::uint64_t late = totals_.analyses_late - start.analyses_late;
    // Showing clearly more distinct frames than the game alone did, engaged
    // pays even with few frames generated, real frames on the GPU late or
    // motion measured late: the game, released once a refresh and late at
    // once, fills refreshes it missed standing by (the lead absorbs its frame
    // time's jitter), and a hitch's late frames cost a few refreshes of a
    // window that still shows more. (b), (c) and (d) then judge nothing;
    // probes check whether standing by would hold.
    const bool winning = passthrough_fps_ > 0.0 && distinct >= passthrough_fps_ + policy.distinct_margin_fps;
    const std::array<bool, 4> failing{
        passthrough_fps_ > 0.0 && !winning,
        !winning && generated < policy.generated_share * gap,
        !winning && refreshes > 0 &&
            static_cast<double>(unready) > policy.unready_share * static_cast<double>(refreshes),
        !winning && timed > 0 && static_cast<double>(late) > policy.late_analyses_share * static_cast<double>(timed),
    };
    for (std::size_t i = 0; i < failing.size(); ++i) {
        guard_failing_[i] = failing[i] ? guard_failing_[i] + 1 : 0;
    }
    auto percent = [](std::uint64_t part, std::uint64_t whole) {
        return Number(whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0, 0) + "%";
    };
    const std::array<std::string, 4> reasons{
        "it showed " + Number(distinct, 0) + " distinct frames/s against " + Number(passthrough_fps_, 0) +
            " passing through",
        "it generated too few frames (" + Number(generated, 1) + "/s of a " + Number(gap, 1) + "/s gap)",
        "real frames reached the GPU too late (" + percent(unready, refreshes) + " of refreshes)",
        "motion was measured too late (" + percent(late, timed) + " of analyses)",
    };
    // Two failing windows in a row: one can be a hitch. A cause failing now
    // (frames on the GPU too late, motion measured too late, too few
    // generated) is named rather than its effect (no more distinct frames
    // than the game alone).
    const std::array<std::size_t, 3> causes{2, 3, 1};
    for (const std::size_t i : causes) {
        if (guard_failing_[i] >= 2) {
            DynamicStandDownLocked(now, reasons[i], false);
            return;
        }
    }
    if (guard_failing_[0] >= 2) {
        const auto cause = std::find_if(causes.begin(), causes.end(), [&](std::size_t i) { return failing[i]; });
        DynamicStandDownLocked(now, reasons[cause != causes.end() ? *cause : 0], false);
        return;
    }
    if (std::none_of(failing.begin(), failing.end(), [](bool value) { return value; })) {
        if (now - dynamic_engaged_at_ >= kDynamicLongEngagement) {
            dynamic_hold_ = policy.hold;
        }
        // Engaged, the game runs at its own rate: one well below what a
        // probe needs standing by would only lose its generated frames for
        // a while and engage again.
        const bool within_reach = app_rate >= (policy.stay_above - kProbeReach) * refresh;
        if (now - dynamic_engaged_at_ >= dynamic_probe_after_ && within_reach) {
            DynamicStandDownLocked(now,
                                   "probes standing by after " +
                                       Number(std::chrono::duration<double>(now - dynamic_engaged_at_).count(), 0) +
                                       " s engaged",
                                   true);
        }
    }
}

void FrameGenPresenter::DynamicEngageLocked(Clock::time_point now, double rate, double refresh) {
    const FrameGenDynamicPolicy& policy = dynamic_policy_;
    const bool probe_failed = dynamic_probing_;
    const double below = (probe_failed ? policy.stay_above : policy.engage_below) * refresh;
    dynamic_standby_ = false;
    dynamic_below_since_.reset();
    passthrough_fps_ = rate;
    dynamic_engaged_at_ = now;
    multiplier_changed_at_ = now;
    cadence_.clear();
    if (standby_since_) {
        standby_seconds_ += std::chrono::duration<double>(now - *standby_since_).count();
        standby_since_.reset();
    }
    guard_.valid = false;
    guard_failing_ = {};
    ++dynamic_engagements_;
    if (probe_failed) {
        dynamic_probing_ = false;
        dynamic_probe_after_ = std::min(dynamic_probe_after_ * 2, policy.probe_max);
    }
    dynamic_reason_ = "the game ran at " + Number(rate, 1) + " fps (below " + Number(below, 1) + " of " +
                      Number(refresh, 1) + ")";
    if (logger_) {
        logger_->Info(
            std::string("Frame Generation (dynamic) engages") + (probe_failed ? " again" : "") + ": the game ran at " +
            Number(rate, 1) + " fps (below " + Number(below, 1) + " of " + Number(refresh, 1) + ") for " +
            Number(std::chrono::duration<double>(policy.engage_after).count(), 1) +
            " s; generated frames fill the refreshes it misses" +
            (probe_failed ? " (next probe in " + std::to_string(dynamic_probe_after_.count() / 1000) + " s)." : "."));
    }
}

void FrameGenPresenter::DynamicStandDownLocked(Clock::time_point now, const std::string& reason, bool probe) {
    const FrameGenDynamicPolicy& policy = dynamic_policy_;
    dynamic_standby_ = true;
    dynamic_below_since_.reset();
    multiplier_changed_at_ = now;
    cadence_.clear();
    standby_since_ = now;
    guard_.valid = false;
    guard_failing_ = {};
    dynamic_reason_ = reason;
    if (probe) {
        // No hold: the probe engages again as soon as the game falls short.
        dynamic_probing_ = true;
        ++dynamic_probes_;
        if (logger_) {
            logger_->Info("Frame Generation (dynamic) " + reason + ": frames pass straight through while it checks "
                          "whether the game holds the refresh rate alone.");
        }
        return;
    }
    ++dynamic_stand_downs_;
    dynamic_probing_ = false;
    const std::chrono::milliseconds hold = dynamic_hold_;
    dynamic_hold_until_ = now + hold;
    if (now - dynamic_engaged_at_ < kDynamicShortEngagement) {
        dynamic_hold_ = std::min(dynamic_hold_ * 2, policy.hold_max);
    }
    if (logger_) {
        logger_->Info("Frame Generation (dynamic) stands down: " + reason +
                      "; frames pass straight through, retrying in " +
                      Number(std::chrono::duration<double>(hold).count(), 0) + " s.");
    }
}

void FrameGenPresenter::DynamicReenableLocked(Clock::time_point now) {
    // What the game ran at before says nothing about it now: it engages
    // again only after running below the refresh rate a while, and the
    // guard and probes start over. A stand-down's hold stays.
    if (!dynamic_standby_) {
        dynamic_standby_ = true;
        standby_since_ = now;
        // Standing by now: the engagement's reason (the game's rate then)
        // would read as why it stands by.
        dynamic_reason_.clear();
    }
    dynamic_below_since_.reset();
    dynamic_probing_ = false;
    multiplier_changed_at_ = now;
    cadence_.clear();
    guard_.valid = false;
    guard_failing_ = {};
}

void FrameGenPresenter::MaybeRetryLocked() {
    const auto now = Clock::now();
    if (automatic_retries_ && state_ != FrameGenState::kPaused && last_pause_at_ &&
        now - *last_pause_at_ >= retry_restore_after_) {
        // The retries held: no pause for that long. A pause later in a long
        // session (a loading screen's hitch hours on) retries by itself
        // again, rather than staying paused for the rest of the session.
        if (logger_) {
            logger_->Info("Frame Generation has run " +
                          Number(std::chrono::duration<double>(now - *last_pause_at_).count(), 0) +
                          " s since it resumed after its last pause: a later pause retries by itself again (up to " +
                          std::to_string(retry_attempts_) + " times).");
        }
        automatic_retries_ = 0;
    }
    if (state_ != FrameGenState::kPaused || !paused_at_ || automatic_retries_ >= retry_attempts_ ||
        now - *paused_at_ < retry_delay_) {
        return;
    }
    ++automatic_retries_;
    paused_at_.reset();
    last_pause_at_ = now; // the quiet stretch starts when it runs again
    consecutive_failures_ = consecutive_demotions_ = 0;
    healthy_streak_ = 0;
    DynamicReenableLocked(Clock::now());
    if (logger_) {
        logger_->Info("Frame Generation retrying after a pause (" + std::to_string(automatic_retries_) + " of " +
                      std::to_string(retry_attempts_) + ").");
    }
    SetStateLocked(user_enabled_ && configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
}

void FrameGenPresenter::SetStateLocked(FrameGenState state, const std::string& reason) {
    if (state == state_ && reason == reason_) {
        return;
    }
    const FrameGenState previous = state_;
    state_ = state;
    reason_ = reason;
    if (!logger_) {
        return;
    }
    if (state == FrameGenState::kActive && full_rate_ && !reason.empty()) {
        logger_->Info("Frame Generation standing by: " + reason + ".");
    } else if (state == FrameGenState::kActive && full_rate_ && DynamicLocked() && dynamic_standby_) {
        // The game's rate right after a switch is the engaged one's, not its own.
        const auto now = Clock::now();
        const double refresh = refresh_period_ > 0 ? 1e9 / static_cast<double>(refresh_period_) : 0.0;
        const double rate = now - multiplier_changed_at_ >= std::chrono::seconds(1) ? RatePerSecond(app_frames_, now) : 0.0;
        logger_->Info("Frame Generation (dynamic) standing by: " +
                      (rate > 0.0 ? "the game runs at " + Number(rate, 1) + " fps" : std::string("frames pass straight through")) +
                      " (engages below " + Number(dynamic_policy_.engage_below * refresh, 1) + ").");
    } else if (state == FrameGenState::kActive && full_rate_) {
        logger_->Info("Frame Generation standing by: the application holds full refresh.");
    } else if (state == FrameGenState::kActive && DynamicLocked()) {
        logger_->Info(std::string("Frame Generation engaged: ") + ToString(settings_.synthesis_mode) +
                      " dynamic, the application runs at its own rate and generated frames fill the refreshes "
                      "between its frames.");
    } else if (state == FrameGenState::kActive) {
        logger_->Info(std::string("Frame Generation engaged: ") + ToString(settings_.synthesis_mode) + " " +
                      MultiplierText(EffectiveMultiplierLocked()) + ", application held at 1/" +
                      std::to_string(EffectiveMultiplierLocked()) + " of the runtime refresh rate.");
    } else if (state == FrameGenState::kPaused || state == FrameGenState::kUnavailable) {
        logger_->Error(std::string("Frame Generation ") + ToString(state) + ": " + reason + ".");
    } else if (previous == FrameGenState::kActive) {
        logger_->Info(std::string("Frame Generation disengaged (") + ToString(state) + ").");
    }
}

void FrameGenPresenter::PauseLocked(const std::string& reason) {
    if (unsupported_composition_) {
        // A failed pass-through submission must not turn a permanent
        // compatibility refusal into a pause that Toggle/retry can re-arm.
        if (logger_) logger_->Error("Frame Generation: " + reason + ".");
        return;
    }
    SetStateLocked(FrameGenState::kPaused, reason);
    paused_at_ = last_pause_at_ = Clock::now();
    healthy_streak_ = 0;
}

void FrameGenPresenter::UnavailableLocked(const std::string& reason) {
    SetStateLocked(FrameGenState::kUnavailable, reason);
}

void FrameGenPresenter::InvalidateContinuityLocked() {
    last_real_.valid = false;
    extrapolated_.valid = false;
    ForgetEngineFramesLocked();
}

void FrameGenPresenter::ForgetEngineFramesLocked() {
    engine_frame_ = {};
    engine_previous_ = {};
    motion_targets_.clear();
    engine_released_.clear();
}

std::optional<FrameGenEngineGateRecord> FrameGenPresenter::EngineGateRecordLocked(const EngineFrame& frame) const {
    if (!frame.valid) {
        return std::nullopt;
    }
    FrameGenEngineGateRecord record;
    record.display_time = frame.frame.display_time;
    record.projection_frame = frame.frame.projection_frame;
    record.complete = frame.frame.complete;
    record.cut = frame.cut;
    record.sequence = frame.sequence;
    for (const FrameGenEngineMotionView& view : frame.frame.views) {
        FrameGenEngineGateView gate;
        gate.view = view.view;
        gate.vectors = reinterpret_cast<std::uint64_t>(view.vectors.swapchain);
        gate.rect = {view.vectors.imageRect.offset.x, view.vectors.imageRect.offset.y, view.vectors.imageRect.extent.width,
                     view.vectors.imageRect.extent.height};
        const auto vectors = swapchains_.find(view.vectors.swapchain);
        gate.readable = view.problems == 0 && vectors != swapchains_.end() && vectors->second.sample_count == 1 &&
                        IsEngineMotionFormat(vectors->second.format);
        gate.frame_skip = view.frame_skip;
        gate.depth = view.depth_encoding;
        record.views.push_back(gate);
    }
    return record;
}

bool FrameGenPresenter::EngineGatesLocked(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                                          const FrameGenEngineGateRecord* record, const FrameGenEngineGateRecord* previous,
                                          const FrameGenEngineGateFacts& facts, bool flipped) {
    const std::uint32_t gates = FrameGenEngineGates(record, previous, facts);
    const bool own = record && record->display_time == facts.end_time;
    if ((gates & kFrameGenEngineOneEye) != 0 && logger_ && !logged_engine_one_eye_) {
        logged_engine_one_eye_ = true;
        logger_->Info("Frame Generation engine motion: the game writes one eye's motion vectors alone (the other's are "
                      "exactly zero everywhere the census reads): neither eye's are used while that holds (gate "
                      "oneEye).");
    }
    ++engine_window_.frames;
    engine_window_.structs += own && record->complete ? 1 : 0;
    for (std::size_t i = 0; i < kFrameGenEngineGateCount; ++i) {
        engine_window_.gated[i] += (gates >> i) & 1u;
    }
    engine_window_.flips += flipped ? 1 : 0;
    // Kept with the frame's record (unless a pause forgot it meanwhile): the
    // next frame pairs with it by this frame's sequence.
    if (!own || !engine_frame_.valid || engine_frame_.frame.display_time != facts.end_time) {
        return false;
    }
    EngineFrame& kept = engine_frame_;
    // Frames no gate holds back, and those whose vectors were read on every
    // view (stage 2: captured, at their release or the frame's end).
    if (gates == 0) {
        ++engine_window_.open;
        engine_window_.captured += !kept.captured.empty() && std::all_of(kept.captured.begin(), kept.captured.end(),
                                                                         [](std::uint8_t c) { return c != 0; })
                                       ? 1
                                       : 0;
    }
    kept.sequence = frame.sequence;
    kept.gates = gates;
    kept.unsettled = (gates & (kFrameGenEngineCut | kFrameGenEngineDecode | kFrameGenEngineRecreate)) != 0 ||
                     facts.paused || flipped;
    // Pivot's change to each view, from the pose the game rendered with and
    // the pose Frame Generation is told. Calibration measures the vectors
    // against the told poses, so only while that change holds still: the
    // game's own poses move the scene as its vectors do.
    kept.pivot.clear();
    for (const FrameGenEngineMotionView& view : kept.frame.views) {
        if (view.view >= projection.viewCount) {
            kept.pivot.clear();
            break;
        }
        kept.pivot.push_back(FrameGenDeclaredChange(view.game, Geometry(projection.views[view.view])));
    }
    const EngineFrame& before = engine_previous_;
    const bool paired = (gates & (kFrameGenEngineNoStruct | kFrameGenEnginePairing)) == 0 && before.valid &&
                        !kept.pivot.empty() && before.pivot.size() == kept.pivot.size();
    kept.pivot_moved = false;
    kept.paired_game.clear();
    for (std::size_t i = 0; paired && i < kept.pivot.size(); ++i) {
        kept.pivot_moved = kept.pivot_moved || FrameGenDeclaredChangeMoved(before.pivot[i], kept.pivot[i]);
        kept.paired_game.push_back(before.frame.views[i].game);
    }
    engine_window_.pivot_moving += kept.pivot_moved ? 1 : 0;
    // No cut, depth encoding change, pause or new swapchain in this frame or
    // the one before. (Settling and the engine path's frames are its use's
    // gates, not its calibration's.)
    kept.calibrate = paired && !kept.pivot_moved && !kept.unsettled && !before.unsettled &&
                     (gates & ~(kFrameGenEngineSettle | kFrameGenEngineUncaptured)) == 0;
    engine_window_.calibrate += kept.calibrate ? 1 : 0;
    return kept.calibrate;
}

void FrameGenPresenter::EngineRequestsLocked(const PendingFrame& frame, bool calibrate,
                                             std::vector<FrameGenViewSubmission>& submissions) {
    const EngineFrame& now = engine_frame_;
    const EngineFrame& before = engine_previous_;
    // The frame's own record, no gate holding it back (its pair, the record
    // before, among them).
    if (!now.valid || now.frame.display_time != frame.end_time || now.gates != 0 || !before.valid ||
        now.paired_game.size() != now.frame.views.size()) {
        return;
    }
    // The lag test: both frames may calibrate (Pivot's change held still
    // over the three), and the head's turn changed by 2 degrees a frame.
    const bool lag = calibrate && before.calibrate && before.paired_game.size() == now.frame.views.size() &&
                     !now.frame.views.empty() &&
                     FrameGenAngularAcceleration(now.frame.views.front().game, now.paired_game.front(),
                                                 before.paired_game.front()) > 2.0;
    for (std::size_t i = 0; i < now.frame.views.size(); ++i) {
        const FrameGenEngineMotionView& view = now.frame.views[i];
        if (view.view >= submissions.size()) continue;
        FrameGenViewRequest& request = submissions[view.view].request;
        request.engine_motion = true;
        request.game_newest = view.game;
        request.game_previous = now.paired_game[i];
        request.engine_lag = lag;
        if (lag) request.game_before = before.paired_game[i];
    }
}

void FrameGenPresenter::EngineDeltaLocked(const PendingFrame& frame, std::vector<FrameGenViewSubmission>& submissions) {
    const EngineFrame& now = engine_frame_;
    // The frame's own record, paired with the frame before's (its game
    // poses), no cut between them and not repeating the frame before.
    constexpr std::uint32_t kHeldBack = kFrameGenEngineNoStruct | kFrameGenEnginePairing | kFrameGenEngineCut |
                                        kFrameGenEngineSkip;
    if (!now.valid || now.frame.display_time != frame.end_time || (now.gates & kHeldBack) != 0 ||
        now.frame.views.empty() || now.paired_game.size() != now.frame.views.size()) {
        return;
    }
    // appSpaceDeltaPose is the frame's: every view carries the same, or the
    // record is not to be read by it.
    const XrPosef& delta = now.frame.views.front().app_delta;
    for (const FrameGenEngineMotionView& view : now.frame.views) {
        const XrPosef& own = view.app_delta;
        if (own.orientation.x != delta.orientation.x || own.orientation.y != delta.orientation.y ||
            own.orientation.z != delta.orientation.z || own.orientation.w != delta.orientation.w ||
            own.position.x != delta.position.x || own.position.y != delta.position.y || own.position.z != delta.position.z) {
            return;
        }
    }
    for (std::size_t i = 0; i < now.frame.views.size(); ++i) {
        const FrameGenEngineMotionView& view = now.frame.views[i];
        if (view.view >= submissions.size()) continue;
        FrameGenViewRequest& request = submissions[view.view].request;
        request.app_delta = true;
        request.delta_orientation = {delta.orientation.x, delta.orientation.y, delta.orientation.z, delta.orientation.w};
        request.delta_position = {delta.position.x, delta.position.y, delta.position.z};
        request.game_newest = view.game;
        request.game_previous = now.paired_game[i];
    }
}

void FrameGenPresenter::LogEngineSummaryLocked(double seconds) {
    const EngineWindow& window = engine_window_;
    if (!logger_ || window.frames == 0 || (engine_noted_ == 0 && window.flips == 0)) {
        return;
    }
    // frames: real frames generated from; structs: their records had a
    // structure on every view; gated: frames each gate held the vectors back
    // in (a frame in several); flips: frames with a view's depth decoded as
    // the frame before described it; cuts: camera cuts among the records
    // (passing frames through too); pivotMoving: frames Pivot's change to
    // the views moved in; calibrate: frames that may calibrate; captured:
    // frames no gate held back whose vectors were read on every view (the
    // census, from stage 2 their capture), of all frames no gate held back.
    std::string gated;
    std::string names;
    for (std::size_t i = 0; i < kFrameGenEngineGateCount; ++i) {
        gated += (i ? "/" : "") + std::to_string(window.gated[i]);
        names += (i ? "/" : "") + std::string(FrameGenEngineGateName(i));
    }
    logger_->Info("Frame Generation engine motion (" + Number(seconds, 0) + " s): frames=" + std::to_string(window.frames) +
                  ", structs=" + std::to_string(window.structs) + ", gated=" + gated + " (" + names +
                  "), flips=" + std::to_string(window.flips) + ", cuts=" + std::to_string(window.cuts) +
                  ", pivotMoving=" + std::to_string(window.pivot_moving) +
                  ", calibrate=" + std::to_string(window.calibrate) + ", captured=" + std::to_string(window.captured) +
                  "/" + std::to_string(window.open) + ".");
    // Stage 4: the world fit against the application's movement (the
    // calibration now: its state, the sign and s_t it has, the correlations
    // and frames it has them from), and since the last summary: frames the
    // movement stood in for the fit, and the watchdog's checks, those 25%
    // off and drops back to the fit.
    const FrameGenWorldDeltaStatus& delta = world_delta_;
    if (delta.mode != FrameGenWorldDeltaMode::kOff) {
        const FrameGenWorldDeltaStatus& logged = world_delta_logged_;
        logger_->Info(
            "Frame Generation world vs delta (" + Number(seconds, 0) + " s): " + ToString(delta.mode) + ", " +
            (delta.locked ? std::string(delta.fixed ? "fixed" : "locked") + ": rotation " +
                                (delta.sign < 0 ? "conjugated" : "as given") + ", s_t " + Number(delta.scale, 3)
                          : std::string("calibrating")) +
            (delta.unread ? " (no fits read back: the fit only)" : "") + "; rotation correlation " +
            Number(delta.rotation_correlation[0], 3) + " as given/" + Number(delta.rotation_correlation[1], 3) +
            " conjugated, translation " + Number(delta.translation_correlation, 3) + ", fitted s_t " +
            Number(delta.fitted_scale, 3) + " over " + std::to_string(delta.informative) + " informative frames (" +
            std::to_string(delta.turning) + " turning) of " + std::to_string(delta.frames) +
            "; set=" + std::to_string(delta.set - logged.set) + ", checks=" + std::to_string(delta.checks - logged.checks) +
            " (failed " + std::to_string(delta.failed - logged.failed) + ", last " + Number(100.0 * delta.last_error, 0) +
            "% off), drops=" + std::to_string(delta.drops) + ".");
        world_delta_logged_ = delta;
    }
    // Stage 3: the validator now (its state, the convention under test or
    // locked, the last batch's fit), and since the last summary: its frames
    // and batches, the views judged and those their verdict held back by
    // reason, the vectors' distance from the measured flow by band (pixels),
    // and the luma error of the vectors against the measured flow's, no
    // motion's, and of the field before against this frame's.
    const FrameGenEngineValidation& now = engine_validation_;
    const FrameGenEngineValidation& last = engine_validation_logged_;
    if (engine_requested_ < FrameGenEngineMode::kObserve || now.frames == 0) {
        return;
    }
    auto ratio = [](double numerator, double numerator_samples, double denominator, double denominator_samples) {
        return numerator_samples > 0.0 && denominator_samples > 0.0 && denominator > 0.0
                   ? Number((numerator / numerator_samples) / (denominator / denominator_samples), 3)
                   : std::string("-");
    };
    std::string reasons;
    for (std::size_t i = 0; i < kFrameGenEngineVerdictReasons; ++i) {
        reasons += (i ? ", " : "") + std::string(FrameGenEngineVerdictName(i)) + " " +
                   std::to_string(now.reasons[i] - last.reasons[i]);
    }
    std::string bands;
    static constexpr const char* kBands[] = {"<4 m", ">=30 m", "sky"};
    for (std::size_t band = 0; band < 3; ++band) {
        const double samples = now.epe_samples[band] - last.epe_samples[band];
        bands += (band ? ", " : "") + std::string(kBands[band]) + " " +
                 (samples > 0.0 ? Number((now.epe[band] - last.epe[band]) / samples, 2) + " px" : std::string("-")) +
                 " (" + Number(samples, 0) + ")";
    }
    const double textured = now.textured - last.textured;
    auto percent = [](std::uint64_t part, std::uint64_t whole) {
        return whole ? Number(100.0 * static_cast<double>(part) / static_cast<double>(whole), 1) : std::string("-");
    };
    logger_->Info(
        "Frame Generation engine convention (" + Number(seconds, 0) + " s): " + ToString(now.state) + ", " +
        (now.name.empty() ? std::string("gains ") + Number(now.convention.gain[0], 4) + "/" +
                                Number(now.convention.gain[1], 4)
                          : now.name) +
        ", " + ToString(now.convention.camera) + (now.convention.jitter ? ", jittered" : "") +
        (now.convention.offset[0] != 0.0f || now.convention.offset[1] != 0.0f
             ? ", offset " + Number(now.convention.offset[0], 5) + "/" + Number(now.convention.offset[1], 5)
             : std::string()) +
        (now.fixed ? " (fixed)" : now.prior ? " (prior)" : "") + "; frames=" + std::to_string(now.frames - last.frames) +
        ", batches=" + std::to_string(now.batches - last.batches) + " (last: gain " + Number(now.gain[0], 4) + "/" +
        Number(now.gain[1], 4) + ", b/a " + Number(now.ratio[0], 2) + "/" + Number(now.ratio[1], 2) + ", corr2 " +
        Number(now.corr2[0], 3) + "/" + Number(now.corr2[1], 3) + ", jitter " + Number(now.jitter_px[0], 2) + "/" +
        Number(now.jitter_px[1], 2) + " px, lag " + std::to_string(now.lag_batches) + (now.lag ? " (late)" : "") +
        "); verdicts=" + std::to_string(now.verdicts - last.verdicts) + ", heldBack=" +
        std::to_string(now.invalid - last.invalid) + " (" + reasons + "); EPE vs measured " + bands +
        "; luma error engine/measured " +
        ratio(now.variant - last.variant, textured, now.measured_cost - last.measured_cost,
              now.with_measured - last.with_measured) +
        ", engine/head " + ratio(now.variant - last.variant, textured, now.zero - last.zero, textured) +
        ", lag/engine " + ratio(now.lag_cost - last.lag_cost, 1.0, now.lag_variant - last.lag_variant, 1.0) +
        // Stage 5: frames the vectors were merged into, and the share of
        // their textured flow cells the vectors moved.
        (now.merged > last.merged || engine_requested_ >= FrameGenEngineMode::kAssist
             ? "; assist: merged=" + std::to_string(now.merged - last.merged) + ", vectors moved " +
                   (now.textured_cells > last.textured_cells
                        ? Number(100.0 * (now.moved_cells - last.moved_cells) / (now.textured_cells - last.textured_cells), 1) +
                              "% of the textured cells"
                        : std::string("no textured cells"))
             : std::string()) +
        // Stage 6: of a session in Lead's frames, the shares the vectors led
        // and the controller held at Assist (the rest: frames whose evidence
        // was not taken, no convention locked, or interpolating, which Lead
        // leaves to Assist without VECTORXR_FRAMEGEN_ENGINE_INTERPOLATE=lead),
        // those led with the optical flow hardware chosen and idle, and the
        // times the vectors were held at Assist.
        (window.lead_frames > 0 || engine_requested_ == FrameGenEngineMode::kLead
             ? "; lead: led " + percent(window.led, window.lead_frames) + "% of " + std::to_string(window.lead_frames) +
                   " frames, held at assist " + percent(window.held_at_assist, window.lead_frames) +
                   "%, hardwareIdle=" + std::to_string(window.hardware_idle) +
                   ", demotions=" + std::to_string(window.demotions)
             : std::string()) +
        // Stage 7: views' frames written with the engine motion map (those
        // that read one built for another frame of the same real frame again),
        // and frames moved past one interval of their real frame.
        (engine_map_ != FrameGenEngineMap::kOff
             ? "; map (" + std::string(ToString(engine_map_)) + "): " + std::to_string(now.maps - last.maps) +
                   " view frames, read again " + std::to_string(now.maps_reused - last.maps_reused) +
                   ", past one interval " + std::to_string(window.reached)
             : std::string()) +
        ".");
    engine_validation_logged_ = now;
}

bool FrameGenPresenter::ResearchFileInUse() const {
    std::scoped_lock lock(mutex_);
    return research_file_;
}

bool FrameGenPresenter::DepthBridgeProbe() const {
    std::scoped_lock lock(mutex_);
    return depth_bridge_probe_;
}

bool FrameGenPresenter::SupersedeOwnFramesLocked() {
    if (std::this_thread::get_id() != end_thread_) {
        return false;
    }
    // What the thread does next decides what these frames were: a begin
    // over them abandons them (OpenXR discards them), an end ends the oldest
    // after all (a wait may return before the previous frame's end).
    // Superseded frames never run extra runtime frames: they hold no wait.
    // Only the frames it began itself: one another thread began may have its
    // end on the way in from there (that thread's own order decides).
    const auto self = std::this_thread::get_id();
    bool engaged = false;
    for (PendingFrame& frame : frames_) {
        if (frame.begun && !frame.claimed && !frame.superseded && frame.begin_thread == self) {
            frame.superseded = true;
            if (frame.engaged && !frame.demoted) {
                frame.demoted = frame.demotion_owed = true;
                engaged = true;
            }
        }
    }
    for (ThreadedFrame& frame : threaded_frames_) {
        if (frame.begun && !frame.claimed && frame.begin_thread == self) {
            frame.superseded = true;
        }
    }
    return engaged;
}

std::size_t FrameGenPresenter::DropAbandonedLocked(bool threaded) {
    // Superseded frames are older than the one beginning, and their thread
    // began this one instead of ending them: OpenXR discards them, and their
    // end never comes. Only those: a begin on another thread can come while
    // a frame's end is on its way in (MSFS 2024 begins its next frame on its
    // wait thread while its render thread ends the last), before that end
    // claims the frame.
    std::size_t dropped = 0;
    if (threaded) {
        for (auto it = threaded_frames_.begin(); it != threaded_frames_.end();) {
            if (it->begun && it->superseded && !it->claimed) {
                BridgeAbandonedLocked(it->id);
                it = threaded_frames_.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
    } else {
        for (auto it = frames_.begin(); it != frames_.end();) {
            if (it->begun && it->superseded && !it->claimed) {
                BridgeAbandonedLocked(it->sequence);
                it = frames_.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
    }
    NoteAbandonedLocked(dropped);
    return dropped;
}

std::size_t FrameGenPresenter::DropOwnDiscardedLocked() {
    // Frames this thread began before the newest it began and has not ended:
    // each later begin on this thread discarded them (OpenXR), so this end
    // is the newest one's. Superseded frames are dropped at that begin;
    // these were begun before any end showed which thread ends frames (a
    // loading screen's waits and begins at a session's start).
    const auto self = std::this_thread::get_id();
    auto own = [&](const PendingFrame& frame) { return frame.begun && !frame.claimed && frame.begin_thread == self; };
    const auto newest = std::find_if(frames_.rbegin(), frames_.rend(), own);
    if (newest == frames_.rend()) {
        return 0;
    }
    const std::uint64_t keep = newest->sequence;
    std::size_t dropped = 0;
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (own(*it) && it->sequence != keep) {
            BridgeAbandonedLocked(it->sequence);
            it = frames_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    NoteAbandonedLocked(dropped);
    return dropped;
}

void FrameGenPresenter::BridgeAbandonedLocked(std::uint64_t sequence) {
    // Nothing was shown between the real frames on either side of an
    // abandoned one: they stay continuous.
    if (last_real_.valid && last_real_.sequence + 1 == sequence) {
        last_real_.sequence = sequence;
    }
}

void FrameGenPresenter::NoteAbandonedLocked(std::size_t count) {
    if (count == 0) {
        return;
    }
    // No runtime call is made for them, and the game's run of steady frames
    // goes on.
    abandoned_frames_ += count;
    abandons_frames_ = true;
    if (logger_ && !logged_abandoned_frames_) {
        logged_abandoned_frames_ = true;
        logger_->Info("Frame Generation: the application begins frames over frames it has not ended (OpenXR "
                      "discards those; OpenComposite does this when a game asks for poses again before submitting "
                      "both eyes); they are dropped.");
    }
    idle_cv_.notify_all();
    WakeSlots(); // the presenter thread no longer waits for them
}

void FrameGenPresenter::LogMemoryBudgetLocked() {
    if (!logger_ || !gpu_) return;
    if (const auto memory = gpu_->MemoryBudget()) {
        constexpr double megabytes = 1024.0 * 1024.0;
        // Frame Generation's own share: its working images as allocated (once
        // it has generated) and the output swapchain images it created.
        const std::string own = working_bytes_ || output_bytes_
                                    ? "; Frame Generation working " + Number(working_bytes_ / megabytes, 0) +
                                          " MB, output images " + Number(output_bytes_ / megabytes, 0) + " MB"
                                    : std::string();
        logger_->Info("Frame Generation process video memory: usage=" + Number(memory->usage_bytes / megabytes, 0) +
                      " MB, budget=" + Number(memory->budget_bytes / megabytes, 0) + " MB" + own +
                      (memory->usage_bytes > memory->budget_bytes ? " (over budget)." : "."));
    }
}

void FrameGenPresenter::NoteFailureLocked(const std::string& error) {
    ++failures_;
    if (++consecutive_failures_ >= kFailuresBeforePause) {
        PauseLocked("generation failed: " + error);
    } else if (logger_) {
        logger_->Error("Frame Generation could not generate a frame: " + error);
    }
}

XrResult FrameGenPresenter::WaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state,
                                      const FrameGenRuntimeCalls& calls) {
    bool superseded = false;
    if (state) {
        std::unique_lock lock(mutex_);
        MaybeRetryLocked();
        superseded = SupersedeOwnFramesLocked();
        if (!compositor_running_ && compositor_.joinable()) {
            // The presenter thread stopped itself after a runtime error.
            StopCompositorLocked(lock);
        }
        const bool inline_engaged = std::any_of(frames_.begin(), frames_.end(), [](const PendingFrame& frame) {
            return frame.engaged && !frame.demoted;
        });
        // A thread that waits again before ending its own frame would wait
        // for itself while the presenter thread waits for that frame to end:
        // such apps stay inline.
        const bool own_frame_open = std::this_thread::get_id() == end_thread_ && !frames_.empty();
        const bool threaded =
            ThreadedEligibleLocked() &&
            (compositor_running_ ? StayThreadedLocked() : WantsEngagementLocked() && !inline_engaged && !own_frame_open);
        if (threaded) {
            if (const auto result = ThreadedWaitLocked(lock, state, calls)) {
                return *result;
            }
            if (compositor_.joinable()) {
                // The presenter thread stopped (or is stopping) under this
                // wait: its frames in flight go before the runtime is waited
                // inline.
                StopCompositorLocked(lock);
            }
        } else if (compositor_running_) {
            ExitThreadedLocked(lock);
        }
    }
    bool engage = false;
    std::uint32_t multiplier = 2;
    std::uint64_t sequence = 0;
    {
        std::unique_lock lock(mutex_);
        MaybeRetryLocked();
        engage = WantsEngagementLocked();
        bool demoted = false;
        auto demote = [&] {
            for (PendingFrame& frame : frames_) {
                if (frame.engaged && !frame.demoted && !frame.claimed) {
                    frame.demoted = true;
                    ++demoted_;
                    demoted = true;
                }
            }
        };
        // The thread that ends frames is waiting again before ending its
        // own: holding it would only stall until the timeout. A frame it
        // began is superseded (never held for, counted only if it ends); one
        // not even begun is demoted. Apps that wait on one thread and end on
        // another take the hold.
        if (std::this_thread::get_id() == end_thread_) {
            superseded = SupersedeOwnFramesLocked() || superseded;
            demote();
        }
        // An engaged frame in progress owns the runtime until its extra
        // wait/begin/end has finished. One whose end has not started yet is
        // demoted after a short hold; one already in its end is finishing a
        // runtime call and is waited for.
        auto blocking = [&] {
            return std::any_of(frames_.begin(), frames_.end(),
                               [](const PendingFrame& frame) { return frame.engaged && !frame.demoted; });
        };
        if (blocking() && !idle_cv_.wait_for(lock, kOverlapWait, [&] { return !blocking(); })) {
            demote();
            // What still blocks is inside its own runtime calls; a second
            // runtime wait now would break frame ordering, so let it finish.
            while (!idle_cv_.wait_for(lock, kClaimedFrameWait, [&] { return !blocking(); })) {
                if (logger_ && !logged_claimed_wait_) {
                    logged_claimed_wait_ = true;
                    logger_->Error("Frame Generation: a generated frame has been inside the runtime for over 5 s.");
                }
            }
        }
        if (demoted) {
            // A demoted frame shows in slot k alone; this frame passes through
            // so the runtime sees a clean single frame next.
            engage = false;
            InvalidateContinuityLocked();
            if (++consecutive_demotions_ >= kDemotionsBeforePause) {
                PauseLocked("the application starts frames before the previous one ends");
            }
        }
        // Until superseded frames have proved abandoned, this frame passes
        // through as one after a demotion does: if the app ends the frame
        // it superseded instead, that frame is demoted (EndFrame).
        const bool held_back = demoted || (superseded && !abandons_frames_);
        if (held_back) {
            engage = false;
        }
        multiplier = EffectiveMultiplierLocked();
        // Automatic and at full rate: nothing to generate, but the feature is
        // doing its job, so it reads as active. So does standing by while the
        // layer composes the quad views again for a while.
        const bool declined = RunnableLocked() && QuadDeclinedLocked();
        const bool full_rate = !held_back && RunnableLocked() && (multiplier < 2 || declined);
        if (full_rate != full_rate_) {
            full_rate_ = full_rate;
            if (state_ == FrameGenState::kActive) state_ = FrameGenState::kWaiting; // re-log the transition
        }
        if (full_rate) {
            SetStateLocked(FrameGenState::kActive,
                           declined && multiplier >= 2 ? "the layer composes the quad views for a while: composed frames "
                                                         "could not be written, so frames pass straight through"
                                                       : "");
        } else if (!engage && clock_displaced_ && RunnableLocked() && multiplier >= 2) {
            SetStateLocked(FrameGenState::kWaiting, RuntimeClockTextLocked());
        } else if (!engage && (state_ == FrameGenState::kActive ||
                               (state_ == FrameGenState::kWaiting && !reason_.empty() && !clock_displaced_))) {
            SetStateLocked(IdleStateLocked(), "");
        }
        sequence = ++wait_sequence_;
    }
    const auto called = Clock::now();
    const XrResult result = RuntimeWait(calls, session, info, state);
    if (XR_FAILED(result) || !state) {
        return result;
    }
    std::scoped_lock lock(mutex_);
    const auto returned = Clock::now();
    NotePhaseLocked(called, returned, state->predictedDisplayTime, state->predictedDisplayPeriod);
    NoteRuntimeClockLocked(returned, state->predictedDisplayTime, state->predictedDisplayPeriod,
                           state->shouldRender == XR_TRUE);
    PendingFrame frame;
    frame.slot_time = state->predictedDisplayTime;
    frame.period = state->predictedDisplayPeriod;
    frame.should_render = state->shouldRender == XR_TRUE;
    // Still wanted after the runtime's wait: turned off meanwhile, the frame
    // passes through (and the state stays off).
    // Refused meanwhile on the ending thread (composition metadata), it
    // passes through too: engaging would report the multiplied period to the
    // game and replace the named refusal with an active state.
    frame.engaged = engage && frame.period > 0 && state_ != FrameGenState::kPaused && user_enabled_ && configured_enabled_ &&
                    !unsupported_composition_;
    frame.multiplier = multiplier;
    frame.sequence = sequence;
    frame.wait_return = Clock::now();
    NoteRuntimePeriodLocked(frame.period, returned);
    if (!frame.engaged && frame.should_render) {
        // Passed straight through: the game's own wait.
        NoteRuntimeHalvingLocked(called, returned, frame.slot_time, frame.period, false);
    }
    if (frame.engaged) {
        state->predictedDisplayPeriod = frame.period * static_cast<XrDuration>(multiplier);
        state->predictedDisplayTime = frame.slot_time + frame.period * static_cast<XrDuration>(multiplier - 1) -
                                      DisplayOffsetLocked(state->predictedDisplayPeriod);
        SetStateLocked(FrameGenState::kActive, "");
    }
    frame.reported_period = state->predictedDisplayPeriod;
    state->predictedDisplayTime = std::max(state->predictedDisplayTime, last_returned_time_ + 1);
    last_returned_time_ = state->predictedDisplayTime;
    frame.app_time = state->predictedDisplayTime;
    // Frames the application abandoned without an end never block a wait.
    while (!frames_.empty() && frames_.front().sequence + 4 < sequence) {
        NoteAbandonedLocked(frames_.front().begun ? 1 : 0);
        frames_.pop_front();
    }
    frames_.push_back(frame);
    return result;
}

XrResult FrameGenPresenter::BeginFrame(XrSession session, const XrFrameBeginInfo* info, const FrameGenRuntimeCalls& calls) {
    {
        std::scoped_lock lock(mutex_);
        // A frame dropped when the presenter thread stopped has no runtime
        // frame; it was waited before anything still open, so begins first.
        if (dropped_unbegun_ > 0) {
            --dropped_unbegun_;
            return XR_SUCCESS;
        }
        // Frames its thread superseded, dropped when the presenter thread
        // stopped: begun over, they were abandoned, and their ends never come.
        if (dropped_superseded_ > 0 && std::this_thread::get_id() == end_thread_) {
            dropped_unended_ -= dropped_superseded_;
            NoteAbandonedLocked(dropped_superseded_);
            dropped_superseded_ = 0;
        }
        // Frames begin in the order they were waited; one the runtime waited
        // for (inline) begins there, one the presenter thread presents only
        // here. Begun over frames its thread superseded, it abandons them:
        // inline, the runtime discards them itself (XR_FRAME_DISCARDED).
        auto unbegun = [](const PendingFrame& frame) { return !frame.begun; };
        auto inline_frame = std::find_if(frames_.begin(), frames_.end(), unbegun);
        if (inline_frame != frames_.end() && DropAbandonedLocked(false) > 0) {
            inline_frame = std::find_if(frames_.begin(), frames_.end(), unbegun);
        }
        if (inline_frame != frames_.end()) {
            inline_frame->begun = true;
            inline_frame->begin_thread = std::this_thread::get_id();
        } else {
            auto threaded_unbegun = [](const ThreadedFrame& frame) { return !frame.begun; };
            auto threaded = std::find_if(threaded_frames_.begin(), threaded_frames_.end(), threaded_unbegun);
            // The presenter thread presents it: no runtime begin reports the
            // frames it abandons, so this one does.
            bool discarded = false;
            if (threaded != threaded_frames_.end() && DropAbandonedLocked(true) > 0) {
                discarded = true;
                threaded = std::find_if(threaded_frames_.begin(), threaded_frames_.end(), threaded_unbegun);
            }
            if (threaded != threaded_frames_.end()) {
                threaded->begun = true;
                threaded->begin_thread = std::this_thread::get_id();
                // The captures released before it: the next wait may be held
                // until they have run on the GPU (GpuGateMarkLocked).
                threaded->capture_mark = last_capture_mark_;
                begun_capture_marks_.push_back(last_capture_mark_);
                while (begun_capture_marks_.size() > 4) {
                    begun_capture_marks_.pop_front();
                }
                slot_cv_.notify_all();
                return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
            }
            if (compositor_running_) {
                // The presenter thread owns the runtime frame loop.
                return XR_SUCCESS;
            }
        }
        if (!RuntimeWaitPendingLocked()) {
            // Nothing waited is left to begin: forwarding would break the
            // runtime's frame order.
            if (logger_ && !logged_order_guard_) {
                logged_order_guard_ = true;
                logger_->Error("Frame Generation: held back an xrBeginFrame with no waited frame to begin.");
            }
            return XR_SUCCESS;
        }
    }
    const XrResult result = RuntimeBegin(calls, session, info);
    WakeSlots();
    return result;
}

XrResult FrameGenPresenter::EndFrame(XrSession session, const XrFrameEndInfo* info, const FrameGenRuntimeCalls& calls,
                                     const FrameGenQuadFrame* quad) {
    const auto entered = Clock::now();
    // Every return: how long the game's xrEndFrame was held here (after any
    // lock below is released).
    struct EndCall {
        FrameGenPresenter* presenter;
        Clock::time_point entered;
        ~EndCall() { presenter->NoteEndCall(entered); }
    } end_call{this, entered};
    std::string unsupported;
    const bool supported = SupportedFrameChains(*info, unsupported);
    bool pass_through;
    {
        std::scoped_lock lock(mutex_);
        if (!supported && !unsupported_composition_) {
            unsupported_composition_ = true;
            UnavailableLocked(unsupported + " cannot be preserved, so Frame Generation is off until the game restarts; "
                          "frames pass straight through");
        }
        pass_through = unsupported_composition_;
    }
    {
        std::optional<ThreadedFrame> threaded;
        {
            std::scoped_lock lock(mutex_);
            NoteQuadEndLocked(quad, FindProjection(*info) != nullptr);
            if (quad && quad->route == FrameGenQuadRoute::kClaimed) {
                // Claimed in the layer's xrEndFrame: this end is that frame's,
                // ahead of any frame dropped since (the claimed frame was the
                // oldest open one). Its primary projection holds four views,
                // which a stereo runtime must never get: it is shown composed
                // by the presenter thread, or not at all.
                const auto claimed =
                    quad_claim_id_ ? std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                                  [&](const ThreadedFrame& pending) { return pending.id == *quad_claim_id_; })
                                   : threaded_frames_.end();
                quad_claim_id_.reset();
                if (claimed == threaded_frames_.end()) {
                    healthy_streak_ = 0;
                    if (logger_ && !logged_quad_lost_claim_) {
                        logged_quad_lost_claim_ = true;
                        logger_->Error("Frame Generation: a claimed quad views frame was dropped before its end "
                                       "(the session stopped); it was not shown.");
                    }
                    return XR_SUCCESS;
                }
                threaded = *claimed;
            }
            // Frames end in the order they were waited, whatever display time
            // an app reports (MSFS reports one period later): the oldest open
            // frame is the one ending. Dropped frames are the oldest, then
            // frames the runtime waited for, then the presenter thread's.
            if (!threaded) {
                if (dropped_unended_ > 0) {
                    // Its runtime frame went to the frames around it when the
                    // presenter thread stopped; there is nothing to end. One
                    // its thread superseded was ended after all.
                    --dropped_unended_;
                    dropped_superseded_ = std::min(dropped_superseded_, dropped_unended_);
                    healthy_streak_ = 0;
                    return XR_SUCCESS;
                }
                if (frames_.empty() && !threaded_frames_.empty()) {
                    const auto it = std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                                 [](const ThreadedFrame& pending) { return !pending.claimed; });
                    if (it != threaded_frames_.end()) {
                        it->claimed = true;
                        abandons_frames_ = abandons_frames_ && !it->superseded;
                        threaded = *it;
                    }
                }
                if (!threaded && frames_.empty() && compositor_running_) {
                    // The presenter thread owns the runtime frame loop and
                    // holds no runtime frame for this one.
                    if (logger_ && !logged_swallowed_) {
                        logged_swallowed_ = true;
                        logger_->Error("Frame Generation: an application frame matched no waited frame while "
                                       "presenting from its own thread; it was not shown.");
                    }
                    return XR_SUCCESS;
                }
            }
        }
        if (threaded) {
            return ThreadedEnd(session, *info, calls, *threaded, entered, quad);
        }
    }
    std::optional<PendingFrame> frame;
    const XrCompositionLayerProjection* projection = FindProjection(*info);
    const bool renderable = info->layerCount > 0 && projection != nullptr;
    {
        std::scoped_lock lock(mutex_);
        end_thread_ = std::this_thread::get_id();
        DropOwnDiscardedLocked();
        // The oldest waited frame is the one ending, as on the threaded path.
        // MSFS ends frames one period after the time it was given, which can
        // equal the prediction of its next pipelined frame: matching by time
        // would take that frame's record instead of this one's.
        const auto it = frames_.begin();
        if (it != frames_.end()) {
            // Claiming under the lock: from here a held wait lets this frame
            // finish its extra runtime frame instead of demoting it, and a
            // begin on another thread does not take it for abandoned.
            it->claimed = true;
            if (it->superseded) {
                // Ended after its thread waited again, not abandoned: frames
                // waited over superseded ones pass through again, and an
                // engaged one was demoted after all.
                abandons_frames_ = false;
                if (it->demotion_owed) {
                    it->demotion_owed = false;
                    ++demoted_;
                    if (++consecutive_demotions_ >= kDemotionsBeforePause) {
                        PauseLocked("the application starts frames before the previous one ends");
                    }
                }
            }
            frame = *it;
            frame->end_time = info->displayTime;
            NoteDisplayOffsetLocked(info->displayTime, it->app_time, it->reported_period);
            app_work_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - it->wait_return).count();
            ++app_work_frames_;
        }
        // Quad views Frame Generation could compose keep their four views as
        // targets (ClaimQuadFrame): the composed views would take them over,
        // and no later view release would be captured.
        if (renderable && !QuadTargetsLocked(quad)) {
            RecordCaptureTargetsLocked(*projection);
        }
    }
    XrResult result = XR_SUCCESS;
    bool open;
    {
        std::scoped_lock lock(mutex_);
        open = RuntimeFrameOpenLocked();
    }
    if (!open) {
        // No runtime frame is begun: forwarding would break the runtime's
        // frame order.
        std::scoped_lock lock(mutex_);
        if (frame) {
            frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                         [&](const PendingFrame& pending) { return pending.sequence == frame->sequence; }),
                          frames_.end());
        }
        healthy_streak_ = 0;
        if (logger_ && !logged_order_guard_) {
            logged_order_guard_ = true;
            logger_->Error("Frame Generation: held back an xrEndFrame with no begun frame to end.");
        }
        WakeSlots();
        return XR_SUCCESS;
    }
    if (!frame || !frame->engaged) {
        result = RuntimeEnd(calls, session, info, false);
        bool eligible;
        {
            std::scoped_lock lock(mutex_);
            eligible = ThreadedEligibleLocked();
        }
        // Refused for composition metadata, nothing is presented from a
        // copy again (and CopyFrame would only report the refused chain).
        if (eligible && renderable && !pass_through) {
            // What the presenter thread shows until its first real frame, and
            // a check that it can repeat this application's layers at all.
            FrameCopy copy;
            std::string unsupported;
            const bool copied = CopyFrame(*info, copy, unsupported);
            std::scoped_lock lock(mutex_);
            if (copied) {
                passthrough_copy_ = std::move(copy);
                // Shown as it is when the presenter thread starts: only while
                // nothing newer was released into its swapchains.
                RecordColorReleasesLocked(passthrough_copy_);
                if (compositor_running_) {
                    // The presenter thread is about to start: its first
                    // slots show this frame, not the one before.
                    newest_copy_ = passthrough_copy_;
                }
            } else if (!threaded_disabled_) {
                threaded_disabled_ = true;
                if (logger_) {
                    logger_->Info("Frame Generation presents inline: the game submits " + unsupported +
                                  ", which the presenter thread cannot repeat between real frames.");
                }
            }
        }
    } else if (frame->demoted || !frame->should_render || !renderable || pass_through) {
        // Present in the one slot this frame still holds.
        result = SubmitAt(session, *info, frame->slot_time, calls);
        std::scoped_lock lock(mutex_);
        InvalidateContinuityLocked();
    } else {
        result = RunCycle(session, *info, *frame, calls);
    }
    {
        std::scoped_lock lock(mutex_);
        if (frame) {
            frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                         [&](const PendingFrame& pending) { return pending.sequence == frame->sequence; }),
                          frames_.end());
        }
        healthy_streak_ = renderable && (!frame || frame->should_render) ? healthy_streak_ + 1 : 0;
        const auto now = Clock::now();
        app_frames_.push_back(now);
        ++totals_.app_frames;
        NoteRealEndLocked(now);
        totals_.non_renderable += renderable ? 0 : 1;
        if (!frame || !frame->engaged) {
            NoteSubmissionLocked(true);
        }
        UpdateAutoLocked(now, frame, frame && frame->engaged && last_cycle_started_late_);
        if (state_ == FrameGenState::kActive && now - last_summary_ >= summary_interval_) {
            const double seconds =
                last_summary_ == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last_summary_).count();
            const Totals& start = summary_totals_;
            const double standby = StandbySecondsLocked(now);
            if (logger_) {
                LogMemoryBudgetLocked();
                // Rates are the window's averages. appWorkMs: the game's own
                // time per frame between its wait and its end. lateCycles:
                // cycles whose first generated slot was already past due, so
                // the game's frame did not fit the time the runtime gives
                // before that slot.
                logger_->Info(
                    "Frame Generation: multiplier=" +
                    (full_rate_ ? std::string(DynamicLocked() && dynamic_standby_ ? "standing by" : "full rate")
                                : MultiplierText(EffectiveMultiplierLocked())) +
                    (settings_.multiplier == FrameGenMultiplier::kAuto ? " (auto)" : "") +
                    ", appFps=" + Number(PerSecond(totals_.app_frames - start.app_frames, seconds), 1) +
                    ", outputFps=" + Number(PerSecond(totals_.submissions - start.submissions, seconds), 1) +
                    ", distinctFps=" + Number(PerSecond(totals_.distinct - start.distinct, seconds), 1) +
                    ", standbyS=" + Number(standby - summary_standby_seconds_, 1) +
                    ", repeatedFrames=" + std::to_string(repeated_submissions_) +
                    ", appWorkMs=" + Number(app_work_frames_ ? app_work_ms_ / app_work_frames_ : 0.0, 1) +
                    ", lateCycles=" + std::to_string(late_cycles_) + "/" + std::to_string(cycles_) +
                    ", threaded=" + std::to_string(compositor_running_) + ", generated=" + std::to_string(generated_) +
                    ", repeated=" + std::to_string(repeated_) + ", failures=" + std::to_string(failures_) +
                    ", demoted=" + std::to_string(demoted_) + ", abandoned=" + std::to_string(abandoned_frames_) +
                    ", captured=+" +
                    std::to_string(totals_.captured - start.captured) + " (" + std::to_string(captured_) + ")" +
                    ", pauses=" + std::to_string(totals_.pauses - start.pauses) + "/" + Number(pause_longest_ms_, 0) +
                    ", autoRetries=" + std::to_string(automatic_retries_) + "/" + std::to_string(retry_attempts_) +
                    ", gpuMs=" + Number(gpu_ms_, 2) + ClockSummaryLocked(start) + DynamicSummaryLocked() +
                    QuadSummaryLocked(start) + ".");
                LogEngineSummaryLocked(seconds);
            }
            NewSummaryWindowLocked(now, standby);
        }
    }
    idle_cv_.notify_all();
    WakeSlots(); // the presenter thread waits for inline frames to end
    return result;
}

XrResult FrameGenPresenter::SubmitAt(XrSession session, const XrFrameEndInfo& info, XrTime time,
                                     const FrameGenRuntimeCalls& calls) {
    XrFrameEndInfo copy = info;
    copy.displayTime = time;
    const XrResult result = RuntimeEnd(calls, session, &copy, false);
    std::scoped_lock lock(mutex_);
    NoteSubmissionLocked(true);
    return result;
}

void FrameGenPresenter::NoteRuntimePeriodLocked(XrDuration period, Clock::time_point now) {
    if (period <= 0) {
        return;
    }
    last_period_ = period;
    // About the refresh period or shorter: the refresh period, at once (no
    // runtime reports less than the refresh period it holds frames to).
    if (refresh_period_ <= 0 || period <= refresh_period_ + refresh_period_ / 20) {
        refresh_period_ = period;
        refresh_candidate_.reset();
        return;
    }
    // About `from` to five times `base`.
    const auto multiple = [](XrDuration longer, XrDuration base, std::uint32_t from) {
        for (std::uint32_t refreshes = from; refreshes <= kMaxThrottleRefreshes; ++refreshes) {
            const XrDuration held = base * static_cast<XrDuration>(refreshes);
            if (std::llabs(longer - held) <= held / 20) {
                return true;
            }
        }
        return false;
    };
    // A longer period is a new refresh rate only once every period reported
    // since agreed with it (it, or a multiple of it) for a while: a hitch's
    // one long period never becomes the refresh period.
    if (refresh_candidate_ && !multiple(period, refresh_candidate_->first, 1)) {
        refresh_candidate_.reset();
    }
    // About two to five times the refresh period: the runtime cutting the
    // frame rate to that fraction, or reporting how late its caller was in
    // whole refreshes, not another refresh rate (a session's refresh rate
    // dropping to exactly such a fraction is not told apart from it).
    if (multiple(period, refresh_period_, 2)) {
        return;
    }
    if (!refresh_candidate_) {
        refresh_candidate_ = std::make_pair(period, now);
    } else if (now - refresh_candidate_->second >= kRefreshChangeAfter) {
        refresh_period_ = refresh_candidate_->first;
        refresh_candidate_.reset();
    }
}

void FrameGenPresenter::NoteRuntimeHalvingLocked(Clock::time_point called, Clock::time_point returned,
                                                 XrTime display_time, XrDuration period, bool presenter) {
    // Each source is measured from its own waits: a switch starts over.
    const std::optional<HalvingWait> previous = halving_last_;
    halving_last_ = HalvingWait{returned, display_time, presenter};
    if (!previous || previous->presenter != presenter) {
        halving_samples_.clear();
        halving_since_ = returned;
        return;
    }
    const XrDuration refresh = refresh_period_;
    if (refresh <= 0 || display_time <= previous->display_time) {
        return;
    }
    // Halved (or cut further): a multiple of the refresh period reported, or
    // a wait asked for well within a refresh of the last one's release that
    // got a refresh after the next. A wait asked for later says nothing of
    // that by itself: its caller missed the next refresh itself (the
    // presenter thread ran late, or the game is slow). Asked for late, a
    // wait shows the runtime holding frames where it reported a multiple
    // and still blocked the wait for half a refresh or more. It shows the
    // opposite where the runtime reported a multiple, released the wait at
    // once, and gave it a display time off the refresh grid, as far after
    // the last as its caller took: the period only echoes the caller's own
    // frame interval in whole refreshes (SteamVR with Pimax's headset
    // driver does this), and the wait got the earliest refresh the runtime
    // had, as one released at the next refresh did: not halved. Released at
    // once for a display time on the grid, it says nothing: a runtime
    // holding frames releases a caller that comes just before its held
    // refresh that way, and may let one that comes after it go at once.
    const auto refreshes = [&](XrDuration duration) {
        return static_cast<std::uint32_t>(std::min<XrDuration>((duration + refresh / 2) / refresh, 64));
    };
    const XrDuration step = display_time - previous->display_time;
    const bool reported = period >= refresh + refresh / 2;
    const std::uint32_t held = reported ? refreshes(period) : refreshes(step);
    const auto waited = returned - called;
    const bool blocked = waited >= std::chrono::nanoseconds(refresh / 2);
    bool halved = held >= 2;
    if (halved && called - previous->returned >= std::chrono::nanoseconds(refresh * 3 / 4)) {
        const XrDuration past = step % refresh;
        const bool off_grid = std::min(past, refresh - past) > refresh / 20;
        if (reported && off_grid &&
            waited < std::max<std::chrono::nanoseconds>(std::chrono::milliseconds(1),
                                                        std::chrono::nanoseconds(refresh / 10))) {
            halved = false;
        } else if (!reported || !blocked) {
            return;
        }
    }
    if (presenter) {
        ++totals_.halving_measured;
        totals_.halving_halved += halved ? 1 : 0;
    }
    halving_samples_.push_back({returned, halved ? held : 1, halved && blocked});
    while (returned - halving_samples_.front().returned > halving_window_) {
        halving_samples_.pop_front();
    }
    // A window measured, with at least half the waits a runtime holding the
    // frame rate to a quarter of its refresh rate releases in it. While
    // halving is reported, the presenter thread's waits decide a quarter of
    // the window after they start: Dynamic engaging again under it stands
    // down again soon, or clears it, rather than presenting halved for a
    // whole window each time.
    std::chrono::nanoseconds window = halving_window_;
    if (presenter && runtime_halving_) {
        window /= 4;
    }
    const std::size_t total = halving_samples_.size();
    if (returned - halving_since_ < window || static_cast<XrDuration>(total) * 8 * refresh < window.count()) {
        return;
    }
    const auto count = static_cast<std::size_t>(std::count_if(
        halving_samples_.begin(), halving_samples_.end(), [](const HalvingSample& sample) { return sample.held >= 2; }));
    // Three quarters of the window's waits halved report it, a quarter or
    // fewer clear it: an occasional missed refresh does neither.
    if (presenter && count * 4 >= total * 3) {
        halving_confirmed_at_ = returned;
        runtime_halving_ = true;
        if (logger_ && !logged_runtime_halving_) {
            logged_runtime_halving_ = true;
            // What was measured: the waits' release, every Nth refresh (the N
            // seen most often), whether most of them blocked, and the period
            // reported. Where it comes from is not measured: the log names
            // the settings that can do this.
            std::array<std::size_t, 9> seen{};
            std::size_t blocked_count = 0;
            for (const HalvingSample& sample : halving_samples_) {
                ++seen[std::min<std::uint32_t>(sample.held, 8)];
                blocked_count += sample.blocked ? 1 : 0;
            }
            const auto every = static_cast<std::uint32_t>(std::max_element(seen.begin() + 2, seen.end()) - seen.begin());
            const std::string hz = Number(1e9 / static_cast<double>(refresh), 0) + " Hz";
            std::string evidence = blocked_count * 2 >= count ? "its frame waits blocked"
                                                              : "from its frame waits' display times";
            if (reported) {
                evidence += "; it reported a frame period of " + Number(static_cast<double>(period) / 1e6, 1) +
                            " ms, " + TimesText(held) + " its refresh period";
            }
            logger_->Info("Frame Generation: the runtime held Frame Generation's frames to every " + EveryText(every) +
                          " refresh at " + hz + " for " +
                          Number(std::chrono::duration<double>(halving_window_).count(), 0) + " s (" + evidence +
                          "). Settings that can do this: SteamVR's Motion Smoothing (Settings > Video), and the "
                          "game's Motion Smoothing and Throttling Behavior in SteamVR's Per-Application Video "
                          "Settings; Pimax Play's Smart Smoothing and Lock Frame Rate to Half Refresh Rate (Device "
                          "settings > Frame rate); Virtual Desktop's SSW; Meta's ASW. Check the ones your runtime "
                          "and headset use, and keep the runtime at full refresh for games that use Frame "
                          "Generation.");
        }
    } else if (runtime_halving_ && count * 4 <= total) {
        runtime_halving_ = false;
        halving_confirmed_at_.reset();
    }
}

void FrameGenPresenter::NotePhaseLocked(Clock::time_point called, Clock::time_point returned, XrTime display_time,
                                        XrDuration period) {
    // A wait that blocked returned on the runtime's own schedule; one that
    // returned at once says nothing about it.
    const auto blocked = std::max<std::chrono::nanoseconds>(std::chrono::milliseconds(1),
                                                            std::chrono::nanoseconds(period / 10));
    if (period > 0 && returned - called >= blocked) {
        phase_wall_ = returned;
        phase_xr_ = display_time;
        phase_echoed_ = false;
    }
}

void FrameGenPresenter::NoteRuntimeClockLocked(Clock::time_point returned, XrTime display_time, XrDuration period,
                                               bool rendered) {
    const std::optional<ClockWait> previous = clock_last_;
    clock_last_ = ClockWait{returned, display_time};
    const XrDuration refresh = refresh_period_ > 0 ? refresh_period_ : period;
    if (!rendered || refresh <= 0) {
        clock_stalled_waits_ = 0;
        return;
    }
    // How far the display time lies past this process's clock: about
    // constant while display times advance with the time. SteamVR, after a
    // game paused its frame loop, lags (up to seconds) and then catches up
    // to it; Varjo's ran tens of seconds past it, then barely moved while its
    // waits still blocked a refresh each. Only running past the highest
    // offset seen while steady counts, so a catch-up never does; that
    // ceiling is first taken over kClockWarmupWaits waits (one wait late off
    // its runtime, or this thread late to see it return, would set it low).
    const XrDuration offset =
        display_time - std::chrono::duration_cast<std::chrono::nanoseconds>(returned.time_since_epoch()).count();
    if (!previous) {
        return;
    }
    const XrDuration jump = std::max<XrDuration>(kClockJumpFloor, 16 * refresh);
    const XrDuration wall = std::chrono::duration_cast<std::chrono::nanoseconds>(returned - previous->returned).count();
    const XrDuration step = display_time - previous->display_time;
    const bool judged = wall >= refresh / 2;
    const bool stalled = judged && step < refresh / 4;
    // Moved on with the time between the waits (a whole number of refreshes
    // where a runtime holds frames or echoes a late caller), not jumping.
    const bool with_time = judged && !stalled && step <= wall + jump;
    clock_stalled_waits_ = stalled ? clock_stalled_waits_ + 1 : 0;
    const bool back = step < 0;
    const bool warm = clock_warmup_waits_ >= kClockWarmupWaits;
    const bool ahead = warm && offset > clock_offset_ceiling_ + jump;
    // The last few waits, for the log when the clock is displaced.
    clock_history_[clock_history_next_++ % clock_history_.size()] = {wall, step, offset - clock_offset_ceiling_};
    if (!clock_displaced_) {
        if (ahead || back || clock_stalled_waits_ >= kClockStalledWaits) {
            ++totals_.clock_unsteady_waits;
            clock_displaced_ = true;
            clock_displaced_at_ = returned;
            clock_steady_waits_ = 0;
            clock_gap_ = ahead ? wall : 0;
            clock_ahead_ = ahead ? offset - clock_offset_ceiling_ : 0;
            ++totals_.clock_displacements;
            clock_displacement_logged_ = logger_ && logged_clock_displacements_ < kLoggedPauses;
            if (clock_displacement_logged_) {
                ++logged_clock_displacements_;
                // waits: each recent wait's time since the one before / its
                // display time's step / its offset over the ceiling, ms.
                std::string waits;
                const std::size_t kept = std::min<std::size_t>(clock_history_next_, clock_history_.size());
                for (std::size_t i = kept; i-- > 0;) {
                    const ClockSample& sample =
                        clock_history_[(clock_history_next_ - 1 - i) % clock_history_.size()];
                    waits += (waits.empty() ? "" : " ") + Number(static_cast<double>(sample.wall) / 1e6, 1) + "/" +
                             Number(static_cast<double>(sample.step) / 1e6, 1) + "/" +
                             Number(static_cast<double>(sample.over) / 1e6, 1);
                }
                logger_->Info("Frame Generation: " + RuntimeClockTextLocked() +
                              "; nothing engages and frames pass straight through until they advance with the time "
                              "again (Varjo's runtime does this after a game stops its frame loop for a while, as while "
                              "loading). Recent waits (ms since the last / display time step / past the ceiling): " +
                              waits + ".");
            }
            return;
        }
        clock_offset_ceiling_ = clock_warmup_waits_ == 0 ? offset : std::max(clock_offset_ceiling_, offset);
        clock_warmup_waits_ += with_time && !warm ? 1 : 0;
        return;
    }
    // Displaced: steady again after waits that advance with the time, even
    // past the ceiling (a runtime that moved its clock on for good, or a
    // ceiling taken low); an odd one that does not is not counted, three
    // stalled in a row or one going back start over. Varjo's creep never
    // counts: its display times barely move.
    if (ahead) {
        clock_ahead_ = std::max(clock_ahead_, offset - clock_offset_ceiling_);
    }
    if (back || clock_stalled_waits_ >= kClockStalledWaits) {
        ++totals_.clock_unsteady_waits;
        clock_steady_waits_ = 0;
        return;
    }
    if (!with_time) {
        return;
    }
    if (clock_steady_waits_++ == 0) {
        clock_steady_since_ = returned;
    }
    if (clock_steady_waits_ >= kClockSteadyWaits && returned - clock_steady_since_ >= kClockSteadyTime) {
        clock_displaced_ = false;
        clock_steady_waits_ = 0;
        clock_stalled_waits_ = 0;
        clock_ahead_ = 0;
        // The ceiling is taken anew from here.
        clock_offset_ceiling_ = offset;
        clock_warmup_waits_ = 0;
        // Engaging again is a change too: its first frames carry its cost
        // and say nothing about the game (velocity7 on Virtual Desktop,
        // 2026-10-14: Auto went 3x -> 4x in the second the clock recovered).
        multiplier_changed_at_ = returned;
        cadence_.clear();
        if (dynamic_standby_) {
            dynamic_reason_ = "the runtime's display times advance with the time again";
        }
        // Logged when the displacement it ends was (past the first few, a
        // recovery alone named an end the log never began).
        if (logger_ && clock_displacement_logged_) {
            clock_displacement_logged_ = false;
            logger_->Info("Frame Generation: the runtime's display times advance with the time again (after " +
                          Number(std::chrono::duration<double>(returned - clock_displaced_at_).count(), 1) + " s).");
        }
    }
}

std::string FrameGenPresenter::RuntimeClockTextLocked() const {
    if (clock_ahead_ > 0) {
        return "the runtime's display times ran " + Number(static_cast<double>(clock_ahead_) / 1e9, 1) +
               " s ahead of the time that passed" +
               (clock_gap_ > 0 ? " (after no frame for " + Number(static_cast<double>(clock_gap_) / 1e9, 1) + " s)"
                               : std::string()) +
               " and barely advance until the time catches up";
    }
    return "the runtime's display times stopped advancing with the time";
}

XrDuration FrameGenPresenter::GameGridLocked() const {
    // A period reported as a multiple of the refresh period for a few
    // refreshes (Varjo's runtime holds frames to every second refresh for a
    // few after a late submission; SteamVR with Pimax's driver echoes a late
    // caller's interval) would time the game's frames that many times further
    // ahead, past what the real frames kept reach, and later frames are never
    // timed earlier again (last_target_time_, last_returned_time_). A runtime
    // holding frames to the multiple for a while (runtime_halving_) times
    // them by it.
    const XrDuration reported = slot_period_ > 0 ? slot_period_ : std::max<XrDuration>(last_period_, 1);
    return runtime_halving_ ? reported : SubmitSpacingLocked(reported);
}

XrDuration FrameGenPresenter::SubmitSpacingLocked(XrDuration period) const {
    // A runtime may report a longer period than the refresh period without
    // holding frames to it: SteamVR with Pimax's headset driver reports a
    // late caller's own frame interval. Spaced by that, submissions would
    // stay that late (the runtime echoing it back): never more than a
    // refresh.
    period = std::max<XrDuration>(period, 1);
    return refresh_period_ > 0 ? std::min(period, refresh_period_) : period;
}

std::pair<Clock::time_point, bool> FrameGenPresenter::ScheduleSubmissionLocked(Clock::time_point now,
                                                                                XrTime display_time,
                                                                                XrDuration period) {
    // Pimax and SteamVR can return waits at once instead of pacing them (a
    // generated cycle's, or any asked for late), so frames would reach the
    // compositor inside one refresh and only the newest would be shown. A
    // submission goes when the runtime would have released its wait, from
    // the last wait that did block, and on a refresh grid: most of a
    // refresh after the last submission was due where the runtime's own
    // cadence is known (this wait blocked, or one before it did: the grid
    // then only guards, and a grid restarted late by a hitch falls back to
    // the runtime's releases within a few refreshes), and nearly a refresh
    // otherwise (a little less, so that waits come a little earlier each
    // refresh until a runtime that holds early ones does, and its cadence
    // is known). Due from when the last one was due, not from when its
    // xrEndFrame returned: that call's own time would add up, refresh after
    // refresh. A grid behind now restarts from now. Runtimes that pace their
    // waits rarely sleep.
    const auto ns = [](XrDuration duration) { return std::chrono::nanoseconds(duration); };
    const XrDuration spacing = SubmitSpacingLocked(period);
    const bool fresh = phase_wall_ != Clock::time_point{} && now - phase_wall_ <= kPhaseLifetime;
    // Aimed at (phased): a wait after the one that set the phase. A wait
    // that blocked set the phase itself: it was released on the runtime's
    // own cadence, which is then known, but there is nothing to aim at and
    // nothing to be late for (the game's own slot, submitted long after its
    // wait returned, is not late).
    const bool phased = fresh && display_time > phase_xr_;
    const bool known = fresh && display_time >= phase_xr_;
    Clock::time_point at = now;
    bool late = false;
    if (phased) {
        const auto ideal = phase_wall_ + ns(display_time - phase_xr_);
        late = now > ideal + ns(period / 2);
        // Held to it only while display times stay on the phase's refresh
        // grid. One off it (more than refresh/20 from a whole number of
        // refreshes) is no refresh's: the runtime echoed a late caller's own
        // interval and returned at once (SteamVR with Pimax's headset
        // driver), for a display time as far after its last release as the
        // caller took, so its waits are due when they return. From then on,
        // until a wait blocks and measures the phase anew, nothing is held to
        // it. The phase is off by however late the blocked wait that set it
        // was seen (its thread held up in a hitch): each submission held to
        // it would carry that error, the next wait would come late and
        // return at once again, and submissions would stay up to 1.5
        // refreshes apart until the phase expired.
        const XrDuration past = (display_time - phase_xr_) % spacing;
        phase_echoed_ = phase_echoed_ || std::min(past, spacing - past) > spacing / 20;
        if (!phase_echoed_) {
            at = std::max(at, ideal);
        }
    }
    if (submit_grid_ != Clock::time_point{}) {
        at = std::max(at, submit_grid_ + ns(spacing * (known ? 17 : 19) / 20));
    }
    // A wrong phase never costs more than about one refresh.
    at = std::min(at, now + ns(spacing * 6 / 5));
    submit_grid_ = at;
    return {at, late};
}

bool FrameGenPresenter::PaceSubmission(XrTime display_time, XrDuration period) {
    if (period <= 0) {
        return false;
    }
    Clock::time_point target;
    bool late = false;
    {
        std::scoped_lock lock(mutex_);
        std::tie(target, late) = ScheduleSubmissionLocked(Clock::now(), display_time, period);
    }
    SleepUntil(timer_, target);
    return late;
}

XrResult FrameGenPresenter::NextSlot(XrSession session, const FrameGenRuntimeCalls& calls, XrDuration period,
                                     XrFrameState& state) {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    state = {XR_TYPE_FRAME_STATE};
    const auto called = Clock::now();
    XrResult result = RuntimeWait(calls, session, &wait_info, &state);
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        PauseLocked("runtime xrWaitFrame failed inside a generated cycle (" + std::to_string(result) + ")");
        return result;
    }
    {
        std::scoped_lock lock(mutex_);
        const auto returned = Clock::now();
        NotePhaseLocked(called, returned, state.predictedDisplayTime, state.predictedDisplayPeriod);
        NoteRuntimeClockLocked(returned, state.predictedDisplayTime, state.predictedDisplayPeriod,
                               state.shouldRender == XR_TRUE);
    }
    PaceSubmission(state.predictedDisplayTime, period);
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    result = RuntimeBegin(calls, session, &begin_info);
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        PauseLocked("runtime xrBeginFrame failed inside a generated cycle (" + std::to_string(result) + ")");
    }
    return result;
}

XrResult FrameGenPresenter::RunCycle(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                                     const FrameGenRuntimeCalls& calls) {
    std::uint32_t projection_index = 0;
    const XrCompositionLayerProjection& projection = *FindProjection(info, &projection_index);
    const std::uint32_t generated = std::max(2u, frame.multiplier) - 1;
    const XrDuration period = frame.period;
    const XrTime newest_time = frame.slot_time + period * static_cast<XrDuration>(generated);
    FrameGenSynthesisMode mode;
    bool continuous;
    XrTime previous_time = 0;
    std::size_t extrapolated_sets = 0;
    {
        std::scoped_lock lock(mutex_);
        mode = settings_.synthesis_mode;
        previous_time = last_real_.display_time;
        continuous = last_real_.valid && last_real_.sequence + 1 == frame.sequence && newest_time > previous_time &&
                     newest_time - previous_time <= period * static_cast<XrDuration>(kMaxMultiplier + 2);
        if (extrapolated_.valid && continuous && extrapolated_.space == projection.space && !extrapolated_.sets.empty() &&
            extrapolated_.sets.front().size() == projection.viewCount) {
            extrapolated_sets = extrapolated_.sets.size();
        }
    }

    // Replaces the primary projection layer with generated views.
    std::vector<const XrCompositionLayerBaseHeader*> layers(info.layers, info.layers + info.layerCount);
    XrCompositionLayerProjection generated_layer{};
    std::vector<XrCompositionLayerProjectionView> with_depth;
    auto with_views = [&](const std::vector<XrCompositionLayerProjectionView>& views, XrSpace space,
                          XrCompositionLayerFlags flags, bool depth) {
        generated_layer = projection;
        // This inline cycle finishes within the application's xrEndFrame:
        // supported value-only metadata may keep the application's chain.
        generated_layer.next = projection.next;
        generated_layer.space = space;
        generated_layer.layerFlags = flags;
        generated_layer.viewCount = static_cast<std::uint32_t>(views.size());
        generated_layer.views = views.data();
        if (depth && views.size() == projection.viewCount) {
            // The newest real frame's depth chain, valid during this end: its
            // depth image is the one the game just released.
            with_depth = views;
            for (std::size_t i = 0; i < with_depth.size(); ++i) {
                with_depth[i].next = projection.views[i].next;
            }
            generated_layer.views = with_depth.data();
        }
        layers[projection_index] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&generated_layer);
        XrFrameEndInfo generated_info = info;
        generated_info.layers = layers.data();
        return generated_info;
    };

    // Interpolated frames need the newest real frame, so they are generated
    // before the first slot; extrapolated ones were generated last cycle.
    std::vector<std::vector<XrCompositionLayerProjectionView>> interpolated;
    std::vector<std::uint8_t> interpolated_exact;
    bool interpolated_ready = false;
    if (mode == FrameGenSynthesisMode::kInterpolate) {
        std::vector<float> fractions;
        for (std::uint32_t slot = 0; slot < generated; ++slot) {
            const XrTime target = frame.slot_time + period * static_cast<XrDuration>(slot);
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time, target) : 1.0f);
        }
        interpolated_ready = Generate(projection, frame, mode, fractions, interpolated, interpolated_exact);
    }
    bool generated_ok = interpolated_ready;

    XrTime time = frame.slot_time;
    bool should_render = frame.should_render;
    bool real_shown = false; // the real frame went out in an earlier slot
    for (std::uint32_t slot = 0; slot <= generated; ++slot) {
        if (slot > 0) {
            XrFrameState state;
            const XrResult next = NextSlot(session, calls, period, state);
            if (XR_FAILED(next)) {
                std::scoped_lock lock(mutex_);
                InvalidateContinuityLocked();
                return next;
            }
            time = state.predictedDisplayTime;
            should_render = state.shouldRender == XR_TRUE;
        } else {
            const bool late = PaceSubmission(time, period);
            std::scoped_lock lock(mutex_);
            ++cycles_;
            late_cycles_ += late ? 1 : 0;
            last_cycle_started_late_ = late;
        }
        XrFrameEndInfo content = info; // the real frame, in the cycle's last slot
        bool distinct = !real_shown;
        bool generated_content = false;
        bool moved = false; // a generated frame, not an exact copy of a real one
        if (slot < generated) {
            if (interpolated_ready) {
                // The newest frame's depth describes its own image and eye
                // position: only an exact copy of it may carry it.
                const bool exact = slot < interpolated_exact.size() && interpolated_exact[slot];
                content = with_views(interpolated[slot], projection.space, projection.layerFlags, exact);
                generated_content = true;
                moved = !exact;
            } else if (mode == FrameGenSynthesisMode::kExtrapolate && slot < extrapolated_sets) {
                // From the previous real frame, whose depth image the game has
                // since replaced: no depth.
                content = with_views(extrapolated_.sets[slot], extrapolated_.space, extrapolated_.flags, false);
                generated_content = true;
                moved = !(slot < extrapolated_.exact.size() && extrapolated_.exact[slot]);
            }
        }
        if (generated_content) {
            distinct = true;
        } else {
            real_shown = true;
        }
        content.displayTime = time;
        if (!should_render) {
            content.layerCount = 0;
            content.layers = nullptr;
            distinct = false;
        }
        const XrResult result = RuntimeEnd(calls, session, &content, true);
        std::scoped_lock lock(mutex_);
        NoteSubmissionLocked(distinct, moved);
        if (XR_FAILED(result)) {
            PauseLocked(std::string(slot < generated ? "runtime rejected a generated frame (" : "runtime rejected the real frame (") +
                        std::to_string(result) + ")");
            InvalidateContinuityLocked();
            return result;
        }
    }

    if (mode == FrameGenSynthesisMode::kExtrapolate) {
        // The newest frame pushed forward, for the next cycle's generated slots.
        std::vector<float> fractions;
        for (std::uint32_t slot = 0; slot < generated; ++slot) {
            const XrTime target = newest_time + period * static_cast<XrDuration>(slot + 1);
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time, target) : 0.0f);
        }
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        std::vector<std::uint8_t> exact;
        const bool ok = Generate(projection, frame, mode, fractions, sets, exact);
        generated_ok = ok;
        std::scoped_lock lock(mutex_);
        extrapolated_.valid = ok;
        extrapolated_.sets = std::move(sets);
        extrapolated_.exact = std::move(exact);
        extrapolated_.generation = next_generation_++;
        extrapolated_.space = projection.space;
        extrapolated_.flags = projection.layerFlags;
    }
    // A frame that did not reach the GPU history (not captured, or failed) is
    // not the previous frame of the next one: its interval and eye motion
    // would be measured against a different pair than the flow.
    if (generated_ok) RecordReal(projection, newest_time, frame.sequence);
    std::scoped_lock lock(mutex_);
    if (!generated_ok) InvalidateContinuityLocked();
    consecutive_demotions_ = 0;
    return XR_SUCCESS;
}

void FrameGenPresenter::RecordReal(const XrCompositionLayerProjection& projection, XrTime display_time,
                                   std::uint64_t sequence) {
    std::scoped_lock lock(mutex_);
    last_real_.interval = last_real_.valid && last_real_.sequence + 1 == sequence && display_time > last_real_.display_time
                              ? display_time - last_real_.display_time
                              : 0;
    // The frame it follows, for predictors that look two intervals back.
    last_real_.before_positions.clear();
    last_real_.before_time = 0;
    if (last_real_.interval > 0) {
        last_real_.before_time = last_real_.display_time;
        for (const ViewRecord& view : last_real_.views) last_real_.before_positions.push_back(view.geometry.position);
    }
    last_real_.valid = true;
    last_real_.sequence = last_real_.generated_sequence = sequence;
    last_real_.display_time = display_time;
    last_real_.space = projection.space;
    last_real_.views.clear();
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        last_real_.views.push_back({Extent(projection.views[i]), Geometry(projection.views[i])});
    }
}

std::vector<std::array<float, 3>> FrameGenPresenter::EyeShiftsLocked(const XrCompositionLayerProjection& projection,
                                                                     XrTime newest_time, FrameGenSynthesisMode mode,
                                                                     bool predicted) const {
    std::vector<std::array<float, 3>> shifts(projection.viewCount, {0.0f, 0.0f, 0.0f});
    const double seconds = static_cast<double>(newest_time - last_real_.display_time) / 1e9;
    if (!last_real_.valid || seconds <= 0.0 || last_real_.views.size() != projection.viewCount) {
        return shifts;
    }
    // One unit of generated-frame time is the interval between the two real
    // frames, so the eyes move as far per unit as they did between them:
    // onward when extrapolating, back toward the previous frame when
    // interpolating. Runtimes are not asked where the eyes will be: Pimax
    // returns about the current pose for any later time, which kept
    // generated frames at the real frame's eye position.
    // Research (VECTORXR_FRAMEGEN_SYNTH_EYE): extrapolating, the eye goes
    // where its predictor puts it one interval on, from the real frames'
    // eyes at their own times (seconds before the newest).
    const double sign = mode == FrameGenSynthesisMode::kExtrapolate ? 1.0 : -1.0;
    const bool predicting = predicted && eye_prediction_.predictor != FrameGenEyePredictor::kVelocity;
    const bool before = last_real_.before_time > 0 && last_real_.before_time < last_real_.display_time &&
                        last_real_.before_positions.size() == projection.viewCount;
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        const XrPosef& pose = projection.views[i].pose;
        const std::array<float, 3>& previous = last_real_.views[i].geometry.position;
        std::array<double, 3> delta{pose.position.x - previous[0], pose.position.y - previous[1],
                                    pose.position.z - previous[2]};
        if (predicting) {
            std::array<FrameGenEyeSample, 3> samples;
            std::size_t count = 0;
            if (before) {
                const std::array<float, 3>& p = last_real_.before_positions[i];
                samples[count++] = {-static_cast<double>(newest_time - last_real_.before_time) / 1e9,
                                    {p[0], p[1], p[2]}};
            }
            samples[count++] = {-seconds,
                                {previous[0], previous[1], previous[2]},
                                last_real_.views[i].geometry.orientation};
            samples[count++] = {0.0,
                                {pose.position.x, pose.position.y, pose.position.z},
                                {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
            if (const auto eye = FrameGenPredictEye(eye_prediction_, samples.data(), count, seconds)) {
                delta = {(*eye)[0] - pose.position.x, (*eye)[1] - pose.position.y, (*eye)[2] - pose.position.z};
            }
        }
        // Into the eye's own view space at the newest frame: the same
        // movement in the view the game rendered, whatever it did to the
        // world around it.
        const std::array<double, 3> local = RotateInverse(pose.orientation, delta);
        const double length = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
        if (std::isfinite(length) && length < kMaxEyeSpeed * seconds) {
            shifts[i] = {static_cast<float>(sign * local[0]), static_cast<float>(sign * local[1]),
                         static_cast<float>(sign * local[2])};
        }
    }
    return shifts;
}

void FrameGenPresenter::NoteEyeMovementLocked(const XrCompositionLayerProjection& projection,
                                              const PendingFrame& frame) {
    const double seconds = static_cast<double>(frame.app_time - last_real_.display_time) / 1e9;
    if (projection.viewCount == 0 || last_real_.views.empty() || seconds <= 0.0) {
        eye_velocity_.reset();
        eye_history_.clear();
        return;
    }
    const XrVector3f& now = projection.views[0].pose.position;
    const std::array<float, 3>& before = last_real_.views[0].geometry.position;
    const std::array<double, 3> moved{now.x - before[0], now.y - before[1], now.z - before[2]};
    const double length = std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
    if (eye_velocity_ && eye_velocity_sequence_ + 1 == frame.sequence && length < kMaxEyeSpeed * seconds) {
        const std::array<double, 3>& velocity = *eye_velocity_;
        const std::array<double, 3> miss{moved[0] - velocity[0] * seconds, moved[1] - velocity[1] * seconds,
                                         moved[2] - velocity[2] * seconds};
        eye_move_sum_mm_ += 1000.0 * length;
        eye_miss_sum_mm_ += 1000.0 * std::sqrt(miss[0] * miss[0] + miss[1] * miss[1] + miss[2] * miss[2]);
        ++eye_frames_;
    }
    eye_velocity_ = std::array<double, 3>{moved[0] / seconds, moved[1] / seconds, moved[2] / seconds};
    eye_velocity_sequence_ = frame.sequence;
    // Research (VECTORXR_FRAMEGEN_SYNTH_EYE): each candidate predictor's
    // miss, from the three real frames before this one (seconds before it,
    // each at its own display time) to its eye, over the same frames for all.
    const XrPosef& pose = projection.views[0].pose;
    const FrameGenEyeSample sample{0.0,
                                   {pose.position.x, pose.position.y, pose.position.z},
                                   {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
    if (!(length < kMaxEyeSpeed * seconds)) {
        eye_history_.clear(); // a tracking glitch: measured again from this frame on
    } else if (eye_history_.empty() || eye_history_sequence_ + 1 != frame.sequence) {
        // Begun again from the real frame before (its eye as it was shown).
        const FrameGenViewGeometry& last = last_real_.views[0].geometry;
        eye_history_.assign(
            1, FrameGenEyeSample{-seconds, {last.position[0], last.position[1], last.position[2]}, last.orientation});
    } else {
        for (FrameGenEyeSample& before : eye_history_) before.time -= seconds; // now before this frame
    }
    if (eye_history_.size() == 3) {
        FrameGenEyePrediction candidate = eye_prediction_;
        for (std::size_t i = 0; i < kFrameGenEyeCandidates.size(); ++i) {
            candidate.predictor = kFrameGenEyeCandidates[i];
            if (const auto eye = FrameGenPredictEye(candidate, eye_history_.data(), eye_history_.size(), 0.0)) {
                const std::array<double, 3> miss{(*eye)[0] - sample.position[0], (*eye)[1] - sample.position[1],
                                                 (*eye)[2] - sample.position[2]};
                eye_predict_sum_mm_[i] += 1000.0 * std::sqrt(miss[0] * miss[0] + miss[1] * miss[1] + miss[2] * miss[2]);
            }
        }
        ++eye_predict_frames_;
        eye_history_.erase(eye_history_.begin());
    }
    eye_history_.push_back(sample);
    eye_history_sequence_ = frame.sequence;
}

void FrameGenPresenter::ProbeEyeLocate(XrSession session, const JitFrame& frame, bool ahead,
                                       const std::vector<XrCompositionLayerProjectionView>& views, XrTime time) {
    if (views.empty() || !frame.copy.has_projection || frame.copy.projection_index >= frame.copy.layers.size()) {
        return;
    }
    const XrSpace space = frame.copy.layers[frame.copy.projection_index].projection.space;
    // Room for quad views' four (stereo asks for two of them).
    std::array<XrView, 4> located{};
    for (XrView& view : located) {
        view = {XR_TYPE_VIEW};
    }
    XrViewState state{XR_TYPE_VIEW_STATE};
    std::uint32_t count = 0;
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    double call_ms = 0.0;
    {
        // As the output images' calls: under submit_mutex_ the space the
        // frame was written in cannot go (ForgetSpace holds it), and the
        // frame still kept in it says it has not gone before. No other lock
        // is held over the call, and its time is the one this thread waited
        // for last (a runtime that blocks locates past its newest wait, as
        // PiOpenXR does, answers it at once).
        std::scoped_lock submit(submit_mutex_);
        PFN_xrLocateViews locate = nullptr;
        XrViewConfigurationType configuration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        {
            std::scoped_lock lock(mutex_);
            if (!eye_probe_ || eye_probe_stopped_ || !xr_.locate_views) {
                return;
            }
            locate = xr_.locate_views;
            configuration = runtime_view_configuration_;
            const bool kept =
                space != XR_NULL_HANDLE && std::any_of(jit_frames_.begin(), jit_frames_.end(), [&](const JitFrame& candidate) {
                    return candidate.id == frame.id && candidate.copy.has_projection &&
                           candidate.copy.projection_index < candidate.copy.layers.size() &&
                           candidate.copy.layers[candidate.copy.projection_index].projection.space == space;
                });
            if (!kept) {
                ++eye_probe_window_.gone;
                return;
            }
        }
        // The runtime session's own configuration (xrLocateViews takes no
        // other): a session begun on quad views natively (Varjo) locates its
        // four, the first the left context eye, as views[0] in the frames.
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = configuration;
        info.displayTime = time;
        info.space = space;
        const std::uint32_t capacity =
            configuration == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET ? 4u : 2u;
        const auto entered = Clock::now();
        {
            const PresenterCall call(*this, "xrLocateViews");
            result = locate(session, &info, &state, capacity, &count, located.data());
        }
        call_ms = Milliseconds(Clock::now() - entered);
    }
    std::scoped_lock lock(mutex_);
    EyeProbeWindow& window = eye_probe_window_;
    ++window.calls;
    window.call_ms_sum += call_ms;
    window.call_ms_max = std::max(window.call_ms_max, call_ms);
    if (call_ms > kEyeProbeSlowMs) {
        // Made right before a refresh: an answer this late is not one a
        // frame could use.
        ++window.slow;
        if (++eye_probe_slow_calls_ >= kEyeProbeSlowStop && !eye_probe_stopped_) {
            eye_probe_stopped_ = true;
            if (logger_) {
                logger_->Info("Frame Generation's eye probe stops for the session: " +
                              std::to_string(eye_probe_slow_calls_) + " of the runtime's locates took longer than " +
                              Number(kEyeProbeSlowMs, 0) + " ms (the last " + Number(call_ms, 1) + " ms).");
            }
        }
        return;
    }
    if (XR_FAILED(result) || count == 0) {
        ++window.failed;
        return;
    }
    if ((state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
        ++window.invalid;
        return;
    }
    if ((state.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) == 0) {
        ++window.untracked;
        return;
    }
    const XrVector3f& at = located[0].pose.position;
    const XrVector3f& shown = views[0].pose.position;
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
        ++window.invalid;
        return;
    }
    // The previous answer's position exactly, for a later time: a runtime
    // answering with its current pose whatever time is asked.
    const std::array<float, 3> position{at.x, at.y, at.z};
    const bool same = eye_probe_last_ && eye_probe_last_time_ != time && *eye_probe_last_ == position;
    eye_probe_last_ = position;
    eye_probe_last_time_ = time;
    if (same) {
        ++window.same;
        return;
    }
    EyeProbeFrame probed;
    probed.time = time;
    probed.space = space;
    probed.ahead = ahead;
    probed.located = {at.x, at.y, at.z};
    probed.used = {shown.x, shown.y, shown.z};
    const std::size_t kind = ahead ? 0 : 1;
    ++window.apart[kind];
    window.apart_sum_mm[kind] += 1000.0 * Distance(probed.located, probed.used);
    eye_probe_pending_.push_back(probed);
    while (eye_probe_pending_.size() > kEyeProbePending) {
        eye_probe_pending_.pop_front();
        ++window.unresolved;
    }
    ResolveEyeProbeLocked();
}

void FrameGenPresenter::NoteEyeProbeRealLocked(const XrCompositionLayerProjection& projection, XrTime display_time,
                                               std::uint64_t sequence) {
    if (projection.viewCount == 0 || !projection.views) {
        return;
    }
    const XrVector3f& at = projection.views[0].pose.position;
    EyeProbeReal real;
    real.time = display_time;
    real.space = projection.space;
    real.sequence = sequence;
    real.position = {at.x, at.y, at.z};
    if (!eye_probe_reals_.empty()) {
        const EyeProbeReal& before = eye_probe_reals_.back();
        if (before.sequence == sequence && before.time == display_time) {
            return; // this frame again
        }
        if (display_time <= before.time) {
            eye_probe_reals_.clear(); // times out of order: no line through them
        } else {
            real.continuous = before.sequence + 1 == sequence && before.space == projection.space;
        }
    }
    eye_probe_reals_.push_back(real);
    while (eye_probe_reals_.size() > kEyeProbeReals) {
        eye_probe_reals_.pop_front();
    }
    ResolveEyeProbeLocked();
}

void FrameGenPresenter::ResolveEyeProbeLocked() {
    if (eye_probe_reals_.empty()) {
        return;
    }
    const XrTime newest = eye_probe_reals_.back().time;
    const XrDuration pause = PauseLimitLocked();
    EyeProbeWindow& window = eye_probe_window_;
    for (auto it = eye_probe_pending_.begin(); it != eye_probe_pending_.end();) {
        if (it->time > newest) {
            ++it; // the real frame after it is still to come
            continue;
        }
        // Where the first eye was at the frame's time: on the line between
        // the real frames either side, each at its own display time (the
        // later one itself at its own time). Not across a pause, a gap in the
        // frames, another space or a tracking glitch.
        std::optional<std::array<double, 3>> truth;
        for (std::size_t i = 1; i < eye_probe_reals_.size(); ++i) {
            const EyeProbeReal& a = eye_probe_reals_[i - 1];
            const EyeProbeReal& b = eye_probe_reals_[i];
            if (it->time <= a.time || it->time > b.time) {
                continue;
            }
            const double seconds = static_cast<double>(b.time - a.time) / 1e9;
            if (b.continuous && a.space == it->space && b.space == it->space && b.time - a.time <= pause &&
                Distance(a.position, b.position) < kMaxEyeSpeed * seconds) {
                const double f = static_cast<double>(it->time - a.time) / static_cast<double>(b.time - a.time);
                truth = std::array<double, 3>{a.position[0] + f * (b.position[0] - a.position[0]),
                                              a.position[1] + f * (b.position[1] - a.position[1]),
                                              a.position[2] + f * (b.position[2] - a.position[2])};
            }
            break;
        }
        if (truth) {
            const std::size_t kind = it->ahead ? 0 : 1;
            ++window.resolved[kind];
            window.located_miss_sum_mm[kind] += 1000.0 * Distance(it->located, *truth);
            window.used_miss_sum_mm[kind] += 1000.0 * Distance(it->used, *truth);
        } else {
            ++window.unresolved;
        }
        it = eye_probe_pending_.erase(it);
    }
}

std::string FrameGenPresenter::EyeProbeTextLocked() const {
    const EyeProbeWindow& window = eye_probe_window_;
    const auto pair = [&](std::size_t kind) {
        return window.resolved[kind] ? EyeProbeMean(window.located_miss_sum_mm[kind], window.resolved[kind], 3) + "/" +
                                           EyeProbeMean(window.used_miss_sum_mm[kind], window.resolved[kind], 3)
                                     : std::string("-");
    };
    return ", eyeLocateMs=" +
           (window.calls ? EyeProbeMean(window.call_ms_sum, window.calls, 3) + "/" + Number(window.call_ms_max, 3)
                         : std::string("-")) +
           ", eyeLocateFromUsedMm=" + EyeProbeMean(window.apart_sum_mm[0], window.apart[0], 3) + "/" +
           EyeProbeMean(window.apart_sum_mm[1], window.apart[1], 3) + ", eyeLocateMissMm=" + pair(0) +
           ", eyeLocateBetweenMissMm=" + pair(1) + ", eyeLocate=calls:" + std::to_string(window.calls) +
           "/resolved:" + std::to_string(window.resolved[0]) + "+" + std::to_string(window.resolved[1]) +
           "/unresolved:" + std::to_string(window.unresolved) + "/failed:" + std::to_string(window.failed) +
           "/invalid:" + std::to_string(window.invalid) + "/untracked:" + std::to_string(window.untracked) +
           "/same:" + std::to_string(window.same) + "/slow:" + std::to_string(window.slow) +
           "/gone:" + std::to_string(window.gone) + (eye_probe_stopped_ ? "/stopped" : "");
}

void FrameGenPresenter::NoteGpuTimingLocked(std::uint64_t submission, const PendingFrame& frame,
                                            const std::optional<FrameGenGpuWindow>& window,
                                            const std::vector<FrameGenAnalysisTiming>& timings,
                                            Clock::time_point capture_pending_from, bool hardware_flow) {
    if (submission && frame.real_due != Clock::time_point{} && frame.generated_due != Clock::time_point{}) {
        gpu_due_.push_back({submission, frame.real_due, frame.generated_due, capture_pending_from, hardware_flow});
        while (gpu_due_.size() > 16) {
            gpu_due_.pop_front();
        }
    }
    // Analyses the GPU finished (newer than the window below, which is read
    // a few submissions late): when their newest capture ended against the
    // real frame's refresh, how long they ran, and whether motion was
    // measured before the frames moved by it were needed.
    for (const FrameGenAnalysisTiming& timing : timings) {
        const auto due = std::find_if(gpu_due_.begin(), gpu_due_.end(),
                                      [&](const GpuDue& entry) { return entry.submission == timing.submission; });
        if (due == gpu_due_.end()) {
            continue;
        }
        if (timing.capture_end != Clock::time_point{}) {
            const double done = Milliseconds(timing.capture_end - due->real_due);
            AddSample(capture_done_ms_, done);
            AddSample(guard_capture_done_ms_, done);
            if (due->capture_pending_from != Clock::time_point{}) {
                auto_capture_pending_ms_[auto_capture_pending_count_++ % auto_capture_pending_ms_.size()] =
                    std::max(0.0, Milliseconds(timing.capture_end - due->capture_pending_from));
            }
        }
        if (timing.start != Clock::time_point{} && timing.end != Clock::time_point{}) {
            AddSample(analysis_ms_, Milliseconds(timing.end - timing.start));
        }
        if (timing.capture_end != Clock::time_point{} && timing.end > timing.capture_end) {
            const double lag = Milliseconds(timing.end - timing.capture_end);
            analysis_lag_ms_ = analysis_lag_ms_ > 0.0 ? analysis_lag_ms_ * 0.9 + lag * 0.1 : lag;
            AddSample(measure_lag_ms_, lag);
        }
        if (timing.hardware_start != Clock::time_point{} && timing.hardware_end != Clock::time_point{}) {
            AddSample(ofa_ms_, Milliseconds(timing.hardware_end - timing.hardware_start));
        }
        if (timing.end != Clock::time_point{}) {
            ++totals_.analyses_timed;
            totals_.analyses_late += timing.end > due->generated_due ? 1 : 0;
            if (due->hardware_flow && timing.hardware_start != Clock::time_point{} &&
                timing.hardware_end != Clock::time_point{}) {
                ++totals_.hardware_timed;
                // Attribute lateness to the optical-flow work itself. A
                // shader pass after timely hardware is not hardware lag.
                if (timing.hardware_end > due->generated_due + interpolate_flow_policy_.hardware_grace) {
                    ++totals_.hardware_late;
                    totals_.hardware_lag_ns += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(timing.hardware_end - due->generated_due).count());
                }
            }
        }
    }
    if (!window) {
        return;
    }
    while (!gpu_due_.empty() && gpu_due_.front().submission <= window->submission) {
        const GpuDue due = gpu_due_.front();
        gpu_due_.pop_front();
        if (due.submission == window->submission) {
            AddSample(gpu_start_ms_, Milliseconds(window->start - due.real_due));
            AddSample(gpu_done_ms_, Milliseconds(window->end - due.generated_due));
        }
    }
}

bool FrameGenPresenter::Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                                 FrameGenSynthesisMode mode, const std::vector<float>& fractions,
                                 std::vector<std::vector<XrCompositionLayerProjectionView>>& sets,
                                 std::vector<std::uint8_t>& exact, bool release, Analysis* analyze,
                                 const FrameGenComposition* composition) {
    const auto capture_pending_from = Clock::now();
    const std::size_t count = analyze ? 0 : std::min(fractions.size(), kFrameGenMaxGenerated);
    if (count == 0 && !analyze) {
        return false;
    }
    // Each generated frame's time past the newest real frame (before it when
    // interpolating), in the fractions' units, and where the eyes move.
    std::vector<float> times(count, 0.0f);
    for (std::size_t index = 0; index < count; ++index) {
        times[index] = mode == FrameGenSynthesisMode::kExtrapolate ? fractions[index] : 1.0f - fractions[index];
    }
    std::vector<std::array<float, 3>> shifts(projection.viewCount, {0.0f, 0.0f, 0.0f});
    std::vector<FrameGenViewSubmission> submissions(projection.viewCount);
    std::vector<FrameGenOutputKind> kinds(projection.viewCount);
    FrameGenTuning tuning;
    bool lead_frame = false; // a frame on the engine path of a session in Lead (stage 6)
    std::string error;
    CaptureNotes notes; // for a research capture of the frame
    {
        std::scoped_lock lock(mutex_);
        if (!gpu_ || state_ == FrameGenState::kUnavailable) {
            return false;
        }
        tuning.mode = mode;
        tuning.spare = frame.spare;
        tuning.quality = settings_.flow_quality;
        tuning.hardware_flow = settings_.motion_estimation == FrameGenMotionEstimation::kHardware;
        // Select only at a real-frame boundary. Shader fallback leaves the
        // backend's optical-flow sessions alive; returning to them only
        // clears temporal hints for the intervening shader stretch.
        tuning.hardware_flow = UpdateInterpolateFlowLocked(Clock::now(), mode, tuning.hardware_flow);
        // Engine motion vectors: from stage 2 the backend keeps raw fields
        // for them (made as the views are set up), in a session whose frames
        // came with vectors the layer reads, for frames the engine path may
        // serve (two views analysed just in time, not composed: gate G9).
        const bool engine_path = engine_capturable_ && analyze && !composition && projection.viewCount == 2;
        // Once the game's vectors can stand in for measured flow (from
        // Assist), the flow ends at the hardware's level whenever the hardware
        // is chosen, whether it or the vectors measure a frame (Observe
        // changes nothing generated): in Lead the hardware is idle on the
        // frames the vectors lead, and Dynamic's interpolating on the shader
        // cores (above) leaves the level as it is too. By the mode the session
        // started in: a switch between Observe and Assist (or Lead) at a config
        // reload leaves the level as it is. Only for frames the vectors may
        // serve, on a backend that reads them: elsewhere (bridged, quad views,
        // Direct3D 11 or Vulkan) the flow stays at its quality's level, as
        // without them.
        tuning.hardware_level = settings_.motion_estimation == FrameGenMotionEstimation::kHardware &&
                                engine_started_ >= FrameGenEngineMode::kAssist && engine_path &&
                                gpu_->SupportsEngineMotion();
        // Stage 5: how the filters and the confidence view treat the cells
        // the vectors moved (research).
        tuning.engine_filter = engine_filter_;
        tuning.engine_cockpit = engine_cockpit_;
        tuning.engine_tint = engine_tint_;
        tuning.diagnostic = settings_.diagnostic_view;
        tuning.flow_view_fine = flow_view_fine_;
        tuning.artifact_suppression = settings_.artifact_suppression;
        tuning.motion_limit_percent = settings_.motion_limit_percent;
        tuning.generated = static_cast<std::uint8_t>(count);
        for (std::size_t index = 0; index < count; ++index) {
            tuning.fractions[index] = fractions[index];
        }
        // Just in time: motion only, filtered for cover frames as well when
        // interpolating.
        tuning.analyze_only = analyze != nullptr;
        tuning.cover = analyze && mode == FrameGenSynthesisMode::kInterpolate;
        // Quad views: the periphery moves as its focus view does where an
        // eye blends them, and a focus view limits its periphery's motion.
        tuning.composition = analyze ? composition : nullptr;
        tuning.history_ring = analyze && !composition ? jit_canvas_ring_ : 0;
        // The engine path's frames (above); elsewhere (a bridged session, a
        // game sending none) the backend makes no raw fields: kCensus. A
        // frame asking for none in between keeps those made. A session in
        // Lead holds the vectors at Assist until their verdicts let them
        // lead (stage 6, FrameGenEngineLeadControl).
        lead_frame = engine_path && engine_requested_ == FrameGenEngineMode::kLead;
        tuning.engine_mode = !engine_path                          ? std::min(engine_requested_, FrameGenEngineMode::kCensus)
                             : lead_frame && !engine_lead_.Leads() ? FrameGenEngineMode::kAssist
                                                                   : engine_requested_;
        tuning.engine_predicate = engine_predicate_;
        tuning.engine_interpolate = engine_interpolate_;
        // Stage 7: the engine motion map (research).
        tuning.engine_map = engine_map_;
        tuning.engine_map_reuse = engine_map_reuse_;
        tuning.engine_reach = engine_reach_;
        // Stage 8: what the previous frame did not see, and the forward map
        // (research).
        tuning.engine_visibility = engine_visibility_;
        tuning.engine_visibility_synthesis = engine_visibility_synthesis_;
        tuning.engine_bidir = engine_bidir_;
        // The frame's engine motion record and the frame before's, as the
        // frame gates read them: taken before a pause forgets them (just in
        // time, AnalyzeJit's pause check came first: such a frame counts as
        // one without a record).
        const std::optional<FrameGenEngineGateRecord> engine_record = EngineGateRecordLocked(engine_frame_);
        const std::optional<FrameGenEngineGateRecord> engine_before = EngineGateRecordLocked(engine_previous_);
        // Nothing is measured across a pause: the motion of a game that
        // stopped for a while says nothing about its next frames.
        const bool paused = last_real_.valid && frame.app_time - last_real_.display_time > PauseLimitLocked();
        if (paused) {
            NotePauseLocked(frame.app_time - last_real_.display_time);
            InvalidateContinuityLocked();
        }
        const bool continuous = last_real_.valid && last_real_.sequence + 1 == frame.sequence &&
                                last_real_.space == projection.space && last_real_.views.size() == projection.viewCount;
        FrameGenEngineGateFacts engine_facts;
        engine_facts.views = projection.viewCount;
        engine_facts.end_time = frame.end_time;
        // The record before was stamped with its frame's own sequence
        // (continuity steps over frames begun over and never ended).
        engine_facts.previous_sequence = last_real_.valid ? last_real_.generated_sequence : 0;
        engine_facts.continuous = continuous;
        engine_facts.paused = paused;
        engine_facts.decoded.resize(projection.viewCount);
        if (swapchain_created_at_) {
            engine_facts.frames_since_swapchain = totals_.app_frames - swapchain_created_frame_;
            engine_facts.seconds_since_swapchain = std::chrono::duration<double>(Clock::now() - *swapchain_created_at_).count();
        }
        engine_facts.captured = true;
        engine_facts.just_in_time = analyze != nullptr;
        engine_facts.composed = analyze && composition;
        // G10: one view's vectors exactly zero everywhere at its newest
        // census while another's are not.
        engine_facts.one_eye =
            std::any_of(engine_empty_.begin(), engine_empty_.end(), [](const auto& empty) { return empty && *empty; }) &&
            std::any_of(engine_empty_.begin(), engine_empty_.end(), [](const auto& empty) { return empty && !*empty; });
        bool flipped = false; // a view's depth decoded as the frame before described it
        if (continuous && last_real_.interval > 0 && frame.app_time > last_real_.display_time) {
            tuning.interval_ratio = std::clamp(static_cast<float>(frame.app_time - last_real_.display_time) /
                                                   static_cast<float>(last_real_.interval),
                                               0.25f, 4.0f);
        }
        if (continuous) {
            // Just in time the movement is kept forward in time: frames are
            // written past the newest frame or before it as each refresh needs.
            shifts = EyeShiftsLocked(projection, frame.app_time, analyze ? FrameGenSynthesisMode::kExtrapolate : mode,
                                     mode == FrameGenSynthesisMode::kExtrapolate);
            NoteEyeMovementLocked(projection, frame);
        }
        if (eye_probe_) {
            NoteEyeProbeRealLocked(projection, frame.app_time, frame.sequence);
        }
        notes.sequence = frame.sequence;
        notes.display_time = frame.app_time;
        notes.previous_time = continuous ? last_real_.display_time : 0;
        notes.previous_interval = continuous ? last_real_.interval : 0;
        notes.period = frame.period;
        notes.space = projection.space;
        notes.lead = lead_slots_;
        notes.mode = mode;
        notes.analyze = analyze != nullptr;
        notes.continuous = continuous;
        for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
            const auto& view = projection.views[i];
            const auto record = swapchains_.find(view.subImage.swapchain);
            if (record == swapchains_.end() || !record->second.released) {
                error = "a projection swapchain image was never released through the layer";
                break;
            }
            // A negative height is how some OpenVR translations flip an image
            // (OpenComposite without invertUsingShaders): OpenXR does not
            // allow it, and Frame Generation would capture nothing.
            const auto& rect = view.subImage.imageRect;
            if (rect.extent.width <= 0 || rect.extent.height <= 0 || rect.offset.x < 0 || rect.offset.y < 0) {
                error = "view " + std::to_string(i) + "'s image rectangle is " + std::to_string(rect.extent.width) +
                        "x" + std::to_string(rect.extent.height) + " at " + std::to_string(rect.offset.x) + "," +
                        std::to_string(rect.offset.y) +
                        ", and Frame Generation needs a positive size (a negative height flips the image, which "
                        "OpenXR does not allow; OpenComposite flips it with invertUsingShaders=true instead)";
                break;
            }
            if (record->second.sample_count > 1) {
                UnavailableLocked("multisampled projection swapchains are not supported");
                return false;
            }
            if (!record->second.capturable) {
                UnavailableLocked("the runtime refused the copy usage on the game's swapchains");
                return false;
            }
            const auto kind = gpu_->Classify(record->second.format);
            if (!kind) {
                UnavailableLocked("projection swapchain format " + std::to_string(record->second.format) +
                                  " is not supported");
                return false;
            }
            kinds[i] = *kind;
            auto& submission = submissions[i];
            submission.source = {view.subImage.swapchain, *record->second.released, view.subImage.imageArrayIndex,
                                 view.subImage.imageRect, record->second.format};
            submission.request.extent = Extent(view);
            if (analyze) {
                // As captured at release (OutputKindLocked): the history's
                // images match it, and frames written later encode for it.
                submission.request.output = OutputKindLocked(i, record->second.format).value_or(*kind);
            }
            submission.request.newest = Geometry(view);
            submission.request.synthesize = continuous && last_real_.views[i].extent.width == submission.request.extent.width &&
                                            last_real_.views[i].extent.height == submission.request.extent.height;
            submission.request.previous = submission.request.synthesize ? last_real_.views[i].geometry : Geometry(view);
            submission.request.temporal = true;
            submission.request.shift = shifts[i];
            if (i == 0) {
                depth_seen_ = true;
                depth_sent_ = depth_readable_ = false;
            }
            // Each depth capture is checked once, by the frame it was taken
            // for (as the backend uses it once): a frame whose depth was not
            // captured (a failed capture, quad views) has no encoding to check.
            std::optional<FrameGenDepthEncoding> decoded;
            if (i < depth_decoded_.size()) {
                decoded = std::exchange(depth_decoded_[i], std::nullopt);
            }
            if (const auto* depth = DepthInfo(view)) {
                const auto depth_record = swapchains_.find(depth->subImage.swapchain);
                submission.request.depth =
                    depth_record != swapchains_.end() && depth_record->second.depth_capturable;
                depth_sent_ = true;
                depth_readable_ = depth_readable_ || (submission.request.depth && gpu_->SupportsDepth());
                // The depth captured at its release was decoded as the frame
                // before described its own (the capture target recorded at
                // its end): the first frame after a game changes its encoding
                // (MSFS: nearZ 1000 in its menus, 356700 in flight) holds
                // depth decoded wrongly. With
                // VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK=1 the view goes
                // without depth for that frame.
                if (decoded) {
                    engine_facts.decoded[i] = decoded;
                    if (!SameFrameGenDepthDescription(*decoded,
                                                      {depth->minDepth, depth->maxDepth, depth->nearZ, depth->farZ})) {
                        flipped = true;
                        submission.request.depth = submission.request.depth && !depth_encoding_check_;
                    }
                }
            }
            engine_facts.captured = engine_facts.captured && record->second.captured_releases == record->second.releases;
        }
        if (!error.empty()) {
            NoteFailureLocked(error);
            return false;
        }
        const bool calibrate = EngineGatesLocked(projection, frame, engine_record ? &*engine_record : nullptr,
                                                 engine_before ? &*engine_before : nullptr, engine_facts, flipped);
        for (FrameGenViewSubmission& submission : submissions) {
            submission.request.engine_calibrate = calibrate;
        }
        if (engine_requested_ >= FrameGenEngineMode::kObserve) {
            EngineRequestsLocked(frame, calibrate, submissions);
        }
        EngineDeltaLocked(frame, submissions);
    }
    std::vector<OutputSpec> specs(projection.viewCount);
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        specs[i] = {Extent(projection.views[i]), kinds[i]};
    }
    if (count && !EnsureOutputs(specs, count, error)) {
        std::scoped_lock lock(mutex_);
        NoteFailureLocked(error);
        return false;
    }

    bool timed_out = false;
    if (count && !AcquireOutputs(count, error, timed_out)) {
        // A timed-out wait keeps its acquisition and is retried next frame;
        // an image the runtime has not handed over is never written or
        // released.
        std::scoped_lock lock(mutex_);
        if (!timed_out) {
            NoteFailureLocked(error);
        }
        ++repeated_;
        return false;
    }

    FrameGenExecuteResult executed;
    bool ok = true;
    double gpu_ms = -1.0;
    std::uint64_t working_bytes = 0;
    double execute_ms = 0.0; // the backend's Execute, on this thread
    std::optional<FrameGenGpuWindow> window;
    std::vector<FrameGenAnalysisTiming> timings;
    FrameGenDepthStatus depth;
    FrameGenWorldDeltaStatus world_delta;
    std::string hardware_status;
    {
        // Vulkan queues need external synchronization with the runtime's own
        // submissions on the session queue. Lock order: queue, GPU,
        // presenter; the backend may wait on fences, so not under mutex_.
        std::unique_lock<std::mutex> queue_lock;
        if (queue_mutex_) queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
        std::scoped_lock gpu_lock(gpu_mutex_);
        if (!gpu_) {
            ok = false;
            error = "the session ended";
        }
        for (std::size_t i = 0; i < submissions.size() && ok; ++i) {
            ok = gpu_->Track(submissions[i].source.swapchain, xr_.images, error);
            for (std::size_t set = 0; set < count && ok; ++set) {
                const Output& output = output_sets_[set][i];
                submissions[i].request.output = output.kind;
                submissions[i].outputs.push_back({output.swapchain, output.index, 0,
                                                  {{0, 0}, {static_cast<std::int32_t>(output.extent.width),
                                                            static_cast<std::int32_t>(output.extent.height)}},
                                                  output.format});
                ok = gpu_->Track(output.swapchain, xr_.images, error);
            }
        }
        if (ok) {
            if (const std::string directory = NextCaptureDirectory(); !directory.empty()) {
                gpu_->DumpNextExecute(directory);
                for (const FrameGenViewSubmission& submission : submissions) {
                    notes.declared.push_back(submission.request.newest);
                }
                WriteCaptureNotes(directory, notes);
            }
        }
        const auto execute_start = Clock::now();
        ok = ok && gpu_->Execute(submissions, tuning, executed, error);
        execute_ms = Milliseconds(Clock::now() - execute_start);
        if (gpu_) {
            gpu_ms = gpu_->LastGpuMilliseconds();
            window = gpu_->LastGpuWindow();
            timings = gpu_->TakeTimings();
            depth = gpu_->DepthStatus();
            world_delta = gpu_->WorldDeltaStatus();
            // Only the log reads it, and the backend asks the driver about
            // each of its images for it (50-200 of them): once a second.
            if (const auto now = Clock::now(); now >= working_bytes_due_) {
                working_bytes = gpu_->WorkingMemoryBytes();
                working_bytes_due_ = now + std::chrono::seconds(1);
            }
            if (tuning.hardware_flow) {
                hardware_status = gpu_->HardwareFlowStatus();
            }
        }
    }
    // Every output here was acquired and waited; release them all so the
    // next frame starts clean (or the caller does, once it may replace what
    // the runtime shows). Unwritten images are not submitted this frame.
    if (release && count) {
        ReleaseOutputs();
    }

    std::scoped_lock lock(mutex_);
    if (!ok) {
        NoteGpuTimingLocked(0, frame, std::nullopt, timings); // earlier analyses still finished
    }
    if (ok || executed.uncaptured) {
        // The backend's views are set up for this ring now (an uncaptured
        // frame's Execute sets them up too): CanvasRingLocked counts them as
        // in use.
        held_ring_ = FrameGenHistoryRing(tuning);
        held_slot_bytes_ = HistorySlotBytes(projection.views, projection.viewCount, EngineMapRingLocked());
    } else if (!ok) {
        // A failed recording may have freed the views' history (Direct3D 12
        // rebuilds them): none counts as held, so the next run's ring is
        // chosen as for images made anew. Counting too little held only
        // errs toward three.
        held_ring_ = 0;
        held_slot_bytes_ = 0;
    }
    if (!ok && executed.uncaptured) {
        // The first frame of a view, or one whose region changed: its image
        // was released before it could be captured. The next one will be.
        ++repeated_;
        if (logger_ && !logged_uncaptured_) {
            logged_uncaptured_ = true;
            logger_->Info("Frame Generation skipped a frame whose image was not captured when the game released it "
                          "(a view's first frame, or its region changed); the next frames are captured.");
        }
        return false;
    }
    if (!ok) {
        NoteFailureLocked(error);
        return false;
    }
    gpu_ms_ = gpu_ms;
    if (working_bytes) working_bytes_ = working_bytes;
    depth_status_ = depth;
    world_delta_ = world_delta;
    consecutive_failures_ = 0;
    // Stage 6: a session in Lead counts its frames (the controller holds the
    // vectors at Assist for so many), those the vectors led (the backend's
    // say), those held at Assist, and those led with the optical flow
    // hardware chosen and idle; the first frame they lead is logged once.
    if (lead_frame) {
        engine_lead_.AddFrame();
        const bool hardware_chosen = settings_.motion_estimation == FrameGenMotionEstimation::kHardware;
        ++engine_window_.lead_frames;
        engine_window_.led += executed.lead ? 1 : 0;
        engine_window_.held_at_assist += tuning.engine_mode == FrameGenEngineMode::kAssist ? 1 : 0;
        engine_window_.hardware_idle += executed.lead && hardware_chosen ? 1 : 0;
        if (executed.lead && logger_ && !logged_engine_lead_) {
            logged_engine_lead_ = true;
            logger_->Info(std::string("Frame Generation: the game's motion vectors lead from this frame "
                                      "(VECTORXR_FRAMEGEN_ENGINE_MV=lead; 30 verdicts in a row let them): ") +
                          (hardware_chosen ? "NVIDIA's optical flow hardware is idle on the frames they lead, and " : "") +
                          (tuning.engine_predicate
                               ? "a frame whose verdict holds them back is measured on the shader cores in the same "
                                 "submission"
                               : "no frame is measured (VECTORXR_FRAMEGEN_ENGINE_PREDICATE=0)") +
                          ", the flow at level " + std::to_string(FrameGenFinalFlowLevel(tuning)) + ".");
        }
    }
    // Stage 4: each of the world delta calibration's decisions (a lock, a
    // drop back to the fit) once.
    if (logger_ && world_delta.decisions > world_delta_decisions_logged_) {
        world_delta_decisions_logged_ = world_delta.decisions;
        logger_->Info(std::string("Frame Generation world vs delta: ") +
                      (world_delta.locked ? "locked, " : "calibrating, ") + world_delta.reason + ".");
    }
    const bool hardware_active = tuning.hardware_flow && hardware_status == "active";
    NoteGpuTimingLocked(executed.submission, frame, window, timings, capture_pending_from, hardware_active);
    if (tuning.hardware_flow && !hardware_status.empty()) {
        interpolate_flow_available_ = hardware_active;
    }
    interpolate_flow_active_ = hardware_active ? "hardware" : "shader";
    if (tuning.hardware_flow && !hardware_status.empty() && !hardware_active) {
        interpolate_flow_reason_ = "hardware unavailable: " + hardware_status;
    }
    if (logger_ && !hardware_status.empty() && hardware_status != logged_hardware_status_) {
        logged_hardware_status_ = hardware_status;
        // Its setup (the driver's optical flow API and sessions) runs in
        // the Execute that first reports it active: how long that held up
        // the calling thread (the game's, in its xrEndFrame).
        logger_->Info(hardware_status == "active"
                          ? "Frame Generation measures motion with NVIDIA's optical flow hardware (experimental; "
                            "setting it up took " +
                                Number(execute_ms, 1) + " ms on this frame)."
                          : "Frame Generation cannot use NVIDIA's optical flow hardware (" + hardware_status +
                                "); block matching measures motion instead.");
    }
    if (logger_ && depth.decisions != logged_depth_decisions_) {
        logged_depth_decisions_ = depth.decisions;
        const FrameGenDepthEvidence& evidence = depth.evidence;
        const double moved = evidence.decoded.samples;
        logger_->Info(
            std::string("Frame Generation checked the game's depth against the other eye: reading it ") +
            ToString(depth.reading) +
            (depth.reading == FrameGenDepthReading::kUnusable
                 ? std::string(", so head movement is compensated for rotation only")
                 : (std::abs(depth.scale - 1.0f) > 0.005f
                        ? " and " + Number(depth.scale, 3) +
                              " times as far as described (its near plane is described wrongly)"
                        : std::string()) +
                       ", head movement is compensated in full") +
            ". Where depth moves content (" + Number(evidence.binocular > 0.0 ? 100.0 * moved / evidence.binocular : 0.0, 0) +
            "% of what both eyes see) it leaves " + Number(evidence.decoded.Ratio(), 2) +
            " of the error without depth (swapped " + Number(evidence.swapped.Ratio(), 2) +
            "); the other eye disproves " + Number(moved > 0.0 ? 100.0 * evidence.decoded.fallback / moved : 0.0, 0) +
            "% of it (swapped " +
            Number(evidence.swapped.samples > 0.0 ? 100.0 * evidence.swapped.fallback / evidence.swapped.samples : 0.0,
                   0) +
            "%; a reading disproved on over half is not used), which is treated as far "
            "(cockpit glass written to depth, or depth wrong there)." +
            (depth.reading == FrameGenDepthReading::kUnusable &&
                     evidence.source_varied == 0.0
                 ? " The depth is the same all over the eye checked (cleared to one value), which shows nothing of "
                   "the scene."
                 : "") +
            (depth.reading == FrameGenDepthReading::kUnusable
                 ? " Some games only fill the depth they send while their own depth reprojection setting is on "
                   "(MSFS 2024: Motion reprojection set to Depth); it is checked again now and then."
                 : ""));
    }
    if (logger_ && depth.empty_view >= 0 && depth.empty_view != logged_empty_view_) {
        logged_empty_view_ = depth.empty_view;
        const int other = 1 - depth.empty_view;
        logger_->Info("Frame Generation: the game leaves view " + std::to_string(depth.empty_view) +
                      "'s depth clear while view " + std::to_string(other) + "'s varies (Star Citizen fills only one " +
                      "eye's): view " + std::to_string(depth.empty_view) + " takes view " + std::to_string(other) +
                      "'s depth, moved to its eye, and the depth check reads view " + std::to_string(other) +
                      ".");
    }
    if (logger_ && depth.batches != logged_depth_batches_ && logger_->IsDebugEnabled()) {
        // Every batch's evidence, decision or not (at most one line a
        // second), to follow the check through a session.
        logged_depth_batches_ = depth.batches;
        if (const auto now = Clock::now(); now - depth_batch_logged_at_ >= std::chrono::seconds(1)) {
            depth_batch_logged_at_ = now;
            const FrameGenDepthEvidence& e = depth.evidence;
            auto score = [&](const FrameGenDepthScore& s) {
                return "moves " + Number(e.binocular > 0.0 ? 100.0 * s.samples / e.binocular : 0.0, 1) +
                       "% (" + Number(s.samples, 0) + "), ratio " + Number(s.Ratio(), 3) + " (error " +
                       Number(s.error, 1) + " / none " + Number(s.none, 1) + "), other eye disproves " +
                       Number(s.samples > 0.0 ? 100.0 * s.fallback / s.samples : 0.0, 1) + "%";
            };
            std::string probes;
            const double centre = e.probes[kFrameGenDepthScaleProbes / 2];
            for (const double probe : e.probes) {
                probes += (probes.empty() ? "" : " ") + Number(centre > 0.0 ? probe / centre : 0.0, 3);
            }
            logger_->Debug("Frame Generation depth check batch " + std::to_string(depth.batches) + ": reading " +
                           ToString(depth.reading) + ", swapped=" + (depth.swapped ? "1" : "0") + ", scale=" +
                           Number(depth.scale, 4) + ", probeStep=" + Number(depth.probe_step, 3) +
                           ", binocular=" + Number(e.binocular, 0) + "; as decoded " + score(e.decoded) +
                           "; swapped " + score(e.swapped) + "; scale probes (error vs centre) " + probes +
                           " over " + Number(e.probed, 0) + " samples; nearer than 10 m " + Number(e.cockpit, 0) +
                           "; cells where depth varies: checked eye " + Number(e.source_varied, 0) + ", other eye " +
                           Number(e.other_varied, 0) + (depth.empty_view >= 0
                                                            ? " (view " + std::to_string(depth.empty_view) +
                                                                  " takes the other eye's depth)"
                                                            : std::string()) +
                           ".");
        }
    }
    const bool synthesized =
        std::any_of(executed.synthesized.begin(), executed.synthesized.end(), [](bool value) { return value; });
    if (synthesized) {
        ++depth_window_frames_;
        if (std::any_of(executed.depth.begin(), executed.depth.end(), [](bool value) { return value; })) {
            ++depth_window_used_;
        }
    }
    if (logger_ && !logged_depth_ &&
        std::any_of(executed.depth.begin(), executed.depth.end(), [](bool value) { return value; })) {
        logged_depth_ = true;
        logger_->Info("Frame Generation uses the game's depth: head movement is compensated exactly, so content "
                      "fixed in the cockpit stays still in generated frames.");
    }
    if (synthesized) {
        generated_ += count;
    } else {
        repeated_ += count;
    }
    if (logger_ && synthesized && !logged_first_generation_) {
        // Just in time (analyze) as well. Each view as the game submits it:
        // its region of the swapchain image and its field of view, where an
        // eye-tracked focus view's movement shows (issue #95).
        logged_first_generation_ = true;
        std::string views;
        for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
            const XrCompositionLayerProjectionView& view = projection.views[i];
            const auto degrees = [](float radians) { return Number(radians * 57.2957795, 1); };
            views += std::string(views.empty() ? "" : "; ") + std::to_string(view.subImage.imageRect.extent.width) +
                     "x" + std::to_string(view.subImage.imageRect.extent.height) + " at " +
                     std::to_string(view.subImage.imageRect.offset.x) + "," +
                     std::to_string(view.subImage.imageRect.offset.y) + " fov " + degrees(view.fov.angleLeft) + "/" +
                     degrees(view.fov.angleRight) + "/" + degrees(view.fov.angleUp) + "/" +
                     degrees(view.fov.angleDown);
        }
        logger_->Info("Frame Generation produced its first frames: views=" + std::to_string(projection.viewCount) + " (" +
                      views + "), generatedPerFrame=" + (analyze ? std::string("just in time") : std::to_string(count)) +
                      ", backend=" + gpu_->Name() +
                      (fractions.empty() ? std::string() : ", firstFraction=" + Number(fractions.front(), 3)) +
                      ", capturedAtRelease=" + std::to_string(captured_ > 0) + ".");
    }
    if (analyze) {
        analyze->frame = executed.frame;
        analyze->synthesized = synthesized;
        analyze->motion_not_before = InterpolateFlowDelayUntilLocked(Clock::now(), mode, hardware_active);
        analyze->kinds = kinds;
        analyze->velocity.assign(projection.viewCount, {0.0f, 0.0f, 0.0f});
        for (std::size_t i = 0; i < analyze->velocity.size() && i < executed.depth.size(); ++i) {
            if (executed.depth[i]) analyze->velocity[i] = shifts[i];
        }
        analyze->depth = std::any_of(executed.depth.begin(), executed.depth.end(), [](bool value) { return value; });
        // Every view with its depth (the engine motion map needs it): one eye
        // moved past one interval and the other not is no pair.
        analyze->merged = executed.merged.size() == projection.viewCount && executed.depth.size() == projection.viewCount;
        for (std::size_t i = 0; analyze->merged && i < executed.merged.size(); ++i) {
            analyze->merged = executed.merged[i] && executed.depth[i];
        }
        sets.clear();
        exact.clear();
        return executed.frame != 0;
    }
    sets.assign(count, {});
    exact.assign(count, 0);
    double moved_mm = 0.0;
    for (std::size_t set = 0; set < count; ++set) {
        // At the newest frame's own time (or written unchanged) a set is
        // that frame exactly: same image, same eye position.
        exact[set] = !synthesized || times[set] <= 0.0f ? 1 : 0;
        auto& views = sets[set];
        views.assign(projection.views, projection.views + projection.viewCount);
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i].next = nullptr; // a generated image has no matching depth
            views[i].subImage.swapchain = output_sets_[set][i].swapchain;
            views[i].subImage.imageRect = submissions[i].outputs[set].rect;
            views[i].subImage.imageArrayIndex = 0;
            if (i < executed.depth.size() && executed.depth[i]) {
                // Seen from where the eye will be (FrameGenViewRequest::shift);
                // the runtime still corrects the rotation.
                const std::array<double, 3> local{times[set] * shifts[i][0], times[set] * shifts[i][1],
                                                  times[set] * shifts[i][2]};
                const std::array<double, 3> world = Rotate(views[i].pose.orientation, local);
                views[i].pose.position.x += static_cast<float>(world[0]);
                views[i].pose.position.y += static_cast<float>(world[1]);
                views[i].pose.position.z += static_cast<float>(world[2]);
                moved_mm = std::max(moved_mm, 1000.0 * std::sqrt(local[0] * local[0] + local[1] * local[1] +
                                                                 local[2] * local[2]));
            }
        }
    }
    if (std::any_of(executed.depth.begin(), executed.depth.end(), [](bool value) { return value; })) {
        shift_window_sum_mm_ += moved_mm;
        shift_window_max_mm_ = std::max(shift_window_max_mm_, moved_mm);
        ++shift_window_frames_;
    }
    return true;
}

bool FrameGenPresenter::CaptureUnderWay() {
    if (capture_frames_left_ > 0) {
        return true;
    }
    const auto now = Clock::now();
    if (now - last_capture_check_ < std::chrono::seconds(1)) {
        return false;
    }
    last_capture_check_ = now;
    const char* local = std::getenv("LOCALAPPDATA");
    if (!local || !*local) {
        return false;
    }
    std::error_code ignored;
    const std::filesystem::path root = std::filesystem::path(local) / "VectorXR";
    const std::filesystem::path request = root / "framegen-capture.request";
    if (!std::filesystem::exists(request, ignored)) {
        return false;
    }
    // "capture", or with what it asks for: "capture frames=7 motion=all".
    std::string word;
    capture_frames_ = kCaptureFrames;
    capture_motion_ = false;
    for (std::ifstream text(request); text >> word;) {
        if (word.rfind("frames=", 0) == 0) {
            capture_frames_ = static_cast<std::uint32_t>(std::clamp(std::atoi(word.c_str() + 7), 1, 64));
        } else if (word.rfind("motion=", 0) == 0) {
            capture_motion_ = true;
        }
    }
    capture_motion_all_ = capture_motion_;
    capture_motion_armed_.reset();
    std::filesystem::remove(request, ignored);
    const std::time_t time = std::time(nullptr);
    std::tm local_time{};
    localtime_s(&local_time, &time);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local_time);
    capture_root_ = (root / "captures" / (std::string("framegen-") + stamp)).string();
    capture_frames_left_ = capture_frames_;
    capture_index_ = 0;
    return true;
}

std::string FrameGenPresenter::NextCaptureDirectory() {
    if (!CaptureUnderWay()) {
        return {};
    }
    std::error_code ignored;
    const std::filesystem::path directory =
        std::filesystem::path(capture_root_) / ("frame-" + std::to_string(capture_index_++));
    if (!std::filesystem::create_directories(directory, ignored) && !std::filesystem::exists(directory, ignored)) {
        capture_frames_left_ = 0;
        return {};
    }
    if (--capture_frames_left_ == 0 && logger_) {
        logger_->Info("Frame Generation captured " + std::to_string(capture_frames_) +
                      " real frames for offline replay in " + capture_root_ + ".");
    }
    return directory.string();
}

void FrameGenPresenter::WriteCaptureNotes(const std::string& directory, const CaptureNotes& notes) {
    // Apart from frame.txt, which the replay reads strictly: the frame as
    // the presenter saw it.
    {
        std::ofstream presenter(directory + "\\presenter.txt");
        presenter.precision(9);
        const XrDuration interval = notes.previous_time ? notes.display_time - notes.previous_time : 0;
        presenter << "sequence " << notes.sequence << "\ndisplay_time " << notes.display_time
                  << "\nprevious_display_time " << notes.previous_time << "\ninterval " << interval
                  << "\nprevious_interval " << notes.previous_interval << "\nratio "
                  << (notes.previous_interval > 0 && interval > 0
                          ? static_cast<double>(interval) / static_cast<double>(notes.previous_interval)
                          : 0.0)
                  << "\nperiod " << notes.period << "\nspace " << reinterpret_cast<std::uint64_t>(notes.space)
                  << "\nlead " << notes.lead << "\nmode " << ToString(notes.mode) << "\nanalyze " << notes.analyze
                  << "\ncontinuous " << notes.continuous << '\n';
    }
    // ...and its engine motion record, the frame before's poses, the census
    // worker's view of its images (FrameGenEngineCensusWorker).
    EngineFrame frame;
    EngineFrame previous;
    std::shared_ptr<FrameGenEngineCensusWorker> worker;
    FrameGenEngineMode mode = FrameGenEngineMode::kOff;
    std::optional<FrameGenEngineImageTracker::Hypothesis> image;
    FrameGenEngineValidation validation;
    FrameGenWorldDeltaStatus delta;
    {
        std::scoped_lock lock(mutex_);
        frame = engine_frame_;
        previous = engine_previous_;
        worker = engine_worker_;
        mode = engine_requested_;
        image = engine_image_;
        validation = engine_validation_;
        delta = world_delta_;
    }
    if (!frame.valid) {
        return;
    }
    std::ofstream motion(directory + "\\motion.txt");
    motion.precision(9);
    auto geometry = [&](const FrameGenViewGeometry& g) {
        motion << g.orientation[0] << ' ' << g.orientation[1] << ' ' << g.orientation[2] << ' ' << g.orientation[3] << ' '
               << g.position[0] << ' ' << g.position[1] << ' ' << g.position[2] << ' ' << g.angle_left << ' '
               << g.angle_right << ' ' << g.angle_up << ' ' << g.angle_down << '\n';
    };
    auto sub_image = [&](const XrSwapchainSubImage& image) {
        motion << reinterpret_cast<std::uint64_t>(image.swapchain) << ' ' << image.imageArrayIndex << ' '
               << image.imageRect.offset.x << ' ' << image.imageRect.offset.y << ' ' << image.imageRect.extent.width << ' '
               << image.imageRect.extent.height;
    };
    const bool paired = previous.valid && previous.frame.projection_frame + 1 == frame.frame.projection_frame;
    motion << "frame " << frame.frame.projection_frame << "\ndisplay_time " << frame.frame.display_time
           << "\nprevious_frame " << (previous.valid ? previous.frame.projection_frame : 0) << "\nprevious_display_time "
           << (previous.valid ? previous.frame.display_time : 0) << "\ncomplete " << frame.frame.complete << "\ninvalid "
           << frame.frame.invalid << "\nviews " << frame.frame.views.size() << '\n';
    for (std::size_t i = 0; i < frame.frame.views.size(); ++i) {
        const FrameGenEngineMotionView& view = frame.frame.views[i];
        motion << "view " << view.view << " source " << ToString(view.source) << " flags " << view.layer_flags
               << " problems " << view.problems << " frame_skip " << view.frame_skip << " relaxed "
               << view.relaxed_interval << '\n';
        motion << "view " << view.view << " vectors ";
        sub_image(view.vectors);
        motion << " format " << view.vectors_format << '\n';
        motion << "view " << view.view << " depth ";
        sub_image(view.depth);
        motion << " depth_is_khr " << view.depth_is_khr << '\n';
        motion << "view " << view.view << " color ";
        sub_image(view.color);
        motion << '\n';
        motion << "view " << view.view << " encoding " << view.depth_encoding.min_depth << ' '
               << view.depth_encoding.max_depth << ' ' << view.depth_encoding.near_z << ' ' << view.depth_encoding.far_z
               << '\n';
        motion << "view " << view.view << " decode " << view.encoding.ext << ' ' << view.encoding.scale[0] << ' '
               << view.encoding.scale[1] << ' ' << view.encoding.scale[2] << ' ' << view.encoding.offset[0] << ' '
               << view.encoding.offset[1] << ' ' << view.encoding.offset[2] << ' ' << view.encoding.two_d << '\n';
        motion << "view " << view.view << " delta " << view.app_delta.orientation.x << ' ' << view.app_delta.orientation.y
               << ' ' << view.app_delta.orientation.z << ' ' << view.app_delta.orientation.w << ' '
               << view.app_delta.position.x << ' ' << view.app_delta.position.y << ' ' << view.app_delta.position.z
               << '\n';
        motion << "view " << view.view << " game ";
        geometry(view.game);
        const FrameGenEngineMotionView* before = nullptr;
        for (const FrameGenEngineMotionView& candidate : previous.frame.views) {
            if (paired && candidate.view == view.view) before = &candidate;
        }
        if (before) {
            motion << "view " << view.view << " game_previous ";
            geometry(before->game);
        }
        if (view.view < notes.declared.size()) {
            motion << "view " << view.view << " declared ";
            geometry(notes.declared[view.view]);
        }
        motion << "view " << view.view << " indices " << frame.color_index[i] << ' ' << frame.depth_index[i] << ' '
               << (frame.released[i] ? static_cast<int>(*frame.released[i]) : -1) << '\n';
        if (worker) {
            if (const auto summary = worker->Summarize(view.view)) {
                motion << "view " << view.view << " policy " << static_cast<int>(summary->best.policy) << ' '
                       << summary->best.parameter << ' ' << summary->best.share << ' ' << summary->locked << " ("
                       << ToString(summary->best.policy) << ")\n";
                motion << "view " << view.view << " chosen "
                       << worker->Image(view.view, frame.frame.projection_frame, frame.color_index[i],
                                        frame.depth_index[i], frame.released[i])
                              .value_or(frame.color_index[i] % kFrameGenEngineImages)
                       << '\n';
                motion << "view " << view.view << " census " << summary->census_frame << ' ' << summary->images;
                for (std::uint32_t k = 0; k < summary->images; ++k) {
                    motion << ' ' << summary->hashes[k];
                }
                motion << '\n';
            }
        }
    }
    // The frame gates as Generate found them (stage 1; sequence 0: nothing
    // was generated from the frame with its record).
    std::string gates;
    for (std::size_t i = 0; i < kFrameGenEngineGateCount; ++i) {
        if ((frame.gates >> i) & 1u) gates += (gates.empty() ? "" : ",") + std::string(FrameGenEngineGateName(i));
    }
    motion << "sequence " << frame.sequence << "\ngates " << frame.gates << ' ' << (gates.empty() ? "none" : gates)
           << "\ncut " << frame.cut << "\npivot_moved " << frame.pivot_moved << "\ncalibrate " << frame.calibrate << '\n';
    // The switch, and the image VECTORXR_FRAMEGEN_ENGINE_MV_IMAGE names (stage
    // 2; none: the tracker's, as `chosen` above).
    motion << "engine_mode " << ToString(mode) << "\nimage_override "
           << (image ? std::string(ToString(image->policy)) + " " + std::to_string(image->parameter) : std::string("none"))
           << '\n';
    // The validator's convention (stage 3: its state, gains, camera, jitter
    // and offset); later stages' choices.
    if (mode >= FrameGenEngineMode::kObserve) {
        motion << "convention " << ToString(validation.state) << ' ' << validation.convention.gain[0] << ' '
               << validation.convention.gain[1] << ' ' << static_cast<int>(validation.convention.camera) << ' '
               << validation.convention.jitter << ' ' << validation.convention.offset[0] << ' '
               << validation.convention.offset[1] << " (" << (validation.name.empty() ? "?" : validation.name) << ", "
               << ToString(validation.convention.camera) << ")\n";
    } else {
        motion << "convention none\n";
    }
    // Stage 4: the world's motion from the application's movement (the
    // backend's mode, whether its calibration locked, the rotation's sign
    // and s_t; none while off).
    if (delta.mode != FrameGenWorldDeltaMode::kOff) {
        motion << "delta_model " << ToString(delta.mode) << ' ' << delta.locked << ' ' << delta.sign << ' ' << delta.scale
               << ' ' << delta.sky << '\n';
    } else {
        motion << "delta_model none\n";
    }
}

bool FrameGenPresenter::EnsureOutputs(const std::vector<OutputSpec>& specs, std::size_t sets, std::string& error,
                                      bool keep_replaced) {
    const std::uint32_t count = static_cast<std::uint32_t>(specs.size());
    if (!output_sets_.empty() && output_sets_.front().size() != count) {
        if (keep_replaced) {
            // Shown until a frame is written into the new ones.
            for (auto& set : output_sets_) {
                for (Output& output : set) {
                    if (output.swapchain) retired_outputs_.push_back(output);
                }
            }
            output_sets_.clear();
            std::scoped_lock lock(mutex_);
            output_kinds_.clear();
            extrapolated_ = {};
        } else {
            DestroyOutputs();
        }
    }
    if (output_kinds_.size() != count) {
        std::scoped_lock lock(mutex_);
        output_kinds_.assign(count, {});
    }
    // Near the video memory budget, sets above the session's ceiling go
    // (the ceiling never rises again, so they are not made again).
    std::size_t ceiling = output_sets_.size();
    {
        std::scoped_lock lock(mutex_);
        if (DynamicLocked()) {
            ceiling = DynamicSetCapLocked();
        } else if (settings_.multiplier == FrameGenMultiplier::kAuto && auto_memory_ceiling_ < kMaxMultiplier) {
            ceiling = auto_memory_ceiling_;
        }
    }
    if (output_sets_.size() > ceiling && ceiling >= sets) {
        TrimOutputs(ceiling);
    }
    // A higher multiplier adds sets; a lower one keeps them, unused, so
    // moving between multipliers never reallocates headset-resolution images
    // (hundreds of milliseconds on the frame thread at high resolutions).
    while (output_sets_.size() < sets) {
        output_sets_.emplace_back(count);
    }
    bool created = false;
    for (std::size_t set = 0; set < sets; ++set) {
        for (std::uint32_t i = 0; i < count; ++i) {
            Output& output = output_sets_[set][i];
            const FrameGenExtent extent = specs[i].extent;
            const FrameGenOutputKind kind = specs[i].kind;
            if (output.swapchain && output.extent.width == extent.width && output.extent.height == extent.height &&
                output.requested == kind) {
                continue;
            }
            if (output.swapchain) {
                {
                    std::scoped_lock lock(mutex_);
                    output_kinds_[i] = {};
                }
                if (keep_replaced) {
                    retired_outputs_.push_back(output);
                } else if (ForgetSwapchain(output.swapchain)) {
                    // Unfinished GPU work may still write it: then it is left
                    // to the runtime, which frees it with the session.
                    std::scoped_lock submit(submit_mutex_);
                    const PresenterCall call(*this, "xrDestroySwapchain");
                    xr_.destroy(output.swapchain);
                }
                output = {};
            }
            std::vector<std::int64_t> candidates;
            XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            {
                std::scoped_lock lock(mutex_);
                candidates = gpu_->OutputFormats(kind);
                if (kind != FrameGenOutputKind::kSrgb8) {
                    for (auto format : gpu_->OutputFormats(FrameGenOutputKind::kSrgb8)) candidates.push_back(format);
                }
                create.usageFlags = gpu_->OutputUsage();
            }
            std::int64_t format = 0;
            for (const auto candidate : candidates) {
                if (runtime_formats_.empty() ||
                    std::find(runtime_formats_.begin(), runtime_formats_.end(), candidate) != runtime_formats_.end()) {
                    format = candidate;
                    break;
                }
            }
            if (!format) {
                error = "the runtime offers no supported output swapchain format";
                return false;
            }
            create.format = format;
            create.sampleCount = 1;
            create.width = extent.width;
            create.height = extent.height;
            create.faceCount = 1;
            create.arraySize = 1;
            create.mipCount = 1;
            const XrResult result = xr_.create(session_, &create, &output.swapchain);
            if (XR_FAILED(result)) {
                output = {};
                error = "output swapchain creation failed (" + std::to_string(result) + ")";
                return false;
            }
            created = true;
            if (xr_.images) {
                xr_.images(output.swapchain, 0, &output.image_count, nullptr);
            }
            std::scoped_lock lock(mutex_);
            output.extent = extent;
            output.format = format;
            output.requested = kind;
            // An sRGB fallback for a UNORM or float source must also encode.
            output.kind = gpu_->Classify(format).value_or(FrameGenOutputKind::kSrgb8);
            output_kinds_[i] = {true, output.requested, output.kind};
            extrapolated_.valid = false;
        }
    }
    if (created) {
        // The allocation stalled this frame: let Auto settle again.
        std::scoped_lock lock(mutex_);
        multiplier_changed_at_ = Clock::now();
        cadence_.clear();
    }
    if (created) {
        // Rough video memory estimate: per view, two full-resolution history
        // copies, a full-resolution intermediate (unless generated frames are
        // drawn straight into the outputs) and the flow pyramid in the
        // backend, plus every output swapchain image.
        double working = 0.0, swapchains = 0.0;
        double copies = 3.0;
        {
            std::scoped_lock lock(mutex_);
            copies = gpu_ && gpu_->DrawsOutputs() ? 2.0 : 3.0;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const Output& first = output_sets_.front()[i];
            const double pixels = static_cast<double>(first.extent.width) * first.extent.height;
            const double bytes = first.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
            working += pixels * (copies * bytes + 3.4);
        }
        for (const auto& set : output_sets_) {
            for (const Output& output : set) {
                const double bytes = output.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
                swapchains += static_cast<double>(output.extent.width) * output.extent.height * bytes *
                              std::max<std::uint32_t>(output.image_count, 1);
            }
        }
        const double megabytes = 1024.0 * 1024.0;
        std::scoped_lock lock(mutex_);
        // Once generating, the backend reports what its images really take.
        if (working_bytes_) working = static_cast<double>(working_bytes_);
        output_bytes_ = static_cast<std::uint64_t>(swapchains);
        output_set_bytes_ = static_cast<std::uint64_t>(swapchains / static_cast<double>(output_sets_.size()));
        if (logger_) {
            logger_->Info("Frame Generation output swapchains ready: generatedPerFrame=" + std::to_string(sets) +
                          ", setsKept=" + std::to_string(output_sets_.size()) +
                          ", imagesPerSwapchain=" + std::to_string(output_sets_.front().front().image_count) +
                          ", estimated video memory " + Number((working + swapchains) / megabytes, 0) +
                          " MB (working " + Number(working / megabytes, 0) + " MB" + (working_bytes_ ? "" : " estimated") +
                          ", output swapchains " + Number(swapchains / megabytes, 0) + " MB).");
            LogMemoryBudgetLocked();
        }
    }
    return true;
}

bool FrameGenPresenter::AcquireOutputs(std::size_t sets, std::string& error, bool& timed_out) {
    timed_out = false;
    for (std::size_t index = 0; index < sets && index < output_sets_.size(); ++index) {
        for (Output& output : output_sets_[index]) {
            if (!output.acquired) {
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrResult acquired;
                {
                    std::scoped_lock submit(submit_mutex_);
                    const PresenterCall call(*this, "xrAcquireSwapchainImage");
                    acquired = xr_.acquire(output.swapchain, &acquire, &output.index);
                }
                if (XR_FAILED(acquired)) {
                    error = "output image acquire failed (" + std::to_string(acquired) + ")";
                    return false;
                }
                output.acquired = true;
                output.waited = false;
            }
            if (!output.waited) {
                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout = kOutputWaitTimeout;
                const XrResult waited = [&] {
                    const PresenterCall call(*this, "xrWaitSwapchainImage");
                    return xr_.wait(output.swapchain, &wait);
                }();
                if (waited == XR_TIMEOUT_EXPIRED) {
                    timed_out = true;
                    error = "output image wait timed out";
                    return false;
                }
                if (XR_FAILED(waited)) {
                    error = "output image wait failed (" + std::to_string(waited) + ")";
                    return false;
                }
                output.waited = true;
            }
        }
    }
    return true;
}

void FrameGenPresenter::ReleaseOutputs() {
    for (auto& set : output_sets_) {
        for (Output& output : set) {
            if (!output.acquired || !output.waited) {
                continue;
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrReleaseSwapchainImage");
                xr_.release(output.swapchain, &release);
            }
            output.acquired = false;
            output.waited = false;
        }
    }
}

void FrameGenPresenter::TrimOutputs(std::size_t keep) {
    {
        // Frames generated into the sets going are dropped: the presenter
        // thread shows the newest real frame until the next batch is ready.
        std::scoped_lock submit(submit_mutex_);
        std::scoped_lock lock(mutex_);
        extrapolated_.valid = false;
        generated_ready_ = false;
        last_shown_set_.reset();
    }
    double freed = 0.0;
    for (std::size_t set = keep; set < output_sets_.size(); ++set) {
        for (Output& output : output_sets_[set]) {
            if (!output.swapchain) continue;
            const double bytes = output.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
            freed += static_cast<double>(output.extent.width) * output.extent.height * bytes *
                     std::max<std::uint32_t>(output.image_count, 1);
            // Unfinished GPU work may still write it: then it is left to
            // the runtime, which frees it with the session.
            if (ForgetSwapchain(output.swapchain)) {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrDestroySwapchain");
                if (xr_.destroy) xr_.destroy(output.swapchain);
            }
        }
    }
    output_sets_.resize(keep);
    std::scoped_lock lock(mutex_);
    output_bytes_ = output_bytes_ > freed ? output_bytes_ - static_cast<std::uint64_t>(freed) : 0;
    if (logger_) {
        logger_->Info("Frame Generation freed " + Number(freed / (1024.0 * 1024.0), 0) +
                      " MB of output images near the video memory budget (" + std::to_string(keep) +
                      " sets kept).");
        LogMemoryBudgetLocked();
    }
}

void FrameGenPresenter::DestroyRetiredOutputs() {
    std::vector<Output> retired = std::move(retired_outputs_);
    retired_outputs_.clear();
    for (Output& output : retired) {
        if (output.acquired && output.waited) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            std::scoped_lock submit(submit_mutex_);
            const PresenterCall call(*this, "xrReleaseSwapchainImage");
            xr_.release(output.swapchain, &release);
        }
        // Unfinished GPU work may still write it: then it is left to the
        // runtime, which frees it with the session.
        if (output.swapchain && ForgetSwapchain(output.swapchain)) {
            std::scoped_lock submit(submit_mutex_);
            const PresenterCall call(*this, "xrDestroySwapchain");
            if (xr_.destroy) xr_.destroy(output.swapchain);
        }
    }
}

void FrameGenPresenter::DestroyOutputs() {
    DestroyRetiredOutputs();
    ReleaseOutputs();
    for (auto& set : output_sets_) {
        for (Output& output : set) {
            if (output.swapchain && ForgetSwapchain(output.swapchain)) {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrDestroySwapchain");
                if (xr_.destroy) xr_.destroy(output.swapchain);
            }
        }
    }
    output_sets_.clear();
    std::scoped_lock lock(mutex_);
    output_kinds_.clear();
    output_bytes_ = output_set_bytes_ = 0;
    extrapolated_ = {};
}

void FrameGenPresenter::NoteSubmissionLocked(bool distinct, bool generated) {
    const auto now = Clock::now();
    submissions_.push_back(now);
    ++totals_.submissions;
    if (distinct) {
        distinct_submissions_.push_back(now);
        ++totals_.distinct;
        totals_.generated_distinct += generated ? 1 : 0;
    } else {
        ++repeated_submissions_;
    }
}

void FrameGenPresenter::NoteEndCall(Clock::time_point entered) {
    const double ms = Milliseconds(Clock::now() - entered);
    std::scoped_lock lock(mutex_);
    AddSample(end_call_ms_, ms);
}

// --- Threaded presentation -----------------------------------------------------

void FrameGenPresenter::RecordDepthReleasesLocked(FrameCopy& copy) const {
    for (FrameCopy::Layer& layer : copy.layers) {
        layer.depth_releases.assign(layer.depth.size(), 0);
        for (std::size_t view = 0; view < layer.depth.size(); ++view) {
            if (const auto it = swapchains_.find(layer.depth[view].subImage.swapchain); it != swapchains_.end()) {
                layer.depth_releases[view] = it->second.releases;
            }
        }
    }
}

void FrameGenPresenter::DropStaleDepthLocked(FrameCopy& copy) const {
    // The runtime reads a swapchain's newest image: once the game has
    // released its next depth image, the copied chain describes that one.
    for (FrameCopy::Layer& layer : copy.layers) {
        for (std::size_t view = 0; view < layer.has_depth.size() && view < layer.depth_releases.size(); ++view) {
            const auto it = swapchains_.find(layer.depth[view].subImage.swapchain);
            if (layer.has_depth[view] && it != swapchains_.end() && it->second.releases != layer.depth_releases[view]) {
                layer.has_depth[view] = 0;
            }
        }
    }
}

void FrameGenPresenter::RecordColorReleasesLocked(FrameCopy& copy) const {
    for (FrameCopy::Layer& layer : copy.layers) {
        layer.color_releases.assign(layer.views.size(), 0);
        for (std::size_t view = 0; view < layer.views.size(); ++view) {
            if (const auto it = swapchains_.find(layer.views[view].subImage.swapchain); it != swapchains_.end()) {
                layer.color_releases[view] = it->second.releases;
            }
        }
    }
}

bool FrameGenPresenter::ColorReleasedSinceLocked(const FrameCopy& copy) const {
    for (const FrameCopy::Layer& layer : copy.layers) {
        for (std::size_t view = 0; view < layer.views.size() && view < layer.color_releases.size(); ++view) {
            const auto it = swapchains_.find(layer.views[view].subImage.swapchain);
            if (it != swapchains_.end() && it->second.releases != layer.color_releases[view]) {
                return true;
            }
        }
    }
    return false;
}

FrameGenPresenter::AsIs FrameGenPresenter::CurrentAsIsLocked(const FrameCopy& copy, std::uint64_t id, XrTime time) const {
    // A copy without its projection (a swapchain or space it used was
    // destroyed) shows nothing of the scene either.
    const auto current = [&](const FrameCopy& candidate) {
        return candidate.valid && !(candidate.has_projection && candidate.projection_stale) &&
               !ColorReleasedSinceLocked(candidate);
    };
    if (current(copy)) {
        return {&copy, id, time};
    }
    // The layer composed newer frames into the same swapchains: the runtime
    // would show the newest one's images with this frame's views. That
    // frame, kept already, is shown instead, unless a frame Frame
    // Generation writes comes first (the refresh showing it would step back).
    for (const JitFrame& frame : jit_frames_) {
        if (frame.time <= time) {
            continue;
        }
        if (!frame.as_is) {
            return {};
        }
        if (current(frame.copy)) {
            return {&frame.copy, frame.id, frame.time};
        }
    }
    return {};
}

bool FrameGenPresenter::CopyFrame(const XrFrameEndInfo& info, FrameCopy& copy, std::string& unsupported) {
    copy = {};
    if (!SupportedFrameChains(info, unsupported)) return false;
    copy.blend_mode = info.environmentBlendMode;
    for (std::uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* header = info.layers ? info.layers[i] : nullptr;
        if (!header) {
            unsupported = "a missing composition layer";
            return false;
        }
        FrameCopy::Layer layer;
        layer.type = header->type;
        for (auto* next = static_cast<const XrBaseInStructure*>(header->next); next; next = next->next) {
            FrameCopy::Layer::Extension extension;
            extension.type = next->type;
            if (next->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) {
                extension.color = *reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(next);
                extension.color.next = nullptr;
            } else {
                extension.blend = *reinterpret_cast<const XrCompositionLayerAlphaBlendFB*>(next);
                extension.blend.next = nullptr;
            }
            layer.extensions.push_back(extension);
        }
        switch (header->type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
            const auto& source = *reinterpret_cast<const XrCompositionLayerProjection*>(header);
            layer.projection = source;
            layer.projection.next = nullptr;
            if (source.viewCount && source.views) {
                layer.views.assign(source.views, source.views + source.viewCount);
            }
            layer.depth.assign(layer.views.size(), XrCompositionLayerDepthInfoKHR{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
            layer.has_depth.assign(layer.views.size(), 0);
            for (std::size_t view = 0; view < layer.views.size(); ++view) {
                const auto* next = static_cast<const XrBaseInStructure*>(layer.views[view].next);
                layer.views[view].next = nullptr;
                if (next && next->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                    layer.depth[view] = *reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(next);
                    layer.depth[view].next = nullptr;
                    layer.has_depth[view] = 1;
                }
            }
            if (!copy.has_projection) {
                copy.has_projection = true;
                copy.projection_index = static_cast<std::uint32_t>(copy.layers.size());
            }
            break;
        }
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            layer.quad = *reinterpret_cast<const XrCompositionLayerQuad*>(header);
            layer.quad.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            layer.cylinder = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(header);
            layer.cylinder.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            layer.equirect = *reinterpret_cast<const XrCompositionLayerEquirectKHR*>(header);
            layer.equirect.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            layer.equirect2 = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(header);
            layer.equirect2.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            layer.cube = *reinterpret_cast<const XrCompositionLayerCubeKHR*>(header);
            layer.cube.next = nullptr;
            break;
        default:
            unsupported = "composition layer type " + std::to_string(header->type);
            return false;
        }
        copy.layers.push_back(std::move(layer));
    }
    copy.valid = true;
    return true;
}

XrFrameEndInfo FrameGenPresenter::BuildFrame(FrameCopy& copy, const std::vector<XrCompositionLayerProjectionView>* generated,
                                            XrSpace generated_space, XrCompositionLayerFlags generated_flags,
                                            bool generated_depth,
                                            std::vector<const XrCompositionLayerBaseHeader*>& pointers) {
    pointers.clear();
    for (std::size_t i = 0; i < copy.layers.size(); ++i) {
        FrameCopy::Layer& layer = copy.layers[i];
        const void* header = nullptr;
        switch (layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            // A claimed quad frame's four views never reach the runtime: it
            // is shown only through the eye views composed from it.
            if (copy.has_projection && i == copy.projection_index && !generated &&
                (copy.projection_stale || copy.quad)) {
                continue;
            }
            if (generated && copy.has_projection && i == copy.projection_index) {
                // The real frame's depth describes its own image and eye
                // position. A generated frame moves content and is seen from
                // where the eye will be, so the depth would misplace near
                // edges for a runtime that corrects head movement with it;
                // generated frames are already seen from the predicted eye
                // position, so such a runtime only rotates them. An exact
                // copy of the real frame keeps it (while the game has not
                // released a newer depth image: DropStaleDepthLocked).
                layer.views = *generated;
                layer.depth.resize(layer.views.size(), XrCompositionLayerDepthInfoKHR{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
                layer.has_depth.resize(layer.views.size(), 0);
                if (!generated_depth) {
                    std::fill(layer.has_depth.begin(), layer.has_depth.end(), std::uint8_t{0});
                }
                layer.projection.space = generated_space;
                layer.projection.layerFlags = generated_flags;
            }
            for (std::size_t view = 0; view < layer.views.size(); ++view) {
                layer.views[view].next = layer.has_depth[view] ? &layer.depth[view] : nullptr;
            }
            layer.projection.viewCount = static_cast<std::uint32_t>(layer.views.size());
            layer.projection.views = layer.views.data();
            header = &layer.projection;
            break;
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            header = &layer.quad;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            header = &layer.cylinder;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            header = &layer.equirect;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            header = &layer.equirect2;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            header = &layer.cube;
            break;
        default:
            continue;
        }
        void* next = nullptr;
        for (auto extension = layer.extensions.rbegin(); extension != layer.extensions.rend(); ++extension) {
            if (extension->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) {
                extension->color.next = next;
                next = &extension->color;
            } else {
                extension->blend.next = next;
                next = &extension->blend;
            }
        }
        // All composition layer types share this header prefix.
        auto* base = reinterpret_cast<XrCompositionLayerBaseHeader*>(const_cast<void*>(header));
        base->next = next;
        pointers.push_back(base);
    }
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.environmentBlendMode = copy.blend_mode;
    info.layerCount = static_cast<std::uint32_t>(pointers.size());
    info.layers = pointers.empty() ? nullptr : pointers.data();
    return info;
}

XrResult FrameGenPresenter::RuntimeWait(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameWaitInfo* info,
                                        XrFrameState* state) {
    XrResult result = XR_SUCCESS;
    {
        const PresenterCall call(*this, "xrWaitFrame");
        result = calls.wait(session, info, state);
    }
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        ++runtime_waits_;
    }
    return result;
}

XrResult FrameGenPresenter::RuntimeBegin(const FrameGenRuntimeCalls& calls, XrSession session,
                                         const XrFrameBeginInfo* info) {
    XrResult result = XR_SUCCESS;
    {
        const PresenterCall call(*this, "xrBeginFrame");
        result = calls.begin(session, info);
    }
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        ++runtime_begins_;
        // A begin over a frame still open discards that frame.
        runtime_ends_ = std::max(runtime_ends_, runtime_begins_ - 1);
        runtime_waits_ = std::max(runtime_waits_, runtime_begins_);
        presenter_frame_open_ = false;
    }
    return result;
}

XrResult FrameGenPresenter::RuntimeEnd(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameEndInfo* info,
                                       bool paced) {
    XrResult result = XR_SUCCESS;
    if (trace_.Enabled() && info) {
        // What the runtime is given: its first projection layer's views.
        const XrCompositionLayerProjection* projection = nullptr;
        for (std::uint32_t i = 0; info->layers && i < info->layerCount && !projection; ++i) {
            if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
            }
        }
        const bool views = projection && projection->views && projection->viewCount > 0;
        trace_.Record("sub", 0, info->displayTime, info->layerCount,
                      views ? reinterpret_cast<std::int64_t>(projection->views[0].subImage.swapchain) : 0,
                      views ? projection->views[0].subImage.imageRect.extent.width : 0);
        if (views) {
            trace_.RecordPose("sub.v0", 0, projection->views[0].pose);
            if (projection->viewCount > 1) {
                trace_.RecordPose("sub.v1", 0, projection->views[1].pose);
            }
        }
    }
    const auto entered = Clock::now();
    {
        const PresenterCall call(*this, "xrEndFrame");
        result = calls.submit(session, info);
    }
    {
        std::scoped_lock lock(mutex_);
        AddSample(runtime_end_ms_, Milliseconds(Clock::now() - entered));
        // Counted even when the runtime rejects the frame: it is over either way.
        if (runtime_ends_ < runtime_begins_) {
            ++runtime_ends_;
        }
        presenter_frame_open_ = false;
        // The submission grid (ScheduleSubmissionLocked): a frame submitted
        // as it came (passed straight through) is the grid's newest point,
        // and so is a paced one that went more than half a refresh after it
        // was due (a hitch).
        const XrDuration spacing = SubmitSpacingLocked(last_period_);
        if (!paced || submit_grid_ == Clock::time_point{} ||
            entered - submit_grid_ > std::chrono::nanoseconds(spacing / 2)) {
            submit_grid_ = entered;
        }
    }
    WakeSlots();
    return result;
}

void FrameGenPresenter::NoteDisplayOffsetLocked(XrTime submitted, XrTime returned, XrDuration period) {
    if (period <= 0 || returned <= 0 || submitted <= 0) {
        return;
    }
    const double sample = static_cast<double>(submitted - returned) / static_cast<double>(period);
    if (sample < -2.0 || sample > 3.0) {
        return; // not a convention, just a stray time
    }
    display_offset_periods_ =
        display_offset_samples_ == 0 ? sample : display_offset_periods_ * 0.9 + sample * 0.1;
    if (display_offset_samples_ < 1000000) {
        ++display_offset_samples_;
    }
    if (logger_ && display_offset_samples_ == 30 && std::abs(display_offset_periods_) >= 0.1) {
        logger_->Info("Frame Generation: the game ends frames " + Number(display_offset_periods_, 2) +
                      " periods after the display time it was given; engaged frames are told that much earlier so "
                      "they are rendered for when they are shown.");
    }
}

XrDuration FrameGenPresenter::DisplayOffsetLocked(XrDuration period) const {
    if (display_offset_samples_ < 8 || std::abs(display_offset_periods_) < 0.1) {
        return 0;
    }
    return static_cast<XrDuration>(std::clamp(display_offset_periods_, -1.0, 2.0) * static_cast<double>(period));
}

void FrameGenPresenter::StripSpace(FrameCopy& copy, XrSpace space) {
    if (!copy.valid || !space) {
        return;
    }
    std::vector<FrameCopy::Layer> kept;
    kept.reserve(copy.layers.size());
    std::optional<std::uint32_t> projection_index;
    for (std::size_t i = 0; i < copy.layers.size(); ++i) {
        FrameCopy::Layer& layer = copy.layers[i];
        const bool primary = copy.has_projection && i == copy.projection_index;
        XrSpace layer_space = XR_NULL_HANDLE;
        switch (layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            layer_space = layer.projection.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            layer_space = layer.quad.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            layer_space = layer.cylinder.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            layer_space = layer.equirect.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            layer_space = layer.equirect2.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            layer_space = layer.cube.space;
            break;
        default:
            break;
        }
        if (layer_space == space) {
            if (!primary) {
                continue;
            }
            // Neither repeated nor reused for generated views, which share
            // its space: the presenter thread waits for the next real frame.
            copy.projection_stale = true;
        }
        if (primary) {
            projection_index = static_cast<std::uint32_t>(kept.size());
        }
        kept.push_back(std::move(layer));
    }
    copy.layers = std::move(kept);
    if (projection_index) {
        copy.projection_index = *projection_index;
    }
}

void FrameGenPresenter::StripSwapchain(FrameCopy& copy, XrSwapchain swapchain) {
    if (!copy.valid || !swapchain) {
        return;
    }
    std::vector<FrameCopy::Layer> kept;
    kept.reserve(copy.layers.size());
    std::optional<std::uint32_t> projection_index;
    for (std::size_t i = 0; i < copy.layers.size(); ++i) {
        FrameCopy::Layer& layer = copy.layers[i];
        const bool primary = copy.has_projection && i == copy.projection_index;
        bool uses = false;
        switch (layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            for (std::size_t view = 0; view < layer.views.size(); ++view) {
                if (layer.has_depth[view] && layer.depth[view].subImage.swapchain == swapchain) {
                    layer.has_depth[view] = 0;
                }
                if (layer.views[view].subImage.swapchain == swapchain) {
                    // The primary projection stays for generated views,
                    // which replace its images; it is never repeated.
                    if (primary) {
                        copy.projection_stale = true;
                    } else {
                        uses = true;
                    }
                }
            }
            break;
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            uses = layer.quad.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            uses = layer.cylinder.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            uses = layer.equirect.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            uses = layer.equirect2.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            uses = layer.cube.swapchain == swapchain;
            break;
        default:
            break;
        }
        if (uses) {
            continue;
        }
        if (primary) {
            projection_index = static_cast<std::uint32_t>(kept.size());
        }
        kept.push_back(std::move(layer));
    }
    copy.layers = std::move(kept);
    if (projection_index) {
        copy.projection_index = *projection_index;
    }
}

void FrameGenPresenter::WakeSlots() {
    slot_cv_.notify_all();
    if (compositor_wake_) {
        SetEvent(static_cast<HANDLE>(compositor_wake_));
    }
}

bool FrameGenPresenter::CompositorWait(std::unique_lock<std::mutex>& lock, Clock::time_point deadline,
                                       const std::function<bool()>& ready) {
    for (;;) {
        if (ready()) {
            return true;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto until = std::min(deadline, now + std::chrono::milliseconds(100));
        if (compositor_wake_ && compositor_timer_) {
            LARGE_INTEGER due{};
            due.QuadPart =
                -std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count() / 100);
            HANDLE handles[] = {static_cast<HANDLE>(compositor_wake_), static_cast<HANDLE>(compositor_timer_)};
            const bool armed = SetWaitableTimer(handles[1], &due, 0, nullptr, nullptr, FALSE) != 0;
            lock.unlock();
            // The wake event is set after every change the presenter thread
            // waits for, so nothing is missed between the check and here.
            WaitForMultipleObjects(armed ? 2 : 1, handles, FALSE, armed ? 200 : 2);
            lock.lock();
        } else {
            slot_cv_.wait_until(lock, until);
        }
    }
}

void FrameGenPresenter::StartCompositorLocked(const FrameGenRuntimeCalls& calls) {
    compositor_calls_ = calls;
    compositor_stop_ = compositor_drain_ = false;
    slot_request_.reset();
    slot_offer_.reset();
    offer_done_ = false;
    last_release_slot_.reset();
    last_offer_slot_.reset();
    auto_busy_count_ = 0;
    auto_capture_pending_count_ = 0;
    auto_busy_since_ = auto_end_held_from_ = auto_end_held_to_ = {};
    auto_end_held_frame_ = 0;
    newest_copy_ = passthrough_copy_;
    newest_copy_id_ = 0;
    newest_copy_fresh_ = false;
    generation_pending_ = generated_ready_ = false;
    next_generated_set_ = 0;
    jit_ = gpu_->SupportsJustInTime();
    // Quad views the layer's compose site offers are composed by Frame
    // Generation for this run (switching restarts it: NoteQuadEndLocked).
    quad_mode_ = jit_ && QuadCapableLocked() && quad_offered_;
    quad_switch_pending_ = false;
    quad_unwritten_in_row_ = 0;
    jit_interpolate_ = settings_.synthesis_mode == FrameGenSynthesisMode::kInterpolate;
    jit_canvas_ring_ = jit_ && !quad_mode_ ? CanvasRingLocked() : FrameGenHistoryRing(false);
    jit_frames_.clear();
    jit_shown_.reset();
    jit_views_.clear();
    jit_content_ = {};
    jit_shown_as_is_ = false;
    jit_content_id_ = 0;
    jit_content_time_ = 0;
    jit_started_ = false;
    last_target_time_ = 0;
    if (quad_mode_ && quad_decline_resumes_) {
        quad_decline_resumes_ = false;
        if (logger_) {
            logger_->Info("Frame Generation composes the quad views again.");
        }
    }
    // Pacing measured by an earlier run (another mode, rate or engagement)
    // does not describe this one.
    ResetLeadLocked();
    dynamic_interval_slots_ = 0.0;
    begun_capture_marks_.clear();
    ++totals_.restarts;
    InvalidateContinuityLocked();
    if (!compositor_timer_) {
        compositor_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (!compositor_wake_) {
        compositor_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    compositor_running_ = true;
    try {
        compositor_ = std::thread([this] { CompositorLoop(); });
    } catch (...) {
        compositor_running_ = false;
        threaded_disabled_ = true;
        if (logger_) {
            logger_->Error("Frame Generation could not start its presenter thread; presenting inline.");
        }
        return;
    }
    if (logger_ && !logged_threaded_) {
        logged_threaded_ = true;
        logger_->Info(std::string("Frame Generation presents from its own thread (") + gpu_->Name() + ", " +
                      ToString(settings_.synthesis_mode) +
                      (jit_ ? ", each frame written right before its refresh" : "") +
                      (quad_mode_ ? ", composing the quad views itself" : "") +
                      "): the game keeps rendering while generated frames are shown, and its frame waits are paced "
                      "without reaching the runtime.");
    }
}

void FrameGenPresenter::JoinCompositorLocked(std::unique_lock<std::mutex>& lock) {
    std::thread thread = std::move(compositor_);
    if (!thread.joinable()) {
        return;
    }
    lock.unlock();
    thread.join();
    lock.lock();
}

void FrameGenPresenter::DropUnclaimedLocked() {
    // Frames whose end has not started lose their slot; one already in its
    // end is handed its slot or released without one. One the ending thread
    // superseded is the newest it began: that thread's next call says whether
    // it was abandoned (a begin) or ends after all (BeginFrame, EndFrame).
    for (auto it = threaded_frames_.begin(); it != threaded_frames_.end();) {
        if (!it->claimed) {
            ++dropped_unended_;
            dropped_unbegun_ += it->begun ? 0 : 1;
            dropped_superseded_ += it->begun && it->superseded ? 1 : 0;
            it = threaded_frames_.erase(it);
        } else {
            ++it;
        }
    }
}

bool FrameGenPresenter::StopCompositorLocked(std::unique_lock<std::mutex>& lock,
                                             std::optional<std::chrono::milliseconds> limit) {
    // Also after the thread stopped itself: its frames would otherwise stay
    // in flight forever and hold every later wait.
    DropUnclaimedLocked();
    if (compositor_running_) {
        compositor_stop_ = true;
        WakeSlots();
        // It stops at its next slot, unless a runtime call holds it: never
        // waited on silently, and with a limit not past it.
        const auto started = Clock::now();
        const auto report = limit ? std::min(*limit, TeardownWait()) : TeardownWait();
        bool reported = false;
        while (!slot_cv_.wait_for(lock, std::chrono::milliseconds(20), [&] { return !compositor_running_; })) {
            const auto waited = Clock::now() - started;
            if (!reported && waited >= report) {
                reported = true;
                if (logger_) {
                    logger_->Error("Frame Generation: its presenter thread has not stopped after " +
                                   Number(std::chrono::duration<double>(waited).count(), 1) + " s (" +
                                   PresenterActivity() + ")" + (limit ? "; it is left stopping." : "."));
                }
            }
            if (limit && waited >= *limit) {
                return false;
            }
        }
    }
    JoinCompositorLocked(lock);
    compositor_stop_ = compositor_drain_ = false;
    slot_request_.reset();
    last_release_slot_.reset();
    generation_pending_ = generated_ready_ = false;
    newest_copy_ = {};
    jit_frames_.clear();
    jit_content_ = {};
    jit_views_.clear();
    jit_shown_.reset();
    jit_shown_as_is_ = false;
    // A claimed frame still ending is not shown (ThreadedEndJit): nothing
    // presents it any more.
    quad_mode_ = false;
    compositor_phase_ = "stopped";
    InvalidateContinuityLocked();
    WakeSlots();
    return true;
}

void FrameGenPresenter::ExitThreadedLocked(std::unique_lock<std::mutex>& lock) {
    compositor_drain_ = true;
    drain_started_slot_ = slot_index_;
    // This thread also ends frames: one it has not started ending would only
    // end after this wait returns, so it gives up its slot.
    if (std::this_thread::get_id() == end_thread_) {
        DropUnclaimedLocked();
    }
    WakeSlots();
    if (!slot_cv_.wait_for(lock, kThreadedDrain, [&] { return !compositor_running_; }) && logger_) {
        logger_->Error("Frame Generation: frames in flight did not end within 2 s; stopping the presenter thread.");
    }
    StopCompositorLocked(lock);
    if (logger_) {
        logger_->Info("Frame Generation stopped presenting from its own thread; frames go straight to the runtime.");
    }
}

void FrameGenPresenter::UpdateLeadLocked(bool late, std::optional<double> slack_ms, std::uint32_t lead,
                                         std::uint32_t multiplier) {
    if (lead_policy_.pinned_lead) {
        return; // tests: the lead stays where it was pinned
    }
    const auto now = Clock::now();
    if (lead_calm_since_ == Clock::time_point{}) {
        lead_calm_since_ = now;
    }
    // A probe nothing late undid within its judging time: its lead holds.
    // A lowering of the ring's ceiling taken for it waits, as long as a late
    // event at it awaits a second (RingJudgedLocked).
    if (lead_probe_ && now - lead_probe_->at >= lead_policy_.judge &&
        !(lead_probe_->ring && multiplier <= 4 && RingJudgedLocked(lead_ring_[multiplier], now))) {
        if (lead_floor_ == 0 || lead_slots_ < lead_floor_) {
            // Only back at the lowest lead a probe proved, from a floor late
            // frames raised: below it, tries still wait as long as the
            // failures there made them. A new lowest lead starts afresh.
            // Either way the next probe back waits explore_after.
            const bool returned =
                lead_slots_ == lead_floor_proven_ && lead_explore_after_ > lead_policy_.explore_after;
            lead_floor_ = lead_slots_;
            lead_floor_proven_ = lead_slots_;
            lead_return_after_ = lead_policy_.explore_after;
            if (!returned) {
                lead_explore_after_ = lead_policy_.explore_after;
                lead_floor_failed_ = false;
            }
        }
        lead_probe_.reset();
    }
    // How long a lead at or below the lowest known to hold waits before a
    // try below it. Above the lowest lead a probe proved (a floor late frames
    // raised): the probe back's own wait, explore_after until one fails. The
    // doubled wait is for below the proven lead, where tries failed; applied
    // above it too, it held a raised floor with a refresh to spare for most
    // of a minute (Star Citizen on Virtual Desktop, 2026-10-14: lead 3 held
    // about 56 s where 2 was proven, after two failed probes to 1 had doubled
    // the wait to 40 s).
    const bool above_proven = lead_floor_proven_ != 0 && lead_slots_ > lead_floor_proven_;
    const auto explore_wait = above_proven ? lead_return_after_ : lead_explore_after_;
    // Frames predicted with another lead say nothing about this one (those
    // in flight across a step would move it twice, or judge a probe by
    // frames it never set).
    if (lead == lead_slots_ && lead_probe_ && lead_probe_->grace > 0) {
        --lead_probe_->grace;
    } else if (lead == lead_slots_) {
        const double period_ms = static_cast<double>(std::max<XrDuration>(last_period_, 1)) / 1e6;
        // Slack: it could have arrived a refresh later and still made its slot.
        const bool spare = !late && slack_ms && *slack_ms > period_ms * 1.1;
        lead_history_.push_back(late ? 1 : spare ? 2 : 0);
        while (lead_history_.size() > 10) {
            lead_history_.pop_front();
        }
        const auto count = [&](std::uint8_t kind) {
            return static_cast<std::size_t>(std::count(lead_history_.begin(), lead_history_.end(), kind));
        };
        const bool hysteresis = lead_policy_.slack_hysteresis;
        // Late: two in the last ten frames. Spare: seven of the last ten.
        bool step_up = count(1) >= 2;
        bool step_down = lead_history_.size() >= 8 && count(2) >= 7;
        if (lead_policy_.hitch_events) {
            // Late: two hitches within event_window, each the late frames
            // within hitch_span of its first. A probe is still undone by two
            // late frames in the last ten: a lead too short is late frame
            // after frame, and judged by hitch events it showed a quarter
            // second of them before going back. (Judging spare time over a
            // longer window as well held a GPU-bound game, whose frames show
            // a refresh to spare only now and then, three slots higher after
            // a slow stretch: TestLeadStepsDownAfterHitches.)
            if (late && (!lead_event_start_ || now - *lead_event_start_ > lead_policy_.hitch_span)) {
                lead_event_start_ = now;
                lead_events_.push_back(now);
            }
            while (!lead_events_.empty() && now - lead_events_.front() > lead_policy_.event_window) {
                lead_events_.pop_front();
            }
            step_up = lead_probe_ ? count(1) >= 2 : lead_events_.size() >= 2;
        }
        // At the ring's lowered ceiling the ring's own verdict counts, by
        // hitch events whatever the policy steps by: a lead the ring holds
        // too short for the game is late frame after frame, a second event a
        // hitch_span after the first; a lone hitch no lead cures is one. By
        // the policy's two late frames in ten each hitch undid the lowering,
        // and a game hitching every few seconds kept the lead the ring is
        // short for (DCS on Virtual Desktop, Pierpaolo 2026-10-20:
        // TestLeadCeilingActsThroughHitches).
        std::uint32_t cap = LeadCapLocked(multiplier);
        const bool at_ceiling = cap < 4 * multiplier && lead_slots_ >= cap;
        const bool judged = at_ceiling && RingJudgedLocked(lead_ring_[multiplier], now);
        bool repeated = false; // late in a second event within event_window
        if (at_ceiling && late) {
            std::optional<Clock::time_point>& event = lead_ring_[multiplier].late_event;
            if (!event || now - *event > lead_policy_.hitch_span) {
                repeated = event && now - *event <= lead_policy_.event_window;
                event = now;
            }
        }
        if (judged && !repeated && step_up) {
            // A lone hitch at a lowering being judged (or its probe): it
            // stays, the hitch's frames forgotten.
            ClearLeadHistoryLocked();
            lead_calm_since_ = now;
            return;
        }
        // A second event at the ceiling is a step up of its own, judged or
        // not: it undoes a lowering being judged, or raises (or is refused)
        // after it. Left to the policy's own step up, frame by frame each
        // burst's second late frame stepped up and cleared the history, so
        // the next burst's first, the one in a second event, counted one late
        // frame: a ceiling that bursts of lateness showed too low was never
        // raised (TestLeadCeilingRaisesOnBursts).
        if (repeated) {
            step_up = true;
        }
        if (step_up) {
            // Late at the ring's ceiling in a second event: it is tried a
            // refresh higher again once its retry wait has run out (the ring
            // may hold that lead now: the game's capture timing changed),
            // else the lead stays (refused).
            // An undone lowering not taken for a probe: the policy's state
            // before it, put back once the lead is (as though never tried).
            const LeadRing* undone = nullptr;
            if (at_ceiling) {
                LeadRing& ring = lead_ring_[multiplier];
                if (judged) {
                    // Taken for the policy's probe, it fails below as one.
                    // Else the floors the lowering clamped come back, and
                    // the policy's next probe keeps its time (reset to now,
                    // it came a probe_after after the undo).
                    if (!lead_probe_) {
                        undone = &ring;
                    }
                    // Late in a second event while judged: undone at once,
                    // and the lead it lowered is needed (for explore_max). A
                    // late frame's refresh shows the frame before moved past
                    // itself, which a refresh lower stands where it stood at
                    // the lead above, its previous images as gone: the
                    // ring's cost stayed, and the lateness came on top (a
                    // game captured 83 ms after release, late at 7 and the
                    // ring a capture short at 8: 303 refreshes found their
                    // images replaced in 7 s against 302 at 8, and 282
                    // frames late against 21).
                    const std::uint32_t lowered = cap;
                    ring.ceiling = ring.trial_previous;
                    ring.needed = ring.trial_from;
                    ring.needed_at = now;
                    ring.trial.reset();
                    ring.late_event.reset();
                    ring.changed_at = now;
                    ++totals_.lead_ring_undone;
                    trace_.Record("ringcap", multiplier, ring.ceiling, lowered);
                    if (logger_ && logged_ring_moves_ < kLoggedRingMoves) {
                        ++logged_ring_moves_;
                        logger_->Info("Frame Generation puts the lead's ceiling at " + std::to_string(multiplier) +
                                      "x back to " +
                                      (ring.ceiling ? std::to_string(ring.ceiling) + " refreshes"
                                                    : std::string("none")) +
                                      ": frames were late at " + std::to_string(lowered) +
                                      " in two hitch events while it was judged; " + std::to_string(ring.needed) +
                                      " is kept though the ring is a capture short there.");
                    }
                    cap = LeadCapLocked(multiplier);
                } else if (repeated && now - ring.changed_at >= ring.retry) {
                    ring.ceiling = cap + 1 >= 4 * multiplier ? 0 : cap + 1;
                    ring.raised = true;
                    ring.late_event.reset();
                    ring.changed_at = now;
                    ++totals_.lead_ring_raises;
                    if (logger_ && logged_ring_moves_ < kLoggedRingMoves) {
                        ++logged_ring_moves_;
                        logger_->Info("Frame Generation tries the lead's ceiling at " + std::to_string(multiplier) +
                                      "x at " + std::to_string(cap + 1) + " refreshes again: frames were late at " +
                                      std::to_string(cap) + " (next try after " +
                                      Number(std::chrono::duration<double>(
                                                 std::min(ring.retry * 2, lead_policy_.explore_max))
                                                 .count(),
                                             1) +
                                      " s if their images are replaced again).");
                    }
                    cap = LeadCapLocked(multiplier);
                } else if (repeated) {
                    ++totals_.lead_ring_refused;
                }
            }
            if (lead_probe_) {
                // The probe (or step down) failed: the lead before it is the
                // lowest known to hold, and below it is tried only rarely
                // (more rarely after each failure there).
                const bool returning = lead_floor_proven_ != 0 && lead_probe_->from == lead_floor_ &&
                                       lead_floor_ > lead_floor_proven_;
                if (returning) {
                    // Back to the proven lead from a floor late frames raised:
                    // the raised floor holds, and the next probe back waits
                    // twice as long. The proven lead and the wait below it
                    // stay, as a hitch no lead cures (Star Citizen's, about
                    // every half minute) can come during the probe back.
                    // Taken as proving the raised lead, that coincidence gave
                    // it the wait the failures below the proven lead had
                    // doubled, so with hitches more often than that it stayed
                    // up, and the next coincidence lifted it a refresh more.
                    lead_return_after_ = std::min(lead_return_after_ * 2, lead_policy_.explore_max);
                } else {
                    const bool exploring = lead_floor_ != 0 && lead_probe_->from <= lead_floor_;
                    lead_floor_ = lead_probe_->from;
                    lead_floor_proven_ = lead_floor_;
                    lead_explore_after_ = exploring ? std::min(lead_explore_after_ * 2, lead_policy_.explore_max)
                                                    : lead_policy_.explore_after;
                    lead_return_after_ = lead_policy_.explore_after;
                }
                lead_floor_failed_ = true;
                if (!lead_probe_->ring) {
                    ++(lead_probe_->slack ? totals_.lead_downs_undone : totals_.lead_probes_failed);
                }
                lead_probe_.reset();
            } else if (hysteresis && lead_floor_ != 0 && lead_slots_ <= lead_floor_) {
                // Late at the lowest lead known to hold: it holds no longer.
                // Late there again lifts it no further than a refresh above
                // the lowest a probe proved: a hitch no lead cures (Star
                // Citizen's, every half minute) raised it a refresh each time,
                // and the lead with it, past what the kept real frames cover
                // (2026-10-13). A game that needs the raised lead fails the
                // probes back to the proven lead, which then come less often
                // (explore_wait).
                const std::uint32_t most = lead_floor_proven_ != 0 ? lead_floor_proven_ + 1 : lead_slots_ + 1;
                lead_floor_ = std::max(lead_floor_, std::min({lead_slots_ + 1, cap, most}));
                lead_floor_failed_ = true;
            }
            // At the cap nothing moves: not counted (the summary marks the cap).
            totals_.lead_ups += lead_slots_ < cap ? 1 : 0;
            lead_slots_ = std::min(lead_slots_ + 1, cap);
            ClearLeadHistoryLocked();
            lead_calm_since_ = now;
            if (undone) {
                lead_floor_ = undone->trial_floor;
                lead_floor_proven_ = undone->trial_proven;
                lead_floor_failed_ = undone->trial_floor_failed;
                lead_calm_since_ = undone->trial_calm;
            }
            return;
        }
        // Frames with a refresh to spare: one slot less, judged as a probe.
        // Below the lowest lead known to hold, once frames were late there,
        // only every explore_wait (as calm probes): a game whose frames
        // mostly fit a lead but a few in ten need one more otherwise steps
        // down and back up about once a second, each time with two late
        // frames (DCS and MSFS 2024, 2026-10-09).
        const bool exploring = lead_floor_ != 0 && lead_slots_ <= lead_floor_ && lead_floor_failed_;
        if (step_down && lead_slots_ > 1 &&
            (!hysteresis || !exploring || now - lead_calm_since_ >= explore_wait)) {
            if (hysteresis) {
                lead_probe_ = LeadProbe{now, lead_slots_};
                lead_probe_->slack = true;
            }
            --lead_slots_;
            ++totals_.lead_downs;
            ClearLeadHistoryLocked();
            lead_calm_since_ = now;
            return;
        }
    }
    // Calm: one slot less (a probe), every probe_after down to the lowest
    // lead known to hold, below it every explore_wait. A burst of hitches
    // leaves latency for seconds, not for good.
    if (lead_probe_ || lead_slots_ <= 1) {
        return;
    }
    if (now - lead_calm_since_ < CalmProbeWaitLocked()) {
        return;
    }
    lead_probe_ = LeadProbe{now, lead_slots_};
    --lead_slots_;
    ++totals_.lead_probes;
    ClearLeadHistoryLocked();
    lead_calm_since_ = now;
}

std::chrono::milliseconds FrameGenPresenter::CalmProbeWaitLocked() const {
    if (lead_floor_ == 0 || lead_slots_ > lead_floor_) {
        return lead_policy_.probe_after;
    }
    const bool above_proven = lead_floor_proven_ != 0 && lead_slots_ > lead_floor_proven_;
    return above_proven ? lead_return_after_ : lead_explore_after_;
}

void FrameGenPresenter::ScoreLeadLocked(JitFrame& frame, bool late) {
    if (frame.scored) {
        return;
    }
    frame.scored = true;
    // Slack counts only when its game frame was also on the GPU a refresh
    // early: one ending early but captured just in time needs its lead.
    UpdateLeadLocked(late, late || !frame.captured_early ? std::nullopt : frame.slack_ms, frame.lead,
                     frame.multiplier);
}

void FrameGenPresenter::ScoreErasedLocked(JitFrame& frame) {
    if (frame.scored) {
        return;
    }
    // Thread-safe without the GPU lock (FrameGenGpu::CaptureReached).
    const bool captured = !gpu_ || !frame.capture_mark || gpu_->CaptureReached(frame.capture_mark);
    ScoreLeadLocked(frame, !captured);
}

std::optional<std::uint64_t> FrameGenPresenter::GpuGateMarkLocked() const {
    if (!gpu_gate_enabled_ || !gpu_gate_armed_ || !gpu_) {
        return std::nullopt;
    }
    // The newest begun frame's mark covers the captures of the frame before
    // it: waiting for it leaves one game frame on the GPU, like a runtime.
    std::uint64_t mark = 0;
    if (gpu_frames_ahead_ <= 0) {
        mark = last_capture_mark_;
    } else if (begun_capture_marks_.size() >= static_cast<std::size_t>(gpu_frames_ahead_)) {
        mark = begun_capture_marks_[begun_capture_marks_.size() - static_cast<std::size_t>(gpu_frames_ahead_)];
    }
    return mark ? std::optional<std::uint64_t>(mark) : std::nullopt;
}

std::uint64_t FrameGenPresenter::ReachedCaptureMarkLocked(std::uint64_t known) const {
    // Thread-safe without the GPU lock (FrameGenGpu::CaptureReached).
    std::uint64_t low = known;
    std::uint64_t high = last_capture_mark_;
    if (!gpu_ || high <= low) {
        return low;
    }
    if (gpu_->CaptureReached(high)) {
        return high;
    }
    while (high - low > 1) {
        const std::uint64_t middle = low + (high - low) / 2;
        (gpu_->CaptureReached(middle) ? low : high) = middle;
    }
    return low;
}

void FrameGenPresenter::NoteGpuWaitLocked(double waited_ms, bool reached, bool progressed) {
    if (waited_ms > 0.5) {
        ++totals_.gpu_waits;
        AddSample(gpu_wait_ms_, waited_ms);
    }
    if (!reached) {
        ++totals_.gpu_wait_timeouts;
    }
    if (reached || progressed) {
        gpu_timeouts_in_row_ = 0;
        return;
    }
    if (++gpu_timeouts_in_row_ >= kGpuTimeoutsBeforeOff && gpu_gate_enabled_) {
        gpu_gate_enabled_ = false;
        if (logger_) {
            logger_->Error("Frame Generation: the GPU never reported the game's frames captured while its waits were "
                           "held (" + std::to_string(kGpuTimeoutsBeforeOff) +
                           " timeouts in a row, no capture running meanwhile); GPU-aware pacing is off for this "
                           "session.");
        }
    }
}

void FrameGenPresenter::NoteGpuEvidenceLocked(std::uint64_t real_frames, std::uint64_t late_gpu,
                                              std::vector<double> capture_done_ms) {
    if (gpu_gate_armed_ || !gpu_gate_enabled_) {
        return;
    }
    const std::optional<double> capture_done = Median(std::move(capture_done_ms));
    const bool late_frames = real_frames > 0 && late_gpu * 100 > real_frames;
    if (!late_frames && !(capture_done && *capture_done > 0.0)) {
        return;
    }
    ArmGpuGateLocked(late_frames ? std::to_string(late_gpu) + " of " + std::to_string(real_frames) +
                                       " real frames were not on the GPU when first needed"
                                 : "its frames were captured " + Number(*capture_done, 1) +
                                       " ms after their refresh, typically");
}

void FrameGenPresenter::ArmGpuGateLocked(const std::string& why) {
    if (gpu_gate_armed_ || !gpu_gate_enabled_) {
        return;
    }
    gpu_gate_armed_ = true;
    trace_.Record("gate", 0);
    if (logger_) {
        logger_->Info("Frame Generation holds the game's waits until its GPU catches up (" + why +
                      "): one game frame may stay queued on its GPU, as a runtime's wait allows.");
    }
}

std::optional<XrResult> FrameGenPresenter::ThreadedWaitLocked(std::unique_lock<std::mutex>& lock, XrFrameState* state,
                                                            const FrameGenRuntimeCalls& calls) {
    const auto asked = Clock::now();
    if (!compositor_running_) {
        JoinCompositorLocked(lock);
        if (!compositor_running_) {
            if (!WantsEngagementLocked() || !ThreadedEligibleLocked()) {
                return std::nullopt;
            }
            StartCompositorLocked(calls);
            if (!compositor_running_) {
                return std::nullopt;
            }
        }
    }
    // Like a runtime: a wait returns only once the previous frame has begun,
    // and at most two frames are in flight.
    slot_cv_.wait_for(lock, std::chrono::seconds(1), [&] {
        return !compositor_running_ ||
               (std::all_of(frames_.begin(), frames_.end(), [](const PendingFrame& frame) { return frame.begun; }) &&
                std::all_of(threaded_frames_.begin(), threaded_frames_.end(),
                            [](const ThreadedFrame& frame) { return frame.begun; }) &&
                threaded_frames_.size() < 2);
    });
    if (!compositor_running_) {
        return std::nullopt;
    }
    // Released every Nth refresh; a late application is released at once.
    // Dynamic releases at most once a refresh, like a runtime: the game runs
    // at its own rate and generated frames fill the refreshes it misses.
    const bool dynamic = DynamicLocked();
    const std::uint32_t multiplier = std::max(2u, EffectiveMultiplierLocked());
    const std::uint32_t cadence = dynamic ? 1 : multiplier;
    std::uint64_t release = last_release_slot_ ? *last_release_slot_ + cadence : slot_index_ + 1;
    release = std::max(release, slot_index_);
    const auto held_from = Clock::now();
    while (!slot_cv_.wait_for(lock, std::chrono::seconds(2),
                              [&] { return !compositor_running_ || slot_index_ >= release; })) {
        if (logger_ && !logged_watchdog_) {
            logged_watchdog_ = true;
            logger_->Error("Frame Generation: the presenter thread has published no refresh for 2 s (" +
                           PresenterActivity() + ").");
        }
    }
    if (!compositor_running_) {
        return std::nullopt;
    }
    // The game's own time for its last frame: since the last wait's hold for
    // the cadence ended (Auto's probes; its GPU's hold below counts as its).
    // Not just in time, its xrEndFrame is held for its frame's slot too
    // (ThreadedEnd): left out, or a game at 2x reads as taking at least the
    // refresh to that slot, however light it is. Only the end of the frame
    // the last wait handed out: a pipelined game's earlier frame ends on
    // another thread while this one is still working, and that time is the
    // game's own.
    const auto held = Clock::now() - held_from;
    if (auto_busy_since_ != Clock::time_point{} && asked > auto_busy_since_) {
        const Clock::duration end_held =
            auto_end_held_frame_ != threaded_id_
                ? Clock::duration::zero()
                : std::max(std::min(auto_end_held_to_, asked) - std::max(auto_end_held_from_, auto_busy_since_),
                           Clock::duration::zero());
        auto_busy_[auto_busy_count_++ % auto_busy_.size()] = Milliseconds(asked - auto_busy_since_ - end_held);
    }
    auto_busy_since_ = asked + held;
    // The cadence counts from the refresh it released this wait in, not from
    // the end of a GPU wait below: a game held for its GPU is not also held
    // back a cadence.
    last_release_slot_ = slot_index_;
    // A runtime's wait holds the game until its GPU has caught up; this one
    // must too, or the game runs frames ahead on its GPU and they are
    // captured after their refresh (one game frame may stay queued).
    if (const std::optional<std::uint64_t> mark = GpuGateMarkLocked()) {
        if (gpu_->CaptureReached(*mark)) {
            // The GPU keeps up: earlier timeouts were a hitch.
            gpu_timeouts_in_row_ = 0;
            gpu_reached_mark_ = std::max(gpu_reached_mark_, *mark);
        } else {
            // Timeouts in a row turn holding off only while nothing runs: a
            // capture finishing since the last wait means a slow GPU (a
            // hitch), not a stuck one.
            const std::uint64_t before = gpu_reached_mark_;
            ++gpu_waiters_;
            FrameGenGpu* const gpu = gpu_.get();
            const XrDuration wait_period = slot_period_ > 0 ? slot_period_ : std::max<XrDuration>(last_period_, 1);
            // At least a few refreshes, and two of the game's own frames: a
            // game whose GPU runs slower than that for a while is held, not
            // let go after each frame.
            const auto timeout = std::max({std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::nanoseconds(4 * wait_period)),
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::nanoseconds(static_cast<std::int64_t>(2.0 * real_interval_ns_))),
                                           std::chrono::milliseconds(50)});
            const std::uint64_t waited_for = *mark;
            lock.unlock();
            const auto begun = Clock::now();
            const bool reached = gpu->WaitCapture(waited_for, timeout);
            const double waited_ms = Milliseconds(Clock::now() - begun);
            lock.lock();
            --gpu_waiters_;
            idle_cv_.notify_all();
            gpu_reached_mark_ = reached ? std::max(gpu_reached_mark_, waited_for) : ReachedCaptureMarkLocked(before);
            NoteGpuWaitLocked(waited_ms, reached, gpu_reached_mark_ > before);
            if (!compositor_running_) {
                return std::nullopt;
            }
        }
    }
    // Turned off, or switched to what this thread does not present, while
    // the game waited: it leaves the presenter thread now, as its next wait
    // would, not handed one more frame and logged engaged (6 of 9 presses
    // turning Frame Generation off in two sessions' logs did).
    if (!StayThreadedLocked()) {
        ExitThreadedLocked(lock);
        return std::nullopt;
    }
    // The game's frames are timed on the refresh grid (GameGridLocked).
    const XrDuration period = GameGridLocked();
    const double period_ms = static_cast<double>(period) / 1e6;
    const bool interpolate = settings_.synthesis_mode == FrameGenSynthesisMode::kInterpolate;
    // Interpolating just in time, a frame is shown after the frames between
    // the previous real frame and it, which are needed from the refresh
    // after the previous one's: it must end about a real frame interval
    // earlier than it is shown.
    const bool interpolate_jit = interpolate && jit_;
    ExpireRingCeilingLocked(multiplier, Clock::now());
    const std::uint32_t cap = LeadCapLocked(multiplier);
    if (lead_slots_ == 0) {
        // Until measured, assume the frame takes one application frame.
        const double latency = app_latency_ms_ > 0.0 ? app_latency_ms_ : period_ms * multiplier;
        const double interval = interpolate_jit ? (dynamic ? latency : period_ms * multiplier) : 0.0;
        lead_slots_ = lead_policy_.pinned_lead
                          ? lead_policy_.pinned_lead
                          : static_cast<std::uint32_t>(std::clamp(std::ceil((latency + interval + 1.0) / period_ms) -
                                                                      (interpolate_jit ? 1.0 : 0.0),
                                                                  1.0, static_cast<double>(cap)));
    }
    // The cap follows the multiplier (Dynamic's refreshes per real frame, 2-4),
    // and the history ring's ceiling below that (LeadCapLocked): a lead above
    // a lowered cap is the cap, or no frame (each told the cap) would match
    // it to be scored, and it would come down only by a probe (DCS 231933:
    // leadSlots=9 in the summary while the game was told 8).
    if (lead_slots_ > cap) {
        ClampLeadLocked(cap);
    }
    // Predict the first slot the frame will really reach. Interpolated
    // frames before it are shown first, so the frame itself is shown (and
    // rendered for) multiplier - 1 slots later; just in time they are made
    // for the slots it is shown after instead.
    const std::uint64_t slots = std::clamp<std::uint64_t>(lead_slots_, 1, cap);
    const std::uint64_t between = interpolate && !interpolate_jit ? multiplier - 1 : 0;
    ThreadedFrame frame;
    frame.id = ++threaded_id_;
    frame.multiplier = multiplier;
    frame.interpolate = interpolate;
    frame.jit = jit_;
    frame.lead = static_cast<std::uint32_t>(slots);
    frame.first_slot = slot_index_ + slots;
    frame.first_time = slot_time_ + period * static_cast<XrDuration>(slots);
    frame.target_slot = frame.first_slot + between;
    frame.target_time = frame.first_time + period * static_cast<XrDuration>(between);
    if (jit_) {
        // Each real frame has a refresh of its own after the one before (a
        // shorter lead would give two the same refresh, and only the newer
        // would ever be shown).
        if (last_target_time_ && frame.target_time < last_target_time_ + period - period / 2) {
            const XrTime behind = last_target_time_ + period - frame.target_time;
            const std::uint64_t extra = static_cast<std::uint64_t>((behind + period / 2) / period);
            frame.first_slot += extra;
            frame.target_slot += extra;
            frame.first_time += period * static_cast<XrDuration>(extra);
            frame.target_time += period * static_cast<XrDuration>(extra);
        }
        last_target_time_ = frame.target_time;
    }
    // Dynamic paces the game like a runtime (once a refresh at most), so it
    // is told the refresh period; a fixed or automatic multiplier, its N.
    frame.reported_period = period * static_cast<XrDuration>(dynamic ? 1 : multiplier);
    frame.predicted =
        std::max(frame.target_time - DisplayOffsetLocked(frame.reported_period), last_returned_time_ + 1);
    frame.wait_return = Clock::now();
    last_returned_time_ = frame.predicted;
    // For the summary: the lead told, and how far past the runtime's own
    // display time for a wait returning now (its last paced wait's, moved on
    // with the time) the game is told to render: Frame Generation's share of
    // its pose horizon. Dynamic releases the game anywhere in a refresh, so
    // that varies by up to a refresh from one frame to the next.
    told_lead_sum_ += frame.lead;
    ++told_lead_frames_;
    told_lead_max_ = std::max(told_lead_max_, frame.lead);
    ++told_lead_counts_[frame.lead];
    if (phase_wall_ != Clock::time_point{}) {
        // As below, from the presenter thread's last paced wait however long
        // ago (the clocks run on together), so every window has it.
        const XrTime now_xr =
            phase_xr_ + std::chrono::duration_cast<std::chrono::nanoseconds>(frame.wait_return - phase_wall_).count();
        AddSample(told_ahead_ms_, static_cast<double>(frame.target_time - now_xr) / 1e6, kMaxFrameSamples);
    }
    if (phase_wall_ != Clock::time_point{} && frame.wait_return - phase_wall_ <= std::chrono::nanoseconds(4 * period)) {
        const XrTime runtime_time =
            phase_xr_ + std::chrono::duration_cast<std::chrono::nanoseconds>(frame.wait_return - phase_wall_).count();
        const double ahead_ms = static_cast<double>(frame.target_time - runtime_time) / 1e6;
        AddSample(game_ahead_ms_, ahead_ms, kMaxFrameSamples);
        // Against the frame told before, unless a pause came between: one by
        // the game's own pace (PauseLimitLocked), not Dynamic's refresh
        // period, or a game slower than four refreshes a frame (Star Citizen
        // at 28 fps at 120 Hz) had none.
        if (last_ahead_at_ != Clock::time_point{} &&
            frame.wait_return - last_ahead_at_ <= std::chrono::nanoseconds(PauseLimitLocked())) {
            AddSample(ahead_jitter_ms_, std::abs(ahead_ms - last_ahead_ms_), kMaxFrameSamples);
        }
        last_ahead_ms_ = ahead_ms;
        last_ahead_at_ = frame.wait_return;
    } else {
        last_ahead_at_ = {};
    }
    threaded_frames_.push_back(frame);
    state->predictedDisplayTime = frame.predicted;
    state->predictedDisplayPeriod = frame.reported_period;
    state->shouldRender = slot_should_render_ ? XR_TRUE : XR_FALSE;
    if (full_rate_) {
        // From standing by (Dynamic's, or Auto at full rate) to a fixed or
        // automatic multiplier: logged engaged, as the inline path logs it
        // (Dynamic logs its own engagement).
        full_rate_ = false;
        if (state_ == FrameGenState::kActive && !dynamic) {
            state_ = FrameGenState::kWaiting;
        }
    }
    SetStateLocked(FrameGenState::kActive, "");
    return XR_SUCCESS;
}

XrResult FrameGenPresenter::ThreadedEnd(XrSession session, const XrFrameEndInfo& info, const FrameGenRuntimeCalls& calls,
                                        const ThreadedFrame& frame, Clock::time_point entered,
                                        const FrameGenQuadFrame* quad) {
    bool pass_through;
    {
        std::scoped_lock lock(mutex_);
        pass_through = unsupported_composition_;
    }
    if (frame.jit && (!pass_through || frame.quad)) {
        // A claimed quad frame cannot be sent as four raw views to stereo.
        // Unsupported projection/view chains were declined before claiming;
        // a new frame/other-layer chain after that claim skips this refresh.
        return ThreadedEndJit(info, frame, entered, quad);
    }
    const XrCompositionLayerProjection* projection = FindProjection(info);
    const bool renderable = info.layerCount > 0 && projection != nullptr;
    FrameCopy copy;
    std::string unsupported;
    // Refused for composition metadata, nothing is presented from a copy
    // again, and CopyFrame would only report the refused chain.
    const bool copied = !pass_through && CopyFrame(info, copy, unsupported);
    std::unique_lock lock(mutex_);
    end_thread_ = std::this_thread::get_id();
    NoteDisplayOffsetLocked(info.displayTime, frame.predicted, frame.reported_period);
    // The time the app rendered for: what it ends the frame with, unless that
    // is far from anything it was told.
    const XrTime content_time =
        std::llabs(info.displayTime - frame.target_time) <= 2 * frame.reported_period ? info.displayTime : frame.target_time;
    const double latency = std::chrono::duration<double, std::milli>(entered - frame.wait_return).count();
    app_latency_ms_ = app_latency_ms_ <= 0.0 ? latency : app_latency_ms_ * 0.9 + latency * 0.1;
    app_work_ms_ += latency;
    ++app_work_frames_;
    if (!renderable) {
        ++totals_.non_renderable;
    } else if (!QuadTargetsLocked(quad)) {
        // Quad views Frame Generation could compose keep their four views
        // as targets (ClaimQuadFrame).
        RecordCaptureTargetsLocked(*projection);
    }
    if (!copied && !pass_through && !threaded_disabled_) {
        // The next wait returns to inline presentation.
        threaded_disabled_ = true;
        if (logger_) {
            logger_->Info("Frame Generation presents inline: the game submits " + unsupported +
                          ", which the presenter thread cannot repeat between real frames.");
        }
    }
    auto erase_frame = [&] {
        threaded_frames_.erase(std::remove_if(threaded_frames_.begin(), threaded_frames_.end(),
                                              [&](const ThreadedFrame& pending) { return pending.id == frame.id; }),
                               threaded_frames_.end());
    };
    // How much later it could have arrived and still made its slot: the
    // presenter thread waits for a due real frame until half a refresh after
    // the slot's wait would have returned.
    std::optional<double> slack_ms;
    if (phase_wall_ != Clock::time_point{} && frame.first_time > phase_xr_) {
        const XrDuration period = std::max<XrDuration>(last_period_, 1);
        const auto deadline = phase_wall_ + std::chrono::nanoseconds(frame.first_time - phase_xr_ + period / 2);
        slack_ms = std::chrono::duration<double, std::milli>(deadline - entered).count();
    }
    const XrTime previous_time = last_real_.display_time;
    const XrDuration period = GameGridLocked();
    // When the runtime releases the wait of the slot shown at `time`.
    auto due = [&](XrTime time) {
        return phase_wall_ != Clock::time_point{} && time > phase_xr_
                   ? phase_wall_ + std::chrono::nanoseconds(time - phase_xr_)
                   : Clock::time_point{};
    };
    const Clock::time_point first_due = due(frame.first_time);
    // Interpolating, the frames between the previous real frame and this one
    // fill the slots before it: generated now, the first shown from here.
    const bool interpolate = !pass_through && frame.interpolate && renderable && copied && frame.multiplier >= 2;
    std::vector<std::vector<XrCompositionLayerProjectionView>> between;
    std::vector<std::uint8_t> between_exact;
    std::vector<XrTime> between_targets;
    bool between_ok = false;
    if (interpolate) {
        lock.unlock();
        // The frames between the previous real frame and this one, then this
        // frame itself (fraction 1 reproduces it) for its own slot, N - 1
        // slots after it ended: by then the game may have released its next
        // image into the same swapchain, and the runtime shows a swapchain's
        // newest image. Released to the runtime only once this frame has its
        // slot: until then the output swapchains keep showing the previous
        // real frame's frames, still being presented.
        std::vector<float> fractions;
        for (std::uint32_t slot = 0; slot < frame.multiplier && fractions.size() < kFrameGenMaxGenerated; ++slot) {
            const XrTime target = frame.first_time + period * static_cast<XrDuration>(slot);
            between_targets.push_back(target);
            fractions.push_back(slot + 1 == frame.multiplier
                                    ? 1.0f
                                    : FrameGenFraction(FrameGenSynthesisMode::kInterpolate, previous_time, content_time,
                                                       target));
        }
        PendingFrame pending;
        pending.app_time = content_time;
        pending.end_time = info.displayTime;
        pending.sequence = frame.id;
        pending.real_due = pending.generated_due = first_due;
        between_ok =
            Generate(*projection, pending, FrameGenSynthesisMode::kInterpolate, fractions, between, between_exact, false);
        if (between_ok) RecordReal(*projection, content_time, frame.id);
        lock.lock();
    }
    // Ask for the first slot at or after the first one this frame fills.
    slot_request_ = SlotRequest{frame.id, frame.first_slot};
    WakeSlots();
    const auto slot_asked = Clock::now();
    while (!slot_cv_.wait_for(lock, std::chrono::seconds(2), [&] {
        return !compositor_running_ || (slot_offer_ && slot_offer_->frame_id == frame.id);
    })) {
        if (logger_ && !logged_watchdog_) {
            logged_watchdog_ = true;
            logger_->Error("Frame Generation: a real frame has waited 2 s for a slot (" + PresenterActivity() + ").");
        }
    }
    // Held for the cadence, as the game's waits are: not its own time
    // (ThreadedWaitLocked).
    auto_end_held_from_ = slot_asked;
    auto_end_held_to_ = Clock::now();
    auto_end_held_frame_ = frame.id;
    if (!slot_offer_ || slot_offer_->frame_id != frame.id) {
        // The presenter thread stopped first; no runtime frame is left for
        // this one.
        if (slot_request_ && slot_request_->frame_id == frame.id) {
            slot_request_.reset();
        }
        erase_frame();
        WakeSlots();
        lock.unlock();
        if (interpolate) {
            ReleaseOutputs();
        }
        return XR_SUCCESS;
    }
    const SlotOffer offer = *slot_offer_;
    lock.unlock();
    if (interpolate) {
        ReleaseOutputs();
    }

    // A late frame skips the frames interpolated for the slots it missed;
    // one late for its own slot is shown itself (the game has not released
    // a newer image yet).
    const std::size_t skip = offer.slot > frame.first_slot ? static_cast<std::size_t>(offer.slot - frame.first_slot) : 0;
    const bool show_between = interpolate && between_ok && skip + 1 < between.size();
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult result = RuntimeBegin(calls, session, &begin_info);
    if (XR_SUCCEEDED(result)) {
        XrFrameEndInfo real = info;
        std::vector<const XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection generated_layer{};
        std::vector<XrCompositionLayerProjectionView> generated_views;
        if (show_between) {
            // The projection layer replaced by the first interpolated frame;
            // the newest frame's depth chain (valid during this end) only on
            // an exact copy of it.
            std::uint32_t projection_index = 0;
            FindProjection(info, &projection_index);
            layers.assign(info.layers, info.layers + info.layerCount);
            generated_views = between[skip];
            const bool depth = skip < between_exact.size() && between_exact[skip];
            for (std::size_t i = 0; i < generated_views.size() && i < projection->viewCount; ++i) {
                generated_views[i].next = depth ? projection->views[i].next : nullptr;
            }
            generated_layer = *projection;
            generated_layer.viewCount = static_cast<std::uint32_t>(generated_views.size());
            generated_layer.views = generated_views.data();
            layers[projection_index] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&generated_layer);
            real.layers = layers.data();
        }
        real.displayTime = offer.time;
        if (!offer.should_render) {
            real.layerCount = 0;
            real.layers = nullptr;
        }
        result = RuntimeEnd(calls, session, &real, true);
    }
    bool generate = !pass_through && !frame.interpolate && XR_SUCCEEDED(result) && renderable && copied && offer.should_render &&
                    frame.multiplier >= 2;

    lock.lock();
    // Frames to extrapolate: one per refresh until the next real frame's,
    // and a spare for that refresh in case the frame is late. Dynamic counts
    // the refreshes to the next frame's predicted slot (it is usually in
    // flight already). One due in the very next refresh still gets its
    // spare: a frame not generated from leaves the next one without a
    // previous frame to measure motion against. Only a game holding full
    // refresh skips generation.
    std::uint32_t count = frame.multiplier;
    bool spare = true;
    if (DynamicLocked()) {
        // A pause is not the game's pace.
        if (last_offer_slot_ && offer.slot > *last_offer_slot_ &&
            static_cast<XrDuration>(offer.slot - *last_offer_slot_) * std::max<XrDuration>(period, 1) <=
                PauseLimitLocked()) {
            const double interval = static_cast<double>(offer.slot - *last_offer_slot_);
            dynamic_interval_slots_ =
                dynamic_interval_slots_ > 0.0 ? dynamic_interval_slots_ * 0.9 + interval * 0.1 : interval;
        }
        std::uint64_t gap = DynamicSlotsLocked();
        for (const ThreadedFrame& next : threaded_frames_) {
            if (next.id > frame.id) {
                gap = next.first_slot > offer.slot ? next.first_slot - offer.slot : 1;
                break;
            }
        }
        const bool full_rate = dynamic_interval_slots_ > 0.0 && dynamic_interval_slots_ < 1.25;
        count = gap <= 1 && full_rate
                    ? 0
                    : static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>(gap, 1),
                                                                         DynamicSetCapLocked()));
        spare = count == gap;
        generate = generate && count > 0;
    }
    last_offer_slot_ = offer.slot;
    NoteSubmissionLocked(offer.should_render,
                         show_between && !(skip < between_exact.size() && between_exact[skip]));
    last_shown_set_.reset(); // a real frame, or a set not among the sets below
    // From here the presenter thread fills its slots from this frame:
    // extrapolating, again until the frames generated from it are ready,
    // then those; interpolating, the rest of the frames before it, then it.
    newest_copy_ = copied ? std::move(copy) : FrameCopy{};
    generated_ready_ = false;
    generation_pending_ = generate;
    next_generated_set_ = 0;
    if (frame.interpolate) {
        extrapolated_ = {};
        extrapolated_.interpolated = true;
        if (interpolate && between_ok && compositor_running_) {
            // The rest of the frames before it, then its copy; only the copy
            // when it was shown itself, repeated if the next frame is late.
            const std::size_t first = show_between ? skip + 1 : between.size() - 1;
            extrapolated_.valid = true;
            extrapolated_.sets.assign(between.begin() + static_cast<std::ptrdiff_t>(first), between.end());
            extrapolated_.exact.assign(between_exact.begin() + static_cast<std::ptrdiff_t>(first), between_exact.end());
            extrapolated_.generation = next_generation_++;
            extrapolated_.targets.assign(between_targets.begin() + static_cast<std::ptrdiff_t>(first),
                                         between_targets.end());
            extrapolated_.space = projection->space;
            extrapolated_.flags = projection->layerFlags;
            generated_ready_ = true;
        }
    }
    RecordDepthReleasesLocked(newest_copy_);
    const Clock::time_point real_due = due(offer.time);
    const Clock::time_point generated_due = due(offer.time + period);
    const bool late = offer.slot > frame.first_slot;
    totals_.late_cpu += late ? 1 : 0;
    // One with no projection moves the lead neither way (ThreadedEndJit).
    if (renderable) {
        UpdateLeadLocked(late, slack_ms, frame.lead, frame.multiplier);
    }
    erase_frame();
    offer_done_ = true;
    if (pass_through) {
        // The original frame just reached the runtime while its pointers
        // were alive. Nothing older may replace it as the thread drains.
        jit_frames_.clear();
        jit_content_ = {};
        jit_views_.clear();
        jit_shown_.reset();
        newest_copy_ = {};
        compositor_drain_ = true;
        drain_started_slot_ = slot_index_;
    }
    WakeSlots();
    if (renderable && offer.should_render) {
        ++healthy_streak_;
    }
    if (XR_FAILED(result)) {
        PauseLocked("runtime rejected the real frame (" + std::to_string(result) + ")");
        InvalidateContinuityLocked();
        return result;
    }
    lock.unlock();

    if (generate) {
        std::vector<float> fractions;
        std::vector<XrTime> targets;
        // One frame per generated slot, plus a spare for the slot the next
        // real frame is due in: if that frame is late, the slot still shows
        // motion instead of repeating the last generated frame.
        for (std::uint32_t slot = 1; slot <= count && fractions.size() < kFrameGenMaxGenerated; ++slot) {
            const XrTime target = offer.time + period * static_cast<XrDuration>(slot);
            targets.push_back(target);
            fractions.push_back(FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, previous_time, content_time, target));
        }
        PendingFrame pending;
        pending.app_time = content_time;
        pending.end_time = info.displayTime;
        pending.sequence = frame.id;
        pending.real_due = real_due;
        pending.generated_due = generated_due;
        pending.spare = spare && fractions.size() == count;
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        std::vector<std::uint8_t> exact;
        const bool ok = Generate(*projection, pending, FrameGenSynthesisMode::kExtrapolate, fractions, sets, exact);
        if (ok) RecordReal(*projection, content_time, frame.id);
        lock.lock();
        if (!ok) InvalidateContinuityLocked();
        if (compositor_running_) {
            extrapolated_.valid = ok;
            extrapolated_.interpolated = false;
            extrapolated_.sets = std::move(sets);
            extrapolated_.exact = std::move(exact);
            extrapolated_.generation = next_generation_++;
            extrapolated_.targets = std::move(targets);
            extrapolated_.space = projection->space;
            extrapolated_.flags = projection->layerFlags;
            generated_ready_ = ok;
        } else {
            // The thread stopped meanwhile: nothing it generated belongs to
            // what comes next.
            InvalidateContinuityLocked();
        }
        generation_pending_ = false;
        WakeSlots();
    } else {
        lock.lock();
        if (!renderable || (frame.interpolate && !between_ok)) {
            InvalidateContinuityLocked();
        }
    }
    app_frames_.push_back(entered);
    ++totals_.app_frames;
    NoteRealEndLocked(entered);
    PendingFrame cadence;
    cadence.engaged = true;
    cadence.multiplier = frame.multiplier;
    cadence.period = period;
    UpdateAutoLocked(entered, cadence);
    SummaryLocked(entered);
    return result;
}

void FrameGenPresenter::SummaryLocked(Clock::time_point now) {
    if (state_ != FrameGenState::kActive || now - last_summary_ < summary_interval_) {
        return;
    }
    // Rates are the window's averages; counts are the window's, with the
    // session's in parentheses where the OSD shows them.
    const double seconds =
        last_summary_ == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last_summary_).count();
    const Totals& start = summary_totals_;
    const auto delta = [&](std::uint64_t Totals::*field) { return totals_.*field - start.*field; };
    const double standby = StandbySecondsLocked(now);
    NoteGpuEvidenceLocked(delta(&Totals::app_frames), delta(&Totals::late_gpu), capture_done_ms_);
    if (logger_) {
        LogMemoryBudgetLocked();
        // A fixed or Auto rate is the most the game is let run: a slower
        // game takes more refreshes per real frame (just in time measures
        // them for every mode).
        const std::string multiplier =
            DynamicLocked() ? "dynamic (" + Number(dynamic_interval_slots_, 2) + " refreshes per real frame)"
                            : MultiplierText(EffectiveMultiplierLocked()) +
                                  (settings_.multiplier == FrameGenMultiplier::kAuto ? " (auto)" : "") +
                                  (jit_ && dynamic_interval_slots_ > 0.0
                                       ? " (" + Number(dynamic_interval_slots_, 2) + " refreshes per real frame)"
                                       : "");
        // lateReal: real frames ended after their refresh was due / not on
        // the GPU when first chosen. gpuWaits: game waits held for its GPU
        // (count/median ms/timeouts). captureDoneMs: an analysis's newest
        // capture ended this long after the real frame's refresh was due.
        // unready: refreshes whose real frame was not captured / moved
        // frames written unmoved (motion not measured yet). unwritten: an
        // output not handed back in time / writing failed / no outputs /
        // nothing to write. endCallMs: the game's xrEndFrame in Frame
        // Generation. runtimeEndMs: the runtime's xrEndFrame for the frames
        // Frame Generation submits (long where the runtime waits for its
        // queue). synthLockMs: the presenter thread's wait for the GPU
        // lock before writing. ringExhausted: refreshes whose real frame's
        // images newer captures had replaced (written unmoved, or the frame
        // before). staleSkipped: refreshes not submitted, nothing current to
        // show (a frame shown as it is had newer images in its swapchains).
        // leadAvg/leadMax: the lead told to the game's frames; leadHist: how
        // many frames were told each lead ("lead:frames"). toldAheadMs
        // (median/90th percentile): gameAheadMs measured from the presenter
        // thread's last paced wait however long ago, so every window has
        // it (gameAheadMs only from one within four refreshes). gameAheadMs
        // (median/95th percentile): how far past the runtime's own
        // prediction the game is told to render, Frame Generation's share
        // of its pose horizon (without Frame Generation 0, or a refresh for
        // a game that submits a period past its told time, as Star Citizen
        // does). aheadJitterMs: its change from one game frame to the next
        // (Dynamic varies it by up to a refresh). endSlackMs (10th
        // percentile/median): the time to spare when the game's frame
        // ended, negative when late. leadSlots marks " (cap)" at 4 refreshes
        // per real frame and " (ring cap)" at the history ring's lower
        // ceiling (FrameGenLeadPolicy::ring_ceiling). Just in time:
        // ringShort: moved refreshes whose real frame's previous images alone
        // newer captures had replaced / of those, not the lead's doing (the
        // presenter thread late, a frame told another lead or multiplier, a
        // short ring) / of those, found more than three quarters of a
        // refresh after the presenter thread's wait returned (its own
        // lateness). ringShortWriteMs (when any): that time, median/max.
        // ringCeiling: the ceiling at the current multiplier ("-": none,
        // "off": turned off). ringMoves: ceilings lowered / raised again.
        // ringRefused: second late events at a ceiling before its retry wait
        // ran out. ringUndone (when any): lowerings undone at once, frames
        // late at them in two hitch events while judged (a lone hitch leaves
        // them). ringHeld (when any): lowerings refused, the ring holding the
        // lead at any capture time or frames late a refresh below it (a
        // lowering from it undone). ringWould (turned off, when any):
        // lowerings it would have made, by its own rules (none while a probe
        // or step down of the policy's is judged, as with it on), so it
        // compares with ringMoves of a flight with it on.
        const std::uint32_t cap_multiplier = std::max(2u, EffectiveMultiplierLocked());
        const std::uint32_t cap4 = 4 * cap_multiplier;
        const std::uint32_t ring_cap = LeadCapLocked(cap_multiplier);
        const std::string ring_text =
            ", ringShort=" + std::to_string(delta(&Totals::ring_short)) + "/" +
            std::to_string(delta(&Totals::ring_short_ignored)) + "/" +
            std::to_string(delta(&Totals::ring_short_late)) +
            (ring_short_write_ms_.empty() ? std::string()
                                          : ", ringShortWriteMs=" + MedianAndMax(ring_short_write_ms_)) +
            ", ringCeiling=" +
            (!RingCeilingLocked() ? std::string("off")
             : ring_cap < cap4    ? std::to_string(ring_cap)
                                  : std::string("-")) +
            ", ringMoves=-" + std::to_string(delta(&Totals::lead_ring_downs)) + "/+" +
            std::to_string(delta(&Totals::lead_ring_raises)) +
            ", ringRefused=" + std::to_string(delta(&Totals::lead_ring_refused)) +
            (delta(&Totals::lead_ring_undone) ? ", ringUndone=" + std::to_string(delta(&Totals::lead_ring_undone))
                                              : std::string()) +
            (delta(&Totals::lead_ring_held) ? ", ringHeld=" + std::to_string(delta(&Totals::lead_ring_held))
                                            : std::string()) +
            (!RingCeilingLocked() && delta(&Totals::ring_short_would)
                 ? ", ringWould=" + std::to_string(delta(&Totals::ring_short_would))
                 : std::string());
        logger_->Info(
            "Frame Generation: multiplier=" + multiplier + ", threaded=1, appFps=" +
            Number(PerSecond(delta(&Totals::app_frames), seconds), 1) +
            ", outputFps=" + Number(PerSecond(delta(&Totals::submissions), seconds), 1) +
            ", distinctFps=" + Number(PerSecond(delta(&Totals::distinct), seconds), 1) +
            ", standbyS=" + Number(standby - summary_standby_seconds_, 1) +
            ", repeatedFrames=" + std::to_string(repeated_submissions_) + ", appLatencyMs=" + Number(app_latency_ms_, 1) +
            ", leadSlots=" + std::to_string(lead_slots_) +
            (lead_slots_ >= cap4 ? " (cap)" : lead_slots_ >= ring_cap && ring_cap < cap4 ? " (ring cap)" : "") +
            ", leadFloor=" + std::to_string(lead_floor_) +
            ", leadMoves=+" + std::to_string(delta(&Totals::lead_ups)) + "/-" +
            std::to_string(delta(&Totals::lead_downs)) + ", leadProbes=" + std::to_string(delta(&Totals::lead_probes)) +
            "/" + std::to_string(delta(&Totals::lead_probes_failed)) +
            ", leadUndone=" + std::to_string(delta(&Totals::lead_downs_undone)) + ", leadAvg=" +
            (told_lead_frames_
                 ? Number(static_cast<double>(told_lead_sum_) / static_cast<double>(told_lead_frames_), 2)
                 : std::string("-")) +
            ", leadMax=" + (told_lead_frames_ ? std::to_string(told_lead_max_) : std::string("-")) +
            ", leadHist=" + LeadCountsText(told_lead_counts_) +
            ", toldAheadMs=" + Percentiles(told_ahead_ms_, 0.5, 0.9) +
            ", gameAheadMs=" + Percentiles(game_ahead_ms_, 0.5, 0.95) +
            ", aheadJitterMs=" + Percentiles(ahead_jitter_ms_, 0.5, 0.95) +
            ", endSlackMs=" + Percentiles(end_slack_ms_, 0.1, 0.5) +
            ", appWorkMs=" + Number(app_work_frames_ ? app_work_ms_ / app_work_frames_ : 0.0, 1) +
            (settings_.multiplier == FrameGenMultiplier::kAuto && auto_capture_pending_count_
                 ? ", autoCapturePendingMs=" + MedianAndMax(std::vector<double>(
                       auto_capture_pending_ms_.begin(), auto_capture_pending_ms_.begin() +
                                                            std::min(auto_capture_pending_count_, auto_capture_pending_ms_.size())))
                 : std::string()) +
            ", lateReal=" + std::to_string(delta(&Totals::late_cpu)) + "/" + std::to_string(delta(&Totals::late_gpu)) +
            ", skippedGenerated=" + std::to_string(skipped_generated_) + ", generated=+" +
            std::to_string(delta(&Totals::generated_distinct)) + " (" + std::to_string(totals_.generated_distinct) +
            ")" + ", copiesSkipped=" + std::to_string(delta(&Totals::copies_skipped)) +
            ", repeated=" + std::to_string(repeated_) + ", failures=" + std::to_string(failures_) +
            ", captured=+" + std::to_string(delta(&Totals::captured)) + " (" + std::to_string(captured_) + ")" +
            ", gpuMs=" + Number(gpu_ms_, 2) + ", depthUsed=" +
            Number(depth_window_frames_ ? 100.0 * depth_window_used_ / depth_window_frames_ : 0.0, 0) +
            "%, eyeShiftMm=" + Number(shift_window_frames_ ? shift_window_sum_mm_ / shift_window_frames_ : 0.0, 1) +
            "/" + Number(shift_window_max_mm_, 1) +
            ", eyeMoveMm=" + Number(eye_frames_ ? eye_move_sum_mm_ / eye_frames_ : 0.0, 1) +
            ", eyeMissMm=" + Number(eye_frames_ ? eye_miss_sum_mm_ / eye_frames_ : 0.0, 1) +
            ", eyePredictMissMm=" + EyePredictMissText(eye_predict_sum_mm_, eye_predict_frames_, eye_prediction_.damping) +
            (eye_probe_ ? EyeProbeTextLocked() : std::string()) +
            ", gpuStartMs=" + MedianAndMax(gpu_start_ms_) + ", gpuDoneMs=" + MedianAndMax(gpu_done_ms_) +
            ", captureDoneMs=" + MedianAndMax(capture_done_ms_) + ", analysisMs=" + MedianAndMax(analysis_ms_) +
            ", measureLagMs=" + MedianAndMax(measure_lag_ms_) +
            ", ofaMs=" + MedianAndMax(ofa_ms_) + ", gpuWaits=" + std::to_string(delta(&Totals::gpu_waits)) + "/" +
            (gpu_wait_ms_.empty() ? std::string("-") : Number(*Median(gpu_wait_ms_), 1)) + "/" +
            std::to_string(delta(&Totals::gpu_wait_timeouts)) +
            (gpu_gate_enabled_ && gpu_gate_armed_ ? "" : gpu_gate_enabled_ ? " (not armed)" : " (off)") +
            ", endCallMs=" + MedianAndMax(end_call_ms_) + ", runtimeEndMs=" + MedianAndMax(runtime_end_ms_) +
            ", synthLockMs=" + MedianAndMax(synth_lock_ms_) +
            (jit_ ? ", justInTime=1, covers=" + std::to_string(jit_covers_) + ", unwritten=" +
                        std::to_string(delta(&Totals::unwritten_wait)) + "/" +
                        std::to_string(delta(&Totals::unwritten_failed)) + "/" +
                        std::to_string(delta(&Totals::unwritten_outputs)) + "/" +
                        std::to_string(delta(&Totals::unwritten_source)) +
                        ", unready=" + std::to_string(delta(&Totals::unready)) + "/" +
                        std::to_string(delta(&Totals::unmeasured)) +
                        ", staleMotion=" + std::to_string(delta(&Totals::stale_motion)) +
                        ", ring=" + std::to_string(JitRingLocked(quad_mode_)) +
                        ", ringExhausted=" + std::to_string(delta(&Totals::ring_exhausted)) + ring_text +
                        ", staleSkipped=" + std::to_string(delta(&Totals::stale_skipped))
                  : std::string()) +
            ", runtimeHalved=" + std::to_string(delta(&Totals::halving_halved)) + "/" +
            std::to_string(delta(&Totals::halving_measured)) + (runtime_halving_ ? " (halving)" : "") +
            ", pauses=" + std::to_string(delta(&Totals::pauses)) + "/" + Number(pause_longest_ms_, 0) +
            ", autoRetries=" + std::to_string(automatic_retries_) + "/" + std::to_string(retry_attempts_) +
            ClockSummaryLocked(start) + DynamicSummaryLocked() + QuadSummaryLocked(start) + ".");
        LogEngineSummaryLocked(seconds);
    }
    NewSummaryWindowLocked(now, standby);
}

void FrameGenPresenter::NewSummaryWindowLocked(Clock::time_point now, double standby) {
    // Both summaries share the window: samples one of them does not print
    // went into the next window the other printed (a stand-down's presenter
    // thread samples into the next engagement's summary, F56).
    last_summary_ = now;
    summary_totals_ = totals_;
    summary_standby_seconds_ = standby;
    cycles_ = 0;
    late_cycles_ = 0;
    eye_move_sum_mm_ = eye_miss_sum_mm_ = 0.0;
    eye_frames_ = 0;
    eye_predict_sum_mm_ = {};
    eye_predict_frames_ = 0;
    eye_probe_window_ = {};
    gpu_start_ms_.clear();
    gpu_done_ms_.clear();
    capture_done_ms_.clear();
    analysis_ms_.clear();
    measure_lag_ms_.clear();
    ofa_ms_.clear();
    gpu_wait_ms_.clear();
    end_call_ms_.clear();
    runtime_end_ms_.clear();
    synth_lock_ms_.clear();
    ring_short_write_ms_.clear();
    told_lead_sum_ = 0;
    told_lead_frames_ = 0;
    told_lead_max_ = 0;
    game_ahead_ms_.clear();
    told_ahead_ms_.clear();
    told_lead_counts_.clear();
    ahead_jitter_ms_.clear();
    end_slack_ms_.clear();
    pause_longest_ms_ = 0.0;
    app_work_ms_ = 0.0;
    app_work_frames_ = 0;
    repeated_submissions_ = 0;
    skipped_generated_ = 0;
    depth_window_frames_ = depth_window_used_ = 0;
    shift_window_sum_mm_ = shift_window_max_mm_ = 0.0;
    shift_window_frames_ = 0;
    jit_covers_ = 0;
    engine_window_ = {};
}

bool FrameGenPresenter::AnalyzeJit(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                                   FrameGenSynthesisMode mode, FrameCopy copy, const JitFrame& score,
                                   std::shared_ptr<const FrameGenQuadFrame> quad) {
    XrTime previous_time = 0;
    {
        std::scoped_lock lock(mutex_);
        // Nothing is measured or moved across a pause: the frame after it is
        // shown as it is, and motion is measured again from the next.
        if (last_real_.valid && frame.app_time - last_real_.display_time > PauseLimitLocked()) {
            NotePauseLocked(frame.app_time - last_real_.display_time);
            InvalidateContinuityLocked();
        }
        if (last_real_.valid && last_real_.sequence + 1 == frame.sequence && frame.app_time > last_real_.display_time) {
            previous_time = last_real_.display_time;
        }
    }
    Analysis analysis;
    std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
    std::vector<std::uint8_t> exact;
    const bool ok =
        Generate(projection, frame, mode, {}, sets, exact, true, &analysis, quad ? &quad->composition : nullptr);
    // A frame that did not reach the GPU history is not the previous frame
    // of the next one (see RunCycle).
    if (ok) RecordReal(projection, frame.app_time, frame.sequence);
    std::scoped_lock lock(mutex_);
    if (!ok || !compositor_running_ || !copy.valid || !copy.has_projection) {
        // Not kept: the frame is shown unchanged in its own refresh and
        // nothing is generated from it (frames are found by id).
        InvalidateContinuityLocked();
        if (!score.scored) {
            UpdateLeadLocked(false, score.slack_ms, score.lead, score.multiplier);
        }
        return false;
    }
    // Frames of another view size (or output kind) are never written again:
    // their output images would be made anew each time one was chosen. So
    // are frames composed otherwise, or into images of another size. Frames
    // shown as the layer composed them write nothing.
    const auto& views = copy.layers[copy.projection_index].views;
    auto differs = [&](const JitFrame& kept) {
        if (kept.as_is) {
            return false;
        }
        if ((kept.quad != nullptr) != (quad != nullptr) ||
            (quad && (kept.quad->composition.output.width != quad->composition.output.width ||
                      kept.quad->composition.output.height != quad->composition.output.height))) {
            return true;
        }
        const auto& kept_views = kept.copy.layers[kept.copy.projection_index].views;
        if (kept_views.size() != views.size() || kept.kinds != analysis.kinds) {
            return true;
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            if (kept_views[i].subImage.imageRect.extent.width != views[i].subImage.imageRect.extent.width ||
                kept_views[i].subImage.imageRect.extent.height != views[i].subImage.imageRect.extent.height) {
                return true;
            }
        }
        return false;
    };
    for (auto it = jit_frames_.begin(); it != jit_frames_.end();) {
        if (differs(*it)) {
            ScoreErasedLocked(*it);
            it = jit_frames_.erase(it);
        } else {
            ++it;
        }
    }
    JitFrame jit = score;
    jit.id = frame.sequence;
    jit.analyzed = analysis.frame;
    jit.motion_not_before = analysis.motion_not_before;
    jit.time = frame.app_time;
    jit.previous_time = analysis.synthesized ? previous_time : 0;
    if (jit.previous_time && !jit_frames_.empty() && !jit_frames_.back().as_is &&
        jit_frames_.back().time == jit.previous_time && jit_frames_.back().analyzed) {
        jit.previous_analyzed = jit_frames_.back().analyzed;
        jit.previous_motion_not_before = jit_frames_.back().motion_not_before;
    }
    RecordDepthReleasesLocked(copy);
    jit.copy = std::move(copy);
    jit.kinds = std::move(analysis.kinds);
    jit.velocity = std::move(analysis.velocity);
    jit.depth = analysis.depth;
    // Stage 7: its cells labelled, the engine motion map moves the head's and
    // the world's content past one interval of it.
    jit.reach = analysis.merged && engine_map_ != FrameGenEngineMap::kOff ? engine_reach_ : 1.0f;
    jit.quad = std::move(quad);
    if (trace_.Enabled()) {
        trace_.Record("keep", jit.id, jit.time, jit.previous_time, static_cast<std::int64_t>(jit.analyzed),
                      static_cast<std::int64_t>(jit.previous_analyzed), 0, static_cast<std::int64_t>(jit.capture_mark));
        if (jit.copy.has_projection && jit.copy.projection_index < jit.copy.layers.size() &&
            !jit.copy.layers[jit.copy.projection_index].views.empty()) {
            trace_.RecordPose("keep.v0", jit.id, jit.copy.layers[jit.copy.projection_index].views[0].pose);
        }
    }
    jit_frames_.push_back(std::move(jit));
    // The backend keeps the images of the last few real frames: as many as
    // its ring for this analysis has (quad views keep more).
    while (jit_frames_.size() > JitRingLocked(jit_frames_.back().quad != nullptr)) {
        ScoreErasedLocked(jit_frames_.front());
        jit_frames_.pop_front();
    }
    return true;
}

XrResult FrameGenPresenter::ThreadedEndJit(const XrFrameEndInfo& info, const ThreadedFrame& frame,
                                           Clock::time_point entered, const FrameGenQuadFrame* quad) {
    // Claimed in the layer's xrEndFrame (ClaimQuadFrame): its primary
    // projection holds the four source views.
    const bool claimed = frame.quad && quad && quad->route == FrameGenQuadRoute::kClaimed;
    const XrCompositionLayerProjection* projection = FindProjection(info);
    const bool renderable = info.layerCount > 0 && projection != nullptr;
    FrameCopy copy;
    std::string unsupported;
    const bool copied = CopyFrame(info, copy, unsupported);
    copy.quad = claimed; // shown only through the views composed from it
    std::unique_lock lock(mutex_);
    end_thread_ = std::this_thread::get_id();
    NoteDisplayOffsetLocked(info.displayTime, frame.predicted, frame.reported_period);
    const double latency = std::chrono::duration<double, std::milli>(entered - frame.wait_return).count();
    app_latency_ms_ = app_latency_ms_ <= 0.0 ? latency : app_latency_ms_ * 0.9 + latency * 0.1;
    app_work_ms_ += latency;
    ++app_work_frames_;
    if (!renderable) {
        ++totals_.non_renderable;
    } else if (!QuadTargetsLocked(quad)) {
        // Quad views Frame Generation could compose keep their four views
        // as targets (ClaimQuadFrame).
        RecordCaptureTargetsLocked(*projection);
    }
    if (!copied && !unsupported_composition_ && !threaded_disabled_) {
        // The next wait returns to inline presentation (refused for
        // composition metadata, Frame Generation is unavailable instead).
        threaded_disabled_ = true;
        if (logger_) {
            logger_->Info("Frame Generation presents inline: the game submits " + unsupported +
                          ", which the presenter thread cannot repeat between real frames.");
        }
    }
    const XrDuration period = GameGridLocked();
    // When it is needed: extrapolating, in its own refresh; interpolating,
    // from the refresh after the newest real frame's (its own, the first),
    // for the frames between. Measured, then written on the GPU, by when
    // the runtime releases that refresh's wait.
    const XrTime needed = !frame.interpolate || jit_frames_.empty()
                              ? frame.target_time
                              : std::min(frame.target_time, jit_frames_.back().time + period);
    // Interpolating with a refresh between it and the newest real frame,
    // that refresh shows it moved: its motion is needed then, not only its
    // capture.
    const bool moved_first = needed < frame.target_time - period / 2;
    // A refresh already past (before the runtime's last paced wait) maps
    // back on the same clock: the frame is that much late.
    std::optional<double> slack_ms;
    Clock::time_point needed_due{};
    if (phase_wall_ != Clock::time_point{}) {
        needed_due = phase_wall_ + std::chrono::nanoseconds(needed - phase_xr_);
        slack_ms = std::chrono::duration<double, std::milli>(needed_due - std::chrono::milliseconds(2) - entered).count();
    }
    if (renderable && slack_ms) {
        AddSample(end_slack_ms_, *slack_ms, kMaxFrameSamples);
    }
    // Ending after its refresh was due, it is late at once. Otherwise it is
    // judged when a refresh first chooses it: its game frame must have
    // reached the GPU by then too (CompositorLoop). One with no projection (a
    // loading screen or a menu) moves the lead neither way: nothing is
    // generated from it, and a game's late loading frames raised it to its
    // cap (Star Citizen's 93 s loading screen, 2026-10-14: 2 -> 8, then a
    // pose horizon of 152 ms for the first 12 s of play).
    const bool late = slack_ms && *slack_ms < 0.0;
    if (late) {
        ++totals_.late_cpu;
        if (renderable) {
            UpdateLeadLocked(true, slack_ms, frame.lead, frame.multiplier);
        }
    }
    JitFrame score;
    score.lead = frame.lead;
    score.multiplier = frame.multiplier;
    score.needed = needed;
    // Moved at its first refresh, it could have come only as much later as
    // its analysis leaves (its capture is judged when that refresh nears).
    score.slack_ms = slack_ms && moved_first ? std::optional<double>(*slack_ms - analysis_lag_ms_) : slack_ms;
    score.capture_mark = last_capture_mark_; // its views were released before its end
    score.scored = late;
    // A pause is not the game's pace.
    if (!jit_frames_.empty() && frame.target_time > jit_frames_.back().time &&
        frame.target_time - jit_frames_.back().time <= PauseLimitLocked()) {
        const double interval =
            static_cast<double>(frame.target_time - jit_frames_.back().time) / static_cast<double>(period);
        dynamic_interval_slots_ =
            dynamic_interval_slots_ > 0.0 ? dynamic_interval_slots_ * 0.9 + interval * 0.1 : interval;
    }
    // The next wait may go ahead; the presenter thread waits for this
    // measurement when a refresh needs it first.
    threaded_frames_.erase(std::remove_if(threaded_frames_.begin(), threaded_frames_.end(),
                                          [&](const ThreadedFrame& pending) { return pending.id == frame.id; }),
                           threaded_frames_.end());
    // Composing quad views, only a claimed frame is measured. Any other
    // (the layer composed it) is shown as it is in its own refresh, after
    // the frames before it, and nothing is generated across it.
    const bool usable = renderable && copied && compositor_running_;
    const bool measure = usable && (claimed || !quad_mode_);
    const bool as_is = usable && !measure;
    // Shown as it is (the fallback when nothing is written), only while its
    // images are still the newest in their swapchains.
    RecordColorReleasesLocked(copy);
    if (measure) {
        // Its four views never reach the runtime as they are.
        if (!claimed) {
            newest_copy_ = copy;
            newest_copy_fresh_ = true;
            newest_copy_id_ = frame.id;
        }
        ++healthy_streak_;
    } else if (as_is) {
        newest_copy_ = copy;
        newest_copy_fresh_ = true;
        newest_copy_id_ = frame.id;
        ++healthy_streak_;
        JitFrame kept = score;
        kept.id = frame.id;
        kept.time = frame.target_time;
        kept.as_is = true;
        kept.scored = true; // scored below: nothing of it waits for the GPU
        RecordDepthReleasesLocked(copy);
        kept.copy = copy;
        trace_.Record("keep", kept.id, kept.time, 0, 0, 0, 1);
        jit_frames_.push_back(std::move(kept));
        while (jit_frames_.size() > JitRingLocked(quad_mode_)) {
            ScoreErasedLocked(jit_frames_.front());
            jit_frames_.pop_front();
        }
        InvalidateContinuityLocked();
    } else if (!renderable) {
        // No projection (a loading screen's quads, or nothing at all): the
        // presenter thread shows its layers as they are until the next frame,
        // and nothing is generated across the gap.
        newest_copy_ = copied ? copy : FrameCopy{};
        newest_copy_fresh_ = true;
        newest_copy_id_ = frame.id;
        for (JitFrame& kept : jit_frames_) {
            ScoreErasedLocked(kept);
        }
        jit_frames_.clear();
        jit_content_ = {};
        jit_views_.clear();
        jit_shown_.reset();
        jit_shown_as_is_ = false;
        InvalidateContinuityLocked();
    }
    if (renderable && !measure && !late) {
        // Not kept: on time.
        UpdateLeadLocked(false, slack_ms, frame.lead, frame.multiplier);
    }
    generation_pending_ = measure;
    WakeSlots();
    lock.unlock();
    if (measure) {
        PendingFrame pending;
        pending.app_time = frame.target_time;
        pending.end_time = info.displayTime;
        pending.sequence = frame.id;
        pending.period = period;
        // Its exact copy needs its capture by its refresh; its motion is
        // needed only a refresh later, by the first frame moved on from it,
        // unless interpolating with a refresh between it and the newest real
        // frame, which shows it moved.
        pending.real_due = needed_due;
        pending.generated_due = needed_due != Clock::time_point{} && !moved_first
                                    ? needed_due + std::chrono::nanoseconds(period)
                                    : needed_due;
        // A claimed frame keeps its composition and eye views, made for its
        // own display time, for every frame written from it.
        std::shared_ptr<FrameGenQuadFrame> kept_quad;
        if (claimed) {
            kept_quad = std::make_shared<FrameGenQuadFrame>(*quad);
            for (XrCompositionLayerProjectionView& view : kept_quad->eyes) view.next = nullptr;
            for (XrCompositionLayerProjectionView& view : kept_quad->source_views) view.next = nullptr;
        }
        const bool kept =
            AnalyzeJit(*projection, pending,
                       frame.interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate,
                       std::move(copy), score, std::move(kept_quad));
        if (claimed && !kept) {
            lock.lock();
            ++totals_.quad_unanalyzed;
            if (logger_ && !logged_quad_unanalyzed_) {
                logged_quad_unanalyzed_ = true;
                logger_->Error("Frame Generation could not measure the motion of a quad views frame it composes; "
                               "the frame was not shown.");
            }
            lock.unlock();
        }
    } else if (as_is && quad && quad->route == FrameGenQuadRoute::kUncaptured) {
        // Its views' releases were not captured (their first frame, or a new
        // region): they are set up from it, so the next ones are (the
        // backend's Execute stops there, uncaptured). Set up as composed, in
        // the ring of the claimed frames that follow (FrameGenHistoryRing).
        XrCompositionLayerProjection source{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        source.space = quad->source_space;
        source.viewCount = static_cast<std::uint32_t>(quad->source_views.size());
        source.views = quad->source_views.data();
        PendingFrame pending;
        pending.app_time = frame.target_time;
        pending.end_time = info.displayTime;
        pending.sequence = frame.id;
        pending.period = period;
        Analysis analysis;
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        std::vector<std::uint8_t> exact;
        Generate(source, pending,
                 frame.interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate, {},
                 sets, exact, true, &analysis, &quad->composition);
        lock.lock();
        ++totals_.quad_setups;
        InvalidateContinuityLocked();
        lock.unlock();
    } else if (as_is) {
        // Nothing analyses it, but its views were captured: analyses' GPU
        // timings are read every few captures (the backend keeps a few).
        std::vector<FrameGenAnalysisTiming> timings;
        {
            std::unique_lock<std::mutex> queue_lock;
            if (queue_mutex_) queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
            std::scoped_lock gpu_lock(gpu_mutex_);
            if (gpu_) timings = gpu_->TakeTimings();
        }
        if (!timings.empty()) {
            lock.lock();
            NoteGpuTimingLocked(0, PendingFrame{}, std::nullopt, timings);
            lock.unlock();
        }
    }
    lock.lock();
    generation_pending_ = false;
    if (unsupported_composition_) {
        // A chain first seen after a quad claim cannot be applied to a raw
        // four-view frame in a stereo runtime. Keep its last image for this
        // refresh; the following frames use the layer's normal composition.
        jit_frames_.clear();
        compositor_drain_ = true;
        drain_started_slot_ = slot_index_;
    }
    WakeSlots();
    app_frames_.push_back(entered);
    ++totals_.app_frames;
    NoteRealEndLocked(entered);
    PendingFrame cadence;
    cadence.engaged = true;
    cadence.multiplier = frame.multiplier;
    cadence.period = period;
    UpdateAutoLocked(entered, cadence);
    SummaryLocked(entered);
    return XR_SUCCESS;
}

FrameGenPresenter::JitChoice FrameGenPresenter::ChooseJitLocked(XrTime time, XrDuration period, bool interpolate,
                                                                bool& due, std::size_t limit) const {
    JitChoice choice;
    due = false;
    const std::size_t count = std::min(limit, jit_frames_.size());
    const XrDuration half = period / 2;
    // A real frame on its way that this refresh needs: interpolating, any
    // (the frames past the newest are made from the next); extrapolating,
    // one rendered for this refresh or an earlier one.
    // Not one an app that abandons such frames superseded: its end never comes.
    const bool needed_in_flight =
        generation_pending_ || std::any_of(threaded_frames_.begin(), threaded_frames_.end(), [&](const ThreadedFrame& frame) {
            return !(frame.superseded && abandons_frames_) && (interpolate || frame.target_time <= time + half);
        });
    if (count == 0) {
        due = needed_in_flight;
        return choice;
    }
    // The newest real frame whose own refresh is this one or earlier.
    std::size_t index = count;
    for (std::size_t i = count; i-- > 0;) {
        if (jit_frames_[i].time <= time + half) {
            index = i;
            break;
        }
    }
    choice.mode = FrameGenSynthesisMode::kInterpolate;
    choice.fraction = 1.0f;
    if (index == count) {
        // Before every kept real frame (the presenter thread just started):
        // the oldest, unchanged.
        choice.index = 0;
        return choice;
    }
    const JitFrame& frame = jit_frames_[index];
    if (time <= frame.time + half) {
        choice.index = index; // its own refresh: its exact copy
        return choice;
    }
    const bool newest = index + 1 == count;
    if (interpolate && !newest) {
        const JitFrame& next = jit_frames_[index + 1];
        if (next.previous_time == frame.time) {
            // Between it and the next: from the next one's measurement.
            choice.index = index + 1;
            choice.fraction = FrameGenFraction(FrameGenSynthesisMode::kInterpolate, frame.time, next.time, time);
        } else {
            // Not continuous: the nearer of the two, unchanged.
            choice.index = time - frame.time < next.time - time ? index : index + 1;
        }
        return choice;
    }
    // Past it, moved on by its motion: extrapolating, until the next real
    // frame's refresh; interpolating, the next real frame is late (a cover
    // frame). For at most a few intervals, and never past a pause: a game
    // that stopped sending frames shows its last one. Content moves one
    // interval at most, or with the engine motion map its reach (stage 7).
    choice.index = index;
    choice.mode = FrameGenSynthesisMode::kExtrapolate;
    choice.fraction = 0.0f;
    if (frame.previous_time && time > frame.time &&
        time - frame.time <= std::min<XrDuration>(4 * (frame.time - frame.previous_time), PauseLimitLocked())) {
        choice.fraction =
            FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, frame.previous_time, frame.time, time, frame.reach);
    }
    if (newest) {
        due = needed_in_flight;
        choice.cover = due && choice.fraction > 0.0f;
    }
    return choice;
}

bool FrameGenPresenter::JitCopyOnly(const JitFrame& frame, const JitChoice& choice) {
    return frame.previous_time == 0 ||
           (choice.mode == FrameGenSynthesisMode::kExtrapolate ? choice.fraction <= 0.0f : choice.fraction >= 1.0f);
}

bool FrameGenPresenter::SynthesizeJit(std::optional<JitFrame>& source, JitChoice& choice,
                                      const std::optional<JitFrame>& fallback, const JitChoice& fallback_choice,
                                      std::vector<XrCompositionLayerProjectionView>& views, JitWrite& write) {
    write = {};
    bool unmeasured = false; // a moved frame written as its real frame's exact copy
    double lock_wait_ms = 0.0;
    // A cover frame shows a real frame past its time while the next is late:
    // its images replaced says nothing of the lead (captured before
    // exact_copy clears it).
    const bool cover = choice.cover;
    bool primary_source = true; // the refresh's own choice is written, not the fallback
    // The ring was a capture short for the frame this refresh chose
    // (JitWrite::ring_short): its previous images replaced, its newest kept.
    const auto note_ring_short = [&](const JitFrame& frame) {
        write.ring_short = true;
        write.ring_short_id = frame.id;
        write.ring_short_lead = frame.lead;
        write.ring_short_multiplier = frame.multiplier;
        write.ring_short_composed = frame.quad != nullptr;
        write.ring_short_at = Clock::now();
    };
    const auto exact_copy = [](JitChoice& unmoved) {
        unmoved.mode = FrameGenSynthesisMode::kInterpolate;
        unmoved.fraction = 1.0f;
        unmoved.cover = false;
    };
    // A frame the layer composed is shown as it is: nothing to write.
    if (source->as_is) {
        write.as_is = true;
        return false;
    }
    {
        // Lock order: GPU, presenter.
        const auto asked = Clock::now();
        std::scoped_lock gpu_lock(gpu_mutex_);
        lock_wait_ms += Milliseconds(Clock::now() - asked);
        if (!gpu_) {
            return false;
        }
        // Interpolating, a frame between two real frames whose newer one's
        // motion is not measured yet gives way to the fallback (the older one
        // moved on past itself, measured already) rather than showing the
        // newer one unchanged before its time, which jumps ahead and then
        // holds (DCS and MSFS 2024, 2026-10-09: 70-140 such refreshes in 30 s).
        const bool cover_when_unmeasured = stale_motion_enabled_ && fallback && fallback->id != source->id &&
                                           !fallback->as_is &&
                                           choice.mode == FrameGenSynthesisMode::kInterpolate &&
                                           fallback_choice.mode == FrameGenSynthesisMode::kExtrapolate;
        bool motion_late = false;
        auto ready = [&](const JitFrame& frame, JitChoice& choice, bool primary, bool& not_captured, bool& stale,
                         bool cover_instead = false) {
            stale = false;
            if (frame.as_is) {
                return true; // shown as the layer composed it
            }
            bool moved = !JitCopyOnly(frame, choice);
            // Newer captures took the ring slot of its previous images: it
            // can only be shown unchanged; of its own images too: not at all
            // (writing would fail and repeat the last frame anyway).
            if (moved && !gpu_->Writable(frame.analyzed, false)) {
                if (primary && !cover && gpu_->Writable(frame.analyzed, true)) {
                    note_ring_short(frame);
                }
                write.ring_exhausted = true;
                moved = false;
                exact_copy(choice);
            }
            if (!moved && !gpu_->Writable(frame.analyzed, true)) {
                write.ring_exhausted = true;
                return false;
            }
            if (moved && Clock::now() >= frame.motion_not_before && gpu_->Measured(frame.analyzed)) {
                return true;
            }
            if (!gpu_->Captured(frame.analyzed)) {
                not_captured = true;
                return false;
            }
            // Its motion is still being measured. Extrapolating, the previous
            // real frame's motion moves it instead, when that was measured
            // and is still kept: the refresh does not stand still and jump
            // the next (MSFS 2024 with NVIDIA's flow, 2026-10-09: motion came
            // 16 ms after the capture, and 8% of refreshes stood still).
            if (moved && stale_motion_enabled_ && choice.mode == FrameGenSynthesisMode::kExtrapolate &&
                frame.previous_analyzed && Clock::now() >= frame.previous_motion_not_before &&
                gpu_->Measured(frame.previous_analyzed) &&
                gpu_->Writable(frame.previous_analyzed, true)) {
                stale = true;
                return true;
            }
            if (moved && cover_instead) {
                motion_late = true;
                return false;
            }
            // Else its real frame unchanged.
            unmeasured = moved;
            exact_copy(choice);
            return true;
        };
        bool stale_source = false;
        if (!ready(*source, choice, true, write.uncaptured, stale_source, cover_when_unmeasured)) {
            // The refresh's own choice is not written (its game frame not
            // captured yet, say): its ring being short is not the lead's.
            write.ring_short = false;
            JitChoice alternative = fallback_choice;
            bool fallback_uncaptured = false;
            bool stale_fallback = false;
            if (!fallback || !ready(*fallback, alternative, false, fallback_uncaptured, stale_fallback)) {
                std::scoped_lock lock(mutex_);
                ++repeated_;
                write.unmeasured = unmeasured;
                write.motion_late = motion_late;
                return false;
            }
            source = fallback;
            choice = alternative;
            stale_source = stale_fallback;
            primary_source = false;
            if (source->as_is) {
                write.as_is = true;
                write.unmeasured = unmeasured;
                return false;
            }
        }
        write.unmeasured = unmeasured;
        write.stale = stale_source;
        write.motion_late = motion_late;
        // Moved by the previous real frame's motion, a frame has no engine
        // motion map of its own: one interval at most (stage 7).
        if (write.stale) {
            choice.fraction = std::min(choice.fraction, 1.0f);
        }
    }
    const JitFrame& frame = *source;
    write.exact = JitCopyOnly(frame, choice);
    if (!frame.copy.has_projection || frame.copy.projection_index >= frame.copy.layers.size()) {
        return false;
    }
    const FrameCopy::Layer& layer = frame.copy.layers[frame.copy.projection_index];
    if (layer.views.empty() || frame.kinds.size() != layer.views.size()) {
        return false;
    }
    // One output image per view; composing quad views, one per eye, in the
    // focus view's kind (the layer's compositor writes its format).
    const FrameGenQuadFrame* quad = frame.quad.get();
    std::vector<OutputSpec> specs;
    if (quad) {
        const FrameGenComposition& composition = quad->composition;
        if (composition.eyes.size() != quad->eyes.size()) {
            return false;
        }
        for (const FrameGenComposeEye& eye : composition.eyes) {
            if (eye.peripheral >= frame.kinds.size() || eye.focus >= frame.kinds.size()) {
                return false;
            }
            specs.push_back({composition.output, frame.kinds[eye.focus]});
        }
    } else {
        for (std::size_t i = 0; i < layer.views.size(); ++i) {
            specs.push_back({Extent(layer.views[i]), frame.kinds[i]});
        }
    }
    std::string error;
    // One set of output images, made on this thread (no other uses them).
    // Composing quad views, images made anew keep the old ones until a frame
    // is written into them: the last frame written still shows them.
    const bool ensured = EnsureOutputs(specs, 1, error, quad != nullptr);
    {
        std::scoped_lock lock(mutex_);
        const bool current = ensured && !output_sets_.empty() && jit_views_.size() == output_sets_.front().size() &&
                             std::equal(jit_views_.begin(), jit_views_.end(), output_sets_.front().begin(),
                                        [](const XrCompositionLayerProjectionView& view, const Output& output) {
                                            return view.subImage.swapchain == output.swapchain;
                                        });
        if (!current && !quad) {
            jit_views_.clear();
            jit_content_ = {};
        }
        if (!ensured) {
            ++totals_.unwritten_outputs;
            write.failed = true;
            write.error = error;
            // Composing quad views, failures hand the views back to the
            // layer for a while instead (CompositorLoop).
            if (!quad) NoteFailureLocked(error);
            return false;
        }
    }
    std::vector<FrameGenImage> images;
    for (Output& output : output_sets_.front()) {
        if (!output.acquired) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrResult acquired;
            {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrAcquireSwapchainImage");
                acquired = xr_.acquire(output.swapchain, &acquire, &output.index);
            }
            if (XR_FAILED(acquired)) {
                std::scoped_lock lock(mutex_);
                ++totals_.unwritten_failed;
                write.failed = true;
                write.error = "output image acquire failed (" + std::to_string(acquired) + ")";
                if (!quad) NoteFailureLocked(write.error);
                return false;
            }
            output.acquired = true;
            output.waited = false;
        }
        if (!output.waited) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = kOutputWaitTimeout;
            const XrResult waited = [&] {
                const PresenterCall call(*this, "xrWaitSwapchainImage");
                return xr_.wait(output.swapchain, &wait);
            }();
            if (XR_FAILED(waited) || waited == XR_TIMEOUT_EXPIRED) {
                // Kept acquired: waited for again next refresh.
                std::scoped_lock lock(mutex_);
                ++totals_.unwritten_wait;
                ++repeated_;
                write.failed = true;
                write.timed_out = waited == XR_TIMEOUT_EXPIRED;
                write.error = write.timed_out ? std::string("an output image was not handed back in time")
                                              : "output image wait failed (" + std::to_string(waited) + ")";
                return false;
            }
            output.waited = true;
        }
        images.push_back({output.swapchain, output.index, 0,
                          {{0, 0}, {static_cast<std::int32_t>(output.extent.width), static_cast<std::int32_t>(output.extent.height)}},
                          output.format});
    }
    bool ok = false;
    {
        const auto asked = Clock::now();
        std::scoped_lock gpu_lock(gpu_mutex_);
        lock_wait_ms += Milliseconds(Clock::now() - asked);
        ok = gpu_ != nullptr;
        // A capture since its readiness above may have taken a ring slot it
        // needs (the game releases its next frame meanwhile): unmoved, or
        // not at all (the images stay acquired for the next refresh).
        if (ok && !write.exact && !gpu_->Writable(frame.analyzed, false)) {
            if (primary_source && !cover && gpu_->Writable(frame.analyzed, true)) {
                note_ring_short(frame);
            }
            write.ring_exhausted = true;
            exact_copy(choice);
            write.exact = true;
        }
        if (ok && !gpu_->Writable(frame.analyzed, true)) {
            write.ring_exhausted = true;
            std::scoped_lock lock(mutex_);
            ++repeated_;
            return false;
        }
        for (const FrameGenImage& image : images) {
            ok = ok && gpu_->Track(image.swapchain, xr_.images, error);
        }
        if (ok && write.stale && !write.exact) {
            std::string stale_error;
            const bool stale_written =
                quad ? gpu_->SynthesizeComposedStale(frame.analyzed, frame.previous_analyzed, choice.fraction,
                                                     quad->composition, images, stale_error)
                     : gpu_->SynthesizeStale(frame.analyzed, frame.previous_analyzed, choice.fraction, images, stale_error);
            if (!stale_written) {
                // Refused (nothing written): its real frame unchanged after all.
                write.stale = false;
                write.unmeasured = unmeasured = true;
                exact_copy(choice);
                write.exact = true;
                ok = quad ? gpu_->SynthesizeComposed(frame.analyzed, choice.mode, choice.fraction, quad->composition,
                                                     images, error)
                          : gpu_->Synthesize(frame.analyzed, choice.mode, choice.fraction, images, error);
            }
        } else {
            write.stale = false;
            ok = ok && (quad ? gpu_->SynthesizeComposed(frame.analyzed, choice.mode, choice.fraction,
                                                        quad->composition, images, error)
                             : gpu_->Synthesize(frame.analyzed, choice.mode, choice.fraction, images, error));
        }
    }
    const bool extrapolate = choice.mode == FrameGenSynthesisMode::kExtrapolate;
    if (!ok) {
        // The images stay acquired and waited for the next refresh; the
        // runtime keeps the last ones released.
        std::scoped_lock lock(mutex_);
        AddSample(synth_lock_ms_, lock_wait_ms);
        ++totals_.unwritten_failed;
        ++repeated_;
        write.failed = true;
        write.error = error.empty() ? std::string("the GPU could not write the frame") : error;
        return false;
    }
    for (Output& output : output_sets_.front()) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        {
            std::scoped_lock submit(submit_mutex_);
            const PresenterCall call(*this, "xrReleaseSwapchainImage");
            xr_.release(output.swapchain, &release);
        }
        output.acquired = false;
        output.waited = false;
    }
    // The real frame's views, showing the images just written, seen from
    // where the eye is at the frame's time (where depth is used). Composed,
    // each eye's view as the layer composes it, moved as its periphery.
    // Frames moved by the previous real frame's motion too (their eye shift
    // reads its depth: FrameGenGpu::SynthesizeStale).
    const float eye_time = write.exact ? 0.0f : extrapolate ? choice.fraction : choice.fraction - 1.0f;
    double moved_mm = 0.0;
    if (quad) {
        views.assign(quad->eyes.begin(), quad->eyes.end());
    } else {
        views = layer.views;
    }
    for (std::size_t i = 0; i < views.size(); ++i) {
        const Output& output = output_sets_.front()[i];
        views[i].next = nullptr;
        views[i].subImage.swapchain = output.swapchain;
        views[i].subImage.imageArrayIndex = 0;
        views[i].subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(output.extent.width),
                                                static_cast<std::int32_t>(output.extent.height)}};
        const std::size_t moved_as = quad ? quad->composition.eyes[i].peripheral : i;
        if (eye_time != 0.0f && moved_as < frame.velocity.size()) {
            const std::array<float, 3>& velocity = frame.velocity[moved_as];
            const std::array<double, 3> local{eye_time * velocity[0], eye_time * velocity[1], eye_time * velocity[2]};
            const std::array<double, 3> world = Rotate(views[i].pose.orientation, local);
            views[i].pose.position.x += static_cast<float>(world[0]);
            views[i].pose.position.y += static_cast<float>(world[1]);
            views[i].pose.position.z += static_cast<float>(world[2]);
            moved_mm = std::max(moved_mm, 1000.0 * std::sqrt(local[0] * local[0] + local[1] * local[1] +
                                                             local[2] * local[2]));
        }
    }
    // Output images replaced since the last frame written: nothing shows
    // them once this frame is submitted.
    if (quad && !retired_outputs_.empty()) {
        DestroyRetiredOutputs();
    }
    std::scoped_lock lock(mutex_);
    consecutive_failures_ = 0;
    AddSample(synth_lock_ms_, lock_wait_ms);
    // How far a moved frame of a real frame generated with depth followed
    // the eyes (eyeShiftMm), as Generate counts the frames it writes.
    if (frame.depth && eye_time != 0.0f) {
        shift_window_sum_mm_ += moved_mm;
        shift_window_max_mm_ = std::max(shift_window_max_mm_, moved_mm);
        ++shift_window_frames_;
    }
    totals_.unmeasured += unmeasured ? 1 : 0;
    // Pair this denominator with stale_motion at the same successful write
    // event. Stale safety copies count too; ordinary real-frame copies do
    // not. A repeated or subsequently discarded write must contribute to
    // both populations, unlike generated_distinct at runtime submission.
    totals_.motion_outputs += !write.exact || write.stale || write.motion_late ? 1 : 0;
    totals_.stale_motion += write.stale || write.motion_late ? 1 : 0;
    // Stage 7: frames the engine motion map moved past one interval.
    engine_window_.reached += extrapolate && !write.exact && choice.fraction > 1.0f ? 1 : 0;
    if (logger_ && !logged_jit_ && !write.exact) {
        logged_jit_ = true;
        logger_->Info("Frame Generation writes each generated frame right before its refresh (" +
                      std::string(jit_interpolate_ ? "interpolating, with cover frames past the newest real frame when "
                                                     "the next one is late"
                                                   : "extrapolating, real frames shown as exact copies") +
                      (quad ? "): one output image per eye, composed from the four quad views, any number of frames "
                              "between real frames."
                            : "): one output image per eye, any number of frames between real frames."));
    }
    return true;
}

void FrameGenPresenter::CompositorLoop() {
    // A compositor-like thread: it must make every refresh even while the
    // game saturates the CPU.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"VectorXR Frame Generation presenter");
    compositor_thread_id_ = GetCurrentThreadId();
    std::unique_lock lock(mutex_);
    const FrameGenRuntimeCalls calls = compositor_calls_;
    const XrSession session = session_;
    void* const timer = compositor_timer_;
    std::vector<const XrCompositionLayerBaseHeader*> pointers;
    const auto ns = [](XrDuration duration) { return std::chrono::nanoseconds(duration); };
    const auto forever = Clock::time_point::max();
    // Draining, every frame in flight is presented, and just in time every
    // real frame kept until its own refresh (the frame thread waits for it):
    // the newest is shown before frames go straight to the runtime again. Not
    // past kDrainRefreshes refreshes, nor while the runtime's display times
    // do not advance with the time: refreshes that never reach the newest
    // frame's would hold the game for the whole drain.
    auto drained = [&] {
        const bool reached = jit_frames_.empty() ||
                             jit_frames_.back().time <= slot_time_ + SubmitSpacingLocked(slot_period_) / 2 ||
                             clock_displaced_ || slot_index_ >= drain_started_slot_ + kDrainRefreshes;
        return threaded_frames_.empty() && (!jit_ || (!generation_pending_ && reached));
    };
    while (!compositor_stop_ && !(compositor_drain_ && drained())) {
        // A runtime frame the application began before the switch owns the
        // runtime until it ends: no runtime wait while any frame is open (a
        // frame of this thread's left open, nothing current to show, is
        // discarded by its next begin).
        compositor_phase_ = "waiting for the game's open runtime frame";
        if (!CompositorWait(lock, Clock::now() + std::chrono::seconds(1), [&] {
                return compositor_stop_ ||
                       ((!RuntimeFrameOpenLocked() || presenter_frame_open_) && !RuntimeWaitPendingLocked());
            })) {
            if (RuntimeWaitPendingLocked()) {
                continue; // a waited frame was never begun: another wait would block in the runtime
            }
            if (logger_ && !logged_order_guard_) {
                logged_order_guard_ = true;
                logger_->Error("Frame Generation: a runtime frame begun before the presenter thread started never "
                               "ended; continuing past it.");
            }
        }
        if (compositor_stop_) {
            break;
        }
        frames_.clear(); // anything left never reached the runtime
        compositor_phase_ = "in the runtime's xrWaitFrame";
        lock.unlock();
        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        const auto called = Clock::now();
        const XrResult waited = RuntimeWait(calls, session, &wait_info, &frame_state);
        const auto returned = Clock::now();
        lock.lock();
        if (XR_FAILED(waited)) {
            PauseLocked("runtime xrWaitFrame failed on the presenter thread (" + std::to_string(waited) + ")");
            break;
        }
        NotePhaseLocked(called, returned, frame_state.predictedDisplayTime, frame_state.predictedDisplayPeriod);
        NoteRuntimeClockLocked(returned, frame_state.predictedDisplayTime, frame_state.predictedDisplayPeriod,
                               frame_state.shouldRender == XR_TRUE);
        const std::uint64_t slot = ++slot_index_;
        slot_time_ = frame_state.predictedDisplayTime;
        slot_period_ = frame_state.predictedDisplayPeriod;
        NoteRuntimePeriodLocked(slot_period_, returned);
        if (frame_state.shouldRender == XR_TRUE) {
            // The headset off, a runtime may pace its waits as it likes.
            NoteRuntimeHalvingLocked(called, returned, slot_time_, slot_period_, true);
        }
        slot_should_render_ = frame_state.shouldRender == XR_TRUE;
        slot_cv_.notify_all();
        // What this refresh's half-refresh thresholds are judged by: at most
        // the refresh period, as submissions are spaced (a period echoing a
        // late caller's own interval would widen them by refreshes). The
        // slots' display times keep the period reported (ThreadedWaitLocked).
        const XrDuration period = SubmitSpacingLocked(slot_period_);

        // Submit when the runtime would have released this wait, on a refresh
        // grid from the previous submission (ScheduleSubmissionLocked).
        const Clock::time_point submit_at = ScheduleSubmissionLocked(returned, slot_time_, period).first;
        compositor_phase_ = "choosing a frame for the refresh";
        // The newest real frame cannot be repeated (a space or swapchain it
        // used was destroyed) and nothing generated from it is ready: take
        // the next real frame as soon as it ends instead.
        auto unusable = [&] {
            const bool jit_ready = jit_ && std::any_of(jit_frames_.begin(), jit_frames_.end(), [&](const JitFrame& frame) {
                                       return frame.id == newest_copy_id_;
                                   });
            return !generated_ready_ && !jit_ready &&
                   (!newest_copy_.valid || (newest_copy_.has_projection && newest_copy_.projection_stale));
        };
        auto eligible = [&] { return slot_request_ && (slot_request_->earliest_slot <= slot || unusable()); };
        auto real_due = [&] {
            // Not one an app that abandons such frames superseded (ChooseJitLocked).
            return std::any_of(threaded_frames_.begin(), threaded_frames_.end(), [&](const ThreadedFrame& frame) {
                return frame.first_slot <= slot && !(frame.superseded && abandons_frames_);
            });
        };
        if (jit_) {
            // Just in time every refresh is this thread's: the next real
            // frame, when this refresh needs it and it is on its way, gets
            // until the refresh is due. Later, a cover frame shows this
            // refresh's time as well (waiting longer would push the next
            // refreshes late too).
            auto due = [&] {
                bool needed = false;
                ChooseJitLocked(slot_time_, period, jit_interpolate_, needed);
                return needed;
            };
            if (due()) {
                CompositorWait(lock, submit_at, [&] { return compositor_stop_ || !due(); });
            }
        } else {
            CompositorWait(lock, submit_at, [&] { return compositor_stop_ || eligible(); });
            if (!compositor_stop_ && !eligible() && (real_due() || generation_pending_)) {
                // A real frame due in this slot, or the frames being generated
                // from the newest one, get half a refresh more.
                CompositorWait(lock, submit_at + ns(period / 2), [&] {
                    return compositor_stop_ || eligible() || (!real_due() && !generation_pending_);
                });
            }
            if (!compositor_stop_ && !eligible() && unusable()) {
                // The runtime keeps showing the last frame meanwhile.
                CompositorWait(lock, submit_at + std::chrono::milliseconds(100),
                               [&] { return compositor_stop_ || eligible(); });
            }
        }
        if (eligible()) {
            // Real frames keep the spacing generated ones get. The request
            // stays: only this thread or a stop after it clears it.
            if (Clock::now() < submit_at) {
                lock.unlock();
                SleepUntil(timer, submit_at);
                lock.lock();
            }
            slot_offer_ = SlotOffer{slot_request_->frame_id, slot, slot_time_, slot_should_render_};
            slot_request_.reset();
            offer_done_ = false;
            compositor_phase_ = "handing a refresh to the game's xrEndFrame";
            WakeSlots();
            CompositorWait(lock, forever, [&] { return offer_done_; });
            slot_offer_.reset();
            continue;
        }

        if (jit_) {
            // Just in time: the frame this refresh shows, written now.
            compositor_phase_ = "writing a generated frame";
            bool due = false;
            JitChoice choice = ChooseJitLocked(slot_time_, period, jit_interpolate_, due);
            if (choice.cover && due) {
                ++jit_covers_;
            }
            // A refresh the runtime does not render (the headset is off):
            // nothing is written or shown, and it says nothing about writing
            // or the lead (real frames it passes over are not scored).
            const bool rendered = slot_should_render_;
            // Real frames a later refresh needs came ahead of it; those whose
            // game frames are on the GPU already: their lead had a refresh to
            // spare (ScoreLeadLocked).
            for (JitFrame& kept : jit_frames_) {
                if (!rendered) {
                    kept.scored = kept.scored || kept.time <= slot_time_ + period / 2;
                } else if (!kept.scored && kept.needed > slot_time_ + period / 2) {
                    kept.ahead = true;
                    if (!kept.captured_early) {
                        kept.captured_early = !gpu_ || !kept.capture_mark || gpu_->CaptureReached(kept.capture_mark);
                    }
                }
            }
            std::optional<JitFrame> source;
            std::optional<JitFrame> fallback;
            JitChoice fallback_choice;
            std::optional<std::uint64_t> chosen_id;
            bool skip = false;
            if (choice.index && rendered) {
                const JitFrame& chosen = jit_frames_[*choice.index];
                chosen_id = chosen.id;
                // The outputs already hold this real frame's exact copy:
                // nothing is written, the runtime shows their newest images
                // again (a game showing its last frame costs no GPU work).
                skip = !chosen.as_is && jit_exact_ && JitCopyOnly(chosen, choice) && jit_shown_ &&
                       std::get<0>(*jit_shown_) == chosen.id && !jit_views_.empty() && !output_sets_.empty() &&
                       jit_views_.size() == output_sets_.front().size() &&
                       std::equal(jit_views_.begin(), jit_views_.end(), output_sets_.front().begin(),
                                  [](const XrCompositionLayerProjectionView& view, const Output& output) {
                                      return view.subImage.swapchain == output.swapchain;
                                  });
                if (!skip) {
                    source = chosen;
                    // As if the chosen real frame had not arrived yet: for
                    // when its game frame is still on the GPU.
                    bool unused = false;
                    fallback_choice = ChooseJitLocked(slot_time_, period, jit_interpolate_, unused, *choice.index);
                    if (fallback_choice.index && !chosen.as_is) {
                        fallback = jit_frames_[*fallback_choice.index];
                    }
                }
            }
            if (!rendered) {
                ++totals_.unrendered;
            } else if (skip) {
                ++totals_.copies_skipped;
            } else if (!choice.index) {
                ++totals_.unwritten_source;
            }
            // Research (VECTORXR_FRAMEGEN_SYNTH_EYE_PROBE): a generated frame
            // written for this refresh, the runtime is asked where the views
            // are at its display time (the one this thread just waited for:
            // never past the runtime's own horizon).
            const bool probe = eye_probe_ && !eye_probe_stopped_ && xr_.locate_views != nullptr;
            const XrTime probe_time = slot_time_;
            lock.unlock();
            std::vector<XrCompositionLayerProjectionView> written_views;
            JitWrite write;
            const bool written = source && SynthesizeJit(source, choice, fallback, fallback_choice, written_views, write);
            if (probe && written && !write.exact) {
                ProbeEyeLocate(session, *source, choice.mode == FrameGenSynthesisMode::kExtrapolate, written_views,
                               probe_time);
            }
            SleepUntil(timer, submit_at);
            std::unique_lock submit_lock(submit_mutex_);
            lock.lock();
            totals_.unready += write.uncaptured ? 1 : 0;
            totals_.ring_exhausted += write.ring_exhausted ? 1 : 0;
            if (write.ring_short) {
                NoteRingShortLocked(write, returned, period);
            }
            if (chosen_id) {
                // Each real frame's lead sample, when a refresh first chooses
                // it: late if its game frame was not on the GPU yet, its
                // motion not measured though the refresh shows it moved
                // (interpolating, the refresh before its own), or the refresh
                // it was needed for has passed. Older frames never chosen
                // were passed over: late if they came after the refresh they
                // were needed for. One that came ahead of it was passed over
                // because that refresh was missed (the presenter thread or
                // the runtime was late; one in ten with Varjo's runtime under
                // a GPU-bound DCS, 2026-10-14) or the next frame's is nearer:
                // no lead cures that, and it says nothing of the frame's
                // own, so it is not scored. Scored late, those raised the
                // lead to its cap and held it there.
                for (JitFrame& kept : jit_frames_) {
                    if (kept.scored || kept.id > *chosen_id) {
                        continue;
                    }
                    if (kept.id < *chosen_id) {
                        if (kept.ahead) {
                            kept.scored = true;
                        } else {
                            ScoreLeadLocked(kept, true);
                        }
                        continue;
                    }
                    // Past the refresh it was needed for though it came ahead
                    // of it: that refresh was missed, as above.
                    const bool past = slot_time_ > kept.needed + period / 2;
                    const bool late =
                        write.uncaptured || write.unmeasured || write.motion_late || (past && !kept.ahead);
                    if (past && !late) {
                        kept.scored = true;
                        continue;
                    }
                    totals_.late_gpu += late && write.uncaptured ? 1 : 0;
                    ScoreLeadLocked(kept, late);
                }
            }
            // Composing quad views, attempted writes that keep failing hand
            // the views back to the layer for a while (Frame Generation stands
            // by meanwhile); a refresh the runtime does not render (nothing is
            // written then) and a frame not on the GPU yet say nothing about
            // writing.
            const auto now = Clock::now();
            if (quad_mode_ && source && !write.as_is && now >= quad_declined_until_) {
                if (written) {
                    quad_unwritten_in_row_ = 0;
                } else if (write.failed) {
                    ++quad_unwritten_in_row_;
                    if (logger_ && quad_unwritten_in_row_ == 1 &&
                        logged_quad_write_failures_ < kLoggedQuadWriteFailures) {
                        ++logged_quad_write_failures_;
                        logger_->Error("Frame Generation could not write a composed frame (" + write.error +
                                       "); the last frame is shown again.");
                    }
                    if (quad_unwritten_in_row_ >= kQuadUnwrittenBeforeDecline) {
                        DeclineQuadLocked(now, write.error);
                    }
                }
            }
            FrameCopy content;
            bool distinct = false;
            bool generated = false;
            bool submit = true; // false: nothing current to show, the runtime shows its last frame again
            // The real frame's layers as they are now: a swapchain or space
            // the game destroyed while the frame was written is gone from
            // them (ForgetSwapchain, ForgetSpace; both hold submit_mutex_).
            const auto find_kept = [&](std::uint64_t id) {
                return std::find_if(jit_frames_.begin(), jit_frames_.end(),
                                    [&](const JitFrame& frame) { return frame.id == id; });
            };
            // A frame shown as it is (nothing of Frame Generation's; the
            // output images keep the last frame written): its copy while its
            // images are still the newest in their swapchains, else a newer
            // frame the layer composed, early, else nothing. What is shown is
            // what a refresh nothing is written for shows again.
            const auto show_as_is = [&](FrameCopy copy, std::uint64_t id, XrTime made_for) {
                const AsIs current = CurrentAsIsLocked(copy, id, made_for);
                if (!current.copy) {
                    submit = false;
                    ++totals_.stale_skipped;
                    return;
                }
                content = *current.copy;
                jit_content_ = content;
                jit_views_.clear();
                jit_exact_ = false;
                jit_shown_as_is_ = true;
                jit_content_id_ = current.id;
                jit_content_time_ = current.time;
                const std::tuple<std::uint64_t, int, float> shown{current.id, -2, 0.0f};
                distinct = jit_shown_ != shown;
                jit_shown_ = shown;
            };
            const auto as_is = write.as_is ? find_kept(source->id) : jit_frames_.end();
            const auto kept = written ? find_kept(source->id) : jit_frames_.end();
            int path = 0; // the trace's "pick": which of the branches below showed the refresh
            if (!rendered) {
                // Not shown: no layers (below), and the last frame submitted
                // stays what a refresh nothing is written for repeats.
                path = 1;
            } else if (as_is != jit_frames_.end()) {
                // A frame the layer composed.
                path = 2;
                show_as_is(as_is->copy, as_is->id, as_is->time);
            } else if (kept != jit_frames_.end()) {
                path = 3;
                content = kept->copy;
                jit_started_ = true;
                jit_content_ = content;
                jit_views_ = std::move(written_views);
                jit_exact_ = write.exact;
                jit_shown_as_is_ = false;
                const std::tuple<std::uint64_t, int, float> shown{source->id, static_cast<int>(choice.mode),
                                                                  write.exact ? 1.0f : choice.fraction};
                distinct = jit_shown_ != shown;
                jit_shown_ = shown;
                generated = true;
            } else if (written && jit_content_.valid && jit_content_.has_projection &&
                       jit_content_.layers[jit_content_.projection_index].projection.space ==
                           source->copy.layers[source->copy.projection_index].projection.space) {
                // Written, but its real frame was dropped meanwhile: the
                // image just released goes with its own views and the last
                // layers shown around it.
                path = 4;
                content = jit_content_;
                jit_views_ = std::move(written_views);
                jit_exact_ = false;
                jit_shown_as_is_ = false;
                generated = true;
            } else if (jit_shown_as_is_ && jit_content_.valid && !written) {
                // Nothing written: the frame last shown as it is, again (a
                // frame written before it would step back).
                path = 5;
                show_as_is(jit_content_, jit_content_id_, jit_content_time_);
            } else if (jit_content_.valid && !jit_views_.empty() && !written) {
                // The last frame written again: the output images' newest.
                path = 6;
                content = jit_content_;
                generated = true;
            } else if (!quad_mode_ || newest_copy_fresh_ || !jit_started_) {
                // Before any frame was written: the frame passed through last.
                // After that (its outputs remade, or its space destroyed) the
                // newest real frame as it is, rather than nothing: showing the
                // game's images, the refresh waits on the runtime's queue for
                // the game's work on them (SubmitRuntimeFrame), and the game
                // waits for them again (FrameGenQueueSubstitution::Flush).
                // Composing quad views, claimed frames never become the newest
                // copy: the frame passed through before the start is shown only
                // until anything else was, then the newest the layer composed.
                path = 7;
                const bool passed_through = quad_mode_ ? !newest_copy_fresh_ : !jit_started_;
                const FrameCopy& fallback_copy = quad_mode_ || jit_started_ ? newest_copy_ : passthrough_copy_;
                const std::uint64_t id = passed_through ? 0 : newest_copy_id_;
                const auto newest = passed_through ? jit_frames_.end() : find_kept(id);
                show_as_is(fallback_copy, id, newest != jit_frames_.end() ? newest->time : 0);
            } else {
                // Composing quad views, with nothing the layer composed since
                // the last frame written: that frame again, as the runtime
                // shows it.
                path = 8;
                submit = false;
                ++totals_.stale_skipped;
            }
            DropStaleDepthLocked(content);
            const bool usable = content.valid && content.has_projection && content.projection_index < content.layers.size();
            const XrSpace space = usable ? content.layers[content.projection_index].projection.space : XR_NULL_HANDLE;
            const XrCompositionLayerFlags flags =
                usable ? content.layers[content.projection_index].projection.layerFlags : 0;
            std::vector<XrCompositionLayerProjectionView> views = jit_views_;
            // Never the game's depth: its images' releases may wait for its
            // GPU work (FrameGenQueueSubstitution), and the frame shows an
            // image of Frame Generation's own.
            const bool depth = false;
            const XrTime time = slot_time_;
            if (trace_.Enabled()) {
                const std::uint64_t shown_id = jit_shown_ ? std::get<0>(*jit_shown_) : 0;
                const int shown_mode = jit_shown_ ? std::get<1>(*jit_shown_) : -3;
                const float shown_fraction = jit_shown_ ? std::get<2>(*jit_shown_) : 0.0f;
                const auto shown_frame = find_kept(shown_id);
                const auto bit = [](bool value, int position) { return value ? (std::int64_t{1} << position) : 0; };
                const std::int64_t flags =
                    bit(rendered, 0) | bit(skip, 1) | bit(written, 2) | bit(write.exact, 3) | bit(write.uncaptured, 4) |
                    bit(write.unmeasured, 5) | bit(write.stale, 6) | bit(write.motion_late, 7) |
                    bit(write.ring_exhausted, 8) | bit(write.as_is, 9) | bit(write.failed, 10) | bit(choice.cover, 11) |
                    bit(submit, 12) | bit(distinct, 13) | bit(generated, 14) | bit(choice.index.has_value(), 15) |
                    (static_cast<std::int64_t>(path) << 16) | (static_cast<std::int64_t>(shown_mode + 3) << 20) |
                    bit(write.ring_short, 24);
                trace_.Record("pick", shown_id, time, shown_frame != jit_frames_.end() ? shown_frame->time : 0, flags,
                              static_cast<std::int64_t>(std::llround(shown_fraction * 1e6)),
                              shown_frame != jit_frames_.end() ? shown_frame->previous_time : 0,
                              static_cast<std::int64_t>(chosen_id.value_or(0)));
            }
            lock.unlock();
            XrFrameEndInfo end_info =
                BuildFrame(content, generated && usable ? &views : nullptr, space, flags, depth, pointers);
            end_info.displayTime = time;
            if (!rendered || !content.valid) {
                end_info.layerCount = 0;
                end_info.layers = nullptr;
                distinct = false;
            }
            XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            compositor_phase_ = "submitting a generated frame";
            // Nothing current to show: the frame is begun, not ended, so the
            // runtime shows its last frame again (the next begin discards it).
            XrResult result = RuntimeBegin(calls, session, &begin_info);
            if (XR_SUCCEEDED(result) && submit) {
                result = RuntimeEnd(calls, session, &end_info, true);
            }
            submit_lock.unlock();
            lock.lock();
            if (!submit) {
                presenter_frame_open_ = XR_SUCCEEDED(result);
            } else {
                // Generated frames count once, when first shown.
                const bool moved = distinct && written && !write.exact;
                generated_ += moved ? 1 : 0;
                NoteSubmissionLocked(distinct, moved);
            }
            if (XR_FAILED(result)) {
                PauseLocked("runtime rejected a frame from the presenter thread (" + std::to_string(result) + ")");
                break;
            }
            continue;
        }

        // Fill the slot: the next frame generated from the newest real frame,
        // or that real frame again.
        lock.unlock();
        SleepUntil(timer, submit_at);
        std::unique_lock submit_lock(submit_mutex_);
        lock.lock();
        FrameCopy content = newest_copy_;
        DropStaleDepthLocked(content);
        std::vector<XrCompositionLayerProjectionView> views;
        bool views_exact = false;
        bool distinct = false;
        const bool use_generated =
            content.valid && content.has_projection && generated_ready_ && extrapolated_.valid &&
            !extrapolated_.sets.empty() &&
            extrapolated_.sets.front().size() == content.layers[content.projection_index].views.size();
        if (use_generated) {
            // The frame made for this refresh: frames made for refreshes
            // already past (a skipped refresh, or frames ready late) are
            // passed over, and the last one stays after its own (the next
            // real frame is late; interpolating, it is the real frame's copy).
            std::size_t index = std::min(next_generated_set_, extrapolated_.sets.size() - 1);
            while (index + 1 < extrapolated_.sets.size() && index < extrapolated_.targets.size() &&
                   extrapolated_.targets[index] + period / 2 < slot_time_) {
                ++index;
                ++skipped_generated_;
            }
            views = extrapolated_.sets[index];
            views_exact = index < extrapolated_.exact.size() && extrapolated_.exact[index];
            next_generated_set_ = index + 1;
            // The last set shown again (the next real frame is late) is a
            // repeat, as is the real frame shown again.
            const std::pair<std::uint64_t, std::size_t> shown{extrapolated_.generation, index};
            distinct = last_shown_set_ != shown;
            last_shown_set_ = shown;
        }
        const XrSpace space = extrapolated_.space;
        const XrCompositionLayerFlags flags = extrapolated_.flags;
        const XrTime time = slot_time_;
        const bool should_render = slot_should_render_;
        lock.unlock();
        XrFrameEndInfo end_info = BuildFrame(content, use_generated ? &views : nullptr, space, flags, views_exact, pointers);
        end_info.displayTime = time;
        if (!should_render || !content.valid) {
            end_info.layerCount = 0;
            end_info.layers = nullptr;
            distinct = false;
        }
        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        compositor_phase_ = "submitting a generated frame";
        XrResult result = RuntimeBegin(calls, session, &begin_info);
        if (XR_SUCCEEDED(result)) {
            result = RuntimeEnd(calls, session, &end_info, true);
        }
        submit_lock.unlock();
        lock.lock();
        NoteSubmissionLocked(distinct, use_generated && !views_exact);
        if (XR_FAILED(result)) {
            PauseLocked("runtime rejected a frame from the presenter thread (" + std::to_string(result) + ")");
            break;
        }
    }
    if (jit_) {
        // Output images acquired for a frame that was never written go back,
        // and ones replaced while composing quad views go: nothing shows them
        // once the run that wrote them has ended (they would stay through
        // standing by otherwise, at headset resolution).
        lock.unlock();
        ReleaseOutputs();
        DestroyRetiredOutputs();
        lock.lock();
    }
    compositor_thread_id_ = 0;
    compositor_running_ = false;
    compositor_phase_ = "stopped";
    slot_request_.reset();
    WakeSlots();
    idle_cv_.notify_all();
}

bool FrameGenPresenter::ShowsOwnImagesOnly() const {
    std::scoped_lock lock(mutex_);
    return compositor_running_ && jit_ && jit_started_;
}

bool FrameGenPresenter::CapturesFrom(XrSwapchain swapchain) const {
    std::scoped_lock lock(mutex_);
    return std::any_of(capture_targets_.begin(), capture_targets_.end(),
                       [&](const CaptureTarget& target) { return target.swapchain == swapchain; });
}

FrameGenStatus FrameGenPresenter::Status() const {
    std::scoped_lock lock(mutex_);
    FrameGenStatus status;
    status.state = state_;
    status.reason = reason_;
    status.mode = settings_.synthesis_mode;
    const auto now = Clock::now();
    status.app_fps = RatePerSecond(app_frames_, now);
    status.output_fps = RatePerSecond(submissions_, now);
    status.distinct_fps = RatePerSecond(distinct_submissions_, now);
    status.gpu_ms = gpu_ms_;
    status.interpolate_flow_active = interpolate_flow_active_;
    status.interpolate_flow_mode = ToString(interpolate_flow_env_.value_or(settings_.interpolate_flow));
    status.interpolate_flow_reason = interpolate_flow_reason_;
    status.interpolate_flow_switches = interpolate_flow_switches_;
    status.interpolate_flow_probes = interpolate_flow_probes_;
    status.interpolate_flow_fallbacks = interpolate_flow_fallbacks_;
    status.interpolate_flow_probing = interpolate_flow_probing_;
    status.generated = generated_;
    status.repeated = repeated_;
    status.failures = failures_;
    status.demoted = demoted_;
    status.abandoned = abandoned_frames_;
    status.captured = captured_;
    status.retry_pending = state_ == FrameGenState::kPaused && automatic_retries_ < retry_attempts_;
    status.multiplier = EffectiveMultiplierLocked();
    status.dynamic = DynamicLocked();
    status.automatic = settings_.multiplier == FrameGenMultiplier::kAuto ||
                       (settings_.multiplier == FrameGenMultiplier::kDynamic && !status.dynamic);
    status.full_rate = full_rate_ && state_ == FrameGenState::kActive;
    const XrDuration grid = refresh_period_ > 0 ? GameGridLocked() : 0;
    if (status.automatic && grid > 0) {
        // UpdateAutoLocked's thresholds as frame rates: a step up once more
        // than 30% of frames take kAutoLateFactor times their refreshes; a
        // probe one step down after five steady seconds, from its own thread
        // only for a game whose own time per frame is within reach of it
        // (AutoProbeInReachLocked), inline for any game holding its step.
        // None while it would not step up: held at full rate (inline cycles
        // that kept starting late, or the runtime halving), or video memory
        // near its budget.
        const double grid_hz = 1e9 / static_cast<double>(grid);
        const bool held = auto_multiplier_ == 1 && ((inline_hold_until_ && now < *inline_hold_until_) ||
                                                     (runtime_halving_ && auto_halving_hold_until_ &&
                                                      now < *auto_halving_hold_until_));
        const bool memory_pressure = !(compositor_running_ && jit_) && memory_budget_ &&
                                     memory_budget_->budget_bytes &&
                                     memory_budget_->usage_bytes >
                                         memory_budget_->budget_bytes - memory_budget_->budget_bytes / 20;
        // Frames are judged late on the period the game is told (the runtime's
        // as it reports it: under SteamVR's period echo, longer than the grid),
        // and not at all while the runtime's clock is displaced.
        const double judged_hz = last_period_ > 0 ? 1e9 / static_cast<double>(last_period_) : grid_hz;
        if (auto_multiplier_ < std::min(auto_memory_ceiling_, kMaxMultiplier) && !held && !memory_pressure &&
            !clock_displaced_) {
            status.auto_step_up_below_fps = judged_hz / (kAutoLateFactor * auto_multiplier_);
        }
        if (auto_multiplier_ > 1) {
            status.auto_step_down_above_fps =
                compositor_running_ ? (dynamic_policy_.stay_above - kProbeReach) * grid_hz / (auto_multiplier_ - 1)
                                    : grid_hz / auto_multiplier_;
        }
    }
    if (status.automatic && compositor_running_) {
        const std::size_t count = std::min(auto_busy_count_, auto_busy_.size());
        if (count >= 8) {
            std::array<double, 32> sorted = auto_busy_;
            std::nth_element(sorted.begin(), sorted.begin() + count / 2, sorted.begin() + count);
            status.auto_game_own_fps = sorted[count / 2] > 0.0 ? 1000.0 / sorted[count / 2] : 0.0;
        }
    }
    // As the 30 s summary says it: just in time and Dynamic measure the
    // refreshes between real frames' slots, while the presenter thread
    // presents (frames passing through inline leave that value behind);
    // otherwise their ends' interval.
    status.refreshes_per_real_frame =
        compositor_running_ && (DynamicLocked() || jit_) && dynamic_interval_slots_ > 0.0 ? dynamic_interval_slots_
        : grid > 0 && real_interval_ns_ > 0.0 ? real_interval_ns_ / static_cast<double>(grid)
                                              : 0.0;
    status.standby = status.dynamic && dynamic_standby_;
    status.dynamic_reason = dynamic_reason_;
    status.dynamic_engagements = dynamic_engagements_;
    status.dynamic_stand_downs = dynamic_stand_downs_;
    status.dynamic_probes = dynamic_probes_;
    status.runtime_halving = runtime_halving_;
    status.runtime_clock_displaced = clock_displaced_;
    status.runtime_clock_displacements = totals_.clock_displacements;
    status.threaded = compositor_running_;
    status.lead_slots = lead_slots_;
    status.lead_floor = lead_floor_;
    status.lead_ups = totals_.lead_ups;
    status.lead_downs = totals_.lead_downs;
    status.lead_probes = totals_.lead_probes;
    status.lead_probes_failed = totals_.lead_probes_failed;
    status.lead_downs_undone = totals_.lead_downs_undone;
    {
        const std::uint32_t multiplier = std::max(2u, EffectiveMultiplierLocked());
        const std::uint32_t cap = LeadCapLocked(multiplier);
        status.lead_ceiling = cap < 4 * multiplier ? cap : 0;
    }
    status.lead_ceiling_on = RingCeilingLocked();
    status.ring_short = totals_.ring_short;
    status.ring_short_ignored = totals_.ring_short_ignored;
    status.ring_short_late = totals_.ring_short_late;
    status.ring_short_would = totals_.ring_short_would;
    status.lead_ring_downs = totals_.lead_ring_downs;
    status.lead_ring_raises = totals_.lead_ring_raises;
    status.lead_ring_refused = totals_.lead_ring_refused;
    status.lead_ring_held = totals_.lead_ring_held;
    status.lead_ring_undone = totals_.lead_ring_undone;
    status.app_frames = totals_.app_frames;
    status.late_cpu = totals_.late_cpu;
    status.late_gpu = totals_.late_gpu;
    status.refresh_period_ns = refresh_period_;
    status.grid_period_ns = refresh_period_ > 0 ? GameGridLocked() : 0;
    status.working_bytes = working_bytes_;
    status.output_bytes = output_bytes_;
    status.memory = memory_budget_;
    status.gpu_gating = gpu_gate_enabled_ && gpu_gate_armed_;
    status.gpu_waits = totals_.gpu_waits;
    status.gpu_wait_timeouts = totals_.gpu_wait_timeouts;
    status.pauses = totals_.pauses;
    status.unready_capture = totals_.unready;
    status.unmeasured_copies = totals_.unmeasured;
    status.stale_motion = totals_.stale_motion;
    status.copies_skipped = totals_.copies_skipped;
    status.unwritten = totals_.unwritten_wait + totals_.unwritten_failed + totals_.unwritten_outputs +
                       totals_.unwritten_source;
    status.ring_exhausted = totals_.ring_exhausted;
    status.stale_skipped = totals_.stale_skipped;
    status.analyses_timed = totals_.analyses_timed;
    status.analyses_late = totals_.analyses_late;
    status.quad = quad_mode_ && compositor_running_;
    status.quad_claimed = totals_.quad_claimed;
    status.quad_declined =
        totals_.quad_uncaptured + totals_.quad_busy + totals_.quad_declined + totals_.quad_not_presenting;
    status.quad_setups = totals_.quad_setups;
    status.quad_unanalyzed = totals_.quad_unanalyzed;
    status.quad_write_declines = quad_write_declines_;
    status.quad_declining = now < quad_declined_until_;
    if (depth_seen_) {
        const FrameGenDepthEvidence& evidence = depth_status_.evidence;
        status.depth = !depth_sent_ ? FrameGenDepthState::kNotSent
                       : !depth_readable_ ? FrameGenDepthState::kNotReadable
                       : depth_status_.reading == FrameGenDepthReading::kUnknown ? FrameGenDepthState::kChecking
                       : depth_status_.reading == FrameGenDepthReading::kUnusable
                           ? (evidence.binocular > 0.0 && evidence.cockpit < 0.005 * evidence.binocular
                                  ? FrameGenDepthState::kEmpty
                                  : FrameGenDepthState::kUnusable)
                           : FrameGenDepthState::kInUse;
    }
    return status;
}

FrameGenOsdText DescribeFrameGen(const FrameGenStatus& status, bool started) {
    // The expanded OSD row says the state and how it runs; the line under it
    // what it does right now, or why not. The OSD separates parts as its other
    // rows do, the app with a middle dot.
    constexpr const char* kOsd = "  /  ";
    constexpr const char* kApp = " \xC2\xB7 ";
    constexpr const char* kArrow = " \xE2\x86\x92 ";
    // The OSD's second line starts as a sentence; reasons start lowercase.
    const auto sentence = [](std::string line) {
        if (!line.empty() && line[0] >= 'a' && line[0] <= 'z') line[0] = static_cast<char>(line[0] - 'a' + 'A');
        return line;
    };
    FrameGenOsdText text;
    text.state = ToString(status.state);
    // The runtime holds the frames presented to half its refresh rate or
    // less: the line under the row says so, and the app's line ends with it.
    const auto halving = [&](FrameGenOsdText& said) {
        if (status.runtime_halving) {
            said.detail = kHalvingDetail;
            said.reason += kApp + std::string(kHalvingReason);
        }
        return said;
    };
    switch (status.state) {
    case FrameGenState::kActive: {
        text.badge = "STBY";
        text.state = "standby";
        // Composed quad views frames kept failing to be written: frames pass
        // straight through for a while, whatever else the state says.
        if (status.quad_declining || (status.full_rate && !status.reason.empty())) {
            text.row = "Standby";
            text.detail = "Generated frames could not be shown, retrying soon";
            text.compact = std::string("Standby") + kOsd + "retrying soon";
            text.reason = text.detail;
            return text;
        }
        // Standing by: the game holds the refresh rate on its own, so its
        // frames pass straight through.
        if (status.standby || status.full_rate) {
            const std::string rate = status.dynamic ? "Dynamic" : "Auto";
            const std::string game = "game at " + Number(status.app_fps, 0) + " fps";
            text.row = std::string("Standby") + kOsd + rate;
            text.detail = "The game runs at " + Number(status.app_fps, 0) + " fps";
            text.compact = std::string("Standby") + kOsd + game;
            text.reason = rate + kApp + game;
            if (status.runtime_clock_displaced) {
                // Nothing engages until the runtime's display times advance
                // with the time again (Varjo's, after a loading stall).
                text.detail = kClockDetail;
                text.reason += kApp + std::string(kClockReason);
            }
            return halving(text);
        }
        // Distinct frames: a frame shown twice (generation or the game ran
        // late) is not counted again.
        const std::string mode = status.mode == FrameGenSynthesisMode::kExtrapolate ? "Extrapolate" : "Interpolate";
        const std::string rate =
            status.dynamic ? std::string("Dynamic") : (status.automatic ? "Auto " : "") + MultiplierText(status.multiplier);
        const std::string rates = Number(status.app_fps, 0) + kArrow + Number(status.distinct_fps, 0) + " fps";
        const std::string gpu = status.gpu_ms > 0.0 ? "GPU " + Number(status.gpu_ms, 1) + " ms" : std::string();
        text.badge = "ON";
        text.state = "active";
        text.shown_fps = status.distinct_fps;
        text.row = std::string("On") + kOsd + mode + kOsd + rate;
        text.detail = rates + (gpu.empty() ? "" : kOsd + gpu);
        text.compact = rate + kOsd + rates;
        text.reason = mode + kApp + rate + kApp + rates + (gpu.empty() ? "" : kApp + gpu);
        text.uses_depth = status.depth == FrameGenDepthState::kInUse;
        text.uses_motion_vectors = status.motion_vectors;
        return halving(text);
    }
    case FrameGenState::kWaiting:
        text.badge = "WAIT";
        text.row = "Starting";
        text.detail = status.runtime_clock_displaced ? kClockDetail : "Waiting for steady frames";
        text.compact = "Starting";
        text.reason = status.runtime_clock_displaced ? std::string(kClockReason) : status.reason;
        return text;
    case FrameGenState::kPaused:
        text.badge = "PAUSED";
        text.row = status.retry_pending ? std::string("Paused") + kOsd + "retrying soon" : std::string("Paused");
        text.detail = sentence(status.reason);
        text.compact = "Paused";
        text.reason = status.reason;
        if (status.retry_pending) text.reason += (text.reason.empty() ? "" : kApp) + std::string("retrying soon");
        return text;
    case FrameGenState::kUnavailable:
        text.badge = "N/A";
        text.row = text.compact = "Unavailable";
        text.detail = sentence(status.reason);
        text.reason = status.reason;
        return text;
    default:
        text.row = text.compact = started ? "Off" : "Disabled";
        if (started) text.badge = "OFF";
        return text;
    }
}

FrameGenOsdText FrameGenPresenter::OsdDescription() const {
    return DescribeFrameGen(Status(), Started());
}

std::string FrameGenPresenter::OsdText() const {
    return OsdDescription().row;
}

std::string FrameGenPresenter::OsdDetailText() const {
    return OsdDescription().detail;
}

std::string FrameGenPresenter::CompactOsdText() const {
    return OsdDescription().compact;
}

} // namespace depthxr
