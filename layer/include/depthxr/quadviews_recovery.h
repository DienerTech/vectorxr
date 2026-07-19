#pragma once

#include <chrono>
#include <optional>

namespace depthxr {

// Gates a compositor recycle until tracking has remained continuously healthy
// for the configured delay. Any unavailable sample while recovery is pending
// restarts the stabilization window.
class QuadViewsRecoveryStabilizer {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void Schedule(TimePoint recovered_at, std::chrono::milliseconds delay) noexcept {
        stabilization_delay_ = delay < std::chrono::milliseconds::zero()
            ? std::chrono::milliseconds::zero()
            : delay;
        not_before_ = recovered_at + stabilization_delay_;
    }

    void NoteUnstable(TimePoint observed_at) noexcept {
        if (not_before_.has_value()) {
            not_before_ = observed_at + stabilization_delay_;
        }
    }

    [[nodiscard]] bool Pending() const noexcept {
        return not_before_.has_value();
    }

    [[nodiscard]] bool Ready(TimePoint now) const noexcept {
        return not_before_.has_value() && now >= *not_before_;
    }

    void Reset() noexcept {
        not_before_.reset();
        stabilization_delay_ = std::chrono::milliseconds::zero();
    }

  private:
    std::chrono::milliseconds stabilization_delay_{0};
    std::optional<TimePoint> not_before_;
};

} // namespace depthxr
