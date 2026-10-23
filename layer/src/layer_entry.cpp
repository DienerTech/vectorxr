#include "depthxr/openxr_layer.h"
#include "depthxr/openxr_loader_api_layer.h"

#include "depthxr/crt_failure_report.h"
#include "depthxr/runtime_compatibility.h"
#include "depthxr/spacewarp_probe.h"
#include "depthxr/vulkan_enable_hooks.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace depthxr {
namespace {

constexpr std::string_view kLayerName = "XR_APILAYER_DIENERTECH_VECTORXR";

std::filesystem::path GetModuleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&GetModuleDirectory),
                            &module)) {
        return {};
    }

    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

XrResult XRAPI_CALL DepthxrDestroyInstance(XrInstance instance) {
    return OpenXrLayer::Instance().DestroyInstance(instance);
}

XrResult XRAPI_CALL DepthxrCreateSession(XrInstance instance,
                                         const XrSessionCreateInfo* create_info,
                                         XrSession* session) {
    return OpenXrLayer::Instance().CreateSession(instance, create_info, session);
}

XrResult XRAPI_CALL DepthxrDestroySession(XrSession session) {
    return OpenXrLayer::Instance().DestroySession(session);
}

XrResult XRAPI_CALL DepthxrBeginSession(XrSession session, const XrSessionBeginInfo* begin_info) {
    return OpenXrLayer::Instance().BeginSession(session, begin_info);
}

XrResult XRAPI_CALL DepthxrEndSession(XrSession session) {
    return OpenXrLayer::Instance().EndSession(session);
}

XrResult XRAPI_CALL DepthxrAttachSessionActionSets(XrSession session,
                                                   const XrSessionActionSetsAttachInfo* attach_info) {
    return OpenXrLayer::Instance().AttachSessionActionSets(session, attach_info);
}

XrResult XRAPI_CALL DepthxrSyncActions(XrSession session, const XrActionsSyncInfo* sync_info) {
    return OpenXrLayer::Instance().SyncActions(session, sync_info);
}

XrResult XRAPI_CALL DepthxrWaitFrame(XrSession session,
                                     const XrFrameWaitInfo* frame_wait_info,
                                     XrFrameState* frame_state) {
    return OpenXrLayer::Instance().WaitFrame(session, frame_wait_info, frame_state);
}

XrResult XRAPI_CALL DepthxrBeginFrame(XrSession session, const XrFrameBeginInfo* frame_begin_info) {
    return OpenXrLayer::Instance().BeginFrame(session, frame_begin_info);
}

XrResult XRAPI_CALL DepthxrEndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
    return OpenXrLayer::Instance().EndFrame(session, frame_end_info);
}

XrResult XRAPI_CALL DepthxrGetSystemProperties(XrInstance instance,
                                               XrSystemId system_id,
                                               XrSystemProperties* properties) {
    return OpenXrLayer::Instance().GetSystemProperties(instance, system_id, properties);
}

XrResult XRAPI_CALL DepthxrEnumerateEnvironmentBlendModes(
    XrInstance instance,
    XrSystemId system_id,
    XrViewConfigurationType view_configuration_type,
    uint32_t environment_blend_mode_capacity_input,
    uint32_t* environment_blend_mode_count_output,
    XrEnvironmentBlendMode* environment_blend_modes) {
    return OpenXrLayer::Instance().EnumerateEnvironmentBlendModes(instance,
                                                                 system_id,
                                                                 view_configuration_type,
                                                                 environment_blend_mode_capacity_input,
                                                                 environment_blend_mode_count_output,
                                                                 environment_blend_modes);
}

XrResult XRAPI_CALL DepthxrEnumerateViewConfigurations(XrInstance instance,
                                                       XrSystemId system_id,
                                                       uint32_t view_configuration_type_capacity_input,
                                                       uint32_t* view_configuration_type_count_output,
                                                       XrViewConfigurationType* view_configuration_types) {
    return OpenXrLayer::Instance().EnumerateViewConfigurations(instance,
                                                              system_id,
                                                              view_configuration_type_capacity_input,
                                                              view_configuration_type_count_output,
                                                              view_configuration_types);
}

XrResult XRAPI_CALL DepthxrGetViewConfigurationProperties(
    XrInstance instance,
    XrSystemId system_id,
    XrViewConfigurationType view_configuration_type,
    XrViewConfigurationProperties* configuration_properties) {
    return OpenXrLayer::Instance().GetViewConfigurationProperties(
        instance, system_id, view_configuration_type, configuration_properties);
}

XrResult XRAPI_CALL DepthxrEnumerateViewConfigurationViews(XrInstance instance,
                                                          XrSystemId system_id,
                                                          XrViewConfigurationType view_configuration_type,
                                                          uint32_t view_capacity_input,
                                                          uint32_t* view_count_output,
                                                          XrViewConfigurationView* views) {
    return OpenXrLayer::Instance().EnumerateViewConfigurationViews(
        instance, system_id, view_configuration_type, view_capacity_input, view_count_output, views);
}

