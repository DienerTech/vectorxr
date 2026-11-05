#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace depthxr {

// When a stalled async xrWaitFrame counts against Auto's async pacing.
//
// DCS on Pimax (Oct 29 S4 flight) left its loading screen at about 18 fps. The
// first two pipelined waits after Turbo engaged both ran into the drain bound,
// half a second apart, which tripped the probe's two-stall fallback the same
// moment async engaged, and Auto recorded a sequenced verdict. A session with
// async pinned then ran clean for 17 minutes, with one stall at its own
// engagement. So stalls right after engaging, or while the game is still
// hitching, are not counted. They still cost the frame its drain, so the grace
// is short and capped: a runtime that interlocks every off-thread wait (the
// case the fallback exists for) still falls back a few seconds after engaging.
//
// A fallback soon after engaging is "early": its sequenced verdict is recorded
// as provisional, so the next session probes async again (runtime_pacing.h).
//
// The times come from the caller (steady_clock in the layer, a script in its
// tests); the frame thread serializes the calls.
class TurboProbeGuard {
  public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        // Stalls this soon after async pacing engages are not counted.
        std::chrono::milliseconds grace{2000};
        // A game hitch (app time only, Turbo's own blocking subtracted) keeps
        // stalls uncounted for this long after it...
        std::chrono::milliseconds hitch_grace{1000};
        // ...but never past this point after engaging.
        std::chrono::milliseconds max_grace{5000};
        // An app frame this slow is a hitch (the cadence gate's healthy bound).
        double hitch_ms{50.0};
        // A fallback this soon after engaging is early.
        std::chrono::milliseconds early_failure{10000};
    };

    void Configure(const Limits& limits) { limits_ = limits; }
    const Limits& Settings() const { return limits_; }
    void Reset() { engaged_at_.reset(); grace_until_ = {}; }

    // Async pacing engaged (each engage, re-engages after a toggle included).
    void Engaged(Clock::time_point now) {
        engaged_at_ = now;
        grace_until_ = now + limits_.grace;
    }

    // Every app frame, with its app-only frame time (negative when unknown).
    void AppFrame(Clock::time_point now, double app_frame_ms) {
        if (!engaged_at_ || app_frame_ms < limits_.hitch_ms) return;
        const Clock::time_point cap = *engaged_at_ + limits_.max_grace;
        grace_until_ = std::max(grace_until_, std::min(cap, now + limits_.hitch_grace));
    }

    bool InGrace(Clock::time_point now) const { return engaged_at_ && now < grace_until_; }

    std::optional<Clock::duration> SinceEngaged(Clock::time_point now) const {
        if (!engaged_at_) return std::nullopt;
        return now - *engaged_at_;
    }

    bool Early(Clock::time_point now) const {
        return engaged_at_ && now - *engaged_at_ < limits_.early_failure;
    }

  private:
    Limits limits_{};
    std::optional<Clock::time_point> engaged_at_;
    Clock::time_point grace_until_{};
};

} // namespace depthxr
