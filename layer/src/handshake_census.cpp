#include "depthxr/handshake_census.h"

#include <openxr/openxr_reflection.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace depthxr {
namespace {

struct EnumName {
    std::int64_t value;
    const char* name;
};

#define DEPTHXR_CENSUS_ENUM(name, value) {static_cast<std::int64_t>(value), #name},
constexpr EnumName kStructureTypes[] = {XR_LIST_ENUM_XrStructureType(DEPTHXR_CENSUS_ENUM)};
constexpr EnumName kResults[] = {XR_LIST_ENUM_XrResult(DEPTHXR_CENSUS_ENUM)};
constexpr EnumName kFormFactors[] = {XR_LIST_ENUM_XrFormFactor(DEPTHXR_CENSUS_ENUM)};
constexpr EnumName kViewConfigurationTypes[] = {XR_LIST_ENUM_XrViewConfigurationType(DEPTHXR_CENSUS_ENUM)};
constexpr EnumName kBlendModes[] = {XR_LIST_ENUM_XrEnvironmentBlendMode(DEPTHXR_CENSUS_ENUM)};
constexpr EnumName kReferenceSpaces[] = {XR_LIST_ENUM_XrReferenceSpaceType(DEPTHXR_CENSUS_ENUM)};
#undef DEPTHXR_CENSUS_ENUM

// The DXGI formats an OpenXR session commonly offers.
constexpr EnumName kDxgiFormats[] = {
    {2, "R32G32B32A32_FLOAT"},  {10, "R16G16B16A16_FLOAT"}, {11, "R16G16B16A16_UNORM"},
    {16, "R32G32_FLOAT"},       {20, "D32_FLOAT_S8X24_UINT"}, {24, "R10G10B10A2_UNORM"},
    {26, "R11G11B10_FLOAT"},    {27, "R8G8B8A8_TYPELESS"},  {28, "R8G8B8A8_UNORM"},
    {29, "R8G8B8A8_UNORM_SRGB"}, {34, "R16G16_FLOAT"},      {40, "D32_FLOAT"},
    {41, "R32_FLOAT"},          {45, "D24_UNORM_S8_UINT"},  {54, "R16_FLOAT"},
    {55, "D16_UNORM"},          {87, "B8G8R8A8_UNORM"},     {91, "B8G8R8A8_UNORM_SRGB"},
};

template <std::size_t N>
std::string Name(const EnumName (&names)[N], std::int64_t value, std::string_view prefix) {
    for (const EnumName& entry : names) {
        if (entry.value == value) {
            std::string_view name(entry.name);
            if (name.starts_with(prefix)) {
                name.remove_prefix(prefix.size());
            }
            return std::string(name);
        }
    }
    return std::to_string(value);
}

const XrBaseInStructure* Find(const void* next, XrStructureType type) {
    for (auto* node = static_cast<const XrBaseInStructure*>(next); node; node = node->next) {
        if (node->type == type) {
            return node;
        }
    }
    return nullptr;
}

std::vector<std::string> Names(const char* const* names, std::uint32_t count) {
    std::vector<std::string> list;
    for (std::uint32_t i = 0; names && i < count; ++i) {
        if (names[i]) {
            list.emplace_back(names[i]);
        }
    }
    return list;
}

bool Contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::vector<std::string> Missing(const std::vector<std::string>& names, const std::vector<std::string>& from) {
    std::vector<std::string> missing;
    for (const std::string& name : names) {
        if (!Contains(from, name)) {
            missing.push_back(name);
        }
    }
    return missing;
}

} // namespace

std::string CensusStructureType(XrStructureType type) {
    return Name(kStructureTypes, type, "XR_TYPE_");
}

std::string CensusChain(const void* next) {
    std::string chain = "[";
    std::uint32_t length = 0;
    for (auto* node = static_cast<const XrBaseInStructure*>(next); node && length < 32; node = node->next, ++length) {
        if (length > 0) {
            chain += ',';
        }
        chain += CensusStructureType(node->type);
    }
    return chain + "]";
}

std::string CensusResult(XrResult result) {
    return Name(kResults, result, "");
}

std::string CensusFormFactor(XrFormFactor form_factor) {
    return Name(kFormFactors, form_factor, "XR_FORM_FACTOR_");
}

std::string CensusViewConfigurationType(XrViewConfigurationType type) {
    return Name(kViewConfigurationTypes, type, "XR_VIEW_CONFIGURATION_TYPE_");
}

std::string CensusBlendMode(XrEnvironmentBlendMode mode) {
    return Name(kBlendModes, mode, "XR_ENVIRONMENT_BLEND_MODE_");
}

std::string CensusReferenceSpace(XrReferenceSpaceType type) {
    return Name(kReferenceSpaces, type, "XR_REFERENCE_SPACE_TYPE_");
}

std::string CensusVersion(XrVersion version) {
    return std::to_string(XR_VERSION_MAJOR(version)) + "." + std::to_string(XR_VERSION_MINOR(version)) + "." +
           std::to_string(XR_VERSION_PATCH(version));
}

