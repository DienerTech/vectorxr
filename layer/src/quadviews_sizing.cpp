#include "depthxr/quadviews_sizing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace depthxr {
namespace {

uint32_t FallbackCanvasMaximum(uint32_t stereo_dimension) {
    constexpr uint32_t kFallbackScale = 4;
    if (stereo_dimension > std::numeric_limits<uint32_t>::max() / kFallbackScale) {
        return std::numeric_limits<uint32_t>::max();
    }
    return std::max<uint32_t>(1, stereo_dimension * kFallbackScale);
}

uint32_t ScaleCanvasDimension(uint32_t stereo_dimension, double density, uint32_t runtime_maximum) {
    const double scaled = std::max(1.0, std::round(static_cast<double>(stereo_dimension) * density));
    const uint32_t maximum = runtime_maximum > 0 ? runtime_maximum : FallbackCanvasMaximum(stereo_dimension);
    return static_cast<uint32_t>(std::min<double>(scaled, std::max<uint32_t>(1, maximum)));
}

} // namespace

QuadViewsCanvasDimensions ComputeQuadViewsCanvasDimensions(uint32_t stereo_width,
                                                           uint32_t stereo_height,
                                                           uint32_t runtime_max_width,
                                                           uint32_t runtime_max_height,
                                                           double focus_scale) {
    // Match the focus texture's density across the full-FOV output so its
    // sub-rectangle remains a 1:1 sample. Never submit below the runtime's
    // stereo-native density.
    const double density = std::max(1.0, focus_scale);
    return {
        ScaleCanvasDimension(stereo_width, density, runtime_max_width),
        ScaleCanvasDimension(stereo_height, density, runtime_max_height),
        density,
    };
}

} // namespace depthxr
