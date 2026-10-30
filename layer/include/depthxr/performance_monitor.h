#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "depthxr/osd_strings.h"

namespace depthxr {

class Logger;

// The On-Screen Display's PERFORMANCE row: how much of the game's frame budget its CPU and the GPU leave, and its
// video memory.
//
// Budget: the time one of the game's frames may take. It is the runtime's refresh period, never the period the game
// is told: Frame Generation changes that, and SteamVR with Pimax's driver echoes a late caller's own frame interval.
// Under Frame Generation it is the game's base frame target, the refresh period times the refreshes each real frame
// fills (Dynamic's current count).
// CPU: the game's own time per frame, from its xrWaitFrame's return to its xrEndFrame (what Frame Generation logs as
// appWorkMs), hidden when that cannot be trusted: the game waits again with a frame open (pipelining), or Turbo
// fabricates its waits.
// GPU: the 3D engine's utilization from Windows' documented "GPU Engine" performance counters, for the whole adapter
// (the runtime's compositor runs in another process) and for the game's process.
// Video memory: the process's local video memory use and budget (IDXGIAdapter3::QueryVideoMemoryInfo) on the
// session's adapter, and Frame Generation's own share.

using PerformanceClock = std::chrono::steady_clock;

// What the row judges by (performance_tests.cpp checks each).
//
// The sampler thread's period. The OSD (5 Hz by default) draws the latest reading.
inline constexpr std::chrono::milliseconds kPerformanceSamplePeriod{500};
// Readings are smoothed exponentially with this time constant, so the panel does not flicker between refreshes.
inline constexpr double kPerformanceSmoothingSeconds = 1.0;
// The game's frame interval past the budget by this factor: it runs below its target (90 Hz: 85.7 fps or less).
inline constexpr double kPerformanceBelowTarget = 1.05;
// Whole-GPU 3D utilization at or over this: GPU-bound. Under 0.6 ms of a 90 Hz refresh is left idle.
inline constexpr double kPerformanceGpuBoundPercent = 95.0;
// Under this the GPU has room: a game below its target with its CPU time over the budget is CPU-bound.
inline constexpr double kPerformanceGpuRoomPercent = 85.0;
// The share of the game's waits that returned while one of its frames was begun and not ended (pipelining, as
// OpenComposite does), from which its CPU time is hidden: such a wait can sit inside the time measured.
inline constexpr double kPerformancePipelinedShare = 0.25;
// The share of the game's waits Turbo fabricated, from which its CPU time is hidden.
inline constexpr double kPerformanceFabricatedShare = 0.10;
// Frame Generation generating: the share of real frames late (FrameGenStatus late_cpu + late_gpu) from which the
// verdict is the side that was late more often.
inline constexpr double kPerformanceLateShare = 0.05;
// A verdict shows, changes or clears only once it has held this long.
inline constexpr std::chrono::milliseconds kPerformanceVerdictHold{1000};
// Without one of the game's frames this long, its CPU time and frame interval are unknown again.
inline constexpr std::chrono::seconds kPerformanceFramesStale{2};
// How often the sampler logs its summary.
inline constexpr std::chrono::seconds kPerformanceSummaryInterval{30};

// The runtime's refresh period from the periods its waits report, the way Frame Generation's presenter keeps it
// (FrameGenPresenter::NoteRuntimePeriodLocked): a period about the refresh period or shorter is taken at once; one
// two to five times it is the runtime cutting the frame rate, or reporting how late its caller was, and is ignored;
// another longer one is taken once the periods reported have agreed with it for a second. A runtime cutting the rate
// from its first frame is indistinguishable from a headset at that fraction of the refresh rate.
class RefreshPeriodFilter {
  public:
    static constexpr std::uint32_t kMaxMultiple = 5;
    static constexpr std::chrono::seconds kChangeAfter{1};

    void Note(std::int64_t period_ns, PerformanceClock::time_point now);
    [[nodiscard]] std::int64_t PeriodNs() const { return period_ns_; }
    void Reset();

