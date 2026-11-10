#pragma once

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <chrono>
#include <optional>
#include <type_traits>

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
// tests). The frame thread makes the calls; the times are atomic so an Auto
// fallback on another thread may read Early and SinceEngaged.
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

    // Limits are set before use (tests); never while frames run.
    void Configure(const Limits& limits) { limits_ = limits; }
    const Limits& Settings() const { return limits_; }
    void Reset() {
        engaged_ns_.store(kNotEngaged, std::memory_order_relaxed);
        grace_until_ns_.store(kNotEngaged, std::memory_order_relaxed);
    }

    // Async pacing engaged (each engage, re-engages after a toggle included).
    void Engaged(Clock::time_point now) {
        grace_until_ns_.store(Ns(now + limits_.grace), std::memory_order_relaxed);
        engaged_ns_.store(Ns(now), std::memory_order_relaxed);
    }

    // Every app frame, with its app-only frame time (negative when unknown).
    void AppFrame(Clock::time_point now, double app_frame_ms) {
        const std::int64_t engaged = engaged_ns_.load(std::memory_order_relaxed);
        if (engaged == kNotEngaged || app_frame_ms < limits_.hitch_ms) return;
        const std::int64_t cap = engaged + Ns(limits_.max_grace);
        const std::int64_t until = std::min(cap, Ns(now + limits_.hitch_grace));
        if (until > grace_until_ns_.load(std::memory_order_relaxed))
            grace_until_ns_.store(until, std::memory_order_relaxed);
    }

    bool InGrace(Clock::time_point now) const {
        return engaged_ns_.load(std::memory_order_relaxed) != kNotEngaged &&
               Ns(now) < grace_until_ns_.load(std::memory_order_relaxed);
    }

    std::optional<Clock::duration> SinceEngaged(Clock::time_point now) const {
        const std::int64_t engaged = engaged_ns_.load(std::memory_order_relaxed);
        if (engaged == kNotEngaged) return std::nullopt;
        return std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(Ns(now) - engaged));
    }

    bool Early(Clock::time_point now) const {
        const auto since = SinceEngaged(now);
        return since && *since < limits_.early_failure;
    }

  private:
    // Written on the frame thread; an Auto fallback can read them from the
    // app's wait/begin thread (NoteTurboRuntimeFailure), so they are atomic
    // (single words: no torn reads, no lock on the frame path).
    static constexpr std::int64_t kNotEngaged = INT64_MIN;
    template <typename T>
    static std::int64_t Ns(T value) {
        if constexpr (std::is_same_v<T, Clock::time_point>) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
        } else {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
        }
    }
    Limits limits_{};
    std::atomic<std::int64_t> engaged_ns_{kNotEngaged};
    std::atomic<std::int64_t> grace_until_ns_{kNotEngaged};
};

} // namespace depthxr
