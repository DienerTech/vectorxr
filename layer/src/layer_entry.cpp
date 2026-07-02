#include "depthxr/openxr_layer.h"
#include "depthxr/openxr_loader_api_layer.h"

#include <algorithm>
#include <cstring>
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

XrResult XRAPI_CALL DepthxrAttachSessionActionSets(XrSession session,
                                                   const XrSessionActionSetsAttachInfo* attach_info) {
    return OpenXrLayer::Instance().AttachSessionActionSets(session, attach_info);
}

XrResult XRAPI_CALL DepthxrSyncActions(XrSession session, const XrActionsSyncInfo* sync_info) {
    return OpenXrLayer::Instance().SyncActions(session, sync_info);
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

bool RuntimeSupportsExtension(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr,
                              std::string_view extension_name) {
    if (!next_get_instance_proc_addr) {
        return false;
    }

    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(next_get_instance_proc_addr(
            XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function)) ||
        !function) {
        return false;
    }

    const auto enumerate_extension_properties =
        reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    uint32_t extension_count = 0;
    XrResult result = enumerate_extension_properties(nullptr, 0, &extension_count, nullptr);
    if (XR_FAILED(result) || extension_count == 0) {
        return false;
    }

    std::vector<XrExtensionProperties> properties(extension_count);
    for (XrExtensionProperties& property : properties) {
        property = {XR_TYPE_EXTENSION_PROPERTIES};
    }
    result = enumerate_extension_properties(
        nullptr, extension_count, &extension_count, properties.data());
    if (XR_FAILED(result)) {
        return false;
    }

    return std::find_if(properties.begin(), properties.end(), [&](const XrExtensionProperties& property) {
               return std::strncmp(property.extensionName,
                                   extension_name.data(),
                                   XR_MAX_EXTENSION_NAME_SIZE) == 0;
           }) != properties.end();
}

} // namespace
} // namespace depthxr

extern "C" {

XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!function || !name) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    const std::string_view requested(name);
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
    if (requested == "xrAttachSessionActionSets") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrAttachSessionActionSets);
        return XR_SUCCESS;
    }
    if (requested == "xrSyncActions") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrSyncActions);
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
    if (requested == "xrLocateViews") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(depthxr::DepthxrLocateViews);
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

    OpenXrLayer::Instance().SetNextProcAddr(next_info->nextGetInstanceProcAddr);

    XrApiLayerCreateInfo chain_info = *layer_info;
    chain_info.nextInfo = next_info->next;

    OpenXrLayer::InstanceCreateDiagnostics diagnostics;
    diagnostics.app_requested_quad_views =
        ApplicationRequestedExtension(instance_create_info, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
    diagnostics.app_requested_varjo_foveated_rendering =
        ApplicationRequestedExtension(instance_create_info, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
    diagnostics.app_requested_eye_gaze =
        ApplicationRequestedExtension(instance_create_info, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);

    // Varjo native passthrough: when the user opts in AND the app requested quad
    // views AND the runtime natively advertises them, forward the Varjo extensions
    // to the runtime instead of stripping them for stereo-composite emulation. This
    // lets the runtime drive the physical focus panels directly. Gated on the
    // runtime probe so it is inert on every non-Varjo headset.
    const bool app_requested_quad_views_for_forwarding =
        diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering;
    bool forward_varjo_quad_extensions = false;
    if (app_requested_quad_views_for_forwarding &&
        OpenXrLayer::Instance().IsVarjoNativePassthroughRequested()) {
        const bool runtime_supports_quad_views =
            RuntimeSupportsExtension(next_info->nextGetInstanceProcAddr, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
        forward_varjo_quad_extensions = runtime_supports_quad_views;
    }
    diagnostics.varjo_native_quad_forwarded = forward_varjo_quad_extensions;

    std::vector<const char*> base_downstream_extensions;
    base_downstream_extensions.reserve(instance_create_info->enabledExtensionCount + 1);
    for (uint32_t i = 0; i < instance_create_info->enabledExtensionCount; ++i) {
        const char* extension_name = instance_create_info->enabledExtensionNames[i];
        if (!extension_name) {
            continue;
        }
        // Keep the layer-owned Varjo extensions in the downstream list only when we
        // are forwarding them for native passthrough; otherwise strip as usual.
        if (IsLayerOwnedExtension(extension_name) && !forward_varjo_quad_extensions) {
            continue;
        }
        base_downstream_extensions.push_back(extension_name);
    }

    const bool app_requested_layer_owned_quadviews =
        diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering;
    if (app_requested_layer_owned_quadviews || diagnostics.app_requested_eye_gaze) {
        diagnostics.cheap_eye_gaze_probe_ran = true;
        diagnostics.cheap_eye_gaze_probe_supported =
            RuntimeSupportsExtension(next_info->nextGetInstanceProcAddr, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }

    std::vector<const char*> first_downstream_extensions = base_downstream_extensions;
    diagnostics.optimistic_eye_gaze_request =
        app_requested_layer_owned_quadviews &&
        !ExtensionListContains(first_downstream_extensions, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    if (diagnostics.optimistic_eye_gaze_request) {
        first_downstream_extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }
    diagnostics.first_downstream_extension_count = static_cast<uint32_t>(first_downstream_extensions.size());

    XrInstanceCreateInfo downstream_create_info = *instance_create_info;
    downstream_create_info.enabledExtensionCount = static_cast<uint32_t>(first_downstream_extensions.size());
    downstream_create_info.enabledExtensionNames =
        first_downstream_extensions.empty() ? nullptr : first_downstream_extensions.data();

    XrResult result = next_info->nextCreateApiLayerInstance(&downstream_create_info, &chain_info, instance);
    diagnostics.first_create_result = result;

    const std::vector<const char*>* successful_downstream_extensions = &first_downstream_extensions;
    if (XR_FAILED(result) && diagnostics.optimistic_eye_gaze_request) {
        diagnostics.retried_without_eye_gaze = true;
        *instance = XR_NULL_HANDLE;

        XrApiLayerCreateInfo retry_chain_info = *layer_info;
        retry_chain_info.nextInfo = next_info->next;

        XrInstanceCreateInfo retry_create_info = *instance_create_info;
        retry_create_info.enabledExtensionCount = static_cast<uint32_t>(base_downstream_extensions.size());
        retry_create_info.enabledExtensionNames =
            base_downstream_extensions.empty() ? nullptr : base_downstream_extensions.data();

        result = next_info->nextCreateApiLayerInstance(&retry_create_info, &retry_chain_info, instance);
        diagnostics.retry_create_result = result;
        successful_downstream_extensions = &base_downstream_extensions;
    }

    if (XR_FAILED(result)) {
        return result;
    }

    diagnostics.final_downstream_extension_count =
        static_cast<uint32_t>(successful_downstream_extensions->size());
    const bool eye_gaze_extension_enabled = ExtensionListContains(
        *successful_downstream_extensions, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);

    return OpenXrLayer::Instance().OnInstanceCreated(
        instance_create_info, *instance, eye_gaze_extension_enabled, diagnostics);
}

XrResult __declspec(dllexport) XRAPI_CALL
    xrNegotiateLoaderApiLayerInterface(const depthxr::XrNegotiateLoaderInfo* loader_info,
                                       const char* api_layer_name,
                                       depthxr::XrNegotiateApiLayerRequest* api_layer_request) {
    using namespace depthxr;

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