  private:
    std::int64_t period_ns_{0};
    std::optional<std::pair<std::int64_t, PerformanceClock::time_point>> candidate_;
};

// The game's frames, from its own frame calls on any of its threads (one short lock per call).
class PerformanceFrameClock {
  public:
    // The game's xrWaitFrame returned; `fabricated`: Turbo made the frame up instead of the runtime.
    void Wait(PerformanceClock::time_point returned, bool fabricated);
    // The game's xrBeginFrame. `thread`: the caller (0: unknown).
    void Begin(std::uint32_t thread = 0);
    // The game's xrEndFrame was entered: its CPU time ends here. `rendered`: with layers. `thread`: the caller.
    void End(PerformanceClock::time_point entered, bool rendered, std::uint32_t thread);
    // Any runtime xrWaitFrame (the game's, Turbo's or Frame Generation's) returned this predictedDisplayPeriod.
    void RuntimePeriod(std::int64_t period_ns, PerformanceClock::time_point now);

    struct Window {
        std::uint64_t waits{0};
        std::uint64_t pipelined{0};  // waits returned while one of the game's frames was begun and not ended
        std::uint64_t fabricated{0}; // waits Turbo fabricated
        std::uint64_t frames{0};     // rendered frames timed from their wait to their end
        double cpu_sum_ms{0.0};
        double cpu_max_ms{0.0};
        std::uint64_t intervals{0}; // between the game's xrEndFrame calls, pauses left out
        double interval_sum_ms{0.0};
        std::int64_t refresh_ns{0};    // RefreshPeriodFilter's period
        std::uint32_t end_thread{0};   // the thread that ended the newest frame (0: none yet)
    };
    // The frames since the last call. The refresh period and the end thread carry over.
    Window Take();
    void Reset();

  private:
    // Waits are matched to ends oldest first; a game that loses frames (begins one twice) never grows this past it.
    static constexpr std::size_t kMaxOpenWaits = 4;
    // A gap between ends longer than this is a pause, not a frame interval.
    static constexpr std::chrono::seconds kPause{1};

    std::mutex mutex_;
    std::deque<PerformanceClock::time_point> waits_;
    bool open_{false};
    std::uint32_t begin_thread_{0}; // the thread that began the newest frame
    // A begin over its own thread's open frame: the runtime discards that
    // frame (OpenComposite does this), so if the same thread ends next, the
    // waits before this one belong to frames that never end.
    std::optional<PerformanceClock::time_point> discard_before_;
    std::optional<PerformanceClock::time_point> last_end_;
    RefreshPeriodFilter refresh_;
    Window window_;
};

enum class PerformanceVerdict : std::uint8_t {
    kNone,
    kCpuBound,
    kGpuBound,
};

// Why the CPU number is shown or not.
enum class PerformanceCpuState : std::uint8_t {
    kWaiting,   // no frames timed yet
    kMeasured,
    kPipelined, // the game waits again with a frame open
    kTurbo,     // Turbo fabricates the game's waits
};

const char* ToString(PerformanceVerdict verdict);   // "none", "cpu-bound", "gpu-bound"
const char* ToString(PerformanceCpuState state);    // "waiting", "measured", "pipelined", "turbo"

// Frame Generation's figures for one sample, from its status (sessions it started in).
struct PerformanceFrameGen {
    bool generating{false};      // engaged: generated frames fill the refreshes between the game's
    std::uint32_t refreshes{1};  // refreshes per real frame while generating (Dynamic: its current count)
    std::int64_t refresh_ns{0};  // the runtime's refresh period as Frame Generation keeps it (0: not known yet)
    std::int64_t grid_ns{0};     // the period the game's frames are timed by: the refresh period, or the runtime's
                                 // multiple of it while the runtime holds frames to it
    std::uint64_t app_frames{0}; // session totals
    std::uint64_t late_cpu{0};
    std::uint64_t late_gpu{0};
    std::uint64_t own_bytes{0};  // its working images and output swapchain images
    std::optional<std::uint64_t> usage_bytes; // its own video memory sample, for sessions the sampler has no adapter of
    std::optional<std::uint64_t> budget_bytes;
};

// One sample's inputs (the sampler thread's, or a test's).
struct PerformanceTick {
    PerformanceClock::time_point now{};
    PerformanceFrameClock::Window frames;
    std::optional<double> gpu_percent;      // whole adapter, 3D engine
    std::optional<double> gpu_game_percent; // the game's process, 3D engine
    bool gpu_unavailable{false};            // the counters could not be opened or read (not just: not read yet)
    std::optional<double> thread_busy_percent; // the game's end-frame thread, of one core
    std::optional<std::uint64_t> usage_bytes;
    std::optional<std::uint64_t> budget_bytes;
    std::optional<PerformanceFrameGen> framegen;
};

struct PerformanceReading {
    double budget_ms{0.0}; // 0: unknown
    double frame_ms{0.0};  // the game's frame interval (0: no frames)
    PerformanceCpuState cpu_state{PerformanceCpuState::kWaiting};
    std::optional<double> cpu_ms; // while kMeasured
    std::optional<double> thread_busy_percent;
    std::optional<double> gpu_percent;
    std::optional<double> gpu_game_percent;
    bool gpu_unavailable{false};
    std::optional<std::uint64_t> usage_bytes;
    std::optional<std::uint64_t> budget_bytes;
    bool framegen_generating{false};
    std::uint32_t framegen_refreshes{1};
    std::uint64_t framegen_bytes{0};
    PerformanceVerdict verdict{PerformanceVerdict::kNone};