XrResult XRAPI_CALL DepthxrGetVisibilityMaskKHR(XrSession session,
                                                XrViewConfigurationType view_configuration_type,
                                                uint32_t view_index,
                                                XrVisibilityMaskTypeKHR visibility_mask_type,
                                                XrVisibilityMaskKHR* visibility_mask) {
    return OpenXrLayer::Instance().GetVisibilityMaskKHR(
        session, view_configuration_type, view_index, visibility_mask_type, visibility_mask);
}

XrResult XRAPI_CALL DepthxrEnumerateSwapchainFormats(XrSession session,
                                                     uint32_t format_capacity_input,
                                                     uint32_t* format_count_output,
                                                     int64_t* formats) {
    return OpenXrLayer::Instance().EnumerateSwapchainFormats(
        session, format_capacity_input, format_count_output, formats);
}

XrResult XRAPI_CALL DepthxrCreateSwapchain(XrSession session,
                                           const XrSwapchainCreateInfo* create_info,
                                           XrSwapchain* swapchain) {
    return OpenXrLayer::Instance().CreateSwapchain(session, create_info, swapchain);
}

XrResult XRAPI_CALL DepthxrDestroySwapchain(XrSwapchain swapchain) {
    return OpenXrLayer::Instance().DestroySwapchain(swapchain);
}

XrResult XRAPI_CALL DepthxrEnumerateSwapchainImages(XrSwapchain swapchain,
                                                    uint32_t image_capacity_input,
                                                    uint32_t* image_count_output,
                                                    XrSwapchainImageBaseHeader* images) {
    return OpenXrLayer::Instance().EnumerateSwapchainImages(
        swapchain, image_capacity_input, image_count_output, images);
}

XrResult XRAPI_CALL DepthxrAcquireSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageAcquireInfo* acquire_info,
                                                 uint32_t* index) {
    return OpenXrLayer::Instance().AcquireSwapchainImage(swapchain, acquire_info, index);
}

XrResult XRAPI_CALL DepthxrWaitSwapchainImage(XrSwapchain swapchain,
                                              const XrSwapchainImageWaitInfo* wait_info) {
    return OpenXrLayer::Instance().WaitSwapchainImage(swapchain, wait_info);
}

XrResult XRAPI_CALL DepthxrReleaseSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageReleaseInfo* release_info) {
    return OpenXrLayer::Instance().ReleaseSwapchainImage(swapchain, release_info);
}

XrResult XRAPI_CALL DepthxrEnumerateReferenceSpaces(XrSession session,
                                                   uint32_t space_capacity_input,
                                                   uint32_t* space_count_output,
                                                   XrReferenceSpaceType* spaces) {
    return OpenXrLayer::Instance().EnumerateReferenceSpaces(
        session, space_capacity_input, space_count_output, spaces);
}

XrResult XRAPI_CALL DepthxrGetReferenceSpaceBoundsRect(XrSession session,
                                                       XrReferenceSpaceType reference_space_type,
                                                       XrExtent2Df* bounds) {
    return OpenXrLayer::Instance().GetReferenceSpaceBoundsRect(session, reference_space_type, bounds);
}

XrResult XRAPI_CALL DepthxrCreateReferenceSpace(XrSession session,
                                                const XrReferenceSpaceCreateInfo* create_info,
                                                XrSpace* space) {
    return OpenXrLayer::Instance().CreateReferenceSpace(session, create_info, space);
}

XrResult XRAPI_CALL DepthxrDestroySpace(XrSpace space) {
    return OpenXrLayer::Instance().DestroySpace(space);
}

XrResult XRAPI_CALL DepthxrCreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action) {
    return OpenXrLayer::Instance().CreateAction(set, info, action);
}
XrResult XRAPI_CALL DepthxrDestroyAction(XrAction action) {
    return OpenXrLayer::Instance().DestroyAction(action);
}
XrResult XRAPI_CALL DepthxrDestroyActionSet(XrActionSet set) {
    return OpenXrLayer::Instance().DestroyActionSet(set);
}
XrResult XRAPI_CALL DepthxrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* bindings) {
    return OpenXrLayer::Instance().SuggestInteractionProfileBindings(instance, bindings);
}
XrResult XRAPI_CALL DepthxrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    return OpenXrLayer::Instance().CreateActionSpace(session, info, space);
}

XrResult XRAPI_CALL DepthxrPollEvent(XrInstance instance, XrEventDataBuffer* event_data) {
    return OpenXrLayer::Instance().PollEvent(instance, event_data);
}

XrResult XRAPI_CALL DepthxrLocateSpace(XrSpace space,
                                       XrSpace base_space,
                                       XrTime time,
                                       XrSpaceLocation* location) {
    return OpenXrLayer::Instance().LocateSpace(space, base_space, time, location);
}

XrResult XRAPI_CALL DepthxrLocateViews(XrSession session,
                                       const XrViewLocateInfo* view_locate_info,
                                       XrViewState* view_state,
                                       uint32_t view_capacity_input,
                                       uint32_t* view_count_output,
                                       XrView* views) {
    return OpenXrLayer::Instance().LocateViews(
        session, view_locate_info, view_state, view_capacity_input, view_count_output, views);
}

