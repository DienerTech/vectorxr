// The layer DLL as the OpenXR loader handles it: loaded, negotiated (which takes the layer's first lock) and
// unloaded, three times, as the loader unloads a layer at xrDestroyInstance. The layer carries its own C and C++
// runtime (/MT), so it must not bring msvcp140 or vcruntime140 into the game's process (a game may ship an old
// one, and a PC without the VC++ redistributable has none), and its runtime must start again after an unload.
#include "depthxr/openxr_loader_api_layer.h"

#include <Windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

using NegotiateFn = XrResult(XRAPI_PTR*)(const depthxr::XrNegotiateLoaderInfo* loader_info,
                                         const char* api_layer_name,
                                         depthxr::XrNegotiateApiLayerRequest* api_layer_request);

// The Debug runtime DLLs too, so a Debug build checks its own configuration (/MTd, not /MDd). ucrtbase.dll is
// not listed: Windows' own DLLs load it.
constexpr const char* kRuntimeDlls[] = {"msvcp140.dll",  "vcruntime140.dll",  "vcruntime140_1.dll",
                                        "msvcp140d.dll", "vcruntime140d.dll", "vcruntime140_1d.dll",
                                        "ucrtbased.dll"};

// A stand-in runtime below the layer: instances 1, 2, ... and functions that
// succeed (the x64 calling convention lets one that takes nothing stand in
// for one that takes arguments: the caller owns them).
int g_instances = 0;
int g_destroyed = 0;
XrInstance g_last_destroyed = XR_NULL_HANDLE;

