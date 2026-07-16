#pragma once

#include <cstdint>

namespace depthxr {

struct QuadViewsCanvasDimensions {
    uint32_t width{1};
    uint32_t height{1};
    double density{1.0};
};

QuadViewsCanvasDimensions ComputeQuadViewsCanvasDimensions(uint32_t stereo_width,
                                                           uint32_t stereo_height,
                                                           uint32_t runtime_max_width,
                                                           uint32_t runtime_max_height,
                                                           double focus_scale);

} // namespace depthxr