bool IsLayerOwnedExtension(std::string_view extension_name) {
    return extension_name == XR_VARJO_QUAD_VIEWS_EXTENSION_NAME ||
           extension_name == XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME;
}

bool ExtensionListContains(const std::vector<const char*>& extensions, std::string_view extension_name) {
    return std::find_if(extensions.begin(), extensions.end(), [&](const char* candidate) {
               return candidate && std::string_view(candidate) == extension_name;
           }) != extensions.end();
}

bool ApplicationRequestedExtension(const XrInstanceCreateInfo* create_info, std::string_view extension_name) {
    if (!create_info || !create_info->enabledExtensionNames) {
        return false;
    }

    for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
        const char* candidate = create_info->enabledExtensionNames[i];
        if (candidate && std::string_view(candidate) == extension_name) {
            return true;
        }
    }
    return false;
}

// Single authoritative pre-instance extension probe used by native Varjo
// forwarding, synthesized-quadviews gaze setup, and startup diagnostics.
struct RuntimeExtensionScan {
    bool ran{false};
    bool complete{false};
    const char* detail{"not-run"};
    XrResult result{XR_SUCCESS};
    uint32_t count{0};
    std::vector<std::string> names;

    bool Contains(std::string_view extension_name) const {
        return std::find(names.begin(), names.end(), extension_name) != names.end();
    }
};

std::string LayerName(const XrApiLayerNextInfo& info) {
    return std::string(info.layerName, strnlen(info.layerName, XR_MAX_API_LAYER_NAME_SIZE));
}

// Layers built from mbucchia's layer template (OpenXR Toolkit,
// Quad-Views-Foveated) call through a null pointer when asked for a function
// before their own instance exists: VectorXR directly above one never probes
// the runtime's extensions that way.
bool NextLayerNeedsAnInstance(const XrApiLayerNextInfo* next_info) {
    return next_info && next_info->next && LayerName(*next_info->next).starts_with("XR_APILAYER_MBUCCHIA_");
}

// OpenXR Toolkit's own bypass for an application: a nonzero DWORD "bypass"
// under HKCU\SOFTWARE\OpenXR_Toolkit\<application name>, or a Chromium engine.
bool OpenXrToolkitBypasses(const XrInstanceCreateInfo& create_info) {
    if (std::string_view(create_info.applicationInfo.engineName) == "Chromium") {
        return true;
    }
    const std::string name(create_info.applicationInfo.applicationName,
                           strnlen(create_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE));
    const int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring key = L"SOFTWARE\\OpenXR_Toolkit\\";
    if (length > 0) {
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), length);
        key += wide;
    }
    DWORD bypass = 0;
    DWORD size = sizeof(bypass);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"bypass", RRF_RT_REG_DWORD, nullptr, &bypass, &size) ==
               ERROR_SUCCESS &&
           bypass != 0;
}

// The loader loads every layer's library before it creates the chain, so a
// loaded Toolkit that is not below VectorXR is above it.
std::string OpenXrToolkitPosition(const XrInstanceCreateInfo& create_info, const XrApiLayerNextInfo* next_info) {
    for (const XrApiLayerNextInfo* below = next_info ? next_info->next : nullptr; below; below = below->next) {
        if (LayerName(*below) == "XR_APILAYER_MBUCCHIA_toolkit") {
            return "below";
        }
    }
    if (!GetModuleHandleW(L"XR_APILAYER_MBUCCHIA_toolkit.dll")) {
        return "absent";
    }
    return OpenXrToolkitBypasses(create_info) ? "above-bypassed" : "above";
}

RuntimeExtensionScan ScanRuntimeInstanceExtensions(const XrApiLayerNextInfo* next_info) {
    RuntimeExtensionScan scan;
    const PFN_xrGetInstanceProcAddr next_get_instance_proc_addr =
        next_info ? next_info->nextGetInstanceProcAddr : nullptr;
    if (!next_get_instance_proc_addr) {
        scan.detail = "next-get-instance-proc-addr-unavailable";
        return scan;
    }
    if (NextLayerNeedsAnInstance(next_info)) {
        scan.detail = "next-layer-needs-an-instance"; // the layers-below line names it
        return scan;
    }

    PFN_xrVoidFunction function = nullptr;
    scan.result = next_get_instance_proc_addr(
        XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function);
    if (XR_FAILED(scan.result)) {
        scan.detail = "enumeration-function-query-failed";
        return scan;
    }
    if (!function) {
        scan.detail = "enumeration-function-unavailable";
        return scan;
    }

    const auto enumerate_extension_properties =
        reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    scan.ran = true;
    uint32_t extension_count = 0;
    scan.result = enumerate_extension_properties(nullptr, 0, &extension_count, nullptr);
    if (XR_FAILED(scan.result)) {
        scan.detail = "enumeration-count-query-failed";
        return scan;
    }
    if (extension_count == 0) {
        scan.count = extension_count;
        scan.complete = true;
        scan.detail = "complete";
        return scan;
    }

    std::vector<XrExtensionProperties> properties(extension_count);
    for (XrExtensionProperties& property : properties) {
        property = {XR_TYPE_EXTENSION_PROPERTIES};
    }
    scan.result = enumerate_extension_properties(nullptr, extension_count, &extension_count, properties.data());
    scan.count = extension_count;
    if (XR_FAILED(scan.result)) {
        scan.detail = "enumeration-list-query-failed";
        return scan;
    }

    scan.names.reserve(extension_count);
    for (const XrExtensionProperties& property : properties) {
        scan.names.emplace_back(property.extensionName);
    }
    scan.complete = true;
    scan.detail = "complete";
    return scan;
}

