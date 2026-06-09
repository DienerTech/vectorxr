#include "depthxr/openxr_layer.h"
#include "depthxr/openxr_loader_api_layer.h"

#include <algorithm>
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

    XrInstanceCreateInfo downstream_create_info = *instance_create_info;
    std::vector<const char*> downstream_extensions;
    downstream_extensions.reserve(instance_create_info->enabledExtensionCount);
    for (uint32_t i = 0; i < instance_create_info->enabledExtensionCount; ++i) {
        const char* extension_name = instance_create_info->enabledExtensionNames[i];
        if (!extension_name || IsLayerOwnedExtension(extension_name)) {
            continue;
        }
        downstream_extensions.push_back(extension_name);
    }
    downstream_create_info.enabledExtensionCount = static_cast<uint32_t>(downstream_extensions.size());
    downstream_create_info.enabledExtensionNames = downstream_extensions.empty() ? nullptr : downstream_extensions.data();

    const XrResult result = next_info->nextCreateApiLayerInstance(&downstream_create_info, &chain_info, instance);
    if (XR_FAILED(result)) {
        return result;
    }

    return OpenXrLayer::Instance().OnInstanceCreated(instance_create_info, *instance);
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
