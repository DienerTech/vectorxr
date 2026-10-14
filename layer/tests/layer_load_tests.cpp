// The layer DLL as the OpenXR loader handles it: loaded, negotiated (which takes the layer's first lock) and
// unloaded, three times, as the loader unloads a layer at xrDestroyInstance. The layer carries its own C and C++
// runtime (/MT), so it must not bring msvcp140 or vcruntime140 into the game's process (a game may ship an old
// one, and a PC without the VC++ redistributable has none), and its runtime must start again after an unload.
#include "depthxr/openxr_loader_api_layer.h"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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

constexpr const char* kRuntimeDlls[] = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"};

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

    std::cout << "Layer load tests passed: loaded, negotiated and unloaded three times without the VC++ runtime DLLs\n";
    return 0;
}
