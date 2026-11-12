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
#include <vector>

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
// The next instance's handle when set: a handle the runtime gives again.
XrInstance g_reuse_handle = XR_NULL_HANDLE;
// The instance the runtime's instance-level functions below were last called with.
XrInstance g_last_called = XR_NULL_HANDLE;

XrResult XRAPI_CALL RtSucceed() {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtPollEvent(XrInstance instance, XrEventDataBuffer*) {
    g_last_called = instance;
    return XR_EVENT_UNAVAILABLE;
}

XrResult XRAPI_CALL RtGetSystem(XrInstance instance, const XrSystemGetInfo*, XrSystemId* system) {
    g_last_called = instance;
    *system = 1;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetSystemProperties(XrInstance instance, XrSystemId, XrSystemProperties* properties) {
    g_last_called = instance;
    std::strncpy(properties->systemName, "Vive Reality system", XR_MAX_SYSTEM_NAME_SIZE - 1);
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

XrResult XRAPI_CALL RtCreateApiLayerInstance(const XrInstanceCreateInfo* info, const depthxr::XrApiLayerCreateInfo*,
                                             XrInstance* instance) {
    // The runtime has no quad views: an instance asking it for them is
    // refused (VectorXR's instance creation keeps them from it).
    for (std::uint32_t i = 0; info->enabledExtensionNames && i < info->enabledExtensionCount; ++i) {
        if (std::string_view(info->enabledExtensionNames[i]) == "XR_VARJO_quad_views") {
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    ++g_instances;
    *instance = g_reuse_handle != XR_NULL_HANDLE ? g_reuse_handle
                                                 : reinterpret_cast<XrInstance>(static_cast<std::uintptr_t>(g_instances));
    g_reuse_handle = XR_NULL_HANDLE;
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
    } else if (requested == "xrPollEvent") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(RtPollEvent);
    } else if (requested == "xrGetSystem") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(RtGetSystem);
    } else if (requested == "xrGetSystemProperties") {
        *function = reinterpret_cast<PFN_xrVoidFunction>(RtGetSystemProperties);
    } else if (requested.starts_with("xr") && !requested.ends_with("KHR") && !requested.ends_with("EXT") &&
               !requested.ends_with("VARJO") && !requested.ends_with("FB")) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(RtSucceed);
    } else {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

using CreateFn = XrResult(XRAPI_PTR*)(const XrInstanceCreateInfo*, const depthxr::XrApiLayerCreateInfo*, XrInstance*);

// An extension, as a game enables: the log of an instance with none (a
// probe) is removed at its xrDestroyInstance.
const std::vector<const char*> kGameExtensions = {"XR_KHR_composition_layer_depth"};

// xrCreateInstance through the layer, as the layer above it calls it, with
// `below` as the name of the layer the stand-in runtime sits behind.
XrResult CreateInstance(CreateFn create, const char* application, const char* below, XrInstance& instance,
                        const std::vector<const char*>& extensions = kGameExtensions) {
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, application, XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.enabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
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

    // A second instance while the layer's first is alive. OpenXR Toolkit and
    // Quad-Views-Foveated create a probe instance through the chain (the
    // game's name, no extension; xrGetSystem and xrGetSystemProperties) and,
    // on a VIVE runtime ("Vive Reality system"), never destroy it before the
    // game creates its own. The game's instance must be VectorXR's.
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
        // A layer above asking before any instance exists (Cheeky Foveated
        // DLSS's extension probe): an error, not a call through an empty pointer.
        {
            PFN_xrVoidFunction early = reinterpret_cast<PFN_xrVoidFunction>(&Expect);
            const XrResult early_result =
                get ? get(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &early) : XR_SUCCESS;
            Expect(XR_FAILED(early_result) && early == nullptr,
                   "instances: a function asked for before any instance is refused, not called through");
        }
        const auto runtime_function = [](PFN_xrVoidFunction function) {
            return function == reinterpret_cast<PFN_xrVoidFunction>(RtSucceed) ||
                   function == reinterpret_cast<PFN_xrVoidFunction>(RtPollEvent);
        };
        const auto intercepted = [&](XrInstance instance) {
            for (const char* name : {"xrEndFrame", "xrWaitFrame", "xrLocateViews", "xrCreateSession", "xrPollEvent"}) {
                const PFN_xrVoidFunction function = Function(get, instance, name);
                if (!function || runtime_function(function)) {
                    return false;
                }
            }
            return true;
        };
        const auto passes_through = [&](XrInstance instance) {
            return Function(get, instance, "xrEndFrame") == reinterpret_cast<PFN_xrVoidFunction>(RtSucceed) &&
                   Function(get, instance, "xrPollEvent") == reinterpret_cast<PFN_xrVoidFunction>(RtPollEvent);
        };
        // A game's instance with an extension VectorXR keeps from a runtime
        // without it (the stand-in refuses it): only VectorXR's instance
        // creation lets it through.
        const std::vector<const char*> game_extensions = {"XR_KHR_composition_layer_depth", "XR_VARJO_quad_views"};
        const std::vector<const char*> no_extensions;

        // The probe, and the functions the layer above takes for it while it
        // is VectorXR's.
        struct Probe {
            XrInstance instance{XR_NULL_HANDLE};
            PFN_xrPollEvent poll{};
            PFN_xrDestroyInstance destroy{};
            PFN_xrGetSystemProperties properties{};
        };
        const auto make_probe = [&] {
            Probe probe;
            Expect(CreateInstance(create, "Game", "XR_APILAYER_TEST_stand_in_runtime", probe.instance,
                                  no_extensions) == XR_SUCCESS &&
                       probe.instance != XR_NULL_HANDLE,
                   "instances: the probe instance is created");
            Expect(intercepted(probe.instance), "instances: the probe is VectorXR's while it is the only instance");
            probe.poll = reinterpret_cast<PFN_xrPollEvent>(Function(get, probe.instance, "xrPollEvent"));
            probe.destroy = reinterpret_cast<PFN_xrDestroyInstance>(Function(get, probe.instance, "xrDestroyInstance"));
            probe.properties =
                reinterpret_cast<PFN_xrGetSystemProperties>(Function(get, probe.instance, "xrGetSystemProperties"));
            Expect(probe.poll && probe.destroy && probe.properties, "instances: the probe's functions");
            XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
            Expect(probe.properties(probe.instance, 1, &system) == XR_SUCCESS &&
                       std::string_view(system.systemName) == "Vive Reality system",
                   "instances: the probe sees a VIVE runtime");
            return probe;
        };
        const auto create_game = [&](const std::string& label) {
            XrInstance game = XR_NULL_HANDLE;
            const XrResult result =
                CreateInstance(create, "Game", "XR_APILAYER_TEST_stand_in_runtime", game, game_extensions);
            Expect(result == XR_SUCCESS && game != XR_NULL_HANDLE,
                   "instances: " + label + ": the game's instance is created through VectorXR's extension handling " +
                       "(result " + std::to_string(static_cast<int>(result)) + ")");
            Expect(intercepted(game), "instances: " + label +
                                          ": the game's instance keeps VectorXR's frame, view and session functions");
            return game;
        };
        const auto destroy_through_layer = [&](XrInstance instance, const std::string& what) {
            const auto destroy = reinterpret_cast<PFN_xrDestroyInstance>(Function(get, instance, "xrDestroyInstance"));
            const int before = g_destroyed;
            Expect(destroy && destroy(instance) == XR_SUCCESS && g_destroyed == before + 1 &&
                       g_last_destroyed == instance,
                   "instances: " + what + " is destroyed in the runtime");
        };
        const auto destroy_probe = [&](const Probe& probe, const std::string& what) {
            const int before = g_destroyed;
            Expect(probe.destroy(probe.instance) == XR_SUCCESS && g_destroyed == before + 1 &&
                       g_last_destroyed == probe.instance,
                   "instances: " + what + " is destroyed in the runtime");
        };

        // The probe stays; the game's instance is VectorXR's, and the probe's
        // functions (and those VectorXR gave for it before) go to the runtime.
        Probe probe = make_probe();
        XrInstance game = create_game("probe kept");
        Expect(game != probe.instance, "instances: the game's instance is a new handle");
        Expect(passes_through(probe.instance), "instances: the retained probe passes straight through");
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        g_last_called = XR_NULL_HANDLE;
        Expect(probe.poll(probe.instance, &event) == XR_EVENT_UNAVAILABLE && g_last_called == probe.instance,
               "instances: VectorXR's xrPollEvent, given for the probe before, goes to the runtime with its handle");
        XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
        g_last_called = XR_NULL_HANDLE;
        Expect(probe.properties(probe.instance, 1, &system) == XR_SUCCESS && g_last_called == probe.instance,
               "instances: VectorXR's xrGetSystemProperties, given for the probe before, goes to the runtime");
        // The probe goes first (through the function VectorXR gave for it):
        // the game's instance is untouched.
        destroy_probe(probe, "the probe, before the game");
        Expect(intercepted(game), "instances: the game's instance is still VectorXR's after the probe went");
        destroy_through_layer(game, "the game's instance, after the probe");

        // The game goes first, then the probe; a new game in between (VR off
        // and on again) is VectorXR's too.
        probe = make_probe();
        game = create_game("game destroyed first");
        destroy_through_layer(game, "the game's instance, before the probe");
        const XrInstance next_game = create_game("a second game, the probe still alive");
        Expect(passes_through(probe.instance), "instances: the probe still passes through after a new game");
        destroy_through_layer(next_game, "the second game's instance");
        destroy_probe(probe, "the probe, after the games");

        // A handle the destroyed probe had comes back for the next game.
        probe = make_probe();
        game = create_game("before the probe's handle comes back");
        destroy_probe(probe, "the probe");
        destroy_through_layer(game, "the game's instance, the probe gone");
        g_reuse_handle = probe.instance;
        game = create_game("the probe's handle reused");
        Expect(game == probe.instance, "instances: the runtime gave the probe's handle again");
        destroy_through_layer(game, "the game's instance on the probe's handle");

        // An instance that can have a session keeps VectorXR: one after it
        // passes straight through.
        XrInstance first = XR_NULL_HANDLE;
        const std::vector<const char*> d3d11 = {"XR_KHR_D3D11_enable"};
        Expect(CreateInstance(create, "First", "XR_APILAYER_TEST_stand_in_runtime", first, d3d11) == XR_SUCCESS &&
                   intercepted(first),
               "instances: an instance with a graphics extension is VectorXR's");
        XrInstance second = XR_NULL_HANDLE;
        const XrResult result = CreateInstance(create, "Second", "XR_APILAYER_TEST_stand_in_runtime", second);
        Expect(result == XR_SUCCESS && second != XR_NULL_HANDLE && second != first,
               "instances: a second instance while the first is alive must be created, not refused (result " +
                   std::to_string(static_cast<int>(result)) + ")");
        Expect(passes_through(second) && Function(get, second, "xrGetInstanceProperties") ==
                                             reinterpret_cast<PFN_xrVoidFunction>(RtGetInstanceProperties),
               "instances: the second instance's functions must be the runtime's");
        Expect(intercepted(first), "instances: the first instance keeps the layer's functions");
        destroy_through_layer(second, "the second instance");
        Expect(!passes_through(second), "instances: a destroyed pass-through handle is forgotten");
        destroy_through_layer(first, "the first instance");

        // Quad-Views-Foveated below the layer is named in its log.
        XrInstance with_qvf = XR_NULL_HANDLE;
        Expect(CreateInstance(create, "Game", "XR_APILAYER_MBUCCHIA_quad_views_foveated", with_qvf) == XR_SUCCESS,
               "instances: an instance above Quad-Views-Foveated is created");
        destroy_through_layer(with_qvf, "the instance above Quad-Views-Foveated");
        // Cheeky Foveated DLSS's layer below is named in its log too.
        XrInstance with_cheeky = XR_NULL_HANDLE;
        Expect(CreateInstance(create, "Game", "XR_APILAYER_CHEEKY_foveated_dlss", with_cheeky) == XR_SUCCESS,
               "instances: an instance above Cheeky Foveated DLSS is created");
        destroy_through_layer(with_cheeky, "the instance above Cheeky Foveated DLSS");
        // With every instance gone the runtime's DLL may be unloaded (MSFS 2024
        // leaving VR, issue 138): nothing taken from it is called again.
        {
            PFN_xrVoidFunction after = reinterpret_cast<PFN_xrVoidFunction>(&Expect);
            const XrResult after_result = get(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &after);
            Expect(XR_FAILED(after_result) && after == nullptr,
                   "instances: after the last instance, the chain below it is not called");
        }
        Expect(FreeLibrary(layer) != FALSE, "instances: the layer is freed");
        // The logs as written once the layer has gone.
        const std::string log = ReadLogs(app_data);
        Expect(log.find("An earlier OpenXR instance (application \"Game\", 0 extensions") != std::string::npos,
               "instances: the retired probe is said in the game's log");
        Expect(log.find("A second OpenXR instance (application \"Second\"") != std::string::npos,
               "instances: the pass-through is said in the log");
        Expect(log.find("Quad-Views-Foveated is loaded below VectorXR") != std::string::npos,
               "instances: Quad-Views-Foveated below the layer is named in the log");
        Expect(log.find("Cheeky Foveated DLSS's OpenXR layer is loaded below VectorXR") != std::string::npos,
               "instances: Cheeky Foveated DLSS below the layer is named in the log");
    }

    std::cout << "Layer load tests passed: loaded, negotiated and unloaded three times without the VC++ runtime DLLs; "
                 "a retained probe leaves the game's instance VectorXR's\n";
    return 0;
}