std::string CensusSwapchainFormat(std::int64_t format, bool dxgi) {
    const std::string name = dxgi ? Name(kDxgiFormats, format, "") : std::to_string(format);
    return name == std::to_string(format) ? name : std::to_string(format) + ":" + name;
}

std::string CensusQuoted(const char* text, std::size_t capacity) {
    return "\"" + (text ? std::string(text, strnlen(text, capacity)) : std::string()) + "\"";
}

std::string CensusList(const std::vector<std::string>& names) {
    std::string list = "(" + std::to_string(names.size()) + ")[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        list += (i ? " " : "") + names[i];
    }
    return list + "]";
}

std::string DescribeInstanceRequest(const XrInstanceCreateInfo& info) {
    const XrApplicationInfo& app = info.applicationInfo;
    std::ostringstream stream;
    stream << "application=" << CensusQuoted(app.applicationName, XR_MAX_APPLICATION_NAME_SIZE)
           << " applicationVersion=" << app.applicationVersion
           << " engine=" << CensusQuoted(app.engineName, XR_MAX_ENGINE_NAME_SIZE)
           << " engineVersion=" << app.engineVersion << " apiVersion=" << CensusVersion(app.apiVersion)
           << " createFlags=" << info.createFlags << " next=" << CensusChain(info.next)
           << " apiLayers=" << CensusList(Names(info.enabledApiLayerNames, info.enabledApiLayerCount))
           << " extensions=" << CensusList(Names(info.enabledExtensionNames, info.enabledExtensionCount));
    return stream.str();
}

std::string DescribeInstanceExtensions(const std::vector<std::string>& requested, bool scan_complete,
                                       std::string_view scan_detail, const std::vector<std::string>& runtime_lists,
                                       const std::vector<std::string>& sent) {
    std::ostringstream stream;
    stream << "runtimeLists(scan=" << scan_detail << ")=" << CensusList(runtime_lists);
    stream << "; requestedNotInRuntimeList="
           << (scan_complete ? CensusList(Missing(requested, runtime_lists)) : std::string("unknown"));
    stream << "; strippedByLayer=" << CensusList(Missing(requested, sent))
           << "; addedByLayer=" << CensusList(Missing(sent, requested)) << "; sentToRuntime=" << sent.size()
           << "; xrEnumerateInstanceExtensionProperties: unseen (the loader answers it from the runtime and the "
              "API layers' manifests, never through a layer)";
    return stream.str();
}

std::string DescribeSystemProperties(XrSystemId system_id, const XrSystemProperties* properties, XrResult result) {
    std::ostringstream stream;
    stream << "systemId=" << system_id << " result=" << CensusResult(result);
    if (!properties) {
        stream << " properties=null";
        return stream.str();
    }
    stream << " chain=" << CensusChain(properties->next);
    if (XR_FAILED(result)) {
        return stream.str();
    }
    stream << " systemName=" << CensusQuoted(properties->systemName, XR_MAX_SYSTEM_NAME_SIZE) << " vendorId=0x"
           << std::hex << properties->vendorId << std::dec
           << " maxSwapchainImage=" << properties->graphicsProperties.maxSwapchainImageWidth << "x"
           << properties->graphicsProperties.maxSwapchainImageHeight
           << " maxLayerCount=" << properties->graphicsProperties.maxLayerCount
           << " orientationTracking=" << properties->trackingProperties.orientationTracking
           << " positionTracking=" << properties->trackingProperties.positionTracking;
    if (const auto* fb = reinterpret_cast<const XrSystemSpaceWarpPropertiesFB*>(
            Find(properties->next, XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB))) {
        stream << " SYSTEM_SPACE_WARP_PROPERTIES_FB.recommendedMotionVectorImageRect="
               << fb->recommendedMotionVectorImageRectWidth << "x" << fb->recommendedMotionVectorImageRectHeight;
    }
    if (const auto* gaze = reinterpret_cast<const XrSystemEyeGazeInteractionPropertiesEXT*>(
            Find(properties->next, XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT))) {
        stream << " SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT.supportsEyeGazeInteraction="
               << gaze->supportsEyeGazeInteraction;
    }
    if (const auto* foveated = reinterpret_cast<const XrSystemFoveatedRenderingPropertiesVARJO*>(
            Find(properties->next, XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO))) {
        stream << " SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO.supportsFoveatedRendering="
               << foveated->supportsFoveatedRendering;
    }
    if (const auto* hands = reinterpret_cast<const XrSystemHandTrackingPropertiesEXT*>(
            Find(properties->next, XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT))) {
        stream << " SYSTEM_HAND_TRACKING_PROPERTIES_EXT.supportsHandTracking=" << hands->supportsHandTracking;
    }
    return stream.str();
}