    // (budget - CPU time) / budget, as a percentage: negative over the budget.
    [[nodiscard]] std::optional<double> CpuHeadroomPercent() const;
    // 100 - whole-GPU 3D utilization.
    [[nodiscard]] std::optional<double> GpuHeadroomPercent() const;
};

// What a verdict is judged from (smoothed).
struct PerformanceJudgement {
    double budget_ms{0.0};
    double frame_ms{0.0};
    std::optional<double> cpu_ms;
    std::optional<double> gpu_percent;
    // Frame Generation generating: the shares of real frames that ended late, or were not on the GPU in time.
    std::optional<double> late_cpu_share;
    std::optional<double> late_gpu_share;
};
// Only when it is clear. Frame Generation generating: the side its late frames blame, when kPerformanceLateShare of
// them are late, else none. Otherwise: GPU-bound with the GPU at kPerformanceGpuBoundPercent; CPU-bound below the
// target, with the CPU time over the budget and the GPU under kPerformanceGpuRoomPercent; else none.
PerformanceVerdict JudgePerformance(const PerformanceJudgement& judgement);

// Smooths samples into a reading.
class PerformanceModel {
  public:
    void Tick(const PerformanceTick& tick);
    [[nodiscard]] const PerformanceReading& Reading() const { return reading_; }
    void Reset();

  private:
    struct Smoothed {
        std::optional<double> value;
        void Add(double sample, double alpha);
        void Clear() { value.reset(); }
    };

