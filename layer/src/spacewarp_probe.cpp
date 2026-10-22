#include "depthxr/spacewarp_probe.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string_view>

namespace depthxr {
namespace {

bool IsSpaceWarpType(XrStructureType type, bool fb, bool ext) {
    return (fb && type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB) ||
           (ext && type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT);
}

SpaceWarpViewInfo Record(const XrBaseInStructure* node, std::uint32_t layer_index, std::uint32_t view_index) {
    SpaceWarpViewInfo info;
    info.type = node->type;
    info.layer_index = layer_index;
    info.view_index = view_index;
    if (node->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB) {
        const auto* fb = reinterpret_cast<const XrCompositionLayerSpaceWarpInfoFB*>(node);
        info.layer_flags = fb->layerFlags;
        info.motion_vector = fb->motionVectorSubImage;
        info.app_space_delta_pose = fb->appSpaceDeltaPose;
        info.depth = fb->depthSubImage;
        info.min_depth = fb->minDepth;
        info.max_depth = fb->maxDepth;
        info.near_z = fb->nearZ;
        info.far_z = fb->farZ;
    } else {
        const auto* ext = reinterpret_cast<const XrFrameSynthesisInfoEXT*>(node);
        info.layer_flags = ext->layerFlags;
        info.motion_vector = ext->motionVectorSubImage;
        info.motion_vector_scale = ext->motionVectorScale;
        info.motion_vector_offset = ext->motionVectorOffset;
        info.app_space_delta_pose = ext->appSpaceDeltaPose;
        info.depth = ext->depthSubImage;
        info.min_depth = ext->minDepth;
        info.max_depth = ext->maxDepth;
        info.near_z = ext->nearZ;
        info.far_z = ext->farZ;
    }
    return info;
}

bool ChainCarriesSpaceWarp(const void* next, bool fb, bool ext) {
    for (auto* node = static_cast<const XrBaseInStructure*>(next); node; node = node->next) {
        if (IsSpaceWarpType(node->type, fb, ext)) {
            return true;
        }
    }
    return false;
}

bool CarriesSpaceWarp(const XrCompositionLayerProjectionView& view, bool fb, bool ext) {
    return ChainCarriesSpaceWarp(view.next, fb, ext);
}

const XrCompositionLayerProjection* ProjectionAt(const XrFrameEndInfo* frame, std::uint32_t i) {
    const XrCompositionLayerBaseHeader* base = frame->layers ? frame->layers[i] : nullptr;
    return base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION
               ? reinterpret_cast<const XrCompositionLayerProjection*>(base)
               : nullptr;
}

void Rect(std::ostringstream& stream, const XrSwapchainSubImage& image) {
    stream << image.imageRect.offset.x << "," << image.imageRect.offset.y << "+" << image.imageRect.extent.width
           << "x" << image.imageRect.extent.height << "[" << image.imageArrayIndex << "]";
}

} // namespace

SpaceWarpProbeSelection ParseSpaceWarpProbe(const char* value) {
    if (!value) {
        return {};
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text == "fb") {
        return {true, false};
    }
    if (text == "ext") {
        return {false, true};
    }
    if (text == "both") {
        return {true, true};
    }
    return {};
}

SpaceWarpProbeSelection ReadSpaceWarpProbeEnvironment() {
    return ParseSpaceWarpProbe(std::getenv("VECTORXR_SPACEWARP_PROBE"));
}

const char* SpaceWarpProbeName(SpaceWarpProbeSelection selection) {
    return selection.fb && selection.ext ? "both" : selection.fb ? "fb" : selection.ext ? "ext" : "off";
}

bool SpaceWarpProbeOwns(bool probe_on, bool application_enabled, bool runtime_lists) {
    return probe_on && application_enabled && !runtime_lists;
}

const char* ToString(MotionVectorSize size) {
    switch (size) {
    case MotionVectorSize::kHalf:
        return "half";
    case MotionVectorSize::kQuarter:
        return "quarter";
    default:
        return "eighth";
    }
}

std::optional<MotionVectorSize> ParseMotionVectorSize(const std::string& value) {
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text == "half") return MotionVectorSize::kHalf;
    if (text == "quarter") return MotionVectorSize::kQuarter;
    if (text == "eighth") return MotionVectorSize::kEighth;
    return std::nullopt;
}

MotionVectorSize ChooseMotionVectorSize(bool read, std::optional<MotionVectorSize> research) {
    if (research) return *research;
    return read ? MotionVectorSize::kHalf : MotionVectorSize::kEighth;
}

XrExtent2Di RecommendedMotionVectorSize(std::uint32_t eye_width, std::uint32_t eye_height, MotionVectorSize size) {
    const std::uint32_t divisor = size == MotionVectorSize::kHalf ? 2 : size == MotionVectorSize::kQuarter ? 4 : 8;
    return {static_cast<std::int32_t>(std::max<std::uint32_t>(1, (eye_width + divisor - 1) / divisor)),
            static_cast<std::int32_t>(std::max<std::uint32_t>(1, (eye_height + divisor - 1) / divisor))};
}

