#pragma once

#include <cstddef>
#include <cstdint>

#include <openxr/openxr.h>

namespace depthxr {

enum XrLoaderInterfaceStructs : uint32_t {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO = 4,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO = 5,
};

constexpr uint32_t XR_LOADER_INFO_STRUCT_VERSION = 1;
constexpr uint32_t XR_API_LAYER_INFO_STRUCT_VERSION = 1;
constexpr uint32_t XR_API_LAYER_CREATE_INFO_STRUCT_VERSION = 1;
constexpr uint32_t XR_API_LAYER_NEXT_INFO_STRUCT_VERSION = 1;
constexpr uint32_t XR_CURRENT_LOADER_API_LAYER_VERSION = 1;

struct XrApiLayerCreateInfo;

using PFN_xrCreateApiLayerInstance =
    XrResult(XRAPI_PTR*)(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer_info, XrInstance* out);

struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
};

struct XrNegotiateApiLayerRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t layerInterfaceVersion;
    XrVersion layerApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
    PFN_xrCreateApiLayerInstance createApiLayerInstance;
};

struct XrApiLayerNextInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    const char* layerName;
    PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance nextCreateApiLayerInstance;
    XrApiLayerNextInfo* next;
};

struct XrApiLayerCreateInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    XrApiLayerNextInfo* nextInfo;
};

} // namespace depthxr
