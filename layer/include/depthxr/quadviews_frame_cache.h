#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>

namespace depthxr {

// Keeps the render geometry and diagnostics produced by one xrLocateViews call
// together. OpenXR applications may locate multiple predicted frames before
// submitting an older one, so consumers must select the complete snapshot for
// the submitted display time instead of combining cached geometry with mutable
// "latest" diagnostic state.
template <typename Payload>
class QuadViewsFrameCache {
public:
    void Store(std::int64_t time, Payload payload, std::size_t max_entries) {
        if (time == 0 || max_entries == 0) {
            return;
        }

        frames_[time] = std::move(payload);
        while (frames_.size() > max_entries) {
            frames_.erase(frames_.begin());
        }
    }

    bool FindNearest(std::int64_t time,
                     std::int64_t max_delta,
                     Payload* payload,
                     std::int64_t* matched_time) const {
        if (!payload || !matched_time || frames_.empty()) {
            if (matched_time) {
                *matched_time = 0;
            }
            return false;
        }

        const auto exact = frames_.find(time);
        if (exact != frames_.end()) {
            *payload = exact->second;
            *matched_time = exact->first;
            return true;
        }

        const auto upper = frames_.lower_bound(time);
        typename std::map<std::int64_t, Payload>::const_iterator best;
        if (upper == frames_.begin()) {
            best = upper;
        } else if (upper == frames_.end()) {
            best = std::prev(upper);
        } else {
            const auto lower = std::prev(upper);
            best = (time - lower->first <= upper->first - time) ? lower : upper;
        }

        const std::int64_t match_delta =
            best->first > time ? best->first - time : time - best->first;
        *matched_time = best->first;
        if (match_delta > max_delta) {
            return false;
        }

        *payload = best->second;
        return true;
    }

    void PruneThrough(std::int64_t time) {
        const auto keep_from = frames_.upper_bound(time);
        if (keep_from != frames_.begin()) {
            frames_.erase(frames_.begin(), keep_from);
        }
    }

    void Clear() {
        frames_.clear();
    }

    [[nodiscard]] std::size_t Size() const {
        return frames_.size();
    }

private:
    std::map<std::int64_t, Payload> frames_;
};

} // namespace depthxr
