#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

namespace depthxr {

// Passive observation of poses returned to the application. A valid flag
// alone does not establish a fresh eye-camera sample or foveated rendering.
struct GazeStatusText {
    std::string text;
    int tone{0}; // 0 neutral, 1 receiving/changing, 2 attention, 3 invalid
};

class GazeStatus {
public:
    using Clock = std::chrono::steady_clock;
    void Reset() { *this = {}; }
    void Observe(bool valid, bool tracked, const std::array<float, 4>& orientation,
                 bool same_space_pair, Clock::time_point now) {
        last_query_ = now;
        valid_ = valid;
        for (float value : orientation) valid_ = valid_ && std::isfinite(value);
        tracked_ = tracked;
        if (!valid_) { previous_.reset(); last_change_.reset(); return; }
        if (!same_space_pair) { previous_.reset(); last_change_.reset(); }
        if (previous_) {
            // q and -q describe the same rotation; compare both signs.
            double positive = 0, negative = 0;
            for (int i = 0; i < 4; ++i) {
                positive += std::pow(orientation[i] - (*previous_)[i], 2);
                negative += std::pow(orientation[i] + (*previous_)[i], 2);
            }
            if (std::min(positive, negative) > 0.00000025) last_change_ = now;
        }
        previous_ = orientation;
    }
    GazeStatusText Describe(bool requested, bool enabled, Clock::time_point now) const {
        if (!requested) return {"Not requested by game", 0};
        if (!enabled) return {"Extension unavailable", 2};
        if (!last_query_) return {"Enabled / no pose queries yet", 2};
        if (now - *last_query_ >= std::chrono::seconds(2)) return {"No recent game pose queries", 2};
        if (!valid_) return {"Game receiving invalid pose", 3};
        if (!tracked_) return {"Valid pose / not tracked", 2};
        if (last_change_ && now - *last_change_ < std::chrono::seconds(2)) return {"Receiving / poses changing", 1};
        return {"Receiving / poses unchanged", 2};
    }
private:
    std::optional<Clock::time_point> last_query_, last_change_;
    std::optional<std::array<float, 4>> previous_;
    bool valid_{false}, tracked_{false};
};
} // namespace depthxr
