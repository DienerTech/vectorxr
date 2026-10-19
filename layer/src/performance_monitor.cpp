#include "depthxr/performance_monitor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>

// The performance row's portable parts (performance_monitor.h): the frame clock, smoothing, the verdict and the
// text. The sampler thread and its counters are in performance_sampler.cpp.

namespace depthxr {
namespace {

double Milliseconds(PerformanceClock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// `value` with `decimals` decimals, in the layer's own (C) locale.
std::string Fixed(double value, int decimals) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string Percent(double value) {
    return std::to_string(std::lround(std::clamp(value, 0.0, 100.0))) + "%";
}

} // namespace

void RefreshPeriodFilter::Note(std::int64_t period_ns, PerformanceClock::time_point now) {
    if (period_ns <= 0) {
        return;
    }
    // About the refresh period or shorter: the refresh period, at once (no runtime reports less than the refresh
    // period it holds frames to).
    if (period_ns_ <= 0 || period_ns <= period_ns_ + period_ns_ / 20) {
        period_ns_ = period_ns;
        candidate_.reset();
        return;
    }
    // About `from` to kMaxMultiple times `base`.
    const auto multiple = [](std::int64_t longer, std::int64_t base, std::uint32_t from) {
        for (std::uint32_t refreshes = from; refreshes <= kMaxMultiple; ++refreshes) {
            const std::int64_t held = base * static_cast<std::int64_t>(refreshes);
            if (std::llabs(longer - held) <= held / 20) {
                return true;
            }
        }
        return false;
    };
    if (candidate_ && !multiple(period_ns, candidate_->first, 1)) {
        candidate_.reset();
    }
    if (multiple(period_ns, period_ns_, 2)) {
        return;
    }
    if (!candidate_) {
        candidate_ = std::make_pair(period_ns, now);
    } else if (now - candidate_->second >= kChangeAfter) {
        period_ns_ = candidate_->first;
        candidate_.reset();
    }
}

void RefreshPeriodFilter::Reset() {
    period_ns_ = 0;
    candidate_.reset();
}

void PerformanceFrameClock::Wait(PerformanceClock::time_point returned, bool fabricated) {
    std::scoped_lock lock(mutex_);
    ++window_.waits;
    if (open_) {
        ++window_.pipelined;
    }
    if (fabricated) {
        ++window_.fabricated;
    }
    if (waits_.size() >= kMaxOpenWaits) {
        waits_.pop_front();
    }
    waits_.push_back(returned);
}

void PerformanceFrameClock::Begin(std::uint32_t thread) {
    std::scoped_lock lock(mutex_);
    if (open_ && thread == begin_thread_ && !waits_.empty()) {
        // Decided at the end: MSFS 2024 also begins a frame before its
        // other thread ends the last, and none of its frames is discarded.
        discard_before_ = waits_.back();
    }
    open_ = true;
    begin_thread_ = thread;
}

void PerformanceFrameClock::End(PerformanceClock::time_point entered, bool rendered, std::uint32_t thread) {
    std::scoped_lock lock(mutex_);
    // Closed at the entry: a wait the game makes on another thread while this end runs (DCS) is not pipelining.
    open_ = false;
    window_.end_thread = thread;
    if (discard_before_ && thread == begin_thread_) {
        // The thread that begun over its open frames ends: those frames were
        // discarded, and their waits would time every later end a frame late.
        while (waits_.size() > 1 && waits_.front() < *discard_before_) {
            waits_.pop_front();
        }
    }
    discard_before_.reset();
    if (!waits_.empty()) {
        // The oldest wait is the frame ending, as Frame Generation's presenter matches them.
        const PerformanceClock::time_point wait = waits_.front();
        waits_.pop_front();
        if (rendered && entered >= wait) {
            const double ms = Milliseconds(entered - wait);
            ++window_.frames;
            window_.cpu_sum_ms += ms;
            window_.cpu_max_ms = std::max(window_.cpu_max_ms, ms);
        }
    }
    if (last_end_ && entered > *last_end_ && entered - *last_end_ <= kPause) {
        ++window_.intervals;
        window_.interval_sum_ms += Milliseconds(entered - *last_end_);
    }
    last_end_ = entered;
}

void PerformanceFrameClock::RuntimePeriod(std::int64_t period_ns, PerformanceClock::time_point now) {
    std::scoped_lock lock(mutex_);
    refresh_.Note(period_ns, now);
}

PerformanceFrameClock::Window PerformanceFrameClock::Take() {
    std::scoped_lock lock(mutex_);
    Window taken = window_;
    taken.refresh_ns = refresh_.PeriodNs();
    window_ = Window{};
    window_.end_thread = taken.end_thread;
    return taken;
}

void PerformanceFrameClock::Reset() {
    std::scoped_lock lock(mutex_);
    waits_.clear();
    open_ = false;
    begin_thread_ = 0;
    discard_before_.reset();
    last_end_.reset();
    refresh_.Reset();
    window_ = Window{};
}

const char* ToString(PerformanceVerdict verdict) {
    switch (verdict) {
    case PerformanceVerdict::kCpuBound:
        return "cpu-bound";
    case PerformanceVerdict::kGpuBound:
        return "gpu-bound";
    default:
        return "none";
    }
}

const char* ToString(PerformanceCpuState state) {
    switch (state) {
    case PerformanceCpuState::kMeasured:
        return "measured";
    case PerformanceCpuState::kPipelined:
        return "pipelined";
    case PerformanceCpuState::kTurbo:
        return "turbo";
    default:
        return "waiting";
    }
}

std::optional<double> PerformanceReading::CpuHeadroomPercent() const {
    if (!cpu_ms || budget_ms <= 0.0) {
        return std::nullopt;
    }
    return (budget_ms - *cpu_ms) / budget_ms * 100.0;
}

std::optional<double> PerformanceReading::GpuHeadroomPercent() const {
    if (!gpu_percent) {
        return std::nullopt;
    }
    return 100.0 - std::clamp(*gpu_percent, 0.0, 100.0);
}

PerformanceVerdict JudgePerformance(const PerformanceJudgement& judgement) {
    if (judgement.budget_ms <= 0.0) {
        return PerformanceVerdict::kNone;
    }
    if (judgement.late_cpu_share || judgement.late_gpu_share) {
        const double cpu = judgement.late_cpu_share.value_or(0.0);
        const double gpu = judgement.late_gpu_share.value_or(0.0);
        if (cpu + gpu < kPerformanceLateShare) {
            return PerformanceVerdict::kNone;
        }
        return gpu > cpu ? PerformanceVerdict::kGpuBound : PerformanceVerdict::kCpuBound;
    }
    if (judgement.gpu_percent && *judgement.gpu_percent >= kPerformanceGpuBoundPercent) {
        return PerformanceVerdict::kGpuBound;
    }
    const bool below_target = judgement.frame_ms > judgement.budget_ms * kPerformanceBelowTarget;
    if (below_target && judgement.cpu_ms && *judgement.cpu_ms > judgement.budget_ms && judgement.gpu_percent &&
        *judgement.gpu_percent < kPerformanceGpuRoomPercent) {
        return PerformanceVerdict::kCpuBound;
    }
    return PerformanceVerdict::kNone;
}

void PerformanceModel::Smoothed::Add(double sample, double alpha) {
    value = value ? *value + alpha * (sample - *value) : sample;
}

void PerformanceModel::Reset() {
    *this = PerformanceModel{};
}

void PerformanceModel::Tick(const PerformanceTick& tick) {
    const double seconds = last_tick_ ? std::clamp(std::chrono::duration<double>(tick.now - *last_tick_).count(), 0.01, 5.0)
                                      : std::chrono::duration<double>(kPerformanceSamplePeriod).count();
    last_tick_ = tick.now;
    const double alpha = 1.0 - std::exp(-seconds / kPerformanceSmoothingSeconds);
    const PerformanceFrameClock::Window& frames = tick.frames;

    // The budget: the refresh period, or under Frame Generation the game's base frame target.
    std::int64_t budget_ns = frames.refresh_ns;
    reading_.framegen_generating = false;
    reading_.framegen_refreshes = 1;
    reading_.framegen_bytes = 0;
    if (tick.framegen) {
        const PerformanceFrameGen& framegen = *tick.framegen;
        const std::int64_t refresh = framegen.refresh_ns > 0 ? framegen.refresh_ns : frames.refresh_ns;
        budget_ns = refresh;
        if (framegen.generating && refresh > 0) {
            const std::uint32_t refreshes = std::max<std::uint32_t>(1, framegen.refreshes);
            budget_ns = (framegen.grid_ns > 0 ? framegen.grid_ns : refresh) * static_cast<std::int64_t>(refreshes);
            reading_.framegen_generating = true;
            reading_.framegen_refreshes = refreshes;
        }
        reading_.framegen_bytes = framegen.own_bytes;
    }
    reading_.budget_ms = budget_ns > 0 ? static_cast<double>(budget_ns) / 1e6 : 0.0;

    // The game's frames. A window with a pipelined or fabricated wait has a wait inside a frame's time, or none
    // the runtime paced: its CPU time is left out.
    if (frames.waits > 0) {
        pipelined_.Add(static_cast<double>(frames.pipelined) / static_cast<double>(frames.waits), alpha);
        fabricated_.Add(static_cast<double>(frames.fabricated) / static_cast<double>(frames.waits), alpha);
    }
    if (frames.intervals > 0) {
        frame_.Add(frames.interval_sum_ms / static_cast<double>(frames.intervals), alpha);
    }
    if (frames.waits > 0 || frames.frames > 0 || frames.intervals > 0) {
        last_frames_ = tick.now;
    } else if (!last_frames_ || tick.now - *last_frames_ >= kPerformanceFramesStale) {
        // Paused, loading or stopped: nothing is known of the game's frames until they come back.
        cpu_.Clear();
        frame_.Clear();
        pipelined_.Clear();
        fabricated_.Clear();
    }
    PerformanceCpuState state = PerformanceCpuState::kWaiting;
    if (fabricated_.value && *fabricated_.value >= kPerformanceFabricatedShare) {
        state = PerformanceCpuState::kTurbo;
    } else if (pipelined_.value && *pipelined_.value >= kPerformancePipelinedShare) {
        state = PerformanceCpuState::kPipelined;
    }
    if (state != PerformanceCpuState::kWaiting) {
        // Hidden: once trusted again, the time starts afresh rather than from before.
        cpu_.Clear();
    } else if (frames.frames > 0 && frames.pipelined == 0 && frames.fabricated == 0) {
        cpu_.Add(frames.cpu_sum_ms / static_cast<double>(frames.frames), alpha);
    }
    if (state == PerformanceCpuState::kWaiting && cpu_.value) {
        state = PerformanceCpuState::kMeasured;
    }
    reading_.cpu_state = state;
    reading_.cpu_ms = state == PerformanceCpuState::kMeasured ? cpu_.value : std::nullopt;
    reading_.frame_ms = frame_.value.value_or(0.0);

    // Counters: a sample without one is unknown, not the last value.
    const auto smooth = [alpha](Smoothed& smoothed, const std::optional<double>& sample) {
        if (sample) {
            smoothed.Add(std::clamp(*sample, 0.0, 100.0), alpha);
        } else {
            smoothed.Clear();
        }
        return smoothed.value;
    };
    reading_.gpu_percent = smooth(gpu_, tick.gpu_percent);
    reading_.gpu_game_percent = smooth(gpu_game_, tick.gpu_game_percent);
    reading_.gpu_unavailable = tick.gpu_unavailable && !reading_.gpu_percent;
    reading_.thread_busy_percent = smooth(thread_, tick.thread_busy_percent);
    reading_.usage_bytes = tick.usage_bytes;
    reading_.budget_bytes = tick.budget_bytes;
    if (!tick.usage_bytes && tick.framegen && tick.framegen->usage_bytes) {
        reading_.usage_bytes = tick.framegen->usage_bytes;
        reading_.budget_bytes = tick.framegen->budget_bytes;
    }

    // Frame Generation generating: which side its late real frames blame, over the frames since the last sample.
    PerformanceJudgement judgement;
    const bool generating = tick.framegen && tick.framegen->generating;
    if (generating && last_framegen_ && tick.framegen->app_frames > last_framegen_->app_frames &&
        tick.framegen->late_cpu >= last_framegen_->late_cpu && tick.framegen->late_gpu >= last_framegen_->late_gpu) {
        const double real = static_cast<double>(tick.framegen->app_frames - last_framegen_->app_frames);
        late_cpu_.Add(std::min(1.0, static_cast<double>(tick.framegen->late_cpu - last_framegen_->late_cpu) / real),
                      alpha);
        late_gpu_.Add(std::min(1.0, static_cast<double>(tick.framegen->late_gpu - last_framegen_->late_gpu) / real),
                      alpha);
    } else if (!generating) {
        late_cpu_.Clear();
        late_gpu_.Clear();
    }
    last_framegen_ = tick.framegen;
    if (generating) {
        // Its late frames alone judge it, from its first sample: nothing is late before they are counted.
        judgement.late_cpu_share = late_cpu_.value.value_or(0.0);
        judgement.late_gpu_share = late_gpu_.value.value_or(0.0);
    }
    judgement.budget_ms = reading_.budget_ms;
    judgement.frame_ms = reading_.frame_ms;
    judgement.cpu_ms = reading_.cpu_ms;
    judgement.gpu_percent = reading_.gpu_percent;
    const PerformanceVerdict judged = JudgePerformance(judgement);
    if (judged == reading_.verdict) {
        pending_ = judged;
    } else if (judged != pending_) {
        pending_ = judged;
        pending_since_ = tick.now;
    } else if (tick.now - pending_since_ >= kPerformanceVerdictHold) {
        reading_.verdict = judged;
    }
}

std::string PerformanceHeadroomText(double percent) {
    const long rounded = std::lround(std::clamp(percent, -999.0, 100.0));
    // A minus sign, not a hyphen: it reads as one in the OSD's Segoe UI.
    return rounded < 0 ? "\xE2\x88\x92" + std::to_string(-rounded) + "%" : "+" + std::to_string(rounded) + "%";
}

std::string PerformanceMemoryText(std::uint64_t bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    const double value = static_cast<double>(bytes);
    // The unit follows the rounded figure: just under 1 GiB reads "1.0 GB", never "1024 MB".
    if (std::lround(value / kMiB) < 1024) {
        return std::to_string(std::lround(value / kMiB)) + " MB";
    }
    return Fixed(value / kGiB, 1) + " GB";
}

PerformanceOsdText DescribePerformance(const PerformanceReading& reading) {
    PerformanceOsdText text;
    if (reading.budget_ms > 0.0) {
        text.target = std::to_string(std::lround(1000.0 / reading.budget_ms)) + " fps target";
    }
    text.cpu_bound = reading.verdict == PerformanceVerdict::kCpuBound;
    text.gpu_bound = reading.verdict == PerformanceVerdict::kGpuBound;
    text.verdict = text.cpu_bound ? "CPU-BOUND" : text.gpu_bound ? "GPU-BOUND" : "";

    const std::optional<double> cpu = reading.CpuHeadroomPercent();
    text.cpu = "CPU " + (cpu ? PerformanceHeadroomText(*cpu) : std::string("--"));
    switch (reading.cpu_state) {
    case PerformanceCpuState::kMeasured:
        text.cpu_detail = Fixed(reading.cpu_ms.value_or(0.0), 1) + " ms";
        if (reading.thread_busy_percent) {
            text.cpu_detail += ", thread " + Percent(*reading.thread_busy_percent);
        }
        break;
    case PerformanceCpuState::kPipelined:
        text.cpu_detail = "pipelined frames";
        break;
    case PerformanceCpuState::kTurbo:
        text.cpu_detail = "Turbo paces the game";
        break;
    default:
        text.cpu_detail = "waiting for frames";
        break;
    }

    const std::optional<double> gpu = reading.GpuHeadroomPercent();
    text.gpu = "GPU " + (gpu ? PerformanceHeadroomText(*gpu) : std::string("--"));
    if (reading.gpu_percent) {
        text.gpu_detail = Percent(*reading.gpu_percent) + " busy";
        if (reading.gpu_game_percent) {
            text.gpu_detail += ", game " + Percent(*reading.gpu_game_percent);
        }
    } else {
        // Before the counters' second collection there is no rate yet.
        text.gpu_detail = reading.gpu_unavailable ? "no GPU counters" : "reading counters";
    }

    if (reading.usage_bytes) {
        text.vram = "VRAM " + PerformanceMemoryText(*reading.usage_bytes);
        text.vram_detail = reading.budget_bytes ? "of " + PerformanceMemoryText(*reading.budget_bytes) : "";
        text.vram_over = reading.budget_bytes && *reading.usage_bytes > *reading.budget_bytes;
    } else {
        text.vram = "VRAM --";
        text.vram_detail = "unknown";
    }
    if (reading.framegen_bytes > 0) {
        text.vram_detail += (text.vram_detail.empty() ? "" : ", ") + std::string("FG ") +
                            PerformanceMemoryText(reading.framegen_bytes);
    }

    text.compact_cpu = text.cpu_bound ? "CPU-BOUND" : text.cpu;
    text.compact_gpu = text.gpu_bound ? "GPU-BOUND" : text.gpu;
    text.compact_vram = text.vram;
    return text;
}

std::optional<GpuEngineInstance> ParseGpuEngineInstance(std::wstring_view name) {
    const auto take = [&name](std::wstring_view prefix) {
        if (name.substr(0, prefix.size()) != prefix) {
            return false;
        }
        name.remove_prefix(prefix.size());
        return true;
    };
    const auto decimal = [&name](std::uint32_t& out) {
        std::uint64_t value = 0;
        std::size_t digits = 0;
        while (digits < name.size() && name[digits] >= L'0' && name[digits] <= L'9' && digits < 10) {
            value = value * 10 + static_cast<std::uint64_t>(name[digits] - L'0');
            ++digits;
        }
        if (!digits || value > 0xffffffffull) {
            return false;
        }
        out = static_cast<std::uint32_t>(value);
        name.remove_prefix(digits);
        return true;
    };
    const auto hex = [&name, &take](std::uint32_t& out) {
        if (!take(L"0x") && !take(L"0X")) {
            return false;
        }
        std::uint32_t value = 0;
        std::size_t digits = 0;
        for (; digits < name.size() && digits < 8; ++digits) {
            const wchar_t c = name[digits];
            const int nibble = c >= L'0' && c <= L'9' ? c - L'0'
                               : c >= L'a' && c <= L'f' ? c - L'a' + 10
                               : c >= L'A' && c <= L'F' ? c - L'A' + 10
                                                        : -1;
            if (nibble < 0) {
                break;
            }
            value = value << 4 | static_cast<std::uint32_t>(nibble);
        }
        if (!digits) {
            return false;
        }
        out = value;
        name.remove_prefix(digits);
        return true;
    };
    GpuEngineInstance instance;
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    if (!take(L"pid_") || !decimal(instance.pid) || !take(L"_luid_") || !hex(high) || !take(L"_") || !hex(low) ||
        !take(L"_phys_") || !decimal(instance.phys) || !take(L"_eng_") || !decimal(instance.engine) ||
        !take(L"_engtype_")) {
        return std::nullopt;
    }
    instance.luid = static_cast<std::uint64_t>(high) << 32 | low;
    instance.type = std::wstring(name);
    return instance;
}

GpuEngineUtilization AggregateGpuEngines(const std::vector<std::pair<GpuEngineInstance, double>>& samples,
                                         std::uint32_t pid, std::optional<std::uint64_t> luid) {
    // Per engine (adapter, node, engine): every process's share summed, as Task Manager's per-engine graph.
    using Engine = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;
    std::map<Engine, double> total;
    std::map<Engine, double> game;
    GpuEngineUtilization result;
    for (const auto& [instance, value] : samples) {
        if (instance.type != L"3D" || (luid && instance.luid != *luid) || !std::isfinite(value)) {
            continue;
        }
        const Engine engine{instance.luid, instance.phys, instance.engine};
        ++result.instances;
        total[engine] += std::max(0.0, value);
        if (instance.pid == pid) {
            game[engine] += std::max(0.0, value);
        }
    }
    if (!result.instances) {
        return result;
    }
    double busiest = 0.0;
    for (const auto& [engine, value] : total) {
        busiest = std::max(busiest, value);
    }
    double own = 0.0;
    for (const auto& [engine, value] : game) {
        own = std::max(own, value);
    }
    result.total = std::min(100.0, busiest);
    result.game = std::min(100.0, own);
    return result;
}

void PerformanceSummary::Stat::Add(double value) {
    ++count;
    sum += value;
    max = std::max(max, value);
}

void PerformanceSummary::Add(const PerformanceReading& reading, const Cost& cost) {
    ++samples_;
    wall_.Add(cost.wall_ms);
    if (reading.budget_ms > 0.0) {
        budget_.Add(reading.budget_ms);
    }
    if (reading.frame_ms > 0.0) {
        frame_.Add(reading.frame_ms);
    }
    if (reading.cpu_ms) {
        cpu_.Add(*reading.cpu_ms);
    }
    if (reading.gpu_percent) {
        gpu_.Add(*reading.gpu_percent);
    }
    if (reading.gpu_game_percent) {
        gpu_game_.Add(*reading.gpu_game_percent);
    }
    if (reading.thread_busy_percent) {
        thread_.Add(*reading.thread_busy_percent);
    }
    pipelined_ += reading.cpu_state == PerformanceCpuState::kPipelined ? 1 : 0;
    turbo_ += reading.cpu_state == PerformanceCpuState::kTurbo ? 1 : 0;
    cpu_bound_ += reading.verdict == PerformanceVerdict::kCpuBound ? 1 : 0;
    gpu_bound_ += reading.verdict == PerformanceVerdict::kGpuBound ? 1 : 0;
    if (reading.usage_bytes) {
        usage_bytes_ = reading.usage_bytes;
        usage_max_ = std::max(usage_max_.value_or(0), *reading.usage_bytes);
    }
    if (reading.budget_bytes) {
        budget_bytes_ = reading.budget_bytes;
    }
    framegen_bytes_ = reading.framegen_bytes;
    framegen_ = framegen_ || reading.framegen_generating;
}

std::string PerformanceSummary::Take(double seconds, std::optional<double> sampler_cpu_percent,
                                     const std::string& gpu_counters) {
    const auto stat = [](const Stat& stat, int decimals, const char* unit) {
        return stat.count ? Fixed(stat.Mean(), decimals) + "/" + Fixed(stat.max, decimals) + unit : std::string("n/a");
    };
    std::string line = "Performance summary (" + Fixed(seconds, 1) + " s, " + std::to_string(samples_) + " samples): ";
    line += "budget avg/max=" + stat(budget_, 2, " ms");
    if (budget_.count) {
        line += " (" + Fixed(1000.0 / budget_.Mean(), 1) + " fps target" + (framegen_ ? ", Frame Generation" : "") + ")";
    }
    line += ", gameFrame avg/max=" + stat(frame_, 2, " ms");
    line += ", cpu avg/max=" + stat(cpu_, 2, " ms");
    if (cpu_.count && budget_.count) {
        line += " (headroom " + PerformanceHeadroomText((budget_.Mean() - cpu_.Mean()) / budget_.Mean() * 100.0) + ")";
    }
    line += ", cpuHidden pipelined/turbo=" + std::to_string(pipelined_) + "/" + std::to_string(turbo_) + " samples";
    line += ", endFrameThreadBusy avg/max=" + stat(thread_, 0, "%");
    line += ", gpu3d avg/max=" + stat(gpu_, 0, "%");
    if (gpu_.count) {
        line += " (headroom " + PerformanceHeadroomText(100.0 - gpu_.Mean()) + ")";
    }
    line += ", gameGpu3d avg/max=" + stat(gpu_game_, 0, "%");
    line += ", vram=";
    if (usage_bytes_) {
        line += PerformanceMemoryText(*usage_bytes_) + " (max " + PerformanceMemoryText(usage_max_.value_or(0)) +
                ") of " + (budget_bytes_ ? PerformanceMemoryText(*budget_bytes_) : std::string("unknown"));
        if (budget_bytes_ && usage_max_ && *usage_max_ > *budget_bytes_) {
            line += " (over budget)";
        }
    } else {
        line += "unknown";
    }
    if (framegen_bytes_) {
        line += ", frameGenerationVram=" + PerformanceMemoryText(framegen_bytes_);
    }
    line += ", verdict cpu/gpu=" + std::to_string(cpu_bound_) + "/" + std::to_string(gpu_bound_) + " samples";
    line += ", gpuCounters=" + gpu_counters;
    line += ", sampler wall avg/max=" + stat(wall_, 2, " ms") + " per sample";
    if (sampler_cpu_percent) {
        line += ", samplerCpu=" + Fixed(*sampler_cpu_percent, 2) + "% of a core";
    }
    *this = PerformanceSummary{};
    return line;
}

} // namespace depthxr
