#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <openxr/openxr.h>

namespace depthxr {

// Bounded provenance for app-visible predictions. This observes timing only:
// neither a timestamp rejection nor a missing match changes a pose horizon.
// The caller serializes publication and lookup with the Turbo state mutex.
class TurboPredictionHistory {
public:
    enum class Origin { kRuntime, kSequencedCache, kAsyncCache, kAsyncExtrapolated };
    struct Prediction {
        std::uint64_t app_wait{0};
        XrTime display_time{0};
        XrTime native_display_time{0};
        XrDuration native_period{0};
        std::uint64_t native_generation{0};
        std::uint64_t trace_id{0};
        Origin origin{Origin::kRuntime};
    };
    static constexpr std::size_t kCapacity = 32;

    void Reset() { *this = {}; }
    void NativeWait(XrTime display_time, XrDuration period) {
        native_display_time_ = display_time;
        native_period_ = period;
        ++native_generation_;
    }
    void AppWait(XrTime display_time, Origin origin, std::uint64_t trace_id) {
        predictions_[next_] = {++app_wait_, display_time, native_display_time_, native_period_,
                               native_generation_, trace_id, origin};
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
    }
    std::optional<Prediction> Find(XrTime display_time) const {
        for (std::size_t offset = 0; offset < count_; ++offset) {
            const auto& prediction = predictions_[(next_ + kCapacity - 1 - offset) % kCapacity];
            if (prediction.display_time == display_time) return prediction;
        }
        return std::nullopt;
    }
    std::uint64_t NativeGeneration() const { return native_generation_; }
    static const char* Name(Origin origin) {
        switch (origin) {
        case Origin::kRuntime: return "runtime-wait";
        case Origin::kSequencedCache: return "sequenced-cache";
        case Origin::kAsyncCache: return "async-cache";
        case Origin::kAsyncExtrapolated: return "async-extrapolated";
        }
        return "unknown";
    }

private:
    std::array<Prediction, kCapacity> predictions_{};
    std::size_t next_{0}, count_{0};
    std::uint64_t app_wait_{0}, native_generation_{0};
    XrTime native_display_time_{0};
    XrDuration native_period_{0};
};

} // namespace depthxr