    PerformanceReading reading_;
    std::optional<PerformanceClock::time_point> last_tick_;
    std::optional<PerformanceClock::time_point> last_frames_;
    Smoothed cpu_, frame_, gpu_, gpu_game_, thread_, pipelined_, fabricated_, late_cpu_, late_gpu_;
    std::optional<PerformanceFrameGen> last_framegen_;
    PerformanceVerdict pending_{PerformanceVerdict::kNone};
    PerformanceClock::time_point pending_since_{};
};

// The OSD's text for a reading. Expanded: three cells (a headline over a detail line) beside a target or verdict
// under the row's label. Compact: the three headlines in one line, the bound side named instead of its headroom.
struct PerformanceOsdText {
    std::string target;  // "90 fps target"; empty when the budget is unknown
    std::string verdict; // "CPU-BOUND", "GPU-BOUND" or empty
    std::string cpu, cpu_detail;
    std::string gpu, gpu_detail;
    std::string vram, vram_detail;
    bool vram_over{false}; // usage over the budget: drawn in the attention colour
    bool cpu_bound{false}, gpu_bound{false};
    std::string compact_cpu, compact_gpu, compact_vram;
};
// In the OSD's language (English by default, as the tests read it).
PerformanceOsdText DescribePerformance(const PerformanceReading& reading, OsdLanguage language = OsdLanguage::En);
// "+32%", "-20%" with a minus sign (U+2212), "+0%"; clamped to -999..100.
std::string PerformanceHeadroomText(double percent);
// "9.8 GB" (GiB, as Task Manager shows them), "612 MB" under one.
std::string PerformanceMemoryText(std::uint64_t bytes);

// One "GPU Engine" counter instance: pid_<pid>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>.
struct GpuEngineInstance {
    std::uint32_t pid{0};
    std::uint64_t luid{0}; // high part << 32 | low part
    std::uint32_t phys{0};
    std::uint32_t engine{0};
    std::wstring type; // "3D", "Copy", "VideoDecode", ...
};
std::optional<GpuEngineInstance> ParseGpuEngineInstance(std::wstring_view name);

struct GpuEngineUtilization {
    std::optional<double> total; // the busiest 3D engine of the adapter, all processes summed (0-100)
    std::optional<double> game;  // the game's process on that adapter's 3D engines (0-100)
    std::size_t instances{0};    // 3D instances counted
};
// From instances and their utilization: only 3D engines of `luid` (all adapters when absent).
GpuEngineUtilization AggregateGpuEngines(const std::vector<std::pair<GpuEngineInstance, double>>& samples,
                                         std::uint32_t pid, std::optional<std::uint64_t> luid);

// The sampler's periodic log line, over the samples since the last.
class PerformanceSummary {
  public:
    struct Cost {
        double wall_ms{0.0}; // one sample's time on the sampler thread
    };
    void Add(const PerformanceReading& reading, const Cost& cost);
    [[nodiscard]] std::size_t Samples() const { return samples_; }
    // "Performance summary (30.0 s, 60 samples): budget=..." and resets. `sampler_cpu_percent`: the sampler
    // thread's CPU time over the window, of one core; `gpu_counters`: their state ("12 instances", or why not).
    std::string Take(double seconds, std::optional<double> sampler_cpu_percent, const std::string& gpu_counters);

  private:
    struct Stat {
        std::size_t count{0};
        double sum{0.0}, max{0.0};
        void Add(double value);
        [[nodiscard]] double Mean() const { return count ? sum / static_cast<double>(count) : 0.0; }
    };
    std::size_t samples_{0};
    Stat budget_, frame_, cpu_, gpu_, gpu_game_, thread_, wall_;
    std::size_t pipelined_{0}, turbo_{0}, cpu_bound_{0}, gpu_bound_{0};
    std::optional<std::uint64_t> usage_bytes_, budget_bytes_, usage_max_;
    std::uint64_t framegen_bytes_{0};
    bool framegen_{false};
};

// The sampler: a below-normal-priority thread that samples at kPerformanceSamplePeriod while the OSD shows the row,
// never on a frame thread. Its counters (pdh.dll, DXGI) are loaded and used on that thread only.
class PerformanceMonitor {
  public:
    // Frame Generation's figures for a sample (sessions it started in), else nullopt. Called on the sampler thread.
    using FrameGenSource = std::function<std::optional<PerformanceFrameGen>()>;

    PerformanceMonitor() = default;
    ~PerformanceMonitor();
    PerformanceMonitor(const PerformanceMonitor&) = delete;
    PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

    // Whether the game's frame calls feed Frames(): one relaxed load.
    [[nodiscard]] bool Wanted() const { return wanted_.load(std::memory_order_relaxed); }
    PerformanceFrameClock& Frames() { return frames_; }

    // A session's adapter (DXGI LUID, absent when unknown). Joins a previous session's thread: never call it with a
    // lock the sampler thread's FrameGenSource takes.
    void BeginSession(std::optional<std::uint64_t> adapter_luid, FrameGenSource framegen, Logger* logger);
    // Stops and joins the thread, which logs its last summary. Same rule as BeginSession.
    void EndSession();
    // Samples only while wanted (the OSD shows the row in a layout); the thread starts the first time. Never waits.
    void SetWanted(bool wanted);
    [[nodiscard]] PerformanceReading Reading() const;

  private:
    void Run();

    PerformanceFrameClock frames_;
    std::atomic<bool> wanted_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
    bool session_{false};
    bool start_failed_{false};
    std::optional<std::uint64_t> adapter_luid_;
    FrameGenSource framegen_;
    Logger* logger_{nullptr};
    PerformanceReading reading_;
    std::thread thread_;
};

} // namespace depthxr
