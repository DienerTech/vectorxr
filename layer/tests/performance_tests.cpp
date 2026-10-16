#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "depthxr/logger.h"
#include "depthxr/performance_monitor.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

using namespace depthxr;
using namespace std::chrono_literals;

namespace {

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool Near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

constexpr std::int64_t kRefresh90 = 11'111'111;
constexpr std::int64_t kRefresh72 = 13'888'889;

void TestRefreshFilter() {
    const auto t0 = PerformanceClock::now();
    RefreshPeriodFilter filter;
    filter.Note(kRefresh90, t0);
    Expect(filter.PeriodNs() == kRefresh90, "the first period is the refresh period");
    // SteamVR with Pimax's driver echoes a late caller's interval: two to five refreshes are never the refresh rate.
    for (int multiple = 2; multiple <= 5; ++multiple) {
        for (int i = 0; i < 40; ++i) filter.Note(kRefresh90 * multiple, t0 + i * 100ms);
        Expect(filter.PeriodNs() == kRefresh90, "a multiple of the refresh period became the refresh period");
    }
    // Another refresh rate (72 Hz) is taken once the periods agree with it for a second, not at the first.
    filter.Note(kRefresh72, t0 + 10s);
    Expect(filter.PeriodNs() == kRefresh90, "a longer period was taken at once");
    filter.Note(kRefresh72, t0 + 10s + 500ms);
    Expect(filter.PeriodNs() == kRefresh90, "a longer period was taken before it held a second");
    filter.Note(kRefresh72, t0 + 11s);
    Expect(filter.PeriodNs() == kRefresh72, "a new refresh rate was not taken after a second");
    // A hitch's one long period never becomes the refresh period.
    filter.Note(17'000'000, t0 + 12s);
    filter.Note(kRefresh72, t0 + 12s + 10ms);
    filter.Note(17'000'000, t0 + 14s);
    Expect(filter.PeriodNs() == kRefresh72, "a hitch became the refresh period");
    // A shorter one is taken at once.
    filter.Note(kRefresh90, t0 + 15s);
    Expect(filter.PeriodNs() == kRefresh90, "a shorter period was not taken at once");
    filter.Note(0, t0 + 16s);
    filter.Note(-5, t0 + 16s);
    Expect(filter.PeriodNs() == kRefresh90, "a period of zero or less was taken");
}

void TestFrameClock() {
    const auto t0 = PerformanceClock::now();
    PerformanceFrameClock clock;
    clock.RuntimePeriod(kRefresh90, t0);
    // Wait returns at t, the game ends its frame 7 ms later; frames 11 ms apart.
    for (int i = 0; i < 10; ++i) {
        const auto t = t0 + i * 11ms;
        clock.Wait(t, false);
        clock.Begin();
        clock.End(t + 7ms, true, 42);
    }
    auto window = clock.Take();
    Expect(window.waits == 10 && window.frames == 10 && window.pipelined == 0 && window.fabricated == 0,
           "frame counts were wrong");
    Expect(Near(window.cpu_sum_ms / window.frames, 7.0, 1e-6) && Near(window.cpu_max_ms, 7.0, 1e-6),
           "CPU time is not from the wait's return to the end");
    Expect(window.intervals == 9 && Near(window.interval_sum_ms / window.intervals, 11.0, 1e-6),
           "frame intervals are not between the ends");
    Expect(window.refresh_ns == kRefresh90 && window.end_thread == 42, "the refresh period or end thread was lost");
    window = clock.Take();
    Expect(window.waits == 0 && window.frames == 0 && window.refresh_ns == kRefresh90 && window.end_thread == 42,
           "Take did not start a new window, or lost what carries over");

    // OpenComposite: the game waits for its next frame with this one begun and not ended. The pipelined wait's
    // frame is timed from the oldest wait, as the frames end.
    auto t = t0 + 1s;
    clock.Wait(t, false);
    clock.Begin();
    clock.Wait(t + 9ms, false);
    clock.End(t + 10ms, true, 42);
    clock.Begin();
    clock.End(t + 20ms, true, 42);
    window = clock.Take();
    Expect(window.waits == 2 && window.pipelined == 1, "a wait with a frame open was not counted as pipelined");
    Expect(window.frames == 2 && Near(window.cpu_max_ms, 11.0, 1e-6), "pipelined frames were not matched oldest first");

    // DCS: a wait on another thread returns while the last frame's end is running (its entry passed): not
    // pipelining.
    t = t0 + 2s;
    clock.Wait(t, false);
    clock.Begin();
    clock.End(t + 8ms, true, 7);
    clock.Wait(t + 9ms, false);
    window = clock.Take();
    Expect(window.pipelined == 0 && window.end_thread == 7, "a wait after the end's entry was counted as pipelined");
    clock.Begin();
    clock.End(t + 15ms, true, 7);
    clock.Take();

    // Turbo's fabricated waits are counted; frames without layers are not timed; a pause (over a second since the
    // last end, here twice) is no interval.
    t = t0 + 4s;
    clock.Wait(t, true);
    clock.Begin();
    clock.End(t + 1ms, false, 7);
    clock.Wait(t + 5s, false);
    clock.Begin();
    clock.End(t + 5s + 3ms, true, 7);
    window = clock.Take();
    Expect(window.fabricated == 1 && window.frames == 1 && Near(window.cpu_sum_ms, 3.0, 1e-6),
           "fabricated waits or unrendered frames were counted wrongly");
    Expect(window.intervals == 0, "a pause was counted as a frame interval");

    // A game that loses frames (waits without ending) never grows the clock past its bound.
    for (int i = 0; i < 100; ++i) clock.Wait(t0 + 10s + i * 1ms, false);
    clock.End(t0 + 10s + 200ms, true, 7);
    window = clock.Take();
    Expect(window.frames == 1 && window.cpu_max_ms < 200.0 - 95.0, "lost frames' waits were kept past the bound");
    clock.Reset();
    window = clock.Take();
    Expect(window.refresh_ns == 0 && window.end_thread == 0, "Reset kept the refresh period or end thread");
}

void TestJudgement() {
    PerformanceJudgement base;
    base.budget_ms = 11.11;
    base.frame_ms = 11.11;
    base.cpu_ms = 6.0;
    base.gpu_percent = 70.0;
    Expect(JudgePerformance(base) == PerformanceVerdict::kNone, "a game with room on both sides got a verdict");
    auto unknown = base;
    unknown.budget_ms = 0.0;
    unknown.gpu_percent = 100.0;
    Expect(JudgePerformance(unknown) == PerformanceVerdict::kNone, "a verdict without a budget");

    // GPU near full: GPU-bound, at the threshold and not below it.
    auto gpu = base;
    gpu.gpu_percent = kPerformanceGpuBoundPercent;
    Expect(JudgePerformance(gpu) == PerformanceVerdict::kGpuBound, "a GPU at the threshold was not GPU-bound");
    gpu.gpu_percent = kPerformanceGpuBoundPercent - 0.1;
    Expect(JudgePerformance(gpu) == PerformanceVerdict::kNone, "a GPU under the threshold was GPU-bound");

    // CPU-bound: below target, CPU over the budget, the GPU with room.
    auto cpu = base;
    cpu.frame_ms = 22.2;
    cpu.cpu_ms = 14.0;
    cpu.gpu_percent = 60.0;
    Expect(JudgePerformance(cpu) == PerformanceVerdict::kCpuBound, "a game below target on its CPU was not CPU-bound");
    auto at_target = cpu;
    at_target.frame_ms = base.budget_ms * kPerformanceBelowTarget;
    Expect(JudgePerformance(at_target) == PerformanceVerdict::kNone, "a game at its target was CPU-bound");
    at_target.frame_ms = base.budget_ms * kPerformanceBelowTarget + 0.01;
    Expect(JudgePerformance(at_target) == PerformanceVerdict::kCpuBound, "just below target was not CPU-bound");
    auto in_budget = cpu;
    in_budget.cpu_ms = base.budget_ms;
    Expect(JudgePerformance(in_budget) == PerformanceVerdict::kNone, "a CPU within its budget was CPU-bound");
    auto busy_gpu = cpu;
    busy_gpu.gpu_percent = kPerformanceGpuRoomPercent;
    Expect(JudgePerformance(busy_gpu) == PerformanceVerdict::kNone, "CPU-bound with a GPU without room");
    busy_gpu.gpu_percent = kPerformanceGpuRoomPercent - 0.1;
    Expect(JudgePerformance(busy_gpu) == PerformanceVerdict::kCpuBound, "a GPU just with room did not allow CPU-bound");
    auto no_gpu = cpu;
    no_gpu.gpu_percent.reset();
    Expect(JudgePerformance(no_gpu) == PerformanceVerdict::kNone, "CPU-bound without knowing the GPU");
    auto hidden = cpu;
    hidden.cpu_ms.reset();
    Expect(JudgePerformance(hidden) == PerformanceVerdict::kNone, "CPU-bound with the CPU time hidden");

    // Frame Generation generating: its late frames decide, and nothing else does.
    auto framegen = base;
    framegen.gpu_percent = 99.0;
    framegen.late_cpu_share = 0.0;
    framegen.late_gpu_share = kPerformanceLateShare - 0.001;
    Expect(JudgePerformance(framegen) == PerformanceVerdict::kNone, "few late frames made a verdict");
    framegen.late_gpu_share = kPerformanceLateShare;
    Expect(JudgePerformance(framegen) == PerformanceVerdict::kGpuBound, "late GPU frames were not GPU-bound");
    framegen.late_cpu_share = 0.10;
    Expect(JudgePerformance(framegen) == PerformanceVerdict::kCpuBound, "late CPU frames were not CPU-bound");
    framegen.late_gpu_share = 0.10;
    Expect(JudgePerformance(framegen) == PerformanceVerdict::kCpuBound, "a tie did not blame the CPU");
}

PerformanceTick Tick(PerformanceClock::time_point now, double cpu_ms, double interval_ms, std::uint64_t frames = 45) {
    PerformanceTick tick;
    tick.now = now;
    tick.frames.waits = frames;
    tick.frames.frames = frames;
    tick.frames.cpu_sum_ms = cpu_ms * static_cast<double>(frames);
    tick.frames.cpu_max_ms = cpu_ms;
    tick.frames.intervals = frames;
    tick.frames.interval_sum_ms = interval_ms * static_cast<double>(frames);
    tick.frames.refresh_ns = kRefresh90;
    tick.frames.end_thread = 9;
    tick.gpu_percent = 50.0;
    tick.gpu_game_percent = 40.0;
    tick.usage_bytes = 6ull << 30;
    tick.budget_bytes = 14ull << 30;
    return tick;
}

void TestModel() {
    const auto t0 = PerformanceClock::now();
    PerformanceModel model;
    model.Tick(Tick(t0, 5.0, 11.11));
    const PerformanceReading* reading = &model.Reading();
    Expect(Near(reading->budget_ms, 11.111, 0.001) && reading->cpu_state == PerformanceCpuState::kMeasured &&
               Near(*reading->cpu_ms, 5.0, 1e-9),
           "the first sample was not taken as it is");
    Expect(Near(*reading->CpuHeadroomPercent(), 55.0, 0.01) && Near(*reading->GpuHeadroomPercent(), 50.0, 1e-9),
           "headroom is not (budget - time) / budget");

    // Smoothing: a step reaches 1 - 1/e of the way in one time constant (two half-second samples).
    model.Tick(Tick(t0 + 500ms, 9.0, 11.11));
    model.Tick(Tick(t0 + 1000ms, 9.0, 11.11));
    const double expected = 5.0 + 4.0 * (1.0 - std::exp(-1.0 / kPerformanceSmoothingSeconds));
    Expect(Near(*reading->cpu_ms, expected, 1e-6), "the smoothing time constant is not one second");

    // Pipelining: a quarter of the waits returned with a frame open hides the CPU time; it comes back afresh.
    PerformanceModel pipelined;
    auto t = t0;
    for (int i = 0; i < 6; ++i, t += 500ms) {
        auto tick = Tick(t, 30.0, 11.11);
        tick.frames.pipelined = tick.frames.waits;
        pipelined.Tick(tick);
    }
    Expect(pipelined.Reading().cpu_state == PerformanceCpuState::kPipelined && !pipelined.Reading().cpu_ms,
           "pipelined frames' CPU time was shown");
    Expect(Contains(DescribePerformance(pipelined.Reading()).cpu, "--"), "a hidden CPU time was described");
    for (int i = 0; i < 8; ++i, t += 500ms) pipelined.Tick(Tick(t, 6.0, 11.11));
    Expect(pipelined.Reading().cpu_state == PerformanceCpuState::kMeasured && Near(*pipelined.Reading().cpu_ms, 6.0, 1e-9),
           "the CPU time did not come back afresh after pipelining stopped");

    // Turbo fabricating waits hides it too.
    PerformanceModel turbo;
    for (int i = 0; i < 4; ++i) {
        auto tick = Tick(t0 + i * 500ms, 3.0, 8.0);
        tick.frames.fabricated = tick.frames.waits / 2;
        turbo.Tick(tick);
    }
    Expect(turbo.Reading().cpu_state == PerformanceCpuState::kTurbo && !turbo.Reading().cpu_ms,
           "Turbo's fabricated waits did not hide the CPU time");

    // A paused game: after kPerformanceFramesStale without frames, nothing is known of them.
    PerformanceModel paused;
    paused.Tick(Tick(t0, 5.0, 11.11));
    PerformanceTick quiet = Tick(t0 + 1s, 0.0, 0.0, 0);
    paused.Tick(quiet);
    Expect(paused.Reading().cpu_ms.has_value(), "a second without frames dropped the CPU time");
    quiet.now = t0 + kPerformanceFramesStale + 1ms;
    paused.Tick(quiet);
    Expect(!paused.Reading().cpu_ms && paused.Reading().frame_ms == 0.0 &&
               paused.Reading().cpu_state == PerformanceCpuState::kWaiting,
           "a paused game kept its old CPU time");

    // Counters missing from a sample are unknown, not the last value.
    quiet.gpu_percent.reset();
    quiet.usage_bytes.reset();
    paused.Tick(quiet);
    Expect(!paused.Reading().gpu_percent && !paused.Reading().usage_bytes && !paused.Reading().gpu_unavailable,
           "missing counters kept their last value, or were called unavailable");
    quiet.gpu_unavailable = true;
    paused.Tick(quiet);
    Expect(paused.Reading().gpu_unavailable, "counters that failed were not called unavailable");

    // Frame Generation: the budget is the game's base target while generating, the refresh period otherwise.
    PerformanceModel framegen;
    auto fg_tick = Tick(t0, 9.0, 22.2);
    fg_tick.frames.refresh_ns = 2 * kRefresh90; // what the game's own waits might echo: never used under FG
    PerformanceFrameGen state;
    state.generating = true;
    state.refreshes = 2;
    state.refresh_ns = kRefresh90;
    state.grid_ns = kRefresh90;
    state.own_bytes = 700ull << 20;
    fg_tick.framegen = state;
    framegen.Tick(fg_tick);
    Expect(Near(framegen.Reading().budget_ms, 22.222, 0.001) && framegen.Reading().framegen_generating &&
               framegen.Reading().framegen_bytes == state.own_bytes,
           "the Frame Generation budget is not the refresh period times its refreshes per real frame");
    state.refreshes = 3; // Dynamic's current count
    fg_tick.framegen = state;
    framegen.Tick(fg_tick);
    Expect(Near(framegen.Reading().budget_ms, 33.333, 0.001), "Dynamic's current target was not the budget");
    state.grid_ns = 2 * kRefresh90; // the runtime holds frames to every second refresh
    state.refreshes = 2;
    fg_tick.framegen = state;
    framegen.Tick(fg_tick);
    Expect(Near(framegen.Reading().budget_ms, 44.444, 0.001), "the runtime's halved grid was not the budget's base");
    state.generating = false;
    fg_tick.framegen = state;
    framegen.Tick(fg_tick);
    Expect(Near(framegen.Reading().budget_ms, 11.111, 0.001), "Frame Generation passing frames through kept its target");
    // Video memory from Frame Generation's own sample when the sampler has no adapter.
    fg_tick.usage_bytes.reset();
    fg_tick.budget_bytes.reset();
    state.usage_bytes = 9ull << 30;
    state.budget_bytes = 8ull << 30;
    fg_tick.framegen = state;
    framegen.Tick(fg_tick);
    Expect(framegen.Reading().usage_bytes == (9ull << 30) && DescribePerformance(framegen.Reading()).vram_over,
           "Frame Generation's video memory sample was not used, or over budget was not marked");

    // Verdicts show only once held for kPerformanceVerdictHold, and clear the same way.
    PerformanceModel held;
    t = t0;
    auto gpu_bound = Tick(t, 6.0, 11.11);
    gpu_bound.gpu_percent = 99.0;
    held.Tick(gpu_bound);
    Expect(held.Reading().verdict == PerformanceVerdict::kNone, "a verdict showed at once");
    for (; t < t0 + kPerformanceVerdictHold; t += 500ms) {
        gpu_bound.now = t;
        held.Tick(gpu_bound);
    }
    Expect(held.Reading().verdict == PerformanceVerdict::kNone, "a verdict showed before it held");
    gpu_bound.now = t0 + kPerformanceVerdictHold;
    held.Tick(gpu_bound);
    Expect(held.Reading().verdict == PerformanceVerdict::kGpuBound, "a held verdict did not show");
    auto room = gpu_bound;
    room.gpu_percent = 10.0;
    for (int i = 1; i <= 2; ++i) {
        room.now = gpu_bound.now + i * 500ms;
        held.Tick(room);
    }
    Expect(held.Reading().verdict == PerformanceVerdict::kGpuBound, "a verdict cleared before the change held");
    for (int i = 3; i <= 6; ++i) {
        room.now = gpu_bound.now + i * 500ms;
        held.Tick(room);
    }
    Expect(held.Reading().verdict == PerformanceVerdict::kNone, "a verdict did not clear");

    // Frame Generation's late frames decide while it generates.
    PerformanceModel late;
    PerformanceFrameGen totals = state;
    totals.generating = true;
    totals.refreshes = 2;
    totals.grid_ns = kRefresh90;
    totals.usage_bytes.reset();
    for (int i = 0; i < 8; ++i) {
        auto tick = Tick(t0 + i * 500ms, 6.0, 22.2);
        tick.gpu_percent = 99.0; // would be GPU-bound outside Frame Generation
        totals.app_frames += 22;
        totals.late_cpu += 4;
        tick.framegen = totals;
        late.Tick(tick);
    }
    Expect(late.Reading().verdict == PerformanceVerdict::kCpuBound,
           "Frame Generation's late CPU frames did not decide the verdict");
}

void TestText() {
    Expect(PerformanceHeadroomText(32.4) == "+32%" && PerformanceHeadroomText(-20.4) == "\xE2\x88\x92" "20%" &&
               PerformanceHeadroomText(-0.4) == "+0%" && PerformanceHeadroomText(100.0) == "+100%" &&
               PerformanceHeadroomText(-5000.0) == "\xE2\x88\x92" "999%" && PerformanceHeadroomText(150.0) == "+100%",
           "headroom text: " + PerformanceHeadroomText(-20.4));
    Expect(PerformanceMemoryText(612ull << 20) == "612 MB" && PerformanceMemoryText((1ull << 30) - (1ull << 19)) == "1.0 GB" &&
               PerformanceMemoryText(static_cast<std::uint64_t>(9.84 * (1ull << 30))) == "9.8 GB",
           "memory text: " + PerformanceMemoryText((1ull << 30) - (1ull << 19)));

    PerformanceReading reading;
    PerformanceOsdText text = DescribePerformance(reading);
    Expect(text.target.empty() && text.cpu == "CPU --" && text.cpu_detail == "waiting for frames" && text.gpu == "GPU --" &&
               text.gpu_detail == "reading counters" && text.vram == "VRAM --" && text.verdict.empty(),
           "an empty reading's text");
    reading.gpu_unavailable = true;
    Expect(DescribePerformance(reading).gpu_detail == "no GPU counters", "unavailable GPU counters were not said");
    reading.gpu_unavailable = false;
    reading.budget_ms = 1000.0 / 90.0;
    reading.cpu_state = PerformanceCpuState::kMeasured;
    reading.cpu_ms = 7.555;
    reading.thread_busy_percent = 84.2;
    reading.gpu_percent = 82.0;
    reading.gpu_game_percent = 70.4;
    reading.usage_bytes = static_cast<std::uint64_t>(9.8 * (1ull << 30));
    reading.budget_bytes = static_cast<std::uint64_t>(14.2 * (1ull << 30));
    reading.framegen_bytes = 612ull << 20;
    text = DescribePerformance(reading);
    Expect(text.target == "90 fps target" && text.cpu == "CPU +32%" && text.cpu_detail == "7.6 ms, thread 84%" &&
               text.gpu == "GPU +18%" && text.gpu_detail == "82% busy, game 70%" && text.vram == "VRAM 9.8 GB" &&
               text.vram_detail == "of 14.2 GB, FG 612 MB" && !text.vram_over,
           "a full reading's text: " + text.cpu + " | " + text.cpu_detail + " | " + text.gpu_detail + " | " +
               text.vram_detail);
    Expect(text.compact_cpu == "CPU +32%" && text.compact_gpu == "GPU +18%" && text.compact_vram == "VRAM 9.8 GB",
           "compact text");
    reading.verdict = PerformanceVerdict::kGpuBound;
    text = DescribePerformance(reading);
    Expect(text.verdict == "GPU-BOUND" && text.gpu_bound && text.compact_gpu == "GPU-BOUND" && text.compact_cpu == "CPU +32%",
           "a GPU-bound verdict's text");
    reading.verdict = PerformanceVerdict::kCpuBound;
    reading.cpu_ms = 14.0;
    text = DescribePerformance(reading);
    Expect(text.verdict == "CPU-BOUND" && text.cpu == "CPU \xE2\x88\x92" "26%" && text.compact_cpu == "CPU-BOUND",
           "a CPU-bound verdict's text: " + text.cpu);
    reading.cpu_state = PerformanceCpuState::kTurbo;
    reading.cpu_ms.reset();
    Expect(DescribePerformance(reading).cpu_detail == "Turbo paces the game", "Turbo's hidden CPU time");
    reading.cpu_state = PerformanceCpuState::kPipelined;
    Expect(DescribePerformance(reading).cpu_detail == "pipelined frames", "pipelining's hidden CPU time");
}

void TestGpuEngines() {
    const auto parsed = ParseGpuEngineInstance(L"pid_1234_luid_0x00000001_0x0000D1A5_phys_0_eng_3_engtype_3D");
    Expect(parsed && parsed->pid == 1234 && parsed->luid == (1ull << 32 | 0xD1A5) && parsed->phys == 0 &&
               parsed->engine == 3 && parsed->type == L"3D",
           "a GPU Engine instance did not parse");
    const auto other = ParseGpuEngineInstance(L"pid_9_luid_0x00000000_0x0000abcd_phys_1_eng_12_engtype_VideoDecode");
    Expect(other && other->luid == 0xabcd && other->phys == 1 && other->engine == 12 && other->type == L"VideoDecode",
           "a lowercase LUID or a longer engine type did not parse");
    const auto empty_type = ParseGpuEngineInstance(L"pid_9_luid_0x0_0x1_phys_0_eng_0_engtype_");
    Expect(empty_type && empty_type->type.empty(), "an instance without an engine type did not parse");
    for (const wchar_t* bad : {L"", L"_Total", L"pid__luid_0x0_0x1_phys_0_eng_0_engtype_3D",
                               L"pid_1_luid_00_0x1_phys_0_eng_0_engtype_3D", L"pid_1_luid_0x0_0x1_phys_0_eng_0"}) {
        Expect(!ParseGpuEngineInstance(bad), "a malformed instance parsed");
    }

    constexpr std::uint64_t kGame = 0x1000, kOther = 0x2000;
    const auto instance = [](std::uint32_t pid, std::uint64_t luid, std::uint32_t engine, const wchar_t* type) {
        GpuEngineInstance result;
        result.pid = pid;
        result.luid = luid;
        result.engine = engine;
        result.type = type;
        return result;
    };
    const std::vector<std::pair<GpuEngineInstance, double>> samples{
        {instance(100, kGame, 0, L"3D"), 55.0},   // the game
        {instance(100, kGame, 0, L"3D"), 5.0},    // the game again (a second device)
        {instance(200, kGame, 0, L"3D"), 25.0},   // the runtime's compositor
        {instance(300, kGame, 0, L"3D"), 30.0},   // another app: the sum passes 100
        {instance(100, kGame, 2, L"Copy"), 90.0}, // not 3D
        {instance(400, kOther, 0, L"3D"), 80.0},  // another adapter
        {instance(200, kGame, 1, L"3D"), 10.0},   // another 3D engine
    };
    const auto game_adapter = AggregateGpuEngines(samples, 100, kGame);
    Expect(game_adapter.total && *game_adapter.total == 100.0 && game_adapter.game && *game_adapter.game == 60.0 &&
               game_adapter.instances == 5,
           "the adapter's busiest 3D engine or the game's share was wrong");
    const auto every_adapter = AggregateGpuEngines(samples, 400, std::nullopt);
    Expect(every_adapter.instances == 6 && *every_adapter.game == 80.0, "an unknown adapter did not take every one");
    const auto idle = AggregateGpuEngines(samples, 999, kGame);
    Expect(idle.game && *idle.game == 0.0, "a process without 3D work was not 0");
    const auto none = AggregateGpuEngines({}, 1, kGame);
    Expect(!none.total && !none.game && none.instances == 0, "no instances gave a value");
}

void TestSummary() {
    PerformanceSummary summary;
    PerformanceReading reading;
    reading.budget_ms = 11.11;
    reading.frame_ms = 11.2;
    reading.cpu_state = PerformanceCpuState::kMeasured;
    reading.cpu_ms = 7.5;
    reading.gpu_percent = 80.0;
    reading.gpu_game_percent = 70.0;
    reading.thread_busy_percent = 84.0;
    reading.usage_bytes = 9ull << 30;
    reading.budget_bytes = 8ull << 30;
    reading.framegen_bytes = 600ull << 20;
    reading.verdict = PerformanceVerdict::kGpuBound;
    summary.Add(reading, {1.5});
    reading.cpu_state = PerformanceCpuState::kPipelined;
    reading.cpu_ms.reset();
    summary.Add(reading, {0.5});
    Expect(summary.Samples() == 2, "the summary did not count its samples");
    const std::string line = summary.Take(1.0, 0.25, "12 3D instances of 40");
    for (const char* part : {"Performance summary (1.0 s, 2 samples)", "budget avg/max=11.11/11.11 ms", "90.0 fps target",
                             "cpu avg/max=7.50/7.50 ms", "headroom +32%", "cpuHidden pipelined/turbo=1/0",
                             "gpu3d avg/max=80/80%", "gameGpu3d avg/max=70/70%", "vram=9.0 GB", "(over budget)",
                             "frameGenerationVram=600 MB", "verdict cpu/gpu=0/2", "gpuCounters=12 3D instances of 40",
                             "sampler wall avg/max=1.00/1.50 ms", "samplerCpu=0.25% of a core"}) {
        Expect(Contains(line, part), std::string("the summary lacks ") + part + ": " + line);
    }
    Expect(summary.Samples() == 0, "Take did not start a new window");
}

// The sampler thread's life: started when wanted, sampling the game's frames off the frame thread, idle when not
// wanted, joined at the session's end. Without an adapter it reads no video memory; GPU utilization comes from
// this machine's counters (CPU only: the kernel's statistics, no GPU work).
void TestMonitor(std::optional<std::uint64_t> luid, bool print, std::chrono::milliseconds run = 1600ms) {
    const auto log_path = std::filesystem::temp_directory_path() / "vectorxr-performance-tests" / "performance.log";
    std::filesystem::path written;
    {
        Logger logger;
        logger.Initialize(log_path);
        written = logger.ActiveLogPath();
        PerformanceMonitor monitor;
        Expect(!monitor.Wanted(), "a new monitor was wanted");
        monitor.SetWanted(true);
        Expect(monitor.Wanted(), "SetWanted did not take");
        // No session: nothing samples.
        std::this_thread::sleep_for(700ms);
        Expect(monitor.Reading().budget_ms == 0.0, "a monitor without a session sampled");
        std::atomic<bool> framegen_called{false};
        monitor.BeginSession(luid, [&framegen_called] {
            framegen_called = true;
            return std::optional<PerformanceFrameGen>{};
        }, &logger);
        Expect(!monitor.Wanted(), "a new session kept the last one's want");
        monitor.SetWanted(true);
        // A 90 Hz game: 4 ms of CPU per frame. Spun, not slept: a sleep may take a whole 15.6 ms clock tick.
        const auto spin = [](PerformanceClock::time_point until) { while (PerformanceClock::now() < until) {} };
        const auto until = PerformanceClock::now() + run;
        while (PerformanceClock::now() < until) {
            const auto start = PerformanceClock::now();
            monitor.Frames().RuntimePeriod(kRefresh90, start);
            monitor.Frames().Wait(start, false);
            monitor.Frames().Begin();
            spin(start + 4ms);
            monitor.Frames().End(PerformanceClock::now(), true, GetCurrentThreadId());
            spin(start + 11ms);
        }
        const PerformanceReading reading = monitor.Reading();
        Expect(Near(reading.budget_ms, 11.111, 0.001), "the sampler did not read the refresh period");
        Expect(reading.cpu_state == PerformanceCpuState::kMeasured && reading.cpu_ms && *reading.cpu_ms >= 4.0 &&
                   *reading.cpu_ms < 9.0,
               "the sampler did not measure the game's CPU time");
        Expect(reading.frame_ms > 10.0 && reading.frame_ms < 20.0, "the sampler did not measure the frame interval");
        Expect(reading.thread_busy_percent.has_value(), "the end-frame thread's busy share was not read");
        Expect(framegen_called, "Frame Generation's figures were not asked for");
        Expect(reading.usage_bytes.has_value() == luid.has_value(), "video memory was read without an adapter, or not with one");
        if (print) {
            const PerformanceOsdText text = DescribePerformance(reading);
            std::cout << "live: " << text.target << " | " << text.cpu << " (" << text.cpu_detail << ") | " << text.gpu
                      << " (" << text.gpu_detail << ") | " << text.vram << " (" << text.vram_detail << ")\n";
        }
        // Not wanted: it idles and forgets.
        monitor.SetWanted(false);
        std::this_thread::sleep_for(800ms);
        Expect(monitor.Reading().budget_ms == 0.0, "an unwanted sampler kept its reading");
        monitor.SetWanted(true);
        monitor.EndSession();
        Expect(!monitor.Wanted() && monitor.Reading().budget_ms == 0.0, "the session's end left the sampler wanted");
        // A second session starts afresh.
        monitor.BeginSession(std::nullopt, {}, &logger);
        monitor.SetWanted(true);
        std::this_thread::sleep_for(600ms);
        monitor.EndSession();
    }
    std::ifstream file(written);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    Expect(Contains(text, "Performance summary ("), "the sampler logged no summary");
    if (print) {
        std::size_t at = 0;
        while ((at = text.find("Performance", at)) != std::string::npos) {
            const std::size_t end = text.find('\n', at);
            std::cout << text.substr(at, end - at) << '\n';
            at = end;
        }
    }
    file.close();
    std::error_code ignored;
    std::filesystem::remove(written, ignored);
}

// What the game's frame thread pays per frame while the row is shown: a wait, a begin and an end (three short
// locks and clock reads), uncontended.
void PrintFrameCost() {
    PerformanceFrameClock clock;
    constexpr int kFrames = 200000;
    const auto start = PerformanceClock::now();
    for (int i = 0; i < kFrames; ++i) {
        clock.Wait(PerformanceClock::now(), false);
        clock.Begin();
        clock.End(PerformanceClock::now(), true, 1);
        if (i % 1000 == 0) clock.Take();
    }
    const double ns = std::chrono::duration<double, std::nano>(PerformanceClock::now() - start).count() / kFrames;
    std::cout << "frame thread cost: " << ns << " ns per frame (wait + begin + end)\n";
}

// The first hardware adapter's LUID, as the session's would be.
std::optional<std::uint64_t> FirstAdapterLuid() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf())); ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            return static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32 |
                   desc.AdapterLuid.LowPart;
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    TestRefreshFilter();
    TestFrameClock();
    TestJudgement();
    TestModel();
    TestText();
    TestGpuEngines();
    TestSummary();
    const bool live = argc > 1 && std::string(argv[1]) == "live";
    // live [seconds]: how long the game's frames run, for the sampler's steady cost in its summaries.
    const auto run = std::chrono::milliseconds(live && argc > 2 ? std::max(2, std::atoi(argv[2])) * 1000 : 1600);
    if (live) PrintFrameCost();
    TestMonitor(live ? FirstAdapterLuid() : std::nullopt, live, run);
    std::cout << "Performance row: refresh filter, frame clock, verdicts, smoothing, text, GPU engine counters, "
                 "summary and sampler lifecycle passed.\n";
    return 0;
}