XrResult XRAPI_CALL RtSucceed() {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtDestroyInstance(XrInstance instance) {
    ++g_destroyed;
    g_last_destroyed = instance;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateInstanceExtensions(const char*, std::uint32_t, std::uint32_t* count,
                                                   XrExtensionProperties*) {
    *count = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
    std::strncpy(properties->runtimeName, "Layer-load stand-in", XR_MAX_RUNTIME_NAME_SIZE - 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateApiLayerInstance(const XrInstanceCreateInfo*, const depthxr::XrApiLayerCreateInfo*,
                                             XrInstance* instance) {
    *instance = reinterpret_cast<XrInstance>(static_cast<std::uintptr_t>(++g_instances));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    *function = nullptr;
    const std::string_view requested(name);
    if (instance == XR_NULL_HANDLE) {
        if (requested == "xrEnumerateInstanceExtensionProperties") {
            *function = reinterpret_cast<PFN_xrVoidFunction>(RtEnumerateInstanceExtensions);
            return XR_SUCCESS;
        }
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (requested == "xrDestroyInstance") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(RtDestroyInstance);
    } else if (requested == "xrGetInstanceProperties") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(RtGetInstanceProperties);
    } else if (requested.starts_with("xr") && !requested.ends_with("KHR") && !requested.ends_with("EXT") &&
               !requested.ends_with("VARJO") && !requested.ends_with("FB")) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(RtSucceed);
    } else {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

using CreateFn = XrResult(XRAPI_PTR*)(const XrInstanceCreateInfo*, const depthxr::XrApiLayerCreateInfo*, XrInstance*);

// xrCreateInstance through the layer, as the layer above it calls it, with
// `below` as the name of the layer the stand-in runtime sits behind.
XrResult CreateInstance(CreateFn create, const char* application, const char* below, XrInstance& instance) {
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, application, XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    // An extension, as a game enables: the log of an instance with none
    // (a probe) is removed at its xrDestroyInstance.
    static const char* const kExtensions[] = {"XR_KHR_composition_layer_depth"};
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = kExtensions;
    depthxr::XrApiLayerNextInfo runtime{};
    runtime.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    runtime.structVersion = depthxr::XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    runtime.structSize = sizeof(runtime);
    std::strncpy(runtime.layerName, below, XR_MAX_API_LAYER_NAME_SIZE - 1);
    runtime.nextGetInstanceProcAddr = RtGetInstanceProcAddr;
    runtime.nextCreateApiLayerInstance = RtCreateApiLayerInstance;
    depthxr::XrApiLayerNextInfo layer = runtime;
    std::strncpy(layer.layerName, "XR_APILAYER_DIENERTECH_VECTORXR", XR_MAX_API_LAYER_NAME_SIZE - 1);
    layer.next = &runtime;
    depthxr::XrApiLayerCreateInfo create_info{};
    create_info.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
    create_info.structVersion = depthxr::XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
    create_info.structSize = sizeof(create_info);
    create_info.nextInfo = &layer;
    instance = XR_NULL_HANDLE;
    return create(&info, &create_info, &instance);
}

PFN_xrVoidFunction Function(PFN_xrGetInstanceProcAddr get, XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    return XR_SUCCEEDED(get(instance, name, &function)) ? function : nullptr;
}

std::string ReadLogs(const std::filesystem::path& folder) {
    std::string text;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            std::ifstream stream(entry.path(), std::ios::binary);
            text.append(std::istreambuf_iterator<char>(stream), {});
        }
    }
    return text;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Expect(argc == 2, "usage: depthxr_layer_load_tests <XR_APILAYER_DIENERTECH_VECTORXR.dll>");
    const std::filesystem::path layer_path = std::filesystem::absolute(argv[1]);

    // Whatever the layer might write goes beside this test, never into the user's VectorXR folder.
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::filesystem::path app_data = std::filesystem::path(exe_path).parent_path() / "layer_load_tests_appdata";
    SetEnvironmentVariableW(L"LOCALAPPDATA", app_data.c_str());

    // This test carries its own runtime as well. Something else that loaded one (an overlay injected into every
    // process) leaves nothing to check there.
    bool runtime_loaded_before = false;
    for (const char* dll : kRuntimeDlls) {
        if (GetModuleHandleA(dll)) {
            std::cout << "note: " << dll << " was loaded before the layer; the runtime check is skipped\n";
            runtime_loaded_before = true;
        }
    }

    for (int round = 1; round <= 3; ++round) {
        const std::string label = "round " + std::to_string(round) + ": ";
        const HMODULE layer = LoadLibraryW(layer_path.c_str());
        Expect(layer != nullptr, label + "the layer loads (error " + std::to_string(GetLastError()) + ")");
        const auto negotiate =
            reinterpret_cast<NegotiateFn>(GetProcAddress(layer, "xrNegotiateLoaderApiLayerInterface"));
        Expect(negotiate != nullptr, label + "the layer exports xrNegotiateLoaderApiLayerInterface");

        depthxr::XrNegotiateLoaderInfo loader_info{};
        loader_info.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader_info.structVersion = depthxr::XR_LOADER_INFO_STRUCT_VERSION;
        loader_info.structSize = sizeof(loader_info);
        loader_info.minInterfaceVersion = 1;
        loader_info.maxInterfaceVersion = depthxr::XR_CURRENT_LOADER_API_LAYER_VERSION;
        loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader_info.maxApiVersion = XR_CURRENT_API_VERSION;
        depthxr::XrNegotiateApiLayerRequest request{};
        request.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
        request.structVersion = depthxr::XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        const XrResult result = negotiate(&loader_info, "XR_APILAYER_DIENERTECH_VECTORXR", &request);
        Expect(result == XR_SUCCESS,
               label + "the layer negotiates (result " + std::to_string(static_cast<int>(result)) + ")");
        Expect(request.getInstanceProcAddr && request.createApiLayerInstance,
               label + "the layer returns its entry points");

        if (!runtime_loaded_before) {
            for (const char* dll : kRuntimeDlls) {
                Expect(!GetModuleHandleA(dll), label + "the layer loaded " + dll);
            }
        }

        Expect(FreeLibrary(layer) != FALSE, label + "the layer is freed");
        Expect(!GetModuleHandleW(layer_path.c_str()), label + "the layer unloads");
    }

    // A second instance while the layer's first is alive: OpenXR Toolkit and
    // Quad-Views-Foveated create a probe instance through the chain and, on a
    // VIVE runtime ("Vive Reality system"), never destroy it before the game
    // creates its own. The game's instance must pass straight through.
    {
        std::error_code error;
        std::filesystem::remove_all(app_data, error);
        const HMODULE layer = LoadLibraryW(layer_path.c_str());
        Expect(layer != nullptr, "instances: the layer loads");
        const auto negotiate =
            reinterpret_cast<NegotiateFn>(GetProcAddress(layer, "xrNegotiateLoaderApiLayerInterface"));
        depthxr::XrNegotiateLoaderInfo loader_info{};
        loader_info.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader_info.structVersion = depthxr::XR_LOADER_INFO_STRUCT_VERSION;
        loader_info.structSize = sizeof(loader_info);
        loader_info.minInterfaceVersion = 1;
        loader_info.maxInterfaceVersion = depthxr::XR_CURRENT_LOADER_API_LAYER_VERSION;
        loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader_info.maxApiVersion = XR_CURRENT_API_VERSION;
        depthxr::XrNegotiateApiLayerRequest request{};
        request.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
        request.structVersion = depthxr::XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        Expect(negotiate && negotiate(&loader_info, "XR_APILAYER_DIENERTECH_VECTORXR", &request) == XR_SUCCESS,
               "instances: the layer negotiates");
        const auto create = reinterpret_cast<CreateFn>(request.createApiLayerInstance);
        const PFN_xrGetInstanceProcAddr get = request.getInstanceProcAddr;

        XrInstance probe = XR_NULL_HANDLE;
        XrInstance game = XR_NULL_HANDLE;
        Expect(CreateInstance(create, "OpenXR Toolkit probe", "XR_APILAYER_TEST_stand_in_runtime", probe) ==
                       XR_SUCCESS && probe != XR_NULL_HANDLE,
               "instances: the first (probe) instance is created");
        const XrResult second = CreateInstance(create, "Game", "XR_APILAYER_TEST_stand_in_runtime", game);
        Expect(second == XR_SUCCESS && game != XR_NULL_HANDLE && game != probe,
               "instances: a second instance while the first is alive must be created, not refused (result " +
                   std::to_string(static_cast<int>(second)) + ")");
        Expect(Function(get, game, "xrEndFrame") == reinterpret_cast<PFN_xrVoidFunction>(RtSucceed) &&
                   Function(get, game, "xrGetInstanceProperties") ==
                       reinterpret_cast<PFN_xrVoidFunction>(RtGetInstanceProperties),
               "instances: the second instance's functions must be the runtime's");
        Expect(Function(get, probe, "xrEndFrame") != reinterpret_cast<PFN_xrVoidFunction>(RtSucceed),
               "instances: the first instance keeps the layer's functions");
        const auto destroy_game = reinterpret_cast<PFN_xrDestroyInstance>(Function(get, game, "xrDestroyInstance"));
        Expect(destroy_game && destroy_game(game) == XR_SUCCESS && g_destroyed == 1 && g_last_destroyed == game,
               "instances: the second instance is destroyed in the runtime");
        Expect(Function(get, game, "xrEndFrame") != reinterpret_cast<PFN_xrVoidFunction>(RtSucceed),
               "instances: a destroyed pass-through handle is forgotten");
        const auto destroy_probe = reinterpret_cast<PFN_xrDestroyInstance>(Function(get, probe, "xrDestroyInstance"));
        Expect(destroy_probe && destroy_probe(probe) == XR_SUCCESS && g_destroyed == 2 && g_last_destroyed == probe,
               "instances: the first instance is destroyed through the layer");

        Expect(FreeLibrary(layer) != FALSE, "instances: the layer is freed");
        // The logs as written once the layer has gone.
        const std::string log = ReadLogs(app_data);
        Expect(log.find("A second OpenXR instance (application \"Game\"") != std::string::npos,
               "instances: the pass-through is said in the log");
    }

    std::cout << "Layer load tests passed: loaded, negotiated and unloaded three times without the VC++ runtime DLLs; "
                 "a second instance passes through\n";
    return 0;
}