std::string DescribeViewConfigurationViews(XrViewConfigurationType type, std::uint32_t capacity,
                                           const std::uint32_t* count, const XrViewConfigurationView* views,
                                           XrResult result) {
    std::ostringstream stream;
    stream << "viewConfiguration=" << CensusViewConfigurationType(type) << " capacity=" << capacity
           << " count=" << (count ? std::to_string(*count) : std::string("null")) << " result=" << CensusResult(result);
    if (XR_FAILED(result) || !views || !count) {
        return stream.str();
    }
    const std::uint32_t shown = std::min(capacity, *count);
    for (std::uint32_t i = 0; i < shown; ++i) {
        const XrViewConfigurationView& view = views[i];
        stream << " view" << i << "=" << view.recommendedImageRectWidth << "x" << view.recommendedImageRectHeight
               << "(max " << view.maxImageRectWidth << "x" << view.maxImageRectHeight << ", samples "
               << view.recommendedSwapchainSampleCount << "/" << view.maxSwapchainSampleCount
               << ", chain=" << CensusChain(view.next);
        if (const auto* synthesis = reinterpret_cast<const XrFrameSynthesisConfigViewEXT*>(
                Find(view.next, XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT))) {
            stream << ", recommendedMotionVectorImageRect=" << synthesis->recommendedMotionVectorImageRectWidth << "x"
                   << synthesis->recommendedMotionVectorImageRectHeight;
        }
        stream << ")";
    }
    return stream.str();
}

std::string DescribeCensusSwapchains(const std::vector<CensusSwapchain>& swapchains) {
    if (swapchains.empty()) {
        return "none";
    }
    std::ostringstream stream;
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        const CensusSwapchain& swapchain = swapchains[i];
        stream << (i ? "; " : "") << swapchain.role << " 0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(swapchain.handle) << std::dec << "(" << swapchain.description
               << ") images=" << swapchain.images << " acquires=" << swapchain.acquires << " waits=" << swapchain.waits
               << " releases=" << swapchain.releases << (swapchain.destroyed ? " destroyed" : "");
    }
    return stream.str();
}

void HandshakeCensus::Reset(bool active) {
    std::scoped_lock lock(mutex_);
    calls_.clear();
    always_logged_.clear();
    seen_.clear();
    names_.clear();
    order_.clear();
    pending_.clear();
    application_called_ = false;
    active_.store(active, std::memory_order_release);
}

std::optional<std::uint64_t> HandshakeCensus::Note(std::string_view function, const std::string& detail, bool always) {
    if (!Active()) {
        return std::nullopt;
    }
    std::scoped_lock lock(mutex_);
    const std::string name(function);
    const std::uint64_t call = ++calls_[name];
    const bool first = seen_.insert(name + '\n' + detail).second;
    if (first || (always && always_logged_[name] < 16)) {
        if (always) {
            ++always_logged_[name];
        }
        return call;
    }
    return std::nullopt;
}

std::uint64_t HandshakeCensus::Calls(std::string_view function) const {
    std::scoped_lock lock(mutex_);
    const auto found = calls_.find(std::string(function));
    return found == calls_.end() ? 0 : found->second;
}

bool HandshakeCensus::FirstApplicationCall() {
    if (!Active()) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    const bool first = !application_called_;
    application_called_ = true;
    return first;
}

void HandshakeCensus::NoteProcAddr(std::string_view name, XrResult result, bool resolved) {
    if (!Active()) {
        return;
    }
    std::scoped_lock lock(mutex_);
    const auto [it, inserted] = names_.try_emplace(std::string(name));
    ++it->second.queries;
    if (inserted) {
        it->second.resolved = resolved;
        it->second.result = result;
        order_.push_back(it->first);
        pending_.push_back(it->first);
    }
}

std::string HandshakeCensus::FlushProcAddrs(std::string_view when) {
    if (!Active()) {
        return {};
    }
    std::scoped_lock lock(mutex_);
    if (pending_.empty()) {
        return {};
    }
    std::ostringstream stream;
    stream << "xrGetInstanceProcAddr, " << pending_.size() << " new name(s) " << when << ":";
    for (const std::string& name : pending_) {
        const LookedUp& looked = names_[name];
        stream << ' ' << name;
        if (!looked.resolved) {
            stream << "=unresolved(" << CensusResult(looked.result) << ")";
        }
    }
    pending_.clear();
    return stream.str();
}

std::string HandshakeCensus::ProcAddrRepeats() const {
    std::scoped_lock lock(mutex_);
    std::ostringstream stream;
    std::size_t repeated = 0;
    for (const std::string& name : order_) {
        const LookedUp& looked = names_.at(name);
        if (looked.queries > 1) {
            stream << ' ' << name << "x" << looked.queries;
            ++repeated;
        }
    }
    if (repeated == 0) {
        return {};
    }
    return "xrGetInstanceProcAddr, " + std::to_string(order_.size()) + " name(s) in all, " +
           std::to_string(repeated) + " looked up more than once:" + stream.str();
}

} // namespace depthxr