bool IsHalfFloatRgbaFormat(std::int64_t format) {
    constexpr std::int64_t kDxgiR16G16B16A16Float = 10;
    constexpr std::int64_t kVkR16G16B16A16Sfloat = 97;
    constexpr std::int64_t kGlRgba16f = 0x881A;
    return format == kDxgiR16G16B16A16Float || format == kVkR16G16B16A16Sfloat || format == kGlRgba16f;
}

const XrFrameEndInfo* WithoutSpaceWarp(const XrFrameEndInfo* frame, bool strip_fb, bool strip_ext,
                                       SpaceWarpFrame& storage) {
    storage.found.clear();
    if (!frame || (!strip_fb && !strip_ext)) {
        return frame;
    }
    const auto any_carried = [&](const XrCompositionLayerProjection* projection) {
        return projection && projection->views &&
               std::any_of(projection->views, projection->views + projection->viewCount,
                           [&](const XrCompositionLayerProjectionView& view) {
                               return CarriesSpaceWarp(view, strip_fb, strip_ext);
                           });
    };
    bool any = false;
    for (std::uint32_t i = 0; i < frame->layerCount && !any; ++i) {
        any = any_carried(ProjectionAt(frame, i));
    }
    if (!any) {
        return frame;
    }

    storage.info = *frame;
    storage.layers.assign(frame->layers, frame->layers + frame->layerCount);
    for (std::uint32_t i = 0; i < frame->layerCount; ++i) {
        const XrCompositionLayerProjection* projection = ProjectionAt(frame, i);
        if (!any_carried(projection)) {
            continue;
        }
        storage.views.emplace_back(projection->views, projection->views + projection->viewCount);
        std::vector<XrCompositionLayerProjectionView>& views = storage.views.back();
        for (std::uint32_t v = 0; v < projection->viewCount; ++v) {
            // `link` is the copied chain's pointer to the node at hand; the
            // rest of the chain is the application's once nothing is left to strip.
            const void** link = &views[v].next;
            while (ChainCarriesSpaceWarp(*link, strip_fb, strip_ext)) {
                const auto* node = static_cast<const XrBaseInStructure*>(*link);
                if (IsSpaceWarpType(node->type, strip_fb, strip_ext)) {
                    storage.found.push_back(Record(node, i, v));
                    storage.found.back().stripped = true;
                    *link = node->next;
                } else if (node->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                    storage.depth_infos.push_back(*reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(node));
                    *link = &storage.depth_infos.back();
                    link = &storage.depth_infos.back().next;
                } else {
                    // Unknown: the rest of the chain stays the application's.
                    for (auto* rest = node->next; rest; rest = rest->next) {
                        if (IsSpaceWarpType(rest->type, strip_fb, strip_ext)) {
                            storage.found.push_back(Record(rest, i, v));
                        }
                    }
                    break;
                }
            }
        }
        storage.projections.push_back(*projection);
        storage.projections.back().views = views.data();
        storage.layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&storage.projections.back());
    }
    storage.info.layers = storage.layers.data();
    return &storage.info;
}

std::string DescribeSpaceWarpView(const SpaceWarpViewInfo& view) {
    const bool fb = view.type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB;
    std::ostringstream stream;
    stream << (fb ? "XrCompositionLayerSpaceWarpInfoFB" : "XrFrameSynthesisInfoEXT") << " layer=" << view.layer_index
           << " view=" << view.view_index << " stripped=" << (view.stripped ? 1 : 0) << " flags=0x" << std::hex
           << view.layer_flags << std::dec << " mv=";
    Rect(stream, view.motion_vector);
    stream << " depth=";
    Rect(stream, view.depth);
    stream << " minDepth=" << view.min_depth << " maxDepth=" << view.max_depth << " nearZ=" << view.near_z
           << " farZ=" << view.far_z;
    const XrPosef& pose = view.app_space_delta_pose;
    stream << " appSpaceDeltaPose=q(" << pose.orientation.x << "," << pose.orientation.y << "," << pose.orientation.z
           << "," << pose.orientation.w << ")p(" << pose.position.x << "," << pose.position.y << ","
           << pose.position.z << ")";
    if (!fb) {
        const XrVector4f& s = view.motion_vector_scale;
        const XrVector4f& o = view.motion_vector_offset;
        stream << " mvScale=(" << s.x << "," << s.y << "," << s.z << "," << s.w << ") mvOffset=(" << o.x << ","
               << o.y << "," << o.z << "," << o.w << ")";
    }
    return stream.str();
}

} // namespace depthxr
