// vectorxr_hardware_probe --gpu-ms: the GPU load's calibration, frame queue and recalibration, the same for every
// graphics API (the dispatches and timestamps are the backends', probe_d3d11.cpp and probe_vulkan.cpp).
#include "probe_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace probe {

double Percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    return sorted[std::min(sorted.size() - 1,
                           static_cast<std::size_t>(static_cast<double>(sorted.size()) * fraction))];
}

uint32_t ScaleIterations(uint32_t iterations, double factor) {
    const double scaled = std::round(static_cast<double>(iterations) * factor);
    if (!std::isfinite(scaled)) {
        return iterations;
    }
    return static_cast<uint32_t>(std::clamp(scaled, 1.0, static_cast<double>(kLoadMaximumIterations)));
}

double CalibrationFactor(double target_ms, double measured_ms) {
    return measured_ms > 0.0 ? std::clamp(target_ms / measured_ms, 1.0 / 64.0, 64.0) : 64.0;
}

double RecalibrationFactor(double target_ms, double measured_ms) {
    return measured_ms > 0.0 ? std::clamp(target_ms / measured_ms, 0.9, 1.1) : 1.1;
}

uint32_t ViewIterations(uint32_t iterations, double share) {
    if (!(share > 0.0)) {
        return 0;
    }
    return ScaleIterations(iterations, std::min(share, 1.0));
}

uint32_t ChunkIterations(uint32_t iterations, uint32_t chunks) {
    if (iterations == 0) {
        return 0;
    }
    return ScaleIterations(iterations, 1.0 / static_cast<double>(std::max(chunks, 1u)));
}

GpuLoad::GpuLoad(std::unique_ptr<GpuLoadBackend> backend, const Options& options)
    : backend_(std::move(backend)),
      target_ms_(options.gpu_ms),
      chunks_(static_cast<uint32_t>(std::max(options.gpu_chunks, 1))),
      frames_ahead_(static_cast<uint64_t>(options.gpu_frames_ahead)),
      fixed_(options.gpu_ms_fixed) {}

void GpuLoad::Calibrate(uint32_t views) {
    views = std::clamp<uint32_t>(views, 1, kMaximumViews);
    iterations_ = kLoadFirstIterations;
    backend_->Warm(iterations_, 30);
    double measured = 0.0;
    for (int round = 0; round < kLoadCalibrationRounds; ++round) {
        std::vector<double> runs;
        for (int run = 0; run < kLoadCalibrationRuns; ++run) {
            const uint32_t view_iterations = ViewIterations(iterations_, 1.0 / views);
            if (const std::optional<double> ms =
                    backend_->TimeViews(views, ChunkIterations(view_iterations, chunks_), chunks_)) {
                runs.push_back(*ms);
            }
        }
        if (runs.empty()) {
            continue; // every run straddled a clock change
        }
        std::sort(runs.begin(), runs.end());
        measured = Percentile(runs, 0.5);
        if (std::abs(measured - target_ms_) <= kLoadTolerance * target_ms_ || round + 1 == kLoadCalibrationRounds) {
            break;
        }
        iterations_ = ScaleIterations(iterations_, CalibrationFactor(target_ms_, measured));
    }
    calibrated_iterations_ = iterations_;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2) << "GPU load: target " << target_ms_ << " ms = " << iterations_
         << " iterations in " << views << " views";
    if (chunks_ > 1) {
        line << " of " << chunks_ << " dispatches each";
    }
    line << " (measured " << measured << " ms)\n";
    std::cout << line.str();
}

void GpuLoad::BeginFrame() {
    if (frames_ahead_ > 0 && frames_ >= frames_ahead_) {
        WaitForFrame(frames_ - frames_ahead_);
    }
    Collect(false);
    // Frames finish in order, so every frame from the oldest uncollected one on is still queued.
    max_queued_ = std::max(max_queued_, frames_ - collected_);
}

void GpuLoad::DispatchView(double share) {
    const std::size_t slot = frames_ % GpuLoadBackend::kRing;
    Frame& frame = ring_[slot];
    if (!open_) {
        while (frames_ - collected_ >= GpuLoadBackend::kRing) {
            CollectOldest(true);
        }
        frame.views_used = 0;
        frame.iterations = iterations_;
        backend_->BeginSlot(slot);
        open_ = true;
    }
    const uint32_t iterations = ViewIterations(frame.iterations, share);
    if (iterations == 0 || frame.views_used >= kMaximumViews) {
        return;
    }
    backend_->RecordTimed(slot, frame.views_used++, ChunkIterations(iterations, chunks_), chunks_);
}

void GpuLoad::EndFrame() {
    if (!open_) {
        return;
    }
    backend_->SignalFrame(frames_ % GpuLoadBackend::kRing);
    open_ = false;
    ++frames_;
    if (frames_ahead_ == 0) {
        WaitForFrame(frames_ - 1);
    }
}

void GpuLoad::Drain() {
    Collect(true);
}

bool GpuLoad::QueueBounded() const {
    return max_queued_ <= (frames_ahead_ > 0 ? frames_ahead_ - 1 : 0);
}

double GpuLoad::MeanMs() const {
    double sum = 0.0;
    for (const double ms : frame_ms_) {
        sum += ms;
    }
    return frame_ms_.empty() ? 0.0 : sum / static_cast<double>(frame_ms_.size());
}