std::string ToLowerAscii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

// Path to the active OpenXR runtime manifest, honoring the loader's precedence:
// the XR_RUNTIME_JSON override, then the per-user then machine-wide registry entry.
// Returns empty when it cannot be resolved.
std::string ReadActiveOpenXrRuntimeManifestPath() {
    if (const char* env_path = std::getenv("XR_RUNTIME_JSON"); env_path && *env_path != '\0') {
        return env_path;
    }

    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &key) != ERROR_SUCCESS) {
            continue;
        }

        wchar_t buffer[2048]{};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        const LONG result =
            RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
            continue;
        }

        int length = static_cast<int>(size / sizeof(wchar_t));
        while (length > 0 && buffer[length - 1] == L'\0') {
            --length;
        }
        if (length <= 0) {
            continue;
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, length, nullptr, 0, nullptr, nullptr);
        if (needed <= 0) {
            continue;
        }
        std::string path(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer, length, path.data(), needed, nullptr, nullptr);
        if (!path.empty()) {
            return path;
        }
    }

    return {};
}

// Reliable Varjo detection: identify the active OpenXR runtime rather than probing
// extensions before instance creation (which proved unreliable on Varjo). Reads no
// OpenXR state, creates no instance, and is a pure no-op on non-Varjo runtimes.
bool ActiveOpenXrRuntimeIsVarjo(std::string& out_path) {
    out_path = ReadActiveOpenXrRuntimeManifestPath();
    if (out_path.empty()) {
        return false;
    }
    if (ToLowerAscii(out_path).find("varjo") != std::string::npos) {
        return true;
    }
    // Fall back to the manifest contents (its runtime name is "Varjo OpenXR Runtime")
    // in case the runtime is installed under a non-obvious path.
    std::ifstream file(out_path, std::ios::binary);
    if (file) {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (ToLowerAscii(std::move(content)).find("varjo") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace
} // namespace depthxr

extern "C" {

XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!function || !name) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    const std::string_view requested(name);
    // Track application gaze independently of VectorXR's Quadviews feature.
    if (requested == "xrCreateAction") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrCreateAction);
        return XR_SUCCESS;
    }
    if (requested == "xrDestroyAction") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrDestroyAction);
        return XR_SUCCESS;
    }
    if (requested == "xrDestroyActionSet") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrDestroyActionSet);
        return XR_SUCCESS;
    }
    if (requested == "xrSuggestInteractionProfileBindings") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrSuggestInteractionProfileBindings);
        return XR_SUCCESS;
    }
    if (requested == "xrCreateActionSpace") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrCreateActionSpace);
        return XR_SUCCESS;
    }
    if (requested == "xrGetInstanceProcAddr") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    if (requested == "xrDestroyInstance") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrDestroyInstance);
        return XR_SUCCESS;
    }
    if (requested == "xrCreateSession") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrCreateSession);
        return XR_SUCCESS;
    }
    if (requested == "xrDestroySession") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrDestroySession);
        return XR_SUCCESS;
    }
    if (requested == "xrBeginSession") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrBeginSession);
        return XR_SUCCESS;
    }
    if (requested == "xrEndSession") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEndSession);
        return XR_SUCCESS;
    }
    if (requested == "xrAttachSessionActionSets") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrAttachSessionActionSets);
        return XR_SUCCESS;
    }
    if (requested == "xrSyncActions") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrSyncActions);
        return XR_SUCCESS;
    }
    if (requested == "xrWaitFrame") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrWaitFrame);
        return XR_SUCCESS;
    }
    if (requested == "xrBeginFrame") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrBeginFrame);
        return XR_SUCCESS;
    }
    if (requested == "xrEndFrame") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEndFrame);
        return XR_SUCCESS;
    }
    if (requested == "xrGetSystemProperties") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrGetSystemProperties);
        return XR_SUCCESS;
    }
    if (requested == "xrEnumerateEnvironmentBlendModes") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEnumerateEnvironmentBlendModes);
        return XR_SUCCESS;
    }
    if (requested == "xrEnumerateViewConfigurations") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEnumerateViewConfigurations);
        return XR_SUCCESS;
    }
    if (requested == "xrGetViewConfigurationProperties") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrGetViewConfigurationProperties);
        return XR_SUCCESS;
    }
    if (requested == "xrEnumerateViewConfigurationViews") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEnumerateViewConfigurationViews);
        return XR_SUCCESS;
    }
    if (requested == "xrGetVisibilityMaskKHR") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrGetVisibilityMaskKHR);
        return XR_SUCCESS;
    }
    if (requested == "xrEnumerateSwapchainFormats") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEnumerateSwapchainFormats);
        return XR_SUCCESS;
    }
    if (requested == "xrCreateSwapchain") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrCreateSwapchain);
        return XR_SUCCESS;
    }
    if (requested == "xrDestroySwapchain") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrDestroySwapchain);
        return XR_SUCCESS;
    }
    if (requested == "xrEnumerateSwapchainImages") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEnumerateSwapchainImages);
        return XR_SUCCESS;
    }
    if (requested == "xrAcquireSwapchainImage") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrAcquireSwapchainImage);
        return XR_SUCCESS;
    }
    if (requested == "xrWaitSwapchainImage") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrWaitSwapchainImage);
        return XR_SUCCESS;
    }
    if (requested == "xrReleaseSwapchainImage") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrReleaseSwapchainImage);
        return XR_SUCCESS;
    }
    if (requested == "xrEnumerateReferenceSpaces") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrEnumerateReferenceSpaces);
        return XR_SUCCESS;
    }
    if (requested == "xrGetReferenceSpaceBoundsRect") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrGetReferenceSpaceBoundsRect);
        return XR_SUCCESS;
    }
    if (requested == "xrCreateReferenceSpace") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrCreateReferenceSpace);
        return XR_SUCCESS;
    }
    if (requested == "xrDestroySpace") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrDestroySpace);
        return XR_SUCCESS;
    }
    if (requested == "xrLocateSpace") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrLocateSpace);
        return XR_SUCCESS;
    }
    if (requested == "xrPollEvent") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrPollEvent);
        return XR_SUCCESS;
    }
    if (requested == "xrLocateViews") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrLocateViews);
        return XR_SUCCESS;
    }
    // XR_KHR_vulkan_enable(2) for Frame Generation's Vulkan bridge, where the runtime offers them.
    if (const PFN_xrVoidFunction hook = depthxr::VulkanEnableHook(instance, name)) {
        *function = hook;
        return XR_SUCCESS;
    }

    return depthxr::OpenXrLayer::Instance().GetInstanceProcAddr(instance, name, function);
}

XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* instance_create_info,
                                             const depthxr::XrApiLayerCreateInfo* layer_info,
                                             XrInstance* instance) {
    using namespace depthxr;

    if (!instance_create_info || !layer_info || !instance || !layer_info->nextInfo) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layer_info->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layer_info->structSize < sizeof(XrApiLayerCreateInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrApiLayerNextInfo* next_info = layer_info->nextInfo;
    if (next_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        next_info->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        next_info->structSize < sizeof(XrApiLayerNextInfo) || !next_info->nextGetInstanceProcAddr ||
        !next_info->nextCreateApiLayerInstance) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (!OpenXrLayer::Instance().CanCreateInstance()) {
        return XR_ERROR_LIMIT_REACHED;
    }

    OpenXrLayer::Instance().SetNextProcAddr(next_info->nextGetInstanceProcAddr);

    XrApiLayerCreateInfo chain_info = *layer_info;
    chain_info.nextInfo = next_info->next;

    OpenXrLayer::InstanceCreateDiagnostics diagnostics;
    // The API layers between VectorXR and the runtime see what VectorXR sends
    // on (a bridged Direct3D 11 game's session is Direct3D 12 for them). The
    // first entry is VectorXR's own.
    for (const XrApiLayerNextInfo* below = next_info->next; below; below = below->next) {
        if (!diagnostics.layers_below.empty()) {
            diagnostics.layers_below += ' ';
        }
        diagnostics.layers_below += std::string(below->layerName, strnlen(below->layerName, XR_MAX_API_LAYER_NAME_SIZE));
    }
    diagnostics.openxr_toolkit = OpenXrToolkitPosition(*instance_create_info, next_info);
    diagnostics.app_requested_quad_views =
        ApplicationRequestedExtension(instance_create_info, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
    diagnostics.app_requested_varjo_foveated_rendering =
        ApplicationRequestedExtension(instance_create_info, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
    diagnostics.app_requested_eye_gaze =
        ApplicationRequestedExtension(instance_create_info, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);

    // Native Varjo extensions are part of the app/runtime contract, not a VectorXR
    // feature toggle. Always preserve them for the active Varjo runtime. VectorXR's
    // profile still decides whether scaling/sharpening modifications are applied,
    // but disabling Quadviews must never disable an application's native path.
    const bool app_requested_layer_owned_quadviews =
        diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering;
    RuntimeExtensionScan extension_scan;
    bool forward_varjo_quad_extensions = false;
    if (app_requested_layer_owned_quadviews) {
        diagnostics.varjo_compatible_eligible = OpenXrLayer::Instance().IsVarjoCompatibleQuadviewsEligible();

        // Always scan (even when ineligible) so the logs show exactly what the
        // pre-instance probe returned — this is the signal the forward hinges on.
        extension_scan = ScanRuntimeInstanceExtensions(next_info);
        diagnostics.pre_instance_extension_scan_ran = extension_scan.ran;
        diagnostics.pre_instance_extension_scan_complete = extension_scan.complete;
        diagnostics.pre_instance_extension_scan_detail = extension_scan.detail;
        diagnostics.pre_instance_extension_scan_result = extension_scan.result;
        diagnostics.pre_instance_extension_count = extension_scan.count;
        diagnostics.runtime_advertises_varjo_quad =
            extension_scan.Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
        diagnostics.runtime_advertises_varjo_foveated =
            extension_scan.Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
        std::string joined_extensions;
        for (const std::string& name : extension_scan.names) {
            if (!joined_extensions.empty()) {
                joined_extensions += ' ';
            }
            joined_extensions += name;
        }
        diagnostics.pre_instance_extensions = std::move(joined_extensions);

        // The manifest identity is a fallback for Varjo, whose pre-instance
        // extension probe can return false negatives. Other runtimes forward
        // only when they advertise every native extension the app requested.
        std::string active_runtime_path;
        diagnostics.active_runtime_is_varjo = ActiveOpenXrRuntimeIsVarjo(active_runtime_path);
        diagnostics.active_runtime_path = std::move(active_runtime_path);

        const bool runtime_supports_requested_native_extensions =
            (!diagnostics.app_requested_quad_views || diagnostics.runtime_advertises_varjo_quad) &&
            (!diagnostics.app_requested_varjo_foveated_rendering ||
             diagnostics.runtime_advertises_varjo_foveated);
        forward_varjo_quad_extensions =
            diagnostics.active_runtime_is_varjo || runtime_supports_requested_native_extensions;
    }
    diagnostics.varjo_compatible_quad_forwarded = forward_varjo_quad_extensions;

    // The space-warp probe (spacewarp_probe.h): its manifests advertise the
    // extensions on a runtime without them, so the layer keeps them from such
    // a runtime. One the runtime lists goes on as it is. A manifest loads the
    // layer while VECTORXR_SPACEWARP_PROBE is set, whatever its value, so any
    // value takes them over (one other than fb, ext or both is warned about).
    diagnostics.spacewarp_probe_value = ReadSpaceWarpProbeVariable();
    diagnostics.spacewarp_probe = SpaceWarpProbeName(ParseSpaceWarpProbe(diagnostics.spacewarp_probe_value.c_str()));
    diagnostics.app_requested_fb_space_warp =
        ApplicationRequestedExtension(instance_create_info, XR_FB_SPACE_WARP_EXTENSION_NAME);
    diagnostics.app_requested_frame_synthesis =
        ApplicationRequestedExtension(instance_create_info, XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME);
    if (!diagnostics.spacewarp_probe_value.empty() &&
        (diagnostics.app_requested_fb_space_warp || diagnostics.app_requested_frame_synthesis)) {
        const RuntimeExtensionScan spacewarp_scan =
            extension_scan.ran ? extension_scan : ScanRuntimeInstanceExtensions(next_info);
        diagnostics.spacewarp_scan_complete = spacewarp_scan.complete;
        diagnostics.runtime_lists_fb_space_warp = spacewarp_scan.Contains(XR_FB_SPACE_WARP_EXTENSION_NAME);
        diagnostics.runtime_lists_frame_synthesis = spacewarp_scan.Contains(XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME);
        diagnostics.fb_space_warp_owned = SpaceWarpProbeOwns(
            true, diagnostics.app_requested_fb_space_warp, diagnostics.runtime_lists_fb_space_warp);
        diagnostics.frame_synthesis_owned = SpaceWarpProbeOwns(
            true, diagnostics.app_requested_frame_synthesis, diagnostics.runtime_lists_frame_synthesis);
    }

    std::vector<const char*> base_downstream_extensions;
    base_downstream_extensions.reserve(instance_create_info->enabledExtensionCount + 1);
    for (uint32_t i = 0; i < instance_create_info->enabledExtensionCount; ++i) {
        const char* extension_name = instance_create_info->enabledExtensionNames[i];
        if (!extension_name) {
            continue;
        }
        // Keep the layer-owned Varjo extensions in the downstream list only when we
        // are forwarding them for Varjo compatible quadviews; otherwise strip as usual.
        if (IsLayerOwnedExtension(extension_name) && !forward_varjo_quad_extensions) {
            continue;
        }
        if ((diagnostics.fb_space_warp_owned && std::string_view(extension_name) == XR_FB_SPACE_WARP_EXTENSION_NAME) ||
            (diagnostics.frame_synthesis_owned &&
             std::string_view(extension_name) == XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME)) {
            continue;
        }
        base_downstream_extensions.push_back(extension_name);
    }

    if (app_requested_layer_owned_quadviews) {
        diagnostics.eye_gaze_probe_state = ClassifyEyeGazeProbe(
            extension_scan.complete,
            extension_scan.Contains(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME));

        std::vector<std::string_view> advertised_extensions;
        advertised_extensions.reserve(extension_scan.names.size());
        for (const std::string& extension_name : extension_scan.names) {
            advertised_extensions.emplace_back(extension_name);
        }

        std::vector<std::string_view> corroborating_forwarded_extensions;
        corroborating_forwarded_extensions.reserve(base_downstream_extensions.size());
        for (const char* extension_name : base_downstream_extensions) {
            if (!extension_name ||
                std::string_view(extension_name) == XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) {
                continue;
            }
            corroborating_forwarded_extensions.emplace_back(extension_name);
            if (extension_scan.complete && !extension_scan.Contains(extension_name)) {
                if (!diagnostics.pre_instance_missing_forwarded_extensions.empty()) {
                    diagnostics.pre_instance_missing_forwarded_extensions += ' ';
                }
                diagnostics.pre_instance_missing_forwarded_extensions += extension_name;
                ++diagnostics.pre_instance_missing_forwarded_extension_count;
            }
        }

        diagnostics.eye_gaze_probe_structurally_unreliable =
            diagnostics.eye_gaze_probe_state == EyeGazeProbeState::kAbsent &&
            IsRuntimeExtensionProbeStructurallyUnreliable(
                extension_scan.complete,
                advertised_extensions,
                corroborating_forwarded_extensions);
    }

    std::vector<const char*> first_downstream_extensions = base_downstream_extensions;
    // Frame Generation runs a Direct3D 11 or Vulkan game on a Direct3D 12
    // session (the Direct3D 11 and Vulkan bridges), which needs
    // XR_KHR_D3D12_enable from the start. It is added only while Frame
    // Generation is enabled for this executable (and the game's API's bridge
    // is not switched off) and the runtime offers it (or its pre-instance
    // list cannot be trusted: such scans sometimes come back empty), ahead of
    // the layer's other additions, and it never makes the application's
    // instance fail (see the retry below).
    constexpr const char* d3d11_extension = "XR_KHR_D3D11_enable";
    constexpr const char* d3d12_extension = "XR_KHR_D3D12_enable";
    diagnostics.app_requested_d3d11 = ApplicationRequestedExtension(instance_create_info, d3d11_extension);
    diagnostics.vulkan_enable1_requested = ApplicationRequestedExtension(instance_create_info, "XR_KHR_vulkan_enable");
    diagnostics.vulkan_enable_version =
        ApplicationRequestedExtension(instance_create_info, "XR_KHR_vulkan_enable2") ? 2
        : diagnostics.vulkan_enable1_requested                                          ? 1
                                                                                        : 0;
    // The application's graphics extension, of an API a bridge takes.
    const char* const bridged_extension = diagnostics.app_requested_d3d11              ? d3d11_extension
                                          : diagnostics.vulkan_enable_version == 2 ? "XR_KHR_vulkan_enable2"
                                          : diagnostics.vulkan_enable_version == 1 ? "XR_KHR_vulkan_enable"
                                                                                   : nullptr;
    if (bridged_extension && !ExtensionListContains(first_downstream_extensions, d3d12_extension) &&
        (diagnostics.app_requested_d3d11 ? OpenXrLayer::Instance().WantsD3D11Bridge()
                                         : OpenXrLayer::Instance().WantsVulkanBridge())) {
        diagnostics.d3d12_wanted = true;
        const RuntimeExtensionScan d3d12_scan =
            extension_scan.ran ? extension_scan : ScanRuntimeInstanceExtensions(next_info);
        // A scan is trusted when it lists the extension the application enabled.
        diagnostics.d3d12_scan_trusted = d3d12_scan.complete && d3d12_scan.Contains(bridged_extension);
        if (!diagnostics.d3d12_scan_trusted || d3d12_scan.Contains(d3d12_extension)) {
            first_downstream_extensions.push_back(d3d12_extension);
            diagnostics.layer_injected_d3d12 = true;
        }
    }
    diagnostics.eye_gaze_probe_known_unreliable =
        IsEyeGazeExtensionProbeKnownUnreliable(diagnostics.active_runtime_path);
    const EyeGazeRequestDecision eye_gaze_request = DecideEyeGazeExtensionRequest({
        app_requested_layer_owned_quadviews,
        forward_varjo_quad_extensions,
        ExtensionListContains(first_downstream_extensions, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME),
        diagnostics.eye_gaze_probe_state,
        diagnostics.eye_gaze_probe_structurally_unreliable,
        diagnostics.eye_gaze_probe_known_unreliable,
    });
    diagnostics.eye_gaze_request_reason = eye_gaze_request.reason;
    diagnostics.layer_injected_eye_gaze_request = eye_gaze_request.inject_extension;
    if (diagnostics.layer_injected_eye_gaze_request) {
        first_downstream_extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }
    constexpr const char* turbo_clock_extension = "XR_KHR_win32_convert_performance_counter_time";
    bool injected_turbo_clock = false;
    if (OpenXrLayer::Instance().WantsTurboClockConversion() &&
        !ExtensionListContains(first_downstream_extensions, turbo_clock_extension)) {
        const auto clock_scan = ScanRuntimeInstanceExtensions(next_info);
        if (clock_scan.Contains(turbo_clock_extension)) {
            first_downstream_extensions.push_back(turbo_clock_extension);
            injected_turbo_clock = true;
        }
    }
    diagnostics.first_downstream_extension_count = static_cast<uint32_t>(first_downstream_extensions.size());

    XrInstanceCreateInfo downstream_create_info = *instance_create_info;
    downstream_create_info.enabledExtensionCount = static_cast<uint32_t>(first_downstream_extensions.size());
    downstream_create_info.enabledExtensionNames =
        first_downstream_extensions.empty() ? nullptr : first_downstream_extensions.data();

    XrResult result = next_info->nextCreateApiLayerInstance(&downstream_create_info, &chain_info, instance);
    diagnostics.first_create_result = result;

    // The layer's own additions are optional: a runtime refusing one of them
    // (XR_KHR_D3D12_enable, an eye-gaze extension its probe hid, or the
    // experimental clock it misadvertised) gets the instance without that one
    // alone, and the others are kept. Only when no single one explains the
    // refusal is the instance created with the application's own list.
    const auto refused_extension = [](XrResult attempt) {
        return attempt == XR_ERROR_EXTENSION_NOT_PRESENT || attempt == XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
    };
    const std::vector<const char*>* successful_downstream_extensions = &first_downstream_extensions;
    std::vector<std::vector<const char*>> retries;
    if (refused_extension(result)) {
        std::vector<const char*> additions;
        if (diagnostics.layer_injected_d3d12) {
            additions.push_back(d3d12_extension);
        }
        if (diagnostics.layer_injected_eye_gaze_request) {
            additions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        }
        if (injected_turbo_clock) {
            additions.push_back(turbo_clock_extension);
        }
        retries = InstanceExtensionRetries(first_downstream_extensions, additions);
    }
    for (const std::vector<const char*>& retry : retries) {
        *instance = XR_NULL_HANDLE;
        chain_info = *layer_info;
        chain_info.nextInfo = next_info->next;
        downstream_create_info.enabledExtensionCount = static_cast<uint32_t>(retry.size());
        downstream_create_info.enabledExtensionNames = retry.empty() ? nullptr : retry.data();
        result = next_info->nextCreateApiLayerInstance(&downstream_create_info, &chain_info, instance);
        successful_downstream_extensions = &retry;
        if (!refused_extension(result)) {
            break;
        }
    }
    if (!retries.empty()) {
        if (diagnostics.layer_injected_d3d12 &&
            !ExtensionListContains(*successful_downstream_extensions, d3d12_extension)) {
            diagnostics.retried_without_d3d12 = true;
            diagnostics.d3d12_retry_result = result;
            diagnostics.d3d12_refused_alone =
                successful_downstream_extensions->size() + 1 == first_downstream_extensions.size();
        }
        if (diagnostics.layer_injected_eye_gaze_request &&
            !ExtensionListContains(*successful_downstream_extensions, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) {
            diagnostics.retried_without_eye_gaze = true;
            diagnostics.retry_create_result = result;
        }
    }

    if (XR_FAILED(result)) {
        return result;
    }

    diagnostics.d3d12_enabled = ExtensionListContains(*successful_downstream_extensions, d3d12_extension);
    diagnostics.final_downstream_extension_count =
        static_cast<uint32_t>(successful_downstream_extensions->size());
    const bool eye_gaze_extension_enabled = ExtensionListContains(
        *successful_downstream_extensions, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);

    diagnostics.turbo_clock_enabled =
        ExtensionListContains(*successful_downstream_extensions, turbo_clock_extension);
    // The runtime's instance exists from here. An exception must never leave
    // the layer (the loader turns it into a failure, or the game ends), and a
    // failure must not keep the instance: the runtime's is destroyed and the
    // layer forgets it, so the application can try again.
    XrResult created = XR_ERROR_RUNTIME_FAILURE;
    try {
        created = OpenXrLayer::Instance().OnInstanceCreated(
            instance_create_info, *instance, eye_gaze_extension_enabled, diagnostics);
    } catch (...) {
        created = XR_ERROR_RUNTIME_FAILURE;
    }
    if (XR_FAILED(created)) {
        OpenXrLayer::Instance().AbandonInstance();
        PFN_xrDestroyInstance next_destroy_instance = nullptr;
        if (XR_SUCCEEDED(next_info->nextGetInstanceProcAddr(
                *instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&next_destroy_instance))) &&
            next_destroy_instance) {
            next_destroy_instance(*instance);
        }
        *instance = XR_NULL_HANDLE;
    }
    return created;
}

XrResult __declspec(dllexport) XRAPI_CALL
    xrNegotiateLoaderApiLayerInterface(const depthxr::XrNegotiateLoaderInfo* loader_info,
                                       const char* api_layer_name,
                                       depthxr::XrNegotiateApiLayerRequest* api_layer_request) {
    using namespace depthxr;

    // Into the layer's own runtime: the game's crash reporting still sees the layer's fatal runtime errors.
    InstallCrtFailureReporting();
    OpenXrLayer::Instance().SetLayerDirectory(GetModuleDirectory());

    if (!loader_info || !api_layer_request || (api_layer_name && std::string_view(api_layer_name) != kLayerName)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loader_info->structSize < sizeof(XrNegotiateLoaderInfo) ||
        api_layer_request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        api_layer_request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        api_layer_request->structSize < sizeof(XrNegotiateApiLayerRequest) ||
        loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    api_layer_request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    api_layer_request->layerApiVersion = XR_CURRENT_API_VERSION;
    api_layer_request->getInstanceProcAddr = xrGetInstanceProcAddr;
    api_layer_request->createApiLayerInstance = xrCreateApiLayerInstance;
    return XR_SUCCESS;
}

}
