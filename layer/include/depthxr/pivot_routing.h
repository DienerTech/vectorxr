#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>

namespace depthxr {

// xrLocateViews may be queried more than once for the same display time. A
// VIEW-relative query has no world-space head orientation to drive Pivot from,
// so it must not advance smoothing or replace the frame's cached pose delta.
constexpr bool ShouldDrivePivotFromLocateViews(bool has_locate_info,
                                               bool requested_base_is_tracked_view) noexcept {
    return has_locate_info && !requested_base_is_tracked_view;
}

inline constexpr std::int64_t kPivotPoseDeltaMatchWindow = 5'000'000;
inline constexpr std::size_t kPivotPoseDeltaMaxEntries = 180;

template <typename Time, typename Pose>
void CachePivotPoseDeltaValue(std::map<Time, Pose>& cache, Time time, const Pose& pose_delta) {
    cache[time] = pose_delta;
    while (cache.size() > kPivotPoseDeltaMaxEntries) {
        cache.erase(cache.begin());
    }
}

template <typename Time, typename Pose>
bool FindPivotPoseDeltaValue(const std::map<Time, Pose>& cache,
                             Time time,
                             const Pose& fallback,
                             Pose* pose_delta,
                             Time* matched_time) {
    if (!pose_delta || !matched_time) {
        return false;
    }

    if (cache.empty()) {
        *pose_delta = fallback;
        *matched_time = 0;
        return false;
    }

    const auto exact = cache.find(time);
    if (exact != cache.end()) {
        *pose_delta = exact->second;
        *matched_time = exact->first;
        return true;
    }

    const auto upper = cache.lower_bound(time);
    typename std::map<Time, Pose>::const_iterator best;
    if (upper == cache.begin()) {
        best = upper;
    } else if (upper == cache.end()) {
        best = std::prev(upper);
    } else {
        const auto lower = std::prev(upper);
        best = (time - lower->first <= upper->first - time) ? lower : upper;
    }

    const Time match_delta = best->first > time ? best->first - time : time - best->first;
    if (match_delta > static_cast<Time>(kPivotPoseDeltaMatchWindow)) {
        *pose_delta = fallback;
        *matched_time = best->first;
        return false;
    }

    *pose_delta = best->second;
    *matched_time = best->first;
    return true;
}

template <typename Time, typename Pose>
void PrunePivotPoseDeltaValues(std::map<Time, Pose>& cache, Time time) {
    const auto keep_from = cache.upper_bound(time);
    if (keep_from != cache.begin()) {
        cache.erase(cache.begin(), keep_from);
    }
}

} // namespace depthxr