void GpuLoad::Report(std::ostream& out) const {
    std::vector<double> sorted = frame_ms_;
    std::sort(sorted.begin(), sorted.end());
    std::ostringstream line;
    line << std::fixed << std::setprecision(2) << "GPU load: target " << target_ms_ << " ms, mean " << MeanMs()
         << " ms, p50 " << Percentile(sorted, 0.5) << " ms, p99 " << Percentile(sorted, 0.99) << " ms ("
         << sorted.size() << " frames timed";
    if (disjoint_frames_ > 0) {
        line << ", " << disjoint_frames_ << " straddled a clock change";
    }
    line << "), iterations " << iterations_ << " (calibrated " << calibrated_iterations_ << ", "
         << (fixed_ ? std::string("fixed") : std::to_string(adjustments_) + " adjustments")
         << (chunks_ > 1 ? ", " + std::to_string(chunks_) + " dispatches a view" : std::string()) << ")\n"
         << "GPU frames ahead " << frames_ahead_ << ": at most " << max_queued_
         << " earlier frames queued when a frame began, " << stalls_ << " stalls of " << frames_ << " frames";
    if (stalls_ > 0) {
        line << ", mean " << stall_ms_ / static_cast<double>(stalls_) << " ms, max " << stall_max_ms_
             << " ms, total " << stall_ms_ << " ms";
    }
    line << "\n";
    out << line.str();
}

// A frame's load: the sum of its views' timers; nullopt when one straddled a clock change.
std::optional<double> GpuLoad::ReadFrame(std::size_t slot) {
    double ms = 0.0;
    for (uint32_t view = 0; view < ring_[slot].views_used; ++view) {
        const std::optional<double> view_ms = backend_->ReadView(slot, view);
        if (!view_ms) {
            return std::nullopt;
        }
        ms += *view_ms;
    }
    return ms;
}

// Blocks until frame `index` has run on the GPU; a wait that blocks is a stall.
void GpuLoad::WaitForFrame(uint64_t index) {
    if (index < collected_) {
        return; // collected: it has run
    }
    const std::size_t slot = index % GpuLoadBackend::kRing;
    if (backend_->FrameDone(slot, true)) {
        return;
    }
    const auto begun = std::chrono::steady_clock::now();
    backend_->WaitFrame(slot);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
    ++stalls_;
    stall_ms_ += ms;
    stall_max_ms_ = std::max(stall_max_ms_, ms);
}

void GpuLoad::Collect(bool wait) {
    while (collected_ < frames_ && CollectOldest(wait)) {
    }
}

// The oldest uncollected frame's timing, once it has run; recalibrates every 30 frames unless --gpu-ms-fixed.
bool GpuLoad::CollectOldest(bool wait) {
    const std::size_t slot = collected_ % GpuLoadBackend::kRing;
    const Frame& frame = ring_[slot];
    if (wait) {
        if (!backend_->FrameDone(slot, true)) {
            backend_->WaitFrame(slot);
        }
    } else if (!backend_->FrameDone(slot, false)) {
        return false;
    }
    ++collected_;
    if (frame.views_used == 0) {
        return true;
    }
    const std::optional<double> ms = ReadFrame(slot);
    if (!ms) {
        ++disjoint_frames_;
        return true;
    }
    frame_ms_.push_back(*ms);
    // Only frames recorded at the current iterations judge them (older ones may still be in flight).
    if (!fixed_ && frame.iterations == iterations_) {
        window_ms_ += *ms;
        if (++window_frames_ >= kLoadRecalibrationFrames) {
            iterations_ = ScaleIterations(
                iterations_, RecalibrationFactor(target_ms_, window_ms_ / static_cast<double>(window_frames_)));
            ++adjustments_;
            window_ms_ = 0.0;
            window_frames_ = 0;
        }
    }
    return true;
}

bool RunGpuCalibrationLoop(GpuLoad& load, const Options& options, const std::function<void()>& begin_frame,
                           const std::function<void()>& end_frame) {
    const uint32_t views = options.quad_views ? 4u : 2u;
    load.Calibrate(views);
    const double period_ms = std::max(options.frame_ms, 1000.0 / 90.0);
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(period_ms));
    const auto started = std::chrono::steady_clock::now();
    auto next = started;
    for (int frame = 0; frame < kLoadCalibrationFrames; ++frame) {
        begin_frame();
        load.BeginFrame();
        for (uint32_t view = 0; view < views; ++view) {
            load.DispatchView(1.0 / views);
        }
        load.EndFrame();
        end_frame();
        // A late frame (the GPU holding it back) starts the next one at once, without catching up.
        next = std::max(next + period, std::chrono::steady_clock::now());
        while (std::chrono::steady_clock::now() < next) {
            if (next - std::chrono::steady_clock::now() > std::chrono::milliseconds(2)) {
                Sleep(1);
            } else {
                std::this_thread::yield();
            }
        }
    }
    load.Drain();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    load.Report(std::cout);
    const double error = (load.MeanMs() - options.gpu_ms) / options.gpu_ms;
    const bool within = std::abs(error) <= kLoadTolerance;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2) << "Frames: " << kLoadCalibrationFrames << " in " << seconds << " s ("
         << kLoadCalibrationFrames / seconds << " fps, paced at " << 1000.0 / period_ms << " Hz, " << views
         << " views)\n"
         << "GPU load check: the mean is " << std::showpos << error * 100.0 << std::noshowpos << "% off the target ("
         << (within ? "within" : "outside") << " 3%); the frame queue "
         << (load.QueueBounded() ? "stayed within" : "exceeded") << " --gpu-frames-ahead\n";
    std::cout << line.str();
    return within && load.QueueBounded();
}

} // namespace probe
