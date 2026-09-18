#include "depthxr/turbo_trace.h"

#include <algorithm>
#include <functional>
#include <sstream>

namespace depthxr {
using namespace std::chrono_literals;

void TurboTimingTrace::Start(Logger& logger) {
    Stop();
    {
        std::scoped_lock lock(mutex_);
        count_ = 0;
        accepted_ = dropped_ = 0;
        next_id_ = 0;
        stopping_ = false;
        start_ = std::chrono::steady_clock::now();
        burst_until_ = start_ + 1s;
        next_burst_ = start_ + 30s;
    }
    logger.Info("Turbo trace v1: wallUnixMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()) +
                "; 1s bursts every 30s and on Turbo transitions; maxEvents=30000, queue=2048. "
                "ns is monotonic time since trace start; thread and id correlate calls. "
                "wait.return a=prediction b=period c=result; predict a=runtimePrediction b=returnedPrediction c=generation; "
                "runtime.wait.return a=prediction b=period c=result; runtime.wait.state a=shouldRender b=xrNow; "
                "worker.job/handoff id=generation; submit.enter a=displayTime b=layerCount; submit.return a=result; "
                "locate a=displayTime; gate.return a=completed; clock a=xrNow. ASW activity is not measured.");
    try {
        worker_ = std::thread([this, &logger] { Run(logger); });
        enabled_.store(true, std::memory_order_release);
    } catch (...) {
        logger.Error("Turbo trace: writer could not start; timing capture disabled.");
    }
}

void TurboTimingTrace::Stop() {
    enabled_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void TurboTimingTrace::Burst() {
    if (!Enabled()) return;
    std::scoped_lock lock(mutex_);
    burst_until_ = std::chrono::steady_clock::now() + 1s;
}

bool TurboTimingTrace::Capturing() {
    if (!Enabled()) return false;
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    return !stopping_ && accepted_ < kBudget && (now < burst_until_ || now >= next_burst_);
}

void TurboTimingTrace::Record(const char* event, std::uint64_t id, std::int64_t a,
                              std::int64_t b, std::int64_t c) {
    if (!Enabled()) return;
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    if (stopping_ || accepted_ >= kBudget) return;
    if (now >= next_burst_) {
        burst_until_ = now + 1s;
        next_burst_ = now + 30s;
    }
    if (now >= burst_until_) return;
    if (count_ == kCapacity) {
        ++dropped_;
        return;
    }
    queue_[count_++] = {event, std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_).count(),
        static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())), id, a, b, c};
    ++accepted_;
    // The writer also wakes once per second. Avoid waking it for every frame.
    if (count_ == kCapacity / 2) cv_.notify_one();
}

void TurboTimingTrace::Run(Logger& logger) {
    try {
        std::array<Event, kCapacity> batch;
        bool budget_reported = false;
        for (;;) {
            std::size_t count;
            std::uint64_t dropped, accepted;
            bool stopping;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, 1s, [this] { return stopping_ || count_ >= kCapacity / 2; });
                count = count_;
                std::copy_n(queue_.begin(), count, batch.begin());
                count_ = 0;
                dropped = dropped_;
                dropped_ = 0;
                accepted = accepted_;
                stopping = stopping_;
            }
            for (std::size_t i = 0; i < count; ++i) {
                const auto& e = batch[i];
                std::ostringstream line;
                line << "Turbo-trace ns=" << e.ns << " thread=" << e.thread << " id=" << e.id
                     << " event=" << e.name << " a=" << e.a << " b=" << e.b << " c=" << e.c;
                logger.Info(line.str());
            }
            if (dropped) logger.Info("Turbo trace: queue overflow; dropped=" + std::to_string(dropped));
            if (accepted >= kBudget && !budget_reported) {
                logger.Info("Turbo trace: session event budget reached; relaunch to capture again.");
                budget_reported = true;
            }
            if (stopping) break;
        }
    } catch (...) {
        enabled_.store(false, std::memory_order_release);
        // Diagnostics must never terminate the host application.
    }
}
} // namespace depthxr
