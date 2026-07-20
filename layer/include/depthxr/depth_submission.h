#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace depthxr {

// Compatibility mode lets the application render with DepthXR-adjusted view
// geometry while the runtime receives the original projection metadata. Keep
// both sides so xrEndFrame only rewrites a layer that actually carried the
// adjusted xrLocateViews result; unrelated projection layers remain untouched.
template <typename Space, typename View>
struct DepthSubmissionGeometryRecord {
    Space space{};
    std::vector<View> original_views;
    std::vector<View> adjusted_views;
};

struct DepthSubmissionLookupDiagnostics {
    bool cache_available{false};
    bool time_candidate{false};
    bool space_candidate{false};
    bool view_count_candidate{false};
};

inline constexpr std::int64_t kDepthSubmissionGeometryMatchWindow = 5'000'000;
inline constexpr std::size_t kDepthSubmissionGeometryMaxFrames = 180;

template <typename Time, typename Space, typename View>
void CacheDepthSubmissionGeometryValue(
    std::map<Time, std::vector<DepthSubmissionGeometryRecord<Space, View>>>& cache,
    Time time,
    Space space,
    std::span<const View> original_views,
    std::span<const View> adjusted_views) {
    auto& records = cache[time];
    for (DepthSubmissionGeometryRecord<Space, View>& record : records) {
        if (record.space == space && record.adjusted_views.size() == adjusted_views.size()) {
            record.original_views.assign(original_views.begin(), original_views.end());
            record.adjusted_views.assign(adjusted_views.begin(), adjusted_views.end());
            return;
        }
    }

    DepthSubmissionGeometryRecord<Space, View> record;
    record.space = space;
    record.original_views.assign(original_views.begin(), original_views.end());
    record.adjusted_views.assign(adjusted_views.begin(), adjusted_views.end());
    records.push_back(std::move(record));

    while (cache.size() > kDepthSubmissionGeometryMaxFrames) {
        cache.erase(cache.begin());
    }
}

template <typename Time, typename Space, typename View>
bool FindDepthSubmissionGeometryValue(
    const std::map<Time, std::vector<DepthSubmissionGeometryRecord<Space, View>>>& cache,
    Time time,
    Space space,
    std::size_t view_count,
    const DepthSubmissionGeometryRecord<Space, View>** geometry,
    Time* matched_time,
    DepthSubmissionLookupDiagnostics* diagnostics = nullptr) {
    if (diagnostics) {
        *diagnostics = {};
        diagnostics->cache_available = !cache.empty();
    }
    if (!geometry || !matched_time) {
        return false;
    }

    bool found = false;
    Time best_time{};
    Time best_delta{};
    const DepthSubmissionGeometryRecord<Space, View>* best_record = nullptr;
    const auto consider = [&](auto candidate) {
        if (candidate == cache.end()) {
            return;
        }
        const Time delta = candidate->first > time ? candidate->first - time : time - candidate->first;
        if (delta > static_cast<Time>(kDepthSubmissionGeometryMatchWindow) ||
            (found && delta >= best_delta)) {
            return;
        }
        if (diagnostics) {
            diagnostics->time_candidate = true;
        }
        for (const DepthSubmissionGeometryRecord<Space, View>& record : candidate->second) {
            if (record.space != space) {
                continue;
            }
            if (diagnostics) {
                diagnostics->space_candidate = true;
            }
            if (record.original_views.size() != view_count ||
                record.adjusted_views.size() != view_count) {
                continue;
            }
            if (diagnostics) {
                diagnostics->view_count_candidate = true;
            }
            found = true;
            best_time = candidate->first;
            best_delta = delta;
            best_record = &record;
            break;
        }
    };

    const auto upper = cache.lower_bound(time);
    consider(upper);
    if (upper != cache.begin()) {
        consider(std::prev(upper));
    }

    if (!found || !best_record) {
        *geometry = nullptr;
        *matched_time = 0;
        return false;
    }

    *geometry = best_record;
    *matched_time = best_time;
    return true;
}

template <typename Time, typename Space, typename View>
void PruneDepthSubmissionGeometryValues(
    std::map<Time, std::vector<DepthSubmissionGeometryRecord<Space, View>>>& cache,
    Time time) {
    const auto keep_from = cache.upper_bound(time);
    if (keep_from != cache.begin()) {
        cache.erase(cache.begin(), keep_from);
    }
}

} // namespace depthxr
