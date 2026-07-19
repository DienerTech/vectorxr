#pragma once

namespace depthxr {

// xrLocateViews may be queried more than once for the same display time. A
// VIEW-relative query has no world-space head orientation to drive Pivot from,
// so it must not advance smoothing or replace the frame's cached pose delta.
constexpr bool ShouldDrivePivotFromLocateViews(bool has_locate_info,
                                               bool requested_base_is_tracked_view) noexcept {
    return has_locate_info && !requested_base_is_tracked_view;
}

} // namespace depthxr

