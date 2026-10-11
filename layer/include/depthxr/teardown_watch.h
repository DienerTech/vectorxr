#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include <Windows.h>

#include "depthxr/logger.h"

namespace depthxr {

// Watches one teardown call the application makes (xrDestroySwapchain,
// xrEndSession, xrDestroySession, xrDestroyInstance): once it has run longer
// than `report_after`, a thread-pool timer logs the step it is in and what
// `context` tells (Frame Generation's presenter thread), then again every few
// seconds, so a teardown that never returns still says where it is. A call
// that was reported logs when it returns. `context` runs on the timer's
// thread while the call runs: it must not take a lock the call may hold.
// An unarmed watch (Frame Generation never ran) reports nothing.
class TeardownWatch {
  public:
    using Clock = std::chrono::steady_clock;

    TeardownWatch(Logger& logger, const char* call, std::chrono::milliseconds report_after,
                  std::function<std::string()> context, bool armed = true)
        : logger_(logger), call_(call), context_(std::move(context)), started_(Clock::now()) {
        const DWORD due = static_cast<DWORD>(std::clamp<long long>(report_after.count(), 1, 600000));
        if (!armed || !CreateTimerQueueTimer(&timer_, nullptr, &TeardownWatch::Fire, this, due, kRepeatMs,
                                             WT_EXECUTEDEFAULT)) {
            timer_ = nullptr;
        }
    }

    ~TeardownWatch() {
        if (timer_) {
            // Waits for a report in progress: none runs after this.
            DeleteTimerQueueTimer(nullptr, timer_, INVALID_HANDLE_VALUE);
        }
        if (reports_.load() > 0) {
            logger_.Info(std::string("Teardown: ") + call_ + " returned after " + Elapsed() + " s.");
        }
    }

    TeardownWatch(const TeardownWatch&) = delete;
    TeardownWatch& operator=(const TeardownWatch&) = delete;

    // The step the call is in from now on (a string literal).
    void Step(const char* step) { step_ = step; }

  private:
    static constexpr DWORD kRepeatMs = 3000;
    static constexpr int kMaxReports = 8;

    static void CALLBACK Fire(void* parameter, BOOLEAN) {
        TeardownWatch& watch = *static_cast<TeardownWatch*>(parameter);
        if (watch.reports_.fetch_add(1) >= kMaxReports) {
            return;
        }
        std::string text =
            std::string("Teardown: ") + watch.call_ + " has run " + watch.Elapsed() + " s, now " + watch.step_.load();
        if (watch.context_) {
            const std::string context = watch.context_();
            if (!context.empty()) {
                text += " (" + context + ")";
            }
        }
        watch.logger_.Error(text + ".");
    }

    std::string Elapsed() const {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", std::chrono::duration<double>(Clock::now() - started_).count());
        return text;
    }

    Logger& logger_;
    const char* call_;
    std::function<std::string()> context_;
    Clock::time_point started_;
    std::atomic<const char*> step_{"starting"};
    std::atomic<int> reports_{0};
    HANDLE timer_{nullptr};
};

} // namespace depthxr
